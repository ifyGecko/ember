/*
 * m6502_ref.c - "6502-ref": the 6502 reference board. RAM, ROM, a 16550 console, two
 * general-purpose timers and a simulation finisher in one I/O page; no chip of any
 * 6502 computer.
 *
 * -o model= picks the core (uam/isa/m6502.h):
 *
 *   model  core
 *   nmos   uam_m6502_nmos     NMOS 6502 with its undefined opcodes (default)
 *   2a03   uam_m6502_2a03     the same without decimal mode
 *   65c02  uam_m6502_w65c02   WDC W65C02S
 *
 * Memory:
 *   0x0000-0x7EFF  RAM (zero page, stack, data; --ram sets its size, at most 0x7F00)
 *   0x7F00         NS16550A UART (byte registers), the console: IRQ line 0
 *   0x7F10         test finisher (byte commands: uam/dev/test_finisher.h)
 *   0x7F20         general-purpose timer 0 (uam/dev/gptimer.h): IRQ line 1
 *   0x7F30         general-purpose timer 1: the NMI
 *   0x8000-0xFFFF  ROM, the vectors at its top. A raw image is loaded at 0x8000
 *                  (--raw-base moves it); reset starts at the reset vector.
 * The timers' flags stay set until the program clears them (STATUS), as a 6502
 * peripheral's do: the core does not acknowledge interrupts. A 6502 has one
 * processor: -o cpus is 1.
 */
#include "uam/boards.h"
#include <stdlib.h>
#include <string.h>

#include "uam/dev/gptimer.h"
#include "uam/dev/test_finisher.h"
#include "uam/dev/uart16550.h"
#include "uam/dev/unknown_mmio.h"
#include "uam/isa/m6502.h"
#include "uam/machine.h"

#define IO_BASE   0x7F00u
#define ROM_BASE  0x8000u
#define UART_LINE  0u
#define TIMER_LINE 1u

typedef struct m6502_model {
    const char             *name;
    const uam_m6502_config *cfg;
} m6502_model;

static const m6502_model models[] = {
    { "nmos",  &uam_m6502_nmos },
    { "2a03",  &uam_m6502_2a03 },
    { "65c02", &uam_m6502_w65c02 },
};

static uam_status add(uam_machine *m, uam_device *d)
{
    if (!d)
        return UAM_ERR_NOMEM;
    uam_machine_add_device(m, d);
    return UAM_OK;
}

static uam_status m6502_ref_build(uam_machine *m, const uam_board_config *cfg)
{
    const char *mname = uam_board_config_option(cfg, "model");
    const m6502_model *md = NULL;
    for (size_t i = 0; i < UAM_ARRAY_LEN(models); i++)
        if (!mname ? strcmp(models[i].name, "nmos") == 0 : strcmp(models[i].name, mname) == 0)
            md = &models[i];
    if (!md) {
        uam_log(UAM_LOG_ERROR, "6502-ref", "model=%s: nmos, 2a03 or 65c02", mname);
        return UAM_ERR_INVALID;
    }
    const char *ncpu = uam_board_config_option(cfg, "cpus");
    if (ncpu && strtoul(ncpu, NULL, 0) != 1) {
        uam_log(UAM_LOG_ERROR, "6502-ref", "cpus=%s: a 6502 has one processor", ncpu);
        return UAM_ERR_INVALID;
    }
    if (cfg->ram_size > IO_BASE) {
        uam_log(UAM_LOG_ERROR, "6502-ref", "--ram: at most 0x%x bytes of RAM", IO_BASE);
        return UAM_ERR_INVALID;
    }

    uam_status st = uam_machine_init(m, &uam_isa_m6502);
    if (st != UAM_OK)
        return st;
    m->board_name = "6502-ref";
    m->reset_pc = ROM_BASE;
    if ((st = uam_m6502_configure(&m->cpu, md->cfg)) != UAM_OK)
        return st;

    /* ROM, erased (0xFF) until the image is loaded. */
    const uint32_t rom = 0x10000u - ROM_BASE;
    uint8_t *erased = malloc(rom);
    if (!erased)
        return UAM_ERR_NOMEM;
    memset(erased, 0xFF, rom);
    st = uam_bus_map_rom(&m->bus, "rom", ROM_BASE, rom, erased, rom);
    free(erased);
    if (st != UAM_OK)
        return st;
    if ((st = uam_bus_map_ram(&m->bus, "ram", 0, cfg->ram_size ? cfg->ram_size : IO_BASE)) != UAM_OK)
        return st;

    uam_device *uart = uam_uart16550_create("uart0", cfg->console);
    uam_device *test = uam_test_finisher_create("test", uam_machine_stop_cb, m);
    uam_device *timer0 = uam_gptimer_create("timer0", &m->sched);
    uam_device *timer1 = uam_gptimer_create("timer1", &m->sched);
    if ((st = add(m, uart)) != UAM_OK || (st = add(m, test)) != UAM_OK || (st = add(m, timer0)) != UAM_OK
        || (st = add(m, timer1)) != UAM_OK)
        return st;
    if ((st = uam_bus_map_mmio(&m->bus, "uart0", IO_BASE, 8, uart)) != UAM_OK
        || (st = uam_bus_map_mmio(&m->bus, "test", IO_BASE + 0x10, 0x10, test)) != UAM_OK
        || (st = uam_bus_map_mmio(&m->bus, "timer0", IO_BASE + 0x20, UAM_GPTIMER_SIZE, timer0)) != UAM_OK
        || (st = uam_bus_map_mmio(&m->bus, "timer1", IO_BASE + 0x30, UAM_GPTIMER_SIZE, timer1)) != UAM_OK)
        return st;

    uam_signal_connect(uam_uart16550_irq_line(uart), uam_cpu_irq_sink, &m->cpu, UART_LINE);
    uam_signal_connect(uam_gptimer_irq(timer0), uam_cpu_irq_sink, &m->cpu, TIMER_LINE);
    uam_signal_connect(uam_gptimer_irq(timer1), uam_cpu_irq_sink, &m->cpu, UAM_M6502_LINE_NMI);
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

const uam_board uam_board_m6502_ref = {
    .name = "6502-ref",
    .description = "6502 reference board (-o model=nmos|2a03|65c02; NS16550A UART, timers, test finisher)",
    .build = m6502_ref_build,
};
