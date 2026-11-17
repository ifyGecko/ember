# Coverage and fuzzing

ember fuzzes firmware in-process: boot once, snapshot at the start of the code under test,
and run every input from that snapshot, counting edge coverage on the way. Everything it needs is
in the tree: the harness, coverage site lists and boundary files made by static analysis
(`fuzz/tools/gen_sites.py`, `gen_boundaries.py`), a coverage-guided fuzz brain that drives
ember over TCP (`fuzz/tools/fuzz_brain.py`), and a shared-memory coverage region for outside
readers (see the end of this page; the formats are in [`fuzz-formats.md`](fuzz-formats.md)).

## Coverage model

**Ember collects coverage itself by default.** A fuzz run with no `--cov-sites` counts the
blocks ember runs ("Block coverage" below): no site list, no rizin or Ghidra pass, and begin,
end and crash pcs may be ELF symbols. A site list made by static analysis is an opt-in for
targets worth it ("Block coverage or a site list").

Coverage is one IR opcode, `UAM_OP_COV A, B` ([`uam-isa.md`](uam-isa.md) §7.5), which the
coverage pass inserts right after the instruction marker of each instrumented instruction:

```
idx = (A ^ prev) & mask
if (map[idx] != 0xff) map[idx]++      // saturating 8-bit counters
prev = B
```

(the general form folds a longer history, for N-grams, and can skip a site that counted
last; see the ISA document).

`A` and `B` are fixed when a block is translated, so the scheme is just a table of
`(guest pc, A, B)` sites (`uam_exec_set_coverage`, `uam/exec.h`):

| Scheme | A | B |
|---|---|---|
| AFL edges | `cur` | `cur >> 1` |
| block counts | `id` | `0` |
| site model, N=1 | `site_id` | `0` |
| site model, N=2 | `site_id` | `spread(site_id)` |
| hashed-pc model, N=1 | `hash_pc(offset, module)` | `0` |
| hashed-pc model, N=2 | `hash_pc(offset, module)` | `A >> 1` |

Without a site table, block mode (the default when fuzzing; `--cov-blocks` for a plain run)
puts a site at every translated block's start, so no site list (and no rizin or Ghidra) is
needed; see "Block coverage" below.

Properties:

- **Coverage off costs nothing**: no block contains COV.
- **The guest is unaffected**: COV touches only the map. `icount`, timing, registers and
  memory are identical with coverage on.
- **Backends agree**: the interpreter and the JIT (chained or not) produce byte-identical
  maps. The JIT emits COV inline, about ten host instructions with no call.
- The map is never snapshot state. Sites match the guest *virtual* pc. Instructions in delay
  slots cannot be sites (translating one is an internal error that stops the run, naming
  the pc).
- **Repeats** (`uam_cov_state.skip_repeat`, `uam_fuzz_config.skip_repeats`): optionally a
  site counts only when another site counted since its last count. A loop with a single
  site then counts once per visit, not once per turn, and no edge from a site to itself
  appears (`--cov-mode leapfrog`, below).

Overhead, synthetic worst case (a MIPS loop of 2-7 instruction blocks): JIT +31%,
interpreter +4%.

### Block coverage (the default)

A fuzz run counts the blocks ember runs unless `--cov-sites` gives a site list
(`--cov-blocks` says so explicitly, and asks for it on a plain run, with `--cov-out`), with
the same N-gram counting as site lists:
`--cov-ngram N` (default 2), `--cov-rotate` and `--cov-mode` apply (blocks count `every`
execution by default, so a loop that is one block shows how often it turned; site lists
default to `leapfrog`). Each block start is a site
with `A = mix(pc) & mask`, `B = A >> 1` (the block model, `fuzz-formats.md`), and three
things make an ember block stand for a basic block, so that the map depends only on the
code's path:

- **Splits.** A block ember had to cut (its length, `--block-insns`; a page end; an
  unfetchable next instruction) marks its continuation, which does not count: the map is
  the same whether blocks hold 32 instructions or 1 (`tests/fuzz/cov_blocks_check.py`).
- **Resumes.** A block entered mid-way, because the hart stopped inside it (a timer came
  due, a hook), does not count.
