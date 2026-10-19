# Using ember with suzzer

ember and [suzzer](https://github.com/ifyGecko/suzzer) are separate projects in separate
repositories. Neither contains the other's files:

- **ember alone** is a complete firmware emulator with its own coverage and fuzz harness. It
  builds, runs and passes its tests with no suzzer anywhere.
- **suzzer alone** is coverage and remote fuzzing for embedded Linux userland (libcov, the
  site and boundary generators, `suzzer_brain.py`).
- **Together**, ember runs firmware under emulation with suzzer's coverage sites and iteration
  boundaries, producing bitmaps that mean the same as libcov's on a real device, so corpora
  and coverage carry over between the two.

They meet only through the formats suzzer publishes: the LCV3 site list, the LCVB1 boundary
file, the LCVS shared-memory region and the SZR1 protocol, specified in suzzer's
`docs/FORMATS.md` and defined in its `src/lcv_formats.h`. ember's support for them lives in
`fuzz/suzzer/` (the only ember code that knows suzzer exists). It includes no suzzer code
beyond that one format header, links none, and runs no suzzer tool while building.

## Getting suzzer into the build

To use the two together, ember's build needs a suzzer checkout to read the format header and
the conformance vectors from. There are two ways to provide one.

**An existing checkout** (no second copy). Point the build at it:

```bash
make SUZZER_DIR=~/src/suzzer
make SUZZER_DIR=~/src/suzzer test
```

**A clone inside ember's tree**, for a self-contained setup:

```bash
make suzzer        # clones into ./suzzer (SUZZER_URL, SUZZER_REV to override)
make               # ./suzzer is picked up automatically
```

`./suzzer` is ignored by git, so it never becomes part of ember's repository. It may also be a
symlink to a checkout kept elsewhere. `make suzzer` does nothing if a checkout is already there.
Update it with git like any other clone, or delete it to drop the integration.

How the build decides:

1. `SUZZER_DIR=path` if given. It must be a suzzer checkout with `src/lcv_formats.h`, or the
   build stops and says so. `SUZZER_DIR=` (empty) turns the integration off even when
   `./suzzer` exists.
2. Otherwise `./suzzer`, if it holds `src/lcv_formats.h`.
3. Otherwise none. ember builds without the suzzer options (`--cov-sites`, `--cov-boundary`,
   ...), and `make test` reports the suzzer tests as skipped.

Switching between these rebuilds what depends on the choice (`build/suzzer.cfg` records it).

## Compatibility

ember needs a suzzer that publishes its format header, `src/lcv_formats.h`. A checkout
without one (older suzzer) is treated as no suzzer at all; `make suzzer` warns about it.

ember's suzzer support implements these format versions: LCV3 3, LCVB1 1, LCVS 1, SZR1 1. The
build checks them against the header (`fuzz/suzzer/scheme.c`). A suzzer that changes a format
version stops ember's build with a message naming the format, instead of being misread.
Changes suzzer makes within a version (new flag bits, new crash reasons, fields taking over
reserved bytes) need nothing from ember.

The conformance vectors are read from the checkout itself (`test/vectors`). So
`make SUZZER_DIR=... test` always checks ember against exactly the suzzer it was built with:
every vector must come out byte for byte, through `UAM_OP_COV` on the interpreter and the JIT.

## What you get

With suzzer configured, `uam-run` (and every board built on `uam_run_main`) gains:

| Option | |
|---|---|
| `--cov-sites LCV3` | coverage sites from a patch list (`gen_patchlist.py`) |
| `--cov-module NAME=BASE` | a module's load base; otherwise the list's base table, then the image's link base |
| `--cov-scheme site\|pc`, `--cov-ngram N`, `--cov-rotate` | match the device's libcov build |
| `--cov-strict` | every site's bytes in memory must match the list |
| `--cov-boundary LCVB1` | iteration boundaries from `gen_boundary.py`: one BEGIN marker for now, any number of END markers |
| `--fuzz-szr HOST:PORT` | fuzz with inputs from `suzzer_brain.py` over SZR1, ember standing in for `suzzer-relay` |

and `make test` adds `test_suzzer` (the format readers and the conformance vectors) and
`suzzer-test` (the options end to end on RV32 and MIPS32 BE/LE, and the SZR1 executor
against a scripted brain, `tests/szr/fake_brain.py`). See
[`fuzzing.md`](fuzzing.md#suzzer) for how the options behave.

A typical session:

```bash
# on the suzzer side: sites and boundaries for the firmware
python3 suzzer/tools/gen_patchlist.py fw.elf fw.lcv3
python3 suzzer/tools/gen_boundary.py --patchlist fw.lcv3 fw.elf fw.lcvb handle_request:begin
# on the ember side
build/uam-run -b mips-ref --cov-sites fw.lcv3 --cov-boundary fw.lcvb --cov-strict \
    --fuzz-input uart0 --fuzz-replay corpus/ fw.elf
```

(The generators and their options belong to suzzer; see its `HOWTO.md`. They need rizin.)

To let suzzer's brain drive ember directly, start the brain and point ember at it:

```bash
python3 suzzer/tools/suzzer_brain.py --listen 127.0.0.1:7000 --seeds seeds/ --out run/
build/uam-run -b mips-ref --cov-sites fw.lcv3 --cov-boundary fw.lcvb --cov-strict \
    --fuzz-input uart0 --fuzz-szr 127.0.0.1:7000 fw.elf
```

Planned on top of this: LCVS shared-memory export for suzzer's reader tools.
