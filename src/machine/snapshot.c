/*
 * snapshot.c - Machine snapshots (uam/snapshot.h).
 *
 * A snapshot is two parts:
 *   - the component state: a state stream (uam/state.h) with one record per
 *     component - "cpu", "isa", "sched", "exec", "dev/<key>", "state/<name>";
 *   - memory: a copy of each RAM/ROM/ROMD backing store, held in an anonymous
 *     mapping so that all-zero pages, which are never copied, cost nothing.
 *
 * File layout (all integers little-endian):
 *     "UAMSNAP\0", u32 version, u32 flags (0)
 *     str board, str isa                     (str = u16 length + bytes)
 *     u64 pc, u64 icount, u64 now
 *     u64 state length, state stream
 *     u32 region count, then per region:
 *         str name, u64 base, u64 size, u8 kind, u8 rom_mode, u64 page count,
 *         page count x (u64 page index, page bytes; the last page of a region
 *         may be short)
 *     u64 checksum of every byte before it
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "uam/snapshot.h"

#define SNAP_MAGIC   "UAMSNAP"   /* 8 bytes with the NUL */
#define SNAP_VERSION 1u

typedef struct snap_region {
    char     *name;
    uint64_t  base, size;
    uint8_t   kind;
    bool      rom_mode;
    uint8_t  *data;        /* size bytes, anonymous mapping; absent pages read as zero */
    uint64_t *present;     /* pages that were copied (non-zero when taken) */
    uint64_t  n_present;
} snap_region;

struct uam_snapshot {
    uint64_t     id;       /* unique per process; names the machine's baseline */
    char        *board, *isa;
    uint64_t     pc, icount, now;
    uint8_t     *state;
    size_t       state_len;
    uint32_t     n_regions;
    snap_region *regions;
};

static uint64_t next_snapshot_id;   /* atomic: machines may live in different threads */

static uint64_t new_snapshot_id(void)
{
    return __atomic_add_fetch(&next_snapshot_id, 1, __ATOMIC_RELAXED);
}

static const char *str_or_empty(const char *s) { return s ? s : ""; }

static uint64_t region_pages(uint64_t size) { return (size + UAM_PAGE_SIZE - 1) >> UAM_PAGE_SHIFT; }

static bool test_bit(const uint64_t *bm, uint64_t i) { return (bm[i >> 6] >> (i & 63)) & 1; }
static void set_bit(uint64_t *bm, uint64_t i) { bm[i >> 6] |= UINT64_C(1) << (i & 63); }

static bool has_backing(const uam_region *r)
{
    return r->kind == UAM_REGION_RAM || r->kind == UAM_REGION_ROM || r->kind == UAM_REGION_ROMD;
}

/* The i-th backed region of the bus, in map order, or NULL. */
static uam_region *backed_region(uam_bus *bus, uint32_t i)
{
    for (uint32_t k = 0; k < bus->n_regions; k++)
        if (has_backing(bus->regions[k]) && i-- == 0)
            return bus->regions[k];
    return NULL;
}

static uint32_t n_backed_regions(const uam_bus *bus)
{
    uint32_t n = 0;
    for (uint32_t k = 0; k < bus->n_regions; k++)
        n += has_backing(bus->regions[k]);
    return n;
}

static bool page_is_zero(const uint8_t *p, size_t n)
{
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        if (w)
            return false;
    }
    for (; i < n; i++)
        if (p[i])
            return false;
    return true;
}

/* ---- snapshot objects ---------------------------------------------------- */

static void free_regions(snap_region *rs, uint32_t n)
{
    for (uint32_t i = 0; rs && i < n; i++) {
        free(rs[i].name);
        if (rs[i].data)
            munmap(rs[i].data, rs[i].size);
        free(rs[i].present);
    }
    free(rs);
}

void uam_snapshot_free(uam_snapshot *s)
{
    if (!s)
        return;
    free(s->board);
    free(s->isa);
    free(s->state);
    free_regions(s->regions, s->n_regions);
    free(s);
}

static uam_status region_alloc(snap_region *sr, const char *name, uint64_t base, uint64_t size)
{
    sr->name = strdup(name);
    sr->base = base;
    sr->size = size;
    sr->present = calloc((region_pages(size) + 63) / 64, sizeof(uint64_t));
    void *p = size ? mmap(NULL, size, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0)
                   : MAP_FAILED;
    sr->data = p == MAP_FAILED ? NULL : p;
    return sr->name && sr->present && sr->data ? UAM_OK : UAM_ERR_NOMEM;
}

