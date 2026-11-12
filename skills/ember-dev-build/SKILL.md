---
name: ember-dev-build
description: "Build an ember (UAM) board for a firmware image and bring it up, iteratively. Use to emulate/boot firmware with ember (uam-run, libuam), write an ember board or device model (UART, timer, interrupt controller, flash; PC chipset: LPC, SMI, SPI, SMBus, Super I/O), boot UEFI/BIOS on an x86-64 PC board, answer unmodelled registers (--mmio-rule), hook a firmware function, run -o cpus=N, make a board snapshot-capable, or debug a stalled bring-up (exit 124/125/126/128/129/131, livelock, triple fault, abi error). ISAs: MIPS32 BE/LE, RV32IMAC, ARMv7-A LE/BE8, x86-64, PowerPC (classic, Book E), AVR, AVR32, PIC, PIC32, 8051, 6502, Z80, Xtensa LE/BE. Input: RECON.md from ember-dev-recon. Keeps emulation code out of ember (pinned out-of-tree library, one file per device, a wiring-only board); one blocker per iteration, journaled. Triggers: 'emulate this firmware with ember', 'boot this BIOS/UEFI on ember', 'write an ember board/device', 'it hangs in ember'. Fuzzing a working board: ember-fuzz."
---

# ember-dev-build: iterative firmware bring-up on ember

Input: `RECON.md` from **ember-dev-recon** (arch and the *Ember support* verdict, confirmed
base, entry and entry state, memory map, ranked MMIO table with register offsets; for a PC,
the flash layout, boot phases and modules, the platform identity and the port / PCI / MSR
tables). Output: an ember board that boots the firmware further and further, plus a journal
(`iter-XX.md`) explaining every step.

Two things make this work: the **organization discipline** (non-negotiable) and the
**iteration loop** (how progress is made). Read `references/organization.md` and
`references/iteration-loop.md` before writing code, and `references/diagnostics.md` before
the first run: every log, trace and probe ember has, what its lines mean, and which one
answers which bring-up question. Use `references/board-skeleton.md`
and `references/device-anatomy.md` (with its tested models in `references/examples/`) while
writing it. **For a PC (x86-64: UEFI, coreboot or a legacy BIOS) also read
`references/pc-platform.md`** before the first run: the PC template, the chipset layer, the
boot phases and their typical blockers, and the tested chipset examples in
`references/examples/pc/`. `references/snapshots.md` covers what a board owes ember's
snapshots and using them to skip a long boot. Once the emulation is functional, fuzzing it
is a separate step: the **ember-fuzz** skill.

