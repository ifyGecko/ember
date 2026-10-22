/*
 * nvme.c - NVM Express controller (uam/dev/nvme.h), following the NVM
 * Express Base Specification 1.4.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uam/dev/nvme.h"

/* controller registers */
enum { R_CAP = 0x00, R_VS = 0x08, R_INTMS = 0x0C, R_INTMC = 0x10, R_CC = 0x14, R_CSTS = 0x1C,
       R_NSSR = 0x20, R_AQA = 0x24, R_ASQ = 0x28, R_ACQ = 0x30, R_DBS = 0x1000 };
#define BAR_SIZE    0x4000u
#define MSIX_TABLE  0x2000u
#define MSIX_PBA    0x3000u
#define NQ          (UAM_NVME_MAX_QUEUES + 1)   /* queue 0 is the admin queue */
#define MQES        1023u                       /* entries per queue - 1 */
#define MDTS        7u                          /* 2^7 pages per transfer */
#define CC_EN       (1u << 0)
#define CSTS_RDY    (1u << 0)

/* status: (SCT << 8) | SC */
enum { SC_OK = 0x000, SC_INVALID_OPCODE = 0x001, SC_INVALID_FIELD = 0x002,
       SC_DATA_XFER = 0x004, SC_INVALID_NS = 0x00B, SC_LBA_RANGE = 0x080,
       SC_CQ_INVALID = 0x100, SC_QID_INVALID = 0x101, SC_QSIZE_INVALID = 0x102,
       SC_VECTOR_INVALID = 0x108, SC_DELETE_INVALID = 0x10C };

typedef struct nq {
    uint64_t base;
    uint16_t size;         /* entries */
    uint16_t head, tail;
    uint16_t cqid;         /* SQ: its completion queue */
    uint16_t iv;           /* CQ: interrupt vector */
    bool     valid, ien, phase;
} nq;

typedef struct nvme {
    uam_device    dev;
    uam_pci_func  fn;
    uint32_t      cc, csts, aqa, intms;
    uint64_t      asq, acq;
    nq            sq[NQ], cq[NQ];
    bool          wce;     /* volatile write cache enabled */
    uam_blockdev *ns[UAM_NVME_MAX_NS];
    uint8_t      *buf;     /* transfer bounce buffer */
    char          serial[21];
} nvme;

static nvme *from_dev(uam_device *d) { return UAM_CONTAINER_OF(d, nvme, dev); }

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

static uint64_t page_size(const nvme *c) { return 4096ull << ((c->cc >> 7) & 0xF); }

static uint64_t cap(void)
{
    return (uint64_t)MQES | 1ull << 16 /* CQR */ | 20ull << 24 /* TO: 10 s */ |
           1ull << 37 /* NVM command set */;
}

/* ---- PRPs ------------------------------------------------------------------ */

typedef struct seg { uint64_t addr, len; } seg;
#define MAX_SEGS ((1u << MDTS) + 1)

/* The memory segments of a `len`-byte transfer described by PRP1/PRP2. */
static uint16_t prp_segs(nvme *c, uint64_t prp1, uint64_t prp2, uint64_t len, seg *s,
                         unsigned *n)
{
    uint64_t ps = page_size(c);
    if (len > (uint64_t)ps << MDTS)
        return SC_INVALID_FIELD;
    *n = 0;
    uint64_t first = ps - (prp1 & (ps - 1));
    if (first > len) first = len;
    s[(*n)++] = (seg){ prp1, first };
    uint64_t rem = len - first;
    if (!rem)
        return SC_OK;
    if (rem <= ps) {
        s[(*n)++] = (seg){ prp2, rem };
        return SC_OK;
    }
    uint64_t list = prp2;
    while (rem) {
        uint8_t e[8];
        if (uam_pci_dma_read(&c->fn, list, e, 8) != UAM_OK)
            return SC_DATA_XFER;
        uint64_t a = le_get(e, 8);
        if ((list & (ps - 1)) == ps - 8 && rem > ps) {   /* last slot: the next list page */
            list = a;
            continue;
        }
        if (*n >= MAX_SEGS)
            return SC_INVALID_FIELD;
        uint64_t l = rem < ps ? rem : ps;
        s[(*n)++] = (seg){ a, l };
        rem -= l;
        list += 8;
    }
    return SC_OK;
}

