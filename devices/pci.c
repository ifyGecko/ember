/*
 * pci.c - PCI configuration space, BAR decoding, bridges, INTx and MSI
 * (uam/dev/pci.h).
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
    uint32_t      addr;         /* CONFIG_ADDRESS */
    uam_device   *cam_through;  /* other accesses to CONFIG_ADDRESS (uam_pci_cam_passthrough) */
    uam_pci_func *funcs;
    uam_pci_intx_fn intx;
    void         *intx_ctx;
    uam_pci_msi_fn msi;
    void         *msi_ctx;
};

struct uam_pci_msix {
    unsigned  n;
    unsigned  table_bar, pba_bar;
    uint32_t  table_off, pba_off;
    uint8_t  *table;          /* 16 bytes per entry, as the function's BAR shows them (LE) */
    uint8_t  *pba;            /* the pending bits, bit v in byte v / 8 (LE qwords) */
};

/* Little-endian values of 1-8 bytes (MSI-X structures take qword accesses). */
static uint64_t get_le(const uint8_t *p, unsigned size)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < size; i++)
        v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static void put_le(uint8_t *p, unsigned size, uint64_t v)
{
    for (unsigned i = 0; i < size; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

/* The MSI-X table's dwords: entry v's dword k. */
static uint32_t tab_get(const uam_pci_msix *x, unsigned v, unsigned k)
{
    return (uint32_t)get_le(x->table + v * 16 + k * 4, 4);
}

static uam_pci_bus *from_cam(uam_device *d)  { return UAM_CONTAINER_OF(d, uam_pci_bus, cam); }
static uam_pci_bus *from_ecam(uam_device *d) { return UAM_CONTAINER_OF(d, uam_pci_bus, ecam); }

static uint16_t cmd_of(const uam_pci_func *f)
{
    return (uint16_t)uam_pci_get(f->cfg, UAM_PCI_COMMAND, 2);
}

/* The bus number a function answers on, or -1 while the bridges above it do
 * not forward configuration cycles to it. */
static int func_bus(const uam_pci_func *f)
{
    if (!f->parent)
        return 0;
    unsigned n = f->parent->cfg[UAM_PCI_SECONDARY_BUS];
    for (const uam_pci_func *a = f->parent; a; a = a->parent) {
        unsigned sec = a->cfg[UAM_PCI_SECONDARY_BUS], sub = a->cfg[UAM_PCI_SUBORDINATE_BUS];
        if (sec == 0 || n < sec || n > sub)
            return -1;
    }
    return func_bus(f->parent) < 0 ? -1 : (int)n;
}

static uam_pci_func *find(uam_pci_bus *pb, unsigned bus, unsigned devfn)
{
    for (uam_pci_func *f = pb->funcs; f; f = f->next)
        if (f->devfn == devfn && func_bus(f) == (int)bus)
            return f;
    return NULL;
}

/* ---- bridge windows ------------------------------------------------------ */

static bool in_window(uint64_t a, uint64_t size, uint64_t base, uint64_t limit)
{
    return base <= limit && a >= base && a + size - 1 <= limit;
}

/* Does every bridge above `f` forward [a, a+size) to it? */
static bool forwarded(const uam_pci_func *f, uint64_t a, uint64_t size, bool io)
{
    for (const uam_pci_func *b = f->parent; b; b = b->parent) {
        const uint8_t *c = b->cfg;
        if (!(cmd_of(b) & (io ? UAM_PCI_CMD_IO : UAM_PCI_CMD_MEM)))
            return false;
        if (io) {
            uint64_t base = (uint64_t)(c[UAM_PCI_IO_BASE] & 0xF0) << 8 |
                            (uint64_t)uam_pci_get(c, UAM_PCI_IO_BASE_HI, 2) << 16;
            uint64_t limit = (uint64_t)(c[UAM_PCI_IO_LIMIT] & 0xF0) << 8 | 0xFFF |
                             (uint64_t)uam_pci_get(c, UAM_PCI_IO_LIMIT_HI, 2) << 16;
            if (!in_window(a, size, base, limit))
                return false;
            continue;
        }
        uint64_t mb = (uint64_t)(uam_pci_get(c, UAM_PCI_MEM_BASE, 2) & 0xFFF0) << 16;
        uint64_t ml = (uint64_t)(uam_pci_get(c, UAM_PCI_MEM_LIMIT, 2) & 0xFFF0) << 16 | 0xFFFFF;
        uint64_t pbase = (uint64_t)(uam_pci_get(c, UAM_PCI_PREF_BASE, 2) & 0xFFF0) << 16 |
                         (uint64_t)uam_pci_get(c, UAM_PCI_PREF_BASE_HI, 4) << 32;
        uint64_t plim = (uint64_t)(uam_pci_get(c, UAM_PCI_PREF_LIMIT, 2) & 0xFFF0) << 16 | 0xFFFFF |
                        (uint64_t)uam_pci_get(c, UAM_PCI_PREF_LIMIT_HI, 4) << 32;
        if (!in_window(a, size, mb, ml) && !in_window(a, size, pbase, plim))
            return false;
    }
    return true;
}

static bool mastering(const uam_pci_func *f)
{
    for (; f; f = f->parent)
        if (!(cmd_of(f) & UAM_PCI_CMD_MASTER))
            return false;
    return true;
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
    f->cap_free = 0x40;
}

void uam_pci_bridge_init(uam_pci_func *f, const char *name, uint16_t vendor, uint16_t device,
                         uint32_t class_rev, const uam_pci_func_ops *ops, void *ctx)
{
    uam_pci_func_init(f, name, vendor, device, class_rev, ops, ctx);
    f->bridge = true;
    f->cfg[UAM_PCI_HEADER] = 0x01;
    memset(f->wmask + UAM_PCI_PRIMARY_BUS, 0xFF, 4);         /* bus numbers, latency */
    f->cfg[UAM_PCI_IO_BASE] = f->cfg[UAM_PCI_IO_LIMIT] = 0x01;      /* 32-bit I/O */
    f->wmask[UAM_PCI_IO_BASE] = f->wmask[UAM_PCI_IO_LIMIT] = 0xF0;
    f->w1c[UAM_PCI_SEC_STATUS + 1] = 0xF9;
    uam_pci_set(f->wmask, UAM_PCI_MEM_BASE, 4, 0xFFF0FFF0);
    uam_pci_set(f->cfg, UAM_PCI_PREF_BASE, 4, 0x00010001);          /* 64-bit prefetchable */
    uam_pci_set(f->wmask, UAM_PCI_PREF_BASE, 4, 0xFFF0FFF0);
    uam_pci_set(f->wmask, UAM_PCI_PREF_BASE_HI, 4, 0xFFFFFFFF);
    uam_pci_set(f->wmask, UAM_PCI_PREF_LIMIT_HI, 4, 0xFFFFFFFF);
    uam_pci_set(f->wmask, UAM_PCI_IO_BASE_HI, 4, 0xFFFFFFFF);
    uam_pci_set(f->wmask, UAM_PCI_BRIDGE_CTL, 2, 0x0FFF);
}

unsigned uam_pci_add_cap(uam_pci_func *f, uint8_t id, unsigned len)
{
    unsigned off = (f->cap_free + 3) & ~3u;
    if (len < 2 || off + len > 0x100)
        return 0;
    f->cap_free = off + len;
    f->cfg[off] = id;
    f->cfg[off + 1] = 0;
    /* append, keeping the list in the order the capabilities were added */
    unsigned link = UAM_PCI_CAP_PTR;
    while (f->cfg[link])
        link = f->cfg[link] + 1u;
    f->cfg[link] = (uint8_t)off;
    f->cfg[UAM_PCI_STATUS] |= UAM_PCI_STATUS_CAPS;
    return off;
}

unsigned uam_pci_add_pcie_cap(uam_pci_func *f, unsigned port_type, unsigned slot_nr)
{
    unsigned o = uam_pci_add_cap(f, UAM_PCI_CAP_PCIE, 0x3C);
    if (!o)
        return 0;
    bool slot = port_type == UAM_PCIE_ROOT_PORT || port_type == UAM_PCIE_DOWNSTREAM;
    uam_pci_set(f->cfg, o + 2, 2, 0x2 | (port_type & 0xF) << 4 | (slot ? 1u << 8 : 0));
    uam_pci_set(f->cfg, o + 4, 4, 1u << 15);                 /* devcap: role-based error reporting */
    uam_pci_set(f->wmask, o + 8, 2, 0x7CFF);                 /* device control */
    f->w1c[o + 0x0A] = 0x0F;                                 /* device status error bits */
    uam_pci_set(f->cfg, o + 0x0C, 4, (slot_nr & 0xFF) << 24 | 1u << 20 | 1u << 4 | 1);
    uam_pci_set(f->wmask, o + 0x10, 2, 0x0FFB);               /* link control */
    uam_pci_set(f->cfg, o + 0x12, 2, 1u << 13 | 1u << 4 | 1);  /* link up, x1, 2.5 GT/s */
    if (slot) {
        uam_pci_set(f->cfg, o + 0x14, 4, (slot_nr & 0x1FFF) << 19 | 1u << 6);  /* hot-plug surprise */
        uam_pci_set(f->wmask, o + 0x18, 2, 0x1FFF);           /* slot control */
        uam_pci_set(f->cfg, o + 0x1A, 2, 1u << 6);            /* presence detected */
        f->w1c[o + 0x1A] = 0x1F;
        uam_pci_set(f->wmask, o + 0x1C, 2, 0x001F);           /* root control */
        f->w1c[o + 0x22] = 0x01;                              /* root status: PME */
    }
    uam_pci_set(f->cfg, o + 0x24, 4, 1u << 4);                /* devcap2: completion timeout disable */
    uam_pci_set(f->wmask, o + 0x28, 2, 0x0410);
    uam_pci_set(f->cfg, o + 0x2C, 4, 0x2);                    /* linkcap2: 2.5 GT/s */
    uam_pci_set(f->cfg, o + 0x30, 2, 0x1);                    /* link control 2: target speed */
    uam_pci_set(f->wmask, o + 0x30, 2, 0x000F);
    return o;
}

/* ---- MSI ----------------------------------------------------------------- */

static unsigned msi_ctl(const uam_pci_func *f)
{
    return uam_pci_get(f->cfg, f->msi_cap + 2, 2);
}

static unsigned msi_data_off(const uam_pci_func *f)
{
    return f->msi_cap + ((msi_ctl(f) & 0x80) ? 0x0C : 0x08);
}

unsigned uam_pci_msi_init(uam_pci_func *f, unsigned log2_vectors, bool addr64, bool maskable)
{
    if (log2_vectors > 5)
        log2_vectors = 5;
    unsigned len = (addr64 ? 0x0E : 0x0A) + (maskable ? 0x0A : 0);
    unsigned o = uam_pci_add_cap(f, UAM_PCI_CAP_MSI, len);
    if (!o)
        return 0;
    f->msi_cap = o;
    uam_pci_set(f->cfg, o + 2, 2, log2_vectors << 1 | (addr64 ? 0x80u : 0) | (maskable ? 0x100u : 0));
    uam_pci_set(f->wmask, o + 2, 2, 0x0071);
    uam_pci_set(f->wmask, o + 4, 4, 0xFFFFFFFC);
    if (addr64)
        uam_pci_set(f->wmask, o + 8, 4, 0xFFFFFFFF);
    unsigned d = o + (addr64 ? 0x0C : 0x08);
    uam_pci_set(f->wmask, d, 2, 0xFFFF);
    if (maskable)
        uam_pci_set(f->wmask, d + 4, 4, (uint32_t)((1ull << (1u << log2_vectors)) - 1));
    return o;
}

static bool msi_on(const uam_pci_func *f)  { return f->msi_cap && (msi_ctl(f) & 1); }
static bool msix_on(const uam_pci_func *f)
{
    return f->msix_cap && (uam_pci_get(f->cfg, f->msix_cap + 2, 2) & 0x8000);
}

bool uam_pci_msi_enabled(const uam_pci_func *f) { return msi_on(f) || msix_on(f); }

static void send_msg(uam_pci_func *f, uint64_t addr, uint32_t data)
{
    if (!mastering(f))
        return;
    uam_pci_bus *pb = f->bus;
    if (pb->msi) {
        pb->msi(pb->msi_ctx, addr, data);
    } else {
        uint8_t b[4];                /* PCI is little-endian */
        put_le(b, 4, data);
        (void)uam_bus_dma_write(pb->sys, addr, b, 4);
    }
}

static void msi_send(uam_pci_func *f, unsigned vec)
{
    unsigned ctl = msi_ctl(f), o = f->msi_cap;
    uint64_t addr = uam_pci_get(f->cfg, o + 4, 4);
    if (ctl & 0x80)
        addr |= (uint64_t)uam_pci_get(f->cfg, o + 8, 4) << 32;
    unsigned mme = (ctl >> 4) & 7;
    uint32_t data = uam_pci_get(f->cfg, msi_data_off(f), 2);
    uint32_t low = (1u << mme) - 1;
    send_msg(f, addr, (data & ~low) | (vec & low));
}

static void msi_retry(uam_pci_func *f)
{
    if (!f->msi_pending || !msi_on(f))
        return;
    uint32_t mask = (msi_ctl(f) & 0x100) ? uam_pci_get(f->cfg, msi_data_off(f) + 4, 4) : 0;
    for (unsigned v = 0; v < 32; v++)
        if ((f->msi_pending & ~mask) >> v & 1) {
            f->msi_pending &= ~(1u << v);
            msi_send(f, v);
        }
    if (msi_ctl(f) & 0x100)
        uam_pci_set(f->cfg, msi_data_off(f) + 8, 4, f->msi_pending);
}

/* ---- MSI-X --------------------------------------------------------------- */

unsigned uam_pci_msix_init(uam_pci_func *f, unsigned n, unsigned table_bar, uint32_t table_off,
                           unsigned pba_bar, uint32_t pba_off)
{
    if (n == 0 || n > 2048 || table_bar > 5 || pba_bar > 5 || ((table_off | pba_off) & 7))
        return 0;
    uam_pci_msix *x = calloc(1, sizeof(*x));
    if (!x)
        return 0;
    x->table = calloc(n, 16);
    x->pba = calloc((n + 63) / 64, 8);
    if (!x->table || !x->pba) {
        free(x->table); free(x->pba); free(x);
        return 0;
    }
    unsigned o = uam_pci_add_cap(f, UAM_PCI_CAP_MSIX, 12);
    if (!o) {
        free(x->table); free(x->pba); free(x);
        return 0;
    }
    x->n = n;
    x->table_bar = table_bar; x->table_off = table_off;
    x->pba_bar = pba_bar; x->pba_off = pba_off;
    for (unsigned i = 0; i < n; i++)
        x->table[i * 16 + 12] = 1;           /* vector control: masked */
    f->msix = x;
    f->msix_cap = o;
    uam_pci_set(f->cfg, o + 2, 2, n - 1);
    uam_pci_set(f->wmask, o + 2, 2, 0xC000);
    uam_pci_set(f->cfg, o + 4, 4, table_off | table_bar);
    uam_pci_set(f->cfg, o + 8, 4, pba_off | pba_bar);
    return o;
}

static bool msix_masked(const uam_pci_func *f, unsigned v)
{
    return (uam_pci_get(f->cfg, f->msix_cap + 2, 2) & 0x4000) || (tab_get(f->msix, v, 3) & 1);
}

static void msix_retry(uam_pci_func *f)
{
    uam_pci_msix *x = f->msix;
    if (!x || !msix_on(f))
        return;
    for (unsigned v = 0; v < x->n; v++)
        if ((x->pba[v / 8] >> (v % 8) & 1) && !msix_masked(f, v)) {
            x->pba[v / 8] &= (uint8_t)~(1u << (v % 8));
            send_msg(f, tab_get(x, v, 0) | (uint64_t)tab_get(x, v, 1) << 32, tab_get(x, v, 2));
        }
}

bool uam_pci_msix_access(uam_pci_func *f, unsigned bar, uint64_t off, unsigned size, bool write,
                         uint64_t *val)
{
    uam_pci_msix *x = f->msix;
    if (!x)
        return false;
    if (bar == x->table_bar && off >= x->table_off && off + size <= x->table_off + x->n * 16ull) {
        uint64_t o = off - x->table_off;
        if (!write) {
            *val = get_le(x->table + o, size);
            return true;
        }
        put_le(x->table + o, size, *val);
        unsigned v = (unsigned)(o / 16);
        put_le(x->table + v * 16 + 12, 4, tab_get(x, v, 3) & 1);   /* only the mask bit is defined */
        msix_retry(f);
        return true;
    }
    uint64_t pba_len = ((x->n + 63) / 64) * 8ull;
    if (bar == x->pba_bar && off >= x->pba_off && off + size <= x->pba_off + pba_len) {
        if (!write)
            *val = get_le(x->pba + (off - x->pba_off), size);
        return true;                         /* the PBA is read-only */
    }
    return false;
}

void uam_pci_msi_notify(uam_pci_func *f, unsigned vector)
{
    if (msix_on(f)) {
        uam_pci_msix *x = f->msix;
        if (vector >= x->n)
            return;
        if (msix_masked(f, vector)) {
            x->pba[vector / 8] |= (uint8_t)(1u << (vector % 8));
            return;
        }
        send_msg(f, tab_get(x, vector, 0) | (uint64_t)tab_get(x, vector, 1) << 32,
                 tab_get(x, vector, 2));
        return;
    }
    if (!msi_on(f) || vector >= (1u << ((msi_ctl(f) >> 4) & 7)) || vector >= 32)
        return;
    if (msi_ctl(f) & 0x100) {
        uint32_t mask = uam_pci_get(f->cfg, msi_data_off(f) + 4, 4);
        if (mask >> vector & 1) {
            f->msi_pending |= 1u << vector;
            uam_pci_set(f->cfg, msi_data_off(f) + 8, 4, f->msi_pending);
            return;
        }
    }
    msi_send(f, vector);
}

/* ---- INTx ---------------------------------------------------------------- */

/* The root-bus slot and pin (0-3) a function's INTx arrives on. */
static void intx_route(const uam_pci_func *f, unsigned *slot, unsigned *pin)
{
    unsigned p = (f->cfg[UAM_PCI_INT_PIN] - 1u) & 3;
    for (; f->parent; f = f->parent)
        p = (p + (f->devfn >> 3)) & 3;
    *slot = f->devfn >> 3;
    *pin = p;
}

static void intx_update(uam_pci_func *f)
{
    uam_pci_bus *pb = f->bus;
    bool out = f->intx && f->cfg[UAM_PCI_INT_PIN] && !(cmd_of(f) & UAM_PCI_CMD_INTX_OFF) &&
               !uam_pci_msi_enabled(f);
    if (f->intx)
        f->cfg[UAM_PCI_STATUS] |= UAM_PCI_STATUS_INTX;
    else
        f->cfg[UAM_PCI_STATUS] &= (uint8_t)~UAM_PCI_STATUS_INTX;
    if (!pb || out == f->intx_out)
        return;
    f->intx_out = out;
    if (!pb->intx)
        return;
    unsigned slot, pin;
    intx_route(f, &slot, &pin);
    bool any = false;                         /* wired-OR with the other functions */
    for (uam_pci_func *g = pb->funcs; g && !any; g = g->next) {
        unsigned s2, p2;
        if (!g->intx_out)
            continue;
        intx_route(g, &s2, &p2);
        any = s2 == slot && p2 == pin;
    }
    pb->intx(pb->intx_ctx, slot, pin, any);
}

void uam_pci_set_intx(uam_pci_func *f, int level)
{
    f->intx = level != 0;
    intx_update(f);
}

void uam_pci_set_intx_router(uam_pci_bus *pb, uam_pci_intx_fn fn, void *ctx)
{
    pb->intx = fn;
    pb->intx_ctx = ctx;
}

void uam_pci_set_msi_sink(uam_pci_bus *pb, uam_pci_msi_fn fn, void *ctx)
{
    pb->msi = fn;
    pb->msi_ctx = ctx;
}

/* ---- DMA ----------------------------------------------------------------- */

uam_status uam_pci_dma_read(uam_pci_func *f, uint64_t addr, void *buf, size_t len)
{
    if (!f->bus || !mastering(f))
        return UAM_ERR_FAULT;
    return uam_bus_dma_read(f->bus->sys, addr, buf, len);
}

uam_status uam_pci_dma_write(uam_pci_func *f, uint64_t addr, const void *buf, size_t len)
{
    if (!f->bus || !mastering(f))
        return UAM_ERR_FAULT;
    return uam_bus_dma_write(f->bus->sys, addr, buf, len);
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

void uam_pci_func_rom(uam_pci_func *f, uint64_t size, uam_device *dev)
{
    if (f->bridge || size < 2048 || (size & (size - 1)) || size > (1ull << 31))
        return;
    f->rom = (uam_pci_bar){ .size = size, .flags = UAM_PCI_BAR_MEM, .dev = dev };
    uam_pci_set(f->cfg, 0x30, 4, 0);
    uam_pci_set(f->wmask, 0x30, 4, ((uint32_t)~(size - 1) & 0xFFFFF800u) | 1u);
}

/* Map, move or unmap one BAR's region as decoding says. */
static void decode_bar(uam_pci_func *f, uam_pci_bar *b, const char *what, bool on, uint64_t sys)
{
    uam_pci_bus *pb = f->bus;
    if (!b->region) {
        if (!on)
            return;
        uam_map_attrs at = { .prio = PCI_BAR_PRIO };
        snprintf(b->rname, sizeof(b->rname), "%s.%s", f->name, what);
        if (uam_bus_map_mmio_ex(pb->sys, b->rname, sys, b->size, b->dev, &at) == UAM_OK)
            b->region = uam_bus_region(pb->sys, b->rname);
        return;
    }
    if (on) {
        uam_bus_region_move(pb->sys, b->region, sys);
        uam_bus_region_enable(pb->sys, b->region, true);
    } else {
        uam_bus_region_enable(pb->sys, b->region, false);
    }
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
    uint16_t cmd = cmd_of(f);
    bool reach = func_bus(f) >= 0;
    for (unsigned i = 0; i < (f->bridge ? 2u : 6u); i++) {
        uam_pci_bar *b = &f->bar[i];
        if (!b->size || !b->dev)
            continue;
        bool io = b->flags & UAM_PCI_BAR_IO;
        uint64_t a = bar_addr(f, i);
        bool on = (cmd & (io ? UAM_PCI_CMD_IO : UAM_PCI_CMD_MEM)) && a != 0
                  && a + b->size - 1 > a && (!io || a + b->size <= 0x10000)
                  && reach && forwarded(f, a, b->size, io);
        char what[8];
        snprintf(what, sizeof(what), "bar%u", i);
        decode_bar(f, b, what, on, io ? pb->io_base + a : a);
    }
    if (f->rom.size && f->rom.dev) {   /* the expansion ROM: enabled by bit 0 */
        uint32_t r = uam_pci_get(f->cfg, 0x30, 4);
        uint64_t a = r & 0xFFFFF800u;
        bool on = (cmd & UAM_PCI_CMD_MEM) && (r & 1) && a != 0 && reach &&
                  forwarded(f, a, f->rom.size, false);
        decode_bar(f, &f->rom, "rom", on, a);
    }
}

/* ---- option ROM images --------------------------------------------------- */

typedef struct rom_image {
    uam_device dev;
    size_t     len;
    uint8_t    data[];
} rom_image;

static uam_status rom_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    rom_image *r = UAM_CONTAINER_OF(d, rom_image, dev);
    *val = 0;
    for (unsigned i = 0; i < size; i++)
        *val |= (uint64_t)(off + i < r->len ? r->data[off + i] : 0xFF) << (8 * i);
    return UAM_OK;
}

static uam_status rom_write(uam_device *d, uint64_t off, unsigned size, uint64_t val)
{
    (void)d; (void)off; (void)size; (void)val;
    return UAM_OK;
}

static void rom_destroy(uam_device *d) { free(UAM_CONTAINER_OF(d, rom_image, dev)); }

static const uam_device_ops rom_ops = {
    .type = "pci-rom-image", .read = rom_read, .write = rom_write, .destroy = rom_destroy,
    .flags = UAM_DEVICE_STATELESS, .peek = rom_read,   /* reads are pure */
};

uam_device *uam_pci_rom_image_create(const char *name, const uint8_t *data, size_t len)
{
    rom_image *r = malloc(sizeof(*r) + len);
    if (!r)
        return NULL;
    memset(r, 0, sizeof(*r));
    uam_device_init(&r->dev, &rom_ops, name);
    r->len = len;
    memcpy(r->data, data, len);
    return &r->dev;
}

static void update_all(uam_pci_bus *pb)
{
    for (uam_pci_func *f = pb->funcs; f; f = f->next)
        uam_pci_update_bars(f);
}

uam_status uam_pci_register_at(uam_pci_bus *pb, uam_pci_func *bridge, uam_pci_func *f,
                               unsigned dev, unsigned fn)
{
    unsigned devfn = (dev & 31) << 3 | (fn & 7);
    if (bridge && (!bridge->bridge || bridge->bus != pb))
        return UAM_ERR_INVALID;
    for (uam_pci_func *g = pb->funcs; g; g = g->next)
        if (g->parent == bridge && g->devfn == devfn)
            return UAM_ERR_INVALID;
    f->bus = pb;
    f->parent = bridge;
    f->devfn = devfn;
    memcpy(f->init, f->cfg, sizeof(f->cfg));
    f->next = pb->funcs;
    pb->funcs = f;
    return UAM_OK;
}

uam_status uam_pci_register(uam_pci_bus *pb, uam_pci_func *f, unsigned dev, unsigned fn)
{
    return uam_pci_register_at(pb, NULL, f, dev, fn);
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
    bool bars = false, all = false, intr = false;
    for (unsigned i = 0; i < size; i++) {
        unsigned o = off + i;
        uint8_t v = (uint8_t)(val >> (8 * i));
        f->cfg[o] = (uint8_t)((f->cfg[o] & ~f->wmask[o]) | (v & f->wmask[o]));
        f->cfg[o] &= (uint8_t)~(v & f->w1c[o]);
        if ((o >= UAM_PCI_BAR0 && o < UAM_PCI_BAR0 + 24) || o == UAM_PCI_COMMAND ||
            (!f->bridge && o >= 0x30 && o < 0x34))
            bars = true;
        if (f->bridge && (o == UAM_PCI_COMMAND || (o >= UAM_PCI_PRIMARY_BUS && o < 0x34)))
            all = true;                    /* bus numbers or windows: the subtree */
        if (o == UAM_PCI_COMMAND + 1 || (f->msi_cap && o >= f->msi_cap && o < f->msi_cap + 0x18) ||
            (f->msix_cap && o >= f->msix_cap + 2 && o < f->msix_cap + 4))
            intr = true;
    }
    if (f->ops && f->ops->config_write)
        f->ops->config_write(f, off, size, val);
    if (intr) {
        intx_update(f);
        msi_retry(f);
        msix_retry(f);
    }
    if (all)
        update_all(pb);
    else if (bars)
        uam_pci_update_bars(f);
}

/* ---- configuration mechanism #1 (CONFIG_ADDRESS / CONFIG_DATA) ------------ */

/* Only a dword access to CONFIG_ADDRESS is a configuration access (PCI Local Bus
 * spec, configuration mechanism #1); the bridge passes any other one through. */
static bool cam_through(const uam_pci_bus *pb, uint64_t off, unsigned size)
{
    return off < 4 && !(off == 0 && size == 4) && pb->cam_through;
}

static uam_status cam_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    uam_pci_bus *pb = from_cam(d);
    if (cam_through(pb, off, size))
        return pb->cam_through->ops->read(pb->cam_through, off, size, val);
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
    if (cam_through(pb, off, size))
        return pb->cam_through->ops->write(pb->cam_through, off, size, v);
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
        f->msi_pending = 0;
        f->intx_out = false;
        if (f->msix) {
            memset(f->msix->table, 0, f->msix->n * 16ull);
            for (unsigned i = 0; i < f->msix->n; i++)
                f->msix->table[i * 16 + 12] = 1;
            memset(f->msix->pba, 0, (f->msix->n + 63) / 64 * 8ull);
        }
        if (f->ops && f->ops->reset)
            f->ops->reset(f);
        uam_pci_update_bars(f);
        intx_update(f);
    }
}

