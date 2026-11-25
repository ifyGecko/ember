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
    uam_link_node *links[MAX_SOURCES];
    unsigned     n_links;
    struct host_fd {
        int            fd;
        uam_host_fd_fn fn;
        void          *ctx;
    }            fds[MAX_SOURCES];  /* uam_host_add_fd: polled, never wakes the hart */
    unsigned     n_fds;
    void       (*tick)(void *ctx);  /* uam_host_set_tick */
    void        *tick_ctx;
    uam_pacing   pacing;
    uint64_t     ticks_per_sec;
    uint64_t     max_ahead;    /* ticks */
    uam_exec    *x;
    bool         started;
    uint64_t     t0_ns;        /* host time at the pacing origin */
    uint64_t     v0;           /* virtual time at the pacing origin */
    uint32_t     idle_skips;   /* virtual pacing: idle skips since the last host poll */
};

/* Virtual pacing: an idle hart skips to its next timer in a few nanoseconds, while
 * looking for host input costs a poll() system call (~1 us). Look once every this
 * many skips, so that a hart idling through a stream of timer events (and, when
 * those cannot wake it, the max_stall livelock count) runs at the skip's speed.
 * Input still arrives within microseconds of host time. */
#define IDLE_POLL_EVERY 64u

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
    for (unsigned i = 0; h && i < h->n_links; i++)
        uam_link_node_set_waiter(h->links[i], NULL, NULL);
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

uam_status uam_host_add_fd(uam_host *h, int fd, uam_host_fd_fn fn, void *ctx)
{
    if (fd < 0 || !fn)
        return UAM_ERR_INVALID;
    if (h->n_fds == MAX_SOURCES)
        return UAM_ERR_NOMEM;
    h->fds[h->n_fds++] = (struct host_fd){ .fd = fd, .fn = fn, .ctx = ctx };
    return UAM_OK;
}

void uam_host_remove_fd(uam_host *h, int fd)
{
    for (unsigned i = 0; i < h->n_fds; i++)
        if (h->fds[i].fd == fd) {
            h->fds[i] = h->fds[--h->n_fds];
            return;
        }
}

void uam_host_set_tick(uam_host *h, void (*fn)(void *ctx), void *ctx)
{
    h->tick = fn;
    h->tick_ctx = ctx;
}

void uam_host_resync(uam_host *h)
{
    h->started = false;
}

/* A node waits for its hub: keep the other sources serviced meanwhile, and
 * give up when the run is asked to stop. */
static bool link_wait(void *ctx, uam_link_node *n)
{
    uam_host *h = ctx;
    (void)n;
    uam_host_poll(h, 1000);
    return !(h->x && h->x->stop_request);
}

uam_status uam_host_add_link(uam_host *h, uam_link_node *n)
{
    if (!n)
        return UAM_ERR_INVALID;
    if (h->n_links == MAX_SOURCES)
        return UAM_ERR_NOMEM;
    h->links[h->n_links++] = n;
    uam_link_node_set_waiter(n, link_wait, h);
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

/* Host input that can wake the machine by itself (not a hub's messages). */
static bool has_input_fds(uam_host *h)
{
    for (unsigned i = 0; i < h->n_cds; i++)
        if (uam_chardev_fd(h->cds[i]) >= 0)
            return true;
    for (unsigned i = 0; i < h->n_nds; i++)
        if (uam_netdev_fd(h->nds[i]) >= 0)
            return true;
    return false;
}

static bool has_fds(uam_host *h)
{
    for (unsigned i = 0; i < h->n_links; i++)
        if (uam_link_node_fd(h->links[i]) >= 0)
            return true;
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
    struct pollfd pfd[4 * MAX_SOURCES];
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
    for (unsigned i = 0; i < h->n_links; i++) {
        int fd = uam_link_node_fd(h->links[i]);
        if (fd >= 0)
            pfd[n++] = (struct pollfd){ .fd = fd, .events = POLLIN };
    }
    if (!n && timeout_ms < 0 && has_fds(h))
        timeout_ms = 100;   /* every source is held back: wait without spinning */
    /* Owner fds come last, after the sources counted above; a callback may add or
     * remove them, so it is called from a copy. */
    const unsigned n_src = n;
    struct host_fd own[MAX_SOURCES];
    const unsigned n_own = h->n_fds;
    for (unsigned i = 0; i < n_own; i++) {
        own[i] = h->fds[i];
        pfd[n++] = (struct pollfd){ .fd = own[i].fd, .events = POLLIN };
    }
    int rc = 0;
    if (n || timeout_ms > 0)
        rc = poll(pfd, n, timeout_ms);   /* EINTR (e.g. SIGINT -> uam_exec_stop) returns early */
    for (unsigned i = 0; rc > 0 && i < n_own; i++)
        if (pfd[n_src + i].revents)
            own[i].fn(own[i].ctx, own[i].fd);
    /* Polling a backend with nothing to read is a cheap non-blocking no-op. */
    for (unsigned i = 0; i < h->n_cds; i++)
        uam_chardev_poll(h->cds[i]);
    for (unsigned i = 0; i < h->n_nds; i++)
        uam_netdev_poll(h->nds[i]);
    for (unsigned i = 0; i < h->n_links; i++)
        uam_link_node_poll(h->links[i]);
    if (h->tick)
        h->tick(h->tick_ctx);
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
    /* A node whose hub keeps time: wait for the grant here, where an idle
     * machine can tell the hub how long it stays quiet. Nothing but a message
     * or a timer wakes it unless host input can. */
    bool timed = false;
    for (unsigned i = 0; i < h->n_links; i++) {
        if (!uam_link_node_timed(h->links[i]))
            continue;
        timed = true;
        uint64_t limit = h->x->max_time && h->x->max_time < UAM_SCHED_TIME_MAX ? h->x->max_time
                                                                               : UAM_SCHED_TIME_MAX;
        int r = uam_link_node_idle(h->links[i], limit, !has_input_fds(h));
        if (r < 0)
            return false;            /* every node is quiet: nothing will ever happen */
        if (r > 0) {
            *skip = 0;
            return true;
        }
    }
    /* Asked to wait for host input alone (the executor gave up on the timers)
     * while timers are armed: the other nodes wait for this one's time to
     * move, so let it. */
    if (timed && ticks == UINT64_MAX && uam_sched_next(h->x->sched) != UINT64_MAX) {
        *skip = 0;
        return true;
    }
    if (h->pacing != UAM_PACE_REALTIME) {
        if (ticks != UINT64_MAX) {
            if (++h->idle_skips >= IDLE_POLL_EVERY) {
                h->idle_skips = 0;
                uam_host_poll(h, 0);
            }
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
