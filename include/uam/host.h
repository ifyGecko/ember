/*
 * uam/host.h - The host side: chardev/netdev backends and the host loop.
 *
 * Nothing in the machine depends on this header. Devices see only the
 * chardev.h / netdev.h interfaces; a program (uam-run, a fuzzer, an external
 * runner) chooses backends here and attaches a host loop to the executor.
 *
 * Pacing:
 *   UAM_PACE_VIRTUAL  (default) virtual time never waits for the host. Idle
 *                     periods are skipped instantly (host input is looked at
 *                     every 64 skips); with no timer pending, or once the
 *                     executor decides the timers cannot wake the hart
 *                     (max_stall), the hart blocks until host input arrives.
 *                     Runs without host input are deterministic.
 *   UAM_PACE_REALTIME virtual time is held to at most `max_ahead` ticks ahead of
 *                     the host clock at `ticks_per_sec`, and idle periods take
 *                     real time, so host input arrives at a plausible guest time
 *                     (interactive consoles, networking).
 */
#ifndef UAM_HOST_H
#define UAM_HOST_H

#include <stdio.h>

#include "uam/chardev.h"
#include "uam/exec.h"
#include "uam/link.h"
#include "uam/netdev.h"

struct uam_machine;

/* ---- chardev backends ---- */
uam_chardev *uam_chardev_null_create(void);
/* Output to `out` (NULL: discard); fixed input bytes (copied), delivered as
 * the frontend accepts them. Deterministic: no host fd. */
uam_chardev *uam_chardev_buffer_create(FILE *out, const void *input, size_t len);
/* Fixed input as a script, run in order: a SEND step queues its bytes for the
 * guest; an EXPECT step holds every later step back until the guest's output,
 * from the moment the step is reached, contains its bytes. Input then goes in
 * when the guest has printed what it prints just before reading it (a shell
 * prompt, "login: "), so nothing earlier in boot can consume or flush it: a
 * firmware polling the console, an OS driver clearing the receive FIFO.
 * Deterministic: the guest's output is. The bytes a match releases are handed
 * to the frontend during the guest write that completed it. Steps are copied;
 * an EXPECT of zero bytes is satisfied at once. */
typedef enum { UAM_CHARDEV_SEND, UAM_CHARDEV_EXPECT } uam_chardev_step_kind;
typedef struct uam_chardev_step {
    uam_chardev_step_kind kind;
    const void           *data;
    size_t                len;
} uam_chardev_step;
uam_chardev *uam_chardev_script_create(FILE *out, const uam_chardev_step *steps, size_t n);
/* stdin/stdout. raw: put a terminal in raw mode; Ctrl-A x quits, Ctrl-A Ctrl-A
 * sends Ctrl-A. Without raw, EOF on stdin just ends input. */
uam_chardev *uam_chardev_stdio_create(bool raw);
/* A pseudo-terminal; its path is logged (connect with screen/picocom). */
uam_chardev *uam_chardev_pty_create(void);
/* Server socket, one client at a time: "tcp:PORT", "tcp:HOST:PORT", "unix:PATH". */
uam_chardev *uam_chardev_socket_create(const char *spec);
/* Parse a backend spec:
 *   null | stdio | stdio-raw | pty | tcp:[HOST:]PORT | unix:PATH
 *   file:OUT[,in=INPUT_FILE]     output file (- = stdout), input read from a file
 *   file:OUT,script=SCRIPT_FILE  output file, input from an expect/send script
 *   string:TEXT                  stdout + fixed input TEXT (\n, \r, \t, \\, \xHH escapes)
 *   script:SCRIPT_FILE           stdout + input from an expect/send script
 * string: and file:...,in= take a last option ,after=PATTERN (escapes as in
 * TEXT; the rest of the spec, commas included): each line of the input
 * (through its \n) waits for PATTERN in the output, typed at a prompt. Write
 * a comma just before "after=" in TEXT as \x2c. A script file has one step
 * per line, the text after the keyword's single space taken verbatim (with
 * the same escapes); blank lines and lines starting with # are skipped:
 *   expect TEXT                  wait for TEXT in the guest's output
 *   send TEXT                    queue TEXT (write \n for Enter) */
