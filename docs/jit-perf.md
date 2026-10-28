# JIT performance: measuring it, and where it stands

This note is about the x86-64 backend (`src/exec/jit_x86.c`). It covers the tools for
measuring the JIT, a log of what each change bought, and how the guest ISAs compare.
Ember's own reference measure is the cross-ISA benchmark in `tests/bench` ("Parity
across ISAs" below): every change is judged on all guests at once with it.

## Measuring

### `--stats`

With `-s`/`--stats`, `uam-run` prints these lines besides the block counters:

```
stats: time=3.921s instructions=1000000000 MIPS=255.0 translate=0.157s ir ops lifted=... final=... code bytes=...
stats: jit calls eval=... load=... store=...
```

- `time` is the host wall time of the run. `instructions` is the guest instructions it
  retired, and `MIPS` is the ratio of the two.
- `translate` is host time spent turning guest code into blocks: lift, verify,
  optimize, passes and compile.
- `ir ops lifted` / `final` count IR instructions as the lifter produced them, and
  after the optimizer and passes.
- `code bytes` is the native code the JIT emitted.
- `jit calls` count calls from native code into C:
  - `eval`: pure ops the JIT does not inline, run by the interpreter's evaluator
    (`ir_eval.h`).
  - `load` / `store`: accesses that missed the inline data-TLB path, such as MMIO, a
    page not yet cached, or a page-crossing access.

The same numbers are in `uam_exec_stats` (`uam_machine_get_stats` sums the processors).

### Profiling native code

`perf record` cannot name samples in JIT code by itself, because that code lives in an
anonymous mapping. Two runner options describe it:

- **`--perf-map`** (env `UAM_PERF_MAP=1`) writes `/tmp/perf-<pid>.map`, one line per
  compiled block: `start size uam:<isa>:0x<guest pc>`. `perf report` then names samples
  by guest block.
- **`--jit-opmap FILE`** (env `UAM_JIT_OPMAP=FILE`) appends, for each compiled block, the
  code offset where each region starts and the IR instruction it came from:

  ```
  B <code addr> <size> <guest pc> <isa>
  M <offset> <kind> <insn index> [<mnemonic>.<width>[@<reg offset>]]
  ```

  The kinds are:
  - `op`: the IR instruction itself. For an `imark` this is the per-instruction
    interrupt/deadline check.
  - `imark-pc`: the fault-pc bookkeeping and icount increment.
  - `slow`: the bridge path of a load or store.
  - `chain`: the block-to-block link check.
  - `prologue`, `epilogue`, `slots`.
  - `R` marks an arena recycle: addresses after it are reused.

Both are cheap enough to leave on in a timed run.

`tools/jitprof.py` joins a perf profile with the op map:

```
uam-run ... --jit --jit-opmap ops.txt                              # under perf record -o perf.data
uam-run ... --jit -t ir 2> ir.log                                  # separate run: printing IR is slow
tools/jitprof.py --opmap ops.txt --perf perf.data --ir ir.log
```

It prints:
- the share of samples in native code by region and by IR opcode;
- the hottest C functions, which are the bridges, device models and run loop;
- the hottest blocks with a per-block breakdown, and the IR of the top few.

Runs are deterministic, so the IR from a second run matches the profiled one.

### Timing

`tools/jitbench.sh --runs N --label NAME -- COMMAND...` runs a command N times and
prints one tab-separated row:
- median wall time and MIPS;
- host instructions per guest instruction, when `perf stat` has hardware counters;
- the translation and `jit calls` statistics;
- a hash of the guest console output, so two builds can be checked for identical guest
  behaviour.

On a VM without a PMU the host-instruction column is `-`; use the median time there.

## External workloads used for the log below

The log of steps 0-8 was measured on two firmware images run by boards that are not
part of ember (separate projects built against the tree under test, nothing of them in
this repository). They are kept here only as the record of those measurements. Both
runs use a fixed instruction budget and no input.
- **x86-64 UEFI boot.** An installed OVMF booted to the UEFI shell,
  1e9 instructions, 512 MiB RAM. This exercises SEC/PEI decompression, DXE drivers,
  I/O-port polling, and x86 flag computation.
