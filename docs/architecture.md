# UAM Firmware Emulator — Architecture

Status: phase 1 implemented, plus a second ISA (MIPS32 with a TLB). IR, interpreter, bus
(priorities, device byte order, ROM devices, aliases, watchpoints), RV32IMAC (`rv32-ref` board) and
MIPS32 big/little-endian (`mips-ref` / `mipsel-ref` boards), host I/O backends, a gdb stub, a reusable
runner, and machine snapshots.
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
        take_exception / irq_accept /     │        ┌──────────────────────────────┐
        sysreg / mmu_translate            │        │ Bus     memory & MMIO        │
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
| `isa.h` | `lift`, register descriptor table, `take_exception`, `irq_take`/`irq_wakeup`/`irq_input`, `sysreg_read`/`sysreg_write`, `default_endian`, optional `translate` (v->p, with fault sub-causes), optional `gdb` register view, `save`/`restore` of ISA-private state | `isa/riscv`, `isa/mips32` |
| `bus.h` | Region map (RAM / ROM / MMIO / ROMD / alias) with priorities, software TLB, device byte order and access widths, watchpoints, fault reporting, DMA master access (`uam_bus_dma_read`/`_write`), dirty-page tracking for snapshots | the core |
| `device.h` | `read`, `write`, optional `write_masked`, `reset`, `destroy`, `save`/`restore` (or `UAM_DEVICE_STATELESS`); declared `endian` and implemented access sizes (declarative register tables are planned) | one file per device |
| `state.h` | State streams: how a component writes and reads back its state (fixed-width little-endian fields, byte arrays, timers with their tie order, signal levels; sticky errors) | the core |
| `sched.h` | Instruction-count clock (saturating at a 2^62-tick horizon), event queue with a per-call firing budget, exit requests, list consistency checks | the core |
| `signal.h` | Interrupt, GPIO and reset lines: raise, lower, connect; edge and level | the core |
| `chardev.h` | Byte streams between a device and the host (UART consoles) | core plumbing; backends in `src/host` |
| `netdev.h` | Ethernet frames between a device and the host (NICs, switches, radio "air") | core plumbing; backends in `src/host` |
| `machine.h` | Composition: machine lifecycle, resets (device reset line, board reset hook), board registry, board config (RAM, unknown-MMIO policy, console, options, named images, named network ports), extra snapshot state (`uam_machine_add_state`), ABI handshake | `src/machine`, `boards/*` |
| `snapshot.h` | Machine snapshots: take / restore in memory (incremental against the last baseline), save / load files | `src/machine` |
| `cpu.h` | ISA-neutral hart state and the core services ISA modules may call (`uam_cpu_xlate_changed`, ...) | the core |
| `loader.h` | ELF / raw images into guest memory | `src/loader` |
| `exec.h` | Run a hart: translation cache, block execution (interpreter or JIT), result handling, reset requests, PC hooks, the host bridge (polling, idle) | `exec/interp`, `exec/jit_x86` |
| `host.h` | Host side: chardev/netdev backends, host loop and real-time pacing | `src/host` |
| `gdbstub.h` | gdb remote protocol server on top of hooks, watchpoints and `isa->gdb` | `src/debug` |
| `runner.h` | The stock command line as a library entry point (`uam_run_main`) | `src/runner` |
| `boards.h` | The stock boards and their registration | `boards/stock.c` |
| `pass.h` (planned) | IR → IR transformations (instrumentation, optimisation) | `analysis/*`, `ir/opt` |

## 3. Dependency rules

Arrows point to what a module may include. Anything not listed is forbidden.

```
ir            ← nothing (pure data + algorithms over it)
isa/*         → ir, isa.h, state.h
exec/*        → ir, isa.h, bus.h, sched.h
analysis/*    → ir, pass.h               (never an ISA or a device)
core (bus, sched, signal, chardev, netdev, state) → their own headers only
                                         (state.h: also sched.h and signal.h, whose objects it saves)
devices/*     → device.h, signal.h, sched.h, chardev.h, netdev.h, state.h,
                bus.h (DMA, and the backing store of a ROMD region it owns)
                                         (never ir, never an ISA, never the host)
boards/*      → everything public except host.h; the only place concrete modules meet
host/, debug/ → public interfaces (chardev, netdev, exec, machine); never a concrete device
runner, fuzz/, tools/ → boards + public headers
```

The consequences are deliberate:

- A device never knows which CPU is driving it, nor which host backend is behind its chardev.
- An ISA never knows which devices exist.
- Byte order and access width are the bus's job: a device states its own and the bus adapts.
- A device that resets the chip raises a reset *line*; the board wires it to the machine.
- Analysis passes work for every ISA for free.
- The library contains no boards: programs register the boards they link.

**Binary interface.** Out-of-tree boards embed public structs (`uam_machine`, `uam_device`, ...)
and are compiled separately from `libuam`, so header/library skew would silently shift field
offsets. `UAM_ABI_VERSION` (`common.h`) is bumped on any change to a public struct layout, enum
or function signature, and `uam_board_register`, `uam_machine_init` and `uam_machine_add_device`
are macros that pass the caller's version and struct sizes (`uam_abi_layout`); the library aborts
with a list of the differences when they do not match its own.

## 4. Source layout

Implemented:

