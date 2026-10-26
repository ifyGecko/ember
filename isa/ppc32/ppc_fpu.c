/*
 * ppc_fpu.c - The PowerPC floating-point unit: arithmetic, multiply-add,
 * rounding to single, conversions to integer, compares, estimates, fsel, and the
 * FPSCR instructions, with the FPSCR's exception, status and result-class bits.
 *
 * Arithmetic runs on the host's IEEE binary64 in FPSCR[RN]'s rounding mode, with
 * the architecture's own rules where hosts differ: NaN propagation (frA, then frB,
 * then frC, quieted; the default NaN is 0x7FF8000000000000), the specific invalid
 * operation bits, tininess detected before rounding, FPSCR[FR] (fraction rounded
 * up) from a second, truncating evaluation. Single-precision results are computed
 * with round-to-odd in double and then rounded once to single, which gives the
 * correctly rounded single result for every operation, multiply-add included.
 *
 * An enabled exception (FPSCR[FEX]) with MSR[FE0,FE1] != 0 takes the program
 * exception precisely (the imprecise modes are allowed to be precise); with VE or
 * ZE set the target register is left unchanged. Not modelled: the exponent
 * adjustment of enabled overflow and underflow results (OE, UE), and the
 * non-IEEE mode (NI).
 */
#include <fenv.h>
#include <float.h>
#include <math.h>
#include <string.h>

#include "ppc.h"

/* FPSCR bits. */
#define FX     0x80000000u
#define FEX    0x40000000u
#define VX     0x20000000u
#define OX     0x10000000u
#define UX     0x08000000u
#define ZX     0x04000000u
#define XX     0x02000000u
#define VXSNAN 0x01000000u
#define VXISI  0x00800000u
#define VXIDI  0x00400000u
#define VXZDZ  0x00200000u
#define VXIMZ  0x00100000u
#define VXVC   0x00080000u
#define FR     0x00040000u
#define FI     0x00020000u
#define FPRF_SHIFT 12
#define FPRF   (0x1Fu << FPRF_SHIFT)
#define FPCC   (0xFu << FPRF_SHIFT)
#define VXSOFT 0x00000400u
#define VXSQRT 0x00000200u
#define VXCVI  0x00000100u
#define VE     0x00000080u
#define OE     0x00000040u
#define UE     0x00000020u
#define ZE     0x00000010u
#define XE     0x00000008u

#define VX_ALL (VXSNAN | VXISI | VXIDI | VXZDZ | VXIMZ | VXVC | VXSOFT | VXSQRT | VXCVI)
#define EXC_ALL (OX | UX | ZX | XX | VX_ALL)   /* bits whose 0 -> 1 change sets FX */

#define D_SIGN  (UINT64_C(1) << 63)
#define D_QUIET (UINT64_C(1) << 51)
#define D_EXP   UINT64_C(0x7FF0000000000000)
#define D_FRAC  UINT64_C(0x000FFFFFFFFFFFFF)
#define D_DEFAULT_NAN UINT64_C(0x7FF8000000000000)

static double dbl(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }
static uint64_t bits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }

static bool is_nan(uint64_t u)  { return (u & D_EXP) == D_EXP && (u & D_FRAC); }
static bool is_snan(uint64_t u) { return is_nan(u) && !(u & D_QUIET); }
static bool is_inf(uint64_t u)  { return (u & ~D_SIGN) == D_EXP; }
static bool is_zero(uint64_t u) { return !(u & ~D_SIGN); }

/* ---- single <-> double, as lfs and stfs move them ---------------------- */

