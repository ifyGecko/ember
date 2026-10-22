/*
 * ext_board.c - "ext-mips": a board defined outside the UAM tree.
 *
 * The same memory map as the stock mips-ref board (so the MIPS self-test runs
 * on it), plus what out-of-tree boards typically need: a CPU identity of the
 * board's own (here derived from the reference identity), a catch-all window for the not-yet-modelled peripheral block
 * under the real devices, a board option (-o banner=TEXT) and a named image
 * slot (--image rom=FILE, loaded at the reset vector).
 */
#include <stdio.h>

#include "uam/dev/test_finisher.h"
#include "uam/dev/uart16550.h"
#include "uam/dev/unimp.h"
#include "uam/isa/mips32.h"
#include "uam/loader.h"

#include "ext_board.h"

#define RAM_SIZE   (UINT64_C(32) << 20)
#define PERIPH     UINT64_C(0x1fd00000)   /* peripheral block */
#define UART_PHYS  UINT64_C(0x1fd003f8)
#define TEST_PHYS  UINT64_C(0x1fd00010)
#define ROM_PHYS   UINT64_C(0x1fc00000)

static uam_status attach(uam_machine *m, uam_device *dev, uint64_t base, uint64_t size)
{
    if (!dev)
        return UAM_ERR_NOMEM;
    uam_machine_add_device(m, dev);
    return uam_bus_map_mmio(&m->bus, dev->name, base, size, dev);
}

static uam_status build(uam_machine *m, const uam_board_config *cfg)
{
    uam_status st = uam_machine_init(m, &uam_isa_mips32be);
    if (st != UAM_OK)
        return st;
    m->board_name = "ext-mips";
    m->reset_pc = 0xBFC00000u;
    /* A board describes its own processor; this one keeps the reference
     * feature set under its own name. */
    uam_mips_config cpu = uam_mips_ref;
    cpu.name = "ext-core";
    if ((st = uam_mips_configure(&m->cpu, &cpu)) != UAM_OK)
        return st;

    if ((st = uam_bus_map_ram(&m->bus, "ram", 0, cfg->ram_size ? cfg->ram_size : RAM_SIZE)) != UAM_OK)
        return st;

    /* Everything in the peripheral block that is not modelled reads as zero. */
    uam_device *gaps = uam_unimp_create("periph", PERIPH);
    if (!gaps)
        return UAM_ERR_NOMEM;
    uam_machine_add_device(m, gaps);
    st = uam_bus_map_mmio_ex(&m->bus, "periph", PERIPH, 0x10000, gaps,
                             &(uam_map_attrs){ .prio = UAM_PRIO_BACKGROUND });
    if (st != UAM_OK)
        return st;

    uam_device *uart = uam_uart16550_create("uart0", cfg->console);
    if ((st = attach(m, uart, UART_PHYS, 8)) != UAM_OK
        || (st = attach(m, uam_test_finisher_create("test", uam_machine_stop_cb, m), TEST_PHYS, 0x10))
               != UAM_OK)
        return st;
    uam_signal_connect(uam_uart16550_irq_line(uart), uam_cpu_irq_sink, &m->cpu, UAM_MIPS_IRQ_HW0);

    const char *rom = uam_board_config_image(cfg, "rom");
    if (rom) {
        if ((st = uam_bus_map_rom(&m->bus, "rom", ROM_PHYS, 0x400000, NULL, 0)) != UAM_OK)
            return st;
        uam_image_info info;
        if ((st = uam_load_raw(&m->bus, NULL, rom, ROM_PHYS, &info)) != UAM_OK)
            return st;
    }

    const char *banner = uam_board_config_option(cfg, "banner");
    if (banner)
        fprintf(stderr, "ext-mips: %s\n", banner);

    uam_machine_reset(m, m->reset_pc);
    return UAM_OK;
}

const uam_board ext_board_mips = {
    .name = "ext-mips",
    .description = "out-of-tree example: MIPS32 BE, mips-ref-like map",
    .build = build,
};
