# Site lists, boundary files and the brain

Ember can take its coverage sites and iteration boundaries from files made by static
analysis (`fuzz/tools/gen_sites.py`, `gen_boundaries.py`) and its inputs from its
coverage-guided brain (`fuzz/tools/fuzz_brain.py`, over `--fuzz-net`). Everything ships in
ember's tree; the project's pinned ember checkout has the tools at `$EMBER/fuzz/tools/`
(`$EMBER` = the project's `ember-src`, or the clone `ember.src` names). That checkout's
`docs/fuzzing.md` (section "Site lists, boundary files and the brain") and
`docs/fuzz-formats.md` are the authority; this page is what matters on a custom board.

## A `fuzz-lists.sh` next to `fuzz.sh`

The two files replace two of `fuzz.sh`'s options, and the runner refuses both at once
(exit 2): `--cov-boundary` replaces `--fuzz-begin`/`--fuzz-end`, and `--cov-sites` replaces
`--cov-blocks`. Keep the rest (oracles, route, budget) the same in a second script:

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

Two routes write the same files, byte for byte: the rizin tools (need rizin and python3's
`rzpipe`) and their Ghidra Jython ports (`fuzz/tools/ghidra/`; headless Ghidra, no rizin).
Use whichever is installed. Ghidra also covers what rizin cannot decode: ARM BE8 images,
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
  are those of the import (recon's, here); names are Ghidra's bare ones. Output paths must be
  absolute: the script runs in Ghidra's directory, not yours.
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
  has no image of its own to take a link base from. A list carries its module base table;
  `--cov-module <module>=<base>` or `--raw-base <BOARD_IMAGE_BASE>` overrides it.
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
with one route there. Where a list is not worth it, keep `--cov-blocks` and `--fuzz-begin`;
the brain drives those too.

## Driving ember from the brain

```bash
python3 $EMBER/fuzz/tools/fuzz_brain.py --listen 127.0.0.1:7000 --seeds fuzz/seeds/ \
    --out fuzz/run/ --status-file fuzz/run/status.jsonl
IMAGE=fw.bin ./fuzz-lists.sh --jit --fuzz-net 127.0.0.1:7000 \
    > build/net.txt 2> build/net.log < /dev/null
python3 $EMBER/fuzz/tools/status_tui.py fuzz/run/status.jsonl   # live status (--once: one frame)
```

- Ember connects, boots to the begin pc, and only then sends HELLO and the INIT map, so a
  long boot is fine (the brain's `--hello-timeout`, 600 s by default; 0 waits forever). `-n`
  still bounds the boot. Any coverage source works: with `--cov-sites` HELLO describes the
  scheme, with `--cov-blocks` none. HELLO's arch is the ISA's name.
- The brain keeps a resumable corpus in `--out`/`corpus` (a restart continues the campaign);
  `--iters N` bounds a run, 0 (the default) runs until stopped. Set `--max-size` (default
  65536) to what the route can deliver: `mem:`'s `max` (default 4096) or the UART's buffer. A
  plain route tests only a longer input's prefix; a `reject` route answers CRASH
  `REJECTED`, which the brain counts (`rejected` in its status) and does not save.
- Verdicts become CRASH frames: `FAULT` 1, `GUARD` 2 (a redzone or `--guard`, with the pc of
  the access), `ORACLE` 3, `HANG` 4, `TIMEOUT` 5. The brain saves each distinct
  `(reason, status)` under `--out/crashes/` (hangs and timeouts under `hangs/`) as the input
  plus an `.info` sidecar (`reason_name=`, `status=`, `len=`, `hex=`), minimized. Both
  directories replay directly (`--fuzz-replay fuzz/run/crashes/`, the `.info` notes are
  skipped), so triage follows `harness.md`. A malformed INPUT gets CRASH `MALFORMED` (7).
- The run ends with exit 0 on the brain's BYE, 2 when the brain closes without one or ember
  hits an internal error, 130 on SIGINT (BYE sent). `--fuzz-net-timeout SEC` ends it (with
  an error) when the brain goes silent for SEC seconds; without it ember waits, and only
  TCP keepalive notices a vanished brain host.
- `--cov-shm PATH` (`--cov-shm-ring N`, default 8 slots) publishes INIT and every ok
  iteration's map in a coverage region (`python3 $EMBER/fuzz/tools/cov_region.py stats
  PATH`). A PATH with a `/` after its first character is a file, any other a shared-memory
  name (`/dev/shm`); a region another live process owns is refused. It works next to
  `--fuzz-replay` too.

## How boundaries behave

Every input starts from one snapshot at the first BEGIN the boot reached; the code between
an END and the next BEGIN runs once, at boot, and counts in INIT; with "any depth" markers
the first END ends the iteration; markers need not be sites (keep both files from one
generator run); crashed iterations are not published to the coverage region; several
processors (`-o cpus=N`) share one map, each in its own history.
