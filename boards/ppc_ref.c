/*
 * ppc_ref.c - "ppc-ref": the 32-bit PowerPC reference board. RAM, a boot ROM
 * under the reset vector, a 16550 console and a simulation finisher.
 *
 *   0x00000000  RAM (64 MiB; the exception vectors at 0 are in RAM)
 *   0xf0000000  NS16550A UART (byte registers)        external interrupt source 0
 *   0xf0000100  test finisher
 *   0xf0001000  IPI doorbells (-o cpus=N, N > 1)       external interrupt source 1
 *   0xfff00000  boot ROM (1 MiB): the reset vector is 0xfff00100 (MSR[IP] = 1)
 *
 * The decrementer and the time base are inside the processor and need no wiring.
 * The processor is the reference identity (uam_ppc_ref); a board for a real chip
 * sets its own with uam_ppc_configure and models its implementation registers
 * (HID0 and the rest) with uam_ppc_set_spr_ops. A raw boot image goes to the ROM
 * with --raw-base 0xfff00000 --entry 0xfff00100.
 *
 * -o cpus=N (up to 8) adds processors: every one starts at the reset address and
 * reads its number in PIR (SPR 1023), and they interrupt each other through the
 * doorbells, processor n's on its external interrupt input. The UART interrupts
 * processor 0.
 */
#include <stdlib.h>

#include "uam/boards.h"
#include "uam/dev/ipi.h"
#include "uam/dev/test_finisher.h"
#include "uam/dev/uart16550.h"
#include "uam/dev/unknown_mmio.h"
#include "uam/isa/ppc.h"
#include "uam/machine.h"

#define PPC_RAM_BASE   UINT64_C(0x00000000)
#define PPC_RAM_SIZE   (UINT64_C(64) << 20)
#define PPC_UART_BASE  UINT64_C(0xf0000000)
#define PPC_TEST_BASE  UINT64_C(0xf0000100)
#define PPC_IPI_BASE   UINT64_C(0xf0001000)
#define PPC_ROM_BASE   UINT64_C(0xfff00000)
#define PPC_ROM_SIZE   (UINT64_C(1) << 20)
#define PPC_UART_IRQ   UAM_PPC_IRQ_INT(0)
#define PPC_IPI_IRQ    UAM_PPC_IRQ_INT(1)
#define PPC_MAX_CPUS   8u

static uam_status ppc_ref_build(uam_machine *m, const uam_board_config *cfg)
{
    uam_status st = uam_machine_init(m, &uam_isa_ppc32);
    if (st != UAM_OK)
        return st;
    m->board_name = "ppc-ref";
    m->reset_pc = PPC_ROM_BASE + 0x100;   /* overridden by the loaded image's entry */
    const char *ncpu_opt = uam_board_config_option(cfg, "cpus");
    unsigned ncpu = ncpu_opt ? (unsigned)strtoul(ncpu_opt, NULL, 0) : 1;
    if (ncpu < 1 || ncpu > PPC_MAX_CPUS) {
        uam_log(UAM_LOG_ERROR, m->board_name, "cpus=%s: 1..%u", ncpu_opt, PPC_MAX_CPUS);
        return UAM_ERR_INVALID;
    }
    if ((st = uam_machine_add_cpus(m, ncpu - 1)) != UAM_OK)
        return st;

    if ((st = uam_bus_map_ram(&m->bus, "ram", PPC_RAM_BASE,
                              cfg->ram_size ? cfg->ram_size : PPC_RAM_SIZE)) != UAM_OK
        || (st = uam_bus_map_rom(&m->bus, "boot-rom", PPC_ROM_BASE, PPC_ROM_SIZE, NULL, 0)) != UAM_OK)
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
    if ((st = uam_bus_map_mmio(&m->bus, "uart0", PPC_UART_BASE, 8, uart)) != UAM_OK
        || (st = uam_bus_map_mmio(&m->bus, "test", PPC_TEST_BASE, 0x10, test)) != UAM_OK)
        return st;
    uam_signal_connect(uam_uart16550_irq_line(uart), uam_cpu_irq_sink, &m->cpu, PPC_UART_IRQ);

    if (ncpu > 1) {
        uam_device *ipi = uam_ipi_create("ipi", ncpu);
        if (!ipi)
            return UAM_ERR_NOMEM;
        uam_machine_add_device(m, ipi);
        if ((st = uam_bus_map_mmio(&m->bus, "ipi", PPC_IPI_BASE, UAM_IPI_SIZE, ipi)) != UAM_OK)
            return st;
        for (unsigned i = 0; i < ncpu; i++)
            uam_signal_connect(uam_ipi_line(ipi, i), uam_cpu_irq_sink, uam_machine_cpu(m, i),
                               PPC_IPI_IRQ);
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

const uam_board uam_board_ppc_ref = {
    .name = "ppc-ref",
    .description = "32-bit PowerPC reference board (NS16550A UART, test finisher, boot ROM; "
                   "-o cpus=N)",
    .build = ppc_ref_build,
};