- **MIPS32 BE VxWorks 5.5.1.** An RTOS image, 4e9 instructions. The boot banner
  comes at about 6e7 instructions; after that it is mostly the idle task spinning on a
  memory word, so this measures the tight-loop costs: load fast path, per-instruction
  checks, chaining.

## Baseline (main at c84a436, before any of the work below)

Medians of 5 runs on a 12-core x86-64 VM (no PMU):

| workload | time | MIPS | translate | eval calls | code bytes |
|---|---:|---:|---:|---:|---:|
| OVMF, 1e9 insns | 3.96 s | 253 | 0.16 s | 14.8 M | 40.5 MB |
| VxWorks, 4e9 insns | 3.10 s | 1291 | 0.05 s | 6.2 M | 13.6 MB |

**OVMF profile.**
- 71% of samples are in native code:
  - x86 flag computation dominates: `put.8` 9.6%, `zext.8` 7.6%, plus most of the
    compares (9.5%) and the `xor`/`and` ALU ops (13.6%) that build CF/OF/AF/ZF/SF.
  - memory fast paths take 15%.
  - the per-instruction checks and fault-pc bookkeeping take 3.5%, and EXIT/chaining 4%.
- The largest C cost is bus `resolve` at 8.7%, from I/O-port and MMIO accesses.
- Blocks average about 340 bytes of host code per guest instruction.

**VxWorks profile.**
- 83% of samples are in native code, and 68% are one 4-instruction idle loop (`lui`,
  `lw`, `bne`, `nop`) that compiles to 664 bytes. Its cost is spread over:
  - the load fast path, 27%;
  - the `ne` compare, 13%;
  - per-instruction checks and bookkeeping, 16%;
  - EXIT and chaining, 14%.
- `uam_tcache_lookup` takes 5.6%: a dispatch every time a chain is cut for host polling.

## Log

Each step below lists its effect on the two workloads (5-run medians, same machine).

