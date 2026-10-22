/*
 * gic.c - GICv2-style interrupt controller for one to eight CPUs (uam/dev/gic.h).
 *
 * Interrupt state, per id: enabled, an edge/software pending latch, the input
 * level (level-sensitive ids are pending while their input is high), active.
 * Ids 0-31 are banked: every CPU has its own (its SGIs and PPIs, and their
 * enable, pending, active, priority and configuration). An SGI is pending for
 * each CPU that sent it, and acknowledging it reports the lowest such source.
 * SPIs are shared and reach the CPUs in their ITARGETSR byte; the CPU that
 * acknowledges one makes it active for itself (and only it can end it).
 *
 * CPU c is signalled while the distributor and its CPU interface are enabled
 * and the highest-priority pending, enabled, inactive interrupt it can see has
 * a priority below both its GICC_PMR and its running priority (its highest
 * active one). Acknowledging (GICC_IAR) makes it active and clears its latch;
 * EOI makes it inactive, after which a still-high level input is pending again.
 *
 * Banked registers (the CPU interface, and the distributor's for ids 0-31)
 * answer the CPU making the access, which the board's `who` callback names.
 *
 * The distributor device owns all state; the CPU interface is a second device
 * over the same object (stateless for snapshots, destroyed with the distributor).
 */
#include <stdio.h>
#include <stdlib.h>

#include "uam/dev/gic.h"

#define SPURIOUS 1023u

/* One CPU's banked ids 0-31 and its CPU interface. */
typedef struct gic_cpu {
    uint32_t    enabled, latch, level, active, edge;
    uint8_t     prio[32];
    uint8_t     sgi_src[16];    /* per SGI: the CPUs that sent it, pending */
    bool        en;
    uint32_t    pmr, bpr;
    uam_signal  irq;
} gic_cpu;

typedef struct gic {
    uam_device  dist;
    uam_device  cpu;
    char        cpu_name[64];
    unsigned    n;              /* interrupt ids */
    unsigned    ncpu;
    /* SPIs (ids 32 up): bitmaps of n/32 words, word 0 unused (banked) */
    uint32_t   *enabled, *latch, *level, *active, *edge;
    uint8_t    *prio, *targets, *owner;   /* [n]; owner: the CPU an active SPI is active on */
    bool        dist_en;
    unsigned  (*who)(void *ctx);
    void       *who_ctx;
    gic_cpu     c[];
} gic;

static gic *from_dist(uam_device *d) { return UAM_CONTAINER_OF(d, gic, dist); }
static gic *from_cpu(uam_device *d) { return UAM_CONTAINER_OF(d, gic, cpu); }

/* The CPU making the current access. */
static unsigned cur(const gic *g)
{
    unsigned c = g->who && g->ncpu > 1 ? g->who(g->who_ctx) : 0;
    return c < g->ncpu ? c : 0;
}

/* The word holding id's bit in one of the five bitmaps, as CPU c sees it. */
enum { ENABLED, LATCH, LEVEL, ACTIVE, EDGE };
static uint32_t *word(gic *g, unsigned c, int kind, unsigned id)
{
    if (id < 32) {
        gic_cpu *b = &g->c[c];
        uint32_t *const w[] = { &b->enabled, &b->latch, &b->level, &b->active, &b->edge };
        return w[kind];
    }
    uint32_t *const m[] = { g->enabled, g->latch, g->level, g->active, g->edge };
    return &m[kind][id / 32];
}

static bool get(const gic *g, unsigned c, int kind, unsigned id)
{
    return (*word((gic *)g, c, kind, id) >> (id % 32)) & 1;
}

static void put(gic *g, unsigned c, int kind, unsigned id, bool v)
{
    uint32_t *w = word(g, c, kind, id);
    *w = v ? *w | (1u << (id % 32)) : *w & ~(1u << (id % 32));
}

static uint8_t *prio_of(gic *g, unsigned c, unsigned id)
{
    return id < 32 ? &g->c[c].prio[id] : &g->prio[id];
}

