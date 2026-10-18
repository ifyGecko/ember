/*
 * state.c - State streams (uam/state.h).
 *
 * Layout: a stream is a sequence of records
 *     u16 name length, name bytes, u64 payload length, payload
 * and a payload is the component's fields, little-endian. Byte arrays carry
 * their own u64 length so that a reader asking for the wrong size fails
 * instead of desynchronising every later field.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uam/state.h"

struct uam_state_writer {
    uint8_t          *buf;
    size_t            len, cap;
    bool              err;
    size_t            rec_len_at;   /* offset of the open record's length field */
    bool              in_rec;
    const uam_sched  *sched;
    const uam_timer **timers;       /* timers saved so far */
    size_t            n_timers, cap_timers;
};

typedef struct pending_timer {
    uam_timer *t;
    uint64_t   when;
    uint32_t   rank;
} pending_timer;

struct uam_state_reader {
    const uint8_t *p;
    size_t         len, pos;
    size_t         end;         /* end of the current record (== len outside one) */
    bool           err;
    char           msg[192];
    char           name[256];   /* current record's name */
    uam_sched     *sched;
    pending_timer *pending;
    size_t         n_pending, cap_pending;
};

/* ---- writer ------------------------------------------------------------- */

uam_state_writer *uam_state_writer_new(const uam_sched *sched)
{
    uam_state_writer *w = calloc(1, sizeof(*w));
    if (w)
        w->sched = sched;
    return w;
}

void uam_state_writer_free(uam_state_writer *w)
{
    if (!w)
        return;
    free(w->buf);
    free(w->timers);
    free(w);
}

bool uam_state_writer_ok(const uam_state_writer *w)
{
    return !w->err;
}

const uint8_t *uam_state_writer_data(const uam_state_writer *w, size_t *len)
{
    *len = w->len;
    return w->buf;
}

static uint8_t *reserve(uam_state_writer *w, size_t n)
{
    if (w->err)
        return NULL;
    if (w->cap - w->len < n) {
        size_t ncap = w->cap ? w->cap : 4096;
        while (ncap - w->len < n)
            ncap *= 2;
        uint8_t *nb = realloc(w->buf, ncap);
        if (!nb) {
            w->err = true;
            return NULL;
        }
        w->buf = nb;
        w->cap = ncap;
    }
    uint8_t *p = w->buf + w->len;
    w->len += n;
    return p;
}

static void put_le(uam_state_writer *w, uint64_t v, unsigned n)
{
    uint8_t *p = reserve(w, n);
    if (p)
        for (unsigned i = 0; i < n; i++)
            p[i] = (uint8_t)(v >> (8 * i));
}

void uam_state_put_u8(uam_state_writer *w, uint8_t v)   { put_le(w, v, 1); }
void uam_state_put_u16(uam_state_writer *w, uint16_t v) { put_le(w, v, 2); }
void uam_state_put_u32(uam_state_writer *w, uint32_t v) { put_le(w, v, 4); }
void uam_state_put_u64(uam_state_writer *w, uint64_t v) { put_le(w, v, 8); }
void uam_state_put_bool(uam_state_writer *w, bool v)    { put_le(w, v ? 1 : 0, 1); }

void uam_state_put_bytes(uam_state_writer *w, const void *data, size_t len)
{
    put_le(w, len, 8);
    uint8_t *p = reserve(w, len);
    if (p && len)
        memcpy(p, data, len);
}

/* Position of t in the scheduler's list: ties keep their firing order. */
static uint32_t timer_rank(const uam_sched *s, const uam_timer *t)
{
    uint32_t i = 0;
    for (const uam_timer *p = s ? s->head : NULL; p; p = p->next, i++)
        if (p == t)
            return i;
    return UINT32_MAX;
}

void uam_state_put_timer(uam_state_writer *w, const uam_timer *t)
{
    uam_state_put_bool(w, t->armed);
    uam_state_put_u64(w, t->armed ? t->when : 0);
    uam_state_put_u32(w, t->armed ? timer_rank(w->sched, t) : 0);
    if (w->err)
        return;
    if (w->n_timers == w->cap_timers) {
        size_t ncap = w->cap_timers ? w->cap_timers * 2 : 16;
        const uam_timer **nt = realloc(w->timers, sizeof(*nt) * ncap);
        if (!nt) {
            w->err = true;
            return;
        }
        w->timers = nt;
        w->cap_timers = ncap;
    }
    w->timers[w->n_timers++] = t;
}

void uam_state_put_signal(uam_state_writer *w, const uam_signal *s)
{
    uam_state_put_u8(w, (uint8_t)s->level);
}

bool uam_state_writer_saved_timer(const uam_state_writer *w, const uam_timer *t)
{
    for (size_t i = 0; i < w->n_timers; i++)
        if (w->timers[i] == t)
            return true;
    return false;
}

void uam_state_begin(uam_state_writer *w, const char *name)
{
    size_t n = strlen(name);
    if (n > 255 || w->in_rec) {
        w->err = true;
        return;
    }
    put_le(w, n, 2);
    uint8_t *p = reserve(w, n);
    if (p)
        memcpy(p, name, n);
    w->rec_len_at = w->len;
    put_le(w, 0, 8);   /* patched by uam_state_end */
    w->in_rec = true;
}

void uam_state_end(uam_state_writer *w)
{
    if (!w->in_rec) {
        w->err = true;
        return;
    }
    w->in_rec = false;
    if (w->err)
        return;
    uint64_t n = w->len - w->rec_len_at - 8;
    for (unsigned i = 0; i < 8; i++)
        w->buf[w->rec_len_at + i] = (uint8_t)(n >> (8 * i));
}

/* ---- reader ------------------------------------------------------------- */

