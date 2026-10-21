/*
 * arm_ref.c - "arm-ref": the ARMv7-A reference board. RAM, a GIC, a 16550
 * console and a simulation finisher.
 *
 *   0x00000000  RAM (64 MiB; the exception vectors at 0 are in RAM)
 *   0x1c000000  NS16550A UART (byte registers)            GIC SPI 32
 *   0x1c001000  test finisher (sifive,test protocol)
 *   0x1c010000  GIC distributor (96 interrupt ids)
 *   0x1c020000  GIC CPU interface
 *
 * The generic timer is inside the CPU; its physical timer drives PPI 30 and its
 * virtual timer PPI 27. The GIC's output is the CPU's IRQ input.
 */
#include "uam/boards.h"
#include "uam/dev/gic.h"
#include "uam/dev/sifive_test.h"
#include "uam/dev/uart16550.h"
#include "uam/dev/unknown_mmio.h"
#include "uam/isa/arm.h"
#include "uam/machine.h"

#define ARM_RAM_BASE   UINT64_C(0x00000000)
#define ARM_RAM_SIZE   (UINT64_C(64) << 20)
#define ARM_UART_BASE  UINT64_C(0x1c000000)
#define ARM_TEST_BASE  UINT64_C(0x1c001000)
#define ARM_GICD_BASE  UINT64_C(0x1c010000)
#define ARM_GICC_BASE  UINT64_C(0x1c020000)
#define ARM_GIC_IRQS   96u
#define ARM_UART_SPI   32u
#define ARM_PPI_PTIMER 30u
#define ARM_PPI_VTIMER 27u

static void timer_to_gic(void *gic, unsigned timer, int level)
{
    uam_gic_input(gic, timer == UAM_ARM_TIMER_VIRT ? ARM_PPI_VTIMER : ARM_PPI_PTIMER, level);
}

static uam_status arm_ref_build(uam_machine *m, const uam_board_config *cfg)
{
    uam_status st = uam_machine_init(m, &uam_isa_arm);
    if (st != UAM_OK)
        return st;
    m->board_name = "arm-ref";
    m->reset_pc = 0;   /* the reset vector; overridden by the loaded image's entry */

    st = uam_bus_map_ram(&m->bus, "ram", ARM_RAM_BASE, cfg->ram_size ? cfg->ram_size : ARM_RAM_SIZE);
    if (st != UAM_OK)
        return st;

    uam_device *uart = uam_uart16550_create("uart0", cfg->console);
    uam_device *test = uam_sifive_test_create("test", uam_machine_stop_cb, m);
    uam_device *gic = uam_gic_create("gic", ARM_GIC_IRQS);
    if (!uart || !test || !gic) {
        if (uart) uart->ops->destroy(uart);
        if (test) test->ops->destroy(test);
        if (gic) gic->ops->destroy(gic);
        return UAM_ERR_NOMEM;
    }
    uam_device *gicc = uam_gic_cpu_iface(gic);
    uam_machine_add_device(m, uart);
    uam_machine_add_device(m, test);
    uam_machine_add_device(m, gic);
    uam_machine_add_device(m, gicc);
    if ((st = uam_bus_map_mmio(&m->bus, "uart0", ARM_UART_BASE, 8, uart)) != UAM_OK
        || (st = uam_bus_map_mmio(&m->bus, "test", ARM_TEST_BASE, 0x10, test)) != UAM_OK
        || (st = uam_bus_map_mmio(&m->bus, "gicd", ARM_GICD_BASE, UAM_GIC_DIST_SIZE, gic)) != UAM_OK
        || (st = uam_bus_map_mmio(&m->bus, "gicc", ARM_GICC_BASE, UAM_GIC_CPU_SIZE, gicc)) != UAM_OK)
        return st;

    uam_signal_connect(uam_uart16550_irq_line(uart), uam_gic_input, gic, ARM_UART_SPI);
    uam_signal_connect(uam_gic_irq_line(gic), uam_cpu_irq_sink, &m->cpu, UAM_ARM_IRQ);
    if ((st = uam_arm_set_timer_sink(&m->cpu, timer_to_gic, gic)) != UAM_OK)
        return st;
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

const uam_board uam_board_arm_ref = {
    .name = "arm-ref",
    .description = "ARMv7-A reference board (GIC, generic timer, NS16550A UART, test finisher)",
    .build = arm_ref_build,
};
