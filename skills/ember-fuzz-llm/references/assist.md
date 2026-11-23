# LLM-assist reference: every mechanism in full

The companion to `SKILL.md`. The ember checkout's
[`docs/fuzzing.md`](../../../docs/fuzzing.md) "LLM-assisted fuzzing" is the authority on the
details, and `build/<board>-run --help` and `fuzz/tools/fuzz_brain.py --help` on the exact
options of the pinned ember (a pin older than this skill may lack some). This file collects
what you need to drive the loop without reading all of that.

## Why a peer, not a plugin

The brain already has a way for another process to add inputs: the **sync directory** used
by parallel workers (`fuzz_launch.py`). Each worker is a directory `SYNC_DIR/<name>/` with a
`queue/`; every worker periodically imports the others' new queue entries. A helper is just
another peer under that directory that happens to only write. Nothing in the brain is
specific to an LLM — you use the same path a second fuzzer would. This is why it is safe:
importing peers is a normal, tested operation, and your inputs go through exactly the same
judging (run once, keep if new coverage) as a worker's.

## The import route, precisely

1. The brain runs with `--sync-dir SYNC_DIR` (set directly, or by `fuzz_launch.py` which
   makes `SYNC_DIR/main`, `SYNC_DIR/w1`, …).
2. You create `SYNC_DIR/<yourname>/queue/` (`mkdir -p`). `<yourname>` matches
   `[A-Za-z0-9_-]{1,32}` and must not be a worker name (`main`, `w1`, …). You need only the
   `queue/` subdirectory; the brain makes nothing else for you.
3. You write input files named `id:NNNNNN,orig:LABEL[,keep]`:
   - **`id:NNNNNN`** — required 6+ digit id with the literal `id:` prefix. A file without it
     is ignored (this is deliberate: it lets you write a temp name, then rename).
   - Ids must **strictly increase** over time. The brain stores the highest id it has
     imported from each peer in `SYNC_DIR/main/.synced/<yourname>` (each worker keeps its own
     `.synced/`), and never re-examines an id at or below it. So reusing or lowering an id
     means the file is silently skipped. Keep a counter; never go back.
   - **`orig:LABEL`** — `[A-Za-z0-9._-]` (no comma). It rides into the queue entry name
     (there cut to 40 characters), the import log line, and the `.info` of any crash the
     input causes. Make it
     say what the input was (`orig:crc_ok_len8`), so a verdict is self-explaining.
   - **`,keep`** — optional, a tag of its own between commas (`,keep` at the end, or
     `,keep,` inside; `,keeper` is not it). With it, the brain adds the input to the queue even if it
     reaches no new coverage (verdict `kept`). Without it, a no-coverage import is dropped
     (`nocov`). Use `,keep` for an input that is the right shape but blocked at a check you
     have not satisfied yet: it becomes a mutation parent the campaign will push on.
4. Write **atomically**: `printf ... > queue/.tmp && mv queue/.tmp 'queue/id:000001,orig:x'`.
   The rename is atomic on a normal filesystem; the temp name has no `id:` prefix so a
   half-written file is never read.
