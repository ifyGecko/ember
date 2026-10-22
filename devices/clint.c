/*
 * clint.c - RISC-V ACLINT MSWI + MTIMER for one or more harts (uam/dev/clint.h).
 *
 * mtime is derived from virtual time, so it never needs ticking. The timer
 * line is level-high while mtime >= mtimecmp; a scheduler timer fires at the
 * exact tick the comparison becomes true. All harts share mtime; each has its
 * own msip and mtimecmp.
 */
#include <stdlib.h>

#include "uam/dev/clint.h"

#define OFF_MSIP     0x0000u   /* + 4 * hart */
#define OFF_MTIMECMP 0x4000u   /* + 8 * hart */
#define OFF_MTIME    0xBFF8u

typedef struct clint_hart {
    struct clint *c;
    uam_signal  msip_line, mtip_line;
    uam_timer   timer;
    uint32_t    msip;
    uint64_t    mtimecmp;
} clint_hart;

typedef struct clint {
    uam_device  dev;
    uam_sched  *sched;
    uint64_t    time_offset;  /* mtime = now + time_offset (mtime is writable) */
    unsigned    n_harts;
    clint_hart  hart[];
} clint;

static clint *to_clint(uam_device *d) { return UAM_CONTAINER_OF(d, clint, dev); }

static uint64_t mtime(clint *c) { return uam_sched_now(c->sched) + c->time_offset; }

