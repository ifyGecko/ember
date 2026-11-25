# The monitor: steering a run while it runs

Every other diagnostic in `diagnostics.md` is decided before the run starts: you pick the
switches, the run goes, you read the log, you change the switches, you run again. The
**monitor** is the other way round: one run stays up, and you stop it, look at it, change
it and let it go on, as often as you like, without restarting. ember's `docs/monitor.md` (in
the pinned checkout) is the reference for every command; this page says **when it is the
right tool and how an agent drives it**.

It needs an ember pin that has it: `build/<board>-run --help` lists `--monitor` (ember
`816133b` and later). Every board runner built on `uam_run_main` has it, `run.sh` passes
it through, and the client `uam-mon` is built with ember:
`build/subprojects/ember/uam-mon` in a project, `build/uam-mon` in an ember checkout.

## When to reach for it (and when not)

| situation | without the monitor | with it |
|---|---|---|
| the boot hangs somewhere after a long run-up | `-n N -d` guesses, a rerun per guess | `stop; status; regs; history; bt` on the hung run, then `x`, `dev`, `cpustate` |
| "would it get past if this register read V?" | `--mmio-rule ADDR=fixed:V` and a rerun from the start | `mmio-rule ADDR=fixed:V; cont`, on the same run, as many tries as needed (`snap save` first, `snap load` between tries) |
| "what is in this structure / register / device right now?" | `--break PC --dump-mem`, rerun | `break add PC; cont; wait; x ADDR 64 w4; dev NAME` |
| "who writes this variable?" | `--guard ADDR+4:w` (the run ends at the hit) | `guard add ADDR+4:w; cont; wait; bt; history 16`, then `cont` to the next writer |
| a console dialog the firmware needs (login, menu) before the part you are working on | `--console script:` written up front, rerun when it is wrong | `send console "root\n"; expect console "# " 10`, interactively, then write the script from what worked |
| experiments from the same point | `--snapshot-at/--save-snapshot`, one rerun per experiment | `snap save here`; try; `snap load here`; try something else |
| a device model's state while the guest drives it | prints in the model, rebuild | `dev NAME` (its `describe` op; write one for your devices, `device-anatomy.md`) |
| two emulations on a link: what is in flight | `-t link` on both, read afterwards | `link`, `link tap on`, `link inject PORT DATA` |

**Not the right tool** for questions a whole-run trace answers better: the order of every
register access (`-t mmio`), a summary of a run (`-t mmio-summary`, `-t exc`), "does this
code ever run" (`--log-pc`). Those stay switches. The monitor is for **looking at a moment**
and **changing the run interactively**. It does not run under fuzzing (the harness refuses
it); under `--gdb` its commands are gdb's `monitor CMD` instead.

## How an agent drives it

There is no interactive terminal for an agent: start the run in the background with a
socket, and talk to it with `uam-mon -c`, one batch of commands per call.

```bash
S=build/mon.sock; M=build/subprojects/ember/uam-mon
IMAGE=fw.bin timeout -k 5 1800 ./run.sh -n 2000000000 --monitor unix:$S --monitor-hold \
    > build/console.txt 2> build/emu.log < /dev/null &
while [ ! -S $S ]; do sleep 0.1; done        # the socket is there once the run listens
$M $S -c "status"                            # running, where, how far
$M $S -c "stop; status; regs; bt"            # stop it and look
$M $S -c "cont"                              # let it go on
$M $S -c "quit"                              # end it: uam-run prints its stop line, exits
```

- **Batches run in order, one reply each.** `stop` replies once the machine has stopped,
  and the commands after it wait for that, so `stop; regs` always reads a stopped machine.
  `step N`, `until PC`, `run-for TICKS` and `wait` reply when the machine has stopped
  again. **After `cont`, put `wait` before anything that needs it stopped**:
  `break add 0x80001234; cont; wait; regs`. Without `wait`, `regs` answers "the machine is
  running" (an error; `uam-mon` then exits 1).
- **`--monitor-hold`** keeps the machine there when the run ends (a crash, a deadlock, the
  `-n` budget), stopped and inspectable; `wait` returns at the end too, with `state: ended`
  and the reason. `quit` ends it with the run's own exit status. Without `--monitor-hold`
  the run exits at its end and the socket goes away.
- **`--monitor-wait`** stops the machine before the first instruction: set breakpoints,
  guards and rules first, then `cont`.
- **`uam-mon` exit status:** 0 all ok, 1 a command failed (its `error:` is on stderr), 2 no
  monitor there (the run has not started listening, or has exited). `-j` gives JSON, one
  object per reply (`data` holds the named results), when you want to parse instead of
  read.
- Keep `timeout -k` on the run as always: a held run waits for `quit` forever otherwise.
  Never run `uam-mon` without `-c` from an agent: it then reads commands from stdin.
