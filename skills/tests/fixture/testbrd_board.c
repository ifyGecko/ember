/*
 * testbrd_board.c - regression fixture for tests/run-tests.sh: the generated board with the
 * example devices of ember-dev-build/references/examples wired in, for fixture/fw.
 */
#include <stdlib.h>
#include <string.h>

#include "uam/isa/mips32.h"
#include "uam/dev/unimp.h"
#include "uam/dev/unknown_mmio.h"
#include "uam/loader.h"
#include "uam/machine.h"

#include "uam/dev/test_finisher.h"
#include "uam/dev/uart16550.h"

#include "nor_flash.h"
#include "reg_stride.h"
#include "soc_intc.h"
#include "soc_reset.h"
#include "soc_timer.h"
#include "soc_uart.h"
#include "testbrd_board.h"
#include "testbrd_hle.h"

/* Memory map: PHYSICAL addresses (ember's bus is physical). */
#define BOARD_RAM_BASE   UINT64_C(0x0)
#define BOARD_RAM_SIZE   UINT64_C(0x2000000)
/* Firmware placement, in the guest's own (virtual) addresses: the loader translates them
 * (MIPS kseg0/kseg1 -> physical) before writing to the bus. */
#define BOARD_IMAGE_BASE UINT64_C(0x80000000)  /* RECON: confirmed base */
#define BOARD_ENTRY      UINT64_C(0x80000000)  /* RECON: entry point */
#define BOARD_RESET_VEC  UINT64_C(0xbfc00000)  /* where the hart starts with no --image os= */

/* Catch-alls over EVERY RECON MMIO window (physical), mapped under the real devices at
 * UAM_PRIO_BACKGROUND. Each reads as zero, logs the first access to each offset and prints
 * its hottest offsets at exit: the bring-up to-do list. Keep a window when a real device is
 * mapped over part of it, so stray offsets still log. */
static const struct board_window {
    const char *name;
    uint64_t    base, size;
} board_windows[] = {
    { "periph", UINT64_C(0x1fd00000), 0x10000 },
    { "soc", UINT64_C(0x18000000), 0x100000 },
    { NULL, 0, 0 },
};

/* Registers the entry point expects from the loader that normally jumps there (RECON
 * "entry state"). Applied by a PC hook each time the hart arrives at BOARD_ENTRY, so they
 * also hold after a guest-requested reset. Leave empty if the entry sets them itself. */
static const struct board_reg {
    const char *name;   /* ISA register name: "sp", "a0", "gp", ... */
    uint64_t    value;
} board_entry_regs[] = {
    { "sp", 0x80400000 },  { "a0", 0x1234 },
    { NULL, 0 },
};

typedef struct board {
    char *os_path;   /* --image os=PATH; re-placed on every system reset */
} board;

/* The soc_uart's RX FIFO as the machine input "uart0" (--fuzz-input uart0). */
static uam_status uart_input(void *ctx, uam_machine *m, const uint8_t *data, size_t len)
{
    (void)m;
    soc_uart_feed(ctx, data, len);   /* the excess is dropped, as on a real line */
    return UAM_OK;
}

static uam_status attach(uam_machine *m, uam_device *dev, uint64_t base, uint64_t size)
{
    if (!dev)
        return UAM_ERR_NOMEM;
    uam_machine_add_device(m, dev);
    return uam_bus_map_mmio(&m->bus, dev->name, base, size, dev);
}

static uam_status map_windows(uam_machine *m)
{
    for (const struct board_window *w = board_windows; w->name; w++) {
        uam_device *u = uam_unimp_create(w->name, w->base);
        if (!u)
            return UAM_ERR_NOMEM;
        uam_machine_add_device(m, u);
        uam_status st = uam_bus_map_mmio_ex(&m->bus, w->name, w->base, w->size, u,
                                            &(uam_map_attrs){ .prio = UAM_PRIO_BACKGROUND });
        if (st != UAM_OK)
            return st;
    }
    return UAM_OK;
}

