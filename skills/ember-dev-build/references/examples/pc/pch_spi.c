/*
 * pch_spi.c - Intel PCH SPI controller, hardware sequencing (pch_spi.h).
 *
 * Register layout (SPIBAR, BAR0): coreboot soc/intel/common/block/fast_spi and the PCH
 * datasheets. The firmware's own SPI driver (edk2 SpiFlashCommonLib/PchSpiLib, coreboot
 * fast_spi_flash.c) is the authority for which of them it uses:
 *   0x00 BFPREG       BIOS region base [14:0] / limit [30:16], 4 KiB units
 *   0x04 HSFSTS_CTL   status: FDONE 0, FCERR 1, AEL 2 (write 1 to clear), SCIP 5, FDOPSS 13,
 *                     FDV 14, FLOCKDN 15; control: FGO 16, FCYCLE [20:17], WET 21,
 *                     FDBC [29:24] (bytes - 1), FSMIE 31
 *   0x08 FADDR        flash linear address [26:0]
 *   0x10-0x4F FDATA   64 data bytes
 *   0x50 FRACC        BIOS master read/write access per region
 *   0x54 FREG0-4      descriptor, BIOS, ME, GbE, PDR regions: base [14:0] / limit [30:16]
 *   0x84 FPR0-4       protected ranges: base [14:0], RPE 15, limit [30:16], WPE 31
 *   0xB4 FDOC / 0xB8 FDOD  descriptor observability: section [14:12], dword index [11:2]
 * Cycles complete when FGO is written (FDONE set, SCIP never seen busy). The other offsets
 * of the first 256 bytes (opcode menus, software sequencing, VSCC) are plain storage.
 * Local emulation model; not part of ember.
 */
#include <stdlib.h>
#include <string.h>

#include "pch_spi.h"

enum {
    R_BFPREG = 0x00, R_HSFS = 0x04, R_FADDR = 0x08, R_FDATA = 0x10, R_FRACC = 0x50,
    R_FREG = 0x54, R_FPR = 0x84, R_FDOC = 0xB4, R_FDOD = 0xB8,
};
#define HS_FDONE   (1u << 0)
#define HS_FCERR   (1u << 1)
#define HS_AEL     (1u << 2)
#define HS_FDOPSS  (1u << 13)
#define HS_FDV     (1u << 14)
#define HS_FLOCKDN (1u << 15)
#define HC_FGO     (1u << 16)
#define HS_W1C     (HS_FDONE | HS_FCERR | HS_AEL)
#define HC_RW      0xBF3E0000u          /* FCYCLE, WET, FDBC, FSMIE */
enum { CYC_READ = 0, CYC_WRITE = 2, CYC_ERASE4K = 3, CYC_ERASE64K = 4, CYC_SFDP = 5,
       CYC_RDID = 6, CYC_WRSR = 7, CYC_RDSR = 8 };
#define BIOS_CNTL  0xDC                 /* PCI config: WPD 0, LE 1, EISS 5 */
#define BC_WPD     0x01u
#define BC_LE      0x02u
#define NREG       5u
#define STORE_SIZE 0x100u

typedef struct pch_spi {
    uam_device   dev;          /* owns everything; also the ROMD window's device */
    uam_device   regs;         /* BAR0 */
    uam_pci_func fn;
    uam_bus     *sys;
    uam_region  *window;
    /* configuration */
    uint8_t     *image;        /* the flash as loaded; the BIOS region's live copy is the window */
    size_t       len;
    uint32_t     bios_base, bios_limit;          /* flash linear addresses, inclusive */
    uint32_t     freg[NREG];                     /* region registers (descriptor) */
    uint32_t     sect[4];                        /* FDOC sections: map, component, region, master */
    bool         fdv;
    uint8_t      jedec[3];
    bool         warned_window;
    /* guest-visible state (saved) */
    uint32_t     hsfs;         /* FDONE FCERR AEL FLOCKDN */
    uint32_t     hsfc;         /* FCYCLE WET FDBC FSMIE */
    uint32_t     faddr, fdoc;
    uint32_t     fpr[5];
    uint8_t      fdata[64];
    uint8_t      store[STORE_SIZE];              /* everything else in the first 256 bytes */
} pch_spi;

static pch_spi *from_dev(uam_device *d)  { return UAM_CONTAINER_OF(d, pch_spi, dev); }
static pch_spi *from_regs(uam_device *d) { return UAM_CONTAINER_OF(d, pch_spi, regs); }

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* ---- the flash ----------------------------------------------------------------- */

