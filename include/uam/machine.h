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

/*
 * A named way to hand the guest a block of bytes (uam_machine_add_input): a UART's
 * receive FIFO, a DMA buffer, a mailbox. Boards publish the inputs their devices
 * offer; tools (a fuzz harness, a replay tool) pick one by name, so they never need to
 * know a concrete device. Called between runs; returns non-OK if the bytes cannot be
 * delivered (e.g. too long).
 */
typedef uam_status (*uam_input_fn)(void *ctx, uam_machine *m, const uint8_t *data, size_t len);
typedef struct uam_machine_input {
    char        *name;
    uam_input_fn fn;
    void        *ctx;
} uam_machine_input;

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
    /* Named inputs (uam_machine_add_input). */
    uam_machine_input *inputs;
    uint32_t           n_inputs, cap_inputs;
    /* Further processors (uam_machine_add_cpus): processor i + 1 is
     * aps[i] / ap_execs[i]; processor 0 is `cpu` / `exec`. */
    uam_cpu          **aps;
    uam_exec         **ap_execs;
    uam_exec         **all_execs;          /* processor 0's first (exec.smp) */
    uint32_t           n_aps;
    uam_cov_state     *ap_cov;             /* coverage state of processor i + 1 (uam_machine_set_coverage) */
};

/*
 * ABI handshake. A board is compiled against these headers but may be linked
 * against a libuam built from other ones; if the struct layouts differ, the
 * two sides disagree on field offsets and memory is silently corrupted. So
 * the entry points a board calls first (uam_board_register, uam_machine_init,
 * uam_machine_add_device) are macros that pass the layout the caller was
 * compiled with. On a mismatch the library logs each differing item and
 * exits with UAM_ABI_EXIT_STATUS (the runner's status for setup errors): a
 * mixed build is never usable, and an error status could be ignored. The check
 * runs before the library touches any of the caller's structs, so nothing is
 * corrupted and no core dump is needed. Programs that want to handle a mismatch
 * themselves can call uam_abi_check(UAM_ABI_LAYOUT, ...) first.
 * A board built against headers older than this handshake fails to link.
 */
#define UAM_ABI_EXIT_STATUS 2
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
 * Multiprocessor machines. Add `count` processors of the machine's ISA after
 * uam_machine_init (and before the first reset): processor i has hartid i,
 * its own registers, translation cache and JIT, and shares the bus and the
 * clock. uam_exec_run(&m->exec, n) then runs all of them, interleaved
 * deterministically in slices of `quantum` instructions (0 = default 4096;
 * uam_machine_set_quantum), n counting every processor's instructions. The
 * ISA decides what an extra processor does at reset (x86: waits for a
 * startup IPI). Snapshots hold every processor and where the interleaving
 * stands, so a restored machine interleaves exactly as the original would.
 */
uam_status uam_machine_add_cpus(uam_machine *m, unsigned count);
unsigned   uam_machine_ncpus(const uam_machine *m);
uam_cpu   *uam_machine_cpu(uam_machine *m, unsigned i);    /* NULL beyond the last */
uam_exec  *uam_machine_exec(uam_machine *m, unsigned i);
void       uam_machine_set_quantum(uam_machine *m, uint32_t insns);
/*
 * Observers on every processor: each processor's executor gets the same PC
 * hook, exception or interrupt observer, or IR pass, and callbacks receive the
 * processor that ran into them as `cpu` (cpu->hartid). With one processor these
 * are the uam_exec_* calls on m->exec. Add them after uam_machine_add_cpus.
 */
uam_status uam_machine_add_pc_hook(uam_machine *m, uint64_t pc, uam_pc_hook_fn fn, void *ctx);
bool       uam_machine_remove_pc_hook(uam_machine *m, uint64_t pc, uam_pc_hook_fn fn, void *ctx);
void       uam_machine_set_exc_hook(uam_machine *m, uam_exc_hook_fn fn, void *ctx);
void       uam_machine_set_irq_hook(uam_machine *m, uam_irq_hook_fn fn, void *ctx);
uam_status uam_machine_add_pass(uam_machine *m, const uam_pass *p);
bool       uam_machine_remove_pass(uam_machine *m, const uam_pass *p);
/*
 * Coverage on every processor, into `state`'s map (uam_exec_set_coverage):
 * processor 0 counts in `state`, every other processor in a copy the machine
 * keeps (same map and history shape, its own history), so an edge never joins
 * two processors' paths and the map does not depend on where slices end
 * beyond what the guest itself does. uam_machine_cov_state(m, i) is processor
 * i's state (NULL with coverage off): reset each one's history
 * (uam_cov_reset_prev) where `state`'s would be reset.
 */
uam_status uam_machine_set_coverage(uam_machine *m, const uam_cov_site *sites, size_t n,
                                    uam_cov_state *state, char *err, size_t errlen);
uam_status uam_machine_set_block_coverage(uam_machine *m, uam_cov_state *state);
uam_cov_state *uam_machine_cov_state(uam_machine *m, unsigned i);
/* Every processor's executor statistics, added up. */
void       uam_machine_get_stats(const uam_machine *m, uam_exec_stats *out);
/* Instructions every processor has retired (their icounts added up). */
uint64_t   uam_machine_icount(const uam_machine *m);
/* The processor running now (its hartid; 0 between runs and with one). For
 * devices that answer each processor differently. Takes the machine. */
unsigned   uam_machine_current_cpu(void *machine);
/*
 * Register state that is neither a device nor the hart - board-private data,
 * state an analysis keeps alongside the guest - so snapshots save and restore
 * it (snapshot.h). `name` identifies it in a snapshot and must be unique.
 */
uam_status uam_machine_add_state(uam_machine *m, const char *name, uam_state_save_fn save,
                                 uam_state_restore_fn restore, void *ctx);
/* Publish a named input (see uam_input_fn); `name` must be unique. */
uam_status uam_machine_add_input(uam_machine *m, const char *name, uam_input_fn fn, void *ctx);
/* The input called `name`, or NULL. */
const uam_machine_input *uam_machine_find_input(const uam_machine *m, const char *name);
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
