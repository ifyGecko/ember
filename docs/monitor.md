# The monitor

The monitor is a live command channel into a running `uam-run`. You can look at the
machine and steer it while it runs, without restarting. It does not touch the guest's
serial line, and a run without it does exactly what it did before (`include/uam/monitor.h`
says why).

This page covers the first set of commands: run control (`stop`, `cont`, `quit`),
`status`, `help` and `json`. Inspection, live rules and traces, snapshots and I/O commands
come in later phases. Boards can add their own commands
(`uam_monitor_add_command`, below).

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
  whenever the guest idles.
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
| `quit [STATUS]` | Ends the run. uam-run exits with STATUS (0-123). Without STATUS: 0, or after the end of the run under `--monitor-hold`, the run's own status. |
| `json on\|off` | JSON replies and events for this client. |

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
