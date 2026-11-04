/*
 * pic32_ref.c - "pic32-ref": the PIC32 reference board, a little-endian MIPS
 * microcontroller with the PIC32 memory map, its vectored interrupt controller,
 * a 16550 console, a general-purpose timer and a simulation finisher.
 *
 *   -o model=mx (the default)  uam_mips_m16_ref: MIPS32r2 + MIPS16e, fixed mapping;
 *                              computed-offset interrupt controller, 96 sources, 64 vectors
 *                              (priority 7 uses shadow set 1); 128 KiB RAM, 512 KiB flash
 *   -o model=mm                uam_mips_umips_ref: microMIPS only, fixed mapping;
 *                              variable-offset controller, 64 sources; 32 KiB RAM, 256 KiB flash
 *   -o model=mz                uam_mips_mz_ref: MIPS32r2 + microMIPS, TLB, FPU, DSP r2;
 *                              variable-offset controller, 256 sources; 512 KiB RAM, 2 MiB flash
 *   -o bootisa=micromips       (mz) reset into microMIPS (Config3.ISA = 3)
 *
 * Physical map (kseg0 = phys + 0x80000000, kseg1 = phys + 0xa0000000):
 *   0x00000000  RAM (--ram overrides the size)
 *   0x1d000000  program flash (read-only; loaded from the image)
 *   0x1f810000  interrupt controller, variable-offset layout (mm, mz)
 *   0x1f820000  NS16550A UART (byte registers), source 3
 *   0x1f820100  test finisher
 *   0x1f820200  general-purpose timer (uam/dev/gptimer.h), source 4
 *   0x1f881000  interrupt controller, computed-offset layout (mx)
 *   0x1fc00000  boot flash (read-only, 64 KiB); the reset address is 0xbfc00000
 *
 * Interrupt sources 0, 1 and 2 are the processor's own: the Count/Compare timer
 * and the two software interrupts (Cause.IP0/1).
 */
#include "uam/boards.h"
#include <stdlib.h>
#include <string.h>

#include "uam/dev/gptimer.h"
#include "uam/dev/pic32_intc.h"
#include "uam/dev/test_finisher.h"
#include "uam/dev/uart16550.h"
#include "uam/dev/unknown_mmio.h"
#include "uam/isa/mips32.h"
#include "uam/machine.h"

#define PIC32_RAM_BASE    UINT64_C(0x00000000)
#define PIC32_FLASH_BASE  UINT64_C(0x1d000000)
#define PIC32_INTC_VO     UINT64_C(0x1f810000)
#define PIC32_UART_BASE   UINT64_C(0x1f820000)
#define PIC32_TEST_BASE   UINT64_C(0x1f820100)
#define PIC32_TIMER_BASE  UINT64_C(0x1f820200)
#define PIC32_INTC_CO     UINT64_C(0x1f881000)
#define PIC32_BOOT_BASE   UINT64_C(0x1fc00000)
#define PIC32_BOOT_SIZE   (UINT64_C(64) << 10)

enum { IRQ_CORE_TIMER = 0, IRQ_CORE_SW0 = 1, IRQ_CORE_SW1 = 2, IRQ_UART = 3, IRQ_TIMER = 4 };

typedef struct pic32_model {
    const char             *name;
    const uam_mips_config  *cpu;
    bool                    variable_offset;
    unsigned                n_irqs, n_vectors;
    uint64_t                ram, flash;
} pic32_model;

static const pic32_model models[] = {
    { "mx", &uam_mips_m16_ref,   false, 96, 64, UINT64_C(128) << 10, UINT64_C(512) << 10 },
    { "mm", &uam_mips_umips_ref, true,  64, 64, UINT64_C(32) << 10,  UINT64_C(256) << 10 },
    { "mz", &uam_mips_mz_ref,    true, 256, 256, UINT64_C(512) << 10, UINT64_C(2) << 20 },
};

static uam_status map_flash(uam_machine *m, const char *name, uint64_t base, uint64_t size)
{
    uint8_t *erased = malloc((size_t)size);
    if (!erased)
        return UAM_ERR_NOMEM;
    memset(erased, 0xFF, (size_t)size);
    uam_status st = uam_bus_map_rom(&m->bus, name, base, size, erased, (size_t)size);
    free(erased);
    return st;
}

