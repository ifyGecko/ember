# Dynamic recon: settle a fact by running the firmware

Recon reads the image. Some facts are far cheaper to observe than to derive: where a
loader puts the stage it unpacks, what the entry receives in its registers, which
registers a boot path really touches, who reads a flash partition. Ember runs the image
deterministically and can report all of these, so when reading is the hard route, run it.

Static work comes first and stays the base of the fact sheet: the architecture, the
*Ember support* verdict, the container and the reset-stage base do not need a run, and a
run needs them. Use a run to **confirm** a static claim, to **fill** a gap static
analysis left (a computed address, a decompressor not worth reversing), or to **choose**
between two readings.

Every option named here is explained, with its output format, in ember-dev-build's
`references/diagnostics.md` (`../ember-dev-build/references/diagnostics.md` from this
skill). Read its "Who touches this address?" table before anything else.

## What to run it on

1. **The minimal board of the build phase (preferred).** ember-dev-build's
   `scripts/new-emulation.sh <board> --dir . --isa ISA [--model M] --ram-base A
   --ram-size N --image-base A --entry A`, then `scripts/build-ember.sh`, gives a board
   with RAM, the image at its base, the entry, and catch-all windows, in a few minutes.
   It is the board the build phase continues with, so nothing is thrown away. Run it as
   `IMAGE=stage.bin ./run.sh ...` (a PC: `FLASH=bios.bin`).
2. **A stock reference board, when the target fits it.** `uam-run -b <arch>-ref` (ember's
   own `build/uam-run`; `meson setup build && meson compile -C build` in the ember
   checkout, `../../` from this skill) loads an ELF by its segments, or a raw image with
   `--raw-base ADDR --entry PC`, and `--ram SIZE` sets the RAM size. Its RAM base and its
   devices are fixed: print them with `uam-run -b <arch>-ref --map -n 1`. Use it only when
   the image's addresses fall inside that RAM. An access that lands on the reference
   board's own UART or finisher meets that device, not the target's hardware.

Either way, bound every run and keep the streams apart:

```bash
timeout -k 5 120 ./run.sh -n 50000000 [options] > console.txt 2> emu.log < /dev/null
```

Runs repeat exactly: an icount printed by one run can be given to the next (`-n N -d`
stops there, `--trace-after N` starts tracing there).

## Which fact, which run

| RECON fact | run | read |
|---|---|---|
| **A stage the loader unpacks or copies** (compressed OS, relocated bootloader) | destination known from the loader's code: `-t blocks --trace-range DST:DST+LEN` shows the first block executed there (the entry, and its icount); then `--break ENTRY --dump-mem p:DST+LEN=stage.bin` | `stage.bin` is the unpacked stage at its run address: import it into Ghidra at DST. No vendor decompressor to reimplement |
| the same, destination unknown | `--log-pc JUMP,REG` on the loader's final jump (from Ghidra) with the register it jumps through; or `--history 32` at the stop of a short run | the jump target is the base and entry of the next stage |
| **Base of a stage**, as a cross-check | run the stage at the candidate base | a right base runs until the first device poll; a wrong one takes an illegal instruction or a fault within a few instructions, or scatters `unknown-mmio` lines over garbage addresses. Supporting evidence only: the paired-hit rule still decides |
| **Entry state** (what the loader hands over) | `--break ENTRY -d` on a run that starts at the loader | the registers at the entry: stack, boot-type argument, pointers to a parameter block; `--dump-mem ADDR+LEN` for the block itself |
| **MMIO table: which registers the boot path uses** | `-t mmio-summary` (run.sh already passes `--unknown-mmio zero`) | first-access lines with the pc of each (`unimp:`, `unknown-mmio:`), the hottest offsets at exit, one summary line per register. Addresses the code computes, which the static scan cannot see, appear here |
| **Polling loops and what ends them** | the same; then `--mmio-rule ADDR=fixed:V` with the value the loop's code tests for | a register read many times, "always 0xV", is a poll; if the run gets past with the rule, the poll row is confirmed by the run |
| **CPU identity and system registers** | `-t sysreg` | the first read and write of each register with its value and pc: PRId, MIDR, PVR, MSRs, cache and MMU setup; an access that faults is a register the CPU model lacks (a finding for *Ember support*) |
| **Who reads a flash partition or a table** (naming partitions from their readers) | `--watch p:ADDR+LEN:r` on the bus address of the data | each reader's pc: decompile those. Only data reads are listed, not code fetched from there, which tells data from XIP code |
| **Who writes a RAM location** (a boot marker, a self-reset magic, a handle) | `--watch p:ADDR+LEN:w`, or `--guard p:ADDR+LEN:w` to stop before the first one | the writer's pc and the value |
| **Does this code run on the boot path** | `--log-pc ADDR`, or `-t blocks --trace-range LO:HI --trace-file blocks.txt` for a region | arrivals with their icount; the blocks executed in order |
| **RAM size the image assumes** | a run with the candidate size | `unknown-mmio` accesses just past the end of RAM: the size is too small |
| **PC firmware: ports, MSRs, PCI config actually used** | the `--isa x86_64` template: its catch-alls log every unmodelled port; `-t mmio-summary`; `-o msr-trace=1` | the port, PCI and MSR tables of the phases the run reached, to set against `FindX86Io`'s |

