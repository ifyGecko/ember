/*
 * mips_ref.c - "mips-ref" / "mipsel-ref": the MIPS32 reference boards
 * (big- and little-endian), RAM plus a 16550 console and a simulation
 * finisher.
 *
 *   phys 0x00000000  RAM (32 MiB), reached via kseg0 0x80000000 / kseg1 0xa0000000
 *   phys 0x1fd003f8  NS16550A UART (byte registers), kseg1 0xbfd003f8
 *   phys 0x1fd00010  test finisher (sifive,test protocol)
 *
 * The UART interrupt is wired to hardware interrupt 0 (Cause.IP2). The CP0
 * Count/Compare timer is internal to the CPU and needs no wiring.
 */
#include "uam/dev/sifive_test.h"
#include "uam/dev/uart16550.h"
#include "uam/dev/unknown_mmio.h"
#include "uam/isa/mips32.h"
#include "uam/machine.h"

#define MIPS_RAM_BASE   UINT64_C(0x00000000)
#define MIPS_RAM_SIZE   (UINT64_C(32) << 20)
#define MIPS_UART_PHYS  UINT64_C(0x1fd003f8)
#define MIPS_TEST_PHYS  UINT64_C(0x1fd00010)
#define MIPS_UART_IRQ   UAM_MIPS_IRQ_HW0  /* Cause.IP2 */

static uam_status mips_ref_build_isa(uam_machine *m, const uam_board_config *cfg, const uam_isa *isa)
{
    uam_status st = uam_machine_init(m, isa);
    if (st != UAM_OK)
        return st;
    m->board_name = isa->default_endian & UAM_ACC_BE ? "mips-ref" : "mipsel-ref";
    m->reset_pc = 0xBFC00000u; /* overridden by the loaded image's entry */

    st = uam_bus_map_ram(&m->bus, "ram", MIPS_RAM_BASE, cfg->ram_size ? cfg->ram_size : MIPS_RAM_SIZE);
    if (st != UAM_OK)
        return st;

    uam_device *uart = uam_uart16550_create("uart0", cfg->console);
    uam_device *test = uam_sifive_test_create("test", uam_machine_stop_cb, m);
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
    .description = "MIPS32 big-endian reference board (NS16550A UART, test finisher)",
    .build = mips_ref_build_be,
};

const uam_board uam_board_mipsel_ref = {
    .name = "mipsel-ref",
    .description = "MIPS32 little-endian reference board (NS16550A UART, test finisher)",
    .build = mips_ref_build_le,
};
