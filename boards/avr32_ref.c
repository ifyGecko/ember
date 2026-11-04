/*
 * avr32_ref.c - "avr32-ref": the AVR32 reference board, RAM, code memory at the
 * processor's reset address, an interrupt controller, a 16550 console and a
 * simulation finisher.
 *
 * -o model=a (the default): an AVR32A processor (uam_avr32a_ref: no MMU, an MPU).
 *   0x00000000  RAM (16 MiB)
 *   0x80000000  code memory (8 MiB, writable), the reset address
 * -o model=b: an AVR32B processor (uam_avr32b_ref: MMU, segmented memory map).
 *   phys 0x00000000  code memory (8 MiB, writable); the reset address 0xa0000000 (P2)
 *   phys 0x10000000  RAM (32 MiB): P1 0x90000000, P2 0xb0000000
 *
 * Both, at addresses that AVR32B reaches untranslated through P4:
 *   0xffff0800  interrupt controller (uam/dev/avr32_intc.h, 32 groups)
 *   0xffff1400  NS16550A UART (byte registers)
 *   0xffff1800  test finisher
 *
 * The COUNT/COMPARE request is interrupt controller group 0, line 0; the UART is
 * group 1, line 0. The controller's level outputs drive INT0..INT3 and it supplies
 * the autovectors.
 */
#include "uam/boards.h"
#include <string.h>

#include "uam/dev/avr32_intc.h"
#include "uam/dev/test_finisher.h"
#include "uam/dev/uart16550.h"
#include "uam/dev/unknown_mmio.h"
#include "uam/isa/avr32.h"
#include "uam/machine.h"

#define AVR32A_RAM_BASE   UINT64_C(0x00000000)
#define AVR32A_RAM_SIZE   (UINT64_C(16) << 20)
#define AVR32A_CODE_BASE  UINT64_C(0x80000000)
#define AVR32B_CODE_BASE  UINT64_C(0x00000000)
#define AVR32B_RAM_BASE   UINT64_C(0x10000000)
#define AVR32B_RAM_SIZE   (UINT64_C(32) << 20)
#define AVR32_CODE_SIZE   (UINT64_C(8) << 20)
#define AVR32_INTC_BASE   UINT64_C(0xffff0800)
#define AVR32_UART_BASE   UINT64_C(0xffff1400)
#define AVR32_TEST_BASE   UINT64_C(0xffff1800)
#define AVR32_INTC_GROUPS 32u
#define AVR32_COMPARE_IRQ 0u          /* group 0, line 0 */
#define AVR32_UART_IRQ    (1u * 32)   /* group 1, line 0 */

static uam_status avr32_ref_build(uam_machine *m, const uam_board_config *cfg)
{
    uam_status st = uam_machine_init(m, &uam_isa_avr32);
    if (st != UAM_OK)
        return st;
    m->board_name = "avr32-ref";
    const char *model = uam_board_config_option(cfg, "model");
    bool b = model && !strcmp(model, "b");
    if (model && !b && strcmp(model, "a")) {
        uam_log(UAM_LOG_ERROR, m->board_name, "model=%s: a or b", model);
        return UAM_ERR_INVALID;
    }
    if ((st = uam_avr32_configure(&m->cpu, b ? &uam_avr32b_ref : &uam_avr32a_ref)) != UAM_OK)
        return st;
    m->reset_pc = b ? 0xa0000000u : 0x80000000u;   /* overridden by the loaded image's entry */

    uint64_t ram_base = b ? AVR32B_RAM_BASE : AVR32A_RAM_BASE;
    uint64_t ram_size = cfg->ram_size ? cfg->ram_size : b ? AVR32B_RAM_SIZE : AVR32A_RAM_SIZE;
    if ((st = uam_bus_map_ram(&m->bus, "ram", ram_base, ram_size)) != UAM_OK
        || (st = uam_bus_map_ram(&m->bus, "code", b ? AVR32B_CODE_BASE : AVR32A_CODE_BASE,
                                 AVR32_CODE_SIZE)) != UAM_OK)
        return st;

    uam_device *intc = uam_avr32_intc_create("intc", AVR32_INTC_GROUPS);
    uam_device *uart = uam_uart16550_create("uart0", cfg->console);
    uam_device *test = uam_test_finisher_create("test", uam_machine_stop_cb, m);
    if (!intc || !uart || !test) {
        if (intc) intc->ops->destroy(intc);
        if (uart) uart->ops->destroy(uart);
        if (test) test->ops->destroy(test);
        return UAM_ERR_NOMEM;
    }
    uam_machine_add_device(m, intc);
    uam_machine_add_device(m, uart);
    uam_machine_add_device(m, test);
    if ((st = uam_bus_map_mmio(&m->bus, "intc", AVR32_INTC_BASE, UAM_AVR32_INTC_SIZE, intc)) != UAM_OK
        || (st = uam_bus_map_mmio(&m->bus, "uart0", AVR32_UART_BASE, 8, uart)) != UAM_OK
        || (st = uam_bus_map_mmio(&m->bus, "test", AVR32_TEST_BASE, 0x10, test)) != UAM_OK)
        return st;

    for (unsigned l = 0; l < 4; l++)
        uam_signal_connect(uam_avr32_intc_level_line(intc, l), uam_cpu_irq_sink, &m->cpu,
                           UAM_AVR32_IRQ_INT0 + l);
    uam_avr32_set_autovector(&m->cpu, uam_avr32_intc_autovector, intc);
    uam_signal_connect(uam_avr32_compare_line(&m->cpu), uam_avr32_intc_input, intc, AVR32_COMPARE_IRQ);
    uam_signal_connect(uam_uart16550_irq_line(uart), uam_avr32_intc_input, intc, AVR32_UART_IRQ);
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

const uam_board uam_board_avr32_ref = {
    .name = "avr32-ref",
    .description = "AVR32 reference board (interrupt controller, NS16550A UART, test finisher; "
                   "-o model=a|b: AVR32A with MPU or AVR32B with MMU)",
    .build = avr32_ref_build,
};
