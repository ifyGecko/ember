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
#include "uam/netdev.h"

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

/* ---- host loop ---- */
typedef enum uam_pacing { UAM_PACE_VIRTUAL, UAM_PACE_REALTIME } uam_pacing;

typedef struct uam_host uam_host;

uam_host  *uam_host_create(void);
/* Does not destroy the chardevs/netdevs added to it. */
void       uam_host_destroy(uam_host *h);
uam_status uam_host_add_chardev(uam_host *h, uam_chardev *cd);
uam_status uam_host_add_netdev(uam_host *h, uam_netdev *nd);
void       uam_host_set_pacing(uam_host *h, uam_pacing pacing, uint64_t ticks_per_sec,
                               uint64_t max_ahead_ticks);
/* Install the host loop as the executor's host bridge (uam_exec_set_host). */
void       uam_host_attach(uam_host *h, uam_exec *x);
/* Service host I/O once, waiting up to timeout_ms (-1: until something happens). */
void       uam_host_poll(uam_host *h, int timeout_ms);

#endif /* UAM_HOST_H */
