# JIT performance: measuring it, and where it stands

This note is about the x86-64 backend (`src/exec/jit_x86.c`). It covers the tools for
measuring the JIT, the reference workloads, and a log of what each change bought.

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

## Reference workloads

Two firmware images, each run on a board kept outside this repository and built
against the tree under test. Both runs use a fixed instruction budget and no input.
- **x86-64 UEFI boot.** An installed OVMF booted on a Q35-class board to the UEFI shell,
  1e9 instructions, 512 MiB RAM. This exercises SEC/PEI decompression, DXE drivers,
  I/O-port polling, and x86 flag computation.
- **MIPS32 BE VxWorks 5.5.1.** A router OS image, 4e9 instructions. The boot banner
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
