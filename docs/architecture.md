# UAM Firmware Emulator — Architecture

Status: phases 1 and 2 implemented, plus more ISAs (MIPS32 with a TLB, ARMv7-A with an
MMU, x86-64, 32-bit PowerPC with the classic and the Book E MMU), the snapshot part of phase 3 and the JIT from
phase 4. IR, interpreter and optimizer, an
x86-64 JIT with block chaining, bus (priorities, device byte order, ROM devices, aliases,
watchpoints, dirty-page tracking), RV32IMAC (`rv32-ref` board), MIPS32 big/little-endian
(`mips-ref` / `mipsel-ref` boards), ARMv7-A, ARM and Thumb-2, little-endian and BE8
(`arm-ref` / `armeb-ref` boards), x86-64 (`x86_64-ref`), 32-bit PowerPC (`ppc-ref`), host
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
| `isa.h` | `lift`, register descriptor table, `take_exception`, `irq_take`/`irq_wakeup`/`irq_input`, `sysreg_read`/`sysreg_write`, `default_endian` (and `fetch_endian` when code and data differ: ARM BE8), optional `translate` (v->p, with fault sub-causes), optional `gdb` register view, `save`/`restore` of ISA-private state, optional `switch_in` (multiprocessor: the platform view a processor sees, such as SMRAM, follows it). An ISA must request an exit (`uam_cpu_request_exit`) on any change that can make `irq_take` newly succeed. The `gdb` view can name pc bits that select an instruction-set state (`pc_state_bits`: ARM Thumb) | `isa/riscv`, `isa/mips32`, `isa/arm`, `isa/x86`, `isa/ppc32` |
| `bus.h` | Region map (RAM / ROM / MMIO / ROMD / alias) with priorities (real devices, then per-register stubs from `UAM_PRIO_STUB`, then catch-all windows at `UAM_PRIO_BACKGROUND`), software TLB, device byte order and access widths, watchpoints, fault reporting, DMA master access (`uam_bus_dma_read`/`_write`), dirty-page tracking for snapshots | the core |
| `device.h` | `read`, `write`, optional `write_masked`, `reset`, `destroy`, `save`/`restore` (or `UAM_DEVICE_STATELESS`); declared `endian` and implemented access sizes (declarative register tables are planned) | one file per device |
| `state.h` | State streams: how a component writes and reads back its state (fixed-width little-endian fields, byte arrays, timers with their tie order, signal levels; sticky errors) | the core |
| `sched.h` | Instruction-count clock (saturating at a 2^62-tick horizon), event queue with a per-call firing budget, a kick when the earliest deadline moves (the machine turns it into an exit request), cancel-all, list consistency checks | the core |
| `signal.h` | Interrupt, GPIO and reset lines: raise, lower, connect; edge and level | the core |
| `blockdev.h` | Fixed-size sectors between a storage controller (AHCI, NVMe) and the host: read, write, flush; read-only and volatile (in-memory overlay) modes | core interface; the file backend in `src/host` |
| `chardev.h` | Byte streams between a device and the host (UART consoles); input can be held back until the run starts (`uam_chardev_hold`) | core plumbing; backends in `src/host` |
| `netdev.h` | Ethernet frames between a device and the host (NICs, switches, radio "air") | core plumbing; backends in `src/host` |
| `machine.h` | Composition: machine lifecycle, resets (device reset line, board reset hook), board registry, board config (RAM, unknown-MMIO policy, console, options, named images, named network ports), extra snapshot state (`uam_machine_add_state`), named inputs a board publishes (`uam_machine_add_input`: a UART FIFO, a DMA buffer), extra processors (`uam_machine_add_cpus`: each with its own translation cache, sharing bus and clock; `uam_machine_current_cpu` for per-processor devices) and observers on all of them (`uam_machine_add_pc_hook`, `_set_exc_hook`, `_set_irq_hook`, `_add_pass`, `_set_coverage` with a coverage history per processor, `_get_stats`), ABI handshake | `src/machine`, `boards/*` |
| `snapshot.h` | Machine snapshots: take / restore in memory (incremental against the last baseline), save / load files | `src/machine` |
| `cpu.h` | ISA-neutral hart state (including the coverage state `UAM_OP_COV` counts into) and the core services ISA modules may call (`uam_cpu_xlate_changed`, ...) | the core |
| `loader.h` | ELF / raw images into guest memory | `src/loader` |
| `exec.h` | Run a hart: translation cache, block execution (interpreter, or JIT with optional block chaining), result handling, spin-loop skipping, reset requests, PC hooks, IR passes, edge coverage (`uam_exec_set_coverage`), the exception observer, the host bridge (polling, idle), instruction / virtual-time / stall (livelock) bounds, statistics; with several processors, processor 0's executor interleaves them deterministically in instruction slices on the shared clock (idle skips only when all wait); where the interleaving stands is machine state, so split and restored runs interleave as one run, and a processor's load-reserved reservation ends with its slice | `exec/interp`, `exec/jit_x86` |
| `host.h` | Host side: chardev/netdev backends, host loop, virtual and real-time pacing | `src/host` |
| `gdbstub.h` | gdb remote protocol server on top of hooks, watchpoints and `isa->gdb`; every processor is a thread (all-stop) | `src/debug` |
| `runner.h` | The stock command line as a library entry point (`uam_run_main`), and runner extensions: options plus setup/attach/finish callbacks that contribute coverage sites, iteration boundaries or a fuzz frontend without the runner knowing what they are for | `src/runner`; stock extensions in `fuzz/stock_exts.c` |
| `boards.h` | The stock boards and their registration | `boards/stock.c` |
| `pass.h` | IR → IR transformations run on every new block, after the optimizer and before native compilation (instrumentation); block editing helpers | `analysis/*`; registered with `uam_exec_add_pass` |
| `fuzz.h` | The persistent-mode fuzz harness (boot to a begin pc, snapshot, one restore per input, verdicts) and its frontend interface; the replay frontend | `fuzz/` (see [`fuzzing.md`](fuzzing.md)) |
| `cov.h` | The edge-coverage pass: `UAM_OP_COV` at a table of guest pcs (or at every block start), knowing nothing about where the table came from | `analysis/cov.c` |