- A human at a terminal uses `--console stdio-raw` and **Ctrl-A m** instead (the terminal
  switches between the guest's UART and an `ember-mon>` prompt), or `uam-mon $S` in a
  second window.

## Recipes for bring-up

**Where is it stuck?** A run that ends in 124 (budget), 125 or 128 tells you little about
the moment it got stuck. Instead:

```bash
... ./run.sh -n 2000000000 --monitor unix:$S --history 64 --backtrace ... &
$M $S -c "status"          # a few times: is icount moving? the pc the same?
$M $S -c "stop; status; regs; history 32; bt; cpustate"
```

`history` shows the loop's blocks; `x ADDR` / `dev NAME` show the register it polls and what
the device model thinks; `cpustate` has the system registers (interrupt enable, pending
cause). Then test the fix live: `mmio-rule ADDR=fixed:V; cont` or
`rule add pc=LOOP,count=1,set a0=1`, and see whether `status` moves on.

**Probe registers without rebuilding.** `stop; snap save before` once; then per hypothesis
`snap load before; mmio-rule ADDR=fixed:V; cont; ...`. A snapshot does not fit any more
once an `mmio-rule` was added after it (it adds a device): to try another value, `snap save`
again after the rule, or restart. What worked goes into the board (or a journaled
`--mmio-rule`), as `diagnostics.md` says.

**Stop exactly where it matters.** `--monitor-wait`, then `break add PC; cont; wait`, or
`until PC` (runs to the next arrival at PC). `step N` steps N instructions in all; `cpu N`
picks the processor that `regs`, `reg`, `x` and `bt` look at. Breakpoints, guards and rules
added through the monitor stop the run *into the monitor* (`reason: break 1`, `guard 2`,
`rule NAME`), where `cont` goes on; `--break`, `--guard`, `--on` given on the command line
still end the run as they always did.

**Who corrupts this?** `guard add p:ADDR+4:w; cont; wait; bt; history 16; regs`. The guard
stops before the write happens; `cont` lets that one write through (the guard is lifted for
one instruction) and runs on to the next. `watch add ...` reports every access as an event
instead, without stopping.

**Bookmark a booted machine.** When the boot reaches the part you are working on,
`stop; snap save booted build/booted.snap`. Later runs start there with
`--load-snapshot build/booted.snap` (the same board and options): this is the same file
`--save-snapshot` writes.

**Drive the console.** `send console "help\n"; expect console "> " 5`; `tap console on`
streams the guest's output to your client as `! output` events. `expect` sees output that
arrived before it was issued (since your client last matched), so `send` then `expect` is
safe. Once a dialog works, write it as the run's `--console script:FILE` for later runs.

**Device state.** `devices` lists them; `dev NAME` prints what a device says about itself.
Give your device models a `describe` op (`device-anatomy.md`) from the start: it costs a
few lines and makes every later `dev NAME` an answer instead of a hex dump.

**Record what you did.** `--monitor-log build/session.log` records every command that
changed the machine with the instruction count it took effect at (`@N write ...`); give
the file to `--monitor-script` on the same command line and the run repeats exactly, the
interpreter and `--jit` alike. Put that file in the iteration's journal when a live
experiment found the answer: it is the evidence, and anyone can replay it.

## Recipes for recon, fuzzing and triage

- **Recon** (ember-dev-recon, `references/dynamic-recon.md`): on the minimal board,
  `--monitor-wait`, `until ENTRY; regs` (the entry state), `x`/`dump ADDR+LEN=FILE` (a
  structure or a stage the loader built), `map` (where windows really are),
  `trace on mmio` for a stretch, then `trace off mmio`.
- **Before a campaign** (ember-fuzz): on a plain run of the campaign's machine,
  `break add BEGIN; cont; wait` proves the boot reaches the begin pc;
  `status` there and again after `until END` gives the instructions an ordinary input
  takes (a basis for `--fuzz-iter-insns`); `input uart0 "..."; cont; expect console ...`
  shows the route delivers. A hanging smoke-test input: replay it as a plain run (the
  triage skill's step 6) and `stop; history; bt` on the hang instead of waiting for the
  budget.
- **Triage** (ember-fuzz-triage): the plain run from `begin.snap` with the input, plus
  `--monitor unix:$S --monitor-wait --monitor-hold`: `guard add` on the buffer, `cont;
  wait; bt; history 32; regs`, and after the crash the machine is still there. `snap save`
  / `snap load` go back to a point within the session; `--monitor-log` makes the session
  part of the report.
- **gdb** (`--gdb`): gdb runs the machine, and `monitor help`, `monitor dev NAME`,
  `monitor snap save NAME`, `monitor rule add ...` reach the monitor from gdb's prompt. Run
  control (`stop`, `step`, `break`, `guard`...) stays gdb's.

## What to keep in mind

- **Commands that read or change the machine need it stopped** (`stop` first): registers,
  memory, `step`, `snap`, `break`, `guard`, `rule`, `mmio-rule`, `input`, `dev`.
  `status`, `map`, `wait`, `send`, `link`, `trace` (but `blocks`, `ir`) work while it runs.
  Commands sent while it runs take effect between blocks, not at an exact instruction:
  for exactness use `break`, `until` or `step`.
- **The monitor does not change a run you do not touch.** A run with `--monitor` and no
  commands is the same run. Every change you make is real: after a `write` or `reg`, what
  follows is no longer the firmware's own run (record it with `--monitor-log`).
- **Snapshots versus live additions.** A snapshot no longer fits after a device
  (`mmio-rule`) or a rule with `count=`/`delay=` was added since it was taken: `snap load`
  refuses, and uam-run's log says which part. Take snapshots after such additions.
- **Not in a script:** `--monitor-script` lines run before the start (or at `@N`), so the
  commands that wait for the machine (`step`, `until`, `run-for`, `wait`, `expect`, `tap`)
  are refused there.
- A run paced to the host clock (`--realtime`, a host network) does not replay the same
  from a `--monitor-log`.
- Under `--gdb`, after `monitor reg`, `monitor snap load` or `monitor reset`, gdb still
  shows the old registers until `maintenance flush register-cache`.
