/*
 * pci.c - PCI configuration space and BAR decoding (uam/dev/pci.h).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uam/dev/pci.h"

/* BARs decode below RAM and real devices of the board, above catch-all windows. */
#define PCI_BAR_PRIO (-10)

struct uam_pci_bus {
    uam_device    cam;          /* owns the bus (reset, snapshots) */
    uam_device    ecam;
    uam_bus      *sys;
    uint64_t      io_base;
    uint32_t      addr;         /* 0xCF8 */
    uam_pci_func *funcs;
    void        (*cf9)(void *ctx, uint8_t val);
    void         *cf9_ctx;
};

static uam_pci_bus *from_cam(uam_device *d)  { return UAM_CONTAINER_OF(d, uam_pci_bus, cam); }
static uam_pci_bus *from_ecam(uam_device *d) { return UAM_CONTAINER_OF(d, uam_pci_bus, ecam); }

static uam_pci_func *find(uam_pci_bus *pb, unsigned bus, unsigned devfn)
{
    if (bus != 0)
        return NULL;
    for (uam_pci_func *f = pb->funcs; f; f = f->next)
        if (f->devfn == devfn)
            return f;
    return NULL;
}

/* ---- functions ----------------------------------------------------------- */

void uam_pci_func_init(uam_pci_func *f, const char *name, uint16_t vendor, uint16_t device,
                       uint32_t class_rev, const uam_pci_func_ops *ops, void *ctx)
{
    memset(f, 0, sizeof(*f));
    f->name = name;
    f->ops = ops;
    f->ctx = ctx;
    uam_pci_set(f->cfg, UAM_PCI_VENDOR, 2, vendor);
    uam_pci_set(f->cfg, UAM_PCI_DEVICE, 2, device);
    uam_pci_set(f->cfg, UAM_PCI_REVISION, 4, class_rev);
    uam_pci_set(f->wmask, UAM_PCI_COMMAND, 2, 0x0547);   /* I/O, memory, master, parity, SERR, INTx disable */
    f->wmask[0x0D] = 0xFF;                               /* latency timer */
    f->wmask[0x0C] = 0xFF;                               /* cache line size */
    f->wmask[UAM_PCI_INT_LINE] = 0xFF;
    f->w1c[UAM_PCI_STATUS + 1] = 0xF9;                   /* error bits */
}

void uam_pci_func_bar(uam_pci_func *f, unsigned idx, uint64_t size, unsigned flags, uam_device *dev)
{
    if (idx >= 6 || (size & (size - 1)))
        return;
    if (!(flags & UAM_PCI_BAR_IO) && size < 16) size = 16;
    if ((flags & UAM_PCI_BAR_IO) && size < 4) size = 4;
    f->bar[idx] = (uam_pci_bar){ .size = size, .flags = flags, .dev = dev };
    unsigned off = UAM_PCI_BAR0 + idx * 4;
    uint32_t low = (flags & UAM_PCI_BAR_IO) ? 1u
                 : (((flags & UAM_PCI_BAR_64) ? 4u : 0u) | ((flags & UAM_PCI_BAR_PREFETCH) ? 8u : 0u));
    uint64_t mask = ~(size - 1);
    uam_pci_set(f->cfg, off, 4, low);
    uam_pci_set(f->wmask, off, 4, (uint32_t)mask & ((flags & UAM_PCI_BAR_IO) ? ~3u : ~15u));
    if ((flags & UAM_PCI_BAR_64) && idx < 5)
        uam_pci_set(f->wmask, off + 4, 4, (uint32_t)(mask >> 32));
}

static uint64_t bar_addr(const uam_pci_func *f, unsigned i)
{
    uint32_t lo = uam_pci_get(f->cfg, UAM_PCI_BAR0 + i * 4, 4);
    if (f->bar[i].flags & UAM_PCI_BAR_IO)
        return lo & ~3u;
    uint64_t a = lo & ~15u;
    if ((f->bar[i].flags & UAM_PCI_BAR_64) && i < 5)
        a |= (uint64_t)uam_pci_get(f->cfg, UAM_PCI_BAR0 + i * 4 + 4, 4) << 32;
    return a;
}