## 3. Dependency rules

Arrows point to what a module may include. Anything not listed is forbidden.

```
ir            ← nothing (pure data + algorithms over it)
isa/*         → ir, isa.h, state.h      (x86 and PowerPC page-table walks also bus.h:
                                         physical reads and accessed/referenced/changed bits)
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
                  state.h snapshot.h pass.h cov.h blockdev.h
include/uam/dev/  clint.h plic.h gic.h uart16550.h test_finisher.h unknown_mmio.h unimp.h
                  mmio_stub.h virtio_net.h clock.h i8259.h i8254.h mc146818.h apic.h hpet.h
                  i8042.h acpi_pm.h pc_misc.h pci.h ahci.h nvme.h cfi_flash.h ipi.h
                  i8237.h ide.h fdc.h vga.h lpt.h
include/uam/isa/  riscv.h mips32.h arm.h x86.h ppc.h
src/ir/           builder.c verify.c print.c opt.c pass.c ir_eval.h
src/core/         bus.c sched.c common.c chardev.c netdev.c state.c
src/exec/         interp.c tcache.c run.c cpu.c jit_x86.c coverage.c jit.h execops.h
analysis/         cov.c (the edge-coverage pass)
src/loader/       loader.c
src/machine/      machine.c snapshot.c
src/host/         chardev_backends.c netdev_backends.c blockdev_file.c hostloop.c
src/debug/        gdbstub.c
src/runner/       runner.c
isa/riscv/        rv_lift.c rv_system.c rv.h (private)
isa/mips32/       mips_lift.c mips_system.c mips.h (private)
isa/arm/          arm_lift.c (A32 and the shared emitters) arm_thumb.c (T16/T32) arm_system.c
                  arm.h (private)
isa/ppc32/        ppc_lift.c ppc_fpu.c ppc_mmu.c ppc_booke.c ppc_system.c ppc.h (private)
isa/x86/          x86_decode.c x86_lift*.c x86_string.c x86_sse*.c x86_x87.c x86_helpers.c
                  x86_mmu.c x86_seg.c x86_task.c x86_msr.c x86_smm.c x86_gdb.c x86_system.c
                  x86.h (private)
devices/          clint.c plic.c gic.c uart16550.c test_finisher.c unknown_mmio.c unimp.c
                  mmio_stub.c virtio_net.c i8259.c i8254.c mc146818.c lapic.c ioapic.c hpet.c
                  i8042.c acpi_pm.c pc_misc.c pci.c ata_core.c ahci.c ide.c nvme.c cfi_flash.c
                  ipi.c i8237.c fdc.c vga.c lpt.c
boards/           rv32_ref.c mips_ref.c arm_ref.c x86_64_ref.c ppc_ref.c stock.c mmio_rules.c (uam_board_add_mmio_rules,
                  any board)
fuzz/             harness.c frontend_replay.c (the harness and the replay frontend),
                  stock_exts.c (the runner extensions uam_run_main uses)
fuzz/suzzer/      scheme.c (the bucket formula) import.c (LCV3/LCVB1 readers, site check)
                  frontend_szr1.c (the SZR1 executor, --fuzz-szr) lcvs.c (the LCVS export,
                  --cov-shm) cli.c (the runner extension for all of them) suzzer.h
tools/            uam-run.c fetch-suzzer.sh (`make suzzer`: clone suzzer into ./suzzer, ignored
                  by git)
examples/         external-board/ (an out-of-tree board built against `make lib`)
tests/boards/     test_boards.c uam-test-run.c  (x86_64-test and rv32-test: reference boards plus
                  the components only the tests use; build/uam-test-run)
tests/unit/       test_ir.c test_core.c test_host.c test_snapshot.c test_jit.c test_mmio.c test_cov.c
                  test_fuzz.c test_suzzer.c test_smp.c test_ppc.c
tests/cov/        cov-check.sh  (block-coverage maps across backends)
tests/fuzz/       fuzz-check.sh fuzztarget.h  (the harness on planted-bug firmware, all ISAs)
tests/suzzer/     suzzer-check.sh mk_lists.py lcvs_check.py  (suzzer's formats through the
                  runner) gen-check.sh  (lists from suzzer's own generators, with rizin)
tests/szr/        szr-check.sh fake_brain.py  (the SZR1 executor against a scripted brain)
                  brain-check.sh  (a session with suzzer's own brain)
tests/snapshot/   snapshot-check.sh  (split runs through --save-snapshot / --load-snapshot)
tests/bench/      bench.c x86-start.S x86.ld  (one C benchmark for every guest ISA; make
                  bench-test checks it, tools/xbench.sh times it)
tests/firmware/       start.S selftest.c nettest.c link.ld  (RISC-V)
tests/net/            net-check.sh  (nettest.elf against real tap and tun interfaces)
tests/firmware-mips/  start.S selftest.c link.ld  (MIPS32)
tests/firmware-arm/   start.S selftest.c fuzztarget.c link.ld  (ARMv7: ARM and Thumb-2 builds)
tests/firmware-ppc/   start.S selftest.c smpstart.S smptest.c fuzztarget.c link.ld  (32-bit PowerPC)
                      start-booke.S link-booke.ld  (the self-test for the Book E model)
```

