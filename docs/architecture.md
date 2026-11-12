# UAM Firmware Emulator — Architecture

Status: phases 1 and 2 implemented, plus more ISAs (MIPS32 with a TLB, ARMv7-A with an
MMU, x86-64, 32-bit PowerPC with the classic and the Book E MMU, 8-bit AVR in every core family, AVR32A
and AVR32B, 8-bit PIC in four core families, the 8051 in three core models, the 6502 in
three core models, the Z80, Xtensa in three core models and both byte orders), the
phase 3 except the cmp-log pass (snapshots, coverage, the fuzz harness, guards, suzzer
interoperation) and the JIT from phase 4. IR, interpreter and optimizer, a
JIT with block chaining for x86-64 and AArch64 hosts, bus (priorities, device byte order, ROM devices, aliases,
watchpoints, dirty-page tracking), RV32IMAC (`rv32-ref` board), MIPS32 big/little-endian
(`mips-ref` / `mipsel-ref` boards), ARMv7-A, ARM and Thumb-2, little-endian and BE8
(`arm-ref` / `armeb-ref` boards), x86-64 (`x86_64-ref`), 32-bit PowerPC (`ppc-ref`), 8-bit AVR
(`avr-ref`), AVR32 (`avr32-ref`), 8-bit PIC (`pic-ref`), PIC32 (`pic32-ref`, on the MIPS32
module), the 8051 (`mcs51-ref`), the 6502
(`6502-ref`), the Z80 (`z80-ref`), Xtensa (`xtensa-ref` / `xtensaeb-ref`), host
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
| `isa.h` | `lift`, register descriptor table, `take_exception`, `irq_take`/`irq_wakeup`/`irq_input`, `sysreg_read`/`sysreg_write`, `default_endian` (and `fetch_endian` when code and data differ: ARM BE8), optional `translate` (v->p, with fault sub-causes), optional `gdb` register view, `save`/`restore` of ISA-private state, optional `switch_in` (multiprocessor: state the platform keeps once for all processors follows the running one, such as a chipset's SMIACT# tracking), optional `irq_lines` (what an interrupt observer sees). An ISA must request an exit (`uam_cpu_request_exit`) on any change that can make `irq_take` newly succeed. The `gdb` view can name pc bits that select an instruction-set state (`pc_state_bits`: ARM Thumb) | `isa/riscv`, `isa/mips32`, `isa/arm`, `isa/x86`, `isa/ppc32`, `isa/avr8`, `isa/avr32`, `isa/pic8`, `isa/mcs51`, `isa/m6502`, `isa/z80`, `isa/xtensa` |
| `bus.h` | Region map (RAM / ROM / MMIO / ROMD / alias) with priorities (real devices, then per-register stubs from `UAM_PRIO_STUB`, then catch-all windows at `UAM_PRIO_BACKGROUND`), software TLB, device byte order and access widths, watchpoints, fault reporting, DMA master access (`uam_bus_dma_read`/`_write`), dirty-page tracking for snapshots; a watch with `UAM_WATCH_FAULT` refuses the access instead of observing it (what guards are built on); one observer of every guest access a device serves (`uam_bus_set_dev_observer`: `--trace mmio`), and the last device read (what a polling loop saw) | the core |
| `device.h` | `read`, `write`, optional `write_masked`, `reset`, `destroy`, `save`/`restore` (or `UAM_DEVICE_STATELESS`), optional `peek` (a side-effect-free read for `--dump-mem` and gdb); declared `endian` and implemented access sizes (declarative register tables are planned) | one file per device |
| `state.h` | State streams: how a component writes and reads back its state (fixed-width little-endian fields, byte arrays, timers with their tie order, signal levels; sticky errors) | the core |
| `sched.h` | Instruction-count clock (saturating at a 2^62-tick horizon), event queue with a per-call firing budget, a kick when the earliest deadline moves (the machine turns it into an exit request), cancel-all, list consistency checks | the core |
| `signal.h` | Interrupt, GPIO and reset lines: connect a line to one sink, set its level (the sink is called on a change; a sink that needs edges detects them) | the core |
| `blockdev.h` | Fixed-size sectors between a storage controller (AHCI, NVMe, IDE, floppy) and the host: read, write, flush; read-only and volatile (in-memory overlay) modes | core interface; the file backend in `src/host` |
| `chardev.h` | Byte streams between a device and the host (UART consoles); input can be held back until the run starts (`uam_chardev_hold`); one tap on guest output (`uam_chardev_set_tap`: `--trace console`) | core plumbing; backends in `src/host` |
| `netdev.h` | Ethernet frames between a device and the host (NICs, switches, radio "air") | core plumbing; backends in `src/host` |
| `machine.h` | Composition: machine lifecycle, resets (device reset line, board reset hook), board registry, board config (RAM, unknown-MMIO policy, console, options, named images, named network ports), extra snapshot state (`uam_machine_add_state`), named inputs a board publishes (`uam_machine_add_input`: a UART FIFO, a DMA buffer), extra processors (`uam_machine_add_cpus`: each with its own translation cache, sharing bus and clock; `uam_machine_current_cpu` for per-processor devices) and observers on all of them (`uam_machine_add_pc_hook`, `_set_exc_hook`, `_set_irq_hook`, `_add_pass`, `_set_coverage` with a coverage history per processor, `_get_stats`), ABI handshake | `src/machine`, `boards/*` |
| `snapshot.h` | Machine snapshots: take / restore in memory (incremental against the last baseline), save / load files | `src/machine` |
| `cpu.h` | ISA-neutral hart state (including the coverage state `UAM_OP_COV` counts into, and `insn_pc`, the instruction a bus, system register or helper access is made for), the core services ISA modules may call (`uam_cpu_xlate_changed`, ...), and observers of system register accesses and interrupt input lines (`uam_cpu_observer`: `--trace sysreg`, `--trace irq`) | the core |
| `loader.h` | ELF / raw images into guest memory | `src/loader` |
| `exec.h` | Run a hart: translation cache, block execution (interpreter, or JIT with optional block chaining), result handling, spin-loop skipping, reset requests, PC hooks, IR passes, edge coverage (`uam_exec_set_coverage`), the exception and interrupt observers (`uam_exec_set_exc_hook`, `uam_exec_set_irq_hook`), the host bridge (polling, idle), instruction / virtual-time / stall (livelock) bounds, a stop at the faulting instruction requested from inside an access (`uam_exec_request_fault_stop`: `UAM_STOP_GUARD`), statistics, device polls (`uam_poll`, with `UAM_TRACE_POLL`: one-block loops that re-read a device register, counted, and the value that ends them worked out by re-evaluating a pass with other values), the `uam_origin` provider while it runs; with several processors, processor 0's executor interleaves them deterministically in instruction slices on the shared clock (idle skips only when all wait); where the interleaving stands is machine state, so split and restored runs interleave as one run, and a processor's load-reserved reservation ends with its slice | `src/exec/interp`, `src/exec/jit` (code generators `jit_x86`, `jit_a64`) |
| `host.h` | Host side: chardev/netdev backends, host loop, virtual and real-time pacing | `src/host` |
| `gdbstub.h` | gdb remote protocol server on top of hooks, watchpoints and `isa->gdb`; every processor is a thread (all-stop) | `src/debug` |
| `runner.h` | The stock command line as a library entry point (`uam_run_main`), and runner extensions: options plus setup/attach/finish callbacks that contribute coverage sites, iteration boundaries or a fuzz frontend without the runner knowing what they are for | `src/runner`; stock extensions in `fuzz/stock_exts.c` |
| `boards.h` | The stock boards and their registration, and board helpers (`uam_board_add_mmio_rules`, `uam_board_add_uart_input`, `uam_board_open_disk`) | `boards/stock.c`, `boards/mmio_rules.c` |
| `pass.h` | IR → IR transformations run on every new block, after the optimizer and before native compilation (instrumentation); block editing helpers | `analysis/*`; registered with `uam_exec_add_pass` |
| `guard.h` | Guarded and watched guest memory: byte ranges (virtual or physical, around an anchor) whose data accesses stop the run at the instruction (`UAM_STOP_GUARD`), become a bus fault for the guest, or are logged; hit records and an observer; the text form `--guard`/`--watch` and fuzz redzones use | `src/machine` (on bus fault watches and `uam_exec_request_fault_stop`) |
| `fuzz.h` | The persistent-mode fuzz harness (boot to a begin pc, snapshot, one restore per input, verdicts) and its frontend interface; the replay frontend | `fuzz/` (see [`fuzzing.md`](fuzzing.md)) |
| `cov.h` | The edge-coverage pass: `UAM_OP_COV` at a table of guest pcs (or at every block start), knowing nothing about where the table came from | `analysis/cov.c` |

## 3. Dependency rules

Arrows point to what a module may include. Anything not listed is forbidden.

```
ir            ← nothing (pure data + algorithms over it)
isa/*         → ir, isa.h, state.h      (x86 and PowerPC page-table walks also bus.h:
                                         physical reads and accessed/referenced/changed bits;
                                         x86 also its SMM save state and port and physical
                                         helper accesses;
                                         AVR interrupt entry: the return address it pushes;
                                         PIC: a named INDF reached through a banked operand;
                                         AVR32, 6502 and Z80 interrupt entry: the stack
                                         pushes; also sched.h for the clock: the timers of
                                         ARM, AVR32, MIPS, PowerPC, x86 and Xtensa; signal.h
                                         for the CPU output lines of MIPS and AVR32)
exec/*        → ir, isa.h, bus.h, sched.h, pass.h, cov.h (it wires passes in)
analysis/*    → ir, pass.h, cov.h        (never an ISA or a device)
core (bus, sched, signal, chardev, netdev, state) → their own headers only
                                         (bus: also device.h, whose ops it calls; state.h: also
                                         sched.h and signal.h, whose objects it saves)
machine       → public interfaces        (it composes them; loader → loader.h, isa.h, bus.h)
devices/*     → device.h, signal.h, sched.h, chardev.h, netdev.h, blockdev.h, state.h,
                other uam/dev/*.h,
                bus.h (DMA, the backing store of a ROMD region it owns, a PCI bus's BAR windows)
                                         (never ir, never an ISA, never the host)
boards/*      → everything public except host.h; the only place concrete modules meet
host/, debug/ → public interfaces (chardev, netdev, blockdev, exec, machine, guard); never a concrete device
runner, fuzz/, tools/ → boards + public headers
fuzz/suzzer/  → as fuzz/, plus suzzer's format header (src/lcv_formats.h of the suzzer
                checkout the build is given; docs/suzzer.md); the only code that knows suzzer's
                formats, built only when a checkout is configured (fuzz/stock_exts.c
                adds its runner extension then)
```

The consequences are deliberate:

- A device never knows which CPU is driving it, nor which host backend is behind its chardev.
  For a log line it may ask `uam_origin_get` (`common.h`) which instruction is behind the
  current access: a processor number, a pc and an icount the executor publishes while it
  runs, never a processor object.
- An ISA never knows which devices exist.
- Byte order and access width are the bus's job: a device states its own and the bus adapts.
- A device that resets the chip raises a reset *line*; the board wires it to the machine.
- Analysis passes work for every ISA for free.
- Devices and ISAs never learn about coverage or fuzzing; core and exec code never mention a
  fuzzer's formats.
- The library registers no boards by itself: a program registers the boards it links
  (`uam_boards_register_stock` for the stock ones).

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
                  state.h snapshot.h pass.h cov.h blockdev.h guard.h fuzz.h
include/uam/dev/  clint.h plic.h gic.h uart16550.h test_finisher.h unknown_mmio.h unimp.h
                  mmio_stub.h virtio_net.h clock.h i8259.h i8254.h mc146818.h apic.h hpet.h
                  i8042.h acpi_pm.h pc_misc.h pci.h ahci.h nvme.h cfi_flash.h ipi.h
                  i8237.h ide.h fdc.h vga.h lpt.h gptimer.h avr32_intc.h mcs51_timer.h
                  mcs51_serial.h mcs51_port.h z80_ctc.h pic32_intc.h
include/uam/isa/  riscv.h mips32.h arm.h x86.h ppc.h avr.h avr32.h pic.h mcs51.h m6502.h z80.h
                  xtensa.h
src/ir/           builder.c verify.c print.c opt.c pass.c ir_eval.h
src/core/         bus.c sched.c common.c chardev.c netdev.c state.c
src/exec/         interp.c tcache.c run.c cpu.c jit.c jit_x86.c jit_a64.c coverage.c spin.c jit.h
                  jit_impl.h execops.h spin.h
analysis/         cov.c (the edge-coverage pass)
src/loader/       loader.c
src/machine/      machine.c snapshot.c guard.c (guards, on bus fault watches)
src/host/         chardev_backends.c netdev_backends.c blockdev_file.c hostloop.c
src/debug/        gdbstub.c
src/runner/       runner.c
isa/riscv/        rv_lift.c rv_system.c rv.h (private)
isa/mips32/       mips_lift.c mips_system.c mips16.c (MIPS16e) mips_umips.c (microMIPS)
                  mips_fpu.c mips_dsp.c (the DSP ASE) umips_dsp.h mips.h (private)
isa/arm/          arm_lift.c (A32 and the shared emitters) arm_thumb.c (T16/T32) arm_system.c
                  arm.h (private)
isa/ppc32/        ppc_lift.c ppc_fpu.c ppc_mmu.c ppc_booke.c ppc_system.c ppc.h (private)
isa/avr8/         avr_lift.c avr_system.c avr_des.c avr.h (private)
isa/avr32/        avr32_dec.c avr32_op.h avr32_lift.c avr32_dsp.c avr32_system.c avr32.h (private)
isa/pic8/         pic_lift.c pic_system.c pic.h (private)
isa/mcs51/        mcs51_lift.c mcs51_system.c mcs51.h (private)
isa/m6502/        m6502_lift.c m6502_system.c m6502.h (private)
isa/z80/          z80_lift.c z80_system.c z80.h (private)
isa/xtensa/       xtensa_decode.c xtensa_opc.h (generated) xtensa_lift.c xtensa_system.c
                  xtensa_fpu.c xtensa.h (private)
isa/x86/          x86_decode.c x86_decode.h x86_lift*.[ch] x86_string.c x86_sse*.c x86_x87.c x86_f80.c x86_helpers.c
                  x86_mmu.c x86_seg.c x86_task.c x86_msr.c x86_smm.c x86_gdb.c x86_system.c
                  x86.h (private)
devices/          clint.c plic.c gic.c uart16550.c test_finisher.c unknown_mmio.c unimp.c
                  mmio_stub.c virtio_net.c i8259.c i8254.c mc146818.c lapic.c ioapic.c hpet.c
                  i8042.c acpi_pm.c pc_misc.c pci.c ata_core.c ahci.c ide.c nvme.c cfi_flash.c
                  ipi.c i8237.c fdc.c vga.c lpt.c gptimer.c avr32_intc.c mcs51_timer.c
                  mcs51_serial.c mcs51_port.c z80_ctc.c pic32_intc.c ata_core.h (private)
boards/           rv32_ref.c mips_ref.c arm_ref.c x86_64_ref.c ppc_ref.c avr_ref.c avr32_ref.c
                  pic_ref.c pic32_ref.c mcs51_ref.c m6502_ref.c z80_ref.c xtensa_ref.c stock.c mmio_rules.c (uam_board_add_mmio_rules,
                  any board)
fuzz/             harness.c frontend_replay.c (the harness and the replay frontend),
                  stock_exts.c (the runner extensions uam_run_main uses)
fuzz/suzzer/      scheme.c (the bucket formula) import.c (LCV3/LCVB1 readers, site check)
                  frontend_szr1.c (the SZR1 executor, --fuzz-szr) lcvs.c (the LCVS export,
                  --cov-shm) cli.c (the runner extension for all of them) suzzer.h
tools/            uam-run.c fetch-suzzer.sh (`make suzzer`: clone suzzer into ./suzzer, ignored
                  by git) xbench.sh jitbench.sh (timing) jitprof.py (native-code profiles,
                  docs/jit-perf.md)
examples/         external-board/ (an out-of-tree board built against `make lib`)
skills/           ember-dev-recon/ ember-dev-build/ ember-fuzz/ (agent skills: SKILL.md, scripts,
                  references, templates) install.sh tests/ (run-tests.sh: the skills' tooling
                  against an ember checkout; never part of the library)
tests/boards/     test_boards.c test_boards.h uam-test-run.c  (x86_64-test and rv32-test: reference boards plus
                  the components only the tests use; build/uam-test-run)
tests/unit/       test_ir.c test_core.c test_host.c test_snapshot.c test_jit.c test_jitdiff.c
                  test_spin.c test_codestore.c test_mmio.c test_cov.c test_fuzz.c test_suzzer.c
                  test_smp.c test_f80.c test_xtensa_fpu.c, and one per ISA: test_riscv.c
                  test_mips.c test_arm.c test_x86.c test_ppc.c test_avr.c test_avr32.c
                  test_pic.c test_mcs51.c test_m6502.c test_z80.c test_xtensa.c
tests/cov/        cov-check.sh  (block-coverage maps across backends)
tests/fuzz/       fuzz-check.sh fuzztarget.h  (the harness on planted-bug firmware, all ISAs)
tests/suzzer/     suzzer-check.sh mk_lists.py lcvs_check.py  (suzzer's formats through the
                  runner) gen-check.sh  (lists from suzzer's own rizin generators; its Ghidra
                  ports write the same files and are not run here)
tests/szr/        szr-check.sh fake_brain.py  (the SZR1 executor against a scripted brain)
                  brain-check.sh  (a session with suzzer's own brain)
tests/snapshot/   snapshot-check.sh  (split runs through --save-snapshot / --load-snapshot)
tests/runner/     runner-check.sh  (uam-run's debugging options: --log-pc, --dump-mem,
                  --trace-after, traces with --jit, --guard/--watch)
                  trace-check.sh  (the bring-up traces: mmio, sysreg, irq, polls, console,
                  --log-stamp, pcs in the catch-all logs; interpreter and JIT alike)
tests/gdb/        gdb-check.py  (the gdb stub over the remote protocol, four processors)
tests/smp/        smp_test.h  (one multiprocessor self-test for RISC-V, MIPS, ARM, PowerPC)
tests/bench/      bench.c Makefile x86-start.S x86.ld pic32.ld  (one C benchmark for every guest ISA but
                  the 8-bit PIC; make
                  bench-test checks it, tools/xbench.sh times it)
tests/firmware/       start.S selftest.c nettest.c smpstart.S smptest.c fuzztarget.c consoletest.c
                      consoletest.script link.ld  (RISC-V; the console test: scripted input)
tests/net/            net-check.sh  (nettest.elf against real tap and tun interfaces)
tests/firmware-mips/  start.S selftest.c smpstart.S smptest.c fuzztarget.c link.ld  (MIPS32)
tests/firmware-arm/   start.S selftest.c smpstart.S smptest.c fuzztarget.c link.ld  (ARMv7: ARM and
                      Thumb-2 builds)
tests/firmware-ppc/   start.S selftest.c smpstart.S smptest.c fuzztarget.c link.ld start-booke.S
                      link-booke.ld  (32-bit PowerPC; Book E: the self-test for that model)
tests/firmware-avr/   selftest.c gen_vectors.py fuzztarget.c  (8-bit AVR, one build per core
                      family, with avr-libc's start-up code)
tests/firmware-avr32/ start.S selftest.c fuzztarget.c link-a.ld link-b.ld  (AVR32A and AVR32B)
tests/firmware-pic/   selftest-{baseline,mid,enhanced,pic18,xinst}.asm gen_vectors.py
                      fuzztarget.asm hex2elf.py  (8-bit PIC, gpasm)
tests/firmware-mcs51/ selftest.asm model-*.asm gen_vectors.py bank.asm mkimage.py fuzztarget.c
                      (the 8051: as31 self-tests per core model, an SDCC fuzz target)
tests/firmware-6502/  selftest.s nmos.s model-*.s gen_vectors.py ihx2bin.py fuzztarget.c
                      (the 6502: sdas6500 self-tests per core model, an SDCC fuzz target)
tests/firmware-z80/   selftest.s model-*.s gen_vectors.py crt0.s fuzztarget.c  (the Z80:
                      sdasz80 self-tests per core model, SDCC start code and fuzz target)
tests/firmware-xtensa/ selftest.S model.h gen_vectors.py window.inc start.S link.ld selftest.ld
                      fuzztarget.c lx106-libgcc.c  (Xtensa: the self-test per model, built
                      with each core's GNU toolchain, and the C programs' start code)
tests/isa-xtensa/     opcodes.py gen-opcodes.py xt-dec.c xt-check.py le2be.py gen-fpu-vectors.py
                      (the opcode table, the decoder against the GNU disassemblers, the
                      big-endian layout of a little-endian build)
tests/firmware-pic32/ start.S probe.S selftest.c fptest.c dsptest.c gen_fpvectors.py
                      gen_dspvectors.py fuzztarget.c link.ld  (PIC32 MX/MM/MZ: MIPS16e,
                      microMIPS, the FPU and DSP against generated vectors)
tests/isa-mips/       mips16-dec.c mips16-check.py umips-dec.c umips-check.py dsp-dec.c
                      dsp-check.py gen-umips-dsp.py  (the MIPS16e, microMIPS and DSP decoders
                      against the GNU disassemblers)
tests/firmware-x86/   selftest.asm smp.asm legacy.asm smm-amd.asm biosrom.asm bios-check.py
                      (x86-64, built with nasm: flat images entered in real mode, and a BIOS ROM)
```

Planned: more passes in `analysis/` (cmp-log, taint), an Intel HEX loader, more boards and
devices.

## 5. Firmware specifics

- **Loader**: raw binary at a base address and ELF (PT_LOAD by physical address); Intel HEX is planned.
- **Unknown MMIO** has three layers, lowest priority first:
  - The global fallback (`devices/unknown_mmio.c`) for addresses no region covers. Unmapped
    accesses fault by default. With `--unknown-mmio zero|echo` this device answers them,
    reading zero or echoing the last value written, and logs the first access to each address
    (with the value written and the instruction: `pc=... icount=N`).
  - Per-block catch-alls (`devices/unimp.c`), mapped at `UAM_PRIO_BACKGROUND` under real
    devices. They are read-as-zero, write-ignored, and report offsets relative to the block
    (the first access to each with its instruction).
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
- **Stock boards**: reference boards, one per architecture, holding only what every board of
  that architecture has. They are examples and starting points, not models of any real
  board: a board emulator adds its own chipset and peripherals out of tree, on the generic
  components or on top of a reference board (run its build, then wire to the parts
  `uam_machine_find_device` finds). `rv32-ref`, an RV32IMAC M-mode reference board (CLINT,
  PLIC, NS16550A UART, test finisher, RAM at 0x80000000), and `mips-ref` / `mipsel-ref`
  (MIPS32 with a NS16550A UART and a test finisher), and `pic32-ref` (a little-endian MIPS
  microcontroller with the PIC32 memory map: `-o model=mx|mm|mz` picks a reference core
  (`-o bootisa=micromips` on mz resets into microMIPS),
  `devices/pic32_intc.c` presents requests to the processor's external interrupt controller
  interface, and the generic UART, timer and test finisher sit in the SFR region), and `arm-ref` / `armeb-ref` (ARMv7-A,
  little-endian and BE8, with a GICv2-style interrupt controller fed by the generic timer and
  the NS16550A UART, a test finisher, RAM at 0 under the vectors), and `ppc-ref` (32-bit
  PowerPC, classic or with `-o model=booke` Book E: RAM at 0 under the vectors, a boot ROM under
  the reset vector (0xfff00100; Book E 0xfffffffc, in the boot page it maps at reset), a NS16550A
  UART and a test finisher), and `x86_64-ref` (the PC/AT core devices, the APICs, HPET, PCI
  configuration, boot flash or a BIOS ROM; no chipset, storage or display; options in its file header
  and the README), and `avr-ref`
  (8-bit AVR, `-o model=` one of the five core families with the memory layout of a common part
  of it: program memory, SRAM, 32 general-purpose I/O registers, a NS16550A UART, the
  general-purpose timer and a test finisher), and `avr32-ref` (AVR32A or, with `-o model=b`,
  AVR32B: RAM, code memory at the reset address, an AVR32-style interrupt controller fed by
  COUNT/COMPARE and the UART, a NS16550A UART and a test finisher), and `pic-ref` (8-bit PIC,
  `-o model=` one of the four core families with the program memory and data space of a
  common part of it, `-o xinst=1` for PIC18's extended instructions: a data space of RAM, a
  NS16550A UART, the general-purpose timer (none on `baseline`, which has no interrupts) and a
  test finisher), and `mcs51-ref` (the
  8051, `-o model=8051|8052|cip51`: program memory (banked on `cip51`), external data RAM, the
  8051's own Timer 0/1, serial port and ports in the SFR space, on `cip51` across SFR pages,
  and a test finisher in external data), `6502-ref` (the 6502, `-o model=nmos|2a03|65c02`:
  RAM, ROM with the vectors, and in an I/O page a NS16550A UART, two general-purpose timers
  (one on IRQ, one on NMI) and a test finisher), and `z80-ref` (the Z80, `-o model=nmos|cmos`:
  64 KiB of RAM, and on I/O ports a NS16550A UART, a Z80 CTC and a test finisher), and
  `xtensa-ref` / `xtensaeb-ref` (Xtensa, `-o model=lx106|lx6|lx7`, `-o cpus=N`: RAM where
  VECBASE and the reset vectors point, a NS16550A UART, two general-purpose timers (one on an
  edge-triggered interrupt, one on the NMI) and a test finisher). Real devices are
  built out of tree on the library (`examples/external-board/`). Ember's own tests also use
  generic components no reference board wires (the 0xB2 SMI port, storage, VGA, printer,
  virtio-net): `tests/boards/` adds them on top of the reference boards as `x86_64-test` and
  `rv32-test`, registered only by the test runner `build/uam-test-run`.
- **Multiprocessor machines** (`-o cpus=N` on `rv32-ref`, `mips-ref`/`mipsel-ref`,
  `arm-ref`/`armeb-ref`, `ppc-ref`, `x86_64-ref` and `xtensa-ref`/`xtensaeb-ref`): processors share the
  bus and the clock and run in deterministic instruction slices (`--quantum`). Every processor
  starts at the reset address and firmware tells them apart by its architectural number:
  `mhartid`; EBase.CPUNum; MPIDR.Aff0; PIR on PowerPC; PRID on Xtensa; the x86 initial APIC ID, where the
  others wait for INIT/SIPI. Per-processor interrupts come from generic parts that know several CPUs: the
  CLINT (a msip and mtimecmp per hart) and PLIC contexts on RISC-V, IPI doorbells
  (`devices/ipi.c`) on MIPS, the GIC on ARM (banked SGIs/PPIs, SGI sources, ITARGETSR-routed
  SPIs, a CPU interface per processor), local APICs on an APIC bus on x86, the decrementer and
  IPI doorbells on the external interrupt input on PowerPC (whose `tlbie` reaches every
  processor on the bus), and on Xtensa each processor's own CCOMPARE timers (the board's
  devices interrupt the first processor; no multiprocessor test covers Xtensa yet).
  `tests/smp/smp_test.h` is one self-test for RISC-V, MIPS, ARM and PowerPC
  (x86: `tests/firmware-x86/smp.asm`): per-processor timer and IPI interrupts
  and a shared log written with atomics, whose hash depends on the interleaving.
- **Legacy PC** (for emulators of real boards that run their BIOS image): the x86 core has
  virtual-8086 mode (IOPL-sensitive instructions, interrupts out of it and IRET back), call
  gates (with the inner-stack switch and parameter copy), hardware task switches (TSS 16/32,
  task gates in the GDT and IDT, nested tasks and IRET, busy bits), the TSS I/O permission
  bitmap, A20M# (the A20 gate), and MMX. The generic devices a PC BIOS drives are there to
  compose: the 8237 DMA controllers, an 82077 floppy controller, an IDE controller (PIO, CHS,
  48-bit, ATAPI, bus-master DMA; its drive side `ata_core.c` is shared with AHCI), a VGA
  (registers, planar memory and the read/write modes, text and graphics rendering for the
  host), a parallel port, and PCI expansion ROMs for option ROMs. `x86_64-test` (tests only)
  wires them as options; `x86_64-ref` boots a legacy BIOS image (`--image bios=`: at 4 GiB, its last 128 KiB below 1
  MiB). A board emulator adds its own chipset (shadow RAM, PAM, the board's I/O) out of tree.
- **ARM state** (`isa/arm`, `uam/isa/arm.h`): Thumb state is bit 0 of the pc, the ARM
  convention for code addresses, so the translation cache, interrupted blocks and ELF entry
  points need nothing ARM-specific, and interworking branches stay plain IR (`BX` compiles and
  chains like any computed exit). PC hooks, coverage sites and fuzz boundaries in Thumb code
  name the pc with bit 0 set; the gdb stub and the suzzer importer (suzzer marks Thumb sites
  by their 2-byte length) translate. IT blocks are continuations (`UAM_IMARK_CONT`) so no
  interrupt splits one; only an exception return into the middle of one leaves IT state for
  the next block, which `cpu->priv` then carries (`uam-isa.md` section 10b). Byte order is
  BE8: code is always little-endian, data follows CPSR.E (also part of `cpu->priv`, so code
  lifted for one order never runs in the other; SETEND, exception entry from SCTLR.EE and
  return from SPSR switch it), and `uam_isa_armeb` resets big-endian. Not modelled: VFP/NEON,
  Jazelle, and the security and virtualization extensions (those instructions take Undefined
  Instruction).
- **RISC-V** (`isa/riscv`, `uam/isa/riscv.h`): RV32IMAC with Zicsr and Zifencei, M-mode only (PMP
  CSRs read as zero); the CPU's interrupt inputs are the mip bits.
- **MIPS32** (`isa/mips32`, `uam/isa/mips32.h`): release 2, as `uam_mips_configure`'s Config
  registers select: a TLB or a fixed mapping, MIPS16e or microMIPS, the FPU, the DSP ASE, GPR
  shadow sets, vectored and external-controller interrupts (`uam_mips_eic_input`).
- **x86-64** (`isa/x86`, `uam/isa/x86.h`): resets to real mode at 0xFFFFFFF0, and I/O ports are
  bus accesses at `UAM_X86_PIO_BASE`. Lines: INTR, NMI, SMI, INIT, A20M. SMM has the Intel 64 and
  AMD64 save-state maps per processor (`uam_x86_set_smm_map`), SMBASE relocation, auto-HALT and
  I/O restart; RSM checks the saved state and shuts down on one no processor could be in; an
  SMI is held in wait-for-SIPI, and SMI or NMI ends shutdown. A processor in SMM presents an
  access to an SMRAM window at `UAM_X86_SMM_VIEW` + address, where the board maps SMRAM, so each
  processor decodes by its own state and nothing switches on entry or exit. Fetches always use
  the view; data takes the normal decode where the window is closed
  (`UAM_X86_SMRAM_DATA_CLOSED`: D_CLS, AClose). Windows come from `uam_x86_set_smram_window` and,
  on AMD, the ASeg while AValid. The core enforces SMRR and AMD's SMM_ADDR/SMM_MASK (TSeg,
  TClose, HWCR.SMMLOCK; `uam_x86_preset_amd_smm`), sending outside accesses to
  `UAM_X86_SMRR_SINK`. Code-fetch checks: Intel's SMM_Code_Chk_En (`UAM_X86_FEAT_SMM_CODE_CHK`)
  and `uam_x86_set_smm_confine`, a machine check on an SMM fetch outside SMRAM.
  MSR_SMM_BLOCKED: `UAM_X86_FEAT_SMM_BLOCKED` with `uam_x86_set_package`.
- **PowerPC** (`isa/ppc32`, `uam/isa/ppc.h`): one 32-bit user instruction set with the FPU,
  big-endian, and two privileged architectures the CPU model selects per hart. The classic one
  (6xx/7xx/74xx, e300/e600): MSR with IR/DR, vectors at 0 or 0xfff00000, the decrementer, BATs,
  segment registers and the hashed page table. Book E (Power ISA III-E: e500/e200/440 class):
  IVPR/IVORs, the base, critical, machine-check and debug save/restore pairs (MCSRR and DSRR as
  the model says), ESR and DEAR, TCR/TSR with the auto-reloading decrementer, the fixed-interval
  timer and the watchdog, DBCR0 debug events (instruction complete, branch taken, trap), and
  TLB arrays searched with the PIDs and address space, managed through the MAS registers. What
  differs between implementations stays outside ember: a board chooses the CPU model
  (`uam_ppc_configure`: PVR, FPU, isel, the cache block dcbz clears, the time base rate; classic:
  4 or 8 BAT pairs, the reset prefix; Book E: the TLB arrays' geometry and page sizes, PIDs, the
  MAS size encoding, the machine-check and debug pairs, the timer bits, the boot TLB entry) and
  models the implementation registers with `uam_ppc_set_spr_ops` (an SPR nobody claims is plain
  storage); on Book E also the DCR bus (`uam_ppc_set_dcr_ops`), a TLB interface other than
  MAS (`uam_ppc_set_tlb_ops` over `uam_ppc_tlb_read/write/find`, for a 440-style tlbwe), and
  what the watchdog's reset does (`uam_ppc_set_reset_sink`). The external interrupt pin has
  eight wired-OR inputs; an interrupt controller (OpenPIC, a SoC's own) is the board's.
  `cpu->priv` carries MSR[PR], MSR[FP] and the trace (Book E: debug event) enables, so
  privileged and FP-unavailable checks are made when a block is lifted. Like a hardware TLB,
  successful translations are kept until the guest invalidates them (tlbie/tlbivax reach every
  processor on the bus). MSR[POW] (Book E: MSR[WE]) sleeps until an interrupt the processor can
  take. A Book E timer has a scheduler event only while its interrupt or reset could happen.
  Not modelled: Book E little-endian pages (E), debug address compares, the 40x and 8xx
  privileged architectures, AltiVec and SPE.
- **8-bit AVR** (`isa/avr8`, `uam/isa/avr.h`): one descriptor for every core family of the AVR
  Instruction Set Manual; the CPU model (`uam_avr_configure`) names the family (AVRe, AVRe+,
  AVRxm, AVRxt, AVRrc), the optional instructions, the program memory size (above 128 KiB
  the PC has 22 bits and return addresses three bytes), the SP at reset, the vector size and
  the boot section. The Harvard machine is laid out on the one bus as the GNU tools lay out
  ELF files: program memory at 0 (the pc is a byte address), the data space at 0x800000, which
  loads and stores reach through `isa->translate` and LPM/ELPM read past it (`PHYS`). The
  core's own I/O registers (SREG, SP, RAMPx, EIND, CCP, and on AVRxm/AVRxt the interrupt
  controller) belong to the hart, and on AVRe/AVRe+ so do data addresses 0-31, the registers:
  a constant address reaches them with GET/PUT or a system register, a pointer access splits
  at run time (uam-isa.md section 10d). Interrupt input n is vector n. AVRe/AVRe+/AVRrc take
  the lowest pending vector while SREG.I is set and clear it; AVRxm's PMIC and AVRxt's CPUINT
  are modelled in the core, because RETI ends their levels: levels and their enables,
  preemption, NMI, round robin, IVSEL, the compact vector table. A board sets each vector's
  level (`uam_avr_set_irq_level`) and may acknowledge a vector to the peripheral that raised
  it (`uam_avr_set_irq_ack`: many AVR peripherals clear their flag when the vector runs) and
  take SPM's writes (`uam_avr_set_spm`). DES runs one DES round per instruction. An AVR has
  no exceptions: an undefined opcode stops into the debugger (TRAP DEBUG, which the fuzz
  harness counts as a crash at that pc), and an access nothing answers halts the core with
  a message. Not modelled: the instruction an AVRe core runs after RETI before the next
  interrupt (after SEI it does run one), configuration change protection, the watchdog,
  fuses, and instruction timing.
- **AVR32** (`isa/avr32`, `uam/isa/avr32.h`): one big-endian descriptor for both
  microarchitectures of the AVR32 Architecture Manual, the CPU model (`uam_avr32_configure`)
  choosing which, the MPU regions or TLB sizes, the INT3 shadow registers and SIMD. The whole
  instruction set is lifted, 16- and 32-bit forms (a decoder generated from the opcode
  table, checked against the AVR32 binutils' disassembler), DSP and saturating arithmetic
  with Q through helpers, and on AVR32B the SIMD instructions. The system side: SR and the
  eight execution modes with their SP banking; events and interrupts as the manual has them
  for each class (AVR32A: SR and PC, and for interrupts R8-R12 and LR, on the system stack;
  AVR32B: RSR/RAR per context and the INT3 shadow), handlers at EVBA plus an offset, for
  interrupts the autovector the board's interrupt controller answers
  (`uam_avr32_set_autovector`); scall/rets/rete/retd, the breakpoint into debug mode, the
  system registers, CONFIG0/1, COUNT/COMPARE, whose request is an output line of the CPU
  (`uam_avr32_compare_line`) that the board routes to its interrupt controller; the AVR32A
  MPU (regions, subregions, access permissions) and the AVR32B segments P0-P4 with an ITLB
  and a DTLB (tlbr/tlbw/tlbs) through `isa->translate`. The interrupt controller
  (`uam/dev/avr32_intc.h`) is a generic one: groups of request lines with a level and an
  autovector each. Not modelled: caches (`cache` does nothing), the Java and secure states
  (their instructions raise the events), coprocessors (coprocessor-absent), the on-chip debug
  registers; TLB Accessed bits are set by tlbw, not by translation (which has no side effects).
- **8-bit PIC** (`isa/pic8`, `uam/isa/pic.h`): one descriptor for the baseline (12-bit),
  mid-range and enhanced mid-range (14-bit) and PIC18 (16-bit) instruction sets; the CPU
  model (`uam_pic_configure`) names the family, the program memory size, the baseline's banks
  and the configuration options XINST (PIC18's extended instruction set and indexed literal
  offset addressing) and STVREN. Program memory is at bus 0 as the PIC tools' hex files lay
  it out (two bytes per 12/14-bit word; the pc is that byte address), the data space at
  0x800000 through `isa->translate`. The core's registers at their fixed addresses belong to
  the hart (INDF, PCL, STATUS, FSR, PCLATH, INTCON; by family BSR, WREG, STKPTR, TOS, the
  enhanced shadow registers, TBLPTR, TABLAT, PROD, PCLATU, RCON), everything else is the
  board's; banked and indirect operands split at run time (uam-isa.md section 10f), and the
  enhanced core maps its FSRs over the linear view of the GPRs and over program memory. The
  hardware stack (2, 8 or 16 levels, PIC18 31 with STKFUL/STKUNF) is hart state, in the gdb
  view and snapshots; with STVREN its overflow and underflow, and the RESET instruction,
  reset the core and call the board's hook (`uam_pic_set_reset_hook`), which on `pic-ref`
  resets the board. Interrupts: the core holds GIE/PEIE (PIC18 with IPEN GIEH/GIEL, two
  vectors, high preempting low) and INTCON's three enable/flag pairs (`uam_pic_intcon_set`);
  every other source is a peripheral whose flag and enable are the board's, which drives the
  peripheral line(s). Entry saves the enhanced core's context in its shadow registers and
  PIC18's W/STATUS/BSR in the fast register stack. TBLWT reaches the board's flash
  controller (`uam_pic_set_tblwt`). A PIC has no exceptions: an undefined opcode stops into
  the debugger, an access nothing answers halts the core with a message. Not modelled: the
  watchdog (CLRWDT and SLEEP set TO and PD only), the priorities of INTCON's own sources on
  PIC18 (INTCON2's TMR0IP/RBIP: high here), the vectored interrupt controllers and 14-bit
  data addresses of the newest PIC18 parts, the enhanced baseline (PIC12F529-class), the
  debug-executive opcodes, instruction timing.
- **8051** (`isa/mcs51`, `uam/isa/mcs51.h`): one descriptor for every core running the MCS-51
  instruction set. What derivatives share is the ISA module (the instruction set, the four
  address spaces, the register banks, the core SFRs ACC, B, PSW, SP, DPL/DPH, IE, IP, PCON);
  how cores differ is the CPU model (`uam_mcs51_configure`: internal RAM size, interrupt
  sources and their extended enable/priority registers, IPH for four levels, a second data
  pointer, the SFR that pages MOVX @Ri, SFR paging, code banking), with presets for the 8051,
  the 8052 and a CIP-51-style core; and every other SFR is a peripheral the board maps, so a
  part with two UARTs or a CAN controller is a board, not another ISA. Code is at bus 0,
  external data and the SFRs at their own bus bases, the internal RAM in the hart (direct
  addresses become GET/PUT, indirect ones helpers). Blocks are translated for one register
  bank and data pointer (`cpu->priv`), so R0-R7 and DPTR are fixed offsets; a PSW or DPS write
  ends the block (uam-isa.md section 10g). Interrupt input n is source n, vector 0x03 + 8n;
  levels in progress end with RETI, a higher level preempts, the lowest source wins a tie;
  with SFR paging and automatic paging on (`uam_mcs51_set_sfr_autopage`, off at reset) the core
  saves the page on entry and restores it on RETI. Code banking maps
  the window above `bank_from` onto flash blocks through `isa->translate` (fetch and MOVC
  banks apart). The 8051's own peripherals are generic devices (`uam/dev/mcs51_timer.h`: Timer
  0/1 in modes 0-3 with TCON's external interrupts; `mcs51_serial.h`: SCON/SBUF;
  `mcs51_port.h`: ports and plain SFR registers). An 8051 has no exceptions: 0xA5 (undefined)
  stops into the debugger, an access nothing answers halts the core. Not modelled: the
  instruction after RETI before the next interrupt (after an IE/IP write one runs), power-down
  (it idles like IDL), instruction timing (one tick per instruction) and Timer 2.
- **6502** (`isa/m6502`, `uam/isa/m6502.h`): one descriptor for the cores running the 6502
  instruction set. What they share is the ISA module (the registers, the one 64 KiB space with
  the zero page and the stack in page 1, the addressing modes, the vectors); how they differ
  is the CPU model (`uam_m6502_configure`): NMOS or CMOS (the 65C02's instructions and modes,
  its undefined opcodes as NOPs, the JMP (abs) fix, valid decimal N and Z, D cleared on
  interrupts, one store per read-modify-write), decimal mode (off on the 2A03), the Rockwell
  and WDC bit operations, WAI and STP, and the NMOS's XAA/LXA constant. The NMOS model has the
  undefined opcodes: the combined instructions, NOPs with operands, the JAMs and the unstable
  ones, and stores the byte it read before the result of a read-modify-write. Presets are the
  NMOS 6502, the 2A03, the WDC W65C02S and the Rockwell R65C02; everything else is the board's
  (a 6502 has no on-chip peripheral). Address a is bus address a. Blocks are translated for one
  value of D on models with decimal mode (`cpu->priv`), so ADC and SBC are binary or decimal
  at lift time (decimal through pure helpers); SED, CLD and PLP end the block (uam-isa.md
  section 10h). Input lines 0-29 are wired-OR into IRQ, line 30 is the edge-triggered NMI and
  31 set overflow; as on the chip, the interrupt poll after CLI, SEI and PLP sees I as it was
  (one more instruction runs after CLI; an IRQ pending at SEI is still taken). WAI waits for a
  request, STP and the NMOS JAMs stop the core until reset. A 6502 has no exceptions: an
  access nothing answers halts the core. Not modelled: instruction timing (one tick per
  instruction), the dummy reads of indexed modes, an NMI hijacking BRK, and the branch that
  delays an interrupt by one instruction.
- **Z80** (`isa/z80`, `uam/isa/z80.h`): the whole Zilog Z80, documented and undocumented
  (IXH/IXL/IYH/IYL, SLL, the DDCB/FDCB forms that copy their result to a register, prefix
  chains, the ED no-ops, flag bits 3 and 5 everywhere, MEMPTR, Q for SCF/CCF, R). The CPU
  model (`uam_z80_configure`) is NMOS or CMOS, which differ in what OUT (C),0 writes; the
  struct leaves room for relatives (8080, Z180) as models. Memory is at bus 0 and the I/O
  ports at their own bus base, with the 16-bit port address the Z80 puts out (A or B on the
  high byte) masked by the board's decoding (`uam_z80_set_io_decode`). /INT is taken in IM 0
  (RST p or CALL nn from the board's acknowledge hook), IM 1 or IM 2 (a vector from the hook
  through I), never directly after EI; /NMI edges call 0x66; RETI is reported to the board,
  as Z80 peripherals watch for it. The registers are the pairs as little-endian words, F as
  the Z80 has it; every instruction queues its writes behind its accesses, adds its opcode
  fetches to R and records whether it set the flags (Q) (uam-isa.md section 10i). The Z80 CTC
  (`uam/dev/z80_ctc.h`) is the device that answers the acknowledge with a vector and ends its
  interrupt at RETI. A Z80 has no exceptions: an access nothing answers halts the core. Not
  modelled: instruction timing (one tick per instruction), R while HALT waits, IM 0 with an
  instruction other than RST or CALL on the bus, and the NMOS LD A,I/R IFF2 race.
- **Xtensa** (`isa/xtensa`, `uam/isa/xtensa.h`): a configurable core, two descriptors (little-
  and big-endian; a big-endian core reverses the fields of each instruction format, which the
  decoder puts back first). The CPU model (`uam_xtensa_config`) lists the options, the
  interrupt table (type and level per interrupt), the levels, the vectors and the region
  attributes' rights; the models are the cores of the ESP8266 (LX106: CALL0 only, Extended
  L32R, alignment exceptions), the ESP32 (LX6) and the ESP32-S3 (LX7), from their published
  configuration headers. The decoder is generated from an opcode table checked against the GNU
  assemblers and disassemblers of those cores. Register windows keep the current window in
  the register file (rotations swap it), with the overflow check before each instruction that
  names a4-a15; zero-overhead loops are a lift-time loop end checked on block entry; the
  XEA2 exceptions, the Interrupt and High-Priority Interrupt options (levels, PS.EXCM, NMI,
  RFI), CCOUNT/CCOMPARE in virtual time, Region Protection as translation, MAC16, booleans,
  S32C1I, and the single-precision FPU (in software, with its divide and square-root step
  instructions) are modelled (uam-isa.md section 10j). Not modelled: designer-defined TIE
  instructions (the ESP32-S3's AI extension, the ESP32's double-precision helpers), the cache
  and OCD instructions, IBREAK/DBREAK/ICOUNT debug exceptions, and timing (one tick per
  instruction).

## 6. Analysis and fuzzing

- **Deterministic by default**: the clock is instruction count, all harts run on one host
  thread, and host time is never read unless real-time pacing is chosen. Every run is bounded
  in virtual time: an instruction budget, a virtual-time limit, and a stall count. After
  `max_stall` timer events with no instruction retired, a waiting hart stops skipping time and
  blocks on host input, or the run ends as a livelock if no input source exists. Each of these
  decisions is made in virtual time.
- **Hosts**: nothing a guest sees depends on the machine ember runs on. The interpreter is
  plain C11. The JIT is one host-independent core (`src/exec/jit.c`: the runtime context and C
  bridges, the deferred-write, cold-op, register-allocation and flag-fusion analyses, the code
  arena, linking, profiling) and a code generator per host instruction set, behind
  `src/exec/jit_impl.h`: `jit_x86.c` (x86-64) and `jit_a64.c` (little-endian AArch64), which emit
  the same block layout, frame discipline and chaining protocol; `test_jitdiff` holds each to
  the interpreter. Elsewhere `--jit` falls back to the interpreter. The arena is mapped
  read-write-execute on Linux; on macOS it is a `MAP_JIT` mapping that the core opens for
  writing only while it places a block or links a slot, and AArch64 code never touches `x18`.
  The x87 model keeps its registers in the 80-bit format and computes on them in software
  (`isa/x86/x86_f80.c`, bit-exact against a real x87 in `test_f80`), so x86 guests get the
  same answers on every host; only the transcendentals use the host's `long double`.
- **JIT block chaining** (`src/exec/jit.h`): a compiled block whose EXIT leads to another compiled
  block jumps straight into it instead of returning to the run loop, for as long as the run loop
  would have nothing to do in between: no exit request, deadline not reached, links current.
  Each EXIT owns a link slot after its code; a slot is linked when the run loop next runs the
  block the EXIT went to. An EXIT to a constant pc ends in a direct jump that linking patches to
  the next block's body; the JIT lists the jumps it has patched and unlinks them all when the
  epoch moves. A computed EXIT's slot holds its first target of the epoch, and its
  other targets (a switch, a return to many callers) go through a jump table shared by all
  blocks, indexed by a hash of the target pc and linked the same way. Links belong to an epoch that
  moves on whenever a linked block may be invalid (a block freed or a code page written, a
  translation or privilege change). Interrupts are still taken only by the run loop, so an ISA
  must request an exit on anything that can make `irq_take` newly succeed (`isa.h`). Chained runs
  are bit-identical to unchained ones and to the interpreter; `--no-chain` turns it off. Native
  code counts instructions in a register against the point where it must stop, and rereads that
  point after every call it makes into C, so a stop or exit requested by a device, a helper or a
  hook takes effect at the next instruction, as on the interpreter. A stop requested from a
  signal handler (`uam_exec_stop`) is seen when native code next returns to the run loop: within
  the host poll interval, or 4096 blocks without one.
- **Stores next to code** (JIT): the bus records which 64-byte lines of a page hold translated
  code, and only a write to such a line drops that code. The JIT's data-TLB keeps a page that
  holds code in its write set under a tag the inline probe cannot match; a store that misses
  there calls a shared stub that reads the page's line mask and stores inline if none of its
  bytes is in a code line, else takes the bus path (`uam_bus_page_host_code`). Firmware that
  keeps data on its code pages stores at full speed, and self-modifying code is still seen.
- **Spin loops** (`src/exec/spin.h`): a block that jumps back to itself and does nothing but read
  memory, compute and write registers (a guest waiting for a flag) leaves the machine as it was
  once one pass has run, until a timer, an exit request or a host poll, which only the run loop
  delivers. Before running such a block the run loop evaluates one pass without effect, on a
  copy of the register file; if that pass would leave the register file unchanged, read only
  RAM or ROM and jump back to the block, it adds every whole pass before the deadline and the
  next host poll to icount (and the block counts) at once, and goes round again. Which blocks
  qualify is decided from the IR alone: no ISA, board or address is involved. A loop that
  counts, stores, reads a device or a system register, calls a helper, or is hooked, traced or
  watched runs pass by pass. Both backends skip alike, and a run that skips ends in exactly the
  state, time, polls and interrupts of one that does not; `UAM_SPIN_SKIP=0` turns it off.
- **Snapshots** (`snapshot.h`): every hart's state and ISA state (with several processors, also
  where their interleaving stands: a run that stops mid-slice resumes there, so split and restored
  runs interleave exactly as one run), every RAM/ROM/ROMD backing store, virtual
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
  (`uam-isa.md` §7.5): `idx = (A ^ prev) & mask; saturating map[idx]++; prev = B` at depth 1 without rotation, the
  classic edge (`uam_cov_state` folds up to seven earlier values for N-grams). The site
  table (guest virtual pc, A, B) is the caller's coverage scheme; block mode puts a site at
  every block start. Coverage has its own opcode rather than a `HELPER` call so that the JIT
  emits it inline (about ten host instructions, no call, chained blocks count too) and so that
  it provably touches nothing but the map: guest state, `icount` and timing are the same with
  coverage on, and the interpreter and the JIT produce byte-identical maps. With coverage off
  no block contains COV. The map is never part of a snapshot. Sites in delay slots cannot be
  instrumented (translation fails, naming the pc). Optionally a site counts only when another
  counted since (`skip_repeat`), the emit stream of a trap-and-rearm coverage runtime.
- **Exception observer** (`uam_exec_set_exc_hook`): sees each synchronous exception before
  the ISA takes it, with precise state, and may stop the run (`UAM_STOP_EXC_HOOK`) instead of
  delivering it. One slot (the fuzz harness owns it). Interrupts and resets are not reported.
- **Guards** (`guard.h`, `--guard`/`--watch`): byte ranges of guest memory, virtual or
  physical, whose data accesses (read, write or both) are refused and stop the run at the
  faulting instruction (`UAM_STOP_GUARD`, resumable: the instruction runs again), refused as a
  bus fault the guest takes, or allowed and logged. A guard is one bus watch per physically
  contiguous piece (`UAM_WATCH_FAULT` for the refusing actions); a virtual range is translated
  as processor 0 maps its data accesses when the guard is added (the AVR and PIC data space,
  not their program memory). The precise stop comes from the executor: the
  watch callback requests it (`uam_exec_request_fault_stop`), the access faults, and the run
  loop returns at the fault's pc instead of delivering the exception, the same on the
  interpreter and the JIT and on any processor. Instruction fetches, debug accesses and DMA are
  exempt; a guarded page leaves the JIT's direct memory access. Guards are configuration, not
  snapshot state. Every hit is recorded and goes to one observer; the fuzz harness's redzones
  are STOP guards.
- **Fuzz harness** (`fuzz.h`, [`fuzzing.md`](fuzzing.md)): boot to a begin pc, snapshot,
  and run each input from the snapshot through a named route (guest memory, MMIO `input`
  rules, or an input a board published such as a UART FIFO), with paired or begin-only
  iteration boundaries. Verdicts come from crash pcs, the exception observer filtered by
  exception class, interrupts from chosen lines, resets, guards (the route's redzones around
  its buffer and any others), and the executor's stop reasons (hangs, the per-input budget);
  an internal error stops the campaign instead. Coverage sites at the boundaries are
  counted by the boundary hooks, in the map they close. Frontends sit behind
  `uam_fuzz_frontend`; the replay frontend runs saved inputs. Observers
  (`uam_fuzz_set_observer`) see every published map, for side channels.
- **suzzer interoperation** (`fuzz/suzzer/`, built only with a suzzer checkout,
  [`suzzer.md`](suzzer.md)): LCV3 site lists and LCVB1 boundary files in, the SZR1 executor
  (`--fuzz-szr`) and LCVS export (`--cov-shm`, an observer) out. It reaches the runner
  through the generic runner-extension interface (`runner.h`), so outside `fuzz/suzzer/` only
  `fuzz/stock_exts.c` adds its extension, and only when a checkout is configured.
- **Planned** (phase 3, on top of snapshots):
  - More passes: cmp-logging, memory hooks, taint (`HELPER` calls, which such passes would
    insert, compile in the JIT like every other IR op).
  - More crash checks: execution outside code regions, an icount watchdog per function.

  PC hooks, watchpoints, guards (`guard.h`) and `uam_exec_stop` are already available to
  build oracles by hand.
- **GDB stub** against the abstract state (`--gdb PORT`): breakpoints are PC hooks, watchpoints
  are bus watches, registers come from `isa->gdb` (per-register widths for mixed files: the
  x86 view is gdb's whole amd64 core set, x87 included). Every processor is a thread in
  all-stop mode; a stop names the processor that hit the breakpoint or watchpoint, and
  registers and steps follow the selected thread. A guard's stop is reported as SIGSEGV on
  the processor that made the access.
- **Host I/O** is outside the machine: devices use `chardev.h` / `netdev.h`; backends (stdio,
  pty, sockets, tap, tun, UDP, in-process pairs, pcap capture) and the host loop live in `src/host`.
  Virtual-time pacing (the default) keeps runs deterministic; an idle hart skips straight to its
  next timer, and host input is looked at every 64 skips. Real-time pacing is opt-in, and
  implied by the runner when a port is attached to a host network interface. The runner holds
  console input until the run starts, so fixed input (`string:`, `in=FILE`) is not lost to the
  board's power-on reset or to a snapshot restore. Fixed input can also wait for the guest's
  output (`,after=PATTERN`, `script:FILE` expect/send steps): the backend watches what the guest
  writes and queues the next input when it matches, so a boot that polls the console or clears
  its FIFO cannot eat input meant for a prompt. Devices take no part; it works on any frontend.
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
4. Board description format, more peripherals, ARMv7-M, JIT (*done*: x86-64 and AArch64 hosts, with block chaining; every IR op compiles, so no block falls back to the interpreter).

Done outside the plan: MIPS32 (CP0, TLB, both byte orders), ARMv7-A (ARM and Thumb-2, CP15,
short-descriptor MMU, generic timer, a GIC, little-endian and BE8), x86-64, 32-bit PowerPC (FPU,
OEA MMU, decrementer; and Book E), 8-bit AVR, AVR32, 8-bit PIC, PIC32, the 8051, the 6502,
the Z80, Xtensa, host networking, a gdb stub, the library build with
an ABI handshake. ARMv7-M (the M-profile NVIC exception model) is still planned.

## 8. Verification

Implemented (`make test`):

- Unit tests: IR semantics, verifier and printer (`test_ir`); bus, resets, hooks, time and
  livelock bounds, the ABI handshake (`test_core`); host backends, the host loop and console
  holding (`test_host`); state streams and snapshots under both backends (`test_snapshot`).
- The IR verifier runs on every translated block (on by default; `--no-verify` skips it), before
  and after the optimizer.
- Also in `make test`: the x86 self-tests (`selftest.asm`, `smm-amd.asm`), the 6502 self-test,
  PIC32 with its FPU and DSP vectors, `test_xtensa_fpu`, the console-script test and the MIPS
  decoder checks. `make net-check` is separate.
- Firmware self-tests: bare-metal RV32, MIPS32 (both byte orders), ARMv7 (built as ARM and
  as Thumb-2 code, little-endian and BE8) and 32-bit PowerPC (integer, CR, branches, every
  load/store form, exceptions, problem state, decrementer, the MMU, the FPU and its FPSCR,
  trace) programs that check themselves and report through the test finisher, run
  on the interpreter and on the JIT; the 8-bit AVR self-test on each core family (every flag
  the ALU instructions define against vectors from the manual's equations, the multiplies,
  every pointer addressing mode, LPM/ELPM, skips over two-word instructions, the core's
  registers through the data space, the timer interrupt with SLEEP, XCH/LAS/LAC/LAT and DES
  against the FIPS 46 example on AVRxm); the AVR32 self-test on AVR32A and AVR32B (the ALU and
  wide arithmetic, every load/store form, bit operations, multiply/divide, atomics, conditional
  execution, DSP, SIMD on B, breakpoint, scall, user mode, exceptions, CONFIG, the COMPARE
  interrupt and priorities, the MPU or MMU), built when an AVR32 toolchain is found; the 8-bit
  PIC self-tests on each core family (every flag the ALU instructions define against vectors
  from the data sheets' definitions, banks, every indirect mode, the stack, computed gotos,
  table reads, interrupts and their context saves, RESET and stack resets) and on PIC18's
  extended instructions, built when gputils is found; the 8051
  self-test on each core model (ADD/ADDC/SUBB/DA/MUL/DIV/RLC/RRC/CJNE flags against vectors
  worked bit by bit from the instruction set's definitions, register banks, the stack and
  calls, every bit instruction, MOVC/MOVX, Timer 0/1 modes, interrupts with priorities and
  idle, and per model the upper RAM, dual DPTR and four levels, or SFR pages and code banks;
  every opcode's length checked against as31); the Z80 self-test on each core model (every
  ALU, INC/DEC, DAA, rotate, CB and 16-bit arithmetic flag, bits 3 and 5 included, and the
  block instructions' flags against vectors from a model of the instruction set, the index
  registers and prefix chains, R, IFF2, Q, MEMPTR, I/O, interrupts in IM 1 and IM 2 from the
  CTC, the instruction after EI, HALT); the Xtensa self-test on each model, little- and
  big-endian (the computational instructions against vectors from a model of their
  definitions, loads and stores, branches, register windows with overflow and underflow
  through the manual's handlers, loops including a loop end in code translated before the
  loop, booleans, MAC16, the FPU, S32C1I, every exception and interrupt level, the NMI,
  Region Protection), and its decoder against the GNU disassemblers; and a multiprocessor self-test on four processors of
  RISC-V, MIPS, ARM, PowerPC and x86 (`tests/smp/`, `tests/firmware-x86/smp.asm`), also split by snapshots;
  `tests/firmware-x86/legacy.asm` (task switches, gates, the I/O bitmap, virtual-8086 mode,
  A20, MMX) and `biosrom.asm`, a BIOS-style ROM from the reset vector through VGA, floppy (DMA),
  IDE and the printer into a boot sector (`bios-check.py`).
- Multiprocessor observers (`test_smp`): machine-wide hooks and the exception observer see every
  processor, per-processor coverage history gives the same map whatever the slicing, and the
  fuzz harness reports the processor that ended an iteration. `tests/gdb/gdb-check.py` drives
  the gdb stub over the remote protocol on four processors.
- Guards: `test_core` parses the text form and runs MIPS guests under the interpreter and the
  JIT: a stop at the exact store with the store refused, debug and DMA accesses let through,
  removal, a virtual range in front of an address, reads passing a write-only guard, an access
  that starts before a guard, the fault action through the exception observer, logging with
  the value (one bus watch for a contiguous range), and the errors (an unmapped virtual range,
  an empty range or direction). `test_fuzz` checks the `mem:` route's redzones (in front, past
  the input and past the buffer, read and write, length fields of either byte order) on every
  fuzz target and backend; `tests/runner/runner-check.sh` checks `--guard`/`--watch` on the x86
  self-test, with the interpreter and the JIT stopping at the same instruction.
- Runner options: `tests/runner/runner-check.sh` (`make runner-test`) checks `--log-pc`,
  `--break`, `--dump-mem`, `--trace-after` (absolute and `+N`, across a snapshot) and block
  traces with `--jit`; `tests/runner/trace-check.sh` (part of `make runner-test`) checks the
  bring-up traces on the RISC-V self-test and on small polling images: every `mmio`,
  `sysreg`, `irq` and `console` line identical under the interpreter, the JIT and the JIT
  without chaining; `polls` finding the bit a loop waits for (and the whole value an
  equality test waits for), `waiting:`/`done:` lines and the stop summary; the
  instruction named by the `unknown-mmio` first-access line; `--log-stamp`; nothing extra
  printed without `--trace`.
- Snapshot determinism: `tests/snapshot/snapshot-check.sh` splits a run with `--save-snapshot`
  / `--load-snapshot` at several points and requires the same console output, pc and icount as
  the uninterrupted run, with the interpreter and the JIT on either side.
- MMIO rules: `test_mmio` covers parsing, every policy, masks, narrow accesses on a big-endian
  CPU, priorities against real devices, catch-alls and holes, the input stream, snapshots, and a
  guest reading a rule through the interpreter and the JIT.
- Spin loops: `test_spin` runs RV32 and MIPS guests that wait on a word a device timer changes
  (on MIPS with a timer interrupt that logs the waiting registers), count down, poll a device
  register and store while waiting, under both backends with and without skipping, with and
  without host polling and over many short runs, and requires identical state, polls,
  interrupts, device reads and block counts.
- Stores next to code: `test_codestore` runs an RV32 guest that stores to data lines of its
  own code page and rewrites a function in another line of it (alternately by a misaligned word
  that ends in the function's line and a byte store into it), across a snapshot restore, under
  the interpreter and the JIT, and requires identical state and inline stores.
- Chaining: `test_jit` runs one guest (calls, returns, an eight-way computed call whose targets
  it rewrites, branches, self-modifying code, DI/EI with a timer, MMIO) through the interpreter and the JIT with and without chaining, with host
  code patches, PC hooks and snapshot restores in between, and requires identical state (also
  with a two-entry jump table, `UAM_JIT_JTAB_BITS=1`, so its links collide); and
  code rewritten on a page that has just become a code page again must run as rewritten. A stop
  requested from a signal handler must end a loop that never calls out of native code. With a
  small code arena (`UAM_JIT_ARENA=bytes`, default 64 MiB) recycled some 70 times while blocks are
  linked, a run must end as on the interpreter; the JIT stops the process rather than patch a
  jump that is not one of the arena's current code.
- Coverage: `test_cov` covers `UAM_OP_COV` (verifier, printer, optimizer, saturation, prev
  chaining), passes (no-op and counting passes change nothing; failing or invalid ones stop
  the run), expected hit counts and edge buckets for a known loop on RV32, MIPS32 BE/LE and
  ARMv7 (ARM and Thumb-2), 32-bit PowerPC, 8-bit AVR, AVR32, PIC18, the 8051, the 6502, the
  Z80 and Xtensa under the interpreter and the JIT with and without chaining, block mode, rejected sites,
  coverage off, and the exception observer on an illegal instruction on each ISA that has
  exceptions.
  `tests/cov/cov-check.sh` runs the self-tests with block coverage on every backend and
  requires identical maps and an outcome identical to the run without coverage.
- AVR (`test_avr`): interrupt entry on each core family (the vector, the return address on
  the stack, SREG.I, the vector size, the acknowledge), the AVRxm PMIC (level enables,
  preemption, NMI, round robin, IVSEL, RETI ending the highest level) and the AVRxt CPUINT
  (LVL1VEC, the compact vector table), the wake rule, the CPU models, and the
  general-purpose timer. The decoder was checked against `avr-objdump` on all 65536 first
  words for every model (the same instructions, the same lengths).
- One unit test per ISA (`tests/unit/test_<arch>.c`) for what the self-test firmware cannot
  reach on its reference board, on a bare machine: `test_riscv` (reset state, which CSRs exist and their WARL
  bits, trap entry and mtval per fault, vectored interrupts and priority, WFI, the CLINT and
  PLIC through their registers, two harts on `rv32-ref`), `test_mips` (the models and reset
  state, CP0 writable bits, every vector including BEV, EBase and the refill vector under EXL,
  BD in delay slots, compatibility, vectored and EIC interrupts with shadow sets, Count/Compare,
  the TLB and fixed mapping, MIPS16e and microMIPS modes, both byte orders), `test_arm` (the
  banked registers of every mode, exception entry in ARM and Thumb state, VBAR/SCTLR.V/TE/EE,
  IRQ and FIQ, WFI, the generic timer, translation-table formats the self-test does not build,
  BE8, the GIC on `arm-ref`), `test_x86` (reset and INIT/SIPI, the interrupt shadow, HLT, real,
  32- and 64-bit delivery with error codes and double faults, #PF and CR2, ring 3, SMM on the
  Intel and AMD save maps, AMD's TSeg/TClose and SMMLOCK, the SMM code-fetch check and
  confinement (machine check, MCIP), RSM's saved-state checks, the SMI held in wait-for-SIPI
  and MSR_SMM_BLOCKED, SMI and NMI ending shutdown, CR4 bits from CPUID and CR4.DE, the SMM
  view - SMRAM windows, D_CLS/AClose, the AMD ASeg, two processors decoding differently at
  once -, MSRs, CPUID, A20M#), `test_avr32` (the models, interrupt levels,
  NMI and their masks, what A stacks and B saves in RSR/RAR, the INT3 shadow bank, rete, SLEEP,
  COUNT/COMPARE, the MPU and the MMU, the interrupt controller on `avr32-ref`), `test_pic`,
  `test_mcs51`, `test_m6502`, `test_z80` and `test_xtensa` (special registers per model, one
  privilege ring, interrupt lines and levels, CCOMPARE, window exceptions, faults, BREAK, the
  ERI, big-endian layout). The six newest (RISC-V, MIPS, ARM, x86, AVR32, Xtensa) run their
  programs interpreted and under the JIT and also check the debugger view and a snapshot of
  the ISA state.
- PowerPC board interface (`test_ppc`): the soft-reset and machine-check pins and checkstop,
  the wired-OR external inputs, the decrementer while MSR[EE] is clear, the implementation SPR
  handlers and every CPU model option; on Book E the watchdog's reset sink and TSR[WRS], the
  extended period select, the critical input, machine checks through either pair, the DCR and
  TLB handlers and the TLB entry API. The self-test is also built for Book E
  (`-o model=booke`: the boot page, IVORs, ESR/DEAR, the timers and watchdog, TLB0/TLB1 through
  MAS with a software TLB reload, PIDs and address spaces, IPROT, debug events).
- Fuzzing: `test_fuzz` runs a hand-assembled planted-bug target on RV32, MIPS32 BE/LE, ARMv7, PowerPC
  under every backend: each verdict and its detail, identical maps and results across
  backends and across restores, paired and begin-only modes, the exception-class policy,
  routes, resets, the frontend protocol, abort, and the replay frontend.
  `tests/fuzz/fuzz-check.sh` does the same through `uam-run --fuzz-replay` on C firmware
  (`fuzztarget.h`) on every stock board but `x86_64-ref` and `xtensaeb-ref` (Thumb-2 code on
  `arm-ref`), including replaying a
  crash file on its own. `test_fuzz` targets RV32, MIPS, ARMv7 and PowerPC. `fuzz-check.sh`
  covers the rest but `x86_64-ref` and `xtensaeb-ref`: the cores without exceptions (8-bit AVR
  and PIC, 8051, 6502, Z80) skip the exception-policy step (`FUZZ_CHECK_NO_TRAPS`), and AVR32A
  needs an AVR32 toolchain.
- suzzer formats: `test_suzzer` reproduces every conformance vector of the configured suzzer (N=1..4 and 8,
  rotation off and on) byte for byte through `UAM_OP_COV` on every backend, checks the map-size rule against suzzer's table, and
  covers the LCV3/LCVB1 readers (every rejection of suzzer's reader checklist), base
  resolution, boundaries, site naming, the site-byte check and the LCVS region's ownership
  rules. `tests/suzzer/suzzer-check.sh` runs `--cov-sites`/`--cov-boundary` end to end on
  RV32, MIPS32 BE/LE, ARM (Thumb-2, LE and BE8) and PowerPC, including N=3 and N=8 with rotation, and the LCVS export read back by suzzer's
  reader; `gen-check.sh` does it with suzzer's own generators; `tests/szr/` checks the SZR1
  executor frame by frame and in a session with suzzer's brain, whose saved crash directory
  replays without its `.info` notes.
- With a suzzer checkout configured: its format header compiles on its own with ember's
  strict flags, and `test_suzzer` reads the checkout's own conformance vectors. Without one,
  the suzzer tests are skipped.
- The external-board example is built and run against the library.
- Cross-ISA benchmark: `make bench-test` builds `tests/bench/bench.c` for every reference
  board but `pic-ref` (RV32, MIPS32 BE/LE, ARM, Thumb-2 LE and BE8, PowerPC, x86-64, AVR, AVR32A
  and AVR32B with an AVR32 toolchain, PIC32 MX/MM/MZ, Xtensa LX106/LX6/LX7 with their
  toolchains, the 8051, the 6502 and 65C02, and the Z80) and natively, and
  requires each guest's checksums, under the interpreter and the JIT, to equal the host
  build's (docs/jit-perf.md, "Parity across ISAs").
- `test_jitdiff` checks the JIT against the interpreter block by block: every pure op at
  every width with temp and immediate operands, then random verified blocks (guards,
  CONT/BREAK imarks, faulting, page-crossing, MMIO and exclusive accesses, helpers,
  sysregs, traps) under every deadline and exit request, and fixed shapes the optimizer
  rewrites (overwritten register writes, flags read back, common subexpressions). Each run must agree on the result,
  registers, memory, icount and on the register file seen at every device, sysreg and helper
  access. `make jitdiff-soak` runs it at length; docs/jit-perf.md has the performance side.
  It holds whichever code generator the host has: `make test` passes on x86-64 and on
  AArch64.
- `test_f80` checks the x87's software 80-bit format: on an x86-64 host bit for bit against
  the host's x87 (arithmetic, square root, compares, conversions, FRNDINT and FISTP under
  every rounding control, FSCALE, FPREM and FPREM1) over special values and random operands
  of every class; on every host, a digest of its results must equal the one recorded on
  x86-64. The x86 self-test's x87 group (110-118) checks the instructions end to end.

Planned: riscv-tests / riscv-arch-test, golden text-form tests for the lifters.
