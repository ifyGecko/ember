# Runner options for triage

Every option here belongs to ember's runner, so it works with any board: `uam-run`, and
the runner of any ember-dev-build project (`./run.sh`, `./fuzz.sh`). `RUN --help` prints
them all. The output shown is real, from a planted bug on an RV32 board; addresses and
register names will differ on yours.

Everything is **off unless asked for**, and everything goes to stderr unless noted.

**In a replay or in a plain run?** Most probes work in both. The exceptions:

| Only in a plain run (`RUN ...`) | Why |
|---|---|
| `--gdb PORT` | a replay cannot be paused (`--fuzz-begin does not combine with --gdb`) |
| `--save-snapshot`, `--snapshot-at` | the harness owns the snapshot |
| `--unknown-mmio stop` | its stop is not a verdict; a replay uses `fault` |
| `--break ADDR` as a stop | in a replay it makes ADDR a crash pc instead (see below) |

In a replay, traces come in sections: first the boot (`trace: the boot to the begin pc
ends`), then one per input (`trace: input 1`). Read from `trace: input` on.

## Pick the option by the question

| You want to know | Use |
|---|---|
| What was the verdict? | `--fuzz-replay INPUT` |
| Which instructions ran just before the stop? | `--history N` |
| Who called the function it crashed in? | `--backtrace` |
| Which exception, with which cause and address? | `-t exc` |
| The registers at the stop? | `-d` |
| The registers each time a pc is reached? | `--log-pc ADDR,REG,...` |
| Memory at the stop? | `--dump-mem ADDR+LEN` |
| Arguments and a buffer when a function is entered? | `--on 'pc=ADDR,log ...,dump ...'` |
| Who reads or writes this memory? | `--watch ADDR+LEN[:r\|w]` |
| Stop at the first write to this memory | `--guard ADDR+LEN:w` |
| Every block executed in a function | `-t blocks --trace-range LO:HI` |
| What did the firmware print, and when? | `-t console` |
| Which device registers did it read, with what values? | `-t mmio`, `-t mmio-summary` |
| Did an interrupt arrive? | `-t irq`, `-t exc` |
| Is a return address being smashed? | `--guard-returns` |
| Did this input take a different path from that one? | `--cov-out FILE` on each, then compare |
| What is at this address: RAM, a device, nothing? | `--map` |
| Does it depend on memory nobody initialised? | `--fuzz-uninit` |
| Only the part of the run near the crash | `--trace-after N` |
| Replays without booting each time | `--fuzz-snapshot FILE` |
| The smallest input that still does it | `scripts/minimize.py INPUT -- FUZZ` |
| Is an address written differently on this architecture? | "Addresses on each architecture" |

## Replay

```
--fuzz-replay FILE|DIR     run each input once and print its verdict
```
```
replay: in/2-store: crash-fault pc=0x800003a6 (store-fault cause 0x0 tval 0x40000000) insns=98 buckets=16
uam-run: fuzz: 1 inputs: 0 ok, 1 crash (fault), 0 crash (guard), 0 crash (oracle), 0 hang, 0 timeout, 0 rejected
```

The `replay:` lines are on **stdout**, everything else on stderr. Exit status: 0 all ok,
20 a fault, 21 an oracle, 25 a guard, 22 a hang, 23 a timeout, 24 rejected, 26 depended on
memory nothing wrote, 2 no verdict at all (a bad option, or the boot never reached the
begin pc). With several inputs it is the most severe one's.

- `--fuzz-iter-insns N`: the per-input budget. Lower it to make a timeout quick to
  reproduce (`--fuzz-iter-insns 20000`).
- `--fuzz-snapshot FILE`: start from the begin snapshot in FILE if there is one (no
  boot), else boot and save it there. It makes every replay of a slow-booting firmware
  quick. FILE is tied to the firmware and the command line: an option that changes the
  machine (`--backtrace`, `--guard-returns`, `--unknown-mmio`, a board option) makes
  ember boot again and replace it, so keep a file of your own for triage and leave the
  campaign's alone. Probes that only observe reuse it. The file also loads in a plain run
  (`--load-snapshot FILE`, same options): it is the state every input starts from.