/* Copy between `buf` and the segments, starting `skip` bytes into them. */
static bool seg_copy(nvme *c, const seg *s, unsigned n, uint64_t skip, uint8_t *buf, uint64_t len,
                     bool to_mem)
{
    for (unsigned i = 0; i < n && len; i++) {
        if (skip >= s[i].len) {
            skip -= s[i].len;
            continue;
        }
        uint64_t k = s[i].len - skip;
        if (k > len) k = len;
        uam_status st = to_mem ? uam_pci_dma_write(&c->fn, s[i].addr + skip, buf, k)
                               : uam_pci_dma_read(&c->fn, s[i].addr + skip, buf, k);
        if (st != UAM_OK)
            return false;
        buf += k;
        len -= k;
        skip = 0;
    }
    return len == 0;
}

/* Move a small buffer (Identify, log pages, features) to the host. */
static uint16_t reply(nvme *c, const uint8_t *sqe, const void *data, uint64_t len)
{
    seg s[MAX_SEGS];
    unsigned n;
    uint16_t st = prp_segs(c, le_get(sqe + 24, 8), le_get(sqe + 32, 8), len, s, &n);
    if (st != SC_OK)
        return st;
    return seg_copy(c, s, n, 0, (uint8_t *)data, len, true) ? SC_OK : SC_DATA_XFER;
}

/* ---- interrupts and completions ------------------------------------------- */

static bool cq_pending(const nq *q)
{
    return q->valid && q->ien && q->head != q->tail;
}

/* INTx: asserted while an interrupt-enabled CQ holds entries the host has not
 * consumed, unless masked in INTMS (vector 0's bit gates INTx). */
static void update_intx(nvme *c)
{
    bool lv = false;
    if (!uam_pci_msi_enabled(&c->fn) && !(c->intms & 1))
        for (unsigned i = 0; i < NQ && !lv; i++)
            lv = cq_pending(&c->cq[i]);
    uam_pci_set_intx(&c->fn, lv);
}

static void post(nvme *c, unsigned cqid, uint32_t dw0, unsigned sqid, uint16_t cid, uint16_t status)
{
    nq *q = &c->cq[cqid];
    if (!q->valid)
        return;
    uint8_t e[16] = { 0 };
    le_put(e, 4, dw0);
    le_put(e + 8, 2, c->sq[sqid].head);
    le_put(e + 10, 2, sqid);
    le_put(e + 12, 2, cid);
    le_put(e + 14, 2, (uint16_t)(status << 1) | (q->phase ? 1 : 0));
    (void)uam_pci_dma_write(&c->fn, q->base + q->tail * 16ull, e, 16);
    if (++q->tail == q->size) {
        q->tail = 0;
        q->phase = !q->phase;
    }
    if (!q->ien)
        return;
    if (uam_pci_msi_enabled(&c->fn)) {
        if (!(c->intms >> (q->iv & 31) & 1) || q->iv >= 32)
            uam_pci_msi_notify(&c->fn, q->iv);
    } else {
        update_intx(c);
    }
}

/* ---- queues ------------------------------------------------------------- */

static void reset_queues(nvme *c)
{
    memset(c->sq, 0, sizeof(c->sq));
    memset(c->cq, 0, sizeof(c->cq));
}

static void enable(nvme *c)
{
    reset_queues(c);
    unsigned sqs = (c->aqa & 0xFFF) + 1, cqs = ((c->aqa >> 16) & 0xFFF) + 1;
    if (sqs < 2 || cqs < 2 || (c->asq & 0xFFF) || (c->acq & 0xFFF)) {
        c->csts |= 1u << 1;                    /* controller fatal status */
        return;
    }
    c->cq[0] = (nq){ .base = c->acq, .size = (uint16_t)cqs, .valid = true, .ien = true,
                     .phase = true, .iv = 0 };
    c->sq[0] = (nq){ .base = c->asq, .size = (uint16_t)sqs, .valid = true, .cqid = 0 };
    c->csts = CSTS_RDY;
}