uam_chardev *uam_chardev_open(const char *spec);

/* ---- netdev backends ---- */
uam_netdev *uam_netdev_null_create(void);
/* Linux tap interface: Ethernet frames, the guest is a station on the host's
 * interface (bridge it, or give it an address). `ifname` NULL or "" creates a
 * new one (needs CAP_NET_ADMIN); an existing persistent interface owned by
 * the user (`ip tuntap add dev NAME mode tap user $USER`) needs no privilege. */
uam_netdev *uam_netdev_tap_create(const char *ifname);
/* Linux tun interface (IP packets) behind an Ethernet adapter: the guest sees
 * one host-side station that answers ARP and IPv6 neighbour solicitations for
 * every address and exchanges IPv4/IPv6 with the host's routing; other
 * ethertypes are dropped. Use tap when the guest needs a real L2 segment. */
uam_netdev *uam_netdev_tun_create(const char *ifname);
/* An already attached tap (tun = false) or tun fd, e.g. inherited from a
 * privileged launcher. Ownership of the fd passes to the netdev. */
uam_netdev *uam_netdev_tap_fd_create(int fd, bool tun);
uam_netdev *uam_netdev_udp_create(uint16_t local_port, const char *remote_host, uint16_t remote_port);
/* Two in-process endpoints wired back to back (deterministic). */
uam_status  uam_netdev_pair_create(uam_netdev **a, uam_netdev **b);
/* Parse: null | tap:[IFNAME] | tun:[IFNAME] | tap-fd:FD | tun-fd:FD |
 * udp:LPORT:RHOST:RPORT, optionally followed by ",pcap=FILE". */
uam_netdev *uam_netdev_open(const char *spec);
/* Record every frame (both directions) to a pcap file. */
uam_status  uam_netdev_capture(uam_netdev *nd, const char *path);

/* ---- links (uam/link.h) ---- */

/*
 * Give a node its machine: the machine's clock at `tick_hz` ticks per emulated
 * second (0: 100 M, the host loop's default rate), the node's state in the
 * machine's snapshots (as "link-NAME"), and one named input per port
 * (uam_machine_add_input, under the port's name) that hands the guest a block
 * of bytes as messages - see uam_link_input_deliver. Call it once the board is
 * built and before the first snapshot. The node must outlive the machine.
 */
uam_status uam_link_node_bind(uam_link_node *n, struct uam_machine *m, uint64_t tick_hz);
/* The named input of a port (ctx is the uam_link_port): the bytes are records,
 * each a 16-bit little-endian length followed by that many payload bytes, and
 * each becomes one UAM_LINK_DATA message handed to the port at once, in order.
 * A record that runs past the end takes what is left; fewer than two bytes are
 * one message. */
uam_status uam_link_input_deliver(void *ctx, struct uam_machine *m, const uint8_t *data,
                                  size_t len);

/*
 * A link port as a chardev or a netdev, for a device model that knows only
 * those interfaces. Chardev: the bytes of one write are one message, and a
 * message's bytes are input. Netdev: a frame is a message at its exact length
 * (no padding to an Ethernet minimum); the line is up while the port reaches a
 * hub. What the device has no room for waits in the port. The adapter takes
 * the port's frontend and saves the chardev's / netdev's queue with the port;
 * destroy it before the node. In a spec: --console link:PORT, --nic P=link:PORT.
 */
uam_chardev *uam_chardev_link_create(uam_link_port *port);
uam_netdev  *uam_netdev_link_create(uam_link_port *port);

/* A time as written on a command line - a number with ps, ns, us, ms or s -
 * in picoseconds. False: not a time. */
bool uam_link_parse_time(const char *text, uint64_t *ps);

