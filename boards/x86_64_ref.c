/*
 * x86_64_ref.c - "x86_64-ref": the x86-64 reference board, a single-processor
 * PC built on a Q35-class chipset (MCH + ICH9) model.
 *
 * Memory map:
 *   0x00000000 - low_top        RAM (up to 2 GiB below 4 GiB)
 *   0x100000000 -               the rest of RAM
 *   0xFEC00000                  I/O APIC
 *   0xFEE00000                  local APIC (relocatable via IA32_APIC_BASE)
 *   4 GiB - flash size - 4 GiB  boot flash (CFI): images "flash", or "vars" + "code"
 *   PCIEXBAR, PMBASE, RCBA      where firmware programs them (chipset model)
 * I/O ports (the aperture at UAM_X86_PIO_BASE):
 *   0x20/0xA0/0x4D0 8259 + ELCR, 0x40-0x43/0x61 8254, 0x70-0x73 RTC/CMOS,
 *   0x80 POST codes, 0x92 system control A, 0xB2-0xB3 APM, 0x3F8 COM1,
 *   0xCF8-0xCFF PCI configuration (0xCF9 reset control), 0x700 test finisher
 *   (sifive,test protocol; for bring-up images), the rest RAZ/WI.
 * Memory no device claims reads as all ones (master abort).
 * Interrupts: ISA IRQs go to the 8259 and to the I/O APIC (IRQ 0 on pin 2);
 * the 8259 feeds the local APIC's LINT0; the local APIC drives the processor.
 *
 * Virtual time: one tick (instruction) is one nanosecond. The local APIC
 * timer counts at `apic-hz` (option, default 1 GHz).
 *
 * Without a flash image the board still runs a flat image loaded into RAM
 * (--raw-base 0 --entry 0), entered in real mode.
 *
 * Options:
 *   -o debugcon=PORT   debug console port writing to the console
 *   -o apic-hz=HZ      local APIC timer input clock
 */
#include <stdlib.h>
#include <string.h>

#include "uam/boards.h"
#include "uam/dev/apic.h"
#include "uam/dev/cfi_flash.h"
#include "uam/dev/i8254.h"
#include "uam/dev/i8259.h"
#include "uam/dev/mc146818.h"
#include "uam/dev/pc_misc.h"
#include "uam/dev/pci.h"
#include "uam/dev/q35.h"
#include "uam/dev/sifive_test.h"
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
#define FLASH_BLOCK    UINT64_C(0x1000)
#define RTC_EPOCH      UINT64_C(1767225600)   /* 2026-01-01 00:00:00 UTC */

/* ISA interrupt: to the 8259 and to the I/O APIC (IRQ 0 is wired to pin 2). */
typedef struct isa_irq { uam_device *pic, *ioapic; } isa_irq;

typedef struct pc_board {
    uam_machine *m;
    uam_device  *lapic, *pic, *mch;
    uam_region  *lapic_region;
    isa_irq      isa;
} pc_board;

static uint64_t pio(uint64_t port) { return UAM_X86_PIO_BASE + port; }

/* ---- platform hooks for the processor ------------------------------------ */

static int pc_inta(void *ctx)
{
    pc_board *b = ctx;
    return uam_lapic_inta(b->lapic);
}

static bool pc_wrmsr(void *ctx, uint32_t msr, uint64_t val)
{
    pc_board *b = ctx;
    if (msr == 0x1B) {   /* IA32_APIC_BASE: move and enable the register page */
        bool en = (val >> 11) & 1;
        uam_bus_region_move(&b->m->bus, b->lapic_region, val & 0xFFFFFF000ull);
        uam_bus_region_enable(&b->m->bus, b->lapic_region, en);
        uam_lapic_set_enabled(b->lapic, en);
    }
    return false;
}

static void pc_smm_changed(void *ctx, bool in_smm)
{
    pc_board *b = ctx;
    uam_q35_mch_smm(b->mch, in_smm);
}

static int pic_inta(void *ctx) { return uam_i8259_inta(ctx); }

static void power_off(void *ctx) { uam_machine_stop_cb(ctx, 0); }

