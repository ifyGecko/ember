/*
 * fdc.c - 82077AA-compatible floppy disk controller (uam/dev/fdc.h).
 *
 * Commands follow the 82077AA data sheet: a command phase (the host writes the
 * command and parameter bytes to the FIFO while MSR shows RQM with DIO clear),
 * an execution phase (immediate here; data through DMA, or the FIFO in
 * non-DMA mode) and a result phase (RQM | DIO | busy until the host has read
 * every result byte).
 */
#include <string.h>
#include <stdlib.h>

#include "uam/dev/fdc.h"
#include "uam/dev/i8237.h"

#define MSR_RQM 0x80
#define MSR_DIO 0x40
#define MSR_NDMA 0x20
#define MSR_CB  0x10

#define DOR_RESET 0x04          /* active low: 0 holds the controller in reset */
#define DOR_DMA   0x08          /* DMA and interrupt enable */

enum phase { PH_CMD, PH_RESULT, PH_PIO_IN, PH_PIO_OUT };

typedef struct fdrive {
    uam_blockdev *bd;
    bool          ro, changed;
    uint8_t       pcn, head;     /* cylinder the head is over, last head used */
    uint8_t       spt, heads, tracks;
} fdrive;

typedef struct fdc {
    uam_device  dev;
    uam_device  dirv;
    uam_device *dma;
    unsigned    chan;
    uam_signal  irq;
    fdrive      d[2];
    uint8_t     dor, dsr, ccr, specify[2], config, precomp, perp;
    bool        lock, ndma;
    uint8_t     phase;
    uint8_t     cmd[9], ncmd, want;
    uint8_t     res[10], nres, rpos;
    bool        intr;            /* INT pending (SENSE INTERRUPT STATUS reports it) */
    uint8_t     st0, reset_polls;
    /* non-DMA data: the sector being moved through the FIFO */
    uint8_t     buf[512];
    unsigned    bpos;
    uint8_t     x_c, x_h, x_r, x_n, x_eot, x_drive;
    bool        x_mt, x_write;
} fdc;

static fdc *from_dev(uam_device *d) { return UAM_CONTAINER_OF(d, fdc, dev); }
static fdc *from_dir(uam_device *d) { return UAM_CONTAINER_OF(d, fdc, dirv); }

static void update_irq(fdc *f) { uam_signal_set(&f->irq, f->intr && (f->dor & DOR_DMA)); }

static void raise(fdc *f)
{
    f->intr = true;
    update_irq(f);
}

/* Parameter bytes after each command byte (by the low five bits). */
static int params_of(uint8_t c)
{
    switch (c & 0x1F) {
    case 0x03: return 2;   /* SPECIFY */
    case 0x04: return 1;   /* SENSE DRIVE STATUS */
    case 0x07: return 1;   /* RECALIBRATE */
    case 0x08: return 0;   /* SENSE INTERRUPT STATUS */
    case 0x0F: return 2;   /* SEEK */
    case 0x05: case 0x06: case 0x09: case 0x0C: case 0x11: case 0x16: case 0x19: case 0x1D:
        return 8;          /* WRITE / READ DATA (and deleted, verify, scan: as reads) */
    case 0x0A: return 1;   /* READ ID */
    case 0x0D: return 5;   /* FORMAT TRACK */
    case 0x10: return 0;   /* VERSION */
    case 0x13: return 3;   /* CONFIGURE */
    case 0x14: return 0;   /* LOCK (bit 7: lock) */
    case 0x0E: return 0;   /* DUMPREG */
    case 0x12: return 1;   /* PERPENDICULAR MODE */
    default:   return 0;
    }
}

static void result(fdc *f, const uint8_t *r, unsigned n)
{
    memcpy(f->res, r, n);
    f->nres = (uint8_t)n;
    f->rpos = 0;
    f->phase = n ? PH_RESULT : PH_CMD;
}

