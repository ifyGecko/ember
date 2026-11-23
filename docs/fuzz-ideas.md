# Design ideas: the fuzz brain's campaign side

A design proposal for ember's fuzz brain (`fuzz/tools/fuzz_brain.py`), its mutator
(`fuzz/tools/mutate.py`) and the protocol between the brain and ember (`--fuzz-net`, the
UAMF protocol in [`fuzz-formats.md`](fuzz-formats.md)). Sections 0, A, B and C are now
implemented (see "Implementation status" below); the list of other ideas at the end is
still a backlog.

Companion reading: [`fuzzing.md`](fuzzing.md) (the harness, routes, verdicts),
[`fuzz-formats.md`](fuzz-formats.md) (the protocol, the coverage buckets, versioning).

## Where we stood (before this work)

Ember's *execution* side is strong: snapshot-based persistent iterations, exact instruction
budgets, deterministic replay, bug oracles (exception classes, crash pcs, interrupts,
guards, redzones) and configurable coverage (block edges, or site lists with N-grams).
The *campaign* side, the part AFL++ and honggfuzz spend most of their effort on, is thin:

- the brain picks the next corpus entry **uniformly at random** for every execution: no
  queue statistics, no favored entries, no energy, no cycles;
- new coverage means "a map byte went from zero to non-zero"; a change in how often an edge
  ran (AFL's hit-count classes) is not noticed;
- there is no trimming of new entries, no calibration, no measure of an input's cost;
- the mutator's operator weights are fixed;
- `mutate.py` has a dictionary (`--dict FILE`, a `name="value"` text format borrowed from
  AFL), but **the brain never passes one in**: it has no `--dict` option and builds its
  `Engine` without a dictionary. Section A replaces it with a format of ember's own;
- one brain drives one ember process; there is no way to run several and share their work;
- every execution returns the **whole coverage map**, which the brain folds in Python byte
  by byte.

This document covers two ideas that fix that: **(A)** a real campaign scheduler in the
brain, with power schedules, and **(B)** parallel workers that share their work, AFL++
style. Both rest on **(0)**, a cheaper coverage reply, which comes first. A short list of
the other ideas from the same review closes the document.

## Implementation status

Built as designed, with these differences:

- **SPARSE** (frame 7) also carries `u32 map_size` before `n`, so its header is 25 bytes;
  OPTIONS is frame 6. Both are specified in [`fuzz-formats.md`](fuzz-formats.md). The INIT
  map stays COVERAGE (it goes out before OPTIONS can arrive).
- **Ember side:** the C scan is `uam_fuzz_map_hits` in the harness (64 bits at a time); the
  next iteration then clears only the listed buckets. The coverage operation recording
  first hits itself is not done (no profile has asked for it yet).
- **Byte order:** ember states it in HELLO (flags bit 15, big-endian) instead of the brain
  deriving it from the ISA name, which is not uniform across ISAs.
- **Calibration** advises a per-input budget (5x the slowest entry) in the brain's log
  rather than setting it: the protocol has no way to change ember's `--fuzz-iter-insns`.
- **Schedules:** `fast`, `explore`, `exploit`, `coe`, `rare`, and the variants `lin`,
  `quad`, `seek`, `mmopt`. In `lin`/`quad` the level counts the current pick
  (`(fuzz_level + 1) / (freq + 1)`), so an entry never chosen is not starved at 0; `seek`
  drops the cost term of the performance score; `mmopt` multiplies the score by 2 within 3
  generations of the deepest entry and 1.5 within 5 (AFL++'s current form). MOpt proper is
  still for later.
- Files: `fuzz/tools/fuzz_queue.py` (the campaign logic), `fuzz_brain.py` (protocol and
  loop), `mutate.py` (dictionary, `DICT_INT_ADJ`, operator records), `fuzz_launch.py`,
  `status_tui.py --sync-dir`. Tests: `tests/fuzz/brain_unit.py`, `brain-check.sh`,
  `parallel-check.sh`, `fake_brain.py`, `test_fuzz`'s hit-list check.
- **C (LLM-assisted fuzzing)** reuses the peer-sync route of B: a helper writes inputs into
  a peer `queue/`, which the brain already imports. Added on top of it, with no protocol
  change: `--import-log` (a per-import JSON-lines verdict feed), the `,keep` name tag (queue
  an import with no new coverage), crash provenance in the `.info` sidecar
  (`src_peer`/`src_id`/`src_orig`), `--watch-dict` (merge dictionary tokens live), and
  `fuzz/tools/fuzz_frontier.py` (replay + rizin CFG → the uncovered branch targets and their
  compare values). Tests: `tests/fuzz/llm-check.sh`. The guide is
  [`fuzzing.md`](fuzzing.md) "LLM-assisted fuzzing" and the `ember-fuzz-llm` skill. All of
  it is diagnostic/opt-in and read-only toward a running campaign.

### Compare logging (done) and compare splitting (tried, dropped)

**Compare logging** is built: the IR op `CMPLOG` (`uam-isa.md` §7.6), the compare pass
`analysis/cmp.c` (EQ/NE/ordered compares, a flags ISA's difference tested for zero,
PowerPC's two-way `cmpw`, and `x ^ c1` / `x + c1` / `x - c1` against a constant folded to
`x` against the constant it must equal, which PowerPC's `xoris` + `cmplwi` needs), the log
in `src/exec/cmp.c`, the harness API (`uam_fuzz_set_cmp`, `uam_fuzz_cmp_arm`,
`uam_fuzz_cmp_log`), the protocol (HELLO bit 14, OPTIONS bit 1, CMP_INPUT, CMPS) and the
brain's input-to-state stage (`fuzz/tools/cmplog.py`). From the seed `hello world!` with no
dictionary it finds the fuzz target's 32-bit magic value in 3000 executions on rv32, mips,
arm and ppc; without it, never. Cost on the cross-ISA benchmark (JIT): +1-3% for every
normal run while it is on (the state detached), about 2x for the one logged run per queue
entry.

**Compare splitting** (compcov: an equality compare counting a coverage bucket for how many
of its significant bytes agree) was built, tested and **removed**:

| | compare logging | splitting |
|---|---|---|
| finds the 32-bit magic value | 3000 runs from `hello world!`, all four boards | not in 100000 runs from a seed with two bytes right |
| progress | the whole value at once | the third byte after 15000-46000 runs (rv32) |
| cost to every run (JIT) | +1-3% | +52-75% (a C call per compare) |

Splitting turns a 2^-32 guess into four 2^-8 guesses, but havoc still has to hit the right
byte of the right entry four times; compare logging reads the answer off the compare. With
the pass's constant folding, compare logging also covers splitting's usual cases (a
constant in parts, an offset constant, either byte order). What splitting would still
help with is a compared value computed from the input in other ways (a parsed number),
and even there each byte needs a lucky mutation. If such a target turns up, a cheaper
splitting is possible: in split-only mode the pass would emit only equality compares of 16
bits or more, and the JIT would test inline (both operands below 256, equal operands)
before calling out.

---

## 0. Prerequisite: sparse coverage replies instead of whole maps

### The bottleneck

Today every input is answered with a COVERAGE frame carrying the full map
(`u64 id, u8 kind, u32 map_size, map`), and the brain folds it:

```python
def fold(bitmap):
    for i, b in enumerate(bitmap):          # every byte of the map, every execution
        if b and not cumulative[i]:
            ...
```

That is O(map size) per execution, in Python, plus the cost of sending the map over TCP:

- the pc scheme uses a 64 KiB map; the site scheme sizes the map from the site count and
  N-gram length, up to 16 MiB (`fuzz-formats.md`, "Map size");
- a typical iteration touches a few hundred to a few thousand buckets, so more than 99% of
  the bytes moved and scanned are zero;
- a Python loop over 64 KiB costs milliseconds, over 16 MiB around a second, which is far
  more than ember spends running a typical input. The brain, not the emulator, sets the
  execution rate.

Every idea below makes this worse: power schedules need per-edge and per-path frequencies
updated on every execution, and parallel workers multiply the number of brains doing it.

### The fix: send the list of buckets that were hit

Ember replies with the **non-zero buckets only**, as a list of `(index, count)` pairs, plus
the facts the scheduler needs that the map does not carry.

**Protocol.** A new frame type, negotiated so that old peers keep working
(`fuzz-formats.md`, "Versioning": unknown frame types are ignored):

| Type | Direction | Payload |
|---|---|---|
| 6 OPTIONS | brain → ember, once, right after HELLO | u32 flags; bit 0 = "send SPARSE instead of COVERAGE" |
| 7 SPARSE | ember → brain | u64 id, u8 kind (1 INIT, 2 ITER), u64 insns (instructions the iteration ran), u32 map_size, u32 n, then n × (u32 index, u8 count) |

- An ember that does not know OPTIONS ignores it and keeps sending COVERAGE; the brain
  handles both. A brain that sends no OPTIONS gets COVERAGE as today.
- `insns` is new information the protocol lacks today (the replay frontend prints it as
  `insns=`); the scheduler needs it as the cost of an input (section A).
- The list could also be sorted by index so that the brain can hash it directly into a
  *path id* (section A, "fast" schedule) without sorting.
- Size: 5 bytes per hit bucket. A 2000-bucket iteration is 10 KB instead of 64 KiB to
  16 MiB; for a dense iteration (more than a fifth of the map hit) ember may fall back to a
  COVERAGE frame, which the brain still understands.

**Ember side.** Building the list must not cost what the brain just saved:
- first version: scan the map in C 64 bits at a time, skipping zero words (memory
  bandwidth speed, no change to the executor), and clear only the listed buckets at the
  next iteration instead of the whole map;
- later, if the scan shows up in profiles: have the coverage state record a bucket's index
  the first time it goes from 0 to 1 in an iteration (one predictable branch in the COV
  operation, in the interpreter and the JIT), so the list exists without scanning.

**Brain side.** The brain keeps its cumulative "virgin" state as a byte array and touches
only the listed indices: O(buckets hit) per execution. The same pass updates the per-edge
hit frequencies and computes the path id, so the schedules cost almost nothing more.

**Hit-count classes.** While changing the fold, adopt AFL's interpretation of counts: a
bucket's count is classified (1, 2, 3, 4-7, 8-15, 16-31, 32-127, 128+), and an input is
interesting if it sets a bucket that was never hit *or* reaches a count class that bucket
never had. That catches loops that run a different number of times (a length check passed,
a parser that went one field further). This changes only the brain's interpretation; the
map and its bucket formula stay as specified, so saved corpora keep their meaning.

---

## A. A campaign scheduler with power schedules

### Concept

Instead of picking a random corpus entry for each execution, the brain keeps a **queue** of
interesting inputs with statistics, walks it in **cycles**, and gives each entry an
**energy**: the number of mutated children it produces this time round. A **power
schedule** is the function that turns an entry's statistics into its energy. Around that
sit the other AFL++ campaign basics: favored entries, trimming, calibration, adaptive
mutator weights and dictionaries.

### Why it helps

Coverage-guided fuzzing spends almost all of its time on a few entries that sit on common
paths (the "happy path" of a parser). Schedules move energy to inputs that exercise rare
paths, are cheap to run and are small, which is where new coverage comes from. AFLFast and
AFL++ report finding the same bugs several times faster than uniform selection on many
targets. Ember has an advantage AFL++ lacks: the cost of an input is its exact,
deterministic instruction count, not a noisy wall-clock time.

### Queue entries and statistics

Each queue entry keeps:

| field | meaning |
|---|---|
| `id`, `parent`, `depth` | lineage: which entry it was mutated from, how many generations deep |
| `size` | input length |
| `insns` | instruction count of its run (from SPARSE), the cost |
| `buckets` | number of buckets it hit (its coverage size) |
| `path` | path id: a hash of its sorted hit list (with count classes) |
| `found_at` | when it was found (execution number, cycle) |
| `times_chosen` (`fuzz_level`) | how often the scheduler picked it |
| `favored` | in the favored set (below) |
| `trimmed`, `calibrated` | done once |
| `new_cov` | it brought new buckets (not only new count classes) |

Global tables, updated on every execution from the hit list:
- `path_freq[path]`: how many generated inputs exercised that exact path (AFLFast's `n_fuzz`);
- `edge_freq[index]`: how many executions hit each bucket (for `rare`);
- averages of `insns`, `size` and `buckets` over the queue (for the performance score).

### The cycle loop

```
loop:
    cull the queue (recompute the favored set if it changed)
    for each entry in the queue, in order:
        skip it with high probability if it is not favored and favored entries are pending
        (AFL: skip 99% of non-favored entries while new favored ones wait, 95%/75% otherwise)
        calibrate and trim it if not done yet
        energy = schedule(entry, stats)
        run `energy` mutations of it (havoc and splice), scoring each child
        entry.times_chosen += 1
    cycle += 1
```

### Favored set (culling)

For every bucket, remember the "top rated" entry that hits it with the smallest
`insns × size`. The favored set is the union of top-rated entries: a small subset that
still covers every bucket seen. Non-favored entries are mostly skipped, which focuses the
campaign without throwing anything away. Recompute it when a new entry changes a top-rated
slot.

### Power schedules

All schedules start from a **performance score** in AFL's spirit, then multiply it by a
schedule-specific factor:

- **performance score:** larger for entries that are cheaper than average (`insns`),
  cover more than average (`buckets`), were found late (a handicap for entries that had no
  time yet) and sit deeper in the lineage (`depth`); bounded to a fixed range;
- **factor:** what distinguishes the schedules; `fuzz_level` is `times_chosen`, `freq` is
  `path_freq[entry.path]`.

The ones to build first:

| schedule | factor (roughly; check AFL++'s `calculate_score` when implementing) | behaviour | use |
|---|---|---|---|
| `fast` (default) | `2^fuzz_level / freq`, capped | energy grows each time an entry is picked, and shrinks when its path is already exercised a lot | AFL++'s default; the best general choice |
| `explore` | 1 (the performance score alone) | even spread of energy, breadth first | early campaigns; diversity in a parallel set |
| `exploit` | the maximum | classic AFL: small, cheap, new entries get the most | a "depth" worker in a parallel set |
| `coe` (cut-off exponential) | like `fast`, but 0 while `freq` is above the mean path frequency | entries on common paths get nothing until the rare ones had their turn | aggressive rarity, next to a `fast` worker |
| `rare` | weighted by how rare the entry's rarest buckets are (`edge_freq` below a threshold) | goes after narrow checks (FairFuzz-like) | parsers with strict checks; protocols |

Ember-specific factors that apply to every schedule (switchable, not schedules of their own):
- **cost:** the performance score uses the exact `insns`, so cheap inputs get more energy
  without timing noise;
- **depth:** extra energy for entries deep in the lineage or reaching many distinct sites.

**Is it worth having several?** Yes, mainly for parallel campaigns: no schedule wins on
every target, and AFL++'s own advice for parallel runs is to give the secondary instances
different `-p` schedules so they explore differently instead of duplicating each other
(`-p` is per instance in AFL++, and the same goes for ours: `--schedule` per worker). Once
the queue, the statistics and the cycle loop exist, each schedule is a few lines: a
function from an entry and the global tables to a factor. The cost is the groundwork, not
the schedules.

**Variants to add later** (each is a small change once the framework exists; AFL++ has them):
- `lin`: factor `fuzz_level / (freq + 1)`, linear growth;
- `quad`: factor `fuzz_level² / (freq + 1)`, quadratic growth;
- `seek`: a variant of `explore` that leaves out some of the performance-score terms;
- `mmopt`: gives the newest queue entries extra energy (from AFL++'s MOpt work).

### Trimming

Once per new entry, before it is fuzzed: remove chunks (from a sixteenth of its length down
to a small minimum, as AFL does) and keep each removal that leaves the **same path id**.
Smaller entries mutate faster and splice better. Ember's determinism means one run per
probe is enough, and `insns` gives the trimmed entry's exact cost. Keep the untrimmed
original in the corpus directory only if asked.

### Calibration

Once per seed and per new entry: run it twice and compare the hit lists. Ember is
deterministic, so they must match. A difference is not normal fuzzing noise as in AFL++; it
is a harness problem (state that leaks between iterations, an uninitialised device, a
snapshot that misses something), and the brain should say so loudly, naming the buckets
that differed, instead of silently scoring "stability". The same pass records `insns` and
can set the per-input budget automatically (about five times the slowest seed, as AFL++
calibrates its timeouts) when `--fuzz-iter-insns` was not given.

### Adaptive mutator weights (MOpt-lite)

`mutate.py` already records which operators produced an input and accepts a weights file.
The brain counts, per operator, how many of its children found new coverage, and
periodically moves weight toward the operators that pay off (with a floor so none dies
out). Each worker can start from different weights. A full MOpt (particle-swarm
optimisation of the weights) can come later if the simple version shows promise.

### Dictionaries (ember's own format)

**Concept.** A dictionary is a list of tokens the mutator inserts into or writes over an
input: magic numbers, command words, header fields, delimiters. Firmware parsers check
exactly such values, and random byte mutation almost never produces a 4-byte magic.

**Why ember's own format.** Ember works with no outside fuzzer, so the format should
contain what ember needs and nothing else. The borrowed `name="value"` lines carry a name
and a level nobody uses, and they cannot express the one thing firmware tokens need most:
**integers in the target's byte order**. A 32-bit magic compared by a big-endian MIPS or
PowerPC parser must be inserted big-endian, the same magic on RISC-V or ARM little-endian.
Ember states the target's byte order in HELLO (a flag bit), so the dictionary should state
the *value* and let the brain lay out the bytes.

**Format** (`*.dict`, UTF-8 text, one token per line; the current parser in `mutate.py` is
replaced, with no compatibility mode, like the fuzz formats themselves):

```
# comment; blank lines ignored
"GET /"                   bytes, with \\ \" \t \n \r \xHH escapes
hex:7f454c46              raw bytes
u8:0x5a                   an integer of that width, in the target's byte order
u16:0x8100
u32:0xdeadbeef
u64:0x0123456789abcdef
u32le:0xdeadbeef          an explicit byte order where the format fixes one
u32be:0xdeadbeef          (network fields: big-endian whatever the CPU)
```

That is all: no names, no levels, no weights. Integer tokens give the mutator one more
thing for free: it can also write the value ±1 (an off-by-one boundary check), which a
byte string cannot express.

**Where tokens come from.**
- written by hand from the protocol or the firmware's analysis (`--dict FILE`, repeatable);
- generated by ember's tools in the same format: strings and comparison immediates from the
  image (an extension of `gen_sites.py`, listed at the end) and, later, operands seen at run
  time. One format serves every source.

**Implementation.** `mutate.py`: a new parser and `Dictionary` that keeps integer tokens as
`(value, width, order)` and resolves the target order once the brain tells it the
endianness; the dictionary operators gain "integer ±1". `fuzz_brain.py`: `--dict FILE`
(repeatable), resolved against HELLO's ISA. Per worker, so different workers can use
different dictionaries. `mutate.py --selftest` covers parsing, escapes, errors with line
numbers and byte order per ISA.

### Implementation sketch

- `fuzz/tools/fuzz_brain.py`: split into a queue module (entries, statistics, culling,
  persistence), a schedules module (one function per schedule, selected by `--schedule`),
  and the campaign loop. The protocol code learns OPTIONS and SPARSE.
- Persistence: queue entries keep their statistics in a sidecar (or in the file name, see
  section B), so a resumed campaign keeps its cycle, favored set and frequencies instead of
  starting cold.
- Status: the status feed and `status_tui.py` gain cycle, favored and pending counts, the
  schedule, executions per second, and calibration failures.
- Tests: unit tests of each schedule on synthetic statistics; `brain-check.sh` runs a short
  campaign per schedule on the planted-bug firmware and requires each planted bug to be
  found; a calibration test with a deliberately leaky harness must be reported.

---

## B. Parallel workers that share their work (AFL++ style)

### Concept

Run several independent fuzzing instances, each a **brain + ember pair** on its own core,
with its own output directory, and have them **periodically import each other's
interesting inputs**. One instance is the main (`-M` in AFL++), the others secondaries
(`-S`). Every worker can use a different schedule, mutator weights, dictionary and random
seed, so the set explores more of the program than N copies of one configuration would.

### Why it helps

- Throughput scales with cores: each ember is single-threaded and each pair is independent.
- Diversity: different schedules and settings find different things, and sharing lets every
  worker build on all of them.
- Fault isolation: a worker that dies (an ember internal error, a lost connection) does not
  stop the campaign; the launcher restarts it and it resumes from its queue.

### Why separate instances and not one brain serving many embers

A single "hub" brain with N ember connections would make sharing trivial, but one Python
process would cap the execution rate for all of them, and one failure would stop
everything. Independent instances that sync through the file system scale with cores and
isolate failures; this is the design AFL++ settled on.

### Layout and naming

```
out/                                (the sync directory, shared)
  main/      queue/  crashes/  hangs/  .synced/  status.jsonl
  w1/        queue/  crashes/  hangs/  .synced/  status.jsonl
  w2/        ...
```

Queue file names record lineage and why the entry was kept (a scheme borrowed from AFL++
because it works, not for compatibility: only ember's brains read these directories):

```
id:000123,src:000045,op:havoc,+cov          found by this worker
id:000124,sync:w2,src:000310                imported from worker w2 (its id 000310)
```

### Sync

On a configurable interval (and once at start), each worker:

1. lists the other workers' `queue/` directories;
2. for each peer, imports the files with an id above the one recorded in
   `.synced/<peer>` (so nothing is re-read);
3. runs each imported input once, without mutating it, and keeps it in its own queue only if
   it brings new coverage *for this worker*; that run is needed for the local hit list and
   statistics, but no calibration is needed, because ember's determinism guarantees the
   input behaves exactly as it did for its finder;
4. records the highest id imported per peer.

Crashes and hangs stay in each worker's directories; the status view aggregates them and
deduplicates across workers by the same key the brain uses.

### Per-worker settings (the important part)

- **A distinct RNG seed per worker, always.** Ember is deterministic and so is the
  mutator: two workers with the same seed, schedule and corpus would produce the same
  inputs and waste their cores. The launcher derives each worker's seed from a campaign
  seed and the worker's name.
- A schedule per worker (`--schedule`); a reasonable spread for 8 workers is
  `fast, explore, exploit, coe, rare, fast, explore, rare`, with the main on `explore` or
  `fast`.
- Optionally different dictionaries and starting mutator weights.
- The main's duties beyond fuzzing: the aggregated status and, later, periodic culling of
  the shared corpus. Otherwise main and secondaries are symmetric.

### Launcher and status

- `fuzz/tools/fuzz_launch.py -n 8 --schedules fast,explore,exploit,coe,rare --dict ...
  --out out -- <uam-run arguments>`: starts N brains on distinct ports and N embers dialling
  them, assigns names, seeds, schedules and dictionaries, restarts a worker whose ember or
  brain exits unexpectedly, and stops everything cleanly on Ctrl-C.
- `status_tui.py` gains a multi-worker view (AFL++'s `afl-whatsup`): per worker and in total,
  executions per second, cycles, queue size, favored entries pending, unique crashes and
  hangs, last find, calibration failures.

### Implementation sketch

- `fuzz_brain.py`: `--name`, `--sync-dir`, `--sync-interval`, `--main`/`--secondary`, the
  file naming, the `.synced/` bookkeeping, import-and-score.
- `fuzz/tools/fuzz_launch.py` (new) and the multi-worker view in `status_tui.py`.
- Nothing changes in ember itself for this part (beyond section 0): each worker is an
  ordinary `--fuzz-net` session.
- Tests: two workers on the planted-bug firmware with different schedules and seeds; each
  must import at least one of the other's entries (visible as `sync:` names) and the set
  must find every planted bug; killing one worker must leave the other running and the
  launcher must restart it.

---

## Suggested order

1. Section 0: OPTIONS + SPARSE with `insns`, the C-side sparse scan, the brain's
   O(hits) fold with hit-count classes. Measure executions per second before and after.
2. Ember's dictionary format and `--dict` in the brain (small, independent; can go first).
3. Queue entries, statistics, cycles, favored set, calibration; the `explore` and `fast`
   schedules.
4. Trimming; `exploit`, `coe`, `rare`; adaptive operator weights.
5. Parallel workers: naming, sync, launcher, multi-worker status.
6. `lin`, `quad`, `seek`, `mmopt` (done); later: MOpt proper.

Throughout: keep ember's coverage map, its bucket formula and the `(reason, status)` crash
key unchanged, so saved corpora and crash sets keep their value.

---

## Other ideas from the review (not specified here)

Recorded so they are not lost; each needs its own design before it is built. (Frontends
for AFL++ or honggfuzz were considered and dropped: ember's brain is the fuzzer, and the
work above brings their campaign features to it.)

- **Batching or an in-process loop:** several inputs per protocol frame, or a C mutator and
  corpus loop inside ember for very short iterations.
- **Peripheral input from the fuzz input** (P2IM / Fuzzware style): unmodelled register reads
  take bytes from the input, with models learned from the register summary (a bit-tested
  register consumes one bit, a poll reads "ready").
- **Interrupt timing as part of the input:** input bytes choose when interrupts are raised,
  to find races between handlers and the main loop; replayable thanks to determinism.
- **Crash and hang deduplication from `--history`:** crashes keyed by (reason, pc, a hash of
  the last K blocks); hangs signed by their loop pcs; optionally AFL++'s "new edges" rule.
- **Automatic triage reports:** the replay line, history, registers, guard hit, exception
  summary and console tail in each crash's sidecar.
- **Comparison feedback:** comparison operands for the brain (input-to-state, as AFL++'s
  CMPLOG: *done*, above), split multi-byte compares (compcov / laf-intel: *tried and
  dropped*, above), value-profile coverage as a second map.
- **Dictionaries from the firmware:** strings and comparison immediates from the image, and
  operands logged at string-compare functions.
- **Input provenance:** taint from the input buffer, so a crash names the input bytes that
  reached it.
- **More oracles:** allocator-aware guards that follow live heap objects; uninitialised
  reads; a shadow stack (return-address integrity); a null-page guard by default; writes to
  code or read-only data; the stack pointer leaving its range; double fetches from input
  buffers or DMA.
- **Instruction-count feedback** (PerfFuzz style): keep inputs that raise the maximum count
  per edge, for algorithmic-complexity bugs.
- **Context-sensitive coverage** (a call-context hash), and counting interrupt-handler edges
  apart so timer interrupts do not add noise.
- **Multi-part inputs and stateful sessions:** one input feeding several routes (UART, MMIO,
  buffer, network frames), and sessions over several snapshots, for protocol fuzzing.
- **Auto-discovery of input routes and boundaries** from the firmware.
- **Tooling:** corpus minimisation, test-case minimisation and a map viewer built on
  `--fuzz-replay --cov-out`. (A frontier viewer — the uncovered branch targets, from a
  block-trace replay and the rizin CFG — is built: `fuzz/tools/fuzz_frontier.py`, section C.)