## Looking at one moment: the monitor

The runs above decide what to report before they start. When the question is about a
moment you cannot name in advance, or needs several looks at the same point, keep one run
up and ask it: the monitor (ember-dev-build's `references/monitor.md`; it needs an ember
with `--monitor` in `--help`). Start it stopped, and drive it with `uam-mon`
(`build/subprojects/ember/uam-mon` in a project, `build/uam-mon` in an ember checkout):

```bash
./run.sh -n 200000000 --monitor unix:mon.sock --monitor-wait --monitor-hold \
    > console.txt 2> emu.log < /dev/null &
while [ ! -S mon.sock ]; do sleep 0.1; done
uam-mon mon.sock -c "until 0x80010000; regs"            # entry state, without a rerun per guess
uam-mon mon.sock -c "x 0x80400000 256 w4; dump p:0x400000+64K=stage.bin"   # what the loader built
uam-mon mon.sock -c "map; trace on mmio; step 200000; trace off mmio"      # windows; a stretch of MMIO
uam-mon mon.sock -c "mmio-rule 0x1f000010=fixed:0x1; cont; wait; status"   # past a poll, then where
uam-mon mon.sock -c "quit"
```

| fact | with the monitor |
|---|---|
| entry state when the entry is reached late, or by a path you have not mapped | `until ENTRY; regs; cpustate` (`cpustate`: system registers too) |
| a structure or stage the firmware builds in RAM | `break add PC; cont; wait; x ADDR LEN w4`, then `dump ADDR+LEN=FILE` for Ghidra |
| which poll a bare board is stuck in, and what gets it past | `stop; history 32` (with `--history 64` on the run), `x` of the register, then `mmio-rule ADDR=fixed:V; cont` on the same run |
| the console dialog a stage needs before it shows the next stage | `send console "...\n"; expect console "PROMPT" 10` |

Record a monitor finding like any run: the command line, the commands (`--monitor-log
FILE` writes them with the icount each took effect at, and `--monitor-script FILE` replays
them exactly), and the quoted reply.

Addresses in every line are ember's: bus byte addresses for code on AVR and PIC (twice
Ghidra's word address), data spaces at their bus bases (`arch-cheatsheet.md`).

## How far to go

A bare board has no devices. The loader may wait on a PLL lock bit or a flash controller
before it does what you came to see. A probe rule gets it past: `--mmio-rule
ADDR=fixed:V` (or `echo`, `set=MASK`). Record each rule in RECON.md with the code that
justifies the value: it is already a finding for the MMIO table.

Stop when the question is answered, or when getting further needs a device model rather
than a rule. Modelling devices is the build phase: hand over there, with the run's
command line and where it stopped.

## What a run does not prove

- **One path.** A run shows what executed with this image, this board and these answers.
  A register that was not touched may be touched on another path (an error branch, a
  different boot mode, a later stage). Absence in a run is not absence in the firmware.
- **Zero steers the path.** Unmodelled registers read as 0. After the first branch that
  depends on such a read, the path may be the firmware's failure path. Facts gathered
  before that point are solid; after it, say which reads the path depended on.
- **A rule is a hypothesis.** "Gets past with `fixed:0x1`" shows that value is accepted,
  not that hardware returns it.

## Recording it

Tag the claim **confirmed (run)** and give the evidence the way static evidence is
given: the command line (board, `-n`, options, rules), the exit status and icount from
the stop line, and the quoted line that shows the fact (for a monitor session, its
`--monitor-log` file and the reply that shows it). List the commands under
*Tooling notes* so the build phase can repeat them.