| step | OVMF time | VxWorks time | notes |
|---|---:|---:|---|
| baseline | 3.96 s | 3.10 s | |
| 0: measurement | 3.92 s | 3.07 s | stats, perf map, op map, tools; no codegen change |
| 1: differential test | - | - | `test_jitdiff`; fixed trunc.1, sext from 1 bit and shifts by an immediate >= 256 in the JIT, immediate guards in the interpreter |
| 2: inline pure ops | 3.91 s | 3.09 s | carry/overflow, rotates, bit counts, bswap, mulh, div/rem and variable shifts inline (POPCNT if the host has it); eval calls 14.8 M / 6.2 M -> 0. These workloads barely used them; ARM/PPC flag code (CARRY, SBORROW) does |
| 3: encodings | 3.72 s | 2.96 s | immediates as imm32/simm32, reg-imm and reg-mem ALU forms, compares at width W (no sign-extension), 32-bit forms that need no mask, PUT of a constant as one store, guards tested in memory; code -17% |
| 4: per-instruction checks | 3.51 s | 2.61 s | one `cmp r13, [icount_stop]` + untaken branch per IMARK (stop stubs out of line), fault pc/parent/cont as a per-instruction record pointed to only on bridge calls; r12 freed |
| 5: optimizer | 3.39 s | 2.51 s | canonical commutative operands, CSE, peepholes for flag bytes read back (zext/trunc/compare/negate/select), PUTs overwritten within one instruction removed; final IR ops -3.8% (OVMF) |
| 6: register allocation | 2.47 s | 2.39 s | temps in r12/rsi/rdi/r8-r10 (linear scan, furthest-next-use spills; live caller-saved registers saved around calls); results defined after a guard's paths join; memory fast paths on rdx/r11. `UAM_JIT_REGS=N` limits the pool (test_jitdiff runs N=1 to stress spills) |
| 7: flag fusion, chain path | 2.37 s | 2.19 s | a compare (or carry/overflow) read once by the next guard or select stays in the host flags (jcc/cmovcc), also across a continuation imark (icount by lea); chained jumps no longer count hops in memory |
| 8a: dispatch | 2.38 s | 2.12 s | a block's code pages are rechecked only when bus->code_gen moved (a code write or a map change); a direct-mapped cache of recent tcache entries; buckets double as blocks accumulate (ABI 15) |
| 8b: bus sub-page cache | 2.07 s | 2.10 s | regions smaller than a page (I/O ports, register blocks) were resolved by a linear scan of every region on each access; a 64-entry cache keyed by 8-byte granule now serves them (ABI 16) |
| 8c: constant-address probes | 2.07 s | 1.86 s | a load/store to a constant address compares one known data-TLB entry with its page (a constant access that crosses a page or is misaligned goes straight to the bridge) |
| 9: deferred register writes | 1.70 s | 1.81 s | a register write that a later instruction of the block overwrites unread is replayed only where the block can stop (IMARK stop stubs, load/store slow paths), and the pure ops only those writes need are recomputed there (docs/jit-dead-puts-design.md). Measured against 2.04 s / 1.87 s for the build before it in the same session; code +20% / +2%, translation 0.17 -> 0.23 s / 0.048 -> 0.061 s. Consoles and the interpreter-vs-JIT state identical on both |
| 10: jump table for computed exits | 1.69 s | 1.72 s | a computed EXIT whose slot holds another target looks its target pc up in a 4096-entry direct-mapped table of links shared by all blocks (hash, then pc and epoch compared) and links it there on a miss, instead of returning to the run loop. Measured against 1.71 s / 1.81 s in the same session; blocks returning to the run loop 0.41 M -> 0.25 M / 5.5 M -> 3.2 M; code +1%. `UAM_JIT_JTAB_BITS=N` uses 2^N entries (test_jit runs N=1 so links collide). Consoles and the interpreter-vs-JIT state identical on both |
| 11: spin loops | 1.69 s | 0.51 s | not a code generation change: the run loop adds the passes of a block that waits on memory (jumps back to itself, only loads, pure ops and register writes) up to the next deadline or host poll to icount at once, after evaluating one pass without effect shows it would change nothing (src/exec/spin.h; both backends). Measured against 1.68 s / 1.72 s in the same session; VxWorks under the interpreter 40.2 s -> 4.4 s; xbench unchanged (no kernel spins). `UAM_SPIN_SKIP=0` turns it off. Consoles, block counts and the interpreter-vs-JIT state identical on both |
| 12: stores next to code | 1.74 s | 0.54 s | a page holding translated code goes into the data-TLB write set under a tag the inline probe never matches; a store that misses calls one of 16 shared stubs (size, byte order, alignment) that read the page's 64-byte code-line mask and store inline unless a byte falls in a code line (src/exec/jit_x86.c, emit_code_probe; `uam_bus_page_host_code`). Unchanged against 1.74 s / 0.53 s in the same session (bridge stores 76 K -> 38 K / 1.56 M -> 1.56 M, the rest device writes); code +1.1% / +1.5% (inline probes instead of stubs were +11% / +17% for 1-2% more on the bench). Consoles and the interpreter-vs-JIT state identical on both |

## Where it stands (after step 8c)

Against the baseline: OVMF 3.91 s -> 2.06 s (256 -> 485 MIPS, 1.9x), VxWorks
3.07 s -> 1.87 s (1302 -> 2143 MIPS, 1.6x).

Correctness, end to end:
- The guest console is identical to the baseline's on both workloads.
- The interpreter and the JIT end with the same console and `-d` state: OVMF at 1e9
  instructions, VxWorks at 2e9.
- The external x86 board's own UEFI boot and secure-boot checks pass on this build.