/* ---- admin commands ------------------------------------------------------- */

static void id_str(uint8_t *p, unsigned n, const char *s)
{
    memset(p, ' ', n);
    size_t l = strlen(s);
    memcpy(p, s, l < n ? l : n);
}

static uint16_t identify(nvme *c, const uint8_t *sqe)
{
    uint8_t d[4096];
    memset(d, 0, sizeof(d));
    unsigned cns = sqe[40], nsid = (unsigned)le_get(sqe + 4, 4);
    switch (cns) {
    case 0x00: {                                    /* namespace */
        if (nsid == 0 || nsid > UAM_NVME_MAX_NS)
            return SC_INVALID_NS;
        uam_blockdev *bd = c->ns[nsid - 1];
        if (bd) {
            le_put(d, 8, bd->sectors);              /* NSZE, NCAP, NUSE */
            le_put(d + 8, 8, bd->sectors);
            le_put(d + 16, 8, bd->sectors);
            d[25] = 0;                              /* NLBAF: one format */
            d[26] = 0;                              /* FLBAS: format 0 */
            le_put(d + 128, 4, 9u << 16);           /* LBAF0: 512-byte blocks */
        }
        break;
    }
    case 0x01: {                                    /* controller */
        le_put(d, 2, uam_pci_get(c->fn.cfg, UAM_PCI_VENDOR, 2));
        le_put(d + 2, 2, uam_pci_get(c->fn.cfg, UAM_PCI_SUBSYS_VENDOR, 2));
        id_str(d + 4, 20, c->serial);
        id_str(d + 24, 40, "UAM NVMe controller");
        id_str(d + 64, 8, "1.0");
        d[72] = 6;                                  /* RAB */
        d[77] = MDTS;
        le_put(d + 80, 4, 0x00010400);              /* VER 1.4 */
        d[258] = 3;                                 /* ACL */
        d[259] = 3;                                 /* AERL */
        d[260] = 0x02;                              /* FRMW: one slot */
        d[261] = 0x02;                              /* LPA: command effects log */
        le_put(d + 266, 2, 0x157);                  /* WCTEMP */
        le_put(d + 268, 2, 0x175);                  /* CCTEMP */
        d[512] = 0x66;                              /* SQES */
        d[513] = 0x44;                              /* CQES */
        le_put(d + 516, 4, UAM_NVME_MAX_NS);        /* NN */
        le_put(d + 520, 2, (1u << 2) | (1u << 3));  /* ONCS: DSM, Write Zeroes */
        d[525] = 1;                                 /* VWC */
        snprintf((char *)d + 768, 256, "nqn.2026-01.org.uam:nvme:%s", c->serial);
        break;
    }
    case 0x02: {                                    /* active namespace list */
        unsigned k = 0;
        for (unsigned i = nsid; i < UAM_NVME_MAX_NS; i++)
            if (c->ns[i])
                le_put(d + 4 * k++, 4, i + 1);
        break;
    }
    case 0x03:                                      /* namespace descriptors: none */
        if (nsid == 0 || nsid > UAM_NVME_MAX_NS || !c->ns[nsid - 1])
            return SC_INVALID_NS;
        break;
    default:
        return SC_INVALID_FIELD;
    }
    return reply(c, sqe, d, sizeof(d));
}