void uam_snapshot_get_info(const uam_snapshot *s, uam_snapshot_info *info)
{
    memset(info, 0, sizeof(*info));
    info->board = s->board;
    info->isa = s->isa;
    info->pc = s->pc;
    info->icount = s->icount;
    info->now = s->now;
    info->state_bytes = s->state_len;
    for (uint32_t i = 0; i < s->n_regions; i++) {
        info->mem_bytes += s->regions[i].size;
        info->mem_pages += s->regions[i].n_present;
    }
}

/* ---- component records --------------------------------------------------- */

static bool device_saved(const uam_device *d)
{
    return !(d->ops->flags & UAM_DEVICE_STATELESS);
}

static const char *device_base(const uam_device *d)
{
    return d->name ? d->name : d->ops->type ? d->ops->type : "device";
}

/* The devices whose state is saved, in machine order, with their record
 * names: "dev/" + name (else type), "#n" appended for the n-th repeat. */
#define KEY_MAX 300
typedef struct dev_entry {
    uam_device *dev;
    char        key[KEY_MAX];
} dev_entry;

typedef struct dev_table {
    dev_entry *e;
    uint32_t   n;
    uint32_t   cursor;   /* records come in machine order: try here first */
} dev_table;

static bool dev_table_build(const uam_machine *m, dev_table *t)
{
    uint32_t n = 0;
    for (const uam_device *d = m->devices; d; d = d->next)
        n += device_saved(d);
    t->e = calloc(n ? n : 1, sizeof(*t->e));
    t->n = 0;
    t->cursor = 0;
    if (!t->e)
        return false;
    for (uam_device *d = m->devices; d; d = d->next) {
        if (!device_saved(d))
            continue;
        const char *base = device_base(d);
        unsigned dup = 0;
        for (const uam_device *o = m->devices; o != d; o = o->next)
            dup += strcmp(device_base(o), base) == 0;
        dev_entry *e = &t->e[t->n++];
        e->dev = d;
        if (dup)
            snprintf(e->key, sizeof(e->key), "dev/%s#%u", base, dup);
        else
            snprintf(e->key, sizeof(e->key), "dev/%s", base);
    }
    return true;
}

static uam_device *dev_table_find(dev_table *t, const char *key)
{
    for (uint32_t k = 0; k < t->n; k++) {
        uint32_t i = (t->cursor + k) % t->n;
        if (!strcmp(t->e[i].key, key)) {
            t->cursor = i + 1;
            return t->e[i].dev;
        }
    }
    return NULL;
}

static void save_cpu(const uam_cpu *cpu, uam_state_writer *w)
{
    uam_state_put_u32(w, (uint32_t)cpu->regfile_size);
    uam_state_put_u64(w, cpu->pc);
    uam_state_put_bytes(w, cpu->regfile, cpu->regfile_size);
    uam_state_put_u64(w, cpu->icount);
    uam_state_put_bool(w, cpu->waiting);
    uam_state_put_u32(w, cpu->priv);
    uam_state_put_bool(w, cpu->resv.valid);
    uam_state_put_u64(w, cpu->resv.addr);
    uam_state_put_u32(w, cpu->resv.size);
}

static void restore_cpu(uam_cpu *cpu, uam_state_reader *r)
{
    uam_state_expect_u32(r, (uint32_t)cpu->regfile_size, "register file size");
    cpu->pc = uam_state_get_u64(r);
    uam_state_get_bytes(r, cpu->regfile, cpu->regfile_size);
    cpu->icount = uam_state_get_u64(r);
    cpu->waiting = uam_state_get_bool(r);
    cpu->priv = uam_state_get_u32(r);
    cpu->resv.valid = uam_state_get_bool(r);
    cpu->resv.addr = uam_state_get_u64(r);
    cpu->resv.size = uam_state_get_u32(r);
}