Script paths are relative to this skill's directory, `skills/ember-dev-build/` in the ember
repository (`skills/install.sh` links it into the agent's skills directory). Requirements:
git, a C11 compiler, make, python3; gdb-multiarch (or the target's own gdb) for debugging; a
cross compiler only for your own test firmware. Ember itself needs libc and libm, for its
floating-point models (the project Makefile links `-lm`).

## Before you start: tools must work
If a tool the task depends on is unavailable or misbehaving, **stop and tell the user instead
of working around it**:
- RECON's *Ember support* verdict says the target needs an ISA, mode or extension ember lacks:
  that is ember ISA work, not a board. Ember has (CPU models selected by the board,
  `include/uam/isa/*.h`): MIPS32r2 BE/LE (TLB or fixed mapping; MIPS16e, microMIPS, FPU, DSP
  ASE r1/r2, shadow sets, EIC as the model configures; no 64-bit), RV32IMAC M-mode (no S/U-mode,
  F/D, 64-bit), ARMv7-A ARM/Thumb-2 LE and BE8 (no VFP/NEON, security/virtualization
  extensions, LPAE, BE32, no ARMv7-M: a Cortex-M image is out of scope; no AArch64), x86-64
  (real, protected, virtual-8086, long mode, SMM, x87/MMX/SSE/SSE2; no AVX, no VMX/SVM, no
  TXT/SGX), 32-bit PowerPC with the FPU, classic (BATs, hashed page table) or Book E (no
  AltiVec, SPE, 40x/8xx), 8-bit AVR (AVRe, AVRe+, AVRxm, AVRxt, AVRrc), AVR32A/B, 8-bit PIC
  (baseline, mid-range, enhanced, PIC18; no PIC24/dsPIC), 8051 (8051, 8052, CIP-51-style), 6502
  (NMOS, 2A03, 65C02; no 65816), Z80 (NMOS, CMOS) and Xtensa LX106/LX6/LX7 LE and BE (no
  TIE instructions, one privilege ring). Check `include/uam/isa/` of the pinned checkout, in
  case more were added;
- the Ghidra MCP server is down, or has the wrong program open (`get_program_info` shows a
  different name, base or hash). The MCP tools cannot open or switch programs. Say so once
  and use headless Ghidra instead: ember-dev-recon's `scripts/ghidra-query.sh` (decompile,
  disasm, xrefs, functions, strings, info on the project `recon.sh` made; read-only, so keep
  names in RECON.md). Ask the user only when that fails too (no Ghidra install, no project);
- the ember checkout holds someone else's work (`build-ember.sh` refuses);
- ember misbehaves: an internal error (exit 126 "internal error"), or the interpreter and
  `--jit` disagree on the same run. Report it with both logs; don't paper over it in a board.

## Setup (once per target)

```bash
scripts/new-emulation.sh <board> --dir <project-dir> --recon RECON.md \
    --isa ISA [--model M] [--ram-base A --ram-size N --image-base A --entry A] \
    [--ember-upstream <url|path> | --ember-src <dedicated clone>] [--ember-commit <sha>]
```
- `--dir .` scaffolds into the current session directory (next to `RECON.md`). Existing files
  are never overwritten.
- `--isa`: `mips32be mips32le rv32 arm armeb x86_64 ppc avr avr32 pic pic32 mcs51 m6502 z80
  xtensa xtensaeb`; `--model` picks the reference CPU model of a family (`ppc`: classic,
  booke; `avr`: eplus, e, xm, xt, rc; `avr32`: a, b; `pic`: pic18, baseline, mid, enhanced;
  `pic32`: mx, mm, mz; `mcs51`: 8052, 8051, cip51; `m6502`: nmos, 2a03, 65c02, r65c02; `z80`:
  nmos, cmos; `xtensa*`: lx6, lx106, lx7). The memory map, reset vector, CPU model call and
  interrupt example follow ember's `boards/<isa>_ref.c` (table in `references/board-skeleton.md`).
- It generates a **minimal board that already builds and runs** (`emulation/<board>_board.c`,
  `main.c`, `Makefile`, `run.sh`), with the RECON numbers you pass filled in. `--isa x86_64`
  generates a **PC board** instead (`templates/board-pc.c.in`): processors with local APICs,
  8259/8254/RTC/HPET/8042/COM1/POST/port 92h, PCI configuration at 0xCF8, the boot flash
  ending at 4 GiB (`FLASH=bios.bin ./run.sh`, or `CODE=` + `VARS=` for an OVMF split),
  catch-alls over every unmodelled I/O port; the chipset is what you add
  (`references/pc-platform.md`).
- **Pin and clone (rule):** the project builds against its own local clone of ember
  (`<project>/ember-src`, from `--ember-upstream`, `$EMBER_UPSTREAM` or `~/ember`), pinned to
  the **latest ember commit** (upstream's HEAD, the default; `--ember-commit` for anything
  else, with the reason in the README). `--ember-src` only names another clone dedicated to
  the project, never an ember working copy. When ember moves on, `scripts/repin.sh` moves the
  pin to the new latest commit and carries the patches along.
- **Every change to ember is a local git patch file (rule):** if the emulation needs ember
  itself changed, the change is committed in the clone (one commit per change, with the
  message it would carry upstream) and exported by `scripts/regen-patches.sh` as a
  `git format-patch` file in `patches/`, which `build-ember.sh` applies to the clone at the pin
  as a commit. Nothing else changes ember. A change general enough for ember then lands
  upstream with `git am` of that file (`references/organization.md`, rules 2 and 4).
- The build helpers are **copied into `<project>/scripts/`**, so the project builds without
  this skill.

```bash
scripts/build-ember.sh      # full: guard -> clone at the pin -> patches as commits -> libuam (out of tree) -> build/<board>-run
scripts/rebuild.sh          # fast: after editing emulation/ files (layering checked, new files picked up)
scripts/regen-patches.sh    # after committing a change to ember in ember-src: export patches/ (rule 4)
scripts/repin.sh            # move to the latest ember commit, rebasing patches/ onto it
```
Before writing code against a new pin, run the API checklist in `references/board-skeleton.md`
and, when the pin moved, the skills' end-to-end test against it:
`skills/tests/run-tests.sh <ember checkout>` (from the ember repository holding this skill).

## The iteration loop (authority: `references/iteration-loop.md`)

0. **Minimal board**: fill the generated board from RECON: RAM at the right *physical*
   address, the image at the recon base (`--image os=`), entry and entry registers
   (`board_entry_regs`), the CPU identity the firmware checks (`uam_<isa>_config` in the
   generated board: MIPS PRId/Config, ARM MIDR, PowerPC PVR, the family model of a
   microcontroller; x86 `uam_x86_cpuid_info`), and a catch-all `unimp` window over every
   recon MMIO window. The console UART is the first real device. A PC starts at the reset
   vector from its flash image instead; its first blockers are chipset registers
   (`references/pc-platform.md`, "The first runs").
1. **Run** bounded by instructions *and* wall time:
   `IMAGE=fw.bin timeout -k 5 300 ./run.sh -n 200000000 > build/console.txt 2> build/emu.log < /dev/null`
   (a PC: `FLASH=bios.bin ...`), then `python3 scripts/log_summary.py build/emu.log`. Runs
   are deterministic: the same inputs give the same run, instruction for instruction, also
   with several processors.
2. **Pick the single next blocker**: the first failure in time order, not the loudest line.
   Exit codes help: 124 budget or `--max-ticks` used (look at hot offsets), 125 deadlock (a
   timer/interrupt not modelled or wired), 128 livelock (a timer fires but its interrupt
   never reaches the CPU), 126 fetch-fault loop, internal error, or a reset nothing handles,
   127 breakpoint or debug trap, 129 a `--guard` hit, 130 interrupted, 131 an unmapped access
   under `--unknown-mmio stop`, 2 setup error (`abi:` =
   rebuild with `build-ember.sh`); any other status is the firmware's own, through a finisher.
   An AVR, 8-bit PIC, 8051, 6502 or Z80 has no bus error: a `core halted` line before a 125
   names the address nothing answered, and no timer is missing. On
   x86 an `x86: ... hart shut down` line means the processor shut down (the run then ends
   idle, 125 or 128): after a triple fault the *first* exception before it is the blocker;
   it also follows a machine check (SMM code fetch outside SMRAM) or an RSM with an
   invalid saved state, which the line names.
3. **Explain it from the firmware**: decompile/disassemble the code at the fault or polling
   site with Ghidra MCP (or, without it, ember-dev-recon's `scripts/ghidra-query.sh`
   `decompile`/`disasm`). Ember's diagnostics (`references/diagnostics.md`, all off until
   asked for, all stamped with the icount so lines from different traces form one timeline)
   say what the firmware expects of the hardware. Pick by question:
   - *what does it want next?* `--unknown-mmio stop --history 32 -d`: the run stops at the
     first register nothing models (exit 131), naming the pc and the path there; or
     `--unknown-mmio zero` to survey all first accesses of a run at once;
   - *which registers does it use, which does it poll?* `-t mmio-summary` (one line per
     register at the stop; "reads always 0xV" on a hot one is what a loop keeps getting);
   - *what protocol does a driver speak?* `-t mmio --trace-mmio-range BASE:END`
     (`--trace-file F` for a long one): order, values, pcs;
   - *interrupts?* `-t irq,exc` (did the line rise, is one pending and enabled at the stop,
     which handlers run); *exceptions?* `-t exc` (its summary: first icount per kind, cause
     and pc); *how did it get here?* `--history N`;
   - *system registers, CPU identity?* `-t sysreg`; *DMA?* `-t dma`; *where was a console
     line printed?* `-t console`;
   - *which call fails, what does a loop check?* `--log-pc ADDR,a0,a1,ra`, `--break ADDR -d`,
     `-n N -d`, `--dump-mem`, gdb (`--gdb`, `scripts/gdb_trace_calls.py`); *who writes this
     memory?* `--watch` / `--guard ADDR+LEN:w`.
   Turn on one or two for the question at hand, not `-t all` on a long boot; narrow with
   `--trace-mmio-range`, `--trace-after N` and `--trace-range`. Test a register hypothesis
   with `--mmio-rule ADDR=fixed:V` before writing C (an x86 I/O port is
   `0x10000000000 + port`). Once the boot is long, start runs from a snapshot
   (`--snapshot-at PC --save-snapshot`, then `--load-snapshot`) and aim `--trace-after` at
   the stretch you are working on.
