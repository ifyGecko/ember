# Source-organization discipline (authoritative)

This is the non-negotiable layout. The point: **your emulation is a first-class, versioned
codebase; ember is a library it links against.** A fresh checkout of the pinned ember commit,
plus what you own (`ember.commit`, `patches/` (normally empty), `emulation/`, `Makefile`),
must rebuild the exact same runner.

Ember was designed for this. Its library contains no boards; a program registers its own
board and reuses the stock command line (`uam_run_main`). So an ember board needs **no file
inside the ember tree**: the project only includes ember's public headers and links its
library.

## The emulation project directory

It can be anywhere: `$EMU_ROOT/<board>` by default, or the session directory that already
holds `RECON.md` (`new-emulation.sh <board> --dir .`).

```
<project>/                       # a git repo you own and commit to
  RECON.md                       # recon fact sheet (input)
  iter-00.md, iter-01.md, ...    # bring-up journal
  emulation/                     # YOUR C sources - the only place you write emulator code
    <board>_board.c / .h       # the board: hart, memory, image placement, wiring. Wires only.
    <board>_hle.c / .h         # (optional) PC hooks that replace firmware functions
    main.c                       # registers the board, calls uam_run_main()
    <soc>_uart.c / .h            # one hardware component per file
    <soc>_intc.c / .h
    <soc>_timer.c / .h
  Makefile                       # builds build/<board>-run from emulation/*.c + libuam.a
  patches/                       # edits to ember ITSELF, as git diffs - normally empty
  ember.commit                   # exact ember commit to build against
  ember.upstream | ember.src     # clone source for <project>/ember-src | an existing checkout
  suzzer.dir                     # (optional, ember-fuzz) suzzer checkout libuam is built with
  scripts/                       # project-local copies of the build helpers
  tools/                         # data generators (flash images, NVRAM, calibration, ...)
  run.sh                         # the runner invocation under construction
  build/        (gitignored)     # build/<board>-run, objects, console captures, logs
  .ember-build/ (gitignored)     # ember's libraries, built out of tree + paths.mk
  ember-src/    (gitignored)     # the project's own ember clone (when no ember.src)
```

## The five rules

