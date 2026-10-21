/*
 * ich9_lpc.c - ICH9 LPC bridge, PCI 00:1f.0: ACPI I/O block, SMI, APM ports,
 * PIRQ routing, RCBA and reset control (uam/dev/q35.h).
 *
 * SMI#: asserted when GBL_SMI_EN is set and an enabled SMI source has its
 * status bit set (APM_CNT writes with APMC_EN, sleep requests with
 * SLP_SMI_EN). Asserting it clears SMI_EN.EOS; no further SMI is generated
 * until the handler writes EOS = 1, when a still-pending source asserts
 * SMI# again.
 */
#include <stdlib.h>
#include <string.h>

#include "uam/dev/clock.h"
#include "uam/dev/q35.h"

enum {
    LPC_PMBASE = 0x40, LPC_ACPI_CNTL = 0x44, LPC_GPIOBASE = 0x48, LPC_GC = 0x4C,
    LPC_PIRQA = 0x60, LPC_PIRQE = 0x68, LPC_IOD = 0x80, LPC_GEN1_DEC = 0x84,
    LPC_GEN_PMCON_1 = 0xA0, LPC_BIOS_CNTL = 0xDC, LPC_RCBA = 0xF0,
};
#define ACPI_EN 0x80

/* ACPI I/O block offsets */
enum {
    PM1_STS = 0x00, PM1_EN = 0x02, PM1_CNT = 0x04, PM1_TMR = 0x08, GPE0_STS = 0x20,
    GPE0_EN = 0x28, SMI_EN = 0x30, SMI_STS = 0x34,
};
enum { SMI_GBL = 1u << 0, SMI_EOS = 1u << 1, SMI_SLP = 1u << 4, SMI_APMC = 1u << 5 };
#define PM_SIZE   0x80u
#define RCRB_SIZE 0x4000u

typedef struct lpc {
    uam_device   dev;          /* owns everything; also the ACPI I/O block */
    uam_device   apm_view, rcrb_view;
    uam_pci_func fn;
    uam_bus     *sys;
    uam_sched   *sched;
    uint64_t     tick_hz, io_base;
    uam_signal   smi, sci, reset;
    uam_region  *pm_region, *rcrb_region;

    uint16_t     pm1_sts, pm1_en;
    uint32_t     pm1_cnt, smi_en, smi_sts;
    uint64_t     gpe0_sts, gpe0_en;
    uint8_t      pm_other[PM_SIZE];   /* registers kept as storage (TCO, ...) */
    uint8_t      apm_cnt, apm_sts;
    bool         smi_active;          /* asserted since EOS was last set */
    uint8_t      pirq_lines;
    uint16_t     irq_levels;          /* the 8259 inputs we drive */
    uint8_t      rcrb[RCRB_SIZE];

    void       (*poweroff)(void *ctx);
    void        *poweroff_ctx;
    void       (*pic)(void *, unsigned, int);
    void        *pic_ctx;
    void       (*ioapic)(void *, unsigned, int);
    void        *ioapic_ctx;
} lpc;

static lpc *from_dev(uam_device *d)  { return UAM_CONTAINER_OF(d, lpc, dev); }
static lpc *from_apm(uam_device *d)  { return UAM_CONTAINER_OF(d, lpc, apm_view); }
static lpc *from_rcrb(uam_device *d) { return UAM_CONTAINER_OF(d, lpc, rcrb_view); }
static lpc *from_fn(uam_pci_func *f) { return f->ctx; }

/* ---- SMI and SCI --------------------------------------------------------- */

static void update_smi(lpc *l)
{
    uint32_t src = l->smi_sts & l->smi_en & (SMI_APMC | SMI_SLP);
    bool want = (l->smi_en & SMI_GBL) && src;
    if (want && !l->smi_active) {
        l->smi_active = true;
        l->smi_en &= ~SMI_EOS;
        uam_signal_set(&l->smi, 1);
        uam_signal_set(&l->smi, 0);   /* the processor takes SMI# on its edge */
    }
}

static void update_sci(lpc *l)
{
    bool sci_en = l->pm1_cnt & 1;
    bool ev = (l->pm1_sts & l->pm1_en) || (l->gpe0_sts & l->gpe0_en);
    uam_signal_set(&l->sci, sci_en && ev);
}

/* ---- ACPI I/O block ------------------------------------------------------ */

static uint32_t pm_timer(lpc *l)
{
    return (uint32_t)uam_clock_from_ticks(uam_sched_now(l->sched), l->tick_hz, UAM_ICH9_PM_HZ)
           & 0xFFFFFF;
}

