/*
 * avr_ref.c - "avr-ref": the 8-bit AVR reference board. Program memory, internal SRAM,
 * a 16550 console, a general-purpose timer and a simulation finisher; no peripheral of
 * any AVR part.
 *
 * -o model= picks the core family and a memory layout of common devices of that family,
 * so the start-up code of their compilers runs unchanged (build the firmware for the
 * part named; only its memory layout matters):
 *
 *   model  core   program   SRAM (data addresses)  devices   compile with
 *   e      AVRe   32 KiB    0x0100-0x08FF          0x00E0    -mmcu=atmega328p
 *   eplus  AVRe+  256 KiB   0x0200-0x21FF          0x00E0    -mmcu=atmega2560 (default)
 *   xm     AVRxm  136 KiB   0x2000-0x3FFF          0x0FE0    -mmcu=atxmega128a1u
 *   xt     AVRxt  48 KiB    0x2800-0x3FFF          0x0FE0    -mmcu=atmega4809
 *   rc     AVRrc  4 KiB     0x0040-0x013F          0x1000    -mmcu=attiny40
 *
 * AVRxt and AVRrc also see program memory in the data space at 0x4000, as those devices
 * do. I/O addresses 0x00-0x1F, the ones SBI/CBI/SBIS/SBIC reach, are 32 general-purpose
 * I/O registers (plain storage, like the GPIOR registers of AVR devices). On the bus program memory is at 0 and data address a at 0x800000 + a
 * (uam/isa/avr.h). The devices, from the device base:
 *
 *   +0x00  NS16550A UART (byte registers), interrupt vector 1
 *   +0x08  test finisher (byte commands: uam/dev/test_finisher.h)
 *   +0x10  general-purpose timer (uam/dev/gptimer.h), interrupt vector 2; taking the
 *          vector clears its flag, as AVR peripherals' flags do
 *
 * On AVRxm both vectors are low level (PMIC.CTRL's LOLVLEN enables them), on AVRxt
 * level 0. An AVR has one processor: -o cpus is 1.
 */
#include "uam/boards.h"
#include <stdlib.h>
#include <string.h>

#include "uam/dev/gptimer.h"
#include "uam/dev/test_finisher.h"
#include "uam/dev/uart16550.h"
#include "uam/dev/unknown_mmio.h"
#include "uam/isa/avr.h"
#include "uam/machine.h"

#define AVR_UART_VECTOR  1u
#define AVR_TIMER_VECTOR 2u

typedef struct avr_model {
    const char           *name;
    const uam_avr_config *cfg;
    uint32_t              sram_lo, sram_hi;   /* data addresses, inclusive */
    uint32_t              dev_base;
    bool                  flash_in_data;      /* program memory at data 0x4000 */
} avr_model;

static const avr_model models[] = {
    { "e",     &uam_avr_e_ref,     0x0100, 0x08FF, 0x00E0, false },
    { "eplus", &uam_avr_eplus_ref, 0x0200, 0x21FF, 0x00E0, false },
    { "xm",    &uam_avr_xm_ref,    0x2000, 0x3FFF, 0x0FE0, false },
    { "xt",    &uam_avr_xt_ref,    0x2800, 0x3FFF, 0x0FE0, true },
    { "rc",    &uam_avr_rc_ref,    0x0040, 0x013F, 0x1000, true },
};

static void timer_ack(void *ctx, unsigned vector)
{
    if (vector == AVR_TIMER_VECTOR)
        uam_gptimer_ack(ctx);
}