/* How a bus shares its medium. */
typedef enum uam_link_medium {
    UAM_LINK_BROADCAST,        /* every other port on the bus gets every message */
    UAM_LINK_POINT_TO_POINT,   /* the same, and a third port is refused */
    UAM_LINK_ARBITRATED,       /* messages for the same time contend: the lowest key takes
                                * the bus, the others return to their senders with
                                * UAM_LINK_F_LOST. Times are rounded up to the bus's slot. With
                                * no time kept (free) every message has the bus to itself. */
    UAM_LINK_CUSTOM,           /* uam_link_hub_set_medium */
} uam_link_medium;

typedef struct uam_link_hub uam_link_hub;

typedef struct uam_link_hub_config {
    uam_link_sync sync;            /* UAM_LINK_FREE unless set */
    uint64_t      unit_hz;         /* time units per emulated second; 0: picoseconds */
    uint64_t      skew;            /* loose: how far a node may run ahead, in units */
    unsigned      start_members;   /* timed modes: grant nothing until this many joined */
} uam_link_hub_config;

uam_link_hub *uam_link_hub_create(const uam_link_hub_config *cfg);
void          uam_link_hub_destroy(uam_link_hub *h);
const uam_link_hub_config *uam_link_hub_get_config(const uam_link_hub *h);
/* Members that have joined and not left. */
unsigned      uam_link_hub_members(const uam_link_hub *h);
/* A bus is created as a broadcast bus the first time a port names it; declare
 * it first to give it another medium. `slot` is an arbitrated bus's time grid
 * in units (0 or 1: messages contend only at exactly the same time). */
uam_status    uam_link_hub_add_bus(uam_link_hub *h, const char *name, uam_link_medium medium,
                                   uint64_t slot);
/* A medium of the caller's own: called for each message sent on the bus, it
 * hands it on with uam_link_hub_deliver (to whom, when and changed how is its
 * business; in a timed mode never earlier than the message's own time). */
typedef void (*uam_link_medium_fn)(void *ctx, uam_link_hub *h, const char *bus, unsigned member,
                                   unsigned port, const uam_link_msg *m);
uam_status    uam_link_hub_set_medium(uam_link_hub *h, const char *bus, uam_link_medium_fn fn,
                                      void *ctx);
void          uam_link_hub_deliver(uam_link_hub *h, unsigned member, unsigned port,
                                   const uam_link_msg *m);
/* Connect a node in this process (its ports join their buses). The node is
 * then driven by its owner: see uam_link_world_run. UAM_ERR_REJECTED: a port
 * the hub will not have (latency 0 in a timed mode, a third end). */
uam_status    uam_link_hub_attach(uam_link_hub *h, uam_link_node *n);
/* Stop waiting for start_members: grant with whoever has joined. */
void          uam_link_hub_start(uam_link_hub *h);

/* Members: what the hub routes between. A transport joins on behalf of a node
 * it reaches some other way. */
typedef struct uam_link_member_ops {
    void (*msg)(void *ctx, unsigned port, const uam_link_msg *m);
    void (*grant)(void *ctx, uint64_t time);
    void (*quiescent)(void *ctx);        /* optional */
} uam_link_member_ops;
/* Returns the member's id (>= 0), or -1. `time` is where the member stands. */
int           uam_link_hub_join(uam_link_hub *h, const char *name, const uam_link_member_ops *ops,
                                void *ctx, uint64_t time);
uam_status    uam_link_hub_add_port(uam_link_hub *h, unsigned member, unsigned port,
                                    const char *bus, uint64_t latency);
void          uam_link_hub_leave(uam_link_hub *h, unsigned member);
bool          uam_link_hub_msg(uam_link_hub *h, unsigned member, unsigned port,
                               const uam_link_msg *m);
/* `received`: how many messages the member had taken from the hub when it said
 * so (UINT64_MAX: all of them). */
void          uam_link_hub_report(uam_link_hub *h, unsigned member, uint64_t reached,
                                  uint64_t quiet, uint64_t received);

