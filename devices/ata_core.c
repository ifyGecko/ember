/*
 * ata_core.c - The ATA/ATAPI device side of the storage controllers (ata_core.h).
 *
 * The commands follow ATA8-ACS and, for ATAPI, MMC-6.
 */
#include <stdio.h>
#include <string.h>

#include "ata_core.h"

static void le_put(uint8_t *p, unsigned n, uint64_t v)
{
    for (unsigned i = 0; i < n; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static uint32_t be_get(const uint8_t *p, unsigned n)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < n; i++)
        v = v << 8 | p[i];
    return v;
}

static void be_put(uint8_t *p, unsigned n, uint32_t v)
{
    for (unsigned i = 0; i < n; i++)
        p[i] = (uint8_t)(v >> (8 * (n - 1 - i)));
}

void ata_drive_reset(ata_drive *d)
{
    d->sense_key = d->asc = d->ascq = 0;
    d->unit_attention = d->atapi;
}

uint32_t ata_signature(const ata_drive *d) { return d->atapi ? ATA_SIG_ATAPI : ATA_SIG_ATA; }

/* ---- ATA ----------------------------------------------------------------- */

static void id_string(uint16_t *w, unsigned words, const char *s)
{
    char tmp[64];
    memset(tmp, ' ', sizeof(tmp));
    size_t n = strlen(s);
    memcpy(tmp, s, n < words * 2 ? n : words * 2);
    for (unsigned i = 0; i < words; i++)
        w[i] = (uint16_t)((uint8_t)tmp[2 * i] << 8 | (uint8_t)tmp[2 * i + 1]);
}

static void identify_disk(const ata_drive *d, uint16_t *w)
{
    memset(w, 0, 512);
    uint64_t n = d->bd->sectors;
    w[0] = 0x0040;
    w[1] = n / (16 * 63) > 16383 ? 16383 : (uint16_t)(n / (16 * 63));
    w[3] = 16;
    w[6] = 63;
    id_string(w + 10, 10, d->serial);
    id_string(w + 23, 4, "1.0");
    id_string(w + 27, 20, d->model);
    w[47] = 0x8010;
    w[49] = 0x0300;                    /* LBA, DMA */
    w[50] = 0x4000;
    w[53] = 0x0006;                    /* words 64-70 and 88 valid */
    uint64_t n28 = n > 0x0FFFFFFF ? 0x0FFFFFFF : n;
    w[60] = (uint16_t)n28;
    w[61] = (uint16_t)(n28 >> 16);
    w[63] = 0x0007;
    w[64] = 0x0003;
    w[65] = w[66] = w[67] = w[68] = 120;
    w[76] = 0x0006;                    /* SATA Gen1 and Gen2 */
    w[80] = 0x01F0;                    /* ATA/ATAPI-4 to -8 */
    w[81] = 0x0028;
    w[82] = 0x4020;                    /* NOP, write cache */
    w[83] = 0x7400;                    /* FLUSH CACHE EXT, FLUSH CACHE, 48-bit */
    w[84] = 0x4000;
    w[85] = 0x4020;
    w[86] = 0x3400;
    w[87] = 0x4000;
    w[88] = 0x203F;                    /* UDMA 0-5, mode 5 selected */
    w[93] = 0x0000;
    for (unsigned i = 0; i < 4; i++)
        w[100 + i] = (uint16_t)(n >> (16 * i));
    w[106] = 0x4000;
    w[217] = 0x0001;                   /* non-rotating */
}

static void identify_disc(const ata_drive *d, uint16_t *w)
{
    memset(w, 0, 512);
    w[0] = 0x85C0;                     /* ATAPI, CD/DVD, removable, 50 us DRQ, 12-byte packets */
    id_string(w + 10, 10, d->serial);
    id_string(w + 23, 4, "1.0");
    id_string(w + 27, 20, d->model);
    w[49] = 0x0300;
    w[53] = 0x0006;
    w[63] = 0x0007;
    w[64] = 0x0003;
    w[65] = w[66] = w[67] = w[68] = 120;
    w[76] = 0x0006;
    w[80] = 0x01F0;
    w[82] = 0x4010;                    /* NOP, PACKET */
    w[83] = 0x4000;
    w[84] = 0x4000;
    w[85] = 0x4010;
    w[86] = 0x0000;
    w[87] = 0x4000;
    w[88] = 0x203F;
}

