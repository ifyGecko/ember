/*
 * uam/link.h - The Link interface: timestamped messages between a guest device
 * model and whatever it is connected to (another emulation, a tool, a recorded
 * trace, a fuzz input).
 *
 * A link carries messages, not a protocol. A message is a payload of exact
 * length plus a key, flags and a kind whose meaning belongs to the device
 * models at the two ends (an identifier to arbitrate on, a channel, a chip
 * select, an error or acknowledge bit). The same interface therefore serves
 * whole frames, single bytes, bus transactions and line levels.
 *
 *     port = uam_link_node_add_port(node, "port0", "bus0", latency);
 *     uam_link_port_set_frontend(port, can_receive, receive, event, dev);
 *     uam_link_send(port, key, flags, UAM_LINK_DATA, bytes, len);    guest transmits
 *     uam_link_accept_input(port);                                   guest has room again
 *
 * Objects:
 *   port   one end of a device model's connection. Messages the frontend
 *          cannot take yet are queued (no limit, nothing is dropped).
 *   node   one machine's ports, its clock, and its connection to a hub (the
 *          transport). A node with no transport is local: what it sends goes
 *          nowhere, what it receives is injected (uam_link_port_inject).
 *   hub    connects nodes: it routes messages over named buses and, if asked,
 *          keeps the nodes' clocks together (uam/host.h).
 *
 * Time. Every message carries a time in the hub's unit (unit_hz units per
 * emulated second, picoseconds unless the hub says otherwise): the moment it
 * takes effect at the receiver, which is the sender's time plus the port's
 * latency. A node converts between that unit and its machine's ticks
 * (uam/sched.h) with the tick rate it was bound with. Three ways of keeping
 * time, chosen on the hub:
 *   UAM_LINK_FREE   (default) time is not kept: a message is delivered when it
 *                   arrives. Costs nothing; not deterministic.
 *   UAM_LINK_LOOSE  no node runs more than a set skew ahead of the others. A
 *                   message whose time has passed is delivered at once and
 *                   flagged UAM_LINK_F_LATE. Not deterministic.
 *   UAM_LINK_EXACT  a node only passes a tick once every message that takes
 *                   effect before it is known (the hub grants time, using the
 *                   latencies as lookahead). Each message is delivered at its
 *                   time, ties in a fixed order: deterministic.
 * In the last two a node stops at its grant and waits for the hub; a latency
 * of 0 is refused, since nothing could then ever be granted.
 *
 * Delivery. At a message's tick it is handed to the frontend after every other
 * timer due at that tick, so that the order does not depend on when the host
 * happened to read it. `receive` is never called from inside uam_link_send on
 * the same node.
 *
 * State. A node saves its queues and timers (uam_link_node_save); the board
 * layer registers that with the machine (uam_link_node_bind in uam/host.h), so
 * a machine with a node snapshots it and one without is unchanged.
 *
 * Dependencies: common.h, sched.h, state.h.
 */
#ifndef UAM_LINK_H
#define UAM_LINK_H

#include "uam/common.h"
#include "uam/sched.h"
#include "uam/state.h"

/* The default time unit: picoseconds. */
#define UAM_LINK_UNIT_HZ UINT64_C(1000000000000)
/* "No time": a quiet node's next event, a grant without limit. */
#define UAM_LINK_NEVER   UINT64_MAX
/* Largest payload carried. */
#define UAM_LINK_MAX_MSG (UINT32_C(16) << 20)
#define UAM_LINK_NAME_MAX 32u

typedef enum uam_link_sync { UAM_LINK_FREE, UAM_LINK_LOOSE, UAM_LINK_EXACT } uam_link_sync;

/* Message kinds. Values from UAM_LINK_KIND_RESERVED up are ember's own (the
 * hub's control channel) and never reach a frontend. */
#define UAM_LINK_DATA          0u   /* payload bytes */
#define UAM_LINK_LEVEL         1u   /* a line changed: key = line, payload[0] = level */
#define UAM_LINK_KIND_RESERVED UINT32_C(0x80000000)

/* Flags. Bits 0..15 are ember's, bits 16..31 belong to the device models. */
#define UAM_LINK_F_ERROR (1u << 0)  /* the sender marks the message as damaged */
#define UAM_LINK_F_LOST  (1u << 1)  /* returned to its sender: it lost the medium to another */
#define UAM_LINK_F_LATE  (1u << 2)  /* set on delivery: its time had already passed */
#define UAM_LINK_F_MODEL_SHIFT 16u

