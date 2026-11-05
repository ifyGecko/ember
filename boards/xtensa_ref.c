/*
 * xtensa_ref.c - "xtensa-ref" and "xtensaeb-ref": the Xtensa reference boards, one per
 * byte order. RAM, a 16550 UART, two general-purpose timers and a test finisher; no
 * other peripheral of any system.
 *
 * -o model= picks the core (uam/isa/xtensa.h): lx106, lx6 (default) or lx7. -o cpus=N
 * (1..4) gives N processors, PRID 0xCDCD, 0xABAB, then 0xABAC, ... (the ESP32's two
 * cores answer 0xCDCD and 0xABAB); all start at the reset vector.
 *
 * Memory:
 *   0x40000000  RAM (--ram sets its size, default 4 MiB). The models' vectors sit at its
 *               start (VECBASE resets to 0x40000000), and so do their reset vectors:
 *               0x40000080 on the LX106, 0x40000400 on the LX6 and LX7. An ELF image
 *               starts at its entry.
 *   0x60000000  16550 UART, the console ("uart0" input route): interrupt 5 (level 1)
 *   0x60001000  timer 0 (uam/dev/gptimer.h): interrupt 10 (edge, level 1)
 *   0x60002000  timer 1: interrupt 14, the NMI
 *   0x60003000  test finisher (uam/dev/test_finisher.h)
 * The interrupt numbers are those both interrupt tables configure alike. CCOUNT and the
 * CCOMPARE timers are the core's own. On xtensaeb-ref the devices' registers are
 * big-endian, as the core is.
 */
#include "uam/boards.h"
#include <stdlib.h>
#include <string.h>

#include "uam/dev/gptimer.h"
#include "uam/dev/test_finisher.h"
#include "uam/dev/uart16550.h"
#include "uam/dev/unknown_mmio.h"
#include "uam/isa/xtensa.h"
#include "uam/machine.h"

#define RAM_BASE   0x40000000u
#define RAM_SIZE   (4u << 20)
#define UART_BASE  0x60000000u
#define TIMER0_BASE 0x60001000u
#define TIMER1_BASE 0x60002000u
#define TEST_BASE  0x60003000u
#define UART_IRQ   5u
#define TIMER0_IRQ 10u
#define NMI_IRQ    14u
#define MAX_CPUS   4u

/* Map a device; its registers in the core's byte order (the timers' are little-endian). */
static uam_status attach(uam_machine *m, uam_device *dev, uint64_t base, uint64_t size)
{
    if (!dev)
        return UAM_ERR_NOMEM;
    uam_machine_add_device(m, dev);
    uam_map_attrs attrs = { .endian = m->cpu.isa->default_endian };
    return uam_bus_map_mmio_ex(&m->bus, dev->name, base, size, dev, &attrs);
}