Planned: more passes in `analysis/` (cmp-log, taint), an Intel HEX loader, more boards and
devices.

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
- **Stock boards**: reference boards, one per architecture, holding only what every board of
  that architecture has. They are examples and starting points, not models of any real
  board: a board emulator adds its own chipset and peripherals out of tree, on the generic
  components or on top of a reference board (run its build, then wire to the parts
  `uam_machine_find_device` finds). `rv32-ref`, an RV32IMAC M-mode reference board (CLINT,
  PLIC, NS16550A UART, test finisher, RAM at 0x80000000), and `mips-ref` / `mipsel-ref`
  (MIPS32 with a NS16550A UART and a test finisher), and `arm-ref` / `armeb-ref` (ARMv7-A,
  little-endian and BE8, with a GICv2-style interrupt controller fed by the generic timer and
  the NS16550A UART, a test finisher, RAM at 0 under the vectors), and `ppc-ref` (32-bit
  PowerPC, classic or with `-o model=booke` Book E: RAM at 0 under the vectors, a boot ROM under
  the reset vector (0xfff00100; Book E 0xfffffffc, in the boot page it maps at reset), a NS16550A
  UART and a test finisher), and `x86_64-ref` (the PC/AT core devices, the APICs, HPET, PCI
  configuration, boot flash or a BIOS ROM; no chipset, storage or display). Real devices are
  built out of tree on the library (`examples/external-board/`). Ember's own tests also use
  generic components no reference board wires (the 0xB2 SMI port, storage, VGA, printer,
  virtio-net): `tests/boards/` adds them on top of the reference boards as `x86_64-test` and
  `rv32-test`, registered only by the test runner `build/uam-test-run`.