- **Interrupts** (`--cov-irq isolate`, the default). An interrupt handler counts with a
  history of its own, and the interrupted code gets its own back when an exception return
  resumes it: where an interrupt lands changes neither. Only an exception return restores
  a saved history (the IR marks them, exit kind EXC_RETURN), so a task that an interrupt
  suspended resumes with its own history whenever a later return goes back to it, and code
  that merely reaches the same pc keeps its own. In `tests/firmware/irqtarget.c`, a timer interrupting a parser at
  six different periods gives one set of buckets with `isolate` and six with `follow` (the
  handler continues the interrupted code's history, as plain edges would).

`--cov-range R` (repeatable) counts only blocks that start in R: `LO-HI`, `LO+LEN`, an ELF
symbol `SYM` (its size), `SYM-SYM2` or `SYM+LEN`. Code outside every range neither counts nor
enters the history, so the parser under test, not the scheduler, the idle task or a C
library, makes the buckets. The map is 64 KiB for N <= 2 and four times larger for each N
above (at most 4 MiB; `--cov-map-size`). Begin, end and crash pcs can be ELF symbols too, so
an image with symbols needs no other tool:

```
uam-run -b rv32-ref --cov-blocks --cov-ngram 3 --cov-rotate --cov-range net_rx-net_rx_end \
    --fuzz-begin net_rx --fuzz-end net_rx_done --fuzz-crash-pc panic \
    --fuzz-input mem:0x80100000,max=1514 --fuzz-net 127.0.0.1:7000 fw.elf
```

Without the stock extensions (an out-of-tree runner), `--cov-blocks` is plain edges between
ember's blocks (`uam_exec_set_block_coverage`). In a program:
`uam_fuzz_set_block_coverage_ex` (`uam_cov_block_opts`: mask, ranges, splits, contexts).

Every coverage op also records each bucket it sets first (`uam_cov_touched`), so the
harness lists and clears an iteration's buckets in time proportional to what it set, not to
the map size: a 4 MiB map costs a fuzz iteration no more than a 64 KiB one.

### Block coverage or a site list

Block coverage is the default because it needs nothing but the image and follows the code's
path exactly. A site list (`--cov-sites`, "Site lists, boundary files and the brain" below)
replaces it, and is worth the static-analysis pass when:

- **Only chosen points should count.** A list instruments exactly the instructions you pick
  (function entries, one module's blocks): in a huge image whose hot paths are mostly noise,
  and where `--cov-range` cannot express the selection, fewer sites mean a smaller, quieter map
  and a cheaper run.
- **Names matter.** A site list names its sites, so an uninstrumentable site or a site check
  names the function, and the brain's buckets map back to the list.
- **Ids should survive a rebuild.** Site ids come from the list, not from addresses, so
  maps of two builds of the same source compare when the list is regenerated alike; block
  buckets follow the pcs.
- **The list comes from elsewhere.** A list exported from another analysis, or made for
  hashed-pc coverage with modules at their own bases (`--cov-module`).

Otherwise keep the default. Site lists and block coverage share the counting (N, rotation,
`--cov-mode`), the brain and the protocol; only block coverage has `--cov-range` and the
interrupt contexts (`--cov-irq`).

## The harness (`uam/fuzz.h`)

1. **Boot** until the first arrival at the begin pc. Coverage so far is the *INIT* map.
   `-n` (`uam_fuzz_config.boot_insns`) bounds the boot; without it a firmware that never
   reaches the begin pc runs until interrupted. `--fuzz-begin-skip N`
   (`uam_fuzz_config.begin_skip`) lets the boot pass the begin pc N times first, for
   firmware that runs its handler during start-up (a self-test, a first request of its
   own): the snapshot is taken at arrival N+1, and everything before it, the skipped
   passes included, is INIT.
2. **Snapshot** there. With `--fuzz-snapshot FILE` (`uam_fuzz_config.save_snapshot`) the
   snapshot is also saved to FILE, and a later run given the same FILE starts from it with
   no boot (the INIT map is then empty, and `--fuzz-begin-skip` is not applied again):
   parallel workers boot once. `FILE.key` holds a fingerprint of what the snapshot was made
   from (the image files' contents and the command line, less the options that only name a
   run's inputs and outputs); a FILE made from anything else is replaced by a fresh boot.
3. **Each input**: restore the snapshot (only the memory lines the last input changed, and
   the components; repeat restores reuse the device table and skip the compatibility
   check), clear the map and `prev`, deliver the input, run
   until the iteration ends, a crash check fires or the budget runs out, then classify.
   A crash does not end the campaign; the next input starts from the snapshot. Neither
   does an input the route refuses (verdict *rejected*, below). An internal error of ember
   itself (a site it cannot instrument, bad IR) does: it would recur on every input, and
   it is not the input's doing.

Iteration boundaries are *paired* (`--fuzz-begin` and one or more `--fuzz-end`: reaching any
end pc ends the iteration) or *begin-only* (only `--fuzz-begin`: arriving there again ends
the iteration); a boundary file (`--cov-boundary`, below) expresses the same two modes. In paired mode, an input that never reaches an end runs out of
budget (a timeout). All are PC hooks. The same input from the same snapshot gives the same map
and verdict, on every backend.

**Coverage at the boundaries.** A coverage site at a boundary pc is counted by its hook,
before the map is published and the history reset: the
begin's hit goes to the map it closes (INIT at boot; in begin-only mode, the iteration
that ends back at begin), an end's to the iteration it ends. An iteration's first count is
the site after the begin, with an empty history.

**Several begins.** `--fuzz-begin` is repeatable (`uam_fuzz_config.more_begin_pcs`), and a
boundary file may hold several BEGIN markers: a target with two request handlers, say. The
snapshot is taken at whichever begin the boot reaches first (after `--fuzz-begin-skip`
arrivals at any of them), and every input starts there, so it reaches whichever handler the
firmware runs for it. During an iteration any begin acts as the begin does: it ends the
iteration in begin-only mode and opens a level when the boundaries are nested.

**Recursion.** With nested boundaries (`uam_fuzz_config.nested`, set from depth-zero
boundary-file markers), a begin reached during an iteration opens a nested level and every end
closes one. Only the end that closes the outermost level ends the iteration, so a recursive
handler is one input. Without nesting, the first end reached ends it.

**Between iterations.** With paired boundaries, the code between an end and the next begin
(the target's own loop, reading the next request) never runs per input, because each input
starts from the snapshot at the begin. It runs once during boot and is counted in the INIT
map. That is intended: it does not depend on the input.

### Input routes (`--fuzz-input`)

| Route | Delivery |
|---|---|
| `mem:ADDR[,len=ADDR][,max=N][,reject]` `[,before=N[:DIR]][,after[=N][:DIR]][,extent=@OFF:SZ[:le\|:be][+D\|-D]]` | the input written at virtual ADDR (truncated to N bytes, default 4096; with `reject`, a longer input is refused instead: verdict rejected), and optionally its length as a 32-bit word in the CPU's byte order; redzones around the buffer (below) |
| `mmio` | the stream that `--mmio-rule ADDR=input` registers read |
| a board input | anything a board published with `uam_machine_add_input`; the stock boards publish their console UART: `uart0`, `com1` on `x86_64-ref` (the receive FIFO; what does not fit is dropped) |

**Redzones.** The `mem:` route can guard the memory around its buffer `[ADDR, ADDR+max)`
with STOP guards ([guards](#guards---guard---watch)): an access that touches one ends the
iteration before it happens, with verdict *crash (guard)*. `DIR` is `r`, `w` or `rw` (the
default), so under- and over-reads and under- and over-writes are each caught or let through.

| Option | Guarded range | Catches |
|---|---|---|
| `before=N` | `[ADDR-N, ADDR)`, armed at the first input | `buf[-1]`, a header read from in front of the buffer |
| `after[=N]` | `[ADDR+L, ADDR+max+N)`, re-armed for each input | anything past the input's legitimate extent L: the unused rest of the buffer, and N bytes beyond it (default 0) |
| `extent=@OFF:SZ[:le\|:be][+D\|-D]` | (places `after`) | L is the SZ-byte (1, 2, 4, 8) length field at input offset OFF, in the given byte order (default the CPU's), plus D, instead of the delivered byte count |

L never falls before the end of the length field itself, which the firmware may read; an input
too short to hold the field gets no `after` zone. Guard memory that belongs to the firmware's
own objects only with care: `after=N` past the buffer is someone else's memory, and code that
terminates a string in place (`buf[len] = 0`) writes into `after`'s zone, so use `after:r`
there. The older forms still work: `guard` is `after:r`, `guard=@OFF:SZ[+-D]` is
`after:r,extent=@OFF:SZ[+-D]`, and `guardabs=ADDR:N` a fixed read zone (`--guard` does that
generally).

The harness never names a device: boards publish inputs, the harness picks one by name.
A route that cannot deliver a particular input (too long for its buffer, not in the shape
it needs) returns `UAM_ERR_REJECTED`: that input's verdict is *rejected*, nothing runs, and
the campaign goes on. Any other error from a route is a harness error and stops the run.

### Verdicts

| Verdict | Cause | detail | `--fuzz-replay` exit |
|---|---|---|---|
| ok | the iteration ended (or the firmware halted with status 0) | 0 | 0 |
| crash (fault) | an exception of a crashing class (`--fuzz-crash-exc`), a fetch fault loop | faulting pc | 20 |
| crash (guard) | a STOP guard was touched: a route's redzone, a `--guard` | pc of the access | 25 |
| crash (oracle) | a `--fuzz-crash-pc` reached, a `--fuzz-crash-irq` interrupt taken, a reset during the iteration, a non-zero halt, a debug trap | the pc (0 for a reset, the status for a halt, the crashing lines for an interrupt) | 21 |
| hang | deadlock or livelock (also an x86 processor shutdown: a triple fault or an RSM with an invalid saved state leaves the hart idle, and only the `x86: ... hart shut down` line on stderr tells it from a wait) | the stop reason (`uam_stop_reason`: `UAM_STOP_DEADLOCK` or `UAM_STOP_LIVELOCK`, numbered as in `uam/exec.h` of this ember) | 22 |
| timeout | `--fuzz-iter-insns` (default 1e7) exhausted | 0 | 23 |
| rejected | the input route refused the input (`UAM_ERR_REJECTED`); nothing ran | 0 | 24 |

A crash (guard) names the guard and the access (`uam_fuzz_result.guard`). Here a read guard
over the rv32 fuzz target's input buffer catches `parse` reading it:

```
build/uam-run -b rv32-ref --cov-blocks --fuzz-begin 0x80000336 --fuzz-crash-pc 0x80000240 \
    --fuzz-input uart0 --guard 0x80000470+256:r,name=g_buf --fuzz-replay in/ build/tests/firmware/fuzztarget.elf
replay: in/1-hello: crash-guard pc=0x80000262 (read 1 at 0x80000470: +0 into guard g_buf [0x80000470, 0x80000570)) insns=60 buckets=8
```

A `--guard ...,fault` instead lets the guest take the bus fault: that is a crash (fault) if its
class crashes, and the result names the guard as well. To a fuzz brain (`--fuzz-net`) a crash
(guard) is reported as `GUARD` with the pc of the access.

Exception classes: `fetch`, `load`, `store` (bus faults), `align`, `xlate` (TLB misses and
protection: firmware with a TLB takes these routinely), `trap` (illegal instructions, but also
system calls and breakpoints), `sysreg`, `all`, `none`. The default is
`fetch,load,store,align`. Exceptions outside the policy are delivered to the firmware as
usual; pointing `--fuzz-crash-pc` at its fatal-error handler catches those that end there.
For x86 SMM handlers, SMM code-fetch confinement (`uam_x86_set_smm_confine`; `-o
smm-confine=1` on `x86_64-ref`) turns an SMM instruction fetch outside SMRAM - a call-out
through a corrupted or NULL pointer - into a `fetch` crash at the first instruction outside.
SMRAM that a board decodes through the SMM view is addressed as SMM sees it, at
`UAM_X86_SMM_VIEW` (0x400000000000) + address: `p:0x400000000000+ADDR` for `--guard` and
`--dump-mem`. A `mem:` route takes a plain address, translated as the hart at the begin pc
stores, so a begin pc inside SMM reaches SMRAM by itself.

`--fuzz-crash-irq N` (repeatable, 0..63) makes taking an interrupt from line N a crash: a
watchdog's warning interrupt, say. Lines are numbered as the ISA reports them: on RV32 the
`mcause` interrupt code (7 machine timer, 11 machine external), on MIPS32 the Cause.IP bit
(0..7) that was pending and enabled, on ARMv7 0 for IRQ and 1 for FIQ (on `arm-ref` every
device interrupt arrives as IRQ, through the GIC), on PowerPC the external interrupt sources
0..7 that were high (on `ppc-ref`: 0 the UART, 1 the IPI doorbell), 8 soft reset, 9 machine
check and 10 the Book E critical input; the timers have no line. On x86-64 they are the
processor's pins that were high (`UAM_X86_IRQ_*`: 0 INTR, 1 NMI, 2 SMI, 3 INIT, 4 A20M#). On
8-bit AVR a line is an interrupt vector (1..63; on `avr-ref`: 1 the UART, 2 the timer); on
AVR32 0..3 are INT0..INT3 and 4 NMI (on `avr32-ref` COUNT/COMPARE and the UART reach the
levels their interrupt controller groups are given); on 8-bit PIC 0 is the (PIC18 with IPEN:
high-priority) peripheral line and 1 the low-priority one (on `pic-ref`: 0 the timer, 1 the
UART); on the 8051 a line is an interrupt source (vector 0x03 + 8n; on `mcs51-ref`: 0 INT0,
1 Timer 0, 2 INT1, 3 Timer 1, 4 the serial port); on the 6502 lines 0-29 are wired-OR into
IRQ and 30 is the NMI (on `6502-ref`: 0 the UART, 1 timer 0, 30 timer 1); on the Z80 0 is
/INT and 1 /NMI (on `z80-ref` /INT is the UART's and the CTC's line); on Xtensa a line is
the interrupt of that number in the model's table (on `xtensa-ref`: 5 the UART, 10 timer 0,
14 timer 1, the NMI).
The interrupt is reported once the CPU has taken it
(`uam_exec_set_irq_hook`, `uam_interrupt`: the interrupted pc, the handler pc, the lines); the
detail is the crashing lines as a mask. Without the option no interrupt hook is installed.

### Guards (`--guard`, `--watch`)

Guards are ember's, not the harness's (`uam/guard.h`): they work in any run, fuzzing or not.
`--guard SPEC` (repeatable) guards a byte range of guest memory:

```
[p:]ADDR[-BEFORE][+AFTER][:r|w|rw][,stop|,fault|,log][,name=NAME]
```

The range is `[ADDR-BEFORE, ADDR+AFTER)` around the anchor ADDR: `0x1000+16` is the 16 bytes
from 0x1000, `0x1000-16` the 16 in front of it, `0x1000-16+16` both, a bare `0x1000` one
byte. ADDR is virtual, translated as processor 0 maps its data accesses when the run starts
(after `--load-snapshot`), a page at a time; `p:` makes it physical. On the 8-bit AVR and PIC,
whose data space sits at 0x800000 on the bus, ADDR is therefore the data address the firmware
uses, as for a `mem:` route; a bus address needs `p:`. Any data access that overlaps the
range hits it, in the directions given (default `rw`). What a hit does:

| Action | The access | The run |
|---|---|---|
| `stop` (default) | does not happen | stops at the instruction (exit 129; in gdb, SIGSEGV on that processor); fuzzing: crash (guard) |
| `fault` | does not happen | the guest takes the bus fault an unmapped address raises |
| `log` | happens | goes on; a line on stderr per access (`--watch SPEC` is `--guard SPEC,log`) |

```
build/uam-run --guard 0x80100000-16+16:w fw.elf     # catch writes 16 bytes either side of 0x80100000
build/uam-run --watch p:0x3000+4:w fw.elf           # log every write to 4 physical bytes
```

On the x86 self-test (`tests/runner/runner-check.sh` checks the same), a watch logs each write
and a guard stops at the first:

```
build/tests/uam-test-run -b x86_64-test --raw-base 0 --entry 0 --watch p:0x3000+4:w build/tests/firmware-x86/selftest.bin
guard p:0x3000+4:w (log): cpu 0 pc 0xc8 write 2 at 0x3000 (+0) value 0xa55a icount=144
guard p:0x3000+4:w (log): cpu 0 pc 0xc8 write 2 at 0x3002 (+2) value 0xa55a icount=145

build/tests/uam-test-run -b x86_64-test --raw-base 0 --entry 0 --guard 0x3000-16+16:w,name=lo build/tests/firmware-x86/selftest.bin
uam-run: guard lo (stop): cpu 0 pc 0xc8 write 2 at 0x3000 (+16) value 0xa55a
uam-run: guarded memory access (status 0) at pc=0xc8 after 144 instructions
```

The offset in parentheses is from the start of the guarded range. A stop names the faulting
instruction; a logged access names the pc its block started at, which can be earlier (0xbf
above). `--stats` adds the number of hits.

In a fuzz run a `--guard` is armed from the start, so the boot is held to it as well (the
fuzz target's start code clearing its BSS stops at a write guard on `g_buf`, and the boot then
fails to reach the begin pc). A route's redzones are armed only once inputs arrive.

Instruction fetches, debug accesses (gdb, `--dump-mem`) and DMA are not guarded. A guarded
page loses the JIT's direct memory access, so every access to it goes through the bus. Guards
are not machine state: snapshots neither save nor restore them. In a program:
`uam_machine_add_guard` (or `uam_machine_add_guard_text` with the same text form),
`uam_machine_guard_last_hit`, `uam_machine_set_guard_observer` for every hit.

### Memory oracles: the return guard and the guarded heap

On flat memory a heap overflow, a use of a freed block or a smashed return address faults
nowhere: the input ends "ok", or at best as a timeout when the processor wanders off. Two
opt-in oracles turn those into guard crashes (verdict `crash-guard`, exit 129 outside
fuzzing). Like guards they are ember's, not the harness's, and work in any run. The fuzz
target `tests/firmware/heaptarget.c` plants one bug of each kind, and
`tests/fuzz/oracle-check.sh` holds both oracles to them on every backend.

**The return guard** (`--guard-returns`, `uam/frames.h`). Ember records where every call
instruction it translates returns to, and stops when a return goes to an address that is not
one of those: a return address overwritten with data.

```
replay: s: crash-guard pc=0x80000364 (return: returns to 0x8000031a, which no call returns to)
```

The rule is deliberately loose, so that it does not misfire: a return into another task, out
of a `longjmp` or into a frame built before tracking began all go to some call's return
address, and pass. What it cannot see is a return address overwritten with another call's
return address. What it does report although nothing is wrong is code that jumps through a
return instruction on purpose to an address no call returns to; the usual case is a
scheduler that starts a new task by returning into its entry function. Such a target shows
up during the boot, once, by address and symbol, and is named with `--return-ok ADDR`
(repeatable; a symbol works). `--guard-returns=log` lists every such target in one run and
goes on:

```
build/uam-run --guard-returns=log -n 30000 fw.elf
guard return (log): cpu 0 pc 0x80000318 returns to 0x80000370, which no call returns to icount=5487
build/uam-run --guard-returns --return-ok co_entry ...
```

Calls and returns are what the lifters mark as such (`docs/uam-isa.md` §7.3); a few idioms
are jumps to it rather than returns and so are not checked: x86 far calls and returns
(`push cs; push ip; retf` switches modes), and a return directly behind a push on the AVR,
8051, 6502 and Z80 (their jump-table idiom). x86 code built with return trampolines
(retpolines) returns to computed addresses everywhere and cannot use the guard.

`--backtrace` prints the calls each processor is inside when the run stops (when fuzzing:
for each input that is not ok), innermost first, named from the ELF image's symbols. It uses
the same tracking and can be used alone:

```
backtrace: #0 0x80000336 <smash> called, returns to 0x80000524 <parse+0xbe>
backtrace: #1 0x800005c2 <fuzz_iteration> called, returns to 0x8000060e <main+0x6>
backtrace: #2 0x80000608 <main> called, returns to 0x8000003c <_start+0x3c>
```

The stack is a best-effort history: code that leaves functions without returning leaves
frames behind until a return to an older frame drops them, tasks are not told apart, and at
most 512 frames are kept per processor. The guard's verdict does not depend on it.

Tracking is a few instructions of translated code per call and per return (a push, and a
compare and pop when the return matches the innermost frame; anything else calls out): about
0.75 ns for a call with its return on the x86-64 JIT. A loop of six instructions around a
call runs 1.5 times slower with it. Nothing when off. Its state (the frames and the set of return addresses)
is machine state: `--fuzz-snapshot` and `--save-snapshot` carry it, and a snapshot taken
without `--guard-returns`/`--backtrace` cannot be loaded with them, nor the reverse. In a
program: `uam_machine_guard_returns`, `uam_machine_add_return_site`,
`uam_machine_track_calls`, `uam_machine_backtrace`.

**The guarded heap** (`uam/heap.h`) is an allocator ember runs *in place of* the firmware's
own, over the memory the firmware's heap used. Every block lies between redzones, every byte
of the arena that is not inside a live block is poisoned, the bookkeeping is the host's, and
freed blocks sit in a quarantine before their memory is handed out again. A guest access
that touches a poisoned byte is a hit, to the byte:

| Hit | What happened |
|---|---|
| `heap-redzone` | an access just outside a live block: an overflow or underflow, by one byte or more |
| `heap-freed` | an access inside a freed block (also through the pointer a `realloc` left behind: blocks always move) |
| `heap-wild` | an access to arena memory that is in no block |
| `heap-double-free` | a free of a block already freed |
| `heap-bad-free` | a free of an address that starts no block |

```
replay: o: crash-guard pc=0x80000508 (heap-redzone: write 1 at 0x80000d18: 1 past the end of the 24-byte block at 0x80000d00, allocated at 0x80000474)
replay: f: crash-guard pc=0x800004c0 (heap-freed: read 1 at 0x80000d00: +0 in the freed 24-byte block at 0x80000d00, allocated at 0x80000474, freed at 0x800004b8)
replay: d: crash-guard pc=0x80000410 (heap-double-free: frees the 24-byte block at 0x80000d00 again, allocated at 0x80000474, freed at 0x800004b4)
```

There is no command-line switch for it, because ember does not know which functions of a
firmware allocate, nor how they take their arguments. **The board does**: it creates the
heap over the arena and hooks the firmware's allocation and release functions at their
first instruction (`uam_machine_add_pc_hook`); each hook reads the arguments from the
registers its calling convention uses, calls `uam_heap_alloc` / `uam_heap_free` /
`uam_heap_realloc` / `uam_heap_calloc`, writes the result and continues at the return
address, so the firmware's allocator never runs. `tests/boards/heap_hooks.c` is a complete
example (60 lines for a RISC-V board), and `skills/ember-dev-build/references/board-skeleton.md`
has the recipe. What to know:

- Checking starts with the heap's first allocation, so start-up code that clears the arena
  (it is usually in `.bss`) is not a hit. Firmware code that walks or sets up its own heap
  structures in the arena *after* that must be hooked or stubbed too, or the heap disarmed
  around it (`uam_heap_arm`).
- The heap replaces the firmware's allocator: bugs inside that allocator, and behaviour that
  depends on its layout or reuse order, are not exercised.
- The blocks and the quarantine are machine state, so each fuzz input starts from the heap
  as it was at the begin pc, and `--fuzz-snapshot` carries it. Create the heap before the
  machine runs.
- Accesses to the arena's pages leave the JIT's direct memory access (each is checked);
  the allocator itself is host code. A heap's action is `stop` (default), `fault` or `log`,
  as for a guard; a board may create several heaps (pools, partitions).

Both oracles report through the guard machinery (`uam_guard_hit.kind`,
`uam_machine_guard_last_hit`, the guard observer), so the harness, the fuzz protocol and the
brain treat their hits like any guard crash: reason "guard", status the pc.

### Replay (`--fuzz-replay FILE|DIR`)

Runs each input once (a directory in name order) and prints one line per input:

```
build/uam-run -b rv32-ref --cov-blocks --fuzz-begin 0x80000336 --fuzz-crash-pc 0x80000240 \
    --fuzz-input uart0 --fuzz-replay crashes/ build/tests/firmware/fuzztarget.elf
replay: crashes//2-store: crash-fault pc=0x800002b2 (store-fault cause 0x0 tval 0x40000000) insns=95 buckets=17
```

(The pcs are `fuzz_iteration` and `fuzz_panic` from `readelf -s`; they move with the
toolchain.) The exit status is the most severe verdict of any input: a crash (the first
one's kind: 20 fault, 21 oracle, 25 guard), else a hang, else a timeout, else a rejected input (24), else 0. So a directory of crashes fails however
it is ordered and whatever other files it holds. Every regular file runs as an input, except
a fuzzer's notes saved beside an input: a runner extension can name their suffixes, and a
file X plus that suffix is then skipped when X is in the directory too. The stock extension
names `.info`, so the brain's `crashes/` or `hangs/` directory replays its inputs
and not their `.info` sidecars (`uam_fuzz_replay_create_ex` in a program). `--cov-out FILE` writes the last input's map. `tests/fuzz/fuzztarget.h`
is a small target with planted bugs used by `meson test -C build --suite fuzz`.

For triage, the traces work in replay too (`-t mmio,exc`, `--trace-file`): the boot's traces
come first and end with `trace: the boot to the begin pc ends` and its summaries, then each
input's begin with `trace: input ID` and end with its own summaries. `--history N` prints, for
each input that is not ok, the last N blocks, exceptions and interrupt entries of its run (an
exception the harness stopped on is in the replay line, not in the history).
`--unknown-mmio stop` is refused while fuzzing: an unmapped access is a verdict there
(`fault` makes it a crash), not a reason to stop the campaign.

### In a program

```c
uam_fuzz_config cfg = { .begin_pc = BEGIN, .crash_pcs = &panic_pc, .n_crash_pcs = 1,
                        .crash_exc = UAM_FUZZ_EXC_DEFAULT };
uam_fuzz *f = uam_fuzz_create(&m, &cfg, err, sizeof(err));
uam_fuzz_set_block_coverage(f);                  /* or uam_fuzz_set_sites */
uam_fuzz_select_route(f, "uart0", err, sizeof(err));
uam_fuzz_boot(f, &r);
for (each input)
    uam_fuzz_run_one(f, data, len, &r);          /* r.verdict, r.detail, uam_fuzz_map(f) */
```

or hand the loop to a frontend (`uam_fuzz_run(f, &frontend, ctx)`: `hello`, an INIT report
with id 0, then one report per input, `bye`).

**Compare logging.** `uam_fuzz_set_cmp(f, UAM_FUZZ_CMP_LOG)` instruments the target's
compares (the compare pass, `uam/cmp.h`; `CMPLOG` in [`uam-isa.md`](uam-isa.md) §7.6).
`uam_fuzz_cmp_arm(f)` makes the next iteration log the values its compares saw, and
`uam_fuzz_cmp_log(f, &n, &dropped)` returns that log afterwards: the distinct
(site, width, kind, a, b), at most 8 per site and 1024 in all. Iterations that are not
armed run with the state detached, at a cost of a load and an untaken branch per compare
(1-3% on the cross-ISA benchmark with the JIT); a logged iteration is about twice as slow.
Maps and verdicts are the same with or without it. The fuzz protocol carries it as
CMP_INPUT and CMPS ([`fuzz-formats.md`](fuzz-formats.md)).

**Stages.** The campaign engine (`uam/fuzz_campaign.h`) also runs two kinds of batch whose
inputs it derives itself from a corpus entry: `uam_fuzz_campaign_i2s_begin` (the entry with
its compares logged, then the input-to-state candidates of `uam/fuzz_i2s.h`; compare
logging must be on) and `uam_fuzz_campaign_trim_begin` (cuts of the entry that keep its
path). Both are driven like a batch of mutations (`batch_next`, the run, `batch_judge`) and
judged against the same record; `uam_fuzz_campaign_i2s` and `uam_fuzz_campaign_trim` return
what the stage found (dictionary tokens; the entry as trimmed). The protocol carries them
as STAGE.

## Limitations

- Sites are keyed on the virtual pc. Code reached through both kseg0 and kseg1 needs two
  sites, and TLB-mapped code (ASIDs) is not distinguished.
- Several processors (`-o cpus=N`) share one map: each counts in its own history, so the map
  does not depend on where the slices end, and a result's pc is that of the processor that
  ended the iteration (a guard hit also gives its number, `uam_fuzz_result.guard.cpu`). Edges
  between processors are not counted.
- Interrupts are reported only when taken, and only as a crash (`--fuzz-crash-irq`); an
  interrupt line that is raised but masked is invisible to the harness.
- Block coverage's interrupt contexts are restored by exception returns: the eight newest
  suspended contexts are kept (an RTOS with more tasks than that suspended by interrupts
  loses the oldest, whose tasks then resume with the handler's history). A task that gave
  up the processor itself, and a synchronous exception's handler, continue the history.
  With ranges, an interrupt taken outside them keeps no context.
- Compare logging sees compares in the IR, so a string compare done in a byte loop
  (`strcmp`, `memcmp`) logs one byte per iteration, and a compared value the code computed
  from the input in other ways than a constant offset (a parsed number, a checksum) is not
  found in the input. Compares in a block whose operands the optimizer knows are constants
  are folded away and not logged (they do not depend on the input).
- The fuzz protocol's CRASH frame carries a guard crash's pc, not its kind: the brain names
  a heap or return crash "guard" like any other, and a replay of the saved input says which
  it is.
- `uam_fuzz_abort` (SIGINT in the runner) ends a campaign at the next instruction boundary
  under the interpreter, and with the JIT at a block boundary at most a few thousand blocks on.

## Site lists, boundary files and the brain

Instead of the default block coverage and `--fuzz-begin`/`--fuzz-end`, a campaign can opt
into coverage sites and iteration boundaries from files made by static analysis ("Block
coverage or a site list" says when that pays); either way its inputs can come from ember's
coverage-guided brain. The formats are in [`fuzz-formats.md`](fuzz-formats.md), the
tools in `fuzz/tools/` (python3; the generators need rizin with python3's `rzpipe`, or
headless Ghidra with its Jython extension).

```bash
python3 fuzz/tools/gen_sites.py --strategy entry fw.elf fw.uams
python3 fuzz/tools/gen_boundaries.py --strategy entry --sites fw.uams fw.elf fw.uamb handle_request:begin
build/uam-run -b mips-ref --cov-sites fw.uams --cov-boundary fw.uamb --cov-strict \
    --fuzz-input uart0 --fuzz-crash-pc 0x80102222 --fuzz-replay crashes/ fw.elf
```

### Site lists and boundaries

| Option | |
|---|---|
| `--cov-sites FILE` | coverage sites from a site list; record *i* is site id *i* |
| `--cov-module NAME=BASE` | a module's load base (repeatable). Otherwise the list's module base table, then, for module 0, the image's link base (lowest ELF `PT_LOAD` vaddr, or `--raw-base`) |
| `--cov-scheme site\|pc`, `--cov-ngram N`, `--cov-rotate` | the bucket model (site ids, or hashed pcs), N-gram length (1..8) and rotation (the order of sites counts); default site, N=2, no rotation. The brain suggests N=3 with rotation for fuzzing |
| `--cov-mode leapfrog\|every` | which executions count. `leapfrog` (the default for site lists): a site counts again only after another site did, so loops count visits and no self-edge appears. `every` (the default for block coverage): each execution counts |
| `--cov-map-size N` | overrides the default map size (site model: from the site count and N; hashed-pc: 64 KiB) |
| `--cov-strict` | each site's bytes in memory must equal the list's `orig[]`, checked at the first begin when fuzzing, else at the end of the run. Without it the check is only reported. A wrong base or byte order shows up here |
| `--cov-boundary FILE` | iteration boundaries instead of `--fuzz-begin`/`--fuzz-end`: any number of BEGIN markers (the snapshot is at the first one the boot reaches) and of END markers, all with the same depth filter. Depth-zero markers keep nested BEGIN/END arrivals (recursion) inside one iteration; with "any depth" the first END reached ends the iteration |

**The generators.** `gen_sites.py` puts one site on every basic block of each function an
ELF's symbol table names (`STT_FUNC`, the entry point, `--symbols` for a raw image;
`--all-functions` adds the functions rizin finds on its own). `--root`, `--cutoff` and
`--max-depth` limit it to a call-graph slice, and `--include-pat`/`--exclude-pat` filter by
name. A raw image needs `--raw --base ADDR --arch ARCH --bits N --endian E` (and
`--symbols FILE`); run it with `--raw-base` and, if its reset code is not at its first byte,
`--entry`. `gen_boundaries.py` writes markers for named functions,
`NAME[:begin|end[:any|zero]]`, and with `--sites` checks that each is a site of the list.

**Headless Ghidra instead of rizin.** `fuzz/tools/ghidra/` holds Jython ports of both, with
the same rules, working on a program analysed in Ghidra. Where both analysers find the same
functions and blocks the two routes write the same file, byte for byte, provided the
program is imported under the image's file name (it becomes module 0's name); a jump table
only one of them resolves is where they can differ. `tests/fuzz/gen-check.sh` compares them
when it finds headless Ghidra with Jython: `GHIDRA_HEADLESS` (the `analyzeHeadless` to run;
empty for none), else `GHIDRA_HOME`, `PATH`, `~/ghidra*` or `/opt/ghidra*`. Its result line
says when it did not compare. The ports also cover what rizin cannot decode: ARM BE8 images, AVR32 and 8-bit PIC ELFs, and MIPS16e/microMIPS
functions. Their first argument is the output file, and a raw image's processor and base
are chosen at import:

```bash
analyzeHeadless /tmp/proj fw -import fw.elf -scriptPath fuzz/tools/ghidra \
    -postScript gen_sites.py $PWD/fw.uams --strategy entry \
    -postScript gen_boundaries.py $PWD/fw.uamb handle_request:begin --strategy entry --sites $PWD/fw.uams
```

`fuzz/tools/ghidra/README.md` has the rest. Ghidra 11.4 and later need the Jython extension
installed.

**Where the BEGIN marker sits.** ember snapshots at the BEGIN marker and delivers each input
there, so the marker must come before the firmware reads its input. The generators' default
strategy (`auto`) puts a block's site on its last instruction except on MIPS. On RV32 that
puts the fuzz target's handler site *after* the UART status read, so every input would run
as "no data". Generate both files with `--strategy entry` and name the input handler. A
marker need not be a site (it then has no coverage of its own), but keep the two files from
one generator run so they agree.

**ARM and Thumb.** ember names Thumb code by its address with bit 0 set (the ARM convention,
`uam/isa/arm.h`): `--fuzz-begin`, `--fuzz-end`, `--fuzz-crash-pc` and `--cov-blocks` maps use
pcs such as `0x10024d` for Thumb code at `0x10024c`, as a Thumb function symbol already gives
them. A site list and a boundary file instead hold the even address and flag Thumb sites
and markers `ALT_STATE`, and ember sets bit 0 for them.

**MIPS16e and microMIPS** (`pic32-ref`) follow the same convention: code in either compressed
ISA runs at its address with bit 0 set, so `--fuzz-begin` and the other pcs take e.g.
`0x9d000741` for MIPS16e code at `0x9d000740`. Unlike ARM, the ELF symbol of such a function
is even (readelf marks it `[MIPS16]` or `[MICROMIPS]`): set bit 0 yourself, as
`tests/fuzz/fuzz-check.sh` does. In a site list they are `ALT_STATE` (the Ghidra route
writes them; rizin gives such functions no sites).

A site on an instruction ember runs as part of the one before it (a branch delay slot, an
instruction inside a Thumb IT block) cannot be instrumented. The run stops with an error
naming the site-list record and module. The generators never place one, so such a site
usually means a wrong base or a hand-made list. Two designs remain open for later, and
either would be fine:

- **Count them.** ember runs a branch and its delay slot as one unit, so a COV right after the
  delay slot's instruction marker would still count once per execution; the verifier's rule
  that COV follows only non-continuation markers would be relaxed for it.
- **Reject them, earlier.** Detect such sites when the list is loaded (a small ISA hook, "is
  this pc a continuation?") instead of when that code is first translated, which can be
  well into a run.

The bucket formula (`fuzz/cov_scheme.c`, [`fuzz-formats.md`](fuzz-formats.md#coverage-buckets))
uses the coverage state's history for N>=2: depth N-1, with a rotation amount per slot when
`--cov-rotate` is given. `test_cov_formats` runs fixed emit sequences through every scheme
(site and pc, N=1, 2, 3, 4 and 8, rotation off and on, two map sizes) as guest code through
`UAM_OP_COV` on each backend; the backends must agree byte for byte and each map must match
its golden hash (`tests/unit/cov_golden.txt`). `tests/fuzz/formats-check.sh` runs the
formats end to end with lists written by `tests/fuzz/mk_lists.py`.

### The brain (`--fuzz-net HOST:PORT`)

`fuzz/tools/fuzz_brain.py` is a coverage-guided fuzzer that drives ember over TCP. It
decides what to work on and keeps the interesting inputs in a disk-backed queue that a
restart resumes; **ember mutates** (see "Mutation in ember" below), runs and judges the
inputs itself and sends back only what is worth keeping. The brain saves crashes and hangs
and runs until stopped (`--iters N` caps the mutated executions). Its campaign logic is in `fuzz/tools/fuzz_queue.py`; the design and
its reasons are in [`fuzz-ideas.md`](fuzz-ideas.md). Under `--out DIR`:

- `queue/id:NNNNNN,...`: the queue. Names record each entry's origin:
  `id:000000,orig:SEEDNAME` (a seed), `id:000012,src:000003,op:havoc,+cov` (mutated from
  entry 3; `+cov` when it reached a new bucket, not only a new hit-count class),
  `id:000020,sync:w2,src:000310` (imported from worker w2's entry 310). `queue/.state/`
  keeps each entry's lineage, depth and how often it was chosen. A later session with the
  same `--out` starts from the queue, with or without `--seeds`.
- `crashes/crash_r<reason>_s<status>_<sha1, 12 digits>`: one input per distinct
  `(reason, status)` of a session, minimized (`--no-minimize` keeps it as found), with a
  `.info` note beside it: `reason=`, `reason_name=`, `status=`, `status_hex=` (the pc, for
  fault, guard and oracle), `len=`, `sig=`, `hex=`.
- `hangs/hang_r<reason>_s<status>_<sha1>`: every distinct input that hung or timed out, up to
  `--max-hangs` (64), not minimized unless `--minimize-timeouts`.

Start ember within `--accept-timeout` (15 s) of the brain; each reply is waited for up to
`--io-timeout` (60 s). `--max-size` (65536) bounds an input's length, and mutations grow
inputs up to it from seeds of any size; `--seed-rng` fixes the campaign's random sequence
(each batch's mutator seed comes from it), `--status-file FILE` writes the feed for the status screen (anew
each session, every `--status-interval` seconds) and `--quiet` drops the log lines. The
brain listens on IPv4 and exits 0 after a clean session, 1 on a protocol error.

**The campaign.** ember keeps the one record of what has been seen (the campaign engine,
`uam/fuzz_campaign.h`) and tells the brain what each run added (JUDGE and FIND frames,
`fuzz-formats.md`); the brain keeps none. An input is interesting when it sets a bucket
never hit or reaches a hit-count class (1, 2, 3, 4-7, 8-15, 16-31, 32-127, 128+) that bucket
never had. A crash is saved when its `(reason, status)` is new (minimized), and up to
`--crashes-per-sig` (4) times when it ran code no earlier crash ran (as found: another way
into that pc, or another bug there); a different crash met while minimizing is saved too.
`--iter-insns N|auto` sets ember's per-input budget (`auto`: 5× the slowest queue entry
after the baseline). The queue is walked
in cycles:

- **Favored entries:** for every bucket, the entry that hits it with the smallest
  instructions × size, and the longest entry that hits it; each of the two sets covers
  every bucket seen. The cheapest entries are the shortest ones, and mutations start from
  the entry's length, so working on those alone would keep a campaign at short inputs:
  the second set has every piece of code mutated from its longest form as well. While
  favored entries wait, 99% of the others are skipped (95% once fuzzed, 75% when new,
  otherwise).
- **Calibration:** each new entry runs twice. ember is deterministic, so the runs must
  match; a difference is reported as `UNSTABLE` with the buckets that differed (the harness
  leaks state between iterations) and counted in the status. `--no-calibrate` skips it.
  After the baseline the brain also suggests a per-input budget (5× the slowest entry).
- **Trimming:** a new entry, when first chosen, loses chunks for as long as its path (its
  buckets and hit-count classes) stays the same, up to `--trim-probes` runs (64);
  `--no-trim` keeps entries as found. ember does the cutting and the runs (a STAGE, like a
  batch of mutations), and a cut that ran new code becomes a queue entry of its own
  (`op:trim`).
- **Energy:** each chosen entry gets a number of mutations from a performance score
  (cheaper, wider, later-found and deeper entries score higher; the cost is the exact
  instruction count) times its power schedule's factor (`--schedule`):

| `--schedule` | gives energy to | use |
|---|---|---|
| `fast` (default) | entries on rarely exercised paths; grows each time an entry is chosen, capped | the general choice |
| `explore` | everyone alike (the score alone) | breadth; a parallel set's variety |
| `exploit` | the score at full strength: small, cheap, new entries | depth |
| `coe` | like `fast`, but nothing to entries whose path runs more often than the mean | aggressive rarity, next to `fast` |
| `rare` | entries that hit the rarest buckets | narrow checks: parsers, protocols |
| `lin` | like `fast`, but energy grows linearly with the times an entry was chosen (÷ its path's frequency + 1) | a gentler `fast` |
| `quad` | like `lin`, but quadratic in the times chosen | between `lin` and `fast` |
| `seek` | like `explore`, without the cost term: slow entries get as much as fast ones | targets where the slow inputs are the interesting ones |
| `mmopt` | like `explore`, plus ×2 for entries within 3 generations of the deepest and ×1.5 within 5 | the newest discoveries |

- **Compare logging (input-to-state):** when ember offers it (HELLO), each queue entry,
  when first chosen and after trimming, goes through an input-to-state stage in ember (a
  STAGE; `uam/fuzz_i2s.h`). The entry runs once with its compares logged; for every compare
  ember finds one operand's bytes in the input (either byte order, at the
  compare's width and at the narrowest width that holds both values) and writes the other
  operand there, plus and minus one for ordered compares: at most `--cmplog-max` (256)
  candidates per entry, run back to back like a batch of mutations, each kept if it is new
  coverage (`op:cmplog` in the queue). Constants of the code that the input lacks become
  dictionary integers (up to 256).
  This finds what havoc cannot guess: a 32-bit magic value from `hello world!` in a few
  thousand executions on every tested board. `--no-cmplog` turns it off; the candidates
  count as mutated executions for `--iters`, and the status counts `cmplog_execs` and
  `cmplog_finds`.
- **Adaptive operators:** every `--reweight-every` (5000) mutated executions, the
  mutator's operators that produced interesting children get more weight (up to 4× their
  default, at least 1); 0 keeps the defaults.

**Mutation in ember.** The brain mirrors its queue into ember (CORPUS frames) and asks for
batches (HAVOC: up to `--batch` (1024) mutations of one entry, at least `--min-energy` (64)
each time an entry is chosen). ember's mutator (`fuzz/mutate.c`, `uam/fuzz_mutate.h`)
copies the entry, splices it first with another entry `--splice` (150) times in 1000 (AFL's
crossover between the first and last differing byte), and stacks 2-128 (`--stack`,
log-uniform) havoc operators:

| Operators | What they do |
|---|---|
| `BIT_FLIP`, `BYTE_FLIP`, `BYTE_ARITH`, `WORD_ARITH_LE/BE`, `DWORD_ARITH_LE/BE` | flip bits; add or subtract up to 35 at 8, 16 or 32 bits in either order |
| `SET_INTERESTING_U8`, `_U16_LE/BE`, `_U32_LE/BE`, `RANDOM_BYTE` | write a boundary value (0, -1, 127, 128, 255, 256, 32767, 65535, 2^31 ...) or a random byte |
| `DELETE_CHUNK`, `CLONE_CHUNK`, `OVERWRITE_CHUNK`, `SWAP_CHUNKS` | delete (up to two thirds of the input), duplicate (up to the input's length), overwrite or swap runs of bytes |
| `INSERT_BLOCK` | insert constant or random bytes (up to the input's length) |
| `RESIZE` | the long step: grow to a length between the input's and `--max-size`, or cut to one between the input's and one byte, either as likely, log-uniformly on that side (each power-of-two size class alike); bytes are added or cut at a random place |
| `DICT_INSERT`, `DICT_OVERWRITE`, `DICT_INT_ADJ` | insert or write a dictionary token; write an integer token ±1 |

Chunk lengths are drawn the way AFL's havoc draws them: a third of the time 1-32 bytes, a
third 32-128, a third 128-1500 (now and then more), within the operator's limit. The length
moves both ways and evenly: whenever an operator that changes it is drawn, a coin decides
between growing and cutting, whatever the weights. `DELETE_CHUNK`, `CLONE_CHUNK` and
`INSERT_BLOCK` become a cut or a growth of the same scale; a `DICT_INSERT` inserts its
token or cuts as many bytes as a token has (as does `DICT_INT_ADJ` where it would insert);
`RESIZE` picks its side. So a mutation is as likely to come out shorter as longer wherever
the input stands between one byte and `--max-size`: with `--max-size 9000`, of the
mutations of a 16-byte, a 512-byte and a 4500-byte input about 40% are shorter, 36% longer
and the rest the same length (39-44% and 42-46% with a dictionary), about 3% of a 16-byte
input's pass 2000 bytes and about 5% of a 9000-byte input's fall under 64. A chain of
mutations from one byte or from the cap covers the whole range and spends about as long
under 64 bytes as over 1000. At the ends the move without room does nothing: an input at
the cap shrinks or stays. A direction whose operators have weight 0 stays off. Set
`--max-size` to the buffer the firmware really has: the lengths spread up to it, and inputs
longer than the route delivers only cost time. ember judges each mutation itself against its record (every INPUT and every batch
counts) and sends a FIND only for new coverage, a novel crash, or a hang (up to
`--max-hangs`); a batch ends with its totals (executions per verdict, per operator, per
path), and says PROGRESS when it has been silent for a second, so a batch of slow inputs
does not look like a dead ember. A board can repair mutated inputs before they run
(`uam_machine_set_input_fixup` on its named input, or `uam_fuzz_set_fixup`: a checksum, a
length field), and what ember reports and the brain saves is the repaired input. ember tells the brain its operators and their default
weights (MUTATOR_INFO), so the two never disagree. Trimming and input-to-state run in ember
as well, as stages of the campaign engine (`uam/fuzz_campaign.h`): the brain names the
entry and a limit, ember derives the inputs, runs them with no frame in between and
reports as it does for a batch. Seeds, calibration, crash minimization and imports from
other workers still go as single INPUTs.
`build/tests/test_mutate` checks the mutator, including the growth figures above, and the
input-to-state candidates.

**Dictionaries** (`--dict FILE`, repeatable) are ember's own format, one token per line:
`"text"` (escapes `\\ \" \t \n \r \xHH`), `hex:BYTES`, and integers `u8:`, `u16:`,
`u32:`, `u64:` laid out in the target's byte order (HELLO says which), or `u16le:` /
`u32be:` ... for a fixed one; a line starting with `#` is a comment. Integer tokens are also written ±1 (the
off-by-one neighbour of a boundary check). A byte token is at most 256 bytes. The brain
reads the files (`fuzz/tools/fuzz_dict.py`; `--selftest` checks the parser, `--check FILE`
lists what a file holds) and hands the tokens to ember's mutator.

```
# a bootloader's command protocol
"BOOT"
hex:a55a
# a magic the firmware compares as a word, and a network-order field
u32:0xdeadbeef
u16be:0x0800
```

**Parallel workers.** `fuzz/tools/fuzz_launch.py -n N --out DIR -- <uam-run arguments>`
runs N brain + ember pairs, each on its own port with its own output directory `DIR/NAME`
(`main`, `w1`, `w2`, ...): its queue, crashes, hangs, `status.jsonl`, `brain.log` and
`ember.log`. The workers share through `DIR` (`fuzz_brain.py --sync-dir DIR --name NAME`):
every `--sync-interval` seconds (60), each one runs the other workers' new queue entries
once and keeps those that are new to it (`sync:` names; `.synced/PEER` records how far it
read). Each worker gets a distinct RNG seed, derived from `--seed-rng` and its name
(identical workers would only repeat each other), and the next schedule of `--schedules` (by default the five main ones; any of the nine can
be listed)
(default all five, round robin); `--dict` and `--seeds` go to every worker, `--brain-arg`
passes anything else. A worker whose brain or ember exits unexpectedly is restarted from
its queue (`--max-restarts`, 5); Ctrl-C stops them all with BYE. The firmware boots once:
the launcher gives every ember `--fuzz-snapshot DIR/begin.snap`, starts `main` first, and
starts the others when `main` has saved the begin snapshot there, so they (and every
restart) skip the boot; a snapshot left from an earlier launch is removed first, and
`--no-shared-boot` boots each worker.
`python3 fuzz/tools/status_tui.py --sync-dir DIR` shows one row per worker and a total.

ember dials the brain (retrying for a few seconds while it starts), runs each INPUT through
the harness and answers it, in lockstep, with one COVERAGE (SPARSE, when asked) or CRASH
frame. It needs fuzzing
boundaries (`--fuzz-begin` or `--cov-boundary`) and replaces `--fuzz-replay`. ember connects
first, boots to the begin pc, and only then sends HELLO and the INIT map, so a long firmware
boot is not spent between HELLO and the first reply; the brain waits for HELLO up to its
`--hello-timeout` (600 s). Any coverage source works: HELLO describes the scheme (site,
hashed-pc or block model, N, rotation); plain block edges (no stock extensions) describe none. HELLO's arch
is the ISA's name (`rv32imac`, `mips32be`, `armv7`, ...).

| Verdict | Reply | `status` |
|---|---|---|
| ok | COVERAGE (or SPARSE), kind ITER | |
| crash (fault) | CRASH `FAULT` (1) | the faulting pc, low 32 bits |
| crash (guard) | CRASH `GUARD` (2) | the pc of the guarded access, low 32 bits |
| crash (oracle) | CRASH `ORACLE` (3) | the detail, low 32 bits: the crash pc, a halt status, the interrupt lines |
| hang | CRASH `HANG` (4) | the stop reason (ember's `uam_stop_reason` number) |
| timeout | CRASH `TIMEOUT` (5) | 0 |
| rejected | CRASH `REJECTED` (6) | 0 (the brain counts it, and saves nothing) |

A malformed INPUT gets CRASH `MALFORMED` (7) and runs nothing; unknown frame types are
ignored. A BYE from the brain ends the run (exit 0); the brain closing the connection without
one is an error (exit 2), and so is an internal error of ember's, which sends BYE first. SIGINT
sends BYE in place of the pending reply and exits 130. TCP keepalive (probes after a minute
of silence, giving up about a minute later) notices a brain host that vanished without
closing the connection. A brain that is still there but stops answering is waited for
unless `--fuzz-net-timeout SEC` is given: then SEC seconds without a frame from the brain
(or without it taking one of ember's) ends the run with an error.

```
python3 fuzz/tools/fuzz_brain.py --listen 127.0.0.1:7000 --seeds seeds/ --out run/ --status-file run/status.jsonl
build/uam-run -b rv32-ref --cov-blocks --fuzz-begin 0x80000336 --fuzz-crash-pc 0x80000240 \
    --fuzz-input uart0 --fuzz-iter-insns 200000 --fuzz-net 127.0.0.1:7000 build/tests/firmware/fuzztarget.elf
python3 fuzz/tools/status_tui.py run/status.jsonl     # a live status screen (--once: one frame)

python3 fuzz/tools/fuzz_launch.py -n 4 --out camp --seeds seeds/ --dict fw.dict -- \
    build/uam-run -b rv32-ref --cov-blocks --fuzz-begin 0x80000336 --fuzz-crash-pc 0x80000240 \
    --fuzz-input uart0 --fuzz-iter-insns 200000 build/tests/firmware/fuzztarget.elf
python3 fuzz/tools/status_tui.py --sync-dir camp      # every worker
```

Replaying the brain's `crashes/` or `hangs/` directory (`--fuzz-replay run/crashes`) runs
the inputs and skips their `.info` notes. `meson test -C build --suite fuzz-lists` checks the protocol frame
by frame against a scripted brain (`tests/fuzz/fake_brain.py`, SPARSE and CMPS included), runs
sessions with the brain (`tests/fuzz/brain-check.sh`: the planted crash is reported, saved
with its reason, minimized, and the session ends with BYE; a session per schedule; the
planted crash found from a dictionary alone; the magic value found by compare logging and
not without it) and a parallel campaign
(`tests/fuzz/parallel-check.sh`: two workers import each other's entries, find both planted
crashes, and survive a killed ember). `meson test -C build --suite unit` runs `tests/fuzz/brain_unit.py` (the
campaign logic on synthetic data), `test_mutate` (the mutator and the input-to-state
candidates), `test_fuzz`'s checks of the harness's hit lists, compare logging and the
campaign engine's batches and stages, and
`test_cov`'s of the compare pass and `CMPLOG` on every ISA and backend.

**Throughput.** `meson test -C build --suite fuzz-throughput` (part of `meson test -C build`) times a HAVOC batch with
a fixed seed, input-to-state stages and trims on the RV32 fuzz target
(`tests/fuzz/throughput.py`) and one long run of plain guest code on the same host, and
compares what one execution costs, in guest instructions at that plain speed, with the
figures recorded per host architecture in `tests/fuzz/throughput.ref`: a stage that costs
over 1.5 times its figure fails, so a change that halves the executions per second is
noticed. `meson compile -C build fuzz-throughput-record` rewrites the figures (on a quiet machine, after a
change meant to move them); `THROUGHPUT_REF=` only reports. Between two inputs the harness
restores its snapshot: only the memory that changed is written back, and translated code
keeps its links and the JIT's data cache its entries for loads unless the input changed
the processor's address translation or the firmware translates through page tables in its
memory ([`uam-isa.md`](uam-isa.md), `uam_xlate.regs_only`).

### The coverage region (`--cov-shm PATH`)

ember publishes the harness's maps in a coverage region, so a program outside it can watch a
run. It is a side channel next to any frontend (`--fuzz-replay` or `--fuzz-net`) and needs
fuzzing. `fuzz/tools/cov_region.py` reads one: `header`, `gen`, `scheme`, `stats REGION
[WHICH]` and `extract REGION OUT [WHICH]`, where WHICH is `bitmap` (the default), `current`,
`init` or a generation number. It takes the region's file path, so a shared-memory name is
`/dev/shm/NAME`; the ring holds the last ring − 1 generations, so `init` and old
generations go away as the run publishes more.

- The ring (`--cov-shm-ring N`, default 8 slots) gets the INIT map, then one ITER snapshot
  per ok iteration, written with the format's seqlock protocol. Crashed, hung and timed-out
  iterations are not published.
- The live bitmap is the union (saturating sums) of everything published, so
  `cov_region.py stats` shows the campaign's coverage so far.
- The header carries the scheme flags (site lists and block coverage; none for plain block
  edges) and
  ember's pid and start time as the owner. A region another live process owns is refused,
  decided under an exclusive `flock`, so two ember processes starting at once cannot both
  take it, with an owner alive only if its pid still has the recorded start time (a reused
  pid is not the owner). A PATH with a `/` after its first character is a file, otherwise a
  shared-memory name under `/dev/shm`. The region stays after ember exits.

```
build/uam-run -b mips-ref --cov-sites fw.uams --cov-boundary fw.uamb --fuzz-input uart0 \
    --cov-shm /tmp/ember.cov --fuzz-net 127.0.0.1:7000 fw.elf
python3 fuzz/tools/cov_region.py stats /tmp/ember.cov
```
