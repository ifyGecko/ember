/*
 * ide.c - Parallel-ATA (IDE) controller with bus-master DMA (uam/dev/ide.h).
 *
 * The task-file protocol follows ATA/ATAPI-6 (PIO data-in/out per DRQ block,
 * PACKET with the byte-count limit, the HOB bytes of 48-bit commands) and the
 * bus-master interface the Programming Interface for Bus Master IDE
 * Controller (PRD tables). The drive side is ata_core.c, shared with AHCI.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uam/dev/ide.h"
#include "ata_core.h"

#define PIO_MAX  (128u << 10)   /* PIO data a command (an ATAPI reply) may hold */

#define ST_ERR  ATA_ST_ERR
#define ST_DRQ  ATA_ST_DRQ
#define ST_DSC  ATA_ST_DSC
#define ST_DRDY ATA_ST_DRDY

#define CTL_NIEN 0x02u
#define CTL_SRST 0x04u
#define CTL_HOB  0x80u

#define BM_START 0x01u          /* command: start/stop */
#define BM_ACTIVE 0x01u         /* status */
#define BM_ERR   0x02u
#define BM_INT   0x04u

enum phase { PH_NONE, PH_DATA_IN, PH_WRITE, PH_CDB };

typedef struct ide ide;

typedef struct ide_ch {
    ide       *h;
    unsigned   idx;
    ata_drive  drv[2];
    uint8_t    heads[2], spt[2], mult[2];      /* CHS geometry, sectors per multiple block */
    /* the task file (both drives see the writes), with the previous bytes (HOB) */
    uint8_t    feat, count, lba[3], hob_feat, hob_count, hob_lba[3];
    uint8_t    device, status, error, ctl;
    bool       intrq;
    uam_signal irq;
    /* PIO data phase */
    uint8_t    phase;
    uint8_t   *buf;
    uint32_t   pos, len;
    uint64_t   next_lba;                        /* READ/WRITE SECTORS: what comes next */
    uint32_t   remaining;
    uint8_t    block;                           /* sectors per DRQ block */
    /* ATAPI PIO: the reply in buf, handed out in byte-count-limited pieces */
    bool       packet;
    uint32_t   total;
    uint8_t    pkt_status, pkt_error;
    /* a DMA command waiting for the bus master */
    bool       dma_pending, dma_atapi;
    ata_taskfile dma_tf;
    uint8_t    cdb[12];
    /* bus master */
    uint8_t    bm_cmd, bm_status;
    uint32_t   bm_prd;
} ide_ch;

/* A channel's command or control block. */
typedef struct ide_view {
    uam_device dev;
    ide_ch    *c;
} ide_view;

struct ide {
    uam_device   dev;            /* bus-master registers; owns the state */
    ide_view     cmdv[2], ctlv[2];
    uam_pci_func fn;
    ide_ch       ch[2];
    uint8_t     *bounce;
};

static ide *from_bm(uam_device *d) { return UAM_CONTAINER_OF(d, ide, dev); }
static ide_ch *from_cmd(uam_device *d) { return UAM_CONTAINER_OF(d, ide_view, dev)->c; }
static ide_ch *from_ctl(uam_device *d) { return UAM_CONTAINER_OF(d, ide_view, dev)->c; }

static unsigned sel(const ide_ch *c) { return (c->device >> 4) & 1; }
static ata_drive *cur(ide_ch *c) { return &c->drv[sel(c)]; }

static void update_irq(ide_ch *c)
{
    uam_signal_set(&c->irq, c->intrq && !(c->ctl & CTL_NIEN));
}

static void raise(ide_ch *c)
{
    c->intrq = true;
    c->bm_status |= BM_INT;
    update_irq(c);
}

/* The signature a drive shows after a reset or EXECUTE DEVICE DIAGNOSTIC. */
static void signature(ide_ch *c)
{
    ata_drive *d = cur(c);
    c->count = 1;
    c->lba[0] = 1;
    c->lba[1] = d->atapi ? 0x14 : 0;
    c->lba[2] = d->atapi ? 0xEB : 0;
    c->error = 0x01;
    c->status = d->bd && !d->atapi ? (ST_DRDY | ST_DSC) : 0;
}

