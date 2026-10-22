/*
 * acpi_pm.c - ACPI fixed-hardware power-management registers
 * (uam/dev/acpi_pm.h).
 *
 * The PM timer is derived from virtual time. TMR_STS is evaluated lazily from
 * the number of top-bit toggles since it was last cleared; while TMR_EN is
 * set a scheduler timer is armed for the next toggle so the SCI is raised on
 * time.
 */
#include <stdlib.h>
#include <string.h>

#include "uam/dev/acpi_pm.h"
#include "uam/dev/clock.h"

#define PM1_STS_MASK 0x8731u
#define PM1_EN_MASK  0x0721u
#define PM1_CNT_MASK 0x1C07u
#define SLP_EN       (1u << 13)

typedef struct acpi_pm {
    uam_device         dev;
    uam_sched         *sched;
    uint64_t           tick_hz;
    uam_acpi_pm_layout lay;
    uam_acpi_pm_hooks  hooks;
    uam_signal         sci;
    uam_timer          timer;
    uint16_t           pm1_sts, pm1_en, pm1_cnt;
    uint64_t           tmr_epoch;   /* toggle count when TMR_STS was last cleared */
    uint8_t            gpe[16];     /* status half, then enable half */
} acpi_pm;

static acpi_pm *from_dev(uam_device *d) { return UAM_CONTAINER_OF(d, acpi_pm, dev); }

static uint64_t cycles(acpi_pm *p)
{
    return uam_clock_from_ticks(uam_sched_now(p->sched), p->tick_hz, UAM_ACPI_PM_TMR_HZ);
}

static unsigned tmr_bits(const acpi_pm *p) { return p->lay.tmr_32bit ? 32 : 24; }

/* Number of times the timer's top bit has toggled. */
static uint64_t toggles(acpi_pm *p) { return cycles(p) >> (tmr_bits(p) - 1); }

static uint16_t pm1_sts(acpi_pm *p)
{
    uint16_t s = p->pm1_sts;
    if (toggles(p) != p->tmr_epoch)
        s |= UAM_ACPI_PM1_TMR;
    return s;
}

static bool gpe_pending(const acpi_pm *p)
{
    unsigned half = p->lay.gpe0_len / 2;
    for (unsigned i = 0; i < half; i++)
        if (p->gpe[i] & p->gpe[half + i])
            return true;
    return false;
}

static void update(acpi_pm *p)
{
    bool ev = (pm1_sts(p) & p->pm1_en & PM1_EN_MASK) || gpe_pending(p);
    uam_signal_set(&p->sci, (p->pm1_cnt & 1) && ev);
    uam_timer_cancel(p->sched, &p->timer);
    if ((p->pm1_en & UAM_ACPI_PM1_TMR) && !(pm1_sts(p) & UAM_ACPI_PM1_TMR)) {
        uint64_t next = (p->tmr_epoch + 1) << (tmr_bits(p) - 1);
        uam_timer_arm(p->sched, &p->timer, uam_clock_to_ticks(next, p->tick_hz, UAM_ACPI_PM_TMR_HZ));
    }
}

static void timer_fired(void *ctx) { update(ctx); }

uint32_t uam_acpi_pm_timer(uam_device *d)
{
    acpi_pm *p = from_dev(d);
    uint64_t c = cycles(p);
    return p->lay.tmr_32bit ? (uint32_t)c : (uint32_t)c & 0xFFFFFF;
}

void uam_acpi_pm_event(uam_device *d, uint16_t bits)
{
    acpi_pm *p = from_dev(d);
    p->pm1_sts |= bits & PM1_STS_MASK & ~UAM_ACPI_PM1_TMR;
    update(p);
}

void uam_acpi_pm_gpe(void *ctx, unsigned bit, int level)
{
    acpi_pm *p = from_dev(ctx);
    if (bit >= (p->lay.gpe0_len / 2) * 8 || !level)
        return;
    p->gpe[bit / 8] |= (uint8_t)(1u << (bit % 8));
    update(p);
}

/* ---- registers ----------------------------------------------------------- */

static bool in(uint32_t base, uint32_t len, uint64_t off)
{
    return base != UAM_ACPI_PM_ABSENT && off >= base && off < (uint64_t)base + len;
}

static uint64_t bytes_of(uint64_t reg, uint64_t at, unsigned size)
{
    return (reg >> (8 * at)) & uam_mask(size * 8);
}

static uam_status pm_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    acpi_pm *p = from_dev(d);
    const uam_acpi_pm_layout *l = &p->lay;
    if (in(l->pm1_evt, 4, off)) {
        uint32_t evt = pm1_sts(p) | (uint32_t)p->pm1_en << 16;
        *val = bytes_of(evt, off - l->pm1_evt, size);
    } else if (in(l->pm1_cnt, 2, off)) {
        *val = bytes_of(p->pm1_cnt, off - l->pm1_cnt, size);
    } else if (in(l->pm_tmr, 4, off)) {
        *val = bytes_of(uam_acpi_pm_timer(d), off - l->pm_tmr, size);
    } else if (in(l->gpe0, l->gpe0_len, off)) {
        uint64_t v = 0;
        for (unsigned i = 0; i < size && off - l->gpe0 + i < l->gpe0_len; i++)
            v |= (uint64_t)p->gpe[off - l->gpe0 + i] << (8 * i);
        *val = v;
    } else if (p->hooks.read) {
        return p->hooks.read(p->hooks.ctx, off, size, val);
    } else {
        *val = 0;
    }
    return UAM_OK;
}