typedef struct uam_link_msg {
    uint64_t       time;    /* when it takes effect, in the hub's unit (set by uam_link_send) */
    uint64_t       key;
    uint32_t       flags;
    uint32_t       kind;
    uint32_t       src;     /* the sending port as the hub numbers it (0: local) */
    uint32_t       seq;     /* the sender's count of messages on that port */
    uint32_t       len;
    const uint8_t *data;
} uam_link_msg;

typedef enum uam_link_event {
    UAM_LINK_EV_UP,        /* the port reaches a hub */
    UAM_LINK_EV_DOWN,      /* the hub is gone */
    UAM_LINK_EV_REWOUND,   /* a snapshot was restored while connected to a hub that does
                            * not keep exact time: the far end did not go back with us */
} uam_link_event;

typedef struct uam_link_stats {
    uint64_t tx, rx;       /* messages sent / handed to the frontend */
    uint64_t tx_dropped;   /* sent with no hub to take them */
    uint64_t late;         /* delivered after their time */
} uam_link_stats;

typedef struct uam_link_port uam_link_port;
typedef struct uam_link_node uam_link_node;

/* ---- frontend (device) side ------------------------------------------------ */

void uam_link_port_set_frontend(uam_link_port *p, bool (*can_receive)(void *ctx),
                                void (*receive)(void *ctx, const uam_link_msg *m),
                                void (*event)(void *ctx, uam_link_event ev), void *ctx);
/* Send one message, stamped now + the port's latency. False: nothing took it. */
bool uam_link_send(uam_link_port *p, uint64_t key, uint32_t flags, uint32_t kind,
                   const void *data, size_t len);
/* The frontend can take messages again. */
void uam_link_accept_input(uam_link_port *p);

const char           *uam_link_port_name(const uam_link_port *p);
const char           *uam_link_port_bus(const uam_link_port *p);
uint64_t              uam_link_port_latency(const uam_link_port *p);
unsigned              uam_link_port_index(const uam_link_port *p);
uam_link_node        *uam_link_port_node(const uam_link_port *p);
bool                  uam_link_port_up(const uam_link_port *p);
const uam_link_stats *uam_link_port_stats(const uam_link_port *p);
/* Messages waiting for the frontend (not those whose time is still to come). */
size_t                uam_link_port_ready(const uam_link_port *p);

/* ---- owner side ------------------------------------------------------------ */

uam_link_node *uam_link_node_create(const char *name);
void           uam_link_node_destroy(uam_link_node *n);
const char    *uam_link_node_name(const uam_link_node *n);

/* A port on bus `bus`, whose messages take `latency` (in the hub's unit) to
 * reach the far end. Ports are added before the node is connected or saved. */
uam_link_port *uam_link_node_add_port(uam_link_node *n, const char *name, const char *bus,
                                      uint64_t latency);
uam_link_port *uam_link_node_find_port(const uam_link_node *n, const char *name);
unsigned       uam_link_node_ports(const uam_link_node *n);
uam_link_port *uam_link_node_port(const uam_link_node *n, unsigned index);

/* Give the node its clock: the machine's scheduler and what a tick is worth
 * (ticks per emulated second). Until then messages are delivered on arrival. */
uam_status     uam_link_node_set_clock(uam_link_node *n, uam_sched *sched, uint64_t tick_hz);
uam_sched     *uam_link_node_sched(const uam_link_node *n);
/* The node's time now, and conversions (0 / saturating without a clock). */
uint64_t       uam_link_node_time(const uam_link_node *n);
uint64_t       uam_link_node_from_ticks(const uam_link_node *n, uint64_t ticks);   /* rounds down */
uint64_t       uam_link_node_to_ticks(const uam_link_node *n, uint64_t time);      /* rounds up */

void           uam_link_node_save(uam_link_node *n, uam_state_writer *w);
void           uam_link_node_restore(uam_link_node *n, uam_state_reader *r);

/* Hand a message to a port as if it had come over the link: at tick `at_tick`
 * of the node's clock, or at once when at_tick is 0 or already past. For input
 * that does not come from a hub (a fuzz input, a recorded trace, a test). */
uam_status     uam_link_port_inject(uam_link_port *p, const uam_link_msg *m, uint64_t at_tick);

