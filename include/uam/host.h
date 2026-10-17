/*
 * uam/host.h - The host side: chardev/netdev backends and the host loop.
 *
 * Nothing in the machine depends on this header. Devices see only the
 * chardev.h / netdev.h interfaces; a program (uam-run, a fuzzer, an external
 * runner) chooses backends here and attaches a host loop to the executor.
 *
 * Pacing:
 *   UAM_PACE_VIRTUAL  (default) virtual time never waits for the host. Idle
 *                     periods are skipped instantly; with no timer pending the
 *                     hart blocks until host input arrives. Runs without host
 *                     input are deterministic.
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
 *   string:TEXT                  stdout + fixed input TEXT (\n, \r, \xHH escapes) */
uam_chardev *uam_chardev_open(const char *spec);

/* ---- netdev backends ---- */
uam_netdev *uam_netdev_null_create(void);
uam_netdev *uam_netdev_tap_create(const char *ifname);
uam_netdev *uam_netdev_udp_create(uint16_t local_port, const char *remote_host, uint16_t remote_port);
/* Two in-process endpoints wired back to back (deterministic). */
uam_status  uam_netdev_pair_create(uam_netdev **a, uam_netdev **b);
/* Parse: null | tap:IFNAME | udp:LPORT:RHOST:RPORT, optionally ",pcap=FILE". */
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
