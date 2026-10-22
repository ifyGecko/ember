/*
 * mc146818.c - Real-time clock and CMOS RAM (uam/dev/mc146818.h).
 *
 * The clock is a count of seconds derived from virtual time; the time
 * registers are computed from it on every read, in BCD or binary and 12- or
 * 24-hour form as register B selects. While register B's SET bit is on, the
 * clock holds still and writes to the time registers change the held time;
 * clearing SET starts it from there. Register 0x32 reads as the century.
 *
 * UIP is set during the last 244 us of every second. The periodic interrupt
 * (rate from register A), the update-ended interrupt and the alarm interrupt
 * raise IRQF and the line while enabled in register B; reading register C
 * clears them. Daylight-saving adjustment is not implemented.
 */
#include <stdlib.h>
#include <string.h>

#include "uam/dev/clock.h"
#include "uam/dev/mc146818.h"

enum { REG_SEC = 0, REG_SEC_ALARM, REG_MIN, REG_MIN_ALARM, REG_HOUR, REG_HOUR_ALARM,
       REG_WDAY, REG_MDAY, REG_MON, REG_YEAR, REG_A, REG_B, REG_C, REG_D, REG_CENTURY = 0x32 };
enum { B_SET = 0x80, B_PIE = 0x40, B_AIE = 0x20, B_UIE = 0x10, B_DM = 0x04, B_24H = 0x02 };
enum { C_IRQF = 0x80, C_PF = 0x40, C_AF = 0x20, C_UF = 0x10 };

typedef struct rtc {
    uam_device dev;
    uam_device bank1;
    uam_sched *sched;
    uint64_t   tick_hz;
    uam_signal irq;
    uam_timer  periodic, second;
    uint8_t    ram[256];
    uint8_t    index0, index1;
    bool       nmi_masked;
    int64_t    offset;      /* clock = epoch + offset + virtual seconds */
    uint64_t   epoch;
    int64_t    held;        /* the clock while SET is on */
} rtc;

static rtc *from_dev(uam_device *d)   { return UAM_CONTAINER_OF(d, rtc, dev); }
static rtc *from_bank1(uam_device *d) { return UAM_CONTAINER_OF(d, rtc, bank1); }

/* ---- calendar (proleptic Gregorian, days since 1970-01-01) --------------- */

static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t z, int64_t *y, unsigned *m, unsigned *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int64_t)yoe + era * 400 + (*m <= 2);
}

typedef struct tm6 { int64_t year; unsigned mon, mday, wday, hour, min, sec; } tm6;

static tm6 split(int64_t t)
{
    tm6 r;
    int64_t days = t >= 0 ? t / 86400 : -((-t + 86399) / 86400);
    int64_t rem = t - days * 86400;
    civil_from_days(days, &r.year, &r.mon, &r.mday);
    r.wday = (unsigned)(((days % 7) + 11) % 7);   /* 1970-01-01 was a Thursday; 0 = Sunday */
    r.hour = (unsigned)(rem / 3600);
    r.min = (unsigned)(rem / 60 % 60);
    r.sec = (unsigned)(rem % 60);
    return r;
}

static int64_t join(const tm6 *t)
{
    return days_from_civil(t->year, t->mon, t->mday) * 86400 + t->hour * 3600 + t->min * 60 + t->sec;
}

/* ---- clock --------------------------------------------------------------- */

static uint64_t vsecs(rtc *r) { return uam_sched_now(r->sched) / r->tick_hz; }

static int64_t clock_now(rtc *r)
{
    if (r->ram[REG_B] & B_SET)
        return r->held;
    return (int64_t)r->epoch + r->offset + (int64_t)vsecs(r);
}

static uint8_t to_reg(rtc *r, unsigned v)
{
    if (r->ram[REG_B] & B_DM)
        return (uint8_t)v;
    return (uint8_t)((v / 10) << 4 | (v % 10));
}

