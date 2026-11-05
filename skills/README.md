# ember skills

Agent skills for taking a raw embedded firmware image from "unknown blob" to "boots on
ember" (the UAM firmware emulator in this repository: guest code lifted into a small IR,
devices behind explicit interfaces), and then fuzzing it there. They are plain `SKILL.md`
folders with no harness-specific content, so any agent that reads the Agent Skills format
can use them; `install.sh` sets them up for Claude Code and Qwen Code. Two skills form the
pipeline every ember build goes through; the third is for later, once the emulation works:

| Skill | Job |
|---|---|
| **`ember-dev-recon`** | Triage an unknown image: container members and flash layout, architecture/endianness and **whether ember can run it**, load address of each stage (with evidence), entry state, memory map, RTOS identity, an MMIO table with per-register offsets and owning functions; on PC firmware the flash descriptor, firmware volumes, I/O ports, PCI config and MSRs. Emits `RECON.md`. |
| **`ember-dev-build`** | Write an ember board for it and bring the firmware up **iteratively**, one blocker and one device at a time, with an `iter-XX.md` journal per iteration. The project is a separate tree that links against its own local ember clone, pinned to the latest ember commit; no emulation code ever lives in the ember tree, and any change ember itself needs is a `git format-patch` file in the project, applied to the clone as a commit and ready to `git am` upstream. Every device saves its own state, so the finished board snapshots and resumes bit-identically. |
| **`ember-fuzz`** | *Later, when the emulation is functional.* Fuzz the firmware with ember's snapshot-based harness: pick a target handler and begin pc, publish the input route from the board, set crash oracles and guards, smoke-test with replay, check coverage, run a campaign (suzzer's brain), and triage crashes into firmware bugs or emulation gaps, journaled in `fuzz-XX.md`. |

They cover every ISA ember has: RV32IMAC, MIPS32r2 (big and little endian, and the PIC32
cores: MIPS16e, microMIPS, FPU, DSP), ARMv7-A (ARM and Thumb-2, LE and BE8), x86-64 PCs from
the reset vector, 32-bit PowerPC (classic and Book E), 8-bit AVR, AVR32 A/B, 8-bit PIC, the
8051, the 6502, the Z80 and Xtensa (LX106, LX6, LX7; LE and BE). `ember-dev-build` scaffolds a
project for each (`new-emulation.sh --isa ... [--model ...]`, defaults from the matching
`boards/*_ref.c`).

Everything about building, running, diagnosing and modelling follows ember's own design:
out-of-tree boards on `libuam`, `uam_device` models behind ember's layering rules,
deterministic virtual time, instruction and virtual-time budgets, per-register MMIO rules,
guarded memory, scripted console input, machine snapshots, ember's diagnostics and its gdb
stub; and, for ember-fuzz, its coverage and fuzz harness and its suzzer interop. The ember
checkout a project pins is the authority on options and APIs; the skills say where to look.

## Requirements

| for | needs |
|---|---|
| recon | python3 (stdlib only); optional `binwalk`; Ghidra for the analysis pass (headless `analyzeHeadless`, or a Ghidra MCP server for interactive work) |
| build | git, a C11 compiler, make, python3, an ember git checkout or URL; `gdb-multiarch` for debugging (see ember-dev-build for which ISAs it can drive) |
| fuzz | a working ember-dev-build project; optionally a suzzer checkout (plus rizin for its list generators) |
| tests | the build requirements; `mips-linux-gnu-gcc` (fixture firmware) and gcc with `-m32` (the PC fixture); ember's own firmware builds (`make firmware firmware-mips ...` in the ember checkout) for the bare-board runs. Optional: gdb-multiarch, OVMF, Ghidra. Parts whose tools are missing are skipped |

## Install

