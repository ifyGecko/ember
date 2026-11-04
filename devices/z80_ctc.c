/*
 * z80_ctc.c - Z80 CTC (uam/dev/z80_ctc.h).
 */
#include <stdlib.h>

#include "uam/dev/z80_ctc.h"

enum { C_CTRL = 0x01, C_RESET = 0x02, C_TC = 0x04, C_TRIG = 0x08, C_P256 = 0x20, C_COUNTER = 0x40,
       C_IE = 0x80 };

typedef struct ctc_chan {
    uint8_t   ctrl;
    uint16_t  tc;          /* time constant, 1-256 */
    uint16_t  count;       /* counter mode: the down-counter */
    bool      want_tc;     /* the next write is the time constant */
    bool      running;     /* timer mode: counting since t0 */
    bool      armed;       /* timer mode with a trigger start: waiting for the edge */
    uint64_t  t0;          /* timer mode: the period started */
    bool      ip, ius;     /* interrupt pending, in service */
    uam_timer t;
} ctc_chan;

typedef struct z80_ctc {
    uam_device dev;
    uam_sched *sched;
    uam_signal irq;
    uint8_t    vector;
    ctc_chan   ch[4];
} z80_ctc;

static z80_ctc *to_ctc(uam_device *d) { return UAM_CONTAINER_OF(d, z80_ctc, dev); }

static unsigned presc(const ctc_chan *c) { return (c->ctrl & C_P256) ? 256u : 16u; }

/* The channel the daisy chain lets interrupt, or -1. */
static int requester(const z80_ctc *t)
{
    for (int i = 0; i < 4; i++) {
        if (t->ch[i].ius)
            return -1;
        if (t->ch[i].ip)
            return i;
    }
    return -1;
}

static void update_irq(z80_ctc *t) { uam_signal_set(&t->irq, requester(t) >= 0); }

static void zero(z80_ctc *t, ctc_chan *c)
{
    if (c->ctrl & C_IE) {
        c->ip = true;
        update_irq(t);
    }
}

static void start_timer(z80_ctc *t, ctc_chan *c, uint64_t when)
{
    c->running = true;
    c->armed = false;
    c->t0 = when;
    uam_timer_arm(t->sched, &c->t, when + (uint64_t)c->tc * presc(c));
}

static void chan_fired(z80_ctc *t, unsigned i)
{
    ctc_chan *c = &t->ch[i];
    start_timer(t, c, c->t0 + (uint64_t)c->tc * presc(c));
    zero(t, c);
}
static void fired0(void *ctx) { chan_fired(ctx, 0); }
static void fired1(void *ctx) { chan_fired(ctx, 1); }
static void fired2(void *ctx) { chan_fired(ctx, 2); }
static void fired3(void *ctx) { chan_fired(ctx, 3); }

static uint8_t chan_read(z80_ctc *t, ctc_chan *c)
{
    if (c->ctrl & C_COUNTER)
        return (uint8_t)c->count;
    if (!c->running)
        return (uint8_t)c->tc;
    const uint64_t done = (uam_sched_now(t->sched) - c->t0) / presc(c);
    return (uint8_t)(c->tc - (done % c->tc));
}

static void chan_write(z80_ctc *t, unsigned i, uint8_t v)
{
    ctc_chan *c = &t->ch[i];
    if (c->want_tc) {
        c->want_tc = false;
        c->tc = v ? v : 256;
        c->count = c->tc;
        if (!(c->ctrl & C_COUNTER)) {
            if (c->ctrl & C_TRIG) {
                c->armed = true;
            } else {
                uam_timer_cancel(t->sched, &c->t);
                start_timer(t, c, uam_sched_now(t->sched));
            }
        }
        return;
    }
    if (!(v & C_CTRL)) {
        if (i == 0)
            t->vector = v & 0xF8;
        return;
    }
    c->ctrl = v;
    c->want_tc = (v & C_TC) != 0;
    if (!(v & C_IE))
        c->ip = false;
    if (v & C_RESET) {
        uam_timer_cancel(t->sched, &c->t);
        c->running = c->armed = false;
    }
    update_irq(t);
}

static uam_status ctc_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    (void)size;
    z80_ctc *t = to_ctc(d);
    *val = off < 4 ? chan_read(t, &t->ch[off]) : 0;
    return UAM_OK;
}

static uam_status ctc_write(uam_device *d, uint64_t off, unsigned size, uint64_t val)
{
    (void)size;
    if (off < 4)
        chan_write(to_ctc(d), (unsigned)off, (uint8_t)val);
    return UAM_OK;
}

