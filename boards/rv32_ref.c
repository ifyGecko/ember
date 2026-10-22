/*
 * rv32_ref.c - "rv32-ref": the RISC-V reference board, an RV32IMAC machine
 * (M-mode; one hart, or -o cpus=N up to 8) with a CLINT, a PLIC, a 16550
 * console and a simulation finisher.
 *
 *   0x00100000  test finisher
 *   0x02000000  CLINT          hart h: msip -> MSI, mtip -> MTI
 *   0x0C000000  PLIC           context 2h -> hart h's MEI (2h + 1 = S-mode, unconnected)
 *   0x10000000  NS16550A UART  irq -> PLIC source 10
 *   0x10001000  virtio-mmio window: slot i at +0x1000 * i (i < 8), irq -> PLIC source 1 + i
 *   0x80000000  RAM (128 MiB by default)
 *
 * Network ports: "net0".."net7" (--nic net0=tap:tap0) put a virtio-net device
 * in the slot of the same number; slots without a port are not mapped.
 * Option -o netN.mac=XX:XX:XX:XX:XX:XX sets a MAC (default 02:75:61:6d:01:0N,
 * locally administered).
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
#include "uam/dev/virtio_net.h"
#include "uam/isa/riscv.h"
#include "uam/machine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RV32REF_TEST_BASE  UINT64_C(0x00100000)
#define RV32REF_CLINT_BASE UINT64_C(0x02000000)
#define RV32REF_PLIC_BASE  UINT64_C(0x0C000000)
#define RV32REF_UART0_BASE UINT64_C(0x10000000)
#define RV32REF_DRAM_BASE  UINT64_C(0x80000000)
#define RV32REF_DRAM_SIZE  (UINT64_C(128) << 20)
#define RV32REF_UART0_IRQ  10u
#define RV32REF_VIRTIO_BASE UINT64_C(0x10001000)
#define RV32REF_VIRTIO_STRIDE UINT64_C(0x1000)
#define RV32REF_VIRTIO_COUNT 8u
#define RV32REF_VIRTIO_IRQ 1u
#define RV32REF_PLIC_SOURCES 96u
#define RV32REF_MAX_CPUS   8u      /* two PLIC contexts each */

static uam_status attach(uam_machine *m, uam_device *dev, uint64_t base, uint64_t size)
{
    if (!dev)
        return UAM_ERR_NOMEM;
    uam_machine_add_device(m, dev);
    return uam_bus_map_mmio(&m->bus, dev->name, base, size, dev);
}

static const char *const virtio_names[RV32REF_VIRTIO_COUNT] = {
    "virtio0", "virtio1", "virtio2", "virtio3", "virtio4", "virtio5", "virtio6", "virtio7",
};

/* The virtio-mmio slots: a virtio-net device for each attached port "netN". */
static uam_status build_virtio(uam_machine *m, const uam_board_config *cfg, uam_device *plic)
{
    for (unsigned i = 0; i < RV32REF_VIRTIO_COUNT; i++) {
        char port[8], key[16];
        snprintf(port, sizeof(port), "net%u", i);
        snprintf(key, sizeof(key), "net%u.mac", i);
        uam_netdev *nd = uam_board_config_netdev(cfg, port);
        if (!nd)
            continue;   /* unplugged: nothing in this slot */
        uint8_t mac[6] = { 0x02, 0x75, 0x61, 0x6d, 0x01, (uint8_t)i };
        const char *opt = uam_board_config_option(cfg, key);
        unsigned b[6];
        if (opt) {
            if (sscanf(opt, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
                uam_log(UAM_LOG_ERROR, "rv32-ref", "bad %s '%s'", key, opt);
                return UAM_ERR_INVALID;
            }
            for (int k = 0; k < 6; k++)
                mac[k] = (uint8_t)b[k];
        }
        uam_device *dev = uam_virtio_net_create(virtio_names[i], &m->bus, nd, mac);
        uam_status st = attach(m, dev, RV32REF_VIRTIO_BASE + i * RV32REF_VIRTIO_STRIDE, UAM_VIRTIO_MMIO_SIZE);
        if (st != UAM_OK)
            return st;
        uam_signal_connect(uam_virtio_net_irq_line(dev), uam_plic_input, plic, RV32REF_VIRTIO_IRQ + i);
    }
    return UAM_OK;
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

    if ((st = build_virtio(m, cfg, plic)) != UAM_OK
        || (st = uam_board_add_uart_input(m, "uart0", uart)) != UAM_OK)
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
    .description = "RV32IMAC reference board (CLINT, PLIC, NS16550A, test finisher, "
                   "virtio-net ports net0..net7; -o cpus=N harts)",
    .build = rv32_ref_build,
};