static bool pending(const gic *g, unsigned c, unsigned id)
{
    if (id < 16)
        return g->c[c].sgi_src[id] != 0;
    return get(g, c, LATCH, id) || (!get(g, c, EDGE, id) && get(g, c, LEVEL, id));
}

/* Whether CPU c can take id: its own banked ids, and the SPIs routed to it. */
static bool routed(const gic *g, unsigned c, unsigned id)
{
    return id < 32 || g->ncpu == 1 || ((g->targets[id] >> c) & 1);
}

/* Active on CPU c. */
static bool active_on(const gic *g, unsigned c, unsigned id)
{
    return get(g, c, ACTIVE, id) && (id < 32 || g->ncpu == 1 || g->owner[id] == c);
}

static unsigned running_prio(gic *g, unsigned c)
{
    unsigned rp = 0x100;
    for (unsigned i = 0; i < g->n; i++)
        if (active_on(g, c, i) && *prio_of(g, c, i) < rp)
            rp = *prio_of(g, c, i);
    return rp;
}

/* CPU c's highest-priority pending, enabled, inactive interrupt (lowest id on
 * ties), or SPURIOUS. */
static unsigned best(gic *g, unsigned c, bool check_masks)
{
    unsigned id = SPURIOUS, p = 0x100;
    for (unsigned i = 0; i < g->n; i++)
        if (routed(g, c, i) && pending(g, c, i) && get(g, c, ENABLED, i) && !get(g, c, ACTIVE, i)
            && *prio_of(g, c, i) < p) {
            id = i;
            p = *prio_of(g, c, i);
        }
    if (id != SPURIOUS && check_masks && (p >= g->c[c].pmr || p >= running_prio(g, c)))
        return SPURIOUS;
    return id;
}

static void update(gic *g)
{
    for (unsigned c = 0; c < g->ncpu; c++)
        uam_signal_set(&g->c[c].irq, g->dist_en && g->c[c].en && best(g, c, true) != SPURIOUS);
}

static void input(gic *g, unsigned c, unsigned id, int level)
{
    if (id < 16 || id >= g->n || c >= g->ncpu)
        return;
    bool was = get(g, c, LEVEL, id);
    put(g, c, LEVEL, id, level);
    if (level && !was && get(g, c, EDGE, id))
        put(g, c, LATCH, id, true);
    update(g);
}

void uam_gic_input(void *ctx, unsigned id, int level) { input(from_dist(ctx), 0, id, level); }

void uam_gic_ppi_input(void *ctx, unsigned in, int level)
{
    input(from_dist(ctx), in / 32, in % 32, level);
}

/* ---- distributor ---- */

static int bitmap_kind(uint64_t off)
{
    switch (off & ~0x7Fu) {
    case 0x100: case 0x180: return ENABLED;
    case 0x200: case 0x280: return LATCH;
    case 0x300: case 0x380: return ACTIVE;
    default: return -1;
    }
}

static uint32_t cfg_word(gic *g, unsigned c, unsigned w)
{
    uint32_t v = 0;
    for (unsigned k = 0; k < 16; k++) {
        unsigned i = w * 16 + k;
        if (i < g->n && get(g, c, EDGE, i))
            v |= 2u << (2 * k);
    }
    return v;
}

/* GICD_ISPENDR/ICPENDR word 0 bits 0-15: SGIs, as pending from the CPU itself. */
static uint32_t sgi_pending_bits(const gic *g, unsigned c)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < 16; i++)
        if (g->c[c].sgi_src[i])
            v |= 1u << i;
    return v;
}