uint64_t ppc_sp_to_dp(uint32_t s)
{
    uint64_t sign = (uint64_t)(s >> 31) << 63;
    uint32_t e = (s >> 23) & 0xFF, f = s & 0x7FFFFF;
    if (e == 0xFF)                       /* infinity or NaN, payload kept (SNaN stays SNaN) */
        return sign | D_EXP | ((uint64_t)f << 29);
    if (e)
        return sign | ((uint64_t)(e - 127 + 1023) << 52) | ((uint64_t)f << 29);
    if (!f)
        return sign;
    int ex = -126;                       /* denormal: normalise */
    while (!(f & 0x800000)) {
        f <<= 1;
        ex--;
    }
    return sign | ((uint64_t)(ex + 1023) << 52) | ((uint64_t)(f & 0x7FFFFF) << 29);
}

uint32_t ppc_dp_to_sp(uint64_t d)
{
    unsigned e = (unsigned)((d >> 52) & 0x7FF);
    if (e > 896 || e == 0)               /* in single range, infinity, NaN or zero: bits moved */
        return (uint32_t)((d >> 32) & 0xC0000000u) | (uint32_t)((d >> 29) & 0x3FFFFFFFu);
    /* Below single's normal range: denormalise (truncating). */
    uint64_t frac = (d & D_FRAC) | (UINT64_C(1) << 52);
    int ex = (int)e - 1023;
    while (ex < -126 && frac) {
        frac >>= 1;
        ex++;
    }
    return (uint32_t)((d >> 32) & 0x80000000u) | (uint32_t)((frac >> 29) & 0x7FFFFF);
}

uint64_t ppc_helper_sp2dp(void *env, uint64_t a, uint64_t b, uint64_t c, uam_status *st)
{
    (void)env; (void)b; (void)c; (void)st;
    return ppc_sp_to_dp((uint32_t)a);
}

uint64_t ppc_helper_dp2sp(void *env, uint64_t a, uint64_t b, uint64_t c, uam_status *st)
{
    (void)env; (void)b; (void)c; (void)st;
    return ppc_dp_to_sp(a);
}

/* ---- the FPSCR ---------------------------------------------------------- */

/* Recompute the summary bits VX and FEX. */
static uint32_t summarise(uint32_t f)
{
    f = (f & VX_ALL) ? (f | VX) : (f & ~VX);
    bool fex = ((f & VX) && (f & VE)) || ((f & OX) && (f & OE)) || ((f & UX) && (f & UE))
            || ((f & ZX) && (f & ZE)) || ((f & XX) && (f & XE));
    return fex ? (f | FEX) : (f & ~FEX);
}

/* Set exception bits, FX for any that was clear. */
static uint32_t raise(uint32_t f, uint32_t exc)
{
    if (exc & ~f & EXC_ALL)
        f |= FX;
    return f | exc;
}

/* Result class (FPRF) of a double, or of a single when `single`. */
static uint32_t fprf_of(uint64_t u, bool single)
{
    bool neg = u >> 63;
    if (is_nan(u))  return 0x11;
    if (is_inf(u))  return neg ? 0x09 : 0x05;
    if (is_zero(u)) return neg ? 0x12 : 0x02;
    bool denorm = single ? fabs(dbl(u)) < FLT_MIN : !(u & D_EXP);
    if (denorm)     return neg ? 0x18 : 0x14;
    return neg ? 0x08 : 0x04;
}

static const int host_rm[4] = { FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD };

/* ---- arithmetic ---------------------------------------------------------- */

enum fop { F_ADD, F_SUB, F_MUL, F_DIV, F_SQRT, F_MADD, F_MSUB, F_RES, F_RSQRTE };

/* Every operation is evaluated in the rounding mode current at the call: the
 * operands and the result pass through volatile objects so that the compiler can
 * neither fold nor merge evaluations made in different modes. */
static double compute(enum fop op, double a_, double b_, double c_)
{
    volatile double a = a_, b = b_, c = c_, r = 0;
    switch (op) {
    case F_ADD:    r = a + b; break;
    case F_SUB:    r = a - b; break;
    case F_MUL:    r = a * c; break;
    case F_DIV:    r = a / b; break;
    case F_SQRT:   r = sqrt(b); break;
    case F_MADD:   r = fma(a, c, b); break;
    case F_MSUB:   r = fma(a, c, -b); break;
    case F_RES:    r = 1.0 / b; break;
    case F_RSQRTE: r = 1.0 / sqrt(b); break;
    }
    return r;
}

