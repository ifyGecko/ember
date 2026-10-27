# Step 9 design: dead register writes across instructions

Status: **approved and implemented** (2026-10-27): stage D1 in 72bc0af, stage D2 in the commit after it; results in docs/jit-perf.md. It builds on the work in docs/jit-perf.md (steps 0–8). Where the implementation differs from the text below, §9 says how.

## 1. What it is, in one paragraph

Lifters write guest registers eagerly. Every x86 ALU instruction stores six flag bytes (CF, PF, AF, ZF, SF, OF). Every flag-setting ARM/Thumb instruction stores four (N, Z, C, V), PowerPC Rc=1 forms store CR0, and so on. In real code the next flag-setting instruction overwrites almost all of these before anything reads them.

Step 5 already removes a write that is overwritten **within the same guest instruction**. This step removes the far more common case: a write overwritten by a **later** instruction of the same block. The catch is that between the two writes the block can still stop:
- an interrupt or deadline stop at an instruction boundary;
- a memory access that faults or goes to a device.

At such a point the guest state must be exact. So the JIT does not delete those writes. It moves them onto the cold paths: the code that runs only when the block actually stops there. The fast path, the one that runs nearly always, skips them, and skips the arithmetic that only fed them.

## 2. Why it is worth doing

### Measured potential

Method: I ran the analysis below on the final IR of every block (`-t ir`). For OVMF I weighted each block by its share of perf samples. All numbers are on the current main.

| Workload / guest | Writes deferrable | Pure ops that only feed them | Hot-path share removed |
|---|---:|---:|---|
| OVMF boot (x86-64) | 9.4% of IR ops | 12.4% | ~35% of native-code ops, ~22% of all samples (op-count proxy) |
| x86 self-test | 5.2% | 8.1% | — |
| Thumb self-test | 7.7% | 4.8% | — |
| ARM self-test | 5.5% | 1.4% | — |
| PowerPC self-test | 3.8% | 0.3% | — |
| MIPS self-test / VxWorks | 6.8% / 7.3% | ~0% | the writes only (no arithmetic to sink) |
| RISC-V self-test | 9.4% | 0% | the writes only |

The op-count share overstates the time saved. Flag arithmetic is cheap register work that the CPU overlaps well, while loads, stores and the chain path cost more per op. My expectations:

| Guest | Expected speedup | Why |
|---|---|---|
| x86 (OVMF) | 8–15% | flag work dominates what is left in hot blocks |
| ARM/Thumb | 3–8% on flag-heavy code | S-suffixed ops and nearly every 16-bit Thumb ALU op set NZCV |
| PowerPC | 1–3% | |
| MIPS, RISC-V | 1–3% | removes a register store each time a register is overwritten by the next instruction, e.g. `lui t1` followed by `lw t1` in the VxWorks idle loop |

Each stage will be judged on ember's own cross-ISA benchmark (`tools/xbench.sh --per-kernel`, every guest at once); the OVMF figures above come from an external workload and are only the estimate that motivated the step.

### Why it matters for parity across ISAs

Final IR ops per guest instruction today (self-tests, after the optimizer, IMARK included):

| RV32 | MIPS | PPC | ARM | Thumb | x86 (self-test / OVMF) |
|---:|---:|---:|---:|---:|---:|
| 3.0 | 3.0 | 4.7 | 5.7 | 6.0 | 7.3 / 7.9 |

The spread between ISAs is almost entirely eager flag and condition modelling. This step attacks exactly that spread, without knowing what a flag is. So it **narrows** the gap between flag-heavy ISAs (x86, ARM, PPC) and flagless ones (MIPS, RISC-V), instead of favouring one.

## 3. Correctness model

The invariant does not change: wherever guest state can be observed, the JIT's register file equals the interpreter's. The interpreter is untouched and keeps doing every write in place.

Places where the register file can be observed in the middle of a block:

| Point | Kind | Why |
|---|---|---|
| EXIT (guarded or not), TRAP, WAIT, end of block | hard | the run loop, exception entry and hooks see everything |
| SYSRD / SYSWR, HELPER (any), AMO / CAS | hard | ISA C code may read or write the register file (e.g. building RFLAGS/CPSR); always a C call anyway |
| A BREAK IMARK (PC hook / breakpoint) | hard | always stops |
| GET of some bytes | hard for those bytes | the block itself reads them |
| A non-first, non-CONT IMARK | soft | stops only on an exit request or deadline: the out-of-line stop stub |
| LD / ST | soft | observable only on the bridge path: a fault, a device access (whose callback may read registers), a watchpoint, a page crossing |
| COV, FENCE, pure ops | none | |