static void finish(ide_ch *c, uint8_t status, uint8_t error)
{
    c->phase = PH_NONE;
    c->status = status;
    c->error = error;
    raise(c);
}

static void abort_cmd(ide_ch *c)
{
    if (cur(c)->atapi) {   /* an ATA command on a packet device shows its signature */
        signature(c);
        c->error = ATA_ERR_ABRT;
    }
    finish(c, ST_DRDY | ST_ERR, ATA_ERR_ABRT);
}

static void channel_reset(ide_ch *c)
{
    c->phase = PH_NONE;
    c->dma_pending = false;
    c->packet = false;
    c->device = 0;
    for (unsigned i = 0; i < 2; i++) {
        ata_drive_reset(&c->drv[i]);
        c->heads[i] = 16;
        c->spt[i] = 63;
        c->mult[i] = 0;
    }
    signature(c);
    c->intrq = false;
    update_irq(c);
}

/* ---- addresses ---------------------------------------------------------- */

/* The command's starting sector: LBA (28- or 48-bit) or CHS. */
static uint64_t start_lba(const ide_ch *c, bool ext)
{
    uint64_t lo = c->lba[0] | (uint64_t)c->lba[1] << 8 | (uint64_t)c->lba[2] << 16;
    if (ext)
        return lo | (uint64_t)c->hob_lba[0] << 24 | (uint64_t)c->hob_lba[1] << 32 |
               (uint64_t)c->hob_lba[2] << 40;
    if (c->device & 0x40)
        return lo | (uint64_t)(c->device & 0x0F) << 24;
    unsigned d = sel(c);
    uint64_t cyl = c->lba[1] | (uint64_t)c->lba[2] << 8, head = c->device & 0x0F, s = c->lba[0];
    return (cyl * c->heads[d] + head) * c->spt[d] + (s ? s - 1 : 0);
}

static uint32_t sector_count(const ide_ch *c, bool ext)
{
    if (ext) {
        uint32_t n = c->count | (uint32_t)c->hob_count << 8;
        return n ? n : 65536;
    }
    return c->count ? c->count : 256;
}

/* ---- PIO sector commands ----------------------------------------------- */

static void load_read_block(ide_ch *c)
{
    ata_drive *d = cur(c);
    uint32_t n = c->remaining < c->block ? c->remaining : c->block;
    if (uam_blockdev_read(d->bd, c->next_lba, c->buf, n) != UAM_OK) {
        finish(c, ST_DRDY | ST_ERR, 0x40);   /* uncorrectable */
        return;
    }
    c->next_lba += n;
    c->remaining -= n;
    c->pos = 0;
    c->len = n * 512;
    c->phase = PH_DATA_IN;
    c->status = ST_DRDY | ST_DSC | ST_DRQ;
    raise(c);
}

static void start_write_block(ide_ch *c, bool irq)
{
    uint32_t n = c->remaining < c->block ? c->remaining : c->block;
    c->pos = 0;
    c->len = n * 512;
    c->phase = PH_WRITE;
    c->status = ST_DRDY | ST_DSC | ST_DRQ;
    if (irq)
        raise(c);
}

static void pio_sectors(ide_ch *c, bool write, bool ext, bool multiple)
{
    ata_drive *d = cur(c);
    uint64_t lba = start_lba(c, ext);
    uint32_t n = sector_count(c, ext);
    if (d->atapi || (multiple && !c->mult[sel(c)])) {
        abort_cmd(c);
        return;
    }
    if (lba > d->bd->sectors || n > d->bd->sectors - lba) {
        finish(c, ST_DRDY | ST_ERR, 0x10);   /* ID not found */
        return;
    }
    c->next_lba = lba;
    c->remaining = n;
    c->block = multiple ? c->mult[sel(c)] : 1;
    if (write)
        start_write_block(c, false);   /* the first block is asked for without an interrupt */
    else
        load_read_block(c);
}

/* ---- ATAPI -------------------------------------------------------------- */

static uint32_t pio_copy(void *ctx, uint8_t *buf, uint32_t len, bool to_host)
{
    ide_ch *c = ctx;
    if (!to_host || c->total >= PIO_MAX)
        return 0;
    if (len > PIO_MAX - c->total)
        len = PIO_MAX - c->total;
    memcpy(c->buf + c->total, buf, len);
    c->total += len;
    return len;
}

