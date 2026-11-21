# gdb on an ember emulation

Every ember emulation has a gdb server built in. gdb connects to it as it would to a
hardware debug probe, and from then on the guest is an ordinary remote target: you can
stop it, look at registers and memory, set breakpoints and watchpoints, single-step, and
change values.

## Connecting

Terminal 1: start the emulation with `--gdb PORT`. It builds the machine, loads the
snapshot if you gave one, and then **waits** without executing anything:

```bash
RUN --load-snapshot begin.snap --console file:/dev/null,in=INPUT --gdb 1234
```

Terminal 2: start gdb on the firmware's ELF file and connect:

```bash
gdb-multiarch fw.elf
(gdb) target remote :1234
```

or in one line: `gdb-multiarch fw.elf -ex 'target remote :1234'`.

- `gdb-multiarch` handles every architecture. A toolchain's own gdb
  (`arm-none-eabi-gdb`, `riscv64-unknown-elf-gdb`) works as well.
- With debug information in the ELF, gdb prints the source line at every stop and
  `list` shows the code; without it you get addresses and disassembly only.
- **No ELF?** Start `gdb-multiarch` with no file. Everything works by address; there are
  no function names, source lines or `info args`. Keep a disassembler open beside it.
- ember tells gdb the architecture and the register set, so you do not `set architecture`.
  Two things it cannot tell:
  - **a big-endian guest** (big-endian MIPS, ARM BE8, PowerPC) **when gdb has no ELF**:
    give gdb `set endian big` before `target remote`, or memory words print byte-swapped.
    With the ELF loaded gdb takes the byte order from it (`show endian` tells);
  - **x86 code that is not 64-bit**: ember always describes the 64-bit register set, so
    for 32-bit code use `set architecture i386` to disassemble correctly (16-bit:
    `set architecture i8086`).
- The server listens on `localhost` only. From another machine, forward the port:
  `ssh -L 1234:localhost:1234 HOST`.
- `--gdb` does not combine with `--fuzz-begin` / `--fuzz-replay`. Debug the plain run
  (SKILL.md step 6).

When you are done: `kill` ends the emulation; `detach` lets it run on without gdb.

### Code that is not where the ELF says

Symbols help only if they sit at the addresses the code runs at. Three common cases where
they do not, and gdb shows the wrong function or none (`bt` full of `??`, a breakpoint
on a name that is never hit):

