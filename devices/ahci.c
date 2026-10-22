/*
 * ahci.c - AHCI 1.3 SATA host bus adapter (uam/dev/ahci.h).
 *
 * Register layout and FIS formats follow the Serial ATA AHCI 1.3.1
 * specification; the commands follow ATA8-ACS and, for ATAPI, MMC-6.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uam/dev/ahci.h"
#include "ata_core.h"

/* generic host control */
enum { HBA_CAP = 0x00, HBA_GHC = 0x04, HBA_IS = 0x08, HBA_PI = 0x0C, HBA_VS = 0x10,
       HBA_CCC_CTL = 0x14, HBA_CCC_PORTS = 0x18, HBA_EM_LOC = 0x1C, HBA_EM_CTL = 0x20,
       HBA_CAP2 = 0x24, HBA_BOHC = 0x28, PORT0 = 0x100, PORT_SIZE = 0x80 };
/* port registers */
enum { P_CLB = 0x00, P_CLBU = 0x04, P_FB = 0x08, P_FBU = 0x0C, P_IS = 0x10, P_IE = 0x14,
       P_CMD = 0x18, P_TFD = 0x20, P_SIG = 0x24, P_SSTS = 0x28, P_SCTL = 0x2C, P_SERR = 0x30,
       P_SACT = 0x34, P_CI = 0x38, P_SNTF = 0x3C, P_FBS = 0x40 };

#define GHC_HR  (1u << 0)
#define GHC_IE  (1u << 1)
#define GHC_AE  (1u << 31)

#define CMD_ST   (1u << 0)
#define CMD_SUD  (1u << 1)
#define CMD_POD  (1u << 2)
#define CMD_CLO  (1u << 3)
#define CMD_FRE  (1u << 4)
#define CMD_FR   (1u << 14)
#define CMD_CR   (1u << 15)
#define CMD_ATAPI (1u << 24)

#define IS_DHRS  (1u << 0)
#define IS_PSS   (1u << 1)
#define IS_PCS   (1u << 6)
#define IS_PRCS  (1u << 22)
#define IS_TFES  (1u << 30)

#define SERR_N   (1u << 16)
#define SERR_X   (1u << 26)

#define ST_DSC   ATA_ST_DSC
#define ST_DRDY  ATA_ST_DRDY
#define ST_BSY   ATA_ST_BSY
#define ST_ERR   ATA_ST_ERR
#define ST_DRQ   ATA_ST_DRQ

typedef struct ahci ahci;

typedef struct ahci_port {
    uint32_t clb, clbu, fb, fbu, is, ie, cmd, tfd, sig, ssts, sctl, serr, sact, ci, sntf;
    bool     sig_pending;          /* signature FIS waiting for FIS receive */
    ata_drive d;                   /* the device (d.bd NULL: none) */
} ahci_port;

struct ahci {
    uam_device   dev;              /* ABAR */
    uam_pci_func fn;
    unsigned     nports;
    uint32_t     cap, ghc, is, cap2, bohc;
    ahci_port    port[UAM_AHCI_MAX_PORTS];
    uint8_t     *buf;              /* transfer bounce buffer */
};

static ahci *from_dev(uam_device *d) { return UAM_CONTAINER_OF(d, ahci, dev); }