4. **Resolve exactly that blocker** in the **one** device that owns it (its own file, device
   layering only), or in the data it reads (flash/NVRAM/SPD/CMOS content, generated by a
   `tools/` script). Start with a stub; deepen only when the guest forces it, and keep the
   device's `save`/`restore` in step with its registers. A PC-hook replacement
   (`<board>_hle.c`) is a last resort, journaled.
5. **Rebuild, re-run, commit the project, write `iter-XX.md`.** An iteration ends when its
   blocker is gone or the failure moves into a different device.

Milestones (embedded): first serial byte → OS init output → interactive prompt → network
(link, then packets) → no guest exceptions over a long run, interpreter and JIT agreeing →
snapshot-exact (a split run equals the uninterrupted one). That last one is also the gate
for ember-fuzz. A PC has its own ladder (POST codes → SEC console output → memory
initialized → DXE → SMM → BDS/UEFI shell → OS loader; `references/pc-platform.md`) and the
same last two milestones.

## Journaling: one `iter-XX.md` per iteration (required)

Starting at iteration 00, write `iter-XX.md` in the session project directory (next to
`RECON.md`) with four parts: **Problem faced**, **Issue found** (quoted console/log_summary/
gdb evidence and the firmware functions that explain it), **Solution chosen** (mapped to the
RECON MMIO/port/PCI/MSR row, and why), and **How it was implemented** (files, register
semantics, re-run result with exit code and instruction count, commit hash, Ghidra renames,
HLE hooks and `--mmio-rule`s kept). Use the template in `references/iteration-loop.md`.
`RECON.md` plus the `iter-XX.md` files are the durable record of the effort.