- **A bootloader that copies itself** to the top of RAM and continues there. Find the
  distance: the pc where it really runs (the replay's `pc=`, a `history:` line) minus the
  same instruction's address in the ELF (`objdump -d`). Then load the symbols a second
  time, moved by that much:
  ```
  (gdb) add-symbol-file fw.elf -o 0x7f00000        every section moved by that offset
  (gdb) info symbol 0x87f003a6                     now names the function
  ```
- **A second image** (a kernel started by the loader, a module, an application slot):
  `add-symbol-file other.elf` adds its symbols beside the first; with `-o OFFSET` if it is
  loaded elsewhere than linked, or `add-symbol-file mod.elf 0xTEXT_ADDR` when you know
  where its `.text` went.
- **Only a raw image and a separate ELF or map file.** Give gdb the ELF anyway
  (`gdb-multiarch fw.elf`): it does not have to be the file ember loaded.

ember's own names (`--backtrace`, `--break SYMBOL`) come from the ELF it was given as the
image, at the addresses in it; for relocated code its lines carry bare addresses, and
`info symbol ADDR` in gdb (after the `add-symbol-file` above) names them.

## What a stop means

When the guest stops, gdb reports a signal. They are not real signals; ember uses them to
say why it stopped:

| gdb shows | Why the guest stopped |
|---|---|
| `Breakpoint N, ...` (SIGTRAP) | a breakpoint, a watchpoint (gdb prints old and new value), or a finished step |
| `SIGINT` | you pressed Ctrl-C |
| `SIGSEGV` | a `--guard`, the return guard or the guarded heap stopped an access **before** it happened; `$pc` is the instruction about to make it |
| `SIGALRM` | the `-n` or `--max-ticks` limit was reached. gdb ignores this signal unless told otherwise: give `handle SIGALRM stop print nopass` first, or the guest runs on past the limit |
| `SIGABRT` | ember could not go on: an internal error, or a fault loop (the exception handler itself cannot be fetched). Look at the registers |
| `[Inferior 1 (Remote target) exited with code N]` | the firmware ended the run itself |

A processor exception (a bus fault, an illegal instruction) is **not** a stop: the guest
takes it and runs its own handler, as real hardware would. To stop there, put a breakpoint
on the handler, or on the faulting instruction itself.

## Cheat sheet

Most commands have short forms: `b`, `c`, `si`, `ni`, `n`, `s`, `bt`, `p`, `x`, `i r`.

### Running and stopping

| Command | Does |
|---|---|
| `continue` (`c`) | run until something stops the guest |
| Ctrl-C | stop a running guest |
| `stepi` (`si`) | execute one instruction (into calls) |
| `nexti` (`ni`) | one instruction, stepping over a call |
| `step` (`s`) / `next` (`n`) | one source line, into / over calls (needs debug info) |
| `finish` | run until the current function returns |
| `until LOCATION`, `advance LOCATION` | run until that place is reached |
| `stepi 20` | twenty instructions |
| `kill` / `detach` | end the emulation / leave it running |

### Breakpoints

| Command | Does |
|---|---|
| `break *0x800003a6` | stop **before** the instruction at that address (the `*` means "address") |
| `break parse` | stop at a function (after its prologue; `break *parse` for its very first instruction) |
| `break file.c:81` | stop at a source line |
| `tbreak ...` | the same, once only |
| `break *0x80000282 if $a0 > 0x100` | only when a condition holds |
| `hbreak ...` | a "hardware" breakpoint: the same in ember, for when gdb refuses to write one into ROM |
| `info breakpoints` | list them, with hit counts |
| `delete N`, `disable N`, `enable N` | remove or switch one |
| `ignore N 99` | skip the next 99 hits of breakpoint N |
| `commands N` ... `end` | gdb commands to run each time N is hit (see "Tracing without stopping") |

Breakpoints do not change guest memory: ember hooks the address. They work in RAM, ROM and
flash alike, in any number.

### Watchpoints: stop when memory is touched

| Command | Stops when |
|---|---|
| `watch *(unsigned int *)0x80004610` | those 4 bytes are **written**; shows old and new value |
| `rwatch *(unsigned char *)0x80000533` | that byte is **read** |
| `awatch *(unsigned short *)0x80001000` | read or written |
| `watch g_len` | a variable is written (needs symbols) |
| `watch -l p->len` | the memory that expression names now, even after `p` goes out of scope |

The cast gives the size: `unsigned char` 1 byte, `unsigned short` 2, `unsigned int` 4,
`unsigned long long` 8. The guest stops just **after** the access; `bt` then shows who made
it. A watchpoint is the tool for "who put this value here".

### Registers

| Command | Shows |
|---|---|
| `info registers` (`i r`) | the general registers |
| `info registers a5 sp` | those registers |
| `info all-registers` | every register ember describes, including floating point and system registers where the architecture's view has them |
| `p/x $a5` | one register, in hex (`$pc`, `$sp` work on every architecture) |
| `set var $a5 = 0x80000530` | change a register |
| `set var $pc = 0x80000400` | continue from somewhere else |

Registers are named as gdb names them for that architecture. System registers that hold
the fault's cause and address (RISC-V `mcause`/`mtval`, MIPS `Cause`/`BadVAddr`, ARM
`DFSR`/`DFAR`, ...) are often **not** in gdb's view. ember prints them itself: run with
`-d` (at the stop) or `-t exc` (as each exception is taken).

### Memory

`x/NFU ADDR` examines memory: N items, format F, unit U.

| Command | Shows |
|---|---|
| `x/16xb 0x80000530` | 16 bytes, hex |
| `x/8xw $sp` | 8 words (4 bytes each) from the stack pointer, hex |
| `x/4xg ADDR` | 4 giant words (8 bytes each) |
| `x/s ADDR` | a zero-terminated string |
| `x/32cb ADDR` | 32 bytes as characters |
| `x/8i $pc` | 8 instructions from the current one |
| `x/8i $pc-16` | the instructions leading here (fixed-length instruction sets only) |
| `p *(struct hdr *)0x80000530` | memory as a structure (needs debug info) |
| `p/x g_len`, `p &g_buf`, `p sizeof(g_buf)` | a variable, its address, its size |
| `set {unsigned char}0x80000533 = 0x3f` | write one byte |
| `set {unsigned int}0x80000540 = 123` | write a word |
| `restore FILE binary 0x80000530` | copy a file into guest memory at that address |
| `dump binary memory out.bin 0x80000530 0x80000630` | save a range of guest memory to a file |
| `find /b 0x80000400, +0x400, 0x46, 0x55, 0x5a` | search a range of memory for those bytes (where did the input land?) |

