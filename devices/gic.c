/*
 * gic.c - GICv2-style interrupt controller for one CPU (uam/dev/gic.h).
 *
 * Interrupt state, per id: enabled, an edge/software pending latch, the input
 * level (level-sensitive ids are pending while their input is high), active.
 * The CPU is signalled while the distributor and the CPU interface are enabled
 * and the highest-priority pending, enabled, inactive interrupt has a priority
 * below both GICC_PMR and the running priority (the highest active one).
 * Acknowledging (GICC_IAR) makes it active and clears its latch; EOI makes it
 * inactive, after which a still-high level input is pending again.
 *
 * The distributor device owns all state; the CPU interface is a second device
 * over the same object (stateless for snapshots, destroyed with the distributor).
 */
#include <stdio.h>
#include <stdlib.h>

#include "uam/dev/gic.h"

#define SPURIOUS 1023u

typedef struct gic {
    uam_device  dist;
    uam_device  cpu;
    char        cpu_name[64];
    unsigned    n;              /* interrupt ids */
    uint32_t   *enabled, *latch, *level, *active, *edge;   /* bitmaps, n/32 words */
    uint8_t    *prio;           /* [n] */
    bool        dist_en, cpu_en;
    uint32_t    pmr, bpr;
    uam_signal  irq;
} gic;

static gic *from_dist(uam_device *d) { return UAM_CONTAINER_OF(d, gic, dist); }
static gic *from_cpu(uam_device *d) { return UAM_CONTAINER_OF(d, gic, cpu); }

static bool bit(const uint32_t *bm, unsigned i) { return (bm[i / 32] >> (i % 32)) & 1; }
static void set_bit(uint32_t *bm, unsigned i, bool v)
{
    if (v)
        bm[i / 32] |= 1u << (i % 32);
    else
        bm[i / 32] &= ~(1u << (i % 32));
}

static bool pending(const gic *g, unsigned i)
{
    return bit(g->latch, i) || (!bit(g->edge, i) && bit(g->level, i));
}

static unsigned running_prio(const gic *g)
{
    unsigned rp = 0x100;
    for (unsigned i = 0; i < g->n; i++)
        if (bit(g->active, i) && g->prio[i] < rp)
            rp = g->prio[i];
    return rp;
}

/* Highest-priority pending, enabled, inactive interrupt (lowest id on ties), or SPURIOUS. */
static unsigned best(const gic *g, bool check_masks)
{
    unsigned id = SPURIOUS, p = 0x100;
    for (unsigned i = 0; i < g->n; i++)
        if (pending(g, i) && bit(g->enabled, i) && !bit(g->active, i) && g->prio[i] < p) {
            id = i;
            p = g->prio[i];
        }
    if (id != SPURIOUS && check_masks && (p >= g->pmr || p >= running_prio(g)))
        return SPURIOUS;
    return id;
}

static void update(gic *g)
{
    uam_signal_set(&g->irq, g->dist_en && g->cpu_en && best(g, true) != SPURIOUS);
}

void uam_gic_input(void *ctx, unsigned id, int level)
{
    gic *g = from_dist(ctx);
    if (id < 16 || id >= g->n)
        return;
    bool was = bit(g->level, id);
    set_bit(g->level, id, level);
    if (level && !was && bit(g->edge, id))
        set_bit(g->latch, id, true);
    update(g);
}

/* ---- distributor ---- */

static uint32_t *bitmap_reg(gic *g, uint64_t off, unsigned *word)
{
    unsigned base = (unsigned)(off & ~0x7Fu);
    *word = (unsigned)(off & 0x7F) / 4;
    if (*word >= g->n / 32)
        return NULL;
    switch (base) {
    case 0x100: case 0x180: return g->enabled;
    case 0x200: case 0x280: return g->latch;
    case 0x300: case 0x380: return g->active;
    default: return NULL;
    }
}

static uint32_t cfg_word(const gic *g, unsigned w)
{
    uint32_t v = 0;
    for (unsigned k = 0; k < 16; k++) {
        unsigned i = w * 16 + k;
        if (i < g->n && bit(g->edge, i))
            v |= 2u << (2 * k);
    }
    return v;
}

