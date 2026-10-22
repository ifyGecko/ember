/*
 * hpet.c - High Precision Event Timer (uam/dev/hpet.h).
 *
 * The main counter is derived from virtual time while enabled. Each enabled
 * comparator arms a scheduler timer for the exact counter value it matches;
 * a periodic comparator then advances by its period. Level-triggered
 * interrupts hold their line until software clears the timer's status bit.
 */
#include <stdlib.h>

#include "uam/dev/clock.h"
#include "uam/dev/hpet.h"

#define PERIOD_FS 69841279u   /* femtoseconds per tick at 14.31818 MHz */

enum { CFG_ENABLE = 1u << 0, CFG_LEGACY = 1u << 1 };
enum {
    TN_LEVEL = 1u << 1, TN_INT_ENB = 1u << 2, TN_PERIODIC = 1u << 3, TN_PER_CAP = 1u << 4,
    TN_SIZE_CAP = 1u << 5, TN_VAL_SET = 1u << 6, TN_32BIT = 1u << 8,
};
#define TN_ROUTE_SHIFT 9
#define TN_RW_MASK (TN_LEVEL | TN_INT_ENB | TN_PERIODIC | TN_VAL_SET | TN_32BIT | (0x1Fu << TN_ROUTE_SHIFT))

typedef struct hpet_timer {
    uint64_t  config, cmp, period;
    uam_timer t;
    int       line;          /* the line it drives (pin or IRQ), -1 none */
    bool      line_legacy;   /* `line` is an ISA IRQ (legacy replacement) */
    bool      level_out;     /* its output level */
    struct hpet *h;
    unsigned  idx;
} hpet_timer;

typedef struct hpet {
    uam_device dev;
    uam_sched *sched;
    uint64_t   tick_hz;
    uint64_t   gen_conf, isr;
    uint64_t   counter_base;   /* counter value at counter_start (while enabled) or frozen */
    uint64_t   counter_start;  /* device cycle the counter last started */
    hpet_timer tm[UAM_HPET_TIMERS];
    uam_signal_sink route, legacy;
    void      *route_ctx, *legacy_ctx;
    uint32_t   route_cap;
    uint16_t   vendor;         /* VENDOR_ID field of the capabilities register */
} hpet;

static hpet *from_dev(uam_device *d) { return UAM_CONTAINER_OF(d, hpet, dev); }

static uint64_t cycles(hpet *h)
{
    return uam_clock_from_ticks(uam_sched_now(h->sched), h->tick_hz, UAM_HPET_HZ);
}

static uint64_t counter(hpet *h)
{
    if (!(h->gen_conf & CFG_ENABLE))
        return h->counter_base;
    return h->counter_base + (cycles(h) - h->counter_start);
}

/* ---- interrupt lines ----------------------------------------------------- */

static int timer_line(hpet *h, unsigned i, bool *legacy)
{
    *legacy = false;
    if ((h->gen_conf & CFG_LEGACY) && i < 2) {
        *legacy = true;
        return i == 0 ? 0 : 8;
    }
    return (int)((h->tm[i].config >> TN_ROUTE_SHIFT) & 0x1F);
}

static void drive(hpet *h, unsigned i, bool level)
{
    hpet_timer *t = &h->tm[i];
    bool legacy;
    int line = timer_line(h, i, &legacy);
    if (t->level_out && (line != t->line || legacy != t->line_legacy || !level)) {
        uam_signal_sink os = t->line_legacy ? h->legacy : h->route;
        if (os)
            os(t->line_legacy ? h->legacy_ctx : h->route_ctx, (unsigned)t->line, 0);
        t->level_out = false;
    }
    t->line = line;
    t->line_legacy = legacy;
    uam_signal_sink sink = legacy ? h->legacy : h->route;
    if (level && !t->level_out && sink) {
        sink(legacy ? h->legacy_ctx : h->route_ctx, (unsigned)line, 1);
        t->level_out = true;
    }
}

static void fire(hpet *h, unsigned i)
{
    hpet_timer *t = &h->tm[i];
    if (!(t->config & TN_INT_ENB))
        return;
    if (t->config & TN_LEVEL) {
        h->isr |= 1ull << i;
        drive(h, i, true);
    } else {
        drive(h, i, true);   /* an edge */
        drive(h, i, false);
    }
}

/* ---- comparators --------------------------------------------------------- */

static uint64_t width_mask(const hpet_timer *t)
{
    return (t->config & TN_32BIT) ? 0xFFFFFFFFull : ~0ull;
}