static unsigned from_reg(rtc *r, uint8_t v)
{
    if (r->ram[REG_B] & B_DM)
        return v;
    return (v >> 4) * 10 + (v & 0xF);
}

static uint8_t hour_reg(rtc *r, unsigned h)
{
    if (r->ram[REG_B] & B_24H)
        return to_reg(r, h);
    unsigned h12 = h % 12 ? h % 12 : 12;
    return (uint8_t)(to_reg(r, h12) | (h >= 12 ? 0x80 : 0));
}

static unsigned hour_from_reg(rtc *r, uint8_t v)
{
    if (r->ram[REG_B] & B_24H)
        return from_reg(r, v);
    unsigned h = from_reg(r, v & 0x7F) % 12;
    return (v & 0x80) ? h + 12 : h;
}

/* ---- interrupts ---------------------------------------------------------- */

static void update_irq(rtc *r)
{
    uint8_t c = r->ram[REG_C] & 0x70;
    if (c & r->ram[REG_B] & 0x70)
        c |= C_IRQF;
    r->ram[REG_C] = c;
    uam_signal_set(&r->irq, (c & C_IRQF) != 0);
}

static uint64_t periodic_ticks(rtc *r)
{
    unsigned rate = r->ram[REG_A] & 0xF;
    if (!rate)
        return 0;
    if (rate < 3)
        rate += 7;
    uint64_t hz = 32768u >> (rate - 1);
    uint64_t t = r->tick_hz / hz;
    return t ? t : 1;
}

static void arm_timers(rtc *r)
{
    uint64_t now = uam_sched_now(r->sched);
    uint64_t p = periodic_ticks(r);
    if ((r->ram[REG_B] & B_PIE) && p)
        uam_timer_arm(r->sched, &r->periodic, (now / p + 1) * p);
    else
        uam_timer_cancel(r->sched, &r->periodic);
    if ((r->ram[REG_B] & (B_UIE | B_AIE)) && !(r->ram[REG_B] & B_SET))
        uam_timer_arm(r->sched, &r->second, (vsecs(r) + 1) * r->tick_hz);
    else
        uam_timer_cancel(r->sched, &r->second);
}

static void periodic_fired(void *ctx)
{
    rtc *r = ctx;
    r->ram[REG_C] |= C_PF;
    update_irq(r);
    arm_timers(r);
}

static bool alarm_match(uint8_t alarm, uint8_t cur)
{
    return (alarm & 0xC0) == 0xC0 || alarm == cur;
}

static void second_fired(void *ctx)
{
    rtc *r = ctx;
    r->ram[REG_C] |= C_UF;
    tm6 t = split(clock_now(r));
    if (alarm_match(r->ram[REG_SEC_ALARM], to_reg(r, t.sec)) &&
        alarm_match(r->ram[REG_MIN_ALARM], to_reg(r, t.min)) &&
        alarm_match(r->ram[REG_HOUR_ALARM], hour_reg(r, t.hour)))
        r->ram[REG_C] |= C_AF;
    update_irq(r);
    arm_timers(r);
}

/* ---- registers ----------------------------------------------------------- */

static uint8_t reg_read(rtc *r, unsigned i)
{
    if (i > REG_YEAR && i != REG_A && i != REG_C && i != REG_CENTURY)
        return r->ram[i];
    tm6 t = split(clock_now(r));
    switch (i) {
    case REG_SEC:  return to_reg(r, t.sec);
    case REG_MIN:  return to_reg(r, t.min);
    case REG_HOUR: return hour_reg(r, t.hour);
    case REG_WDAY: return to_reg(r, t.wday + 1);
    case REG_MDAY: return to_reg(r, t.mday);
    case REG_MON:  return to_reg(r, t.mon);
    case REG_YEAR: return to_reg(r, (unsigned)(t.year % 100));
    case REG_CENTURY: return to_reg(r, (unsigned)(t.year / 100));
    case REG_A: {
        uint64_t frac = uam_sched_now(r->sched) % r->tick_hz;
        bool uip = !(r->ram[REG_B] & B_SET) &&
                   frac >= r->tick_hz - uam_clock_to_ticks(244, r->tick_hz, 1000000);
        return (uint8_t)((r->ram[REG_A] & 0x7F) | (uip ? 0x80 : 0));
    }
    case REG_C: {
        uint8_t v = r->ram[REG_C];
        r->ram[REG_C] = 0;
        update_irq(r);
        return v;
    }
    default:       /* alarms */
        return r->ram[i];
    }
}

