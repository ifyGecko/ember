# Snapshots on your board

Ember can capture a running machine and return to it, bit-identically. That works on an
out-of-tree board with no extra code, **provided every device takes part in snapshots**.
This page covers what the board owes them and how to use them during bring-up. Snapshots are
also what ember's fuzz harness is built on, so a board that passes the split-run check below
is ready for the **ember-fuzz** skill later, without changes to its devices.

## 1. A snapshot-ready board

Taking a snapshot (`--save-snapshot`, `--snapshot-at`) or restoring one (`--load-snapshot`)
fails, naming the culprit:

| error | fix |
|---|---|
| take: `device X (type) has no save/restore` | implement `save`/`restore` in its ops (`device-anatomy.md`, "Snapshots"), or set `.flags = UAM_DEVICE_STATELESS` if it really holds no guest-visible state |
| take: `an armed timer (deadline N) is not saved by its owner` | the device that arms it must `uam_state_put_timer` / `uam_state_get_timer` it |
| restore: `X left N bytes of its saved state unread (save and restore disagree)`, `state ends early`, `saved array is N bytes, expected M`, `... is N in the snapshot but M in this machine` | the device's `save` and `restore` read different fields, or its layout changed since the snapshot was taken: fix the pair, then take a new snapshot |
| restore: `snapshot is of board ...`, `memory region N differs`, `snapshot holds state for 'X', which this machine ...` | load with the same board build, `-o` options, `--image`s, `--ram`, `--mmio-rule`s and `--nic` ports the snapshot was taken with |

What the snapshot covers by itself: RAM/ROM/ROMD contents (and each ROMD region's mode),
every processor's state (registers, CP0/CSRs/SPRs/MSRs, TLBs), virtual time, every device's `save` stream, extra state
registered with `uam_machine_add_state`, and the MMIO-rule input position. What it does
**not** cover: configuration (PC hooks, watchpoints, trace flags, `--jit`), host backends,
and anything a board or device keeps outside those. So:

- **Board-private state the guest can observe** (a latched strap, a counter in `board_data`)
  is registered with `uam_machine_add_state(m, "board", save, restore, ctx)`. Configuration
  fixed at build time (`os_path`, sizes) is not state.
- **Direct writes to a backing store** (`region->host`, `uam_bus_romd_backing`) are reported
  with `uam_bus_romd_dirty` / `uam_bus_mark_dirty`. Otherwise an incremental restore (it only
  copies back dirty 4 KiB pages) silently keeps the guest's change.
- **HLE hooks are configuration**: a board installs the same hooks on every build, so they
  hold after a restore. A hook that keeps its own counters registers them as extra state.

**Verify it** the way ember and `skills/tests/run-tests.sh` do: a run split by
`--save-snapshot` / `--load-snapshot` must equal the uninterrupted run (console bytes, final
pc and instruction count), with the interpreter and `--jit` on either side.

```bash
IMAGE=fw.bin ./run.sh -n 300000000 > build/full.txt 2> build/full.log < /dev/null
IMAGE=fw.bin ./run.sh -n 120000000 --save-snapshot build/s.snap > build/a.txt 2> build/a.log < /dev/null
IMAGE=fw.bin ./run.sh -n 180000000 --jit --load-snapshot build/s.snap > build/b.txt 2> build/b.log < /dev/null
cat build/a.txt build/b.txt | cmp - build/full.txt && tail -n 3 build/full.log build/b.log
```

(After `--load-snapshot`, `-n` counts from the snapshot, so the two budgets add up to the
first; the exit line's instruction count is cumulative. If the second run says it cannot open
the snapshot, `build/a.log` says why the save failed.) A mismatch is usually a device (or
board state) that saves too little, or a direct backing-store write that was not reported;
find it by bisecting the split point. A mismatch between interpreter and JIT on
the same side is an ember bug: report it.

## 2. Snapshots during bring-up

Once a boot is long, stop re-running it every iteration:

```bash
./run.sh --snapshot-at 0x8023f00c --save-snapshot build/booted.snap -n 2000000000 --jit ...
./run.sh --load-snapshot build/booted.snap -n 50000000 ...          # every later run
./run.sh --load-snapshot build/booted.snap --console 'string:help\r' ...   # script a shell
```

- `--snapshot-at PC` saves on the first arrival and keeps running; add `--break PC` to stop
  there. Without it the snapshot is taken when the run stops (budget, `--break`, Ctrl-C).
- A snapshot belongs to one board build. **Changing a device that is part of it invalidates
  it** when its saved layout changes, and always risks stale state: re-take the snapshot
  whenever the iteration touched anything that ran before the snapshot point. Journal which
  snapshot (pc, icount, emulation commit) an iteration's runs started from.
- Fixed console input (`string:`, `file:...,in=`) is held until the run starts, after the
  power-on reset and the `--load-snapshot`. A snapshot at the shell prompt plus `string:`
  input is the deterministic way to script a shell from a booted state; from a cold boot,
  `,after=PROMPT` or `script:FILE` wait for the prompt instead (patterns match only output
  printed in the current run, so not a prompt from before the snapshot).