static bool geometry(fdrive *d)
{
    static const struct { uint32_t sectors; uint8_t tracks, heads, spt; } fmt[] = {
        { 320, 40, 1, 8 }, { 360, 40, 1, 9 }, { 640, 40, 2, 8 }, { 720, 40, 2, 9 },
        { 1440, 80, 2, 9 }, { 2400, 80, 2, 15 }, { 2880, 80, 2, 18 }, { 3360, 80, 2, 21 },
        { 5760, 80, 2, 36 },
    };
    for (size_t i = 0; i < sizeof(fmt) / sizeof(fmt[0]); i++)
        if (d->bd && d->bd->sectors == fmt[i].sectors) {
            d->tracks = fmt[i].tracks;
            d->heads = fmt[i].heads;
            d->spt = fmt[i].spt;
            return true;
        }
    return false;
}

static uint8_t st3(const fdc *f, unsigned drv, unsigned head)
{
    const fdrive *d = &f->d[drv];
    return (uint8_t)(0x20 | (d->ro ? 0x40 : 0) | (d->pcn == 0 ? 0x10 : 0) |
                     (d->heads == 2 ? 0x08 : 0) | (head ? 4 : 0) | drv);
}

/* ---- data transfer ------------------------------------------------------- */

static uint64_t lba_of(const fdrive *d, unsigned c, unsigned h, unsigned r)
{
    return ((uint64_t)c * d->heads + h) * d->spt + (r - 1);
}

/* The sector after (c, h, r), with multi-track; false past the end of the track(s). */
static bool advance(fdc *f, const fdrive *d)
{
    if (f->x_r < f->x_eot) {
        f->x_r++;
        return true;
    }
    f->x_r = 1;
    if (f->x_mt && f->x_h == 0 && d->heads == 2) {
        f->x_h = 1;
        return true;
    }
    if (f->x_mt) f->x_h = 0;
    f->x_c++;
    return false;
}

static void rw_result(fdc *f, uint8_t st0, uint8_t st1, uint8_t st2)
{
    uint8_t r[7] = { (uint8_t)(st0 | (f->x_h << 2) | f->x_drive), st1, st2, f->x_c, f->x_h, f->x_r, f->x_n };
    result(f, r, 7);
    raise(f);
}

/* READ DATA / WRITE DATA, sector by sector, through DMA until terminal count
 * or the end of the track. */
static void rw_dma(fdc *f)
{
    fdrive *d = &f->d[f->x_drive];
    for (;;) {
        if (f->x_r == 0 || f->x_r > d->spt || f->x_h >= d->heads || f->x_c >= d->tracks) {
            rw_result(f, 0x40, 0x04, 0x00);           /* abnormal: no data (sector not found) */
            return;
        }
        uint64_t lba = lba_of(d, f->x_c, f->x_h, f->x_r);
        bool tc = false;
        uint8_t sec[512];
        if (f->x_write) {
            unsigned n = uam_i8237_transfer(f->dma, f->chan, sec, 512, &tc);
            if (n < 512 || uam_blockdev_write(d->bd, lba, sec, 1) != UAM_OK) {
                rw_result(f, 0x40, 0x10, 0x00);       /* overrun */
                return;
            }
        } else {
            if (uam_blockdev_read(d->bd, lba, sec, 1) != UAM_OK) {
                rw_result(f, 0x40, 0x20, 0x20);       /* data error */
                return;
            }
            unsigned n = uam_i8237_transfer(f->dma, f->chan, sec, 512, &tc);
            if (n < 512 && !tc) {
                rw_result(f, 0x40, 0x10, 0x00);
                return;
            }
        }
        bool more = advance(f, d);
        if (tc) {
            rw_result(f, 0x00, 0x00, 0x00);
            return;
        }
        if (!more) {
            rw_result(f, 0x40, 0x80, 0x00);           /* end of cylinder */
            return;
        }
    }
}