- **Multiprocessor machines** (`-o cpus=N` on every reference board): processors share the
  bus and the clock and run in deterministic instruction slices (`--quantum`). Every processor
  starts at the reset address and firmware tells them apart by its architectural number:
  `mhartid`; EBase.CPUNum; MPIDR.Aff0; PIR on PowerPC; the x86 initial APIC ID, where the
  others wait for INIT/SIPI. Per-processor interrupts come from generic parts that know several CPUs: the
  CLINT (a msip and mtimecmp per hart) and PLIC contexts on RISC-V, IPI doorbells
  (`devices/ipi.c`) on MIPS, the GIC on ARM (banked SGIs/PPIs, SGI sources, ITARGETSR-routed
  SPIs, a CPU interface per processor), local APICs on an APIC bus on x86, the decrementer and
  IPI doorbells on the external interrupt input on PowerPC (whose `tlbie` reaches every
  processor on the bus).
  `tests/smp/smp_test.h` is one self-test for all four: per-processor timer and IPI interrupts
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
  return from SPSR switch it), and `uam_isa_armeb` resets big-endian.
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
  Each EXIT owns a link slot after its code; a slot is linked when the run loop next runs the
  block the EXIT went to. A computed EXIT's slot holds its first target of the epoch, and its
  other targets (a switch, a return to many callers) go through a jump table shared by all
  blocks, indexed by a hash of the target pc and linked the same way. Links belong to an epoch that
  moves on whenever a linked block may be invalid (a block freed or a code page written, a
  translation or privilege change). Interrupts are still taken only by the run loop, so an ISA
  must request an exit on anything that can make `irq_take` newly succeed (`isa.h`). Chained runs
  are bit-identical to unchained ones and to the interpreter; `--no-chain` turns it off.
- **Stores next to code** (JIT): the bus records which 64-byte lines of a page hold translated
  code, and only a write to such a line drops that code. The JIT's data-TLB keeps a page that
  holds code in its write set under a tag the inline probe cannot match; a store that misses
  there calls a shared stub that reads the page's line mask and stores inline if none of its
  bytes is in a code line, else takes the bus path (`uam_bus_page_host_code`). Firmware that
  keeps data on its code pages stores at full speed, and self-modifying code is still seen.
- **Spin loops** (`exec/spin.h`): a block that jumps back to itself and does nothing but read
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
  (`uam-isa.md` §7.5): `idx = (A ^ prev) & mask; saturating map[idx]++; prev = B`. The site
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
- **Fuzz harness** (`fuzz.h`, [`fuzzing.md`](fuzzing.md)): boot to a begin pc, snapshot,
  and run each input from the snapshot through a named route (guest memory, MMIO `input`
  rules, or an input a board published such as a UART FIFO), with paired or begin-only
  iteration boundaries. Verdicts come from crash pcs, the exception observer filtered by
  exception class, resets, and the executor's stop reasons (hangs, the per-input budget);
  an internal error stops the campaign instead. Coverage sites at the boundaries are
  counted by the boundary hooks, in the map they close. Frontends sit behind
  `uam_fuzz_frontend`; the replay frontend runs saved inputs. Observers
  (`uam_fuzz_set_observer`) see every published map, for side channels.
- **suzzer interoperation** (`fuzz/suzzer/`, built only with a suzzer checkout,
  [`suzzer.md`](suzzer.md)): LCV3 site lists and LCVB1 boundary files in, the SZR1 executor
  (`--fuzz-szr`) and LCVS export (`--cov-shm`, an observer) out. It reaches the runner
  through the generic runner-extension interface (`runner.h`), so nothing outside
  `fuzz/suzzer/` names suzzer.
- **Planned** (phase 3, on top of snapshots):
  - More passes: cmp-logging, memory hooks, taint (`HELPER` calls, which such passes would
    insert, compile in the JIT like every other IR op).
  - More crash checks: execution outside code regions, an icount watchdog per function.

  PC hooks, watchpoints and `uam_exec_stop` are already available to build oracles by hand.