static void save_exec(const uam_exec *x, uam_state_writer *w)
{
    uam_state_put_bool(w, x->reset_request);
    uam_state_put_u32(w, (uint32_t)x->reset_kind);
    uam_state_put_bool(w, x->hook_fired);
    uam_state_put_u64(w, x->hook_fired_pc);
    uam_state_put_u64(w, x->hook_fired_icount);
    uam_state_put_bool(w, x->last_fetch_fault_valid);
    uam_state_put_u64(w, x->last_fetch_fault_pc);
    uam_state_put_u64(w, x->last_fetch_fault_icount);
    uam_state_put_u64(w, x->stall);
    uam_state_put_u64(w, x->stall_icount);
}

static void restore_exec(uam_exec *x, uam_state_reader *r)
{
    x->reset_request = uam_state_get_bool(r);
    x->reset_kind = uam_state_get_u32(r) == UAM_RESET_CPU ? UAM_RESET_CPU : UAM_RESET_SYSTEM;
    x->hook_fired = uam_state_get_bool(r);
    x->hook_fired_pc = uam_state_get_u64(r);
    x->hook_fired_icount = uam_state_get_u64(r);
    x->last_fetch_fault_valid = uam_state_get_bool(r);
    x->last_fetch_fault_pc = uam_state_get_u64(r);
    x->last_fetch_fault_icount = uam_state_get_u64(r);
    x->stall = uam_state_get_u64(r);
    x->stall_icount = uam_state_get_u64(r);
}

/* Write every component's record. Returns false (logged) if one cannot be saved. */
static bool save_components(uam_machine *m, uam_state_writer *w, dev_table *devs)
{
    const uam_isa *isa = m->cpu.isa;
    if (!isa->save || !isa->restore) {
        uam_log(UAM_LOG_ERROR, "snapshot", "ISA %s does not support snapshots", isa->name);
        return false;
    }
    for (uam_device *d = m->devices; d; d = d->next) {
        if (device_saved(d) && (!d->ops->save || !d->ops->restore)) {
            uam_log(UAM_LOG_ERROR, "snapshot", "device %s (%s) has no save/restore; implement them "
                    "(uam/state.h) or mark it UAM_DEVICE_STATELESS", str_or_empty(d->name),
                    str_or_empty(d->ops->type));
            return false;
        }
    }

    uam_state_begin(w, "cpu");
    save_cpu(&m->cpu, w);
    uam_state_end(w);
    uam_state_begin(w, "isa");
    isa->save(&m->cpu, w);
    uam_state_end(w);
    uam_state_begin(w, "sched");
    uam_state_put_u64(w, m->sched.idle);
    uam_state_end(w);
    uam_state_begin(w, "exec");
    save_exec(&m->exec, w);
    uam_state_end(w);
    for (uint32_t i = 0; i < devs->n; i++) {
        uam_state_begin(w, devs->e[i].key);
        devs->e[i].dev->ops->save(devs->e[i].dev, w);
        uam_state_end(w);
    }
    for (uint32_t i = 0; i < m->n_state_parts; i++) {
        char key[300];
        snprintf(key, sizeof(key), "state/%s", m->state_parts[i].name);
        uam_state_begin(w, key);
        m->state_parts[i].save(m->state_parts[i].ctx, w);
        uam_state_end(w);
    }
    if (!uam_state_writer_ok(w)) {
        uam_log(UAM_LOG_ERROR, "snapshot", "out of memory saving the machine state");
        return false;
    }

    /* A timer armed by nobody's save would be lost by a restore. */
    for (const uam_timer *t = m->sched.head; t; t = t->next) {
        if (!uam_state_writer_saved_timer(w, t)) {
            uam_log(UAM_LOG_ERROR, "snapshot", "an armed timer (deadline %llu) is not saved by its "
                    "owner; the device, ISA or extra state that arms it must save it with "
                    "uam_state_put_timer", (unsigned long long)t->when);
            return false;
        }
    }
    return true;
}

/* ---- take ----------------------------------------------------------------- */

/* Make s the machine's baseline: memory now equals it, track writes from here. */
static void set_baseline(uam_machine *m, const uam_snapshot *s)
{
    uam_bus_dirty_reset(&m->bus);
    uam_cpu_dtlb_flush(&m->cpu);   /* cached write access to now-clean pages */
    m->snap_base = s->id;
}