/* Hand the host the next piece of an ATAPI PIO reply, or end the command. */
static void packet_next(ide_ch *c)
{
    if (c->pos >= c->total) {
        c->packet = false;
        c->count = 0x03;                      /* I/O, C/D: complete */
        finish(c, c->pkt_status, c->pkt_error);
        return;
    }
    uint32_t limit = c->lba[1] | (uint32_t)c->lba[2] << 8;
    if (limit == 0 || limit == 0xFFFF)
        limit = 0xFFFE;
    limit &= ~1u;
    uint32_t n = c->total - c->pos < limit ? c->total - c->pos : limit;
    c->len = c->pos + n;
    c->lba[1] = (uint8_t)n;
    c->lba[2] = (uint8_t)(n >> 8);
    c->count = 0x02;                          /* I/O: data to the host */
    c->phase = PH_DATA_IN;
    c->status = ST_DRDY | ST_DSC | ST_DRQ;
    raise(c);
}

static void run_dma(ide_ch *c);

static void packet(ide_ch *c)
{
    ata_drive *d = cur(c);
    if (c->feat & 1) {                        /* DMA: when the bus master starts */
        c->dma_pending = true;
        c->dma_atapi = true;
        c->phase = PH_NONE;
        c->status = ST_DRDY | ST_DSC;
        run_dma(c);
        return;
    }
    ata_io io = { .ctx = c, .copy = pio_copy, .bounce = c->h->bounce };
    ata_result r = { 0 };
    c->total = 0;
    atapi_exec(d, c->cdb, &io, &r);
    c->pkt_status = r.status;
    c->pkt_error = r.error;
    c->pos = 0;
    c->packet = true;
    if (r.status & ST_ERR)
        c->total = 0;
    packet_next(c);
}

/* ---- bus-master DMA ------------------------------------------------------ */

typedef struct prd_walk {
    ide_ch  *c;
    uint32_t done;
    bool     ok;
} prd_walk;

static uint32_t prd_copy(void *ctx, uint8_t *buf, uint32_t len, bool to_host)
{
    prd_walk *w = ctx;
    ide *h = w->c->h;
    uint64_t skip = w->done;
    uint32_t moved = 0;
    for (uint32_t at = w->c->bm_prd, i = 0; i < 8192 && moved < len; i++, at += 8) {
        uint8_t e[8];
        if (uam_pci_dma_read(&h->fn, at, e, 8) != UAM_OK)
            break;
        uint32_t addr = (uint32_t)(e[0] | e[1] << 8 | e[2] << 16 | (uint32_t)e[3] << 24) & ~1u;
        uint32_t cnt = (uint32_t)(e[4] | e[5] << 8);
        if (!cnt)
            cnt = 65536;
        bool eot = e[7] & 0x80;
        if (skip >= cnt) {
            skip -= cnt;
        } else {
            uint32_t n = cnt - (uint32_t)skip;
            if (n > len - moved)
                n = len - moved;
            uam_status st = to_host ? uam_pci_dma_write(&h->fn, addr + skip, buf + moved, n)
                                    : uam_pci_dma_read(&h->fn, addr + skip, buf + moved, n);
            if (st != UAM_OK)
                break;
            moved += n;
            skip = 0;
        }
        if (eot)
            break;
    }
    w->done += moved;
    if (moved < len)
        w->ok = false;
    return moved;
}

static void run_dma(ide_ch *c)
{
    if (!c->dma_pending || !(c->bm_cmd & BM_START))
        return;
    c->dma_pending = false;
    ata_drive *d = cur(c);
    prd_walk w = { c, 0, true };
    ata_io io = { .ctx = &w, .copy = prd_copy, .bounce = c->h->bounce };
    ata_result r = { 0 };
    if (c->dma_atapi)
        atapi_exec(d, c->cdb, &io, &r);
    else
        ata_exec(d, &c->dma_tf, &io, &r);
    c->bm_status &= (uint8_t)~BM_ACTIVE;
    if (!w.ok && !(r.status & ST_ERR))
        c->bm_status |= BM_ERR;
    if (c->dma_atapi)
        c->count = 0x03;
    finish(c, r.status, r.error);
}