**OVMF profile.**
- 64% of samples are now in native code.
- Compares take 14%, zero-extensions 4.6%, `put.8` 1.8%, and part of the ALU ops
  build the x86 flags (CF/OF/AF/ZF/SF/PF). Roughly 15-20% of the run is flag work.
  Most of it is overwritten by the next instruction and never read, which is what
  removing dead flag stores across instructions would recover.
- The remaining C costs are small: port I/O 3.5%, libc 6%, translation 4%.

**VxWorks profile.** Its idle loop is now one constant-address load, a fused compare
and branch, and the chain path back to itself.

## Parity across ISAs

The reference workloads say little about how the guests compare. `tests/bench/bench.c`
is one C benchmark built for every reference board and natively: CRC-32, heapsort,
matrix multiply, string search, a switch-dispatch bytecode interpreter, pointer
chasing, 64-bit arithmetic and number formatting, about 5e8 guest instructions in all.
It uses fixed-width types only, so every build prints the same checksums.
- `make bench-test` (part of `make test`) runs two rounds on every guest, under the
  interpreter and the JIT, and requires the host build's checksums.
- `tools/xbench.sh` (`make xbench`) times the full run on every guest. Each does the
  same work, so wall time is the number to compare; `vs_best` divides it by the fastest
  guest's. `--per-kernel` times each kernel on its own and prints the ratio matrix with
  a geometric mean per guest; `--interp` uses the interpreter.

Whole run, JIT, medians of 5 (main at a352e5b):

| Guest | Guest instructions | Time | MIPS | vs best |
|---|---:|---:|---:|---:|
| RV32 | 519M | 0.384 s | 1351 | 1.00 |
| MIPS32 BE / LE | 540M | 0.407 / 0.409 s | 1327 / 1320 | 1.06 / 1.07 |
| ARM | 451M | 0.465 s | 971 | 1.21 |
| Thumb-2 LE / BE8 | 450M | 0.467 / 0.464 s | 965 / 971 | 1.22 / 1.21 |
| PowerPC | 502M | 0.474 s | 1058 | 1.23 |
| x86-64 | 454M | 0.657 s | 692 | 1.71 |

Each kernel on its own (`--per-kernel --rounds 1600`, medians of 3): time over the
fastest guest's on that kernel.

| Kernel | RV32 | MIPS | MIPSel | ARM | Thumb | Thumb BE8 | PPC | x86 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| crc | 1.00 | 1.17 | 1.18 | 1.31 | 1.44 | 1.45 | 1.07 | 2.84 |
| sort | 1.00 | 1.20 | 1.20 | 1.37 | 1.34 | 1.35 | 1.38 | 1.58 |
| matmul | 1.00 | 1.12 | 1.10 | 1.35 | 1.68 | 1.70 | 1.01 | 3.73 |
| string | 1.00 | 1.06 | 1.09 | 1.36 | 1.51 | 1.55 | 1.37 | 2.11 |
| interp | 1.40 | 1.02 | 1.38 | 1.00 | 1.42 | 1.05 | 1.41 | 1.38 |
| list | 1.00 | 1.16 | 1.13 | 1.38 | 1.47 | 1.47 | 1.30 | 1.95 |
| u64 | 1.00 | 1.11 | 1.09 | 1.20 | 1.56 | 1.55 | 1.33 | 1.55 |
| fmt | 1.00 | 1.14 | 1.15 | 1.25 | 1.57 | 1.55 | 1.30 | 3.28 |
| **geomean** | 1.04 | 1.12 | 1.16 | 1.27 | 1.50 | 1.45 | 1.26 | 2.17 |

Under the interpreter (one run) the order is the same and the spread wider: MIPS 1.00,
RV32 1.08, PPC 1.30, ARM 1.40, Thumb 1.58, x86 2.79.

What it shows:
- **x86 is the outlier, and flags are why.** On the ALU kernels (crc, matmul, fmt,
  string) x86 runs 2.1-3.7x the time of RV32 for about the same instruction count. The
  matmul inner loop (`mov; imul; add; add; add; cmp; jne`) is about 90 IR ops, three
  quarters of them CF/OF/AF/ZF/SF/PF computations that the next instruction overwrites
  before `jne` reads ZF. That is what step 9 removes.