/* Non-DMA: the next sector through the FIFO. */
static void pio_sector(fdc *f)
{
    fdrive *d = &f->d[f->x_drive];
    if (f->x_r == 0 || f->x_r > d->spt || f->x_c >= d->tracks) {
        rw_result(f, 0x40, 0x04, 0x00);
        return;
    }
    f->bpos = 0;
    if (f->x_write) {
        f->phase = PH_PIO_OUT;
    } else {
        if (uam_blockdev_read(d->bd, lba_of(d, f->x_c, f->x_h, f->x_r), f->buf, 1) != UAM_OK) {
            rw_result(f, 0x40, 0x20, 0x20);
            return;
        }
        f->phase = PH_PIO_IN;
    }
    raise(f);
}

/* ---- commands ------------------------------------------------------------- */

static void execute(fdc *f)
{
    uint8_t c = f->cmd[0], op = c & 0x1F;
    unsigned drv = f->cmd[1] & 1, head = (f->cmd[1] >> 2) & 1;
    fdrive *d = &f->d[drv];
    switch (op) {
    case 0x03:                                           /* SPECIFY */
        f->specify[0] = f->cmd[1];
        f->specify[1] = f->cmd[2];
        f->ndma = f->cmd[2] & 1;
        result(f, NULL, 0);
        return;
    case 0x04: {                                         /* SENSE DRIVE STATUS */
        uint8_t r = st3(f, drv, head);
        result(f, &r, 1);
        return;
    }
    case 0x07:                                           /* RECALIBRATE */
        d->pcn = 0;
        f->st0 = (uint8_t)(0x20 | drv);                  /* seek end */
        if (d->bd)
            d->changed = false;
        result(f, NULL, 0);
        raise(f);
        return;
    case 0x0F:                                           /* SEEK */
        d->pcn = f->cmd[2];
        d->head = (uint8_t)head;
        f->st0 = (uint8_t)(0x20 | (head << 2) | drv);
        if (d->bd)
            d->changed = false;                          /* a step clears disk change */
        result(f, NULL, 0);
        raise(f);
        return;
    case 0x08: {                                         /* SENSE INTERRUPT STATUS */
        if (f->reset_polls) {                            /* after a reset: one per drive */
            unsigned n = 4 - f->reset_polls--;
            uint8_t r[2] = { (uint8_t)(0xC0 | n), f->d[n & 1].pcn };
            if (!f->reset_polls) {
                f->intr = false;
                update_irq(f);
            }
            result(f, r, 2);
            return;
        }
        if (!f->intr) {
            uint8_t r = 0x80;
            result(f, &r, 1);
            return;
        }
        f->intr = false;
        update_irq(f);
        uint8_t r[2] = { f->st0, f->d[f->st0 & 1].pcn };
        result(f, r, 2);
        return;
    }
    case 0x10: {                                         /* VERSION: 82077AA */
        uint8_t r = 0x90;
        result(f, &r, 1);
        return;
    }
    case 0x13:                                           /* CONFIGURE */
        f->config = f->cmd[2];
        f->precomp = f->cmd[3];
        result(f, NULL, 0);
        return;
    case 0x14: {                                         /* LOCK / UNLOCK */
        f->lock = (c & 0x80) != 0;
        uint8_t r = f->lock ? 0x10 : 0x00;
        result(f, &r, 1);
        return;
    }
    case 0x0E: {                                         /* DUMPREG */
        uint8_t r[10] = { f->d[0].pcn, f->d[1].pcn, 0, 0, f->specify[0], f->specify[1],
                          f->x_eot, (uint8_t)((f->lock ? 0x80 : 0) | (f->perp & 0x7F)), f->config,
                          f->precomp };
        result(f, r, 10);
        return;
    }
    case 0x12:                                           /* PERPENDICULAR MODE */
        f->perp = f->cmd[1];
        result(f, NULL, 0);
        return;
    case 0x0A: {                                         /* READ ID */
        f->x_drive = (uint8_t)drv;
        f->x_c = d->pcn;
        f->x_h = (uint8_t)head;
        f->x_r = 1;
        f->x_n = 2;
        if (!d->bd) {
            rw_result(f, 0x40, 0x01, 0x00);              /* missing address mark */
            return;
        }
        rw_result(f, 0x00, 0x00, 0x00);
        return;
    }
    case 0x05: case 0x06: case 0x09: case 0x0C: case 0x11: case 0x16: case 0x19: case 0x1D:
        f->x_drive = (uint8_t)drv;
        f->x_c = f->cmd[2];
        f->x_h = f->cmd[3];
        f->x_r = f->cmd[4];
        f->x_n = f->cmd[5];
        f->x_eot = f->cmd[6];
        f->x_mt = (c & 0x80) != 0;
        f->x_write = op == 0x05 || op == 0x09;
        if (!d->bd) {
            rw_result(f, 0x40, 0x01, 0x00);
            return;
        }
        if (f->x_write && d->ro) {
            rw_result(f, 0x40, 0x02, 0x00);              /* not writable */
            return;
        }
        if (f->x_n != 2) {
            rw_result(f, 0x40, 0x04, 0x00);
            return;
        }
        if (f->ndma) pio_sector(f);
        else         rw_dma(f);
        return;
    case 0x0D: {                                         /* FORMAT TRACK: C, H, R, N per sector by DMA */
        unsigned n = f->cmd[3];
        uint8_t fill = f->cmd[5];
        f->x_drive = (uint8_t)drv;
        f->x_h = (uint8_t)head;
        f->x_c = d->pcn;
        f->x_n = f->cmd[2];
        if (!d->bd || d->ro) {
            rw_result(f, 0x40, d->bd ? 0x02 : 0x01, 0x00);
            return;
        }
        uint8_t sec[512];
        memset(sec, fill, sizeof(sec));
        for (unsigned i = 0; i < n; i++) {
            uint8_t id[4];
            if (uam_i8237_transfer(f->dma, f->chan, id, 4, NULL) < 4)
                break;
            if (id[2] >= 1 && id[2] <= d->spt && id[0] < d->tracks && id[1] < d->heads)
                uam_blockdev_write(d->bd, lba_of(d, id[0], id[1], id[2]), sec, 1);
            f->x_r = id[2];
        }
        rw_result(f, 0x00, 0x00, 0x00);
        return;
    }
    default: {
        uint8_t r = 0x80;                                /* invalid command */
        result(f, &r, 1);
        return;
    }
    }
}

