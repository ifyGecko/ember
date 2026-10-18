/*
 * plic.c - SiFive-compatible PLIC (uam/dev/plic.h).
 *
 * Gateway model (level-triggered): a source becomes pending while its input
 * is high and it is not already being serviced. A claim returns the highest
 * priority pending source enabled for the context with priority > threshold
 * (ties go to the lowest id), clears its pending bit and marks it in
 * service. A complete ends service; if the input is still high, the source
 * becomes pending again.
 */
#include <stdlib.h>

#include "uam/dev/plic.h"

#define OFF_PRIORITY  0x000000u
#define OFF_PENDING   0x001000u
#define OFF_ENABLE    0x002000u
#define ENABLE_STRIDE 0x80u
#define OFF_CONTEXT   0x200000u
#define CTX_STRIDE    0x1000u

#define WORDS(n) (((n) + 31) / 32)

typedef struct plic {
    uam_device  dev;
    unsigned    n_sources, n_contexts;
    uint32_t   *priority;   /* [n_sources] */
    uint32_t   *level;      /* input levels, bitmap */
    uint32_t   *pending;    /* bitmap */
    uint32_t   *in_service; /* bitmap */
    uint32_t   *enable;     /* [n_contexts][WORDS(n_sources)] */
    uint32_t   *threshold;  /* [n_contexts] */
    uam_signal *out;        /* [n_contexts] */
} plic;

static plic *to_plic(uam_device *d) { return UAM_CONTAINER_OF(d, plic, dev); }

static bool bit(const uint32_t *bm, unsigned i) { return (bm[i / 32] >> (i % 32)) & 1; }
static void set_bit(uint32_t *bm, unsigned i, bool v)
{
    if (v)
        bm[i / 32] |= 1u << (i % 32);
    else
        bm[i / 32] &= ~(1u << (i % 32));
}

/* Best claimable source for a context, or 0. */
static unsigned best_source(const plic *p, unsigned ctx)
{
    const uint32_t *en = &p->enable[ctx * WORDS(p->n_sources)];
    unsigned best = 0;
    uint32_t best_prio = p->threshold[ctx];
    for (unsigned s = 1; s < p->n_sources; s++) {
        if (bit(p->pending, s) && bit(en, s) && p->priority[s] > best_prio) {
            best = s;
            best_prio = p->priority[s];
        }
    }
    return best;
}

static void update(plic *p)
{
    for (unsigned c = 0; c < p->n_contexts; c++)
        uam_signal_set(&p->out[c], best_source(p, c) != 0);
}

void uam_plic_input(void *ctx, unsigned src, int level)
{
    plic *p = to_plic(ctx);
    if (src == 0 || src >= p->n_sources)
        return;
    set_bit(p->level, src, level);
    if (level && !bit(p->in_service, src))
        set_bit(p->pending, src, true);
    update(p);
}

static uam_status plic_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    plic *p = to_plic(d);
    (void)size;
    *val = 0;
    if (off < OFF_PENDING) {
        unsigned s = (unsigned)(off / 4);
        if (s < p->n_sources)
            *val = p->priority[s];
    } else if (off < OFF_ENABLE) {
        unsigned w = (unsigned)((off - OFF_PENDING) / 4);
        if (w < WORDS(p->n_sources))
            *val = p->pending[w];
    } else if (off < OFF_CONTEXT) {
        unsigned c = (unsigned)((off - OFF_ENABLE) / ENABLE_STRIDE);
        unsigned w = (unsigned)((off - OFF_ENABLE) % ENABLE_STRIDE / 4);
        if (c < p->n_contexts && w < WORDS(p->n_sources))
            *val = p->enable[c * WORDS(p->n_sources) + w];
    } else {
        unsigned c = (unsigned)((off - OFF_CONTEXT) / CTX_STRIDE);
        unsigned r = (unsigned)((off - OFF_CONTEXT) % CTX_STRIDE);
        if (c >= p->n_contexts)
            return UAM_OK;
        if (r == 0) {
            *val = p->threshold[c];
        } else if (r == 4) { /* claim */
            unsigned s = best_source(p, c);
            if (s) {
                set_bit(p->pending, s, false);
                set_bit(p->in_service, s, true);
                update(p);
            }
            *val = s;
        }
    }
    return UAM_OK;
}

static uam_status plic_write(uam_device *d, uint64_t off, unsigned size, uint64_t v)
{
    plic *p = to_plic(d);
    (void)size;
    uint32_t v32 = (uint32_t)v;
    if (off < OFF_PENDING) {
        unsigned s = (unsigned)(off / 4);
        if (s > 0 && s < p->n_sources)
            p->priority[s] = v32 & 7;
    } else if (off < OFF_ENABLE) {
        /* pending bits are read-only */
    } else if (off < OFF_CONTEXT) {
        unsigned c = (unsigned)((off - OFF_ENABLE) / ENABLE_STRIDE);
        unsigned w = (unsigned)((off - OFF_ENABLE) % ENABLE_STRIDE / 4);
        if (c < p->n_contexts && w < WORDS(p->n_sources))
            p->enable[c * WORDS(p->n_sources) + w] = w == 0 ? (v32 & ~1u) : v32; /* source 0 is reserved */
    } else {
        unsigned c = (unsigned)((off - OFF_CONTEXT) / CTX_STRIDE);
        unsigned r = (unsigned)((off - OFF_CONTEXT) % CTX_STRIDE);
        if (c >= p->n_contexts)
            return UAM_OK;
        if (r == 0) {
            p->threshold[c] = v32 & 7;
        } else if (r == 4 && v32 > 0 && v32 < p->n_sources) { /* complete */
            set_bit(p->in_service, v32, false);
            if (bit(p->level, v32))
                set_bit(p->pending, v32, true);
        }
    }
    update(p);
    return UAM_OK;
}

