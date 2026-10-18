/*
 * clint.c - SiFive-compatible CLINT for one hart (uam/dev/clint.h).
 *
 * mtime is derived from virtual time, so it never needs ticking. The timer
 * line is level-high while mtime >= mtimecmp; a scheduler timer fires at the
 * exact tick the comparison becomes true.
 */
#include <stdlib.h>

#include "uam/dev/clint.h"

#define OFF_MSIP     0x0000u
#define OFF_MTIMECMP 0x4000u
#define OFF_MTIME    0xBFF8u

typedef struct clint {
    uam_device  dev;
    uam_sched  *sched;
    uam_signal  msip_line, mtip_line;
    uam_timer   timer;
    uint32_t    msip;
    uint64_t    mtimecmp;
    uint64_t    time_offset;  /* mtime = now + time_offset (mtime is writable) */
} clint;

static clint *to_clint(uam_device *d) { return UAM_CONTAINER_OF(d, clint, dev); }

static uint64_t mtime(clint *c) { return uam_sched_now(c->sched) + c->time_offset; }

/* Re-evaluate the timer comparison and (re)arm the wake-up. */
static void update_timer(clint *c)
{
    if (mtime(c) >= c->mtimecmp) {
        uam_timer_cancel(c->sched, &c->timer);
        uam_signal_set(&c->mtip_line, 1);
    } else {
        uam_signal_set(&c->mtip_line, 0);
        if (c->mtimecmp == UINT64_MAX)
            uam_timer_cancel(c->sched, &c->timer); /* "never": no wake-up needed */
        else
            uam_timer_arm(c->sched, &c->timer, c->mtimecmp - c->time_offset);
    }
}

static void timer_fired(void *ctx) { update_timer(ctx); }

/* Accesses to the 64-bit registers may be 4 or 8 bytes wide. */
static uint64_t read64(uint64_t reg, uint64_t off, unsigned size)
{
    return (reg >> ((off & 7) * 8)) & uam_mask(size * 8);
}

static uint64_t write64(uint64_t reg, uint64_t off, unsigned size, uint64_t v)
{
    unsigned shift = (unsigned)(off & 7) * 8;
    uint64_t m = uam_mask(size * 8) << shift;
    return (reg & ~m) | ((v << shift) & m);
}

static uam_status clint_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    clint *c = to_clint(d);
    if (off < OFF_MSIP + 4)
        *val = c->msip;
    else if (off >= OFF_MTIMECMP && off < OFF_MTIMECMP + 8 && size <= 8 - (off & 7))
        *val = read64(c->mtimecmp, off, size);
    else if (off >= OFF_MTIME && off < OFF_MTIME + 8 && size <= 8 - (off & 7))
        *val = read64(mtime(c), off, size);
    else
        *val = 0;
    return UAM_OK;
}

static uam_status clint_write(uam_device *d, uint64_t off, unsigned size, uint64_t v)
{
    clint *c = to_clint(d);
    if (off < OFF_MSIP + 4) {
        c->msip = (uint32_t)v & 1;
        uam_signal_set(&c->msip_line, (int)c->msip);
    } else if (off >= OFF_MTIMECMP && off < OFF_MTIMECMP + 8 && size <= 8 - (off & 7)) {
        c->mtimecmp = write64(c->mtimecmp, off, size, v);
        update_timer(c);
    } else if (off >= OFF_MTIME && off < OFF_MTIME + 8 && size <= 8 - (off & 7)) {
        uint64_t t = write64(mtime(c), off, size, v);
        c->time_offset = t - uam_sched_now(c->sched);
        update_timer(c);
    }
    return UAM_OK;
}

static void clint_reset(uam_device *d)
{
    clint *c = to_clint(d);
    c->msip = 0;
    c->mtimecmp = UINT64_MAX;
    c->time_offset = 0;
    uam_signal_set(&c->msip_line, 0);
    update_timer(c);
}

static void clint_destroy(uam_device *d)
{
    clint *c = to_clint(d);
    uam_timer_cancel(c->sched, &c->timer);
    free(c);
}

static void clint_save(uam_device *d, uam_state_writer *w)
{
    clint *c = to_clint(d);
    uam_state_put_u32(w, c->msip);
    uam_state_put_u64(w, c->mtimecmp);
    uam_state_put_u64(w, c->time_offset);
    uam_state_put_timer(w, &c->timer);
    uam_state_put_signal(w, &c->msip_line);
    uam_state_put_signal(w, &c->mtip_line);
}

static void clint_restore(uam_device *d, uam_state_reader *r)
{
    clint *c = to_clint(d);
    c->msip = uam_state_get_u32(r);
    c->mtimecmp = uam_state_get_u64(r);
    c->time_offset = uam_state_get_u64(r);
    uam_state_get_timer(r, &c->timer);
    uam_state_get_signal(r, &c->msip_line);
    uam_state_get_signal(r, &c->mtip_line);
}

static const uam_device_ops clint_ops = {
    .type = "sifive,clint0",
    .read = clint_read,
    .write = clint_write,
    .reset = clint_reset,
    .destroy = clint_destroy,
    .save = clint_save,
    .restore = clint_restore,
};

uam_device *uam_clint_create(const char *name, uam_sched *sched)
{
    clint *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    uam_device_init(&c->dev, &clint_ops, name);
    c->sched = sched;
    uam_signal_init(&c->msip_line);
    uam_signal_init(&c->mtip_line);
    uam_timer_init(&c->timer, timer_fired, c);
    c->mtimecmp = UINT64_MAX;
    return &c->dev;
}

uam_signal *uam_clint_msip_line(uam_device *d) { return &to_clint(d)->msip_line; }
uam_signal *uam_clint_mtip_line(uam_device *d) { return &to_clint(d)->mtip_line; }