static bool set_reg(uam_cpu *cpu, const char *name, uint64_t value)
{
    for (size_t i = 0; i < cpu->isa->n_regs; i++) {
        const uam_regdesc *r = &cpu->isa->regs[i];
        if (strcmp(r->name, name) == 0) {
            uam_cpu_reg_write(cpu, r->offset, r->width, value);
            return true;
        }
    }
    uam_log(UAM_LOG_ERROR, "testbrd", "entry register '%s' unknown to %s", name, cpu->isa->name);
    return false;
}

static uam_hook_action entry_hook(void *ctx, uam_cpu *cpu, uint64_t pc)
{
    (void)ctx;
    (void)pc;
    for (const struct board_reg *r = board_entry_regs; r->name; r++)
        set_reg(cpu, r->name, r->value);
    return UAM_HOOK_CONTINUE;
}

/* Place the OS image (raw at BOARD_IMAGE_BASE, or ELF by its own headers) and return where
 * the hart should start. */
static uam_status place_os(uam_machine *m, const board *b, uint64_t *entry)
{
    uam_image_info info;
    uam_status st = uam_load_image(&m->bus, &m->cpu, b->os_path, BOARD_IMAGE_BASE, &info);
    if (st != UAM_OK) {
        uam_log(UAM_LOG_ERROR, "testbrd", "cannot load %s: %s", b->os_path, uam_status_str(st));
        return st;
    }
    *entry = info.elf_class ? info.entry : BOARD_ENTRY;
    return UAM_OK;
}

/* A device asked for a system reset (RAM is preserved, devices are reset afterwards). We
 * skip the real boot loader, so do its job: put a fresh copy of the OS in place. */
static void board_reset(uam_machine *m, uam_reset_kind kind, void *ctx)
{
    board *b = ctx;
    uint64_t entry;
    if (kind == UAM_RESET_SYSTEM && b->os_path && place_os(m, b, &entry) == UAM_OK)
        m->reset_pc = entry;
}

static void board_fini(uam_machine *m)
{
    board *b = m->board_data;
    if (b)
        free(b->os_path);
    free(b);
    m->board_data = NULL;
}