### 1. Your code never lives in the ember tree
Every line you write is under `emulation/` (board, devices, hooks) or `tools/` (data
generators). The Makefile compiles every `emulation/*.c`, so a new device is a new file and
nothing else. Ember's libraries are built **out of tree** (`make -C <ember> BUILD=<project>/
.ember-build lib`), so even the build output stays in the project.

### 2. Ember is checked out at a pinned commit
`build-ember.sh` puts the checkout at `ember.commit` (`checkout --force`, `reset --hard`),
applies `patches/`, and builds the libraries. It never commits into ember and never touches
untracked files.

- By default the project builds in its own clone (`<project>/ember-src`, cloned from
  `ember.upstream`: `$EMBER_UPSTREAM`, else `~/ember`). Nobody else's work is ever at risk.
- With `--ember-src DIR` it builds in an existing checkout. Because the reset would discard
  or move someone's work, `build-ember.sh` **refuses** when that checkout has tracked edits
  the project does not own, or sits at a different commit than the pin. Use `--force` only
  when discarding/moving is intended.
- Choosing the commit: ember's current HEAD is the usual choice; record *why* in the project
  README. Moving the pin later means re-running `build-ember.sh` and the skills' test
  (`skills/tests/run-tests.sh <checkout>`, in the ember repository), then fixing API drift
  (see the checklist in `board-skeleton.md`). The
  library and the runner are always rebuilt together: a runner linked against a libuam
  from other headers stops with `abi:` errors (exit 2) instead of misbehaving. Snapshot
  files are not guaranteed to survive a pin move (ember's format or a stock device's state
  may change) and do not survive a change to a device's saved layout; re-take them.

### 3. One hardware component = one file, and ember's layering holds
SoC UART, timer, interrupt controller, reset/clock block, flash controller, Ethernet MAC,
switch, radio, FPGA: each is its own `.c` (+ `.h` for its create function and lines). The
board file is the only file that knows several components exist, and it only wires them.

Ember's dependency rules (its `docs/architecture.md`, section 3) apply to your files too, and
`check-layering.sh` (run by `rebuild.sh`) enforces them:

| file | may include |
|---|---|
| device files | `uam/common.h access.h device.h signal.h sched.h chardev.h netdev.h blockdev.h state.h bus.h`, stock device headers `uam/dev/*.h`, other device headers of the project |
| `*_board.[ch]`, `*_hle.[ch]`, `main.c` | any public ember header except `uam/host.h` |

A device never sees the CPU, the ISA, the executor, the machine or the host. What it needs
from them arrives as a **parameter from the board**: a `uam_sched *` for time, a `uam_bus *`
for DMA, a `uam_chardev *` / `uam_netdev *` for host I/O, a callback (`uam_finish_fn`) to end
the run, a `uam_signal` it raises for interrupts and chip resets, a number for its clock.
Its snapshot state goes through its own `save`/`restore` ops (`uam/state.h`). That is what
keeps devices reusable across boards and testable on their own. A device never knows about
coverage or the fuzz harness either: the most ember-fuzz adds to one is a plain "bytes
arrived" feed function, which the board publishes.

Reuse ember's stock models (`include/uam/dev/`) instead of re-implementing them:
`uam_uart16550` (byte-spaced 16550; behind `reg_stride` for word-spaced ones), `uam_unimp`
(catch-all windows), `uam_unknown_mmio`, `uam_mmio_stub` (per-register rules; use
`--mmio-rule`), `uam_cfi_flash`, `uam_gptimer` (a generic down-counter), `uam_ipi`
(doorbells between processors), `uam_clint` / `uam_plic` (SiFive layouts), `uam_gic`,
`uam_avr32_intc`, `uam_pic32_intc`, the 8051's `uam_mcs51_timer` / `_serial` / `_port`, the
`uam_z80_ctc`, the PC devices (`pc-platform.md`), `uam_virtio_net`, `uam_test_finisher`
(for test firmware). All of them take part in snapshots. They are generic: a part-specific
block (a SoC's own UART or timer) is a device of the project, never an edit to ember.

### 4. Edits to ember itself are patches, and rare
Boards, devices and hooks are written against ember's public headers and need no patch.
A patch is only for a capability ember lacks (an instruction the lifter rejects, a CPU-model
field, a bus or runner feature). Ember is a project you can commit to, so the better fix is
to land the change in ember and re-pin `ember.commit`; a patch is the stopgap until then.

Make such edits **in the checkout**, review them with `git -C <ember> diff`, capture them
with `regen-patches.sh` (`--add PATH` for new files), and say in the journal why the public
API was not enough. `rebuild.sh` refuses to build while the checkout holds edits that are not
captured, so the project never silently depends on an uncommitted tweak.

### 5. Data the firmware reads is generated, not hand-edited
Flash partitions, NVRAM, calibration blobs, MAC addresses: produce them with a script under
`tools/` that writes into `build/`, and pass them to the board as named images
(`--image flash=build/flash.bin`). Label synthetic content as synthetic in the script and the
journal.

## The scripts (copied into `<project>/scripts/` by `new-emulation.sh`)

| script | does |
|---|---|
| `build-ember.sh` | full build: clone if needed → guard → pinned checkout → patches → ember libraries (out of tree; `--suzzer DIR` adds suzzer support for ember-fuzz) → `build/<board>-run` → confirm the board is registered |
| `rebuild.sh` | fast build: checkout still pinned, no uncaptured edits, patches applied → library make (no-op) → layering check → project make |
| `regen-patches.sh` | capture edits to ember files as patches |
| `check-layering.sh` | enforce rule 3 on `emulation/` |
| `log_summary.py` | condense the runner's stderr into the next blocker |
| `gdb_trace_calls.py` | gdb helper: log calls and return values of guest functions |

Project and checkout resolution: `--proj` / `$PROJ_DIR` / the script's own project /
`$PWD`; then `--ember-src` / `$EMBER_SRC` / `ember.src` / `<project>/ember-src`.