/*
 * Between processes. A hub serves connections on "unix:PATH" or
 * "tcp:[HOST:]PORT" (uam_link_server; the uam-link-hub tool is this and a
 * command line), and a node connects to it: the hub tells it how time is kept,
 * the node declares its ports. The node's name identifies it on the hub and
 * must be unique there; ties between messages are broken by it, so a run does
 * not depend on the order the nodes connected in. Add the ports first.
 * UAM_ERR_FAULT: no hub there; UAM_ERR_REJECTED: the hub refused a port (the
 * reason is in err).
 */
uam_status uam_link_node_connect(uam_link_node *n, const char *spec, char *err, size_t errlen);
/*
 * For a snapshot of every node at the same moment (exact mode): each node runs
 * to `time`, then calls this, which tells the hub and returns once every node
 * stands there and everything sent before has been handed over. A snapshot
 * taken then, on each node, is one consistent state of the whole; the hub holds
 * nothing and needs no file. False: the hub went away. A node that is not
 * connected to another process returns true at once.
 */
bool       uam_link_node_barrier(uam_link_node *n, uint64_t time);

typedef struct uam_link_server uam_link_server;
uam_link_server *uam_link_server_create(uam_link_hub *hub, const char *spec);
void             uam_link_server_destroy(uam_link_server *s);
/* Serve for up to timeout_ms (<0: until something happens). False once every
 * node that joined has left. */
bool             uam_link_server_step(uam_link_server *s, int timeout_ms);
/* Log joins, leaves and every message routed, one line each (NULL: nothing). */
void             uam_link_server_set_log(uam_link_server *s, FILE *log);
uint64_t         uam_link_server_messages(const uam_link_server *s);

/*
 * Several machines in one process, one thread: each has a node attached to the
 * same hub (uam_link_hub_attach) and bound to it (uam_link_node_bind). The
 * world runs them in turn - in a timed mode each up to its grant, the one
 * furthest behind first; with no time kept, `slice` instructions at a time -
 * so the result is deterministic in every mode. The machines must not have a
 * host loop that blocks.
 */
typedef struct uam_link_world uam_link_world;
uam_link_world *uam_link_world_create(uam_link_hub *hub);
void            uam_link_world_destroy(uam_link_world *w);
uam_status      uam_link_world_add(uam_link_world *w, struct uam_machine *m, uam_link_node *n);
void            uam_link_world_set_slice(uam_link_world *w, uint64_t insns);
/* Run until every machine has stopped or waits for a message that cannot come
 * (returns true), or until all have reached `until_time` in the hub's unit
 * (UAM_LINK_NEVER: no bound; returns false). Can be called again. */
bool            uam_link_world_run(uam_link_world *w, uint64_t until_time);
/* Why machine `index` (in the order added) stopped; UAM_STOP_NONE while it runs
 * or waits. */
uam_stop_reason uam_link_world_reason(const uam_link_world *w, unsigned index);

/* ---- host loop ---- */
typedef enum uam_pacing { UAM_PACE_VIRTUAL, UAM_PACE_REALTIME } uam_pacing;

typedef struct uam_host uam_host;

uam_host  *uam_host_create(void);
/* Does not destroy the chardevs/netdevs added to it. */
void       uam_host_destroy(uam_host *h);
uam_status uam_host_add_chardev(uam_host *h, uam_chardev *cd);
uam_status uam_host_add_netdev(uam_host *h, uam_netdev *nd);
/* Service a link node's connection to its hub: messages are taken as they
 * arrive and, where the hub keeps time, the machine waits for its grants here
 * (an idle machine without blocking the others). Add it before the run. */
uam_status uam_host_add_link(uam_host *h, uam_link_node *n);
void       uam_host_set_pacing(uam_host *h, uam_pacing pacing, uint64_t ticks_per_sec,
                               uint64_t max_ahead_ticks);
/* Install the host loop as the executor's host bridge (uam_exec_set_host). */
void       uam_host_attach(uam_host *h, uam_exec *x);
/* Service host I/O once, waiting up to timeout_ms (-1: until something happens). */
void       uam_host_poll(uam_host *h, int timeout_ms);

#endif /* UAM_HOST_H */
