/*
 * x86_64_ref.c - "x86_64-ref": the x86-64 reference board.
 *
 * A single-processor PC made only of ember's generic components, for testing
 * the ISA and the standard PC devices. It has no chipset: platforms that need
 * one (memory-controller registers, SMRAM, ACPI and SMI logic) build their own
 * board on these components out of tree.
 *
 * Memory map:
 *   0x00000000 - low_top        RAM (up to 2 GiB below 4 GiB)
 *   0x100000000 -               the rest of RAM
 *   0xFEC00000                  I/O APIC
 *   0xFED00000                  HPET
 *   0xFEE00000                  local APIC (relocatable via IA32_APIC_BASE)
 *   4 GiB - flash size - 4 GiB  boot flash (CFI): images "flash", or "vars" + "code"
 * I/O ports (the aperture at UAM_X86_PIO_BASE):
 *   0x20/0xA0/0x4D0 8259 + ELCR, 0x40-0x43/0x61 8254, 0x60/0x64 8042 PS/2,
 *   0x70-0x73 RTC/CMOS,
 *   0x80 POST codes, 0x92 system control A (and, with the 8042's output
 *   port, the A20 gate), 0xB2 software SMI (any write
 *   raises SMI#), 0x3F8 COM1, 0xCF8-0xCFF PCI
 *   configuration, 0x700 test finisher, the rest
 *   RAZ/WI.
 * PCI bus 0: an AHCI controller at 00:02.0 when a disk or disc is given, an
 * NVMe controller at 00:03.0 when an NVMe image is.
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
 *   -o debugcon=PORT   debug console port writing to the console
 *   -o cpus=N          processors (default 1; up to 64): one local APIC each
 *                      (APIC ID = index), the others wait for a startup IPI
 *   -o apic-hz=HZ      local APIC timer input clock
 *   -o persist=1       write flash programs/erases back to the image files
 *   -o disk=PATH       ATA disk image on AHCI port 0 (-o disk-mode=rw|ro|volatile)
 *   -o cdrom=PATH      ATAPI disc image on AHCI port 1 (read-only)
 *   -o nvme=PATH       NVMe namespace 1 image (-o nvme-mode=rw|ro|volatile)
 *   -o ide0=PATH       ATA disk on the IDE primary master, -o ide1=PATH on the
 *                      primary slave, -o idecd=PATH an ATAPI disc on the
 *                      secondary master (-o <key>-mode=...); the controller
 *                      (PCI 00:04.0, compatibility mode) decodes 0x1F0/0x3F6
 *                      (IRQ 14) and 0x170/0x376 (IRQ 15)
 *   -o floppy=PATH     floppy image in drive A: (-o floppyb=PATH: drive B:),
 *                      82077 controller at 0x3F0-0x3F5/0x3F7, IRQ 6, DMA 2
 *   -o vga=1           VGA at 0x3B0-0x3DF and 0xA0000-0xBFFFF (PCI 00:05.0);
 *                      --image vgabios=PATH is its BIOS, at 0xC0000 and
 *                      behind the function's expansion ROM BAR
 *   -o vgadump=1       print the VGA text screen when the machine is torn down
 *   -o lpt=1           parallel port at 0x378 (IRQ 7) printing to the console
 * Always: the two 8237 DMA controllers (0x00-0x0F, 0xC0-0xDF, pages at
 * 0x81-0x8F).
 *
 * --image bios=PATH is a legacy BIOS ROM (up to 1 MiB): it ends at 4 GiB, and
 * its last 128 KiB (all of it if smaller) also appear just below 1 MiB, the
 * way PC chipsets decode the boot ROM; execution starts at the reset vector.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uam/boards.h"
#include "uam/dev/ahci.h"
#include "uam/dev/apic.h"
#include "uam/dev/cfi_flash.h"
#include "uam/dev/hpet.h"
#include "uam/dev/fdc.h"
#include "uam/dev/i8042.h"
#include "uam/dev/i8237.h"
#include "uam/dev/ide.h"
#include "uam/dev/lpt.h"
#include "uam/dev/vga.h"
#include "uam/dev/nvme.h"
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
/* PCI functions of the board carry a vendor ID no vendor is assigned:
 * drivers bind them by class code. */