- `--jit`: the same replay on the JIT. The verdict lines must be identical.
- `--break ADDR` **in a replay** turns ADDR into a crash pc: the input stops there with
  `crash-oracle pc=ADDR insns=N`. Useful to learn how many instructions it takes to reach
  a point, or whether a point is reached at all.

## The stop: history, call chain, registers, memory

```
--history N        the last N events of each processor before the stop
```
```
history: cpu0: the last 8 events before the stop (oldest first):
history:   blk 0x80000356 icount=511
history:   blk 0x800003a2 icount=515
```
A `blk` line is a straight run of instructions starting at that address. Exceptions and
interrupt entries appear in the list too. In a replay it is printed for each input that is
not ok. A loop shows as the same address repeating.

```
--backtrace        the call chain at the stop, innermost first
```
```
backtrace: #0 0x80000282 <parse.constprop.0> called, returns to 0x800003fa <fuzz_iteration+0x42>
backtrace: #1 0x800003b8 <fuzz_iteration> called, returns to 0x8000043a <main+0x32>
```
Each line is a function that was called and has not returned: its entry address, and
where it will return to. ember tracks the calls as they execute, so this works without
debug information and on a stack gdb cannot unwind. Names come from the ELF's symbols;
with a raw image the lines carry addresses only. It is part of a snapshot's state: load a
snapshot with `--backtrace` only if it was saved with it.

```
-d, --dump-state   every processor's registers when the run stops
```
```
pc=800003a6 priv=3 icount=517
zero=00000000  ra  =800003fa  sp  =80004610  gp  =00000000
...
mstatus=00001800 mie=00000000 mip=00000000 mtvec=8000004c
mepc=00000000 mcause=00000000 mtval=00000000
```
This includes the system registers gdb often cannot show (the fault cause and address, the
trap vector, the status word). In a replay it shows the state at the last input's verdict.
For a `crash-fault` that state is **before** the exception is taken (the harness stops the
input there), so the fault registers still hold their old values, 0 in the sample above:
read the cause and address from the verdict or the `exc:` line. They are filled in a
plain run, once the firmware's handler has been entered.

```
--dump-mem [p:]ADDR+LEN[=FILE]    memory when the run stops; repeatable
```
```
uam-run: virtual memory 0x80000470+0x10:
  0000000080000470  d0 fd ff ff 22 00 00 00 00 00 00 00 10 00 00 00  |...."...........|
```
`ADDR` is a virtual address as the processor sees it; `p:` makes it physical. `=FILE`
writes the raw bytes to a file instead (to compare two runs with `cmp`, or load in Ghidra).

```
--break ADDR       stop when ADDR is reached (plain run; exit status 127); repeatable
```
Combine it: `--break 0xPC -d --dump-mem 0xBUF+64 --backtrace` is "show me everything just
before this instruction". ADDR may be an ELF symbol (`--break parse`, `--break main+0x10`).

## Traces (`-t KIND[,KIND...]`, repeatable)

| Kind | One line per | Sample |
|---|---|---|
| `exc` | exception taken, interrupt taken, reset; a summary at the stop | `exc: kind=3 (store-fault) pc=0x800003a6 cause=0x0 tval=0x40000000 icount=517` |
| `blocks` | block executed | `blk 0x800003a2 icount=515` |
| `mmio` | access a device serves, with the value | `mmio: r 1 0x10000005 uart0+0x5 = 0x60 pc=0x80000424 icount=294` |
| `mmio-summary` | (at the stop) register touched: counts and values | `mmio:   0x10000005 uart0+0x5: 18 reads (always 0x60), 0 writes, first pc=0x80000424` |
| `sysreg` | first read and first write of each system register; `sysreg-all`: every one | `sysreg: first write 0x305 <- 0x8000004c pc=0x80000010 icount=5` |
| `irq` | change of an interrupt line into a processor | `irq-in: cpu0 line 7 = 1 icount=...` |
| `console` | line the firmware prints, with when and from where | `console: icount=312 pc=0x8000042e "ready"` |
| `dma` | transfer a device makes into or out of guest memory | `dma: w 64 0x80001720 dram+0x1720 pc=.. icount=N` |
| `link` | message on a `--link` port | `link p0 rx time=.. key=0x0 len=17 data=.. pc=.. icount=N` |
| `ir` | translated block, as ember's intermediate code | for suspected ember bugs only |
| `all` | `exc,mmio,sysreg,irq,console,dma,link` | a first look at a short run |