static uam_status pic32_ref_build(uam_machine *m, const uam_board_config *cfg)
{
    uam_status st = uam_machine_init(m, &uam_isa_mips32le);
    if (st != UAM_OK)
        return st;
    m->board_name = "pic32-ref";
    const char *model = uam_board_config_option(cfg, "model");
    const pic32_model *md = &models[0];
    if (model) {
        md = NULL;
        for (size_t i = 0; i < UAM_ARRAY_LEN(models); i++)
            if (!strcmp(model, models[i].name))
                md = &models[i];
        if (!md) {
            uam_log(UAM_LOG_ERROR, m->board_name, "model=%s: mx, mm or mz", model);
            return UAM_ERR_INVALID;
        }
    }
    uam_mips_config cpu = *md->cpu;
    const char *bootisa = uam_board_config_option(cfg, "bootisa");
    if (bootisa) {
        bool um = !strcmp(bootisa, "micromips");
        if ((!um && strcmp(bootisa, "mips32")) || ((cpu.config3 >> 14) & 3) < 2) {
            uam_log(UAM_LOG_ERROR, m->board_name,
                    "bootisa=%s: mips32 or micromips, on a model with both (mz)", bootisa);
            return UAM_ERR_INVALID;
        }
        cpu.config3 = (cpu.config3 & ~(3u << 14)) | ((um ? 3u : 2u) << 14);
        if (um)
            cpu.config3 |= 1u << 16;   /* ISAOnExc: handlers in microMIPS too */
    }
    if ((st = uam_mips_configure(&m->cpu, &cpu)) != UAM_OK)
        return st;
    m->reset_pc = 0xBFC00000u;   /* overridden by the loaded image's entry */

    if ((st = uam_bus_map_ram(&m->bus, "ram", PIC32_RAM_BASE,
                              cfg->ram_size ? cfg->ram_size : md->ram)) != UAM_OK
        || (st = map_flash(m, "flash", PIC32_FLASH_BASE, md->flash)) != UAM_OK
        || (st = map_flash(m, "boot-flash", PIC32_BOOT_BASE, PIC32_BOOT_SIZE)) != UAM_OK)
        return st;

    const uam_pic32_intc_config icfg = {
        .n_irqs = md->n_irqs, .n_vectors = md->n_vectors,
        .variable_offset = md->variable_offset, .srs_priority = md->variable_offset ? 0 : 7,
    };
    uam_device *intc = uam_pic32_intc_create("intc", &icfg);
    uam_device *uart = uam_uart16550_create("uart0", cfg->console);
    uam_device *test = uam_test_finisher_create("test", uam_machine_stop_cb, m);
    uam_device *timer = uam_gptimer_create("timer", &m->sched);
    if (!intc || !uart || !test || !timer) {
        if (intc) intc->ops->destroy(intc);
        if (uart) uart->ops->destroy(uart);
        if (test) test->ops->destroy(test);
        if (timer) timer->ops->destroy(timer);
        return UAM_ERR_NOMEM;
    }
    uam_machine_add_device(m, intc);
    uam_machine_add_device(m, uart);
    uam_machine_add_device(m, test);
    uam_machine_add_device(m, timer);
    if ((st = uam_bus_map_mmio(&m->bus, "intc", md->variable_offset ? PIC32_INTC_VO : PIC32_INTC_CO,
                               UAM_PIC32_INTC_SIZE, intc)) != UAM_OK
        || (st = uam_bus_map_mmio(&m->bus, "uart0", PIC32_UART_BASE, 8, uart)) != UAM_OK
        || (st = uam_bus_map_mmio(&m->bus, "test", PIC32_TEST_BASE, 0x10, test)) != UAM_OK
        || (st = uam_bus_map_mmio(&m->bus, "timer", PIC32_TIMER_BASE, UAM_GPTIMER_SIZE, timer)) != UAM_OK)
        return st;

    uam_pic32_intc_set_eic(intc, uam_mips_eic_input, &m->cpu);
    uam_signal_connect(uam_mips_timer_line(&m->cpu), uam_pic32_intc_input, intc, IRQ_CORE_TIMER);
    uam_signal_connect(uam_mips_swint_line(&m->cpu, 0), uam_pic32_intc_input, intc, IRQ_CORE_SW0);
    uam_signal_connect(uam_mips_swint_line(&m->cpu, 1), uam_pic32_intc_input, intc, IRQ_CORE_SW1);
    uam_signal_connect(uam_uart16550_irq_line(uart), uam_pic32_intc_input, intc, IRQ_UART);
    uam_signal_connect(uam_gptimer_irq(timer), uam_pic32_intc_input, intc, IRQ_TIMER);
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

const uam_board uam_board_pic32_ref = {
    .name = "pic32-ref",
    .description = "PIC32 reference board (vectored interrupt controller, NS16550A UART, timer, "
                   "test finisher; -o model=mx|mm|mz: MIPS16e, microMIPS-only, or microMIPS "
                   "with FPU and DSP)",
    .build = pic32_ref_build,
};