static uint64_t field(uint64_t reg, unsigned roff, uint64_t off, unsigned size)
{
    return (reg >> (8 * (off - roff))) & uam_mask(size * 8);
}

static uam_status pm_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    lpc *l = from_dev(d);
    if (off >= PM_SIZE) {
        *val = 0;
        return UAM_OK;
    }
    if (off < 2)                       *val = field(l->pm1_sts, PM1_STS, off, size);
    else if (off < 4)                  *val = field(l->pm1_en, PM1_EN, off, size);
    else if (off < 8)                  *val = field(l->pm1_cnt, PM1_CNT, off, size);
    else if (off < 12)                 *val = field(pm_timer(l), PM1_TMR, off, size);
    else if (off >= 0x20 && off < 0x28) *val = field(l->gpe0_sts, GPE0_STS, off, size);
    else if (off >= 0x28 && off < 0x30) *val = field(l->gpe0_en, GPE0_EN, off, size);
    else if (off >= 0x30 && off < 0x34) *val = field(l->smi_en, SMI_EN, off, size);
    else if (off >= 0x34 && off < 0x38) *val = field(l->smi_sts, SMI_STS, off, size);
    else {
        uint64_t v = 0;
        for (unsigned i = 0; i < size && off + i < PM_SIZE; i++)
            v |= (uint64_t)l->pm_other[off + i] << (8 * i);
        *val = v;
    }
    return UAM_OK;
}

static uint64_t merge(uint64_t reg, unsigned roff, uint64_t off, unsigned size, uint64_t v)
{
    unsigned sh = 8 * (unsigned)(off - roff);
    uint64_t m = uam_mask(size * 8) << sh;
    return (reg & ~m) | ((v << sh) & m);
}

static uint64_t w1c(uint64_t reg, unsigned roff, uint64_t off, unsigned size, uint64_t v)
{
    unsigned sh = 8 * (unsigned)(off - roff);
    return reg & ~((v & uam_mask(size * 8)) << sh);
}

static void sleep_request(lpc *l)
{
    unsigned typ = (l->pm1_cnt >> 10) & 7;
    l->pm1_cnt &= ~(1u << 13);   /* SLP_EN reads as 0 */
    if (l->smi_en & SMI_SLP) {   /* firmware traps the sleep request in SMM */
        l->smi_sts |= SMI_SLP;
        update_smi(l);
        return;
    }
    if (typ == 7 || typ == 6 || typ == 5) {   /* S5 soft off; S4/S3 are not modelled: off too */
        if (typ != 7)
            uam_log(UAM_LOG_WARN, "ich9", "S%u sleep is not modelled: powering off", typ == 6 ? 4u : 3u);
        if (l->poweroff)
            l->poweroff(l->poweroff_ctx);
    }
}

static uam_status pm_write(uam_device *d, uint64_t off, unsigned size, uint64_t v)
{
    lpc *l = from_dev(d);
    if (off >= PM_SIZE)
        return UAM_OK;
    if (off < 2) {
        l->pm1_sts = (uint16_t)w1c(l->pm1_sts, PM1_STS, off, size, v);
    } else if (off < 4) {
        l->pm1_en = (uint16_t)merge(l->pm1_en, PM1_EN, off, size, v);
    } else if (off < 8) {
        l->pm1_cnt = (uint32_t)merge(l->pm1_cnt, PM1_CNT, off, size, v) & 0x3C07;
        if (l->pm1_cnt & (1u << 13))
            sleep_request(l);
    } else if (off < 12) {
        /* the PM timer is read-only */
    } else if (off >= 0x20 && off < 0x28) {
        l->gpe0_sts = w1c(l->gpe0_sts, GPE0_STS, off, size, v);
    } else if (off >= 0x28 && off < 0x30) {
        l->gpe0_en = merge(l->gpe0_en, GPE0_EN, off, size, v);
    } else if (off >= 0x30 && off < 0x34) {
        uint32_t nv = (uint32_t)merge(l->smi_en, SMI_EN, off, size, v);
        bool eos = nv & SMI_EOS;
        l->smi_en = nv & ~SMI_EOS;
        if (eos) {               /* end of SMI: another may be generated */
            l->smi_en |= SMI_EOS;
            l->smi_active = false;
        }
        update_smi(l);
    } else if (off >= 0x34 && off < 0x38) {
        l->smi_sts = (uint32_t)w1c(l->smi_sts, SMI_STS, off, size, v);
    } else {
        for (unsigned i = 0; i < size && off + i < PM_SIZE; i++)
            l->pm_other[off + i] = (uint8_t)(v >> (8 * i));
    }
    update_sci(l);
    return UAM_OK;
}

