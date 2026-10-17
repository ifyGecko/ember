/*
 * virt.c - "virt": a RISC-V RV32 board memory-map compatible with QEMU's
 * `virt` machine (single hart, M-mode). Compatibility is deliberate: the
 * same firmware ELF runs under QEMU, which serves as a reference for
 * differential testing.
 *
 *   0x00100000  sifive,test finisher
 *   0x02000000  CLINT          msip -> MSI, mtip -> MTI
 *   0x0C000000  PLIC           context 0 -> MEI (context 1 = S-mode, unconnected)
 *   0x10000000  NS16550A UART  irq -> PLIC source 10
 *   0x80000000  RAM (128 MiB by default)
 *
 * This is the only file that knows both the ISA and the concrete devices.
 */
#include "uam/dev/clint.h"
#include "uam/dev/plic.h"
#include "uam/dev/sifive_test.h"
#include "uam/dev/uart16550.h"
#include "uam/dev/unknown_mmio.h"
#include "uam/isa/riscv.h"
#include "uam/machine.h"

#include <string.h>

#define VIRT_TEST_BASE  UINT64_C(0x00100000)
#define VIRT_CLINT_BASE UINT64_C(0x02000000)
#define VIRT_PLIC_BASE  UINT64_C(0x0C000000)
#define VIRT_UART0_BASE UINT64_C(0x10000000)
#define VIRT_DRAM_BASE  UINT64_C(0x80000000)
#define VIRT_DRAM_SIZE  (UINT64_C(128) << 20)
#define VIRT_UART0_IRQ  10u
#define VIRT_PLIC_SOURCES 96u

static uam_status attach(uam_machine *m, uam_device *dev, uint64_t base, uint64_t size)
{
    if (!dev)
        return UAM_ERR_NOMEM;
    uam_machine_add_device(m, dev);
    return uam_bus_map_mmio(&m->bus, dev->name, base, size, dev);
}

static uam_status virt_build(uam_machine *m, const uam_board_config *cfg)
{
    uam_status st = uam_machine_init(m, &uam_isa_rv32);
    if (st != UAM_OK)
        return st;
    m->board_name = "virt";
    m->reset_pc = VIRT_DRAM_BASE;

    st = uam_bus_map_ram(&m->bus, "dram", VIRT_DRAM_BASE, cfg->ram_size ? cfg->ram_size : VIRT_DRAM_SIZE);
    if (st != UAM_OK)
        return st;

    uam_device *test = uam_sifive_test_create("test", uam_machine_stop_cb, m);
    uam_device *clint = uam_clint_create("clint", &m->sched);
    uam_device *plic = uam_plic_create("plic", VIRT_PLIC_SOURCES, 2);
    uam_device *uart = uam_uart16550_create("uart0", cfg->console);

    if ((st = attach(m, test, VIRT_TEST_BASE, UAM_SIFIVE_TEST_SIZE)) != UAM_OK
        || (st = attach(m, clint, VIRT_CLINT_BASE, UAM_CLINT_SIZE)) != UAM_OK
        || (st = attach(m, plic, VIRT_PLIC_BASE, UAM_PLIC_SIZE)) != UAM_OK
        || (st = attach(m, uart, VIRT_UART0_BASE, UAM_UART16550_SIZE)) != UAM_OK)
        return st;

    /* Interrupt wiring. */
    uam_signal_connect(uam_clint_msip_line(clint), uam_cpu_irq_sink, &m->cpu, UAM_RV_IRQ_MSI);
    uam_signal_connect(uam_clint_mtip_line(clint), uam_cpu_irq_sink, &m->cpu, UAM_RV_IRQ_MTI);
    uam_signal_connect(uam_plic_context_line(plic, 0), uam_cpu_irq_sink, &m->cpu, UAM_RV_IRQ_MEI);
    uam_signal_connect(uam_uart16550_irq_line(uart), uam_plic_input, plic, VIRT_UART0_IRQ);

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

const uam_board uam_board_virt = {
    .name = "virt",
    .description = "RV32IMAC, QEMU virt-compatible map (CLINT, PLIC, NS16550A, test finisher)",
    .build = virt_build,
};
