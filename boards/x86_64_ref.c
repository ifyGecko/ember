/*
 * x86_64_ref.c - "x86_64-ref": the x86-64 reference board.
 *
 * A PC made only of ember's generic components and only of what every x86-64
 * PC has: the PC/AT core devices, the APICs, PCI configuration and a boot
 * flash. It is an example and a starting point, not a model of any board. It
 * has no chipset (memory-controller registers, SMRAM control, ACPI and SMI
 * logic) and no optional peripherals (storage, display, printer): a board adds
 * those out of tree, building on these components or on this board
 * (uam_machine_find_device finds its parts after its build has run).
 *
 * Memory map:
 *   0x00000000 - low_top        RAM (up to 2 GiB below 4 GiB)
 *   0x100000000 -               the rest of RAM
 *   0xFEC00000                  I/O APIC
 *   0xFED00000                  HPET
 *   0xFEE00000                  local APIC (relocatable via IA32_APIC_BASE)
 *   4 GiB - flash size - 4 GiB  boot flash (CFI): image "flash"
 * I/O ports (the aperture at UAM_X86_PIO_BASE):
 *   0x20/0xA0/0x4D0 8259 + ELCR, 0x40-0x43/0x61 8254, 0x60/0x64 8042 PS/2,
 *   0x70-0x73 RTC/CMOS,
 *   0x00-0x0F/0xC0-0xDF/0x81-0x8F 8237 DMA controllers and page registers,
 *   0x80 POST codes, 0x92 system control A (and, with the 8042's output
 *   port, the A20 gate), 0x3F8 COM1, 0xCF8-0xCFF PCI configuration,
 *   0x700 test finisher, the rest RAZ/WI.
 * PCI bus 0: empty.
 * Memory no device claims reads as all ones (master abort).
 * Interrupts: ISA IRQs go to the 8259 and to the I/O APIC (IRQ 0 on pin 2);
 * the 8259 feeds the local APIC's LINT0; the local APIC drives the processor.
 * PCI INTx of slot S, pin P (0 = INTA) arrives on I/O APIC pin
 * 16 + (S + P) mod 4; MSI writes are interrupt messages to the local APIC.
 *
 * Virtual time: one tick (instruction) is one nanosecond. The local APIC
 * timer counts at `apic-hz` (option, default 1 GHz); the TSC counts virtual
 * ticks, and the APIC also offers x2APIC mode and the TSC-deadline timer.
 *
 * Without a flash image the board runs a flat image loaded into RAM
 * (--raw-base 0 --entry 0), entered in real mode.
 *
 * Options:
 *   -o cpus=N          processors (default 1; up to 64): one local APIC each
 *                      (APIC ID = index), the others wait for a startup IPI
 *   -o apic-hz=HZ      local APIC timer input clock
 *   -o smm-map=MAP     SMRAM save-state map: intel64 (default) or amd64
 *                      (uam_x86_set_smm_map), for the SMM handler's vendor
 *   -o persist=1       write flash programs/erases back to the image file
 *
 * --image bios=PATH is a legacy BIOS ROM (up to 1 MiB): it ends at 4 GiB, and
 * its last 128 KiB (all of it if smaller) also appear just below 1 MiB, the
 * way PC chipsets decode the boot ROM; execution starts at the reset vector.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uam/boards.h"
#include "uam/dev/apic.h"
#include "uam/dev/cfi_flash.h"
#include "uam/dev/hpet.h"
#include "uam/dev/i8042.h"
#include "uam/dev/i8237.h"
#include "uam/dev/i8254.h"
#include "uam/dev/i8259.h"
#include "uam/dev/mc146818.h"
#include "uam/dev/pc_misc.h"
#include "uam/dev/pci.h"
#include "uam/dev/test_finisher.h"
#include "uam/dev/uart16550.h"
#include "uam/dev/unimp.h"
#include "uam/dev/unknown_mmio.h"
#include "uam/isa/x86.h"
#include "uam/loader.h"
#include "uam/machine.h"

#define TICK_HZ        UINT64_C(1000000000)
#define DEFAULT_RAM    (UINT64_C(512) << 20)
#define LOW_RAM_MAX    UINT64_C(0x80000000)
#define HIGH_RAM_BASE  UINT64_C(0x100000000)
#define LAPIC_BASE     UINT64_C(0xFEE00000)
#define IOAPIC_BASE    UINT64_C(0xFEC00000)
#define HPET_BASE      UINT64_C(0xFED00000)
#define FLASH_BLOCK    UINT64_C(0x1000)
#define RTC_EPOCH      UINT64_C(1767225600)   /* 2026-01-01 00:00:00 UTC */

