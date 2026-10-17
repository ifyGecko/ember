/*
 * hostloop.c - The host loop (uam/host.h): polls backend file descriptors on
 * behalf of the executor and, in real-time mode, paces virtual time against
 * the host clock.
 */
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <time.h>

#include "uam/host.h"

#define MAX_SOURCES 32u

struct uam_host {
    uam_chardev *cds[MAX_SOURCES];
    unsigned     n_cds;
    uam_netdev  *nds[MAX_SOURCES];
    unsigned     n_nds;
    uam_pacing   pacing;
    uint64_t     ticks_per_sec;
    uint64_t     max_ahead;    /* ticks */
    uam_exec    *x;
    bool         started;
    uint64_t     t0_ns;        /* host time at the pacing origin */
    uint64_t     v0;           /* virtual time at the pacing origin */
};

static uint64_t host_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

uam_host *uam_host_create(void)
{
    uam_host *h = calloc(1, sizeof(*h));
    if (h) {
        h->ticks_per_sec = 100000000u;   /* 100 M instructions per second */
        h->max_ahead = 1000000u;         /* 10 ms at that rate */
    }
    return h;
}

void uam_host_destroy(uam_host *h)
{
    if (h && h->x)
        uam_exec_set_host(h->x, NULL);
    free(h);
}

uam_status uam_host_add_chardev(uam_host *h, uam_chardev *cd)
{
    if (!cd)
        return UAM_ERR_INVALID;
    if (h->n_cds == MAX_SOURCES)
        return UAM_ERR_NOMEM;
    h->cds[h->n_cds++] = cd;
    return UAM_OK;
}

uam_status uam_host_add_netdev(uam_host *h, uam_netdev *nd)
{
    if (!nd)
        return UAM_ERR_INVALID;
    if (h->n_nds == MAX_SOURCES)
        return UAM_ERR_NOMEM;
    h->nds[h->n_nds++] = nd;
    return UAM_OK;
}

void uam_host_set_pacing(uam_host *h, uam_pacing pacing, uint64_t ticks_per_sec,
                         uint64_t max_ahead_ticks)
{
    h->pacing = pacing;
    if (ticks_per_sec)
        h->ticks_per_sec = ticks_per_sec;
    if (max_ahead_ticks)
        h->max_ahead = max_ahead_ticks;
    h->started = false;
}

static bool has_fds(uam_host *h)
{
    for (unsigned i = 0; i < h->n_cds; i++)
        if (uam_chardev_fd(h->cds[i]) >= 0)
            return true;
    for (unsigned i = 0; i < h->n_nds; i++)
        if (uam_netdev_fd(h->nds[i]) >= 0)
            return true;
    return false;
}

/* Service every source; wait up to timeout_ms for one with a host fd. */
void uam_host_poll(uam_host *h, int timeout_ms)
{
    struct pollfd pfd[2 * MAX_SOURCES];
    unsigned n = 0;
    for (unsigned i = 0; i < h->n_cds; i++) {
        int fd = uam_chardev_fd(h->cds[i]);
        if (fd >= 0)
            pfd[n++] = (struct pollfd){ .fd = fd, .events = POLLIN };
    }
    /* A netdev whose guest queue is full is not watched: the host kernel holds
     * its frames until the guest takes some (backpressure). */
    for (unsigned i = 0; i < h->n_nds; i++) {
        int fd = uam_netdev_fd(h->nds[i]);
        if (fd >= 0 && uam_netdev_can_input(h->nds[i]))
            pfd[n++] = (struct pollfd){ .fd = fd, .events = POLLIN };
    }
    if (!n && timeout_ms < 0 && has_fds(h))
        timeout_ms = 100;   /* every source is held back: wait without spinning */
    if (n || timeout_ms > 0) {
        int rc = poll(pfd, n, timeout_ms);
        (void)rc;  /* EINTR (e.g. SIGINT -> uam_exec_stop) simply returns early */
    }
    /* Polling a backend with nothing to read is a cheap non-blocking no-op. */
    for (unsigned i = 0; i < h->n_cds; i++)
        uam_chardev_poll(h->cds[i]);
    for (unsigned i = 0; i < h->n_nds; i++)
        uam_netdev_poll(h->nds[i]);
}

/* ---- real-time pacing ------------------------------------------------------ */

static void pacing_start(uam_host *h)
{
    if (!h->started) {
        h->started = true;
        h->t0_ns = host_ns();
        h->v0 = uam_sched_now(h->x->sched);
    }
}

/* Host time (ns) at which virtual time `v` is due. */
static uint64_t due_ns(const uam_host *h, uint64_t v)
{
    uint64_t dv = v > h->v0 ? v - h->v0 : 0;
    return h->t0_ns + (uint64_t)((double)dv * 1e9 / (double)h->ticks_per_sec);
}

/* Virtual time corresponding to the host clock now. */
static uint64_t host_virtual_now(const uam_host *h)
{
    uint64_t dt = host_ns() - h->t0_ns;
    return h->v0 + (uint64_t)((double)dt * (double)h->ticks_per_sec / 1e9);
}

static int ms_until(uint64_t when_ns)
{
    uint64_t now = host_ns();
    if (when_ns <= now)
        return 0;
    uint64_t ms = (when_ns - now + 999999u) / 1000000u;
    return ms > 1000 ? 1000 : (int)ms;   /* wake at least once a second */
}

static void bridge_poll(void *ctx)
{
    uam_host *h = ctx;
    uam_host_poll(h, 0);
    if (h->pacing != UAM_PACE_REALTIME)
        return;
    pacing_start(h);
    uint64_t v = uam_sched_now(h->x->sched);
    /* Ahead of the host clock by more than allowed: wait, servicing input. */
    while (!h->x->stop_request && v > host_virtual_now(h) + h->max_ahead)
        uam_host_poll(h, ms_until(due_ns(h, v - h->max_ahead)));
}

static bool bridge_idle(void *ctx, uint64_t ticks, uint64_t *skip)
{
    uam_host *h = ctx;
    if (h->pacing != UAM_PACE_REALTIME) {
        if (ticks != UINT64_MAX) {
            uam_host_poll(h, 0);
            *skip = ticks;           /* deterministic: idle time costs nothing */
            return true;
        }
        if (!has_fds(h))
            return false;            /* nothing can ever wake the hart */
        uam_host_poll(h, -1);        /* wait for host input */
        *skip = 0;
        return true;
    }

    pacing_start(h);
    uint64_t now_v = uam_sched_now(h->x->sched);
    if (ticks == UINT64_MAX && !has_fds(h))
        return false;
    int timeout = ticks == UINT64_MAX ? 1000 : ms_until(due_ns(h, now_v + ticks));
    uam_host_poll(h, timeout);
    /* Advance virtual time by the host time that passed, up to the next event. */
    uint64_t hv = host_virtual_now(h);
    uint64_t passed = hv > now_v ? hv - now_v : 0;
    *skip = passed < ticks ? passed : ticks;
    return true;
}

void uam_host_attach(uam_host *h, uam_exec *x)
{
    h->x = x;
    h->started = false;
    uam_exec_host bridge = { .poll = bridge_poll, .idle = bridge_idle, .ctx = h,
                             .poll_interval = 1024 };
    uam_exec_set_host(x, &bridge);
}
