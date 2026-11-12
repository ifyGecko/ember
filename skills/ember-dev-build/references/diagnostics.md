# Diagnostics: ember's logs, traces and probes, and how to use them for bring-up

Ember tells you what the firmware wants from the hardware, if you ask the right question.
This page lists every logging and tracing mechanism the runner has (`uam-run`, and every
board runner built on `uam_run_main`, so `run.sh` takes all of them), what each line means,
and which one answers which bring-up question. `iteration-loop.md` says *when* in the loop
to use them; this page says *which* and *how*.

## Ground rules

- **Everything diagnostic goes to stderr; the guest console goes to stdout.** Keep them
  apart (`> build/console.txt 2> build/emu.log`). `--trace-file FILE` moves the traces
  (and `--watch`, `--history`) out of stderr into FILE, so a big trace never buries the
  log lines and the stop line that `log_summary.py` reads.
- **Off unless asked.** A plain run prints only the catch-all logs below and the stop line.
  Every trace and probe is a switch; turn on the one or two that answer the current
  question. `-t all` on a long boot produces gigabytes.
- **The icount is the common clock.** Runs are deterministic, and every trace line carries
  the instruction count at which it happened (`icount=N`), plus `cpu=N` on a processor
  other than 0. `--log-stamp` prefixes the log lines too (`@N [warn] ...`). So lines from
  different traces sort into one timeline, and any icount you read can be fed back:
  `-n N -d` stops exactly there, `--trace-after N` starts tracing there, and the run after
  your next edit reaches the same icount until your change takes effect.
- **Interpreter and `--jit` print the same traces.** If they differ, that is an ember bug:
  stop and report it. `--trace blocks` and `--history` make the JIT run block by block
  (no chaining), so they cost speed; the other traces do not change how code runs.
