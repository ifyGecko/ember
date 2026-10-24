/*
 * bus.c - Region map, priorities, device byte order, ROMD, aliases, software
 * TLB, watchpoints, code-page tracking (uam/bus.h).
 */
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "uam/bus.h"
#include "uam/device.h"

void uam_bus_init(uam_bus *bus)
{
    memset(bus, 0, sizeof(*bus));
    bus->next_watch_id = 1;
}

static bool has_backing(const uam_region *r)
{
    return r->kind == UAM_REGION_RAM || r->kind == UAM_REGION_ROM || r->kind == UAM_REGION_ROMD;
}

void uam_bus_fini(uam_bus *bus)
{
    for (uint32_t i = 0; i < bus->n_regions; i++) {
        uam_region *r = bus->regions[i];
        if (r->host)
            munmap(r->host, r->size);
        free(r->page_gen);
        free(r->code_page);
        free(r->code_lines);
        free(r->dirty);
        free(r);
    }
    free(bus->regions);
    free(bus->watches);
    memset(bus, 0, sizeof(*bus));
}

static void tlb_flush(uam_bus *bus)
{
    bus->direct_gen++;
    memset(bus->tlb, 0, sizeof(bus->tlb));
}

static bool overlaps(uint64_t a, uint64_t asize, uint64_t b, uint64_t bsize)
{
    return a <= b + bsize - 1 && b <= a + asize - 1;
}

/* ---- region map -------------------------------------------------------- */

static uam_status add_region(uam_bus *bus, const uam_region *proto, uam_region **out)
{
    if (proto->size == 0 || proto->base + proto->size - 1 < proto->base)
        return UAM_ERR_INVALID;
    uint32_t pos = bus->n_regions;
    for (uint32_t i = 0; i < bus->n_regions; i++) {
        const uam_region *o = bus->regions[i];
        if (o->prio == proto->prio && !o->disabled && !proto->disabled
            && overlaps(proto->base, proto->size, o->base, o->size)) {
            uam_log(UAM_LOG_ERROR, "bus", "region %s overlaps %s at the same priority (%d)",
                    proto->name, o->name, o->prio);
            return UAM_ERR_INVALID;
        }
        if (pos == bus->n_regions && o->prio < proto->prio)
            pos = i;
    }
    if (bus->n_regions == bus->cap_regions) {
        uint32_t ncap = bus->cap_regions ? bus->cap_regions * 2 : 8;
        uam_region **nr = realloc(bus->regions, sizeof(*nr) * ncap);
        if (!nr)
            return UAM_ERR_NOMEM;
        bus->regions = nr;
        bus->cap_regions = ncap;
    }
    uam_region *r = malloc(sizeof(*r));
    if (!r)
        return UAM_ERR_NOMEM;
    *r = *proto;
    memmove(&bus->regions[pos + 1], &bus->regions[pos], sizeof(*bus->regions) * (bus->n_regions - pos));
    bus->regions[pos] = r;
    bus->n_regions++;
    tlb_flush(bus);
    if (out)
        *out = r;
    return UAM_OK;
}

static uam_status map_backed(uam_bus *bus, const uam_region *proto, const void *data, size_t len,
                             uam_region **out)
{
    uam_region r = *proto;
    uint64_t npages = (r.size + UAM_PAGE_SIZE - 1) >> UAM_PAGE_SHIFT;

    /* Anonymous mapping: zero-filled and populated lazily, so large RAM is cheap. */
    void *host = mmap(NULL, r.size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                      -1, 0);
    if (host == MAP_FAILED)
        return UAM_ERR_NOMEM;
    r.host = host;
    r.page_gen = calloc(npages, sizeof(uint32_t));
    r.code_page = calloc(npages, 1);
    r.code_lines = calloc(npages, sizeof(uint64_t));
    r.dirty = calloc((npages + 63) / 64, sizeof(uint64_t));
    if (!r.page_gen || !r.code_page || !r.code_lines || !r.dirty) {
        munmap(host, r.size);
        free(r.page_gen);
        free(r.code_page);
        free(r.code_lines);
        free(r.dirty);
        return UAM_ERR_NOMEM;
    }
    if (data && len)
        memcpy(r.host, data, len < r.size ? len : r.size);
    /* A generation of its own: a page that comes to resolve to another region
     * (a remap) never matches code translated from this one. */
    uint32_t g = ++bus->gen_seq;
    for (uint64_t i = 0; i < npages; i++)
        r.page_gen[i] = g;

    uam_status st = add_region(bus, &r, out);
    if (st != UAM_OK) {
        munmap(host, r.size);
        free(r.page_gen);
        free(r.code_page);
        free(r.code_lines);
        free(r.dirty);
    }
    return st;
}