static void fifo_write(fdc *f, uint8_t b)
{
    if (f->phase == PH_PIO_OUT) {
        f->buf[f->bpos++] = b;
        if (f->bpos == 512) {
            fdrive *d = &f->d[f->x_drive];
            uam_blockdev_write(d->bd, lba_of(d, f->x_c, f->x_h, f->x_r), f->buf, 1);
            if (advance(f, d)) pio_sector(f);
            else               rw_result(f, 0x40, 0x80, 0x00);
        }
        return;
    }
    if (f->phase != PH_CMD)
        return;
    if (f->ncmd == 0)
        f->want = (uint8_t)(1 + params_of(b));
    f->cmd[f->ncmd++] = b;
    if (f->ncmd >= f->want) {
        f->ncmd = 0;
        execute(f);
    }
}

static uint8_t fifo_read(fdc *f)
{
    if (f->phase == PH_PIO_IN) {
        uint8_t b = f->buf[f->bpos++];
        if (f->bpos == 512) {
            if (advance(f, &f->d[f->x_drive])) pio_sector(f);
            else                               rw_result(f, 0x40, 0x80, 0x00);
        }
        return b;
    }
    if (f->phase != PH_RESULT)
        return 0xFF;
    uint8_t b = f->res[f->rpos++];
    if (f->rpos >= f->nres) {
        f->phase = PH_CMD;
        if ((f->cmd[0] & 0x1F) != 0x08) {                 /* reading results ends the interrupt */
            f->intr = false;
            update_irq(f);
        }
    }
    return b;
}

