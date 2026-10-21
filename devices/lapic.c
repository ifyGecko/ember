/*
 * lapic.c - x86 local APIC, xAPIC MMIO interface (uam/dev/apic.h).
 *
 * Implemented: ID, version, TPR/PPR, EOI (with the EOI broadcast for
 * level-triggered vectors), LDR/DFR, SVR, ISR/TMR/IRR, ESR, ICR (fixed IPIs
 * and NMIs to itself; INIT and SIPI to other processors are dropped: there
 * are none), the LVT with the timer in one-shot and periodic mode, and LINT0
 * and LINT1. While the APIC is disabled in IA32_APIC_BASE or in the SVR,
 * LINT0 and LINT1 act as the processor's INTR and NMI pins, so 8259
 * interrupts reach the processor before firmware programs virtual-wire mode.
 * Not implemented: x2APIC, TSC-deadline mode, the thermal/performance/CMCI
 * sources, arbitration and logical-destination filtering.
 */
#include <stdlib.h>
#include <string.h>

#include "uam/dev/apic.h"
#include "uam/dev/clock.h"

enum {
    R_ID = 0x02, R_VER = 0x03, R_TPR = 0x08, R_APR = 0x09, R_PPR = 0x0A, R_EOI = 0x0B,
    R_RRD = 0x0C, R_LDR = 0x0D, R_DFR = 0x0E, R_SVR = 0x0F, R_ISR = 0x10, R_TMR = 0x18,
    R_IRR = 0x20, R_ESR = 0x28, R_CMCI = 0x2F, R_ICR_LO = 0x30, R_ICR_HI = 0x31,
    R_LVT_TIMER = 0x32, R_LVT_THERM = 0x33, R_LVT_PERF = 0x34, R_LVT_LINT0 = 0x35,
    R_LVT_LINT1 = 0x36, R_LVT_ERR = 0x37, R_TIMER_INIT = 0x38, R_TIMER_CUR = 0x39,
    R_TIMER_DIV = 0x3E,
};
#define LVT_MASKED (1u << 16)
#define LVT_LEVEL  (1u << 15)
#define SVR_ENABLE (1u << 8)

enum { LVT_TIMER = 0, LVT_THERM, LVT_PERF, LVT_LINT0, LVT_LINT1, LVT_ERR, LVT_CMCI, N_LVT };

typedef struct lapic {
    uam_device dev;
    uam_sched *sched;
    uint64_t   tick_hz, bus_hz;
    uam_signal intr, nmi, smi;
    uam_timer  timer;
    bool       hw_enabled;
    uint8_t    id;
    uint32_t   tpr, ldr, dfr, svr, esr, icr_lo, icr_hi;
    uint32_t   lvt[N_LVT];
    uint32_t   irr[8], isr[8], tmr[8];
    uint32_t   timer_init, timer_div;
    uint64_t   timer_start;     /* bus cycle the count (re)started */
    bool       timer_running;
    bool       lint[2];         /* pin levels */
    int      (*extint)(void *ctx);
    void      *extint_ctx;
    uam_apic_eoi_fn eoi_fn;
    void      *eoi_ctx;
} lapic;

static lapic *from_dev(uam_device *d) { return UAM_CONTAINER_OF(d, lapic, dev); }

static bool sw_enabled(const lapic *a) { return (a->svr & SVR_ENABLE) != 0; }
static bool enabled(const lapic *a)    { return a->hw_enabled && sw_enabled(a); }

static int highest_bit(const uint32_t v[8])
{
    for (int i = 7; i >= 0; i--)
        if (v[i])
            return i * 32 + 31 - __builtin_clz(v[i]);
    return -1;
}

static bool test_bit(const uint32_t v[8], unsigned n) { return (v[n >> 5] >> (n & 31)) & 1; }
static void set_bit(uint32_t v[8], unsigned n)        { v[n >> 5] |= 1u << (n & 31); }
static void clear_bit(uint32_t v[8], unsigned n)      { v[n >> 5] &= ~(1u << (n & 31)); }

static uint32_t ppr(const lapic *a)
{
    int isrv = highest_bit(a->isr);
    uint32_t isr_class = isrv < 0 ? 0 : (uint32_t)isrv & 0xF0;
    return (a->tpr & 0xF0) >= isr_class ? a->tpr & 0xFF : isr_class;
}