**Hard point:** every earlier write must already be done in place. Writes are deferred only past soft points.

**Soft point:** the writes still pending are replayed on that point's cold path only:
- in the IMARK stop stub, before it stores the result pc;
- in a load/store slow path, before the bridge call.

The inline data-TLB path covers only plain, unwatched RAM/ROM pages, with no device and no fault. On that path nothing can observe the register file, so skipping the replay there is invisible. A replay is idempotent (same values, same order), so replaying at several soft points, or replaying something a later write overwrites anyway, is harmless.

Why the two backends agree: at every hard point, both have performed every write. At every soft point that is taken (stop, fault, device), the JIT has replayed exactly the pending writes, in program order, before anything outside the block runs. Everywhere else the difference cannot be seen.

## 4. Design

### 4.1 Where it lives: inside the JIT, not in the IR

The whole analysis runs in `jit_x86.c` on the final block, just before code emission. Consequences:

- **Unchanged:** no IR change, no new opcode or flag, and no change to the interpreter, verifier, printer, optimizer or user passes. The interpreter stays the literal reference.
- **Ordering:** it sees the block after user passes and hook marking, so BREAK IMARKs and inserted instructions are already there.
- **Information:** the JIT already has the helper table and the register-file size.
- **Fallback:** `UAM_JIT_NODEFER=1` turns it off (default on), and it is off automatically when the block runs interpreted.

An IR-level alternative (a `uam_finalize` pass that writes stub lists into the block) was considered and rejected. It would have to teach the interpreter, verifier and printer about stubs for no gain.

### 4.2 Analysis

**Backward pass.** It keeps K, a byte bitmap over the register file: the bytes that a later unguarded write overwrites before any hard read of them.
- Unguarded PUT whose bytes are all in K: mark it deferred, then add its bytes to K.
- Unguarded PUT not fully in K: add its bytes to K.
- Guarded PUT whose bytes are all in K: mark it deferred (its guard is kept for the replay). K is unchanged: a guarded write does not reliably overwrite.
- GET: remove its bytes from K.
- Hard point: clear K.
- Soft point: no change to K.

**Forward pass.** It walks the block keeping a list of pending deferred writes.
- At a deferred PUT: add it to the list.
- At any PUT: drop pending writes it fully covers.
- **Overlap rule.** A non-deferred PUT that only partly overlaps a pending one would be clobbered by a later replay. When that happens, the pending write is un-deferred (it is done in place, as today). Un-deferring only restores original behaviour, so it is always safe.
- At each soft point: record the pending list as that point's stub.
- At a hard point: the list is empty by construction (the backward pass emptied K).

**Cold ops (stage D2).** A pure op is cold if every reader is a deferred PUT or another cold op. For x86 this is most of the flag arithmetic: the compares, XOR/AND for OF/AF, and the ZEXT.8s. A cold op is not emitted on the fast path. A stub that needs it recomputes it on the cold path.

### 4.3 Code generation

- **Deferred PUTs and cold ops:** not emitted in the main stream.
- **Stub contents:** a soft point's stub emits, in program order:
  1. the cold ops its pending writes need (transitively), each computed into `rax` and stored to its `jc->temps` slot, so a later cold op reads its operands from the slot;
  2. the pending writes, each guarded if the original was.

  It uses only scratch registers, so it fits before the existing bridge call or stop-stub code.
- **IMARK stop stubs:** these already exist out of line. The replay goes before `set_res_pc`.
- **Load/store slow paths:** the replay goes right after the icount flush and before `emit_site`. Stores happen there; nothing the bridge reads is disturbed.
- **Register allocation:**
  - A deferred write's value and guard temps, and the operands of the cold ops a stub needs, count as read at every soft point whose stub uses them. They stay live (in a register or slot) up to the last such point.
  - A deferred PUT itself is no longer a read.
  - Cold temps are never given a register; inside a stub they live in their slot.
  - This is the same use-position machinery the allocator already has, with extra positions.