static uint8_t msr(const fdc *f)
{
    switch (f->phase) {
    case PH_RESULT:  return MSR_RQM | MSR_DIO | MSR_CB;
    case PH_PIO_IN:  return MSR_RQM | MSR_DIO | MSR_NDMA | MSR_CB;
    case PH_PIO_OUT: return MSR_RQM | MSR_NDMA | MSR_CB;
    default:         return (uint8_t)(MSR_RQM | (f->ncmd ? MSR_CB : 0));
    }
}

static void controller_reset(fdc *f)
{
    f->phase = PH_CMD;
    f->ncmd = 0;
    f->nres = f->rpos = 0;
    f->ndma = false;
    f->reset_polls = 4;
    f->intr = true;                                      /* the reset interrupt */
    update_irq(f);
}

/* ---- registers ---------------------------------------------------------------- */

static uam_status regs_read(uam_device *dv, uint64_t off, unsigned size, uint64_t *val)
{
    (void)size;
    fdc *f = from_dev(dv);
    switch (off) {
    case 2:  *val = f->dor; break;
    case 3:  *val = 0; break;                            /* tape drive register */
    case 4:  *val = msr(f); break;
    case 5:  *val = fifo_read(f); break;
    default: *val = 0xFF; break;
    }
    return UAM_OK;
}

static uam_status regs_write(uam_device *dv, uint64_t off, unsigned size, uint64_t val)
{
    (void)size;
    fdc *f = from_dev(dv);
    uint8_t b = (uint8_t)val;
    switch (off) {
    case 2: {
        bool was_reset = !(f->dor & DOR_RESET);
        f->dor = b;
        if (!(b & DOR_RESET)) {
            f->phase = PH_CMD;
            f->intr = false;
        } else if (was_reset) {
            controller_reset(f);                         /* leaving reset */
        }
        update_irq(f);
        break;
    }
    case 4:                                              /* data rate select */
        f->dsr = b & 0x7F;
        if (b & 0x80)
            controller_reset(f);                         /* software reset */
        break;
    case 5: fifo_write(f, b); break;
    default: break;
    }
    return UAM_OK;
}

static uam_status dir_read(uam_device *dv, uint64_t off, unsigned size, uint64_t *val)
{
    (void)off; (void)size;
    fdc *f = from_dir(dv);
    *val = f->d[f->dor & 1].changed ? 0x80 : 0x00;       /* disk change, selected drive */
    return UAM_OK;
}

static uam_status dir_write(uam_device *dv, uint64_t off, unsigned size, uint64_t val)
{
    (void)off; (void)size;
    from_dir(dv)->ccr = (uint8_t)val & 3;
    return UAM_OK;
}

/* ---- lifecycle -------------------------------------------------------------------- */

static void fdc_reset(uam_device *dv)
{
    fdc *f = from_dev(dv);
    f->dor = 0;                                          /* held in reset until the BIOS releases it */
    f->dsr = f->ccr = f->config = f->precomp = f->perp = 0;
    f->specify[0] = f->specify[1] = 0;
    f->lock = false;
    f->phase = PH_CMD;
    f->ncmd = f->nres = f->rpos = 0;
    f->intr = false;
    f->reset_polls = 0;
    for (unsigned i = 0; i < 2; i++) {
        f->d[i].pcn = f->d[i].head = 0;
        f->d[i].changed = f->d[i].bd != NULL;
    }
    update_irq(f);
}

static void fdc_destroy(uam_device *dv) { free(from_dev(dv)); }
static void dir_destroy(uam_device *dv) { (void)dv; }

