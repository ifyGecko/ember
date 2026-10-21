/*
 * q35_mch.c - Q35-class memory controller hub, PCI 00:00.0 (uam/dev/q35.h).
 *
 * The MCH decodes nothing itself: it moves and switches overlay regions on
 * the system bus as firmware programs PCIEXBAR, PAM and SMRAM, and as the
 * processor enters and leaves SMM.
 */
#include <stdlib.h>

#include "uam/dev/q35.h"

enum {
    MCH_MCHBAR = 0x48, MCH_GGC = 0x52, MCH_DEVEN = 0x54, MCH_PCIEXBAR = 0x60, MCH_DMIBAR = 0x68,
    MCH_PAM0 = 0x90, MCH_SMRAM = 0x9D, MCH_ESMRAMC = 0x9E, MCH_TOM = 0xA0, MCH_TOUUD = 0xA2,
    MCH_GBSM = 0xA4, MCH_BGSM = 0xA8, MCH_TSEGMB = 0xAC, MCH_TOLUD = 0xB0,
};
enum { SMRAM_D_OPEN = 0x40, SMRAM_D_CLS = 0x20, SMRAM_D_LCK = 0x10, SMRAM_G_SMRAME = 0x08 };
enum { ESMRAMC_H_SMRAME = 0x80, ESMRAMC_T_EN = 0x01 };

#define OVERLAY_PRIO 5   /* above RAM */

/* BIOS-area segments behind PAM: base, size, PAM register, nibble (0 low, 1 high) */
static const struct { uint32_t base, size; uint8_t reg, hi; } pam_seg[] = {
    { 0xE0000, 0x4000, MCH_PAM0 + 5, 0 }, { 0xE4000, 0x4000, MCH_PAM0 + 5, 1 },
    { 0xE8000, 0x4000, MCH_PAM0 + 6, 0 }, { 0xEC000, 0x4000, MCH_PAM0 + 6, 1 },
    { 0xF0000, 0x10000, MCH_PAM0, 1 },
};
#define N_PAM UAM_ARRAY_LEN(pam_seg)

typedef struct mch {
    uam_device     dev;        /* owns the function; also the "nothing here" device */
    uam_pci_func   fn;
    uam_bus       *sys;
    uam_pci_bus   *pci;
    uam_region    *flash;
    uint64_t       low_ram_top;
    uam_region    *ecam;
    uam_region    *pam[N_PAM];
    uam_region    *aseg;
    uam_region    *tseg[3];    /* 1, 2, 8 MiB */
    bool           in_smm;
} mch;

static mch *from_dev(uam_device *d) { return UAM_CONTAINER_OF(d, mch, dev); }
static mch *from_fn(uam_pci_func *f) { return f->ctx; }

/* ---- the "nothing here" device: reads all ones, ignores writes ----------- */

static uam_status none_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    (void)d; (void)off;
    *val = size >= 8 ? ~0ull : (1ull << (8 * size)) - 1;
    return UAM_OK;
}

static uam_status none_write(uam_device *d, uint64_t off, unsigned size, uint64_t v)
{
    (void)d; (void)off; (void)size; (void)v;
    return UAM_OK;
}

/* ---- decode updates ------------------------------------------------------ */

static void update_ecam(mch *m)
{
    uint64_t bar = uam_pci_get(m->fn.cfg, MCH_PCIEXBAR, 4) |
                   (uint64_t)uam_pci_get(m->fn.cfg, MCH_PCIEXBAR + 4, 4) << 32;
    bool en = bar & 1;
    unsigned len = (bar >> 1) & 3;
    uint64_t size = len == 1 ? (128ull << 20) : len == 2 ? (64ull << 20) : (256ull << 20);
    uint64_t base = bar & 0xFFFFFFFFFull & ~(size - 1);   /* bits 35:26..28 */
    if (!m->ecam) {
        if (!en)
            return;
        uam_map_attrs at = { .prio = OVERLAY_PRIO };
        if (uam_bus_map_mmio_ex(m->sys, "mch.ecam", base, size, uam_pci_ecam(m->pci), &at) == UAM_OK)
            m->ecam = uam_bus_region(m->sys, "mch.ecam");
        return;
    }
    if (en)
        uam_bus_region_move(m->sys, m->ecam, base);
    uam_bus_region_enable(m->sys, m->ecam, en);
}