static void dma_command(ide_ch *c, uint8_t cmd, bool ext)
{
    ata_drive *d = cur(c);
    if (d->atapi) {
        abort_cmd(c);
        return;
    }
    uint64_t lba = start_lba(c, ext);
    c->dma_tf = (ata_taskfile){ .command = cmd, .count = ext ? (uint16_t)(c->count | c->hob_count << 8) : c->count,
                                .lba = lba, .device = (uint8_t)(0x40 | ((lba >> 24) & 0x0F)) };
    c->dma_pending = true;
    c->dma_atapi = false;
    c->phase = PH_NONE;
    c->status = ST_DRDY | ST_DSC | ST_DRQ;
    run_dma(c);
}

/* ---- commands ------------------------------------------------------------ */

static uint32_t no_data(void *ctx, uint8_t *buf, uint32_t len, bool to_host)
{
    (void)ctx; (void)buf; (void)len; (void)to_host;
    return 0;
}

static void command(ide_ch *c, uint8_t cmd)
{
    ata_drive *d = cur(c);
    c->intrq = false;
    update_irq(c);
    c->phase = PH_NONE;
    c->packet = false;
    if (cmd == 0x90) {                            /* EXECUTE DEVICE DIAGNOSTIC */
        c->device &= (uint8_t)~0x10;
        signature(c);
        raise(c);
        return;
    }
    if (!d->bd)
        return;                                   /* nobody answers */
    switch (cmd) {
    case 0xEC: case 0xA1: {                       /* IDENTIFY (PACKET) DEVICE */
        if ((cmd == 0xA1) != d->atapi) {
            abort_cmd(c);
            return;
        }
        uint16_t w[256];
        ata_identify(d, w);
        if (!d->atapi) {   /* the current CHS translation and multiple setting */
            unsigned s = sel(c);
            uint64_t cyl = d->bd->sectors / ((uint64_t)c->heads[s] * c->spt[s]);
            w[54] = (uint16_t)(cyl > 16383 ? 16383 : cyl);
            w[55] = c->heads[s];
            w[56] = c->spt[s];
            uint64_t chs = (uint64_t)w[54] * w[55] * w[56];
            w[57] = (uint16_t)chs;
            w[58] = (uint16_t)(chs >> 16);
            w[59] = c->mult[s] ? (uint16_t)(0x100 | c->mult[s]) : 0;
            w[53] |= 0x0001;
        }
        for (unsigned i = 0; i < 256; i++) {
            c->buf[2 * i] = (uint8_t)w[i];
            c->buf[2 * i + 1] = (uint8_t)(w[i] >> 8);
        }
        c->pos = 0;
        c->len = 512;
        c->remaining = 0;
        c->phase = PH_DATA_IN;
        c->status = ST_DRDY | ST_DSC | ST_DRQ;
        raise(c);
        return;
    }
    case 0x20: case 0x21: pio_sectors(c, false, false, false); return;
    case 0x24:            pio_sectors(c, false, true, false); return;
    case 0xC4:            pio_sectors(c, false, false, true); return;
    case 0x29:            pio_sectors(c, false, true, true); return;
    case 0x30: case 0x31: pio_sectors(c, true, false, false); return;
    case 0x34:            pio_sectors(c, true, true, false); return;
    case 0xC5:            pio_sectors(c, true, false, true); return;
    case 0x39:            pio_sectors(c, true, true, true); return;
    case 0xC8: case 0xC9: case 0xCA: case 0xCB: dma_command(c, cmd, false); return;
    case 0x25: case 0x35:                       dma_command(c, cmd, true); return;
    case 0xC6:                                    /* SET MULTIPLE MODE */
        if (d->atapi || (c->count & (c->count - 1)) || c->count > 16) {
            abort_cmd(c);
            return;
        }
        c->mult[sel(c)] = c->count;
        finish(c, ST_DRDY | ST_DSC, 0);
        return;
    case 0x91:                                    /* INITIALIZE DEVICE PARAMETERS */
        if (d->atapi || !c->count) {
            abort_cmd(c);
            return;
        }
        c->heads[sel(c)] = (uint8_t)((c->device & 0x0F) + 1);
        c->spt[sel(c)] = c->count;
        finish(c, ST_DRDY | ST_DSC, 0);
        return;
    case 0xA0:                                    /* PACKET: the CDB comes next */
        if (!d->atapi) {
            abort_cmd(c);
            return;
        }
        c->pos = 0;
        c->len = 12;
        c->phase = PH_CDB;
        c->count = 0x01;                          /* C/D: the command packet */
        c->status = ST_DRDY | ST_DSC | ST_DRQ;
        return;
    default: {                                    /* the rest: no data */
        ata_taskfile tf = { .command = cmd, .device = c->device,
                            .features = (uint16_t)(c->feat | c->hob_feat << 8),
                            .count = (uint16_t)(c->count | c->hob_count << 8),
                            .lba = start_lba(c, true) };
        ata_io io = { .ctx = c, .copy = no_data, .bounce = c->h->bounce };
        ata_result r = { 0 };
        ata_exec(d, &tf, &io, &r);
        if (!(r.status & ST_ERR) && (cmd == 0xF8 || cmd == 0x27 || cmd == 0x08 || cmd == 0xE5)) {
            c->count = (uint8_t)r.count;
            for (unsigned i = 0; i < 3; i++) {
                c->lba[i] = (uint8_t)(r.lba >> (8 * i));
                c->hob_lba[i] = (uint8_t)(r.lba >> (24 + 8 * i));
            }
            if (cmd == 0xF8)
                c->device = (uint8_t)((c->device & 0xF0) | ((r.lba >> 24) & 0x0F));
        }
        finish(c, r.status, r.error);
        return;
    }
    }
}