```bash
skills/install.sh          # symlinks the skills into every harness found: Claude Code, Qwen Code
skills/install.sh --claude # only Claude Code: $CLAUDE_SKILLS_DIR or ~/.claude/skills
skills/install.sh --qwen   # only Qwen Code: $QWEN_SKILLS_DIR, $QWEN_HOME/skills or ~/.qwen/skills
skills/install.sh --agents # the harness-neutral ~/.agents/skills ($AGENTS_SKILLS_DIR), which
                           # Qwen Code also reads: use it or --qwen, not both
skills/install.sh --uninstall [--claude|--qwen|--agents]   # remove the links it made
```
The links point into this checkout, so pulling ember updates the skills. Re-running the
script replaces links left by an earlier install (for example from the former `ember-dev`
repository) and refuses to touch a real directory of the same name. Before linking it checks
each `SKILL.md` the way the harnesses do: a `name` equal to the folder name (lowercase
letters, digits and `-`) and a `description` of 1 to 1024 characters. Restart the harness, or
run `/skills`, to see them. For another harness, point its skills directory at the three
folders or copy them.

## Test against an ember checkout

```bash
skills/tests/run-tests.sh ~/ember
```
Scaffolds a project for every ISA and model, builds each warning-free and checks the board
registers; runs ember's own self-test firmware on each bare board (each must end at the
board's console UART); runs a fixture firmware that exercises every example device (UART,
word-spaced 16550, timer, interrupt controller, reset block, NOR flash as ROMD, an HLE hook,
a guest-triggered chip reset, an `--mmio-rule`), and on the PC template the PC examples as a
Q35-class chipset from the reset vector (and OVMF to the UEFI shell when it is installed);
splits runs with `--save-snapshot` / `--load-snapshot` across the interpreter and the JIT
(every example device must save and restore its state exactly); replays fuzz inputs through
the board's named input and summarizes them with ember-fuzz's `replay_summary.py` (oracle
crashes, and crash-guard verdicts from a `--guard`); runs the recon tools on the fixture
images; and checks the guardrails (layering, uncaptured ember edits, a dirty working copy).
Re-run it whenever the ember pin moves.

## Environment variables (all optional)

| Variable | Used by | Default |
|---|---|---|
| `GHIDRA_HOME` | recon | auto-detected (`analyzeHeadless` on PATH, `~/ghidra*`, `/opt/ghidra*`) |
| `GHIDRA_PROJECTS` | recon | `~/GhidraProjects` |
| `EMU_ROOT` | build | `~/emu` (default parent of new projects when `--dir` is not given) |
| `EMBER_UPSTREAM` | build, tests | `~/ember` (clone source for a project's own ember checkout) |
| `EMBER_SRC` | build | build in this existing checkout (else `ember.src`, else `<project>/ember-src`) |
| `CLAUDE_SKILLS_DIR`, `QWEN_SKILLS_DIR` (`QWEN_HOME`), `AGENTS_SKILLS_DIR` | install | see Install |

## Layout

```
skills/
  install.sh
  ember-dev-recon/  SKILL.md
                    scripts/  recon.sh, basefind.py, mmio_scan.py, flashmap_scan.py, uefi_scan.py,
                              fwinfo.py, ghidra-query.sh,
                              ghidra/{FirmwareRecon,FindMmio,FindX86Io,Decompile,SeedEntries}.java
                    references/  arch-cheatsheet, mmio-discovery, recon-report-schema,
                                 recon-pitfalls, uefi-recon
  ember-dev-build/  SKILL.md
                    scripts/  new-emulation.sh, build-ember.sh, rebuild.sh, regen-patches.sh, repin.sh,
                              check-layering.sh, _common.sh, log_summary.py, gdb_trace_calls.py
                    templates/  board.c.in, board-pc.c.in, board.h.in, main.c.in, Makefile.in,
                                master_abort_log.{c,h}
                    references/  organization, iteration-loop, board-skeleton, device-anatomy,
                                 snapshots, pc-platform, examples/ (tested device models;
                                 examples/pc/ a Q35-class chipset)
  ember-fuzz/       SKILL.md
                    scripts/  replay_summary.py
                    references/  harness, suzzer
  tests/
    run-tests.sh    end-to-end regression against an ember checkout
    fixture/        the wired test boards, an HLE example, and the fixture firmware
```

The skills and their examples are documentation for emulation projects, never part of the
ember library: the example device models (including the PC chipset ones) show how a project
writes its own, in its own tree.
