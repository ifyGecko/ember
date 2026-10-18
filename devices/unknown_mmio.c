/*
 * unknown_mmio.c - Logged fallback for unmapped accesses (uam/dev/unknown_mmio.h).
 */
#include <stdlib.h>

#include "uam/dev/unknown_mmio.h"

#define TABLE_SIZE 4096u   /* distinct addresses tracked; power of two */

typedef struct entry {
    uint64_t addr;
    uint64_t value;
    bool     used;
} entry;

typedef struct unknown_mmio {
    uam_device         dev;
    uam_unknown_policy policy;
    size_t             count;
    entry              table[TABLE_SIZE];
} unknown_mmio;

static unknown_mmio *to_um(uam_device *d) { return UAM_CONTAINER_OF(d, unknown_mmio, dev); }

/* Find or create the entry for addr; logs the first access. NULL if full. */
static entry *lookup(unknown_mmio *u, uint64_t addr, bool is_write, unsigned size)
{
    uint32_t h = (uint32_t)((addr * UINT64_C(0x9E3779B97F4A7C15)) >> 52) & (TABLE_SIZE - 1);
    for (uint32_t probe = 0; probe < TABLE_SIZE; probe++) {
        entry *e = &u->table[(h + probe) & (TABLE_SIZE - 1)];
        if (e->used && e->addr == addr)
            return e;
        if (!e->used) {
            e->used = true;
            e->addr = addr;
            e->value = 0;
            u->count++;
            uam_log(UAM_LOG_WARN, "unknown-mmio", "first %s of %u bytes at 0x%llx",
                    is_write ? "write" : "read", size, (unsigned long long)addr);
            return e;
        }
    }
    return NULL;
}

static uam_status um_read(uam_device *d, uint64_t addr, unsigned size, uint64_t *val)
{
    unknown_mmio *u = to_um(d);
    entry *e = lookup(u, addr, false, size);
    *val = (u->policy == UAM_UNKNOWN_POLICY_ECHO && e) ? e->value & uam_mask(size * 8) : 0;
    return UAM_OK;
}

static uam_status um_write(uam_device *d, uint64_t addr, unsigned size, uint64_t v)
{
    unknown_mmio *u = to_um(d);
    entry *e = lookup(u, addr, true, size);
    if (e)
        e->value = v;
    return UAM_OK;
}

static void um_destroy(uam_device *d)
{
    unknown_mmio *u = to_um(d);
    if (u->count)
        uam_log(UAM_LOG_WARN, "unknown-mmio", "%zu distinct unmapped addresses were accessed",
                u->count);
    free(u);
}

/* The echo policy makes the last value written to each address visible. */
static void um_save(uam_device *d, uam_state_writer *w)
{
    unknown_mmio *u = to_um(d);
    uam_state_put_u32(w, (uint32_t)u->count);
    for (uint32_t i = 0; i < TABLE_SIZE; i++) {
        const entry *e = &u->table[i];
        if (!e->used)
            continue;
        uam_state_put_u32(w, i);
        uam_state_put_u64(w, e->addr);
        uam_state_put_u64(w, e->value);
    }
}

static void um_restore(uam_device *d, uam_state_reader *r)
{
    unknown_mmio *u = to_um(d);
    uint32_t n = uam_state_get_u32(r);
    if (n > TABLE_SIZE)
        uam_state_fail(r, "%u addresses saved, the table holds %u", (unsigned)n, TABLE_SIZE);
    if (!uam_state_ok(r))
        return;
    for (uint32_t i = 0; i < TABLE_SIZE; i++)
        u->table[i].used = false;
    u->count = n;
    for (uint32_t k = 0; k < n && uam_state_ok(r); k++) {
        uint32_t i = uam_state_get_u32(r);
        if (i >= TABLE_SIZE) {
            uam_state_fail(r, "table slot %u out of range", (unsigned)i);
            return;
        }
        entry *e = &u->table[i];
        e->used = true;
        e->addr = uam_state_get_u64(r);
        e->value = uam_state_get_u64(r);
    }
}

static const uam_device_ops um_ops = {
    .type = "uam,unknown-mmio",
    .read = um_read,
    .write = um_write,
    .destroy = um_destroy,
    .save = um_save,
    .restore = um_restore,
};

uam_device *uam_unknown_mmio_create(const char *name, uam_unknown_policy policy)
{
    unknown_mmio *u = calloc(1, sizeof(*u));
    if (!u)
        return NULL;
    uam_device_init(&u->dev, &um_ops, name);
    u->policy = policy;
    return &u->dev;
}

size_t uam_unknown_mmio_count(uam_device *d) { return to_um(d)->count; }