static uint64_t le_get(const uint8_t *p, unsigned n)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < n; i++)
        v |= (uint64_t)p[i] << (8 * i);
    return v;
}
static void le_put(uint8_t *p, unsigned n, uint64_t v)
{
    for (unsigned i = 0; i < n; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}
/* ---- interrupts ---------------------------------------------------------- */

static void update_irq(ahci *h)
{
    uam_pci_set_intx(&h->fn, (h->ghc & GHC_IE) && h->is);
}

static void port_event(ahci *h, unsigned pi, uint32_t bits)
{
    ahci_port *p = &h->port[pi];
    p->is |= bits;
    if (p->is & p->ie) {
        bool fresh = !(h->is & (1u << pi));
        h->is |= 1u << pi;
        if (fresh && (h->ghc & GHC_IE) && uam_pci_msi_enabled(&h->fn))
            uam_pci_msi_notify(&h->fn, 0);
    }
    update_irq(h);
}

/* ---- received FISes ------------------------------------------------------ */

static uint64_t fb_addr(const ahci_port *p)  { return p->fb | (uint64_t)p->fbu << 32; }
static uint64_t clb_addr(const ahci_port *p) { return p->clb | (uint64_t)p->clbu << 32; }

static void post_fis(ahci *h, ahci_port *p, unsigned off, const uint8_t *fis, unsigned len)
{
    if (p->cmd & CMD_FRE)
        (void)uam_pci_dma_write(&h->fn, fb_addr(p) + off, fis, len);
}

/* A D2H register FIS: status, error and the taskfile values in `tf` (lba
 * 6 bytes, count 2 bytes), `irq` the I bit. */
static void post_d2h(ahci *h, ahci_port *p, uint8_t status, uint8_t error, uint64_t lba,
                     uint16_t count, bool irq)
{
    uint8_t f[20] = { 0x34, irq ? 0x40 : 0, status, error };
    le_put(f + 4, 3, lba);
    f[7] = 0xA0 | 0x40;
    le_put(f + 8, 3, lba >> 24);
    le_put(f + 12, 2, count);
    post_fis(h, p, 0x40, f, sizeof(f));
    p->tfd = (uint32_t)error << 8 | status;
}

static void post_pio_setup(ahci *h, ahci_port *p, uint8_t status, uint16_t bytes)
{
    uint8_t f[20] = { 0x5F, 0x20 | 0x40 /* device to host, I */, ST_DRDY | ST_DRQ, 0 };
    f[15] = status;                 /* E_Status */
    le_put(f + 16, 2, bytes);
    post_fis(h, p, 0x20, f, sizeof(f));
}

static void send_signature(ahci *h, ahci_port *p)
{
    if (!(p->cmd & CMD_FRE)) {
        p->sig_pending = true;
        return;
    }
    p->sig_pending = false;
    uint8_t status = p->d.atapi ? 0x00 : (ST_DRDY | ST_DSC);
    uint8_t f[20] = { 0x34, 0, status, 0x01 };
    le_put(f + 4, 3, p->sig >> 8);  /* LBA low/mid/high: 0x01/0x14/0xEB or 0x00 */
    le_put(f + 12, 1, p->sig);      /* sector count */
    post_fis(h, p, 0x40, f, sizeof(f));
    unsigned pi = (unsigned)(p - h->port);
    port_event(h, pi, IS_DHRS);
}

/* ---- port state ---------------------------------------------------------- */

/* The link came up after a COMRESET (or the HBA reset): the device sends its
 * signature. */
static void link_up(ahci *h, unsigned pi)
{
    ahci_port *p = &h->port[pi];
    if (!p->d.bd) {
        p->ssts = 0;
        p->tfd = 0x7F;
        p->sig = 0xFFFFFFFFu;
        return;
    }
    p->ssts = 0x123;                /* active, Gen2, device present with phy communication */
    p->sig = ata_signature(&p->d);
    p->tfd = p->d.atapi ? 0x00 : (ST_DRDY | ST_DSC);
    p->serr |= SERR_N | SERR_X;
    ata_drive_reset(&p->d);
    port_event(h, pi, IS_PCS | IS_PRCS);
    send_signature(h, p);
}

static void port_reset(ahci *h, unsigned pi)
{
    ahci_port *p = &h->port[pi];
    ata_drive d = p->d;
    bool atapi = d.atapi;
    memset(p, 0, sizeof(*p));
    p->d = d;
    p->cmd = CMD_SUD | CMD_POD | (atapi ? CMD_ATAPI : 0);
    link_up(h, pi);
    p->is = 0;                      /* nothing pending after a reset */
}

static void hba_reset(ahci *h)
{
    h->ghc = GHC_AE;
    h->is = 0;
    h->bohc = 0;
    for (unsigned i = 0; i < h->nports; i++)
        port_reset(h, i);
    h->is = 0;
    update_irq(h);
}

/* ---- scatter/gather ------------------------------------------------------ */

typedef struct cmd_ctx {
    ahci      *h;
    ahci_port *p;
    uint64_t   ctba;
    unsigned   prdtl;
    uint32_t   done;            /* bytes moved so far (PRDBC) */
} cmd_ctx;

/* Copy `len` bytes between `buf` and the PRD list, starting `c->done` bytes
 * into it. Returns the bytes copied (less when the PRDs run out). */
static uint32_t sg_copy(cmd_ctx *c, uint8_t *buf, uint32_t len, bool to_mem)
{
    uint64_t skip = c->done;
    uint32_t moved = 0;
    for (unsigned i = 0; i < c->prdtl && moved < len; i++) {
        uint8_t e[16];
        if (uam_pci_dma_read(&c->h->fn, c->ctba + 0x80 + i * 16ull, e, 16) != UAM_OK)
            break;
        uint64_t dba = le_get(e, 8) & ~1ull;
        uint64_t dbc = (le_get(e + 12, 4) & 0x3FFFFF) + 1;
        if (skip >= dbc) {
            skip -= dbc;
            continue;
        }
        uint64_t n = dbc - skip;
        if (n > len - moved)
            n = len - moved;
        uam_status st = to_mem ? uam_pci_dma_write(&c->h->fn, dba + skip, buf + moved, n)
                               : uam_pci_dma_read(&c->h->fn, dba + skip, buf + moved, n);
        if (st != UAM_OK)
            break;
        moved += (uint32_t)n;
        skip = 0;
    }
    c->done += moved;
    return moved;
}

static uint32_t io_copy(void *ctx, uint8_t *buf, uint32_t len, bool to_host)
{
    return sg_copy(ctx, buf, len, to_host);
}

/* ---- command list -------------------------------------------------------- */

static void run_slot(ahci *h, unsigned pi, unsigned slot)
{
    ahci_port *p = &h->port[pi];
    uint64_t hdr = clb_addr(p) + slot * 32ull;
    uint8_t ch[16], fis[64];
    p->ci &= ~(1u << slot);
    if (uam_pci_dma_read(&h->fn, hdr, ch, 16) != UAM_OK)
        return;
    uint32_t dw0 = (uint32_t)le_get(ch, 4);
    cmd_ctx c = { .h = h, .p = p, .ctba = le_get(ch + 8, 8) & ~0x7Full, .prdtl = dw0 >> 16 };
    if (uam_pci_dma_read(&h->fn, c.ctba, fis, sizeof(fis)) != UAM_OK)
        return;
    ata_result r = { .status = ST_DRDY | ST_ERR, .error = ATA_ERR_ABRT };
    ata_io io = { .ctx = &c, .copy = io_copy, .bounce = h->buf };
    if (fis[0] != 0x27 || !p->d.bd) {
        /* abort */
    } else if (!(fis[1] & 0x80)) {             /* device control: software reset */
        uint32_t zero = 0;
        (void)uam_pci_dma_write(&h->fn, hdr + 4, &zero, 4);
        if (fis[15] & 0x04) {
            p->tfd = ST_BSY;
        } else {
            p->tfd = p->d.atapi ? 0x00 : (ST_DRDY | ST_DSC);
            send_signature(h, p);
        }
        return;
    } else if (fis[2] == 0xA0 && p->d.atapi) {   /* PACKET */
        uint8_t cdb[16];
        if (uam_pci_dma_read(&h->fn, c.ctba + 0x40, cdb, 16) != UAM_OK)
            return;
        atapi_exec(&p->d, cdb, &io, &r);
    } else {
        ata_taskfile tf = { .command = fis[2], .device = fis[7],
                            .features = (uint16_t)(fis[3] | fis[11] << 8),
                            .count = (uint16_t)le_get(fis + 12, 2),
                            .lba = le_get(fis + 4, 3) | le_get(fis + 8, 3) << 24 };
        ata_exec(&p->d, &tf, &io, &r);
    }
    uint32_t prdbc = c.done;
    (void)uam_pci_dma_write(&h->fn, hdr + 4, &prdbc, 4);
    uint32_t ev = IS_DHRS;
    if (io.pio && !(r.status & ST_ERR)) {
        post_pio_setup(h, p, r.status, (uint16_t)(c.done > 0xFFFF ? 0 : c.done));
        ev |= IS_PSS;
    }
    post_d2h(h, p, r.status, r.error, r.lba, r.count, true);
    if (r.status & ST_ERR)
        ev |= IS_TFES;
    port_event(h, pi, ev);
}

static void process(ahci *h, unsigned pi)
{
    ahci_port *p = &h->port[pi];
    while ((p->cmd & CMD_ST) && p->ci) {
        unsigned slot = (unsigned)__builtin_ctz(p->ci);
        run_slot(h, pi, slot);
        p->sact &= ~(1u << slot);
    }
}

/* ---- registers ----------------------------------------------------------- */

static uint32_t port_read(ahci *h, unsigned pi, unsigned r)
{
    ahci_port *p = &h->port[pi];
    switch (r) {
    case P_CLB:  return p->clb;
    case P_CLBU: return p->clbu;
    case P_FB:   return p->fb;
    case P_FBU:  return p->fbu;
    case P_IS:   return p->is;
    case P_IE:   return p->ie;
    case P_CMD:  return p->cmd;
    case P_TFD:  return p->tfd;
    case P_SIG:  return p->sig;
    case P_SSTS: return p->ssts;
    case P_SCTL: return p->sctl;
    case P_SERR: return p->serr;
    case P_SACT: return p->sact;
    case P_CI:   return p->ci;
    case P_SNTF: return p->sntf;
    default:     return 0;
    }
}

static void port_write(ahci *h, unsigned pi, unsigned r, uint32_t v, uint32_t m)
{
    ahci_port *p = &h->port[pi];
#define MERGE(reg) ((reg) = ((reg) & ~m) | (v & m))
    switch (r) {
    case P_CLB:  MERGE(p->clb); p->clb &= ~0x3FFu; break;
    case P_CLBU: MERGE(p->clbu); break;
    case P_FB:   MERGE(p->fb); p->fb &= ~0xFFu; break;
    case P_FBU:  MERGE(p->fbu); break;
    case P_IS:
        p->is &= ~(v & m);
        if (!(p->is & p->ie))
            update_irq(h);
        break;
    case P_IE:   MERGE(p->ie); p->ie &= 0xFDC000FFu; port_event(h, pi, 0); break;
    case P_CMD: {
        uint32_t old = p->cmd, nv = (old & ~m) | (v & m);
        const uint32_t rw = CMD_ST | CMD_CLO | CMD_FRE | CMD_ATAPI | 0xF0000000u | 0x00F00000u;
        p->cmd = (old & ~rw) | (nv & rw);
        if (p->cmd & CMD_CLO) {
            p->tfd &= ~(uint32_t)(ST_BSY | ST_DRQ);
            p->cmd &= ~CMD_CLO;
        }
        p->cmd &= ~0xF0000000u;                       /* ICC: transitions complete at once */
        if (p->cmd & CMD_FRE) p->cmd |= CMD_FR;
        else                  p->cmd &= ~CMD_FR;
        if (p->cmd & CMD_ST) {
            p->cmd |= CMD_CR;
        } else {
            p->cmd &= ~CMD_CR;
            p->ci = p->sact = 0;
        }
        if ((p->cmd & CMD_FRE) && p->sig_pending)
            send_signature(h, p);
        if ((p->cmd & CMD_ST) && !(old & CMD_ST))
            process(h, pi);
        break;
    }
    case P_SCTL: {
        unsigned old_det = p->sctl & 0xF;
        MERGE(p->sctl);
        p->sctl &= 0x00000FFFu;
        unsigned det = p->sctl & 0xF;
        if (det == 1) {                                /* COMRESET asserted */
            p->ssts = p->d.bd ? 0x001 : 0;
            p->tfd = 0x80;
        } else if (det == 4) {                         /* offline */
            p->ssts = 0x4;
        } else if (det == 0 && old_det != 0) {
            link_up(h, pi);
        }
        break;
    }
    case P_SERR: p->serr &= ~(v & m); break;
    case P_SACT: if (p->cmd & CMD_ST) p->sact |= v & m; break;
    case P_CI:
        if (p->cmd & CMD_ST) {
            p->ci |= v & m;
            process(h, pi);
        }
        break;
    case P_SNTF: p->sntf &= ~(v & m); break;
    default: break;
    }
#undef MERGE
}

static uam_status ahci_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    (void)size;
    ahci *h = from_dev(d);
    uint64_t v = 0;
    if (off >= PORT0) {
        unsigned pi = (unsigned)((off - PORT0) / PORT_SIZE);
        if (pi < h->nports)
            v = port_read(h, pi, (unsigned)((off - PORT0) % PORT_SIZE));
    } else {
        switch (off) {
        case HBA_CAP:  v = h->cap; break;
        case HBA_GHC:  v = h->ghc; break;
        case HBA_IS:   v = h->is; break;
        case HBA_PI:   v = h->nports >= 32 ? 0xFFFFFFFFu : (1u << h->nports) - 1; break;
        case HBA_VS:   v = 0x00010301; break;
        case HBA_CAP2: v = h->cap2; break;
        case HBA_BOHC: v = h->bohc; break;
        default: break;
        }
    }
    *val = v;
    return UAM_OK;
}