/* ---- the data register -------------------------------------------------- */

static uint64_t data_read(ide_ch *c, unsigned size)
{
    if (c->phase != PH_DATA_IN)
        return 0xFFFFFFFFu & ((1ull << (8 * size)) - 1);
    uint64_t v = 0;
    for (unsigned i = 0; i < size && c->pos < c->len; i++)
        v |= (uint64_t)c->buf[c->pos++] << (8 * i);
    if (c->pos >= c->len) {
        if (c->packet) {
            packet_next(c);
        } else if (c->remaining) {
            load_read_block(c);
        } else {
            c->phase = PH_NONE;
            c->status = ST_DRDY | ST_DSC;
        }
    }
    return v;
}

static void data_write(ide_ch *c, unsigned size, uint64_t v)
{
    if (c->phase != PH_WRITE && c->phase != PH_CDB)
        return;
    for (unsigned i = 0; i < size && c->pos < c->len; i++) {
        uint8_t b = (uint8_t)(v >> (8 * i));
        if (c->phase == PH_CDB) c->cdb[c->pos++] = b;
        else                    c->buf[c->pos++] = b;
    }
    if (c->pos < c->len)
        return;
    if (c->phase == PH_CDB) {
        packet(c);
        return;
    }
    ata_drive *d = cur(c);
    uint32_t n = c->len / 512;
    if (uam_blockdev_write(d->bd, c->next_lba, c->buf, n) != UAM_OK) {
        finish(c, ST_DRDY | ST_ERR, 0x40);
        return;
    }
    c->next_lba += n;
    c->remaining -= n;
    if (c->remaining)
        start_write_block(c, true);
    else
        finish(c, ST_DRDY | ST_DSC, 0);
}

/* ---- register views --------------------------------------------------------- */

static bool any_drive(const ide_ch *c) { return c->drv[0].bd || c->drv[1].bd; }

static uam_status cmd_read(uam_device *dv, uint64_t off, unsigned size, uint64_t *val)
{
    ide_ch *c = from_cmd(dv);
    bool hob = (c->ctl & CTL_HOB) != 0;
    if (off == 0) {
        *val = data_read(c, size);
        return UAM_OK;
    }
    if (!any_drive(c)) {
        *val = 0xFF;                              /* nothing on the cable */
        return UAM_OK;
    }
    bool present = cur(c)->bd != NULL;
    switch (off) {
    case 1: *val = hob ? c->hob_feat : c->error; break;
    case 2: *val = hob ? c->hob_count : c->count; break;
    case 3: case 4: case 5: *val = hob ? c->hob_lba[off - 3] : c->lba[off - 3]; break;
    case 6: *val = c->device | 0xA0; break;
    case 7:
        *val = present ? c->status : 0;
        c->intrq = false;                         /* reading status acknowledges */
        update_irq(c);
        break;
    default: *val = 0xFF; break;
    }
    return UAM_OK;
}

