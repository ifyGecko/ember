# Site lists, boundary files and the brain

Ember collects coverage itself by default (its blocks: `references/harness.md`, "Coverage"),
and begin, end and crash pcs can be ELF symbols, so a campaign needs no static analysis. It
can instead take its coverage sites and iteration boundaries from files made by static
analysis (`fuzz/tools/gen_sites.py`, `gen_boundaries.py`): opt in when a target is worth it
(only chosen points instrumented, site names in errors, ids stable across rebuilds). Its
inputs come from its coverage-guided brain (`fuzz/tools/fuzz_brain.py`, over `--fuzz-net`)
either way. Everything ships in
ember's tree; the project's pinned ember checkout has the tools at `$EMBER/fuzz/tools/`
(`$EMBER` = the project's `subprojects/ember`, or the clone `ember.src` names). That checkout's
`docs/fuzzing.md` (section "Site lists, boundary files and the brain") and
`docs/fuzz-formats.md` are the authority; this page is what matters on a custom board.

## Opting in: a `fuzz-lists.sh` next to `fuzz.sh`

The two files replace two of `fuzz.sh`'s settings, and the runner refuses both at once
(exit 2): `--cov-boundary` replaces `--fuzz-begin`/`--fuzz-end`, and `--cov-sites` replaces
the default block coverage (and `--cov-blocks`, `--cov-range`, `--cov-irq`). Keep the rest (oracles, route, budget) the same in a second script:

```bash
#!/usr/bin/env bash
# fuzz-lists.sh - fuzz.sh with generated sites and boundaries (fuzz-XX.md)
set -euo pipefail
PROJ="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export TRACE="${TRACE-}"
export UNKNOWN_MMIO="${UNKNOWN_MMIO-fault}"
exec "$PROJ/run.sh" -n 2000000000 \
    --console "file:$PROJ/build/fuzz-console.txt" \
    --cov-sites "$PROJ/fuzz/fw.uams" --cov-boundary "$PROJ/fuzz/fw.uamb" --cov-strict \
    --fuzz-crash-pc 0x80001234 \
    --fuzz-input uart0 \
    --fuzz-iter-insns 5000000 \
    "$@"
```

## Making the lists

Two routes follow the same rules: the rizin tools (need rizin and python3's `rzpipe`) and
their Ghidra Jython ports (`fuzz/tools/ghidra/`; headless Ghidra, no rizin). Use whichever
is installed, and make a site list and its boundary file with the same one: the files are
the same, byte for byte, only where both analysers find the same functions and blocks (a
jump table one of them does not resolve gives the other more sites). Ghidra also covers what rizin cannot decode: ARM BE8 images,
AVR32 and 8-bit PIC ELFs, MIPS16e/microMIPS functions.

With rizin:

```bash
T=$EMBER/fuzz/tools
python3 $T/gen_sites.py --strategy entry fw.elf fuzz/fw.uams
python3 $T/gen_boundaries.py --strategy entry --sites fuzz/fw.uams fw.elf fuzz/fw.uamb \
    handle_request:begin
IMAGE=fw.bin ./fuzz-lists.sh --fuzz-replay corpus/
```

With headless Ghidra, on the project ember-dev-recon's `recon.sh` already made (the stage
imported at the RECON base and analysed, `$GHIDRA_PROJECTS/<file name>_recon`), so nothing is
analysed twice and RECON's function names are the ones the markers use:

```bash
H="$GHIDRA_HOME/support/analyzeHeadless"; P="${GHIDRA_PROJECTS:-$HOME/GhidraProjects}"
# -process takes the program's name in the project: the stage file as recon imported it
"$H" "$P" fw.bin_recon -process fw.bin -noanalysis -scriptPath "$EMBER/fuzz/tools/ghidra" \
    -postScript gen_sites.py "$PWD/fuzz/fw.uams" --strategy entry \
    -postScript gen_boundaries.py "$PWD/fuzz/fw.uamb" handle_request:begin \
    --strategy entry --sites "$PWD/fuzz/fw.uams"
IMAGE=fw.bin ./fuzz-lists.sh --fuzz-replay corpus/
```

  (Without a recon project: `"$H" <dir> <name> -import fw.elf -scriptPath ... -postScript
  gen_sites.py ...` imports and analyses first; later calls use `-process`.)

- **The Ghidra ports' command line** (`fuzz/tools/ghidra/README.md` is the authority): the
  first argument is the output file, since the program is the one Ghidra has open; there is
  no `--raw/--base/--arch/--bits/--endian/--cpu`, since the processor, base and byte order
  are those of the import (recon's, here); names are Ghidra's bare ones. A relative output
  path is taken from the directory `analyzeHeadless` was started in. An error ends the
  script without a file: look for its `error:` line in the headless log.
- **They are Jython 2.7 scripts.** Ghidra 11.4 and later no longer bundle Jython: install
  the Jython extension (`Extensions/Ghidra/ghidra_*_Jython.zip`, unzipped into
  `Ghidra/Extensions/`) first.
- **Which functions get sites**: those an ELF's symbol table names (`STT_FUNC`), its entry
  point, and `--symbols FILE` addresses. Stripped firmware has few of those: pass
  `--symbols` (RECON's names: `nm` output, `ADDR NAME` lines or a Ghidra symbol-table CSV),
  name the functions in the Ghidra project, or use `--all-functions` to take every function
  the analyser found.
- **Generate against the bytes the board places**: the stage being fuzzed, at the RECON base.
  For a raw image with the rizin tools: `--raw --base <RECON base> --arch ARCH --bits N
  --endian E --symbols FILE` (`--cpu` for rizin's CPU variant). With Ghidra the import
  already has the base.
- **Where the sites are.** The board places the image itself (`--image os=`), so the runner
  has no image of its own to take a link base from. A list carries its module base table,
  and only `--cov-module <module>=<base>` overrides it (the module name is the image's file
  name, or the program's name in the Ghidra project). `--raw-base` matters only for a
  hand-made list without a base table.
  `--cov-strict` compares every site's bytes in memory with the list at the first begin
  (`cov: site check: N of N sites match the site list, 0 do not`) and catches a wrong base
  or byte order; always use it.
- **The BEGIN marker must precede the input read**: `--strategy entry` (the MIPS default
  anyway; elsewhere the default puts a block's site on its last instruction, after the read).
- **Recursion**: a marker spec is `SYM[:kind[:depth_filter]]`. `handle_request:begin:zero` and
  a matching `:end:zero` keep nested arrivals inside one input; "any depth" markers end the
  iteration at the first END.
- **Thumb, MIPS16e, microMIPS**: the files hold even addresses and flag such sites and
  markers `ALT_STATE`; ember sets bit 0 for them. Pcs you pass yourself (`--fuzz-crash-pc`)
  take bit 0 as in `harness.md`.
- The generators never place a site on an instruction ember runs as part of the one before
  it (a MIPS delay slot, an instruction inside a Thumb IT block); one there stops the run
  naming the record, so it means a wrong base or a hand-made list.
- For fuzzing, the brain suggests `--cov-ngram 3 --cov-rotate`; the defaults are site ids,
  N=2, no rotation, `--cov-mode leapfrog`.

**Which ISAs.** The site-list path is tested end to end on RV32, MIPS32 BE/LE, ARM (Thumb-2,
LE and BE8) and 32-bit PowerPC. The generators also write correct lists (every site's bytes
check out) for 8-bit AVR, Xtensa, 6502, Z80, 8051 (raw, with `--symbols`) and PIC32, though
ember does not test those end to end; rizin and Ghidra disagree on AVR, so make both files
with one route there. Where a list is not worth it (the usual case), keep the default block
coverage and `--fuzz-begin`; the brain drives those too.

## Driving ember from the brain

```bash
python3 $EMBER/fuzz/tools/fuzz_brain.py --listen 127.0.0.1:7000 --seeds fuzz/seeds/ \
    --out fuzz/run/ --status-file fuzz/run/status.jsonl
IMAGE=fw.bin ./fuzz-lists.sh --jit --fuzz-net 127.0.0.1:7000 \
    > build/net.txt 2> build/net.log < /dev/null
python3 $EMBER/fuzz/tools/status_tui.py fuzz/run/status.jsonl   # live status (--once: one frame)
```

**Parallel workers.** One brain drives one ember, and ember is single-threaded: use the
cores with `fuzz_launch.py`, which runs N brain + ember pairs that share their queues through
one directory, each with its own seed and the next of `--schedules` (round robin), and
restarts a worker that dies. The firmware boots once: the first worker saves the begin
snapshot (`--fuzz-snapshot DIR/begin.snap`) and the others, and every restart, start from it
(`--no-shared-boot` boots each worker; a snapshot made from another firmware or command
line is detected by its `.key` fingerprint and replaced; if the first worker has not saved
it within `--boot-timeout`, 3600 s, the others boot themselves). `-n` is 1 to 256,
`--iters N` ends each worker after N mutated executions, `--brain-arg` passes an option on
to every brain, and the launcher exits 0 when all workers ended cleanly, 1 when one ran out
of restarts:

```bash
IMAGE=fw.bin python3 $EMBER/fuzz/tools/fuzz_launch.py -n 8 --out fuzz/camp \
    --seeds fuzz/seeds/ --dict fuzz/fw.dict -- ./fuzz-lists.sh --jit   # ember's command after --
python3 $EMBER/fuzz/tools/status_tui.py --sync-dir fuzz/camp   # one row per worker
```

Each worker's files are under `fuzz/camp/NAME/` (`main`, `w1`, ...: queue, crashes, hangs,
`brain.log`, `ember.log`); entries imported from another worker are named `sync:PEER`.
Crashes are deduplicated per worker: replay every `crashes/` directory, or group them with
`scripts/replay_summary.py`. A worker does not minimize a crash another worker already holds
minimized (its copy stays as found, `minimized=peer` in the `.info`). A restarted worker
takes up the crashes it had left for later (`minimized=later`) again.

- Ember connects, boots to the begin pc, and only then sends HELLO and the INIT map, so a
  long boot is fine (the brain's `--hello-timeout`, 600 s by default; 0 waits forever). `-n`
  still bounds the boot. Any coverage source works: HELLO describes the scheme (the block
  model by default, or the site list's). HELLO's arch is the ISA's name.
- Start the brain first and ember within 15 s of it (`--accept-timeout`). The brain keeps a
  resumable queue in `--out`/`queue` (`id:NNNNNN,...` names recording each entry's origin;
  a restart continues the campaign); `--iters N` bounds a run, 0 (the default) runs until
  stopped. It asks ember for sparse replies (the hit buckets and the instruction count), and
  ember lists those from what the coverage op recorded, so a big map costs nothing extra.
- The campaign (`docs/fuzzing.md`, "The campaign"): a favored set, calibration (each new
  entry runs twice; an `UNSTABLE` line means the harness leaks state between iterations:
  fix that first, the readiness gate in `SKILL.md`), trimming (ember cuts and runs; a cut
  that reaches new code is kept as an `op:trim` entry), and energy from a power
  schedule: `--schedule fast` (default), `explore`, `exploit`, `coe`, `rare`, and the
  variants `lin`, `quad` (gentler `fast`), `seek` (`explore` ignoring cost) and `mmopt`
  (`explore` favoring the newest, deepest entries). After the
  baseline it suggests a `--fuzz-iter-insns` (5x the slowest entry).
- Compare logging (on when ember offers it in HELLO; `--no-cmplog` turns it off): each
  queue entry, when first chosen, goes through an input-to-state stage that ember runs by
  itself: the entry once with its compares logged, then every input with a compare's other
  operand written where one operand's bytes are in the entry (both byte orders, also at
  the narrowest width; ±1 for ordered compares), up to `--cmplog-max` (256) runs per entry (fewer for an entry slower than average, 16 at least; with `fuzz_launch.py` only `main` runs the stage unless `--cmplog-workers N|all` says otherwise). Code constants the input
  lacks join the dictionary. Normal runs pay 1-3% for it. It reaches what havoc cannot
  guess (a 32-bit magic value from an unrelated seed in a few thousand runs), but not
  compares done a byte at a time in a loop (`strcmp`, `memcmp`) or on values computed
  from the input (parsed numbers, checksums): give those a dictionary. Its finds are
  `op:cmplog` queue entries; the status shows `cmplog_execs` and `cmplog_finds`. The
  log works with any coverage, but with function-entry sites only, an input that passes a
  compare inside a function is not new coverage and is dropped: the default block coverage
  (or a site list with block-level sites) lets compare logging chain one compare after
  another.
- A dictionary (`--dict FILE`, repeatable) in ember's format: `"text"`, `hex:BYTES`,
  `u8:`/`u16:`/`u32:`/`u64:` integers in the target's byte order (from HELLO), `u32be:`
  and the like for a fixed order. Take the tokens from the handler's compares in Ghidra:
  magic numbers, command words, header bytes; integer tokens are also tried ±1. ember does
  the mutating (havoc + splice, `fuzz/mutate.c`) in batches the brain asks for (HAVOC) and
  reports only new coverage, new crashes and hangs, so the brain is no bottleneck: exec
  rate follows the target's path length. Lengths move both ways evenly between one byte
  and `--max-size` (a mutation is as likely shorter as longer; `RESIZE` crosses the range
  in one step), so a one-byte seed is fine. Set `--max-size` (default 65536) to what the
  route can deliver: `mem:`'s `max` (default 4096) or the UART's buffer. A
  plain route tests only a longer input's prefix; a `reject` route answers CRASH
  `REJECTED`, which the brain counts (`rejected` in its status) and does not save.
- Verdicts become CRASH frames: `FAULT` 1, `GUARD` 2 (a redzone or `--guard`, with the pc of
  the access), `ORACLE` 3, `HANG` 4, `TIMEOUT` 5. The brain saves one input per distinct
  `(reason, status)` of a session under `--out/crashes/` (and up to
  `--crashes-per-sig` (4) in all, as found, when a crash at that pc hit a bucket no earlier
  crash at it had: look at those too, they may be other bugs; a crash met while minimizing
  another is saved as found the same way), as
  `crash_r<reason>_s<status>_<sha1>` plus an `.info` note (`reason=`, `reason_name=`,
  `status=`, `status_hex=`, `minimized=`, `len=`, `sig=`, `hex=`). Only the first crash is
  minimized at once: one bug often faults at many instructions, each a `(reason, status)`
  of its own. The later ones are saved as found and minimized one by one within
  `--minimize-share` percent of the runs (default 2), those that ran the most code no
  earlier crash ran first. Nothing is dropped or merged: triage every file, and read
  `minimized=later` as "not shrunk yet", not as "duplicate". With `--fuzz-uninit` on ember's side (an
  opt-in check, `references/harness.md`) the inputs whose run depended on memory nothing wrote go
  under `uninit/`, one per distinct finding, with a `.info` (`differs=`, the filled run's verdict
  and length, and its `read_before_write=` lines); they are not crashes and the input's coverage
  still counts. Hangs and timeouts go under `hangs/`:
  every distinct input, up to `--max-hangs` (64), not minimized unless
  `--minimize-timeouts`. Both directories replay directly (`--fuzz-replay fuzz/run/crashes/`, the `.info` notes are
  skipped), so triage follows `harness.md`. A malformed INPUT gets CRASH `MALFORMED` (7).
- The run ends with exit 0 on the brain's BYE, 2 when the brain closes without one or ember
  hits an internal error, 130 on SIGINT (BYE sent). `--fuzz-net-timeout SEC` ends it (with
  an error) when the brain goes silent for SEC seconds; without it ember waits, and only
  TCP keepalive notices a vanished brain host.
- `--cov-shm PATH` (`--cov-shm-ring N`, default 8 slots) publishes INIT and every ok
  iteration's map in a coverage region (`python3 $EMBER/fuzz/tools/cov_region.py stats
  PATH`; the tool takes a file path, so a shared-memory name is `/dev/shm/NAME`). A PATH
  with a `/` after its first character is a file, any other a shared-memory name; a region another live process owns is refused. It works next to
  `--fuzz-replay` too.

## How boundaries behave

Every input starts from one snapshot at the first BEGIN the boot reached; the code between
an END and the next BEGIN runs once, at boot, and counts in INIT; with "any depth" markers
the first END ends the iteration; markers need not be sites (keep both files from one
generator run); crashed iterations are not published to the coverage region; several
processors (`-o cpus=N`) share one map, each in its own history.