static uint16_t admin(nvme *c, const uint8_t *sqe, uint32_t *dw0, bool *hold)
{
    unsigned op = sqe[0];
    uint32_t cdw10 = (uint32_t)le_get(sqe + 40, 4), cdw11 = (uint32_t)le_get(sqe + 44, 4);
    uint64_t prp1 = le_get(sqe + 24, 8);
    unsigned qid = cdw10 & 0xFFFF, qsize = (cdw10 >> 16) + 1;
    switch (op) {
    case 0x06:
        return identify(c, sqe);
    case 0x05: {                                    /* create I/O completion queue */
        if (qid == 0 || qid >= NQ || c->cq[qid].valid)
            return SC_QID_INVALID;
        if (qsize < 2 || qsize > MQES + 1)
            return SC_QSIZE_INVALID;
        if (!(cdw11 & 1))
            return SC_INVALID_FIELD;                /* only physically contiguous queues */
        unsigned iv = cdw11 >> 16;
        if (iv >= NQ)
            return SC_VECTOR_INVALID;
        c->cq[qid] = (nq){ .base = prp1 & ~0xFFFull, .size = (uint16_t)qsize, .valid = true,
                           .ien = (cdw11 >> 1) & 1, .iv = (uint16_t)iv, .phase = true };
        return SC_OK;
    }
    case 0x01: {                                    /* create I/O submission queue */
        unsigned cqid = cdw11 >> 16;
        if (qid == 0 || qid >= NQ || c->sq[qid].valid)
            return SC_QID_INVALID;
        if (qsize < 2 || qsize > MQES + 1)
            return SC_QSIZE_INVALID;
        if (!(cdw11 & 1))
            return SC_INVALID_FIELD;
        if (cqid == 0 || cqid >= NQ || !c->cq[cqid].valid)
            return SC_CQ_INVALID;
        c->sq[qid] = (nq){ .base = prp1 & ~0xFFFull, .size = (uint16_t)qsize, .valid = true,
                           .cqid = (uint16_t)cqid };
        return SC_OK;
    }
    case 0x00:                                      /* delete I/O submission queue */
        if (qid == 0 || qid >= NQ || !c->sq[qid].valid)
            return SC_QID_INVALID;
        c->sq[qid].valid = false;
        return SC_OK;
    case 0x04:                                      /* delete I/O completion queue */
        if (qid == 0 || qid >= NQ || !c->cq[qid].valid)
            return SC_QID_INVALID;
        for (unsigned i = 1; i < NQ; i++)
            if (c->sq[i].valid && c->sq[i].cqid == qid)
                return SC_DELETE_INVALID;
        c->cq[qid].valid = false;
        update_intx(c);
        return SC_OK;
    case 0x09: case 0x0A: {                         /* set / get features */
        unsigned fid = cdw10 & 0xFF;
        if (fid == 0x07) {                          /* number of queues (0-based) */
            *dw0 = (UAM_NVME_MAX_QUEUES - 1) << 16 | (UAM_NVME_MAX_QUEUES - 1);
            return SC_OK;
        }
        if (fid == 0x06) {                          /* volatile write cache */
            if (op == 0x09) c->wce = cdw11 & 1;
            *dw0 = c->wce;
            return SC_OK;
        }
        if (fid == 0 || fid > 0x7F)
            return SC_INVALID_FIELD;
        *dw0 = 0;                                   /* others: accepted, read as 0 */
        return SC_OK;
    }
    case 0x02: {                                    /* get log page: zeroes */
        uint64_t numd = ((cdw10 >> 16) | ((uint64_t)(cdw11 & 0xFFFF) << 16)) + 1;
        uint64_t len = numd * 4;
        if (len > 4096)
            len = 4096;
        uint8_t z[4096];
        memset(z, 0, sizeof(z));
        return reply(c, sqe, z, len);
    }
    case 0x0C:                                      /* asynchronous event request */
        *hold = true;
        return SC_OK;
    case 0x08:                                      /* abort: nothing is outstanding */
        *dw0 = 1;
        return SC_OK;
    default:
        return SC_INVALID_OPCODE;
    }
}

/* ---- NVM commands --------------------------------------------------------- */

