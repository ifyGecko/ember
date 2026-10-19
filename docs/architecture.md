# UAM Firmware Emulator — Architecture

Status: phases 1 and 2 implemented, plus a second ISA (MIPS32 with a TLB), the snapshot part of
phase 3 and the JIT from phase 4. IR, interpreter and optimizer, an x86-64 JIT with block chaining,
bus (priorities, device byte order, ROM devices, aliases, watchpoints, dirty-page tracking),
RV32IMAC (`rv32-ref` board) and MIPS32 big/little-endian (`mips-ref` / `mipsel-ref` boards), host
I/O backends (console, tap/tun/UDP networking), a gdb stub, a reusable runner, machine snapshots,
IR passes with edge coverage and an exception observer, and a library build for out-of-tree
boards. See section 7 for what remains.
Language: C11 (no C++). Goal: analysis and fuzzing of raw firmware images.

## 1. Core idea

The **Universal Abstract Machine (UAM)** only knows *computation*. Every guest ISA is lifted
into one small, ISA-neutral IR (see [`uam-isa.md`](uam-isa.md)). Everything else about the
emulated system leaves the machine through a few explicit **interfaces**. Each aspect lives in a
**module** behind one of them, and modules never reach around an interface to touch each other.

```
 firmware image ─► Loader ─► guest memory (via Bus)

 guest code ─► ISA module ──lift──► UAM-IR blocks ──► optimizer ──► Executor (interpreter or JIT)
                  │                       ▲                    │
                  │ arch hooks             │ IR passes          │ the only ways out of the machine:
                  ▼                       │ (analysis)         ▼
        take_exception / irq_take /       │        ┌──────────────────────────────┐
        sysreg / translate                │        │ Bus     memory & MMIO        │
                                          │        │ Sched   virtual time, events │
                                          │        │ Signal  interrupt lines      │
                                          │        └──────────────┬───────────────┘
                                          │                       ▼
                                          │          Devices (one module each),
                                          │          composed by a Board
                                          └── coverage, cmp-log, taint, tracing
```

## 2. Interfaces

Each interface is one public header containing a C vtable (a struct of function pointers)
plus opaque handles. Implementations are selected at run time by the board.