/* ISA interrupt: to the 8259 and to the I/O APIC (IRQ 0 is wired to pin 2). */
typedef struct isa_irq { uam_device *pic, *ioapic; } isa_irq;

#define MAX_CPUS 64

typedef struct ref_board ref_board;
typedef struct ref_cpu { ref_board *b; unsigned i; } ref_cpu;   /* platform hook context */

struct ref_board {
    uam_machine *m;
    unsigned     ncpu;
    uam_device  *lapic[MAX_CPUS];
    ref_cpu      pcpu[MAX_CPUS];
    char         lapic_name[MAX_CPUS][12];
    uam_region  *lapic_region;
    isa_irq      isa;
};

static uint64_t pio(uint64_t port) { return UAM_X86_PIO_BASE + port; }

/* ---- platform hooks for the processor ------------------------------------ */

static int ref_inta(void *ctx)
{
    ref_cpu *c = ctx;
    return uam_lapic_inta(c->b->lapic[c->i]);
}

static bool ref_wrmsr(void *ctx, uint32_t msr, uint64_t val)
{
    ref_cpu *c = ctx;
    ref_board *b = c->b;
    uam_device *lapic = b->lapic[c->i];
    if (msr == 0x1B) {   /* IA32_APIC_BASE: move the register page, enable this APIC */
        bool en = (val >> 11) & 1, x2 = (val >> 10) & 1;
        uam_bus_region_move(&b->m->bus, b->lapic_region, val & 0xFFFFFFFFFF000ull);
        if (b->ncpu == 1)
            uam_bus_region_enable(&b->m->bus, b->lapic_region, en && !x2);
        uam_lapic_set_enabled(lapic, en);
        uam_lapic_set_x2apic(lapic, x2);
        return false;
    }
    return uam_lapic_wrmsr(lapic, msr, val);   /* x2APIC, IA32_TSC_DEADLINE */
}

static bool ref_rdmsr(void *ctx, uint32_t msr, uint64_t *val)
{
    ref_cpu *c = ctx;
    return uam_lapic_rdmsr(c->b->lapic[c->i], msr, val);
}

static int pic_inta(void *ctx) { return uam_i8259_inta(ctx); }

/* A20M# reaches every processor. */
static void a20m_all(void *ctx, unsigned input, int level)
{
    (void)input;
    ref_board *b = ctx;
    for (unsigned i = 0; i < b->ncpu; i++)
        uam_cpu_irq_sink(uam_machine_cpu(b->m, i), UAM_X86_IRQ_A20M, level);
}

static void isa_sink(void *ctx, unsigned irq, int level)
{
    isa_irq *w = ctx;
    uam_i8259_input(w->pic, irq, level);
    uam_ioapic_input(w->ioapic, irq == 0 ? 2 : irq, level);
}

/* PCI INTx: the standard swizzle onto I/O APIC pins 16-19. */
static void pci_intx(void *ctx, unsigned slot, unsigned pin, int level)
{
    uam_ioapic_input(ctx, 16 + ((slot + pin) & 3), level);
}

static void board_fini(uam_machine *m)
{
    free(m->board_data);
    m->board_data = NULL;
}

/* ---- construction -------------------------------------------------------- */

static uam_status add(uam_machine *m, uam_device *d)
{
    if (!d)
        return UAM_ERR_NOMEM;
    uam_machine_add_device(m, d);
    return UAM_OK;
}

static uam_status map_io(uam_machine *m, const char *name, uint64_t port, uint64_t size,
                         uam_device *d)
{
    return uam_bus_map_mmio(&m->bus, name, pio(port), size, d);
}

