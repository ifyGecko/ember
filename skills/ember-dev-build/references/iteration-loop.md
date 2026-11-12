# The iterative bring-up loop

Bring the emulation up one piece at a time: each iteration resolves exactly one blocker, then
you rebuild and run again. Never model a device speculatively; model it because the guest
just needed it. That keeps the work bounded and every step verifiable.

```
  (0) minimal board: RAM + image@base + PC=entry (+ entry regs) + catch-all windows
        |
        v
  (1) run with an instruction budget -> console (stdout) + diagnostics (stderr)
        |
        v
  (2) identify THE next blocker (first failure in time order, not the loudest)
        |
        v
  (3) explain it from the firmware (decompile/disassemble the code involved)
        |
        v
  (4) implement/extend ONE device (its own file) - or fix the DATA it needs
        |
        v
  (5) rebuild (rebuild.sh) -> re-run -> commit -> journal
        |
        +-----> back to (1)
```

## Ember is deterministic: use it
Virtual time is counted in guest instructions (plus the ticks an idle hart skips to its next
timer), harts run on one host thread, and host time is never read (unless `--realtime` or a
host network interface is attached). The same image, board and
input give **the same run, instruction for instruction**. Consequences:

- Bound runs with an **instruction budget** (`-n N`, exit 124 when used up), and wrap the
  command in `timeout -k 5 <secs>` as well. `-n` counts *instructions*; virtual time also
  advances while the hart idles in WAIT/wfi. Ember bounds that side itself:
  - `--max-ticks N` stops after N ticks of virtual time, idle included (exit 124);
  - a WAIT that timers keep skipping ahead without a single instruction retiring ends
    after `--max-stall` such events (default 4194304, about 0.1 s of host time): as a **livelock
    (exit 128)** when nothing could deliver input, else by sleeping until console or
    network input arrives;
  - virtual time never passes 2^62 ticks.
  So a run killed by `timeout` while the hart was idle was almost always waiting for host
  input: keep stdin on `/dev/null` and attach no host `--nic` for unattended runs.
- `-n` and the other numeric options are parsed as C integers (decimal, `0x...`): write
  `200000000`, not `2e8` (which means 2).