static uam_status dist_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    gic *g = from_dist(d);
    unsigned c = cur(g);
    *val = 0;
    if (off >= 0x400 && off < 0xC00) {                 /* byte registers */
        for (unsigned k = 0; k < size; k++) {
            uint64_t o = off + k;
            unsigned i = (unsigned)(o & 0x3FF);
            uint64_t b = 0;
            if (i < g->n) {
                if (o < 0x800)
                    b = *prio_of(g, c, i);
                else                                    /* ITARGETSR: banked ids name the reader */
                    b = i < 32 || g->ncpu == 1 ? 1u << c : g->targets[i];
            }
            *val |= b << (8 * k);
        }
        return UAM_OK;
    }
    if (size != 4)
        return UAM_OK;
    unsigned w = (unsigned)(off & 0x7F) / 4;
    int kind;
    if (off == 0x000)
        *val = g->dist_en;
    else if (off == 0x004)
        *val = (g->n / 32 - 1) | ((g->ncpu - 1) << 5);  /* ITLinesNumber, CPUNumber */
    else if (off == 0x008)
        *val = 0;                                       /* IIDR: no implementer */
    else if (off >= 0xC00 && off < 0xD00) {
        unsigned cw = (unsigned)(off - 0xC00) / 4;
        if (cw < g->n / 16)
            *val = cfg_word(g, c, cw);
    } else if (off >= 0x200 && off < 0x300) {          /* pending, as software sees it */
        if (w < g->n / 32)
            for (unsigned k = 0; k < 32; k++)
                if (pending(g, c, w * 32 + k))
                    *val |= 1u << k;
    } else if ((kind = bitmap_kind(off)) >= 0 && w < g->n / 32)
        *val = *word(g, c, kind, w * 32);
    return UAM_OK;
}

/* GICD_SGIR from CPU src. */
static void send_sgi(gic *g, unsigned src, uint32_t v)
{
    unsigned id = v & 0xF, list;
    switch ((v >> 24) & 3) {
    case 0:  list = (v >> 16) & 0xFF; break;        /* the target list */
    case 1:  list = 0xFFu & ~(1u << src); break;    /* all but the sender */
    case 2:  list = 1u << src; break;               /* the sender only */
    default: list = 0; break;
    }
    if (g->ncpu == 1)
        list = 1;                                   /* one CPU: every filter reaches it */
    for (unsigned t = 0; t < g->ncpu; t++)
        if ((list >> t) & 1)
            g->c[t].sgi_src[id] |= (uint8_t)(1u << src);
}

static uam_status dist_write(uam_device *d, uint64_t off, unsigned size, uint64_t v64)
{
    gic *g = from_dist(d);
    unsigned c = cur(g);
    uint32_t v = (uint32_t)v64;
    if (off >= 0x400 && off < 0xC00) {
        for (unsigned k = 0; k < size; k++) {
            uint64_t o = off + k;
            unsigned i = (unsigned)(o & 0x3FF);
            uint8_t b = (uint8_t)(v64 >> (8 * k));
            if (i >= g->n)
                continue;
            if (o < 0x800)
                *prio_of(g, c, i) = b;
            else if (i >= 32 && g->ncpu > 1)            /* ITARGETSR of an SPI */
                g->targets[i] = (uint8_t)(b & ((1u << g->ncpu) - 1));
        }
        update(g);
        return UAM_OK;
    }
    if (size != 4)
        return UAM_OK;
    unsigned w = (unsigned)(off & 0x7F) / 4;
    int kind;
    if (off == 0x000) {
        g->dist_en = v & 1;
    } else if (off >= 0xC00 && off < 0xD00) {
        unsigned cw = (unsigned)(off - 0xC00) / 4;
        for (unsigned k = 0; k < 16; k++) {
            unsigned i = cw * 16 + k;
            if (i >= 16 && i < g->n)                   /* SGIs are always edge-triggered */
                put(g, c, EDGE, i, (v >> (2 * k + 1)) & 1);
        }
    } else if (off == 0xF00) {
        send_sgi(g, c, v);
    } else if ((kind = bitmap_kind(off)) >= 0 && w < g->n / 32) {
        bool set = (off & 0x80) == 0;                  /* IS* at 0x?00, IC* at 0x?80 */
        uint32_t *bm = word(g, c, kind, w * 32);
        uint32_t bits = v;
        if (kind == LATCH && w == 0) {                 /* SGIs: pending from the writer itself */
            for (unsigned i = 0; i < 16; i++)
                if ((v >> i) & 1)
                    g->c[c].sgi_src[i] = set ? g->c[c].sgi_src[i] | (uint8_t)(1u << c) : 0;
            bits &= ~0xFFFFu;                          /* their state is sgi_src */
        }
        *bm = set ? *bm | bits : *bm & ~bits;
        if (kind == ACTIVE && w > 0 && set)            /* activated by software: on the writer */
            for (unsigned k = 0; k < 32; k++)
                if ((v >> k) & 1)
                    g->owner[w * 32 + k] = (uint8_t)c;
    }
    update(g);
    return UAM_OK;
}