static bool in_bios(const pch_spi *s, uint32_t a) { return a >= s->bios_base && a <= s->bios_limit; }

static uint8_t flash_byte(pch_spi *s, uint32_t a)
{
    if (in_bios(s, a))
        return uam_bus_romd_backing(s->window)[a - s->bios_base];
    return a < s->len ? s->image[a] : 0xFF;
}

/* May the BIOS write [a, a+n)? Only the BIOS region, with WPD set, outside every
 * write-protected range. */
static bool may_write(pch_spi *s, uint32_t a, uint32_t n)
{
    if (!(s->fn.cfg[BIOS_CNTL] & BC_WPD) || !in_bios(s, a) || !in_bios(s, a + n - 1))
        return false;
    for (unsigned i = 0; i < 5; i++) {
        uint32_t pr = s->fpr[i];
        if (!(pr & 0x80000000u))
            continue;
        uint32_t lo = (pr & 0x7FFF) << 12, hi = ((pr >> 16) & 0x7FFF) << 12 | 0xFFF;
        if (a <= hi && a + n - 1 >= lo)
            return false;
    }
    return true;
}

static void cycle(pch_spi *s)
{
    unsigned cyc = (s->hsfc >> 17) & 0xF, n = ((s->hsfc >> 24) & 0x3F) + 1;
    uint32_t a = s->faddr & 0x7FFFFFF;
    uint8_t *mem = uam_bus_romd_backing(s->window);
    bool ok = true;
    switch (cyc) {
    case CYC_READ:
        if (a + n > s->len) { ok = false; break; }
        for (unsigned i = 0; i < n; i++)
            s->fdata[i] = flash_byte(s, a + i);
        break;
    case CYC_WRITE:
        if (!(ok = may_write(s, a, n)))
            break;
        for (unsigned i = 0; i < n; i++)
            mem[a - s->bios_base + i] &= s->fdata[i];   /* NOR programming only clears bits */
        uam_bus_romd_dirty(s->sys, s->window, a - s->bios_base, n);
        break;
    case CYC_ERASE4K:
    case CYC_ERASE64K: {
        uint32_t sz = cyc == CYC_ERASE4K ? 0x1000 : 0x10000;
        a &= ~(sz - 1);
        if (!(ok = may_write(s, a, sz)))
            break;
        memset(mem + (a - s->bios_base), 0xFF, sz);
        uam_bus_romd_dirty(s->sys, s->window, a - s->bios_base, sz);
        break;
    }
    case CYC_RDID:
        memset(s->fdata, 0, sizeof(s->fdata));
        memcpy(s->fdata, s->jedec, 3);
        break;
    case CYC_SFDP:
    case CYC_RDSR:
        memset(s->fdata, 0, sizeof(s->fdata));          /* no SFDP table; status: idle */
        break;
    case CYC_WRSR:
        break;
    default:
        ok = false;
        uam_log(UAM_LOG_WARN, "pch-spi", "%s: flash cycle %u not modelled", s->dev.name, cyc);
    }
    s->hsfs |= HS_FDONE | (ok ? 0 : HS_FCERR);
}

/* ---- SPIBAR -------------------------------------------------------------------- */

static uint32_t reg_get(pch_spi *s, uint64_t off)
{
    if (off >= R_FDATA && off < R_FDATA + 64)
        return le32(&s->fdata[off - R_FDATA]);
    if (off >= R_FREG && off < R_FREG + 4 * NREG)
        return s->freg[(off - R_FREG) / 4];
    if (off >= R_FPR && off < R_FPR + 20)
        return s->fpr[(off - R_FPR) / 4];
    switch (off) {
    case R_BFPREG: return (s->bios_base >> 12) | (s->bios_limit >> 12) << 16;
    case R_HSFS:   return s->hsfs | s->hsfc | HS_FDOPSS | (s->fdv ? HS_FDV : 0);
    case R_FADDR:  return s->faddr;
    case R_FRACC:  return 0x021Bu;   /* BIOS master: reads desc/BIOS/GbE/PDR, writes BIOS */
    case R_FDOC:   return s->fdoc;
    case R_FDOD: {
        uint32_t o = s->sect[(s->fdoc >> 12) & 3] + (s->fdoc & 0xFFC);
        return s->fdv && o + 4 <= s->len ? le32(&s->image[o]) : 0xFFFFFFFFu;
    }
    }
    return off + 4 <= STORE_SIZE ? le32(&s->store[off]) : 0;
}

static uam_status regs_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    (void)size;
    *val = reg_get(from_regs(d), off);
    return UAM_OK;
}