- A run is a reproducible experiment. Narrow a problem by instruction count: `-n N -d` dumps
  the hart at exactly instruction N; `--trace blocks --trace-after N --trace-range LO:HI` shows
  the blocks executed after that point (N is the absolute icount the runner prints, also
  after `--load-snapshot`; `+N` counts from the run's start); rerunning gives the identical
  trace. For only the last stretch before a stop, `--history N` is cheaper than a full block
  trace.
- Compare before/after an edit by diffing two logs from the same `-n`.
- Interpreter vs `--jit` must agree (the JIT chains blocks by default; `--no-chain` turns
  that off, and must agree too). `--jit` compiles on x86-64 and AArch64 hosts; elsewhere it
  falls back to the interpreter. The JIT is much faster for long boots, but if a run
  behaves differently with and without `--jit`, that is an ember bug: stop and report it
  with both logs instead of working around it. To narrow it for the report, `--no-chain`,
  `--no-opt` and `--block-insns 1` each change only how blocks are formed and run, and
  `--trace ir` prints what was translated.
- A run can be cut anywhere with `--save-snapshot` and resumed with `--load-snapshot`,
  bit-identically. Once the boot is long, take a snapshot past the part you are no longer
  changing and start every run from it (`snapshots.md`, section 2); re-take it
  whenever an iteration changes anything that runs before it.

## (0) The minimal board
`new-emulation.sh` generates it (`board-skeleton.md` explains every part): RAM at the
physical address from RECON, the OS placed with `--image os=` at the confirmed base, the hart
starting at the entry (registers the loader would have set come from `board_entry_regs`), a
catch-all `unimp` window over **every** RECON MMIO window, and the unknown-MMIO fallback. Fill
the constants and windows from RECON.md. Iteration 0 succeeds when the run fails for a
*specific, explainable* reason (typically: polling the console UART's status register).

## (1) Run, always bounded
`run.sh` passes `--image os=$IMAGE --unknown-mmio zero --trace exc --stats`. Console output is
stdout, everything diagnostic is stderr:

```bash
IMAGE=/path/to/os.bin timeout -k 5 300 ./run.sh -n 200000000 \
    > build/console.txt 2> build/emu.log < /dev/null
python3 scripts/log_summary.py build/emu.log
```

Size `-n` to the boot: start around 100000000 (seconds in the interpreter), raise it as the guest
gets further, add `--jit` once runs get long. Redirect stdin from `/dev/null` so console input
ends immediately: then a guest that only waits for input stops with **exit 125 (deadlock)**
or, if a timer keeps firing, **exit 128 (livelock)**, instead of waiting for you.

**Console input.** Deterministic: `--console string:'help\r'` (escapes are honoured). The
runner holds fixed input until the run starts (after the board's power-on reset and any
`--load-snapshot`) and then hands it to the UART. Most RTOS shells want `\r`. A shell that
drains its UART while booting discards input typed ahead, as on hardware: release each line
only when the guest has printed its prompt, `--console 'string:help\r,after=-> '` (the last
option; one pattern for every line), or a script of `expect TEXT` / `send TEXT` lines for a
dialogue with different prompts (`--console script:build/login.txt`; `file:OUT,script=FILE`
also captures the output). Input still unsent when the run ends is a warning (`log_summary.py`
reports it: the prompt never came). Patterns only match output of the current run: after
`--load-snapshot` at a prompt, use plain `string:` input. All of it stays instruction-exact; a
`pty` / `tcp:PORT` console typed by hand does not.

**What `log_summary.py` reads** (all printed by ember itself):

| line | source |
|---|---|
| `[warn] unimp: win+0xOFF (0xPHYS): first read/write ... pc=0x.. icount=N` | first access to each offset of a catch-all window, with the instruction that made it (decompile it) |
| `[warn] unimp:   win+0xOFF: R reads, W writes (last 0x..)` | per-window hottest offsets, printed at exit: polls |
| `[warn] unknown-mmio: first read ... at 0xADDR pc=0x.. icount=N` | an address no window or device covers (a write shows its value), and who touched it |
| `[info] mmio-stub: rule@0xADDR (fixed:0x1): first read ...` | `-v`: first access to each register an `--mmio-rule` answers; a per-rule summary at exit |
| `[error] snapshot: device X ... has no save/restore` | a device that does not take part in snapshots (`device-anatomy.md`) |
| `[error] abi: ...` (exit 2) | board and libuam built from different ember headers: `scripts/build-ember.sh` |
| `exc: kind=K (NAME) pc=.. cause=.. tval=.. icount=N` | `--trace exc`: every synchronous exception (kind 0 = `trap`, a guest trap; 2/3 = `load-fault`/`store-fault`; cause is ISA-defined: the MIPS ExcCode, RISC-V mcause, x86 vector, ...; `uam/isa/*.h`) |
| `guard NAME (stop\|fault\|log): cpu N pc .. read\|write S at ..` | a `--guard` / `--watch` hit (a `--watch` line ends with `icount=N`) |
| `[error] z80: pc=..: load from nothing at 0x..: core halted` | an AVR, 8-bit PIC, 8051, 6502 or Z80 stopped: it has no bus-error exception to take |
| `[warn] m6502: pc=..: JAM: the core stops until reset` | an NMOS 6502 executed a JAM opcode |
| `[warn] chardev: console input: N byte(s) never sent, waiting for '...'` | scripted console input whose prompt never came |
| `irq: entered handler at .. from pc=.. lines=0x.. icount=N`, `reset: system icount=N` | `--trace exc`: interrupt entries (the lines pending), device-requested resets |
| `mmio: r\|w SIZE 0xADDR dev+0xOFF = 0xVAL pc=.. icount=N` | `--trace mmio`: every access a device served (x86 ports at `0x10000000000 + port`) |
| `irq-in: cpuN line L = 0\|1 icount=..`; `irq-in: cpuN line L: R rises, F falls, level V at the stop`; `irq-in: at the stop cpuN has an interrupt pending and enabled ...` | `--trace irq`: a processor's interrupt inputs |
| `sysreg: first read\|write 0xID = 0xVAL pc=..`, `sysreg:   0xID: R reads, W writes` | `--trace sysreg`: system registers (ids as `uam/isa/<arch>.h` numbers them); a faulting access says so |
| `console: icount=N pc=.. "text"` | `--trace console`: each console line and where in the run it was printed |
| `mmio:   0xADDR dev+0xOFF: R reads (always\|last 0xV), W writes (last 0xV), first pc=..`; `mmio: most accessed: ..` | `--trace mmio` / `mmio-summary`, at the stop: each device register the firmware used; "always" on a hot register is what a polling loop keeps getting |
| `exc:   KIND cause=.. pc=..: N times ..`; `irq:   0xHANDLER: N times ..` | `--trace exc`, at the stop: exceptions by kind, cause and pc; interrupt handlers entered |
| `dma: r\|w LEN 0xADDR region+0xOFF pc=.. icount=N` | `--trace dma`: a device's DMA transfer |
| `history: cpuN: the last K events ...`, `history:   blk\|exc\|irq ...` | `--history N`: the path to the stop, oldest first |
| `uam-run: unmapped read\|write of S bytes at 0x.. by cpu N pc 0x.. (icount N)` | `--unknown-mmio stop`: the access that stopped the run (exit 131) |
| `uam-run: pc 0x.. reached (hit H, icount N) a0=0x.. ra=0x..` | `--log-pc ADDR,a0,ra`: the registers at each arrival |
| `@ICOUNT [warn] ...` | `--log-stamp`: the icount at which a log line was made |
| `<runner>: <reason> (status N) at pc=.. after N instructions` | how the run ended |

Hot offsets are only printed when the run ends normally (budget, deadlock, livelock, finisher,
SIGINT): another reason to bound runs with `-n` rather than `kill`.

## (2) Pick the blocker
Read in **time order**: `log_summary.py` lists first events top-down, then how the run ended.

| Symptom | Likely cause | Next step |
|---|---|---|
| exit 124, one window offset read millions of times | spinning on a status/busy bit | rerun with `--trace mmio --trace-mmio-range` over that register: the pc and the value it keeps reading; decompile the loop for the bit it tests, then make it read "ready"/self-clear (try `--mmio-rule ADDR=fixed:V` first) |
| exit 124, no hot offset, pc in a small range | spinning on RAM or a register that is now storage, or a delay loop calibrated against a timer that never moves | `-n N -d` / gdb: find the loop (below) |
| exit 124 "virtual time limit" (`--max-ticks`), few instructions | the hart mostly idled: a tick arrives, but too rarely or not where the guest looks | check the timer's rate (`ticks_per_count`) and which interrupt the guest enabled |
| exit 129 (guarded memory access) | a data access touched a `--guard` range; the guard line names pc, access and address | the code at that pc (who writes there, and why) |
| exit 131 (access to an unmapped address) | `--unknown-mmio stop`: the first access nothing decodes; the `unmapped read\|write` line names address, pc and icount | decompile that pc; add the window or device, or answer it with `--mmio-rule`; `-d` / `--history N` show the state and the path there |
| exit 125 (deadlock) | the guest executed WAIT and nothing can wake it: the tick timer or the interrupt it waits for is not modelled or not wired | model the timer / wire its line through the interrupt controller to the right CPU input; `--trace irq` shows whether any CPU input line ever rose, and `--trace mmio` on the interrupt controller what the guest unmasked |
| exit 128 (livelock) | WAIT while a timer keeps firing but its interrupt is never delivered (unwired line, masked in the intc, wrong Cause.IP / mip bit), or a device re-arms a timer by itself | as for exit 125: follow the timer's line to the CPU input the guest enabled |
| `core halted` in the log, then exit 125 | an AVR, 8-bit PIC, 8051, 6502 or Z80 reached an access nothing answers (`--unknown-mmio fault`, a write to ROM, a fetch where no memory is). These cores have no bus error to raise, so the hart stops; no timer is missing | the line's address is a bus address (`board-skeleton.md`, "Address spaces"): give it a device, RAM or a catch-all window |
| `JAM` in the log, or `JAMMED` / `STOPPED` in `-d`, then exit 125 | an NMOS JAM opcode or a 65C02 STP: execution went astray (wrong base, bad jump) or the model is wrong (a 65C02 opcode run on `nmos`) | the pc in Ghidra; re-check the base and `--model` |
| killed by `timeout` while idle | waiting for console/network input (stdin not `/dev/null`, a pty/tcp console, a host `--nic`) | that is the guest waiting for you: script the input, or run unattended |
| `exc: kind=0 cause=9` (MIPS Bp) early, no output | compiler divide-by-zero trap (`break 7`): a clock/divider register reads 0 | decompile the function at pc; give the register a sane reset value |
| `exc: kind=2/3` (LOAD/STORE_FAULT, MIPS DBE) with tval in an MMIO range | an access outside every window with `--unknown-mmio fault`, or a write to a ROM region | add the window, or use ROMD for flash that takes commands |
| exception storm at one vector | the vectors aren't installed yet, so the *first* exception is the real one | look at the first exception and its pc |
| exit 126 "fetch fault loop" | jumped to unmapped code, or the exception vector itself is not fetchable (BEV vectors in an unmapped boot ROM) | check memory map, BEV/EBase, reset vector |
| `unknown-mmio` accesses all over, garbage-looking addresses | wrong base or wrong RAM size | re-validate recon, don't model these |
| output stops after a banner | waiting for a timer tick or an interrupt (often then exit 125) | timer / interrupt controller |
| a subsystem prints an error and continues | that feature's device is missing or wrong | trace its failing return code (below) |
| task crash / NULL dereference | often a handle left NULL by an earlier non-fatal failure | find the *earlier* failure and fix that |
| prompt shown but input ignored | console RX is interrupt-driven | UART RX + IRQ + interrupt controller |
| `exc: kind=0 cause=10` (MIPS RI) / RISC-V illegal instruction right away | wrong base, entry, endianness, or an instruction ember lacks | re-check recon; for a real gap see `organization.md` rule 4 |
| `exc: kind=0 cause=11` (MIPS CpU) | FPU (COP1) use on a CPU model without one (`uam_mips_ref` has none), or Status.CU1 still clear | if the part has an FPU, set Config1.FP (and `fir`) in the board's CPU model (as `uam_mips_mz_ref`); a soft-float/lazy-FPU kernel handles CpU itself |
| illegal instruction early on a configurable core (MIPS16e/microMIPS/DSP, Xtensa options, AVR/PIC/8051 families) | the CPU model lacks an option the part has, or the wrong family/model | match the model to the part (`--model`, the config's option bits); a missing option's instructions trap as on hardware |
| exit 2 with `abi:` errors | the runner was linked against a libuam built from other headers | `scripts/build-ember.sh` (rebuilds both) |
| `--save-snapshot` fails: `device X has no save/restore` | a device without snapshot support | add `save`/`restore` (or `UAM_DEVICE_STATELESS`) |

## (3) Explain it from the firmware
- Decompile the function at the faulting pc or the polling site (Ghidra MCP
  `decompile_function_by_address`; without the MCP server, headless:
  `<ember-dev-recon>/scripts/ghidra-query.sh --blob fw.bin decompile 0x<pc>`, and `disasm`,
  `xrefs` the same way). For loops of register stores (bit-bang, delays) read the
  **disassembly**; the decompiler drops those stores as dead.
- To identify a register, `get_xrefs_to` the page or search the disassembly for the `lui`
  value. Every function that touches the device tells you part of its semantics.
- Find RTOS tables that encode the hardware wiring: interrupt priority/vector tables, device
  descriptor arrays `{base, irq, id}`, chip-revision compares.
- Rename the functions you understand in Ghidra (`rename_function_by_address`) and cite
  their addresses in the device file header and the journal.

**Ember's own probes** (no gdb needed):

```bash
./run.sh -n 200000000 --log-pc 0x8023f00c ...  # every arrival at a pc, with icount
./run.sh --break 0x8023f00c -d ...             # stop there, dump the hart (exit 127)
./run.sh -n 123456789 -d ...                   # the hart at exactly instruction N
./run.sh --trace blocks --trace-range 0x80230000:0x80240000 --trace-after 100000000 ...
./run.sh --map ...                             # the bus map the board built (rules included)
./run.sh --break 0x8023f00c --dump-mem 0x80100000+256 ...   # hex-dump guest memory when it stops
./run.sh --watch 0x80301234+4 ...              # log every data access to those bytes (value, pc)
./run.sh --guard 0x80301234+4:w ...            # stop BEFORE the first write there (exit 129)
./run.sh --mmio-rule 0x18040010=fixed:0x1 -v ...  # does the guest get past if this reads 1?
./run.sh --trace mmio --trace-mmio-range 0x18040000:0x18041000 ...  # one device's protocol
./run.sh --trace irq,sysreg ...                # interrupt inputs; system registers used
./run.sh --trace console --log-stamp ...       # console lines and log lines with their icount
./run.sh --snapshot-at 0x8023f00c --save-snapshot build/here.snap ...  # bookmark a state
```

**Probe register hypotheses with `--mmio-rule` before writing C.** "The driver polls
+0x10 bit 0" becomes `--mmio-rule ADDR=fixed:0x1` (or `echo,set=0x1` for a register that
also stores writes); if the guest gets past, the semantics go into the owning device in
step (4). Rules are probes: one left in `run.sh` past the iteration is journaled like an HLE
hook, and goes when the block gets a device.

**Find which call fails (or what loops) with gdb, without stopping the guest.** Ember's
runner waits for gdb before executing the first instruction; breakpoints are PC hooks and
watchpoints are bus watches (`watch *(int *)0xb8000014` traps the guest's MMIO accesses).

```bash
./run.sh --gdb 1234 > build/console-gdb.txt 2> build/emu-gdb.log < /dev/null &
gdb-multiarch -q -batch -ex 'set endian big' -ex 'target remote :1234' \
  -ex 'source scripts/gdb_trace_calls.py' \
  -ex 'trace 0x80012340 suspect_a' -ex 'trace 0x80045670 suspect_b' -ex continue > build/gdb.txt 2>&1
```

- The stub sends a target description, so gdb picks the architecture; a big-endian guest
  (MIPS BE, ARM BE8, PowerPC, AVR32, Xtensa BE) still needs `set endian big` (`run.sh` records
  `GDB_ENDIAN`). gdb-multiarch works with MIPS, RISC-V, ARM, x86, PowerPC and AVR; its
  generic Xtensa rejects ember's register set (a gdb built for that core may not), AVR32
  needs an avr32 gdb, and the 6502, 8051, PIC and Z80 descriptions name no gdb architecture.
  There, use ember's own probes (`--log-pc`, `--break -d`, `--dump-mem`, `--watch`).
- Write gdb output to a file, not a pipe. Batch sessions killed by `timeout` lose piped output.
- For a hang: interrupt or `--break` at a pc in the loop, `x/12i $pc-24`, then trace the
  callers. The repeating call and its arguments name the register being polled.
- For a value that goes wrong (a size, a pointer, a status the guest computed), set a
  watchpoint on the RAM holding it: `watch`, `rwatch` and `awatch` all work, also with
  `--jit`, and stop at the instruction that made the access - often faster than tracing
  calls. The address is translated to physical when the watchpoint is set, so set it once
  the guest's mapping of that address is final. Without gdb, `--watch ADDR+LEN` logs every
  access (pc, value) and `--guard ADDR+LEN:w` stops before the first write; both translate
  ADDR as processor 0 maps it when the run starts (`p:ADDR` for a physical address).
- Never run `pkill -f <pattern>` in the same shell command that contains the pattern; it kills
  that shell. Use `pkill -x <board>-run` or the PID.

## (4) Resolve exactly that blocker
Map the address to a row of the recon MMIO table and change the **one** device that owns it.
Start with a stub (plausible reads, logged writes) and deepen only when the guest forces it
(a TX path once it prints, a counter once it waits, a value once it compares). Every register
you add goes into the device's `save`/`restore` in the same edit.
`device-anatomy.md` has the patterns, the tested example models and the semantics traps.

**Not every blocker is a device.** Some are data the firmware expects to find: calibration
or NVRAM in flash, a board-config partition, a boot parameter block, a MAC address. When
the guest reads valid-looking storage and rejects the contents, fix the image (a generator
script under `tools/`, documented in the journal), not the device. Mark synthetic data
clearly as synthetic.

**Replacing a firmware function (HLE) is a last resort.** A PC hook
(`uam_exec_add_pc_hook`, in `<board>_hle.c`) can emulate a function: set the return
register, set `cpu->pc` to the return address. Use it only for code whose hardware is out of
scope (a radio calibration that needs real RF, a cryptographic co-processor self-test), never
to skip a device you could model. List every hook in the journal and in the board header:
each one is a place where the emulation does not behave like the hardware.

**When a blocker shows RECON.md was wrong** (a mislabeled region, a missing memory, a
requirement that didn't hold), fix RECON.md in place, tagged `*[build: iter-XX]*` with the
evidence, as well as journaling it. The fact sheet stays the single source of truth, and the
error class is worth adding to `ember-dev-recon/references/recon-pitfalls.md` if it is generic.
Before modeling a value the OS displays (counters, link state), check that the code really
reads it from hardware. Software counters that are never updated look like a missing device.

**Iteration boundaries:** an iteration ends when its blocker is gone, or when the failure
point moves into a *different* device. Commit and journal, then start the next one. Several
sub-steps inside the same device (wake-up bit, then chip revision) belong to the same
iteration.

## (5) Rebuild, re-run, commit, journal
- Edited `emulation/` files (new files are picked up automatically): `scripts/rebuild.sh`.
- Changed ember itself (rare): commit the change in `ember-src` with the message it would
  carry upstream (or `scripts/regen-patches.sh -m "..."`), run `scripts/regen-patches.sh`,
  review `patches/`, then `scripts/build-ember.sh` (organization.md, rule 4).
- ember has moved on: `scripts/repin.sh` to the latest commit, re-run, journal the move.
- Commit the emulation repo with a message naming the blocker cleared, so each iteration is
  bisectable. Because runs are deterministic, `git bisect run` with a fixed `-n` and a grep
  on the console works for regressions.
- Write `iter-XX.md` (below), then go back to (1).

## Journaling: `iter-XX.md` per iteration (required)

Write one `iter-XX.md` per iteration, numbered from 00, in the **session project directory**
(next to `RECON.md`). Never overwrite a previous one. Keep entries honest: if an iteration
ended still blocked, say so and carry the problem forward.

```markdown
# iter-XX — <short title, e.g. "model the UART TX path">

- date / commit: <date> / <emulation-repo short hash>; ember <ember.commit short hash>
- milestone targeted: <first serial byte | banner | prompt | network | ...>
- run: `./run.sh -n <budget> [...]` -> exit <code> after <N> instructions
  (from snapshot <file, pc, icount, commit> if the run started from one)

## 1. Problem faced
<what this iteration tried to achieve; where the previous one left off>

## 2. Issue found
<the concrete blocker, quoted from the console / log_summary / -d dump / gdb trace>
<the firmware code that explains it: function addresses, what they read/write/poll>

## 3. Solution chosen
<device or data change, mapped to the RECON.md MMIO row; why this over alternatives>

## 4. How it was implemented
- files touched (emulation/, tools/, patches/ if any - each patch's subject, and why the public
  API was not enough; whether it is general enough to propose upstream)
- register semantics added
- re-run result: <next blocker or milestone, with quoted output and instruction count>
- Ghidra renames/comments made; HLE hooks and `--mmio-rule`s kept (if any) and why
```

## Milestones (natural checkpoints)
1. First serial byte.
2. RTOS/kernel banner and init output.
3. Shell / console prompt that accepts input.
4. Network: link up, then packets (see `device-anatomy.md`, network).
5. Clean steady state: no guest exceptions over a long run (`-n 10000000000 --jit`), and
   interpreter and JIT agree.
6. Snapshot-exact: a run split by `--save-snapshot` / `--load-snapshot` equals the
   uninterrupted run, interpreter and JIT on either side (`snapshots.md`). This is where
   bring-up ends; fuzzing the result is the ember-fuzz skill.

## Anti-patterns
- Modeling devices the guest hasn't needed yet (you will guess the semantics wrong).
- Fixing the loudest symptom instead of the first failure.
- One giant `devices.c`, or device code in the board file (see `organization.md`).
- A device that includes `uam/machine.h` to reach the CPU or stop the run.
- A device without `save`/`restore` (snapshots then fail for the whole board), or
  one that saves too little (split runs diverge).
- Writing a device model for a hypothesis an `--mmio-rule` could have tested in one run; and
  the opposite, `--mmio-rule`s left in `run.sh` in place of a device.
- Patching ember for something the public API already does (catch-alls, per-register rules,
  PC hooks, ROMD, aliases, reset lines, snapshots all exist).
- HLE hooks that hide a device you could model.
- Chasing garbage addresses that really come from a wrong base. Re-validate recon instead.
- Unbounded runs (`-n` missing) from an automated session.