| Interface | Responsibility | Implemented by |
|---|---|---|
| `ir.h` | IR types, opcodes, block builder API, verifier, printer | the core (data only) |
| `isa.h` | `lift`, register descriptor table, `take_exception`, `irq_take`/`irq_wakeup`/`irq_input`, `sysreg_read`/`sysreg_write`, `default_endian`, optional `translate` (v->p, with fault sub-causes), optional `gdb` register view, `save`/`restore` of ISA-private state. An ISA must request an exit (`uam_cpu_request_exit`) on any change that can make `irq_take` newly succeed | `isa/riscv`, `isa/mips32` |
| `bus.h` | Region map (RAM / ROM / MMIO / ROMD / alias) with priorities (real devices, then per-register stubs from `UAM_PRIO_STUB`, then catch-all windows at `UAM_PRIO_BACKGROUND`), software TLB, device byte order and access widths, watchpoints, fault reporting, DMA master access (`uam_bus_dma_read`/`_write`), dirty-page tracking for snapshots | the core |
| `device.h` | `read`, `write`, optional `write_masked`, `reset`, `destroy`, `save`/`restore` (or `UAM_DEVICE_STATELESS`); declared `endian` and implemented access sizes (declarative register tables are planned) | one file per device |
| `state.h` | State streams: how a component writes and reads back its state (fixed-width little-endian fields, byte arrays, timers with their tie order, signal levels; sticky errors) | the core |
| `sched.h` | Instruction-count clock (saturating at a 2^62-tick horizon), event queue with a per-call firing budget, a kick when the earliest deadline moves (the machine turns it into an exit request), cancel-all, list consistency checks | the core |
| `signal.h` | Interrupt, GPIO and reset lines: raise, lower, connect; edge and level | the core |
| `chardev.h` | Byte streams between a device and the host (UART consoles); input can be held back until the run starts (`uam_chardev_hold`) | core plumbing; backends in `src/host` |
| `netdev.h` | Ethernet frames between a device and the host (NICs, switches, radio "air") | core plumbing; backends in `src/host` |
| `machine.h` | Composition: machine lifecycle, resets (device reset line, board reset hook), board registry, board config (RAM, unknown-MMIO policy, console, options, named images, named network ports), extra snapshot state (`uam_machine_add_state`), named inputs a board publishes (`uam_machine_add_input`: a UART FIFO, a DMA buffer), ABI handshake | `src/machine`, `boards/*` |
| `snapshot.h` | Machine snapshots: take / restore in memory (incremental against the last baseline), save / load files | `src/machine` |
| `cpu.h` | ISA-neutral hart state (including the coverage state `UAM_OP_COV` counts into) and the core services ISA modules may call (`uam_cpu_xlate_changed`, ...) | the core |
| `loader.h` | ELF / raw images into guest memory | `src/loader` |
| `exec.h` | Run a hart: translation cache, block execution (interpreter, or JIT with optional block chaining), result handling, reset requests, PC hooks, IR passes, edge coverage (`uam_exec_set_coverage`), the exception observer, the host bridge (polling, idle), instruction / virtual-time / stall (livelock) bounds, statistics | `exec/interp`, `exec/jit_x86` |
| `host.h` | Host side: chardev/netdev backends, host loop, virtual and real-time pacing | `src/host` |
| `gdbstub.h` | gdb remote protocol server on top of hooks, watchpoints and `isa->gdb` | `src/debug` |
| `runner.h` | The stock command line as a library entry point (`uam_run_main`), and runner extensions: options plus setup/attach/finish callbacks that contribute coverage sites, iteration boundaries or a fuzz frontend without the runner knowing what they are for | `src/runner`; stock extensions in `fuzz/stock_exts.c` |
| `boards.h` | The stock boards and their registration | `boards/stock.c` |
| `pass.h` | IR → IR transformations run on every new block, after the optimizer and before native compilation (instrumentation); block editing helpers | `analysis/*`; registered with `uam_exec_add_pass` |
| `fuzz.h` | The persistent-mode fuzz harness (boot to a begin pc, snapshot, one restore per input, verdicts) and its frontend interface; the replay frontend | `fuzz/` (see [`fuzzing.md`](fuzzing.md)) |
| `cov.h` | The edge-coverage pass: `UAM_OP_COV` at a table of guest pcs (or at every block start), knowing nothing about where the table came from | `analysis/cov.c` |

## 3. Dependency rules

Arrows point to what a module may include. Anything not listed is forbidden.

```
ir            ← nothing (pure data + algorithms over it)
isa/*         → ir, isa.h, state.h
exec/*        → ir, isa.h, bus.h, sched.h, pass.h, cov.h (it wires passes in)
analysis/*    → ir, pass.h, cov.h        (never an ISA or a device)
core (bus, sched, signal, chardev, netdev, state) → their own headers only
                                         (state.h: also sched.h and signal.h, whose objects it saves)
devices/*     → device.h, signal.h, sched.h, chardev.h, netdev.h, state.h,
                bus.h (DMA, and the backing store of a ROMD region it owns)
                                         (never ir, never an ISA, never the host)
boards/*      → everything public except host.h; the only place concrete modules meet
host/, debug/ → public interfaces (chardev, netdev, exec, machine); never a concrete device
runner, fuzz/, tools/ → boards + public headers
fuzz/suzzer/  → as fuzz/, plus suzzer's format header (src/lcv_formats.h of the suzzer
                checkout the build is given; docs/suzzer.md); the only place that knows
                suzzer exists, built only when a checkout is configured
```

The consequences are deliberate:

- A device never knows which CPU is driving it, nor which host backend is behind its chardev.
- An ISA never knows which devices exist.
- Byte order and access width are the bus's job: a device states its own and the bus adapts.
- A device that resets the chip raises a reset *line*; the board wires it to the machine.
- Analysis passes work for every ISA for free.
- Devices and ISAs never learn about coverage or fuzzing; core and exec code never mention a
  fuzzer's formats.