- **Code size:** stubs are duplicated per soft point. Estimate for OVMF: about 81k stubs averaging 6 writes plus their cold ops, i.e. tens of percent more code, all of it out of line. Two controls:
  1. **D2 stub cap.** If a stub would exceed N ops (start with 32), the writes feeding it are un-deferred.
  2. **Shared replay sequences**, only if measurement shows icache or translation-time cost. Consecutive soft points with the same pending list `call` one replay sequence ending in `ret`. Native code runs on its own stack frame, so this is legal.

### 4.4 Stages

- **D1: defer the writes only.** Values are still computed on the fast path. This is small and validates the observation-point machinery; RISC-V, MIPS and PPC gain most of what they will get here.
- **D2: sink cold ops into the stubs.** This is where x86 and Thumb gain.

Each stage gets its own commit and measurement, and goes ahead only if the differential tests stay green under soak.

## 5. Testing

- **test_jitdiff already checks the right things.** It compares the register file at every device access, sysreg and helper call (via the logged register hash), at faults, at every deadline k, with an exit already requested, with exits raised by a device, and at the end. A soft-point replay that is missing or wrong shows up there.
- **New level C patterns:**
  - flag-like writes overwritten two instructions later with a load or store in between (RAM, device, fault, page crossing);
  - the partial-overlap case of §4.2;
  - guarded deferred writes;
  - cold chains that need transitive recomputation;
  - a GET between a deferred write and its killer;
  - a CONT IMARK in between (no stop) versus a plain IMARK (stop).
- **Mutation checks**, as in earlier steps: drop a replay, replay out of order, ignore the overlap rule, forget a soft point.
- **Bisecting:** `UAM_JIT_NODEFER=1`. `--stats` gets counters for deferred writes, sunk ops and stub bytes.
- **End to end:** both workloads keep identical consoles and identical interpreter-vs-JIT state, and `ovmf-check` / `secureboot-check` pass.

## 6. Risks

| Risk | Mitigation |
|---|---|
| A missed observation point (some path that reads the register file mid-block) | The classification is deliberately conservative: anything that calls C is hard, except load/store slow paths, which replay before calling |
| Code size and translation time | Measured per stage; cap and shared replays as above |
| Allocator pressure from longer-lived values | Spills are already handled; `UAM_JIT_REGS=1` soak exercises them |

## 7. Alternatives rejected

- **Lazy flags in the lifters.** Keep the last operation and operands, and compute flags when they are read. This is architecture-specific work (x86 differently from ARM differently from PPC), and it changes the register-file layout: ABI, snapshots, gdb, every C reader of RFLAGS/CPSR. It is the opposite of keeping ember generic.
- **An IMARK flag meaning "no stop here".** This would let deferral cross instruction boundaries trivially, but it changes when interrupts and deadlines are taken. That is visible to the guest and differs from the interpreter.

## 9. As implemented

- **Where:** `defer_analyze` and `cold_analyze` in `src/exec/jit_x86.c`, run on each block before register allocation; the replays are emitted by `emit_replays`.
- **IMARK stop stubs** stay at the end of the block. Each records, at its IMARK, which register holds each temp its replay reads, and the stub is emitted with those locations, so stubs and load/store slow paths share one replay emitter.
- **Hard points** also include loads and stores that never take the inline path (exclusive, physical, unprivileged, or a constant address that crosses a page or is misaligned): they always call C, so a replay there would run every time.
- **Recomputed ops** use rax, rcx and rdx. On a load/store slow path the address in rdx is loaded again after them; the store value in r11 is untouched. Only inline pure ops are sunk: one that needs the C evaluator stays on the fast path.
- **D2 stub cap:** a soft point that would recompute more than 32 ops keeps all of them on the fast path (the writes stay deferred), and the analysis repeats until no list is over the cap. Keeping ops on the fast path only shortens the other lists.
- **No `--stats` counters:** they would change `uam_exec_stats`, and with it the library ABI, for a debugging aid. The op map (`UAM_JIT_OPMAP`, `tools/jitprof.py`) labels replay code as its own region kind, `replay`; `UAM_JIT_NODEFER=1` still turns the step off for bisecting.
- **Tests:** test_jitdiff level D (deferral shapes under every stop condition) and the mutation checks listed in the D1 and D2 commits. The end-to-end runs earned their place: the first D2 build skipped a stop stub's replay when it read no temps (only constants pending, as in a MIPS `li` overwritten in a branch delay slot), which VxWorks hit within 1e7 instructions while every unit test passed; level D's pattern 8 now covers it.