/* ---- CPU interface ---- */

static uam_status cpu_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    gic *g = from_cpu(d);
    unsigned c = cur(g);
    gic_cpu *b = &g->c[c];
    (void)size;
    *val = 0;
    switch (off) {
    case 0x00: *val = b->en; break;
    case 0x04: *val = b->pmr; break;
    case 0x08: *val = b->bpr; break;
    case 0x0C: {                                       /* IAR */
        unsigned id = g->dist_en && b->en ? best(g, c, true) : SPURIOUS;
        unsigned src = 0;
        if (id < 16) {                                 /* the lowest sender */
            while (!((b->sgi_src[id] >> src) & 1))
                src++;
            b->sgi_src[id] &= (uint8_t)~(1u << src);
            put(g, c, ACTIVE, id, true);
        } else if (id != SPURIOUS) {
            put(g, c, LATCH, id, false);
            put(g, c, ACTIVE, id, true);
            if (id >= 32)
                g->owner[id] = (uint8_t)c;
        }
        if (id != SPURIOUS)
            update(g);
        *val = id | (src << 10);
        break;
    }
    case 0x14: { unsigned rp = running_prio(g, c); *val = rp > 0xFF ? 0xFF : rp; break; }
    case 0x18: *val = best(g, c, false); break;
    case 0xFC: *val = 0x00020000u; break;              /* IIDR: GICv2, no implementer */
    default: break;
    }
    return UAM_OK;
}