/* Boot flash ending at 4 GiB: image "flash". */
static uam_status map_flash(uam_machine *m, const uam_board_config *cfg)
{
    const char *path = uam_board_config_image(cfg, "flash");
    if (!path)
        return UAM_OK;
    uint8_t *data;
    size_t len;
    uam_status st = uam_load_file(path, &data, &len);
    if (st != UAM_OK)
        return st;
    uint64_t size = (len + FLASH_BLOCK - 1) & ~(FLASH_BLOCK - 1);
    if (!size || size > (UINT64_C(16) << 20)) {
        free(data);
        uam_log(UAM_LOG_ERROR, "x86_64-ref", "%s: flash image size %zu not supported", path, len);
        return UAM_ERR_INVALID;
    }
    uam_device *f = uam_cfi_flash_map(&m->bus, "flash", UINT64_C(0x100000000) - size, size,
                                      FLASH_BLOCK, data, len);
    free(data);
    if (!f)
        return UAM_ERR_NOMEM;
    uam_machine_add_device(m, f);
    const char *persist = uam_board_config_option(cfg, "persist");
    if (persist && strcmp(persist, "0") != 0)
        return uam_cfi_flash_set_backing_file(f, path);
    return UAM_OK;
}

/* A legacy BIOS ROM: ending at 4 GiB, its last 128 KiB also below 1 MiB. */
static uam_status map_bios(uam_machine *m, const char *path)
{
    uint8_t *data;
    size_t len;
    uam_status st = uam_load_file(path, &data, &len);
    if (st != UAM_OK)
        return st;
    if (len < 0x10000 || len > 0x100000 || (len & 0xFFF)) {
        free(data);
        uam_log(UAM_LOG_ERROR, "x86_64-ref", "%s: a BIOS image is 64 KiB to 1 MiB in 4 KiB steps", path);
        return UAM_ERR_INVALID;
    }
    uint64_t base = UINT64_C(0x100000000) - len;
    st = uam_bus_map_rom(&m->bus, "bios", base, len, data, len);
    free(data);
    if (st != UAM_OK)
        return st;
    uint64_t low = len < 0x20000 ? len : 0x20000;
    return uam_bus_map_alias(&m->bus, "bios-low", 0x100000 - low, low, uam_bus_region(&m->bus, "bios"),
                             len - low, &(uam_map_attrs){ .prio = 1 });
}

/* CMOS memory-size bytes of the PC/AT layout (base and extended memory). */
static void cmos_memory(uam_device *rtc, uint64_t low)
{
    uint64_t ext_kb = low > (1u << 20) ? (low - (1u << 20)) >> 10 : 0;
    if (ext_kb > 0xFFFF) ext_kb = 0xFFFF;
    uam_mc146818_nvram_set(rtc, 0x15, 0x80);   /* 640 KiB base memory */
    uam_mc146818_nvram_set(rtc, 0x16, 0x02);
    uam_mc146818_nvram_set(rtc, 0x17, (uint8_t)ext_kb);
    uam_mc146818_nvram_set(rtc, 0x18, (uint8_t)(ext_kb >> 8));
    uam_mc146818_nvram_set(rtc, 0x30, (uint8_t)ext_kb);
    uam_mc146818_nvram_set(rtc, 0x31, (uint8_t)(ext_kb >> 8));
}

