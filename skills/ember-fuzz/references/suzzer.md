# suzzer with ember

[suzzer](https://github.com/ifyGecko/suzzer) is a separate project: coverage and remote
fuzzing for embedded Linux userland (libcov, site and boundary generators,
`suzzer_brain.py`).
Ember reads its published formats and plays the executor for its brain, so a firmware fuzzed
under ember produces maps that mean the same as libcov's on a device, and corpora carry
over. Ember's `docs/suzzer.md` and `docs/fuzzing.md` (section "suzzer") in the pinned
checkout are the authority; this page is what matters on a custom board.

## Build the project's ember with suzzer

The suzzer options exist only in a libuam built against a suzzer checkout (ember reads
suzzer's `src/lcv_formats.h` and nothing else). ember-dev-build's `build-ember.sh` does it:

```bash
scripts/build-ember.sh --suzzer ~/src/suzzer    # remembered in suzzer.dir; --suzzer off removes it
build/<board>-run --help | grep -- --cov-sites  # present now
```

A suzzer whose format versions differ from ember's (LCV3 3, LCVB1 1, LCVS 1, SZR1 1) stops
the library build, naming the format: pin a matching suzzer or move the ember pin.

## A `fuzz-suzzer.sh` next to `fuzz.sh`

suzzer's files replace two of `fuzz.sh`'s options, and the runner refuses both at once
(exit 2): `--cov-boundary` replaces `--fuzz-begin`/`--fuzz-end`, and `--cov-sites` replaces
`--cov-blocks`. Keep the rest (oracles, route, budget) the same in a second script:

```bash
#!/usr/bin/env bash
# fuzz-suzzer.sh - fuzz.sh with suzzer's sites and boundaries (fuzz-XX.md)
set -euo pipefail
PROJ="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export TRACE="${TRACE-}"
exec "$PROJ/run.sh" -n 2000000000 \
    --console "file:$PROJ/build/fuzz-console.txt" \
    --cov-sites "$PROJ/fuzz/fw.lcv3" --cov-boundary "$PROJ/fuzz/fw.lcvb" --cov-strict \
    --fuzz-crash-pc 0x80001234 \
    --fuzz-input uart0 \
    --fuzz-iter-insns 5000000 \
    "$@"
```

## Site lists and boundaries

```bash
# suzzer side (needs rizin): sites and boundaries for the exact bytes the board runs
python3 suzzer/tools/gen_patchlist.py --strategy entry fw.elf fuzz/fw.lcv3
python3 suzzer/tools/gen_boundary.py --strategy entry --patchlist fuzz/fw.lcv3 fw.elf \
    fuzz/fw.lcvb handle_request:begin
# ember side
IMAGE=fw.bin ./fuzz-suzzer.sh --fuzz-replay corpus/
```

- **Generate against the bytes the board places**: the stage being fuzzed, at the RECON base.
  For a raw image, suzzer's `--raw --base <RECON base> --symbols ...`.
- **Where the sites are.** The board places the image itself (`--image os=`), so the runner
  has no image of its own to take a link base from. A list generated from an ELF carries
  its base and needs nothing more; for a list generated from a raw image, add
  `--cov-module <module>=<base>` (the module name the list records) or
  `--raw-base <BOARD_IMAGE_BASE>` to `fuzz-suzzer.sh`. `--cov-strict` compares every
  site's bytes in memory with the list at the first begin (`cov: site check: N of N sites
  match the patch list, 0 do not`) and catches a wrong base or byte order; always use it.
- **The BEGIN marker must precede the input read**: `--strategy entry` (the MIPS default
  anyway; RV32's default puts a function's site after its first read).
- **Thumb** (`arm-ref`): suzzer gives every site its even address and marks Thumb sites by
  their 2-byte length; the importer sets bit 0 on those, and on each boundary marker whose
  closest site at or below it is one, so a Thumb marker needs `--cov-sites` next to
  `--cov-boundary`. Pcs you pass yourself (`--fuzz-crash-pc`) take bit 0 as in
  `harness.md`. The importer applies the same rule (bit 0 on 2-byte sites only) to
  MIPS16e/microMIPS code on PIC32, where no list is tested end to end: prefer
  `--cov-blocks` there.
- A site in a MIPS branch delay slot cannot be instrumented; the run stops naming the
  record. suzzer's generators never place one, so it means a wrong base or a hand-made list.
- Match the device's libcov build when maps must compare with a device:
  `--cov-scheme site|pc`, `--cov-ngram N`, `--cov-rotate`, `--cov-mode leapfrog` (default).

**Which ISAs.** ember's site-list path is tested end to end (`suzzer-check.sh`) on RV32,
MIPS32 BE/LE, ARM (Thumb-2, LE and BE8) and 32-bit PowerPC; suzzer's generators describe
x86-64, AArch64, ARM/Thumb, PowerPC, RISC-V and MIPS. On the other ember ISAs (8-bit AVR and
PIC, AVR32, 8051, 6502, Z80, Xtensa) keep `--cov-blocks` and `--fuzz-begin`; the brain
still drives them.

## Driving ember from suzzer's brain

```bash
python3 suzzer/tools/suzzer_brain.py --listen 127.0.0.1:7000 --seeds fuzz/seeds/ --out fuzz/run/
IMAGE=fw.bin ./fuzz-suzzer.sh --jit --fuzz-szr 127.0.0.1:7000 \
    > build/szr.txt 2> build/szr.log < /dev/null
```

- Ember connects, boots to the begin pc, and only then sends HELLO and the INIT map, so a
  long boot is fine; an older brain without `--hello-timeout` needs a larger `--io-timeout`.
  `-n` still bounds the boot. Any coverage source works: with `--cov-sites` HELLO
  describes the scheme, with `--cov-blocks` none. HELLO's arch is the ISA's (`rv32`,
  `mips32be`, `mips32le`, `armv7`, `armv7be`, `ppc32`, `x86-64`, `avr`, `avr32`, `pic`,
  `mcs51`, `m6502`, `z80`, `xtensa`, `xtensaeb`).
- Verdicts become CRASH frames (fault 5, oracle 6, hang 7, timeout 8, rejected 3; a crash
  (guard), from a redzone or a `--guard`, is sent as GUEST_FAULT 5 with the pc of the
  access, so only a replay tells it from a fault and names the guard); the brain
  saves and minimizes them under `--out`. Its `crashes/` and `hangs/` directories replay
  directly (`--fuzz-replay fuzz/run/crashes/`, the `.info` notes are skipped), so triage
  follows `harness.md`. A malformed INPUT gets CRASH `MALFORMED` (4) and runs nothing.
- The run ends with exit 0 on the brain's BYE, 2 when the brain closes without one or ember
  hits an internal error, 130 on SIGINT (BYE sent). `--fuzz-szr-timeout SEC` ends it (with
  an error) when the brain goes silent for SEC seconds; without it ember waits, and only
  TCP keepalive notices a vanished brain host.
- `--cov-shm PATH` (`--cov-shm-ring N`, default 8 slots) publishes INIT and every ok
  iteration's map in an LCVS region for suzzer's readers (`suzzer/test/shm_bitmap.py stats
  PATH`). A PATH with a `/` after its first character is a file, any other a shared-memory
  name (`/dev/shm`); a region another live process owns is refused. It works next to
  `--fuzz-replay` too.

## Differences from libcov on a device

Every input starts from one snapshot at the first BEGIN the boot reached; there are no GAP
maps (code between an END and the next BEGIN counts in INIT); with "any depth" markers the
first END ends the iteration; markers need not be sites (keep both lists from one generator
run); crashed iterations are not published to LCVS; no counterpart to libcov's MT variant
(several processors share one map, each in its own history).
See ember's `docs/suzzer.md` for the full list before comparing ember's maps with a device's.