uam_status uam_snapshot_take(uam_machine *m, uam_snapshot **out)
{
    *out = NULL;
    uam_snapshot *s = calloc(1, sizeof(*s));
    uam_state_writer *w = uam_state_writer_new(&m->sched);
    dev_table devs = { 0 };
    if (!s || !w || !dev_table_build(m, &devs)) {
        free(s);
        uam_state_writer_free(w);
        free(devs.e);
        return UAM_ERR_NOMEM;
    }
    bool saved = save_components(m, w, &devs);
    free(devs.e);
    if (!saved) {
        uam_state_writer_free(w);
        free(s);
        return UAM_ERR_INVALID;
    }
    size_t len;
    const uint8_t *data = uam_state_writer_data(w, &len);
    s->state = malloc(len ? len : 1);
    if (s->state)
        memcpy(s->state, data, len);
    s->state_len = len;
    uam_state_writer_free(w);

    s->board = strdup(str_or_empty(m->board_name));
    s->isa = strdup(m->cpu.isa->name);
    s->pc = m->cpu.pc;
    s->icount = m->cpu.icount;
    s->now = uam_sched_now(&m->sched);
    s->n_regions = n_backed_regions(&m->bus);
    s->regions = calloc(s->n_regions ? s->n_regions : 1, sizeof(*s->regions));
    if (!s->state || !s->board || !s->isa || !s->regions) {
        uam_snapshot_free(s);
        return UAM_ERR_NOMEM;
    }

    for (uint32_t i = 0; i < s->n_regions; i++) {
        const uam_region *r = backed_region(&m->bus, i);
        snap_region *sr = &s->regions[i];
        if (region_alloc(sr, str_or_empty(r->name), r->base, r->size) != UAM_OK) {
            uam_snapshot_free(s);
            return UAM_ERR_NOMEM;
        }
        sr->kind = (uint8_t)r->kind;
        sr->rom_mode = r->rom_mode;
        for (uint64_t p = 0, np = region_pages(r->size); p < np; p++) {
            uint64_t off = p << UAM_PAGE_SHIFT;
            size_t n = (size_t)(r->size - off < UAM_PAGE_SIZE ? r->size - off : UAM_PAGE_SIZE);
            if (page_is_zero(r->host + off, n))
                continue;
            memcpy(sr->data + off, r->host + off, n);
            set_bit(sr->present, p);
            sr->n_present++;
        }
    }

    s->id = new_snapshot_id();
    set_baseline(m, s);
    *out = s;
    return UAM_OK;
}

/* ---- restore -------------------------------------------------------------- */

typedef enum { REC_CPU, REC_ISA, REC_SCHED, REC_EXEC, REC_DEV, REC_PART, REC_UNKNOWN } rec_kind;

/* What a record name refers to in machine m. */
static rec_kind classify(uam_machine *m, dev_table *devs, const char *name, uam_device **dev,
                         uam_state_part **part)
{
    if (!strcmp(name, "cpu"))   return REC_CPU;
    if (!strcmp(name, "isa"))   return REC_ISA;
    if (!strcmp(name, "sched")) return REC_SCHED;
    if (!strcmp(name, "exec"))  return REC_EXEC;
    if (!strncmp(name, "dev/", 4) && (*dev = dev_table_find(devs, name)))
        return REC_DEV;
    if (!strncmp(name, "state/", 6)) {
        for (uint32_t i = 0; i < m->n_state_parts; i++) {
            if (!strcmp(m->state_parts[i].name, name + 6)) {
                *part = &m->state_parts[i];
                return REC_PART;
            }
        }
    }
    return REC_UNKNOWN;
}

