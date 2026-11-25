# The monitor

The monitor is a live command channel into a running `uam-run`. You can look at the
machine and steer it while it runs, without restarting. It does not touch the guest's
serial line, and a run without it does exactly what it did before (`include/uam/monitor.h`
says why).

Commands so far: run control (`stop`, `cont`, `wait`, `step`, `until`, `run-for`,
`quit`), the machine's state (`status`, `cpu`, `regs`, `reg`, `map`, `history`, `bt`), its
memory (`x`, `write`, `load`, `dump`), live instrumentation (`break`, `guard`, `watch`,
`rule`, `trace`, `mmio-rule`), and the machine as a whole (`snap`, `reset`, `devices`,
`dev`, `heap`, `irq`, `cpustate`). I/O commands come in a later phase. Boards can add their
own commands (`uam_monitor_add_command`, below).

## Connecting

```
uam-run ... --monitor unix:mon.sock fw.bin     # a Unix socket (the usual way)
uam-mon mon.sock                               # a prompt; or plain socat - UNIX-CONNECT:mon.sock
uam-mon mon.sock -c "stop; status; cont"       # one shot, for scripts
```

| Way in | What it is |
|---|---|
| `--monitor unix:PATH` | a Unix socket. File permissions decide who gets in. A stale socket file at PATH is replaced; the file is removed when the run ends. |
| `--monitor tcp:PORT` | TCP on 127.0.0.1 only, with no authentication. From another host, forward it over ssh. |
| `--monitor stdio` | stdin/stdout, one line at a time, for when the guest console is somewhere else (`--console pty`, `file:`, `unix:`, `link:`). |
| `--console stdio-raw` | **Ctrl-A m** switches the terminal between the guest and the monitor. |

`--monitor` can be given up to four times, and several clients can be connected at once.
Every client sees the events.

### Guest serial on stdin/stdout

With `--console stdio-raw`, your terminal is the guest's UART. Press **Ctrl-A m** and the
terminal becomes the monitor (an `ember-mon>` prompt):

- What you type goes to the monitor, not the guest.
- The guest's output is held and written out when you switch back. Up to 1 MiB is kept;
  anything beyond that is counted and reported.
- Ctrl-A m again gives the terminal back to the guest, and so does `cont`.
- When the machine stops, or the run ends under `--monitor-hold`, the prompt comes up by
  itself.
- Ctrl-A x still quits, and Ctrl-A Ctrl-A still sends a Ctrl-A.
- The prompt edits lines with backspace, Ctrl-U (clear the line) and Ctrl-C (drop the
  line).

This works in any run whose console is `stdio-raw` on a terminal. No `--monitor` is needed.
A `--monitor unix:...` given as well is available at the same time, for a script or a
second window.

### Options

| Option | Effect |
|---|---|
| `--monitor-wait` | Stopped before the first instruction; `cont` starts it. |
| `--monitor-hold` | When the run ends (any reason), stay for inspection until `quit`. The exit status is the run's own, unless `quit N` gives one. |
| `--monitor-script FILE` | Commands to run before the first instruction, one per line. Output goes to stderr, prefixed `uam-run: monitor:`. A failing command stops the script and the run (exit 2). The machine starts afterwards, unless the script said `stop` or `--monitor-wait` is set. |

The monitor does not combine with fuzzing or with `--gdb`.

## Protocol

One command per line: `[#ID ]COMMAND [ARG...]`.

- **Arguments** are separated by blanks. `"double quotes"` keep blanks; write `\"` and `\\`
  inside them for a literal quote or backslash.
- **Replies:** the command's output lines, then one final line, `ok` or `error: WHY`. If the
  command had an `#ID`, the final line starts `#ID ` too, so a client can tell exactly
  where a reply ends.
- **Named results** are shown as `name: value` lines.
- **Events** are lines starting `! `. They arrive between replies, never inside one:
  - `! hello protocol=1 state=running` — when a client connects
  - `! stopped reason=monitor pc=0x9 icount=370118` — when the machine stops
  - `! ended reason=instruction-limit-reached status=0 pc=0x9 icount=50000000` — when the
    run ends

  Each value is one word: spaces in it are written as `-`.
- **Comments:** a line starting `# ` (a `#` that is not an id) is ignored.
- **JSON:** after `json on`, that client gets one JSON object per line:

```
{"id":"7","ok":true,"lines":["cpu 0 pc=0x9 icount=1028"],"data":{"state":"stopped","icount":"1028",...}}
{"id":"8","ok":false,"error":"unknown command 'nope' (help lists them)"}
{"event":"stopped","reason":"monitor","pc":"0x9","icount":"1028"}
```

  In `data`, every value is a string.