static void isa_sink(void *ctx, unsigned irq, int level)
{
    isa_irq *w = ctx;
    uam_i8259_input(w->pic, irq, level);
    uam_ioapic_input(w->ioapic, irq == 0 ? 2 : irq, level);
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

/* Boot flash ending at 4 GiB: "flash" alone, or "vars" below "code". */
static uam_status map_flash(uam_machine *m, const uam_board_config *cfg, uam_region **boot)
{
    const char *names[2] = { uam_board_config_image(cfg, "code"), uam_board_config_image(cfg, "vars") };
    const char *whole = uam_board_config_image(cfg, "flash");
    uint64_t top = UINT64_C(0x100000000);
    *boot = NULL;
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
        if (i == 0)
            *boot = uam_cfi_flash_region(f);
    }
    return UAM_OK;
}

/* CMOS memory-size bytes, as PC firmware reads them. */
static void cmos_memory(uam_device *rtc, uint64_t low, uint64_t high)
{
    uint64_t ext_kb = low > (1u << 20) ? (low - (1u << 20)) >> 10 : 0;
    if (ext_kb > 0xFFFF) ext_kb = 0xFFFF;
    uam_mc146818_nvram_set(rtc, 0x15, 0x80);   /* 640 KiB base memory */
    uam_mc146818_nvram_set(rtc, 0x16, 0x02);
    uam_mc146818_nvram_set(rtc, 0x17, (uint8_t)ext_kb);
    uam_mc146818_nvram_set(rtc, 0x18, (uint8_t)(ext_kb >> 8));
    uam_mc146818_nvram_set(rtc, 0x30, (uint8_t)ext_kb);
    uam_mc146818_nvram_set(rtc, 0x31, (uint8_t)(ext_kb >> 8));
    uint64_t above16 = low > (16u << 20) ? (low - (16u << 20)) >> 16 : 0;
    uam_mc146818_nvram_set(rtc, 0x34, (uint8_t)above16);
    uam_mc146818_nvram_set(rtc, 0x35, (uint8_t)(above16 >> 8));
    uint64_t above4g = high >> 16;
    uam_mc146818_nvram_set(rtc, 0x5B, (uint8_t)above4g);
    uam_mc146818_nvram_set(rtc, 0x5C, (uint8_t)(above4g >> 8));
    uam_mc146818_nvram_set(rtc, 0x5D, (uint8_t)(above4g >> 16));
}

