/*
 * unimp.c - RAZ/WI catch-all window with per-offset statistics (uam/dev/unimp.h).
 */
#include <stdlib.h>

#include "uam/dev/unimp.h"

#define TABLE_SIZE 1024u  /* distinct offsets tracked; power of two */
#define MAX_PROBE  32u    /* slots an offset may sit past its hash: bounds a miss once full */
#define HOT_LIST   10u

typedef struct slot {
    uint64_t off;
    uint64_t reads, writes;
    uint64_t last_write;
    bool     used;
} slot;

typedef struct unimp {
    uam_device dev;
    uint64_t   base;
    uint64_t   total;
    size_t     n_used;
    slot       table[TABLE_SIZE];
} unimp;

static unimp *to_unimp(uam_device *d) { return UAM_CONTAINER_OF(d, unimp, dev); }

static slot *track(unimp *u, uint64_t off, bool is_write, unsigned size, uint64_t v)
{
    u->total++;
    uint32_t h = (uint32_t)((off * UINT64_C(0x9E3779B97F4A7C15)) >> 54) & (TABLE_SIZE - 1);
    /* Linear probing limited to MAX_PROBE slots, for lookups and inserts alike, so an
     * untracked offset costs MAX_PROBE probes once the table fills, not the whole table
     * (a guest hammering such an offset otherwise scans 1024 slots per access). */
    for (uint32_t probe = 0; probe < MAX_PROBE; probe++) {
        slot *s = &u->table[(h + probe) & (TABLE_SIZE - 1)];
        if (s->used && s->off == off)
            return s;
        if (!s->used) {
            s->used = true;
            s->off = off;
            u->n_used++;
            if (is_write)
                uam_log(UAM_LOG_WARN, "unimp", "%s+0x%llx (0x%llx): first write of %u bytes: 0x%llx",
                        u->dev.name, (unsigned long long)off, (unsigned long long)(u->base + off),
                        size, (unsigned long long)v);
            else
                uam_log(UAM_LOG_WARN, "unimp", "%s+0x%llx (0x%llx): first read of %u bytes",
                        u->dev.name, (unsigned long long)off, (unsigned long long)(u->base + off),
                        size);
            return s;
        }
    }
    return NULL; /* neighbourhood full: still RAZ/WI, just not tracked */
}

static uam_status unimp_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    slot *s = track(to_unimp(d), off, false, size, 0);
    if (s)
        s->reads++;
    *val = 0;
    return UAM_OK;
}

static uam_status unimp_write(uam_device *d, uint64_t off, unsigned size, uint64_t v)
{
    slot *s = track(to_unimp(d), off, true, size, v);
    if (s) {
        s->writes++;
        s->last_write = v;
    }
    return UAM_OK;
}

static void unimp_destroy(uam_device *d)
{
    unimp *u = to_unimp(d);
    if (u->total) {
        uam_log(UAM_LOG_WARN, "unimp", "%s: %llu accesses to %zu unimplemented offsets; hottest:",
                d->name, (unsigned long long)u->total, u->n_used);
        bool shown[TABLE_SIZE] = { false };
        for (unsigned k = 0; k < HOT_LIST; k++) {
            int best = -1;
            for (uint32_t i = 0; i < TABLE_SIZE; i++) {
                const slot *s = &u->table[i];
                if (s->used && !shown[i]
                    && (best < 0 || s->reads + s->writes > u->table[best].reads + u->table[best].writes))
                    best = (int)i;
            }
            if (best < 0)
                break;
            shown[best] = true;
            const slot *s = &u->table[best];
            uam_log(UAM_LOG_WARN, "unimp", "  %s+0x%llx: %llu reads, %llu writes (last 0x%llx)",
                    d->name, (unsigned long long)s->off, (unsigned long long)s->reads,
                    (unsigned long long)s->writes, (unsigned long long)s->last_write);
        }
    }
    free(u);
}

static const uam_device_ops unimp_ops = {
    .type = "uam,unimp",
    .read = unimp_read,
    .write = unimp_write,
    .destroy = unimp_destroy,
    /* Reads are always zero: the table is diagnostics, not guest-visible state. */
    .flags = UAM_DEVICE_STATELESS,
};

uam_device *uam_unimp_create(const char *name, uint64_t base)
{
    unimp *u = calloc(1, sizeof(*u));
    if (!u)
        return NULL;
    uam_device_init(&u->dev, &unimp_ops, name);
    u->base = base;
    return &u->dev;
}

uint64_t uam_unimp_accesses(uam_device *d) { return to_unimp(d)->total; }
size_t   uam_unimp_offsets(uam_device *d) { return to_unimp(d)->n_used; }
