---
name: ember-fuzz
description: "Fuzz firmware that already runs on an ember (UAM) board with ember's in-process, snapshot-based fuzz harness, on any ember ISA. Use when the emulation built with ember-dev-build works and the user wants to fuzz it, measure coverage, replay or triage crashing inputs, or use generated site lists, boundary files and ember's fuzz brain. Covers: the readiness gate (snapshot-exact, deterministic), the handler and begin/end pcs (Ghidra), input routes (a guest buffer, MMIO input rules, a board's named input), crash oracles (exception classes, crash pcs, interrupts, mem: redzones, --guard; cores without exceptions), --fuzz-replay smoke tests, coverage, campaigns (ember's brain over --fuzz-net, or uam/fuzz.h), crash dedup. Not for bring-up (ember-dev-build). Triggers: 'fuzz this firmware with ember', '--fuzz-begin', '--fuzz-replay', 'fuzz input route', 'uart0 input', 'coverage map', 'triage crashes', 'crash-guard', 'redzone', '--guard', 'fuzz brain', '--fuzz-net', '--cov-sites'."
---

# ember-fuzz: fuzzing firmware on a working ember board

Input: an emulation project from **ember-dev-build** (its `emulation/`, `run.sh`,
`RECON.md` and `iter-XX.md` journal) whose firmware reaches the code you want to fuzz.
Output: a fuzz configuration that is checked against hand-made inputs, a corpus or
campaign, and reproducible crash reports, journaled in `fuzz-XX.md`.

Ember's harness boots once to a **begin pc**, snapshots there, and runs every input from that
snapshot through a named **route**, classifying each run as ok, crash (fault, oracle or
guard), hang, timeout or rejected. Runs are deterministic: the same input from the same
snapshot gives the same verdict and the same coverage map, on the interpreter and the JIT.
It works the same on every ISA ember has; what differs per core is which crash oracles
exist. Inputs come from a frontend: saved inputs (`--fuzz-replay`), ember's coverage-guided
brain (`fuzz/tools/fuzz_brain.py` over `--fuzz-net`), or your own program on `uam/fuzz.h`.

Read `references/harness.md` before configuring a target (routes, boundaries, oracles,
per-ISA notes, verdicts, coverage, triage), and `references/lists-and-brain.md` for site lists, boundary files and
the brain (the lists come from ember's rizin generators or, without rizin, from their Ghidra
ports run by headless Ghidra on the recon project). This skill ships in ember's repository (`skills/ember-fuzz/`, beside
`skills/ember-dev-recon/` and `skills/ember-dev-build/`), but the project links the ember
it pins: that checkout's `docs/fuzzing.md`, `docs/fuzz-formats.md` and `build/<board>-run --help`
are the authority on option details, and a pin older than this skill may lack options used
here. Script paths are relative to this skill's directory; python3 is all
`scripts/replay_summary.py` needs.

## Gate: is the emulation ready to fuzz?
Fuzzing an emulation that is still missing devices finds emulator gaps, not firmware bugs.
Before anything else, check and **stop and hand back to ember-dev-build** if one fails:
- the firmware boots past the target handler's first input read, with no guest exceptions
  on the way (ember-dev-build milestones 1-5 as far as the target needs them);
- the board is **snapshot-exact**: a run split by `--save-snapshot` / `--load-snapshot`
  equals the uninterrupted run, interpreter and JIT on either side (ember-dev-build
  `references/snapshots.md`). Every device needs `save`/`restore`; the harness cannot start
  otherwise;
- the interpreter and `--jit` agree on the boot. If they don't, that is an ember bug to
  report, not something to fuzz around. (`--jit` compiles on x86-64 and AArch64 hosts;
  elsewhere it falls back to the interpreter and this check proves nothing.)

All board and device changes below follow ember-dev-build's organization rules: the project
is the same, the board only wires, one device per file, `scripts/rebuild.sh` after edits.

## Workflow

1. **Pick the target** in Ghidra (MCP, or headless through ember-dev-recon's
   `scripts/ghidra-query.sh` when the server is unavailable): the function that consumes
   untrusted input (a command parser, a packet handler, a config loader). Record its entry,
   where it reads its input (a FIFO, a buffer, registers), and the RTOS's
   fatal-error/exception reporter. Name the functions; cite addresses. On a core without
   exceptions (8-bit AVR and PIC, 8051, 6502, Z80) there is no exception reporter: note
   the firmware's own error paths and see `references/harness.md`, "Per ISA".
2. **Pick the route** (`references/harness.md`, "Routes"): the device the input arrives
   through, published by the board as a named input (`uart0`, `eth0.rx`; ember's PC board
   names its serial port `com1`), is the most faithful; `mem:ADDR` to a buffer the handler
   reads is the quickest; `mmio` feeds `--mmio-rule ...=input` registers (`--trace mmio
   --trace-mmio-range` on a normal run shows which registers the handler reads its input
   from, with their pcs). If the firmware
   reaches the handler only after console interaction (a login, a menu choice), script it
   for the boot (`--console file:OUT,script=FILE`, or `string:TEXT,after=PATTERN`), all of
   it consumed before the begin pc (`references/harness.md`, "Reaching the handler").
3. **Publish the input** if the route is a device: a feed function in the device's file and
   a wrapper plus `uam_machine_add_input` in the board (`references/harness.md`,
   "Publishing an input"). Rebuild.