static void ctc_reset(uam_device *d)
{
    z80_ctc *t = to_ctc(d);
    for (unsigned i = 0; i < 4; i++) {
        ctc_chan *c = &t->ch[i];
        uam_timer_cancel(t->sched, &c->t);
        c->ctrl = C_RESET;
        c->tc = 256;
        c->count = 0;
        c->want_tc = c->running = c->armed = c->ip = c->ius = false;
        c->t0 = 0;
    }
    t->vector = 0;
    update_irq(t);
}

static void ctc_destroy(uam_device *d)
{
    z80_ctc *t = to_ctc(d);
    for (unsigned i = 0; i < 4; i++)
        uam_timer_cancel(t->sched, &t->ch[i].t);
    free(t);
}

static void ctc_save(uam_device *d, uam_state_writer *w)
{
    z80_ctc *t = to_ctc(d);
    uam_state_put_u32(w, t->vector);
    for (unsigned i = 0; i < 4; i++) {
        const ctc_chan *c = &t->ch[i];
        const uint8_t b[] = { c->ctrl, c->want_tc, c->running, c->armed, c->ip, c->ius };
        uam_state_put_bytes(w, b, sizeof(b));
        uam_state_put_u32(w, c->tc);
        uam_state_put_u32(w, c->count);
        uam_state_put_u64(w, c->t0);
        uam_state_put_timer(w, &c->t);
    }
    uam_state_put_signal(w, &t->irq);
}

static void ctc_restore(uam_device *d, uam_state_reader *r)
{
    z80_ctc *t = to_ctc(d);
    t->vector = (uint8_t)uam_state_get_u32(r);
    for (unsigned i = 0; i < 4; i++) {
        ctc_chan *c = &t->ch[i];
        uint8_t b[6];
        uam_state_get_bytes(r, b, sizeof(b));
        c->ctrl = b[0]; c->want_tc = b[1] != 0; c->running = b[2] != 0; c->armed = b[3] != 0;
        c->ip = b[4] != 0; c->ius = b[5] != 0;
        c->tc = (uint16_t)uam_state_get_u32(r);
        if (c->tc == 0 || c->tc > 256)
            c->tc = 256;
        c->count = (uint16_t)uam_state_get_u32(r);
        c->t0 = uam_state_get_u64(r);
        uam_state_get_timer(r, &c->t);
    }
    uam_state_get_signal(r, &t->irq);
}

static const uam_device_ops ctc_ops = {
    .type = "zilog,z80-ctc", .impl_min = 1, .impl_max = 1,
    .read = ctc_read, .peek = ctc_read, .write = ctc_write,
    .reset = ctc_reset, .destroy = ctc_destroy, .save = ctc_save, .restore = ctc_restore,
};

uam_device *uam_z80_ctc_create(const char *name, uam_sched *sched)
{
    if (!sched)
        return NULL;
    z80_ctc *t = calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    uam_device_init(&t->dev, &ctc_ops, name);
    t->sched = sched;
    static void (*const fns[4])(void *) = { fired0, fired1, fired2, fired3 };
    for (unsigned i = 0; i < 4; i++) {
        uam_timer_init(&t->ch[i].t, fns[i], t);
        t->ch[i].tc = 256;
        t->ch[i].ctrl = C_RESET;
    }
    uam_signal_init(&t->irq);
    return &t->dev;
}

uam_signal *uam_z80_ctc_irq(uam_device *d) { return &to_ctc(d)->irq; }

bool uam_z80_ctc_pending(uam_device *d) { return requester(to_ctc(d)) >= 0; }

uint8_t uam_z80_ctc_ack(uam_device *d)
{
    z80_ctc *t = to_ctc(d);
    const int i = requester(t);
    if (i < 0)
        return 0xFF;
    t->ch[i].ip = false;
    t->ch[i].ius = true;
    update_irq(t);
    return (uint8_t)(t->vector | (unsigned)i << 1);
}

void uam_z80_ctc_reti(uam_device *d)
{
    z80_ctc *t = to_ctc(d);
    for (unsigned i = 0; i < 4; i++)
        if (t->ch[i].ius) {
            t->ch[i].ius = false;
            break;
        }
    update_irq(t);
}

void uam_z80_ctc_trigger(uam_device *d, unsigned ch)
{
    z80_ctc *t = to_ctc(d);
    if (ch >= 4)
        return;
    ctc_chan *c = &t->ch[ch];
    if (c->ctrl & C_RESET)
        return;
    if (c->ctrl & C_COUNTER) {
        if (--c->count == 0) {
            c->count = c->tc;
            zero(t, c);
        }
    } else if (c->armed) {
        start_timer(t, c, uam_sched_now(t->sched));
    }
}