- **GDB stub** against the abstract state (`--gdb PORT`): breakpoints are PC hooks, watchpoints
  are bus watches, registers come from `isa->gdb` (per-register widths for mixed files: the
  x86 view is gdb's whole amd64 core set, x87 included). Every processor is a thread in
  all-stop mode; a stop names the processor that hit the breakpoint or watchpoint, and
  registers and steps follow the selected thread.
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
4. Board description format, more peripherals, ARMv7-M, JIT (*done*: x86-64 host, with block chaining; every IR op compiles, so no block falls back to the interpreter).

Done outside the plan: MIPS32 (CP0, TLB, both byte orders), ARMv7-A (ARM and Thumb-2, CP15,
short-descriptor MMU, generic timer, a GIC, little-endian and BE8), x86-64, 32-bit PowerPC (FPU,
OEA MMU, decrementer), host networking, a gdb stub, the library build with
an ABI handshake. ARMv7-M (the M-profile NVIC exception model) is still planned.

## 8. Verification

Implemented (`make test`):

- Unit tests: IR semantics, verifier and printer (`test_ir`); bus, resets, hooks, time and
  livelock bounds, the ABI handshake (`test_core`); host backends, the host loop and console
  holding (`test_host`); state streams and snapshots under both backends (`test_snapshot`).
- The IR verifier runs on every translated block (on by default; `--no-verify` skips it), before
  and after the optimizer.
- Firmware self-tests: bare-metal RV32, MIPS32 (both byte orders), ARMv7 (built as ARM and
  as Thumb-2 code, little-endian and BE8) and 32-bit PowerPC (integer, CR, branches, every
  load/store form, exceptions, problem state, decrementer, the MMU, the FPU and its FPSCR,
  trace) programs that check themselves and report through the test finisher, run
  on the interpreter and on the JIT; and a multiprocessor self-test on four processors of each
  architecture (`tests/smp/`, `tests/firmware-x86/smp.asm`), also split by snapshots;
  `tests/firmware-x86/legacy.asm` (task switches, gates, the I/O bitmap, virtual-8086 mode,
  A20, MMX) and `biosrom.asm`, a BIOS-style ROM from the reset vector through VGA, floppy (DMA),
  IDE and the printer into a boot sector (`bios-check.py`).
- Multiprocessor observers (`test_smp`): machine-wide hooks and the exception observer see every
  processor, per-processor coverage history gives the same map whatever the slicing, and the
  fuzz harness reports the processor that ended an iteration. `tests/gdb/gdb-check.py` drives
  the gdb stub over the remote protocol on four processors.
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
  code rewritten on a page that has just become a code page again must run as rewritten.
- Coverage: `test_cov` covers `UAM_OP_COV` (verifier, printer, optimizer, saturation, prev
  chaining), passes (no-op and counting passes change nothing; failing or invalid ones stop
  the run), expected hit counts and edge buckets for a known loop on RV32, MIPS32 BE/LE and
  ARMv7 (ARM and Thumb-2) and 32-bit PowerPC
  under the interpreter and the JIT with and without chaining, block mode, rejected sites,
  coverage off, and the exception observer on an illegal instruction on each ISA.
  `tests/cov/cov-check.sh` runs the self-tests with block coverage on every backend and
  requires identical maps and an outcome identical to the run without coverage.
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
  (`fuzztarget.h`) on every stock board (Thumb-2 code on `arm-ref`), including replaying a
  crash file on its own.
- suzzer formats: `test_suzzer` reproduces every conformance vector of the configured suzzer (N=1..4 and 8,
  rotation off and on) byte for byte through `UAM_OP_COV` on every backend, checks the map-size rule against suzzer's table, and
  covers the LCV3/LCVB1 readers (every rejection of suzzer's reader checklist), base
  resolution, boundaries, site naming, the site-byte check and the LCVS region's ownership
  rules. `tests/suzzer/suzzer-check.sh` runs `--cov-sites`/`--cov-boundary` end to end on
  every stock board, including N=3 and N=8 with rotation, and the LCVS export read back by suzzer's
  reader; `gen-check.sh` does it with suzzer's own generators; `tests/szr/` checks the SZR1
  executor frame by frame and in a session with suzzer's brain, whose saved crash directory
  replays without its `.info` notes.
- With a suzzer checkout configured: its format header compiles on its own with ember's
  strict flags, and `test_suzzer` reads the checkout's own conformance vectors. Without one,
  the suzzer tests are skipped.
- The external-board example is built and run against the library.
- Cross-ISA benchmark: `make bench-test` builds `tests/bench/bench.c` for every reference
  board (RV32, MIPS32 BE/LE, ARM, Thumb-2 LE and BE8, PowerPC, x86-64) and natively, and
  requires each guest's checksums, under the interpreter and the JIT, to equal the host
  build's (docs/jit-perf.md, "Parity across ISAs").
- `test_jitdiff` checks the JIT against the interpreter block by block: every pure op at
  every width with temp and immediate operands, then random verified blocks (guards,
  CONT/BREAK imarks, faulting, page-crossing, MMIO and exclusive accesses, helpers,
  sysregs, traps) under every deadline and exit request, and fixed shapes the optimizer
  rewrites (overwritten register writes, flags read back, common subexpressions). Each run must agree on the result,
  registers, memory, icount and on the register file seen at every device, sysreg and helper
  access. `make jitdiff-soak` runs it at length; docs/jit-perf.md has the performance side.

Planned: riscv-tests / riscv-arch-test, golden text-form tests for the lifters.
