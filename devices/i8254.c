/*
 * i8254.c - 8254 programmable interval timer and port 61h (uam/dev/i8254.h).
 *
 * Counters are derived from virtual time rather than ticked: a loaded
 * counter remembers the device cycle it started at, and its value and output
 * follow from the cycles elapsed since. Counter 0's interrupt edges (the end
 * of each period in modes 2 and 3, terminal count in modes 0 and 4) come from
 * a scheduler timer armed for the exact cycle.
 *
 * Modes 0-5, the latch and read-back commands, and the LSB/MSB access
 * sequences are implemented. Mode 3 counts by one per cycle when read (the
 * output timing is exact). BCD counting is not implemented.
 */
#include <stdlib.h>

#include "uam/dev/clock.h"
#include "uam/dev/i8254.h"

typedef struct counter {
    uint8_t  mode, rw, bcd;
    bool     loaded;       /* a count has been written since the control word */
    bool     running;      /* counting (loaded, gated, and triggered for modes 1/5) */
    uint32_t n;            /* initial count, 1..65536 */
    uint64_t start;        /* device cycle counting began */
    uint64_t frozen;       /* cycles elapsed when the gate stopped a mode 0/4 count */
    bool     gate;
    uint8_t  wbyte;        /* LSB/MSB write state: 0 = next is LSB */
    uint16_t wlsb;
    uint8_t  rbyte;        /* LSB/MSB read state */
    bool     latched;
    uint16_t latch;
    uint8_t  latch_bytes;  /* bytes of the latch still to read */
    bool     status_latched;
    uint8_t  status;
    uint8_t  speaker;      /* port 61h bit 1 (counter 2 only) */
} counter;

typedef struct i8254 {
    uam_device dev;
    uam_device p61;
    uam_sched *sched;
    uint64_t   tick_hz;
    uam_signal irq;
    uam_timer  timer;      /* counter 0 output edges */
    counter    c[3];
} i8254;

static i8254 *from_dev(uam_device *d) { return UAM_CONTAINER_OF(d, i8254, dev); }
static i8254 *from_p61(uam_device *d) { return UAM_CONTAINER_OF(d, i8254, p61); }

static uint64_t cycles_now(i8254 *p)
{
    return uam_clock_from_ticks(uam_sched_now(p->sched), p->tick_hz, UAM_I8254_HZ);
}

static uint64_t elapsed(i8254 *p, const counter *c)
{
    if (!c->running)
        return c->frozen;
    return cycles_now(p) - c->start;
}

static uint16_t value(i8254 *p, const counter *c)
{
    if (!c->loaded)
        return 0;
    uint64_t e = elapsed(p, c);
    switch (c->mode) {
    case 2: case 3:
        return (uint16_t)(c->n - (e % c->n));
    default:
        return (uint16_t)((c->n - e) & 0xFFFF);
    }
}

static bool out(i8254 *p, const counter *c)
{
    if (!c->loaded)
        return c->mode != 0;
    uint64_t e = elapsed(p, c);
    switch (c->mode) {
    case 0: return e >= c->n;
    case 1: return !c->running || e >= c->n;
    case 2: return !c->running || (e % c->n) != c->n - 1;
    case 3: return !c->running || (e % c->n) < (c->n + 1) / 2;
    default: return !c->running || e != c->n;   /* 4, 5: one-cycle strobe */
    }
}

/* Counter 0: set the line for the current state and arm the next edge. */
static void update_irq(i8254 *p)
{
    counter *c = &p->c[0];
    uam_timer_cancel(p->sched, &p->timer);
    if (!c->loaded || !c->running) {
        uam_signal_set(&p->irq, out(p, c));
        return;
    }
    uint64_t e = elapsed(p, c), next;
    switch (c->mode) {
    case 2: case 3:
        uam_signal_set(&p->irq, 1);
        next = c->start + (e / c->n + 1) * c->n;
        break;
    case 0: case 1:
        uam_signal_set(&p->irq, e >= c->n);
        if (e >= c->n)
            return;
        next = c->start + c->n;
        break;
    default:
        uam_signal_set(&p->irq, 1);
        if (e >= c->n)
            return;
        next = c->start + c->n;
        break;
    }
    uam_timer_arm(p->sched, &p->timer, uam_clock_to_ticks(next, p->tick_hz, UAM_I8254_HZ));
}

static void timer_fired(void *ctx)
{
    i8254 *p = ctx;
    counter *c = &p->c[0];
    if (c->mode != 0 && c->mode != 1)
        uam_signal_set(&p->irq, 0);   /* the period's low pulse: a rising edge follows */
    update_irq(p);
}