- The library contains no boards: programs register the boards they link.

**Binary interface.** Out-of-tree boards embed public structs (`uam_machine`, `uam_device`, ...)
and are compiled separately from `libuam`, so header/library skew would silently shift field
offsets. `UAM_ABI_VERSION` (`common.h`) is bumped on any change to a public struct layout, enum
or function signature, and `uam_board_register`, `uam_machine_init` and `uam_machine_add_device`
are macros that pass the caller's version and struct sizes (`uam_abi_layout`); when they do not
match its own, the library lists the differences and exits with status 2 (`UAM_ABI_EXIT_STATUS`),
before touching any of the caller's structs.

## 4. Source layout

Implemented:

```
include/uam/      common.h access.h ir.h isa.h cpu.h bus.h device.h signal.h sched.h exec.h
                  loader.h machine.h chardev.h netdev.h host.h gdbstub.h runner.h boards.h
                  state.h snapshot.h pass.h cov.h
include/uam/dev/  clint.h plic.h uart16550.h sifive_test.h unknown_mmio.h unimp.h mmio_stub.h
                  virtio_net.h
include/uam/isa/  riscv.h mips32.h
src/ir/           builder.c verify.c print.c opt.c pass.c ir_eval.h
src/core/         bus.c sched.c common.c chardev.c netdev.c state.c
src/exec/         interp.c tcache.c run.c cpu.c jit_x86.c coverage.c jit.h execops.h
analysis/         cov.c (the edge-coverage pass)
src/loader/       loader.c
src/machine/      machine.c snapshot.c
src/host/         chardev_backends.c netdev_backends.c hostloop.c
src/debug/        gdbstub.c
src/runner/       runner.c
isa/riscv/        rv_lift.c rv_system.c rv.h (private)
isa/mips32/       mips_lift.c mips_system.c mips.h (private)
devices/          clint.c plic.c uart16550.c sifive_test.c unknown_mmio.c unimp.c mmio_stub.c
                  virtio_net.c
boards/           rv32_ref.c mips_ref.c stock.c mmio_rules.c (uam_board_add_mmio_rules, any board)
fuzz/             harness.c frontend_replay.c (the harness and the replay frontend),
                  stock_exts.c (the runner extensions uam_run_main uses)
fuzz/suzzer/      scheme.c (the bucket formula) import.c (LCV3/LCVB1 readers, site check)
                  cli.c (the --cov-sites/--cov-boundary runner extension) suzzer.h
tools/            uam-run.c fetch-suzzer.sh (`make suzzer`: clone suzzer into ./suzzer, ignored
                  by git)
examples/         external-board/ (an out-of-tree board built against `make lib`)
tests/unit/       test_ir.c test_core.c test_host.c test_snapshot.c test_jit.c test_mmio.c test_cov.c
                  test_fuzz.c test_suzzer.c
tests/cov/        cov-check.sh  (block-coverage maps across backends)
tests/fuzz/       fuzz-check.sh fuzztarget.h  (the harness on planted-bug firmware, all ISAs)
tests/suzzer/     suzzer-check.sh mk_lists.py  (suzzer's formats through the runner)
tests/snapshot/   snapshot-check.sh  (split runs through --save-snapshot / --load-snapshot)
tests/firmware/       start.S selftest.c nettest.c link.ld  (RISC-V)
tests/net/            net-check.sh  (nettest.elf against real tap and tun interfaces)
tests/firmware-mips/  start.S selftest.c link.ld  (MIPS32)
```

Planned: more passes in `analysis/` (cmp-log, taint), the suzzer importers and frontends
(`fuzz/suzzer/`), an Intel HEX loader, more boards and devices.

## 5. Firmware specifics