static uam_status build(uam_machine *m, const uam_board_config *cfg, const uam_isa *isa,
                        const char *name)
{
    const char *mname = uam_board_config_option(cfg, "model");
    const uam_xtensa_config *model = !mname || strcmp(mname, "lx6") == 0 ? &uam_xtensa_lx6
                                     : strcmp(mname, "lx106") == 0       ? &uam_xtensa_lx106
                                     : strcmp(mname, "lx7") == 0         ? &uam_xtensa_lx7
                                                                         : NULL;
    if (!model) {
        uam_log(UAM_LOG_ERROR, name, "model=%s: lx106, lx6 or lx7", mname);
        return UAM_ERR_INVALID;
    }
    const char *ncpu_opt = uam_board_config_option(cfg, "cpus");
    unsigned ncpu = ncpu_opt ? (unsigned)strtoul(ncpu_opt, NULL, 0) : 1;
    if (ncpu < 1 || ncpu > MAX_CPUS) {
        uam_log(UAM_LOG_ERROR, name, "cpus=%s: 1..%u", ncpu_opt, MAX_CPUS);
        return UAM_ERR_INVALID;
    }

    uam_status st = uam_machine_init(m, isa);
    if (st != UAM_OK)
        return st;
    m->board_name = name;
    m->reset_pc = model->reset_vector;
    if ((st = uam_machine_add_cpus(m, ncpu - 1)) != UAM_OK)
        return st;
    for (unsigned i = 0; i < ncpu; i++) {
        uam_cpu *cpu = uam_machine_cpu(m, i);
        if ((st = uam_xtensa_configure(cpu, model)) != UAM_OK)
            return st;
        uam_xtensa_set_prid(cpu, i == 0 ? 0xCDCDu : 0xABAAu + i);
    }

    if ((st = uam_bus_map_ram(&m->bus, "ram", RAM_BASE, cfg->ram_size ? cfg->ram_size : RAM_SIZE)) != UAM_OK)
        return st;
    uam_device *uart = uam_uart16550_create("uart0", cfg->console);
    uam_device *t0 = uam_gptimer_create("timer0", &m->sched);
    uam_device *t1 = uam_gptimer_create("timer1", &m->sched);
    uam_device *test = uam_test_finisher_create("test", uam_machine_stop_cb, m);
    if ((st = attach(m, uart, UART_BASE, UAM_UART16550_SIZE)) != UAM_OK
        || (st = attach(m, t0, TIMER0_BASE, UAM_GPTIMER_SIZE)) != UAM_OK
        || (st = attach(m, t1, TIMER1_BASE, UAM_GPTIMER_SIZE)) != UAM_OK
        || (st = attach(m, test, TEST_BASE, UAM_TEST_FINISHER_SIZE)) != UAM_OK)
        return st;

    /* The devices interrupt the first processor. */
    uam_cpu *cpu0 = uam_machine_cpu(m, 0);
    uam_signal_connect(uam_uart16550_irq_line(uart), uam_cpu_irq_sink, cpu0, UART_IRQ);
    uam_signal_connect(uam_gptimer_irq(t0), uam_cpu_irq_sink, cpu0, TIMER0_IRQ);
    uam_signal_connect(uam_gptimer_irq(t1), uam_cpu_irq_sink, cpu0, NMI_IRQ);
    if ((st = uam_board_add_uart_input(m, "uart0", uart)) != UAM_OK)
        return st;

    if (cfg->unknown_mmio != UAM_UNKNOWN_FAULT) {
        uam_device *um = uam_unknown_mmio_create(
            "unknown-mmio", cfg->unknown_mmio == UAM_UNKNOWN_ECHO ? UAM_UNKNOWN_POLICY_ECHO
                                                                  : UAM_UNKNOWN_POLICY_ZERO);
        if (!um)
            return UAM_ERR_NOMEM;
        uam_machine_add_device(m, um);
        uam_bus_set_unmapped(&m->bus, um);
    }

    uam_machine_reset(m, m->reset_pc);
    return UAM_OK;
}

static uam_status xtensa_ref_build(uam_machine *m, const uam_board_config *cfg)
{
    return build(m, cfg, &uam_isa_xtensa, "xtensa-ref");
}

static uam_status xtensaeb_ref_build(uam_machine *m, const uam_board_config *cfg)
{
    return build(m, cfg, &uam_isa_xtensaeb, "xtensaeb-ref");
}

const uam_board uam_board_xtensa_ref = {
    .name = "xtensa-ref",
    .description = "Xtensa reference board, little-endian (-o model=lx106|lx6|lx7, -o cpus=N; "
                   "RAM, a 16550, two timers and a test finisher)",
    .build = xtensa_ref_build,
};

const uam_board uam_board_xtensaeb_ref = {
    .name = "xtensaeb-ref",
    .description = "Xtensa reference board, big-endian (-o model=lx106|lx6|lx7, -o cpus=N; "
                   "RAM, a 16550, two timers and a test finisher)",
    .build = xtensaeb_ref_build,
};