/* ---- APM ports (0xB2 APM_CNT, 0xB3 APM_STS) ------------------------------ */

static uam_status apm_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    (void)size;
    lpc *l = from_apm(d);
    *val = off ? l->apm_sts : l->apm_cnt;
    return UAM_OK;
}

static uam_status apm_write(uam_device *d, uint64_t off, unsigned size, uint64_t v)
{
    (void)size;
    lpc *l = from_apm(d);
    if (off) {
        l->apm_sts = (uint8_t)v;
        return UAM_OK;
    }
    l->apm_cnt = (uint8_t)v;
    if (l->smi_en & SMI_APMC) {
        l->smi_sts |= SMI_APMC;
        update_smi(l);
    }
    return UAM_OK;
}

/* ---- root complex register block (storage) ------------------------------- */

static uam_status rcrb_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    lpc *l = from_rcrb(d);
    uint64_t v = 0;
    for (unsigned i = 0; i < size && off + i < RCRB_SIZE; i++)
        v |= (uint64_t)l->rcrb[off + i] << (8 * i);
    *val = v;
    return UAM_OK;
}

static uam_status rcrb_write(uam_device *d, uint64_t off, unsigned size, uint64_t v)
{
    lpc *l = from_rcrb(d);
    for (unsigned i = 0; i < size && off + i < RCRB_SIZE; i++)
        l->rcrb[off + i] = (uint8_t)(v >> (8 * i));
    return UAM_OK;
}

/* ---- PIRQ routing --------------------------------------------------------- */

static void update_irqs(lpc *l)
{
    uint16_t lv = 0;
    for (unsigned p = 0; p < 8; p++) {
        if (!(l->pirq_lines & (1u << p)))
            continue;
        uint8_t r = l->fn.cfg[(p < 4 ? LPC_PIRQA : LPC_PIRQE) + (p & 3)];
        if (!(r & 0x80))
            lv |= (uint16_t)(1u << (r & 0xF));
    }
    uint16_t changed = lv ^ l->irq_levels;
    l->irq_levels = lv;
    for (unsigned irq = 0; irq < 16 && l->pic; irq++)
        if (changed & (1u << irq))
            l->pic(l->pic_ctx, irq, (lv >> irq) & 1);
}

void uam_ich9_pirq(void *ctx, unsigned pirq, int level)
{
    lpc *l = from_dev(ctx);
    if (pirq > 7)
        return;
    if (level) l->pirq_lines |= (uint8_t)(1u << pirq);
    else       l->pirq_lines &= (uint8_t)~(1u << pirq);
    if (l->ioapic)
        l->ioapic(l->ioapic_ctx, 16 + pirq, level);
    update_irqs(l);
}

/* ---- configuration space ------------------------------------------------- */

static void update_pm_decode(lpc *l)
{
    uint32_t base = uam_pci_get(l->fn.cfg, LPC_PMBASE, 4) & 0xFF80;
    bool on = (l->fn.cfg[LPC_ACPI_CNTL] & ACPI_EN) && base;
    if (!l->pm_region) {
        if (!on)
            return;
        if (uam_bus_map_mmio(l->sys, "ich9.acpi", l->io_base + base, PM_SIZE, &l->dev) == UAM_OK)
            l->pm_region = uam_bus_region(l->sys, "ich9.acpi");
        return;
    }
    if (on)
        uam_bus_region_move(l->sys, l->pm_region, l->io_base + base);
    uam_bus_region_enable(l->sys, l->pm_region, on);
}

static void update_rcba(lpc *l)
{
    uint32_t r = uam_pci_get(l->fn.cfg, LPC_RCBA, 4);
    bool on = r & 1;
    uint64_t base = r & 0xFFFFC000u;
    if (!l->rcrb_region) {
        if (!on)
            return;
        if (uam_bus_map_mmio(l->sys, "ich9.rcrb", base, RCRB_SIZE, &l->rcrb_view) == UAM_OK)
            l->rcrb_region = uam_bus_region(l->sys, "ich9.rcrb");
        return;
    }
    if (on)
        uam_bus_region_move(l->sys, l->rcrb_region, base);
    uam_bus_region_enable(l->sys, l->rcrb_region, on);
}

