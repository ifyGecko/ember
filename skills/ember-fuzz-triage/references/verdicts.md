# Reading a verdict, and what usually causes it

A replay prints one line per input:

```
replay: PATH: VERDICT [pc=0x...] [(details)] insns=N buckets=N
```

`pc` is where the input stopped, `insns` the instructions it ran from the begin pc,
`buckets` how much coverage it touched (two inputs with different `buckets` took different
paths). This page explains each verdict, what to do first, and the usual root causes.

## crash-fault: the processor took an exception

```
replay: a: crash-fault pc=0x800003a6 (store-fault cause 0x0 tval 0x40000000) insns=98 buckets=16
```

`pc` is the instruction that faulted. In the parentheses: the kind of exception, the
architecture's own cause code, and `tval`, the value that went with it (for a memory
fault, **the address that was accessed**).

| Kind | The instruction tried to | Look at |
|---|---|---|
| `load-fault` | read an address where nothing is | `tval`: the address. Which register held it? |
| `store-fault` | write such an address | the same |
| `fetch-fault` | execute from such an address | `pc` is the bad address itself: how did execution get there? (below) |
| `load-misaligned`, `store-misaligned` | access a multi-byte value at an odd address | a pointer computed from input bytes |
| `load-xlate`, `store-xlate`, `fetch-xlate` | use a virtual address with no valid mapping (page fault, TLB miss, protection) | firmware with an MMU; only crashes if the campaign asked for `xlate` |
| `trap` | execute an illegal instruction, a breakpoint, a system call, a division error (the architecture's cause says which) | `cause`; on x86 its low byte is the vector (6 = #UD, 13 = #GP, 0 = #DE) |
| `sysreg` | access a system register it may not | privileged code run in the wrong mode, or garbage executed as code |
| `fetch fault loop` | (in the details) even the exception handler cannot be fetched | the vector table or its register was overwritten |

**Read the address first.** It tells you most of the story:

| The faulting address (`tval`) is | Usually means |
|---|---|
| 0, or a small number (below 0x1000) | a **null pointer**, plus a field offset: `tval` 0x18 is `ptr->field` with `ptr == NULL`, the field at offset 0x18 |
| made of bytes from the input (`0x41414141`, or 4 bytes you can find with `xxd`) | the firmware used input bytes as a pointer, or an overflow replaced a pointer with input data |
| just past the end of RAM, a stack or a buffer | a loop ran off the end: a length or index out of range |
| close to a valid address, off by a large round number | an index multiplied by an element size, with the index unchecked or negative |
| a valid-looking address in a device's range | the firmware touched a peripheral the board does not have: probably an emulation gap |
| all ones (`0xffffffff`), or near it | an error code (`-1`) used as a pointer or index |
| a round constant that the code itself loads (`0x40000000`: the disassembly shows it as an immediate), in no range of `--map` | the firmware addresses a peripheral or memory this board does not have. The input only chose the path. Does the real device have something there? Yes: an emulation gap. No: a firmware bug |

Use `RUN --map -n 1` to see what is at the address (`flags.md`). Replaying with
`--unknown-mmio zero` is the quick test for "an access to nothing": if the verdict
becomes `ok`, that is what it was.

**A fetch fault, or a crash at a pc that is not code**: execution jumped somewhere wild.
The instruction that jumped is the last entry of `--history`. It is nearly always one of:

- a **return** to a smashed return address (a stack buffer overflow). Confirm with
  `--guard-returns`: the replay then stops at the bad return with `crash-guard ... (return:
  returns to 0x..., which no call returns to)`, and the function it names is the one whose
  stack frame was overwritten;
- a **call through a function pointer** that was overwritten or never set (a null or
  garbage entry in a handler table indexed by an input byte);
- a jump table indexed by an unchecked value.

**First steps**: step 4 of SKILL.md (history, backtrace, registers), then find the
register that held `tval` and trace it back (step 8).

## crash-guard: an access to guarded memory, stopped before it happened

```
replay: b: crash-guard pc=0x80000262 (read 1 at 0x80000470: +0 into guard g_buf [0x80000470, 0x80000570)) insns=60 buckets=8
```

`pc` is the instruction that tried. In the parentheses: read or write, how many bytes, the
address, where that is relative to the guarded range, the guard's name and its range.
Because the access did not happen, memory is still intact: this is the best kind of crash
to triage, and it points at the faulty instruction directly.

| The details say | Meaning |
|---|---|
| `+0 into guard mem-after` | the first byte past the input: an off-by-one read or write |
| `+N into guard mem-after`, N large | the firmware trusted a length field from the input |
| `N before` / guard `mem-before` | it went in front of the buffer: a negative index or an underflow |
| a write that is always `+0 into mem-after`, on every input | the firmware terminating a string in place: usually not a bug |
| `(heap-redzone: ... N past the end of the M-byte block ...)` | a heap buffer overflow by N bytes |
| `(heap-freed: ...)` | use after free |
| `(heap-double-free ...)`, `(heap-bad-free ...)` | freeing twice, or freeing something that was never allocated |
| `(heap-wild: ...)` | an access into the heap area that is in no block at all |
| `(return: returns to 0x..., which no call returns to)` | a smashed return address |

Heap lines also name where the block was allocated and freed. `--backtrace` adds the call
chain of the access.

**In a plain run the guard has to be given again**, or nothing stops: a `--guard` of the
campaign as it is; a `mem:` route's redzone as `--guard LO+LEN[:r|w]` with the range the
line shows in brackets; the heap and return guards through the campaign's own options.
The run then ends before the access with exit status 129, and under gdb with a SIGSEGV at
that instruction, where `bt` and the registers show who overran what.

**First steps**: the pc is the bug's location or one call below it. Work out which length
or index the instruction used (the registers in `-d`), and where it came from in the input.

## crash-oracle: the firmware itself said "fatal"

```
replay: c: crash-oracle pc=0x80000272 insns=99 buckets=17
```

| Form | Meaning |
|---|---|
| `crash-oracle pc=0x...` | execution reached an address the campaign marked fatal with `--fuzz-crash-pc` (a panic or assert function, an exception reporter), or a debug-trap instruction |
| `crash-oracle (reset)` | the machine was reset during the input: by the firmware, or by a watchdog |
| `crash-oracle status=N` | the firmware halted with a non-zero status |
| `crash-oracle interrupt lines=0x.. (at pc .., handler ..)` | an interrupt the campaign watches for was raised |

Here `pc` is the **panic function**, not the cause. The cause is whoever called it:

- `--backtrace` shows the chain of callers. The frame below the panic function is the
  check that failed.
- If the panic is reached from the exception handler, the real event is an exception that
  the firmware caught: run with `-t exc` and triage the **first** `exc:` line of the input
  as a crash-fault. Its `pc` is the true crash site.
- Firmware often prints a message before panicking. Run the replay with the console
  visible (`--console stdio`, or look in the file the campaign sends it to) and
  `-t console` to see what it said and when.

A **reset** needs one more question: was it the watchdog? If `--fuzz-iter-insns` is longer
than the watchdog period, a slow input gets reset without any bug. Compare the input's
`insns=` with the instruction count at which a clean input finishes, and look for writes
to the watchdog's registers with `-t mmio`.

## hang: waiting for something that never comes

```
replay: d: hang (deadlock (waiting with no interrupt source)) insns=5120 buckets=31
```

The firmware went to sleep (a wait-for-interrupt instruction) and nothing can wake it, or
a timer keeps firing without its interrupt arriving (`livelock`).

| Usually | How to tell |
|---|---|
| the input made the firmware wait for a reply, an acknowledge or more data that the fuzzer never sends | `--history` and `--backtrace` end in a receive or wait function. Often not a bug: the protocol needs a second message |
| the input made it disable interrupts and then wait | `-t irq` and the status register in `-d`: the interrupt is pending but masked |
| the device it waits on is not modelled | `-t mmio-summary`: a status register that always reads the same value. An emulation gap |
| a real denial of service: one message stops the device for good | the wait has no timeout in the firmware; confirm in the code |

## timeout: the instruction budget ran out

```
replay: e: timeout insns=100000 buckets=16
```

The input was still running after `--fuzz-iter-insns` instructions. Either it loops for
ever, or it is only slow.

1. Make it cheap to study: replay with a small budget and the history on.
   ```bash
   FUZZ --fuzz-iter-insns 20000 --history 16 --fuzz-replay INPUT < /dev/null
   ```
   ```
   history:   blk 0x800002f6 icount=20407
   history:   blk 0x800002f6 icount=20411
   history:   blk 0x800002f6 icount=20415
   ```
   The same block again and again is a tight loop; a short cycle of a few blocks is a
   loop with a body; no repetition means it is doing real work (slow, not stuck).
2. Find the loop's exit condition: disassemble the block (`objdump`, Ghidra, or gdb's
   `x/12i 0x800002f6`). What does the branch at its end compare?
