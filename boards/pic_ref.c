/*
 * pic_ref.c - "pic-ref": the 8-bit PIC reference board. Program memory, a data space of
 * RAM, a 16550 console, a general-purpose timer and a simulation finisher; no peripheral
 * of any PIC part.
 *
 * -o model= picks the core family and the program memory and data space size of a common
 * device of that family (uam/isa/pic.h):
 *
 *   model     core              program     data RAM (data addresses)    devices
 *   baseline  baseline 12-bit   2K words    0x000-0x0FF, 8 banks         0xF0 (bank 7)
 *   mid       mid-range 14-bit  8K words    0x000-0x1FF, 4 banks         0x10C, timer 0x18C
 *   enhanced  enhanced 14-bit   32K words   0x000-0xFFF, 32 banks        0x10C, timer 0x18C
 *   pic18     PIC18 (default)   64 KiB      0x000-0x7FF, 0xF60-0xFFF     0xF60, timer 0xF70
 *
 * -o xinst=1 turns on PIC18's extended instruction set (the XINST configuration bit).
 *
 * Every data address the core does not own is RAM, except PIC18's 0x800-0xF5F (nothing).
 * On the 14-bit families the last 16 bytes of each bank (0x70-0x7F) are one common RAM, as
 * on the parts. The devices, from the device base:
 *
 *   +0x00  NS16550A UART (byte registers), interrupt line UAM_PIC_IRQ_LOW
 *   +0x08  test finisher (byte commands: uam/dev/test_finisher.h)
 *   timer  general-purpose timer (uam/dev/gptimer.h), interrupt line UAM_PIC_IRQ_PERIPH
 *
 * Both lines are peripheral interrupts (PEIE); with PIC18's IPEN the timer is high priority
 * and the UART low. A RESET instruction, or a stack overflow or underflow with STVREN (on
 * for the enhanced and PIC18 models), resets the board. A PIC has one processor: -o cpus
 * is 1.
 */
#include "uam/boards.h"
#include <stdlib.h>
#include <string.h>

#include "uam/dev/gptimer.h"
#include "uam/dev/test_finisher.h"
#include "uam/dev/uart16550.h"
#include "uam/dev/unknown_mmio.h"
#include "uam/isa/pic.h"
#include "uam/machine.h"

typedef struct pic_model {
    const char           *name;
    const uam_pic_config *cfg;
    uint32_t              ram_lo, ram_hi;     /* data addresses, inclusive */
    uint32_t              sfr_lo;             /* PIC18: RAM of the access bank's upper half */
    uint32_t              dev_base, timer;    /* timer: 0 for none */
    unsigned              common_banks;       /* banks that share 0x70-0x7F */
} pic_model;

static const pic_model models[] = {
    { "baseline", &uam_pic_baseline_ref, 0x000, 0x0FF, 0, 0x0F0, 0, 0 },
    { "mid",      &uam_pic_mid_ref,      0x000, 0x1FF, 0, 0x10C, 0x18C, 4 },
    { "enhanced", &uam_pic_enhanced_ref, 0x000, 0xFFF, 0, 0x10C, 0x18C, 32 },
    { "pic18",    &uam_pic18_ref,        0x000, 0x7FF, 0xF60, 0xF60, 0xF70, 0 },
};

static void reset_board(void *ctx, uam_pic_reset_cause cause)
{
    (void)cause;
    uam_machine_reset_sink(ctx, UAM_RESET_SYSTEM, 1);
}