static uam_status cpu_write(uam_device *d, uint64_t off, unsigned size, uint64_t v64)
{
    gic *g = from_cpu(d);
    unsigned c = cur(g);
    gic_cpu *b = &g->c[c];
    uint32_t v = (uint32_t)v64;
    (void)size;
    switch (off) {
    case 0x00: b->en = v & 1; break;
    case 0x04: b->pmr = v & 0xFF; break;
    case 0x08: b->bpr = v & 7; break;
    case 0x10: {                                       /* EOIR */
        unsigned id = v & 0x3FF;
        if (id < g->n && active_on(g, c, id))
            put(g, c, ACTIVE, id, false);
        break;
    }
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
    for (unsigned i = 0; i < w; i++)
        g->enabled[i] = g->latch[i] = g->active[i] = g->edge[i] = 0;
    for (unsigned i = 0; i < g->n; i++) {
        g->prio[i] = 0;
        g->targets[i] = 0;
        g->owner[i] = 0;
    }
    for (unsigned c = 0; c < g->ncpu; c++) {
        gic_cpu *b = &g->c[c];
        b->enabled = b->latch = b->active = 0;
        b->edge = 0xFFFFu;                             /* SGIs edge, the rest level */
        for (unsigned i = 0; i < 32; i++)
            b->prio[i] = 0;
        for (unsigned i = 0; i < 16; i++)
            b->sgi_src[i] = 0;
        b->en = false;
        b->pmr = b->bpr = 0;
    }
    g->dist_en = false;
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
    free(g->targets);
    free(g->owner);
    free(g);
}

static void cpu_destroy(uam_device *d) { (void)d; }   /* owned by the distributor */

/*
 * The layout of a one-CPU GIC, then (with several) the rest. Word 0 of each
 * bitmap and priorities 0-31 are CPU 0's bank; its SGIs pending count as
 * latched (from itself).
 */
static void put_bitmap(uam_state_writer *w, const gic *g, int kind)
{
    const gic_cpu *b = &g->c[0];
    const uint32_t word0[] = { b->enabled, b->latch | sgi_pending_bits(g, 0), b->level,
                               b->active, b->edge };
    const uint32_t *const m[] = { g->enabled, g->latch, g->level, g->active, g->edge };
    uam_state_put_u32(w, word0[kind]);
    for (unsigned i = 1; i < g->n / 32; i++)
        uam_state_put_u32(w, m[kind][i]);
}

static void get_bitmap(uam_state_reader *r, gic *g, int kind)
{
    gic_cpu *b = &g->c[0];
    uint32_t *const word0[] = { &b->enabled, &b->latch, &b->level, &b->active, &b->edge };
    uint32_t *const m[] = { g->enabled, g->latch, g->level, g->active, g->edge };
    *word0[kind] = uam_state_get_u32(r);
    for (unsigned i = 1; i < g->n / 32; i++)
        m[kind][i] = uam_state_get_u32(r);
}

static void put_bank(uam_state_writer *w, const gic_cpu *b)
{
    uam_state_put_u32(w, b->enabled);
    uam_state_put_u32(w, b->latch);
    uam_state_put_u32(w, b->level);
    uam_state_put_u32(w, b->active);
    uam_state_put_u32(w, b->edge);
    uam_state_put_bytes(w, b->prio, sizeof(b->prio));
    uam_state_put_bool(w, b->en);
    uam_state_put_u32(w, b->pmr);
    uam_state_put_u32(w, b->bpr);
    uam_state_put_signal(w, &b->irq);
}

static void get_bank(uam_state_reader *r, gic_cpu *b)
{
    b->enabled = uam_state_get_u32(r);
    b->latch = uam_state_get_u32(r);
    b->level = uam_state_get_u32(r);
    b->active = uam_state_get_u32(r);
    b->edge = uam_state_get_u32(r);
    uam_state_get_bytes(r, b->prio, sizeof(b->prio));
    b->en = uam_state_get_bool(r);
    b->pmr = uam_state_get_u32(r);
    b->bpr = uam_state_get_u32(r);
    uam_state_get_signal(r, &b->irq);
}

static void gic_save(uam_device *d, uam_state_writer *w)
{
    gic *g = from_dist(d);
    uam_state_put_u32(w, g->n);
    for (int k = ENABLED; k <= EDGE; k++)
        put_bitmap(w, g, k);
    uam_state_put_bytes(w, g->c[0].prio, 32);
    uam_state_put_bytes(w, g->prio + 32, g->n - 32);
    uam_state_put_bool(w, g->dist_en);
    uam_state_put_bool(w, g->c[0].en);
    uam_state_put_u32(w, g->c[0].pmr);
    uam_state_put_u32(w, g->c[0].bpr);
    uam_state_put_signal(w, &g->c[0].irq);
    if (g->ncpu == 1)
        return;
    uam_state_put_u32(w, g->ncpu);
    for (unsigned c = 1; c < g->ncpu; c++)
        put_bank(w, &g->c[c]);
    for (unsigned c = 0; c < g->ncpu; c++)
        uam_state_put_bytes(w, g->c[c].sgi_src, sizeof(g->c[c].sgi_src));
    uam_state_put_bytes(w, g->targets, g->n);
    uam_state_put_bytes(w, g->owner, g->n);
}

static void gic_restore(uam_device *d, uam_state_reader *r)
{
    gic *g = from_dist(d);
    uam_state_expect_u32(r, g->n, "GIC interrupt count");
    if (!uam_state_ok(r))
        return;
    for (int k = ENABLED; k <= EDGE; k++)
        get_bitmap(r, g, k);
    uam_state_get_bytes(r, g->c[0].prio, 32);
    uam_state_get_bytes(r, g->prio + 32, g->n - 32);
    g->dist_en = uam_state_get_bool(r);
    g->c[0].en = uam_state_get_bool(r);
    g->c[0].pmr = uam_state_get_u32(r);
    g->c[0].bpr = uam_state_get_u32(r);
    uam_state_get_signal(r, &g->c[0].irq);
    /* CPU 0's SGIs: the latch bits (a one-CPU GIC's only sender is itself) */
    for (unsigned i = 0; i < 16; i++)
        g->c[0].sgi_src[i] = (g->c[0].latch >> i) & 1;
    g->c[0].latch &= ~0xFFFFu;
    if (g->ncpu == 1)
        return;
    uam_state_expect_u32(r, g->ncpu, "GIC CPU count");
    if (!uam_state_ok(r))
        return;
    for (unsigned c = 1; c < g->ncpu; c++)
        get_bank(r, &g->c[c]);
    for (unsigned c = 0; c < g->ncpu; c++)
        uam_state_get_bytes(r, g->c[c].sgi_src, sizeof(g->c[c].sgi_src));
    uam_state_get_bytes(r, g->targets, g->n);
    uam_state_get_bytes(r, g->owner, g->n);
}

static const uam_device_ops dist_ops = {
    .type = "arm,gicv2-dist",
    .read = dist_read,
    .write = dist_write,
    .reset = gic_reset,
    .destroy = gic_destroy,
    .save = gic_save,
    .restore = gic_restore,
};

static const uam_device_ops cpu_ops = {
    .type = "arm,gicv2-cpu",
    .impl_min = 4, .impl_max = 4,
    .read = cpu_read,
    .write = cpu_write,
    .destroy = cpu_destroy,
    .flags = UAM_DEVICE_STATELESS,
};

uam_device *uam_gic_create_cpus(const char *name, unsigned n_irqs, unsigned n_cpus,
                                unsigned (*who)(void *ctx), void *who_ctx)
{
    if (n_irqs < 64 || n_irqs > UAM_GIC_MAX_IRQS || n_irqs % 32 || n_cpus < 1
        || n_cpus > UAM_GIC_MAX_CPUS || (n_cpus > 1 && !who))
        return NULL;
    gic *g = calloc(1, sizeof(*g) + n_cpus * sizeof(gic_cpu));
    if (!g)
        return NULL;
    uam_device_init(&g->dist, &dist_ops, name);
    snprintf(g->cpu_name, sizeof(g->cpu_name), "%s-cpu", name);
    uam_device_init(&g->cpu, &cpu_ops, g->cpu_name);
    g->n = n_irqs;
    g->ncpu = n_cpus;
    g->who = who;
    g->who_ctx = who_ctx;
    unsigned w = n_irqs / 32;
    g->enabled = calloc(w, sizeof(uint32_t));
    g->latch = calloc(w, sizeof(uint32_t));
    g->level = calloc(w, sizeof(uint32_t));
    g->active = calloc(w, sizeof(uint32_t));
    g->edge = calloc(w, sizeof(uint32_t));
    g->prio = calloc(n_irqs, 1);
    g->targets = calloc(n_irqs, 1);
    g->owner = calloc(n_irqs, 1);
    if (!g->enabled || !g->latch || !g->level || !g->active || !g->edge || !g->prio
        || !g->targets || !g->owner) {
        gic_destroy(&g->dist);
        return NULL;
    }
    for (unsigned c = 0; c < n_cpus; c++)
        uam_signal_init(&g->c[c].irq);
    gic_reset(&g->dist);
    return &g->dist;
}

uam_device *uam_gic_create(const char *name, unsigned n_irqs)
{
    return uam_gic_create_cpus(name, n_irqs, 1, NULL, NULL);
}

uam_device *uam_gic_cpu_iface(uam_device *d) { return &from_dist(d)->cpu; }
uam_signal *uam_gic_irq_line(uam_device *d) { return &from_dist(d)->c[0].irq; }

uam_signal *uam_gic_cpu_irq_line(uam_device *d, unsigned cpu)
{
    gic *g = from_dist(d);
    return cpu < g->ncpu ? &g->c[cpu].irq : NULL;
}