static void lpc_config_write(uam_pci_func *f, unsigned off, unsigned size, uint32_t val)
{
    (void)val;
    lpc *l = from_fn(f);
    unsigned end = off + size;
    if ((off < LPC_PMBASE + 4 && end > LPC_PMBASE) || (off <= LPC_ACPI_CNTL && end > LPC_ACPI_CNTL))
        update_pm_decode(l);
    if (off < LPC_RCBA + 4 && end > LPC_RCBA)
        update_rcba(l);
    if ((off < LPC_PIRQA + 4 && end > LPC_PIRQA) || (off < LPC_PIRQE + 4 && end > LPC_PIRQE))
        update_irqs(l);
}

static void lpc_func_reset(uam_pci_func *f)
{
    lpc *l = from_fn(f);
    update_pm_decode(l);
    update_rcba(l);
    update_irqs(l);
}

static const uam_pci_func_ops lpc_fn_ops = {
    .config_write = lpc_config_write,
    .reset = lpc_func_reset,
};

void uam_ich9_reset_control(void *ctx, uint8_t val)
{
    lpc *l = from_dev(ctx);
    if (val & 0x04) {            /* RST_CPU: a reset of the kind SYS_RST selects */
        uam_signal_set(&l->reset, 1);
        uam_signal_set(&l->reset, 0);
    }
}

/* ---- the device ---------------------------------------------------------- */

static void lpc_reset(uam_device *d)
{
    lpc *l = from_dev(d);
    l->pm1_sts = l->pm1_en = 0;
    l->pm1_cnt = 0;
    l->gpe0_sts = l->gpe0_en = 0;
    l->smi_en = l->smi_sts = 0;
    l->smi_active = false;
    memset(l->pm_other, 0, sizeof(l->pm_other));
    l->apm_cnt = l->apm_sts = 0;
    memset(l->rcrb, 0, sizeof(l->rcrb));
    uam_signal_set(&l->sci, 0);
    uam_signal_set(&l->reset, 0);
}

static void lpc_destroy(uam_device *d) { free(from_dev(d)); }

static void lpc_save(uam_device *d, uam_state_writer *w)
{
    lpc *l = from_dev(d);
    uam_state_put_u16(w, l->pm1_sts);
    uam_state_put_u16(w, l->pm1_en);
    uam_state_put_u32(w, l->pm1_cnt);
    uam_state_put_u32(w, l->smi_en);
    uam_state_put_u32(w, l->smi_sts);
    uam_state_put_u64(w, l->gpe0_sts);
    uam_state_put_u64(w, l->gpe0_en);
    uam_state_put_bytes(w, l->pm_other, sizeof(l->pm_other));
    uam_state_put_u8(w, l->apm_cnt);
    uam_state_put_u8(w, l->apm_sts);
    uam_state_put_bool(w, l->smi_active);
    uam_state_put_u8(w, l->pirq_lines);
    uam_state_put_u16(w, l->irq_levels);
    uam_state_put_bytes(w, l->rcrb, sizeof(l->rcrb));
    uam_state_put_signal(w, &l->smi);
    uam_state_put_signal(w, &l->sci);
    uam_state_put_signal(w, &l->reset);
}

static void lpc_restore(uam_device *d, uam_state_reader *r)
{
    lpc *l = from_dev(d);
    l->pm1_sts = uam_state_get_u16(r);
    l->pm1_en = uam_state_get_u16(r);
    l->pm1_cnt = uam_state_get_u32(r);
    l->smi_en = uam_state_get_u32(r);
    l->smi_sts = uam_state_get_u32(r);
    l->gpe0_sts = uam_state_get_u64(r);
    l->gpe0_en = uam_state_get_u64(r);
    uam_state_get_bytes(r, l->pm_other, sizeof(l->pm_other));
    l->apm_cnt = uam_state_get_u8(r);
    l->apm_sts = uam_state_get_u8(r);
    l->smi_active = uam_state_get_bool(r);
    l->pirq_lines = uam_state_get_u8(r);
    l->irq_levels = uam_state_get_u16(r);
    uam_state_get_bytes(r, l->rcrb, sizeof(l->rcrb));
    uam_state_get_signal(r, &l->smi);
    uam_state_get_signal(r, &l->sci);
    uam_state_get_signal(r, &l->reset);
    update_pm_decode(l);
    update_rcba(l);
}