static void bus_destroy(uam_device *d)
{
    uam_pci_bus *pb = from_cam(d);
    for (uam_pci_func *f = pb->funcs; f; f = f->next)
        if (f->msix) {
            free(f->msix->table);
            free(f->msix->pba);
            free(f->msix);
            f->msix = NULL;
        }
    free(pb);
}

static void bus_save(uam_device *d, uam_state_writer *w)
{
    uam_pci_bus *pb = from_cam(d);
    uam_state_put_u32(w, pb->addr);
    for (uam_pci_func *f = pb->funcs; f; f = f->next) {
        uam_state_put_bytes(w, f->cfg, sizeof(f->cfg));
        uam_state_put_u32(w, f->msi_pending);
        uam_state_put_u32(w, (uint32_t)f->intx | (uint32_t)f->intx_out << 1);
        if (f->msix) {
            uam_state_put_bytes(w, f->msix->table, f->msix->n * 16ull);
            uam_state_put_bytes(w, f->msix->pba, (f->msix->n + 63) / 64 * 8ull);
        }
    }
}

static void bus_restore(uam_device *d, uam_state_reader *r)
{
    uam_pci_bus *pb = from_cam(d);
    pb->addr = uam_state_get_u32(r);
    for (uam_pci_func *f = pb->funcs; f; f = f->next) {
        uam_state_get_bytes(r, f->cfg, sizeof(f->cfg));
        f->msi_pending = uam_state_get_u32(r);
        uint32_t ix = uam_state_get_u32(r);
        f->intx = ix & 1;
        f->intx_out = ix >> 1 & 1;
        if (f->msix) {
            uam_state_get_bytes(r, f->msix->table, f->msix->n * 16ull);
            uam_state_get_bytes(r, f->msix->pba, (f->msix->n + 63) / 64 * 8ull);
        }
    }
    update_all(pb);
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

uam_pci_bus *uam_pci_bus_of(uam_device *cam)
{
    return cam && cam->ops == &cam_ops ? (uam_pci_bus *)cam : NULL;   /* cam comes first */
}

void uam_pci_cam_passthrough(uam_pci_bus *pb, uam_device *dev)
{
    pb->cam_through = dev;
}
