/*
 * mips_ref.c - "mips-ref" / "mipsel-ref": the MIPS32 reference boards
 * (big- and little-endian), RAM plus a 16550 console and a simulation
 * finisher.
 *
 *   phys 0x00000000  RAM (32 MiB), reached via kseg0 0x80000000 / kseg1 0xa0000000
 *   phys 0x1fd003f8  NS16550A UART (byte registers), kseg1 0xbfd003f8
 *   phys 0x1fd00010  test finisher
 *
 * The UART interrupt is wired to hardware interrupt 0 (Cause.IP2). The CP0
 * Count/Compare timer is internal to the CPU and needs no wiring.
 *
 * -o cpus=N (up to 8) adds processors: every one starts at the reset address
 * and reads its number in EBase.CPUNum, and they interrupt each other through
 * IPI doorbells (uam/dev/ipi.h) at phys 0x1fd01000 (kseg1 0xbfd01000), processor
 * n's line on its hardware interrupt 1 (Cause.IP3). The UART interrupts
 * processor 0.
 */
#include "uam/boards.h"
#include <stdlib.h>

#include "uam/dev/ipi.h"
#include "uam/dev/test_finisher.h"
#include "uam/dev/uart16550.h"
#include "uam/dev/unknown_mmio.h"
#include "uam/isa/mips32.h"
#include "uam/machine.h"

#define MIPS_RAM_BASE   UINT64_C(0x00000000)
#define MIPS_RAM_SIZE   (UINT64_C(32) << 20)
#define MIPS_UART_PHYS  UINT64_C(0x1fd003f8)
#define MIPS_TEST_PHYS  UINT64_C(0x1fd00010)
#define MIPS_UART_IRQ   UAM_MIPS_IRQ_HW0  /* Cause.IP2 */
#define MIPS_IPI_PHYS   UINT64_C(0x1fd01000)
#define MIPS_IPI_IRQ    UAM_MIPS_IRQ_HW1  /* Cause.IP3 */
#define MIPS_MAX_CPUS   8u

static uam_status mips_ref_build_isa(uam_machine *m, const uam_board_config *cfg, const uam_isa *isa)
{
    uam_status st = uam_machine_init(m, isa);
    if (st != UAM_OK)
        return st;
    m->board_name = isa->default_endian & UAM_ACC_BE ? "mips-ref" : "mipsel-ref";
    m->reset_pc = 0xBFC00000u; /* overridden by the loaded image's entry */
    const char *ncpu_opt = uam_board_config_option(cfg, "cpus");
    unsigned ncpu = ncpu_opt ? (unsigned)strtoul(ncpu_opt, NULL, 0) : 1;
    if (ncpu < 1 || ncpu > MIPS_MAX_CPUS) {
        uam_log(UAM_LOG_ERROR, m->board_name, "cpus=%s: 1..%u", ncpu_opt, MIPS_MAX_CPUS);
        return UAM_ERR_INVALID;
    }
    if ((st = uam_machine_add_cpus(m, ncpu - 1)) != UAM_OK)
        return st;

    st = uam_bus_map_ram(&m->bus, "ram", MIPS_RAM_BASE, cfg->ram_size ? cfg->ram_size : MIPS_RAM_SIZE);
    if (st != UAM_OK)
        return st;

    uam_device *uart = uam_uart16550_create("uart0", cfg->console);
    uam_device *test = uam_test_finisher_create("test", uam_machine_stop_cb, m);
    if (!uart || !test) {
        if (uart) uart->ops->destroy(uart);
        if (test) test->ops->destroy(test);
        return UAM_ERR_NOMEM;
    }
    uam_machine_add_device(m, uart);
    uam_machine_add_device(m, test);
    if ((st = uam_bus_map_mmio(&m->bus, "uart0", MIPS_UART_PHYS, 8, uart)) != UAM_OK
        || (st = uam_bus_map_mmio(&m->bus, "test", MIPS_TEST_PHYS, 0x10, test)) != UAM_OK)
        return st;

    uam_signal_connect(uam_uart16550_irq_line(uart), uam_cpu_irq_sink, &m->cpu, MIPS_UART_IRQ);
    if (ncpu > 1) {
        uam_device *ipi = uam_ipi_create("ipi", ncpu);
        if (!ipi)
            return UAM_ERR_NOMEM;
        uam_machine_add_device(m, ipi);
        if ((st = uam_bus_map_mmio(&m->bus, "ipi", MIPS_IPI_PHYS, UAM_IPI_SIZE, ipi)) != UAM_OK)
            return st;
        for (unsigned i = 0; i < ncpu; i++)
            uam_signal_connect(uam_ipi_line(ipi, i), uam_cpu_irq_sink, uam_machine_cpu(m, i),
                               MIPS_IPI_IRQ);
    }
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

static uam_status mips_ref_build_be(uam_machine *m, const uam_board_config *cfg)
{
    return mips_ref_build_isa(m, cfg, &uam_isa_mips32be);
}

static uam_status mips_ref_build_le(uam_machine *m, const uam_board_config *cfg)
{
    return mips_ref_build_isa(m, cfg, &uam_isa_mips32le);
}

const uam_board uam_board_mips_ref = {
    .name = "mips-ref",
    .description = "MIPS32 big-endian reference board (NS16550A UART, test finisher; -o cpus=N)",
    .build = mips_ref_build_be,
};

const uam_board uam_board_mipsel_ref = {
    .name = "mipsel-ref",
    .description = "MIPS32 little-endian reference board (NS16550A UART, test finisher; -o cpus=N)",
    .build = mips_ref_build_le,
};