uam_status uam_bus_map_ram_ex(uam_bus *bus, const char *name, uint64_t base, uint64_t size,
                              const uam_map_attrs *attrs, uam_region **out)
{
    uam_region r = { .name = name, .base = base, .size = size, .kind = UAM_REGION_RAM,
                     .prio = attrs ? attrs->prio : 0, .disabled = attrs && attrs->disabled };
    return map_backed(bus, &r, NULL, 0, out);
}

uam_status uam_bus_map_ram(uam_bus *bus, const char *name, uint64_t base, uint64_t size)
{
    uam_region r = { .name = name, .base = base, .size = size, .kind = UAM_REGION_RAM };
    return map_backed(bus, &r, NULL, 0, NULL);
}

uam_status uam_bus_map_rom(uam_bus *bus, const char *name, uint64_t base, uint64_t size,
                           const void *data, size_t len)
{
    uam_region r = { .name = name, .base = base, .size = size, .kind = UAM_REGION_ROM };
    return map_backed(bus, &r, data, len, NULL);
}

uam_status uam_bus_map_mmio_ex(uam_bus *bus, const char *name, uint64_t base, uint64_t size,
                               uam_device *dev, const uam_map_attrs *attrs)
{
    if (!dev)
        return UAM_ERR_INVALID;
    uam_region r = { .name = name, .base = base, .size = size, .kind = UAM_REGION_MMIO, .dev = dev,
                     .prio = attrs ? attrs->prio : 0, .endian = attrs ? attrs->endian : 0,
                     .disabled = attrs && attrs->disabled };
    return add_region(bus, &r, NULL);
}

uam_status uam_bus_map_mmio(uam_bus *bus, const char *name, uint64_t base, uint64_t size,
                            uam_device *dev)
{
    return uam_bus_map_mmio_ex(bus, name, base, size, dev, NULL);
}

uam_status uam_bus_map_romd(uam_bus *bus, const char *name, uint64_t base, uint64_t size,
                            uam_device *dev, const void *data, size_t len,
                            const uam_map_attrs *attrs, uam_region **out)
{
    if (!dev)
        return UAM_ERR_INVALID;
    uam_region r = { .name = name, .base = base, .size = size, .kind = UAM_REGION_ROMD, .dev = dev,
                     .rom_mode = true, .prio = attrs ? attrs->prio : 0,
                     .disabled = attrs && attrs->disabled,
                     .endian = attrs ? attrs->endian : 0 };
    return map_backed(bus, &r, data, len, out);
}

uam_status uam_bus_map_alias(uam_bus *bus, const char *name, uint64_t base, uint64_t size,
                             const uam_region *target, uint64_t offset, const uam_map_attrs *attrs)
{
    if (!target)
        return UAM_ERR_INVALID;
    if (target->kind == UAM_REGION_ALIAS) {   /* alias of an alias: flatten */
        offset += target->target_off;
        target = target->target;
    }
    if (offset > target->size || size > target->size - offset)
        return UAM_ERR_INVALID;
    uam_region r = { .name = name, .base = base, .size = size, .kind = UAM_REGION_ALIAS,
                     .target = (uam_region *)target, .target_off = offset,
                     .prio = attrs ? attrs->prio : 0, .disabled = attrs && attrs->disabled };
    return add_region(bus, &r, NULL);
}

void uam_bus_set_unmapped(uam_bus *bus, uam_device *dev)
{
    bus->unmapped = dev;
}

void uam_bus_set_code_write_hook(uam_bus *bus, uam_code_write_fn fn, void *ctx)
{
    bus->on_code_write = fn;
    bus->code_write_ctx = ctx;
}

uam_region *uam_bus_region(uam_bus *bus, const char *name)
{
    for (uint32_t i = 0; i < bus->n_regions; i++)
        if (bus->regions[i]->name && strcmp(bus->regions[i]->name, name) == 0)
            return bus->regions[i];
    return NULL;
}

/* ---- watchpoints ------------------------------------------------------- */

int uam_bus_add_watch(uam_bus *bus, uint64_t base, uint64_t size, unsigned kinds,
                      uam_watch_fn fn, void *ctx)
{
    if (!fn || !size || !kinds)
        return -1;
    if (bus->n_watches == bus->cap_watches) {
        uint32_t ncap = bus->cap_watches ? bus->cap_watches * 2 : 4;
        uam_watch *nw = realloc(bus->watches, sizeof(*nw) * ncap);
        if (!nw)
            return -1;
        bus->watches = nw;
        bus->cap_watches = ncap;
    }
    int id = bus->next_watch_id++;
    bus->watches[bus->n_watches++] = (uam_watch){ .id = id, .base = base, .size = size,
                                                  .kinds = kinds, .fn = fn, .ctx = ctx };
    tlb_flush(bus);  /* watched pages must take the slow path */
    return id;
}

