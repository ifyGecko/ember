/*
 * heap.c - The guarded heap (uam/heap.h).
 *
 * Layout: blocks are kept in a table sorted by address, live ones and freed ones still in
 * quarantine alike; between any two there are at least `redzone` bytes, and as many before
 * the first and after the last. New blocks come from above the highest address used so
 * far while that lasts, then from the first gap that fits; when nothing fits, the oldest
 * quarantined blocks are let go until something does.
 *
 * Checking: one UAM_WATCH_CHECK watch (bus.h) per physical piece of the arena. The bus
 * asks it before every guest data access to those pages, and it answers from the table:
 * an access that lies wholly inside one live block passes, anything else is a hit.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uam/heap.h"
#include "uam/state.h"
#include "uam/uninit.h"

typedef struct blk {
    uint64_t addr, size;
    uint64_t alloc_site, free_site;
    bool     live;
} blk;

typedef struct piece {
    uam_heap *h;
    uint64_t  pa, len;   /* physical range */
    uint64_t  off;       /* its offset in the arena */
    int       wid;       /* the bus watch */
} piece;

struct uam_heap {
    uam_machine     *m;
    char            *name, *state_name;
    uint64_t         base, size;
    uint32_t         align, redzone;
    uint64_t         qmax;
    int              fill_alloc, fill_free;
    uam_guard_action action;
    piece           *pieces;
    uint32_t         n_pieces;
    /* state */
    bool             armed;      /* accesses are checked */
    bool             arm_auto;   /* ... from the first allocation on (until uam_heap_arm) */
    blk             *b;          /* sorted by addr */
    uint32_t         n, cap;
    uint64_t         top;        /* no block has ever been at or above it */
    uint64_t        *q;          /* quarantined blocks' addresses, oldest first: a ring */
    uint32_t         q_head, q_n, q_cap;
    uint64_t         q_bytes;
    uint64_t         live_blocks, live_bytes;
    /* not state */
    uam_heap_stats   st;
};