static uam_status regs_write_masked(uam_device *d, uint64_t off, unsigned size, uint64_t val,
                                    uint64_t mask)
{
    (void)size;
    pch_spi *s = from_regs(d);
    uint32_t v = (uint32_t)val, m = (uint32_t)mask;
    uint32_t merged = (reg_get(s, off) & ~m) | (v & m);
    if (off >= R_FDATA && off < R_FDATA + 64) {
        for (unsigned i = 0; i < 4; i++)
            s->fdata[off - R_FDATA + i] = (uint8_t)(merged >> (8 * i));
    } else if (off >= R_FPR && off < R_FPR + 20) {
        if (!(s->hsfs & HS_FLOCKDN))
            s->fpr[(off - R_FPR) / 4] = merged;
    } else if (off == R_HSFS) {
        s->hsfs &= ~(v & m & HS_W1C);                 /* write 1 to clear */
        if (v & m & HS_FLOCKDN)
            s->hsfs |= HS_FLOCKDN;                    /* write-once lock */
        s->hsfc = (s->hsfc & ~(m & HC_RW)) | (v & m & HC_RW);
        if (v & m & HC_FGO)
            cycle(s);                                 /* completes now; FGO reads 0 */
    } else if (off == R_FADDR) {
        s->faddr = merged & 0x7FFFFFF;
    } else if (off == R_FDOC) {
        s->fdoc = merged & 0x7FFC;
    } else if (off == R_BFPREG || off == R_FRACC || off == R_FDOD ||
               (off >= R_FREG && off < R_FREG + 4 * NREG)) {
        /* read-only */
    } else if (off + 4 <= STORE_SIZE) {
        for (unsigned i = 0; i < 4; i++)
            s->store[off + i] = (uint8_t)(merged >> (8 * i));
    }
    return UAM_OK;
}

static uam_status regs_write(uam_device *d, uint64_t off, unsigned size, uint64_t val)
{
    return regs_write_masked(d, off, size, val, 0xFFFFFFFFu);
}

/* ---- the BIOS window: reads come from the backing store; writes go nowhere ------- */

static uam_status window_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    pch_spi *s = from_dev(d);
    const uint8_t *mem = uam_bus_romd_backing(s->window);
    uint64_t v = 0;
    for (unsigned i = 0; i < size; i++)
        v |= (uint64_t)mem[off + i] << (8 * i);
    *val = v;
    return UAM_OK;
}

static uam_status window_write(uam_device *d, uint64_t off, unsigned size, uint64_t val)
{
    pch_spi *s = from_dev(d);
    if (!s->warned_window) {
        s->warned_window = true;
        uam_log(UAM_LOG_WARN, "pch-spi", "%s: write of %u bytes (0x%llx) to the BIOS window at +0x%llx "
                "ignored: SPI flash is written through the controller", s->dev.name, size,
                (unsigned long long)val, (unsigned long long)off);
    }
    return UAM_OK;
}

/* ---- PCI function -------------------------------------------------------------- */

static void spi_config_write(uam_pci_func *f, unsigned off, unsigned size, uint32_t val)
{
    (void)val;
    if (off <= BIOS_CNTL && off + size > BIOS_CNTL && (f->cfg[BIOS_CNTL] & BC_LE))
        f->wmask[BIOS_CNTL] &= (uint8_t)~BC_LE;       /* lock enable is write-once */
}

static void spi_func_reset(uam_pci_func *f)
{
    f->wmask[BIOS_CNTL] = 0x23;                       /* WPD, LE, EISS */
}

static const uam_pci_func_ops spi_fn_ops = {
    .config_write = spi_config_write,
    .reset = spi_func_reset,
};

/* ---- device lifecycle ------------------------------------------------------------ */

static void spi_reset(uam_device *d)
{
    pch_spi *s = from_dev(d);
    s->hsfs = s->hsfc = s->faddr = s->fdoc = 0;
    memset(s->fpr, 0, sizeof(s->fpr));
    memset(s->fdata, 0, sizeof(s->fdata));
    memset(s->store, 0, sizeof(s->store));
}

static void spi_destroy(uam_device *d)
{
    pch_spi *s = from_dev(d);
    free(s->image);
    free(s);
}

static void spi_save(uam_device *d, uam_state_writer *w)
{
    pch_spi *s = from_dev(d);
    uam_state_put_u32(w, s->hsfs);
    uam_state_put_u32(w, s->hsfc);
    uam_state_put_u32(w, s->faddr);
    uam_state_put_u32(w, s->fdoc);
    for (unsigned i = 0; i < 5; i++)
        uam_state_put_u32(w, s->fpr[i]);
    uam_state_put_bytes(w, s->fdata, sizeof(s->fdata));
    uam_state_put_bytes(w, s->store, sizeof(s->store));
}

