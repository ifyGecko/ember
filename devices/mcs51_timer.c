/*
 * mcs51_timer.c - The 8051's Timer 0/1 and TCON (uam/dev/mcs51_timer.h).
 *
 * The counters advance lazily: every access first brings them up to the current tick,
 * and a scheduler timer is armed only for the next overflow that would set a flag that
 * is still clear.
 */
#include <stdlib.h>

#include "uam/dev/mcs51_timer.h"

enum { TF1 = 0x80, TR1 = 0x40, TF0 = 0x20, TR0 = 0x10, IE1 = 0x08, IT1 = 0x04, IE0 = 0x02, IT0 = 0x01 };
enum { GATE = 0x8, CT = 0x4 };

typedef struct m51timer {
    uam_device dev;
    uam_sched *sched;
    uam_timer  t;
    uam_signal irq[4];      /* IE0, TF0, IE1, TF1 */
    uint8_t    tcon, tmod, tl[2], th[2];
    uint8_t    pin[2];      /* INT0/INT1 levels (1 = high) */
    uint64_t   last;        /* the tick the counters were last brought up to */
} m51timer;

static m51timer *to_t(uam_device *d) { return UAM_CONTAINER_OF(d, m51timer, dev); }

static unsigned mode(const m51timer *m, unsigned i) { return (m->tmod >> (4 * i)) & 3u; }
static unsigned ctl(const m51timer *m, unsigned i) { return (m->tmod >> (4 * i)) & 0xCu; }
static uint8_t  tf(unsigned i) { return i ? TF1 : TF0; }

/* Does timer i count? (TL0 alone in mode 3; TH0 then is the extra timer.) */
static bool runs(const m51timer *m, unsigned i)
{
    const uint8_t tr = i ? TR1 : TR0;
    if (!(m->tcon & tr) || (ctl(m, i) & CT) || ((ctl(m, i) & GATE) && !m->pin[i]))
        return false;
    return !(i == 1 && mode(m, 1) == 3);
}

/* Does timer i's overflow set its flag? Not Timer 1's while Timer 0 is in mode 3. */
static bool flags(const m51timer *m, unsigned i) { return !(i == 1 && mode(m, 0) == 3); }

/* Advance timer i (modes 0-2, or TL0 in mode 3) by n ticks; true on an overflow. */
static bool advance(m51timer *m, unsigned i, uint64_t n)
{
    switch (mode(m, i)) {
    case 0: {
        const uint64_t c = ((uint64_t)m->th[i] << 5 | (m->tl[i] & 0x1F)) + n;
        m->th[i] = (uint8_t)(c >> 5);
        m->tl[i] = (uint8_t)((m->tl[i] & 0xE0) | (c & 0x1F));
        return c >= 0x2000;
    }
    case 1: {
        const uint64_t c = ((uint64_t)m->th[i] << 8 | m->tl[i]) + n;
        m->th[i] = (uint8_t)(c >> 8);
        m->tl[i] = (uint8_t)c;
        return c >= 0x10000;
    }
    case 2: {
        const uint64_t to_ovf = 256u - m->tl[i];
        if (n < to_ovf) {
            m->tl[i] = (uint8_t)(m->tl[i] + n);
            return false;
        }
        const uint64_t period = 256u - m->th[i];
        m->tl[i] = (uint8_t)(m->th[i] + (n - to_ovf) % period);
        return true;
    }
    default: {                                       /* TL0 as an 8-bit timer */
        const uint64_t c = m->tl[i] + n;
        m->tl[i] = (uint8_t)c;
        return c >= 0x100;
    }
    }
}

/* Ticks until timer i next overflows (it runs). */
static uint64_t to_overflow(const m51timer *m, unsigned i)
{
    switch (mode(m, i)) {
    case 0:  return 0x2000u - ((unsigned)m->th[i] << 5 | (m->tl[i] & 0x1F));
    case 1:  return 0x10000u - ((unsigned)m->th[i] << 8 | m->tl[i]);
    default: return 256u - m->tl[i];
    }
}