/* Everything that can be checked without changing the machine. */
static bool compatible(uam_machine *m, const uam_snapshot *s, dev_table *devs)
{
    if (strcmp(s->board, str_or_empty(m->board_name)) || strcmp(s->isa, m->cpu.isa->name)) {
        uam_log(UAM_LOG_ERROR, "snapshot", "snapshot is of board '%s' (%s), the machine is '%s' (%s)",
                s->board, s->isa, str_or_empty(m->board_name), m->cpu.isa->name);
        return false;
    }
    if (!m->cpu.isa->restore) {
        uam_log(UAM_LOG_ERROR, "snapshot", "ISA %s does not support snapshots", m->cpu.isa->name);
        return false;
    }
    if (s->n_regions != n_backed_regions(&m->bus)) {
        uam_log(UAM_LOG_ERROR, "snapshot", "snapshot has %u memory regions, the machine %u",
                (unsigned)s->n_regions, (unsigned)n_backed_regions(&m->bus));
        return false;
    }
    for (uint32_t i = 0; i < s->n_regions; i++) {
        const uam_region *r = backed_region(&m->bus, i);
        const snap_region *sr = &s->regions[i];
        if (strcmp(sr->name, str_or_empty(r->name)) || sr->base != r->base || sr->size != r->size
            || sr->kind != (uint8_t)r->kind) {
            uam_log(UAM_LOG_ERROR, "snapshot", "memory region %u differs: snapshot '%s' at 0x%llx "
                    "(0x%llx bytes), machine '%s' at 0x%llx (0x%llx bytes)", (unsigned)i, sr->name,
                    (unsigned long long)sr->base, (unsigned long long)sr->size,
                    str_or_empty(r->name), (unsigned long long)r->base, (unsigned long long)r->size);
            return false;
        }
    }

    /* Records and components must match one to one. */
    uam_state_reader *rd = uam_state_reader_new(s->state, s->state_len, NULL);
    if (!rd)
        return false;
    bool ok = true;
    unsigned n_core = 0, n_dev = 0, n_part = 0;
    const char *name;
    while (ok && (name = uam_state_next(rd))) {
        uam_device *d = NULL;
        uam_state_part *p = NULL;
        switch (classify(m, devs, name, &d, &p)) {
        case REC_CPU: case REC_ISA: case REC_SCHED: case REC_EXEC: n_core++; break;
        case REC_DEV:
            n_dev++;
            if (!d->ops->restore) {
                uam_log(UAM_LOG_ERROR, "snapshot", "device %s cannot restore its state", name + 4);
                ok = false;
            }
            break;
        case REC_PART: n_part++; break;
        case REC_UNKNOWN:
            uam_log(UAM_LOG_ERROR, "snapshot", "snapshot holds state for '%s', which this machine "
                    "does not have", name);
            ok = false;
            break;
        }
        uam_state_skip(rd);
    }
    if (ok && !uam_state_ok(rd)) {
        uam_log(UAM_LOG_ERROR, "snapshot", "corrupt state: %s", uam_state_error(rd));
        ok = false;
    }
    uam_state_reader_free(rd);
    if (!ok)
        return false;

    unsigned want_dev = devs->n;
    if (n_core != 4 || n_dev != want_dev || n_part != m->n_state_parts) {
        uam_log(UAM_LOG_ERROR, "snapshot", "snapshot has state for %u devices and %u extra parts, "
                "the machine has %u and %u (or a core record is missing)", n_dev, n_part, want_dev,
                (unsigned)m->n_state_parts);
        return false;
    }
    return true;
}

static bool restore_components(uam_machine *m, const uam_snapshot *s, dev_table *devs)
{
    uam_state_reader *r = uam_state_reader_new(s->state, s->state_len, &m->sched);
    if (!r) {
        uam_log(UAM_LOG_ERROR, "snapshot", "out of memory");
        return false;
    }
    /* Owners re-arm their own timers; anything they do not re-arm stays idle. */
    uam_sched_cancel_all(&m->sched);
    const char *name;
    while ((name = uam_state_next(r))) {
        uam_device *d = NULL;
        uam_state_part *p = NULL;
        switch (classify(m, devs, name, &d, &p)) {
        case REC_CPU:   restore_cpu(&m->cpu, r); break;
        case REC_ISA:   m->cpu.isa->restore(&m->cpu, r); break;
        case REC_SCHED: m->sched.idle = uam_state_get_u64(r); break;
        case REC_EXEC:  restore_exec(&m->exec, r); break;
        case REC_DEV:   d->ops->restore(d, r); break;
        case REC_PART:  p->restore(p->ctx, r); break;
        case REC_UNKNOWN: uam_state_fail(r, "unexpected record"); break;
        }
        uam_state_leave(r, name);
    }
    bool ok = uam_state_ok(r);
    if (ok)
        uam_state_arm_timers(r);
    else
        uam_log(UAM_LOG_ERROR, "snapshot", "restore failed: %s", uam_state_error(r));
    uam_state_reader_free(r);
    return ok;
}

/* Bring backed memory to the snapshot's contents. Pages that already match
 * are left alone (their translated code stays valid). */