static void start_count(i8254 *p, counter *c)
{
    c->start = cycles_now(p);
    c->frozen = 0;
    /* modes 1 and 5 wait for a gate edge; the others need the gate high */
    c->running = c->gate && c->mode != 1 && c->mode != 5;
}

static void set_gate(i8254 *p, counter *c, bool g)
{
    if (g == c->gate)
        return;
    if (!g) {
        if (c->running && (c->mode == 0 || c->mode == 4))
            c->frozen = elapsed(p, c);
        else if (c->mode == 2 || c->mode == 3)
            c->frozen = 0;
        if (c->mode != 1 && c->mode != 5)
            c->running = false;
    } else if (c->loaded) {
        if (c->mode == 0 || c->mode == 4) {
            c->start = cycles_now(p) - c->frozen;   /* resume */
        } else {
            c->start = cycles_now(p);               /* retrigger / reload */
            c->frozen = 0;
        }
        c->running = true;
    }
    c->gate = g;
}

static void control(i8254 *p, uint8_t v)
{
    unsigned sc = v >> 6;
    if (sc == 3) {                         /* read-back */
        for (unsigned i = 0; i < 3; i++) {
            if (!(v & (2u << i)))
                continue;
            counter *c = &p->c[i];
            if (!(v & 0x20) && !c->latched) {
                c->latched = true;
                c->latch = value(p, c);
                c->latch_bytes = c->rw == 3 ? 2 : 1;
            }
            if (!(v & 0x10) && !c->status_latched) {
                c->status_latched = true;
                c->status = (uint8_t)((out(p, c) ? 0x80 : 0) | (c->loaded ? 0 : 0x40) |
                                      c->rw << 4 | c->mode << 1 | c->bcd);
            }
        }
        return;
    }
    counter *c = &p->c[sc];
    unsigned rw = (v >> 4) & 3;
    if (rw == 0) {                         /* counter latch */
        if (!c->latched) {
            c->latched = true;
            c->latch = value(p, c);
            c->latch_bytes = c->rw == 3 ? 2 : 1;
        }
        return;
    }
    c->rw = (uint8_t)rw;
    c->mode = (v >> 1) & 7;
    if (c->mode > 5)
        c->mode -= 4;                      /* 6, 7 are 2, 3 */
    c->bcd = v & 1;
    c->loaded = false;
    c->running = false;
    c->wbyte = c->rbyte = 0;
    c->latched = c->status_latched = false;
    if (sc == 0)
        update_irq(p);
}

static void write_count(i8254 *p, unsigned i, uint8_t v)
{
    counter *c = &p->c[i];
    uint32_t n;
    switch (c->rw) {
    case 1: n = v; break;
    case 2: n = (uint32_t)v << 8; break;
    default:
        if (c->wbyte == 0) {
            c->wlsb = v;
            c->wbyte = 1;
            return;
        }
        c->wbyte = 0;
        n = c->wlsb | (uint32_t)v << 8;
        break;
    }
    c->n = n ? n : 0x10000;
    if (c->mode == 3 && c->n == 1) c->n = 2;
    c->loaded = true;
    start_count(p, c);
    if (i == 0)
        update_irq(p);
}

static uint8_t read_count(i8254 *p, unsigned i)
{
    counter *c = &p->c[i];
    if (c->status_latched) {
        c->status_latched = false;
        return c->status;
    }
    uint16_t v = c->latched ? c->latch : value(p, c);
    uint8_t b;
    switch (c->rw) {
    case 1: b = (uint8_t)v; break;
    case 2: b = (uint8_t)(v >> 8); break;
    default:
        b = c->rbyte ? (uint8_t)(v >> 8) : (uint8_t)v;
        c->rbyte ^= 1;
        break;
    }
    if (c->latched && (c->rw != 3 || c->rbyte == 0))
        c->latched = false;
    return b;
}

static uam_status pit_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    (void)size;
    i8254 *p = from_dev(d);
    *val = off < 3 ? read_count(p, (unsigned)off) : 0xFF;
    return UAM_OK;
}

static uam_status pit_write(uam_device *d, uint64_t off, unsigned size, uint64_t v)
{
    (void)size;
    i8254 *p = from_dev(d);
    if (off < 3)
        write_count(p, (unsigned)off, (uint8_t)v);
    else
        control(p, (uint8_t)v);
    return UAM_OK;
}

/* Port 61h: bit 0 counter-2 gate, bit 1 speaker data, bit 4 refresh toggle
 * (every 15 us), bit 5 counter-2 output. */