static void set_time_field(rtc *r, unsigned i, uint8_t v)
{
    int64_t now = clock_now(r);
    tm6 t = split(now);
    switch (i) {
    case REG_SEC:  t.sec = from_reg(r, v) % 60; break;
    case REG_MIN:  t.min = from_reg(r, v) % 60; break;
    case REG_HOUR: t.hour = hour_from_reg(r, v) % 24; break;
    case REG_MDAY: t.mday = from_reg(r, v) ? from_reg(r, v) : 1; break;
    case REG_MON:  t.mon = from_reg(r, v) && from_reg(r, v) <= 12 ? from_reg(r, v) : 1; break;
    case REG_YEAR: t.year = t.year / 100 * 100 + from_reg(r, v) % 100; break;
    case REG_CENTURY: t.year = (int64_t)from_reg(r, v) * 100 + t.year % 100; break;
    default: return;   /* the day of the week follows from the date */
    }
    int64_t nt = join(&t);
    if (r->ram[REG_B] & B_SET)
        r->held = nt;
    else
        r->offset += nt - now;
}

static void reg_write(rtc *r, unsigned i, uint8_t v)
{
    switch (i) {
    case REG_SEC: case REG_MIN: case REG_HOUR: case REG_MDAY: case REG_MON: case REG_YEAR:
    case REG_CENTURY:
        set_time_field(r, i, v);
        if (i == REG_CENTURY)
            r->ram[i] = v;
        return;
    case REG_WDAY:
        return;
    case REG_A:
        r->ram[REG_A] = v & 0x7F;
        arm_timers(r);
        return;
    case REG_B: {
        bool was_set = r->ram[REG_B] & B_SET;
        if (!was_set && (v & B_SET))
            r->held = clock_now(r);
        r->ram[REG_B] = v;
        if (was_set && !(v & B_SET))
            r->offset = r->held - (int64_t)r->epoch - (int64_t)vsecs(r);
        if (v & B_SET)
            r->ram[REG_B] &= (uint8_t)~B_UIE;
        update_irq(r);
        arm_timers(r);
        return;
    }
    case REG_C: case REG_D:
        return;   /* read-only */
    default:
        r->ram[i] = v;
        return;
    }
}

static uam_status bank0_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    (void)size;
    rtc *r = from_dev(d);
    *val = (off & 1) ? reg_read(r, r->index0) : 0xFF;
    return UAM_OK;
}

static uam_status bank0_write(uam_device *d, uint64_t off, unsigned size, uint64_t v)
{
    (void)size;
    rtc *r = from_dev(d);
    if (off & 1) {
        reg_write(r, r->index0, (uint8_t)v);
    } else {
        r->index0 = v & 0x7F;
        r->nmi_masked = (v & 0x80) != 0;
    }
    return UAM_OK;
}

static uam_status bank1_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    (void)size;
    rtc *r = from_bank1(d);
    *val = (off & 1) ? r->ram[0x80 | r->index1] : 0xFF;
    return UAM_OK;
}

static uam_status bank1_write(uam_device *d, uint64_t off, unsigned size, uint64_t v)
{
    (void)size;
    rtc *r = from_bank1(d);
    if (off & 1)
        r->ram[0x80 | r->index1] = (uint8_t)v;
    else
        r->index1 = v & 0x7F;
    return UAM_OK;
}