### When commands take effect

- **While the machine runs,** commands are read between blocks: every 1024 blocks, and
  whenever the guest idles. Commands that read or change the machine's state (registers,
  memory, `step`, `history`, `bt`, `break`, `guard`, `watch`, `rule`, `mmio-rule`) need it
  stopped and say so otherwise. `map`, `status`, `cpu`, `wait`, `trace` (but `blocks` and
  `ir`) and `help` work at any time.
- **What the monitor adds stops into the monitor.** A breakpoint, guard or rule added
  here stops the run into the monitor, where `cont` goes on. The ones uam-run was started
  with (`--break`, `--guard`, `--on`) still end the run, as without a monitor
  (`--monitor-hold` keeps it there for a look).
- **Memory and registers** are accessed the way a debugger does (`include/uam/debug.h`,
  which the gdb stub and `--dump-mem` use as well). A device region answers a read only
  through its side-effect-free read (`uam_device_ops.peek`); otherwise the command says
  why it could not read.
- **`stop`** replies only once the machine has actually stopped. Lines the same client
  sends after it wait until then, so `stop; status` always shows a stopped machine.
- **A machine stopped by the monitor** keeps its whole state. `cont` goes on as if nothing
  had happened. Under `--realtime`, pacing restarts from the host clock, so the pause is
  not made up for.
- **SIGINT** (or Ctrl-A x) ends the run, stopped or not. Exit codes are as usual (130 for
  SIGINT).

## Commands