static uam_status cmd_write(uam_device *dv, uint64_t off, unsigned size, uint64_t val)
{
    ide_ch *c = from_cmd(dv);
    uint8_t v = (uint8_t)val;
    if (off == 0) {
        data_write(c, size, val);
        return UAM_OK;
    }
    c->ctl &= (uint8_t)~CTL_HOB;                  /* a task-file write clears HOB */
    switch (off) {
    case 1: c->hob_feat = c->feat; c->feat = v; break;
    case 2: c->hob_count = c->count; c->count = v; break;
    case 3: case 4: case 5: c->hob_lba[off - 3] = c->lba[off - 3]; c->lba[off - 3] = v; break;
    case 6: c->device = v; break;
    case 7: command(c, v); break;
    default: break;
    }
    return UAM_OK;
}

static uam_status ctl_read(uam_device *dv, uint64_t off, unsigned size, uint64_t *val)
{
    (void)off; (void)size;
    ide_ch *c = from_ctl(dv);
    *val = !any_drive(c) ? 0xFF : cur(c)->bd ? c->status : 0;   /* alternate status */
    return UAM_OK;
}

static uam_status ctl_write(uam_device *dv, uint64_t off, unsigned size, uint64_t val)
{
    (void)off; (void)size;
    ide_ch *c = from_ctl(dv);
    uint8_t v = (uint8_t)val;
    bool srst_end = (c->ctl & CTL_SRST) && !(v & CTL_SRST);
    c->ctl = v;
    if (v & CTL_SRST) {
        c->phase = PH_NONE;
        c->status = 0x80;                         /* busy while held */
    } else if (srst_end) {
        channel_reset(c);
    }
    update_irq(c);
    return UAM_OK;
}

static uam_status bm_read(uam_device *dv, uint64_t off, unsigned size, uint64_t *val)
{
    ide *h = from_bm(dv);
    *val = 0;
    for (unsigned i = 0; i < size; i++) {
        uint64_t o = off + i;
        ide_ch *c = &h->ch[(o >> 3) & 1];
        unsigned r = (unsigned)o & 7;
        uint8_t b = r == 0 ? c->bm_cmd : r == 2 ? c->bm_status
                  : r >= 4 ? (uint8_t)(c->bm_prd >> (8 * (r - 4))) : 0;
        *val |= (uint64_t)b << (8 * i);
    }
    return UAM_OK;
}

static uam_status bm_write(uam_device *dv, uint64_t off, unsigned size, uint64_t val)
{
    ide *h = from_bm(dv);
    for (unsigned i = 0; i < size; i++) {
        uint64_t o = off + i;
        ide_ch *c = &h->ch[(o >> 3) & 1];
        unsigned r = (unsigned)o & 7;
        uint8_t b = (uint8_t)(val >> (8 * i));
        if (r == 0) {
            bool start = (b & BM_START) && !(c->bm_cmd & BM_START);
            c->bm_cmd = b & 0x09;
            if (start) {
                c->bm_status |= BM_ACTIVE;
                run_dma(c);
            } else if (!(b & BM_START)) {
                c->bm_status &= (uint8_t)~BM_ACTIVE;
            }
        } else if (r == 2) {
            c->bm_status = (uint8_t)((c->bm_status & ~0x60 & ~(b & (BM_ERR | BM_INT))) | (b & 0x60));
        } else if (r >= 4) {
            unsigned sh = 8 * (r - 4);
            c->bm_prd = (c->bm_prd & ~(0xFFu << sh)) | (uint32_t)b << sh;
            c->bm_prd &= ~3u;
        }
    }
    return UAM_OK;
}

/* ---- lifecycle ----------------------------------------------------------------- */

static void ide_reset(uam_device *dv)
{
    ide *h = from_bm(dv);
    for (unsigned i = 0; i < 2; i++) {
        ide_ch *c = &h->ch[i];
        c->ctl = 0;
        c->bm_cmd = 0;
        c->bm_status = 0x60;                      /* both drives DMA capable */
        c->bm_prd = 0;
        channel_reset(c);
    }
}