## Guardrails
- Never change ember except through `patches/`: no edits in ember's own checkout, no
  uncommitted or unexported changes left in the clone (`rebuild.sh` refuses). Each patch is
  one commit with an upstream-quality message, so a general fix is `git am`-ready.
- Build against the project's own clone pinned to the latest ember commit; re-pin with
  `repin.sh` rather than drifting behind.
- One device per file. The board file only wires. Devices include only ember's device-side
  headers; what they need from the machine comes in as parameters (`check-layering.sh`).
  A chipset device that moves decode windows receives the `uam_bus *` and the regions it owns.
- Every device implements `save`/`restore` (or is `UAM_DEVICE_STATELESS`) from the day it is
  written; one device without it breaks snapshots for the whole board.
- `--mmio-rule`s are probes. One that outlives its iteration is journaled like an HLE hook
  and replaced by a device when the block gets one.
- Model the minimum that unblocks the guest; a logging stub is a valid milestone.
- Label what isn't real: synthetic calibration/NVRAM/SPD/CMOS data, values chosen rather than
  observed (a CPUID signature, a chipset revision), every HLE hook.
- Never read host time in a device; derive everything from `uam_sched_now()` (a board's
  `tick_hz` and `uam/dev/clock.h` convert to a device clock) so runs stay deterministic. Arm
  timers only when the guest programs them.
- Rename understood functions in Ghidra as you go, and cite addresses in code comments and
  journals.
- Run every emulator invocation with `-n` and `timeout -k`, stdin from `/dev/null`; never
  `pkill -f` a pattern that appears in your own command line. Long PC boots: `--jit`, and
  `TRACE=` once timer interrupts run (`--trace exc` logs every interrupt entry), or move it
  to a file with `--trace-file build/trace.txt` (its `exc:` summary is at the end).
