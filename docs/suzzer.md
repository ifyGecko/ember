# Using ember with suzzer

ember and [suzzer](https://github.com/ifyGecko/suzzer) are separate projects in separate
repositories. Neither contains the other's files:

- **ember alone** is a complete firmware emulator with its own coverage and fuzz harness. It
  builds, runs and passes its tests with no suzzer anywhere.
- **suzzer alone** is coverage and remote fuzzing for embedded Linux userland (libcov, the
  site and boundary generators, `suzzer_brain.py`).
- **Together**, ember runs firmware under emulation with suzzer's coverage sites and iteration
  boundaries, producing bitmaps that mean the same as libcov's on a real device, so corpora
  and coverage carry over between the two. ember reproduces libcov's bucket formula and its
  emit stream (FORMATS.md 5.7: leapfrog's collapsed repeats, boundary markers counted in
  the snapshot they close), and suzzer's brain drives ember as it drives `suzzer-relay`.
  The differences that remain are listed under [Differences from libcov](#differences-from-libcov).

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
make suzzer        # clones into ./suzzer: SSH, then HTTPS (SUZZER_URL, SUZZER_REV to override)
make               # ./suzzer is picked up automatically
```

`./suzzer` is ignored by git, so it never becomes part of ember's repository. It may also be a
symlink to a checkout kept elsewhere. `make suzzer` does nothing if a checkout is already there.
Update it with git like any other clone, or delete it to drop the integration.

How the build decides:

1. `SUZZER_DIR=path` if given. It must be a suzzer checkout with `src/lcv_formats.h`, or the
   build stops and says so. `SUZZER_DIR=` (empty) turns the integration off even when
   `./suzzer` exists.
2. Otherwise the choice this build directory made last (`build/suzzer.cfg` records it): the
   same checkout, or off. So `make SUZZER_DIR=~/src/suzzer` once and plain `make` (or
   `make test`) after it keeps the suzzer options. If that checkout is gone, the build says
   so and goes on to 3.
3. Otherwise `./suzzer`, if it holds `src/lcv_formats.h`.
4. Otherwise none. ember builds without the suzzer options (`--cov-sites`, `--cov-boundary`,
   ...), and `make test` reports the suzzer tests as skipped.

Switching between these rebuilds what depends on the choice. `make clean` forgets it. After
`make SUZZER_DIR=` a clone made by `make suzzer` is used only once a path is given again
(`make SUZZER_DIR=suzzer`).

`make suzzer` clones over SSH first and, if that fails (no keys on this machine), over
HTTPS, which needs a GitHub credential helper or token while the repository is private.
`SUZZER_URL` picks the URL instead.

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
| `--cov-mode leapfrog\|every` | which executions count: libcov's default (`leapfrog`, the default here too) or every one |
| `--cov-strict` | every site's bytes in memory must match the list |
| `--cov-boundary LCVB1` | iteration boundaries from `gen_boundary.py`: any number of BEGIN and END markers |
| `--fuzz-szr HOST:PORT` | fuzz with inputs from `suzzer_brain.py` over SZR1, ember standing in for `suzzer-relay` |
| `--fuzz-szr-timeout SEC` | give up when the brain sends nothing for SEC seconds (default: wait; TCP keepalive still notices a vanished host) |
| `--cov-shm PATH`, `--cov-shm-ring N` | publish INIT and every ok iteration's map in an LCVS region for suzzer's readers |

and `make test` adds `test_suzzer` (the format readers and the conformance vectors) and
`suzzer-test`: the options end to end on RV32 and MIPS32 BE/LE (with lists from suzzer's own
generators too, when rizin and rzpipe are installed), the LCVS region read back by
suzzer's `test/shm_bitmap.py` and an independent reader, the SZR1 executor against a
scripted brain (`tests/szr/fake_brain.py`), and a session with suzzer's own
`tools/suzzer_brain.py` (`tests/szr/brain-check.sh`). See
[`fuzzing.md`](fuzzing.md#suzzer) for how the options behave.

A typical session:

```bash
# on the suzzer side: sites and boundaries for the firmware
python3 suzzer/tools/gen_patchlist.py --strategy entry fw.elf fw.lcv3
python3 suzzer/tools/gen_boundary.py --strategy entry --patchlist fw.lcv3 fw.elf fw.lcvb \
    handle_request:begin
# on the ember side
build/uam-run -b mips-ref --cov-sites fw.lcv3 --cov-boundary fw.lcvb --cov-strict \
    --fuzz-input uart0 --fuzz-crash-pc 0x80102000 --fuzz-replay corpus/ fw.elf
```

(The generators and their options belong to suzzer; see its `HOWTO.md`. They need rizin.
`--strategy entry` puts the BEGIN marker at the handler's first instruction, before it reads
any input, which is where ember snapshots; on RV32 the generators' default would place it
after the read. `--fuzz-crash-pc` names the firmware's fatal-error routine, e.g. from
`nm fw.elf`. For a raw image, generate with suzzer's `--raw --base ... --symbols ...` and run
it with `--raw-base` and, if its reset code is not at its first byte, `--entry`.)

To let suzzer's brain drive ember directly, start the brain and point ember at it:

```bash
python3 suzzer/tools/suzzer_brain.py --listen 127.0.0.1:7000 --seeds seeds/ --out run/
build/uam-run -b mips-ref --cov-sites fw.lcv3 --cov-boundary fw.lcvb --cov-strict \
    --fuzz-input uart0 --fuzz-szr 127.0.0.1:7000 fw.elf
```

Add `--cov-shm /tmp/fw.lcvs` to either command to watch the run with suzzer's region
readers, e.g. `python3 suzzer/test/shm_bitmap.py stats /tmp/fw.lcvs`.

## Differences from libcov

What ember does differently from libcov on a device, by design or for now:

- **One snapshot.** With several BEGIN markers, every input starts from the snapshot at the
  first BEGIN the boot reached; on a device each BEGIN starts an iteration where it is.
  During an iteration every BEGIN closes it (begin-only) or opens a level (nested) in both.
- **"Any depth" paired markers.** ember ends the iteration at the first END; libcov
  publishes and resets at every nested BEGIN and END. Depth-zero-only markers (recursion
  kept in one input) behave the same in both.
- **No GAP snapshots.** Each input starts from the snapshot at BEGIN, so the code between an
  END and the next BEGIN runs once, at boot, and is counted in INIT.
- **Markers need not be sites.** ember stops at a boundary pc whether or not the patch list
  has a site there; libcov only fires markers that are sites. Use lists from one generator
  run.
- **Crashed iterations.** Their maps go to the frontend (the brain gets CRASH frames, replay
  prints them) but not to the LCVS ring, matching a device whose process died.
- **libcov settings without an ember equivalent:** `LIBCOV_MODE=oneshot`,
  `LIBCOV_BOUNDARY_REARM` and marker emulation. `--cov-map-size` stands in for libcov's
  map-size knobs, and `--fuzz-begin-skip N` for `LIBCOV_BOUNDARY_INIT_GRACE` (it counts
  arrivals at the BEGIN marker, where libcov counts every boundary that fires).
- **Single hart.** libcov's MT variant has no counterpart; ember never sets `MT`.