static uint16_t io(nvme *c, const uint8_t *sqe)
{
    unsigned op = sqe[0], nsid = (unsigned)le_get(sqe + 4, 4);
    if (nsid == 0 || nsid > UAM_NVME_MAX_NS || !c->ns[nsid - 1])
        return SC_INVALID_NS;
    uam_blockdev *bd = c->ns[nsid - 1];
    uint64_t slba = le_get(sqe + 40, 8);
    uint32_t nlb = (uint32_t)(le_get(sqe + 48, 4) & 0xFFFF) + 1;
    switch (op) {
    case 0x00:                                      /* flush */
        return uam_blockdev_flush(bd) == UAM_OK ? SC_OK : SC_DATA_XFER;
    case 0x09:                                      /* dataset management */
        return SC_OK;
    case 0x01: case 0x02: case 0x08: {              /* write, read, write zeroes */
        if (slba > bd->sectors || nlb > bd->sectors - slba)
            return SC_LBA_RANGE;
        const uint32_t ss = bd->sector_size;
        if (op == 0x08) {
            memset(c->buf, 0, (size_t)ss * nlb < (1u << 22) ? (size_t)ss * nlb : (1u << 22));
            for (uint32_t done = 0; done < nlb;) {
                uint32_t k = nlb - done < (1u << 22) / ss ? nlb - done : (1u << 22) / ss;
                if (uam_blockdev_write(bd, slba + done, c->buf, k) != UAM_OK)
                    return SC_DATA_XFER;
                done += k;
            }
            return SC_OK;
        }
        seg s[MAX_SEGS];
        unsigned n;
        uint64_t len = (uint64_t)nlb * ss;
        uint16_t st = prp_segs(c, le_get(sqe + 24, 8), le_get(sqe + 32, 8), len, s, &n);
        if (st != SC_OK)
            return st;
        uint32_t per = (1u << 22) / ss;
        for (uint32_t done = 0; done < nlb;) {
            uint32_t k = nlb - done < per ? nlb - done : per;
            uint64_t off = (uint64_t)done * ss;
            if (op == 0x02) {
                if (uam_blockdev_read(bd, slba + done, c->buf, k) != UAM_OK ||
                    !seg_copy(c, s, n, off, c->buf, (uint64_t)k * ss, true))
                    return SC_DATA_XFER;
            } else {
                if (!seg_copy(c, s, n, off, c->buf, (uint64_t)k * ss, false) ||
                    uam_blockdev_write(bd, slba + done, c->buf, k) != UAM_OK)
                    return SC_DATA_XFER;
            }
            done += k;
        }
        return SC_OK;
    }
    default:
        return SC_INVALID_OPCODE;
    }
}

static void process(nvme *c, unsigned sqid)
{
    nq *q = &c->sq[sqid];
    while ((c->csts & CSTS_RDY) && q->valid && q->head != q->tail) {
        uint8_t sqe[64];
        if (uam_pci_dma_read(&c->fn, q->base + q->head * 64ull, sqe, 64) != UAM_OK) {
            c->csts |= 1u << 1;                     /* fatal: the queue is unreadable */
            return;
        }
        if (++q->head == q->size)
            q->head = 0;
        uint16_t cid = (uint16_t)le_get(sqe + 2, 2);
        uint32_t dw0 = 0;
        bool hold = false;
        uint16_t st = sqid == 0 ? admin(c, sqe, &dw0, &hold) : io(c, sqe);
        if (!hold)
            post(c, q->cqid, dw0, sqid, cid, st);
    }
}

/* ---- registers ------------------------------------------------------------ */

static uint64_t reg_read64(nvme *c, uint64_t off)
{
    switch (off) {
    case R_CAP:   return cap();
    case R_VS:    return 0x00010400u;
    case R_INTMS: case R_INTMC: return c->intms;
    case R_CC:    return c->cc;
    case R_CSTS:  return c->csts;
    case R_AQA:   return c->aqa;
    case R_ASQ:   return c->asq;
    case R_ACQ:   return c->acq;
    default:      return 0;
    }
}