static void update_pam(mch *m)
{
    for (unsigned i = 0; i < N_PAM; i++) {
        if (!m->pam[i])
            continue;
        uint8_t v = m->fn.cfg[pam_seg[i].reg] >> (pam_seg[i].hi ? 4 : 0);
        /* read enable clear: reads go to the DMI side, i.e. the BIOS flash */
        uam_bus_region_enable(m->sys, m->pam[i], !(v & 1));
    }
}

static uint64_t tolud(const mch *m)
{
    return (uint64_t)(uam_pci_get(m->fn.cfg, MCH_TOLUD, 2) >> 4) << 20;
}

static void update_smram(mch *m)
{
    uint8_t smram = m->fn.cfg[MCH_SMRAM], esm = m->fn.cfg[MCH_ESMRAMC];
    bool g = smram & SMRAM_G_SMRAME;
    /* A-segment: RAM in SMM or while open; otherwise legacy video space */
    bool aseg_ram = g && (m->in_smm || (smram & SMRAM_D_OPEN));
    uam_bus_region_enable(m->sys, m->aseg, !aseg_ram);
    /* TSEG: hidden outside SMM */
    unsigned szsel = (esm >> 1) & 3;
    bool tseg_on = g && (esm & ESMRAMC_T_EN) && !m->in_smm && szsel < 3;
    static const uint64_t tsz[3] = { 1u << 20, 2u << 20, 8u << 20 };
    for (unsigned i = 0; i < 3; i++) {
        bool on = tseg_on && i == szsel;
        if (on)
            uam_bus_region_move(m->sys, m->tseg[i], tolud(m) - tsz[i]);
        uam_bus_region_enable(m->sys, m->tseg[i], on);
    }
}

void uam_q35_mch_smm(uam_device *d, bool in_smm)
{
    mch *m = from_dev(d);
    if (m->in_smm == in_smm)
        return;
    m->in_smm = in_smm;
    update_smram(m);
}

/* ---- configuration space ------------------------------------------------- */

static void mch_config_write(uam_pci_func *f, unsigned off, unsigned size, uint32_t val)
{
    (void)val;
    mch *m = from_fn(f);
    unsigned end = off + size;
    if (off < MCH_PCIEXBAR + 8 && end > MCH_PCIEXBAR)
        update_ecam(m);
    if (off < MCH_PAM0 + 7 && end > MCH_PAM0)
        update_pam(m);
    if (off < MCH_SMRAM + 2 && end > MCH_SMRAM) {
        if (f->cfg[MCH_SMRAM] & SMRAM_D_LCK) {
            /* locked: only D_CLS stays writable, and D_OPEN is forced off */
            f->cfg[MCH_SMRAM] &= (uint8_t)~SMRAM_D_OPEN;
            f->wmask[MCH_SMRAM] = SMRAM_D_CLS;
            f->wmask[MCH_ESMRAMC] = 0;
        }
        update_smram(m);
    }
    if (off < MCH_TOLUD + 2 && end > MCH_TOLUD)
        update_smram(m);
}

static void mch_func_reset(uam_pci_func *f)
{
    mch *m = from_fn(f);
    f->wmask[MCH_SMRAM] = 0x78;
    f->wmask[MCH_ESMRAMC] = 0x87;
    update_ecam(m);
    update_pam(m);
    update_smram(m);
}

static const uam_pci_func_ops mch_fn_ops = {
    .config_write = mch_config_write,
    .reset = mch_func_reset,
};

static void mch_destroy(uam_device *d) { free(from_dev(d)); }

static void mch_save(uam_device *d, uam_state_writer *w)
{
    uam_state_put_bool(w, from_dev(d)->in_smm);
}

static void mch_restore(uam_device *d, uam_state_reader *r)
{
    mch *m = from_dev(d);
    m->in_smm = uam_state_get_bool(r);
    /* the PCI bus restored the configuration space; rebuild the decode */
    update_ecam(m);
    update_pam(m);
    update_smram(m);
}

static const uam_device_ops mch_ops = {
    .type = "intel,q35-mch", .read = none_read, .write = none_write,
    .destroy = mch_destroy, .save = mch_save, .restore = mch_restore,
};