bool uam_bus_remove_watch(uam_bus *bus, int id)
{
    for (uint32_t i = 0; i < bus->n_watches; i++) {
        if (bus->watches[i].id == id) {
            memmove(&bus->watches[i], &bus->watches[i + 1],
                    sizeof(*bus->watches) * (bus->n_watches - i - 1));
            bus->n_watches--;
            tlb_flush(bus);
            return true;
        }
    }
    return false;
}

static bool watched(const uam_bus *bus, uint64_t base, uint64_t size)
{
    for (uint32_t i = 0; i < bus->n_watches; i++)
        if (overlaps(base, size, bus->watches[i].base, bus->watches[i].size))
            return true;
    return false;
}

static void fire_watches(uam_bus *bus, uint64_t addr, unsigned size, bool is_write, uint64_t val)
{
    for (uint32_t i = 0; i < bus->n_watches; i++) {
        const uam_watch *w = &bus->watches[i];
        if ((w->kinds & (is_write ? UAM_WATCH_WRITE : UAM_WATCH_READ))
            && overlaps(addr, size, w->base, w->size))
            w->fn(w->ctx, addr, size, is_write, val);
    }
}

/* ---- lookup ------------------------------------------------------------ */

typedef enum { RES_ONE, RES_SPLIT, RES_NONE } res_kind;

typedef struct hit {
    uam_region *r;       /* resolved region (never an alias) */
    uint64_t    off;     /* offset of the access within r */
    bool        watched; /* a watchpoint intersects the access */
} hit;

/* The highest-priority region containing addr, as mapped (may be an alias). */
static uam_region *winner(const uam_bus *bus, uint64_t addr, uint32_t *idx)
{
    for (uint32_t i = 0; i < bus->n_regions; i++) {
        uam_region *r = bus->regions[i];
        if (!r->disabled && addr - r->base < r->size) {
            if (idx)
                *idx = i;
            return r;
        }
    }
    return NULL;
}

/* Does any region before position `upto` (higher priority) intersect the range? */
static bool shadowed(const uam_bus *bus, uint32_t upto, uint64_t base, uint64_t size)
{
    for (uint32_t i = 0; i < upto; i++)
        if (!bus->regions[i]->disabled && overlaps(base, size, bus->regions[i]->base, bus->regions[i]->size))
            return true;
    return false;
}

/*
 * Resolve an access of `size` bytes at addr. RES_ONE: one region serves the
 * whole access. RES_SPLIT: the access spans regions (serve it bytewise).
 * RES_NONE: nothing is mapped at any of its bytes.
 */
static res_kind resolve(uam_bus *bus, uint64_t addr, unsigned size, hit *h)
{
    const uint64_t page = addr & UAM_PAGE_MASK;
    const unsigned slot = (unsigned)(page >> UAM_PAGE_SHIFT) & (UAM_BUS_TLB_SIZE - 1);
    const uint64_t end = addr + size - 1;

    if (bus->tlb[slot].r && bus->tlb[slot].page == page && (end & UAM_PAGE_MASK) == page) {
        h->r = bus->tlb[slot].r;
        h->off = addr - bus->tlb[slot].origin;
        h->watched = false;  /* watched pages are never cached */
        return RES_ONE;
    }

    uint32_t idx = 0;
    uam_region *w = winner(bus, addr, &idx);
    if (!w) {
        for (uint32_t i = 0; i < bus->n_regions; i++)
            if (!bus->regions[i]->disabled
                && overlaps(addr, size, bus->regions[i]->base, bus->regions[i]->size))
                return RES_SPLIT;
        h->watched = bus->n_watches && watched(bus, addr, size);
        return RES_NONE;
    }
    if (end - w->base >= w->size || end < addr || shadowed(bus, idx, addr, size))
        return RES_SPLIT;

    uint64_t origin = w->base;
    uam_region *r = w;
    if (w->kind == UAM_REGION_ALIAS) {
        origin = w->base - w->target_off;
        r = w->target;
    }
    h->r = r;
    h->off = addr - origin;
    h->watched = bus->n_watches && watched(bus, addr, size);

    /* Cache the page only if this region owns all of it and no watch is there. */
    if (page >= w->base && page + UAM_PAGE_SIZE - 1 <= w->base + w->size - 1
        && !shadowed(bus, idx, page, UAM_PAGE_SIZE)
        && !(bus->n_watches && watched(bus, page, UAM_PAGE_SIZE))) {
        bus->tlb[slot].page = page;
        bus->tlb[slot].r = r;
        bus->tlb[slot].origin = origin;
    }
    return RES_ONE;
}