static uam_status dist_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    gic *g = from_dist(d);
    *val = 0;
    if (off >= 0x400 && off < 0xC00) {                 /* byte registers */
        for (unsigned k = 0; k < size; k++) {
            uint64_t o = off + k;
            unsigned i = (unsigned)(o & 0x3FF);
            uint64_t b = 0;
            if (i < g->n)
                b = o < 0x800 ? g->prio[i] : 1;        /* ITARGETSR: CPU 0 */
            *val |= b << (8 * k);
        }
        return UAM_OK;
    }
    if (size != 4)
        return UAM_OK;
    unsigned w;
    uint32_t *bm;
    if (off == 0x000)
        *val = g->dist_en;
    else if (off == 0x004)
        *val = g->n / 32 - 1;                          /* ITLinesNumber; one CPU */
    else if (off == 0x008)
        *val = 0x0200143Bu;
    else if (off >= 0xC00 && off < 0xD00) {
        unsigned cw = (unsigned)(off - 0xC00) / 4;
        if (cw < g->n / 16)
            *val = cfg_word(g, cw);
    } else if (off >= 0x200 && off < 0x300) {          /* pending, as software sees it */
        w = (unsigned)(off & 0x7F) / 4;
        if (w < g->n / 32)
            for (unsigned k = 0; k < 32; k++)
                if (pending(g, w * 32 + k))
                    *val |= 1u << k;
    } else if ((bm = bitmap_reg(g, off, &w)) != NULL)
        *val = bm[w];
    return UAM_OK;
}

static uam_status dist_write(uam_device *d, uint64_t off, unsigned size, uint64_t v64)
{
    gic *g = from_dist(d);
    uint32_t v = (uint32_t)v64;
    if (off >= 0x400 && off < 0x800) {
        for (unsigned k = 0; k < size; k++) {
            unsigned i = (unsigned)((off + k) & 0x3FF);
            if (i < g->n)
                g->prio[i] = (uint8_t)(v64 >> (8 * k));
        }
        update(g);
        return UAM_OK;
    }
    if (size != 4 || (off >= 0x800 && off < 0xC00))
        return UAM_OK;
    unsigned w;
    uint32_t *bm;
    if (off == 0x000) {
        g->dist_en = v & 1;
    } else if (off >= 0xC00 && off < 0xD00) {
        unsigned cw = (unsigned)(off - 0xC00) / 4;
        for (unsigned k = 0; k < 16; k++) {
            unsigned i = cw * 16 + k;
            if (i >= 16 && i < g->n)                   /* SGIs are always edge-triggered */
                set_bit(g->edge, i, (v >> (2 * k + 1)) & 1);
        }
    } else if (off == 0xF00) {                         /* SGIR: every filter reaches CPU 0 */
        set_bit(g->latch, v & 0xF, true);
    } else if ((bm = bitmap_reg(g, off, &w)) != NULL) {
        bool set = (off & 0x80) == 0;                  /* IS* at 0x?00, IC* at 0x?80 */
        bm[w] = set ? bm[w] | v : bm[w] & ~v;
    }
    update(g);
    return UAM_OK;
}

/* ---- CPU interface ---- */

static uam_status cpu_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    gic *g = from_cpu(d);
    (void)size;
    *val = 0;
    switch (off) {
    case 0x00: *val = g->cpu_en; break;
    case 0x04: *val = g->pmr; break;
    case 0x08: *val = g->bpr; break;
    case 0x0C: {                                       /* IAR */
        unsigned id = g->dist_en && g->cpu_en ? best(g, true) : SPURIOUS;
        if (id != SPURIOUS) {
            set_bit(g->latch, id, false);
            set_bit(g->active, id, true);
            update(g);
        }
        *val = id;
        break;
    }
    case 0x14: { unsigned rp = running_prio(g); *val = rp > 0xFF ? 0xFF : rp; break; }
    case 0x18: *val = best(g, false); break;
    case 0xFC: *val = 0x0202143Bu; break;
    default: break;
    }
    return UAM_OK;
}

static uam_status cpu_write(uam_device *d, uint64_t off, unsigned size, uint64_t v64)
{
    gic *g = from_cpu(d);
    uint32_t v = (uint32_t)v64;
    (void)size;
    switch (off) {
    case 0x00: g->cpu_en = v & 1; break;
    case 0x04: g->pmr = v & 0xFF; break;
    case 0x08: g->bpr = v & 7; break;
    case 0x10:                                         /* EOIR */
        if ((v & 0x3FF) < g->n)
            set_bit(g->active, v & 0x3FF, false);
        break;
    default: break;
    }
    update(g);
    return UAM_OK;
}