void ata_identify(const ata_drive *d, uint16_t w[256])
{
    if (d->atapi) identify_disc(d, w);
    else          identify_disk(d, w);
}

static void ata_ok(ata_result *r)    { r->status = ATA_ST_DRDY | ATA_ST_DSC; r->error = 0; }
static void ata_abort(ata_result *r) { r->status = ATA_ST_DRDY | ATA_ST_ERR; r->error = ATA_ERR_ABRT; }

/* Move `count` sectors from `lba` between the disk and the host. */
static bool ata_rw(ata_drive *d, ata_io *io, uint64_t lba, uint32_t count, bool write)
{
    uam_blockdev *bd = d->bd;
    const uint32_t ss = bd->sector_size, per = ATA_XFER_MAX / ss;
    if (lba > bd->sectors || count > bd->sectors - lba)
        return false;
    while (count) {
        uint32_t n = count < per ? count : per;
        if (write) {
            if (io->copy(io->ctx, io->bounce, n * ss, false) != n * ss ||
                uam_blockdev_write(bd, lba, io->bounce, n) != UAM_OK)
                return false;
        } else {
            if (uam_blockdev_read(bd, lba, io->bounce, n) != UAM_OK ||
                io->copy(io->ctx, io->bounce, n * ss, true) != n * ss)
                return false;
        }
        lba += n;
        count -= n;
    }
    return true;
}

void ata_exec(ata_drive *p, const ata_taskfile *tf, ata_io *io, ata_result *r)
{
    uint8_t cmd = tf->command;
    uint64_t lba48 = tf->lba & 0xFFFFFFFFFFFFull;
    uint64_t lba28 = (tf->lba & 0xFFFFFF) | (uint64_t)(tf->device & 0x0F) << 24;
    uint32_t cnt48 = tf->count, cnt28 = tf->count & 0xFF;
    if (!cnt48) cnt48 = 65536;
    if (!cnt28) cnt28 = 256;
    uint32_t sig = ata_signature(p);
    io->pio = false;
    ata_abort(r);
    if (p->atapi && cmd != 0xA1 && cmd != 0x08 && cmd != 0xEF && cmd != 0xE5 &&
        cmd != 0xE0 && cmd != 0xE1 && cmd != 0xE2 && cmd != 0xE3) {
        /* ATA commands abort on a packet device, which shows its signature */
        r->lba = sig >> 8;
        r->count = (uint16_t)(sig & 0xFF);
        return;
    }
    switch (cmd) {
    case 0xEC:                                         /* IDENTIFY DEVICE */
    case 0xA1: {                                       /* IDENTIFY PACKET DEVICE */
        if ((cmd == 0xA1) != p->atapi)
            return;
        uint16_t w[256];
        ata_identify(p, w);
        uint8_t b[512];
        for (unsigned i = 0; i < 256; i++)
            le_put(b + 2 * i, 2, w[i]);
        io->pio = true;
        io->copy(io->ctx, b, 512, true);
        ata_ok(r);
        return;
    }
    case 0x25: case 0x24: case 0x35: case 0x34:        /* READ/WRITE DMA EXT, SECTORS EXT */
        if (ata_rw(p, io, lba48, cnt48, cmd == 0x35 || cmd == 0x34))
            ata_ok(r);
        io->pio = cmd == 0x24 || cmd == 0x34;
        return;
    case 0xC8: case 0x20: case 0x21: case 0xCA: case 0x30: case 0x31:  /* 28-bit */
        if (ata_rw(p, io, lba28, cnt28, cmd == 0xCA || cmd == 0x30 || cmd == 0x31))
            ata_ok(r);
        io->pio = cmd != 0xC8 && cmd != 0xCA;
        return;
    case 0xE7: case 0xEA:                              /* FLUSH CACHE (EXT) */
        if (uam_blockdev_flush(p->bd) == UAM_OK)
            ata_ok(r);
        return;
    case 0xE5:                                         /* CHECK POWER MODE */
        ata_ok(r);
        r->count = 0xFF;
        return;
    case 0xF8:                                         /* READ NATIVE MAX ADDRESS */
        ata_ok(r);
        r->lba = (p->bd->sectors - 1) & 0x0FFFFFFF;
        return;
    case 0x27:                                         /* READ NATIVE MAX ADDRESS EXT */
        ata_ok(r);
        r->lba = p->bd->sectors - 1;
        return;
    case 0x08:                                         /* DEVICE RESET */
        r->status = 0;
        r->error = 0x01;
        r->lba = sig >> 8;
        r->count = (uint16_t)(sig & 0xFF);
        return;
    case 0xEF: case 0xC6: case 0x91: case 0x00:        /* SET FEATURES/MULTIPLE, INIT PARAMS, NOP */
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE6:   /* power modes */
    case 0x40: case 0x42: case 0x70:                   /* VERIFY (EXT), SEEK */
        if (cmd != 0x00)
            ata_ok(r);
        return;
    default:
        return;                                        /* abort */
    }
}