5. The brain imports every `--sync-interval` seconds (default 60). For an interactive
   session, start the campaign with a short interval (e.g. `--sync-interval 5`) so your
   inputs land quickly. It imports in id order, runs each input once, and:
   - new coverage (a new bucket, or a new hit-count class of a bucket already hit — the
     same test as for a seed or another worker's find) → queued, verdict `newcov`;
   - no new coverage + `,keep` → queued, verdict `kept`;
   - no new coverage, no `,keep` → dropped, verdict `nocov`;
   - a crash/hang/rejected → handled like any run (saved under `crashes/`/`hangs/`, or
     counted for `rejected`), verdict `crash`/`hang`/`rejected`.

An entry you contributed shows in the queue as
`id:NNNNNN,orig:<LABEL>,sync:<yourname>,src:<your id>`. If the brain stops part way through
an import pass (Ctrl-C, ember's BYE, a dropped connection), it records how far it got, so a
restart neither runs those inputs again nor logs them twice.
The brain's own log (`brain.log`, or stdout) prints `sync: imported N of M new entries from
<yourname>` each interval.

## The import log (`--import-log FILE`)

Off unless given. Appended, not truncated (so it spans sessions and restarts). One JSON
object per imported input, newline-delimited. Always present: `t` (seconds into the
session; it restarts with each session), `time` (Unix seconds: orders lines across
sessions), `worker` (which brain imported it — `main` or a `wN`), `peer` (you), `id`, `orig`,
`size`, `verdict`. Per verdict:

- `newcov` / `kept`: `new_buckets` (new coverage buckets), `new_classes` (new buckets or
  new hit-count classes; 0 for `kept`), `insns` (instructions the run took), `entry` (the
  queue entry name it became).
- `nocov`: `new_buckets` (0), `new_classes` (0), `insns`.
- `crash` / `hang`: `crash` is a fault, guard or oracle; `hang` is a hang **or** a timeout
  (there is no separate `timeout` verdict: `reason_name` tells them apart).
  `reason_name` (`FAULT`/`GUARD`/`ORACLE`/`HANG`/`TIMEOUT`), `status` (the
  pc for fault/guard/oracle as `0x…`, else a number), `file` (the saved file under
  `crashes/` or `hangs/`; may be absent if the crash was a duplicate the brain did not
  re-save).
- `rejected` / `other`: no extra fields.

In a parallel campaign each worker imports independently, so one of your inputs can appear in
several workers' lines (different `worker`), and a `newcov`/`kept` input becomes a queue
entry in **each** worker's queue — the same as a worker's own find reaching the others. It
runs once per worker; that is the price of every worker mutating from it. Read the whole
file; match your `orig:`/`id`.

Tail it between passes:
```
tail -n 20 SYNC_DIR/imports.jsonl
```
A quick filter for your last label, e.g. with python:
```
python3 - <<'PY'
import json
rows=[json.loads(l) for l in open("SYNC_DIR/imports.jsonl") if l.strip()]
for r in rows[-10:]:
    print(r["id"], r["orig"], r["verdict"], r.get("new_buckets"), r.get("reason_name") or "")
PY
```

## Crash provenance

A crash caused by an imported input writes three extra lines in its `.info` sidecar (next to
the usual `reason=`, `status=`, `minimized=`, `hex=`, …): `src_peer=<yourname>`,
`src_id=<the id>`, `src_orig=<LABEL>`. So `grep -l src_peer=<yourname> crashes/*.info` finds
the crashes your inputs found. The provenance survives minimization. Hand the crash file (and
`.info`) to **ember-fuzz-triage** — a verdict of `crash` is a finding to explain, not a
confirmed firmware bug (it may be an emulation gap).

## Live dictionaries (`--watch-dict FILE`, repeatable)

`--dict` is read once at startup. `--watch-dict FILE` names a file the brain re-reads every
`--watch-dict-interval` seconds (default 5) and whose new tokens it merges into ember's
mutator **while fuzzing**. Format is the plain ember dictionary (same as `--dict`), one token
per line:

- `"text"` — a byte string; escapes `\\ \" \t \n \r \xHH`.
- `hex:7f454c46` — raw bytes.
- `u8:`/`u16:`/`u32:`/`u64:VALUE` — an integer in the target's byte order (the brain knows it
  from HELLO); `u16le:`/`u32be:`/… for an explicit order (network fields are big-endian).
- `# comment` and blank lines are ignored.

Write atomically: copy the file, add your lines to the copy, rename it over the original:
```
cp SYNC_DIR/live.dict SYNC_DIR/.live.dict.tmp
printf 'u32:0xdeadbeef\n"RESET"\n' >> SYNC_DIR/.live.dict.tmp
mv SYNC_DIR/.live.dict.tmp SYNC_DIR/live.dict
```
**Never append to the watched file in place** (`>> live.dict`): the brain may read it part
way through a line, and a cut line can be a valid but wrong token (`u32:0xdeadbeef` read as
`u32:0xdead`) that is merged and never removed. A parse error on a half-written file is
logged and retried, not fatal. Merging is additive and deduplicated — it never removes a token, and the
brain keeps using the ones already merged. This is often your highest-value move: one correct
magic value or command string helps every queue entry, where a whole crafted input helps only
its own lineage. Pull tokens from the firmware: compared immediates, string literals, opcode
or command tables, struct tags.

A bad or missing `--watch-dict` file at startup is not fatal either (unlike `--dict`): it is
logged and read again once it parses.

## The frontier report (`fuzz/tools/fuzz_frontier.py`)

Block coverage buckets `mix(pc) & mask`, so you cannot invert the map to addresses. This tool
reconstructs the uncovered edges a different way:

1. It copies your corpus (leaving out `.info` sidecars) and replays the copy through
   `uam-run -t blocks` (read-only, safe beside a live campaign: entries added meanwhile
   cannot shift which input is which), collecting the block pcs each input ran, attributed
   by the `trace: input N` markers and ember's own `replay:` lines.
2. It reads the firmware's CFG with rizin (via `rzpipe`, as `fuzz/tools/gen_sites.py` does):
   each basic block's start, size, successors (taken/fall-through, or a switch's jump-table
   cases), and the disassembly of its last few instructions. An ember block runs on past a
   point another branch jumps to (a loop head, the join after an `if`), so a rizin block also
   counts as run when a block that ran flows into it with no branch of its own.
3. A **frontier branch** is a conditional block (or a switch) that ran but one of whose
   targets never did. For each it reports: the branch pc (symbolized), the uncovered target
   (symbolized), the edge kind (`taken`, `fall-through`, or `case N` with `case_value` in
   JSON), the last ~3 instructions of the block (usually the compare and its constant — the
   value to aim at — though the tool does not decode it for you), how many corpus inputs
   reach the branch, and the **smallest** one (the input to mutate or hand-edit).

Invocation (the trailing command is your replay command **minus** the firmware positional and
**minus** any `-t`/`--fuzz-replay`; the tool adds `-t blocks --fuzz-replay <corpus>
--trace-file <tmp>` and `--trace-range` from `--range`):
```
python3 fuzz/tools/fuzz_frontier.py <firmware> --corpus SYNC_DIR/main/queue --range <fn> -- \
    build/<board>-run -b <board> --fuzz-begin <sym> --fuzz-crash-pc <sym> --fuzz-input <route>
```
Options:
- `--range SYM | LO-HI | LO+LEN` (repeatable): restrict the CFG and the trace to those pcs.
  A bare `SYM` also matches the compiler's clones (`parse` → `parse.constprop.0`). **Use it**
  — on a whole firmware the report is large and most of it is library/scheduler noise.
- `--module NAME:PATH` + `--base NAME=ADDR`: an extra module's ELF and its load base
  (multi-module images).
- `--all-functions`: include functions rizin found only by analysis (`fcn.*`), off by
  default.
- `--top N` (default 40; 0 = all); `--json` (a list of records, for machine reading).
- `--trace FILE`: parse a block trace you already captured instead of replaying (then no
  trailing command). The corpus must be unchanged since: inputs are matched to the trace in
  ember's replay order (every file not starting with `.`, by name — `.info` files too, if
  the directory has them). Capture one with `uam-run … -t blocks --trace-file FILE --fuzz-replay
  <corpus> <firmware>`.

Needs rizin on `PATH` and `rzpipe` importable; without them it prints why and exits 2. Then
fall back to reading the firmware yourself (Ghidra MCP, `objdump -d`, the ember-dev-recon
project) around the pcs your corpus does reach.

Records are sorted most-useful first: branches some input already reaches (so a mutation can
flip them), smallest input first. Those are where to spend effort — you already have an input
one byte away from new code.

## Reading the campaign state

- `SYNC_DIR/<worker>/status.jsonl` — the live feed, one object per `--status-interval`. Key
  fields: `execs`, `execs_per_sec`, `corpus`, `edges`, `new_edges`, `init_edges`, `cycles`,
  `favored`, `crashes`, `crashes_unique`, `hangs_unique`, `rejected`, `imported`,
  `cmplog_finds`, `stage` (what it is doing now), `config` (the run's settings). Watch
  `edges`: climbing means progress on its own; flat across cycles means stuck.
- `SYNC_DIR/<worker>/queue/` — names encode lineage: `orig:` (seed), `src:…,op:havoc,+cov`
  (mutant, `+cov` = new bucket not just new hit-count class), `op:cmplog`/`op:trim`,
  `sync:<peer>,src:<id>` (imported). The set of entries tells you which input shapes already
  reach code.
- `SYNC_DIR/<worker>/crashes/` and `hangs/` — findings so far, by `(reason, status)`.
- `fuzz/tools/status_tui.py --sync-dir SYNC_DIR` — a human status screen across all workers
  (the user may want it up).

## Interpreting verdicts into the next move

- `newcov` → the shape works and found something. Write near-variants (same structure,
  different field values) to push deeper, and read the frontier again for the new edges it
  opened.
- `kept` → in the queue but gated. Look at the frontier branch nearest it; the compare there
  tells you the byte/word to change. Or add that constant to the live dict.
- `nocov` → it died at an earlier check than you targeted. Trace one input (`uam-run …
  -t blocks --fuzz-replay <that input>`), or read the code: the input is not reaching your
  branch at all. Fix the earlier field first.
- `rejected` → the route refused it before the firmware saw it: a length field, a checksum,
  or a board input fix-up. Satisfy it (or, if the board repairs it, you need not — see
  Board-specific features). `--history`/traces on a replay of the input show where it was
  dropped.
- `crash`/`hang` → triage it (ember-fuzz-triage). Keep fuzzing; one input rarely exhausts a
  bug.

## Multi-worker notes

In a `fuzz_launch.py` campaign you still write to one peer directory
(`SYNC_DIR/llm/queue/`); **every** worker imports it, so a good input reaches the whole
campaign. Compare logging runs in `main` only, so if you want your input to seed the i2s
stage, `main` importing it is what matters. The import log interleaves all workers' imports;
filter by `worker` if you care which one kept yours.