const uam_region *uam_bus_find(const uam_bus *bus, uint64_t addr)
{
    return winner(bus, addr, NULL);
}

/* ---- value <-> bytes --------------------------------------------------- */

static uint64_t bytes_to_val(const uint8_t *p, unsigned size, bool be)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < size; i++)
        v |= (uint64_t)p[be ? size - 1 - i : i] << (8 * i);
    return v;
}

static void val_to_bytes(uint8_t *p, unsigned size, bool be, uint64_t v)
{
    for (unsigned i = 0; i < size; i++)
        p[be ? size - 1 - i : i] = (uint8_t)(v >> (8 * i));
}

static uint64_t bswap_n(uint64_t v, unsigned size)
{
    uint64_t r = 0;
    for (unsigned i = 0; i < size; i++)
        r |= ((v >> (8 * i)) & 0xFF) << (8 * (size - 1 - i));
    return r;
}

static bool valid_size(unsigned size)
{
    return size == 1 || size == 2 || size == 4 || size == 8;
}

/* ---- device dispatch (byte order, implemented widths) ------------------ */

static bool dev_is_be(const uam_region *r, const uam_device *d)
{
    unsigned e = (r && r->endian) ? r->endian : d->ops->endian;
    return e & UAM_ACC_BE;
}

/* Size of the device register units an access of `size` is served with. */
static unsigned unit_size(const uam_device_ops *o, unsigned size)
{
    unsigned mn = o->impl_min ? o->impl_min : 1, mx = o->impl_max ? o->impl_max : 8;
    return size < mn ? mn : size > mx ? mx : size;
}

/* A device read through `rd`: the device's read handler, or its peek handler
 * for a debugger. */
typedef uam_status (*dev_reader)(uam_device *dev, uint64_t off, unsigned size, uint64_t *val);

static uam_status dev_read_with(const uam_region *r, uam_device *d, dev_reader rd, uint64_t off,
                                unsigned size, bool acc_be, uint64_t *val)
{
    const uam_device_ops *o = d->ops;
    const bool dbe = dev_is_be(r, d);
    const unsigned u = unit_size(o, size);

    if (u == size && (!o->impl_min || (off & (o->impl_min - 1)) == 0)) {
        uint64_t v = 0;
        uam_status st = rd(d, off, size, &v);
        if (st != UAM_OK)
            return st;
        v &= uam_mask(size * 8);
        *val = acc_be != dbe ? bswap_n(v, size) : v;
        return UAM_OK;
    }

    /* Serve through the device's register units, byte lane by byte lane. */
    uint8_t bytes[8];
    for (uint64_t ub = off & ~(uint64_t)(u - 1); ub < off + size; ub += u) {
        uint64_t uv = 0;
        uam_status st = rd(d, ub, u, &uv);
        if (st != UAM_OK)
            return st;
        for (unsigned k = 0; k < u; k++) {
            uint64_t pos = ub + k;
            if (pos >= off && pos < off + size)
                bytes[pos - off] = (uint8_t)(uv >> (8 * (dbe ? u - 1 - k : k)));
        }
    }
    *val = bytes_to_val(bytes, size, acc_be);
    return UAM_OK;
}

static uam_status dev_read(const uam_region *r, uam_device *d, uint64_t off, unsigned size,
                           bool acc_be, uint64_t *val)
{
    return dev_read_with(r, d, d->ops->read, off, size, acc_be, val);
}

static uam_status dev_write(const uam_region *r, uam_device *d, uint64_t off, unsigned size,
                            bool acc_be, uint64_t val)
{
    const uam_device_ops *o = d->ops;
    const bool dbe = dev_is_be(r, d);
    const unsigned u = unit_size(o, size);

    if (u == size && (!o->impl_min || (off & (o->impl_min - 1)) == 0))
        return o->write(d, off, size, acc_be != dbe ? bswap_n(val, size) : val);

    uint8_t bytes[8];
    val_to_bytes(bytes, size, acc_be, val);
    for (uint64_t ub = off & ~(uint64_t)(u - 1); ub < off + size; ub += u) {
        uint64_t uv = 0, mask = 0;
        bool full = ub >= off && ub + u <= off + size;
        if (!full && !o->write_masked) {
            /* Read-merge-write for devices without byte enables. */
            uam_status st = o->read(d, ub, u, &uv);
            if (st != UAM_OK)
                return st;
        }
        for (unsigned k = 0; k < u; k++) {
            uint64_t pos = ub + k;
            if (pos < off || pos >= off + size)
                continue;
            unsigned sh = 8 * (dbe ? u - 1 - k : k);
            uv = (uv & ~(UINT64_C(0xFF) << sh)) | ((uint64_t)bytes[pos - off] << sh);
            mask |= UINT64_C(0xFF) << sh;
        }
        uam_status st = (!full && o->write_masked) ? o->write_masked(d, ub, u, uv, mask)
                                                   : o->write(d, ub, u, uv);
        if (st != UAM_OK)
            return st;
    }
    return UAM_OK;
}