- **Loader**: raw binary at a base address and ELF (PT_LOAD by physical address); Intel HEX is planned.
- **Unknown MMIO** has three layers, lowest priority first:
  - The global fallback (`devices/unknown_mmio.c`) for addresses no region covers. Unmapped
    accesses fault by default. With `--unknown-mmio zero|echo` this device answers them,
    reading zero or echoing the last value written, and logs the first access to each address.
  - Per-block catch-alls (`devices/unimp.c`), mapped at `UAM_PRIO_BACKGROUND` under real
    devices. They are read-as-zero, write-ignored, and report offsets relative to the block.
  - Per-register rules (`devices/mmio_stub.c`, `--mmio-rule`, `uam_board_add_mmio_rules`).
    Each rule is a range of `width`-byte registers with a policy: `zero`, `echo`,
    `fixed:VALUE`, `fault` (bus error), or `input`. An `input` register takes its next
    `width` bytes from an input stream (`--mmio-input`, `uam_mmio_input`), which is how fuzz
    input reaches firmware through MMIO reads. Every read can then be adjusted by `set` /
    `clear` masks.

    A rule is its own small region at `UAM_PRIO_STUB + i`: it beats catch-alls and holes,
    real devices beat it, and later rules beat earlier ones. It is mapped in the CPU's byte
    order, so a fixed value reads as written. Stubs log the first access to each register
    (`-v`), take part in snapshots, and so does the input position.
- **Flash**: memory-mapped NOR with a command interface is a ROMD region: fetchable in ROM
  mode, writes go to the controller device, which updates the backing store and marks it
  dirty (translated code from it is dropped). Mirrors are aliases of one backing store.
- **Resets**: a device asserts a reset line wired to `uam_machine_reset_sink`; the executor
  resets the machine between instructions and keeps running (RAM preserved, board reset
  hook for re-loading images).
- **HAL interception**: replace a firmware function with a native handler through a PC hook
  (`uam_exec_add_pc_hook`): the hook reads arguments, writes results and sets the pc. Hooks are
  placed by address; lookup by symbol name is planned.
- **Stock boards**: `rv32-ref`, an RV32IMAC M-mode reference board (CLINT, PLIC, NS16550A UART,
  sifive,test finisher, virtio-net ports, RAM at 0x80000000), and `mips-ref` / `mipsel-ref`
  (MIPS32 with a NS16550A UART and a test finisher). Real devices are built out of tree on the
  library (`examples/external-board/`).

## 6. Analysis and fuzzing

- **Deterministic by default**: the clock is instruction count, one hart runs on one host
  thread, and host time is never read unless real-time pacing is chosen. Every run is bounded
  in virtual time: an instruction budget, a virtual-time limit, and a stall count. After
  `max_stall` timer events with no instruction retired, a waiting hart stops skipping time and
  blocks on host input, or the run ends as a livelock if no input source exists. Each of these
  decisions is made in virtual time.
- **JIT block chaining** (`exec/jit.h`): a compiled block whose EXIT leads to another compiled
  block jumps straight into it instead of returning to the run loop, for as long as the run loop
  would have nothing to do in between: no exit request, deadline not reached, links current.
  Each EXIT owns a link slot after its code (a computed EXIT caches its last target); a slot is
  linked when the run loop next runs the block the EXIT went to. Links belong to an epoch that
  moves on whenever a linked block may be invalid (a block freed or a code page written, a
  translation or privilege change). Interrupts are still taken only by the run loop, so an ISA
  must request an exit on anything that can make `irq_take` newly succeed (`isa.h`). Chained runs
  are bit-identical to unchained ones and to the interpreter; `--no-chain` turns it off.
- **Snapshots** (`snapshot.h`): the hart and ISA state, every RAM/ROM/ROMD backing store, virtual
  time and armed timers, each device's state (`save`/`restore` over `state.h`), and extra state
  a board or analysis registers. Taking or restoring a snapshot makes it the machine's baseline
  and turns on the bus's per-4 KiB dirty bitmap: `uam_bus_page_host` withholds write access to
  clean pages, so an executor's first store to a page (JIT fast path included) goes through
  `uam_bus_write` and marks it. Restoring the baseline copies back only dirty pages; any other
  snapshot is compared page by page. Pages that end up unchanged keep their translated code.
  Timers are saved by their owners with their position among equal deadlines, and re-armed in
  that order, so restored runs are bit-identical. A device without `save`/`restore` (and not
  `UAM_DEVICE_STATELESS`), or an armed timer nobody saved, makes taking a snapshot fail.
  Snapshot files are host-independent and checksummed; restore checks the board, ISA, memory
  map and component list before changing anything.