static uam_status avr_ref_build(uam_machine *m, const uam_board_config *cfg)
{
    const char *mname = uam_board_config_option(cfg, "model");
    const avr_model *md = NULL;
    for (size_t i = 0; i < UAM_ARRAY_LEN(models); i++)
        if (!mname ? strcmp(models[i].name, "eplus") == 0 : strcmp(models[i].name, mname) == 0)
            md = &models[i];
    if (!md) {
        uam_log(UAM_LOG_ERROR, "avr-ref", "model=%s: e, eplus, xm, xt or rc", mname);
        return UAM_ERR_INVALID;
    }
    const char *ncpu = uam_board_config_option(cfg, "cpus");
    if (ncpu && strtoul(ncpu, NULL, 0) != 1) {
        uam_log(UAM_LOG_ERROR, "avr-ref", "cpus=%s: an AVR has one processor", ncpu);
        return UAM_ERR_INVALID;
    }

    uam_status st = uam_machine_init(m, &uam_isa_avr);
    if (st != UAM_OK)
        return st;
    m->board_name = "avr-ref";
    m->reset_pc = 0;
    if ((st = uam_avr_configure(&m->cpu, md->cfg)) != UAM_OK)
        return st;

    /* Program memory, erased (0xFF) until the image is loaded. */
    const uint32_t flash = md->cfg->flash_size;
    uint8_t *erased = malloc(flash);
    if (!erased)
        return UAM_ERR_NOMEM;
    memset(erased, 0xFF, flash);
    st = uam_bus_map_rom(&m->bus, "flash", 0, flash, erased, flash);
    free(erased);
    if (st != UAM_OK)
        return st;
    if (md->flash_in_data) {
        const uint64_t n = flash < 0xC000 ? flash : 0xC000;
        if ((st = uam_bus_map_alias(&m->bus, "flash-data", UAM_AVR_DATA_BASE + 0x4000, n,
                                    uam_bus_region(&m->bus, "flash"), 0, NULL)) != UAM_OK)
            return st;
    }
    if ((st = uam_bus_map_ram(&m->bus, "gpior", UAM_AVR_DATA_BASE + (md->cfg->core <= UAM_AVR_EPLUS ? 0x20 : 0),
                              0x20)) != UAM_OK)
        return st;
    const uint64_t sram = cfg->ram_size ? cfg->ram_size : md->sram_hi - md->sram_lo + 1;
    if ((st = uam_bus_map_ram(&m->bus, "sram", UAM_AVR_DATA_BASE + md->sram_lo, sram)) != UAM_OK)
        return st;

    uam_device *uart = uam_uart16550_create("uart0", cfg->console);
    uam_device *test = uam_test_finisher_create("test", uam_machine_stop_cb, m);
    uam_device *timer = uam_gptimer_create("timer0", &m->sched);
    if (!uart || !test || !timer) {
        if (uart) uart->ops->destroy(uart);
        if (test) test->ops->destroy(test);
        if (timer) timer->ops->destroy(timer);
        return UAM_ERR_NOMEM;
    }
    uam_machine_add_device(m, uart);
    uam_machine_add_device(m, test);
    uam_machine_add_device(m, timer);
    const uint64_t dev = UAM_AVR_DATA_BASE + md->dev_base;
    if ((st = uam_bus_map_mmio(&m->bus, "uart0", dev, 8, uart)) != UAM_OK
        || (st = uam_bus_map_mmio(&m->bus, "test", dev + 0x08, 4, test)) != UAM_OK
        || (st = uam_bus_map_mmio(&m->bus, "timer0", dev + 0x10, UAM_GPTIMER_SIZE, timer)) != UAM_OK)
        return st;

    uam_signal_connect(uam_uart16550_irq_line(uart), uam_cpu_irq_sink, &m->cpu, AVR_UART_VECTOR);
    uam_signal_connect(uam_gptimer_irq(timer), uam_cpu_irq_sink, &m->cpu, AVR_TIMER_VECTOR);
    uam_avr_set_irq_ack(&m->cpu, timer_ack, timer);
    if (md->cfg->core == UAM_AVR_XM || md->cfg->core == UAM_AVR_XT) {
        const unsigned lvl = md->cfg->core == UAM_AVR_XM ? 1 : 0;   /* low level / level 0 */
        uam_avr_set_irq_level(&m->cpu, AVR_UART_VECTOR, lvl);
        uam_avr_set_irq_level(&m->cpu, AVR_TIMER_VECTOR, lvl);
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

const uam_board uam_board_avr_ref = {
    .name = "avr-ref",
    .description = "8-bit AVR reference board (-o model=e|eplus|xm|xt|rc; NS16550A UART, timer, test finisher)",
    .build = avr_ref_build,
};
