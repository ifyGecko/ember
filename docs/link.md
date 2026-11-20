# Links: connecting emulations

A **link** carries timestamped messages between a device model and whatever it
is connected to: another emulation, a tool, a recorded trace, a fuzz input. It
is how two or more emulated devices talk over a channel that is not Ethernet —
a serial protocol, a field bus, a radio, a handful of wires — and how they do
so with **deterministic timing** when that is needed.

ember ships the mechanism and no protocol. A message is a payload of exact
length plus a key, flags and a kind; what those mean belongs to the device
models at the two ends.

Contents: [quick start](#quick-start) · [when to use it](#when-to-use-a-link)
· [concepts](#concepts) · [time](#keeping-time-free-loose-exact) ·
[using a port](#using-a-port) · [buses](#buses-and-media) ·
[one process](#several-machines-in-one-process) · [snapshots](#snapshots) ·
[fuzzing](#fuzzing) · [diagnostics](#diagnostics) · [cost](#what-it-costs) ·
[limits](#limits) · [wire protocol](#the-wire-protocol) · [API](#api-map)

## Quick start

Start a hub, then connect two emulations to it. Each gives its port a name, a
bus, and its own name on the hub. Here the two boards' console UARTs are wired
to each other, which works on every board:

```sh
uam-link-hub unix:/tmp/hub.sock --nodes 2 &
while [ ! -S /tmp/hub.sock ]; do sleep 0.1; done      # the hub is listening

uam-run -b BOARD --link p0=unix:/tmp/hub.sock,bus=wire --link-name left  \
        --console link:p0 left.elf &
uam-run -b BOARD --link p0=unix:/tmp/hub.sock,bus=wire --link-name right \
        --console link:p0 right.elf
```

On a board with a network port, `--nic PORT=link:p0` connects that instead
(ember's own reference boards have none; `build/tests/uam-test-run -b rv32-test`
has `net0`), and a board can hand a port to a device of its own
([using a port](#using-a-port)).

That is the default, `free` mode: messages are delivered as they arrive and
nothing is promised about time. For a run that is the same every time:

```sh
uam-link-hub unix:/tmp/hub.sock --sync exact --nodes 2 &
uam-run ... --link p0=unix:/tmp/hub.sock,bus=wire,latency=50us ...
```

About the hub:

- **Start it first.** An emulation that finds no hub exits with status 2.
- **`--nodes N` starts everyone together.** Each emulation waits at the hub,
  for as long as it takes, until N have connected. Without it each starts as it
  connects, and a message sent on a bus nobody else has joined yet goes
  nowhere, as on a cable not plugged in.
- **It leaves with its last emulation** (`--keep` makes it stay), and an
  emulation whose hub goes away exits with status 132.
- It listens on `unix:PATH` or `tcp:[HOST:]PORT`; HOST defaults to 127.0.0.1,
  so name an address of the machine (`tcp:0.0.0.0:7000`) for emulations on
  other machines to reach it.

## When to use a link

| You have | Use |
|---|---|
| Two emulations, Ethernet frames of 60 bytes or more, timing does not matter | `--nic PORT=udp:...` still works and needs no hub |
| Frames shorter than 60 bytes (they must arrive at their real length) | a link |
| More than two devices on one medium | a link (a bus) |
| A byte stream between two emulated UARTs | `--console link:PORT` |
| A device of your own: a bus controller, a radio, GPIO lines | a link port taken by the board |
| A run that must be repeatable, or a snapshot of several devices at once | a link, `--sync exact` |
| Fuzzing a device through its communication channel | a link port left unconnected, `--fuzz-input PORT` |

## Concepts

- **Message**: `time`, `key` (64 bits), `flags` (32), `kind` (32), and a
  payload of 0 to 16 MiB at its exact length. `key` is whatever the medium
  arbitrates or filters on (an identifier, a channel, a chip select). Flag bits
  0–15 are ember's (`UAM_LINK_F_ERROR`, `_LOST`, `_LATE`), bits 16–31 are the
  device models'. Kinds: `UAM_LINK_DATA` (payload bytes) and `UAM_LINK_LEVEL`
  (a line changed: `key` is the line, `payload[0]` the level).
- **Port**: one end of a device model's connection. What the device cannot
  take yet waits in the port; nothing is dropped.
- **Node**: one machine's ports, its clock, and its connection to a hub. Its
  name identifies it on the hub and must be unique there.
- **Hub**: connects nodes. It routes messages over named **buses** and decides
  how time is kept. `uam-link-hub` is a hub as a process; a program can also
  hold one itself (`uam_link_hub_create`).
- **Latency**: how long a port's messages take to reach the far end. Each
  message is stamped with the sender's time plus its port's latency: that is
  when it takes effect at the receiver.
- **Grant**: on a hub that keeps time, the time a node may run to. A node that
  reaches its grant tells the hub where it stands and waits for a new one: a
  **round trip** to the hub, which is what keeping time costs.

Port, bus and node names are up to 31 characters.

## Keeping time: free, loose, exact

The hub chooses (`--sync`); every node connected to it follows.

| Mode | What is guaranteed | Cost | Use it for |
|---|---|---|---|
| `free` (default) | Nothing about time. A message is delivered when it arrives. | none | real hardware and tools at the far end; links whose protocol tolerates any timing |
| `loose[=TIME]` | No node runs more than TIME (default 1 ms) further ahead than the others allow. A message whose time has already passed is delivered at once and flagged `UAM_LINK_F_LATE`. Not deterministic. Every port needs a latency, as for `exact`. | a round trip to the hub per grant | emulations whose guest timeouts must not fire spuriously |
| `exact` | Every message is delivered at its time, ties in a fixed order. A run is deterministic. | a round trip per grant; grants are a latency long while a guest is busy | repeatable runs, regression tests, a snapshot of all nodes at one moment |

**`free` and guest time.** Each emulation's virtual time runs as fast as its
host allows, so two `free` nodes drift apart: one can be seconds of guest time
ahead of the other. A guest timeout (a retransmit timer, a bus-off counter)
can then fire although the answer took a millisecond of host time. Either pace
both to the host clock (`--realtime`), or use `loose`.

**How `exact` works.** A node only passes a tick once every message that takes
effect before it is known. Each node tells the hub where it stands; the hub
grants it time up to the earliest moment a neighbour's message could arrive,
which is that neighbour's position plus its latency. The latency is therefore
the *lookahead*: the longer it is, the further a busy node runs between round
trips. A machine whose processors all wait tells the hub how long it stays
quiet (until its next timer), and the others run ahead to there.

What `exact` needs:

- **A latency on every port** (`latency=TIME`, at least one time unit). A hub
  that keeps time, `loose` or `exact`, refuses a port without one.
- **Every node present at the start**: `--nodes N` holds every node at the
  hub until N have joined (in any mode), so none runs ahead alone.
- **Stable names** (`--link-name`): ties between messages for the same instant
  are broken by the sender's name, so the order nodes connected in does not
  matter.
- **No other source of nondeterminism**: an interactive console or a `tap`
  network port still makes a run differ. Use `--console file:OUT` or `null`.
  (A console that can produce input also keeps an idle node from promising to
  stay quiet, which costs round trips.)

What `exact` promises to be identical from run to run: the guests' state and
everything they observe, and the `--trace link` output. Where blocks are cut
follows the grants, so `--trace blocks` and the executor's statistics may
differ.

**What a tick is worth.** A machine's ticks count instructions; on a link they
have to mean time. `--link-tick-hz N` says how many ticks make an emulated
second (default: `--realtime`'s rate, else 100 M). Two machines with different
rates meet in link time. The hub's time unit is the picosecond (`--unit`
changes it; 64 bits of picoseconds are 213 days).

## Using a port

`--link PORT=HUB[,bus=BUS][,latency=TIME]` creates port `PORT`. `HUB` is
`unix:PATH`, `tcp:HOST:PORT`, or `none` (not connected). `BUS` defaults to the
port's name, so two emulations that both call their port `p0` meet without it.
`TIME` is a number with `ps`, `ns`, `us`, `ms` or `s`, and defaults to 0.
Ports that name the same hub share one connection (one node); ports on
different hubs, or some on a hub and some on `none`, make one node per hub,
all under the emulation's `--link-name`. A port is then used in one of three
ways.

**As a console** — `--console link:PORT`. The bytes of one UART write are one
message; a message's bytes are the UART's input. Any board's console works.

**As a network port** — `--nic NAME=link:PORT[,pcap=FILE]`. A frame is a
message at its exact length (nothing is padded to 60 bytes). The line is up
while the port reaches a hub.

**By a device of the board's own.** The board looks the port up by name and
its device model takes it:

```c
/* in the board's build function */
uam_link_port *port = uam_board_config_link(cfg, "bus0");   /* --link bus0=... */
if (port)
    my_controller_attach(dev, port);

/* in the device model */
static bool my_can_receive(void *ctx) { return rx_fifo_has_room(ctx); }

static void my_receive(void *ctx, const uam_link_msg *m)
{
    my_dev *d = ctx;
    if (m->flags & UAM_LINK_F_LOST) {       /* our own message: it lost the bus */
        retry_later(d);
        return;
    }
    rx_fifo_push(d, m->key, m->data, m->len);
    uam_signal_set(&d->irq, 1);
}

void my_controller_attach(my_dev *d, uam_link_port *port)
{
    d->port = port;
    uam_link_port_set_frontend(port, my_can_receive, my_receive, NULL, d);
}

/* the guest wrote the transmit register */
uam_link_send(d->port, identifier, 0, UAM_LINK_DATA, payload, len);

/* the guest read the receive FIFO: there is room again */
uam_link_accept_input(d->port);
```

A line (a GPIO, a strobe) is the same with `UAM_LINK_LEVEL`: send when the
level changes, with the line number as the key. A device model may use any
kind below `0x80000000` for messages of its own; the hub passes them on unread.

What a device model should know:

- **Nothing connected.** `uam_board_config_link` returns NULL when the option
  was not given, and `uam_link_send` returns false when the port reaches no
  hub. Model both as the real part behaves with nothing on the wire.
- **Events.** The fourth argument of `uam_link_port_set_frontend` is called
  with `UAM_LINK_EV_UP` when the port reaches a hub, `UAM_LINK_EV_DOWN` when
  the hub is gone, and `UAM_LINK_EV_REWOUND` after a snapshot restore the far
  end did not take part in ([snapshots](#snapshots)).
- **No room.** While `can_receive` returns false the message waits in the
  port. Call `uam_link_accept_input` when room appears; a model that is not
  told when that is asks to be offered the messages again later with
  `uam_link_port_retry(port, ticks)`.
- **Flags of its own** start at `1u << UAM_LINK_F_MODEL_SHIFT` (bit 16).
- **State.** What the device has taken belongs in its own `save`/`restore`;
  what still waits in the port is saved by the node. An adapter that keeps
  messages on the port's behalf registers them with `uam_link_port_set_state`.
- **Headers.** A device includes `uam/link.h` only. `uam/host.h` (hubs,
  connections, the chardev and netdev adapters) is for the program that builds
  the machine, not for boards or devices.

A worked example with its state saved is the `msgport` device of the test
board `rv32-test` (`tests/boards/test_boards.c`), on the port `msg0`.

## Buses and media

A bus is created the first time a port names it, as a broadcast bus. Declare
it on the hub to give it another medium (`--bus NAME=MEDIUM`):

| Medium | Behaviour |
|---|---|
| `broadcast` | Every other port on the bus gets every message. |
| `p2p` | The same; a third port is refused. |
| `arbitrated[,slot=TIME]` | Messages for the same time contend: the lowest key takes the bus and goes to everyone else; the others return to their senders flagged `UAM_LINK_F_LOST`. Times are rounded up to the slot. Under `free` nothing contends: each message has the bus to itself. |
| custom | In a program of your own: `uam_link_hub_set_medium(hub, bus, fn, ctx)` installs `fn(ctx, hub, bus, member, port, msg)`, called for each message sent on the bus; it hands messages on with `uam_link_hub_deliver(hub, member, port, msg)`: to whom, when, and changed how is its business (on a hub that keeps time, never earlier than the message's own time). |

A program that holds the hub itself declares a bus with
`uam_link_hub_add_bus(hub, name, medium, slot)`, which is what `--bus` does.

## Several machines in one process

A program can build several machines, attach each one's node to a hub it
created, and let `uam_link_world_run` drive them on one thread
(`uam/host.h`, `uam/machine.h`):

```c
uam_link_hub *hub = uam_link_hub_create(&(uam_link_hub_config){ .sync = UAM_LINK_EXACT });
uam_link_node *na = uam_link_node_create("a");
uam_link_port *pa = uam_link_node_add_port(na, "p0", "wire", 50 * 1000 * 1000 /* ps */);
/* ... build machine A, hand pa to its device, then: */
uam_link_node_bind(na, &machine_a, 0);
uam_link_hub_attach(hub, na);
/* ... the same for B ... */
uam_link_world *w = uam_link_world_create(hub);
uam_link_world_add(w, &machine_a, na);
uam_link_world_add(w, &machine_b, nb);
uam_link_world_run(w, UAM_LINK_NEVER);
```

No sockets and no waiting: in a timed mode the world runs each machine up to
its grant, the one furthest behind first; under `free`, a slice of
instructions at a time (`uam_link_world_set_slice`, 4096 unless set) — so the
result is deterministic in every mode. `uam-run` itself runs one machine.

- Bind each node to its machine (`uam_link_node_bind`) before adding it to the
  world.
- `uam_link_world_run(w, until)` returns true when every machine has stopped
  or waits for a message that cannot come, and false when all have reached
  `until` (a time in the hub's unit; `UAM_LINK_NEVER` for no bound). It can be
  called again. `uam_link_world_reason(w, i)` says why machine `i` stopped.
- The machines must not have a host loop that blocks waiting for input.

## Snapshots

A node's queues and timers are part of its machine's snapshot (the record
`link-NAME`), and so is the queue of a console or network port that sits on a
link. Rules:

- A snapshot taken with link ports restores only with the same ports in the
  same order and the same `--link-name`: the record is named after the node,
  and each port's name is checked. Create ports before the first snapshot.
- A machine without link ports is unchanged: its snapshots are byte for byte
  what they were.
- **Not connected** (`none`): a snapshot is complete.
- **Connected to a `free` or `loose` hub**, in another process or this one: a
  snapshot holds this node only. What is at the far end does not go back with
  it. On restore the device models get `UAM_LINK_EV_REWOUND` (a network port
  sees its line drop and come back), so a protocol can resynchronise as after
  a pulled cable.
- **Several machines in one process, `exact`**: take a snapshot of each
  machine between two calls of `uam_link_world_run`. To go back, disconnect
  every node (`uam_link_node_set_transport(node, NULL, NULL)`), restore the
  machines, and attach the nodes to a **new** hub and world: a hub's own
  record of where its members stand only moves forward. The run then ends as
  the uninterrupted one would have (`test_link`, `test_world_snapshot`).
- **Connected to an `exact` hub**: take one consistent snapshot of every node
  by stopping them all at the same link time:

  ```sh
  uam-run ... --link-stop-at 2ms --save-snapshot a.snap     # on every node
  ```

  Each node runs to that time, the hub confirms once all stand there and
  everything sent before has been handed over, and each saves and exits with
  status 0 (not the 124 of a time limit). To resume,
  start a new hub and every node with `--load-snapshot`; the run continues
  exactly as the uninterrupted one would have. The hub needs no file.

## Fuzzing

A fuzz run restores a snapshot for every input, and whatever is at the other
end of a hub does not go back with it. So:

- **A run that fuzzes is not connected to a hub** — `uam-run` refuses it.
- **The port is the input.** Leave it unconnected and name it as the route:

  ```sh
  uam-run -b BOARD --link bus0=none --fuzz-begin ADDR --fuzz-input bus0 ...
  ```

  The input's bytes are records, each a 16-bit little-endian length followed
  by that many payload bytes; each record becomes one `UAM_LINK_DATA` message
  handed to the port, in order. A record that runs past the end takes what is
  left; an input shorter than two bytes is one message, and so is a single
  byte left behind the last record: no byte of an input goes unused. The messages carry key 0 and no
  flags. A board that wants another framing, or keys and flags from the input,
  publishes an input of its own (`uam_machine_add_input`) and hands each
  message to the port with `uam_link_port_inject(port, &msg, 0)`.
- A port named like an input the board already has (`uart0` on the stock
  boards) does not become one; the runner says so. Give the port another name.

Fuzzing several live emulations together is not built (see [limits](#limits)).

## Diagnostics

All off unless asked for.

- `--trace link`: one line per message a port sends (`tx`) or hands to its
  device (`rx`), with time, key, flags, kind, length, the first 16 payload
  bytes, and for `rx` the node's own time (`at=`), followed by the guest pc
  and instruction count. `time=` and `at=` are in the hub's unit, picoseconds
  unless the hub was given another. A program of its own gets the same
  messages with `uam_link_node_set_tap`.
- `uam-link-hub --log`: joins, leaves, refusals, and every message routed.
- A message delivered after its time carries `UAM_LINK_F_LATE` and is counted
  in the port's statistics (`uam_link_port_stats`: sent, received, dropped for
  want of a hub, late).

Exit statuses: `uam-run` exits **132** when its hub goes away, **125** when
every node on a timed hub is idle with nothing left that could wake any of
them, and **2** when a hub cannot be reached or refuses a port.

## What it costs

A machine with no link pays nothing: no check on any path, and its snapshots
are unchanged. With a link, `tests/link/throughput.py` measures the rest
(`meson test -C build --suite link-throughput`); the figures for the recording
host are in `tests/link/throughput.ref`.

Two `linktest` guests on `rv32-test` exchanging 17-byte frames through a hub on one
x86-64 host, on the JIT (`--runs 5`, the best of each):

| Run | Result | Host time per message |
|---|---|---|
| `udp` — `--nic udp:` straight to the peer, 60-byte frames, no time kept | 31 600 messages/s | 32 µs |
| `free` — through a hub that keeps no time | 19 800 messages/s | 51 µs |
| `loose` — 1 ms skew | 31 400 messages/s | 32 µs |
| `exact` — lockstep, guests asleep between frames, 50 µs latency | 26 600 messages/s | 38 µs |
| `bytes` — lockstep, the console as the link, one message a byte, 87 µs latency, the UART polled | 6 900 messages/s | 145 µs |

| Run | Host time, relative to the same guests running free of each other |
|---|---|
| `exact-busy` — lockstep, both guests polling, 500 µs latency | 7.4× |
| 50 µs latency | 22× |
| 5 µs latency | 205× |

(The timed modes look for the other side's answer for 30 µs before going to sleep
for it, which is why `loose` and `exact` beat `free` here; `free` never waits for the
hub at all, it only sleeps when its guest does. The guests' polling loop runs at
3.6 G instructions/s on this host, so a latency of 50 µs at 100 M ticks/s is 1.4 µs of
host time: the busy figures are almost entirely the round trips.)

How to read that:

- `free` costs a hop through the hub process per message, compared with a UDP
  socket straight to the peer.
- `exact` with guests that sleep between messages costs little more than
  `free`: an idle machine tells the hub how long it stays quiet, so a message
  costs a few round trips whatever the latency.
- `exact` with guests that never sleep costs one round trip per latency of
  guest time, so the slowdown grows as the latency shrinks. Set the latency to
  what the medium really takes for a **frame**, not for a bit.
- One message per UART byte means one latency per byte. A device model of
  your own can send a whole frame as one message.

## Limits

Inherent in keeping exact time:

- **Nothing takes effect in zero time between two emulated processors.** A
  latency of at least one time unit is the lookahead. A full-duplex exchange
  where the answer travels during the request (SPI), or an acknowledge inside
  the transfer (I²C), arrives one step late across a link; model the
  responder as a device in the same machine, or have the controller's model
  hold the guest until the answer is in.
- **`exact` needs every node to keep virtual time.** Real hardware, or a tool
  that does not speak the hub's protocol with reports and grants, belongs on a
  `free` or `loose` hub.
- **A node waiting for a peer that is stopped waits too**: pausing one
  emulation in a debugger pauses all of them, and a node blocked that way
  cannot be interrupted from its own debugger.

Not built yet (in `TODO.md`):

- fuzzing several live emulations together (not planned until a target needs
  it: the port as the input, from a snapshot taken after the peers' handshake,
  covers the usual cases);
- several machines on several host threads (one process, one thread works);
- `uam-run` driving more than one machine;
- a machine on more than one hub that keeps time (several buses on one hub
  are fine, and so are several hubs when at most one keeps time);
- a shared-memory transport, and a converter from the hub's log to pcapng.

## The wire protocol

A unix or TCP stream of frames, all integers little-endian:

```
u32 length of the body    u16 type    u16 0    body
```

A reader skips frame types it does not know. Version 1:

| Type | From | Body |
|---|---|---|
| 1 HELLO | node | u32 version, u32 capabilities (0), name |
| 2 WELCOME | hub | u32 version, u32 capabilities (0), u32 sync (0 free, 1 loose, 2 exact), u32 0, u64 time units per second, u64 skew |
| 3 PORT | node | u32 port, u32 0, u64 latency in units, bus name |
| 4 READY | node | — (its ports are declared) |
| 5 JOINED | hub | — (sent once every node the hub waits for, `--nodes`, has joined) |
| 6 ERROR | hub | text; the connection ends |
| 7 MSG | both | u32 port, u32 flags, u32 kind, u32 source, u32 sequence, u32 0, u64 time, u64 key, payload |
| 8 REPORT | node | u64 reached, u64 quiet until, u64 messages received so far |
| 9 GRANT | hub | u64 time the node may run to |
| 10 QUIESCENT | hub | — (every node is quiet; nothing will happen) |
| 11 BARRIER_AT | node | u64 time (it stands there) |
| 12 BARRIER_DONE | hub | u64 time (every node does) |
| 13 BYE | either | — |

A session: HELLO, WELCOME, one PORT per port, READY, JOINED (or ERROR), then
MSG in both directions and, on a hub that keeps time, REPORT and GRANT. A node
that does not report is simply never granted time by anyone waiting on it, so
a tool that only sends and receives messages belongs on a `free` hub.

## API map

| Header | What |
|---|---|
| `uam/link.h` | messages; ports (`uam_link_send`, `uam_link_port_set_frontend`, `uam_link_accept_input`, `uam_link_port_retry`, `uam_link_port_inject`, `uam_link_port_set_state`, `uam_link_port_stats`); nodes (`uam_link_node_create`, `_add_port`, `_set_tap`, `_save`, `_restore`); the transport interface a hub connection implements |
| `uam/host.h` | `uam_link_node_bind`, the hub (`uam_link_hub_create`, `_add_bus`, `_set_medium`, `_deliver`, `_attach`), `uam_link_node_connect` (it waits while the hub holds the node for `--nodes`), the server (`uam_link_server_*`), `uam_link_world_*`, `uam_chardev_link_create`, `uam_netdev_link_create`, `uam_host_add_link`, `uam_link_node_barrier`, `uam_link_parse_time` |
| `uam/machine.h` | `uam_board_config.links`, `uam_board_config_link` |

Sources: `src/core/link.c` (ports, nodes, timed delivery, grants, saved
state), `src/host/link_hub.c` (routing, granting, the in-process world),
`src/host/link_wire.c` (protocol, transport, server), `src/host/link_adapters.c`
(chardev and netdev on a port), `src/host/link_node.c` (binding to a machine),
`tools/uam-link-hub.c`. Tests: `tests/unit/test_link.c`,
`tests/link/link-check.sh`, `tests/link/throughput.py`.