static void rtc_reset(uam_device *d)
{
    rtc *r = from_dev(d);
    /* RESET# clears the interrupt enables and flags; the time and RAM survive */
    r->ram[REG_B] &= (uint8_t)~(B_PIE | B_AIE | B_UIE);
    r->ram[REG_C] = 0;
    r->index0 = r->index1 = 0;
    update_irq(r);
    arm_timers(r);
}

static void rtc_destroy(uam_device *d)
{
    rtc *r = from_dev(d);
    uam_timer_cancel(r->sched, &r->periodic);
    uam_timer_cancel(r->sched, &r->second);
    free(r);
}

static void rtc_save(uam_device *d, uam_state_writer *w)
{
    rtc *r = from_dev(d);
    uam_state_put_bytes(w, r->ram, sizeof(r->ram));
    uam_state_put_u8(w, r->index0);
    uam_state_put_u8(w, r->index1);
    uam_state_put_bool(w, r->nmi_masked);
    uam_state_put_u64(w, (uint64_t)r->offset);
    uam_state_put_u64(w, (uint64_t)r->held);
    uam_state_put_timer(w, &r->periodic);
    uam_state_put_timer(w, &r->second);
    uam_state_put_signal(w, &r->irq);
}

static void rtc_restore(uam_device *d, uam_state_reader *rd)
{
    rtc *r = from_dev(d);
    uam_state_get_bytes(rd, r->ram, sizeof(r->ram));
    r->index0 = uam_state_get_u8(rd);
    r->index1 = uam_state_get_u8(rd);
    r->nmi_masked = uam_state_get_bool(rd);
    r->offset = (int64_t)uam_state_get_u64(rd);
    r->held = (int64_t)uam_state_get_u64(rd);
    uam_state_get_timer(rd, &r->periodic);
    uam_state_get_timer(rd, &r->second);
    uam_state_get_signal(rd, &r->irq);
}

static const uam_device_ops rtc_ops = {
    .type = "pc,rtc-cmos", .impl_min = 1, .impl_max = 1,
    .read = bank0_read, .write = bank0_write, .reset = rtc_reset, .destroy = rtc_destroy,
    .save = rtc_save, .restore = rtc_restore,
};
static const uam_device_ops bank1_ops = {
    .type = "pc,rtc-cmos-bank1", .impl_min = 1, .impl_max = 1,
    .read = bank1_read, .write = bank1_write, .flags = UAM_DEVICE_STATELESS,
};

uam_device *uam_mc146818_create(const char *name, uam_sched *sched, uint64_t tick_hz,
                                uint64_t epoch)
{
    rtc *r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    uam_device_init(&r->dev, &rtc_ops, name);
    uam_device_init(&r->bank1, &bank1_ops, name);
    r->sched = sched;
    r->tick_hz = tick_hz ? tick_hz : 1000000000u;
    r->epoch = epoch;
    uam_signal_init(&r->irq);
    uam_timer_init(&r->periodic, periodic_fired, r);
    uam_timer_init(&r->second, second_fired, r);
    r->ram[REG_A] = 0x26;          /* 32.768 kHz time base, 1024 Hz periodic rate */
    r->ram[REG_B] = B_24H;
    r->ram[REG_D] = 0x80;          /* valid RAM and time */
    return &r->dev;
}

uam_device *uam_mc146818_bank0(uam_device *d) { return d; }
uam_device *uam_mc146818_bank1(uam_device *d) { return &from_dev(d)->bank1; }
uam_signal *uam_mc146818_irq_line(uam_device *d) { return &from_dev(d)->irq; }

void uam_mc146818_nvram_set(uam_device *d, unsigned index, uint8_t val)
{
    if (index < 256 && (index > REG_D || index >= 0x80))
        from_dev(d)->ram[index] = val;
}

uint8_t uam_mc146818_nvram_get(uam_device *d, unsigned index)
{
    return index < 256 ? from_dev(d)->ram[index] : 0;
}

bool uam_mc146818_nmi_masked(uam_device *d) { return from_dev(d)->nmi_masked; }
