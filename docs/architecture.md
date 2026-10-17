# UAM Firmware Emulator — Architecture

Status: phase 1 implemented, plus a second ISA (MIPS32 with a TLB). IR, interpreter, bus
(priorities, device byte order, ROM devices, aliases, watchpoints), RV32IMAC (`virt` board) and
MIPS32 big/little-endian (`mipssim` board), host I/O backends, a gdb stub, and a reusable runner.
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
| `isa.h` | `lift`, register descriptor table, `take_exception`, `irq_take`/`irq_wakeup`/`irq_input`, `sysreg_read`/`sysreg_write`, `default_endian`, optional `translate` (v->p, with fault sub-causes), optional `gdb` register view | `isa/riscv`, `isa/mips32` |
| `bus.h` | Region map (RAM / ROM / MMIO / ROMD / alias) with priorities, software TLB, device byte order and access widths, watchpoints, fault reporting, DMA master access | the core |
| `device.h` | `read`, `write`, optional `write_masked`, `reset`, `destroy`; declared `endian` and implemented access sizes (`save` / `restore` and declarative register tables are planned) | one file per device |
| `sched.h` | Instruction-count clock, event queue, exit requests, list consistency checks | the core |
| `signal.h` | Interrupt, GPIO and reset lines: raise, lower, connect; edge and level | the core |
| `chardev.h` | Byte streams between a device and the host (UART consoles) | core plumbing; backends in `src/host` |
| `netdev.h` | Ethernet frames between a device and the host (NICs, switches, radio "air") | core plumbing; backends in `src/host` |
| `machine.h` | Composition: machine lifecycle, resets (device reset line, board reset hook), board registry, board config (RAM, unknown-MMIO policy, console, options, named images) | `src/machine`, `boards/*` |
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
isa/*         → ir, isa.h
exec/*        → ir, isa.h, bus.h, sched.h
analysis/*    → ir, pass.h               (never an ISA or a device)
core (bus, sched, signal, chardev, netdev, snapshot) → their own headers only
devices/*     → device.h, signal.h, sched.h, chardev.h, netdev.h,
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

## 4. Source layout

Implemented:

```
include/uam/      common.h access.h ir.h isa.h cpu.h bus.h device.h signal.h sched.h exec.h
                  loader.h machine.h chardev.h netdev.h host.h gdbstub.h runner.h boards.h
include/uam/dev/  clint.h plic.h uart16550.h sifive_test.h unknown_mmio.h unimp.h
include/uam/isa/  riscv.h mips32.h
src/ir/           builder.c verify.c print.c opt.c ir_eval.h
src/core/         bus.c sched.c common.c chardev.c netdev.c
src/exec/         interp.c tcache.c run.c cpu.c jit_x86.c execops.h
src/loader/       loader.c
src/machine/      machine.c
src/host/         chardev_backends.c netdev_backends.c hostloop.c
src/debug/        gdbstub.c
src/runner/       runner.c
isa/riscv/        rv_lift.c rv_system.c rv.h (private)
isa/mips32/       mips_lift.c mips_system.c mips.h (private)
devices/          clint.c plic.c uart16550.c sifive_test.c unknown_mmio.c unimp.c
boards/           virt.c mipssim.c stock.c
tools/            uam-run.c
examples/         external-board/ (an out-of-tree board built against `make lib`)
tests/unit/       test_ir.c test_core.c test_host.c
tests/firmware/       start.S selftest.c link.ld qemu-check.sh  (RISC-V)
tests/firmware-mips/  start.S selftest.c link.ld qemu-check.sh  (MIPS32)
```

Planned: `src/core/snapshot.c`, `analysis/` (coverage, cmp-log, taint),
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
- **First board**: `virt`, an RV32IMAC M-mode board whose memory map matches QEMU's `virt` machine
  (CLINT, PLIC, NS16550A UART, sifive,test finisher, RAM at 0x80000000). The same firmware
  ELF runs under QEMU, which `make qemu-check` uses as a differential reference.

## 6. Analysis and fuzzing

- **Deterministic by default**: the clock is instruction count, harts run round-robin on one
  host thread, and host time is never read.
- **Snapshots**: the register file, plus RAM with a per-4 KiB dirty bitmap (restore copies back
  only dirty pages), plus device save/restore, plus the scheduler.
- **Instrumentation passes** insert `HELPER` calls: edge coverage, cmp-logging, memory hooks, taint.
- **Fuzz input routing**: MMIO reads, peripheral receive FIFOs, DMA buffers.
- **Crash oracles**: faults, unmapped access, execution outside code regions, illegal
  instructions, an icount watchdog, and panic/assert hooks.
- **Harness**: AFL++ persistent mode on top of snapshot restore.
- **GDB stub** against the abstract state (`--gdb PORT`): breakpoints are PC hooks, watchpoints
  are bus watches, registers come from `isa->gdb`.
- **Host I/O** is outside the machine: devices use `chardev.h` / `netdev.h`; backends (stdio,
  pty, sockets, tap, UDP, in-process pairs, pcap capture) and the host loop live in `src/host`.
  Virtual-time pacing (the default) keeps runs deterministic; real-time pacing is opt-in.

## 7. Phasing

1. IR (builder, verifier, printer), interpreter, bus with RAM/ROM/MMIO, RV32I lifter.
2. RV32IMAC, M-mode CSRs and traps, CLINT/PLIC/UART board. Milestone: boot a FreeRTOS demo printing over UART.
3. Snapshots, coverage and cmp-log passes, unknown-MMIO fuzz policy, AFL++ harness.
4. Board description format, more peripherals, Cortex-M, JIT.

## 8. Verification

- Per-instruction differential testing against QEMU/Unicorn: random instruction and state,
  then compare the register file and memory.
- riscv-tests / riscv-arch-test under the interpreter.
- IR verifier enabled on every block in debug builds, plus golden text-form tests for the lifter.
- Determinism: identical traces across runs, and identical re-execution after a snapshot restore.
