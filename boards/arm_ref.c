/*
 * arm_ref.c - "arm-ref" / "armeb-ref": the ARMv7-A reference boards, little- and
 * big-endian (BE8: big-endian data, little-endian code). RAM, a GIC, a 16550
 * console and a simulation finisher.
 *
 *   0x00000000  RAM (64 MiB; the exception vectors at 0 are in RAM)
 *   0x1c000000  NS16550A UART (byte registers)            GIC SPI 32
 *   0x1c001000  test finisher
 *   0x1c010000  GIC distributor (96 interrupt ids)
 *   0x1c020000  GIC CPU interface
 *
 * The generic timer is inside the CPU; its physical timer drives PPI 30 and its
 * virtual timer PPI 27. The GIC's output is the CPU's IRQ input. The devices'
 * registers are little-endian on both boards: a big-endian guest byte-swaps its
 * 32-bit GIC accesses, as on BE8 hardware.
 *
 * -o cpus=N (up to 8) adds processors: every one starts at the reset address and
 * reads its number in MPIDR.Aff0; each has its own generic timer (its PPIs) and
 * GIC CPU interface (banked at the same address), and they interrupt each other
 * with SGIs. SPIs reach the processors their ITARGETSR byte names.
 */
#include <stdlib.h>

#include "uam/boards.h"
#include "uam/dev/gic.h"
#include "uam/dev/test_finisher.h"
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
#define ARM_MAX_CPUS   8u

/* A processor's generic timer, to its own PPIs. */
typedef struct timer_route {
    uam_device *gic;
    unsigned    cpu;
} timer_route;

static void timer_to_gic(void *ctx, unsigned timer, int level)
{
    const timer_route *r = ctx;
    uam_gic_ppi_input(r->gic, 32 * r->cpu + (timer == UAM_ARM_TIMER_VIRT ? ARM_PPI_VTIMER
                                                                          : ARM_PPI_PTIMER), level);
}

static void free_board(uam_machine *m)
{
    free(m->board_data);
    m->board_data = NULL;
}

static uam_status arm_ref_build_isa(uam_machine *m, const uam_board_config *cfg, const uam_isa *isa)
{
    uam_status st = uam_machine_init(m, isa);
    if (st != UAM_OK)
        return st;
    m->board_name = isa == &uam_isa_armeb ? "armeb-ref" : "arm-ref";
    m->reset_pc = 0;   /* the reset vector; overridden by the loaded image's entry */
    const char *ncpu_opt = uam_board_config_option(cfg, "cpus");
    unsigned ncpu = ncpu_opt ? (unsigned)strtoul(ncpu_opt, NULL, 0) : 1;
    if (ncpu < 1 || ncpu > ARM_MAX_CPUS) {
        uam_log(UAM_LOG_ERROR, m->board_name, "cpus=%s: 1..%u", ncpu_opt, ARM_MAX_CPUS);
        return UAM_ERR_INVALID;
    }
    if ((st = uam_machine_add_cpus(m, ncpu - 1)) != UAM_OK)
        return st;
    timer_route *routes = calloc(ncpu, sizeof(*routes));
    if (!routes)
        return UAM_ERR_NOMEM;
    m->board_data = routes;
    m->board_fini = free_board;

    st = uam_bus_map_ram(&m->bus, "ram", ARM_RAM_BASE, cfg->ram_size ? cfg->ram_size : ARM_RAM_SIZE);
    if (st != UAM_OK)
        return st;

    uam_device *uart = uam_uart16550_create("uart0", cfg->console);
    uam_device *test = uam_test_finisher_create("test", uam_machine_stop_cb, m);
    uam_device *gic = uam_gic_create_cpus("gic", ARM_GIC_IRQS, ncpu, uam_machine_current_cpu, m);
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
    for (unsigned i = 0; i < ncpu; i++) {
        uam_cpu *cpu = uam_machine_cpu(m, i);
        routes[i] = (timer_route){ gic, i };
        uam_signal_connect(uam_gic_cpu_irq_line(gic, i), uam_cpu_irq_sink, cpu, UAM_ARM_IRQ);
        if ((st = uam_arm_set_timer_sink(cpu, timer_to_gic, &routes[i])) != UAM_OK)
            return st;
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

static uam_status arm_ref_build(uam_machine *m, const uam_board_config *cfg)
{
    return arm_ref_build_isa(m, cfg, &uam_isa_arm);
}

static uam_status armeb_ref_build(uam_machine *m, const uam_board_config *cfg)
{
    return arm_ref_build_isa(m, cfg, &uam_isa_armeb);
}

const uam_board uam_board_arm_ref = {
    .name = "arm-ref",
    .description = "ARMv7-A reference board (GIC, generic timer, NS16550A UART, test finisher; "
                   "-o cpus=N)",
    .build = arm_ref_build,
};

const uam_board uam_board_armeb_ref = {
    .name = "armeb-ref",
    .description = "ARMv7-A big-endian (BE8) reference board (GIC, generic timer, NS16550A UART, "
                   "test finisher; -o cpus=N)",
    .build = armeb_ref_build,
};