static uam_status x86_ref_build(uam_machine *m, const uam_board_config *cfg)
{
    uam_status st = uam_machine_init(m, &uam_isa_x86_64);
    if (st != UAM_OK)
        return st;
    m->board_name = "x86_64-ref";
    m->reset_pc = 0xFFFFFFF0ull;

    pc_board *b = calloc(1, sizeof(*b));
    if (!b)
        return UAM_ERR_NOMEM;
    b->m = m;
    m->board_data = b;
    m->board_fini = board_fini;
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

    uam_region *boot = NULL;
    if ((st = map_flash(m, cfg, &boot)) != UAM_OK)
        return st;

    /* interrupt controllers */
    uam_device *pic = uam_i8259_create("pic");
    const char *ahz = uam_board_config_option(cfg, "apic-hz");
    uint64_t apic_hz = ahz ? strtoull(ahz, NULL, 0) : 1000000000u;
    uam_device *lapic = uam_lapic_create("lapic", &m->sched, TICK_HZ, apic_hz, 0);
    uam_device *ioapic = uam_ioapic_create("ioapic", 0);
    if ((st = add(m, pic)) != UAM_OK || (st = add(m, lapic)) != UAM_OK || (st = add(m, ioapic)) != UAM_OK)
        return st;
    b->pic = pic;
    b->lapic = lapic;
    isa->pic = pic;
    isa->ioapic = ioapic;
    if ((st = map_io(m, "pic-master", 0x20, 2, uam_i8259_master(pic))) != UAM_OK ||
        (st = map_io(m, "pic-slave", 0xA0, 2, uam_i8259_slave(pic))) != UAM_OK ||
        (st = map_io(m, "elcr", 0x4D0, 2, uam_i8259_elcr(pic))) != UAM_OK ||
        (st = uam_bus_map_mmio(&m->bus, "lapic", LAPIC_BASE, UAM_LAPIC_SIZE, lapic)) != UAM_OK ||
        (st = uam_bus_map_mmio(&m->bus, "ioapic", IOAPIC_BASE, UAM_IOAPIC_SIZE, ioapic)) != UAM_OK)
        return st;
    b->lapic_region = uam_bus_region(&m->bus, "lapic");
    uam_signal_connect(uam_i8259_intr_line(pic), uam_lapic_lint, lapic, 0);
    uam_lapic_set_extint(lapic, pic_inta, pic);
    uam_ioapic_set_target(ioapic, uam_lapic_deliver, lapic);
    uam_lapic_set_eoi_hook(lapic, uam_ioapic_eoi, ioapic);
    uam_signal_connect(uam_lapic_intr_line(lapic), uam_cpu_irq_sink, &m->cpu, UAM_X86_IRQ_INTR);
    uam_signal_connect(uam_lapic_nmi_line(lapic), uam_cpu_irq_sink, &m->cpu, UAM_X86_IRQ_NMI);

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
    cmos_memory(rtc, low, high);

    /* COM1 */
    uam_device *uart = uam_uart16550_create("com1", cfg->console);
    if ((st = add(m, uart)) != UAM_OK || (st = map_io(m, "com1", 0x3F8, 8, uart)) != UAM_OK)
        return st;
    uam_signal_connect(uam_uart16550_irq_line(uart), isa_sink, isa, 4);
    if ((st = uam_board_add_uart_input(m, "com1", uart)) != UAM_OK)
        return st;

    /* chipset: PCI, MCH, ICH9 */
    uam_pci_bus *pci = uam_pci_bus_create("pci", &m->bus, UAM_X86_PIO_BASE);
    if (!pci)
        return UAM_ERR_NOMEM;
    uam_machine_add_device(m, uam_pci_bus_device(pci));
    if ((st = map_io(m, "pci-cfg", 0xCF8, 8, uam_pci_cam(pci))) != UAM_OK)
        return st;
    uam_q35_mch_config mc = { .sys = &m->bus, .pci = pci, .low_ram_top = low, .boot_flash = boot };
    uam_device *mch = uam_q35_mch_create("mch", &mc);
    uam_device *lpc = uam_ich9_lpc_create("lpc", pci, &m->bus, &m->sched, TICK_HZ, UAM_X86_PIO_BASE);
    if ((st = add(m, mch)) != UAM_OK || (st = add(m, lpc)) != UAM_OK)
        return st;
    b->mch = mch;
    if ((st = map_io(m, "apm", 0xB2, 2, uam_ich9_apm(lpc))) != UAM_OK)
        return st;
    uam_pci_set_cf9_hook(pci, uam_ich9_reset_control, lpc);
    uam_ich9_set_irq_targets(lpc, uam_i8259_input, pic, uam_ioapic_input, ioapic);
    uam_ich9_set_poweroff(lpc, power_off, m);
    uam_signal_connect(uam_ich9_smi_line(lpc), uam_cpu_irq_sink, &m->cpu, UAM_X86_IRQ_SMI);
    uam_signal_connect(uam_ich9_sci_line(lpc), isa_sink, isa, 9);
    uam_signal_connect(uam_ich9_reset_line(lpc), uam_machine_reset_sink, m, UAM_RESET_SYSTEM);

    /* small platform registers */
    uam_device *sysctl = uam_pc_sysctl_create("sysctl");
    uam_device *post = uam_postcode_create("post");
    uam_device *test = uam_sifive_test_create("test", uam_machine_stop_cb, m);
    if ((st = add(m, sysctl)) != UAM_OK || (st = add(m, post)) != UAM_OK || (st = add(m, test)) != UAM_OK)
        return st;
    if ((st = map_io(m, "sysctl", 0x92, 1, sysctl)) != UAM_OK ||
        (st = map_io(m, "post", 0x80, 1, post)) != UAM_OK ||
        (st = map_io(m, "test", 0x700, 0x10, test)) != UAM_OK)
        return st;
    uam_signal_connect(uam_pc_sysctl_reset_line(sysctl), uam_machine_reset_sink, m, UAM_RESET_SYSTEM);
    const char *dc = uam_board_config_option(cfg, "debugcon");
    if (dc) {
        uam_device *con = uam_debugcon_create("debugcon", cfg->console, 0xFF);
        if ((st = add(m, con)) != UAM_OK ||
            (st = map_io(m, "debugcon", strtoull(dc, NULL, 0) & 0xFFFF, 1, con)) != UAM_OK)
            return st;
    }

    /* the processor's view of the platform */
    uam_x86_platform plat = { .ctx = b, .inta = pc_inta, .wrmsr = pc_wrmsr,
                              .smm_changed = pc_smm_changed };
    uam_x86_set_platform(&m->cpu, &plat);

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
    .description = "x86-64 PC: Q35-class chipset (MCH, ICH9), 8259/8254/RTC, APICs, CFI boot flash",
    .build = x86_ref_build,
};