/* ---- accesses ---------------------------------------------------------- */

/* The 64-byte lines of one page that [off, off+len) touches (within the page). */
static uint64_t line_mask(uint64_t off, uint64_t len)
{
    unsigned first = (unsigned)(off & (UAM_PAGE_SIZE - 1)) >> 6;
    unsigned last = (unsigned)((off + len - 1) & (UAM_PAGE_SIZE - 1)) >> 6;
    uint64_t hi = last == 63 ? ~0ull : (1ull << (last + 1)) - 1;
    return hi & ~((1ull << first) - 1);
}

/* Bytes [off, off+len) of r's backing store changed. A page with translated
 * code loses it (new generation, hook) when a written line holds code. */
static void note_write(uam_bus *bus, uam_region *r, uint64_t off, uint64_t len)
{
    while (len) {
        uint64_t pidx = off >> UAM_PAGE_SHIFT;
        uint64_t room = UAM_PAGE_SIZE - (off & (UAM_PAGE_SIZE - 1));
        uint64_t n = room < len ? room : len;
        if (!r->code_page[pidx]) {
            r->page_gen[pidx] = ++bus->gen_seq;
        } else if (r->code_lines[pidx] & line_mask(off, n)) {
            r->page_gen[pidx] = ++bus->gen_seq;
            r->code_page[pidx] = 0;
            r->code_lines[pidx] = 0;
            r->n_code_pages--;
            if (bus->on_code_write)
                bus->on_code_write(bus->code_write_ctx, (r->base + off) & UAM_PAGE_MASK);
        }
        off += n;
        len -= n;
    }
}

static void note_code_change(uam_bus *bus, uam_region *r, uint64_t off)
{
    note_write(bus, r, off & UAM_PAGE_MASK, UAM_PAGE_SIZE);
}

/* Mark the pages of r's backing store in [off, off+len) dirty (len > 0). */
static void mark_dirty(uam_region *r, uint64_t off, uint64_t len)
{
    for (uint64_t p = off >> UAM_PAGE_SHIFT; p <= (off + len - 1) >> UAM_PAGE_SHIFT; p++)
        r->dirty[p >> 6] |= UINT64_C(1) << (p & 63);
}

static uam_status read_bytewise(uam_bus *bus, uint64_t addr, unsigned size, unsigned attrs,
                                uint64_t *val)
{
    uint8_t bytes[8];
    for (unsigned i = 0; i < size; i++) {
        uint64_t b;
        uam_status st = uam_bus_read(bus, addr + i, 1, attrs & ~UAM_ACC_ALIGNED, &b);
        if (st != UAM_OK)
            return st;
        bytes[i] = (uint8_t)b;
    }
    *val = bytes_to_val(bytes, size, attrs & UAM_ACC_BE);
    return UAM_OK;
}

uam_status uam_bus_read(uam_bus *bus, uint64_t addr, unsigned size, unsigned attrs, uint64_t *val)
{
    if (!valid_size(size))
        return UAM_ERR_INVALID;
    if ((attrs & UAM_ACC_ALIGNED) && (addr & (size - 1)))
        return UAM_ERR_MISALIGNED;
    const bool be = attrs & UAM_ACC_BE;
    const bool side_effect_free = attrs & (UAM_ACC_FETCH | UAM_ACC_DEBUG);
    hit h;
    uam_status st;

    switch (resolve(bus, addr, size, &h)) {
    case RES_SPLIT:
        return read_bytewise(bus, addr, size, attrs, val);
    case RES_NONE:
        if (!bus->unmapped || side_effect_free)
            return UAM_ERR_FAULT;
        st = dev_read(NULL, bus->unmapped, addr, size, be, val);
        break;
    case RES_ONE:
    default:
        switch (h.r->kind) {
        case UAM_REGION_ROMD:
            if (!h.r->rom_mode && !(attrs & UAM_ACC_DEBUG)) {
                if (attrs & UAM_ACC_FETCH)
                    return UAM_ERR_FAULT;  /* no execution from device mode */
                st = dev_read(h.r, h.r->dev, h.off, size, be, val);
                break;
            }
            /* ROM mode reads the backing store */
            __attribute__((fallthrough));
        case UAM_REGION_RAM:
        case UAM_REGION_ROM:
            *val = bytes_to_val(h.r->host + h.off, size, be);
            st = UAM_OK;
            break;
        case UAM_REGION_MMIO:
            /* No execution from devices; a debugger reads only what a device
             * can serve without side effects (its peek handler). */
            if ((attrs & UAM_ACC_FETCH) || (side_effect_free && !h.r->dev->ops->peek))
                return UAM_ERR_FAULT;
            st = side_effect_free
                ? dev_read_with(h.r, h.r->dev, h.r->dev->ops->peek, h.off, size, be, val)
                : dev_read(h.r, h.r->dev, h.off, size, be, val);
            break;
        default:
            return UAM_ERR_FAULT;
        }
        break;
    }
    if (st == UAM_OK && h.watched && !side_effect_free)
        fire_watches(bus, addr, size, false, *val);
    return st;
}