Addresses are virtual, as the processor currently maps them (on x86, linear).

### Where am I, and how did I get here

| Command | Shows |
|---|---|
| `bt` (`backtrace`) | the call chain |
| `frame N`, `up`, `down` | look at a caller's frame; `info registers` and `info locals` then apply to it |
| `info args`, `info locals` | arguments and local variables (needs debug info) |
| `info symbol 0x800003a6` | which function an address is in |
| `info line *0x800003a6` | which source line |
| `info address parse` | where a symbol is |
| `list *0x800003a6` | the source around an address |
| `disassemble` | the current function; `disassemble /s` with source; `disassemble 0xA,0xB` a range |
| `info threads`, `thread N` | the processors of a multiprocessor machine (each is a thread) |

gdb's `bt` reads the stack, so it fails when the stack is corrupt, in hand-written
assembly, and often in firmware without debug info ("Backtrace stopped"). ember's own
`--backtrace` (printed when the run stops) is built from the calls actually executed and
works in all of those cases: use it when `bt` gives up.

## Recipes

**Stop just before the crash and look.**
```
(gdb) break *0x800003a6
(gdb) continue
(gdb) x/i $pc                    => 0x800003a6 <parse+292>:  sw  s0,0(a5)
(gdb) info registers a5 s0       a5 = 0x40000000: the address it is about to store to
(gdb) bt
```

**Step through the fault into the handler.** Break on the trap vector first (its address
is the trap-vector register in `-d`'s output, or the firmware's exception entry symbol):
```
(gdb) break *0x8000004c
(gdb) stepi
(gdb) info registers pc          now at the handler
```

**Find who wrote a bad value.** Restart from the snapshot (`kill`, start the run again),
then, before continuing:
```
(gdb) watch *(unsigned int *)0x80004610
(gdb) continue                   stops after each write; repeat until the bad value appears
(gdb) bt
```

**Find who reads a byte of the input.**
```
(gdb) rwatch *(unsigned char *)0x80000533
(gdb) continue
```

**Stop when a function is called with a suspicious argument.**
```
(gdb) break parse if n > 256
(gdb) break *0x80000282 if $a0 > 256        the same without debug info
```

**Test a theory by changing a value.** Stop before the bad use, correct the register or
memory, and continue. If the crash goes away, that value is the cause:
```
(gdb) break *0x800003a6
(gdb) continue
(gdb) set var $a5 = 0x80000530
(gdb) continue
```

**Put an input in memory by hand** (a `mem:` route, or to try a variation without
re-running the fuzzer), at the snapshot, before `continue`:
```
(gdb) restore INPUT binary 0xBUF
(gdb) set {unsigned int}0xLEN_ADDR = 4
```

**Save state to compare two runs.**
```
(gdb) dump binary memory good.bin 0x80000400 0x80000800
```
Do the same in the other run and `cmp -l good.bin bad.bin`.

## Tracing without stopping

Attach commands to a breakpoint and end them with `continue`: gdb prints and carries on.

```
(gdb) break *0x80000282
(gdb) commands
> silent
> printf "parse(n=%d, p=0x%x) from 0x%x\n", $a0, $a1, $ra
> continue
> end
(gdb) continue
```

For "log this function's arguments and what it returned", ember-dev-build has a ready
script that knows each architecture's calling convention:

```bash
gdb-multiarch -q -batch -ex 'target remote :1234' \
    -ex 'source <ember>/skills/ember-dev-build/scripts/gdb_trace_calls.py' \
    -ex 'trace 0x80000282 parse' -ex continue > calls.txt 2>&1
```

Without gdb at all, the runner's `--on 'pc=ADDR,log ...'` and `--log-pc` do the same and
also work in a replay (`flags.md`).