- **IR passes** (`pass.h`): registered passes see each new block after the optimizer and the
  verifier and before PC-hook marking and the JIT, in registration order; the block is
  verified again after each. A pass may only insert ops, so guest semantics are untouched.
  Adding or removing one flushes translated code; passes are not snapshot state.
- **Edge coverage** (`cov.h`, `uam_exec_set_coverage`): the coverage pass inserts
  `UAM_OP_COV A, B` directly after the IMARK of each instrumented instruction
  (`uam-isa.md` §7.5): `idx = (A ^ prev) & mask; saturating map[idx]++; prev = B`. The site
  table (guest virtual pc, A, B) is the caller's coverage scheme; block mode puts a site at
  every block start. Coverage has its own opcode rather than a `HELPER` call so that the JIT
  emits it inline (about ten host instructions, no call, chained blocks count too) and so that
  it provably touches nothing but the map: guest state, `icount` and timing are the same with
  coverage on, and the interpreter and the JIT produce byte-identical maps. With coverage off
  no block contains COV. The map is never part of a snapshot. Sites in delay slots cannot be
  instrumented (translation fails, naming the pc).
- **Exception observer** (`uam_exec_set_exc_hook`): sees each synchronous exception before
  the ISA takes it, with precise state, and may stop the run (`UAM_STOP_EXC_HOOK`) instead of
  delivering it. One slot (the fuzz harness owns it). Interrupts and resets are not reported.
- **Fuzz harness** (`fuzz.h`, [`fuzzing.md`](fuzzing.md)): boot to a begin pc, snapshot,
  and run each input from the snapshot through a named route (guest memory, MMIO `input`
  rules, or an input a board published such as a UART FIFO), with paired or begin-only
  iteration boundaries. Verdicts come from crash pcs, the exception observer filtered by
  exception class, resets, and the executor's stop reasons (hangs, the per-input budget).
  Frontends sit behind `uam_fuzz_frontend`; the replay frontend runs saved inputs.
- **Planned** (phase 3, on top of snapshots):
  - More passes: cmp-logging, memory hooks, taint (these need `HELPER` support in the JIT).
  - More crash checks: execution outside code regions, an icount watchdog per function.
  - suzzer interoperation (*LCV3/LCVB1 done*, see [`fuzzing.md`](fuzzing.md)): the SZR1
    frontend, LCVS export.

  PC hooks, watchpoints and `uam_exec_stop` are already available to build oracles by hand.
- **GDB stub** against the abstract state (`--gdb PORT`): breakpoints are PC hooks, watchpoints
  are bus watches, registers come from `isa->gdb`.
- **Host I/O** is outside the machine: devices use `chardev.h` / `netdev.h`; backends (stdio,
  pty, sockets, tap, tun, UDP, in-process pairs, pcap capture) and the host loop live in `src/host`.
  Virtual-time pacing (the default) keeps runs deterministic; an idle hart skips straight to its
  next timer, and host input is looked at every 64 skips. Real-time pacing is opt-in, and
  implied by the runner when a port is attached to a host network interface. The runner holds
  console input until the run starts, so fixed input (`string:`, `in=FILE`) is not lost to the
  board's power-on reset or to a snapshot restore.
- **Networking** follows the console's path. A board names its ports and looks them up in its
  config (`uam_board_config_netdev`); a missing port is an unplugged cable. The NIC model
  (any MAC, switch or radio) is a netdev frontend and moves frames with bus DMA. The netdev
  carries Ethernet frames; a tun backend adapts them to IP packets. The host loop stops
  reading a backend while the guest's queue is full, so the host kernel applies backpressure.

## 7. Phasing

