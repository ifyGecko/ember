/*
 * Uninitialised-read support: the bitmap of written pages, its place in snapshots, and the
 * arming of the bus's lazily filled pages. See uam/uninit.h.
 */
#include <stdlib.h>
#include <string.h>

#include "uam/uninit.h"

void uam_machine_uninit_fini(uam_machine *m);   /* machine.c */

struct uam_uninit {
    uam_machine     *m;
    uint32_t         pattern;
    uint8_t          pat[4];     /* the pattern in the guest's data byte order */
    bool             armed;
    uam_uninit_touch touch[UAM_UNINIT_TOUCHES];
    uint64_t         n_touch;    /* all of them; touch[] holds the first few */
};

/* Written pages in snapshots: for each RAM region that has a bitmap, its base and size and
 * the words, so that a snapshot made over another map is refused. */
static void save(void *ctx, uam_state_writer *w)
{
    struct uam_uninit *u = ctx;
    const uam_bus *bus = &u->m->bus;
    uint32_t n = 0;
    for (uint32_t i = 0; i < bus->n_regions; i++)
        n += bus->regions[i]->written != NULL;
    uam_state_put_u32(w, n);
    for (uint32_t i = 0; i < bus->n_regions; i++) {
        const uam_region *r = bus->regions[i];
        if (!r->written)
            continue;
        const uint64_t words = (((r->size + UAM_PAGE_SIZE - 1) >> UAM_PAGE_SHIFT) + 63) >> 6;
        uam_state_put_u64(w, r->base);
        uam_state_put_u64(w, r->size);
        for (uint64_t k = 0; k < words; k++)
            uam_state_put_u64(w, r->written[k]);
    }
}

static void restore(void *ctx, uam_state_reader *rd)
{
    struct uam_uninit *u = ctx;
    uam_bus *bus = &u->m->bus;
    uint32_t n = 0;
    for (uint32_t i = 0; i < bus->n_regions; i++)
        n += bus->regions[i]->written != NULL;
    if (uam_state_get_u32(rd) != n) {
        uam_state_fail(rd, "uninit: the snapshot covers another set of RAM regions");
        return;
    }
    for (uint32_t i = 0; i < bus->n_regions && uam_state_ok(rd); i++) {
        uam_region *r = bus->regions[i];
        if (!r->written)
            continue;
        const uint64_t words = (((r->size + UAM_PAGE_SIZE - 1) >> UAM_PAGE_SHIFT) + 63) >> 6;
        if (uam_state_get_u64(rd) != r->base || uam_state_get_u64(rd) != r->size) {
            uam_state_fail(rd, "uninit: region '%s' is not the one the snapshot recorded",
                           r->name ? r->name : "?");
            return;
        }
        for (uint64_t k = 0; k < words; k++)
            r->written[k] = uam_state_get_u64(rd);
    }
}

static struct uam_uninit *get(const uam_machine *m) { return m->uninit; }

uam_status uam_machine_uninit_enable(uam_machine *m, uint32_t pattern)
{
    if (m->uninit)
        return UAM_ERR_INVALID;
    struct uam_uninit *u = calloc(1, sizeof(*u));
    if (!u)
        return UAM_ERR_NOMEM;
    u->m = m;
    u->pattern = pattern;
    const bool be = m->cpu.isa && (m->cpu.isa->default_endian & UAM_ACC_BE);
    for (unsigned i = 0; i < 4; i++)
        u->pat[i] = (uint8_t)(pattern >> (be ? 24 - 8 * i : 8 * i));
    uam_status st = uam_bus_written_track(&m->bus);
    if (st == UAM_OK)
        st = uam_machine_add_state(m, "uninit", save, restore, u);
    if (st != UAM_OK) {
        free(u);
        return st;
    }
    m->uninit = u;
    return UAM_OK;
}

bool uam_machine_uninit_enabled(const uam_machine *m) { return get(m) != NULL; }

uint32_t uam_machine_uninit_pattern(const uam_machine *m)
{
    return get(m) ? get(m)->pattern : 0;
}

void uam_machine_uninit_exclude(uam_machine *m, uint64_t addr, uint64_t len)
{
    if (get(m))
        uam_bus_written_mark(&m->bus, addr, len);
}

void uam_machine_uninit_boot_done(uam_machine *m)
{
    if (get(m))
        uam_bus_written_merge(&m->bus);
}

/* The first access to a waiting page. Only reads are kept: a page whose first access is a
 * write is the ordinary case, and says nothing. */
static void observe(void *ctx, const uam_region *r, uint64_t off, bool write)
{
    struct uam_uninit *u = ctx;
    if (write)
        return;
    if (u->n_touch < UAM_UNINIT_TOUCHES) {
        uam_uninit_touch *t = &u->touch[u->n_touch];
        const uam_machine *m = u->m;
        const uam_cpu *cpu = m->exec.running ? m->exec.running : &m->cpu;
        t->cpu = cpu->hartid;
        t->pc = cpu->insn_pc;   /* a device's DMA shows the last instruction its processor ran */
        t->paddr = r->base + off;
    }
    u->n_touch++;
}

uam_status uam_machine_uninit_arm(uam_machine *m)
{
    struct uam_uninit *u = get(m);
    if (!u)
        return UAM_ERR_INVALID;
    u->n_touch = 0;
    uam_status st = uam_bus_lazy_arm(&m->bus, u->pat, observe, u);
    u->armed = st == UAM_OK;
    return st;
}

void uam_machine_uninit_disarm(uam_machine *m)
{
    struct uam_uninit *u = get(m);
    if (!u || !u->armed)
        return;
    uam_bus_lazy_disarm(&m->bus);
    u->armed = false;
}

bool uam_machine_uninit_armed(const uam_machine *m) { return get(m) && get(m)->armed; }

const uam_uninit_touch *uam_machine_uninit_touches(const uam_machine *m, size_t *n,
                                                   uint64_t *total)
{
    const struct uam_uninit *u = get(m);
    *n = u ? (u->n_touch < UAM_UNINIT_TOUCHES ? (size_t)u->n_touch : UAM_UNINIT_TOUCHES) : 0;
    if (total)
        *total = u ? u->n_touch : 0;
    return u ? u->touch : NULL;
}

void uam_machine_uninit_bytes(const uam_machine *m, uint64_t addr, uint8_t *buf, size_t len)
{
    const struct uam_uninit *u = get(m);
    for (size_t i = 0; i < len; i++)
        buf[i] = u ? u->pat[(addr + i) & 3] : 0;
}

void uam_machine_uninit_fini(uam_machine *m)
{
    free(m->uninit);
    m->uninit = NULL;
}