static void arm(hpet *h, unsigned i)
{
    hpet_timer *t = &h->tm[i];
    uam_timer_cancel(h->sched, &t->t);
    if (!(h->gen_conf & CFG_ENABLE))
        return;
    uint64_t m = width_mask(t);
    uint64_t now = counter(h);
    uint64_t delta = (t->cmp - now) & m;
    if (delta == 0)
        delta = m == ~0ull ? ~0ull : m + 1;   /* a full wrap away */
    uint64_t now_c = cycles(h);
    if (delta > UINT64_MAX - now_c)
        return;                               /* beyond the end of virtual time */
    uint64_t at = uam_clock_to_ticks(now_c + delta, h->tick_hz, UAM_HPET_HZ);
    if (at != UINT64_MAX)
        uam_timer_arm(h->sched, &t->t, at);
}

static void timer_fired(void *ctx)
{
    hpet_timer *t = ctx;
    hpet *h = t->h;
    fire(h, t->idx);
    if ((t->config & TN_PERIODIC) && t->period)
        t->cmp = (t->cmp + t->period) & width_mask(t);
    arm(h, t->idx);
}

static void rearm_all(hpet *h)
{
    for (unsigned i = 0; i < UAM_HPET_TIMERS; i++)
        arm(h, i);
}

/* ---- registers ----------------------------------------------------------- */

static uint64_t cap_id(const hpet *h)
{
    return (uint64_t)PERIOD_FS << 32 | (uint64_t)h->vendor << 16 | 1u << 15 /* legacy route capable */ |
           1u << 13 /* 64-bit counter */ | (UAM_HPET_TIMERS - 1) << 8 | 0x01;
}

static uint64_t reg_read(hpet *h, uint64_t r)
{
    switch (r) {
    case 0x000: return cap_id(h);
    case 0x010: return h->gen_conf;
    case 0x020: return h->isr;
    case 0x0F0: return counter(h);
    }
    if (r >= 0x100 && r < 0x100 + 0x20 * UAM_HPET_TIMERS) {
        hpet_timer *t = &h->tm[(r - 0x100) / 0x20];
        switch ((r - 0x100) % 0x20) {
        case 0x00: return t->config | (uint64_t)h->route_cap << 32;
        case 0x08: return t->cmp;
        default:   return 0;   /* FSB route: not offered */
        }
    }
    return 0;
}

static void reg_write(hpet *h, uint64_t r, uint64_t v, uint64_t mask)
{
    switch (r) {
    case 0x010: {
        uint64_t old = h->gen_conf;
        uint64_t c = counter(h);
        h->gen_conf = ((old & ~mask) | (v & mask)) & (CFG_ENABLE | CFG_LEGACY);
        if ((old ^ h->gen_conf) & CFG_ENABLE) {
            h->counter_base = c;               /* freeze or restart from here */
            h->counter_start = cycles(h);
        }
        for (unsigned i = 0; i < UAM_HPET_TIMERS; i++)   /* re-route held levels */
            drive(h, i, (h->tm[i].config & TN_LEVEL) && (h->isr >> i & 1));
        rearm_all(h);
        return;
    }
    case 0x020:
        h->isr &= ~(v & mask);
        for (unsigned i = 0; i < UAM_HPET_TIMERS; i++)
            if (!(h->isr >> i & 1) && (h->tm[i].config & TN_LEVEL))
                drive(h, i, false);
        return;
    case 0x0F0:
        if (!(h->gen_conf & CFG_ENABLE)) {   /* writable only while halted */
            h->counter_base = (h->counter_base & ~mask) | (v & mask);
            rearm_all(h);
        }
        return;
    }
    if (r >= 0x100 && r < 0x100 + 0x20 * UAM_HPET_TIMERS) {
        unsigned i = (unsigned)((r - 0x100) / 0x20);
        hpet_timer *t = &h->tm[i];
        switch ((r - 0x100) % 0x20) {
        case 0x00: {
            uint64_t rw = TN_RW_MASK & (i == 0 ? ~0ull : ~(uint64_t)TN_PERIODIC);
            t->config = (t->config & ~(mask & rw)) | (v & mask & rw);
            if (!(t->config & TN_LEVEL)) {
                h->isr &= ~(1ull << i);
            }
            drive(h, i, (t->config & TN_LEVEL) && (h->isr >> i & 1));
            arm(h, i);
            return;
        }
        case 0x08: {
            uint64_t val = (t->cmp & ~mask) | (v & mask);
            if ((t->config & TN_PERIODIC) && !(t->config & TN_VAL_SET)) {
                t->period = val & width_mask(t);   /* writes after VAL_SET set the period */
            } else {
                t->cmp = val & width_mask(t);
                if (t->config & TN_PERIODIC)
                    t->period = t->cmp - (counter(h) & width_mask(t));
            }
            t->config &= ~(uint64_t)TN_VAL_SET;
            arm(h, i);
            return;
        }
        default:
            return;
        }
    }
}

