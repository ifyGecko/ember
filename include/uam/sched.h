/*
 * uam/sched.h - The Time interface: deterministic virtual time and events.
 *
 * Virtual time is measured in ticks:
 *
 *     now = instructions issued (the hart's icount) + ticks skipped while idle
 *
 * No host time is ever consulted, so a run is fully deterministic. Devices
 * arm timers for an absolute tick; the executor fires due timers between
 * blocks and stops a block early (at an instruction boundary) when the next
 * timer comes due. While a hart waits (WFI) with nothing else to do, the
 * executor skips time forward to the next timer.
 *
 * Virtual time never passes UAM_SCHED_TIME_MAX, which leaves ample headroom
 * for the `now + period` arithmetic devices use to re-arm timers: a deadline
 * computed from any reachable `now` cannot wrap past 2^64.
 *
 * Dependencies: common.h.
 */
#ifndef UAM_SCHED_H
#define UAM_SCHED_H

#include "uam/common.h"

typedef struct uam_timer uam_timer;
typedef void (*uam_timer_fn)(void *ctx);

struct uam_timer {
    uint64_t     when;   /* absolute tick */
    uam_timer_fn fn;
    void        *ctx;
    bool         armed;
    uam_timer   *next;   /* sorted list, owned by the scheduler */
};

/* Called whenever the earliest deadline may have moved (lets the executor
 * stop the current block early). */
typedef void (*uam_sched_kick_fn)(void *ctx);

typedef struct uam_sched {
    const uint64_t   *icount;   /* the hart's instruction counter */
    uint64_t          idle;     /* ticks skipped while idle */
    uam_timer        *head;     /* armed timers, sorted by `when` */
    uam_sched_kick_fn kick;
    void             *kick_ctx;
} uam_sched;

void     uam_sched_init(uam_sched *s, const uint64_t *icount);
void     uam_sched_set_kick(uam_sched *s, uam_sched_kick_fn fn, void *ctx);

/* The virtual-time horizon: uam_sched_skip_to saturates here, and the executor
 * stops (UAM_STOP_TIME_LIMIT) instead of running past it. */
#define UAM_SCHED_TIME_MAX (UINT64_C(1) << 62)

/* At most this many timers fire per uam_sched_run_due call, so that a timer
 * re-arming itself at or before `now` cannot trap the caller. */
#define UAM_SCHED_MAX_FIRE 4096u

static inline uint64_t uam_sched_now(const uam_sched *s) { return *s->icount + s->idle; }

/* Initialise a timer once, when its owner is created. Never re-initialise an
 * armed timer: cancel it (e.g. in a reset handler) instead. Debug builds abort
 * when a re-initialised timer that is still linked is armed again. */
void     uam_timer_init(uam_timer *t, uam_timer_fn fn, void *ctx);
void     uam_timer_arm(uam_sched *s, uam_timer *t, uint64_t when);
void     uam_timer_cancel(uam_sched *s, uam_timer *t);
/* Disarm every timer (snapshot restore: owners re-arm their own). */
void     uam_sched_cancel_all(uam_sched *s);

/* Earliest armed deadline, or UINT64_MAX if none. */
uint64_t uam_sched_next(const uam_sched *s);
/* Fire timers whose deadline is <= now, in deadline order, at most
 * UAM_SCHED_MAX_FIRE of them. Returns the number fired; more may still be due
 * (uam_sched_next() <= now) when it returns UAM_SCHED_MAX_FIRE. */
unsigned uam_sched_run_due(uam_sched *s);
/* Idle: advance time to `when` (no-op if in the past), saturating at
 * UAM_SCHED_TIME_MAX. */
void     uam_sched_skip_to(uam_sched *s, uint64_t when);

/* Consistency checks for tests: the timer list is acyclic, sorted, and every
 * linked timer is marked armed. */
bool     uam_sched_check(const uam_sched *s);
unsigned uam_sched_count(const uam_sched *s);

#endif /* UAM_SCHED_H */