static uam_status write_bytewise(uam_bus *bus, uint64_t addr, unsigned size, unsigned attrs,
                                 uint64_t val)
{
    uint8_t bytes[8];
    val_to_bytes(bytes, size, attrs & UAM_ACC_BE, val);
    for (unsigned i = 0; i < size; i++) {
        uam_status st = uam_bus_write(bus, addr + i, 1, attrs & ~UAM_ACC_ALIGNED, bytes[i]);
        if (st != UAM_OK)
            return st;
    }
    return UAM_OK;
}

uam_status uam_bus_write(uam_bus *bus, uint64_t addr, unsigned size, unsigned attrs, uint64_t val)
{
    if (!valid_size(size))
        return UAM_ERR_INVALID;
    if ((attrs & UAM_ACC_ALIGNED) && (addr & (size - 1)))
        return UAM_ERR_MISALIGNED;
    const bool be = attrs & UAM_ACC_BE;
    const bool debug = attrs & UAM_ACC_DEBUG;
    val &= uam_mask(size * 8);
    hit h;
    uam_status st;

    switch (resolve(bus, addr, size, &h)) {
    case RES_SPLIT:
        return write_bytewise(bus, addr, size, attrs, val);
    case RES_NONE:
        if (!bus->unmapped || debug)
            return UAM_ERR_FAULT;
        st = dev_write(NULL, bus->unmapped, addr, size, be, val);
        break;
    case RES_ONE:
    default:
        switch (h.r->kind) {
        case UAM_REGION_ROM:
            return UAM_ERR_FAULT;
        case UAM_REGION_ROMD:
            if (!debug) {
                st = dev_write(h.r, h.r->dev, h.off, size, be, val);
                break;
            }
            /* a debugger pokes the backing store */
            __attribute__((fallthrough));
        case UAM_REGION_RAM:
            val_to_bytes(h.r->host + h.off, size, be, val);
            mark_dirty(h.r, h.off, size);
            if (h.r->n_code_pages)
                note_write(bus, h.r, h.off, size);
            st = UAM_OK;
            break;
        case UAM_REGION_MMIO:
            if (debug)
                return UAM_ERR_FAULT;
            st = dev_write(h.r, h.r->dev, h.off, size, be, val);
            break;
        default:
            return UAM_ERR_FAULT;
        }
        break;
    }
    if (st == UAM_OK && h.watched && !debug)
        fire_watches(bus, addr, size, true, val);
    return st;
}

uam_status uam_bus_load_bytes(uam_bus *bus, uint64_t addr, const void *data, size_t len)
{
    const uint8_t *src = data;
    while (len) {
        hit h;
        if (resolve(bus, addr, 1, &h) != RES_ONE || !has_backing(h.r)) {
            uam_log(UAM_LOG_ERROR, "bus", "cannot load image bytes at 0x%llx: no RAM/ROM there",
                    (unsigned long long)addr);
            return UAM_ERR_FAULT;
        }
        /* Stay within this page: the next one may belong to another region. */
        uint64_t room = UAM_PAGE_SIZE - (addr & (UAM_PAGE_SIZE - 1));
        if (room > h.r->size - h.off)
            room = h.r->size - h.off;
        size_t n = (size_t)room < len ? (size_t)room : len;
        memcpy(h.r->host + h.off, src, n);
        note_write(bus, h.r, h.off, n);
        mark_dirty(h.r, h.off, n);
        addr += n;
        src += n;
        len -= n;
    }
    return UAM_OK;
}

/* ---- DMA --------------------------------------------------------------- */