```
include/uam/      common.h access.h ir.h isa.h cpu.h bus.h device.h signal.h sched.h exec.h
                  loader.h machine.h chardev.h netdev.h host.h gdbstub.h runner.h boards.h
                  state.h snapshot.h
include/uam/dev/  clint.h plic.h uart16550.h sifive_test.h unknown_mmio.h unimp.h virtio_net.h
include/uam/isa/  riscv.h mips32.h
src/ir/           builder.c verify.c print.c opt.c ir_eval.h
src/core/         bus.c sched.c common.c chardev.c netdev.c state.c
src/exec/         interp.c tcache.c run.c cpu.c jit_x86.c execops.h
src/loader/       loader.c
src/machine/      machine.c snapshot.c
src/host/         chardev_backends.c netdev_backends.c hostloop.c
src/debug/        gdbstub.c
src/runner/       runner.c
isa/riscv/        rv_lift.c rv_system.c rv.h (private)
isa/mips32/       mips_lift.c mips_system.c mips.h (private)
devices/          clint.c plic.c uart16550.c sifive_test.c unknown_mmio.c unimp.c virtio_net.c
boards/           rv32_ref.c mips_ref.c stock.c
tools/            uam-run.c
examples/         external-board/ (an out-of-tree board built against `make lib`)
tests/unit/       test_ir.c test_core.c test_host.c test_snapshot.c test_jit.c
tests/snapshot/   snapshot-check.sh  (split runs through --save-snapshot / --load-snapshot)
tests/firmware/       start.S selftest.c nettest.c link.ld  (RISC-V)
tests/net/            net-check.sh  (nettest.elf against real tap and tun interfaces)
tests/firmware-mips/  start.S selftest.c link.ld  (MIPS32)
```

Planned: `analysis/` (coverage, cmp-log, taint),
`fuzz/afl_harness.c`, an Intel HEX loader, more boards and devices.

## 5. Firmware specifics

- **Loader**: raw binary at a base address and ELF (PT_LOAD by physical address); Intel HEX is planned.
- **Unknown MMIO**: a device that claims unmodelled regions. Its policy can be set per register:
  log-and-zero, echo the last write, a fixed value, or fuzz input. Every access is logged to
  drive bring-up. Per-block catch-alls (`devices/unimp.c`) are mapped at
  `UAM_PRIO_BACKGROUND` under real devices and report offsets relative to the block.
- **Flash**: memory-mapped NOR with a command interface is a ROMD region: fetchable in ROM
  mode, writes go to the controller device, which updates the backing store and marks it
  dirty (translated code from it is dropped). Mirrors are aliases of one backing store.
- **Resets**: a device asserts a reset line wired to `uam_machine_reset_sink`; the executor
  resets the machine between instructions and keeps running (RAM preserved, board reset
  hook for re-loading images).
- **HAL interception**: replace a firmware function, by address or symbol, with a native handler.
- **First board**: `rv32-ref`, an RV32IMAC M-mode reference board (CLINT, PLIC, NS16550A UART,
  sifive,test finisher, virtio-net ports, RAM at 0x80000000).

## 6. Analysis and fuzzing

- **Deterministic by default**: the clock is instruction count, harts run round-robin on one
  host thread, and host time is never read.
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
- **Instrumentation passes** insert `HELPER` calls: edge coverage, cmp-logging, memory hooks, taint.
- **Fuzz input routing**: MMIO reads, peripheral receive FIFOs, DMA buffers.
- **Crash oracles**: faults, unmapped access, execution outside code regions, illegal
  instructions, an icount watchdog, and panic/assert hooks.
- **Harness**: AFL++ persistent mode on top of snapshot restore.
- **GDB stub** against the abstract state (`--gdb PORT`): breakpoints are PC hooks, watchpoints
  are bus watches, registers come from `isa->gdb`.
- **Host I/O** is outside the machine: devices use `chardev.h` / `netdev.h`; backends (stdio,
  pty, sockets, tap, tun, UDP, in-process pairs, pcap capture) and the host loop live in `src/host`.
  Virtual-time pacing (the default) keeps runs deterministic; real-time pacing is opt-in, and
  implied by the runner when a port is attached to a host network interface.
- **Networking** follows the console's path. A board names its ports and looks them up in its
  config (`uam_board_config_netdev`); a missing port is an unplugged cable. The NIC model
  (any MAC, switch or radio) is a netdev frontend and moves frames with bus DMA. The netdev
  carries Ethernet frames; a tun backend adapts them to IP packets. The host loop stops
  reading a backend while the guest's queue is full, so the host kernel applies backpressure.

## 7. Phasing

1. IR (builder, verifier, printer), interpreter, bus with RAM/ROM/MMIO, RV32I lifter.
2. RV32IMAC, M-mode CSRs and traps, CLINT/PLIC/UART board. Milestone: boot a FreeRTOS demo printing over UART.
3. Snapshots, coverage and cmp-log passes, unknown-MMIO fuzz policy, AFL++ harness.
4. Board description format, more peripherals, Cortex-M, JIT.

## 8. Verification

- Per-instruction differential testing between the interpreter and the JIT: random
  instruction and state, then compare the register file and memory.
- riscv-tests / riscv-arch-test under the interpreter.
- IR verifier enabled on every block in debug builds, plus golden text-form tests for the lifter.
- Determinism: identical traces across runs, and identical re-execution after a snapshot restore.
- Chaining: `test_jit` runs one guest (calls, returns, branches, self-modifying code, DI/EI
  with a timer, MMIO) through the interpreter and the JIT with and without chaining, with host
  code patches, PC hooks and snapshot restores in between, and requires identical state.