void uam_pci_update_bars(uam_pci_func *f)
{
    uam_pci_bus *pb = f->bus;
    if (!pb)
        return;
    uint16_t cmd = (uint16_t)uam_pci_get(f->cfg, UAM_PCI_COMMAND, 2);
    for (unsigned i = 0; i < 6; i++) {
        uam_pci_bar *b = &f->bar[i];
        if (!b->size || !b->dev)
            continue;
        bool io = b->flags & UAM_PCI_BAR_IO;
        uint64_t a = bar_addr(f, i);
        bool on = (cmd & (io ? UAM_PCI_CMD_IO : UAM_PCI_CMD_MEM)) && a != 0
                  && a + b->size - 1 > a && (!io || a + b->size <= 0x10000);
        uint64_t sys = io ? pb->io_base + a : a;
        if (!b->region) {
            if (!on)
                continue;
            uam_map_attrs at = { .prio = PCI_BAR_PRIO };
            snprintf(b->rname, sizeof(b->rname), "%s.bar%u", f->name, i);
            if (uam_bus_map_mmio_ex(pb->sys, b->rname, sys, b->size, b->dev, &at) == UAM_OK)
                b->region = uam_bus_region(pb->sys, b->rname);
            continue;
        }
        if (on) {
            uam_bus_region_move(pb->sys, b->region, sys);
            uam_bus_region_enable(pb->sys, b->region, true);
        } else {
            uam_bus_region_enable(pb->sys, b->region, false);
        }
    }
}

uam_status uam_pci_register(uam_pci_bus *pb, uam_pci_func *f, unsigned dev, unsigned fn)
{
    unsigned devfn = (dev & 31) << 3 | (fn & 7);
    if (find(pb, 0, devfn))
        return UAM_ERR_INVALID;
    f->bus = pb;
    f->devfn = devfn;
    memcpy(f->init, f->cfg, sizeof(f->cfg));
    f->next = pb->funcs;
    pb->funcs = f;
    return UAM_OK;
}

/* ---- configuration access ------------------------------------------------ */

uint32_t uam_pci_cfg_read(uam_pci_bus *pb, unsigned bus, unsigned devfn, unsigned off, unsigned size)
{
    uam_pci_func *f = find(pb, bus, devfn);
    if (!f || off + size > UAM_PCI_CFG_SIZE)
        return (uint32_t)((1ull << (8 * size)) - 1);
    if (f->ops && f->ops->config_read)
        f->ops->config_read(f, off, size);
    return uam_pci_get(f->cfg, off, size);
}

void uam_pci_cfg_write(uam_pci_bus *pb, unsigned bus, unsigned devfn, unsigned off, unsigned size,
                       uint32_t val)
{
    uam_pci_func *f = find(pb, bus, devfn);
    if (!f || off + size > UAM_PCI_CFG_SIZE)
        return;
    bool bars = false;
    for (unsigned i = 0; i < size; i++) {
        unsigned o = off + i;
        uint8_t v = (uint8_t)(val >> (8 * i));
        f->cfg[o] = (uint8_t)((f->cfg[o] & ~f->wmask[o]) | (v & f->wmask[o]));
        f->cfg[o] &= (uint8_t)~(v & f->w1c[o]);
        if ((o >= UAM_PCI_BAR0 && o < UAM_PCI_BAR0 + 24) || o == UAM_PCI_COMMAND)
            bars = true;
    }
    if (f->ops && f->ops->config_write)
        f->ops->config_write(f, off, size, val);
    if (bars)
        uam_pci_update_bars(f);
}

/* ---- 0xCF8 / 0xCFC ------------------------------------------------------- */

static uam_status cam_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    uam_pci_bus *pb = from_cam(d);
    if (off < 4) {
        *val = (pb->addr >> (8 * off)) & (uint32_t)((1ull << (8 * size)) - 1);
        return UAM_OK;
    }
    if (!(pb->addr & 0x80000000u)) {
        *val = (1ull << (8 * size)) - 1;
        return UAM_OK;
    }
    unsigned reg = (pb->addr & 0xFC) + (unsigned)(off - 4);
    *val = uam_pci_cfg_read(pb, (pb->addr >> 16) & 0xFF, (pb->addr >> 8) & 0xFF, reg, size);
    return UAM_OK;
}

