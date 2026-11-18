# Ember's fuzz harness on your board

The harness (`uam/fuzz.h`, ember `docs/fuzzing.md`) does three things per campaign:

1. **Boot** until the first arrival at a begin pc (`-n` bounds it). Coverage up to here is
   the *INIT* map. `--fuzz-begin-skip N` lets the boot pass the begin pc N times first.
2. **Snapshot** there.
3. **Per input**: restore the snapshot, clear the map, deliver the input through the route,
   run until the iteration ends, an oracle fires, or `--fuzz-iter-insns` runs out; classify.
   A crash does not end the campaign. An ember internal error does.

Everything below is configured on the runner's command line, so it works with the
project's `build/<board>-run` through `run.sh`.

## A `fuzz.sh` for the project

Keep the configuration in one script next to `run.sh`, committed, so every journal entry
can cite it:

```bash
#!/usr/bin/env bash
# fuzz.sh - fuzz configuration for <target> (fuzz-XX.md). Extra args go to the runner, e.g.
#   --fuzz-replay fuzz/smoke/      --cov-out build/cov.map      --jit
set -euo pipefail
PROJ="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export TRACE="${TRACE-}"      # no per-input exception trace (TRACE=exc to get it back)
export UNKNOWN_MMIO="${UNKNOWN_MMIO-fault}"   # a wild access faults instead of reading 0
exec "$PROJ/run.sh" -n 2000000000 \
    --console "file:$PROJ/build/fuzz-console.txt" \
    --cov-ngram 3 --cov-rotate \
    --fuzz-begin 0x80123450 \
    --fuzz-crash-pc 0x80001234 \
    --fuzz-input uart0 \
    --fuzz-iter-insns 5000000 \
    "$@"
```

```bash
IMAGE=fw.bin timeout -k 5 600 ./fuzz.sh --fuzz-replay fuzz/smoke/ > build/replay.txt 2> build/replay.log < /dev/null
python3 <ember-fuzz>/scripts/replay_summary.py build/replay.txt
```

`fuzz.sh` goes through `run.sh`, so the board's images, options and rules stay in one
place (a PC project sets `FLASH=` or `CODE=`/`VARS=` instead of `IMAGE=`); it turns off
`run.sh`'s exception trace, which is noise across thousands of inputs. (A project
scaffolded before ember-fuzz existed has `${TRACE:-exc}` in `run.sh`; change it to
`${TRACE-exc}` so an empty `TRACE` switches the trace off.) It also moves the guest's
console off stdout: the replay lines go to stdout through a buffer while console output is
written straight away, so a handler that prints (echo, a prompt) would land between the
replay lines, and in the middle of one. The last `--console` given wins, so this overrides
one in `run.sh`. Plain runs and triage keep the console on stdout.

**`UNKNOWN_MMIO=fault` is what makes wild pointers crashes.** `run.sh` defaults to `zero` for
bring-up (a PC project to `fault` already), and with `zero` an access no device or RAM claims
reads 0 and goes on: the same load is `crash-fault` under `fault` and `ok` under `zero`. A
boot that now stops at an unmodelled register has found a hole in the board: give it a
device or a `board_windows[]` catch-all rather than going back to `zero`, and remember that
a catch-all answers wild accesses inside its window too.

