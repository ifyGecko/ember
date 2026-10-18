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
#include "uam/netdev.h"
#include "uam/sched.h"
#include "uam/state.h"

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

/*
 * A network attachment for a board port ("--nic wan=tap:tap0"). Boards look
 * ports up by their own names (uam_board_config_netdev); a port with no
 * attachment is a cable left unplugged. The netdev is owned by the caller and
 * must outlive the machine; the caller also services it (uam_host_add_netdev).
 */
typedef struct uam_board_netdev {
    const char *name;
    uam_netdev *nd;
} uam_board_netdev;

typedef struct uam_board_config {
    uint64_t                ram_size;      /* 0 = board default */
    uam_unknown_mmio_policy unknown_mmio;
    uam_chardev            *console;       /* console backend; NULL discards output.
                                            * Owned by the caller; must outlive the machine. */
    const uam_board_option *options;
    size_t                  n_options;
    const uam_board_image  *images;
    size_t                  n_images;
    const uam_board_netdev *netdevs;
    size_t                  n_netdevs;
} uam_board_config;

/* Lookups for boards: the last matching entry, or NULL. */
const char *uam_board_config_option(const uam_board_config *cfg, const char *key);
const char *uam_board_config_image(const uam_board_config *cfg, const char *name);
uam_netdev *uam_board_config_netdev(const uam_board_config *cfg, const char *name);

typedef struct uam_machine uam_machine;

/*
 * Board hook run when the guest (or the host) requests a reset, before the
 * machine is reset. Typical work: re-load boot images into RAM (a reboot may
 * have overwritten them), or choose m->reset_pc. Devices are reset by their own `reset` op.
 */
typedef void (*uam_machine_reset_hook)(uam_machine *m, uam_reset_kind kind, void *ctx);

/* Extra state saved in snapshots (uam_machine_add_state). */
typedef void (*uam_state_save_fn)(void *ctx, uam_state_writer *w);
typedef void (*uam_state_restore_fn)(void *ctx, uam_state_reader *r);
typedef struct uam_state_part {
    char                *name;
    uam_state_save_fn    save;
    uam_state_restore_fn restore;
    void                *ctx;
} uam_state_part;

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
    /* Snapshots (snapshot.h). */
    uam_state_part *state_parts;         /* registered with uam_machine_add_state */
    uint32_t        n_state_parts, cap_state_parts;
    uint64_t        snap_base;           /* snapshot the bus's dirty pages are relative to */
};

/*
 * ABI handshake. A board is compiled against these headers but may be linked
 * against a libuam built from other ones; if the struct layouts differ, the
 * two sides disagree on field offsets and memory is silently corrupted. So
 * the entry points a board calls first (uam_board_register, uam_machine_init,
 * uam_machine_add_device) are macros that pass the layout the caller was
 * compiled with. On a mismatch the library logs each differing item and
 * aborts: a mixed build is never usable, and an error status could be ignored.
 * A board built against headers older than this handshake fails to link.
 */
typedef struct uam_abi_layout {
    uint32_t version;   /* UAM_ABI_VERSION; always the first member */
    uint32_t self;      /* sizeof(uam_abi_layout) */
    uint32_t machine, exec, cpu, bus, sched, timer, device, chardev, netdev,
             board, board_config;
} uam_abi_layout;

#define UAM_ABI_LAYOUT (&(const uam_abi_layout){                                  \
        UAM_ABI_VERSION, sizeof(uam_abi_layout), sizeof(uam_machine),           \
        sizeof(uam_exec), sizeof(uam_cpu), sizeof(uam_bus), sizeof(uam_sched),  \
        sizeof(uam_timer), sizeof(uam_device), sizeof(uam_chardev),             \
        sizeof(uam_netdev), sizeof(uam_board), sizeof(uam_board_config) })

/* True if `caller` matches the library; otherwise logs every difference,
 * attributed to `entry`, and returns false. */
bool uam_abi_check(const uam_abi_layout *caller, const char *entry);

uam_status uam_machine_init_abi(uam_machine *m, const uam_isa *isa, const uam_abi_layout *abi);
#define uam_machine_init(m, isa) uam_machine_init_abi((m), (isa), UAM_ABI_LAYOUT)
void       uam_machine_fini(uam_machine *m);
/* Take ownership of a device (reset/destroy with the machine). */
void       uam_machine_add_device_abi(uam_machine *m, uam_device *dev, const uam_abi_layout *abi);
#define uam_machine_add_device(m, dev) uam_machine_add_device_abi((m), (dev), UAM_ABI_LAYOUT)
/* Reset the hart and every device; start executing at `entry` (which becomes
 * the machine's reset_pc). RAM and the translation cache are preserved. */
void       uam_machine_reset(uam_machine *m, uint64_t entry);
/*
 * Register state that is neither a device nor the hart - board-private data,
 * state an analysis keeps alongside the guest - so snapshots save and restore
 * it (snapshot.h). `name` identifies it in a snapshot and must be unique.
 */
uam_status uam_machine_add_state(uam_machine *m, const char *name, uam_state_save_fn save,
                                 uam_state_restore_fn restore, void *ctx);
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
uam_status       uam_board_register_abi(const uam_board *board, const uam_abi_layout *abi);
#define uam_board_register(board) uam_board_register_abi((board), UAM_ABI_LAYOUT)
const uam_board *uam_board_find(const char *name);
void             uam_board_list(FILE *out);

#endif /* UAM_MACHINE_H */