static float to_single(double x_)
{
    volatile double x = x_;
    volatile float f = (float)x;
    return f;
}

typedef struct fres {
    uint64_t r;        /* result bits (a double) */
    uint32_t exc;      /* exception bits raised */
    bool     fr, fi;
    bool     suppress; /* enabled invalid / zero-divide: frD is not written */
} fres;

/* Round a double result (or a NaN) to single precision, as frsp does. */
static fres round_single(uint64_t x, uint32_t fpscr)
{
    const int rm = fpscr & 3;
    fres o = { 0 };
    if (is_nan(x)) {
        if (is_snan(x))
            o.exc |= VXSNAN;
        o.r = (x | D_QUIET) & ~UINT64_C(0x1FFFFFFF);
        return o;
    }
    fesetround(FE_TOWARDZERO);
    float tz = to_single(dbl(x));
    feclearexcept(FE_ALL_EXCEPT);
    fesetround(host_rm[rm]);
    float f = to_single(dbl(x));
    int e = fetestexcept(FE_ALL_EXCEPT);
    o.fi = e & FE_INEXACT;
    o.fr = o.fi && fabsf(f) != fabsf(tz);
    if (e & FE_OVERFLOW)
        o.exc |= OX;
    bool tiny = !is_zero(x) && fabs(dbl(x)) < FLT_MIN;   /* before rounding */
    if (tiny && (o.fi || (fpscr & UE)))
        o.exc |= UX;
    if (o.fi)
        o.exc |= XX;
    o.r = bits((double)f);
    return o;
}

/*
 * One arithmetic operation on the bits of frA, frB, frC (`use` says which take part:
 * 1 A, 2 B, 4 C). NaN operands, invalid operations and zero divides follow the
 * architecture; the rest is the host's IEEE arithmetic.
 */