/* Re-evaluate a hart's timer comparison and (re)arm its wake-up. */
static void update_timer(clint_hart *h)
{
    clint *c = h->c;
    if (mtime(c) >= h->mtimecmp) {
        uam_timer_cancel(c->sched, &h->timer);
        uam_signal_set(&h->mtip_line, 1);
    } else {
        uam_signal_set(&h->mtip_line, 0);
        if (h->mtimecmp == UINT64_MAX)
            uam_timer_cancel(c->sched, &h->timer); /* "never": no wake-up needed */
        else
            uam_timer_arm(c->sched, &h->timer, h->mtimecmp - c->time_offset);
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

/* The hart whose msip (or mtimecmp) register `off` falls in, or NULL. */
static clint_hart *msip_of(clint *c, uint64_t off)
{
    return off < OFF_MSIP + 4u * c->n_harts ? &c->hart[(off - OFF_MSIP) / 4] : NULL;
}

static clint_hart *mtimecmp_of(clint *c, uint64_t off, unsigned size)
{
    if (off < OFF_MTIMECMP || off >= OFF_MTIMECMP + 8u * c->n_harts || size > 8 - (off & 7))
        return NULL;
    return &c->hart[(off - OFF_MTIMECMP) / 8];
}

static uam_status clint_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    clint *c = to_clint(d);
    clint_hart *h;
    if ((h = msip_of(c, off)))
        *val = h->msip;
    else if ((h = mtimecmp_of(c, off, size)))
        *val = read64(h->mtimecmp, off, size);
    else if (off >= OFF_MTIME && off < OFF_MTIME + 8 && size <= 8 - (off & 7))
        *val = read64(mtime(c), off, size);
    else
        *val = 0;
    return UAM_OK;
}

static uam_status clint_write(uam_device *d, uint64_t off, unsigned size, uint64_t v)
{
    clint *c = to_clint(d);
    clint_hart *h;
    if ((h = msip_of(c, off))) {
        h->msip = (uint32_t)v & 1;
        uam_signal_set(&h->msip_line, (int)h->msip);
    } else if ((h = mtimecmp_of(c, off, size))) {
        h->mtimecmp = write64(h->mtimecmp, off, size, v);
        update_timer(h);
    } else if (off >= OFF_MTIME && off < OFF_MTIME + 8 && size <= 8 - (off & 7)) {
        uint64_t t = write64(mtime(c), off, size, v);
        c->time_offset = t - uam_sched_now(c->sched);
        for (unsigned i = 0; i < c->n_harts; i++)
            update_timer(&c->hart[i]);
    }
    return UAM_OK;
}

static void clint_reset(uam_device *d)
{
    clint *c = to_clint(d);
    c->time_offset = 0;
    for (unsigned i = 0; i < c->n_harts; i++) {
        clint_hart *h = &c->hart[i];
        h->msip = 0;
        h->mtimecmp = UINT64_MAX;
        uam_signal_set(&h->msip_line, 0);
        update_timer(h);
    }
}

static void clint_destroy(uam_device *d)
{
    clint *c = to_clint(d);
    for (unsigned i = 0; i < c->n_harts; i++)
        uam_timer_cancel(c->sched, &c->hart[i].timer);
    free(c);
}

/* Hart 0 first, as a one-hart CLINT always saved itself, then the others. */
static void save_hart(clint_hart *h, uam_state_writer *w, bool regs)
{
    if (regs) {
        uam_state_put_u32(w, h->msip);
        uam_state_put_u64(w, h->mtimecmp);
        return;
    }
    uam_state_put_timer(w, &h->timer);
    uam_state_put_signal(w, &h->msip_line);
    uam_state_put_signal(w, &h->mtip_line);
}

static void clint_save(uam_device *d, uam_state_writer *w)
{
    clint *c = to_clint(d);
    save_hart(&c->hart[0], w, true);
    uam_state_put_u64(w, c->time_offset);
    save_hart(&c->hart[0], w, false);
    for (unsigned i = 1; i < c->n_harts; i++) {
        save_hart(&c->hart[i], w, true);
        save_hart(&c->hart[i], w, false);
    }
}

static void restore_hart(clint_hart *h, uam_state_reader *r, bool regs)
{
    if (regs) {
        h->msip = uam_state_get_u32(r);
        h->mtimecmp = uam_state_get_u64(r);
        return;
    }
    uam_state_get_timer(r, &h->timer);
    uam_state_get_signal(r, &h->msip_line);
    uam_state_get_signal(r, &h->mtip_line);
}

static void clint_restore(uam_device *d, uam_state_reader *r)
{
    clint *c = to_clint(d);
    restore_hart(&c->hart[0], r, true);
    c->time_offset = uam_state_get_u64(r);
    restore_hart(&c->hart[0], r, false);
    for (unsigned i = 1; i < c->n_harts; i++) {
        restore_hart(&c->hart[i], r, true);
        restore_hart(&c->hart[i], r, false);
    }
}

static const uam_device_ops clint_ops = {
    .type = "riscv,aclint",
    .read = clint_read,
    .write = clint_write,
    .reset = clint_reset,
    .destroy = clint_destroy,
    .save = clint_save,
    .restore = clint_restore,
};

uam_device *uam_clint_create_harts(const char *name, uam_sched *sched, unsigned n_harts)
{
    if (n_harts < 1 || n_harts > UAM_CLINT_MAX_HARTS)
        return NULL;
    clint *c = calloc(1, sizeof(*c) + n_harts * sizeof(clint_hart));
    if (!c)
        return NULL;
    uam_device_init(&c->dev, &clint_ops, name);
    c->sched = sched;
    c->n_harts = n_harts;
    for (unsigned i = 0; i < n_harts; i++) {
        clint_hart *h = &c->hart[i];
        h->c = c;
        uam_signal_init(&h->msip_line);
        uam_signal_init(&h->mtip_line);
        uam_timer_init(&h->timer, timer_fired, h);
        h->mtimecmp = UINT64_MAX;
    }
    return &c->dev;
}

uam_device *uam_clint_create(const char *name, uam_sched *sched)
{
    return uam_clint_create_harts(name, sched, 1);
}

uam_signal *uam_clint_hart_msip_line(uam_device *d, unsigned hart)
{
    clint *c = to_clint(d);
    return hart < c->n_harts ? &c->hart[hart].msip_line : NULL;
}

uam_signal *uam_clint_hart_mtip_line(uam_device *d, unsigned hart)
{
    clint *c = to_clint(d);
    return hart < c->n_harts ? &c->hart[hart].mtip_line : NULL;
}

uam_signal *uam_clint_msip_line(uam_device *d) { return uam_clint_hart_msip_line(d, 0); }
uam_signal *uam_clint_mtip_line(uam_device *d) { return uam_clint_hart_mtip_line(d, 0); }