static void ide_destroy(uam_device *dv)
{
    ide *h = from_bm(dv);
    free(h->ch[0].buf);
    free(h->ch[1].buf);
    free(h->bounce);
    free(h);
}

static void cmdv_destroy(uam_device *dv) { (void)dv; }

static void ide_save(uam_device *dv, uam_state_writer *w)
{
    ide *h = from_bm(dv);
    for (unsigned i = 0; i < 2; i++) {
        ide_ch *c = &h->ch[i];
        for (unsigned k = 0; k < 2; k++) {
            ata_drive *d = &c->drv[k];
            const uint8_t b[] = { d->sense_key, d->asc, d->ascq, d->unit_attention,
                                  c->heads[k], c->spt[k], c->mult[k] };
            uam_state_put_bytes(w, b, sizeof(b));
        }
        const uint8_t r[] = { c->feat, c->count, c->lba[0], c->lba[1], c->lba[2], c->hob_feat,
                              c->hob_count, c->hob_lba[0], c->hob_lba[1], c->hob_lba[2],
                              c->device, c->status, c->error, c->ctl, c->intrq, c->phase,
                              c->block, c->packet, c->pkt_status, c->pkt_error, c->dma_pending,
                              c->dma_atapi, c->bm_cmd, c->bm_status };
        uam_state_put_bytes(w, r, sizeof(r));
        uam_state_put_u32(w, c->pos);
        uam_state_put_u32(w, c->len);
        uam_state_put_u32(w, c->total);
        uam_state_put_u32(w, c->remaining);
        uam_state_put_u64(w, c->next_lba);
        uam_state_put_u32(w, c->bm_prd);
        uam_state_put_bytes(w, c->cdb, sizeof(c->cdb));
        uam_state_put_u8(w, c->dma_tf.command);
        uam_state_put_u8(w, c->dma_tf.device);
        uam_state_put_u16(w, c->dma_tf.features);
        uam_state_put_u16(w, c->dma_tf.count);
        uam_state_put_u64(w, c->dma_tf.lba);
        uint32_t held = c->total > c->len ? c->total : c->len;
        uam_state_put_u32(w, held);
        uam_state_put_bytes(w, c->buf, held);
        uam_state_put_signal(w, &c->irq);
    }
}

static void ide_restore(uam_device *dv, uam_state_reader *r)
{
    ide *h = from_bm(dv);
    for (unsigned i = 0; i < 2; i++) {
        ide_ch *c = &h->ch[i];
        for (unsigned k = 0; k < 2; k++) {
            ata_drive *d = &c->drv[k];
            uint8_t b[7];
            uam_state_get_bytes(r, b, sizeof(b));
            d->sense_key = b[0]; d->asc = b[1]; d->ascq = b[2]; d->unit_attention = b[3];
            c->heads[k] = b[4]; c->spt[k] = b[5]; c->mult[k] = b[6];
        }
        uint8_t x[24];
        uam_state_get_bytes(r, x, sizeof(x));
        c->feat = x[0]; c->count = x[1]; c->lba[0] = x[2]; c->lba[1] = x[3]; c->lba[2] = x[4];
        c->hob_feat = x[5]; c->hob_count = x[6]; c->hob_lba[0] = x[7]; c->hob_lba[1] = x[8];
        c->hob_lba[2] = x[9]; c->device = x[10]; c->status = x[11]; c->error = x[12];
        c->ctl = x[13]; c->intrq = x[14]; c->phase = x[15]; c->block = x[16]; c->packet = x[17];
        c->pkt_status = x[18]; c->pkt_error = x[19]; c->dma_pending = x[20]; c->dma_atapi = x[21];
        c->bm_cmd = x[22]; c->bm_status = x[23];
        c->pos = uam_state_get_u32(r);
        c->len = uam_state_get_u32(r);
        c->total = uam_state_get_u32(r);
        c->remaining = uam_state_get_u32(r);
        c->next_lba = uam_state_get_u64(r);
        c->bm_prd = uam_state_get_u32(r);
        uam_state_get_bytes(r, c->cdb, sizeof(c->cdb));
        c->dma_tf.command = uam_state_get_u8(r);
        c->dma_tf.device = uam_state_get_u8(r);
        c->dma_tf.features = uam_state_get_u16(r);
        c->dma_tf.count = uam_state_get_u16(r);
        c->dma_tf.lba = uam_state_get_u64(r);
        uint32_t held = uam_state_get_u32(r);
        if (held > PIO_MAX) {
            uam_state_fail(r, "ide: PIO buffer");
            return;
        }
        uam_state_get_bytes(r, c->buf, held);
        if (c->pos > PIO_MAX || c->len > PIO_MAX || c->total > PIO_MAX) {
            uam_state_fail(r, "ide: PIO position");
            return;
        }
        uam_state_get_signal(r, &c->irq);
    }
}