static fres arith(enum fop op, uint64_t A, uint64_t B, uint64_t C, unsigned use, bool single,
                  uint32_t fpscr)
{
    fres o = { 0 };
    const int rm = fpscr & 3;
    const uint64_t in[3] = { A, B, C };

    /* NaN operands: the first of A, B, C, quieted. */
    for (unsigned k = 0; k < 3; k++)
        if ((use & (1u << k)) && is_snan(in[k]))
            o.exc |= VXSNAN;
    for (unsigned k = 0; k < 3; k++) {
        if ((use & (1u << k)) && is_nan(in[k])) {
            o.r = in[k] | D_QUIET;
            if (single)
                o.r &= ~UINT64_C(0x1FFFFFFF);
            o.suppress = (o.exc & VXSNAN) && (fpscr & VE);
            return o;
        }
    }

    /* Invalid operations and zero divides. */
    double a = dbl(A), b = dbl(B), c = dbl(C);
    uint32_t inv = 0;
    switch (op) {
    case F_ADD:
    case F_SUB:
        if (is_inf(A) && is_inf(B) && ((A ^ B) >> 63) == (op == F_ADD))
            inv = VXISI;
        break;
    case F_MUL:
        if ((is_inf(A) && is_zero(C)) || (is_zero(A) && is_inf(C)))
            inv = VXIMZ;
        break;
    case F_DIV:
        if (is_inf(A) && is_inf(B))
            inv = VXIDI;
        else if (is_zero(A) && is_zero(B))
            inv = VXZDZ;
        else if (is_zero(B) && !is_inf(A))
            o.exc |= ZX;
        break;
    case F_SQRT:
    case F_RSQRTE:
        if ((B >> 63) && !is_zero(B))
            inv = VXSQRT;
        else if (op == F_RSQRTE && is_zero(B))
            o.exc |= ZX;
        break;
    case F_RES:
        if (is_zero(B))
            o.exc |= ZX;
        break;
    case F_MADD:
    case F_MSUB:
        if ((is_inf(A) && is_zero(C)) || (is_zero(A) && is_inf(C)))
            inv = VXIMZ;
        else if ((is_inf(A) || is_inf(C)) && is_inf(B)
                 && (((A ^ C ^ B) >> 63) ^ (op == F_MSUB)))
            inv = VXISI;
        break;
    }
    if (inv) {
        o.exc |= inv;
        o.r = D_DEFAULT_NAN;
        o.suppress = fpscr & VE;
        return o;
    }
    if ((o.exc & ZX) && (fpscr & ZE)) {
        o.suppress = true;
        return o;
    }

    if (single) {
        /* Round to odd in double, then once to single. */
        fesetround(FE_TOWARDZERO);
        feclearexcept(FE_ALL_EXCEPT);
        double odd = compute(op, a, b, c);
        bool inexact = fetestexcept(FE_INEXACT);
        if (inexact && !isinf(odd))
            odd = dbl(bits(odd) | 1);
        float tz = to_single(odd);
        feclearexcept(FE_ALL_EXCEPT);
        fesetround(host_rm[rm]);
        float f = to_single(odd);
        int e = fetestexcept(FE_ALL_EXCEPT);
        o.fi = inexact || (e & FE_INEXACT);
        o.fr = o.fi && fabsf(f) != fabsf(tz);
        if (e & FE_OVERFLOW)
            o.exc |= OX;
        /* round to odd keeps the exact result's side of FLT_MIN, and zero only for zero */
        bool tiny = odd != 0 && fabs(odd) < FLT_MIN;
        if (tiny && (o.fi || (fpscr & UE)))
            o.exc |= UX;
        o.r = bits((double)f);
    } else {
        fesetround(FE_TOWARDZERO);
        feclearexcept(FE_ALL_EXCEPT);
        double tz = compute(op, a, b, c);
        bool tz_inexact = fetestexcept(FE_INEXACT);
        feclearexcept(FE_ALL_EXCEPT);
        fesetround(host_rm[rm]);
        double r = compute(op, a, b, c);
        int e = fetestexcept(FE_ALL_EXCEPT);
        o.fi = e & FE_INEXACT;
        o.fr = o.fi && fabs(r) != fabs(tz);
        if (e & FE_OVERFLOW)
            o.exc |= OX;
        /* tiny before rounding: the truncated result is below the normal range */
        bool tiny = fabs(tz) < DBL_MIN && (tz != 0 || tz_inexact);
        if (tiny && (o.fi || (fpscr & UE)))
            o.exc |= UX;
        o.r = bits(r);
    }
    if (o.fi)
        o.exc |= XX;
    return o;
}

/* fctiw / fctiwz: to a signed word, saturating. */
static fres to_int(uint64_t B, bool toward_zero, uint32_t fpscr)
{
    fres o = { 0 };
    if (is_nan(B)) {
        o.exc = VXCVI | (is_snan(B) ? VXSNAN : 0);
        o.r = 0x80000000u;
        o.suppress = fpscr & VE;
        return o;
    }
    double b = dbl(B);
    fesetround(toward_zero ? FE_TOWARDZERO : host_rm[fpscr & 3]);
    double r = nearbyint(b);
    int32_t v;
    if (r > 2147483647.0 || r < -2147483648.0) {
        o.exc = VXCVI;
        o.suppress = fpscr & VE;
        v = (B >> 63) ? INT32_MIN : INT32_MAX;
    } else {
        v = (int32_t)r;
        o.fi = r != b;
        o.fr = o.fi && fabs(r) > fabs(b);
        if (o.fi)
            o.exc |= XX;
    }
    o.r = UINT64_C(0xFFF8000000000000) | (uint32_t)v;
    return o;
}

/* ---- the helper --------------------------------------------------------- */