/* The vector the processor would take, or -1. */
static int deliverable(const lapic *a)
{
    int v = highest_bit(a->irr);
    if (v < 0 || ((uint32_t)v & 0xF0) <= (ppr(a) & 0xF0))
        return -1;
    return v;
}

/* LINT0 passes the 8259 straight to INTR. */
static bool lint0_extint(const lapic *a)
{
    if (!enabled(a))
        return true;
    uint32_t l = a->lvt[LVT_LINT0];
    return !(l & LVT_MASKED) && ((l >> 8) & 7) == UAM_APIC_EXTINT;
}

static void update(lapic *a)
{
    bool ext = lint0_extint(a) && a->lint[0];
    uam_signal_set(&a->intr, ext || deliverable(a) >= 0);
}

static void accept(lapic *a, uint8_t vector, bool level)
{
    if (vector < 16) {
        a->esr |= 1u << 6;   /* received illegal vector */
        return;
    }
    set_bit(a->irr, vector);
    if (level) set_bit(a->tmr, vector);
    else       clear_bit(a->tmr, vector);
    update(a);
}

static void pulse(uam_signal *s)
{
    uam_signal_set(s, 0);
    uam_signal_set(s, 1);
    uam_signal_set(s, 0);
}

void uam_lapic_deliver(void *ctx, uint8_t vector, unsigned mode, bool level, uint8_t dest,
                       bool logical)
{
    lapic *a = from_dev(ctx);
    (void)dest; (void)logical;   /* one processor: every message is for us */
    switch (mode) {
    case UAM_APIC_FIXED: case UAM_APIC_LOWEST:
        if (enabled(a))
            accept(a, vector, level);
        break;
    case UAM_APIC_NMI:
        pulse(&a->nmi);
        break;
    case UAM_APIC_SMI:
        pulse(&a->smi);
        break;
    case UAM_APIC_EXTINT:
        update(a);
        break;
    default:   /* INIT / SIPI: not modelled for the boot processor */
        break;
    }
}

/* An LVT entry fires (timer, LINT pins). */
static void lvt_fire(lapic *a, unsigned idx)
{
    uint32_t l = a->lvt[idx];
    if (!enabled(a) || (l & LVT_MASKED))
        return;
    unsigned mode = (l >> 8) & 7;
    if (idx == LVT_TIMER)
        mode = UAM_APIC_FIXED;
    switch (mode) {
    case UAM_APIC_FIXED:
        accept(a, (uint8_t)l, idx == LVT_LINT0 || idx == LVT_LINT1 ? (l & LVT_LEVEL) != 0 : false);
        break;
    case UAM_APIC_NMI:
        pulse(&a->nmi);
        break;
    case UAM_APIC_SMI:
        pulse(&a->smi);
        break;
    default:
        update(a);
        break;
    }
}

void uam_lapic_lint(void *ctx, unsigned pin, int level)
{
    lapic *a = from_dev(ctx);
    if (pin > 1)
        return;
    bool rising = level && !a->lint[pin];
    a->lint[pin] = level != 0;
    if (!enabled(a)) {
        if (pin == 1 && rising)
            pulse(&a->nmi);   /* the processor's NMI pin */
        update(a);
        return;
    }
    if (pin == 0 && lint0_extint(a)) {
        update(a);
        return;
    }
    uint32_t l = a->lvt[pin ? LVT_LINT1 : LVT_LINT0];
    if (rising || ((l & LVT_LEVEL) && level))
        lvt_fire(a, pin ? LVT_LINT1 : LVT_LINT0);
}

int uam_lapic_inta(uam_device *d)
{
    lapic *a = from_dev(d);
    int v = deliverable(a);
    if (v >= 0) {
        clear_bit(a->irr, (unsigned)v);
        set_bit(a->isr, (unsigned)v);
        update(a);
        return v;
    }
    if (lint0_extint(a) && a->lint[0] && a->extint) {
        int ev = a->extint(a->extint_ctx);
        update(a);
        return ev;
    }
    update(a);
    return (int)(a->svr & 0xFF);   /* spurious */
}