static uam_status build(uam_machine *m, const uam_board_config *cfg)
{
    uam_status st = uam_machine_init(m, &uam_isa_mips32be);
    if (st != UAM_OK)
        return st;
    m->board_name = "testbrd";
    m->reset_pc = BOARD_RESET_VEC;
    /* CPU identity the firmware checks: a board-local copy of the reference identity. */
    uam_mips_config cpu_model = uam_mips_ref;
    cpu_model.name = "testbrd-cpu";
    if ((st = uam_mips_configure(&m->cpu, &cpu_model)) != UAM_OK)
        return st;
    board *b = calloc(1, sizeof(*b));
    if (!b)
        return UAM_ERR_NOMEM;
    m->board_data = b;
    m->board_fini = board_fini;
    m->on_reset = board_reset;
    m->on_reset_ctx = b;

    st = uam_bus_map_ram(&m->bus, "ram", BOARD_RAM_BASE, cfg->ram_size ? cfg->ram_size : BOARD_RAM_SIZE);
    if (st != UAM_OK || (st = map_windows(m)) != UAM_OK)
        return st;

    bool con_soc = uam_board_config_option(cfg, "con") && !strcmp(uam_board_config_option(cfg, "con"), "soc");
    uam_device *u16 = uam_uart16550_create("uart1.inner", con_soc ? NULL : cfg->console);
    uam_device *uart1 = reg_stride_create("uart1", u16, 2);
    if (!uart1)
        return UAM_ERR_NOMEM;
    uam_machine_add_device(m, uart1);
    if ((st = uam_bus_map_mmio_ex(&m->bus, "uart1", 0x18020000, 8 << 2, uart1,
                                  &(uam_map_attrs){ .endian = UAM_ACC_BE })) != UAM_OK)
        return st;
    uam_device *uart = soc_uart_create("uart0", con_soc ? cfg->console : NULL);
    uam_device *timer = soc_timer_create("timer", &m->sched, 4);
    uam_device *intc = soc_intc_create("intc");
    uam_device *rst = soc_reset_create("rst");
    if ((st = attach(m, uart, 0x18030000, SOC_UART_SIZE)) != UAM_OK
        || (st = attach(m, timer, 0x18040000, SOC_TIMER_SIZE)) != UAM_OK
        || (st = attach(m, intc, 0x18050000, SOC_INTC_SIZE)) != UAM_OK
        || (st = attach(m, rst, 0x18060000, SOC_RESET_SIZE)) != UAM_OK
        || (st = attach(m, uam_test_finisher_create("test", uam_machine_stop_cb, m), 0x1fd00010, 0x10)) != UAM_OK
        || (st = uam_machine_add_input(m, "uart0", uart_input, uart)) != UAM_OK)
        return st;
    /* 64 KiB NOR flash at phys 0x1f000000 (4 KiB sectors), mirrored once above itself. */
    uam_device *fl = nor_flash_create("flash", &m->bus, 0x1000, 0x000122a8);
    uam_region *flash;
    if (!fl)
        return UAM_ERR_NOMEM;
    uam_machine_add_device(m, fl);
    if ((st = uam_bus_map_romd(&m->bus, "flash", 0x1f000000, 0x10000, fl, NULL, 0, NULL, &flash)) != UAM_OK
        || (st = uam_bus_map_alias(&m->bus, "flash.mirror", 0x1f010000, 0x10000, flash, 0, NULL)) != UAM_OK)
        return st;
    nor_flash_set_region(fl, flash);
    memset(uam_bus_romd_backing(flash), 0xFF, 0x10000);    /* erased chip (no --image flash=) */
    uam_signal_connect(soc_timer_irq_line(timer), soc_intc_input, intc, 3);
    if (!uam_board_config_option(cfg, "noirq"))      /* -o noirq=1: the deadlock test */
        uam_signal_connect(soc_intc_output(intc), uam_cpu_irq_sink, &m->cpu, UAM_MIPS_IRQ_HW0);
    uam_signal_connect(soc_reset_line(rst), uam_machine_reset_sink, m, UAM_RESET_SYSTEM);

    /* Addresses no window covers: fault (default, like a bus error) or log (--unknown-mmio). */
    if (cfg->unknown_mmio != UAM_UNKNOWN_FAULT) {
        uam_device *um = uam_unknown_mmio_create(
            "unknown-mmio", cfg->unknown_mmio == UAM_UNKNOWN_ECHO ? UAM_UNKNOWN_POLICY_ECHO
                                                                  : UAM_UNKNOWN_POLICY_ZERO);
        if (!um)
            return UAM_ERR_NOMEM;
        uam_machine_add_device(m, um);
        uam_bus_set_unmapped(&m->bus, um);
    }

    uint64_t entry = BOARD_RESET_VEC;
    const char *os = uam_board_config_image(cfg, "os");
    if (os) {
        if (!(b->os_path = strdup(os)))
            return UAM_ERR_NOMEM;
        if ((st = place_os(m, b, &entry)) != UAM_OK)
            return st;
    }
    if (board_entry_regs[0].name && (st = uam_exec_add_pc_hook(&m->exec, entry, entry_hook, b)) != UAM_OK)
        return st;

    if ((st = testbrd_hle_install(m, cfg)) != UAM_OK)
        return st;
    uam_machine_reset(m, entry);
    return UAM_OK;
}

const uam_board testbrd_board = {
    .name = "testbrd",
    .description = "testbrd firmware emulation (mips32be)",
    .build = build,
};
