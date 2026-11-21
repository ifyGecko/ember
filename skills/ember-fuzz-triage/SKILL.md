---
name: ember-fuzz-triage
description: "Work out what a crashing fuzz input does to firmware running on an ember (UAM) emulation: where it crashes, how execution got there, why (which input bytes, which values), and whether it is a firmware bug or an emulation gap. Use when a fuzz campaign (ember-fuzz, the fuzz brain, --fuzz-replay) left crash, hang or timeout inputs and someone has to explain them. Generic to every ember board and ISA: replaying one input, reading verdict lines (crash-fault, crash-guard, crash-oracle, hang, timeout), --history, --backtrace, traces, --watch/--guard, --on rules, --log-pc, --dump-mem, reproducing the crash as a plain run from a snapshot, and driving it from gdb (--gdb, target remote, breakpoints, watchpoints, stepping, memory and registers, scripted sessions). Step by step for a newcomer or a small model; ends in a report. Triggers: 'triage this crash', 'why does this input crash', 'debug a fuzz crash', 'connect gdb to ember', 'what does this crash-fault mean', 'reproduce a fuzz crash'. Finding crashes: ember-fuzz."
---

# ember-fuzz-triage: from a crashing input to an explanation

A fuzzer hands you a file and says "this crashed". This skill turns that into an answer a
developer can act on:

| Question | You are done when you can say |
|---|---|
| **What** happened? | the verdict, in one line: "a store to address 0x40000000 faulted" |
| **Where?** | the instruction (pc), the function it is in, and the source line if there is one |
| **How** did execution get there? | the call chain, and the path taken through the function |
| **Why?** | which bytes of the input produced the bad value, and the check that is missing or wrong |
| **So what?** | firmware bug or emulation gap; what an attacker controls; the smallest input that shows it |

Nothing here depends on a particular board. It uses only what every ember emulation has:
the runner's options and its gdb stub.

**Words used below**

- `RUN`: the command that runs your emulation without fuzzing. In an ember-dev-build
  project that is `./run.sh`; with ember's own boards, `build/uam-run -b BOARD IMAGE`.
- `FUZZ`: the same command plus the campaign's fuzz options (`--fuzz-begin`,
  `--fuzz-input`, `--fuzz-crash-pc`, ...), but **without** `--fuzz-replay` or
  `--fuzz-net`. In a project that is `./fuzz.sh`. You need the exact options the campaign
  used: ask for them, or read `fuzz.sh` or the journal (`fuzz-XX.md`).
- *begin pc*: the address given to `--fuzz-begin`. Every input starts there.
- *verdict*: what the harness decided about one input (table in step 2).
- *icount*: the count of guest instructions executed so far. ember prints it everywhere,
  and it is the same in every run, so it works as a timestamp.

References, read when the step sends you there:
`references/verdicts.md` (what each verdict and exception means, typical causes),
`references/flags.md` (every useful runner option, with the output it prints),
`references/gdb.md` (connecting gdb, a command cheat sheet, scripted sessions, pitfalls),
`references/report.md` (the report to fill in). Two scripts: `scripts/triage.py` (the
first pass, step 4) and `scripts/minimize.py` (the smallest input, step 8).

## Rules that keep you out of trouble

1. **Reproduce before you reason.** Replay the input and see the verdict yourself. Never
   explain a crash from its file name or from someone's description.
2. **One input at a time.** Pick the smallest input of a group and stay with it.
3. **Change nothing but what you are testing.** Same board, same image, same options as
   the campaign. A different `--unknown-mmio` policy or a missing `--fuzz-crash-pc` gives
   a different verdict.
4. **ember is deterministic.** The same input gives the same instruction count and the
   same registers every time. If two runs differ, stop and find out why first (step 3).
5. **The crash site is rarely the cause.** The instruction that faults is where the
   damage showed. The bug is where the bad value was made, usually earlier.
6. **Write down every command that told you something**, with its output, as you go. The
   report is assembled from those notes.
7. **Bound every run** (`timeout 120 ...` in front, or `-n N`): a guest that loops runs
   until it is stopped.

## Step 1: collect what you need

- The input file or directory. The fuzz brain saves crashes as
  `crashes/crash_r<reason>_s<status>_<sha1>` with an `.info` note beside each, hangs
  and timeouts under `hangs/`, and (a campaign run with `--fuzz-uninit`) inputs that read
  memory nothing wrote under `uninit/`.