static const uam_device_ops bm_ops = {
    .type = "ide-busmaster", .read = bm_read, .write = bm_write, .reset = ide_reset,
    .destroy = ide_destroy, .save = ide_save, .restore = ide_restore,
};
static const uam_device_ops cmd_ops = {
    .type = "ide-command", .read = cmd_read, .write = cmd_write, .destroy = cmdv_destroy,
    .flags = UAM_DEVICE_STATELESS,
};
static const uam_device_ops ctl_ops = {
    .type = "ide-control", .impl_min = 1, .impl_max = 1, .read = ctl_read, .write = ctl_write,
    .destroy = cmdv_destroy, .flags = UAM_DEVICE_STATELESS,
};

uam_device *uam_ide_create(const char *name, uint16_t vendor, uint16_t device, uint8_t revision)
{
    ide *h = calloc(1, sizeof(*h));
    if (!h)
        return NULL;
    h->bounce = malloc(ATA_XFER_MAX);
    h->ch[0].buf = malloc(PIO_MAX);
    h->ch[1].buf = malloc(PIO_MAX);
    if (!h->bounce || !h->ch[0].buf || !h->ch[1].buf) {
        ide_destroy(&h->dev);
        return NULL;
    }
    uam_device_init(&h->dev, &bm_ops, name);
    for (unsigned i = 0; i < 2; i++) {
        uam_device_init(&h->cmdv[i].dev, &cmd_ops, name);
        uam_device_init(&h->ctlv[i].dev, &ctl_ops, name);
        h->cmdv[i].c = h->ctlv[i].c = &h->ch[i];
        h->ch[i].h = h;
        h->ch[i].idx = i;
        uam_signal_init(&h->ch[i].irq);
    }
    uam_pci_func *f = &h->fn;
    uam_pci_func_init(f, name, vendor, device, 0x01018000u | revision, NULL, h);
    uam_pci_func_bar(f, 4, 16, UAM_PCI_BAR_IO, &h->dev);
    ide_reset(&h->dev);
    return &h->dev;
}

uam_device   *uam_ide_cmd(uam_device *d, unsigned ch) { return &from_bm(d)->cmdv[ch & 1].dev; }
uam_device   *uam_ide_ctl(uam_device *d, unsigned ch) { return &from_bm(d)->ctlv[ch & 1].dev; }
uam_device   *uam_ide_bm(uam_device *d) { return d; }
uam_signal   *uam_ide_irq_line(uam_device *d, unsigned ch) { return &from_bm(d)->ch[ch & 1].irq; }
uam_pci_func *uam_ide_func(uam_device *d) { return &from_bm(d)->fn; }

uam_status uam_ide_attach(uam_device *dv, unsigned channel, unsigned drive, uam_blockdev *bd,
                          bool atapi)
{
    ide *h = from_bm(dv);
    if (channel > 1 || drive > 1 || !bd || bd->sector_size != (atapi ? 2048u : 512u))
        return UAM_ERR_INVALID;
    ide_ch *c = &h->ch[channel];
    ata_drive *d = &c->drv[drive];
    if (d->bd)
        return UAM_ERR_INVALID;
    d->bd = bd;
    d->atapi = atapi;
    snprintf(d->serial, sizeof(d->serial), "UAM-IDE-%u%u", channel, drive);
    d->model = atapi ? "UAM ATAPI DVD-ROM" : "UAM ATA disk";
    d->product = "ATAPI DVD-ROM   ";
    channel_reset(c);
    return UAM_OK;
}
