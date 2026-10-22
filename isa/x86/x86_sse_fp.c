/*
 * x86_sse_fp.c - SSE/SSE2 floating point for the X86_H_SSE helper: packed and
 * scalar single/double arithmetic, square root, reciprocal estimates, min/max,
 * compares (CMPccPS/PD/SS/SD, COMISS/UCOMISS/COMISD/UCOMISD) and the
 * conversions between single, double and integers.
 *
 * Arithmetic runs on the host's IEEE binary32/binary64 in MXCSR's rounding
 * mode; the exception flags it raises are accumulated in MXCSR. Unmasking
 * an exception in MXCSR does not raise #XM, and DAZ/FTZ are not applied.
 */
#include <fenv.h>
#include <math.h>
#include <string.h>

#include "x86.h"

bool x86_sse_fp(uam_cpu *cpu, uint64_t desc, uint64_t lin, uam_status *st);

typedef union fv { float f[4]; double d[2]; uint32_t u[4]; uint64_t q[2]; int32_t i[4]; } fv;

enum { MX_IE = 1u << 0, MX_ZE = 1u << 2, MX_OE = 1u << 3, MX_UE = 1u << 4, MX_PE = 1u << 5 };

static fv get_x(uam_cpu *cpu, unsigned i)
{
    fv v;
    v.q[0] = uam_cpu_reg_read(cpu, X86_OFF_XMM(i), 64);
    v.q[1] = uam_cpu_reg_read(cpu, X86_OFF_XMM(i) + 8, 64);
    return v;
}

static void set_x(uam_cpu *cpu, unsigned i, const fv *v)
{
    uam_cpu_reg_write(cpu, X86_OFF_XMM(i), 64, v->q[0]);
    uam_cpu_reg_write(cpu, X86_OFF_XMM(i) + 8, 64, v->q[1]);
}

static uint32_t mxcsr(uam_cpu *cpu) { return (uint32_t)uam_cpu_reg_read(cpu, X86_OFF_MXCSR, 32); }

/* Enter MXCSR's rounding mode with clear host flags; returns the host mode. */
static int fp_begin(uam_cpu *cpu)
{
    static const int rc[4] = { FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO };
    int old = fegetround();
    fesetround(rc[(mxcsr(cpu) >> 13) & 3]);
    feclearexcept(FE_ALL_EXCEPT);
    return old;
}

static void fp_end(uam_cpu *cpu, int old, uint32_t extra)
{
    int e = fetestexcept(FE_ALL_EXCEPT);
    uint32_t f = extra;
    if (e & FE_INVALID)   f |= MX_IE;
    if (e & FE_DIVBYZERO) f |= MX_ZE;
    if (e & FE_OVERFLOW)  f |= MX_OE;
    if (e & FE_UNDERFLOW) f |= MX_UE;
    if (e & FE_INEXACT)   f |= MX_PE;
    if (f)
        uam_cpu_reg_write(cpu, X86_OFF_MXCSR, 32, mxcsr(cpu) | f);
    fesetround(old);
}

static uam_status load(uam_cpu *cpu, uint64_t desc, uint64_t lin, unsigned bytes, fv *s)
{
    memset(s, 0, sizeof(*s));
    if (!X86_SSE_MEM(desc)) {
        *s = get_x(cpu, X86_SSE_RM(desc));
        return UAM_OK;
    }
    for (unsigned i = 0; i < bytes; i += 8) {
        uint64_t v = 0;
        uam_status r = x86_lin_read(cpu, lin + i, bytes - i >= 8 ? 8 : bytes - i, &v, false);
        if (r != UAM_OK)
            return r;
        s->q[i / 8] = v;
    }
    return UAM_OK;
}

/* ---- element operations ------------------------------------------------- */

static float  minf(float a, float b)   { return a < b ? a : b; }   /* NaN or ±0: the source */
static float  maxf(float a, float b)   { return a > b ? a : b; }
static double mind(double a, double b) { return a < b ? a : b; }
static double maxd(double a, double b) { return a > b ? a : b; }

static float arith_f(unsigned op, float a, float b)
{
    switch (op) {
    case 0x51: return sqrtf(b);
    case 0x52: return 1.0f / sqrtf(b);
    case 0x53: return 1.0f / b;
    case 0x58: return a + b;
    case 0x59: return a * b;
    case 0x5C: return a - b;
    case 0x5D: return minf(a, b);
    case 0x5E: return a / b;
    default:   return maxf(a, b);
    }
}

static double arith_d(unsigned op, double a, double b)
{
    switch (op) {
    case 0x51: return sqrt(b);
    case 0x58: return a + b;
    case 0x59: return a * b;
    case 0x5C: return a - b;
    case 0x5D: return mind(a, b);
    case 0x5E: return a / b;
    default:   return maxd(a, b);
    }
}

/* CMPcc predicate 0-7 (EQ LT LE UNORD NEQ NLT NLE ORD). Ordered compares
 * of a NaN set IE for LT/LE/NLT/NLE; quiet NaNs pass EQ/UNORD/NEQ/ORD. */
