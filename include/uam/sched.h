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

static inline uint64_t uam_sched_now(const uam_sched *s) { return *s->icount + s->idle; }

/* Initialise a timer once, when its owner is created. Never re-initialise an
 * armed timer: cancel it (e.g. in a reset handler) instead. Debug builds abort
 * when a re-initialised timer that is still linked is armed again. */
void     uam_timer_init(uam_timer *t, uam_timer_fn fn, void *ctx);
void     uam_timer_arm(uam_sched *s, uam_timer *t, uint64_t when);
void     uam_timer_cancel(uam_sched *s, uam_timer *t);

/* Earliest armed deadline, or UINT64_MAX if none. */
uint64_t uam_sched_next(const uam_sched *s);
/* Fire every timer whose deadline is <= now. Returns the number fired. */
unsigned uam_sched_run_due(uam_sched *s);
/* Idle: advance time to `when` (no-op if in the past). */
void     uam_sched_skip_to(uam_sched *s, uint64_t when);

/* Consistency checks for tests: the timer list is acyclic, sorted, and every
 * linked timer is marked armed. */
bool     uam_sched_check(const uam_sched *s);
unsigned uam_sched_count(const uam_sched *s);

#endif /* UAM_SCHED_H */