- **Narrow before you widen:** a range (`--trace-mmio-range LO:HI`, `--trace-range LO:HI`),
  a start (`--trace-after N`, or `+N` from the run's start), a summary instead of a stream
  (`mmio-summary`), a bounded run (`-n`).

## 1. Always on: the catch-all logs and the stop line

These come from the board's fallbacks and need no switch. Each first access names the
instruction that made it (`pc=0x.. icount=N`): decompile that pc.

| line | from | tells you |
|---|---|---|
| `[warn] unimp: WIN+0xOFF (0xPHYS): first read\|write of S bytes[: 0xV] pc=.. icount=N` | a catch-all `unimp` window over a RECON MMIO range | the first touch of each unmodelled register, in time order: the next device to write, and which register first |
| `[warn] unimp: WIN: N accesses to K unimplemented offsets; hottest:` then `[warn] unimp:   WIN+0xOFF: R reads, W writes (last 0xV)` | the same, at exit | the hottest offsets: a read offset with millions of reads is a status bit polled in a loop |
| `[warn] unknown-mmio: first read\|write of S bytes at 0xADDR ... pc=.. icount=N` | `--unknown-mmio zero` / `echo` (run.sh's default is `zero`) | an address no window covers: a window RECON missed, or a garbage pointer from a wrong base |
| `[info] mmio-stub: RULE (POLICY): first read ...` (needs `-v`) and a per-rule summary | `--mmio-rule` | your hypothesis registers being used |
| `[warn] master-abort: first read ...` | the PC board's unclaimed-address device | an x86 address or I/O port nothing decodes (`pc-platform.md`) |
| `[error] ISA: pc=..: load from nothing at 0x..: core halted` | AVR, 8-bit PIC, 8051, 6502, Z80 | these cores have no bus error: the hart stopped on an access nothing answers |
| `x86: triple fault ...`, `x86: ... hart shut down` | the x86 core | the processor shut down; the *first* exception before it is the blocker |
| `[warn] chardev: console input: N byte(s) never sent, waiting for '...'` | scripted console input | the prompt never came |
| `[error] snapshot: ...`, `[error] abi: ...` | snapshots, the library check | a device without save/restore; a board built against other headers |
| `RUNNER: REASON (status S) at pc=0x.. after N instructions` | the runner, always last | how the run ended: the exit code table in SKILL.md / `iteration-loop.md` |
| `stats: ...` | `-s` / `--stats` (run.sh passes it) | instructions, exceptions, interrupts, idle ticks vs instructions (a hart that mostly idled), time |

## 2. Traces (`-t KIND[,KIND...]`, repeatable)

| kind | each event | at the stop | answers |
|---|---|---|---|
| `mmio` | `mmio: r\|w SIZE 0xADDR DEV+0xOFF = 0xVAL pc=.. icount=N` for every access a device serves (MMIO, a ROM device in device mode, unmapped addresses, x86 ports at `0x10000000000 + port`); `(fault)` when it failed | the per-register summary (next row) | the register protocol a driver speaks: order, values, which pc |
| `mmio-summary` | nothing | `mmio:   0xADDR DEV+0xOFF: R reads (always\|last 0xV), W writes (last 0xV), first pc=..` for every register touched, sorted by address, then `mmio: most accessed: ...` | the register map the firmware uses (cheap even on a long boot); the register a stalled run polls ("always" on a hot register is the value it keeps getting) |
| `exc` | `exc: kind=K (NAME) pc=.. cause=.. tval=.. icount=N`; `irq: entered handler at 0xH from pc=0xF lines=0xL icount=N`; `reset: system\|cpu icount=N` | `exc:   NAME cause=.. pc=..: N times, last tval=.., first icount=..` per (kind, cause, pc); `irq:   0xH: N times, last lines=..` per handler | where control went; an exception storm in one line; which interrupts the firmware actually takes and how often |
| `irq` | `irq-in: cpuN line L = 0\|1 icount=N (cpuM at pc=..)` for every change of a processor's interrupt input | each line's rises and falls and its level; `irq-in: at the stop cpuN has an interrupt pending and enabled ...` | did the device raise its line, did it reach the CPU, is it masked |
| `sysreg` | `sysreg: first read\|write 0xID = 0xV pc=.. icount=N` (the first of each register, and every access that faults: the guest then takes an exception) | `sysreg:   0xID: R reads, W writes, first at pc=..` | which core and implementation registers the firmware relies on (ids as `uam/isa/<arch>.h` numbers them): CPU identity, caches, MMU, timers |
| `sysreg-all` | every system register access | as `sysreg` | a register whose value sequence matters |
| `console` | `console: icount=N pc=.. "text"` per line the guest prints | | where in the run a message was printed: aim `--trace-after`, `-n N -d` and `--snapshot-at` at a console line |
| `dma` | `dma: r\|w LEN 0xADDR REGION+0xOFF pc=.. icount=N` for every DMA transfer a device makes; `(fault)` when no RAM is there | | descriptor rings and buffers of network, storage and DMA engines: what the device reads and writes, and whether the addresses the driver gave it are sane |
| `blocks` | `blk 0xPC icount=N` per block executed (narrow it: `--trace-range LO:HI`, `--trace-after N`) | | control flow through a region |
| `ir` | the IR of each translated block | | ember lifter questions (reports, not bring-up) |
| `all` | `exc,mmio,sysreg,irq,console,dma` | | a first look at a *short* run |

An unknown name is an error (exit 2) and lists the valid ones. `--trace-after N` holds
back every trace but `ir` until icount N, and the summaries and sysreg's "first" count
from there. `--trace-mmio-range LO:HI` limits `mmio` and its summary to physical
addresses in [LO, HI).

## 3. Probes

| probe | does | use it for |
|---|---|---|
| `--unknown-mmio stop` | the first access to an address nothing decodes does not happen; the run stops at that instruction (exit 131) with `RUNNER: unmapped read\|write of S bytes at 0xADDR by cpu N pc 0x.. (icount N)` | the fastest bring-up loop on a new board: each run stops exactly at the next unmodelled register, with `-d`, `--dump-mem` and `--save-snapshot` capturing the machine *before* the access |
| `--unknown-mmio zero` / `echo` / `fault` | answer unmapped accesses with 0 / the last value written / a bus fault for the guest | `zero`: survey all unmodelled addresses in one run; `fault`: hardware-faithful (and what fuzzing uses) |
| `--history N` | keeps each processor's last N events (blocks entered, exceptions taken, interrupt handlers entered) and prints them at the stop: `history: cpuN: the last K events ...` then `history:   blk\|exc\|irq ...` | "how did it get here": the path into a fault loop, a deadlock, a guard or unmapped stop, a breakpoint, or simply the stop at `-n`. Far cheaper than `--trace blocks` |
| `--log-pc ADDR[,REG...]` | `RUNNER: pc 0xADDR reached (hit H, icount N) a0=0x.. ra=0x..` at each arrival | a function's calls and arguments; a loop's iteration count. Register names as the ISA's gdb description has them (`a0` on RISC-V, `r4` on MIPS, `r0` on ARM, `rdi` on x86-64, `r3` on PowerPC); an unknown name lists them |
| `--break ADDR` (+ `-d`) | stop when ADDR is reached (exit 127) and dump the processors | the state at a point of interest |
| `-n N -d` | stop at exactly instruction N and dump every processor | the state at an icount from any trace line |
| `--dump-mem [p:]ADDR+LEN[=FILE]` | hex-dump (or save) guest memory when the run stops | a descriptor ring, a buffer, a structure the firmware built |
| `--watch [p:]ADDR+LEN` | log every data access to those bytes: `guard ... (log): cpu N pc .. read\|write S at .. value .. icount=N` | who writes this variable, and with what |
| `--guard [p:]ADDR+LEN[:w]` | stop *before* the first access there (exit 129) | the instruction that corrupts memory |
| `--mmio-rule ADDR=POLICY` (+ `-v`) | answer one register with a fixed value, an echo, a set/clear mask, ... | test a register hypothesis in one run before writing C |
| `--map` | print the bus map the board built | is the window where you think it is |
| `--snapshot-at PC --save-snapshot F` / `--load-snapshot F` | bookmark a state and start later runs there | skip a long boot; trace only the part you are working on |
| `--gdb PORT` (`scripts/gdb_trace_calls.py`) | a gdb remote stub (breakpoints, watchpoints) | interactive digging, call tracing without stopping the guest |
| `-v` / `-v -v` | more logging (`mmio-stub` first accesses; POST codes on a PC) | |

## 4. Recipes: which tool for which bring-up question

**First runs of a new board: what does it want?**
1. `./run.sh -n 50000000 -t mmio-summary,exc --history 32` with `--unknown-mmio stop`:
   the run stops at the first register nothing models, names its pc, and the history shows
   the code path into it. Decompile that pc; add the window or a stub device; repeat. Each
   iteration costs one run and needs no guessing about *which* register comes next.
2. When stops come too often to handle one at a time (a driver probing a whole block),
   switch to `--unknown-mmio zero` and read every first access of the run in time order
   (`log_summary.py` "first distinct events"), plus the `mmio-summary` lines for the
   registers your models *do* serve.

**Writing a device model from what the firmware does:**
- `-t mmio --trace-mmio-range BASE:BASE+SIZE --trace-file build/dev.trace` lists every
  access to the block in order with values and pcs: the init sequence, the reset values it
  expects (reads), the configuration it writes, and the pcs of the driver's functions
  (decompile them for the bit meanings).
- `-t mmio-summary` over the same range gives one line per register: which ones matter at
  all (model those), which are write-only configuration (a stub that stores them), which are
  read in loops.
- Test the semantics with `--mmio-rule` (`fixed:V`, `echo,set=0x1`, ...) and `-v` before
  writing C; then put them in the device and keep its `save`/`restore` in step.

**A stalled run (exit 124 with the budget used, or a hang):**
- `-t mmio-summary`: the most accessed register, with reads "always 0xV", is the one a
  polling loop waits on, and V is what it keeps getting. Decompile the loop (its pc is in
  `--history 64`, or `-t mmio --trace-mmio-range` over that register) for the bit it wants,
  then `--mmio-rule ADDR=fixed:WANTED` to confirm.
- No hot register: the loop spins on RAM or on a timer that never moves. `--history 64`
  shows the loop's blocks; `--log-pc LOOP_PC,REG` shows the counter or pointer it checks;
  `--watch` the RAM word it reads to see who should have written it.

**Deadlock (125) or livelock (128): interrupts.**
- `-t irq,exc`: does any CPU input line rise (`irq-in:` lines), and at the stop is an
  interrupt pending and enabled? Are handlers entered (`irq:` summary) and how often?
- No line ever rose: the timer or device never asserts, or is not wired: `-t mmio` on the
  timer and the interrupt controller shows what the guest programmed and unmasked.
- A line rose but no handler entry: masked in the CPU (`-t sysreg` shows the status/mask
  register writes) or wired to the wrong input.

**Exceptions, crashes, a fault loop (126):**
- `-t exc`: the summary lists every (kind, cause, pc) with its first icount; the earliest
  one is usually the real blocker, a big count at one pc is a storm.
- `--history 64` shows the blocks leading to the stop; `-n FIRST_ICOUNT -d` (from the
  summary) dumps the registers at the first fault. `--trace blocks --trace-after N
  --trace-range LO:HI` when you need every block of a stretch.

**A wrong value in memory (a size, a pointer, a status):** `--watch ADDR+LEN` (every
access, with pc and value) or `--guard ADDR+LEN:w` (stop before the first write), then
decompile the writer. gdb `watch` does the same interactively.

**Which function fails:** `--log-pc` on the candidates with their argument and return
registers (`--log-pc 0x80012340,a0,a1,ra`), or `scripts/gdb_trace_calls.py`; a call that
returns an error code leads to the device it needed.

**DMA devices (network, storage, DMA engines):** `-t dma,mmio` shows the descriptor ring
address the driver programs (an MMIO write), then the device's reads of descriptors and
writes of buffers and status. A `(fault)` DMA line means the address is not RAM: a wrong
base or descriptor format. `--dump-mem` the ring at a `--break` to read it whole.

**System registers and CPU identity:** `-t sysreg` lists the first read and write of each
register with its value: the identity registers the firmware checks (PRId, MIDR, PVR,
x86 MSRs), cache and MMU setup, and every access that faults (a register the CPU model
lacks). Match the CPU model to what it expects. On the PC template, `-o msr-trace=1` also
logs every MSR no device answers (`pc-platform.md`).

**Long boots:** `-t console` gives the icount of each console line; take a snapshot past
the stable part (`--snapshot-at PC --save-snapshot`), start every run from it with
`--load-snapshot`, and aim `--trace-after ICOUNT` (absolute, as printed) at the stretch
you are working on.

**Several processors:** every trace line ends with `cpu=N` for processors other than 0;
`--history` prints each processor's events; `-d` dumps all of them.

**Fuzzing triage** (ember-fuzz): in `--fuzz-replay` each input gets its own trace section
(`trace: input ID`) and summaries, and `--history N` prints for every input that is not ok.

## 5. Cost and size

| cheap (fine on long runs) | moderate | heavy (narrow it first) |
|---|---|---|
| the catch-all logs, `-s`, `mmio-summary`, `console`, `exc` on a boot without timer interrupts, `sysreg`, `irq` (unless a timer line toggles constantly), `--unknown-mmio stop`, `--log-pc` on a rare pc, `--watch`/`--guard` on cold memory | `--history N` (no chaining with `--jit`), `exc` once timer interrupts run (a line per interrupt; the summary stays short), `dma` on a busy NIC | `mmio` without a range on a polling loop, `blocks`, `sysreg-all`, `ir`, `all` |

Write heavy traces to a file (`--trace-file`), and grep them; `log_summary.py` reads the
summary lines, the first events and the stop line, not millions of access lines.

## 6. `log_summary.py`

`python3 scripts/log_summary.py build/emu.log` condenses the stderr of a run into the next
blocker: how the run ended (with the exit code's meaning), the first distinct events in
time order (first accesses, first exceptions, the unmapped stop), the hottest catch-all
offsets, unmapped pages, exceptions by kind and pc, and, when the run had them, the
`mmio` register summary (with likely polls marked), the `exc` summary, `irq` lines, `dma`
per region, the `--history` tail and `--log-pc` registers. Read it top-down; the first
event is usually the blocker.