static uam_status nvme_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    nvme *c = from_dev(d);
    if (uam_pci_msix_access(&c->fn, 0, off, size, false, val))
        return UAM_OK;
    if (off >= R_DBS) {
        *val = 0;                                   /* doorbells read as 0 */
        return UAM_OK;
    }
    /* 64-bit registers may be read as two halves */
    uint64_t base = off & ~7ull;
    bool wide = base == R_CAP || base == R_ASQ || base == R_ACQ;
    uint64_t v = wide ? reg_read64(c, base) >> (8 * (off - base)) : reg_read64(c, off & ~3ull);
    *val = size == 8 ? v : (uint32_t)v;
    return UAM_OK;
}

static void set_cc(nvme *c, uint32_t v)
{
    uint32_t old = c->cc;
    c->cc = v & 0x00FFC7F1u;
    if ((c->cc & CC_EN) && !(old & CC_EN)) {
        enable(c);
    } else if (!(c->cc & CC_EN) && (old & CC_EN)) {
        reset_queues(c);
        c->csts = 0;
        update_intx(c);
    }
    if ((c->cc >> 14) & 3) {                        /* shutdown notification: done at once */
        for (unsigned i = 0; i < UAM_NVME_MAX_NS; i++)
            if (c->ns[i])
                (void)uam_blockdev_flush(c->ns[i]);
        c->csts = (c->csts & ~(3u << 2)) | (2u << 2);
    } else {
        c->csts &= ~(3u << 2);
    }
}

static uam_status nvme_write(uam_device *d, uint64_t off, unsigned size, uint64_t v)
{
    nvme *c = from_dev(d);
    if (uam_pci_msix_access(&c->fn, 0, off, size, true, &v))
        return UAM_OK;
    if (off >= R_DBS) {
        unsigned idx = (unsigned)((off - R_DBS) / 4), qid = idx / 2;
        if (qid >= NQ)
            return UAM_OK;
        if (idx & 1) {                              /* completion queue head */
            nq *q = &c->cq[qid];
            if (q->valid && v < q->size)
                q->head = (uint16_t)v;
            update_intx(c);
        } else {                                    /* submission queue tail */
            nq *q = &c->sq[qid];
            if (q->valid && v < q->size) {
                q->tail = (uint16_t)v;
                process(c, qid);
            }
        }
        return UAM_OK;
    }
    switch (off) {
    case R_INTMS: c->intms |= (uint32_t)v; update_intx(c); break;
    case R_INTMC: c->intms &= ~(uint32_t)v; update_intx(c); break;
    case R_CC:    set_cc(c, (uint32_t)v); break;
    case R_AQA:   c->aqa = (uint32_t)v & 0x0FFF0FFFu; break;
    case R_ASQ:   c->asq = size == 8 ? v : (c->asq & ~0xFFFFFFFFull) | (uint32_t)v; break;
    case R_ASQ + 4: c->asq = (c->asq & 0xFFFFFFFFu) | v << 32; break;
    case R_ACQ:   c->acq = size == 8 ? v : (c->acq & ~0xFFFFFFFFull) | (uint32_t)v; break;
    case R_ACQ + 4: c->acq = (c->acq & 0xFFFFFFFFu) | v << 32; break;
    case R_NSSR:  break;                            /* subsystem reset: not supported */
    default: break;
    }
    return UAM_OK;
}

/* ---- device ----------------------------------------------------------------- */

static void nvme_reset(uam_device *d)
{
    nvme *c = from_dev(d);
    c->cc = c->csts = c->aqa = c->intms = 0;
    c->asq = c->acq = 0;
    c->wce = false;
    reset_queues(c);
}

static void nvme_destroy(uam_device *d)
{
    nvme *c = from_dev(d);
    free(c->buf);
    free(c);
}

static void put_q(uam_state_writer *w, const nq *q)
{
    uam_state_put_u64(w, q->base);
    uam_state_put_u32(w, (uint32_t)q->size | (uint32_t)q->head << 16);
    uam_state_put_u32(w, (uint32_t)q->tail | (uint32_t)q->cqid << 16);
    uam_state_put_u32(w, (uint32_t)q->iv | (uint32_t)q->valid << 16 | (uint32_t)q->ien << 17 |
                         (uint32_t)q->phase << 18);
}