#define REF_PCI_VENDOR 0xFFFEu
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
    uam_blockdev *disk, *cdrom, *nvme, *ide[3], *floppy[2];
    uam_device   *vga;
    bool          vgadump;
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

/* The software SMI port reaches every processor's SMI#. */
static void smi_all(void *ctx, unsigned input, int level)
{
    (void)input;
    ref_board *b = ctx;
    for (unsigned i = 0; i < b->ncpu; i++)
        uam_cpu_irq_sink(uam_machine_cpu(b->m, i), UAM_X86_IRQ_SMI, level);
}

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
    ref_board *b = m->board_data;
    if (b) {
        if (b->vga && b->vgadump)
            uam_vga_text_dump(b->vga, stderr);
        uam_blockdev_close(b->disk);
        uam_blockdev_close(b->cdrom);
        uam_blockdev_close(b->nvme);
        for (unsigned i = 0; i < 3; i++)
            uam_blockdev_close(b->ide[i]);
        for (unsigned i = 0; i < 2; i++)
            uam_blockdev_close(b->floppy[i]);
    }
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

/* Boot flash ending at 4 GiB: "flash" alone, or "vars" below "code". */
static uam_status map_flash(uam_machine *m, const uam_board_config *cfg)
{
    const char *names[2] = { uam_board_config_image(cfg, "code"), uam_board_config_image(cfg, "vars") };
    const char *whole = uam_board_config_image(cfg, "flash");
    uint64_t top = UINT64_C(0x100000000);
    if (whole) {
        names[0] = whole;
        names[1] = NULL;
    }
    for (unsigned i = 0; i < 2; i++) {
        if (!names[i])
            continue;
        uint8_t *data;
        size_t len;
        uam_status st = uam_load_file(names[i], &data, &len);
        if (st != UAM_OK)
            return st;
        uint64_t size = (len + FLASH_BLOCK - 1) & ~(FLASH_BLOCK - 1);
        if (!size || size > (UINT64_C(16) << 20)) {
            free(data);
            uam_log(UAM_LOG_ERROR, "x86_64-ref", "%s: flash image size %zu not supported", names[i], len);
            return UAM_ERR_INVALID;
        }
        top -= size;
        uam_device *f = uam_cfi_flash_map(&m->bus, i == 0 ? "flash-code" : "flash-vars", top, size,
                                          FLASH_BLOCK, data, len);
        free(data);
        if (!f)
            return UAM_ERR_NOMEM;
        uam_machine_add_device(m, f);
        const char *persist = uam_board_config_option(cfg, "persist");
        if (persist && strcmp(persist, "0") != 0 &&
            (st = uam_cfi_flash_set_backing_file(f, names[i])) != UAM_OK)
            return st;
    }
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

    /* storage */
    if ((st = uam_board_open_disk(cfg, "disk", 512, &b->disk)) != UAM_OK ||
        (st = uam_board_open_disk(cfg, "cdrom", 2048, &b->cdrom)) != UAM_OK)
        return st;
    if (b->disk || b->cdrom) {
        uam_device *ahci = uam_ahci_create("ahci", 2, REF_PCI_VENDOR, 0x0001, 0);
        if ((st = add(m, ahci)) != UAM_OK ||
            (st = uam_pci_register(pci, uam_ahci_func(ahci), 2, 0)) != UAM_OK)
            return st;
        if ((b->disk && (st = uam_ahci_attach(ahci, 0, b->disk, false)) != UAM_OK) ||
            (b->cdrom && (st = uam_ahci_attach(ahci, 1, b->cdrom, true)) != UAM_OK))
            return st;
    }
    if ((st = uam_board_open_disk(cfg, "nvme", 512, &b->nvme)) != UAM_OK)
        return st;
    if (b->nvme) {
        uam_device *nvme = uam_nvme_create("nvme", REF_PCI_VENDOR, 0x0002, 0);
        if ((st = add(m, nvme)) != UAM_OK ||
            (st = uam_pci_register(pci, uam_nvme_func(nvme), 3, 0)) != UAM_OK ||
            (st = uam_nvme_attach(nvme, 1, b->nvme)) != UAM_OK)
            return st;
    }

    /* legacy storage: DMA controllers, IDE, floppy */
    uam_device *dma = uam_i8237_create("dma", &m->bus);
    if ((st = add(m, dma)) != UAM_OK ||
        (st = map_io(m, "dma1", 0x00, 16, uam_i8237_dma1(dma))) != UAM_OK ||
        (st = map_io(m, "dma2", 0xC0, 32, uam_i8237_dma2(dma))) != UAM_OK ||
        (st = map_io(m, "dma-page", 0x81, 15, uam_i8237_page(dma))) != UAM_OK)
        return st;
    static const char *const ide_keys[3] = { "ide0", "ide1", "idecd" };
    for (unsigned i = 0; i < 3; i++)
        if ((st = uam_board_open_disk(cfg, ide_keys[i], i == 2 ? 2048 : 512, &b->ide[i])) != UAM_OK)
            return st;
    if (b->ide[0] || b->ide[1] || b->ide[2]) {
        uam_device *ide = uam_ide_create("ide", REF_PCI_VENDOR, 0x0003, 0);
        if ((st = add(m, ide)) != UAM_OK ||
            (st = uam_pci_register(pci, uam_ide_func(ide), 4, 0)) != UAM_OK ||
            (st = map_io(m, "ide0", 0x1F0, 8, uam_ide_cmd(ide, 0))) != UAM_OK ||
            (st = map_io(m, "ide0-ctl", 0x3F6, 1, uam_ide_ctl(ide, 0))) != UAM_OK ||
            (st = map_io(m, "ide1", 0x170, 8, uam_ide_cmd(ide, 1))) != UAM_OK ||
            (st = map_io(m, "ide1-ctl", 0x376, 1, uam_ide_ctl(ide, 1))) != UAM_OK)
            return st;
        if ((b->ide[0] && (st = uam_ide_attach(ide, 0, 0, b->ide[0], false)) != UAM_OK) ||
            (b->ide[1] && (st = uam_ide_attach(ide, 0, 1, b->ide[1], false)) != UAM_OK) ||
            (b->ide[2] && (st = uam_ide_attach(ide, 1, 0, b->ide[2], true)) != UAM_OK))
            return st;
        uam_signal_connect(uam_ide_irq_line(ide, 0), isa_sink, isa, 14);
        uam_signal_connect(uam_ide_irq_line(ide, 1), isa_sink, isa, 15);
    }
    static const char *const fd_keys[2] = { "floppy", "floppyb" };
    for (unsigned i = 0; i < 2; i++)
        if ((st = uam_board_open_disk(cfg, fd_keys[i], 512, &b->floppy[i])) != UAM_OK)
            return st;
    if (b->floppy[0] || b->floppy[1]) {
        uam_device *fdc = uam_fdc_create("fdc", dma, 2);
        if ((st = add(m, fdc)) != UAM_OK ||
            (st = map_io(m, "fdc", 0x3F0, 6, uam_fdc_regs(fdc))) != UAM_OK ||
            (st = map_io(m, "fdc-dir", 0x3F7, 1, uam_fdc_dir(fdc))) != UAM_OK)
            return st;
        for (unsigned i = 0; i < 2; i++)
            if (b->floppy[i] && (st = uam_fdc_insert(fdc, i, b->floppy[i], false)) != UAM_OK) {
                uam_log(UAM_LOG_ERROR, "x86_64-ref", "%s: not a floppy image size", fd_keys[i]);
                return st;
            }
        uam_signal_connect(uam_fdc_irq_line(fdc), isa_sink, isa, 6);
    }

    /* display and printer */
    const char *vgaopt = uam_board_config_option(cfg, "vga");
    const char *vgabios = uam_board_config_image(cfg, "vgabios");
    if ((vgaopt && strcmp(vgaopt, "0") != 0) || vgabios) {
        uam_device *vga = uam_vga_create("vga", REF_PCI_VENDOR, 0x0004, 0);
        if ((st = add(m, vga)) != UAM_OK ||
            (st = map_io(m, "vga", 0x3B0, UAM_VGA_IO_SIZE, uam_vga_io(vga))) != UAM_OK ||
            (st = uam_bus_map_mmio_ex(&m->bus, "vga-mem", 0xA0000, UAM_VGA_MEM_SIZE, uam_vga_mem(vga),
                                      &(uam_map_attrs){ .prio = 1 })) != UAM_OK)
            return st;
        if (vgabios) {
            uint8_t *data;
            size_t len;
            if ((st = uam_load_file(vgabios, &data, &len)) != UAM_OK)
                return st;
            uint64_t romsz = 2048;
            while (romsz < len)
                romsz <<= 1;
            uam_device *img = uam_pci_rom_image_create("vgabios", data, len);
            st = img ? uam_bus_map_rom(&m->bus, "vgabios-isa", 0xC0000, (len + 511) & ~511ull, data, len)
                     : UAM_ERR_NOMEM;
            free(data);
            if (st != UAM_OK || (st = add(m, img)) != UAM_OK)
                return st;
            uam_pci_func_rom(uam_vga_func(vga), romsz, img);
        }
        if ((st = uam_pci_register(pci, uam_vga_func(vga), 5, 0)) != UAM_OK)
            return st;
        b->vga = vga;
        const char *dump = uam_board_config_option(cfg, "vgadump");
        b->vgadump = dump && strcmp(dump, "0") != 0;
    }
    const char *lptopt = uam_board_config_option(cfg, "lpt");
    if (lptopt && strcmp(lptopt, "0") != 0) {
        uam_device *lpt = uam_lpt_create("lpt1", cfg->console);
        if ((st = add(m, lpt)) != UAM_OK || (st = map_io(m, "lpt1", 0x378, 3, lpt)) != UAM_OK)
            return st;
        uam_signal_connect(uam_lpt_irq_line(lpt), isa_sink, isa, 7);
    }

    /* small platform registers */
    uam_device *sysctl = uam_pc_sysctl_create("sysctl");
    uam_device *post = uam_postcode_create("post");
    uam_device *test = uam_test_finisher_create("test", uam_machine_stop_cb, m);
    uam_device *smip = uam_smi_port_create("smi-port");
    if ((st = add(m, sysctl)) != UAM_OK || (st = add(m, post)) != UAM_OK || (st = add(m, test)) != UAM_OK ||
        (st = add(m, smip)) != UAM_OK)
        return st;
    uam_signal_connect(uam_smi_port_line(smip), smi_all, b, 0);
    if ((st = map_io(m, "sysctl", 0x92, 1, sysctl)) != UAM_OK ||
        (st = map_io(m, "smi-port", 0xB2, 1, smip)) != UAM_OK ||
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
    const char *dc = uam_board_config_option(cfg, "debugcon");
    if (dc) {
        uam_device *con = uam_debugcon_create("debugcon", cfg->console, 0xFF);
        if ((st = add(m, con)) != UAM_OK ||
            (st = map_io(m, "debugcon", strtoull(dc, NULL, 0) & 0xFFFF, 1, con)) != UAM_OK)
            return st;
    }

    /* each processor's view of the platform; its APIC offers x2APIC mode and
     * the TSC-deadline timer */
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
    uam_bus_set_unmapped(&m->bus, um);

    uam_machine_reset(m, m->reset_pc);
    return UAM_OK;
}

const uam_board uam_board_x86_64_ref = {
    .name = "x86_64-ref",
    .description = "x86-64 PC of generic components: 8259/8254/RTC, APICs, PCI config, CFI flash",
    .build = x86_ref_build,
};
