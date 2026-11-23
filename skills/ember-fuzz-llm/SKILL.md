---
name: ember-fuzz-llm
description: "Guide a running ember (UAM) fuzz campaign as an LLM: read its state, craft inputs the blind mutator has not reached, feed them into the worker queue beside it, add dictionary tokens live, and see which branches stay uncovered. Use when a fuzz campaign (ember-fuzz, the fuzz brain over --fuzz-net, fuzz_launch.py) runs or is about to and the user wants LLM-assisted or human-in-the-loop fuzzing, smarter seeding, or reasoned inputs for a parser/protocol/magic/checksum the mutator struggles with. The campaign keeps fuzzing; this only adds inputs. Covers the peer import route (id:NNNNNN,orig: inputs, the ,keep tag), the import log (per-input verdicts), crash provenance, live dictionaries (--watch-dict), and the uncovered-branch report (fuzz/tools/fuzz_frontier.py: replay + CFG + compare values). Needs a board (ember-dev-build) and fuzzable target (ember-fuzz). Triggers: 'LLM-assisted fuzzing', 'help the fuzzer', 'guide the fuzz queue', 'inject inputs into the fuzzer', 'what is still uncovered', 'fuzz frontier'."
---

# ember-fuzz-llm: guiding a live ember fuzz campaign

A fuzz campaign is blind, cheap and fast: millions of small random steps. You are the
opposite — a few expensive, reasoned steps. This skill lets the two run **together**. The
campaign (ember-fuzz / the fuzz brain) keeps fuzzing exactly as it would. On your own clock
you read its state, work out an input the mutator has not reached (a parser's `"BOOT"` +
length + CRC, a 32‑bit magic, a command the firmware checks for), and feed it in. Everything
here is **additive and read‑only toward the running campaign**: you never stop or slow it,
and a campaign with no helper behaves identically.

**This skill assumes the standard ember interface, which is fully documented** (here, in the
ember checkout's [`docs/fuzzing.md`](../../docs/fuzzing.md) "LLM-assisted fuzzing", and in
`build/<board>-run --help`). But **a specific board or its project patches may add its own
features** — extra input routes, named regions, crash oracles, `--on` rules, traces, link
ports, snapshot hooks, debug commands. Read **Board-specific features** below before you
decide the stock tools are all you have.

Prerequisites, in order: a board that boots the firmware (**ember-dev-build**) and a target
that fuzzes cleanly (**ember-fuzz**: a begin pc, an input route, oracles, a deterministic
snapshot). This skill does not set those up; it attaches to a campaign that already runs.
Read [`references/assist.md`](references/assist.md) for the full detail of every mechanism;
this page is the workflow.

## The interface, in one picture

The brain (or each `fuzz_launch.py` worker) already imports **peers** through a shared
directory. You are a peer that only writes.

```
SYNC_DIR/               # the brain's --sync-dir
  main/                 # a worker: its queue/, crashes/, hangs/, status.jsonl
    status.jsonl        #   <- you READ: campaign totals, live
    crashes/*.info      #   <- you READ: crashes (src_peer names yours)
    queue/              #   <- you READ: the live corpus (names record lineage)
  w1/ ...               # more workers (parallel campaign)
  llm/                  # YOU (any name that is not main/wN): just a queue/
    queue/
      id:000000,orig:boot_frame        # <- you WRITE inputs here
      id:000001,orig:crc_guess,keep
imports.jsonl           # <- you READ: what each of your inputs did (--import-log)
live.dict               # <- you WRITE tokens here (--watch-dict), merged live
```

## Start the campaign so a helper can attach

If you are starting the campaign, give the brain the three assist switches (all optional,
all off by default). If it is already running **without** them, ask the user to restart it
with them — nothing here works unless `--sync-dir` is set, and the feedback needs
`--import-log`.

Single brain:
```
python3 fuzz/tools/fuzz_brain.py --listen 127.0.0.1:7000 --seeds seeds/ \
    --name main --sync-dir camp --import-log camp/imports.jsonl \
    --watch-dict camp/live.dict --status-file camp/main/status.jsonl &
build/<board>-run --fuzz-begin <sym> --fuzz-crash-pc <sym> --fuzz-input <route> \
    --fuzz-iter-insns <N> --fuzz-net 127.0.0.1:7000 <firmware>
```
Parallel campaign (the launcher sets `--sync-dir`/`--status-file` per worker; pass the rest
through `--brain-arg`, written with `=` because the value itself starts with `--`):
```
python3 fuzz/tools/fuzz_launch.py -n 4 --out camp --seeds seeds/ \
    --brain-arg=--import-log=camp/imports.jsonl \
    --brain-arg=--watch-dict=camp/live.dict -- \
    build/<board>-run --fuzz-begin <sym> --fuzz-crash-pc <sym> --fuzz-input <route> \
    --fuzz-iter-insns <N> <firmware>
```
Your peer directory is `camp/llm/queue/` — create it (`mkdir -p camp/llm/queue`). Use any
name except `main`/`wN`. Confirm the brain is past boot: its `brain.log` says `baseline:`.