- **Thumb trails ARM by ~15%** on the same instruction counts: nearly every 16-bit
  Thumb ALU op sets NZCV, again dead flag work.
- **The interpreter kernel is bimodal** (1.0 or 1.4 by guest, varying between runs):
  its switch is an indirect jump, and a computed exit links only its last target, so
  about 30% of its block exits return to the run loop (3.8M of 12.6M on MIPS). A
  multi-target cache for computed exits would help every ISA alike (step 10 did that).
- RV32, MIPS and PPC are within 1.3x of each other everywhere; endianness costs nothing
  measurable (MIPS BE/LE, Thumb LE/BE8).

### After step 9

Step 9 defers register writes that a later instruction overwrites unread, and the
arithmetic that only feeds them (docs/jit-dead-puts-design.md). Each kernel's time
before over after (`--per-kernel --times --rounds 1600`, medians of 5; above 1 is
faster):

| Kernel | RV32 | MIPS | MIPSel | ARM | Thumb | Thumb BE8 | PPC | x86 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| crc | 1.03 | 1.01 | 1.03 | 1.01 | 1.08 | 1.09 | 1.01 | 1.58 |
| sort | 0.99 | 1.00 | 1.03 | 1.01 | 1.02 | 1.01 | 1.01 | 1.02 |
| matmul | 0.99 | 1.00 | 1.00 | 1.02 | 1.20 | 1.22 | 1.00 | 2.11 |
| string | 1.01 | 1.02 | 1.03 | 1.03 | 1.04 | 1.04 | 1.02 | 1.34 |
| list | 1.03 | 1.03 | 1.03 | 1.01 | 1.04 | 1.05 | 1.01 | 1.29 |
| u64 | 1.10 | 1.13 | 1.15 | 1.28 | 1.64 | 1.60 | 1.13 | 3.32 |
| fmt | 1.04 | 1.08 | 1.03 | 1.04 | 1.26 | 1.22 | 1.04 | 1.93 |
| **geomean** | 1.03 | 1.04 | 1.04 | 1.05 | 1.17 | 1.16 | 1.03 | 1.68 |

The interp kernel is left out: it is bimodal between runs on every build (above).
Parity, as each guest's geometric-mean kernel time over RV32's (interp left out; the
ratio matrix overstates the others now that x86, with native 64-bit arithmetic, is the
fastest guest on u64):

| | MIPS BE / LE | PPC | ARM | Thumb LE / BE8 | x86 |
|---|---:|---:|---:|---:|---:|
| before step 9 | 1.12 / 1.12 | 1.23 | 1.30 | 1.48 / 1.50 | 2.27 |
| after | 1.11 / 1.10 | 1.23 | 1.27 | 1.31 / 1.33 | 1.39 |

x86 is still the slowest guest on most kernels (crc 1.78x, matmul 1.71x, fmt 1.74x of
RV32; not yet profiled after this step). Thumb's gap to ARM is down to 3-4%.

### After step 10

Step 10 gives computed exits a shared jump table, so an EXIT with many targets chains
too. On the interp kernel, whose switch is such an EXIT, the share of blocks entered by
chaining goes from 70% to 99.9% on every guest, and its time drops 2.9-5.5x (RV32
0.68 -> 0.13 s, x86 0.48 -> 0.17 s); it is no longer bimodal. The other kernels are
unchanged (within +-2%, run-to-run noise). Each kernel's time after the step
(`--per-kernel --times --rounds 1600`, medians of 5):

