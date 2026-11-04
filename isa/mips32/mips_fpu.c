/*
 * mips_fpu.c - The MIPS32 release 2 floating-point unit (CP1): single and double
 * precision, 32- and 64-bit integer conversions, the 16 compare conditions into
 * eight condition codes, unfused multiply-add (COP1X), indexed loads and stores,
 * and the FCSR's rounding mode, flags, enables and cause bits.
 *
 * Status.FR selects the register model (lift-time, from cpu->priv): with FR = 1 a
 * 64-bit FPR per register; with FR = 0 32-bit registers, a double in an even/odd
 * pair (mips.h has the layout). Loads, stores and moves are lifted inline; every
 * arithmetic, conversion and compare instruction is one helper that reads its
 * operands from the register file and writes its result and FCSR.
 *
 * Arithmetic runs on the host's IEEE binary64/binary32 in FCSR.RM's rounding mode,
 * with the architecture's rules where hosts differ: NaNs use the legacy encoding
 * (quiet = fraction MSB clear), every NaN result is the default NaN (0x7FBFFFFF,
 * 0x7FF7FFFFFFFFFFFF) and a signalling NaN operand is an invalid operation; an
 * invalid conversion to an integer gives 2^31-1 (2^63-1); tininess is detected
 * before rounding. Single-precision results are computed with round-to-odd in
 * double and then rounded once, which is exact. FCSR.FS flushes subnormal results
 * to zero (inputs are used as they are). An enabled exception raises the
 * floating-point exception with FCSR.Cause set and the destination unchanged; no
 * operation raises Unimplemented Operation (subnormals are handled in "hardware").
 * MADD/MSUB/NMADD/NMSUB round the product, then the sum.
 */
#include <fenv.h>
#include <float.h>
#include <math.h>
#include <string.h>

#include "mips.h"

/* FCSR fields. Flags 6:2, Enables 11:7, Cause 17:12 (bit 17 = E). */
enum { X_I = 1, X_U = 2, X_O = 4, X_Z = 8, X_V = 16 };
#define FCSR_RM(f)      ((f) & 3u)
#define FCSR_FLAGS(f)   (((f) >> 2) & 0x1Fu)
#define FCSR_ENABLES(f) (((f) >> 7) & 0x1Fu)
#define FCSR_CAUSE_MASK (0x3Fu << 12)
#define FCSR_FS         (1u << 24)
#define FCSR_WMASK      0xFF83FFFFu    /* FCC7..1, FS, FCC0, Cause, Enables, Flags, RM */

#define S_DEFAULT_NAN 0x7FBFFFFFu
#define D_DEFAULT_NAN UINT64_C(0x7FF7FFFFFFFFFFFF)

static const int host_rm[4] = { FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD };

static double   dbl(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }
static uint64_t dbits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static float    flt(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t fbits(float f)  { uint32_t u; memcpy(&u, &f, 4); return u; }

static bool d_nan(uint64_t u)  { return (u & UINT64_C(0x7FF0000000000000)) == UINT64_C(0x7FF0000000000000) && (u << 12); }
static bool d_snan(uint64_t u) { return d_nan(u) && (u & (UINT64_C(1) << 51)); }   /* legacy: MSB set */
static bool s_nan(uint32_t u)  { return (u & 0x7F800000u) == 0x7F800000u && (u << 9); }
static bool s_snan(uint32_t u) { return s_nan(u) && (u & (1u << 22)); }

/* ---- the register file ------------------------------------------------- */

static bool fr64(const uam_cpu *cpu) { return cpu->priv & MIPS_PRIV_FR; }

static uint32_t fpr_off32(bool fr, unsigned n)
{
    return fr ? MIPS_FPR_OFF(n) : MIPS_FPR_OFF(n & ~1u) + 4u * (n & 1u);
}
static uint32_t fpr_off64(bool fr, unsigned n) { return MIPS_FPR_OFF(fr ? n : n & ~1u); }

static uint32_t get_s(uam_cpu *cpu, unsigned n) { return (uint32_t)uam_cpu_reg_read(cpu, fpr_off32(fr64(cpu), n), 32); }
static uint64_t get_d(uam_cpu *cpu, unsigned n) { return uam_cpu_reg_read(cpu, fpr_off64(fr64(cpu), n), 64); }
static void set_s(uam_cpu *cpu, unsigned n, uint32_t v) { uam_cpu_reg_write(cpu, fpr_off32(fr64(cpu), n), 32, v); }
static void set_d(uam_cpu *cpu, unsigned n, uint64_t v) { uam_cpu_reg_write(cpu, fpr_off64(fr64(cpu), n), 64, v); }

static uint32_t fcsr(uam_cpu *cpu) { return (uint32_t)uam_cpu_reg_read(cpu, MIPS_FCSR_OFF, 32); }
static void set_fcsr(uam_cpu *cpu, uint32_t v) { uam_cpu_reg_write(cpu, MIPS_FCSR_OFF, 32, v); }

/* ---- arithmetic -------------------------------------------------------- */

/* A result and the exceptions (X_*) the operation raised. */
typedef struct { uint64_t r; unsigned x; } fres;

/* The host's flags as X_* bits (underflow is decided separately). */
static unsigned host_x(void)
{
    int e = fetestexcept(FE_ALL_EXCEPT);
    return (e & FE_INEXACT ? X_I : 0) | (e & FE_OVERFLOW ? X_O : 0)
         | (e & FE_DIVBYZERO ? X_Z : 0) | (e & FE_INVALID ? X_V : 0);
}

enum dop { OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_SQRT };

static double host_op(enum dop op, double a, double b)
{
    volatile double va = a, vb = b;
    switch (op) {
    case OP_ADD:  return va + vb;
    case OP_SUB:  return va - vb;
    case OP_MUL:  return va * vb;
    case OP_DIV:  return va / vb;
    default:      return sqrt(va);
    }
}

/* Underflow from a result r (rounded) and rz (the same operation rounded toward
 * zero): tiny before rounding iff |rz| < min normal, or rz = 0 with an inexact
 * result. Signalled when tiny and inexact, or tiny with U enabled. */
static unsigned underflow(double rz, bool inexact, bool single, bool u_enabled)
{
    double mn = single ? (double)FLT_MIN : DBL_MIN;
    bool tiny = fabs(rz) < mn && (rz != 0 || inexact);
    return tiny && (inexact || u_enabled) ? X_U : 0;
}

/* Flush a subnormal result to zero (FCSR.FS). */
static void flush(fres *o, bool single)
{
    bool sub = single ? ((o->r & 0x7F800000u) == 0 && (o->r & 0x7FFFFFu))
                      : ((o->r & UINT64_C(0x7FF0000000000000)) == 0 && (o->r << 12));
    if (sub) {
        o->r &= single ? 0x80000000u : UINT64_C(0x8000000000000000);
        o->x |= X_U | X_I;
    }
}

/* One basic operation in double: round-to-odd (for a single result) or in rm. */
static fres d_arith(enum dop op, uint64_t A, uint64_t B, bool single, uint32_t f)
{
    fres o = { 0, 0 };
    bool nanin = d_nan(A) || (op != OP_SQRT && d_nan(B));
    if (d_snan(A) || (op != OP_SQRT && d_snan(B)))
        o.x |= X_V;
    if (nanin) {
        o.r = single ? S_DEFAULT_NAN : D_DEFAULT_NAN;
        return o;
    }
    double a = dbl(A), b = dbl(B);
    fesetround(FE_TOWARDZERO);
    feclearexcept(FE_ALL_EXCEPT);
    double rz = host_op(op, a, b);
    unsigned xz = host_x();
    if (xz & (X_V)) {                       /* inf-inf, 0*inf, 0/0, inf/inf, sqrt(<0) */
        o.x |= X_V;
        o.r = single ? S_DEFAULT_NAN : D_DEFAULT_NAN;
        fesetround(FE_TONEAREST);
        return o;
    }
    if (single) {
        /* round-to-odd in double, then once to single in rm */
        uint64_t odd = dbits(rz);
        if ((xz & X_I) && !isinf(rz))
            odd |= 1;
        double dv = dbl(odd);
        fesetround(host_rm[FCSR_RM(f)]);
        feclearexcept(FE_ALL_EXCEPT);
        volatile float r = (float)dv;
        unsigned x = host_x() & (X_O | X_I);
        fesetround(FE_TOWARDZERO);
        volatile float rzs = (float)dv;
        fesetround(FE_TONEAREST);
        bool inexact = (x & X_I) || (xz & X_I);
        if (xz & X_Z) x |= X_Z;
        if (inexact) x |= X_I;
        x |= underflow((double)rzs, inexact, true, FCSR_ENABLES(f) & X_U);
        o.r = fbits(r);
        o.x |= x;
        return o;
    }
    fesetround(host_rm[FCSR_RM(f)]);
    feclearexcept(FE_ALL_EXCEPT);
    double r = host_op(op, a, b);
    unsigned x = host_x() & (X_O | X_I | X_Z);
    fesetround(FE_TONEAREST);
    x |= underflow(rz, x & X_I, false, FCSR_ENABLES(f) & X_U);
    o.r = dbits(r);
    o.x |= x;
    return o;
}

/* Widen a single operand exactly; a NaN keeps its signalling-ness (for d_arith). */
static uint64_t widen(uint32_t s)
{
    if (s_nan(s))
        return (s_snan(s) ? UINT64_C(0x7FF8000000000000) : UINT64_C(0x7FF0000000000001));
    return dbits((double)flt(s));
}

static fres arith(enum dop op, bool single, uint64_t a, uint64_t b, uint32_t f)
{
    fres o = single ? d_arith(op, widen((uint32_t)a), widen((uint32_t)b), true, f)
                    : d_arith(op, a, b, false, f);
    if (f & FCSR_FS)
        flush(&o, single);
    return o;
}

/* ---- conversions ------------------------------------------------------- */

/* Float (as double) to a 32- or 64-bit integer in rounding mode rm. */
static fres to_int(double v, bool nan, int rm, bool l)
{
    fres o = { 0, 0 };
    const double lim = l ? 9223372036854775808.0 : 2147483648.0;
    uint64_t dflt = l ? UINT64_C(0x7FFFFFFFFFFFFFFF) : 0x7FFFFFFFu;
    if (nan || isinf(v)) {
        o.x = X_V;
        o.r = dflt;
        return o;
    }
    /* The integer part and the remainder are exact; round from them (not rint, whose
     * rounding mode the compiler may assume). */
    double r = trunc(v), frac = v - r;
    bool inexact = frac != 0;
    if (inexact) {
        switch (rm) {
        case 0: /* nearest, ties to even */
            if (fabs(frac) > 0.5 || (fabs(frac) == 0.5 && fmod(r, 2.0) != 0))
                r += frac > 0 ? 1 : -1;
            break;
        case 2: if (frac > 0) r += 1; break;   /* toward +inf */
        case 3: if (frac < 0) r -= 1; break;   /* toward -inf */
        default: break;                         /* toward zero */
        }
    }
    if (r >= lim || r < -lim) {
        o.x = X_V;
        o.r = dflt;
        return o;
    }
    o.r = l ? (uint64_t)(int64_t)r : (uint32_t)(int32_t)r;
    o.x = inexact ? X_I : 0;
    return o;
}

static fres int_to(int64_t v, bool single, uint32_t f)
{
    fres o = { 0, 0 };
    fesetround(host_rm[FCSR_RM(f)]);
    feclearexcept(FE_ALL_EXCEPT);
    if (single) {
        volatile float r = (float)v;
        o.r = fbits(r);
    } else {
        volatile double r = (double)v;
        o.r = dbits(r);
    }
    o.x = fetestexcept(FE_INEXACT) ? X_I : 0;
    fesetround(FE_TONEAREST);
    return o;
}

/* ---- the helper -------------------------------------------------------- */

/* Commit an operation: Cause from its exceptions; an enabled one (or E) traps
 * with the destination untouched; otherwise the flags accumulate. True if the
 * result is to be written. */
static bool commit(uam_cpu *cpu, unsigned x, uam_status *st)
{
    uint32_t f = fcsr(cpu);
    f = (f & ~FCSR_CAUSE_MASK) | ((uint32_t)x << 12);
    if (x & FCSR_ENABLES(f)) {
        set_fcsr(cpu, f);
        mips_st(cpu)->hfault = MIPS_HF_FPE;
        *st = UAM_ERR_ILLEGAL;
        return false;
    }
    set_fcsr(cpu, f | ((uint32_t)x << 2));
    return true;
}

static void set_fcc(uam_cpu *cpu, unsigned cc, bool v)
{
    uint32_t f = fcsr(cpu), bit = cc ? 1u << (24 + cc) : 1u << 23;
    set_fcsr(cpu, v ? f | bit : f & ~bit);
}

static bool get_fcc(uam_cpu *cpu, unsigned cc)
{
    return fcsr(cpu) & (cc ? 1u << (24 + cc) : 1u << 23);
}

static uint64_t ri(uam_cpu *cpu, uam_status *st)
{
    mips_st(cpu)->hfault = MIPS_HF_RI;
    *st = UAM_ERR_ILLEGAL;
    return 0;
}

/* C.cond.fmt: bit 0 unordered, 1 equal, 2 less, 3 signalling (also on quiet NaNs). */
static void compare(uam_cpu *cpu, unsigned cond, unsigned cc, bool nanA, bool nanB, bool snan,
                    double a, double b, uam_status *st)
{
    unsigned x = 0;
    bool un = nanA || nanB;
    if (snan || (un && (cond & 8)))
        x |= X_V;
    bool r = (un && (cond & 1)) || (!un && a == b && (cond & 2)) || (!un && a < b && (cond & 4));
    if (commit(cpu, x, st))
        set_fcc(cpu, cc, r);
}

static uint64_t cop1x(uam_cpu *cpu, uint32_t i, uam_status *st)
{
    unsigned fr = (i >> 21) & 31, ft = (i >> 16) & 31, fs = (i >> 11) & 31, fd = (i >> 6) & 31;
    unsigned fn = i & 0x3F, kind = fn >> 3;            /* 4 MADD 5 MSUB 6 NMADD 7 NMSUB */
    bool single = (fn & 7) == 0;
    if (kind < 4 || (fn & 7) > 1)
        return ri(cpu, st);
    uint32_t f = fcsr(cpu);
    uint64_t A = single ? get_s(cpu, fs) : get_d(cpu, fs), B = single ? get_s(cpu, ft) : get_d(cpu, ft);
    uint64_t C = single ? get_s(cpu, fr) : get_d(cpu, fr);
    fres p = arith(OP_MUL, single, A, B, f);
    fres s = arith(kind & 1 ? OP_SUB : OP_ADD, single, p.r, C, f);
    unsigned x = p.x | s.x;
    uint64_t r = s.r;
    if (kind >= 6) {                                   /* negate a non-NaN result */
        bool nan = single ? s_nan((uint32_t)r) : d_nan(r);
        if (!nan)
            r ^= single ? 0x80000000u : UINT64_C(0x8000000000000000);
    }
    if (commit(cpu, x, st)) {
        if (single) set_s(cpu, fd, (uint32_t)r);
        else        set_d(cpu, fd, r);
    }
    return 0;
}

uint64_t mips_fpu_op(void *env, uint64_t insn, uint64_t b, uint64_t c, uam_status *st)
{
    (void)b; (void)c;
    uam_cpu *cpu = env;
    uint32_t i = (uint32_t)insn;
    if ((i >> 26) == 0x13)
        return cop1x(cpu, i, st);

    unsigned fmt = (i >> 21) & 31, ft = (i >> 16) & 31, fs = (i >> 11) & 31, fd = (i >> 6) & 31;
    unsigned fn = i & 0x3F;
    uint32_t f = fcsr(cpu);
    bool S = fmt == 16, D = fmt == 17, W = fmt == 20, L = fmt == 21;

    if (W || L) {   /* CVT.S/D from W/L */
        int64_t v = W ? (int32_t)get_s(cpu, fs) : (int64_t)get_d(cpu, fs);
        if (fn != 32 && fn != 33)
            return ri(cpu, st);
        fres o = int_to(v, fn == 32, f);
        if (commit(cpu, o.x, st)) {
            if (fn == 32) set_s(cpu, fd, (uint32_t)o.r);
            else          set_d(cpu, fd, o.r);
        }
        return 0;
    }
    if (!S && !D)
        return ri(cpu, st);

    uint64_t A = S ? get_s(cpu, fs) : get_d(cpu, fs);
    uint64_t B = S ? get_s(cpu, ft) : get_d(cpu, ft);
    bool nanA = S ? s_nan((uint32_t)A) : d_nan(A), nanB = S ? s_nan((uint32_t)B) : d_nan(B);
    bool snA = S ? s_snan((uint32_t)A) : d_snan(A), snB = S ? s_snan((uint32_t)B) : d_snan(B);
    double a = S ? (double)flt((uint32_t)A) : dbl(A), bb = S ? (double)flt((uint32_t)B) : dbl(B);
    fres o = { 0, 0 };
    bool wd = D;                 /* destination format: double? */

    if (fn >= 48) {              /* C.cond.fmt cc, fs, ft */
        if ((i >> 6) & 3)
            return ri(cpu, st);
        compare(cpu, fn & 15, (i >> 8) & 7, nanA, nanB, snA || snB, a, bb, st);
        return 0;
    }
    switch (fn) {
    case 0: o = arith(OP_ADD, S, A, B, f); break;
    case 1: o = arith(OP_SUB, S, A, B, f); break;
    case 2: o = arith(OP_MUL, S, A, B, f); break;
    case 3: o = arith(OP_DIV, S, A, B, f); break;
    case 4: o = arith(OP_SQRT, S, A, 0, f); break;
    case 21: /* RECIP = 1 / fs */
        o = arith(OP_DIV, S, S ? 0x3F800000u : dbits(1.0), A, f);
        break;
    case 22: { /* RSQRT = 1 / sqrt(fs) */
        fres q = arith(OP_SQRT, S, A, 0, f);
        o = arith(OP_DIV, S, S ? 0x3F800000u : dbits(1.0), q.r, f);
        o.x |= q.x;
        break;
    }
    case 5: case 7: /* ABS / NEG: arithmetic (legacy) on NaNs */
        if (nanA) {
            o.r = S ? S_DEFAULT_NAN : D_DEFAULT_NAN;
            o.x = snA ? X_V : 0;
        } else {
            uint64_t sign = S ? 0x80000000u : UINT64_C(0x8000000000000000);
            o.r = fn == 5 ? A & ~sign : A ^ sign;
        }
        break;
    case 6:  o.r = A; break;                                    /* MOV */
    case 17: /* MOVF / MOVT fd, fs, cc */
        if ((i >> 17) & 1)
            return ri(cpu, st);
        if (get_fcc(cpu, (i >> 18) & 7) != ((i >> 16) & 1))
            return 0;
        o.r = A;
        break;
    case 18: case 19: { /* MOVZ / MOVN fd, fs, rt */
        uint32_t g = ft ? (uint32_t)uam_cpu_reg_read(cpu, MIPS_REG_OFF(ft), 32) : 0;
        if ((g == 0) != (fn == 18))
            return 0;
        o.r = A;
        break;
    }
    case 8: case 9: case 10: case 11:     /* ROUND TRUNC CEIL FLOOR .L */
    case 12: case 13: case 14: case 15:   /* ... .W */
    case 36: case 37: {                   /* CVT.W CVT.L (FCSR.RM) */
        static const int rmk[4] = { 0, 1, 2, 3 };   /* nearest, zero, +inf, -inf */
        int rm = fn >= 36 ? (int)FCSR_RM(f) : rmk[fn & 3];
        bool l = fn >= 36 ? fn == 37 : fn < 12;
        o = to_int(a, nanA, rm, l);
        if (!l) {
            if (commit(cpu, o.x, st))
                set_s(cpu, fd, (uint32_t)o.r);
            return 0;
        }
        wd = true;
        break;
    }
    case 32: /* CVT.S.D */
        if (!D)
            return ri(cpu, st);
        {
            fres r = { 0, 0 };
            if (nanA) {
                r.r = S_DEFAULT_NAN;
                r.x = snA ? X_V : 0;
            } else {
                /* rounding a double to single: as an operation x + 0 in single */
                fesetround(host_rm[FCSR_RM(f)]);
                feclearexcept(FE_ALL_EXCEPT);
                volatile float v = (float)a;
                unsigned x = host_x() & (X_O | X_I);
                fesetround(FE_TOWARDZERO);
                volatile float vz = (float)a;
                fesetround(FE_TONEAREST);
                x |= underflow((double)vz, x & X_I, true, FCSR_ENABLES(f) & X_U);
                r.r = fbits(v);
                r.x = x;
                if (f & FCSR_FS)
                    flush(&r, true);
            }
            o = r;
        }
        wd = false;
        break;
    case 33: /* CVT.D.S: exact */
        if (!S)
            return ri(cpu, st);
        if (nanA) {
            o.r = D_DEFAULT_NAN;
            o.x = snA ? X_V : 0;
        } else {
            o.r = dbits(a);
        }
        wd = true;
        break;
    default:
        return ri(cpu, st);
    }
    if (commit(cpu, o.x, st)) {
        if (wd) set_d(cpu, fd, o.r);
        else    set_s(cpu, fd, (uint32_t)o.r);
    }
    return 0;
}

/* ---- control registers ------------------------------------------------- */

uint64_t mips_cfc1(void *env, uint64_t fs, uint64_t b, uint64_t c, uam_status *st)
{
    (void)b; (void)c; (void)st;
    uam_cpu *cpu = env;
    uint32_t f = fcsr(cpu);
    switch ((unsigned)fs) {
    case 0:  return mips_st(cpu)->cfg.fir;
    case 25: return ((f >> 24) & 0xFEu) | ((f >> 23) & 1u);        /* FCCR: FCC7..0 */
    case 26: return f & (FCSR_CAUSE_MASK | (0x1Fu << 2));         /* FEXR */
    case 28: return (f & ((0x1Fu << 7) | 3u)) | ((f & FCSR_FS) ? 4u : 0); /* FENR */
    case 31: return f;
    default: return 0;
    }
}

uint64_t mips_ctc1(void *env, uint64_t fs, uint64_t v64, uint64_t c, uam_status *st)
{
    (void)c;
    uam_cpu *cpu = env;
    uint32_t f = fcsr(cpu), v = (uint32_t)v64;
    switch ((unsigned)fs) {
    case 25: f = (f & ~0xFE800000u) | ((v & 0xFEu) << 24) | ((v & 1u) << 23); break;
    case 26: f = (f & ~(FCSR_CAUSE_MASK | (0x1Fu << 2))) | (v & (FCSR_CAUSE_MASK | (0x1Fu << 2))); break;
    case 28: f = (f & ~((0x1Fu << 7) | 3u | FCSR_FS)) | (v & ((0x1Fu << 7) | 3u)) | ((v & 4u) ? FCSR_FS : 0); break;
    case 31: f = v & FCSR_WMASK; break;
    default: return 0;
    }
    set_fcsr(cpu, f);
    /* Cause bits whose Enable is set (or E) raise the exception at once. */
    if ((((f >> 12) & 0x1F) & FCSR_ENABLES(f)) || (f & (1u << 17))) {
        mips_st(cpu)->hfault = MIPS_HF_FPE;
        *st = UAM_ERR_ILLEGAL;
    }
    return 0;
}

/* ---- lifting ----------------------------------------------------------- */

static uam_ref ea(ml *L, unsigned base, uint32_t off)
{
    return mips_bin(L, UAM_OP_ADD, mips_reg_r(L, base), mips_imm(L, off));
}

lift_result mips_lift_fpu(ml *L, uint32_t i)
{
    if (!(L->priv & MIPS_PRIV_CU1)) {
        mips_trap(L, NONE, EXC_CPU, mips_imm(L, 1));
        return LIFT_DONE;
    }
    bool fr = L->priv & MIPS_PRIV_FR;
    unsigned op = i >> 26, rs = (i >> 21) & 31, rt = (i >> 16) & 31, fs = (i >> 11) & 31;
    unsigned fd = (i >> 6) & 31, fl = L->endian | UAM_ACC_ALIGNED;
    uint32_t off = (uint32_t)(int32_t)(int16_t)i;

    switch (op) {
    case 0x31: /* LWC1 ft, off(base) */
        uam_put(L->b, 32, fpr_off32(fr, rt), uam_ld(L->b, 32, fl, ea(L, rs, off), NONE), NONE);
        return LIFT_SEQ;
    case 0x35: /* LDC1 */
        uam_put(L->b, 64, fpr_off64(fr, rt), uam_ld(L->b, 64, fl, ea(L, rs, off), NONE), NONE);
        return LIFT_SEQ;
    case 0x39: /* SWC1 */
        uam_st(L->b, 32, fl, ea(L, rs, off), uam_get(L->b, 32, fpr_off32(fr, rt)), NONE);
        return LIFT_SEQ;
    case 0x3D: /* SDC1 */
        uam_st(L->b, 64, fl, ea(L, rs, off), uam_get(L->b, 64, fpr_off64(fr, rt)), NONE);
        return LIFT_SEQ;
    case 0x13: { /* COP1X: indexed loads/stores, PREFX, multiply-add */
        unsigned fn = i & 0x3F;
        uam_ref a = mips_bin(L, UAM_OP_ADD, mips_reg_r(L, rs), mips_reg_r(L, rt));
        switch (fn) {
        case 0x00: uam_put(L->b, 32, fpr_off32(fr, fd), uam_ld(L->b, 32, fl, a, NONE), NONE); return LIFT_SEQ;
        case 0x01: uam_put(L->b, 64, fpr_off64(fr, fd), uam_ld(L->b, 64, fl, a, NONE), NONE); return LIFT_SEQ;
        case 0x05: /* LUXC1: the doubleword at (base+index) & ~7 */
            a = mips_bin(L, UAM_OP_AND, a, mips_imm(L, ~7u));
            uam_put(L->b, 64, fpr_off64(fr, fd), uam_ld(L->b, 64, fl, a, NONE), NONE);
            return LIFT_SEQ;
        case 0x08: uam_st(L->b, 32, fl, a, uam_get(L->b, 32, fpr_off32(fr, fs)), NONE); return LIFT_SEQ;
        case 0x09: uam_st(L->b, 64, fl, a, uam_get(L->b, 64, fpr_off64(fr, fs)), NONE); return LIFT_SEQ;
        case 0x0D:
            a = mips_bin(L, UAM_OP_AND, a, mips_imm(L, ~7u));
            uam_st(L->b, 64, fl, a, uam_get(L->b, 64, fpr_off64(fr, fs)), NONE);
            return LIFT_SEQ;
        case 0x0F: return LIFT_SEQ;                                  /* PREFX */
        default:
            mips_helper(L, MIPS_HELPER_FPU, mips_imm(L, i), NONE, NONE);
            return LIFT_SEQ;
        }
    }
    default: /* 0x11 COP1 */
        break;
    }
    switch (rs) {
    case 0x00: /* MFC1 rt, fs */
        mips_reg_w(L, rt, uam_get(L->b, 32, fpr_off32(fr, fs)));
        return LIFT_SEQ;
    case 0x03: /* MFHC1 rt, fs: the high word of the 64-bit register */
        mips_reg_w(L, rt, uam_get(L->b, 32, fpr_off64(fr, fs) + 4));
        return LIFT_SEQ;
    case 0x04: /* MTC1 */
        uam_put(L->b, 32, fpr_off32(fr, fs), mips_reg_r(L, rt), NONE);
        return LIFT_SEQ;
    case 0x07: /* MTHC1 */
        uam_put(L->b, 32, fpr_off64(fr, fs) + 4, mips_reg_r(L, rt), NONE);
        return LIFT_SEQ;
    case 0x02: /* CFC1 rt, fs */
        mips_reg_w(L, rt, mips_helper(L, MIPS_HELPER_CFC1, mips_imm(L, fs), NONE, NONE));
        return LIFT_SEQ;
    case 0x06: /* CTC1 rt, fs: may raise the exception; may change the rounding mode */
        mips_helper(L, MIPS_HELPER_CTC1, mips_imm(L, fs), mips_reg_r(L, rt), NONE);
        return LIFT_SEQ;
    case 0x10: case 0x11: case 0x14: case 0x15: /* S D W L */
        mips_helper(L, MIPS_HELPER_FPU, mips_imm(L, i), NONE, NONE);
        return LIFT_SEQ;
    default:
        mips_ri(L);
        return LIFT_DONE;
    }
}