## The loop

Repeat at whatever pace you like. Each pass is cheap for you and free for the campaign.

### 1. Read the state

- **`SYNC_DIR/main/status.jsonl`** — one JSON object per status tick (newest last). Fields
  that matter: `execs_per_sec`, `corpus`, `edges` (buckets hit), `new_edges`, `cycles`,
  `crashes_unique`, `hangs_unique`, `imported`, `cmplog_finds`. Rising `edges` means the
  campaign is still finding things on its own; flat `edges` over several cycles means it is
  stuck — your moment. In a parallel campaign read each `SYNC_DIR/<worker>/status.jsonl`.
- **`SYNC_DIR/main/queue/`** — the live corpus. Names record lineage: `orig:` (a seed),
  `src:NNNNNN,op:havoc,+cov` (a mutant that reached a new bucket),
  `orig:boot_frame,sync:llm,src:000003` (one of yours that was kept). In a parallel
  campaign every worker imports your input, so it shows in each worker's queue and log. Reading the queue tells you what shapes already reach code.
- **`SYNC_DIR/main/crashes/*.info`** — crashes found so far, by `(reason, status)`; a crash
  from your input carries `src_peer=`, `src_id=`, `src_orig=`.

### 2. Find what is uncovered — `fuzz_frontier.py`

Block coverage hashes the pc, so the map cannot tell you *which* branch has only gone one
way. This tool does. It replays the queue read‑only, reads the firmware CFG with rizin, and
lists every **frontier branch** — a conditional whose block ran but a target did not — with
the **last instructions of its block** (usually the compare and the constant to aim at;
switch cases come with their case value) and the **smallest queue input
that reaches it** (the input to mutate or edit).

```
python3 fuzz/tools/fuzz_frontier.py <firmware> --corpus camp/main/queue --range <fn> -- \
    build/<board>-run --fuzz-begin <sym> --fuzz-crash-pc <sym> --fuzz-input <route>
```
(The uam-run command after `--` is your replay command **without** the firmware positional
and **without** `-t`/`--fuzz-replay`; the tool adds those.) `--range <fn>` narrows it to the
code under test — use it, or the report is huge and noisy. `--json` for machine reading,
`--top N` to cap, `--trace FILE` to parse a trace you already captured. Needs rizin and
`rzpipe`; without them it exits 2 and you fall back to reading the disassembly yourself
(Ghidra MCP, `objdump`, the recon project).

Read a frontier branch, then read the firmware around its address to learn what input makes
it go the other way.

### 3. Craft and feed an input

Write raw bytes for the route (`uart0`, `mem:…`), bounded by the brain's `--max-size`.
**Name and write it correctly** (see [`references/assist.md`](references/assist.md) for the
full rules):

- Name it `id:NNNNNN,orig:LABEL`. The `id:NNNNNN` prefix is **required**, and ids must only
  ever **increase** (the brain remembers the highest it took from you and never looks lower).
  `LABEL` is a short tag that follows the input into the queue, the import log and any crash.
- **Write atomically**: a temp name with no `id:` prefix, then `mv` into place. The brain
  ignores a name without `id:`, so it never reads a half‑written file.
- Add **`,keep`** (`id:000007,orig:crc_frame,keep`) to make the brain queue the input even
  if it adds no new coverage — a structurally‑right input that is not a find on its own, but
  a good mutation parent.

```
printf 'BOOT\x10\x00...' > camp/llm/queue/.tmp
mv camp/llm/queue/.tmp 'camp/llm/queue/id:000000,orig:boot_frame'
```

The brain imports on its `--sync-interval` (default 60 s; a campaign you start for this can
use a shorter one). Write a **batch** of related guesses at once (vary the length field, the
magic, the command byte) rather than one at a time.

### 4. Add dictionary tokens live — `--watch-dict`