The runner refuses (exit 2) `--fuzz-*` options without `--fuzz-begin` (or a boundary
file's `--cov-boundary`), fuzzing without a frontend (`--fuzz-replay`, or `--fuzz-net`), and
`--fuzz-begin` together with `--gdb` or `--save-snapshot`.

### Reaching the handler

Firmware that reaches the handler only after console input (a login, a menu choice, a
command that starts the service) gets that input during the boot, scripted so it is
deterministic and typed at the prompt rather than eaten by a boot-time FIFO clear:

```bash
# fuzz/boot.script, one step per line: expect TEXT / send TEXT (\n, \r, \t, \xHH escapes)
#   expect login:\x20
#   send admin\n
#   expect >\x20
#   send cli\n
# in fuzz.sh, the console line becomes
    --console "file:$PROJ/build/fuzz-console.txt,script=$PROJ/fuzz/boot.script" \
# or, for a single line typed once "> " is printed
    --console "string:cli\n,after=> " \
```

The bytes the board's UART holds when the boot reaches the begin pc are part of the
snapshot: anything the boot sent but the firmware has not read yet precedes **every**
input (on ember's fuzz target, a leftover `ok` turns every planted crash into an ok). So
the script's last `send` must be consumed before the begin pc, typically the command that
enters the handler's read loop. Input still waiting in the console when the snapshot is
taken is host state, not snapshot state: never leave a step pending into the iterations.

## Boundaries

| mode | options | an iteration ends |
|---|---|---|
| begin-only | `--fuzz-begin PC` | when a begin pc is reached again (the handler loop came round) |
| paired | `--fuzz-begin PC --fuzz-end PC` (both repeatable) | at any end pc; no end reached = timeout |

- **The begin pc must come before the input is read.** The snapshot is taken on arrival and
  the input is delivered right after; if the handler already polled an empty FIFO, every
  input runs as "no data". Use the handler's first instruction.
- Firmware that runs the handler during start-up (a self-test, a first request of its own):
  `--fuzz-begin-skip N` (the snapshot is at arrival N+1. A boot that never gets there runs until something stops it, so bound it with `-n`: it then stops with exit 2).
- Several handlers: repeat `--fuzz-begin`; the snapshot is at whichever the boot reaches
  first, and each input goes wherever the firmware takes it.
- Recursion: with `--fuzz-end` the first end reached ends the iteration, so a recursive
  handler stops at its innermost return. Keeping it in one input takes nested boundaries:
  depth-zero markers in a boundary file (`references/lists-and-brain.md`), or
  `uam_fuzz_config.nested` in a program.
- Code between an end and the next begin (the RTOS loop that waits for the next request)
  runs once, at boot, never per input.
- Sites and boundary pcs are guest **virtual** pcs; code reached through both kseg0 and
  kseg1 has two of each.
- Code in a compressed ISA mode runs at its address **with bit 0 set**: Thumb (a Thumb
  function symbol already has it), and MIPS16e and microMIPS on PIC32-class MIPS (the ELF
  symbol is even, readelf marks it `[MIPS16]`/`[MICROMIPS]`: set bit 0 yourself). Every
  pc option and every replay `pc=` uses that form.
- On 8-bit PIC the pc is the program-memory bus address: twice the word address on the
  baseline and mid-range families, the PC itself on PIC18.
- Several processors (`-o cpus=N`): any of them can hit a boundary, a crash pc, an
  exception or a guard; the result's pc is that of the processor that ended the iteration
  (a guard hit also names it, `uam_fuzz_result.guard.cpu`).

## Routes (`--fuzz-input`)

| route | delivery | when |
|---|---|---|
| a board input (`uart0`, ...) | the board's published input function: bytes queued as if received | the input really arrives through a device: most faithful, exercises the driver |
| `mem:ADDR[,len=ADDR][,max=N][,reject]` `[,before=N[:DIR]][,after[=N][:DIR]][,extent=@OFF:SZ[:le\|:be][+D\|-D]]` | written at virtual ADDR as a store translates it (truncated to N, default 4096; `reject` refuses longer ones), optionally its length as a 32-bit word in the CPU's byte order; optional redzones around the buffer ("Redzones" below) | a handler that takes a buffer and length: quickest, skips the driver |
| `mmio` | the stream `--mmio-rule ADDR=input,width=N` registers read (`--mmio-input` for a plain run) | firmware that reads its input straight from device registers |

A route that cannot deliver an input (too long for its buffer) returns `UAM_ERR_REJECTED`:
verdict *rejected*, nothing runs, the campaign goes on. Any other error stops the campaign
(exit 2).

Every stock reference board publishes its console UART as `uart0` (the 8051 board its
serial port), except the x86-64 PC board, whose is `com1`. Those feeds drop what does not
fit the receive buffer without a word (4096 bytes on ember's 16550, 256 on the 8051
serial port), so a longer input is tested as its prefix and is not *rejected*.

### Redzones (`mem:` route)

The `mem:` route can guard the memory around its buffer `[ADDR, ADDR+max)` with STOP guards
("Guards" below): an access that touches one ends the iteration before it happens, with
verdict *crash (guard)*. `DIR` is `r`, `w` or `rw` (the default), so under- and over-reads
and under- and over-writes are each caught or let through.

| option | guarded range | catches |
|---|---|---|
| `before=N` | `[ADDR-N, ADDR)`, armed at the first input | `buf[-1]`, a header read from in front of the buffer |
| `after[=N]` | `[ADDR+L, ADDR+max+N)`, re-armed for each input | anything past the input's legitimate extent L: the unused rest of the buffer, and N bytes beyond it (default 0) |
| `extent=@OFF:SZ[:le\|:be][+D\|-D]` | (places `after`; without `after` it is an error) | L is the SZ-byte (1, 2, 4, 8) length field at input offset OFF, in the given byte order (default the CPU's), plus D, instead of the delivered byte count |

**When to use them.** On flat memory (MIPS kseg0, RV32 M-mode, ARM and x86 with identity
maps, every 8-bit core) an over-read or overflow of a buffer lands in other mapped RAM: no
exception, no oracle fires, and the bug only shows if the corruption later crashes
something. Redzones turn the out-of-bounds access itself into the crash, at the instruction
that made it. Use them whenever the handler parses a buffer with lengths or offsets from the
input (TLVs, packet headers, string copies), which is most targets (on 8-bit AVR and PIC,
`--guard` instead: "Per ISA"):
- `after:r` (or plain `after`) for a parser that should never read past what it was given;
  add `extent=@OFF:SZ` when the input carries its own length (a header's length field), so
  a lying length is caught even inside the delivered bytes. Example: a 4-byte header whose
  big-endian 16-bit field at offset 2 counts the payload after it is
  `extent=@2:2:be+4`.
- `before=N` for code that indexes backwards or reads a header in front of the buffer.
- `after=N` to cover memory past the buffer's end (ADDR+max), which belongs to someone else:
  only with care, and size N from the memory map in Ghidra.
- Code that terminates a string in place (`buf[len] = 0`) writes into `after`'s zone
  legitimately: use `after:r` there, or every input is a crash.

L never falls before the end of the length field itself, which the firmware may read; an
input too short to hold the field gets no `after` zone. The older forms still work: `guard`
is `after:r`, `guard=@OFF:SZ[+-D]` is `after:r,extent=@OFF:SZ[+-D]`, and `guardabs=ADDR:N`
a fixed read zone (`--guard` does that generally). They now end at the buffer's end
(ADDR+max), no longer at the end of the page: a configuration written for the old behaviour
needs `after=N` to cover further.

### Publishing an input

The device gets a feed function in its own file and header: no machine, no harness, just
"bytes arrived". The tested example is `soc_uart_feed` in ember-dev-build's
`references/examples/soc_uart.c`:

```c
/* soc_uart.c: queue bytes as if received; return how many fit the RX FIFO */
size_t soc_uart_feed(uam_device *d, const uint8_t *data, size_t len)
{
    soc_uart *s = to_uart(d);
    size_t n = len < RX_CAP - s->rx_count ? len : RX_CAP - s->rx_count;
    uart_receive(s, data, n);          /* the same path chardev input takes */
    return n;
}
```

The board, the only file that sees `uam_machine`, wraps it and publishes it by name:

```c
static uam_status uart_input(void *ctx, uam_machine *m, const uint8_t *data, size_t len)
{
    (void)m;
    soc_uart_feed(ctx, data, len);   /* the excess is dropped, as on a real line */
    return UAM_OK;
}
/* in build(), after the UART is attached: */
if ((st = uam_machine_add_input(m, "uart0", uart_input, uart)) != UAM_OK)
    return st;
```

- A stock 16550 needs no wrapper: `uam_board_add_uart_input(m, "uart0", uart)` from
  `uam/boards.h`.
- Choose the semantics deliberately: a FIFO drops the excess (as a line does) or the wrapper
  returns `UAM_ERR_REJECTED` for inputs that cannot fit, so they are not counted as tested.
- A NIC route queues one frame per input into the RX ring the driver gave the device; a
  mailbox route writes a message and raises the doorbell interrupt. Either way it goes
  through the device's own receive path, so the interrupt and status bits are right.
- Delivery happens between runs, right after the snapshot restore: the feed must leave the
  device exactly as a real arrival would (interrupt line raised, status bits set).
- When the firmware drops inputs whose checksum, length field or framing is wrong, give the
  input a fix-up: `uam_machine_set_input_fixup(m, "nic0", fix, ctx)` with
  `size_t fix(void *ctx, uint8_t *data, size_t len, size_t cap)` repairs each mutation in
  place before it runs (it must be idempotent). ember's mutator then spends its runs past
  the check, and what the brain saves is the repaired input, which replays as it is.

## Oracles

| oracle | option | verdict |
|---|---|---|
| exception classes | `--fuzz-crash-exc LIST` (default `fetch,load,store,align`; also `xlate`, `trap`, `sysreg`, `all`, `none`, and `default` to add to the default set: `default,trap`) | crash (fault), detail = faulting pc |
| crash pcs | `--fuzz-crash-pc PC` (repeatable) | crash (oracle), detail = pc |
| redzones | `mem:` route `before=`, `after`, `extent=` (above) | crash (guard), detail = pc of the access |
| guarded memory | `--guard SPEC` (repeatable, below) | crash (guard) (`,stop`, the default), or the bus fault for `--fuzz-crash-exc` to classify (`,fault`) |
| interrupts | `--fuzz-crash-irq N` (0..63): line N as the ISA reports it ("Per ISA") | crash (oracle), detail = lines |
| reset during an iteration | always | crash (oracle) |
| firmware halt with non-zero status | always | crash (oracle), `status=` |
| debug trap (an encoding the core lifts to a stop into the debugger: "Per ISA") | always | crash (oracle), detail = pc |
| deadlock / livelock | always | hang |
| `--fuzz-iter-insns` used up | default 10000000 | timeout |

Choosing them from the firmware:
- `--fuzz-crash-pc` at the RTOS's fatal paths: the exception reporter (VxWorks
  `excExcHandle`/`excTask` path, a Linux `die`/`panic`), `assert` failure, stack-check fail.
  Exceptions the firmware *handles* are delivered to it as usual; catching them at these
  functions is what turns them into crashes.
- Firmware with a TLB (kuseg tasks, Linux user space) takes TLB refills all the time: never
  add `xlate`. `trap` also covers system calls and breakpoints (on x86, software `INT n`).
- **A watchdog** whose reset fires during a slow input is a crash (oracle, reset), which is
  usually not a bug: set `--fuzz-iter-insns` below the watchdog period, or give the board an
  option that disables it for fuzzing (`-o wdt=off`, passed to the watchdog device's create
  function as a parameter) and journal it.
- Size `--fuzz-iter-insns` from an ok replay's `insns=` with a large margin (10x).
- Memory-safety bugs need a memory oracle: redzones on a `mem:` route, `--guard` around the
  firmware's own buffer on a device route (the buffer the driver copies the input into,
  from Ghidra). Without one, an overflow that corrupts memory without faulting is an ok.

### Per ISA

What a wild access or a bad instruction does differs by core; choose the oracles from it.

**Cores with exceptions** (RV32, MIPS32 incl. PIC32, ARMv7, PowerPC, AVR32, Xtensa, x86-64):
every class above exists; what the policy leaves out goes to the firmware's handler, where a
`--fuzz-crash-pc` catches it. One special case:
- **x86-64 PC boards**: an access no device claims reads all ones (master abort) and never
  faults, so the default classes rarely fire. CPU faults such as #UD, #GP and #DE are
  mostly class `trap` (the replay shows `trap cause 0x6` for #UD: the cause's low byte is
  the vector; some raised by helpers and MSR/CR accesses are `sysreg`), page faults
  `xlate`. `trap` also catches software `INT n`, which legacy BIOS code runs all the
  time: there, crash pcs at the firmware's exception handler are the oracle. ember's own
  fuzz-check does not run on `x86_64-ref`.
- **SMM handlers (SW SMI through port 0xB2, a comm buffer)**: turn on SMM code-fetch
  confinement (`uam_x86_set_smm_confine` in the board; `-o smm-confine=1` on the PC template and `x86_64-ref`).
  An SMM instruction fetch outside SMRAM (SMRR, and on AMD the TSeg/ASeg) is then a
  `fetch`-class crash at the first instruction outside - a call-out through a corrupted
  or NULL function pointer, a jump into memory the OS controls - instead of whatever that
  memory happens to hold (`crash-fault pc=0x2000 (fetch-fault cause 0x80000000 tval
  0x2000)`; without confinement the same input ran on and timed out). Firmware that
  enables Intel's SMM_Code_Chk gets the same crash. It needs SMRAM defined (SMRR programmed, SMM_ADDR/SMM_MASK
  with TValid - `uam_x86_preset_amd_smm` where the security processor sets them - or the
  board's SMRAM windows). Where the board decodes SMRAM through the SMM view, a `--guard`
  or `--dump-mem` inside SMRAM needs `p:0x400000000000+ADDR` (`UAM_X86_SMM_VIEW` + ADDR),
  since an unprefixed address is translated as processor 0 maps data when the run starts,
  outside SMM. A `mem:` route takes no `p:`: its plain address is translated as the hart
  stores at the begin pc, so a begin pc inside SMM reaches SMRAM by itself.
- **A processor shutdown is a `hang`, not a crash**: a triple fault, a machine check with
  CR4.MCE clear, or an RSM whose saved state no processor could be in (a handler corrupting
  its save area) all read `hang (deadlock (waiting with no interrupt source))`. Only stderr
  tells them from a real wait (`x86: RSM with an invalid saved state (...) at pc=..: hart
  shut down`), so replay each hang alone and read it.

**Cores without exceptions** (8-bit AVR, 8-bit PIC, 8051, 6502, Z80): there is no handler
and no class but the bus faults.
- A fetch, load or store that nothing answers (with `--unknown-mmio fault`, as `fuzz.sh`
  sets it) is still seen by the policy as `fetch`/`load`/`store`: with the default it is a crash (fault)
  (`store-fault ... tval` = the bus address). Outside the policy the core halts, and the
  input reads as a hang (`deadlock (waiting with no interrupt source)`). So never narrow
  the policy on these cores; `--fuzz-crash-exc none` turns every wild access into a hang.
- A wild access that lands in RAM is not a fault: on a board that is all RAM (`z80-ref`
  has 64 KiB of it; ember's own check leaves a hole with `--ram 0xFF00`) only guards see it.
- Encodings the core does not have stop into the debugger on 8-bit AVR (also `BREAK`),
  PIC and the 8051 (its one undefined opcode, 0xA5): crash (oracle) with that pc. The
  6502's `STP` and NMOS `JAM` stop the core until reset: a hang.
- Addresses. 8-bit AVR and PIC data space is at bus address 0x800000 + data address. A
  `mem:` route, its redzones and a `--guard` without `p:` take the plain data address, as
  the firmware uses it (an avr-gcc data symbol minus 0x800000), and ember adds the base;
  `p:`, `--dump-mem` and MMIO rules take the bus address (an avr-gcc data symbol already
  has that value). Passing the symbol value to `mem:` or an
  unprefixed `--guard` lands at 0x1000000 + address, outside the data space. The 8051's
  external data (MOVX) is at 0x1000000 + address and the Z80's I/O ports at 0x10000 + port,
  bus addresses everywhere. The 8051's internal RAM is not on the bus: it cannot be guarded
  or written by a `mem:` route.

`--fuzz-crash-irq` lines, as each ISA reports them (ember's `docs/fuzzing.md` has the
reference boards' wiring):

| ISA | line N |
|---|---|
| RV32 | the `mcause` interrupt code (7 machine timer, 11 machine external) |
| MIPS32 | the Cause.IP bit (0..7) that was pending and enabled |
| ARMv7 | 0 IRQ, 1 FIQ (on `arm-ref` every device interrupt is IRQ, through the GIC) |
| PowerPC | 0..7 external sources, 8 soft reset, 9 machine check, 10 Book E critical input; the timers have none |
| x86-64 | the processor's pins: 0 INTR, 1 NMI, 2 SMI, 3 INIT, 4 A20M# |
| 8-bit AVR | the interrupt vector (1..63) |
| AVR32 | 0..3 INT0..INT3, 4 NMI |
| 8-bit PIC | 0 the peripheral (PIC18 with IPEN: high-priority) line, 1 the low-priority one |
| 8051 | the interrupt source n (vector 0x03 + 8n) |
| 6502 | 0..29 wired-OR into IRQ, 30 NMI |
| Z80 | 0 /INT, 1 /NMI |
| Xtensa | the interrupt of that number in the model's table |

## Guards (`--guard`, `--watch`)

Guards are ember's, not the harness's (`uam/guard.h`): they work in any run, fuzzing or not.
`--guard SPEC` (repeatable) guards a byte range of guest memory:

```
[p:]ADDR[-BEFORE][+AFTER][:r|w|rw][,stop|,fault|,log][,name=NAME]
```

The range is `[ADDR-BEFORE, ADDR+AFTER)` around the anchor ADDR: `0x1000+16` is the 16 bytes
from 0x1000, `0x1000-16` the 16 in front of it, `0x1000-16+16` both, a bare `0x1000` one
byte. Numbers are C literals; BEFORE and AFTER take a K/M suffix. ADDR is virtual,
translated as processor 0 maps it when the run starts (after `--load-snapshot`), a page at a
time; `p:` makes it physical. The translation is a data access's, so on the 8-bit AVR and
PIC ADDR is the data address and ember adds the 0x800000 base ("Per ISA"). Any
data access that overlaps the range hits it, in the directions given (default `rw`). The
name defaults to the text before the first `,`.

| action | the access | the run |
|---|---|---|
| `stop` (default) | does not happen | stops at the instruction (exit 129; in gdb, SIGSEGV on that processor); fuzzing: crash (guard) |
| `fault` | does not happen | the guest takes the bus fault an unmapped address raises: crash (fault) if `--fuzz-crash-exc` covers it, the result naming the guard as well |
| `log` | happens | goes on; a line on stderr per access (`--watch SPEC` is `--guard SPEC,log`) |

```bash
./run.sh --guard 0x80100000-16+16:w        # catch writes 16 bytes either side of 0x80100000
./run.sh --watch p:0x3000+4:w              # log every write to 4 physical bytes
./fuzz.sh --guard 0x80000470+256:r,name=g_buf --fuzz-replay fuzz/smoke/
```

- **A `--guard` is armed during boot.** In a fuzz run it is armed from the start, so the
  boot is held to it as well: start-up code clearing BSS or a `memset` of the buffer stops
  at a write guard, and the boot then never reaches the begin pc: no snapshot, no inputs
  run (exit 2, `boot did not reach the begin pc ...: guarded memory access`). Use `:r` for
  a buffer the boot writes, guard memory the boot does not touch, or, in a program, add the
  guard (`uam_machine_add_guard`) after `uam_fuzz_boot`. A route's redzones are armed only
  once inputs arrive, so they never have this problem.
- Instruction fetches, debug accesses (gdb, `--dump-mem`) and DMA are not guarded: a NIC
  route whose device DMAs the frame into the ring does not trip a guard on the ring; the
  driver reading past it does.
- A guarded page loses the JIT's direct memory access, so every access to it goes through
  the bus: keep guards small and few on hot pages.
- Guards are not machine state: snapshots neither save nor restore them, and they stay in
  place across the harness's restores.
- `--watch` is a debugging aid for triage (who writes this word?), not an oracle.

## Return guard and guarded heap

Two more memory oracles, for what redzones and guards cannot cover because the address is
not known beforehand. Both report as `crash-guard` (exit 25) with a kind in the replay line.

**`--guard-returns`** stops when a return goes to an address that no call instruction
returns to: a return address overwritten on the stack. Add it to `fuzz.sh`; it costs a few
percent on ordinary code (more where tiny functions are called in a tight loop: measure).

- The boot is held to it. A stop during the boot (`boot did not reach the begin pc ...:
  guarded memory access`) is nearly always a scheduler entering a new task by *returning*
  into its entry function. Run the plain boot once with `--guard-returns=log`: each such
  target is listed once (`guard return (log): ... returns to 0x...`); check in Ghidra that
  it is a task entry or a deliberate computed return, then name it: `--return-ok ADDR` (or a
  symbol), repeatable. Journal each one.
- It passes returns into other tasks, `longjmp` and frames built before tracking, and it
  misses a return address overwritten with another valid return address.
- Not for x86 code built with return trampolines (retpolines).
- `replay: ...: crash-guard pc=0x... (return: returns to 0x..., which no call returns to)`:
  the pc is the returning instruction, so the function that owns it is the one whose frame
  was overwritten. `--backtrace` shows the calls above it.

**The guarded heap** needs the board: ember-dev-build's `board-skeleton.md` ("A guarded heap
behind the firmware's allocator") hooks the firmware's allocation functions into
`uam/heap.h`. With that in the board, switch it on in `fuzz.sh` (`-o guard-heap=1`).

- Kinds: `heap-redzone` (an access just outside a live block: the off-by-one, the trusted
  length), `heap-freed` (inside a freed block, also through a pointer `realloc` left
  behind), `heap-wild` (arena memory in no block), `heap-double-free`, `heap-bad-free`.
- The line names the block and where it was allocated and freed (`allocated at 0x...,
  freed at 0x...`: return addresses in the callers); look those up in Ghidra first.
- Every input starts from the heap as it was at the begin pc (it is in the snapshot).
- It replaces the firmware's allocator, so it finds the firmware's misuse of the heap, not
  bugs of the allocator itself. A hit inside allocator-looking code after the boot means a
  heap routine the board did not hook: back to ember-dev-build.
- Heap-heavy handlers run slower with it (accesses to the arena are checked one by one).
  For throughput, run most workers without and one or two with it, sharing their queues.

A snapshot (`--fuzz-snapshot`, `--save-snapshot`) carries both oracles' state and is only
loaded with the same oracles on; `fuzz_launch.py`'s shared boot does that by itself as long
as every worker gets the same options.

## Reads of memory nothing wrote (`--fuzz-uninit`)

Opt-in, off by default, and a differential check, not a shadow memory: **a clean result is not
a proof that nothing reads uninitialised memory.** Use it when the handler's bugs could be of
that kind (a field of a request struct left unset, a buffer used before its fill, a heap block
read before a write) and the board runs on ember RAM that the boot leaves zeroed.

What it does: an input that found new coverage (every input under `--fuzz-replay`) runs a
second time from the same snapshot with the RAM pages that no write reached during the boot
filled with a pattern (`0x1337c0d3` unless `--fuzz-uninit=PATTERN`), each filling itself on its
first touch. The runs are compared: verdict, instruction count, path through the coverage map,
and everything the devices were sent. A difference means the input made the firmware depend on
a byte it never wrote. The boot itself runs on zeros as always.

```bash
./fuzz.sh --fuzz-uninit --fuzz-replay fuzz/smoke/          # replay: every input is checked
./fuzz.sh --fuzz-uninit --fuzz-net 127.0.0.1:7000          # campaign: new-coverage inputs are checked
```

Reading a replay result (exit 26, a second line for the input):

```
replay: in/3-U: uninit differs=insns,path (filled run: ok, insns=110); read before any write: pc=0x80000244 at 0x80100000;
```

`differs=` lists what changed (`verdict`, `detail`, `insns`, `path`, `output`). The reads are the
filled run's first reads of pages nothing had written, as a pc and a physical address: decompile
that pc. It names a page, not a byte, and the first such read is not always the one that
matters (a harmless read of unwritten memory can come first). With no read listed, the page was
partly written by the boot, or the input's own buffer tail was read (the input is written into a
page whose rest is then the pattern). In a campaign the brain saves the input under `uninit/`
with a `.info`, once per distinct finding; it is not a crash, and the input's coverage still
counts.

Know before relying on it:
- A page the boot wrote even once is left alone (page granularity): half-filled structures,
  the stack below the live frame in a used page, `.bss` and `.data` (the loader's zero fill is
  a write). Fresh heap blocks are covered only with the guarded heap hooked
  (`board-skeleton.md`), which then fills each allocation with the pattern in the second run.
- Stale values (memory written earlier in the same input, then freed or popped) are the same in
  both runs. A read that changes nothing compared, or where the pattern and zero take the same
  branch, is invisible; a second run of the same inputs with another pattern helps with the
  last.
- Hardware that zeroes RAM the firmware relies on makes *every* input differ, with the same read
  each time: name that range with `--fuzz-uninit-exclude LO-HI` (physical) and run again.
- A snapshot made with it loads only with it. Costs nothing while off; on, the first store to
  each RAM page takes the bus's slow path once, and a second run costs as much as the
  first, so a campaign does it only for inputs that found new coverage.

## Verdicts and replay

`--fuzz-replay FILE|DIR` runs each input once (a directory in name order) and prints one line
per input:

```
replay: fuzz/smoke/1-ok: ok insns=41 buckets=7
replay: fuzz/smoke/2-bug: crash-oracle pc=0x800002c4 insns=44 buckets=8
replay: crashes/id17: crash-fault pc=0x800002b2 (store-fault cause 0x0 tval 0x40000000) insns=95 buckets=17
replay: crashes/id23: hang (deadlock (waiting with no interrupt source)) insns=5120 buckets=31
replay: crashes/id31: crash-guard pc=0x80000262 (read 1 at 0x80000470: +0 into guard g_buf [0x80000470, 0x80000570)) insns=60 buckets=8
```

Other details: `crash-oracle (reset)`, `crash-oracle status=N` (a non-zero halt),
`crash-oracle interrupt lines=0x.. (at pc .., handler ..)`, `crash-fault pc=.. (fetch fault
loop ...)` (a handler that cannot be fetched), `rejected (the route refused N bytes; nothing
ran)`. A debug trap and a crash pc both print `crash-oracle pc=`. The input name is the path
as given to `--fuzz-replay`, and stderr ends with a count per verdict (`uam-run: fuzz: N
inputs: ...`).

Exit status: the most severe verdict of any input: a crash (the first one's kind: 20 fault,
21 oracle, 25 guard), else 22 hang, else 23 timeout, else 26 depended on memory nothing wrote
(`--fuzz-uninit`, below), else 24 rejected, else 0. Exit 2 means
no verdict at all: a bad option, or a campaign that stopped, most often because the boot
never reached the begin pc (`fuzz: boot did not reach the begin pc 0x..: <why> (N
arrivals)` on stderr: `-n` ran out, a `--guard` the boot touched, a crash during boot), a
route error, or an ember internal error. `--cov-out FILE` writes the last input's map.
`scripts/replay_summary.py` groups a replay's lines by verdict and location, with each
group's smallest input as its reproducer.

**Reading a crash-guard line.** `pc=` is the instruction that made the access (it did not
happen: a STOP guard ends the iteration before it). The parentheses name the access and the
guard: its direction and size, its address, how far into the guard it starts (`+N into`, or
`N before` for an access that starts in front of the range and overlaps it), the guard's name
and its range. A route's redzones are named `mem-before`, `mem-after` and `mem-abs`
(`guardabs=`); a `--guard` by its `name=` or its text before the first `,`. So:
- `mem-after`, `+0 into`: the first byte past the input's extent, the classic off-by-one;
  a large offset is a length the parser trusted. The `mem-after` range moves with each
  input (it starts at L), so compare offsets, not addresses, across inputs.
- `mem-before`: an underflow, a negative index or a header read from in front of the buffer.
- a write into `mem-after` that is always `+0`, on every input: the firmware terminating
  the string in place, not a bug; switch that zone to `:r`.
- a `crash-fault` line can name a guard too: a `,fault` guard whose bus fault crashed.

## Coverage

**By default the harness counts ember's own blocks** (`--cov-blocks` is implied when fuzzing
without `--cov-sites`): N-gram edges (`--cov-ngram N`, default 2, `--cov-rotate`), with a
block ember split counting once, a block resumed mid-way (a timer came due) not at all, and
interrupt handlers counting with a history of their own (`--cov-irq isolate`; `follow`
gives the plain behaviour; the interrupted code, or the task an RTOS resumes, gets its own
history back at the exception return). Blocks count every execution (`--cov-mode every`,
their default), so loop counts show in the hit-count classes. The maps depend only on the code's path: the same at
`--block-insns 32` and 1, and wherever a timer interrupt lands. `--cov-range` (repeatable:
`LO-HI`, `LO+LEN`, `SYM`, `SYM-SYM2`, `SYM+LEN`) counts only blocks starting in a range, to keep
an RTOS's scheduler and other tasks out. The map is 64 KiB up to N=2 and 4x per N above
(`--cov-map-size`); its size costs an iteration nothing, as the coverage op records which
buckets it set. Interpreter and JIT give byte-identical maps and the guest runs exactly as
without coverage, so maps from two runs compare directly. Begin, end and crash pcs may be ELF
symbols (`--fuzz-begin net_rx`).

**A site list** (`--cov-sites`, `references/lists-and-brain.md`; `uam_fuzz_set_sites` in a
program) replaces it when the target is worth a rizin or Ghidra pass: only chosen points
instrumented, site names in errors, ids stable across rebuilds of the same source. A site in a
MIPS branch delay slot or a Thumb IT block cannot be instrumented (the run stops, naming it). Block coverage
works on every ISA; ember's fuzz-check compares the maps of interpreter, JIT and unchained
JIT on every stock board but `x86_64-ref` and `xtensaeb-ref` (several only when their
toolchain is installed). With several processors all
count into one map, each in its own history; edges between processors are not counted.

## Triage

1. `--fuzz-replay` the crash (and hang) directory; `replay_summary.py` it.
2. For each group, replay its smallest input alone, with the interpreter and `--jit`. Both
   must give the same verdict; if not, report an ember bug. Add `--history 64` to see the
   input's last blocks, exceptions and interrupt entries before the verdict (printed for
   every input that is not ok), `--backtrace` for the call stack at the verdict, and traces as needed (`-t mmio,exc`, `--trace-file F`):
   in replay the boot's traces come first, then each input's in its own `trace: input ID`
   section with its own summaries. `--unknown-mmio stop` is refused while fuzzing (its stop
   is not a verdict): use `fault` there, `stop` in the plain run of step 3.
3. Reproduce it as a plain run for debugging: snapshot at the begin pc once
   (`./run.sh --snapshot-at <begin> --save-snapshot build/begin.snap --break <begin>`, plus
   `fuzz.sh`'s boot console script if it has one, so the snapshot is the harness's), then
   feed the input the same way and use ember-dev-build's probes (`--break`, `-d`,
   `--log-pc ADDR,REG...`, `--history N`, `--trace blocks`, gdb; its
   `references/diagnostics.md` lists them all). When the fuzzed UART is also the console, the input
   arrives the same way through it: `--load-snapshot build/begin.snap --console
   file:/dev/null,in=INPUT`; the run then reaches the crash at the snapshot's icount plus the
   replay's `insns=`. For other routes, keep the snapshot and use gdb (`--gdb`) to write the
   input where the route would. For a crash (guard), pass the same guard to the plain run
   (`--guard ADDR...,name=...`; for a redzone, the zone's range from the replay line): it
   stops at the faulting instruction (exit 129), and under `--gdb` the processor reports
   SIGSEGV there, so `bt` and the registers show who overran the buffer. `--watch` on the
   buffer logs every access leading up to it. On a core without exceptions a crash (fault)
   shows in the plain run as `[error] avr: pc=0x194: store to nothing at 0x4000: core
   halted` (the ISA's name first) and the run then ends as a deadlock (exit 125).
4. Classify: a firmware bug (journal it with the input, pc, call chain), or an emulation gap
   (the device answered unlike hardware: a status bit, a length, an interrupt that never
   comes). A gap is fixed in the device under ember-dev-build, then the crash is replayed
   again.

## In a program

When neither replay nor the brain fits (a custom mutator, a differential check), link the
project against the same libuam and drive `uam/fuzz.h` directly:

```c
uam_fuzz_config cfg = { .begin_pc = BEGIN, .crash_pcs = &panic_pc, .n_crash_pcs = 1,
                        .crash_exc = UAM_FUZZ_EXC_DEFAULT };
uam_fuzz *f = uam_fuzz_create(&m, &cfg, err, sizeof(err));
uam_fuzz_set_block_coverage(f);
uam_fuzz_select_route(f, "uart0", err, sizeof(err));
uam_fuzz_boot(f, &r);
for (each input)
    uam_fuzz_run_one(f, data, len, &r);      /* r.verdict, r.detail, uam_fuzz_map(f, &size) */
```

`uam_fuzz_set_block_coverage` gives plain edges between blocks. For the model `uam-run`
uses by default (splits, ranges, interrupt contexts, N-grams) call
`uam_fuzz_set_block_coverage_ex` with `uam_cov_block_opts` (`uam/cov.h`) and set
`cfg.history`; maps from the two are not comparable.

Such a program is analysis code, not part of the board: keep it outside `emulation/` (e.g.
`fuzz/driver.c` with its own `executable()` in the project's `meson.build`, linking
`uam_dep` and the board's sources), and build the machine through the board's
registered `build` exactly as the runner does. A result that names a guard
(`r.guard_valid`) carries the hit in `r.guard` (`uam_guard_hit`: name, processor, pc, the
access's address and the guard's range, so `addr - start` is the offset into it, size,
direction). Guards a program adds with `uam_machine_add_guard` after `uam_fuzz_boot` are
not armed during boot.