static void eoi(lapic *a)
{
    int v = highest_bit(a->isr);
    if (v < 0)
        return;
    clear_bit(a->isr, (unsigned)v);
    if (test_bit(a->tmr, (unsigned)v) && !(a->svr & (1u << 12)) && a->eoi_fn)
        a->eoi_fn(a->eoi_ctx, (uint8_t)v);
    update(a);
}

/* ---- timer --------------------------------------------------------------- */

static unsigned divisor(const lapic *a)
{
    unsigned v = (a->timer_div & 3) | ((a->timer_div & 8) >> 1);
    return 1u << ((v + 1) & 7);
}

static uint64_t bus_now(lapic *a)
{
    return uam_clock_from_ticks(uam_sched_now(a->sched), a->tick_hz, a->bus_hz);
}

static uint32_t timer_current(lapic *a)
{
    if (!a->timer_running || !a->timer_init)
        return 0;
    uint64_t counts = (bus_now(a) - a->timer_start) / divisor(a);
    bool periodic = ((a->lvt[LVT_TIMER] >> 17) & 3) == 1;
    if (periodic)
        return a->timer_init - (uint32_t)(counts % a->timer_init);
    return counts >= a->timer_init ? 0 : a->timer_init - (uint32_t)counts;
}

static void arm_timer(lapic *a)
{
    uam_timer_cancel(a->sched, &a->timer);
    if (!a->timer_running || !a->timer_init)
        return;
    uint64_t period = (uint64_t)a->timer_init * divisor(a);
    uint64_t done = bus_now(a) - a->timer_start;
    uint64_t next = a->timer_start + (done / period + 1) * period;
    uam_timer_arm(a->sched, &a->timer, uam_clock_to_ticks(next, a->tick_hz, a->bus_hz));
}

static void timer_fired(void *ctx)
{
    lapic *a = ctx;
    bool periodic = ((a->lvt[LVT_TIMER] >> 17) & 3) == 1;
    lvt_fire(a, LVT_TIMER);
    if (periodic)
        arm_timer(a);
    else
        a->timer_running = false;
}

/* ---- registers ----------------------------------------------------------- */

static uint32_t lvt_mask(unsigned idx)
{
    switch (idx) {
    case LVT_TIMER: return 0x000710FF;   /* vector, mask, mode (2 bits) */
    case LVT_LINT0: case LVT_LINT1: return 0x0001A7FF;
    case LVT_ERR:   return 0x000100FF;
    default:        return 0x000107FF;
    }
}

static int lvt_index(unsigned r)
{
    if (r == R_CMCI) return LVT_CMCI;
    if (r >= R_LVT_TIMER && r <= R_LVT_ERR) return (int)(r - R_LVT_TIMER);
    return -1;
}

static uint32_t reg_read(lapic *a, unsigned r)
{
    int li = lvt_index(r);
    if (li >= 0)
        return a->lvt[li] | (sw_enabled(a) ? 0 : LVT_MASKED);
    if (r >= R_ISR && r < R_ISR + 8) return a->isr[r - R_ISR];
    if (r >= R_TMR && r < R_TMR + 8) return a->tmr[r - R_TMR];
    if (r >= R_IRR && r < R_IRR + 8) return a->irr[r - R_IRR];
    switch (r) {
    case R_ID:         return (uint32_t)a->id << 24;
    case R_VER:        return 0x00050014;   /* 6 LVT entries (CMCI not counted), version 0x14 */
    case R_TPR:        return a->tpr;
    case R_APR:        return 0;
    case R_PPR:        return ppr(a);
    case R_LDR:        return a->ldr;
    case R_DFR:        return a->dfr;
    case R_SVR:        return a->svr;
    case R_ESR:        return a->esr;
    case R_ICR_LO:     return a->icr_lo & ~(1u << 12);   /* delivery is immediate */
    case R_ICR_HI:     return a->icr_hi;
    case R_TIMER_INIT: return a->timer_init;
    case R_TIMER_CUR:  return timer_current(a);
    case R_TIMER_DIV:  return a->timer_div;
    default:           return 0;
    }
}

static void send_ipi(lapic *a)
{
    unsigned mode = (a->icr_lo >> 8) & 7, shorthand = (a->icr_lo >> 18) & 3;
    uint8_t dest = (uint8_t)(a->icr_hi >> 24);
    bool to_self = shorthand == 1 || shorthand == 2 ||
                   (shorthand == 0 && (dest == a->id || dest == 0xFF));
    if (!to_self || mode == UAM_APIC_INIT || mode == UAM_APIC_SIPI)
        return;
    uam_lapic_deliver(&a->dev, (uint8_t)a->icr_lo, mode, false, dest, false);
}