static uam_status p61_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    (void)off; (void)size;
    i8254 *p = from_p61(d);
    counter *c = &p->c[2];
    uint64_t refresh = (cycles_now(p) / 18) & 1;
    *val = (c->gate ? 1u : 0u) | (c->speaker ? 2u : 0u) | (unsigned)(refresh << 4) |
           (out(p, c) ? 0x20u : 0u);
    return UAM_OK;
}

static uam_status p61_write(uam_device *d, uint64_t off, unsigned size, uint64_t v)
{
    (void)off; (void)size;
    i8254 *p = from_p61(d);
    p->c[2].speaker = (v >> 1) & 1;
    set_gate(p, &p->c[2], v & 1);
    return UAM_OK;
}

static void pit_reset(uam_device *d)
{
    i8254 *p = from_dev(d);
    for (unsigned i = 0; i < 3; i++)
        p->c[i] = (counter){ .gate = i != 2, .rw = 3 };
    uam_timer_cancel(p->sched, &p->timer);
    uam_signal_set(&p->irq, 0);
}

static void pit_destroy(uam_device *d)
{
    i8254 *p = from_dev(d);
    uam_timer_cancel(p->sched, &p->timer);
    free(p);
}

static void pit_save(uam_device *d, uam_state_writer *w)
{
    i8254 *p = from_dev(d);
    for (unsigned i = 0; i < 3; i++) {
        const counter *c = &p->c[i];
        const uint8_t b[] = { c->mode, c->rw, c->bcd, c->loaded, c->running, c->gate, c->wbyte,
                              c->rbyte, c->latched, c->latch_bytes, c->status_latched, c->status,
                              c->speaker };
        uam_state_put_bytes(w, b, sizeof(b));
        uam_state_put_u32(w, c->n);
        uam_state_put_u64(w, c->start);
        uam_state_put_u64(w, c->frozen);
        uam_state_put_u16(w, c->wlsb);
        uam_state_put_u16(w, c->latch);
    }
    uam_state_put_timer(w, &p->timer);
    uam_state_put_signal(w, &p->irq);
}

static void pit_restore(uam_device *d, uam_state_reader *r)
{
    i8254 *p = from_dev(d);
    for (unsigned i = 0; i < 3; i++) {
        counter *c = &p->c[i];
        uint8_t b[13];
        uam_state_get_bytes(r, b, sizeof(b));
        c->mode = b[0]; c->rw = b[1]; c->bcd = b[2]; c->loaded = b[3]; c->running = b[4];
        c->gate = b[5]; c->wbyte = b[6]; c->rbyte = b[7]; c->latched = b[8];
        c->latch_bytes = b[9]; c->status_latched = b[10]; c->status = b[11]; c->speaker = b[12];
        c->n = uam_state_get_u32(r);
        c->start = uam_state_get_u64(r);
        c->frozen = uam_state_get_u64(r);
        c->wlsb = uam_state_get_u16(r);
        c->latch = uam_state_get_u16(r);
    }
    uam_state_get_timer(r, &p->timer);
    uam_state_get_signal(r, &p->irq);
}

static const uam_device_ops pit_ops = {
    .type = "intel,8254", .impl_min = 1, .impl_max = 1,
    .read = pit_read, .write = pit_write, .reset = pit_reset, .destroy = pit_destroy,
    .save = pit_save, .restore = pit_restore,
};
static const uam_device_ops p61_ops = {
    .type = "pc,port61", .impl_min = 1, .impl_max = 1,
    .read = p61_read, .write = p61_write, .flags = UAM_DEVICE_STATELESS,
};

uam_device *uam_i8254_create(const char *name, uam_sched *sched, uint64_t tick_hz)
{
    i8254 *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    uam_device_init(&p->dev, &pit_ops, name);
    uam_device_init(&p->p61, &p61_ops, name);
    p->sched = sched;
    p->tick_hz = tick_hz ? tick_hz : 1000000000u;
    uam_signal_init(&p->irq);
    uam_timer_init(&p->timer, timer_fired, p);
    for (unsigned i = 0; i < 3; i++)
        p->c[i] = (counter){ .gate = i != 2, .rw = 3 };
    return &p->dev;
}

uam_device *uam_i8254_ports(uam_device *d)  { return d; }
uam_device *uam_i8254_port61(uam_device *d) { return &from_dev(d)->p61; }
uam_signal *uam_i8254_irq_line(uam_device *d) { return &from_dev(d)->irq; }