static void update_irq(m51timer *m)
{
    uam_signal_set(&m->irq[0], (m->tcon & IE0) != 0);
    uam_signal_set(&m->irq[1], (m->tcon & TF0) != 0);
    uam_signal_set(&m->irq[2], (m->tcon & IE1) != 0);
    uam_signal_set(&m->irq[3], (m->tcon & TF1) != 0);
}

/* Bring the counters up to now. */
static void sync(m51timer *m)
{
    const uint64_t now = uam_sched_now(m->sched);
    const uint64_t n = now - m->last;
    m->last = now;
    if (!n)
        return;
    for (unsigned i = 0; i < 2; i++)
        if (runs(m, i) && advance(m, i, n) && flags(m, i))
            m->tcon |= tf(i);
    if (mode(m, 0) == 3 && (m->tcon & TR1)) {      /* TH0, run by TR1, sets TF1 */
        const uint64_t c = m->th[0] + n;
        m->th[0] = (uint8_t)c;
        if (c >= 0x100)
            m->tcon |= TF1;
    }
}

/* Arm the scheduler timer for the next overflow that sets a clear flag. */
static void schedule(m51timer *m)
{
    uint64_t next = UINT64_MAX;
    for (unsigned i = 0; i < 2; i++)
        if (runs(m, i) && flags(m, i) && !(m->tcon & tf(i))) {
            const uint64_t d = to_overflow(m, i);
            if (d < next)
                next = d;
        }
    if (mode(m, 0) == 3 && (m->tcon & TR1) && !(m->tcon & TF1)) {
        const uint64_t d = 256u - m->th[0];
        if (d < next)
            next = d;
    }
    uam_timer_cancel(m->sched, &m->t);
    if (next != UINT64_MAX)
        uam_timer_arm(m->sched, &m->t, m->last + next);
    update_irq(m);
}

static void fired(void *ctx)
{
    m51timer *m = ctx;
    sync(m);
    schedule(m);
}

static uam_status tm_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    (void)size;
    m51timer *m = to_t(d);
    sync(m);
    switch (off) {
    case 0:  *val = m->tcon; break;
    case 1:  *val = m->tmod; break;
    case 2:  *val = m->tl[0]; break;
    case 3:  *val = m->tl[1]; break;
    case 4:  *val = m->th[0]; break;
    case 5:  *val = m->th[1]; break;
    default: *val = 0; break;
    }
    schedule(m);   /* sync may have set a flag */
    return UAM_OK;
}

/* A side-effect-free read: the counters as they would read now. */
static uam_status tm_peek(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    m51timer copy = *to_t(d);
    (void)size;
    sync(&copy);
    const uint8_t regs[6] = { copy.tcon, copy.tmod, copy.tl[0], copy.tl[1], copy.th[0], copy.th[1] };
    *val = off < 6 ? regs[off] : 0;
    return UAM_OK;
}

static uam_status tm_write(uam_device *d, uint64_t off, unsigned size, uint64_t val)
{
    (void)size;
    m51timer *m = to_t(d);
    const uint8_t v = (uint8_t)val;
    sync(m);
    switch (off) {
    case 0: {
        /* IE0/IE1 follow the pins while level-triggered */
        m->tcon = v;
        if (!(v & IT0)) m->tcon = (uint8_t)((m->tcon & ~IE0) | (m->pin[0] ? 0 : IE0));
        if (!(v & IT1)) m->tcon = (uint8_t)((m->tcon & ~IE1) | (m->pin[1] ? 0 : IE1));
        break;
    }
    case 1: m->tmod = v; break;
    case 2: m->tl[0] = v; break;
    case 3: m->tl[1] = v; break;
    case 4: m->th[0] = v; break;
    case 5: m->th[1] = v; break;
    default: break;
    }
    schedule(m);
    return UAM_OK;
}