3. Watch that value. In a plain run stopped with `-n`, read the registers (`-d`), or log
   them each time round: `--log-pc 0x800002f6,REG1,REG2`. A counter that never changes, a
   length that is huge, or a pointer that does not advance is the answer.
4. Decide: an **infinite loop** (the counter cannot reach the limit: for example a length
   field of 0 that the loop subtracts from the remaining size) is a bug, a denial of
   service. A loop whose count comes straight from the input (4 billion iterations) is
   usually one too. A loop that waits on a device status bit that never changes is an
   emulation gap (`-t mmio-summary` shows the register).

If raising the budget tenfold turns the timeout into `ok`, it was only slow: raise the
campaign's `--fuzz-iter-insns`.

## rejected, and exit status 2

`rejected (the route refused N bytes; nothing ran)`: the input is longer than the route
can deliver. Nothing was tested. Not a finding.

Exit status 2 with no `replay:` line means the replay itself did not run. The message on
stderr says why; the common one is `fuzz: boot did not reach the begin pc ...`, which
means the boot needs something the campaign gave it (a console script, an image, options).

## Depends on memory nothing wrote (`--fuzz-uninit`, exit status 26)

```
replay: u: ok insns=110 buckets=22
replay: u: uninit differs=insns,path (filled run: ok, insns=116); read before any write: pc=0x80000244 at 0x80100000;
```