static void reg_write(lapic *a, unsigned r, uint32_t v)
{
    int li = lvt_index(r);
    if (li >= 0) {
        a->lvt[li] = v & lvt_mask((unsigned)li);
        if (!sw_enabled(a))
            a->lvt[li] |= LVT_MASKED;
        if (li == LVT_TIMER)
            arm_timer(a);
        update(a);
        return;
    }
    switch (r) {
    case R_TPR: a->tpr = v & 0xFF; update(a); break;
    case R_EOI: eoi(a); break;
    case R_LDR: a->ldr = v & 0xFF000000u; break;
    case R_DFR: a->dfr = v | 0x0FFFFFFFu; break;
    case R_SVR:
        a->svr = v & 0x11FF;
        if (!sw_enabled(a))
            for (unsigned i = 0; i < N_LVT; i++)
                a->lvt[i] |= LVT_MASKED;
        update(a);
        break;
    case R_ESR: a->esr = 0; break;
    case R_ICR_HI: a->icr_hi = v & 0xFF000000u; break;
    case R_ICR_LO: a->icr_lo = v & 0x000CDFFF; send_ipi(a); break;
    case R_TIMER_INIT:
        a->timer_init = v;
        a->timer_start = bus_now(a);
        a->timer_running = v != 0;
        arm_timer(a);
        break;
    case R_TIMER_DIV: {
        uint32_t cur = timer_current(a);
        a->timer_div = v & 0xB;
        /* keep the current count: restart from it at the new rate */
        if (a->timer_running && a->timer_init) {
            uint64_t done = (uint64_t)(a->timer_init - cur) * divisor(a);
            a->timer_start = bus_now(a) - done;
            arm_timer(a);
        }
        break;
    }
    default: break;
    }
}

static uam_status lapic_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    (void)size;
    lapic *a = from_dev(d);
    *val = (off & 0xF) ? 0 : reg_read(a, (unsigned)(off >> 4) & 0xFF);
    return UAM_OK;
}

static uam_status lapic_write(uam_device *d, uint64_t off, unsigned size, uint64_t v)
{
    (void)size;
    lapic *a = from_dev(d);
    if (!(off & 0xF))
        reg_write(a, (unsigned)(off >> 4) & 0xFF, (uint32_t)v);
    return UAM_OK;
}

static void lapic_reset(uam_device *d)
{
    lapic *a = from_dev(d);
    bool lint0 = a->lint[0], lint1 = a->lint[1];
    a->tpr = a->ldr = a->esr = a->icr_lo = a->icr_hi = 0;
    a->dfr = 0xFFFFFFFFu;
    a->svr = 0xFF;
    for (unsigned i = 0; i < N_LVT; i++)
        a->lvt[i] = LVT_MASKED;
    memset(a->irr, 0, sizeof(a->irr));
    memset(a->isr, 0, sizeof(a->isr));
    memset(a->tmr, 0, sizeof(a->tmr));
    a->timer_init = a->timer_div = 0;
    a->timer_running = false;
    a->hw_enabled = true;
    a->lint[0] = lint0;
    a->lint[1] = lint1;
    uam_timer_cancel(a->sched, &a->timer);
    update(a);
}

static void lapic_destroy(uam_device *d)
{
    lapic *a = from_dev(d);
    uam_timer_cancel(a->sched, &a->timer);
    free(a);
}