static uam_status hpet_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    hpet *h = from_dev(d);
    uint64_t r = reg_read(h, off & ~7ull);
    *val = (r >> (8 * (off & 7))) & uam_mask(size * 8);
    return UAM_OK;
}

static uam_status hpet_write(uam_device *d, uint64_t off, unsigned size, uint64_t v)
{
    hpet *h = from_dev(d);
    unsigned sh = 8 * (unsigned)(off & 7);
    reg_write(h, off & ~7ull, v << sh, uam_mask(size * 8) << sh);
    return UAM_OK;
}

static void hpet_reset(uam_device *d)
{
    hpet *h = from_dev(d);
    for (unsigned i = 0; i < UAM_HPET_TIMERS; i++) {
        drive(h, i, false);
        uam_timer_cancel(h->sched, &h->tm[i].t);
        h->tm[i].config = TN_SIZE_CAP | (i == 0 ? TN_PER_CAP : 0);
        h->tm[i].cmp = ~0ull;
        h->tm[i].period = 0;
    }
    h->gen_conf = h->isr = 0;
    h->counter_base = 0;
    h->counter_start = cycles(h);
}

static void hpet_destroy(uam_device *d)
{
    hpet *h = from_dev(d);
    for (unsigned i = 0; i < UAM_HPET_TIMERS; i++)
        uam_timer_cancel(h->sched, &h->tm[i].t);
    free(h);
}

static void hpet_save(uam_device *d, uam_state_writer *w)
{
    hpet *h = from_dev(d);
    uam_state_put_u64(w, h->gen_conf);
    uam_state_put_u64(w, h->isr);
    uam_state_put_u64(w, h->counter_base);
    uam_state_put_u64(w, h->counter_start);
    for (unsigned i = 0; i < UAM_HPET_TIMERS; i++) {
        hpet_timer *t = &h->tm[i];
        uam_state_put_u64(w, t->config);
        uam_state_put_u64(w, t->cmp);
        uam_state_put_u64(w, t->period);
        uam_state_put_u32(w, (uint32_t)t->line);
        uam_state_put_bool(w, t->line_legacy);
        uam_state_put_bool(w, t->level_out);
        uam_state_put_timer(w, &t->t);
    }
}

static void hpet_restore(uam_device *d, uam_state_reader *r)
{
    hpet *h = from_dev(d);
    h->gen_conf = uam_state_get_u64(r);
    h->isr = uam_state_get_u64(r);
    h->counter_base = uam_state_get_u64(r);
    h->counter_start = uam_state_get_u64(r);
    for (unsigned i = 0; i < UAM_HPET_TIMERS; i++) {
        hpet_timer *t = &h->tm[i];
        t->config = uam_state_get_u64(r);
        t->cmp = uam_state_get_u64(r);
        t->period = uam_state_get_u64(r);
        t->line = (int)uam_state_get_u32(r);
        t->line_legacy = uam_state_get_bool(r);
        t->level_out = uam_state_get_bool(r);
        uam_state_get_timer(r, &t->t);
    }
}

static const uam_device_ops hpet_ops = {
    .type = "ia-pc,hpet", .impl_min = 4, .impl_max = 8,
    .read = hpet_read, .write = hpet_write, .reset = hpet_reset, .destroy = hpet_destroy,
    .save = hpet_save, .restore = hpet_restore,
};

uam_device *uam_hpet_create(const char *name, uam_sched *sched, uint64_t tick_hz)
{
    hpet *h = calloc(1, sizeof(*h));
    if (!h)
        return NULL;
    uam_device_init(&h->dev, &hpet_ops, name);
    h->sched = sched;
    h->tick_hz = tick_hz ? tick_hz : 1000000000u;
    for (unsigned i = 0; i < UAM_HPET_TIMERS; i++) {
        h->tm[i].h = h;
        h->tm[i].idx = i;
        h->tm[i].line = -1;
        uam_timer_init(&h->tm[i].t, timer_fired, &h->tm[i]);
    }
    hpet_reset(&h->dev);
    return &h->dev;
}

void uam_hpet_set_sinks(uam_device *d, uam_signal_sink route, void *route_ctx,
                        uam_signal_sink legacy, void *legacy_ctx, uint32_t route_cap)
{
    hpet *h = from_dev(d);
    h->route = route;
    h->route_ctx = route_ctx;
    h->legacy = legacy;
    h->legacy_ctx = legacy_ctx;
    h->route_cap = route_cap;
}

bool uam_hpet_legacy(uam_device *d) { return (from_dev(d)->gen_conf & CFG_LEGACY) != 0; }

void uam_hpet_set_vendor(uam_device *dev, uint16_t vendor)
{
    from_dev(dev)->vendor = vendor;
}
