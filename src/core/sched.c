/*
 * sched.c - Virtual time and timers (uam/sched.h).
 *
 * Timers live in a singly linked list sorted by deadline. Machines have a
 * handful of timers, so a list beats a heap in clarity and in practice.
 */
#include <stdio.h>
#include <stdlib.h>

#include "uam/sched.h"

void uam_sched_init(uam_sched *s, const uint64_t *icount)
{
    s->icount = icount;
    s->idle = 0;
    s->head = NULL;
    s->kick = NULL;
    s->kick_ctx = NULL;
}

void uam_sched_set_kick(uam_sched *s, uam_sched_kick_fn fn, void *ctx)
{
    s->kick = fn;
    s->kick_ctx = ctx;
}

void uam_timer_init(uam_timer *t, uam_timer_fn fn, void *ctx)
{
    t->when = 0;
    t->fn = fn;
    t->ctx = ctx;
    t->armed = false;
    t->next = NULL;
}

static void unlink_timer(uam_sched *s, uam_timer *t)
{
    for (uam_timer **pp = &s->head; *pp; pp = &(*pp)->next) {
        if (*pp == t) {
            *pp = t->next;
            break;
        }
    }
    t->next = NULL;
    t->armed = false;
}

#ifndef NDEBUG
/* A timer that believes it is unarmed must not be on the list: that happens
 * when uam_timer_init() is called on a linked timer, and inserting it again
 * would corrupt the list (a cycle or a lost tail). Fail loudly instead. */
static void check_unlinked(const uam_sched *s, const uam_timer *t)
{
    for (const uam_timer *p = s->head; p; p = p->next) {
        if (p == t) {
            fprintf(stderr, "uam sched: timer %p re-initialised while armed "
                            "(use uam_timer_cancel before re-use)\n", (const void *)t);
            abort();
        }
    }
}
#endif

void uam_timer_arm(uam_sched *s, uam_timer *t, uint64_t when)
{
    if (t->armed)
        unlink_timer(s, t);
#ifndef NDEBUG
    else
        check_unlinked(s, t);
#endif
    t->when = when;
    t->armed = true;
    uam_timer **pp = &s->head;
    while (*pp && (*pp)->when <= when)
        pp = &(*pp)->next;
    t->next = *pp;
    *pp = t;
    if (s->head == t && s->kick)
        s->kick(s->kick_ctx);
}

void uam_timer_cancel(uam_sched *s, uam_timer *t)
{
    if (t->armed)
        unlink_timer(s, t);
}

void uam_sched_cancel_all(uam_sched *s)
{
    while (s->head) {
        uam_timer *t = s->head;
        s->head = t->next;
        t->next = NULL;
        t->armed = false;
    }
}

uint64_t uam_sched_next(const uam_sched *s)
{
    return s->head ? s->head->when : UINT64_MAX;
}

unsigned uam_sched_run_due(uam_sched *s)
{
    unsigned fired = 0;
    while (fired < UAM_SCHED_MAX_FIRE && s->head && s->head->when <= uam_sched_now(s)) {
        uam_timer *t = s->head;
        s->head = t->next;
        t->next = NULL;
        t->armed = false;
        t->fn(t->ctx); /* may re-arm itself */
        fired++;
    }
    return fired;
}

void uam_sched_skip_to(uam_sched *s, uint64_t when)
{
    uint64_t now = uam_sched_now(s);
    if (when > UAM_SCHED_TIME_MAX)
        when = UAM_SCHED_TIME_MAX;
    if (when > now)
        s->idle += when - now;
}

bool uam_sched_check(const uam_sched *s)
{
    /* Floyd's cycle check, then order and flag consistency. */
    const uam_timer *slow = s->head, *fast = s->head;
    while (fast && fast->next) {
        slow = slow->next;
        fast = fast->next->next;
        if (slow == fast)
            return false;
    }
    for (const uam_timer *t = s->head; t; t = t->next) {
        if (!t->armed)
            return false;
        if (t->next && t->next->when < t->when)
            return false;
    }
    return true;
}

unsigned uam_sched_count(const uam_sched *s)
{
    unsigned n = 0;
    for (const uam_timer *t = s->head; t; t = t->next)
        n++;
    return n;
}
