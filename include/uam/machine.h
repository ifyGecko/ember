/*
 * uam/machine.h - Machine composition and boards.
 *
 * A machine owns one instance of each core service (bus, scheduler, hart,
 * executor) plus the devices a board creates. Boards are the only code that
 * knows concrete ISAs and devices at the same time; they build a machine
 * from a uam_board_config.
 *
 * Dependencies: everything public (this is the composition layer).
 */
#ifndef UAM_MACHINE_H
#define UAM_MACHINE_H

#include <stdio.h>

#include "uam/bus.h"
#include "uam/chardev.h"
#include "uam/cpu.h"
#include "uam/device.h"
#include "uam/exec.h"
#include "uam/isa.h"
#include "uam/sched.h"

typedef enum uam_unknown_mmio_policy {
    UAM_UNKNOWN_FAULT, /* unmapped access faults (like real hardware bus errors) */
    UAM_UNKNOWN_ZERO,  /* reads return 0, writes ignored; logged */
    UAM_UNKNOWN_ECHO,  /* reads return the last value written to that address; logged */
} uam_unknown_mmio_policy;

/* A board-specific option, passed through untouched ("-o key=value"). */
typedef struct uam_board_option {
    const char *key, *value;
} uam_board_option;

/* A named image for the board to place itself ("--image flash=fw.bin"). */
typedef struct uam_board_image {
    const char *name, *path;
} uam_board_image;

typedef struct uam_board_config {
    uint64_t                ram_size;      /* 0 = board default */
    uam_unknown_mmio_policy unknown_mmio;
    uam_chardev            *console;       /* console backend; NULL discards output.
                                            * Owned by the caller; must outlive the machine. */
    const uam_board_option *options;
    size_t                  n_options;
    const uam_board_image  *images;
    size_t                  n_images;
} uam_board_config;

/* Lookups for boards: the last matching entry, or NULL. */
const char *uam_board_config_option(const uam_board_config *cfg, const char *key);
const char *uam_board_config_image(const uam_board_config *cfg, const char *name);

typedef struct uam_machine uam_machine;

/*
 * Board hook run when the guest (or the host) requests a reset, before the
 * machine is reset. Typical work: re-load boot images into RAM (QEMU's "ROM
 * reset"), or choose m->reset_pc. Devices are reset by their own `reset` op.
 */
typedef void (*uam_machine_reset_hook)(uam_machine *m, uam_reset_kind kind, void *ctx);

struct uam_machine {
    const char *board_name;
    uam_bus     bus;
    uam_sched   sched;
    uam_cpu     cpu;
    uam_exec    exec;
    uam_device *devices;   /* owned; destroyed with the machine */
    uint64_t    reset_pc;  /* where a reset starts: the board vector, then the last entry
                            * passed to uam_machine_reset() (e.g. the image entry) */
    uam_machine_reset_hook on_reset;
    void                  *on_reset_ctx;
    void       *board_data;              /* board-private state, if any */
    void      (*board_fini)(uam_machine *m); /* frees board_data; runs before devices */
};

uam_status uam_machine_init(uam_machine *m, const uam_isa *isa);
void       uam_machine_fini(uam_machine *m);
/* Take ownership of a device (reset/destroy with the machine). */
void       uam_machine_add_device(uam_machine *m, uam_device *dev);
/* Reset the hart and every device; start executing at `entry` (which becomes
 * the machine's reset_pc). RAM and the translation cache are preserved. */
void       uam_machine_reset(uam_machine *m, uint64_t entry);
/* Stop callback usable by devices: ctx is the machine. */
void       uam_machine_stop_cb(void *ctx, int64_t status);
/*
 * Reset line sink usable by devices: a rising edge requests a reset of kind
 * `input` (uam_reset_kind), performed between instructions without leaving
 * uam_exec_run. The device lowers its line again, typically in its `reset`.
 *     uam_signal_connect(&rst->sys_reset, uam_machine_reset_sink, m, UAM_RESET_SYSTEM);
 */
void       uam_machine_reset_sink(void *machine, unsigned input, int level);

typedef struct uam_board {
    const char *name;
    const char *description;
    uam_status (*build)(uam_machine *m, const uam_board_config *cfg);
} uam_board;

/*
 * Board registry. The library contains no boards; a program registers the
 * ones it links (see uam/boards.h for the stock set, which uam-run uses).
 * Registering a name twice replaces the earlier board.
 */
#define UAM_MAX_BOARDS 64u
uam_status       uam_board_register(const uam_board *board);
const uam_board *uam_board_find(const char *name);
void             uam_board_list(FILE *out);

#endif /* UAM_MACHINE_H */