static void set_err(char *err, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void set_err(char *err, size_t n, const char *fmt, ...)
{
    if (!err || !n)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

/* ---- the table ------------------------------------------------------------ */

/* The index of the last block at or below addr, or -1. */
static int64_t find_le(const uam_heap *h, uint64_t addr)
{
    uint32_t lo = 0, hi = h->n;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (h->b[mid].addr <= addr)
            lo = mid + 1;
        else
            hi = mid;
    }
    return (int64_t)lo - 1;
}

static int64_t find_at(const uam_heap *h, uint64_t addr)
{
    const int64_t i = find_le(h, addr);
    return i >= 0 && h->b[i].addr == addr ? i : -1;
}

/* The bytes a block keeps others away from: its own, and one for an empty block, so that
 * every block has an address of its own. */
static uint64_t extent(const blk *b)
{
    return b->size ? b->size : 1;
}

static bool insert_at(uam_heap *h, uint32_t i, const blk *nb)
{
    if (h->n == h->cap) {
        const uint32_t ncap = h->cap ? h->cap * 2 : 64;
        blk *p = realloc(h->b, sizeof(*p) * ncap);
        if (!p)
            return false;
        h->b = p;
        h->cap = ncap;
    }
    memmove(&h->b[i + 1], &h->b[i], sizeof(*h->b) * (h->n - i));
    h->b[i] = *nb;
    h->n++;
    return true;
}

static void remove_at(uam_heap *h, uint32_t i)
{
    memmove(&h->b[i], &h->b[i + 1], sizeof(*h->b) * (h->n - i - 1));
    h->n--;
}

/* ---- the quarantine ------------------------------------------------------- */

static bool q_push(uam_heap *h, uint64_t addr)
{
    if (h->q_n == h->q_cap) {
        const uint32_t ncap = h->q_cap ? h->q_cap * 2 : 64;
        uint64_t *nq = malloc(sizeof(*nq) * ncap);
        if (!nq)
            return false;
        for (uint32_t i = 0; i < h->q_n; i++)
            nq[i] = h->q[(h->q_head + i) % h->q_cap];
        free(h->q);
        h->q = nq;
        h->q_cap = ncap;
        h->q_head = 0;
    }
    h->q[(h->q_head + h->q_n++) % h->q_cap] = addr;
    return true;
}

/* Let the oldest quarantined block go: its memory may be handed out again. */
static bool q_evict(uam_heap *h)
{
    if (!h->q_n)
        return false;
    const uint64_t addr = h->q[h->q_head];
    h->q_head = (h->q_head + 1) % h->q_cap;
    h->q_n--;
    const int64_t i = find_at(h, addr);
    if (i >= 0 && !h->b[i].live) {
        h->q_bytes -= extent(&h->b[i]);
        remove_at(h, (uint32_t)i);
    }
    return true;
}

/* ---- guest memory --------------------------------------------------------- */

/* Read or write arena memory as a device would (not checked, not a guest access). */
static void arena_rw(uam_heap *h, uint64_t addr, void *buf, uint64_t len, bool write)
{
    uint8_t *p = buf;
    while (len) {
        const uint64_t off = addr - h->base;
        uint64_t chunk = 0;
        for (uint32_t i = 0; i < h->n_pieces; i++) {
            const piece *pc = &h->pieces[i];
            if (off < pc->off || off - pc->off >= pc->len)
                continue;
            chunk = pc->len - (off - pc->off);
            if (chunk > len)
                chunk = len;
            if (write)
                uam_bus_dma_write(&h->m->bus, pc->pa + (off - pc->off), p, (size_t)chunk);
            else
                uam_bus_dma_read(&h->m->bus, pc->pa + (off - pc->off), p, (size_t)chunk);
            break;
        }
        if (!chunk)
            return;   /* outside the arena: not ours */
        addr += chunk;
        p += chunk;
        len -= chunk;
    }
}

static void fill(uam_heap *h, uint64_t addr, uint64_t len, int byte)
{
    uint8_t buf[4096];
    memset(buf, byte, sizeof(buf));
    while (len) {
        const uint64_t chunk = len < sizeof(buf) ? len : sizeof(buf);
        arena_rw(h, addr, buf, chunk, true);
        addr += chunk;
        len -= chunk;
    }
}

/* A fresh allocation in a run the uninitialised-read check has filled (uam/uninit.h):
 * the pattern, not what the block's memory held before, so a read of it before any write
 * shows. This also reaches memory the boot had already written, which the page fill
 * cannot. */
static void fill_uninit(uam_heap *h, uint64_t addr, uint64_t len)
{
    uint8_t buf[4096];
    while (len) {
        const uint64_t chunk = len < sizeof(buf) ? len : sizeof(buf);
        uam_machine_uninit_bytes(h->m, addr, buf, (size_t)chunk);
        arena_rw(h, addr, buf, chunk, true);
        addr += chunk;
        len -= chunk;
    }
}

/* ---- reports -------------------------------------------------------------- */

static void report(uam_heap *h, uam_guard_kind kind, const blk *b, uint64_t addr, uint64_t paddr,
                   unsigned size, bool is_write, uint64_t val, bool in_access)
{
    const unsigned hart = uam_machine_current_cpu(h->m);
    const uam_cpu *cpu = uam_machine_cpu(h->m, hart);
    const bool refused = h->action != UAM_GUARD_LOG;
    uam_guard_hit hit = {
        .kind = kind, .name = h->name, .action = h->action, .cpu = hart,
        .pc = in_access ? cpu->insn_pc : cpu->pc,
        .addr = addr, .paddr = paddr, .size = size, .is_write = is_write,
        .val = refused && !is_write ? 0 : val,
    };
    if (b) {
        hit.start = b->addr;
        hit.len = b->size;
        hit.site[0] = b->alloc_site;
        hit.site[1] = b->free_site;
    }
    h->st.hits++;
    uam_machine_guard_report(h->m, &hit, in_access && refused);
}

/* The bus asks: may the guest access [pa, pa + size)? true refuses. */
static bool check(void *ctx, uint64_t pa, unsigned size, bool is_write, uint64_t val)
{
    const piece *pc = ctx;
    uam_heap *h = pc->h;
    if (!h->armed)
        return false;
    /* The part of the access inside this piece (one that begins before it, or runs past
     * it, is only this watch's business there). */
    uint64_t n = size;
    if (pa < pc->pa) {
        n -= pc->pa - pa;
        pa = pc->pa;
    }
    if (n > pc->len - (pa - pc->pa))
        n = pc->len - (pa - pc->pa);
    const uint64_t a = h->base + pc->off + (pa - pc->pa);
    const int64_t i = find_le(h, a);
    const blk *at = i >= 0 ? &h->b[i] : NULL;
    if (at && at->live && a - at->addr < at->size && n <= at->size - (a - at->addr))
        return false;   /* wholly inside a live block */

    uam_guard_kind kind = UAM_GUARD_HEAP_WILD;
    const blk *b = NULL;
    const blk *next = (uint32_t)(i + 1) < h->n ? &h->b[i + 1] : NULL;
    if (at && a - at->addr < extent(at)) {
        /* It starts inside a block: a freed one, or a live one it runs out of. */
        b = at;
        kind = at->live ? UAM_GUARD_HEAP_REDZONE : UAM_GUARD_HEAP_FREED;
    } else if (next && next->addr - a < n) {
        /* It starts before a block and runs into it. */
        b = next;
        kind = next->live ? UAM_GUARD_HEAP_REDZONE : UAM_GUARD_HEAP_FREED;
    } else {
        /* Between blocks: the nearer one's redzone, if it is that near. */
        const uint64_t d_at = at ? a - (at->addr + at->size) : UINT64_MAX;
        const uint64_t d_next = next ? next->addr - (a + n) : UINT64_MAX;
        b = d_at <= d_next ? at : next;
        if (b && (d_at <= d_next ? d_at : d_next) < h->redzone)
            kind = UAM_GUARD_HEAP_REDZONE;
        else
            b = NULL;
    }
    report(h, kind, b, a, pa, (unsigned)n, is_write, val, true);
    return h->action != UAM_GUARD_LOG;
}

/* ---- the allocator -------------------------------------------------------- */

static bool pow2(uint64_t v)
{
    return v && !(v & (v - 1));
}

/* base + size as one past the arena's last byte never wraps (uam_heap_create). */
static uint64_t arena_end(const uam_heap *h)
{
    return h->base + h->size;
}

/* A place for `need` bytes aligned to `al` in [lo, hi), a redzone from each end. */
static bool fits(const uam_heap *h, uint64_t lo, uint64_t hi, uint64_t need, uint64_t al,
                 uint64_t *out)
{
    if (hi - lo < 2 * (uint64_t)h->redzone)
        return false;
    lo += h->redzone;
    hi -= h->redzone;
    const uint64_t a = (lo + al - 1) & ~(al - 1);
    if (a < lo || a > hi || hi - a < need)
        return false;
    *out = a;
    return true;
}

static bool place(uam_heap *h, uint64_t need, uint64_t al, uint64_t *addr, uint32_t *idx)
{
    /* Above everything used so far. */
    if (fits(h, h->top, arena_end(h), need, al, addr)) {
        *idx = h->n;
        return true;
    }
    /* The first gap that fits. */
    uint64_t lo = h->base;
    for (uint32_t i = 0; i <= h->n; i++) {
        const uint64_t hi = i < h->n ? h->b[i].addr : arena_end(h);
        if (hi > lo && fits(h, lo, hi, need, al, addr)) {
            *idx = i;
            return true;
        }
        if (i < h->n)
            lo = h->b[i].addr + extent(&h->b[i]);
    }
    return false;
}

uint64_t uam_heap_alloc(uam_heap *h, uint64_t size, uint64_t align, uint64_t site)
{
    const uint64_t al = align > h->align ? align : h->align;
    const uint64_t need = size ? size : 1;
    h->st.allocs++;
    if (!pow2(al) || size > h->size) {
        h->st.failed++;
        return 0;
    }
    uint64_t addr = 0;
    uint32_t idx = 0;
    while (!place(h, need, al, &addr, &idx)) {
        if (!q_evict(h)) {
            h->st.failed++;
            return 0;
        }
    }
    if (h->arm_auto) {   /* whatever cleared or set up this memory before is not a hit */
        h->armed = true;
        h->arm_auto = false;
    }
    const blk nb = { .addr = addr, .size = size, .alloc_site = site, .live = true };
    if (!insert_at(h, idx, &nb)) {
        h->st.failed++;
        return 0;
    }
    if (addr + need > h->top)
        h->top = addr + need;
    h->live_blocks++;
    h->live_bytes += size;
    if (h->live_bytes > h->st.peak_bytes)
        h->st.peak_bytes = h->live_bytes;
    if (h->fill_alloc >= 0 && size)
        fill(h, addr, size, h->fill_alloc);
    if (size && uam_machine_uninit_armed(h->m))
        fill_uninit(h, addr, size);
    return addr;
}

uint64_t uam_heap_calloc(uam_heap *h, uint64_t n, uint64_t size, uint64_t site)
{
    if (size && n > UINT64_MAX / size) {
        h->st.allocs++;
        h->st.failed++;
        return 0;
    }
    const uint64_t p = uam_heap_alloc(h, n * size, 0, site);
    if (p && n && size)
        fill(h, p, n * size, 0);
    return p;
}

/* A free of p, which is no live block: say which kind of mistake. */
static void bad_free(uam_heap *h, uint64_t p)
{
    const int64_t i = find_le(h, p);
    const blk *b = i >= 0 && p - h->b[i].addr < extent(&h->b[i]) ? &h->b[i] : NULL;
    report(h, b && !b->live && b->addr == p ? UAM_GUARD_HEAP_DOUBLE_FREE : UAM_GUARD_HEAP_BAD_FREE, b,
           p, p, 0, false, 0, false);
}

bool uam_heap_free(uam_heap *h, uint64_t p, uint64_t site)
{
    if (!p)
        return true;
    h->st.frees++;
    const int64_t i = find_at(h, p);
    if (i < 0 || !h->b[i].live) {
        bad_free(h, p);
        return false;
    }
    blk *b = &h->b[i];
    b->live = false;
    b->free_site = site;
    h->live_blocks--;
    h->live_bytes -= b->size;
    if (h->fill_free >= 0 && b->size)
        fill(h, b->addr, b->size, h->fill_free);
    if (!h->qmax || !q_push(h, p)) {
        remove_at(h, (uint32_t)i);   /* no quarantine: its memory is free at once */
        return true;
    }
    h->q_bytes += extent(b);
    while (h->q_bytes > h->qmax && q_evict(h))
        ;
    return true;
}

uint64_t uam_heap_realloc(uam_heap *h, uint64_t p, uint64_t size, uint64_t site)
{
    if (!p)
        return uam_heap_alloc(h, size, 0, site);
    if (!size) {
        uam_heap_free(h, p, site);
        return 0;
    }
    int64_t i = find_at(h, p);
    if (i < 0 || !h->b[i].live) {
        bad_free(h, p);
        return 0;
    }
    const uint64_t old = h->b[i].size;
    const uint64_t np = uam_heap_alloc(h, size, 0, site);
    if (!np)
        return 0;
    uint8_t buf[4096];
    for (uint64_t done = 0, n = old < size ? old : size; done < n;) {
        const uint64_t chunk = n - done < sizeof(buf) ? n - done : sizeof(buf);
        arena_rw(h, p + done, buf, chunk, false);
        arena_rw(h, np + done, buf, chunk, true);
        done += chunk;
    }
    uam_heap_free(h, p, site);
    return np;
}

bool uam_heap_block(const uam_heap *h, uint64_t addr, uam_heap_info *out)
{
    const int64_t i = find_le(h, addr);
    if (i < 0 || addr - h->b[i].addr >= extent(&h->b[i]))
        return false;
    const blk *b = &h->b[i];
    *out = (uam_heap_info){ .addr = b->addr, .size = b->size, .live = b->live,
                            .alloc_site = b->alloc_site, .free_site = b->free_site };
    return true;
}

void uam_heap_arm(uam_heap *h, bool on)
{
    h->armed = on;
    h->arm_auto = false;
}

void uam_heap_get_stats(const uam_heap *h, uam_heap_stats *out)
{
    *out = h->st;
    out->live_blocks = h->live_blocks;
    out->live_bytes = h->live_bytes;
    out->quarantined_blocks = h->n - h->live_blocks;
    out->quarantined_bytes = h->q_bytes;
}

/* ---- snapshots ------------------------------------------------------------ */

static void save(void *ctx, uam_state_writer *w)
{
    const uam_heap *h = ctx;
    uam_state_put_bool(w, h->armed);
    uam_state_put_bool(w, h->arm_auto);
    uam_state_put_u64(w, h->top);
    uam_state_put_u32(w, h->n);
    for (uint32_t i = 0; i < h->n; i++) {
        const blk *b = &h->b[i];
        uam_state_put_u64(w, b->addr);
        uam_state_put_u64(w, b->size);
        uam_state_put_u64(w, b->alloc_site);
        uam_state_put_u64(w, b->free_site);
        uam_state_put_bool(w, b->live);
    }
    uam_state_put_u32(w, h->q_n);
    for (uint32_t i = 0; i < h->q_n; i++)
        uam_state_put_u64(w, h->q[(h->q_head + i) % h->q_cap]);
}

static void restore(void *ctx, uam_state_reader *r)
{
    uam_heap *h = ctx;
    h->armed = uam_state_get_bool(r);
    h->arm_auto = uam_state_get_bool(r);
    h->top = uam_state_get_u64(r);
    const uint32_t n = uam_state_get_u32(r);
    h->n = 0;
    h->live_blocks = h->live_bytes = h->q_bytes = 0;
    h->q_head = h->q_n = 0;
    for (uint32_t i = 0; i < n && uam_state_ok(r); i++) {
        blk b = { 0 };
        b.addr = uam_state_get_u64(r);
        b.size = uam_state_get_u64(r);
        b.alloc_site = uam_state_get_u64(r);
        b.free_site = uam_state_get_u64(r);
        b.live = uam_state_get_bool(r);
        if ((h->n && b.addr <= h->b[h->n - 1].addr) || b.addr < h->base
            || b.addr - h->base >= h->size || b.size > h->size - (b.addr - h->base)) {
            uam_state_fail(r, "heap: blocks out of order or outside the arena");
            return;
        }
        if (!insert_at(h, h->n, &b)) {
            uam_state_fail(r, "out of memory");
            return;
        }
        if (b.live) {
            h->live_blocks++;
            h->live_bytes += b.size;
        } else {
            h->q_bytes += extent(&b);
        }
    }
    const uint32_t qn = uam_state_get_u32(r);
    for (uint32_t i = 0; i < qn && uam_state_ok(r); i++)
        if (!q_push(h, uam_state_get_u64(r)))
            uam_state_fail(r, "out of memory");
}

/* ---- creation ------------------------------------------------------------- */

/* The arena's physical pieces: one per page of a virtual arena, merged where consecutive
 * pages are physically contiguous (as a guard's range is split, guard.c). */
static bool split(uam_heap *h, bool phys, char *err, size_t errlen)
{
    uam_cpu *cpu = uam_machine_cpu(h->m, 0);
    const bool virt = !phys && cpu->isa->translate;
    const uint64_t cap = virt ? h->size / UAM_PAGE_SIZE + 2 : 1;
    h->pieces = calloc(cap, sizeof(*h->pieces));
    if (!h->pieces) {
        set_err(err, errlen, "out of memory");
        return false;
    }
    for (uint64_t off = 0; off < h->size;) {
        uint64_t va = h->base + off, pa = va, chunk = h->size - off;
        if (virt) {
            const uint64_t room = UAM_PAGE_SIZE - (va & (UAM_PAGE_SIZE - 1));
            if (chunk > room)
                chunk = room;
            uam_xlate xl = { 0 };
            if (cpu->isa->translate(cpu, va, UAM_XLATE_LOAD, &xl) != UAM_OK
                && cpu->isa->translate(cpu, va, UAM_XLATE_DEBUG, &xl) != UAM_OK) {
                set_err(err, errlen, "processor 0 does not map virtual 0x%llx",
                        (unsigned long long)va);
                return false;
            }
            if (!xl.fixed && !xl.regs_only)
                cpu->xlate_tables = true;
            pa = xl.paddr;
        }
        piece *last = h->n_pieces ? &h->pieces[h->n_pieces - 1] : NULL;
        if (last && last->pa + last->len == pa)
            last->len += chunk;
        else
            h->pieces[h->n_pieces++] = (piece){ .h = h, .pa = pa, .len = chunk, .off = off };
        off += chunk;
    }
    return true;
}

uam_heap *uam_heap_create(uam_machine *m, const uam_heap_opts *o, char *err, size_t errlen)
{
    if (err && errlen)
        err[0] = '\0';
    const uint32_t align = o->align ? o->align : 8;
    const uint32_t redzone = o->redzone ? o->redzone : 32;
    if (!o->size || o->base + o->size < o->base) {
        set_err(err, errlen, "a heap needs an arena that stays within the address space");
        return NULL;
    }
    if (!pow2(align) || o->fill_alloc > 255 || o->fill_free > 255
        || (o->action != UAM_GUARD_STOP && o->action != UAM_GUARD_FAULT && o->action != UAM_GUARD_LOG)) {
        set_err(err, errlen, "bad heap options (alignment, fill byte or action)");
        return NULL;
    }
    if (o->size < 2 * (uint64_t)redzone + align) {
        set_err(err, errlen, "the arena is smaller than its redzones");
        return NULL;
    }
    uam_heap *h = calloc(1, sizeof(*h));
    const char *name = o->name && *o->name ? o->name : "heap";
    const size_t nl = strlen(name) + sizeof("heap/");
    if (!h || !(h->name = strdup(name)) || !(h->state_name = malloc(nl))) {
        set_err(err, errlen, "out of memory");
        goto fail;
    }
    snprintf(h->state_name, nl, "heap/%s", name);
    h->m = m;
    h->base = o->base;
    h->size = o->size;
    h->align = align;
    h->redzone = redzone;
    h->qmax = o->quarantine == UINT64_MAX ? 0 : o->quarantine ? o->quarantine : o->size / 4;
    h->fill_alloc = o->fill_alloc;
    h->fill_free = o->fill_free;
    h->action = o->action;
    h->arm_auto = true;
    h->top = h->base;
    if (!split(h, o->phys, err, errlen))
        goto fail;
    uam_status st = uam_machine_add_state(m, h->state_name, save, restore, h);
    if (st != UAM_OK) {
        set_err(err, errlen, st == UAM_ERR_NOMEM ? "out of memory"
                                                 : "the machine already has a heap called '%s'", name);
        goto fail;
    }
    for (uint32_t i = 0; i < h->n_pieces; i++) {
        piece *p = &h->pieces[i];
        p->wid = uam_bus_add_check(&m->bus, p->pa, p->len, UAM_WATCH_READ | UAM_WATCH_WRITE, check, p);
        if (p->wid < 0) {
            set_err(err, errlen, "out of memory");
            uam_heap_destroy(h);
            return NULL;
        }
    }
    return h;
fail:
    if (h) {
        free(h->pieces);
        free(h->name);
        free(h->state_name);
    }
    free(h);
    return NULL;
}

void uam_heap_destroy(uam_heap *h)
{
    if (!h)
        return;
    for (uint32_t i = 0; i < h->n_pieces; i++)
        if (h->pieces[i].wid > 0)
            uam_bus_remove_watch(&h->m->bus, h->pieces[i].wid);
    uam_machine_remove_state(h->m, h->state_name);
    free(h->pieces);
    free(h->b);
    free(h->q);
    free(h->name);
    free(h->state_name);
    free(h);
}