static void lapic_save(uam_device *d, uam_state_writer *w)
{
    lapic *a = from_dev(d);
    const uint32_t r[] = { a->tpr, a->ldr, a->dfr, a->svr, a->esr, a->icr_lo, a->icr_hi,
                           a->timer_init, a->timer_div };
    for (size_t i = 0; i < UAM_ARRAY_LEN(r); i++) uam_state_put_u32(w, r[i]);
    for (unsigned i = 0; i < N_LVT; i++) uam_state_put_u32(w, a->lvt[i]);
    for (unsigned i = 0; i < 8; i++) {
        uam_state_put_u32(w, a->irr[i]);
        uam_state_put_u32(w, a->isr[i]);
        uam_state_put_u32(w, a->tmr[i]);
    }
    uam_state_put_u64(w, a->timer_start);
    uam_state_put_bool(w, a->timer_running);
    uam_state_put_bool(w, a->hw_enabled);
    uam_state_put_bool(w, a->lint[0]);
    uam_state_put_bool(w, a->lint[1]);
    uam_state_put_timer(w, &a->timer);
    uam_state_put_signal(w, &a->intr);
    uam_state_put_signal(w, &a->nmi);
    uam_state_put_signal(w, &a->smi);
}

static void lapic_restore(uam_device *d, uam_state_reader *rd)
{
    lapic *a = from_dev(d);
    uint32_t *const r[] = { &a->tpr, &a->ldr, &a->dfr, &a->svr, &a->esr, &a->icr_lo, &a->icr_hi,
                            &a->timer_init, &a->timer_div };
    for (size_t i = 0; i < UAM_ARRAY_LEN(r); i++) *r[i] = uam_state_get_u32(rd);
    for (unsigned i = 0; i < N_LVT; i++) a->lvt[i] = uam_state_get_u32(rd);
    for (unsigned i = 0; i < 8; i++) {
        a->irr[i] = uam_state_get_u32(rd);
        a->isr[i] = uam_state_get_u32(rd);
        a->tmr[i] = uam_state_get_u32(rd);
    }
    a->timer_start = uam_state_get_u64(rd);
    a->timer_running = uam_state_get_bool(rd);
    a->hw_enabled = uam_state_get_bool(rd);
    a->lint[0] = uam_state_get_bool(rd);
    a->lint[1] = uam_state_get_bool(rd);
    uam_state_get_timer(rd, &a->timer);
    uam_state_get_signal(rd, &a->intr);
    uam_state_get_signal(rd, &a->nmi);
    uam_state_get_signal(rd, &a->smi);
}

static const uam_device_ops lapic_ops = {
    .type = "intel,xapic", .impl_min = 4, .impl_max = 4,
    .read = lapic_read, .write = lapic_write, .reset = lapic_reset, .destroy = lapic_destroy,
    .save = lapic_save, .restore = lapic_restore,
};

uam_device *uam_lapic_create(const char *name, uam_sched *sched, uint64_t tick_hz,
                             uint64_t bus_hz, uint8_t apic_id)
{
    lapic *a = calloc(1, sizeof(*a));
    if (!a)
        return NULL;
    uam_device_init(&a->dev, &lapic_ops, name);
    a->sched = sched;
    a->tick_hz = tick_hz ? tick_hz : 1000000000u;
    a->bus_hz = bus_hz ? bus_hz : 100000000u;
    a->id = apic_id;
    uam_signal_init(&a->intr);
    uam_signal_init(&a->nmi);
    uam_signal_init(&a->smi);
    uam_timer_init(&a->timer, timer_fired, a);
    lapic_reset(&a->dev);
    return &a->dev;
}

uam_signal *uam_lapic_intr_line(uam_device *d) { return &from_dev(d)->intr; }
uam_signal *uam_lapic_nmi_line(uam_device *d)  { return &from_dev(d)->nmi; }
uam_signal *uam_lapic_smi_line(uam_device *d)  { return &from_dev(d)->smi; }

void uam_lapic_set_extint(uam_device *d, int (*inta)(void *ctx), void *ctx)
{
    lapic *a = from_dev(d);
    a->extint = inta;
    a->extint_ctx = ctx;
}

void uam_lapic_set_eoi_hook(uam_device *d, uam_apic_eoi_fn fn, void *ctx)
{
    lapic *a = from_dev(d);
    a->eoi_fn = fn;
    a->eoi_ctx = ctx;
}

void uam_lapic_set_enabled(uam_device *d, bool on)
{
    lapic *a = from_dev(d);
    if (a->hw_enabled == on)
        return;
    a->hw_enabled = on;
    if (!on) {   /* disabling resets the APIC to its power-on register state */
        bool l0 = a->lint[0], l1 = a->lint[1];
        lapic_reset(d);
        a->hw_enabled = false;
        a->lint[0] = l0;
        a->lint[1] = l1;
    }
    update(a);
}