The input behaved differently when RAM the firmware never wrote was filled with a
pattern: it reads uninitialised memory. The second line names what differed and the read
(pc and address). The fuzz brain saves such inputs under `uninit/`, each with an `.info`. On
real hardware that memory holds whatever was there before, so the bug shows up as
behaviour that "depends on what ran earlier". Find the variable at that address and why
it is used before being set.

## Emulation gap or firmware bug: the checklist

Go through these for the value that caused the crash (SKILL.md step 8):

1. **Where did the bad value come from?**
   - From the input, through code: a firmware bug, if a check is missing.
   - From a device register: go to 2.
   - From RAM nobody wrote: go to 4.
2. **Is that device modelled?** `-t mmio --trace-mmio-range` over the device, or
   `-t mmio-summary`. A register that always returns 0 (or always the same value) from a
   stub is not hardware behaviour. Test the theory in one run:
   `--mmio-rule ADDR=fixed:VALUE` with the value hardware would give. If the crash goes
   away, it is an emulation gap.
3. **Would the real device ever deliver this input?** A UART that receives 4096 bytes in
   one burst, a frame longer than the medium allows, a message before the handshake the
   protocol requires. If not, it is a harness artefact: restrict the input in the
   campaign.
4. **Would real boot code have initialised it?** If the campaign starts from a snapshot
   taken before some initialisation the real device always performs, the "bug" cannot
   happen on hardware.
5. **Does it reproduce on the interpreter and the JIT alike?** If not, it is ember's.

When in doubt, say so in the report and list what would settle it. A report that says
"probably an emulation gap: the PLL status register reads 0, see the trace" is useful; a
confident wrong classification is not.

## How serious is it?

Write what the input controls, not an adjective:

- **What does the attacker control?** The address written to, the value written, the
  length, the jump target, or nothing (a fixed null dereference).
- **A write** to an attacker-chosen address, or a controlled jump target, can usually be
  turned into running the attacker's code. **A read** out of bounds leaks memory. A fixed
  null dereference, a hang or an infinite loop stops the device (a denial of service).
- **Who can send the input?** Anyone on the network, someone on the local bus, only an
  authenticated user, only someone with physical access.
- **What happens afterwards on the real device?** A watchdog reboot, a permanent hang, a
  silent corruption.