static void plic_reset(uam_device *d)
{
    plic *p = to_plic(d);
    unsigned w = WORDS(p->n_sources);
    for (unsigned s = 0; s < p->n_sources; s++)
        p->priority[s] = 0;
    for (unsigned i = 0; i < w; i++) {
        p->in_service[i] = 0;
        p->pending[i] = p->level[i]; /* still-high inputs remain pending */
    }
    for (unsigned i = 0; i < w * p->n_contexts; i++)
        p->enable[i] = 0;
    for (unsigned c = 0; c < p->n_contexts; c++)
        p->threshold[c] = 0;
    update(p);
}

static void plic_destroy(uam_device *d)
{
    plic *p = to_plic(d);
    free(p->priority);
    free(p->level);
    free(p->pending);
    free(p->in_service);
    free(p->enable);
    free(p->threshold);
    free(p->out);
    free(p);
}

static void put_words(uam_state_writer *w, const uint32_t *v, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        uam_state_put_u32(w, v[i]);
}

static void get_words(uam_state_reader *r, uint32_t *v, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        v[i] = uam_state_get_u32(r);
}

static void plic_save(uam_device *d, uam_state_writer *w)
{
    plic *p = to_plic(d);
    const unsigned words = WORDS(p->n_sources);
    uam_state_put_u32(w, p->n_sources);
    uam_state_put_u32(w, p->n_contexts);
    put_words(w, p->priority, p->n_sources);
    put_words(w, p->level, words);
    put_words(w, p->pending, words);
    put_words(w, p->in_service, words);
    put_words(w, p->enable, p->n_contexts * words);
    put_words(w, p->threshold, p->n_contexts);
    for (unsigned c = 0; c < p->n_contexts; c++)
        uam_state_put_signal(w, &p->out[c]);
}

static void plic_restore(uam_device *d, uam_state_reader *r)
{
    plic *p = to_plic(d);
    const unsigned words = WORDS(p->n_sources);
    uam_state_expect_u32(r, p->n_sources, "PLIC source count");
    uam_state_expect_u32(r, p->n_contexts, "PLIC context count");
    if (!uam_state_ok(r))
        return;
    get_words(r, p->priority, p->n_sources);
    get_words(r, p->level, words);
    get_words(r, p->pending, words);
    get_words(r, p->in_service, words);
    get_words(r, p->enable, p->n_contexts * words);
    get_words(r, p->threshold, p->n_contexts);
    for (unsigned c = 0; c < p->n_contexts; c++)
        uam_state_get_signal(r, &p->out[c]);
}

static const uam_device_ops plic_ops = {
    .type = "sifive,plic-1.0.0",
    .read = plic_read,
    .write = plic_write,
    .reset = plic_reset,
    .destroy = plic_destroy,
    .save = plic_save,
    .restore = plic_restore,
};

uam_device *uam_plic_create(const char *name, unsigned n_sources, unsigned n_contexts)
{
    if (n_sources < 2 || n_sources > UAM_PLIC_MAX_SOURCES || n_contexts == 0
        || n_contexts > UAM_PLIC_MAX_CONTEXTS)
        return NULL;
    plic *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    uam_device_init(&p->dev, &plic_ops, name);
    p->n_sources = n_sources;
    p->n_contexts = n_contexts;
    unsigned w = WORDS(n_sources);
    p->priority = calloc(n_sources, sizeof(uint32_t));
    p->level = calloc(w, sizeof(uint32_t));
    p->pending = calloc(w, sizeof(uint32_t));
    p->in_service = calloc(w, sizeof(uint32_t));
    p->enable = calloc((size_t)w * n_contexts, sizeof(uint32_t));
    p->threshold = calloc(n_contexts, sizeof(uint32_t));
    p->out = calloc(n_contexts, sizeof(uam_signal));
    if (!p->priority || !p->level || !p->pending || !p->in_service || !p->enable
        || !p->threshold || !p->out) {
        plic_destroy(&p->dev);
        return NULL;
    }
    for (unsigned c = 0; c < n_contexts; c++)
        uam_signal_init(&p->out[c]);
    return &p->dev;
}

uam_signal *uam_plic_context_line(uam_device *d, unsigned ctx)
{
    plic *p = to_plic(d);
    return ctx < p->n_contexts ? &p->out[ctx] : NULL;
}