static const uam_device_ops lpc_ops = {
    .type = "intel,ich9-lpc", .read = pm_read, .write = pm_write, .reset = lpc_reset,
    .destroy = lpc_destroy, .save = lpc_save, .restore = lpc_restore,
};
static const uam_device_ops apm_ops = {
    .type = "intel,ich9-apm", .impl_min = 1, .impl_max = 1,
    .read = apm_read, .write = apm_write, .flags = UAM_DEVICE_STATELESS,
};
static const uam_device_ops rcrb_ops = {
    .type = "intel,ich9-rcrb", .read = rcrb_read, .write = rcrb_write,
    .flags = UAM_DEVICE_STATELESS,
};

uam_device *uam_ich9_lpc_create(const char *name, uam_pci_bus *pci, uam_bus *sys,
                                uam_sched *sched, uint64_t tick_hz, uint64_t io_base)
{
    lpc *l = calloc(1, sizeof(*l));
    if (!l)
        return NULL;
    uam_device_init(&l->dev, &lpc_ops, name);
    uam_device_init(&l->apm_view, &apm_ops, name);
    uam_device_init(&l->rcrb_view, &rcrb_ops, name);
    l->sys = sys;
    l->sched = sched;
    l->tick_hz = tick_hz ? tick_hz : 1000000000u;
    l->io_base = io_base;
    uam_signal_init(&l->smi);
    uam_signal_init(&l->sci);
    uam_signal_init(&l->reset);

    uam_pci_func *f = &l->fn;
    uam_pci_func_init(f, "lpc", 0x8086, UAM_ICH9_LPC_DID, 0x06010002, &lpc_fn_ops, l);
    f->cfg[UAM_PCI_HEADER] = 0x80;                       /* multi-function device */
    uam_pci_set(f->cfg, UAM_PCI_COMMAND, 2, 0x0007);
    uam_pci_set(f->cfg, UAM_PCI_SUBSYS_VENDOR, 2, 0x8086);
    uam_pci_set(f->cfg, UAM_PCI_SUBSYS, 2, UAM_ICH9_LPC_DID);
    uam_pci_set(f->cfg, LPC_PMBASE, 4, 1);               /* I/O space indicator */
    uam_pci_set(f->wmask, LPC_PMBASE, 4, 0xFF80);
    f->wmask[LPC_ACPI_CNTL] = 0x87;
    uam_pci_set(f->cfg, LPC_GPIOBASE, 4, 1);
    uam_pci_set(f->wmask, LPC_GPIOBASE, 4, 0xFFC0);
    f->wmask[LPC_GC] = 0x11;
    for (unsigned i = 0; i < 4; i++) {
        f->cfg[LPC_PIRQA + i] = f->cfg[LPC_PIRQE + i] = 0x80;   /* routing disabled */
        f->wmask[LPC_PIRQA + i] = f->wmask[LPC_PIRQE + i] = 0x8F;
    }
    uam_pci_set(f->wmask, LPC_IOD, 4, 0x3F7F1377);
    for (unsigned i = 0; i < 16; i++)
        f->wmask[LPC_GEN1_DEC + i] = 0xFF;
    uam_pci_set(f->wmask, LPC_GEN_PMCON_1, 4, 0x0000FFFF);
    uam_pci_set(f->wmask, LPC_GEN_PMCON_1 + 4, 4, 0x0000FFFF);
    f->wmask[LPC_BIOS_CNTL] = 0x3B;
    f->cfg[LPC_BIOS_CNTL] = 0x02 | 0x08;                 /* BLE, TSS defaults */
    uam_pci_set(f->wmask, LPC_RCBA, 4, 0xFFFFC001);

    if (uam_pci_register(pci, f, 31, 0) != UAM_OK) {
        free(l);
        return NULL;
    }
    return &l->dev;
}

uam_device *uam_ich9_apm(uam_device *d)       { return &from_dev(d)->apm_view; }
uam_signal *uam_ich9_smi_line(uam_device *d)  { return &from_dev(d)->smi; }
uam_signal *uam_ich9_sci_line(uam_device *d)  { return &from_dev(d)->sci; }
uam_signal *uam_ich9_reset_line(uam_device *d) { return &from_dev(d)->reset; }

void uam_ich9_set_poweroff(uam_device *d, void (*fn)(void *ctx), void *ctx)
{
    lpc *l = from_dev(d);
    l->poweroff = fn;
    l->poweroff_ctx = ctx;
}

void uam_ich9_set_irq_targets(uam_device *d, void (*pic)(void *, unsigned, int), void *pic_ctx,
                              void (*ioapic)(void *, unsigned, int), void *ioapic_ctx)
{
    lpc *l = from_dev(d);
    l->pic = pic;
    l->pic_ctx = pic_ctx;
    l->ioapic = ioapic;
    l->ioapic_ctx = ioapic_ctx;
}