/* ---- lifecycle and snapshots ---- */

static void gic_reset(uam_device *d)
{
    gic *g = from_dist(d);
    unsigned w = g->n / 32;
    for (unsigned i = 0; i < w; i++) {
        g->enabled[i] = g->latch[i] = g->active[i] = 0;
        g->edge[i] = i == 0 ? 0xFFFFu : 0;             /* SGIs edge, the rest level */
    }
    for (unsigned i = 0; i < g->n; i++)
        g->prio[i] = 0;
    g->dist_en = g->cpu_en = false;
    g->pmr = g->bpr = 0;
    update(g);
}

static void gic_destroy(uam_device *d)
{
    gic *g = from_dist(d);
    free(g->enabled);
    free(g->latch);
    free(g->level);
    free(g->active);
    free(g->edge);
    free(g->prio);
    free(g);
}

static void cpu_destroy(uam_device *d) { (void)d; }   /* owned by the distributor */

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

static void gic_save(uam_device *d, uam_state_writer *w)
{
    gic *g = from_dist(d);
    unsigned n = g->n / 32;
    uam_state_put_u32(w, g->n);
    put_words(w, g->enabled, n);
    put_words(w, g->latch, n);
    put_words(w, g->level, n);
    put_words(w, g->active, n);
    put_words(w, g->edge, n);
    uam_state_put_bytes(w, g->prio, g->n);
    uam_state_put_bool(w, g->dist_en);
    uam_state_put_bool(w, g->cpu_en);
    uam_state_put_u32(w, g->pmr);
    uam_state_put_u32(w, g->bpr);
    uam_state_put_signal(w, &g->irq);
}

static void gic_restore(uam_device *d, uam_state_reader *r)
{
    gic *g = from_dist(d);
    unsigned n = g->n / 32;
    uam_state_expect_u32(r, g->n, "GIC interrupt count");
    if (!uam_state_ok(r))
        return;
    get_words(r, g->enabled, n);
    get_words(r, g->latch, n);
    get_words(r, g->level, n);
    get_words(r, g->active, n);
    get_words(r, g->edge, n);
    uam_state_get_bytes(r, g->prio, g->n);
    g->dist_en = uam_state_get_bool(r);
    g->cpu_en = uam_state_get_bool(r);
    g->pmr = uam_state_get_u32(r);
    g->bpr = uam_state_get_u32(r);
    uam_state_get_signal(r, &g->irq);
}

static const uam_device_ops dist_ops = {
    .type = "arm,gic-400-dist",
    .read = dist_read,
    .write = dist_write,
    .reset = gic_reset,
    .destroy = gic_destroy,
    .save = gic_save,
    .restore = gic_restore,
};

static const uam_device_ops cpu_ops = {
    .type = "arm,gic-400-cpu",
    .impl_min = 4, .impl_max = 4,
    .read = cpu_read,
    .write = cpu_write,
    .destroy = cpu_destroy,
    .flags = UAM_DEVICE_STATELESS,
};

uam_device *uam_gic_create(const char *name, unsigned n_irqs)
{
    if (n_irqs < 64 || n_irqs > UAM_GIC_MAX_IRQS || n_irqs % 32)
        return NULL;
    gic *g = calloc(1, sizeof(*g));
    if (!g)
        return NULL;
    uam_device_init(&g->dist, &dist_ops, name);
    snprintf(g->cpu_name, sizeof(g->cpu_name), "%s-cpu", name);
    uam_device_init(&g->cpu, &cpu_ops, g->cpu_name);
    g->n = n_irqs;
    unsigned w = n_irqs / 32;
    g->enabled = calloc(w, sizeof(uint32_t));
    g->latch = calloc(w, sizeof(uint32_t));
    g->level = calloc(w, sizeof(uint32_t));
    g->active = calloc(w, sizeof(uint32_t));
    g->edge = calloc(w, sizeof(uint32_t));
    g->prio = calloc(n_irqs, 1);
    if (!g->enabled || !g->latch || !g->level || !g->active || !g->edge || !g->prio) {
        gic_destroy(&g->dist);
        return NULL;
    }
    uam_signal_init(&g->irq);
    gic_reset(&g->dist);
    return &g->dist;
}

uam_device *uam_gic_cpu_iface(uam_device *d) { return &from_dist(d)->cpu; }
uam_signal *uam_gic_irq_line(uam_device *d) { return &from_dist(d)->irq; }
