# TODO

Capabilities ember's fuzz layer does not have yet. Each entry says what is missing, why it
matters and where it would go. Nothing here is started.

## Coverage you can read

Block-coverage buckets are hashes of a block's pc folded with its history
(`docs/fuzz-formats.md`, "Coverage buckets"), so a map cannot be turned back into "these
blocks ran". With site lists the site ids gave that; with block coverage as the default
nothing does.

- A covered-pc log: the distinct block pcs a campaign reached (per worker, merged across
  workers), written by ember next to the map. The coverage pass already knows every block
  it instruments; a second, plain bitmap or set keyed on the pc is enough.
- An export a disassembler can highlight (a Ghidra script, a plain `pc size` text file), and
  a diff of two logs, to see where a campaign is stuck.
- Per-function coverage figures from the ELF symbols (`uam_elf_symbol` reads the table).

## Multi-part inputs

One input is one buffer delivered once at the begin pc. A network stack needs a sequence:
a handshake, then a payload, with the firmware's replies in between.

- An input made of parts, each delivered when the firmware asks for the next one (the
  route's device runs dry, or a pc is reached), with the part boundaries kept by the
  mutator (insert, drop, duplicate, reorder and splice whole parts).
- A way for a route to answer from the firmware's own output (sequence numbers, cookies).
- Snapshots at more than one depth, so that a campaign can fuzz the state after part k
  without replaying parts 1..k each time.

## Memory oracles

The return guard (`--guard-returns`) and the guarded heap (`uam/heap.h`) exist
(`docs/fuzzing.md`, "Memory oracles"). What they leave open:

- The return guard's rule is "a return to an address no call returns to". A stricter one,
  "a return from the innermost call that does not go where that call returns to", would also
  catch a return address overwritten with another valid one, and would not need
  `--return-ok` for schedulers that enter tasks by returning. It needs the stack pointer at
  each call and return (which register that is per ISA, an exact register file at the FRAME
  op in the JITs, and how far a return moves it on the ISAs that keep return addresses in
  memory).
- Frame stacks per task: today one history per processor, so a task switch mixes them.
- The guarded heap's checks leave the JIT's inline memory path for the arena's pages; a
  shadow the JIT tests inline would keep heap-heavy targets fast.
- Reads of memory never written since the snapshot (uninitialised reads), per range, and of
  heap blocks never written since their allocation.
- A guard crash's kind (heap-redzone, return, ...) in the fuzz protocol's CRASH frame, so
  the brain can name and group crashes by it.

## Crash triage

- Grouping by call history (the frame stack `--backtrace` prints is there to hash); today a
  crash is known by its reason, its pc and whether it reached code no earlier crash reached.
- A standalone minimiser for a saved input (the brain minimises only what it finds itself),
  and a corpus minimiser: the smallest set of queue entries with the same coverage.
- A report per crash group: the reproducer, the history (`--history`), the faulting access.

## Fuzzing without the brain

The campaign engine (`uam/fuzz_campaign.h`) mutates, runs and judges inputs in ember, but a
campaign's queue, schedules and files are the Python brain's.

- A `uam-run` mode that runs a whole campaign in C: a queue directory, a schedule, saved
  crashes, a status file in the brain's format.
- Several machines in one process (one per thread) sharing one begin snapshot and one
  record of seen coverage, in place of N processes that re-run each other's finds.

## Input-to-state inside ember

The stage runs in the campaign engine (`uam/fuzz_i2s.h`, `uam_fuzz_campaign_i2s_begin`).
What it leaves open:

- Constants in the code's compares could seed the dictionary when blocks are translated,
  before any input is run.
- Compares made a byte at a time in a loop (`strcmp`, `memcmp`) and on values computed from
  the input give no candidates; routines recognised at their call (the board hooks them, as
  for the guarded heap) could log both buffers.
- Colorization: telling which input bytes a compare's operand came from, instead of looking
  for the operand's bytes anywhere in the input.

## Site lists: sites on continuation instructions

A site on an instruction ember runs as part of the one before it (a branch delay slot, an
instruction inside a Thumb IT block) stops the run with an error when that code is first
translated, which can be well into a run (`docs/fuzzing.md`). Either of two changes would
do:

- Count them: a branch and its delay slot run as one unit, so a COV right after the delay
  slot's instruction marker would still count once per execution; the verifier's rule that
  COV follows only non-continuation markers would be relaxed for it.
- Reject them earlier: detect such sites when the list is loaded (a small ISA hook, "is
  this pc a continuation?").

## Mutator

- Operators that know a length or checksum field (found from compare logs, or declared).
- Per-entry operator statistics (MOpt proper) instead of one set of weights per session.
- Crash minimization in ember (the brain minimizes with one INPUT per probe; trimming is a
  stage of the campaign engine already).

## Throughput

`meson test -C build --suite fuzz-throughput` checks what an execution costs against recorded figures
(`tests/fuzz/throughput.py`). What a profile of the RV32 fuzz target still shows between two
inputs, after restores stopped dropping translations:

- Dirty tracking starts over at every restore, so the first store to each page an input
  writes takes the slow path again (and the JIT's data cache is refilled for it). Keeping
  the pages an input wrote marked across restores, and comparing them with the snapshot
  each time, would save that; it needs a rule for letting the set shrink again.
- A device's `restore` reads its state field by field (an interrupt controller's arrays one
  word at a time); a machine with many devices pays for all of them per input.
- The figures are recorded for x86-64 only.