static bool cmp_pred(unsigned p, double a, double b, uint32_t *flags)
{
    bool un = isnan(a) || isnan(b);
    if (un && (p == 1 || p == 2 || p == 5 || p == 6))
        *flags |= MX_IE;
    switch (p & 7) {
    case 0: return !un && a == b;
    case 1: return !un && a < b;
    case 2: return !un && a <= b;
    case 3: return un;
    case 4: return un || a != b;
    case 5: return un || !(a < b);
    case 6: return un || !(a <= b);
    default: return !un;
    }
}

static bool snan_f(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return (u & 0x7F800000u) == 0x7F800000u && (u & 0x007FFFFFu) && !(u & 0x00400000u);
}
static bool snan_d(double d)
{
    uint64_t u;
    memcpy(&u, &d, 8);
    return (u & 0x7FF0000000000000ull) == 0x7FF0000000000000ull && (u & 0x000FFFFFFFFFFFFFull) &&
           !(u & 0x0008000000000000ull);
}

/* Double to integer of `bits` bits; out of range or NaN is the integer
 * indefinite value (the most negative integer) and sets IE. */
static int64_t to_int(double v, unsigned bits, bool chop, uint32_t *flags)
{
    double r = chop ? trunc(v) : nearbyint(v);
    double lim = bits == 32 ? 2147483648.0 : 9223372036854775808.0;
    if (isnan(r) || r >= lim || r < -lim) {
        *flags |= MX_IE;
        return bits == 32 ? INT32_MIN : INT64_MIN;
    }
    if (r != v)
        *flags |= MX_PE;
    return (int64_t)r;
}

/* ---- the instructions ---------------------------------------------------- */

#define FAIL(r) do { *st = (r); return true; } while (0)

static void set_cmp_flags(uam_cpu *cpu, double a, double b)
{
    uint32_t f = x86_get_rflags(cpu) & ~(uint32_t)(RFLAGS_CF | RFLAGS_PF | RFLAGS_ZF | RFLAGS_SF |
                                                   RFLAGS_OF | RFLAGS_AF);
    if (isnan(a) || isnan(b)) f |= RFLAGS_ZF | RFLAGS_PF | RFLAGS_CF;
    else if (a < b)           f |= RFLAGS_CF;
    else if (a == b)          f |= RFLAGS_ZF;
    x86_set_rflags(cpu, f);
}