static uam_status pm_write(uam_device *d, uint64_t off, unsigned size, uint64_t v)
{
    acpi_pm *p = from_dev(d);
    const uam_acpi_pm_layout *l = &p->lay;
    if (in(l->pm1_evt, 4, off)) {
        unsigned at = (unsigned)(off - l->pm1_evt);
        uint32_t w = (uint32_t)((v & uam_mask(size * 8)) << (8 * at));
        uint32_t m = (uint32_t)(uam_mask(size * 8) << (8 * at));
        uint16_t clr = (uint16_t)(w & PM1_STS_MASK);
        p->pm1_sts &= (uint16_t)~clr;
        if (clr & UAM_ACPI_PM1_TMR)
            p->tmr_epoch = toggles(p);
        if (m >> 16)
            p->pm1_en = (uint16_t)(((p->pm1_en & ~(m >> 16)) | (w >> 16)) & PM1_EN_MASK);
    } else if (in(l->pm1_cnt, 2, off)) {
        unsigned at = (unsigned)(off - l->pm1_cnt);
        uint16_t w = (uint16_t)((v & uam_mask(size * 8)) << (8 * at));
        uint16_t m = (uint16_t)(uam_mask(size * 8) << (8 * at));
        p->pm1_cnt = (uint16_t)(((p->pm1_cnt & ~m) | (w & m)) & PM1_CNT_MASK);
        if (w & m & SLP_EN) {
            unsigned typ = (p->pm1_cnt >> 10) & 7;
            if (p->hooks.sleep)
                p->hooks.sleep(p->hooks.ctx, typ);
        }
    } else if (in(l->pm_tmr, 4, off)) {
        /* read-only */
    } else if (in(l->gpe0, l->gpe0_len, off)) {
        unsigned half = l->gpe0_len / 2;
        for (unsigned i = 0; i < size && off - l->gpe0 + i < l->gpe0_len; i++) {
            unsigned b = (unsigned)(off - l->gpe0) + i;
            uint8_t x = (uint8_t)(v >> (8 * i));
            if (b < half) p->gpe[b] &= (uint8_t)~x;
            else          p->gpe[b] = x;
        }
    } else if (p->hooks.write) {
        return p->hooks.write(p->hooks.ctx, off, size, v);
    }
    update(p);
    return UAM_OK;
}

static void pm_reset(uam_device *d)
{
    acpi_pm *p = from_dev(d);
    p->pm1_sts = p->pm1_en = p->pm1_cnt = 0;
    memset(p->gpe, 0, sizeof(p->gpe));
    p->tmr_epoch = toggles(p);
    update(p);
}

static void pm_destroy(uam_device *d)
{
    acpi_pm *p = from_dev(d);
    uam_timer_cancel(p->sched, &p->timer);
    free(p);
}

static void pm_save(uam_device *d, uam_state_writer *w)
{
    acpi_pm *p = from_dev(d);
    uam_state_put_u16(w, p->pm1_sts);
    uam_state_put_u16(w, p->pm1_en);
    uam_state_put_u16(w, p->pm1_cnt);
    uam_state_put_u64(w, p->tmr_epoch);
    uam_state_put_bytes(w, p->gpe, sizeof(p->gpe));
    uam_state_put_timer(w, &p->timer);
    uam_state_put_signal(w, &p->sci);
}

static void pm_restore(uam_device *d, uam_state_reader *r)
{
    acpi_pm *p = from_dev(d);
    p->pm1_sts = uam_state_get_u16(r);
    p->pm1_en = uam_state_get_u16(r);
    p->pm1_cnt = uam_state_get_u16(r);
    p->tmr_epoch = uam_state_get_u64(r);
    uam_state_get_bytes(r, p->gpe, sizeof(p->gpe));
    uam_state_get_timer(r, &p->timer);
    uam_state_get_signal(r, &p->sci);
}

static const uam_device_ops pm_ops = {
    .type = "acpi,pm-registers", .read = pm_read, .write = pm_write, .reset = pm_reset,
    .destroy = pm_destroy, .save = pm_save, .restore = pm_restore,
};

uam_device *uam_acpi_pm_create(const char *name, uam_sched *sched, uint64_t tick_hz,
                               const uam_acpi_pm_layout *layout, const uam_acpi_pm_hooks *hooks)
{
    if (layout->gpe0 != UAM_ACPI_PM_ABSENT && (layout->gpe0_len > 16 || (layout->gpe0_len & 1)))
        return NULL;
    acpi_pm *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    uam_device_init(&p->dev, &pm_ops, name);
    p->sched = sched;
    p->tick_hz = tick_hz ? tick_hz : 1000000000u;
    p->lay = *layout;
    if (p->lay.gpe0 == UAM_ACPI_PM_ABSENT)
        p->lay.gpe0_len = 0;
    if (hooks)
        p->hooks = *hooks;
    uam_signal_init(&p->sci);
    uam_timer_init(&p->timer, timer_fired, p);
    return &p->dev;
}

uam_signal *uam_acpi_pm_sci_line(uam_device *d) { return &from_dev(d)->sci; }