- The `FUZZ` command line.
- The firmware image, as an ELF with symbols if one exists. With a raw image you work
  with addresses and a disassembler (Ghidra) open beside you.
- `gdb-multiarch` (or the gdb of the target's toolchain), `xxd`, `nm`, `objdump`.

Look at the input before running it:

```bash
xxd crash_input | head          # the bytes; is it text, a packet, a few bytes?
wc -c crash_input               # its length
```

## Step 2: replay, and group

```bash
timeout 600 FUZZ --fuzz-replay crashes/ < /dev/null > replay.txt 2> replay.log
cat replay.txt
```

One line per input:

```
replay: crashes/a: crash-fault pc=0x800003a6 (store-fault cause 0x0 tval 0x40000000) insns=98 buckets=16
replay: crashes/b: crash-oracle pc=0x80000272 insns=99 buckets=17
replay: crashes/c: timeout insns=100000 buckets=16
```

| Verdict | Meaning in one sentence |
|---|---|
| `ok` | the input ran to the end of the iteration; nothing was detected |
| `crash-fault` | the processor took an exception the campaign counts as a crash (a bad load, store or fetch) |
| `crash-guard` | an access touched guarded memory (a redzone, a `--guard`, the guarded heap, a bad return) and was stopped before it happened |
| `crash-oracle` | the firmware reached a "this is fatal" address (`--fuzz-crash-pc`), reset, halted with an error, or raised a watched interrupt |
| `hang` | the firmware waits for something that can never come |
| `timeout` | the input used up its instruction budget (`--fuzz-iter-insns`): probably a loop |
| `rejected` | the input could not be delivered (too long for the route); nothing ran |

`pc=` is where it stopped, `insns=` how many instructions the input ran from the begin pc,
`buckets=` how much coverage it touched. `references/verdicts.md` explains every form of
these lines.

Many files are often one bug. Group them and take each group's smallest input:

```bash
python3 <ember>/skills/ember-fuzz/scripts/replay_summary.py replay.txt
```
```
4 inputs: 1 crash-fault, 1 crash-oracle, 1 timeout, 1 ok

crash-fault  pc=0x800003a6 (store-fault cause 0x0)  x1         <- one group: verdict, location, how many inputs (x1)
    in/2-store  (4 bytes, 98 insns, 16 buckets)                <- its smallest input: triage this one
```

If an input you were told crashes prints `ok`, your options differ from the campaign's
(rule 3). Fix that before going on.

**A firmware that takes long to boot**: every replay boots it again. Add
`--fuzz-snapshot triage.snap` to `FUZZ`: the first replay saves the state at the begin pc,
the later ones start from it (`starting from the begin snapshot ... (no boot)`). Use a
file of your own, never the campaign's: an option that changes the machine (`--backtrace`,
`--guard-returns`, `--unknown-mmio`, ...) makes ember boot again and **replace** the file
(`was made from another firmware or command line: booting and replacing it`). Probes that
only observe (`--history`, `-t`, `--watch`, `--on ... log`) reuse it.

## Step 3: confirm it is repeatable

```bash
FUZZ --fuzz-replay INPUT < /dev/null | grep '^replay:'          # twice: identical lines
FUZZ --jit --fuzz-replay INPUT < /dev/null | grep '^replay:'    # the JIT: identical again
```

- Two interpreter runs that differ: something outside the guest feeds the run (a console
  on a terminal, a network port on a host interface). Use the campaign's console setting.
- The interpreter and `--jit` disagree: that is an ember bug, not a firmware bug. Report
  it with the input and both lines, and triage with the interpreter.

## Step 4: the first pass, in one command

```bash
python3 <this skill>/scripts/triage.py INPUT -- FUZZ
```

It runs the checks of step 3 and one more replay with the diagnostic options on, and writes
`triage-<input name>/report.md` with the verdict, the last blocks executed, the call
chain, the exceptions taken and the registers at the stop. Read that file. The same by
hand:

```bash
FUZZ --history 32 --backtrace -t exc -d --fuzz-replay INPUT < /dev/null 2>&1 | tee first-pass.txt
```

What each part tells you (real output, from a planted bug):