4. **Configure the boundaries and oracles**: `--fuzz-begin` at the handler entry (before the
   input is read), optional `--fuzz-end`, `--fuzz-crash-pc` at the fatal-error reporter,
   `--fuzz-crash-exc` for the firmware's normal exceptions, `--fuzz-iter-insns` from a
   measured ok run. Put them in a `fuzz.sh` next to `run.sh` that wraps it, sends the
   guest console to a file, away from the replay lines, and sets `UNKNOWN_MMIO=fault`:
   `run.sh`'s bring-up default answers wild accesses with 0, so no fault oracle would fire.
   **Add a memory oracle** whenever the handler parses lengths or offsets from the input:
   on flat memory an overflow faults nowhere, so without one it is an ok. On a `mem:` route
   use its redzones (`after:r`, plus `extent=@OFF:SZ` when the input carries its own
   length field, `before=N` for underflows); on a device route, `--guard` the firmware's
   own buffer (`references/harness.md`, "Redzones" and "Guards"; on 8-bit AVR and PIC both
   take the plain data address: "Per ISA"). A `--guard` is armed
   during boot: guard only what the boot does not write (`:r` otherwise), or the boot
   stops at it and never reaches the begin pc.
   **An x86 SMM handler** also wants SMM code-fetch confinement (`uam_x86_set_smm_confine`,
   `-o smm-confine=1` on the PC template and `x86_64-ref`): a call-out of SMRAM becomes a `fetch` crash at its
   first instruction; a `--guard` in SMRAM decoded through the SMM view takes
   `p:0x400000000000+ADDR`, and a processor shutdown reads as a `hang` ("Per ISA").
5. **Smoke-test with replay**: a directory of hand-made inputs whose verdicts you can
   predict (an empty input, a valid request, an oversized one, one that reaches a known bad
   path if there is one). `--fuzz-replay` must give exactly those verdicts, twice, and with
   `--jit`. A `hang` on an ordinary input usually means a device still missing on that path:
   back to ember-dev-build. To see why an input crashed or hung, replay it alone with
   `--history 64` (its last blocks, exceptions and interrupt entries) and, as needed,
   `-t mmio,exc`: each input gets its own `trace: input ID` section and summaries.
6. **Check coverage moves**: `--cov-blocks --cov-out` for two different inputs must give
   different maps; identical maps mean the input never reaches the handler (begin pc after
   the read, wrong route, input dropped by a full FIFO).
7. **Run the campaign**: the brain (`references/lists-and-brain.md`) or a program on
   `uam/fuzz.h`. Give the brain a dictionary of the handler's magic values and command
   words (`--dict`, ember's format: integers follow the target's byte order). Use every
   core: `fuzz_launch.py -n N` runs N brain + ember workers that share their queues, each
   on a different power schedule. An `UNSTABLE` line from the brain means the harness leaks
   state between iterations: back to the readiness gate. Keep inputs, crashes and hangs in
   `fuzz/` of the project.
8. **Triage**: replay the crash directory, group with `scripts/replay_summary.py`, reproduce
   each group's smallest input alone, then debug it with ember-dev-build's tools from the
   snapshot (`--load-snapshot`, `--break`, `-d`, gdb). A `crash-guard` (replay exit 25;
   to the brain it arrives as `GUARD` with the pc) is an out-of-bounds access stopped
   before it happened: its replay line names the guard and the access's offset into it
   (`read 1 at 0x80000470: +0 into guard mem-after [...]`), which says whether it is an
   off-by-one, a trusted length or an underflow (`references/harness.md`, "Reading a
   crash-guard line"). Decide for each: firmware bug, or emulation gap (a device that
   behaves unlike hardware). Emulation gaps go back to ember-dev-build as iterations.

## Journaling: `fuzz-XX.md` (required)
One per configuration change or campaign, next to `RECON.md` and the `iter-XX.md` files:
the target (function, address, why), route, boundaries and oracles (the exact `fuzz.sh`
command), the ember and emulation commits, smoke-test verdicts, campaign length and
coverage, and every crash group with its reproducer, root cause, and firmware-bug-or-
emulation-gap call.

## Guardrails
- Don't fuzz an emulation that fails the gate. Don't paper over a device gap the fuzzer
  found with an oracle or an HLE hook; fix the device (ember-dev-build).
- Every crash is reproduced from a single input file with `--fuzz-replay` before it is
  reported, interpreter and JIT.
- Oracles describe the firmware, not the emulator: a crash pc is a firmware error reporter;
  an exception class is excluded only because the firmware takes it normally (TLB refills,
  x86 software interrupts). On a core without exceptions keep `fetch,load,store`: a bus
  fault outside the policy halts the core, and the input reads as a hang.
  A redzone or guard covers only memory the firmware has no business touching: a zone the
  firmware legitimately writes (a terminator at `buf[len]`, a neighbouring object) makes
  every input a crash; narrow it (`:r`, a smaller `after=N`) and journal why.
- Keep the begin pc before the input read; a snapshot taken after it fuzzes nothing.
- Fuzzing changes nothing in ember. A harness feature ember lacks is ember work (ask the
  user), as in ember-dev-build's organization rule 4.
- Bound everything: `-n` for the boot, `--fuzz-iter-insns` per input, `timeout -k` around
  replays, stdin from `/dev/null`.
