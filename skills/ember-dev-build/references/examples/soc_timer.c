/*
 * soc_timer.c - <SoC> timer @ phys 0x........: a free-running 32-bit up-counter with one
 * compare-match interrupt. Driver: sysClkInt 0x........ acks STATUS, adds the period to
 * COMPARE. The counter is derived from virtual time, so it never needs ticking: ember
 * fires the scheduler timer at the exact tick the match happens.
 */
#include <stdlib.h>

#include "soc_timer.h"

#define R_COUNT   0x0u
#define R_COMPARE 0x4u
#define R_CTRL    0x8u
#define  CTRL_IRQ_EN  (1u << 1)
#define R_STATUS  0xcu         /* W1C */
#define  ST_MATCH     (1u << 0)

typedef struct soc_timer {
    uam_device  dev;
    uam_sched  *sched;
    uam_signal  irq;
    uam_timer   match;          /* initialised once at create, re-armed, never re-initialised */
    uint64_t    ticks_per_count;
    uint64_t    base;           /* tick at which COUNT was 0 */
    uint32_t    compare, ctrl, status;
} soc_timer;

static soc_timer *to_timer(uam_device *d) { return UAM_CONTAINER_OF(d, soc_timer, dev); }

static uint64_t count64(soc_timer *t)
{
    return (uam_sched_now(t->sched) - t->base) / t->ticks_per_count;
}

static void update(soc_timer *t)
{
    uam_signal_set(&t->irq, (t->status & ST_MATCH) && (t->ctrl & CTRL_IRQ_EN));
}

/* Arm the wake-up for the next COUNT == COMPARE (modulo 2^32). */
static void arm(soc_timer *t)
{
    uint64_t c = count64(t);
    uint32_t delta = t->compare - (uint32_t)c;
    uint64_t steps = delta ? delta : UINT64_C(1) << 32;
    uam_timer_arm(t->sched, &t->match, t->base + (c + steps) * t->ticks_per_count);
}

/* One-shot: the driver re-programs COMPARE for the next tick. The hardware would match
 * again after a full wrap, but an armed timer lets an idle hart skip ahead to it; re-arming
 * here would turn "interrupt not wired" into a stall that ember only ends after --max-stall
 * timer events (exit 128, livelock) instead of an immediate, clean deadlock (exit 125). */
static void match_fired(void *ctx)
{
    soc_timer *t = ctx;
    t->status |= ST_MATCH;
    update(t);
}

static uam_status timer_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    soc_timer *t = to_timer(d);
    (void)size;
    switch (off) {
    case R_COUNT:   *val = (uint32_t)count64(t); break;
    case R_COMPARE: *val = t->compare; break;
    case R_CTRL:    *val = t->ctrl; break;
    case R_STATUS:  *val = t->status; break;
    default:        *val = 0; break;
    }
    return UAM_OK;
}

static uam_status timer_write(uam_device *d, uint64_t off, unsigned size, uint64_t v)
{
    soc_timer *t = to_timer(d);
    (void)size;
    switch (off) {
    case R_COMPARE: t->compare = (uint32_t)v; arm(t); break;
    case R_CTRL:    t->ctrl = (uint32_t)v; break;
    case R_STATUS:  t->status &= ~(uint32_t)v; break;
    default: break;
    }
    update(t);
    return UAM_OK;
}

static void timer_reset(uam_device *d)
{
    soc_timer *t = to_timer(d);
    t->base = uam_sched_now(t->sched);
    t->compare = UINT32_MAX;
    t->ctrl = t->status = 0;
    uam_timer_cancel(t->sched, &t->match);   /* nothing armed until the guest asks */
    update(t);
}

static void timer_destroy(uam_device *d)
{
    soc_timer *t = to_timer(d);
    uam_timer_cancel(t->sched, &t->match);
    free(t);
}

/* Snapshots: the time base is an absolute tick and virtual time is restored with the
 * machine, so COUNT carries on exactly. The armed timer must be saved by its owner (taking
 * a snapshot fails otherwise); restore re-arms it in its original order among ties. */
static void timer_save(uam_device *d, uam_state_writer *w)
{
    soc_timer *t = to_timer(d);
    uam_state_put_u64(w, t->base);
    uam_state_put_u32(w, t->compare);
    uam_state_put_u32(w, t->ctrl);
    uam_state_put_u32(w, t->status);
    uam_state_put_timer(w, &t->match);
    uam_state_put_signal(w, &t->irq);
}

static void timer_restore(uam_device *d, uam_state_reader *r)
{
    soc_timer *t = to_timer(d);
    t->base = uam_state_get_u64(r);
    t->compare = uam_state_get_u32(r);
    t->ctrl = uam_state_get_u32(r);
    t->status = uam_state_get_u32(r);
    uam_state_get_timer(r, &t->match);
    uam_state_get_signal(r, &t->irq);
}

static const uam_device_ops soc_timer_ops = {
    .type = "soc-timer",
    .endian = UAM_ACC_BE,
    .impl_min = 4, .impl_max = 4,
    .read = timer_read,
    .write = timer_write,
    .reset = timer_reset,
    .destroy = timer_destroy,
    .save = timer_save,
    .restore = timer_restore,
};

uam_device *soc_timer_create(const char *name, uam_sched *sched, uint64_t ticks_per_count)
{
    soc_timer *t = calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    uam_device_init(&t->dev, &soc_timer_ops, name);
    t->sched = sched;
    t->ticks_per_count = ticks_per_count ? ticks_per_count : 1;
    uam_signal_init(&t->irq);
    uam_timer_init(&t->match, match_fired, t);
    return &t->dev;
}

uam_signal *soc_timer_irq_line(uam_device *d) { return &to_timer(d)->irq; }