## A whole session from a script

For a repeatable session (and for an agent that cannot type into gdb), give the commands
on the command line or in a file, and let gdb exit at the end:

```bash
RUN --load-snapshot begin.snap --console file:/dev/null,in=INPUT --gdb 1234 > run.log 2>&1 &
sleep 0.5                                   # let it start listening
timeout 60 gdb-multiarch -q -nx -batch fw.elf \
    -ex 'target remote :1234' \
    -ex 'break *0x800003a6' -ex 'continue' \
    -ex 'bt' -ex 'info registers' -ex 'x/8i $pc' -ex 'x/32xb $a1' \
    -ex 'kill' > gdb.txt 2>&1
wait
```

- `-batch` exits after the last command; `-nx` ignores your `~/.gdbinit`; `-q` drops the
  banner.
- Or put the commands in a file, one per line, and use `-x session.gdb`.
- **Always end with `kill`** (or `detach`) and put `timeout` in front: a `continue` that
  never hits a breakpoint otherwise waits for ever, and the emulation with it.
- Redirect gdb's output to a file. If a timeout kills the session, piped output is lost.
- If a command prints `Cannot execute this command while the target is running`, an
  earlier `continue` or `stepi` did not come back. See the pitfalls.

## Pitfalls

- **`stepi` over an instruction that faults never returns** on architectures where gdb
  steps by planting a temporary breakpoint at the next instruction (RISC-V, MIPS, ARM).
  The exception goes to the handler, the next instruction is never reached, and the guest
  runs on. Break on the handler first. `finish` out of a function that crashes hangs the
  same way.
- **`continue` with no breakpoint ahead** runs until the firmware ends, which firmware
  usually does not. Press Ctrl-C, or set the breakpoint first. To make a run stop by
  itself, start it with `-n N` **and** tell gdb `handle SIGALRM stop print nopass` before
  `continue`: the limit arrives as SIGALRM, which gdb otherwise passes over silently.
- **A breakpoint that is never hit.** Check the address is an instruction boundary
  (`x/3i ADDR-8`), that the code is not in a different mode, and that execution gets
  there at all (`--log-pc ADDR` in a plain run, or `--break ADDR` in a replay). On ARM,
  gdb wants a Thumb address **without** its low bit (`break *0x100288`), while ember's
  own options and every pc it prints have the bit **set** (`--break 0x100289`). Inside a
  Thumb IT block and on a MIPS delay slot a breakpoint stops at that instruction. On AVR
  after `SEI`, the 8051 after an `IE`/`IP` write and Z80 behind a prefix it cannot: the
  emulation ends with an error naming the pc (gdb sees the target go away). Break on the
  instruction before it.
- **Symbols are at the wrong addresses** (relocated or separately loaded code): see "Code
  that is not where the ELF says".
- **`bt` shows one frame, or nonsense.** No debug info, or a damaged stack. Use ember's
  `--backtrace`.
- **`<optimized out>`** for a variable: the compiler kept it in a register or removed it.
  Read the register directly; the disassembly shows which one.
- **A function name gdb does not know.** The compiler may have renamed it
  (`parse.constprop.0`, `parse.isra.1`) or inlined it. `info functions parse` lists what
  exists; break on an address otherwise.
- **Words look byte-swapped.** A big-endian guest and no ELF given to gdb: `set endian
  big` before connecting.
- **Data is not where the symbol says, on 8-bit AVR or PIC.** gdb uses bus addresses:
  data space is at 0x800000 + the data address, which is what an avr-gcc data symbol
  already holds (`x/4xb &g_buf` works). ember's `--watch` and `--guard` take the data
  address without that base (`flags.md`, "Addresses on each architecture").
- **gdb refuses to connect, or times out.** The emulation is not waiting yet (it prints a
  line when it listens), or another one still holds the port. Use a different port.
- **There are no `monitor` commands and no reverse execution.** To "go back", restart from
  the snapshot: the run is deterministic, so you arrive at exactly the same state. Use the
  instruction count as your position (`-n N` stops the run at instruction N).
- **Changing memory or registers changes the run.** After `set var`, what follows is no
  longer the crash the fuzzer found. Restart from the snapshot for a clean reproduction.