uam_device *uam_q35_mch_create(const char *name, const uam_q35_mch_config *cfg)
{
    mch *m = calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    uam_device_init(&m->dev, &mch_ops, name);
    m->sys = cfg->sys;
    m->pci = cfg->pci;
    m->flash = cfg->boot_flash;
    m->low_ram_top = cfg->low_ram_top;

    uam_pci_func *f = &m->fn;
    uam_pci_func_init(f, "mch", 0x8086, UAM_Q35_MCH_DID, 0x06000002, &mch_fn_ops, m);
    uam_pci_set(f->wmask, UAM_PCI_COMMAND, 2, 0);       /* the host bridge decodes nothing */
    uam_pci_set(f->cfg, UAM_PCI_COMMAND, 2, 0x0006);
    uam_pci_set(f->cfg, UAM_PCI_SUBSYS_VENDOR, 2, 0x8086);
    uam_pci_set(f->cfg, UAM_PCI_SUBSYS, 2, UAM_Q35_MCH_DID);
    uam_pci_set(f->wmask, MCH_MCHBAR, 4, 0xFFFFC001);
    uam_pci_set(f->wmask, MCH_MCHBAR + 4, 4, 0xF);
    uam_pci_set(f->wmask, MCH_GGC, 2, 0xFFF2);
    uam_pci_set(f->cfg, MCH_GGC, 2, 0x0030);
    uam_pci_set(f->wmask, MCH_DEVEN, 4, 0x3FB);
    uam_pci_set(f->cfg, MCH_DEVEN, 4, 0x3DB);
    uam_pci_set(f->wmask, MCH_PCIEXBAR, 4, 0xFC000007);
    uam_pci_set(f->wmask, MCH_PCIEXBAR + 4, 4, 0xF);
    uam_pci_set(f->wmask, MCH_DMIBAR, 4, 0xFFFFF001);
    uam_pci_set(f->wmask, MCH_DMIBAR + 4, 4, 0xF);
    f->wmask[MCH_PAM0] = 0x30;
    for (unsigned i = 1; i < 7; i++)
        f->wmask[MCH_PAM0 + i] = 0x33;
    f->cfg[MCH_SMRAM] = 0x02;                            /* C_BASE_SEG: A-segment */
    f->wmask[MCH_SMRAM] = 0x78;
    f->cfg[MCH_ESMRAMC] = 0x38;
    f->wmask[MCH_ESMRAMC] = 0x87;
    uam_pci_set(f->wmask, MCH_TOM, 2, 0xFFFF);
    uam_pci_set(f->wmask, MCH_TOUUD, 2, 0xFFFF);
    uam_pci_set(f->wmask, MCH_GBSM, 4, 0xFFF00000);
    uam_pci_set(f->wmask, MCH_BGSM, 4, 0xFFF00000);
    uam_pci_set(f->wmask, MCH_TSEGMB, 4, 0xFFF00000);
    uam_pci_set(f->wmask, MCH_TOLUD, 2, 0xFFF0);
    uam_pci_set(f->cfg, MCH_TOLUD, 2, (uint32_t)(cfg->low_ram_top >> 20) << 4);
    uam_pci_set(f->cfg, MCH_TSEGMB, 4, (uint32_t)cfg->low_ram_top & 0xFFF00000);

    /* overlays: created disabled, switched by the registers */
    uam_map_attrs at = { .prio = OVERLAY_PRIO, .disabled = true };
    uam_status st = uam_bus_map_mmio_ex(m->sys, "mch.aseg", 0xA0000, 0x20000, &m->dev, &at);
    static const char *const tname[3] = { "mch.tseg1m", "mch.tseg2m", "mch.tseg8m" };
    static const uint64_t tsz[3] = { 1u << 20, 2u << 20, 8u << 20 };
    for (unsigned i = 0; i < 3 && st == UAM_OK; i++)
        st = uam_bus_map_mmio_ex(m->sys, tname[i], cfg->low_ram_top - tsz[i], tsz[i], &m->dev, &at);
    if (st != UAM_OK) {
        free(m);
        return NULL;
    }
    m->aseg = uam_bus_region(m->sys, "mch.aseg");
    for (unsigned i = 0; i < 3; i++)
        m->tseg[i] = uam_bus_region(m->sys, tname[i]);
    if (m->flash && m->flash->size >= 0x20000) {
        static const char *const pname[N_PAM] = { "pam.e0000", "pam.e4000", "pam.e8000",
                                                  "pam.ec000", "pam.f0000" };
        for (unsigned i = 0; i < N_PAM; i++) {
            uint64_t off = m->flash->size - 0x20000 + (pam_seg[i].base - 0xE0000);
            if (uam_bus_map_alias(m->sys, pname[i], pam_seg[i].base, pam_seg[i].size, m->flash,
                                  off, &at) == UAM_OK)
                m->pam[i] = uam_bus_region(m->sys, pname[i]);
        }
    }
    if (uam_pci_register(m->pci, f, 0, 0) != UAM_OK) {
        free(m);
        return NULL;
    }
    mch_func_reset(f);
    return &m->dev;
}