```
exc: kind=3 (store-fault) pc=0x800003a6 cause=0x0 tval=0x40000000 icount=517
history:   blk 0x80000356 icount=511          <- the blocks executed just before, oldest first
history:   blk 0x800003a2 icount=515
backtrace: #0 0x80000282 <parse> called, returns to 0x800003fa <fuzz_iteration+0x42>
backtrace: #1 0x800003b8 <fuzz_iteration> called, returns to 0x8000043a <main+0x32>
pc=800003a6 ... a5=40000000 ...               <- the registers when it stopped (-d)
```

- **What**: `store-fault ... tval=0x40000000`: a store to address 0x40000000 faulted.
- **Where**: `pc=0x800003a6`, inside the function at the top of the backtrace (`parse`).
- **How**: `main` called `fuzz_iteration`, which called `parse`; the `history` lines are
  the path through it.

Two things in a replay's output that look wrong and are not. The summary may say
`exc: 0 exceptions taken` right after an `exc: kind=...` line, and the fault registers in
the `-d` dump (`mcause`/`mtval`, `cause`/`badvaddr`, `dfsr`/`dfar`) are 0: the harness
stopped the input **at** the exception, before the processor took it. The cause and the
address are in the `exc:` line and in the verdict.

That answers three of the five questions. If a line is missing, `references/flags.md`
says which option prints it and what stops it from appearing.

## Step 5: find the code

With an ELF:

```bash
addr2line -f -e fw.elf 0x800003a6            # the function, and file:line with debug info
nm -n fw.elf | less                          # all symbols by address: the one just below the pc
objdump -d --disassemble=FUNCTION fw.elf     # that whole function; find the pc in it
```

Disassemble from the start of a function, as above, not from an address near the pc: on
an architecture with instructions of different lengths, starting in the middle of one
prints garbage. The function's name must be spelled as `nm` shows it.

(Use the target's own `objdump` if the host's does not know the architecture:
`riscv64-linux-gnu-objdump`, `arm-none-eabi-objdump`, ...)

With a raw image, go to the pc in Ghidra and read the decompiled function. Either way,
write down: the function's name or address, what the faulting instruction does, and which
register held the bad value. In the example the instruction is `sw s0,0(a5)`: it stores
through `a5`, and `a5` is 0x40000000. The question for the rest of the triage is now
precise: **where did `a5` get that value?**

**On ARM, an odd pc is Thumb code.** ember prints and takes Thumb addresses with bit 0
set (`pc=0x100289`); `objdump`, `addr2line`, Ghidra and gdb use the address without it
(0x100288). `references/flags.md`, "Addresses on each architecture", has this and the
other per-architecture differences (cores without exceptions, separate code and data
spaces); read it once if your target is not a flat 32-bit one.

## Step 6: reproduce it as a plain run

A replay cannot be paused. To stop the guest and look around, run the same input without
the fuzz harness, starting from the state every input starts from.

**The plain run must be the campaign's machine.** `RUN` here means the runner with every
board option the campaign has: the images, `-o` options, `--mmio-rule`s, and above all the
same `--unknown-mmio` policy. In an ember-dev-build project `run.sh` defaults to
`UNKNOWN_MMIO=zero` (for bring-up) while `fuzz.sh` sets `fault`: with `zero` a wild access
reads 0 and nothing crashes. So in a project, every `RUN` below is

```bash
UNKNOWN_MMIO=fault ./run.sh ...        # or whatever fuzz.sh exports
```

**6a. Save a snapshot at the begin pc**, once. `BEGIN_PC` is the value the campaign gives
`--fuzz-begin`, an address or an ELF symbol, copied as it is. Add every state-changing
option you will want later (see the box):

```bash
RUN --backtrace --snapshot-at BEGIN_PC --save-snapshot begin.snap --break BEGIN_PC
```

It stops with `breakpoint at ...` (exit status 127); that is the expected result. If the
campaign needs console input to reach the handler (`fuzz.sh` has a `--console script:` or
`string:`), give the same here.

If the campaign uses `--fuzz-begin-skip N` or several `--fuzz-begin`, the first arrival at
that pc is not the state the inputs start from. Let the harness make the snapshot instead,
with the options you want in the plain run, and load that file:

```bash
FUZZ --backtrace --fuzz-snapshot begin.snap --fuzz-replay INPUT < /dev/null
```