/* ---- ATAPI --------------------------------------------------------------- */

static void sense(ata_drive *p, uint8_t key, uint8_t asc, uint8_t ascq)
{
    p->sense_key = key;
    p->asc = asc;
    p->ascq = ascq;
}

/* Reply with up to `alloc` bytes of `data`. */
static void reply(ata_io *c, const uint8_t *data, uint32_t len, uint32_t alloc)
{
    c->copy(c->ctx, (uint8_t *)data, len < alloc ? len : alloc, true);
}

void atapi_exec(ata_drive *p, const uint8_t cdb[12], ata_io *c, ata_result *r)
{
    uam_blockdev *bd = p->bd;
    uint64_t blocks = bd->sectors;
    uint8_t b[64];
    memset(b, 0, sizeof(b));
    bool good = true;
    uint8_t op = cdb[0];

    if (p->unit_attention && op != 0x12 && op != 0x03 && op != 0x46 && op != 0x4A) {
        p->unit_attention = false;
        sense(p, 0x06, 0x28, 0x00);            /* not ready to ready change, medium may have changed */
        good = false;
        goto done;
    }
    if (op != 0x03)
        sense(p, 0, 0, 0);
    switch (op) {
    case 0x00:                                 /* TEST UNIT READY */
    case 0x1B:                                 /* START STOP UNIT */
    case 0x1E:                                 /* PREVENT ALLOW MEDIUM REMOVAL */
    case 0x35:                                 /* SYNCHRONIZE CACHE */
        break;
    case 0x03:                                 /* REQUEST SENSE */
        b[0] = 0x70;
        b[2] = p->sense_key;
        b[7] = 10;
        b[12] = p->asc;
        b[13] = p->ascq;
        reply(c, b, 18, cdb[4]);
        sense(p, 0, 0, 0);
        break;
    case 0x12:                                 /* INQUIRY */
        if (cdb[1] & 1) {                      /* vital product data pages: none */
            sense(p, 0x05, 0x24, 0x00);
            good = false;
            break;
        }
        b[0] = 0x05;                           /* CD/DVD */
        b[1] = 0x80;                           /* removable */
        b[2] = 0x05;
        b[3] = 0x32;
        b[4] = 31;
        memcpy(b + 8, "UAM     ", 8);
        memcpy(b + 16, p->product, 16);
        memcpy(b + 32, "1.0 ", 4);
        reply(c, b, 36, be_get(cdb + 3, 2));
        break;
    case 0x25:                                 /* READ CAPACITY(10) */
        be_put(b, 4, (uint32_t)(blocks ? blocks - 1 : 0));
        be_put(b + 4, 4, bd->sector_size);
        reply(c, b, 8, 8);
        break;
    case 0x28: case 0xA8: {                    /* READ(10), READ(12) */
        uint32_t lba = be_get(cdb + 2, 4);
        uint32_t n = op == 0x28 ? be_get(cdb + 7, 2) : be_get(cdb + 6, 4);
        if (!ata_rw(p, c, lba, n, false)) {
            sense(p, 0x05, 0x21, 0x00);        /* logical block address out of range */
            good = false;
        }
        break;
    }
    case 0x43: {                               /* READ TOC/PMA/ATIP */
        unsigned fmt = cdb[2] & 0xF ? cdb[2] & 0xF : cdb[9] >> 6;
        bool msf = cdb[1] & 2;
        uint32_t alloc = be_get(cdb + 7, 2), len;
        if (fmt == 0) {
            b[2] = 1; b[3] = 1;                /* first and last track */
            b[5] = 0x14; b[6] = 1;             /* track 1: data */
            b[13] = 0x16; b[14] = 0xAA;        /* lead-out */
            uint32_t lo = (uint32_t)blocks;
            if (msf) {
                b[8 + 1] = 0; b[8 + 2] = 2; b[8 + 3] = 0;   /* 00:02:00 */
                uint32_t f = lo + 150;
                b[16 + 1] = (uint8_t)(f / 4500); b[16 + 2] = (uint8_t)(f / 75 % 60);
                b[16 + 3] = (uint8_t)(f % 75);
            } else {
                be_put(b + 16, 4, lo);
            }
            len = 20;
        } else if (fmt == 1) {                 /* multi-session information */
            b[2] = 1; b[3] = 1;
            b[5] = 0x14; b[6] = 1;
            len = 12;
        } else {
            sense(p, 0x05, 0x24, 0x00);
            good = false;
            break;
        }
        be_put(b, 2, len - 2);
        reply(c, b, len, alloc);
        break;
    }
    case 0x1A: case 0x5A: {                    /* MODE SENSE(6/10): no pages */
        unsigned page = cdb[2] & 0x3F;
        uint32_t alloc = op == 0x1A ? cdb[4] : be_get(cdb + 7, 2);
        unsigned hdr = op == 0x1A ? 4 : 8, len = hdr;
        if (page == 0x2A || page == 0x3F) {    /* capabilities and mechanical status */
            uint8_t *m = b + hdr;
            m[0] = 0x2A; m[1] = 0x14;
            m[2] = 0x3B;                       /* reads CD-R/RW, DVD-ROM/R */
            m[6] = 0x29;                       /* lock, eject, tray */
            be_put(m + 8, 2, 706);             /* 4x */
            len += 22;
        } else if (page != 0x01 && page != 0x0D && page != 0x0E) {
            sense(p, 0x05, 0x24, 0x00);
            good = false;
            break;
        }
        if (op == 0x1A) b[0] = (uint8_t)(len - 1);
        else            be_put(b, 2, len - 2);
        reply(c, b, len, alloc);
        break;
    }
    case 0x46:                                 /* GET CONFIGURATION */
        be_put(b, 4, 4);
        be_put(b + 6, 2, blocks > 1440000 ? 0x0010 : 0x0008);   /* DVD-ROM or CD-ROM */
        reply(c, b, 8, be_get(cdb + 7, 2));
        break;
    case 0x4A:                                 /* GET EVENT STATUS NOTIFICATION */
        if (!(cdb[1] & 1)) {                   /* only polled operation */
            sense(p, 0x05, 0x24, 0x00);
            good = false;
            break;
        }
        be_put(b, 2, 2);
        b[2] = 0x80;                           /* no event available */
        b[3] = 0x10;                           /* media class supported */
        reply(c, b, 4, be_get(cdb + 7, 2));
        break;
    case 0x51:                                 /* READ DISC INFORMATION */
        be_put(b, 2, 32);
        b[2] = 0x0E;                           /* complete, finalized */
        b[3] = 1; b[4] = 1; b[5] = 1; b[6] = 1;
        reply(c, b, 34, be_get(cdb + 7, 2));
        break;
    default:
        sense(p, 0x05, 0x20, 0x00);            /* invalid command operation code */
        good = false;
        break;
    }
done:
    if (good) {
        r->status = ATA_ST_DRDY | ATA_ST_DSC;
        r->error = 0;
    } else {
        r->status = ATA_ST_DRDY | ATA_ST_ERR;
        r->error = (uint8_t)(p->sense_key << 4);
    }
    r->count = 0x03;                           /* I/O and C/D: command complete */
}