static void restore_memory(uam_machine *m, const uam_snapshot *s)
{
    const bool incremental = m->snap_base == s->id && m->bus.track_dirty;
    for (uint32_t i = 0; i < s->n_regions; i++) {
        uam_region *r = backed_region(&m->bus, i);
        const snap_region *sr = &s->regions[i];
        for (uint64_t p = 0, np = region_pages(r->size); p < np; p++) {
            if (incremental) {
                /* Skip clean pages a bitmap word at a time. */
                if (!r->dirty[p >> 6]) {
                    p |= 63;
                    continue;
                }
                if (!uam_region_page_dirty(r, p))
                    continue;
            }
            uint64_t off = p << UAM_PAGE_SHIFT;
            size_t n = (size_t)(r->size - off < UAM_PAGE_SIZE ? r->size - off : UAM_PAGE_SIZE);
            if (memcmp(r->host + off, sr->data + off, n))
                uam_bus_region_store(&m->bus, r, off, sr->data + off, n);
        }
        if (r->kind == UAM_REGION_ROMD)
            uam_bus_romd_set_mode(&m->bus, r, sr->rom_mode);
    }
}

uam_status uam_snapshot_restore(uam_machine *m, const uam_snapshot *s)
{
    dev_table devs;
    if (!dev_table_build(m, &devs))
        return UAM_ERR_NOMEM;
    if (!compatible(m, s, &devs)) {
        free(devs.e);
        return UAM_ERR_INVALID;
    }
    /* Memory first, so that device and extra-state restore callbacks see the
     * restored memory (to resync a flash file, re-read descriptors, ...). */
    restore_memory(m, s);
    devs.cursor = 0;
    bool ok = restore_components(m, s, &devs);
    free(devs.e);
    if (!ok) {
        m->snap_base = 0;
        return UAM_ERR_INVALID;
    }
    set_baseline(m, s);
    /* A stop requested after the snapshot (the firmware finishing, SIGINT)
     * belongs to the run that is being undone: the restored machine runs. */
    m->exec.stop_request = false;
    m->exec.stop_status = 0;
    /* TLB contents and privilege may differ: drop mapped translations and
     * stop any block in flight at the next boundary. */
    uam_cpu_xlate_changed(&m->cpu);
    return UAM_OK;
}

/* ---- files ---------------------------------------------------------------- */

/* Streaming checksum: independent of how the bytes are split into calls. */
typedef struct hasher {
    uint64_t h;
    uint8_t  buf[8];
    unsigned n;
} hasher;

static void hash_word(hasher *hs, uint64_t w)
{
    hs->h = (hs->h ^ w) * UINT64_C(0x9E3779B97F4A7C15);
    hs->h ^= hs->h >> 29;
}

static void hash_update(hasher *hs, const void *data, size_t len)
{
    const uint8_t *p = data;
    while (len && hs->n) {
        hs->buf[hs->n++] = *p++;
        len--;
        if (hs->n == 8) {
            uint64_t w;
            memcpy(&w, hs->buf, 8);
            hash_word(hs, w);
            hs->n = 0;
        }
    }
    for (; len >= 8; p += 8, len -= 8) {
        uint64_t w;
        memcpy(&w, p, 8);
        hash_word(hs, w);
    }
    while (len--)
        hs->buf[hs->n++] = *p++;
}

static uint64_t hash_final(const hasher *hs)
{
    hasher t = *hs;
    uint64_t w = 0;
    memcpy(&w, t.buf, t.n);
    hash_word(&t, w ^ ((uint64_t)t.n << 56));
    return t.h;
}

typedef struct sfile {
    FILE  *f;
    hasher hs;
    bool   err;
} sfile;

static void fput(sfile *sf, const void *p, size_t n)
{
    if (sf->err || !n)
        return;
    hash_update(&sf->hs, p, n);
    if (fwrite(p, 1, n, sf->f) != n)
        sf->err = true;
}

static void fput_le(sfile *sf, uint64_t v, unsigned n)
{
    uint8_t b[8];
    for (unsigned i = 0; i < n; i++)
        b[i] = (uint8_t)(v >> (8 * i));
    fput(sf, b, n);
}

static void fput_str(sfile *sf, const char *s)
{
    size_t n = strlen(s);
    fput_le(sf, n, 2);
    fput(sf, s, n);
}

