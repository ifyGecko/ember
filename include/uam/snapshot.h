/*
 * uam/snapshot.h - Machine snapshots: capture a running machine, return to it.
 *
 * A snapshot holds everything that decides what the machine does next:
 *
 *   - every RAM, ROM and ROMD backing store (and each ROMD region's mode),
 *   - the hart (pc, registers, instruction count, WAIT, privilege,
 *     reservation) and the ISA module's private state (isa->save),
 *   - virtual time and every armed timer,
 *   - each device's state (device ops save/restore, state.h),
 *   - extra state registered with uam_machine_add_state (board data, ...),
 *   - the executor's pending reset request and its per-arrival hook latch.
 *
 * It does not hold host-side state (console and network backends, pacing,
 * gdb), configuration (PC hooks, watchpoints, trace flags, JIT on/off) or
 * statistics: those belong to the program driving the machine. Nor does it
 * hold a pending stop request: restore clears it, so a machine whose
 * firmware stopped the run (or that was interrupted) runs again from the
 * snapshot.
 *
 * Typical uses:
 *
 *   Boot once, analyse many times, in one process:
 *       uam_snapshot *booted;
 *       uam_exec_run(&m.exec, boot_insns);              // the slow part, once
 *       uam_snapshot_take(&m, &booted);
 *       for (each test) {
 *           uam_snapshot_restore(&m, booted);           // cheap: dirty pages only
 *           ... inject input, uam_exec_run(&m.exec, n), check ...
 *       }
 *
 *   Boot once, analyse in later processes:
 *       uam_snapshot_save(booted, "booted.snap");       // after booting
 *       ...
 *       board->build(&m, &cfg);                         // same board, same config
 *       uam_snapshot_load("booted.snap", &s);
 *       uam_snapshot_restore(&m, s);
 *
 * Restore is incremental. Taking or restoring a snapshot makes it the
 * machine's baseline and turns on the bus's dirty-page tracking (bus.h);
 * restoring the baseline again copies back only the pages written since,
 * while restoring any other snapshot compares every page. Either way,
 * translated (and JIT-compiled) code survives for pages whose contents end up
 * unchanged.
 *
 * A snapshot is only restored into a machine built by the same board with the
 * same memory map and devices; restore checks the board and ISA names, the
 * backed regions (name, base, size, kind), the register file size, and that
 * the device and extra-state records match one to one, before changing
 * anything. Devices are matched by name (the first duplicate is "name#1",
 * ...), in the order they were added to the machine.
 *
 * Restore brings memory back first, then calls the restore callbacks, so a
 * device or extra-state callback sees the restored memory (e.g. to rewrite a
 * flash image file from the restored flash contents).
 *
 * Take and restore are called between uam_exec_run calls or from a PC hook
 * (a hook that restores a snapshot returns UAM_HOOK_CONTINUE; the run goes on
 * from the restored state). Every device must implement save/restore or be
 * UAM_DEVICE_STATELESS (device.h); otherwise take fails and names it.
 *
 * Dependencies: machine.h, state.h.
 */
#ifndef UAM_SNAPSHOT_H
#define UAM_SNAPSHOT_H

#include "uam/machine.h"
#include "uam/state.h"

typedef struct uam_snapshot uam_snapshot;

typedef struct uam_snapshot_info {
    const char *board;        /* board name ("" if the board set none) */
    const char *isa;          /* ISA name */
    uint64_t    pc, icount;   /* hart at the snapshot */
    uint64_t    now;          /* virtual time */
    uint64_t    mem_bytes;    /* backed memory covered (RAM, ROM, ROMD) */
    uint64_t    mem_pages;    /* pages stored (all-zero pages are not) */
    size_t      state_bytes;  /* size of the hart, device and extra state */
} uam_snapshot_info;

/* Capture the machine. On failure (a device without save/restore, an armed
 * timer nobody saves, no memory) logs why and leaves the machine untouched. */
uam_status uam_snapshot_take(uam_machine *m, uam_snapshot **out);

/*
 * Return the machine to the snapshot. Incompatible snapshots are rejected
 * (UAM_ERR_INVALID) before anything changes. If a component then rejects its
 * own state (a configuration mismatch detected by its restore), the error is
 * logged, UAM_ERR_INVALID is returned and the machine is left inconsistent:
 * restore another snapshot or discard the machine.
 */
uam_status uam_snapshot_restore(uam_machine *m, const uam_snapshot *s);

void       uam_snapshot_free(uam_snapshot *s);
void       uam_snapshot_get_info(const uam_snapshot *s, uam_snapshot_info *info);

/* Files. The format is host-independent and carries a checksum; save writes
 * a temporary file next to `path` and renames it into place. */
uam_status uam_snapshot_save(const uam_snapshot *s, const char *path);
uam_status uam_snapshot_load(const char *path, uam_snapshot **out);

/* take + save, and load + restore, for the common one-shot case. */
uam_status uam_machine_save_snapshot(uam_machine *m, const char *path);
uam_status uam_machine_load_snapshot(uam_machine *m, const char *path);

#endif /* UAM_SNAPSHOT_H */