/* Resolve one DMA chunk: at most the rest of addr's page within one backed region. */
static uam_status dma_chunk(uam_bus *bus, uint64_t addr, size_t len, bool write, hit *h, size_t *n)
{
    if (resolve(bus, addr, 1, h) != RES_ONE)
        return UAM_ERR_FAULT;
    bool ok = write ? h->r->kind == UAM_REGION_RAM
                    : h->r->kind == UAM_REGION_RAM || h->r->kind == UAM_REGION_ROM
                          || (h->r->kind == UAM_REGION_ROMD && h->r->rom_mode);
    if (!ok)
        return UAM_ERR_FAULT;
    uint64_t room = UAM_PAGE_SIZE - (addr & (UAM_PAGE_SIZE - 1));
    if (room > h->r->size - h->off)
        room = h->r->size - h->off;
    *n = (size_t)room < len ? (size_t)room : len;
    return UAM_OK;
}

uam_status uam_bus_dma_read(uam_bus *bus, uint64_t addr, void *buf, size_t len)
{
    uint8_t *dst = buf;
    while (len) {
        hit h;
        size_t n;
        uam_status st = dma_chunk(bus, addr, len, false, &h, &n);
        if (st != UAM_OK)
            return st;
        memcpy(dst, h.r->host + h.off, n);
        addr += n;
        dst += n;
        len -= n;
    }
    return UAM_OK;
}

uam_status uam_bus_dma_write(uam_bus *bus, uint64_t addr, const void *buf, size_t len)
{
    const uint8_t *src = buf;
    while (len) {
        hit h;
        size_t n;
        uam_status st = dma_chunk(bus, addr, len, true, &h, &n);
        if (st != UAM_OK)
            return st;
        memcpy(h.r->host + h.off, src, n);
        note_write(bus, h.r, h.off, n);
        mark_dirty(h.r, h.off, n);
        addr += n;
        src += n;
        len -= n;
    }
    return UAM_OK;
}

/* ---- run-time decode changes ------------------------------------------ */

static void map_changed(uam_bus *bus, uint64_t page)
{
    tlb_flush(bus);
    /* Chained translations bypass the lookup that would revalidate them. */
    if (bus->on_code_write)
        bus->on_code_write(bus->code_write_ctx, page);
}

void uam_bus_region_enable(uam_bus *bus, uam_region *r, bool on)
{
    if (r->disabled == !on)
        return;
    r->disabled = !on;
    map_changed(bus, r->base & UAM_PAGE_MASK);
}

void uam_bus_region_move(uam_bus *bus, uam_region *r, uint64_t base)
{
    if (r->base == base || base + r->size - 1 < base)
        return;
    r->base = base;
    map_changed(bus, base & UAM_PAGE_MASK);
}

/* ---- ROMD -------------------------------------------------------------- */

static void drop_code_pages(uam_bus *bus, uam_region *r, uint64_t off, uint64_t len)
{
    if (!len)
        return;
    for (uint64_t p = off >> UAM_PAGE_SHIFT; p <= (off + len - 1) >> UAM_PAGE_SHIFT; p++)
        note_code_change(bus, r, p << UAM_PAGE_SHIFT);
}

void uam_bus_romd_set_mode(uam_bus *bus, uam_region *r, bool rom)
{
    if (r->kind != UAM_REGION_ROMD || r->rom_mode == rom)
        return;
    r->rom_mode = rom;
    bus->direct_gen++;
    /* Code fetched in ROM mode is no longer what a fetch would see. */
    if (!rom && r->n_code_pages) {
        uint64_t npages = (r->size + UAM_PAGE_SIZE - 1) >> UAM_PAGE_SHIFT;
        for (uint64_t p = 0; p < npages && r->n_code_pages; p++)
            if (r->code_page[p])
                note_code_change(bus, r, p << UAM_PAGE_SHIFT);
    }
}

uint8_t *uam_bus_romd_backing(uam_region *r)
{
    return r->kind == UAM_REGION_ROMD ? r->host : NULL;
}

void uam_bus_romd_dirty(uam_bus *bus, uam_region *r, uint64_t off, uint64_t len)
{
    if (r->kind != UAM_REGION_ROMD || off >= r->size)
        return;
    if (len > r->size - off)
        len = r->size - off;
    drop_code_pages(bus, r, off, len);
    if (len)
        mark_dirty(r, off, len);
}

/* ---- dirty pages (snapshots) ------------------------------------------- */

void uam_bus_dirty_reset(uam_bus *bus)
{
    for (uint32_t i = 0; i < bus->n_regions; i++) {
        uam_region *r = bus->regions[i];
        if (has_backing(r)) {
            uint64_t npages = (r->size + UAM_PAGE_SIZE - 1) >> UAM_PAGE_SHIFT;
            memset(r->dirty, 0, sizeof(uint64_t) * ((npages + 63) / 64));
        }
    }
    bus->track_dirty = true;
    bus->direct_gen++;   /* clean pages must lose their direct write access */
}