1. IR (builder, verifier, printer), interpreter, bus with RAM/ROM/MMIO, RV32I lifter. *Done.*
2. RV32IMAC, M-mode CSRs and traps, CLINT/PLIC/UART board. Milestone: boot a FreeRTOS demo printing over UART. *Implemented* (the FreeRTOS milestone itself is not part of the test suite).
3. Snapshots (*done*), coverage (*done*: IR passes, `UAM_OP_COV`, the coverage pass) and
   cmp-log passes, unknown-MMIO fuzz policy (*done*: per-register rules with an `input`
   policy), a persistent-mode fuzz harness (*done*, with a replay frontend).
4. Board description format, more peripherals, Cortex-M, JIT (*done*: x86-64, with block chaining).

Done outside the plan: MIPS32 (CP0, TLB, both byte orders), host networking, a gdb stub, the
library build with an ABI handshake.

## 8. Verification

Implemented (`make test`):

- Unit tests: IR semantics, verifier and printer (`test_ir`); bus, resets, hooks, time and
  livelock bounds, the ABI handshake (`test_core`); host backends, the host loop and console
  holding (`test_host`); state streams and snapshots under both backends (`test_snapshot`).
- The IR verifier runs on every translated block (on by default; `--no-verify` skips it), before
  and after the optimizer.
- Firmware self-tests: bare-metal RV32 and MIPS32 (both byte orders) programs that check
  themselves and report through the test finisher, run on the interpreter (the snapshot
  check below runs them on the JIT as well).
- Snapshot determinism: `tests/snapshot/snapshot-check.sh` splits a run with `--save-snapshot`
  / `--load-snapshot` at several points and requires the same console output, pc and icount as
  the uninterrupted run, with the interpreter and the JIT on either side.
- MMIO rules: `test_mmio` covers parsing, every policy, masks, narrow accesses on a big-endian
  CPU, priorities against real devices, catch-alls and holes, the input stream, snapshots, and a
  guest reading a rule through the interpreter and the JIT.
- Chaining: `test_jit` runs one guest (calls, returns, branches, self-modifying code, DI/EI
  with a timer, MMIO) through the interpreter and the JIT with and without chaining, with host
  code patches, PC hooks and snapshot restores in between, and requires identical state.
- Coverage: `test_cov` covers `UAM_OP_COV` (verifier, printer, optimizer, saturation, prev
  chaining), passes (no-op and counting passes change nothing; failing or invalid ones stop
  the run), expected hit counts and edge buckets for a known loop on RV32 and MIPS32 BE/LE
  under the interpreter and the JIT with and without chaining, block mode, rejected sites,
  coverage off, and the exception observer on an illegal instruction on each ISA.
  `tests/cov/cov-check.sh` runs the self-tests with block coverage on every backend and
  requires identical maps and an outcome identical to the run without coverage.
- Fuzzing: `test_fuzz` runs a hand-assembled planted-bug target on RV32 and MIPS32 BE/LE
  under every backend: each verdict and its detail, identical maps and results across
  backends and across restores, paired and begin-only modes, the exception-class policy,
  routes, resets, the frontend protocol, abort, and the replay frontend.
  `tests/fuzz/fuzz-check.sh` does the same through `uam-run --fuzz-replay` on C firmware
  (`fuzztarget.h`) on all three ISAs, including replaying a crash file on its own.
- suzzer formats: `test_suzzer` reproduces every conformance vector of the configured suzzer (N=1..4 and 8,
  rotation off and on) byte for byte through `UAM_OP_COV` on every backend, checks the map-size rule against suzzer's table, and
  covers the LCV3/LCVB1 readers (every rejection of suzzer's reader checklist), base
  resolution, boundaries, site naming and the site-byte check. `tests/suzzer/suzzer-check.sh`
  runs `--cov-sites`/`--cov-boundary` end to end on all three ISAs, including N=3 and N=8
  with rotation.
- With a suzzer checkout configured: its format header compiles on its own with ember's
  strict flags, and `test_suzzer` reads the checkout's own conformance vectors. Without one,
  the suzzer tests are skipped.
- The external-board example is built and run against the library.

Planned: per-instruction differential testing between the interpreter and the JIT (random
instruction and state), riscv-tests / riscv-arch-test, golden text-form tests for the lifters.