static uam_status x86_ref_build(uam_machine *m, const uam_board_config *cfg)
{
    uam_status st = uam_machine_init(m, &uam_isa_x86_64);
    if (st != UAM_OK)
        return st;
    m->board_name = "x86_64-ref";
    m->reset_pc = 0xFFFFFFF0ull;

    ref_board *b = calloc(1, sizeof(*b));
    if (!b)
        return UAM_ERR_NOMEM;
    b->m = m;
    m->board_data = b;
    m->board_fini = board_fini;
    const char *ncpu = uam_board_config_option(cfg, "cpus");
    b->ncpu = ncpu ? (unsigned)strtoul(ncpu, NULL, 0) : 1;
    if (b->ncpu < 1 || b->ncpu > MAX_CPUS)
        return UAM_ERR_INVALID;
    if ((st = uam_machine_add_cpus(m, b->ncpu - 1)) != UAM_OK)
        return st;
    isa_irq *isa = &b->isa;

    /* RAM */
    uint64_t ram = cfg->ram_size ? cfg->ram_size : DEFAULT_RAM;
    uint64_t low = ram < LOW_RAM_MAX ? ram : LOW_RAM_MAX, high = ram - low;
    if ((st = uam_bus_map_ram(&m->bus, "ram", 0, low)) != UAM_OK)
        return st;
    if (high && (st = uam_bus_map_ram(&m->bus, "ram-high", HIGH_RAM_BASE, high)) != UAM_OK)
        return st;

    /* the port space: catch-all first, real devices on top */
    uam_device *iowin = uam_unimp_create("io", UAM_X86_PIO_BASE);
    if ((st = add(m, iowin)) != UAM_OK ||
        (st = uam_bus_map_mmio_ex(&m->bus, "io", UAM_X86_PIO_BASE, UAM_X86_PIO_SIZE, iowin,
                                  &(uam_map_attrs){ .prio = UAM_PRIO_BACKGROUND })) != UAM_OK)
        return st;

    if ((st = map_flash(m, cfg)) != UAM_OK)
        return st;
    const char *bios = uam_board_config_image(cfg, "bios");
    if (bios && (st = map_bios(m, bios)) != UAM_OK)
        return st;

    /* interrupt controllers */
    uam_device *pic = uam_i8259_create("pic");
    const char *ahz = uam_board_config_option(cfg, "apic-hz");
    uint64_t apic_hz = ahz ? strtoull(ahz, NULL, 0) : 1000000000u;
    uam_device *ioapic = uam_ioapic_create("ioapic", (uint8_t)b->ncpu);
    uam_apic_bus *abus = uam_apic_bus_create("apic-bus");
    if ((st = add(m, pic)) != UAM_OK || (st = add(m, ioapic)) != UAM_OK ||
        (st = add(m, abus ? uam_apic_bus_device(abus) : NULL)) != UAM_OK)
        return st;
    isa->pic = pic;
    isa->ioapic = ioapic;
    /* one local APIC per processor (APIC ID = processor index), on one bus */
    for (unsigned i = 0; i < b->ncpu; i++) {
        uam_cpu *cpu = uam_machine_cpu(m, i);
        snprintf(b->lapic_name[i], sizeof(b->lapic_name[i]), i ? "lapic%u" : "lapic", i);
        uam_device *l = uam_lapic_create(b->lapic_name[i], &m->sched, TICK_HZ, apic_hz, (uint8_t)i);
        if ((st = add(m, l)) != UAM_OK || (st = uam_apic_bus_add(abus, l)) != UAM_OK)
            return st;
        b->lapic[i] = l;
        b->pcpu[i] = (ref_cpu){ b, i };
        uam_lapic_set_eoi_hook(l, uam_ioapic_eoi, ioapic);
        uam_lapic_set_startup(l, uam_x86_sipi, cpu);
        uam_lapic_set_tsc(l, uam_x86_read_tsc, cpu);
        uam_signal_connect(uam_lapic_intr_line(l), uam_cpu_irq_sink, cpu, UAM_X86_IRQ_INTR);
        uam_signal_connect(uam_lapic_nmi_line(l), uam_cpu_irq_sink, cpu, UAM_X86_IRQ_NMI);
        uam_signal_connect(uam_lapic_smi_line(l), uam_cpu_irq_sink, cpu, UAM_X86_IRQ_SMI);
        uam_signal_connect(uam_lapic_init_line(l), uam_cpu_irq_sink, cpu, UAM_X86_IRQ_INIT);
    }
    /* every processor sees its own APIC at the register page */
    uam_device *lwin = b->ncpu == 1 ? b->lapic[0]
                     : uam_lapic_window_create("lapic-window", b->lapic, b->ncpu,
                                               uam_machine_current_cpu, m);
    if (b->ncpu > 1 && (st = add(m, lwin)) != UAM_OK)
        return st;
    if ((st = map_io(m, "pic-master", 0x20, 2, uam_i8259_master(pic))) != UAM_OK ||
        (st = map_io(m, "pic-slave", 0xA0, 2, uam_i8259_slave(pic))) != UAM_OK ||
        (st = map_io(m, "elcr", 0x4D0, 2, uam_i8259_elcr(pic))) != UAM_OK ||
        (st = uam_bus_map_mmio(&m->bus, "lapic", LAPIC_BASE, UAM_LAPIC_SIZE, lwin)) != UAM_OK ||
        (st = uam_bus_map_mmio(&m->bus, "ioapic", IOAPIC_BASE, UAM_IOAPIC_SIZE, ioapic)) != UAM_OK)
        return st;
    b->lapic_region = uam_bus_region(&m->bus, "lapic");
    /* the 8259 is wired to the bootstrap processor's LINT0 */
    uam_signal_connect(uam_i8259_intr_line(pic), uam_lapic_lint, b->lapic[0], 0);
    uam_lapic_set_extint(b->lapic[0], pic_inta, pic);
    uam_ioapic_set_target(ioapic, uam_apic_bus_deliver, abus);

    /* timers, RTC/CMOS */
    uam_device *pit = uam_i8254_create("pit", &m->sched, TICK_HZ);
    uam_device *rtc = uam_mc146818_create("rtc", &m->sched, TICK_HZ, RTC_EPOCH);
    if ((st = add(m, pit)) != UAM_OK || (st = add(m, rtc)) != UAM_OK)
        return st;
    if ((st = map_io(m, "pit", 0x40, 4, uam_i8254_ports(pit))) != UAM_OK ||
        (st = map_io(m, "port61", 0x61, 1, uam_i8254_port61(pit))) != UAM_OK ||
        (st = map_io(m, "rtc", 0x70, 2, uam_mc146818_bank0(rtc))) != UAM_OK ||
        (st = map_io(m, "rtc-bank1", 0x72, 2, uam_mc146818_bank1(rtc))) != UAM_OK)
        return st;
    uam_signal_connect(uam_i8254_irq_line(pit), isa_sink, isa, 0);
    uam_signal_connect(uam_mc146818_irq_line(rtc), isa_sink, isa, 8);
    cmos_memory(rtc, low);

    /* HPET: comparators route to any I/O APIC pin, or replace IRQ 0/8 */
    uam_device *hpet = uam_hpet_create("hpet", &m->sched, TICK_HZ);
    if ((st = add(m, hpet)) != UAM_OK ||
        (st = uam_bus_map_mmio(&m->bus, "hpet", HPET_BASE, UAM_HPET_SIZE, hpet)) != UAM_OK)
        return st;
    uam_hpet_set_sinks(hpet, uam_ioapic_input, ioapic, isa_sink, isa, (1u << UAM_IOAPIC_PINS) - 1);

    /* PS/2 controller */
    uam_device *kbc = uam_i8042_create("i8042");
    if ((st = add(m, kbc)) != UAM_OK ||
        (st = map_io(m, "kbc-data", 0x60, 1, uam_i8042_data(kbc))) != UAM_OK ||
        (st = map_io(m, "kbc-cmd", 0x64, 1, uam_i8042_cmd(kbc))) != UAM_OK)
        return st;
    uam_signal_connect(uam_i8042_kbd_irq(kbc), isa_sink, isa, 1);
    uam_signal_connect(uam_i8042_aux_irq(kbc), isa_sink, isa, 12);
    uam_signal_connect(uam_i8042_reset_line(kbc), uam_machine_reset_sink, m, UAM_RESET_SYSTEM);

    /* COM1 */
    uam_device *uart = uam_uart16550_create("com1", cfg->console);
    if ((st = add(m, uart)) != UAM_OK || (st = map_io(m, "com1", 0x3F8, 8, uart)) != UAM_OK)
        return st;
    uam_signal_connect(uam_uart16550_irq_line(uart), isa_sink, isa, 4);
    if ((st = uam_board_add_uart_input(m, "com1", uart)) != UAM_OK)
        return st;

    /* PCI configuration space: an empty bus 0 behind 0xCF8/0xCFC */
    uam_pci_bus *pci = uam_pci_bus_create("pci", &m->bus, UAM_X86_PIO_BASE);
    if (!pci)
        return UAM_ERR_NOMEM;
    uam_machine_add_device(m, uam_pci_bus_device(pci));
    if ((st = map_io(m, "pci-cfg", 0xCF8, 8, uam_pci_cam(pci))) != UAM_OK)
        return st;
    uam_pci_set_intx_router(pci, pci_intx, ioapic);
    uam_pci_set_msi_sink(pci, uam_apic_bus_msi, abus);

    /* the 8237 DMA controllers */
    uam_device *dma = uam_i8237_create("dma", &m->bus);
    if ((st = add(m, dma)) != UAM_OK ||
        (st = map_io(m, "dma1", 0x00, 16, uam_i8237_dma1(dma))) != UAM_OK ||
        (st = map_io(m, "dma2", 0xC0, 32, uam_i8237_dma2(dma))) != UAM_OK ||
        (st = map_io(m, "dma-page", 0x81, 15, uam_i8237_page(dma))) != UAM_OK)
        return st;

    /* small platform registers */
    uam_device *sysctl = uam_pc_sysctl_create("sysctl");
    uam_device *post = uam_postcode_create("post");
    uam_device *test = uam_test_finisher_create("test", uam_machine_stop_cb, m);
    if ((st = add(m, sysctl)) != UAM_OK || (st = add(m, post)) != UAM_OK || (st = add(m, test)) != UAM_OK)
        return st;
    if ((st = map_io(m, "sysctl", 0x92, 1, sysctl)) != UAM_OK ||
        (st = map_io(m, "post", 0x80, 1, post)) != UAM_OK ||
        (st = map_io(m, "test", 0x700, 0x10, test)) != UAM_OK)
        return st;
    uam_signal_connect(uam_pc_sysctl_reset_line(sysctl), uam_machine_reset_sink, m, UAM_RESET_SYSTEM);
    /* A20: enabled by the 8042's output port or by port 0x92 */
    uam_device *a20 = uam_a20_gate_create("a20");
    if ((st = add(m, a20)) != UAM_OK)
        return st;
    uam_signal_connect(uam_i8042_a20_line(kbc), uam_a20_gate_input, a20, 0);
    uam_signal_connect(uam_pc_sysctl_a20_line(sysctl), uam_a20_gate_input, a20, 1);
    uam_signal_connect(uam_a20_gate_a20m_line(a20), a20m_all, b, 0);

    /* each processor's view of the platform; its APIC offers x2APIC mode and
     * the TSC-deadline timer */
    const char *smap = uam_board_config_option(cfg, "smm-map");
    uam_x86_smm_map smm_map = UAM_X86_SMM_MAP_INTEL64;
    if (smap && !strcmp(smap, "amd64"))
        smm_map = UAM_X86_SMM_MAP_AMD64;
    else if (smap && strcmp(smap, "intel64")) {
        uam_log(UAM_LOG_ERROR, "x86_64-ref", "smm-map must be intel64 or amd64, not '%s'", smap);
        return UAM_ERR_INVALID;
    }
    for (unsigned i = 0; i < b->ncpu; i++) {
        uam_cpu *cpu = uam_machine_cpu(m, i);
        uam_x86_platform plat = { .ctx = &b->pcpu[i], .inta = ref_inta, .rdmsr = ref_rdmsr,
                                  .wrmsr = ref_wrmsr };
        uam_x86_set_platform(cpu, &plat);
        uam_x86_cpuid_info id;
        uam_x86_get_cpuid(cpu, &id);
        id.features |= UAM_X86_FEAT_X2APIC | UAM_X86_FEAT_TSC_DEADLINE;
        id.logical_cpus = b->ncpu;
        id.initial_apic_id = i;
        uam_x86_set_cpuid(cpu, &id);
        uam_x86_set_smm_map(cpu, smm_map);
    }

    /* Unclaimed accesses master-abort (all ones), as on a PC, unless the user
     * asked for the zero/echo policies. */
    uam_device *um = cfg->unknown_mmio == UAM_UNKNOWN_FAULT
        ? uam_master_abort_create("master-abort")
        : uam_unknown_mmio_create("unknown-mmio", cfg->unknown_mmio == UAM_UNKNOWN_ECHO
                                                      ? UAM_UNKNOWN_POLICY_ECHO
                                                      : UAM_UNKNOWN_POLICY_ZERO);
    if ((st = add(m, um)) != UAM_OK)
        return st;
    if (cfg->unknown_mmio == UAM_UNKNOWN_FAULT) {
        uam_master_abort_name_window(um, UAM_X86_PIO_BASE, UAM_X86_PIO_SIZE, "I/O port", true);
        uam_master_abort_name_window(um, UAM_X86_SMRR_SINK, UINT64_C(1) << 32,
                                     "SMRR range outside SMM", false);
    }
    uam_bus_set_unmapped(&m->bus, um);

    uam_machine_reset(m, m->reset_pc);
    return UAM_OK;
}

const uam_board uam_board_x86_64_ref = {
    .name = "x86_64-ref",
    .description = "x86-64 PC of generic components: 8259/8254/RTC, APICs, PCI config, CFI flash",
    .build = x86_ref_build,
};