uam_state_reader *uam_state_reader_new(const uint8_t *data, size_t len, uam_sched *sched)
{
    uam_state_reader *r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    r->p = data;
    r->len = r->end = len;
    r->sched = sched;
    return r;
}

void uam_state_reader_free(uam_state_reader *r)
{
    if (!r)
        return;
    free(r->pending);
    free(r);
}

void uam_state_fail(uam_state_reader *r, const char *fmt, ...)
{
    if (r->err)
        return;
    r->err = true;
    int n = 0;
    if (r->name[0])
        n = snprintf(r->msg, sizeof(r->msg), "%s: ", r->name);
    if (n < 0 || (size_t)n >= sizeof(r->msg))
        n = 0;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->msg + n, sizeof(r->msg) - (size_t)n, fmt, ap);
    va_end(ap);
}

bool uam_state_ok(const uam_state_reader *r)
{
    return !r->err;
}

const char *uam_state_error(const uam_state_reader *r)
{
    return r->err ? r->msg : NULL;
}

static const uint8_t *take(uam_state_reader *r, size_t n)
{
    if (r->err)
        return NULL;
    if (r->end - r->pos < n) {
        uam_state_fail(r, "state ends early (wanted %zu more bytes, %zu left)", n, r->end - r->pos);
        return NULL;
    }
    const uint8_t *p = r->p + r->pos;
    r->pos += n;
    return p;
}

static uint64_t get_le(uam_state_reader *r, unsigned n)
{
    const uint8_t *p = take(r, n);
    uint64_t v = 0;
    if (p)
        for (unsigned i = 0; i < n; i++)
            v |= (uint64_t)p[i] << (8 * i);
    return v;
}

uint8_t  uam_state_get_u8(uam_state_reader *r)  { return (uint8_t)get_le(r, 1); }
uint16_t uam_state_get_u16(uam_state_reader *r) { return (uint16_t)get_le(r, 2); }
uint32_t uam_state_get_u32(uam_state_reader *r) { return (uint32_t)get_le(r, 4); }
uint64_t uam_state_get_u64(uam_state_reader *r) { return get_le(r, 8); }
bool     uam_state_get_bool(uam_state_reader *r) { return get_le(r, 1) != 0; }

void uam_state_get_bytes(uam_state_reader *r, void *data, size_t len)
{
    uint64_t n = get_le(r, 8);
    if (r->err)
        return;
    if (n != len) {
        uam_state_fail(r, "saved array is %llu bytes, expected %zu", (unsigned long long)n, len);
        return;
    }
    const uint8_t *p = take(r, len);
    if (p && len)
        memcpy(data, p, len);
}

void uam_state_get_timer(uam_state_reader *r, uam_timer *t)
{
    bool armed = uam_state_get_bool(r);
    uint64_t when = uam_state_get_u64(r);
    uint32_t rank = uam_state_get_u32(r);
    if (r->err)
        return;
    /* Whatever the timer was doing before, it is idle until the restored
     * timers are armed together (uam_state_arm_timers). */
    if (r->sched)
        uam_timer_cancel(r->sched, t);
    if (!armed)
        return;
    if (r->n_pending == r->cap_pending) {
        size_t ncap = r->cap_pending ? r->cap_pending * 2 : 16;
        pending_timer *np = realloc(r->pending, sizeof(*np) * ncap);
        if (!np) {
            uam_state_fail(r, "out of memory");
            return;
        }
        r->pending = np;
        r->cap_pending = ncap;
    }
    r->pending[r->n_pending++] = (pending_timer){ .t = t, .when = when, .rank = rank };
}

void uam_state_get_signal(uam_state_reader *r, uam_signal *s)
{
    s->level = uam_state_get_u8(r) != 0;
}

void uam_state_expect_u32(uam_state_reader *r, uint32_t want, const char *what)
{
    uint32_t got = uam_state_get_u32(r);
    if (!r->err && got != want)
        uam_state_fail(r, "%s is %u in the snapshot but %u in this machine", what, (unsigned)got,
                       (unsigned)want);
}

const char *uam_state_next(uam_state_reader *r)
{
    r->end = r->len;
    r->name[0] = 0;
    if (r->err || r->pos == r->len)
        return NULL;
    uint16_t n = (uint16_t)get_le(r, 2);
    const uint8_t *name = take(r, n);
    uint64_t plen = get_le(r, 8);
    if (r->err)
        return NULL;
    if (plen > r->len - r->pos) {
        uam_state_fail(r, "record overruns the stream");
        return NULL;
    }
    memcpy(r->name, name, n);
    r->name[n] = 0;
    r->end = r->pos + (size_t)plen;
    return r->name;
}

void uam_state_leave(uam_state_reader *r, const char *who)
{
    if (!r->err && r->pos != r->end)
        uam_state_fail(r, "%s left %zu bytes of its saved state unread (save and restore disagree)",
                       who ? who : "component", r->end - r->pos);
    if (!r->err)
        r->pos = r->end;
    r->end = r->len;
}

void uam_state_skip(uam_state_reader *r)
{
    if (!r->err)
        r->pos = r->end;
    r->end = r->len;
}

static int by_rank(const void *a, const void *b)
{
    const pending_timer *x = a, *y = b;
    return x->rank < y->rank ? -1 : x->rank > y->rank;
}

void uam_state_arm_timers(uam_state_reader *r)
{
    if (!r->sched || !r->n_pending)
        return;
    qsort(r->pending, r->n_pending, sizeof(*r->pending), by_rank);
    for (size_t i = 0; i < r->n_pending; i++)
        uam_timer_arm(r->sched, r->pending[i].t, r->pending[i].when);
    r->n_pending = 0;
}