> **A snapshot loads only with the options it was saved with.** `--backtrace`,
> `--guard-returns`, `--fuzz-uninit`, the `--unknown-mmio` policy, `--link` ports, and
> `--on` rules with `count=` or `delay=` are part of the saved state. The error reads
> `snapshot has N core records ... the machine needs ...` (an option is missing from the
> snapshot; the hint about a processor count is only a guess) or `snapshot holds state for
> '...', which this machine does not have` (the snapshot has one you did not give). Save
> the snapshot again with the option, or load it with the same ones.

**6b. Run the input from the snapshot.** How depends on the campaign's `--fuzz-input`:

| Route | Feed the input in a plain run with |
|---|---|
| the console UART (`uart0`, `com1`) | `--console file:/dev/null,in=INPUT` |
| `mmio` | `--mmio-input INPUT` (and the same `--mmio-rule ...=input` rules) |
| `mem:ADDR[,len=LADDR]` | gdb: `restore INPUT binary ADDR`, then write the length (step 7, "Loading the input") |
| another board input, or a link port | stay in replay: every probe of step 8 works there. If the bytes end up in a buffer, load them there from gdb as for `mem:` |

```bash
RUN --backtrace --load-snapshot begin.snap --console file:/dev/null,in=INPUT -t exc -n 1000000
```

**6c. Check that it is the same run.** The crash must appear at the icount the replay
predicts: *the snapshot's icount* (printed when it was saved and when it is loaded) *plus
the replay's `insns=`*. In the example 419 + 98 = 517, and the plain run prints `exc: ...
icount=517`. If it does not match, the input is not arriving the way the fuzzer delivers
it; fix that before believing anything else.

A plain run does not stop at a crash the way the harness does. What it does instead
depends on the verdict:

- **crash-fault**: the processor takes the exception and the firmware's own handler runs,
  usually into a panic loop, so the run goes on until `-n` ends it (exit status 124). That
  is expected. Stop it yourself where the replay said: `--break 0xPC -d` prints the
  registers just before the faulting instruction executes. On a core that has no
  exceptions (AVR, 8-bit PIC, 8051, 6502, Z80) the core halts at the faulting instruction
  and the run ends as a deadlock (exit status 125) with the pc of the fault.
- **crash-guard**: the harness's guards are not there. Pass the same one: a `--guard`
  from `FUZZ` as it is, a `mem:` redzone as `--guard LO+LEN` with the range the replay
  line names (`guard mem-after [LO, HI)`), the guarded heap's `-o` options and
  `--guard-returns` as the campaign has them (they are part of the snapshot: 6a). The run
  then stops before the access (exit status 129; under gdb, a SIGSEGV there).
- **crash-oracle** at a `--fuzz-crash-pc`: `--break` that address.
- **hang**: the run ends by itself (exit status 125 or 128). **timeout**: `-n` ends it.

A `--break` that never fires on ARM Thumb code: give the address with bit 0 set, and if
the instruction is inside an IT block (`it`, `itt`, `ite` ... a few lines above it in the
disassembly), break on the IT instruction: ember stops only at the start of such a group.

## Step 7: look around with gdb

Start the run waiting for a debugger, then connect from a second terminal:

```bash
RUN --backtrace --load-snapshot begin.snap --console file:/dev/null,in=INPUT --gdb 1234
```
```bash
gdb-multiarch fw.elf -ex 'target remote :1234'
```

gdb learns the architecture and registers from ember, so nothing else needs setting. The
guest is stopped at the snapshot. A first session:

```
(gdb) break *0x800003a6          stop before the faulting instruction (the * means "address")
(gdb) continue
(gdb) bt                         the call chain
(gdb) info registers             all registers;  info registers a5  for one
(gdb) x/4i $pc                   disassemble 4 instructions from here
(gdb) x/16xb 0x80000530          16 bytes of memory in hex;  x/s ADDR  as a string
(gdb) info args                  the function's arguments (needs debug info)
(gdb) kill                       end the run
```

`references/gdb.md` has the full cheat sheet: breakpoints on functions and addresses,
watchpoints, stepping, changing registers and memory, saving memory to a file, running a
whole session from a script, several processors, and what each stop signal means.

Three things to know before you step:

- **Do not `stepi` over the instruction that faults, and do not `finish` out of a function
  that crashes.** On several architectures gdb steps by planting a breakpoint at the next
  instruction; the fault jumps to the exception handler instead, so gdb waits for ever.
  First put a breakpoint on the handler (`break *HANDLER`; the handler's address is the
  trap vector, for example `mtvec` in the `-d` output), then `stepi` or `continue`.
- **Names may differ from the source.** A register is named as gdb names it (`fp`, not
  `s0`, on RISC-V); a wrong name in `--log-pc` or `--on` makes ember list the valid ones.
  A function may be `parse.constprop.0` in the image; `nm fw.elf | grep parse` shows it.
- **Fault registers are often not in gdb's view** (`mcause`, `mtval`, `ESR`, ...). ember's
  `-d` and `-t exc` print them.

**Loading the input for a `mem:` route**, at the snapshot, before `continue`:

```
(gdb) restore INPUT binary 0xADDR                      copy the file into guest memory
(gdb) set {unsigned int}0xLADDR = 123                  its length, where len= points
```

## Step 8: follow the bad value back to the input

This is the "why". Work backwards from the crash site, one question at a time.

**Who wrote the bad value?** If it lives in memory (a pointer in a struct, a length on the
stack), watch that memory:

```
(gdb) watch *(unsigned int *)0x80004610        stops after every write, showing old and new value
(gdb) continue
```

or without gdb, as a log of every access with the pc that made it:

```bash
RUN ... --watch 0x80004610+4              # works in a replay too
```
```
guard 0x80004610+4 (log): cpu 0 pc 0x800003ec write 4 at 0x80004610 (+0) value 0x40000000 icount=436
```

**Where does the input sit, and who reads it?** Find the buffer the bytes land in (the
first argument of the parser, or what a `watch` on a likely buffer shows), then log every
read of it:

```bash
RUN ... --watch 0xBUF+LEN:r
```

The pcs in those lines are the code that parses the input, in order. The last reads before
the crash are usually the bytes that matter.

**What did the function see?** Log its arguments each time it is entered, and dump the
buffer it was given (this works in a replay, on any route):

```bash
FUZZ --on 'pc=0x80000282,log a0 a1,dump a1 a0' --fuzz-replay INPUT < /dev/null
```
```
rule pc=0x80000282: cpu 0 pc 0x80000282 a0=0x4 a1=0x80000530 icount=464
  0000000080000530  46 55 5a 21                                      |FUZ!|
