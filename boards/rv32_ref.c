/*
 * rv32_ref.c - "rv32-ref": the RISC-V reference board, an RV32IMAC machine
 * (M-mode; one hart, or -o cpus=N up to 8) with a CLINT, a PLIC, a 16550
 * console and a simulation finisher.
 *
 *   0x00100000  test finisher
 *   0x02000000  CLINT          hart h: msip -> MSI, mtip -> MTI
 *   0x0C000000  PLIC           context 2h -> hart h's MEI (2h + 1 = S-mode, unconnected)
 *   0x10000000  NS16550A UART  irq -> PLIC source 10
 *   0x80000000  RAM (128 MiB by default)
 *
 * An example and a starting point: a board adds its own peripherals (network,
 * storage, ...) out of tree, on these components or on this board.
 *
 * Every hart starts at the reset address; firmware tells them apart by
 * mhartid (and parks the ones it does not use).
 *
 * This is the only file that knows both the ISA and the concrete devices.
 */
#include "uam/boards.h"
#include "uam/dev/clint.h"
#include "uam/dev/plic.h"
#include "uam/dev/test_finisher.h"
#include "uam/dev/uart16550.h"
#include "uam/dev/unknown_mmio.h"
#include "uam/isa/riscv.h"
#include "uam/machine.h"

#include <stdlib.h>

#define RV32REF_TEST_BASE  UINT64_C(0x00100000)
#define RV32REF_CLINT_BASE UINT64_C(0x02000000)
#define RV32REF_PLIC_BASE  UINT64_C(0x0C000000)
#define RV32REF_UART0_BASE UINT64_C(0x10000000)
#define RV32REF_DRAM_BASE  UINT64_C(0x80000000)
#define RV32REF_DRAM_SIZE  (UINT64_C(128) << 20)
#define RV32REF_UART0_IRQ  10u
#define RV32REF_PLIC_SOURCES 96u
#define RV32REF_MAX_CPUS   8u      /* two PLIC contexts each */

static uam_status attach(uam_machine *m, uam_device *dev, uint64_t base, uint64_t size)
{
    if (!dev)
        return UAM_ERR_NOMEM;
    uam_machine_add_device(m, dev);
    return uam_bus_map_mmio(&m->bus, dev->name, base, size, dev);
}

static uam_status rv32_ref_build(uam_machine *m, const uam_board_config *cfg)
{
    uam_status st = uam_machine_init(m, &uam_isa_rv32);
    if (st != UAM_OK)
        return st;
    m->board_name = "rv32-ref";
    m->reset_pc = RV32REF_DRAM_BASE;
    const char *ncpu_opt = uam_board_config_option(cfg, "cpus");
    unsigned ncpu = ncpu_opt ? (unsigned)strtoul(ncpu_opt, NULL, 0) : 1;
    if (ncpu < 1 || ncpu > RV32REF_MAX_CPUS) {
        uam_log(UAM_LOG_ERROR, "rv32-ref", "cpus=%s: 1..%u", ncpu_opt, RV32REF_MAX_CPUS);
        return UAM_ERR_INVALID;
    }
    if ((st = uam_machine_add_cpus(m, ncpu - 1)) != UAM_OK)
        return st;

    st = uam_bus_map_ram(&m->bus, "dram", RV32REF_DRAM_BASE, cfg->ram_size ? cfg->ram_size : RV32REF_DRAM_SIZE);
    if (st != UAM_OK)
        return st;

    uam_device *test = uam_test_finisher_create("test", uam_machine_stop_cb, m);
    uam_device *clint = uam_clint_create_harts("clint", &m->sched, ncpu);
    uam_device *plic = uam_plic_create("plic", RV32REF_PLIC_SOURCES, 2 * ncpu);
    uam_device *uart = uam_uart16550_create("uart0", cfg->console);

    if ((st = attach(m, test, RV32REF_TEST_BASE, UAM_TEST_FINISHER_SIZE)) != UAM_OK
        || (st = attach(m, clint, RV32REF_CLINT_BASE, UAM_CLINT_SIZE)) != UAM_OK
        || (st = attach(m, plic, RV32REF_PLIC_BASE, UAM_PLIC_SIZE)) != UAM_OK
        || (st = attach(m, uart, RV32REF_UART0_BASE, UAM_UART16550_SIZE)) != UAM_OK)
        return st;

    /* Interrupt wiring. */
    for (unsigned h = 0; h < ncpu; h++) {
        uam_cpu *cpu = uam_machine_cpu(m, h);
        uam_signal_connect(uam_clint_hart_msip_line(clint, h), uam_cpu_irq_sink, cpu, UAM_RV_IRQ_MSI);
        uam_signal_connect(uam_clint_hart_mtip_line(clint, h), uam_cpu_irq_sink, cpu, UAM_RV_IRQ_MTI);
        uam_signal_connect(uam_plic_context_line(plic, 2 * h), uam_cpu_irq_sink, cpu, UAM_RV_IRQ_MEI);
    }
    uam_signal_connect(uam_uart16550_irq_line(uart), uam_plic_input, plic, RV32REF_UART0_IRQ);

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

const uam_board uam_board_rv32_ref = {
    .name = "rv32-ref",
    .description = "RV32IMAC reference board (CLINT, PLIC, NS16550A, test finisher; "
                   "-o cpus=N harts)",
    .build = rv32_ref_build,
};