---

## 8. Architecture neutrality: steps 0–8 and the rest of ember

You asked whether my changes, or anything else in ember, lean toward one architecture. Short answer:
- **Steps 0–8:** no change is guest-specific. Each acts on IR shapes or runtime structures that every ISA produces.
- **Elsewhere in ember:** an audit of the generic layers found four real leaks and two bugs on the generic/ISA boundary. All have small generic fixes; nothing has been changed yet.

### 8.1 Steps 0–8

| Step | Generic? | Which guests gain most, and why |
|---|---|---|
| 0–1 measurement, differential test | yes | all; the test checks every IR op and random blocks, not any one ISA |
| 2 inline pure ops | yes | ARM, MIPS, PPC (CARRY/SCARRY/SBORROW, CLZ); x86 (POPCNT, CTZ). Each ISA uses a different subset; all are now inline |
| 3 encodings | yes | all |
| 4 one check per instruction | yes | all; more instructions per block means more gain, so short-instruction ISAs like MIPS/RV gained most |
| 5 optimizer | yes | the peepholes match shapes the x86, ARM and PPC lifters all emit for flag bytes; CSE and canonical operands help every ISA |
| 6 register allocation | yes | all; more temps per instruction means more gain, which favours x86 and ARM (it narrowed their gap) |
| 7 flag fusion | yes | any compare feeding a guard or select: MIPS/RV branches, ARM conditions, x86 jcc, PPC bc |
| 8 dispatch, bus sub-page cache, constant-address probe | yes | sub-page cache: any small MMIO register block on any SoC (x86 I/O ports are one case); constant address: MIPS `lui`+`lw`, x86 absolute operands, ARM literal-pool targets |

One host-side fact, unchanged: the JIT targets x86-64 hosts only. That concerns the machine ember runs on, not the guests it runs. Other hosts use the interpreter.

### 8.2 Audit of the generic layers

I read src/core, src/exec, src/ir, src/machine, src/runner, src/loader, src/host, include/uam, fuzz and analysis, and verified the important findings in the code.

**Clean:**
- The bus has no I/O-port space and no SMM. The x86 port convention (2^40 + port) lives only in include/uam/isa/x86.h, isa/x86 and the x86 board.
- Endianness is per access. The register file is little-endian by definition, and each device declares its own byte order.
- There is no 32-bit address assumption in generic code.
- No generic device includes an ISA header.
- Several hooks were added for one ISA but are generic, and are used by others or available to them: CONT IMARKs (MIPS delay slots, ARM IT blocks, x86 STI shadow), `isa->translate`/`xlate_gen`, `fetch_endian`, `pc_state_bits`, `switch_in`, region enable/move.

**Leaks (ISA behaviour inside generic code), confirmed.** All five fixed since this review (commits 7b28eae..a352e5b), each with a test that fails without the fix where one is possible:
1. **Thumb bit masked for every ISA.** `src/exec/run.c:46` and `:398` clear pc bit 0 unconditionally. For x86 (odd pcs) `mark_block_code` then misses the last byte of a block. If that byte starts a new 64-byte line, a guest write there does not invalidate the translation.
   - Fix: use the ISA's `pc_state_bits` (already declared in isa.h and used by the gdb stub).
   - **Fixed (2be1aca):** `uam_isa_pc_state_bits()`; x86 self-test 97 patches such a byte.
2. **ARM-specific code in the suzzer fuzz frontend.** `fuzz/suzzer/cli.c:23,240,274` calls `uam_arm_is`; `fuzz/suzzer/import.c:346-374` guesses Thumb from site lengths. This creates a link dependency on the ARM module.
   - Fix: also go through `pc_state_bits`.
   - **Fixed (1af0c59):** `sz_state_sites`/`sz_state_bounds`; `sz_check_sites` also read the wrong bytes for a 2-byte x86 site at an odd address.