static uam_status pic_ref_build(uam_machine *m, const uam_board_config *cfg)
{
    const char *mname = uam_board_config_option(cfg, "model");
    const pic_model *md = NULL;
    for (size_t i = 0; i < UAM_ARRAY_LEN(models); i++)
        if (strcmp(models[i].name, mname ? mname : "pic18") == 0)
            md = &models[i];
    if (!md) {
        uam_log(UAM_LOG_ERROR, "pic-ref", "model=%s: baseline, mid, enhanced or pic18", mname);
        return UAM_ERR_INVALID;
    }
    const char *ncpu = uam_board_config_option(cfg, "cpus");
    if (ncpu && strtoul(ncpu, NULL, 0) != 1) {
        uam_log(UAM_LOG_ERROR, "pic-ref", "cpus=%s: a PIC has one processor", ncpu);
        return UAM_ERR_INVALID;
    }
    uam_pic_config pc = *md->cfg;
    const char *xinst = uam_board_config_option(cfg, "xinst");
    if (xinst && strcmp(xinst, "0") != 0) {
        if (pc.core != UAM_PIC18) {
            uam_log(UAM_LOG_ERROR, "pic-ref", "xinst: only the pic18 model has the extended instruction set");
            return UAM_ERR_INVALID;
        }
        pc.features |= UAM_PIC_F_XINST;
        pc.name = "pic18-xinst-ref";
    }

    uam_status st = uam_machine_init(m, &uam_isa_pic);
    if (st != UAM_OK)
        return st;
    m->board_name = "pic-ref";
    if ((st = uam_pic_configure(&m->cpu, &pc)) != UAM_OK)
        return st;
    /* the reset vector: the last word on baseline */
    m->reset_pc = pc.core == UAM_PIC_BASELINE ? 2u * (pc.prog_words - 1u) : 0;

    /* Program memory, erased (0xFF) until the image is loaded. */
    const uint32_t flash = 2u * pc.prog_words;
    uint8_t *erased = malloc(flash);
    if (!erased)
        return UAM_ERR_NOMEM;
    memset(erased, 0xFF, flash);
    st = uam_bus_map_rom(&m->bus, "flash", 0, flash, erased, flash);
    free(erased);
    if (st != UAM_OK)
        return st;

    const uint64_t data = UAM_PIC_DATA_BASE;
    const uint64_t ram = cfg->ram_size ? cfg->ram_size : md->ram_hi - md->ram_lo + 1u;
    if ((st = uam_bus_map_ram(&m->bus, "ram", data + md->ram_lo, ram)) != UAM_OK)
        return st;
    if (md->sfr_lo && (st = uam_bus_map_ram(&m->bus, "sfr-ram", data + md->sfr_lo, 0x1000u - md->sfr_lo)) != UAM_OK)
        return st;
    if (pc.core == UAM_PIC_BASELINE
        && (st = uam_bus_map_ram(&m->bus, "option-tris", data + UAM_PIC_SPECIAL, 0x20)) != UAM_OK)
        return st;
    const uam_map_attrs over = { .prio = 1 };
    const uam_region *ramr = uam_bus_region(&m->bus, "ram");
    for (unsigned b = 1; b < md->common_banks; b++) {
        char name[24];
        snprintf(name, sizeof(name), "common%u", b);
        if ((st = uam_bus_map_alias(&m->bus, name, data + b * 0x80u + 0x70u, 0x10, ramr, 0x70, &over)) != UAM_OK)
            return st;
    }

    uam_device *uart = uam_uart16550_create("uart0", cfg->console);
    uam_device *test = uam_test_finisher_create("test", uam_machine_stop_cb, m);
    uam_device *timer = md->timer ? uam_gptimer_create("timer0", &m->sched) : NULL;
    if (!uart || !test || (md->timer && !timer)) {
        if (uart) uart->ops->destroy(uart);
        if (test) test->ops->destroy(test);
        if (timer) timer->ops->destroy(timer);
        return UAM_ERR_NOMEM;
    }
    uam_machine_add_device(m, uart);
    uam_machine_add_device(m, test);
    if (timer)
        uam_machine_add_device(m, timer);
    const uint64_t dev = data + md->dev_base;
    if ((st = uam_bus_map_mmio_ex(&m->bus, "uart0", dev, 8, uart, &over)) != UAM_OK
        || (st = uam_bus_map_mmio_ex(&m->bus, "test", dev + 0x08, 4, test, &over)) != UAM_OK)
        return st;
    if (timer && (st = uam_bus_map_mmio_ex(&m->bus, "timer0", data + md->timer, UAM_GPTIMER_SIZE, timer, &over)) != UAM_OK)
        return st;
    if (pc.core != UAM_PIC_BASELINE)   /* baseline parts have no interrupts */
        uam_signal_connect(uam_uart16550_irq_line(uart), uam_cpu_irq_sink, &m->cpu, UAM_PIC_IRQ_LOW);
    if (timer)
        uam_signal_connect(uam_gptimer_irq(timer), uam_cpu_irq_sink, &m->cpu, UAM_PIC_IRQ_PERIPH);
    uam_pic_set_reset_hook(&m->cpu, reset_board, m);
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

const uam_board uam_board_pic_ref = {
    .name = "pic-ref",
    .description = "8-bit PIC reference board (-o model=baseline|mid|enhanced|pic18, xinst=1; "
                   "NS16550A UART, timer, test finisher)",
    .build = pic_ref_build,
};