static void spi_restore(uam_device *d, uam_state_reader *r)
{
    pch_spi *s = from_dev(d);
    s->hsfs = uam_state_get_u32(r);
    s->hsfc = uam_state_get_u32(r);
    s->faddr = uam_state_get_u32(r);
    s->fdoc = uam_state_get_u32(r);
    for (unsigned i = 0; i < 5; i++)
        s->fpr[i] = uam_state_get_u32(r);
    uam_state_get_bytes(r, s->fdata, sizeof(s->fdata));
    uam_state_get_bytes(r, s->store, sizeof(s->store));
}

static const uam_device_ops spi_ops = {
    .type = "intel,pch-spi", .impl_min = 1, .impl_max = 8,
    .read = window_read, .write = window_write, .reset = spi_reset, .destroy = spi_destroy,
    .save = spi_save, .restore = spi_restore,
};
static const uam_device_ops regs_ops = {
    .type = "intel,pch-spi-bar", .impl_min = 4, .impl_max = 4,
    .read = regs_read, .write = regs_write, .write_masked = regs_write_masked,
    .flags = UAM_DEVICE_STATELESS,                    /* saved by its owner */
};

/* Regions from the flash descriptor; without one, the whole image is the BIOS region. */
static void parse_descriptor(pch_spi *s)
{
    for (unsigned i = 0; i < NREG; i++)
        s->freg[i] = 0x00007FFF;                      /* unused: base > limit */
    s->bios_base = 0;
    s->bios_limit = (uint32_t)s->len - 1;
    if (s->len < 0x1000 || le32(&s->image[0x10]) != 0x0FF0A55Au)
        goto no_descriptor;
    uint32_t flmap0 = le32(&s->image[0x14]), flmap1 = le32(&s->image[0x18]);
    s->sect[0] = 0x10;
    s->sect[1] = (flmap0 & 0xFF) << 4;
    s->sect[2] = ((flmap0 >> 16) & 0xFF) << 4;
    s->sect[3] = (flmap1 & 0xFF) << 4;
    if (s->sect[2] + 4 * NREG > s->len)
        goto no_descriptor;
    for (unsigned i = 0; i < NREG; i++)
        s->freg[i] = le32(&s->image[s->sect[2] + 4 * i]) & 0x7FFF7FFF;
    uint32_t base = (s->freg[1] & 0x7FFF) << 12, limit = ((s->freg[1] >> 16) & 0x7FFF) << 12 | 0xFFF;
    if (base > limit || limit >= s->len)
        goto no_descriptor;
    s->bios_base = base;
    s->bios_limit = limit;
    s->fdv = true;
    return;
no_descriptor:
    s->fdv = false;
    s->freg[1] = (s->bios_limit >> 12) << 16;
}

uam_device *pch_spi_create(const char *name, uam_bus *sys, const uint8_t *image, size_t len,
                           uint64_t window_top, uint16_t vendor, uint16_t device,
                           const uint8_t jedec_id[3])
{
    if (!len || (len & 0xFFF) || len > 0x8000000)
        return NULL;
    pch_spi *s = calloc(1, sizeof(*s));
    if (!s || !(s->image = malloc(len))) {
        free(s);
        return NULL;
    }
    memcpy(s->image, image, len);
    s->len = len;
    uam_device_init(&s->dev, &spi_ops, name);
    uam_device_init(&s->regs, &regs_ops, name);
    s->sys = sys;
    memcpy(s->jedec, jedec_id, 3);
    parse_descriptor(s);
    uint32_t bios_size = s->bios_limit - s->bios_base + 1;
    if (uam_bus_map_romd(sys, name, window_top - bios_size, bios_size, &s->dev,
                         s->image + s->bios_base, bios_size, NULL, &s->window) != UAM_OK) {
        spi_destroy(&s->dev);
        return NULL;
    }
    uam_pci_func *f = &s->fn;
    uam_pci_func_init(f, name, vendor, device, 0x0C800000, &spi_fn_ops, s);
    uam_pci_func_bar(f, 0, PCH_SPI_BAR_SIZE, UAM_PCI_BAR_MEM, &s->regs);
    spi_func_reset(f);
    return &s->dev;
}

uam_pci_func *pch_spi_func(uam_device *d)   { return &from_dev(d)->fn; }
uam_region   *pch_spi_window(uam_device *d) { return from_dev(d)->window; }
