# Links: connecting your board to another emulation

Use this when the firmware talks to **another device**: a second MCU over a serial line, a
field bus with several nodes, a radio, a host controller, a handful of wires. Ember's
**links** carry timestamped messages between emulations, through a **hub**, with a choice
of how time is kept. Ember knows no protocol: your device model gives the messages their
meaning. The full guide is `docs/link.md` in the ember checkout; this page is what a board
developer needs.

Do not reach for `--nic udp:` unless the channel really is Ethernet with frames of 60 bytes
or more and timing does not matter: it pads short frames, has one fixed peer, and keeps no
time.

## 1. Which way to use a port

`--link PORT=HUB[,bus=BUS][,latency=TIME]` on the command line creates a port. `HUB` is
`unix:PATH`, `tcp:HOST:PORT`, or `none` (not connected: a test, a fuzz input). Then:

| the firmware's channel | what to do | board code |
|---|---|---|
| a UART you already model (ember's 16550, or yours on a `uam_chardev`) | `--console link:PORT` | none |
| a NIC you already model on a `uam_netdev` | `--nic NAME=link:PORT` (frames keep their exact length) | none |
| anything else: a bus controller, a radio, an SPI/I2C block, GPIO lines | a device model that takes the port | below |

A board has **one** console; a second UART that should talk over a link is "anything
else": give that UART model a link port instead of a chardev, or a second chardev made
with `uam_chardev_link_create(port)`.

## 2. A device on a link

The board looks the port up by a name of its own and hands it to the device; the device
includes only `uam/link.h` (allowed for devices, `check-layering.sh` knows it).

```c
/* board.c: --link bus0=...  ->  the controller's port (NULL: not given, nothing connected) */
uam_link_port *port = uam_board_config_link(cfg, "bus0");
uam_device *ctl = my_ctl_create("ctl0", &m->sched, port);

/* my_ctl.c */
static bool ctl_can_receive(void *ctx) { return ((my_ctl *)ctx)->rx_count < RX_DEPTH; }

static void ctl_receive(void *ctx, const uam_link_msg *m)
{
    my_ctl *s = ctx;
    if (m->flags & UAM_LINK_F_LOST) {       /* our own frame back: it lost the bus */
        s->status |= ST_ARB_LOST;
    } else {
        rx_push(s, m->key, m->data, m->len);    /* key: whatever the medium arbitrates on */
        s->status |= ST_RX_READY;
    }
    uam_signal_set(&s->irq, irq_level(s));
}

/* create: */  if (port) uam_link_port_set_frontend(port, ctl_can_receive, ctl_receive, NULL, s);
/* guest wrote the transmit register: */
uam_link_send(s->port, identifier, 0, UAM_LINK_DATA, s->tx_buf, s->tx_len);
/* guest read the receive FIFO, there is room again (also at the end of restore): */
uam_link_accept_input(s->port);
/* destroy: */ if (s->port) uam_link_port_set_frontend(s->port, NULL, NULL, NULL, NULL);
```

- A message has a `key` (64 bits: an identifier, a channel, a chip select), `flags` (bits
  16-31 are yours, `1u << UAM_LINK_F_MODEL_SHIFT` up; bits 0-15 are ember's), a `kind`
  (`UAM_LINK_DATA`, or `UAM_LINK_LEVEL` for a line: key = line number, `data[0]` = level)
  and a payload at its exact length (0 to 16 MiB).
- `can_receive` false is never a loss: the message waits in the port. Call
  `uam_link_accept_input` whenever room appears. If your model is not told when it has
  room, `uam_link_port_retry(port, ticks)` looks again later, at a fixed guest time.
- `uam_link_send` returns false when nothing is connected; model that as the real part
  would behave with nothing on the wire (no acknowledge, a timeout), not as an error.
- **Snapshots.** What the device has taken (its FIFO) is the device's own state; what still
  waits in the port is saved by ember. A port must exist before the first snapshot, and a
  snapshot only restores on a board with the same ports.
- Do not send from inside `receive` in a way that cannot terminate; an answer sent from
  `receive` is fine.
- **Time a transmission takes** (a byte time, a frame time) is the model's: arm a timer
  and raise "transmit done" then, as on any device. The link's `latency` is only how long
  the message is on the wire.

## 3. Running two emulations

```bash
build/subprojects/ember/uam-link-hub unix:/tmp/hub.sock --nodes 2 &   # built with the project, as part of ember
build/<board>-run --link bus0=unix:/tmp/hub.sock --link-name ecu1 fw1.bin &
build/<board>-run --link bus0=unix:/tmp/hub.sock --link-name ecu2 fw2.bin
```

The two emulations may be the same board with different firmware, two different board
projects, or one of yours and one of ember's own (`uam-run`); all that has to match is the
hub and the bus name.

- Ports that name the same **bus** on the same hub are connected. The bus defaults to the
  port's name, so two boards that both call their port `bus0` meet without `bus=`.
- Every emulation needs its own `--link-name` on a hub.
- `--nodes N` holds every emulation at the hub until N have connected. Without it each
  starts as it connects, and what the first one sends before the second is there goes
  nowhere (a cable not plugged in yet): firmware that sends once at boot and then waits
  hangs.
- A bus is broadcast unless the hub is told otherwise: `--bus NAME=p2p` (two ends),
  `--bus NAME=arbitrated[,slot=TIME]` (messages for the same time contend, the lowest key
  wins, the losers come back flagged `UAM_LINK_F_LOST`; without kept time nothing contends).
- `uam-link-hub --log` prints every join, refusal and routed message.

## 4. How time is kept (the hub decides)

| `uam-link-hub --sync` | guarantee | use it when |
|---|---|---|
| `free` (default) | none: a message arrives when it arrives | the far end is real hardware or a tool; first contact between two boards |
| `loose[=TIME]` | no emulation more than TIME (1 ms) ahead; late messages flagged | guest timeouts fire that should not |
| `exact` | lockstep; the run is the same every time | reproducing a bug, regression tests, a snapshot of both |

Start with `free`. The usual first failure is **a guest timeout that fires although the
peer answered**: under `free` each emulation's guest clock runs as fast as its host
allows, so one can be seconds of guest time ahead. Fix: `--realtime` on both, or a
`loose` hub.

`exact` needs, or it refuses or stalls:

- `latency=TIME` on **every** port (`50us`, `1ms`, ...): the time the medium takes for a
  frame. It is the lookahead; a smaller one costs more round trips when the guests are busy.
- `--nodes N` on the hub (above), so no emulation runs ahead before the others have joined.
- a console that cannot produce input (`--console file:OUT` or `null`), or the run is not
  repeatable and an idle guest costs round trips.
- `--link-tick-hz N` if your board's ticks are not worth 10 ns (the default 100 M ticks per
  emulated second): give the rate your timer devices assume.

One snapshot of all of them: `--link-stop-at TIME --save-snapshot FILE` on each (exit 0
when all stand at TIME), then a new hub and `--load-snapshot FILE` on each.

## 5. When it does not work

| symptom | cause | do |
|---|---|---|
| exit 2, `cannot reach a hub` | the hub is not running, or another path | start `uam-link-hub` first |
| exit 2, `needs a latency` | a hub that keeps time, a port without `latency=` | add it |
| exit 2, `already connected` | two emulations with the same name | `--link-name` |
| `nothing uses link port 'X'` | the board has no port of that name and no `--console`/`--nic` takes it | the name in `uam_board_config_link` and on the command line must match |
| the first message is lost, then both wait | one emulation sent before the other had joined the hub | `uam-link-hub --nodes N` |
| nothing arrives | different bus names; or the frontend never has room | `uam-link-hub --log` (is it routed?), `--trace link` on both (tx on one, rx on the other?) |
| exit 132 | the hub went away | it exits when its last emulation leaves unless `--keep`; a crashed hub |
| exit 125 on a timed hub | every emulation idle with nothing armed: nobody will ever send | what was the last `link ... rx` line; which side should have answered |
| the pair hangs under `exact` | one emulation stopped (in gdb, at a breakpoint): the others wait for its time | let it run; debug one side on a `free` hub |
| a guest timeout under `free` | guest clocks drift apart | `--realtime` on both, or `--sync loose` |
| a full-duplex or acknowledged transfer is off by one step under `exact` | nothing crosses a link in zero time | make the responder a device in the same board, or hold the guest in the controller model until the answer is in |

`--trace link` prints one line per message sent (`tx`) and handed to the device (`rx`),
with its time, key, flags, length, first bytes, and the guest pc; under `exact` the two
sides' traces are identical from run to run, so `diff` finds what changed.

## 6. Fuzzing through a link

A fuzz run is never connected to a hub (ember refuses it). The port itself is the input:
`--link bus0=none --fuzz-input bus0`, each record of the input (a 16-bit little-endian
length, then the payload) one message to your device. A board that needs keys or flags in
the input publishes its own input with `uam_machine_add_input` and calls
`uam_link_port_inject`. See the **ember-fuzz** skill.