static void fdc_save(uam_device *dv, uam_state_writer *w)
{
    fdc *f = from_dev(dv);
    const uint8_t b[] = { f->dor, f->dsr, f->ccr, f->specify[0], f->specify[1], f->config,
                          f->precomp, f->perp, f->lock, f->ndma, f->phase, f->ncmd, f->want,
                          f->nres, f->rpos, f->intr, f->st0, f->reset_polls, f->x_c, f->x_h,
                          f->x_r, f->x_n, f->x_eot, f->x_drive, f->x_mt, f->x_write,
                          f->d[0].pcn, f->d[0].head, f->d[0].changed,
                          f->d[1].pcn, f->d[1].head, f->d[1].changed };
    uam_state_put_bytes(w, b, sizeof(b));
    uam_state_put_bytes(w, f->cmd, sizeof(f->cmd));
    uam_state_put_bytes(w, f->res, sizeof(f->res));
    uam_state_put_u32(w, f->bpos);
    uam_state_put_bytes(w, f->buf, sizeof(f->buf));
    uam_state_put_signal(w, &f->irq);
}

static void fdc_restore(uam_device *dv, uam_state_reader *r)
{
    fdc *f = from_dev(dv);
    uint8_t b[32];
    uam_state_get_bytes(r, b, sizeof(b));
    f->dor = b[0]; f->dsr = b[1]; f->ccr = b[2]; f->specify[0] = b[3]; f->specify[1] = b[4];
    f->config = b[5]; f->precomp = b[6]; f->perp = b[7]; f->lock = b[8]; f->ndma = b[9];
    f->phase = b[10]; f->ncmd = b[11] % 9; f->want = b[12]; f->nres = b[13] % 11;
    f->rpos = b[14] % 11; f->intr = b[15]; f->st0 = b[16]; f->reset_polls = b[17];
    f->x_c = b[18]; f->x_h = b[19]; f->x_r = b[20]; f->x_n = b[21]; f->x_eot = b[22];
    f->x_drive = b[23] & 1; f->x_mt = b[24]; f->x_write = b[25];
    f->d[0].pcn = b[26]; f->d[0].head = b[27]; f->d[0].changed = b[28];
    f->d[1].pcn = b[29]; f->d[1].head = b[30]; f->d[1].changed = b[31];
    uam_state_get_bytes(r, f->cmd, sizeof(f->cmd));
    uam_state_get_bytes(r, f->res, sizeof(f->res));
    f->bpos = uam_state_get_u32(r) % 512;
    uam_state_get_bytes(r, f->buf, sizeof(f->buf));
    uam_state_get_signal(r, &f->irq);
}

static const uam_device_ops regs_ops = {
    .type = "pc,82077-fdc", .impl_min = 1, .impl_max = 1, .read = regs_read, .write = regs_write,
    .reset = fdc_reset, .destroy = fdc_destroy, .save = fdc_save, .restore = fdc_restore,
};
static const uam_device_ops dir_ops = {
    .type = "pc,82077-fdc-dir", .impl_min = 1, .impl_max = 1, .read = dir_read, .write = dir_write,
    .destroy = dir_destroy, .flags = UAM_DEVICE_STATELESS,
};

uam_device *uam_fdc_create(const char *name, uam_device *dma, unsigned dma_channel)
{
    if (!dma)
        return NULL;
    fdc *f = calloc(1, sizeof(*f));
    if (!f)
        return NULL;
    uam_device_init(&f->dev, &regs_ops, name);
    uam_device_init(&f->dirv, &dir_ops, name);
    uam_signal_init(&f->irq);
    f->dma = dma;
    f->chan = dma_channel;
    fdc_reset(&f->dev);
    return &f->dev;
}

uam_device *uam_fdc_regs(uam_device *d) { return d; }
uam_device *uam_fdc_dir(uam_device *d)  { return &from_dev(d)->dirv; }
uam_signal *uam_fdc_irq_line(uam_device *d) { return &from_dev(d)->irq; }

uam_status uam_fdc_insert(uam_device *dv, unsigned drive, uam_blockdev *bd, bool read_only)
{
    fdc *f = from_dev(dv);
    if (drive > 1 || (bd && bd->sector_size != 512))
        return UAM_ERR_INVALID;
    fdrive *d = &f->d[drive];
    d->bd = bd;
    d->ro = read_only;
    d->changed = true;
    if (bd && !geometry(d)) {
        d->bd = NULL;
        return UAM_ERR_INVALID;
    }
    return UAM_OK;
}