/* One observer of every message (tracing): `out` is true for a message the
 * guest sends, false when one is handed to the frontend. NULL removes it. */
typedef void (*uam_link_tap_fn)(void *ctx, const uam_link_port *p, const uam_link_msg *m, bool out);
void           uam_link_node_set_tap(uam_link_node *n, uam_link_tap_fn fn, void *ctx);

/* The hub went away while the node was connected. */
bool           uam_link_node_lost(const uam_link_node *n);
/* Called once when that happens (a runner stops the machine). */
void           uam_link_node_set_lost_handler(uam_link_node *n, void (*fn)(void *ctx), void *ctx);

/* ---- transport side (what connects a node to a hub) -------------------------- */

typedef struct uam_link_transport {
    const char *type;
    /* A message leaves the node on port `port`. False: not taken. */
    bool (*send)(void *ctx, unsigned port, const uam_link_msg *m);
    /* Timed modes: the node stands at `reached` and, left alone, does nothing
     * before `quiet` (>= reached; UAM_LINK_NEVER: nothing at all). */
    void (*report)(void *ctx, uint64_t reached, uint64_t quiet);
    /* Host file descriptor that becomes readable when the hub has something, or -1. */
    int  (*fd)(void *ctx);
    /* Take what has arrived, waiting up to timeout_ms for something (0: do not
     * wait, <0: until something arrives): calls uam_link_node_input, _grant,
     * _quiescent. False: the hub is gone. NULL for a transport driven by its
     * owner (the in-process hub), which never waits. */
    bool (*poll)(void *ctx, int timeout_ms);
    void (*close)(void *ctx);
} uam_link_transport;

/* Connect (ops != NULL) or disconnect the node. The node closes its transport
 * when it is destroyed or reconnected. */
void uam_link_node_set_transport(uam_link_node *n, const uam_link_transport *ops, void *ctx);
bool uam_link_node_connected(const uam_link_node *n);
/* What the hub decided: how time is kept and in which unit. */
void uam_link_node_set_sync(uam_link_node *n, uam_link_sync sync, uint64_t unit_hz);
uam_link_sync uam_link_node_sync(const uam_link_node *n);
uint64_t uam_link_node_unit_hz(const uam_link_node *n);
/* A message arrives for port `port`. */
void uam_link_node_input(uam_link_node *n, unsigned port, const uam_link_msg *m);
/* The node may run while its time is below `time`: every message that takes
 * effect before it has been handed over. Grants never move back. */
void uam_link_node_grant(uam_link_node *n, uint64_t time);
/* Every node is quiet and nothing is under way: no message will ever come. */
void uam_link_node_quiescent(uam_link_node *n);
void uam_link_node_set_lost(uam_link_node *n);

/* ---- keeping time (for the host loop and for drivers) ------------------------ */

/* Whether the node waits for grants: connected, a timed mode, a clock. */
bool     uam_link_node_timed(const uam_link_node *n);
/* The tick the node may not pass (UINT64_MAX: no limit). */
uint64_t uam_link_node_grant_tick(const uam_link_node *n);
/* The earliest tick at which a message is due to be delivered, or UINT64_MAX. */
uint64_t uam_link_node_next_delivery(const uam_link_node *n);
/* Take what the hub has sent without waiting. False: the hub is gone. */
bool     uam_link_node_poll(uam_link_node *n);
int      uam_link_node_fd(const uam_link_node *n);
/*
 * How a node waits for its hub. By default it blocks in its transport; a host
 * loop installs a waiter that also services the machine's other host sources.
 * The waiter waits a while for anything to happen, calls uam_link_node_poll,
 * and returns false to give up (the run was asked to stop).
 */
void     uam_link_node_set_waiter(uam_link_node *n, bool (*wait)(void *ctx, uam_link_node *n),
                                  void *ctx);
/*
 * The machine is idle (every processor waits). If the node's grant ends before
 * the machine's next own event, advance to the grant, tell the hub and wait
 * for more. `limit` is a tick time must not pass; `promise` says that nothing
 * but a message or a timer can wake the machine, so the hub may let the others
 * run ahead until then (false when host input can wake it).
 * Returns 0: nothing to do (not timed, or the next event is within the grant),
 *         1: waited; look at the machine again,
 *        -1: nothing will ever happen (every node is quiet).
 */
int      uam_link_node_idle(uam_link_node *n, uint64_t limit, bool promise);

#endif /* UAM_LINK_H */
