/*
 * z80_ref.c - "z80-ref": the Z80 reference board. 64 KiB of RAM, and on the I/O ports a
 * 16550 UART, a Z80 CTC and a simulation finisher; no other peripheral of any system.
 *
 * -o model= picks the core (uam/isa/z80.h): nmos (default) or cmos.
 *
 * Memory: RAM from 0 (--ram sets its size, at most 64 KiB; nothing above it), into which an
 * image is loaded (a raw binary at 0). Execution starts at 0.
 * I/O ports (the low byte decoded, as most Z80 systems do):
 *   0x00-0x07  16550 UART, the console ("uart0" input route)
 *   0x10-0x13  CTC channels 0-3 (uam/dev/z80_ctc.h)
 *   0xF0-0xFF  test finisher (byte commands: uam/dev/test_finisher.h)
 * /INT is the UART's interrupt ORed with the CTC's. In IM 2 the CTC answers the
 * interrupt acknowledge with its vector; the UART has none (the bus reads 0xFF, RST 38h
 * in IM 0). RETI ends the CTC channel in service. A Z80 board has one processor: -o cpus
 * is 1.
 */
#include "uam/boards.h"
#include <stdlib.h>
#include <string.h>

#include "uam/dev/test_finisher.h"
#include "uam/dev/uart16550.h"
#include "uam/dev/unknown_mmio.h"
#include "uam/dev/z80_ctc.h"
#include "uam/isa/z80.h"
#include "uam/machine.h"

#define UART_PORT 0x00u
#define CTC_PORT  0x10u
#define TEST_PORT 0xF0u

typedef struct z80_ref {
    uam_cpu    *cpu;
    uam_device *uart, *ctc;
} z80_ref;

/* /INT: either source. */
static void int_sink(void *ctx, unsigned input, int level)
{
    (void)input; (void)level;
    z80_ref *b = ctx;
    uam_cpu_irq_sink(b->cpu, UAM_Z80_LINE_INT,
                     uam_uart16550_irq_line(b->uart)->level | uam_z80_ctc_irq(b->ctc)->level);
}

static uint8_t inta(void *ctx, unsigned n)
{
    z80_ref *b = ctx;
    return n == 0 && uam_z80_ctc_pending(b->ctc) ? uam_z80_ctc_ack(b->ctc) : 0xFF;
}

static void reti(void *ctx) { uam_z80_ctc_reti(((z80_ref *)ctx)->ctc); }

static uam_status uart_input(void *ctx, uam_machine *m, const uint8_t *data, size_t len)
{
    (void)m;
    uam_uart16550_feed(ctx, data, len);
    return UAM_OK;
}

static void board_fini(uam_machine *m)
{
    free(m->board_data);
    m->board_data = NULL;
}

static uam_status add(uam_machine *m, uam_device *d)
{
    if (!d)
        return UAM_ERR_NOMEM;
    uam_machine_add_device(m, d);
    return UAM_OK;
}

static uam_status z80_ref_build(uam_machine *m, const uam_board_config *cfg)
{
    const char *mname = uam_board_config_option(cfg, "model");
    const uam_z80_config *model = !mname || strcmp(mname, "nmos") == 0 ? &uam_z80_nmos_ref
                                  : strcmp(mname, "cmos") == 0         ? &uam_z80_cmos_ref
                                                                       : NULL;
    if (!model) {
        uam_log(UAM_LOG_ERROR, "z80-ref", "model=%s: nmos or cmos", mname);
        return UAM_ERR_INVALID;
    }
    const char *ncpu = uam_board_config_option(cfg, "cpus");
    if (ncpu && strtoul(ncpu, NULL, 0) != 1) {
        uam_log(UAM_LOG_ERROR, "z80-ref", "cpus=%s: a Z80 board has one processor", ncpu);
        return UAM_ERR_INVALID;
    }
    if (cfg->ram_size > 0x10000) {
        uam_log(UAM_LOG_ERROR, "z80-ref", "--ram: at most 64 KiB");
        return UAM_ERR_INVALID;
    }

    uam_status st = uam_machine_init(m, &uam_isa_z80);
    if (st != UAM_OK)
        return st;
    m->board_name = "z80-ref";
    m->reset_pc = 0;
    if ((st = uam_z80_configure(&m->cpu, model)) != UAM_OK)
        return st;
    uam_z80_set_io_decode(&m->cpu, 0x00FF);

    z80_ref *b = calloc(1, sizeof(*b));
    if (!b)
        return UAM_ERR_NOMEM;
    m->board_data = b;
    m->board_fini = board_fini;
    b->cpu = &m->cpu;

    if ((st = uam_bus_map_ram(&m->bus, "ram", 0, cfg->ram_size ? cfg->ram_size : 0x10000)) != UAM_OK)
        return st;
    b->uart = uam_uart16550_create("uart0", cfg->console);
    b->ctc = uam_z80_ctc_create("ctc", &m->sched);
    uam_device *test = uam_test_finisher_create("test", uam_machine_stop_cb, m);
    if ((st = add(m, b->uart)) != UAM_OK || (st = add(m, b->ctc)) != UAM_OK || (st = add(m, test)) != UAM_OK)
        return st;
    if ((st = uam_bus_map_mmio(&m->bus, "uart0", UAM_Z80_IO_BASE + UART_PORT, UAM_UART16550_REGS, b->uart)) != UAM_OK
        || (st = uam_bus_map_mmio(&m->bus, "ctc", UAM_Z80_IO_BASE + CTC_PORT, UAM_Z80_CTC_SIZE, b->ctc)) != UAM_OK
        || (st = uam_bus_map_mmio(&m->bus, "test", UAM_Z80_IO_BASE + TEST_PORT, 0x10, test)) != UAM_OK)
        return st;

    uam_signal_connect(uam_uart16550_irq_line(b->uart), int_sink, b, 0);
    uam_signal_connect(uam_z80_ctc_irq(b->ctc), int_sink, b, 1);
    uam_z80_set_inta(&m->cpu, inta, b);
    uam_z80_set_reti(&m->cpu, reti, b);
    if ((st = uam_machine_add_input(m, "uart0", uart_input, b->uart)) != UAM_OK)
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

const uam_board uam_board_z80_ref = {
    .name = "z80-ref",
    .description = "Z80 reference board (-o model=nmos|cmos; 64 KiB RAM, a 16550, a CTC and a test finisher on I/O ports)",
    .build = z80_ref_build,
};