static uam_status ahci_write_masked(uam_device *d, uint64_t off, unsigned size, uint64_t val,
                                    uint64_t mask)
{
    (void)size;
    ahci *h = from_dev(d);
    uint32_t v = (uint32_t)val, m = (uint32_t)mask;
    if (off >= PORT0) {
        unsigned pi = (unsigned)((off - PORT0) / PORT_SIZE);
        if (pi < h->nports)
            port_write(h, pi, (unsigned)((off - PORT0) % PORT_SIZE), v, m);
        return UAM_OK;
    }
    switch (off) {
    case HBA_GHC:
        if (v & m & GHC_HR) {
            hba_reset(h);
            return UAM_OK;
        }
        h->ghc = GHC_AE | (((h->ghc & ~m) | (v & m)) & GHC_IE);
        update_irq(h);
        break;
    case HBA_IS:
        h->is &= ~(v & m);
        for (unsigned i = 0; i < h->nports; i++)   /* still pending ports stay set */
            if (h->port[i].is & h->port[i].ie)
                h->is |= 1u << i;
        update_irq(h);
        break;
    case HBA_BOHC: {
        uint32_t b = (h->bohc & ~m) | (v & m);
        if (b & (1u << 1))                         /* OS ownership requested: the BIOS lets go */
            b &= ~1u;
        h->bohc = b & 0x1Fu & ~(1u << 4);
        break;
    }
    default: break;
    }
    return UAM_OK;
}

