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
  multi-target cache for computed exits would help every ISA alike.
- RV32, MIPS and PPC are within 1.3x of each other everywhere; endianness costs nothing
  measurable (MIPS BE/LE, Thumb LE/BE8).

Not done yet:
- **Dead flag stores across instructions (step 9 of the plan).** This defers PUTs that
  a later instruction overwrites, and replays them at interrupt exits and on memory
  slow paths. Its gate (flag work at least 10% of OVMF time) is met, but it needs a
  design review first.
- **"Option B" for icount.** Drop `inc r13` per instruction and check the deadline
  once at block entry. It changes how stopping at a deadline is implemented, so it
  needs sign-off.