void uam_bus_mark_dirty(uam_bus *bus, uint64_t addr, uint64_t len)
{
    while (len) {
        hit h;
        uint64_t room = UAM_PAGE_SIZE - (addr & (UAM_PAGE_SIZE - 1));
        uint64_t n = room < len ? room : len;
        if (resolve(bus, addr, 1, &h) == RES_ONE && has_backing(h.r))
            mark_dirty(h.r, h.off, n < h.r->size - h.off ? n : h.r->size - h.off);
        if (n >= len)
            break;
        addr += n;
        len -= n;
    }
}

void uam_bus_region_store(uam_bus *bus, uam_region *r, uint64_t off, const void *src, uint64_t len)
{
    if (!has_backing(r) || off >= r->size || !len)
        return;
    if (len > r->size - off)
        len = r->size - off;
    memcpy(r->host + off, src, len);
    drop_code_pages(bus, r, off, len);
    mark_dirty(r, off, len);
}

/* ---- code pages -------------------------------------------------------- */

void uam_bus_mark_code_range(uam_bus *bus, uint64_t addr, uint64_t len)
{
    while (len) {
        uint64_t room = UAM_PAGE_SIZE - (addr & (UAM_PAGE_SIZE - 1));
        uint64_t n = room < len ? room : len;
        hit h;
        if (resolve(bus, addr, 1, &h) == RES_ONE && has_backing(h.r)) {
            uint64_t p = h.off >> UAM_PAGE_SHIFT;
            h.r->code_lines[p] |= line_mask(h.off, n);
            if (!h.r->code_page[p]) {
                h.r->code_page[p] = 1;
                h.r->n_code_pages++;
                bus->direct_gen++;   /* writes to this page must now take the slow path */
            }
        }
        addr += n;
        len -= n;
    }
}

void uam_bus_mark_code(uam_bus *bus, uint64_t addr)
{
    uam_bus_mark_code_range(bus, addr & UAM_PAGE_MASK, UAM_PAGE_SIZE);
}

uint8_t *uam_bus_page_host(uam_bus *bus, uint64_t paddr, bool write)
{
    const uint64_t page = paddr & UAM_PAGE_MASK;
    uint32_t idx = 0;
    const uam_region *w = winner(bus, page, &idx);
    /* The whole page must belong to one region, unshadowed and unwatched. */
    if (!w || page < w->base || page + UAM_PAGE_SIZE - 1 > w->base + w->size - 1
        || shadowed(bus, idx, page, UAM_PAGE_SIZE)
        || (bus->n_watches && watched(bus, page, UAM_PAGE_SIZE)))
        return NULL;
    const uam_region *r = w;
    uint64_t off = page - w->base;
    if (w->kind == UAM_REGION_ALIAS) {
        r = w->target;
        off += w->target_off;
    }
    switch (r->kind) {
    case UAM_REGION_RAM:
        if (write && (r->code_page[off >> UAM_PAGE_SHIFT]
                      || (bus->track_dirty && !uam_region_page_dirty(r, off >> UAM_PAGE_SHIFT))))
            return NULL;
        break;
    case UAM_REGION_ROM:
        if (write)
            return NULL;
        break;
    case UAM_REGION_ROMD:
        if (write || !r->rom_mode)
            return NULL;
        break;
    default:
        return NULL;
    }
    return r->host + off;
}

uint32_t uam_bus_page_gen(uam_bus *bus, uint64_t addr)
{
    hit h;
    if (resolve(bus, addr, 1, &h) != RES_ONE || !has_backing(h.r))
        return 0;
    return h.r->page_gen[h.off >> UAM_PAGE_SHIFT];
}

void uam_bus_dump_map(const uam_bus *bus, FILE *out)
{
    static const char *const kinds[] = { "ram", "rom", "mmio", "romd", "alias" };
    for (uint32_t i = 0; i < bus->n_regions; i++) {
        const uam_region *r = bus->regions[i];
        fprintf(out, "  %016llx-%016llx %-5s prio %5d  %s", (unsigned long long)r->base,
                (unsigned long long)(r->base + r->size - 1), kinds[r->kind], r->prio,
                r->name ? r->name : "?");
        if (r->kind == UAM_REGION_ALIAS)
            fprintf(out, " -> %s+0x%llx", r->target->name ? r->target->name : "?",
                    (unsigned long long)r->target_off);
        if (r->kind == UAM_REGION_ROMD)
            fprintf(out, " (%s mode)", r->rom_mode ? "rom" : "device");
        if (r->disabled)
            fputs(" (disabled)", out);
        fputc('\n', out);
    }
}