static uam_status ahci_write(uam_device *d, uint64_t off, unsigned size, uint64_t val)
{
    return ahci_write_masked(d, off, size, val, 0xFFFFFFFFu);
}

/* ---- device -------------------------------------------------------------- */

static void ahci_reset(uam_device *d)
{
    hba_reset(from_dev(d));
}

static void ahci_destroy(uam_device *d)
{
    ahci *h = from_dev(d);
    free(h->buf);
    free(h);
}

static void ahci_save(uam_device *d, uam_state_writer *w)
{
    ahci *h = from_dev(d);
    uam_state_put_u32(w, h->ghc);
    uam_state_put_u32(w, h->is);
    uam_state_put_u32(w, h->bohc);
    for (unsigned i = 0; i < h->nports; i++) {
        ahci_port *p = &h->port[i];
        uint32_t r[] = { p->clb, p->clbu, p->fb, p->fbu, p->is, p->ie, p->cmd, p->tfd, p->sig,
                         p->ssts, p->sctl, p->serr, p->sact, p->ci, p->sntf };
        for (unsigned k = 0; k < sizeof(r) / sizeof(r[0]); k++)
            uam_state_put_u32(w, r[k]);
        uam_state_put_u8(w, p->sig_pending);
        uam_state_put_u8(w, p->d.sense_key);
        uam_state_put_u8(w, p->d.asc);
        uam_state_put_u8(w, p->d.ascq);
        uam_state_put_u8(w, p->d.unit_attention);
    }
}