static void get_q(uam_state_reader *r, nq *q)
{
    q->base = uam_state_get_u64(r);
    uint32_t a = uam_state_get_u32(r), b = uam_state_get_u32(r), e = uam_state_get_u32(r);
    q->size = (uint16_t)a; q->head = (uint16_t)(a >> 16);
    q->tail = (uint16_t)b; q->cqid = (uint16_t)(b >> 16);
    q->iv = (uint16_t)e; q->valid = e >> 16 & 1; q->ien = e >> 17 & 1; q->phase = e >> 18 & 1;
}

static void nvme_save(uam_device *d, uam_state_writer *w)
{
    nvme *c = from_dev(d);
    uam_state_put_u32(w, c->cc);
    uam_state_put_u32(w, c->csts);
    uam_state_put_u32(w, c->aqa);
    uam_state_put_u32(w, c->intms);
    uam_state_put_u64(w, c->asq);
    uam_state_put_u64(w, c->acq);
    uam_state_put_bool(w, c->wce);
    for (unsigned i = 0; i < NQ; i++) {
        put_q(w, &c->sq[i]);
        put_q(w, &c->cq[i]);
    }
}

static void nvme_restore(uam_device *d, uam_state_reader *r)
{
    nvme *c = from_dev(d);
    c->cc = uam_state_get_u32(r);
    c->csts = uam_state_get_u32(r);
    c->aqa = uam_state_get_u32(r);
    c->intms = uam_state_get_u32(r);
    c->asq = uam_state_get_u64(r);
    c->acq = uam_state_get_u64(r);
    c->wce = uam_state_get_bool(r);
    for (unsigned i = 0; i < NQ; i++) {
        get_q(r, &c->sq[i]);
        get_q(r, &c->cq[i]);
    }
}

static const uam_device_ops nvme_ops = {
    .type = "nvme", .impl_min = 4, .impl_max = 8,
    .read = nvme_read, .write = nvme_write, .reset = nvme_reset, .destroy = nvme_destroy,
    .save = nvme_save, .restore = nvme_restore,
};

uam_device *uam_nvme_create(const char *name, uint16_t vendor, uint16_t device, uint8_t revision)
{
    nvme *c = calloc(1, sizeof(*c));
    if (!c || !(c->buf = malloc(1u << 22))) {
        free(c);
        return NULL;
    }
    uam_device_init(&c->dev, &nvme_ops, name);
    snprintf(c->serial, sizeof(c->serial), "UAM-%.16s", name ? name : "nvme");
    uam_pci_func *f = &c->fn;
    uam_pci_func_init(f, name, vendor, device, 0x01080200u | revision, NULL, c);
    uam_pci_func_bar(f, 0, BAR_SIZE, UAM_PCI_BAR_MEM | UAM_PCI_BAR_64, &c->dev);
    f->cfg[UAM_PCI_INT_PIN] = 1;
    unsigned pm = uam_pci_add_cap(f, UAM_PCI_CAP_PM, 8);
    uam_pci_set(f->cfg, pm + 2, 2, 0x0003);
    uam_pci_set(f->wmask, pm + 4, 2, 0x0003);
    uam_pci_msi_init(f, 0, true, true);
    uam_pci_msix_init(f, NQ, 0, MSIX_TABLE, 0, MSIX_PBA);
    uam_pci_add_pcie_cap(f, UAM_PCIE_ENDPOINT, 0);
    nvme_reset(&c->dev);
    return &c->dev;
}

uam_pci_func *uam_nvme_func(uam_device *d) { return &from_dev(d)->fn; }

uam_status uam_nvme_attach(uam_device *d, unsigned nsid, uam_blockdev *bd)
{
    nvme *c = from_dev(d);
    if (nsid == 0 || nsid > UAM_NVME_MAX_NS || c->ns[nsid - 1] || !bd || bd->sector_size != 512)
        return UAM_ERR_INVALID;
    c->ns[nsid - 1] = bd;
    return UAM_OK;
}