static void tm_reset(uam_device *d)
{
    m51timer *m = to_t(d);
    uam_timer_cancel(m->sched, &m->t);
    m->tcon = m->tmod = 0;
    m->tl[0] = m->tl[1] = m->th[0] = m->th[1] = 0;
    m->pin[0] = m->pin[1] = 1;
    m->last = uam_sched_now(m->sched);
    update_irq(m);
}

static void tm_destroy(uam_device *d)
{
    m51timer *m = to_t(d);
    uam_timer_cancel(m->sched, &m->t);
    free(m);
}

static void tm_save(uam_device *d, uam_state_writer *w)
{
    m51timer *m = to_t(d);
    const uint8_t b[] = { m->tcon, m->tmod, m->tl[0], m->tl[1], m->th[0], m->th[1], m->pin[0], m->pin[1] };
    uam_state_put_bytes(w, b, sizeof(b));
    uam_state_put_u64(w, m->last);
    uam_state_put_timer(w, &m->t);
    for (unsigned k = 0; k < 4; k++)
        uam_state_put_signal(w, &m->irq[k]);
}

static void tm_restore(uam_device *d, uam_state_reader *r)
{
    m51timer *m = to_t(d);
    uint8_t b[8];
    uam_state_get_bytes(r, b, sizeof(b));
    m->tcon = b[0]; m->tmod = b[1]; m->tl[0] = b[2]; m->tl[1] = b[3];
    m->th[0] = b[4]; m->th[1] = b[5]; m->pin[0] = b[6]; m->pin[1] = b[7];
    m->last = uam_state_get_u64(r);
    uam_state_get_timer(r, &m->t);
    for (unsigned k = 0; k < 4; k++)
        uam_state_get_signal(r, &m->irq[k]);
}

static const uam_device_ops tm_ops = {
    .type = "uam,mcs51-timer", .impl_min = 1, .impl_max = 1,
    .read = tm_read, .peek = tm_peek, .write = tm_write, .reset = tm_reset, .destroy = tm_destroy,
    .save = tm_save, .restore = tm_restore,
};

uam_device *uam_mcs51_timer_create(const char *name, uam_sched *sched)
{
    if (!sched)
        return NULL;
    m51timer *m = calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    uam_device_init(&m->dev, &tm_ops, name);
    m->sched = sched;
    uam_timer_init(&m->t, fired, m);
    for (unsigned k = 0; k < 4; k++)
        uam_signal_init(&m->irq[k]);
    m->pin[0] = m->pin[1] = 1;
    m->last = uam_sched_now(sched);
    return &m->dev;
}

uam_signal *uam_mcs51_timer_irq(uam_device *d, unsigned k) { return &to_t(d)->irq[k & 3]; }

void uam_mcs51_timer_ack(uam_device *d, unsigned k)
{
    m51timer *m = to_t(d);
    sync(m);
    switch (k) {
    case 0: if (m->tcon & IT0) m->tcon &= (uint8_t)~IE0; break;
    case 1: m->tcon &= (uint8_t)~TF0; break;
    case 2: if (m->tcon & IT1) m->tcon &= (uint8_t)~IE1; break;
    case 3: m->tcon &= (uint8_t)~TF1; break;
    default: return;
    }
    schedule(m);
}

void uam_mcs51_timer_int_pin(void *dev, unsigned n, int level)
{
    m51timer *m = to_t(dev);
    if (n > 1)
        return;
    sync(m);   /* GATE: the count so far ran under the old level */
    const uint8_t old = m->pin[n], ie = n ? IE1 : IE0, it = n ? IT1 : IT0;
    m->pin[n] = level ? 1 : 0;
    if (m->tcon & it) {
        if (old && !m->pin[n])
            m->tcon |= ie;
    } else {
        m->tcon = (uint8_t)((m->tcon & ~ie) | (m->pin[n] ? 0 : ie));
    }
    schedule(m);
}