static uint64_t fpr(uam_cpu *cpu, unsigned r) { return uam_cpu_reg_read(cpu, PPC_FPR_OFF(r), 64); }

/*
 * One instruction of opcode 59 or 63 (the lifter checked the encoding and that the
 * FPU is usable). Writes frD, the FPSCR and, for record forms, CR1.
 */
uint64_t ppc_helper_fp(void *env, uint64_t insn, uint64_t unused_b, uint64_t unused_c, uam_status *st)
{
    (void)unused_b; (void)unused_c;
    uam_cpu *cpu = env;
    ppc_state *s = ppc_st(cpu);
    const uint32_t i = (uint32_t)insn;
    const unsigned op = i >> 26, d = (i >> 21) & 31, ra = (i >> 16) & 31, rb = (i >> 11) & 31;
    const unsigned rc = (i >> 6) & 31, xo = (i >> 1) & 0x3FF;
    const bool a_form = (xo & 0x1F) >= 16, single = op == 59;
    const uint32_t old = (uint32_t)uam_cpu_reg_read(cpu, PPC_FPSCR_OFF, 32);
    uint32_t f = old;
    const int host_mode = fegetround();
    bool arithmetic = false, explicit_fpscr = false;
    fres o = { 0 };
    bool write = false;

    if (a_form) {
        uint64_t A = fpr(cpu, ra), B = fpr(cpu, rb), C = fpr(cpu, rc);
        bool negate = false;
        arithmetic = true;
        switch (xo & 0x1F) {
        case 18: o = arith(F_DIV, A, B, C, 3, single, f); break;
        case 20: o = arith(F_SUB, A, B, C, 3, single, f); break;
        case 21: o = arith(F_ADD, A, B, C, 3, single, f); break;
        case 22: o = arith(F_SQRT, A, B, C, 2, single, f); break;
        case 24: o = arith(F_RES, A, B, C, 2, true, f); break;          /* fres: a single estimate */
        case 25: o = arith(F_MUL, A, B, C, 5, single, f); break;
        case 26: o = arith(F_RSQRTE, A, B, C, 2, false, f); break;      /* frsqrte */
        case 28: o = arith(F_MSUB, A, B, C, 7, single, f); break;
        case 29: o = arith(F_MADD, A, B, C, 7, single, f); break;
        case 30: o = arith(F_MSUB, A, B, C, 7, single, f); negate = true; break;
        case 31: o = arith(F_MADD, A, B, C, 7, single, f); negate = true; break;
        default: /* 23: fsel, no exceptions and no FPSCR change */
            arithmetic = false;
            o.r = (!is_nan(A) && dbl(A) >= 0.0) ? C : B;
            break;
        }
        if (negate && !is_nan(o.r))
            o.r ^= D_SIGN;
        write = !o.suppress;
    } else {
        uint64_t B = fpr(cpu, rb);
        switch (xo) {
        case 12: /* frsp */
            arithmetic = true;
            o = round_single(B, f);
            o.suppress = (o.exc & VXSNAN) && (f & VE);
            write = !o.suppress;
            break;
        case 14: case 15: /* fctiw, fctiwz */
            o = to_int(B, xo == 15, f);
            f = raise(f, o.exc);
            f = (f & ~(FR | FI)) | (o.fr ? FR : 0) | (o.fi ? FI : 0);
            write = !o.suppress;
            break;
        case 0: case 32: { /* fcmpu, fcmpo */
            uint64_t A = fpr(cpu, ra);
            unsigned c;
            if (is_nan(A) || is_nan(B))
                c = 1;
            else
                c = dbl(A) < dbl(B) ? 8 : dbl(A) > dbl(B) ? 4 : 2;
            uint32_t exc = 0;
            bool snan = is_snan(A) || is_snan(B);
            if (snan)
                exc |= VXSNAN;
            if (xo == 32 && ((snan && !(f & VE)) || (!snan && c == 1)))
                exc |= VXVC;
            f = raise(f, exc);
            f = (f & ~FPCC) | (c << FPRF_SHIFT);
            uam_cpu_reg_write(cpu, PPC_CR_OFF((i >> 23) & 7), 8, c);
            o.exc = exc;
            break;
        }
        case 40: o.r = B ^ D_SIGN; write = true; break;            /* fneg */
        case 72: o.r = B; write = true; break;                     /* fmr */
        case 136: o.r = B | D_SIGN; write = true; break;           /* fnabs */
        case 264: o.r = B & ~D_SIGN; write = true; break;          /* fabs */
        case 583: o.r = UINT64_C(0xFFF8000000000000) | f; write = true; break;  /* mffs */
        case 64: { /* mcrfs: copy a field to the CR, clear its exception bits */
            unsigned sf = (i >> 18) & 7;
            uam_cpu_reg_write(cpu, PPC_CR_OFF((i >> 23) & 7), 8, (f >> (28 - 4 * sf)) & 0xF);
            uint32_t field = 0xFu << (28 - 4 * sf);
            f &= ~(field & (FX | EXC_ALL));
            break;
        }
        case 38: case 70: { /* mtfsb1, mtfsb0 (FEX and VX are summaries only) */
            uint32_t bit = 0x80000000u >> d;
            if (bit != FEX && bit != VX) {
                if (xo == 70)
                    f &= ~bit;
                else
                    f = (bit & EXC_ALL) ? raise(f, bit) : (f | bit);
            }
            explicit_fpscr = true;
            break;
        }
        case 134: { /* mtfsfi */
            unsigned fld = (i >> 23) & 7, v = (i >> 12) & 0xF;
            uint32_t mask = 0xFu << (28 - 4 * fld);
            f = (f & ~mask) | ((uint32_t)v << (28 - 4 * fld));
            explicit_fpscr = true;
            break;
        }
        case 711: { /* mtfsf */
            unsigned fm = (i >> 17) & 0xFF;
            uint32_t mask = 0;
            for (unsigned k = 0; k < 8; k++)
                if (fm & (0x80u >> k))
                    mask |= 0xFu << (28 - 4 * k);
            f = (f & ~mask) | ((uint32_t)B & mask);
            explicit_fpscr = true;
            break;
        }
        default:
            break;
        }
    }

    if (arithmetic) {
        f = raise(f, o.exc);
        if (o.suppress) {
            f &= ~(FR | FI);
        } else {
            f = (f & ~(FR | FI | FPRF)) | (o.fr ? FR : 0) | (o.fi ? FI : 0)
              | (fprf_of(o.r, single || xo == 12 || (xo & 0x1F) == 24) << FPRF_SHIFT);
        }
    }
    f = summarise(f);
    fesetround(host_mode);

    if (write)
        uam_cpu_reg_write(cpu, PPC_FPR_OFF(d), 64, o.r);
    uam_cpu_reg_write(cpu, PPC_FPSCR_OFF, 32, f);
    if (i & 1)   /* Rc: CR1 = FX FEX VX OX */
        uam_cpu_reg_write(cpu, PPC_CR_OFF(1), 8, f >> 28);

    /* An enabled exception this instruction raised (or, for the FPSCR instructions,
     * FEX turned on), in a precise or imprecise mode. */
    uint32_t live = ((f & VE) ? VX_ALL : 0) | ((f & OE) ? OX : 0) | ((f & UE) ? UX : 0)
                  | ((f & ZE) ? ZX : 0) | ((f & XE) ? XX : 0);
    bool enabled = (f & FEX) && (explicit_fpscr ? !(old & FEX) : (o.exc & live) != 0);
    if (enabled && (s->s.msr & (MSR_FE0 | MSR_FE1))) {
        s->hfault = PPC_HF_FP_ENABLED;
        *st = UAM_ERR_ILLEGAL;
    }
    return 0;
}