Often the cheapest, highest‑value move: one right token helps every queue entry. When you
read a magic value, a command string, an opcode table or a struct tag out of the firmware,
append it to the watched dict file (same format as `--dict`: `"text"`, `hex:…`, `u32:…`,
`u16be:…`). The brain re‑reads every `--watch-dict-interval` (5 s) and merges new tokens into
the mutator **while it runs**. Write atomically (temp then rename). Merging is additive; it
never removes a token.

```
cp camp/live.dict camp/.live.dict.tmp
printf 'u32:0xdeadbeef\n"RESET"\nhex:a55a\n' >> camp/.live.dict.tmp
mv camp/.live.dict.tmp camp/live.dict
```
Never append to `camp/live.dict` in place: the brain can read a cut line as a valid but
wrong token (`u32:0xdead`), and a merged token is never removed.

### 5. Read back the verdict — `imports.jsonl`

After a sync interval, read the line for your `orig:` label. One JSON object per imported
input, with a `verdict`:

| `verdict` | what it means | your next move |
|---|---|---|
| `newcov` | reached new coverage (a new bucket or hit-count class); now a queue entry | good shape — write near‑variants of it |
| `kept` | no new coverage, but `,keep` queued it | a scaffold; mutate the bytes that gate the branch |
| `nocov` | no new coverage; dropped | the input did not get past an earlier check — fix that first |
| `crash` | a fault/guard/oracle crash | a finding; see its `file` under `crashes/` and hand to **ember-fuzz-triage** |
| `hang` | it hung or timed out (`reason_name` says which) | often a real bug too; triage it |
| `rejected` | the route refused it (bad length/checksum) | satisfy the route's fix‑up/length/CRC |
| `other` | ran abnormally | retry; the connection may have been busy |

`nocov` and `rejected` are the useful failures: they tell you the input died at a check you
can see in the code. Fix that byte, bump the id, write again.

## Board-specific features — always check, then use them

The stock ember interface above is complete and documented, **but the board you are fuzzing
is custom code and its project may patch ember.** It can offer things no generic run has, and
if one helps you reason or triage, **use it** — do not limit yourself to the stock options.
Before and during the loop, look for extras and fold them in:

- **The board's own inputs and regions.** `build/<board>-run --help` and the board source
  (`emulation/boards/…`, its `run.sh`, the ember-dev-build `iter-XX.md` journal) may define
  named input routes, named memory/MMIO regions, input fix‑ups (a checksum/length the board
  repairs for you — then you need not compute it), or extra crash oracles and `--guard`
  presets. A named input or a fix‑up changes what bytes you write and how the route judges
  them.
- **Board/project diagnostics.** The project may add traces, `--on` rule shorthands, extra
  `--log-pc` registers, dump regions, or link ports (`--link`, `uam-link-hub`) that reveal
  state the stock trace does not. If a board‑specific trace or oracle shows you why an input
  is `rejected` or where it stalls, that is faster than guessing.
- **Patches to ember itself.** ember-dev-build keeps any ember change as a `git format-patch`
  in the project (applied to the pinned clone). Such a patch can add an option this skill
  does not mention. Treat `build/<board>-run --help` and the board/journal as the authority;
  this skill is the authority only on the stock mechanism.

When a board feature would help, say so and use it; when in doubt, read the board source and
the journal rather than assume the stock set is all there is. Conversely, do not invent
options: confirm one exists in `--help` or the source before relying on it.

## Guardrails

- **Never stop or reconfigure the running campaign to "help" it.** Your whole effect is
  files you write into your own `queue/` and the watched dict. If something needs a brain
  option (`--import-log`, `--watch-dict`), ask the user to restart with it; do not kill it.
- **Determinism is the campaign's contract.** Do not feed inputs that depend on wall‑clock or
  external state; the route delivers bytes, nothing else.
- **A crash you cause is still a crash to explain.** Hand `crashes/<file>` (+ `.info`, which
  names your input) to **ember-fuzz-triage**; do not declare a bug from the verdict alone.
- **Ids only go up, names are atomic.** The two rules that make imports reliable. Breaking
  either means your inputs are silently skipped or half‑read.
- **Stay within the target.** Write inputs for the route the campaign fuzzes; do not add new
  routes or change boundaries — that is ember-fuzz's job, and changing it invalidates the
  shared queue.

## Checks

`tests/fuzz/llm-check.sh` (in `meson test -C build --suite fuzz-lists`) exercises this whole
interface against the planted‑bug target: an imported input saved as a crash that names its
origin, a `,keep` input queued without new coverage, a token merged live, and
`fuzz_frontier.py` reporting the parser's uncovered branches with their compare bytes. Read
it for a worked, runnable example.