| Command | What it does |
|---|---|
| `help [COMMAND]` | Lists the commands, or says what one does. |
| `status` | `state` (`running`, `stopped`, `ended`), `reason` (why it is stopped: `start`, `monitor`; or how the run ended, with `status`), `board`, `icount`, `time` (scheduler ticks), `cpus`, and one line per processor: `cpu N pc=0x... icount=N [waiting]`. |
| `stop` | Stops the machine at the next block boundary. The reply carries `state`, `pc` and `icount`. Before the start, it keeps the machine stopped. |
| `cont` | A stopped machine goes on. Refused once the run has ended. |
| `wait` | Replies once the machine stops (a breakpoint, a guard, a rule, another client's `stop`) or the run ends, with the same fields as `stop`. At once if it is not running. For scripts: `cont; wait; regs`. |
| `step [N]` | Runs N instructions (default 1) in all, across every processor, then stops. Replies once it has stopped (`reason: step`). Needs the machine stopped; not in a script. |
| `until PC` | Runs until a processor reaches PC again (where one stands now does not count), then stops (`reason: until`). An ARM Thumb address can be given with or without bit 0. |
| `run-for TICKS` | Runs for TICKS of virtual time (scheduler ticks, as `status` shows `time`), then stops (`reason: run-for`). A `--max-ticks` limit still ends the run. |
| `cpu [N]` | Selects the processor that `regs`, `reg`, the memory commands and `bt` look at, for this client (0 unless chosen). Shows the selection. |
| `regs` | Every register in the ISA's debugger view, as `name: 0x...`, padded to the register's width. In JSON, the members of `data`. |
| `reg NAME [VALUE]` | One register by its debugger name (`a0`, `r3`, `rax`, `pc`). With VALUE, it is set first. Registers wider than 64 bits can be read but not set. An unknown name lists the registers. |
| `x [p:]ADDR [LEN] [w1\|w2\|w4\|w8]` | Shows LEN bytes (64 by default, at most 64 KiB) as bytes with text, or as 2-, 4- or 8-byte words in the guest's byte order. ADDR is virtual, as the selected processor maps it; `p:` makes it physical. A read that stops early shows what it got, with `short: WHY`. |
| `write [p:]ADDR w1\|w2\|w4\|w8 VALUE...` | Writes the values one after another, in the guest's byte order. |
| `load [p:]ADDR FILE` | Writes a file's bytes (at most 64 MiB) to memory. |
| `dump [p:]ADDR+LEN[=FILE]` | Memory as `--dump-mem` takes it: shown (at most 64 KiB), or the raw bytes written to FILE. |
| `map` | The bus: every region with its range, kind (ram, rom, mmio, romd, alias), priority and name. Works while the machine runs. |
| `history [N]` | The last N events (all kept, by default) of the selected processor: blocks, exceptions, interrupts. Needs a history: `--history N`, or `history on`. |
| `history on [N] \| off \| clear` | Keep the last N events per processor (64 by default), keep none, or forget what is kept. |
| `bt` | The selected processor's call chain, innermost first, named from an ELF image's symbols as `--backtrace` names them. Needs `--backtrace`. |
| `break add PC \| del ID\|all \| list` | Breakpoints: the run stops into the monitor when a processor reaches PC (`reason: break ID`), and `cont` goes on from there. `list` shows each with its hits. |
| `guard add SPEC \| del ID\|all \| list` | Guards as `--guard` takes them (`[p:]ADDR[-BEFORE][+AFTER][:r\|w\|rw][,stop\|,fault\|,log][,name=NAME]`). A hit stops the run into the monitor at the access, before it happens (`reason: guard ID`). `cont` lets that one access happen: the guard is lifted for one instruction, then it is back. |
| `watch add SPEC \| del ID\|all \| list` | Watches as `--watch` takes them: every access is an event, `! watch id=ID pc=0x... what=...`, and the run goes on. |
| `rule add TEXT \| load FILE \| list \| on ID \| off ID \| del ID` | Rules as `--on` and `--on-file` take them (`include/uam/rules.h`), with the image's symbols. A rule's `stop` stops the run into the monitor (`reason: rule NAME`). `list` shows each rule's id, name, whether it is on, and its counts. A deleted rule keeps its id. Rules with `count=` or `delay=` become part of the snapshot state (as `rules-monitor`). |
| `trace [on LIST \| off LIST \| file PATH\|-]` | The `-t` traces (LIST as `-t` takes it; `off all` turns every one off): on, off, and where they go (`-` is stderr). Without arguments, which are on. `blocks` and `ir` change how code is translated and need the machine stopped; `link` can only be set when the run starts. |
| `mmio-rule RULE` | Answers an unmodelled register, as `--mmio-rule` does, from now on (above every `--mmio-rule`, below every real device). Not `input`. |
| `snap save NAME [FILE]` | Keeps the machine as it is now, in memory as NAME (and in FILE, as `--save-snapshot` writes it). |
| `snap load NAME\|FILE` | Puts the machine back to snapshot NAME, or to the snapshot in FILE (`--save-snapshot`'s files). It is stopped there (`reason: snap`); `cont` goes on. This also works after the run has ended under `--monitor-hold`: the run comes back. A snapshot no longer fits once a device (`mmio-rule`) or a rule with `count=` or `delay=` was added after it; uam-run's log says which part. |
| `snap list \| write NAME FILE \| read NAME FILE \| drop NAME` | The snapshots kept (pc, icount, time, pages), one written to a file, one read from a file (not restored), one forgotten. |
| `reset [system\|cpu]` | Resets the machine (devices and processors; RAM stays), or the processors only, as a reset the guest asks for would: the board's reset hook first. Stopped there (`reason: reset`), also after the end of the run. |
| `devices` | Every device: name, type, regions mapped, and whether it can be read without side effects (`peek`) and describe itself (`describe`). |
| `dev NAME` | One device: its type, its regions, and what it says of its state (`uam_device_ops.describe`); without that, its registers as it shows them without side effects. |
| `heap [ADDR]` | The board's guarded heaps (`include/uam/heap.h`): arena and counts. With ADDR, the block it lies in, live or freed, and where it was allocated and freed. |
| `irq [LINE 0\|1]` | Drives an interrupt input line of the selected processor, as a device would (the next device that drives it decides again). Without arguments, the lines the monitor drove. |
| `cpustate` | The selected processor's whole state as the ISA dumps it (`-d`): system and control registers too. |
| `quit [STATUS]` | Ends the run. uam-run exits with STATUS (0-123). Without STATUS: 0, or after the end of the run under `--monitor-hold`, the run's own status. |
| `json on\|off` | JSON replies and events for this client. |

## A device's description

A device model can say what state it is in for `dev NAME`: `uam_device_ops.describe` is
called with a function to call once per line of text (no newline). It must change nothing.
The 16550 UART has one:

```c
static void uart_describe(uam_device *d, void (*line)(void *ctx, const char *text), void *ctx)
{
    char t[160];
    snprintf(t, sizeof(t), "ier=0x%02x lcr=0x%02x ...", ...);
    line(ctx, t);
}
```

## A board's own commands

```c
static void press(uam_monitor_call *c, void *ctx)
{
    my_board *b = ctx;
    if (uam_monitor_argc(c) != 2) {
        uam_monitor_fail(c, "press: which button?");
        return;
    }
    ...
    uam_monitor_field(c, "button", "%s", uam_monitor_arg(c, 1));
}

uam_monitor_add_command(mon, "press", "BUTTON", "press and release a button", 0, press, b);
```

- `UAM_MONITOR_STOPPED_ONLY` refuses a command while the machine runs.
- `uam_monitor_rest(c, i)` gives the line from argument `i` on, as typed (for text with its
  own syntax).
- A name that is already taken is replaced.
- ember names no device here: a board developer decides what a board's commands do.