In a replay, `exc` prints the line of the exception that ended an input and then a
summary saying `0 exceptions taken`: both are right. The harness judged the exception a
crash and ended the input before the processor took it. An exception the campaign does
not count as a crash is taken, runs the firmware's handler, and is in the summary.

Narrowing a trace, which you nearly always want:

```
--trace-after N            trace only from instruction count N on (+N: N after the run starts)
--trace-range LO:HI        blocks: only those with a pc in [LO, HI)
--trace-mmio-range LO:HI   mmio: only addresses in [LO, HI)
--trace-file FILE          write the traces (and --watch, --history, rule logs) to FILE
```

The crash is at a known icount (the `exc:` line, or the snapshot's icount plus `insns=`),
so `--trace-after` a little before it gives just the approach. Example: the last blocks
through one function before a crash at icount 517:

```bash
FUZZ -t blocks --trace-range 0x80000282:0x800003b8 --trace-after 480 --fuzz-replay INPUT
```

Reading a `mmio` line: `r`/`w`, the access size in bytes, the address, the device and
offset, the value, then the instruction that made the access. A register on the crashing
path that `always` reads the same value, on a device that is only a stub, is the first
suspect for an emulation gap.

## Logging

```
-v            more log lines: [info] (addresses of the symbols given, the coverage scheme)
-v -v         [debug] as well
--log-stamp   start each log line with @ICOUNT, so log lines can be placed among trace lines
```
`-v` shows what a symbol resolved to (`--fuzz-begin fuzz_iteration = 0x800003b8`), which
is the quickest check that the campaign fuzzes the function you think it does.

```
--log-pc ADDR[,REG...]     one line each time ADDR is reached, with those registers; repeatable
```
```
uam-run: pc 0x800003a6 reached (hit 1, icount 516) a5=0x40000000 fp=0x211c16
```
ADDR is a number here (not a symbol). Registers are named as gdb names them; a wrong name
prints the list of valid ones. Use it to watch a value across many iterations of a loop
without stopping.

```
-s, --stats    counts at the end: blocks, exceptions, interrupts, resets, instructions
```
```
stats: blocks translated=23 executed=197 exceptions=0 interrupts=0 resets=0 idle ticks=0 virtual time=517
```
`resets=1` on a crashing input means the firmware (or a watchdog) reset the chip.

## Rules: log, dump or stop on a condition (`--on`)

A rule fires at a pc, or after an access to memory, optionally only when a condition
holds, and then logs values, dumps memory, changes something, or stops.

```
--on 'WHERE[,if COND][,count=N],ACTION[,ACTION...]'      repeatable; --on-file FILE: one per line
  WHERE   pc=ADDR | write=ADDR[,len=N] | read=ADDR[,len=N] | access=ADDR[,len=N]
  ACTION  log EXPR...  |  dump ADDR LEN  |  set DST=EXPR  |  stop
  EXPR    numbers, registers, ELF symbols, [ADDR] or [ADDR]:1|2|4|8 for memory,
          and C operators; in an access rule also addr and value
```

```bash
# the arguments of a function and the buffer it was given, each time it is entered
--on 'pc=0x80000282,log a0 a1,dump a1 a0'
# stop the first time a length is larger than the buffer
--on 'pc=parse,if a0>0x100,log a0,stop'
# who stores 0x40000000 into this word?
--on 'write=0x80004610,len=4,if value==0x40000000,log pc ra,stop'
# test a theory without rebuilding anything: just before the faulting instruction, give
# the register a good value. If the verdict becomes ok, that register is the cause
--on 'pc=0x800003a6,set a5=0x80000530'
```
```
rule pc=0x80000282: cpu 0 pc 0x80000282 a0=0x4 a1=0x80000530 icount=464
rule pc=0x80000282: cpu 0 pc 0x80000282 dump 0x80000530+0x4 icount=464
  0000000080000530  46 55 5a 21                                      |FUZ!|
```

Rules work in a replay, on any input route, which makes them the debugger to use when the
crash cannot be reproduced as a plain run. A rule at a pc fires **before** that
instruction executes. A symbol must be spelled as the image has it (`nm fw.elf`). `stop`
ends a plain run with exit status 127; in a replay it ends the input with the verdict
`crash-oracle pc=...` at the instruction that fired the rule.

## Memory: who touches it, and stopping at the first touch

```
--watch [p:]ADDR+LEN[:r|w]      log every access by the processors; repeatable
```
```
guard 0x80000530+4 (log): cpu 0 pc 0x800003ec write 1 at 0x80000530 (+0) value 0x46 icount=436
guard 0x80000530+4 (log): cpu 0 pc 0x8000029a read 1 at 0x80000530 (+0) value 0x46 icount=474
```
Each line: the instruction (`pc`), read or write, the size, the address and its offset
into the watched range, the value, and when. Watching the input buffer gives the parser's
reads in order. It does not see instruction fetches, or a device writing memory by itself
(`-t dma` shows that).

**Firmware with an MMU.** The address of a `--watch`, a `--guard` and the `write=`/`read=`
of a rule is virtual, and it is translated **once, as processor 0 maps data when the run
starts** (in a plain run from a snapshot: at the snapshot). The range is then that
physical memory, whoever touches it and through whatever mapping. If the address is not
mapped yet at that moment, or you mean another address space (a process, a mode with its
own view), give the physical address with `p:`. gdb's `x` and `--dump-mem` translate when
they are used, as the processor maps memory then.

```
--guard [p:]ADDR[-BEFORE][+AFTER][:r|w|rw][,stop|,fault|,log][,name=NAME]
```
```
uam-run: guard bad (stop): cpu 0 pc 0x800003a6 write 4 at 0x40000000 (+0) value 0x211c16
uam-run: guarded memory access (status 0) at pc=0x800003a6 after 517 instructions
```
The access is stopped **before** it happens (exit status 129; in a replay the verdict is
`crash-guard`; under gdb the stop is a SIGSEGV at that instruction). Use it to catch
corruption at the instruction that does it:

- a variable that "changes by itself": `--guard 0xVAR+4:w`
- anything written near an address: `--guard 0xADDR-16+16:w`
- a buffer overflow: `--guard 0xBUF_END+64` (the bytes just past the buffer)

```
--guard-returns[=log]      stop when a return goes somewhere no call returns to
--return-ok ADDR           a return target the firmware uses on purpose; repeatable
```
The detector for a smashed return address: the run stops at the bad return itself, not
thousands of instructions later where the wild jump finally faults. If it fires on a clean
input, the firmware returns to that address deliberately (a scheduler starting a task):
run with `=log` once to list those, and pass each as `--return-ok`.

## Comparing two inputs

```
--cov-out FILE     write the coverage map of the (last) input
```
```bash
FUZZ --cov-out ok.map    --fuzz-replay good_input  < /dev/null > /dev/null
FUZZ --cov-out crash.map --fuzz-replay crash_input < /dev/null > /dev/null
cmp -l ok.map crash.map | wc -l        # 0: the same path; more: where they part
```
Two inputs with identical maps took the same path. To see **where** two inputs diverge,
trace the blocks of each to a file and compare them:

```bash
FUZZ -t blocks --trace-file a.txt --fuzz-replay good_input  < /dev/null > /dev/null
FUZZ -t blocks --trace-file b.txt --fuzz-replay crash_input < /dev/null > /dev/null
diff <(sed -n '/trace: input/,$p' a.txt | cut -d' ' -f2) <(sed -n '/trace: input/,$p' b.txt | cut -d' ' -f2) | head
```
The first differing line is the branch where the crashing input left the good one's path.

## The machine

```
--map                      the bus map: what is RAM, what is a device, where
```
It prints the map and then runs the guest as usual, so bound it: `RUN --map -n 1`.
```
  0000000080000000-0000000087ffffff ram   prio     0  dram
  0000000010000000-00000000100000ff mmio  prio     0  uart0
```
Look the faulting address up in it. Not in any range: the access went nowhere (a wild or
null pointer, or a peripheral this board does not have). In a device's range: the firmware touched a peripheral, and the question
becomes what that device answered.

```
--unknown-mmio fault|zero|echo|stop     what an access to nothing does
```
`fault` raises a bus error (what a campaign uses, so wild pointers crash). `stop` (plain
run only) ends the run at the first such access with exit status 131, naming the address
and pc. If a crash vanishes under `zero`, it was an access to an unmapped address.

```
--mmio-rule ADDR[+SIZE]=POLICY     answer a register: zero, echo, fixed:VALUE, fault, input
```
Tests an emulation-gap theory in one run: if the firmware crashes because a status
register reads 0, `--mmio-rule 0xADDR=fixed:0x1` shows whether the crash goes away when it
reads what hardware would return.

## Addresses on each architecture

The samples on this page are RV32, where an address is an address. Elsewhere:

| Target | What differs |
|---|---|
| **ARM Thumb** | ember prints and takes a Thumb address **with bit 0 set**: the verdict's `pc=0x100289`, `--break`, `--on pc=`, `--log-pc`, `--snapshot-at`, and the symbol values `nm` shows. Without the bit the option never fires. `objdump`, `addr2line`, Ghidra, gdb's `break *` and the pc in `-d` use the address **without** it (0x100288). |
| **Thumb IT blocks** | an `it`/`itt`/`ite...` instruction and the up to four it governs run as one unit, but a `--break`, rule or `--log-pc` on any of them fires at that instruction: the ones before it have run, and `-d` and gdb show the IT bits of the one stopped at. It fires whether or not the instruction's condition holds. |
| **MIPS16e, microMIPS** (PIC32-class) | as Thumb: bit 0 set in everything ember prints and takes. The ELF symbol is even (`readelf -s` marks it `[MIPS16]`/`[MICROMIPS]`): add the bit yourself. |
| **MIPS** | kseg0 and kseg1 are two virtual addresses of the same memory: a `--break` or rule matches only the one the code runs at. A `--break`, rule, `--log-pc` or fuzz pc on a delay slot is an error (as on the hardware, nothing stops between a branch and its slot): use the branch the message names. In a plain run, the firmware's `epc` for a fault in a delay slot is the branch, not the slot (`Cause.BD` is set). |
| **x86** | addresses are linear. An access to nothing reads all ones and does not fault, so wild pointers show up as guards, traps (`trap cause` = the vector in the low byte) or crash pcs. A triple fault is a `hang`; stderr says what shut the processor down. |
| **8-bit AVR, 8-bit PIC, 8051, 6502, Z80** | no exceptions: in a plain run a wild access halts the core and the run ends as a deadlock (exit status 125) at that pc. With `--fuzz-crash-exc none` in a replay it is a `hang`. |
| **8-bit AVR, 8-bit PIC** | code and data are separate spaces. Data is on the bus at 0x800000 + the data address. `--guard`, `--watch` and a `mem:` route take the plain data address (an avr-gcc data symbol **minus** 0x800000); `p:`, `--dump-mem` and gdb take the bus address (the symbol's value as it is). On PIC the pc is the program-memory bus address (twice the word address, except PIC18). |
| **8051** | external data (MOVX) is at 0x1000000 + address. Internal RAM is not on the bus: it cannot be watched or guarded. |
| **AVR after `SEI`, 8051 after an `IE`/`IP` write, Z80 behind a prefix, MIPS delay slots** | the instruction runs as part of the one before it and cannot be stopped at: a `--break`, rule, `--log-pc` or fuzz pc on it ends the run, when execution first reaches that code, with `a pc hook at 0x... is on an instruction that continues the one before it ...: hook 0x... instead` (exit status 126; a replay or campaign: 2). Use the address it names. (x86 after `STI`/`MOV SS` and Z80 after `EI` can be stopped at.) |
| **Z80** | I/O ports are at 0x10000 + port, for `-t mmio` and `--mmio-rule`. |
| **Several processors** (`-o cpus=N`) | every line names its processor (`cpu 1`, `cpu1:`); `--history`, `--backtrace` and `-d` print one section each. A rule takes `cpu=N`. In gdb each processor is a thread. |

The ember-fuzz skill's `references/harness.md` ("Boundaries", "Per ISA") has the full
list, with what each core does on a bad instruction.

## Snapshots

```
--snapshot-at ADDR --save-snapshot FILE     save the state when ADDR is first reached
--load-snapshot FILE                        start from it (same board and options)
```
"Same options" includes the `--unknown-mmio` policy, `--backtrace`, `--guard-returns`,
`--fuzz-uninit`, `--link` ports and rules with `count=` or `delay=`: each adds state to
the machine. Two errors say so: `snapshot has N core records, state for N devices and N
extra parts; the machine needs ...` (you gave an option the snapshot was saved without)
and `snapshot holds state for 'state/frames', which this machine does not have` (the
snapshot was saved with one you left out; `frames` is `--backtrace`).
ADDR may be a symbol. Saving at the begin pc gives the state every fuzz input starts from
(SKILL.md step 6). You can also save one a little before the crash (`--snapshot-at` a
function on the way) and start each debugging run there.

## Bounding a run

```
-n N               stop after N instructions (exit status 124)
--max-ticks N      stop after N ticks of guest time, idle time included (124)
```
Always bound a plain run, and put `timeout SECONDS` in front as well.

## Memory nobody initialised

```
--fuzz-uninit[=PATTERN]    run each input twice, the second time with never-written RAM filled
```
A crash that "comes and goes", or that only happens after some other input, often reads
memory the firmware never wrote. With this option a replay reports inputs whose behaviour
depends on such memory (exit status 26) and names the read. It is part of a snapshot's
state, like `--backtrace`.

## Ruling ember out

If you suspect the emulator and not the firmware, these change how ember executes without
changing what the guest should do. The verdict line must stay the same under all of them:

```
--jit              compile blocks to native code
--jit --no-chain   the JIT without block chaining
--no-opt           no optimisation of translated blocks
--block-insns 1    one instruction per block
```
A verdict that changes is an ember bug: report it with the input and both command lines.

## Exit statuses of a plain run

| Status | Meaning |
|---|---|
| 0, 1-123 | the firmware ended the run itself with that status |
| 2 | a bad option, an image that does not load, a snapshot that does not fit |
| 124 | the instruction or time limit was reached |
| 125 | deadlock: the firmware waits and nothing can wake it |
| 126 | an ember internal error, or a fault loop (the handler itself cannot be fetched) |
| 127 | a `--break`, a rule's `stop`, or a debug trap |
| 128 | livelock: a timer keeps firing but its interrupt never reaches the processor |
| 129 | a `--guard`, the return guard or the guarded heap stopped it |
| 130 | interrupted (Ctrl-C, SIGTERM) |
| 131 | `--unknown-mmio stop` |
| 132 | the link hub went away |

`timeout` also returns 124 when it kills a run; the last line on stderr (`uam-run: ...
(status N) at pc=... after N instructions`) says how the run really ended.