bool x86_sse_fp(uam_cpu *cpu, uint64_t desc, uint64_t lin, uam_status *st)
{
    unsigned op = X86_SSE_OP(desc), pfx = X86_SSE_PFX(desc), reg = X86_SSE_REG(desc);
    unsigned imm = X86_SSE_IMM(desc);
    bool ps = pfx == X86_PFX_NONE, pd = pfx == X86_PFX_66;
    bool ss = pfx == X86_PFX_F3, sd = pfx == X86_PFX_F2;
    fv d = get_x(cpu, reg), s;
    uint32_t extra = 0;
    uam_status r;
    int rm_old;

    switch (op) {
    case 0x51: case 0x58: case 0x59: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
    case 0x52: case 0x53: {                  /* arithmetic */
        if ((op == 0x52 || op == 0x53) && (pd || sd))
            return false;
        unsigned bytes = ss ? 4 : sd ? 8 : 16;
        if ((r = load(cpu, desc, lin, bytes, &s)) != UAM_OK)
            FAIL(r);
        rm_old = fp_begin(cpu);
        if (ps || ss) {
            for (unsigned i = 0; i < (ss ? 1u : 4u); i++) {
                if ((op == 0x5D || op == 0x5F) && (isnan(d.f[i]) || isnan(s.f[i])))
                    extra |= MX_IE;
                d.f[i] = arith_f(op, d.f[i], s.f[i]);
            }
        } else {
            for (unsigned i = 0; i < (sd ? 1u : 2u); i++) {
                if ((op == 0x5D || op == 0x5F) && (isnan(d.d[i]) || isnan(s.d[i])))
                    extra |= MX_IE;
                d.d[i] = arith_d(op, d.d[i], s.d[i]);
            }
        }
        fp_end(cpu, rm_old, extra);
        set_x(cpu, reg, &d);
        return true;
    }
    case 0xC2: {                             /* CMPccPS/PD/SS/SD */
        unsigned bytes = ss ? 4 : sd ? 8 : 16;
        if ((r = load(cpu, desc, lin, bytes, &s)) != UAM_OK)
            FAIL(r);
        rm_old = fp_begin(cpu);
        if (ps || ss) {
            for (unsigned i = 0; i < (ss ? 1u : 4u); i++) {
                if (snan_f(d.f[i]) || snan_f(s.f[i])) extra |= MX_IE;
                d.u[i] = cmp_pred(imm, d.f[i], s.f[i], &extra) ? ~0u : 0;
            }
        } else {
            for (unsigned i = 0; i < (sd ? 1u : 2u); i++) {
                if (snan_d(d.d[i]) || snan_d(s.d[i])) extra |= MX_IE;
                d.q[i] = cmp_pred(imm, d.d[i], s.d[i], &extra) ? ~0ull : 0;
            }
        }
        fp_end(cpu, rm_old, extra);
        set_x(cpu, reg, &d);
        return true;
    }
    case 0x2E: case 0x2F: {                  /* (U)COMISS / (U)COMISD */
        if (!ps && !pd)
            return false;
        if ((r = load(cpu, desc, lin, ps ? 4 : 8, &s)) != UAM_OK)
            FAIL(r);
        double a = ps ? d.f[0] : d.d[0], b = ps ? s.f[0] : s.d[0];
        bool sig = ps ? (snan_f(d.f[0]) || snan_f(s.f[0])) : (snan_d(d.d[0]) || snan_d(s.d[0]));
        if (sig || (op == 0x2F && (isnan(a) || isnan(b))))
            uam_cpu_reg_write(cpu, X86_OFF_MXCSR, 32, mxcsr(cpu) | MX_IE);
        set_cmp_flags(cpu, a, b);
        return true;
    }
    case 0x5A: {                             /* CVTPS2PD CVTPD2PS CVTSS2SD CVTSD2SS */
        if ((r = load(cpu, desc, lin, ps ? 8 : ss ? 4 : sd ? 8 : 16, &s)) != UAM_OK)
            FAIL(r);
        rm_old = fp_begin(cpu);
        if (ps)      { fv o; o.d[0] = s.f[0]; o.d[1] = s.f[1]; d = o; }
        else if (pd) { fv o = { 0 }; o.f[0] = (float)s.d[0]; o.f[1] = (float)s.d[1]; d = o; }
        else if (ss) d.d[0] = s.f[0];
        else         d.f[0] = (float)s.d[0];
        fp_end(cpu, rm_old, 0);
        set_x(cpu, reg, &d);
        return true;
    }
    case 0x5B: {                             /* CVTDQ2PS CVTPS2DQ CVTTPS2DQ */
        if (sd)
            return false;
        if ((r = load(cpu, desc, lin, 16, &s)) != UAM_OK)
            FAIL(r);
        rm_old = fp_begin(cpu);
        for (unsigned i = 0; i < 4; i++) {
            if (ps) d.f[i] = (float)s.i[i];
            else    d.i[i] = (int32_t)to_int(s.f[i], 32, ss, &extra);
        }
        fp_end(cpu, rm_old, extra);
        set_x(cpu, reg, &d);
        return true;
    }
    case 0xE6: {                             /* CVTTPD2DQ CVTPD2DQ CVTDQ2PD */
        if (ps)
            return false;
        if ((r = load(cpu, desc, lin, ss ? 8 : 16, &s)) != UAM_OK)
            FAIL(r);
        rm_old = fp_begin(cpu);
        fv o = { 0 };
        if (ss) {
            o.d[0] = s.i[0];
            o.d[1] = s.i[1];
        } else {
            o.i[0] = (int32_t)to_int(s.d[0], 32, pd, &extra);
            o.i[1] = (int32_t)to_int(s.d[1], 32, pd, &extra);
        }
        fp_end(cpu, rm_old, extra);
        set_x(cpu, reg, &o);
        return true;
    }
    case 0x2A: {                             /* CVTSI2SS / CVTSI2SD */
        if (!ss && !sd)
            return false;                    /* the MMX forms */
        unsigned w = X86_SSE_REXW(desc) ? 8 : 4;
        uint64_t v = 0;
        if (X86_SSE_MEM(desc)) {
            if ((r = x86_lin_read(cpu, lin, w, &v, false)) != UAM_OK)
                FAIL(r);
        } else {
            v = x86_gpr(cpu, X86_SSE_RM(desc));
        }
        int64_t iv = w == 8 ? (int64_t)v : (int64_t)(int32_t)v;
        rm_old = fp_begin(cpu);
        if (ss) d.f[0] = (float)iv;
        else    d.d[0] = (double)iv;
        fp_end(cpu, rm_old, 0);
        set_x(cpu, reg, &d);
        return true;
    }
    case 0x2C: case 0x2D: {                  /* CVT(T)SS2SI / CVT(T)SD2SI */
        if (!ss && !sd)
            return false;
        if ((r = load(cpu, desc, lin, ss ? 4 : 8, &s)) != UAM_OK)
            FAIL(r);
        unsigned bits = X86_SSE_REXW(desc) ? 64 : 32;
        rm_old = fp_begin(cpu);
        int64_t v = to_int(ss ? (double)s.f[0] : s.d[0], bits, op == 0x2C, &extra);
        fp_end(cpu, rm_old, extra);
        x86_set_gpr(cpu, reg, bits == 64 ? (uint64_t)v : (uint32_t)v);
        return true;
    }
    default:
        return false;
    }
}