| Kernel | RV32 | MIPS | MIPSel | ARM | Thumb | Thumb BE8 | PPC | x86 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| crc | 0.118 | 0.137 | 0.139 | 0.151 | 0.161 | 0.161 | 0.128 | 0.211 |
| sort | 0.650 | 0.761 | 0.749 | 0.874 | 0.856 | 0.859 | 0.858 | 1.007 |
| matmul | 0.092 | 0.102 | 0.102 | 0.125 | 0.127 | 0.131 | 0.093 | 0.161 |
| string | 0.309 | 0.328 | 0.331 | 0.417 | 0.455 | 0.453 | 0.408 | 0.491 |
| interp | 0.133 | 0.121 | 0.122 | 0.152 | 0.155 | 0.157 | 0.151 | 0.166 |
| list | 0.169 | 0.197 | 0.191 | 0.240 | 0.242 | 0.246 | 0.222 | 0.262 |
| u64 | 0.051 | 0.054 | 0.054 | 0.051 | 0.053 | 0.053 | 0.065 | 0.025 |
| fmt | 0.083 | 0.092 | 0.093 | 0.105 | 0.105 | 0.104 | 0.107 | 0.148 |
| **geomean** | 0.146 | 0.159 | 0.159 | 0.185 | 0.190 | 0.191 | 0.177 | 0.201 |

Parity over RV32, now with the interp kernel included: MIPS 1.09 / 1.09, PPC 1.21,
ARM 1.26, Thumb 1.30 / 1.31, x86 1.38.

### After step 11

68% of the VxWorks run was one 4-instruction block: load a word, compare, branch back
(its idle loop). Step 11 skips the passes of such loops up to the next event instead of
running them, which leaves exactly the state running them would. VxWorks (4e9
instructions) takes 0.51 s, from 1.72 s; OVMF waits by polling I/O ports, which reads
devices, so it runs as before; the bench kernels do not spin and are unchanged.

### After step 12

A page that holds translated code had no data-TLB write entry, so every store to it took
the C path even when it missed the code lines. The bench keeps its data next to its code
(about 30 M such stores on sort on every guest, 73% of RV32 sort's time). Each kernel's
time before over after (`--per-kernel --times --rounds 1600`, medians of 5):

| Kernel | RV32 | MIPS | MIPSel | ARM | Thumb | Thumb BE8 | PPC | x86 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| crc | 1.55 | 1.62 | 1.60 | 1.53 | 1.59 | 1.62 | 1.65 | 1.57 |
| sort | 2.80 | 2.90 | 2.95 | 2.37 | 2.50 | 2.43 | 2.62 | 2.86 |
| matmul | 1.19 | 1.21 | 1.19 | 1.16 | 1.17 | 1.17 | 1.22 | 1.19 |
| string | 1.18 | 1.23 | 1.21 | 1.17 | 1.20 | 1.19 | 1.17 | 1.24 |
| interp | 0.99 | 0.98 | 0.99 | 0.99 | 0.99 | 0.99 | 0.97 | 0.99 |
| list | 1.68 | 1.71 | 1.76 | 1.53 | 1.56 | 1.53 | 1.66 | 1.83 |
| u64 | 0.98 | 0.98 | 1.00 | 1.00 | 0.98 | 1.00 | 0.99 | 1.00 |
| fmt | 1.00 | 0.99 | 0.99 | 1.00 | 0.99 | 0.99 | 1.00 | 1.00 |
| **geomean** | 1.33 | 1.36 | 1.36 | 1.29 | 1.30 | 1.30 | 1.33 | 1.36 |

Geometric-mean kernel time per guest after it: RV32 0.110 s, MIPS 0.117 / 0.116,
PPC 0.133, ARM 0.143, Thumb 0.145 / 0.147, x86 0.149. The workloads store little next
to their code and are unchanged.

Not done yet:
- **The x86 gap** (x86 at 1.4-1.8x RV32's kernel time): writes live at block exits
  and narrow register reads that are not forwarded. Proposed as step 13 in
  docs/jit-carry-design.md, for review.
- **Translation cost on OVMF**: 0.23 s of 1.69 s.
- **"Option B" for icount.** Drop `inc r13` per instruction and check the deadline
  once at block entry. It changes how stopping at a deadline is implemented, so it
  needs sign-off.