static uam_status cam_write(uam_device *d, uint64_t off, unsigned size, uint64_t v)
{
    uam_pci_bus *pb = from_cam(d);
    if (off == 1 && size == 1) {           /* 0xCF9: reset control, not the address */
        if (pb->cf9)
            pb->cf9(pb->cf9_ctx, (uint8_t)v);
        return UAM_OK;
    }
    if (off < 4) {
        if (off == 0 && size == 4)
            pb->addr = (uint32_t)v & 0x80FFFFFCu;
        return UAM_OK;                     /* narrower address writes are not decoded */
    }
    if (!(pb->addr & 0x80000000u))
        return UAM_OK;
    unsigned reg = (pb->addr & 0xFC) + (unsigned)(off - 4);
    uam_pci_cfg_write(pb, (pb->addr >> 16) & 0xFF, (pb->addr >> 8) & 0xFF, reg, size, (uint32_t)v);
    return UAM_OK;
}

/* ---- ECAM ---------------------------------------------------------------- */

static uam_status ecam_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    uam_pci_bus *pb = from_ecam(d);
    *val = uam_pci_cfg_read(pb, (unsigned)(off >> 20) & 0xFF, (unsigned)(off >> 12) & 0xFF,
                            (unsigned)off & 0xFFF, size);
    return UAM_OK;
}

static uam_status ecam_write(uam_device *d, uint64_t off, unsigned size, uint64_t v)
{
    uam_pci_bus *pb = from_ecam(d);
    uam_pci_cfg_write(pb, (unsigned)(off >> 20) & 0xFF, (unsigned)(off >> 12) & 0xFF,
                      (unsigned)off & 0xFFF, size, (uint32_t)v);
    return UAM_OK;
}

/* ---- the bus device ------------------------------------------------------ */

static void bus_reset(uam_device *d)
{
    uam_pci_bus *pb = from_cam(d);
    pb->addr = 0;
    for (uam_pci_func *f = pb->funcs; f; f = f->next) {
        memcpy(f->cfg, f->init, sizeof(f->cfg));
        if (f->ops && f->ops->reset)
            f->ops->reset(f);
        uam_pci_update_bars(f);
    }
}

static void bus_destroy(uam_device *d)
{
    free(from_cam(d));
}

static void bus_save(uam_device *d, uam_state_writer *w)
{
    uam_pci_bus *pb = from_cam(d);
    uam_state_put_u32(w, pb->addr);
    for (uam_pci_func *f = pb->funcs; f; f = f->next)
        uam_state_put_bytes(w, f->cfg, sizeof(f->cfg));
}

static void bus_restore(uam_device *d, uam_state_reader *r)
{
    uam_pci_bus *pb = from_cam(d);
    pb->addr = uam_state_get_u32(r);
    for (uam_pci_func *f = pb->funcs; f; f = f->next) {
        uam_state_get_bytes(r, f->cfg, sizeof(f->cfg));
        uam_pci_update_bars(f);
    }
}

static const uam_device_ops cam_ops = {
    .type = "pci,host-cam", .impl_min = 1, .impl_max = 4,
    .read = cam_read, .write = cam_write, .reset = bus_reset, .destroy = bus_destroy,
    .save = bus_save, .restore = bus_restore,
};
static const uam_device_ops ecam_ops = {
    .type = "pci,host-ecam", .impl_min = 1, .impl_max = 4,
    .read = ecam_read, .write = ecam_write, .flags = UAM_DEVICE_STATELESS,
};

uam_pci_bus *uam_pci_bus_create(const char *name, uam_bus *sys, uint64_t io_base)
{
    uam_pci_bus *pb = calloc(1, sizeof(*pb));
    if (!pb)
        return NULL;
    uam_device_init(&pb->cam, &cam_ops, name);
    uam_device_init(&pb->ecam, &ecam_ops, name);
    pb->sys = sys;
    pb->io_base = io_base;
    return pb;
}

uam_device *uam_pci_bus_device(uam_pci_bus *pb) { return &pb->cam; }
uam_device *uam_pci_cam(uam_pci_bus *pb)        { return &pb->cam; }
uam_device *uam_pci_ecam(uam_pci_bus *pb)       { return &pb->ecam; }

void uam_pci_set_cf9_hook(uam_pci_bus *pb, void (*fn)(void *ctx, uint8_t val), void *ctx)
{
    pb->cf9 = fn;
    pb->cf9_ctx = ctx;
}