```

**Which branch decided it?** In gdb, break at the function and step line by line (`next`)
or instruction by instruction (`stepi`), watching the registers the comparison uses. Or
trace the blocks of that function only: `-t blocks --trace-range LO:HI`.

**The bad value is not from the input at all.** Sometimes the faulting address or value
is a constant in the code (the disassembly shows it being loaded as an immediate just
before the crash), or comes from a device. Then the input did not *make* the value; it
only *chose the path* that uses it. The question becomes "which input bytes steer
execution here?":

- log the reads of the input buffer (`--watch 0xBUF+LEN:r`): each read is followed by a
  compare, and the pcs give you the compares in order;
- compare the block trace of this input with that of an input that does not crash
  (`references/flags.md`, "Comparing two inputs"): the first block that differs is the
  branch that decided;
- then ask whether the constant is right: `--map` says what is at that address on this
  board, and the checklist in step 9 whether hardware would have something there.

**Prove it by changing the input.** Once you believe byte N is what matters, flip it and
replay:

```bash
cp INPUT try; printf '\x00' | dd of=try bs=1 seek=N conv=notrunc 2>/dev/null
FUZZ --fuzz-replay try < /dev/null | grep '^replay:'
```

If the verdict changes as you predicted, you have the cause. If it does not, your theory
is wrong: go back, do not adjust the story to fit.

**Shrink the input.** The minimal reproducer is the input from which no byte can be
removed without changing the result. Do not do that by hand:

```bash
python3 <this skill>/scripts/minimize.py INPUT -- FUZZ        # writes INPUT.min
```
```
minimize: 100 bytes: crash-fault 0x800003a6 store-fault
minimize: 50 bytes (pieces of 50)
...
minimize: INPUT.min: 4 bytes after 10 replay runs; removing any one byte changes the result
```

It keeps the verdict, its pc and the kind of fault the same. Check the `.info` note
first: `minimized=yes` means the fuzz brain did it already. The brain minimises only the
first crash of a campaign at once; `minimized=later` or `peer` is an input as found, and
worth shrinking before you read it byte by byte. Every byte that is left matters, which
is half of the "why".

**Does it explain the other groups?** One bug often stops at several pcs, and step 2
grouped by pc. When you have the cause, replay the whole crash directory with the cause
taken away, for example a rule that puts a good value in the register just before the bad
use (`--on 'pc=0xPC,set a5=0x80000530'`, `references/flags.md`) or the `--mmio-rule` that
closes an emulation gap. The groups that turn `ok` are this bug; the ones that still
crash are others, to triage on their own.

`references/flags.md` lists the other probes (`--log-pc`, `--guard`, `--guard-returns`,
`--dump-mem`, `-t mmio`, `-t irq`) and when each is the right one.

## Step 9: firmware bug, or emulation gap?

Not every crash is a bug in the firmware. Decide which of these it is:

| It is | Signs | Do |
|---|---|---|
| **a firmware bug** | the bad value comes from the input, through code that does not check it (a length used unchecked, an index, a pointer taken from the packet) | report it (step 10) |
| **an emulation gap** | the bad value comes from a device: a register that reads 0 or a constant where hardware would give something else, an interrupt that never arrives, memory the real boot would have initialised | not a firmware bug. Give it to whoever maintains the board (ember-dev-build); replay again after the fix |
| **a harness artefact** | it only happens under the fuzzer's setup: an iteration cut short, a watchdog reset because the budget is longer than the watchdog period, a `--fuzz-crash-pc` on a path the firmware takes normally, an input longer than the real channel could deliver | fix the campaign's options (ember-fuzz), not the firmware |
| **an ember bug** | interpreter and `--jit` disagree; or the instruction's result is wrong by the architecture's manual | report to ember with the input and both outputs |

To tell the first two apart, look at where the value came from in step 8. If the fault
is an access to an address where the board has nothing (`RUN --map -n 1`; replaying with
`--unknown-mmio zero` makes the crash go away), ask whether the **real** device has
something there: if it does, the board is missing a device (an emulation gap); if it does
not, the firmware really does access nothing (a bug). Then check the
devices: `-t mmio` on the replay shows every device register the input's run read, with
the value returned. A register that "always reads 0x0" on the path to the crash, belonging
to a device nobody modelled, is the usual gap. `references/verdicts.md` has a checklist
for each verdict.

## Step 10: write it up

Fill in `references/report.md`, one report per distinct bug. It must let someone who was
not there reproduce the crash with one command and understand the cause without running
anything: the reproducer bytes, the exact command, the verdict line, the call chain, the
faulting instruction and registers, the chain from input byte to bad value, and the
classification with its evidence.

## When you are stuck

- **The replay says `ok`**: options differ from the campaign's; a crash oracle is missing.
- **`boot did not reach the begin pc`** (exit 2): the boot needs the campaign's console
  script or images; or `-n` is too small.
- **No `backtrace:` lines**: add `--backtrace`. Names need an ELF; otherwise it prints
  addresses, which Ghidra turns into functions.
- **The plain run does not crash where the replay did**: step 6c. The input is not
  arriving the same way, or the machine is not the campaign's: `--unknown-mmio` is `zero`
  where the campaign has `fault` (a project's `run.sh`), a guard is missing, or the
  snapshot is not the harness's (`--fuzz-begin-skip`).
- **A `--break`, `--on pc=` or `--log-pc` never fires**: not an instruction boundary; on
  ARM Thumb, bit 0 is missing or the instruction is inside an IT block (step 6).
- **Symbols do not match the addresses** (a bootloader that copies itself, a module
  loaded at run time): `references/gdb.md`, "Code that is not where the ELF says".
- **gdb says `Cannot execute this command while the target is running`**: the guest is
  running and has not hit a breakpoint. Press Ctrl-C to stop it, then look at `$pc`.
- **The crash is far from anything that looks wrong**: memory was corrupted earlier. Turn
  the corruption into an immediate stop: `--guard-returns` for a smashed return address,
  `--guard ADDR-16+16:w` around a variable that changes unexpectedly, the campaign's
  redzones or guarded heap for buffers (`references/flags.md`).
- **A timeout**: `--history 16` shows the loop's blocks; `references/verdicts.md`,
  "timeout", walks through finding why it does not end.