static void ahci_restore(uam_device *d, uam_state_reader *r)
{
    ahci *h = from_dev(d);
    h->ghc = uam_state_get_u32(r);
    h->is = uam_state_get_u32(r);
    h->bohc = uam_state_get_u32(r);
    for (unsigned i = 0; i < h->nports; i++) {
        ahci_port *p = &h->port[i];
        uint32_t *f[] = { &p->clb, &p->clbu, &p->fb, &p->fbu, &p->is, &p->ie, &p->cmd, &p->tfd,
                          &p->sig, &p->ssts, &p->sctl, &p->serr, &p->sact, &p->ci, &p->sntf };
        for (unsigned k = 0; k < sizeof(f) / sizeof(f[0]); k++)
            *f[k] = uam_state_get_u32(r);
        p->sig_pending = uam_state_get_u8(r);
        p->d.sense_key = uam_state_get_u8(r);
        p->d.asc = uam_state_get_u8(r);
        p->d.ascq = uam_state_get_u8(r);
        p->d.unit_attention = uam_state_get_u8(r);
    }
}

static const uam_device_ops ahci_ops = {
    .type = "ahci", .impl_min = 4, .impl_max = 4,
    .read = ahci_read, .write = ahci_write, .write_masked = ahci_write_masked,
    .reset = ahci_reset, .destroy = ahci_destroy, .save = ahci_save, .restore = ahci_restore,
};