3. **PC chipset behaviour in the generic PCI device.** `devices/pci.c:680-684` treats a byte write to CF9 as reset control.
   - Fix: make it a board option or callback.
   - **Fixed (44d822b):** the CAM now implements the generic PCI rule (only a dword access to CONFIG_ADDRESS is a configuration access) and passes the rest to `uam_pci_cam_passthrough`. The `uam_pci_set_cf9_hook` compatibility wrapper kept at first was removed in bc4e678: a board wires its own reset-control device through the pass-through.
4. **Smaller ones:**
   - `devices/uart16550.c` supports register stride 1 only (PC layout). SoC 16550s at stride 4 cannot use it. Fix: add a register-shift option. **Fixed (35e6de4):** `uam_uart16550_create_cfg` with `reg_shift` and the slot byte order.
   - `devices/pci.c:245`: the fallback MSI write uses host byte order. Fix: write explicit little-endian. **Fixed (a352e5b)**, together with the MSI-X table and PBA, which had the same problem.

**Bug on the generic/ISA boundary, confirmed in the code:**
5. **JIT data-TLB not flushed on privilege changes.** The data-TLB is flushed when the bus or translation generation changes, but not when `cpu->priv` changes. MIPS (`mips_system.c:97`, `mode_update`) and x86 (`x86_system.c:105`, `x86_update_priv`) change privilege without flushing, yet their translation depends on privilege. After dropping to user mode, the JIT could therefore access a kernel-only page inline where the interpreter faults. ARM flushes for itself.
   - Neither workload is affected: both run privileged.
   - Fix: flush generically in the run loop when `priv` changes, next to the existing checks. That fixes all ISAs at once, and ARM's own flush becomes redundant.
   - **Fixed (7b28eae):** the JIT flushes on entry when `cpu->priv` differs from the level it last ran at (it already tracked that level for its links), and stops chaining when a helper changes `priv` (MIPS ERET, RISC-V MRET), which would otherwise run on into blocks translated at the old level. ARM's own flush is gone. test_jitdiff checks a store through a priv-dependent translation.

**Design choices in the IR that cost some ISAs more than others** (not bugs; listed so we decide deliberately):

| Choice | Effect |
|---|---|
| Register GET/PUT must be 8–64 bits wide | Flags are bytes: each write is ZEXT.8+PUT.8 and each read GET.8+TRUNC. Peepholes remove the round trips within a block; step 9 removes the dead ones across instructions |
| No add-with-carry-in form | ADC/SBC (ARM, x86, PPC adde) cost about 8 IR ops. A generic `CARRY3`-style op would be a candidate if profiles ever show it |
| Edge cases follow particular ISAs | Division-by-zero results follow RISC-V and shift-by-≥width follows ARM. The other ISAs pay one extra op (a guard or an AND) |
| Other limits | One translation per pc: code reached alternately under two privilege tags is retranslated each time. The minimum page size is 4 KiB: sub-4K pages (MIPS PageGrain 1K, ARMv5 tiny pages) are not supported. PC hooks never fire on an instruction that continues another (inside an ARM IT block, after x86 STI), which the docs describe only as "delay slot" |

### 8.3 Measuring parity, not just reasoning about it

The in-repo self-tests run for milliseconds and measure translation, not execution speed. The honest way to check "roughly equal performance per ISA" is one shared C benchmark (CRC, sort, matrix, string search, a switch-dispatch interpreter) built for every guest. Report its guest-instructions-per-second and host-time-per-source-iteration for each guest.

The cross toolchains are already used for the self-tests; x86 needs a small freestanding C entry. I recommend adding it, as `tests/bench/`, before or alongside step 9, so that both step 9 and any future change are judged on all ISAs at once.

**Done:** `tests/bench/` with `make bench-test` and `tools/xbench.sh` (docs/jit-perf.md, "Parity across ISAs"). Per-kernel geometric mean of time over the fastest guest, before step 9: RV32 1.04, MIPS 1.12 / 1.16 (BE/LE), PPC 1.26, ARM 1.27, Thumb 1.50 / 1.45 (LE/BE8), x86 2.17. The x86 gap is largest on the ALU kernels (matmul 3.7x, fmt 3.3x, crc 2.8x), where about three quarters of the IR is dead flag computation: step 9's gate holds on this benchmark too, and it is the measure step 9 should be judged by.