uam_status uam_snapshot_save(const uam_snapshot *s, const char *path)
{
    size_t plen = strlen(path);
    char *tmp = malloc(plen + 8);
    if (!tmp)
        return UAM_ERR_NOMEM;
    memcpy(tmp, path, plen);
    memcpy(tmp + plen, ".tmp", 5);
    sfile sf = { .f = fopen(tmp, "wb"), .hs = { .h = 0 } };
    if (!sf.f) {
        uam_log(UAM_LOG_ERROR, "snapshot", "cannot create %s: %s", tmp, strerror(errno));
        free(tmp);
        return UAM_ERR_INVALID;
    }
    setvbuf(sf.f, NULL, _IOFBF, 1u << 20);

    fput(&sf, SNAP_MAGIC, 8);
    fput_le(&sf, SNAP_VERSION, 4);
    fput_le(&sf, 0, 4);
    fput_str(&sf, s->board);
    fput_str(&sf, s->isa);
    fput_le(&sf, s->pc, 8);
    fput_le(&sf, s->icount, 8);
    fput_le(&sf, s->now, 8);
    fput_le(&sf, s->state_len, 8);
    fput(&sf, s->state, s->state_len);
    fput_le(&sf, s->n_regions, 4);
    for (uint32_t i = 0; i < s->n_regions; i++) {
        const snap_region *sr = &s->regions[i];
        fput_str(&sf, sr->name);
        fput_le(&sf, sr->base, 8);
        fput_le(&sf, sr->size, 8);
        fput_le(&sf, sr->kind, 1);
        fput_le(&sf, sr->rom_mode, 1);
        fput_le(&sf, sr->n_present, 8);
        for (uint64_t p = 0, np = region_pages(sr->size); p < np; p++) {
            if (!test_bit(sr->present, p))
                continue;
            uint64_t off = p << UAM_PAGE_SHIFT;
            fput_le(&sf, p, 8);
            fput(&sf, sr->data + off, (size_t)(sr->size - off < UAM_PAGE_SIZE ? sr->size - off
                                                                              : UAM_PAGE_SIZE));
        }
    }
    uint64_t sum = hash_final(&sf.hs);
    fput_le(&sf, sum, 8);

    bool ok = !sf.err && fflush(sf.f) == 0;
    ok = fclose(sf.f) == 0 && ok;
    if (ok && rename(tmp, path) != 0)
        ok = false;
    if (!ok) {
        uam_log(UAM_LOG_ERROR, "snapshot", "writing %s failed: %s", path, strerror(errno));
        remove(tmp);
    }
    free(tmp);
    return ok ? UAM_OK : UAM_ERR_INVALID;
}

static bool fget(sfile *sf, void *p, size_t n)
{
    if (sf->err || !n)
        return !sf->err;
    if (fread(p, 1, n, sf->f) != n) {
        sf->err = true;
        return false;
    }
    hash_update(&sf->hs, p, n);
    return true;
}

static uint64_t fget_le(sfile *sf, unsigned n)
{
    uint8_t b[8] = { 0 };
    fget(sf, b, n);
    uint64_t v = 0;
    for (unsigned i = 0; i < n; i++)
        v |= (uint64_t)b[i] << (8 * i);
    return v;
}

static char *fget_str(sfile *sf)
{
    size_t n = (size_t)fget_le(sf, 2);
    char *s = malloc(n + 1);
    if (!s) {
        sf->err = true;
        return NULL;
    }
    fget(sf, s, n);
    s[n] = 0;
    return s;
}

/* Region sizes beyond this are taken as corruption, not as a request to map them. */
#define SNAP_MAX_REGION (UINT64_C(1) << 40)
#define SNAP_MAX_STATE  (UINT64_C(1) << 32)
#define SNAP_MAX_REGIONS 4096u