uam_device *uam_ahci_create(const char *name, unsigned nports, uint16_t vendor, uint16_t device,
                            uint8_t revision)
{
    if (nports == 0 || nports > UAM_AHCI_MAX_PORTS)
        return NULL;
    ahci *h = calloc(1, sizeof(*h));
    if (!h || !(h->buf = malloc(ATA_XFER_MAX))) {
        free(h);
        return NULL;
    }
    uam_device_init(&h->dev, &ahci_ops, name);
    h->nports = nports;
    h->cap = 1u << 31 /* S64A */ | 1u << 24 /* SCLO */ | 2u << 20 /* Gen2 */ |
             1u << 18 /* AHCI only */ | 31u << 8 /* 32 slots */ | (nports - 1);
    h->cap2 = 0;
    uam_pci_func *f = &h->fn;
    uam_pci_func_init(f, name, vendor, device, 0x01060100u | revision, NULL, h);
    uam_pci_func_bar(f, 5, nports > 30 ? 0x2000 : 0x1000, UAM_PCI_BAR_MEM, &h->dev);
    f->cfg[UAM_PCI_INT_PIN] = 1;
    unsigned pm = uam_pci_add_cap(f, UAM_PCI_CAP_PM, 8);
    uam_pci_set(f->cfg, pm + 2, 2, 0x0003);            /* PM 1.2 */
    uam_pci_set(f->wmask, pm + 4, 2, 0x0003);          /* power state */
    uam_pci_msi_init(f, 0, true, false);
    for (unsigned i = 0; i < nports; i++)
        h->port[i].sig = 0xFFFFFFFFu;
    hba_reset(h);
    return &h->dev;
}

uam_pci_func *uam_ahci_func(uam_device *d) { return &from_dev(d)->fn; }

uam_status uam_ahci_attach(uam_device *d, unsigned port, uam_blockdev *bd, bool atapi)
{
    ahci *h = from_dev(d);
    if (port >= h->nports || h->port[port].d.bd || !bd ||
        bd->sector_size != (atapi ? 2048u : 512u))
        return UAM_ERR_INVALID;
    ata_drive *dr = &h->port[port].d;
    dr->bd = bd;
    dr->atapi = atapi;
    snprintf(dr->serial, sizeof(dr->serial), "UAM-AHCI-%02u", port);
    dr->model = atapi ? "UAM SATA DVD-ROM" : "UAM SATA disk";
    dr->product = "SATA DVD-ROM    ";
    port_reset(h, port);
    h->is = 0;
    return UAM_OK;
}