uam_status uam_snapshot_load(const char *path, uam_snapshot **out)
{
    *out = NULL;
    sfile sf = { .f = fopen(path, "rb") };
    if (!sf.f) {
        uam_log(UAM_LOG_ERROR, "snapshot", "cannot open %s: %s", path, strerror(errno));
        return UAM_ERR_INVALID;
    }
    setvbuf(sf.f, NULL, _IOFBF, 1u << 20);
    uam_snapshot *s = calloc(1, sizeof(*s));
    const char *why = "truncated or unreadable";
    uam_status st = UAM_ERR_INVALID;
    if (!s) {
        st = UAM_ERR_NOMEM;
        goto fail;
    }

    char magic[8];
    if (!fget(&sf, magic, 8) || memcmp(magic, SNAP_MAGIC, 8)) {
        why = "not a UAM snapshot";
        goto fail;
    }
    uint32_t version = (uint32_t)fget_le(&sf, 4);
    fget_le(&sf, 4);
    if (!sf.err && version != SNAP_VERSION) {
        why = "unsupported snapshot format version";
        goto fail;
    }
    s->board = fget_str(&sf);
    s->isa = fget_str(&sf);
    s->pc = fget_le(&sf, 8);
    s->icount = fget_le(&sf, 8);
    s->now = fget_le(&sf, 8);
    uint64_t slen = fget_le(&sf, 8);
    if (sf.err || slen > SNAP_MAX_STATE)
        goto fail;
    s->state_len = (size_t)slen;
    s->state = malloc(s->state_len ? s->state_len : 1);
    if (!s->state) {
        st = UAM_ERR_NOMEM;
        goto fail;
    }
    if (!fget(&sf, s->state, s->state_len))
        goto fail;
    uint32_t nreg = (uint32_t)fget_le(&sf, 4);
    if (sf.err || nreg > SNAP_MAX_REGIONS)
        goto fail;
    s->regions = calloc(nreg ? nreg : 1, sizeof(*s->regions));
    if (!s->regions) {
        st = UAM_ERR_NOMEM;
        goto fail;
    }
    s->n_regions = nreg;
    for (uint32_t i = 0; i < nreg; i++) {
        snap_region *sr = &s->regions[i];
        char *name = fget_str(&sf);
        uint64_t base = fget_le(&sf, 8), size = fget_le(&sf, 8);
        if (sf.err || !name || size == 0 || size > SNAP_MAX_REGION) {
            free(name);
            goto fail;
        }
        st = region_alloc(sr, name, base, size);
        free(name);
        if (st != UAM_OK)
            goto fail;
        st = UAM_ERR_INVALID;
        sr->kind = (uint8_t)fget_le(&sf, 1);
        sr->rom_mode = fget_le(&sf, 1) != 0;
        uint64_t npres = fget_le(&sf, 8), np = region_pages(size), last = 0;
        if (sf.err || npres > np)
            goto fail;
        for (uint64_t k = 0; k < npres; k++) {
            uint64_t p = fget_le(&sf, 8);
            if (sf.err || p >= np || (k && p <= last)) {
                why = "corrupt page table";
                goto fail;
            }
            last = p;
            uint64_t off = p << UAM_PAGE_SHIFT;
            if (!fget(&sf, sr->data + off, (size_t)(size - off < UAM_PAGE_SIZE ? size - off
                                                                               : UAM_PAGE_SIZE)))
                goto fail;
            set_bit(sr->present, p);
        }
        sr->n_present = npres;
    }
    uint64_t want = hash_final(&sf.hs);
    uint64_t sum = fget_le(&sf, 8);
    if (sf.err)
        goto fail;
    if (sum != want) {
        why = "checksum mismatch (corrupt file)";
        goto fail;
    }
    fclose(sf.f);
    s->id = new_snapshot_id();
    *out = s;
    return UAM_OK;

fail:
    if (st != UAM_ERR_NOMEM)
        uam_log(UAM_LOG_ERROR, "snapshot", "cannot load %s: %s", path, why);
    fclose(sf.f);
    uam_snapshot_free(s);
    return st;
}

/* ---- one-shot helpers ----------------------------------------------------- */

uam_status uam_machine_save_snapshot(uam_machine *m, const char *path)
{
    uam_snapshot *s;
    uam_status st = uam_snapshot_take(m, &s);
    if (st != UAM_OK)
        return st;
    st = uam_snapshot_save(s, path);
    uam_snapshot_free(s);
    return st;
}

uam_status uam_machine_load_snapshot(uam_machine *m, const char *path)
{
    uam_snapshot *s;
    uam_status st = uam_snapshot_load(path, &s);
    if (st != UAM_OK)
        return st;
    st = uam_snapshot_restore(m, s);
    uam_snapshot_free(s);
    return st;
}
