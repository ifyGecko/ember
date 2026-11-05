/*
 * xtensa_fpu.c - The Floating-Point Coprocessor option's computational instructions:
 * single-precision arithmetic (the fused multiply-adds included), compares into the
 * Boolean registers, conversions to and from integers, and the divide and square-root
 * step instructions libgcc builds __divsf3, __ieee754_sqrtf, __recipsf2 and __rsqrtsf2
 * from (DIV0.S ... DIVN.S).
 *
 * Everything is integer arithmetic on the bit patterns, so results and flags are the
 * same on every host whatever its floating-point environment, and fused operations
 * round once. Conventions:
 *  - FCR[1:0] is the rounding mode of the arithmetic, FLOAT.S/UFLOAT.S and DIVN.S;
 *    MADDN.S always rounds to nearest even; the float-to-integer conversions have their
 *    own fixed mode.
 *  - FSR flags (V Z O U I) are sticky ORs of what each instruction may raise (its
 *    iclass); FP exceptions never trap (the enables in FCR are ignored).
 *  - Every arithmetic NaN result is the default NaN 0x7FC00000. The ISA has no
 *    signalling NaNs, so a NaN operand raises nothing: V is raised only by an invalid
 *    operation on non-NaN operands (inf - inf, 0 * inf, an fma whose product is
 *    0 * inf, an out-of-range or NaN conversion, an ordered compare with a NaN).
 *  - Subnormals are inputs and results like any other value (no flushing).
 *  - Underflow tininess is detected before rounding (the exact result's magnitude is
 *    below 2^-126); U is raised when the result is tiny and inexact.
 *
 * The step instructions are undocumented. Their roles in the libgcc sequences are known,
 * their intermediate bit patterns are not; this module uses a self-consistent 32-bit
 * encoding with no state outside the FP registers (so it survives a context switch):
 *  - x = (-1)^s * X * 2^E with X in [1,4) and E even, for finite non-zero x.
 *  - NEXP01.S = -(-1)^s * X. DIV0.S = (-1)^s * 1/X and SQRT0.S = 1/sqrt(X), both
 *    truncated to 8 significant bits. CONST.S 0, 1, 2, 3 = 0.0, 1.0, 2.0, 0.5.
 *  - MKDADJ.S (D = E_dividend - E_divisor) and MKSADJ.S (D = E / 2) make an adjust word:
 *    D clamped to [-186, 186], h = D < 0, A = ((D + 128h + 127) & 0xFF) << 23 |
 *    (D & 0xFF) << 14; when the IEEE result is special (zero, infinity or NaN) the word
 *    is 0x8 | code << 1 | sign instead (code 0 zero, 1 infinity, 2 NaN). MKDADJ.S sets
 *    bit 0 of a normal word when the divisor's significand is all ones.
 *  - ADDEXP.S adds the adjust's exponent field (biased) to fr's and XORs the signs;
 *    ADDEXPM.S adds A[21:14] and XORs A[22]. On a special adjust ADDEXP.S writes the
 *    marker exponent 63 and ADDEXPM.S writes the special result itself. When bit 0 is
 *    set ADDEXP.S also makes fr's significand 1 + 2^-23: the correctly rounded reciprocal
 *    of an all-ones significand is a power of two plus one ulp, the one case where the
 *    Newton step rounds to the power of two instead, which would round a power-of-two
 *    dividend's quotient the wrong way in round-to-nearest.
 *  - DIVN.S recovers q's true exponent from the difference of fr's and ft's (wrapped)
 *    exponent fields, computes q + fs * y exactly and rounds once with FCR.RM; a marker
 *    difference returns fr, the special result.
 *  - RECIP0.S and RSQRT0.S are 8-bit approximations of the whole value (sign and exponent
 *    included).
 */
#include <stdbool.h>

#include "xtensa.h"
#include "xtensa_opc.h"

uam_status xt_fpu_exec(uam_cpu *cpu, unsigned id, uint32_t insn);

#define SIGN  0x80000000u
#define DNAN  0x7FC00000u
#define PINF  0x7F800000u
#define FMAX  0x7F7FFFFFu
#define ONE   0x3F800000u

enum { RM_NEAR, RM_ZERO, RM_UP, RM_DOWN };

/* FSR flag bits. */
enum { FL_I = 1u << 7, FL_U = 1u << 8, FL_O = 1u << 9, FL_Z = 1u << 10, FL_V = 1u << 11 };

static bool is_nan(uint32_t b)  { return (b & 0x7FFFFFFFu) > PINF; }
static bool is_inf(uint32_t b)  { return (b & 0x7FFFFFFFu) == PINF; }
static bool is_zero(uint32_t b) { return (b & 0x7FFFFFFFu) == 0; }

/* (-1)^neg * sig * 2^exp. sig == 0 is a zero; a rounded-off tail may be kept as a
 * sticky 1 in bit 0. */
typedef struct xval {
    uint32_t neg;
    int      exp;
    uint64_t sig;
} xval;

static unsigned clz64(uint64_t x)   /* x != 0 */
{
#if defined(__GNUC__)
    return (unsigned)__builtin_clzll(x);
#else
    unsigned n = 0;
    while (!(x >> 63)) {
        x <<= 1;
        n++;
    }
    return n;
#endif
}

/* A finite value (zero, subnormal or normal) as sig * 2^exp, sig < 2^24. */
static xval unpack(uint32_t b)
{
    xval v = { b >> 31, -149, b & 0x7FFFFFu };
    unsigned e = (b >> 23) & 0xFF;
    if (e) {
        v.sig |= 0x800000u;
        v.exp = (int)e - 150;
    }
    return v;
}

/* Round v (sig != 0) to binary32 with IEEE overflow, subnormal and flag rules. */
static uint32_t round_pack(xval v, unsigned rm, uint32_t *fl)
{
    unsigned n = clz64(v.sig);
    if (n == 0) {
        v.sig = (v.sig >> 1) | (v.sig & 1);
        v.exp++;
    } else {
        v.sig <<= n - 1;
        v.exp -= (int)n - 1;
    }
    int e = v.exp + 62;                 /* the value is in [2^e, 2^(e+1)) */
    bool tiny = e < -126;               /* before rounding */
    unsigned sh = tiny ? 39u + (unsigned)(-126 - e) : 39u;
    uint64_t m, rem, half;
    if (sh > 63) {                      /* below half the smallest subnormal */
        m = 0;
        rem = 1;
        half = 2;
    } else {
        m = v.sig >> sh;
        rem = v.sig & ((UINT64_C(1) << sh) - 1);
        half = UINT64_C(1) << (sh - 1);
    }
    uint32_t s = v.neg << 31;
    if (rem) {
        bool up;
        *fl |= tiny ? FL_I | FL_U : FL_I;
        switch (rm) {
        case RM_NEAR: up = rem > half || (rem == half && (m & 1)); break;
        case RM_UP:   up = !v.neg; break;
        case RM_DOWN: up = v.neg; break;
        default:      up = false; break;
        }
        m += up;
    }
    if (tiny)                           /* m <= 2^23, which packs as the smallest normal */
        return s | (uint32_t)m;
    if (m >> 24) {
        m >>= 1;
        e++;
    }
    if (e > 127) {
        *fl |= FL_O | FL_I;
        bool inf = rm == RM_NEAR || (rm == RM_UP && !v.neg) || (rm == RM_DOWN && v.neg);
        return s | (inf ? PINF : FMAX);
    }
    return s | (uint32_t)(e + 127) << 23 | ((uint32_t)m & 0x7FFFFFu);
}

/* sig at bit 61 (sig has at most 48 significant bits, so this only shifts left). */
static xval top61(xval v)
{
    unsigned n = clz64(v.sig) - 2;
    v.sig <<= n;
    v.exp -= (int)n;
    return v;
}

/* a + b rounded once. The smaller operand is shifted right with a sticky bit: both have
 * their low 14 bits clear at bit 61, so the sticky stays well below the rounding point,
 * and a cancellation of more than one bit only happens when nothing was shifted out. */
static uint32_t add_round(xval a, xval b, unsigned rm, uint32_t *fl)
{
    if (!a.sig || !b.sig) {
        if (a.sig)
            return round_pack(a, rm, fl);
        if (b.sig)
            return round_pack(b, rm, fl);
        return (a.neg == b.neg ? a.neg : rm == RM_DOWN) ? SIGN : 0;
    }
    a = top61(a);
    b = top61(b);
    if (a.exp < b.exp || (a.exp == b.exp && a.sig < b.sig)) {
        xval x = a;
        a = b;
        b = x;
    }
    unsigned d = (unsigned)(a.exp - b.exp);
    if (d > 62)
        b.sig = 1;
    else if (d)
        b.sig = (b.sig >> d) | ((b.sig & ((UINT64_C(1) << d) - 1)) != 0);
    if (a.neg == b.neg) {
        a.sig += b.sig;
    } else {
        a.sig -= b.sig;
        if (!a.sig)                     /* exact cancellation */
            return rm == RM_DOWN ? SIGN : 0;
    }
    return round_pack(a, rm, fl);
}

static uint32_t f_add(uint32_t a, uint32_t b, unsigned rm, uint32_t *fl)
{
    if (is_nan(a) || is_nan(b))
        return DNAN;
    if (is_inf(a)) {
        if (is_inf(b) && (a ^ b) >> 31) {
            *fl |= FL_V;
            return DNAN;
        }
        return a;
    }
    if (is_inf(b))
        return b;
    return add_round(unpack(a), unpack(b), rm, fl);
}

static uint32_t f_mul(uint32_t a, uint32_t b, unsigned rm, uint32_t *fl)
{
    uint32_t s = (a ^ b) & SIGN;
    if (is_nan(a) || is_nan(b))
        return DNAN;
    if (is_inf(a) || is_inf(b)) {
        if (is_zero(a) || is_zero(b)) {
            *fl |= FL_V;
            return DNAN;
        }
        return s | PINF;
    }
    xval x = unpack(a), y = unpack(b);
    xval p = { s >> 31, x.exp + y.exp, x.sig * y.sig };
    return p.sig ? round_pack(p, rm, fl) : s;
}

/* r + (-1)^flip * s * t, rounded once. */
static uint32_t f_fma(uint32_t r, uint32_t s, uint32_t t, uint32_t flip, unsigned rm,
                      uint32_t *fl)
{
    uint32_t pneg = ((s ^ t) >> 31) ^ flip;
    if (is_nan(r) || is_nan(s) || is_nan(t))
        return DNAN;
    if ((is_inf(s) && is_zero(t)) || (is_zero(s) && is_inf(t))) {
        *fl |= FL_V;
        return DNAN;
    }
    if (is_inf(s) || is_inf(t)) {
        if (is_inf(r) && (r >> 31) != pneg) {
            *fl |= FL_V;
            return DNAN;
        }
        return pneg << 31 | PINF;
    }
    if (is_inf(r))
        return r;
    xval x = unpack(s), y = unpack(t);
    xval p = { pneg, x.exp + y.exp, x.sig * y.sig };
    return add_round(p, unpack(r), rm, fl);
}

/* Total order of the non-NaN values (+0 == -0). */
static int32_t fkey(uint32_t b)
{
    return (b & SIGN) ? -(int32_t)(b & 0x7FFFFFFFu) : (int32_t)b;
}

/* FLOAT.S / UFLOAT.S: AR * 2^-t, rounded with FCR.RM (never tiny, never too large). */
static uint32_t f_from_int(uint32_t a, unsigned t, bool uns, unsigned rm, uint32_t *fl)
{
    uint32_t neg = !uns && (a >> 31);
    uint64_t mag = neg ? (uint64_t)(0u - a) : a;
    if (!mag)
        return 0;
    xval v = { neg, -(int)t, mag };
    return round_pack(v, rm, fl);
}

/* TRUNC.S, ROUND.S, FLOOR.S, CEIL.S, UTRUNC.S: FR * 2^t rounded with mode. Out of range
 * (and NaN) saturates and raises V; a negative UTRUNC.S result is 0x80000000. */
static uint32_t f_to_int(uint32_t x, unsigned t, unsigned mode, bool uns, uint32_t *fl)
{
    uint32_t neg = x >> 31;
    uint32_t pmax = uns ? 0xFFFFFFFFu : 0x7FFFFFFFu;
    if (is_nan(x) || is_inf(x)) {
        *fl |= FL_V;
        return is_nan(x) || !neg ? pmax : SIGN;
    }
    xval v = unpack(x);
    if (!v.sig)
        return 0;
    int k = v.exp + (int)t;
    uint64_t n, rem = 0, half = 0;
    if (k >= 0) {
        n = k > 39 ? UINT64_MAX : v.sig << k;   /* > 39: at least 2^40 */
    } else if (-k > 63) {
        n = 0;
        rem = 1;
        half = 2;
    } else {
        unsigned sh = (unsigned)-k;
        n = v.sig >> sh;
        rem = v.sig & ((UINT64_C(1) << sh) - 1);
        half = UINT64_C(1) << (sh - 1);
    }
    if (rem) {
        switch (mode) {
        case RM_NEAR: n += rem > half || (rem == half && (n & 1)); break;
        case RM_UP:   n += !neg; break;
        case RM_DOWN: n += neg; break;
        default:      break;
        }
    }
    if (uns && neg) {
        if (n) {
            *fl |= FL_V;
            return SIGN;
        }
    } else if (n > (neg ? UINT64_C(0x80000000) : pmax)) {
        *fl |= FL_V;
        return neg ? SIGN : pmax;
    }
    if (rem)
        *fl |= FL_I;
    return neg ? (uint32_t)(0u - (uint32_t)n) : (uint32_t)n;
}

/* ---- divide and square-root steps ---------------------------------------------- */

/* Finite non-zero b = (-1)^s * M * 2^(e-23), M in [2^23, 2^24). */
static void fnorm(uint32_t b, uint32_t *m, int *e)
{
    xval v = unpack(b);
    unsigned n = clz64(v.sig) - 40;
    *m = (uint32_t)(v.sig << n);
    *e = v.exp + 23 - (int)n;
}

/* b = (-1)^s * X * 2^E, X = M * 2^(k-23) in [1,4), E even. */
static void fsplit(uint32_t b, uint32_t *m, unsigned *k, int *E)
{
    int e;
    fnorm(b, m, &e);
    *k = (unsigned)e & 1;
    *E = e - (int)*k;
}

/* floor(sqrt(v)) */
static uint64_t isqrt64(uint64_t v)
{
    uint64_t r = 0;
    for (int b = 31; b >= 0; b--) {
        uint64_t c = r | UINT64_C(1) << b;
        if (c * c <= v)
            r = c;
    }
    return r;
}

/* q * 2^exp, exact (q has at most 9 significant bits and the value is in range). */
static uint32_t pack_exact(uint32_t neg, int exp, uint64_t q)
{
    uint32_t junk = 0;
    xval v = { neg, exp, q };
    return round_pack(v, RM_NEAR, &junk);
}

/* 1/m truncated to 8 bits for m = M * 2^-23 in [1,2): q * 2^-n, q in [128, 256]. */
static uint64_t recip8(uint32_t m, int *n)
{
    uint64_t q = (UINT64_C(1) << 30) / m;
    *n = 7;
    if (q < 128) {
        q = (UINT64_C(1) << 31) / m;
        *n = 8;
    }
    return q;
}

/* 1/sqrt(X) truncated to 8 bits for X = M * 2^(k-23): q * 2^-8, q in (128, 256]. */
static uint64_t rsqrt8(uint32_t m, unsigned k)
{
    return isqrt64((UINT64_C(1) << (39 - k)) / m);
}

static uint32_t f_nexp01(uint32_t x)
{
    uint32_t m;
    unsigned k;
    int E;
    if (is_nan(x))
        return DNAN;
    if (is_inf(x) || is_zero(x))
        return x ^ SIGN;
    fsplit(x, &m, &k, &E);
    return (~x & SIGN) | (127u + k) << 23 | (m & 0x7FFFFFu);
}

static uint32_t f_div0(uint32_t x)
{
    uint32_t m;
    unsigned k;
    int E, n;
    if (is_nan(x) || is_inf(x) || is_zero(x))
        return ONE;
    fsplit(x, &m, &k, &E);
    /* 1/X = (1/m) * 2^-k with m = X * 2^-k in [1,2) */
    uint64_t q = recip8(m, &n);
    return pack_exact(x >> 31, -n - (int)k, q);
}

static uint32_t f_sqrt0(uint32_t x)
{
    uint32_t m;
    unsigned k;
    int E;
    if (is_nan(x) || is_inf(x) || is_zero(x) || (x & SIGN))
        return ONE;
    fsplit(x, &m, &k, &E);
    return pack_exact(0, -8, rsqrt8(m, k));
}

static uint32_t f_recip0(uint32_t x, uint32_t *fl)
{
    uint32_t m;
    int e, n;
    if (is_nan(x))
        return DNAN;
    if (is_zero(x)) {
        *fl |= FL_Z;
        return (x & SIGN) | PINF;
    }
    if (is_inf(x))
        return x & SIGN;
    fnorm(x, &m, &e);
    uint64_t q = recip8(m, &n);        /* 1/x ~ q * 2^(-n-e) */
    if (7 - n - e > 127)               /* 1/x of a small subnormal: too large */
        return (x & SIGN) | PINF;
    uint32_t junk = 0;
    xval v = { x >> 31, -n - e, q };
    return round_pack(v, RM_ZERO, &junk);   /* truncated, like the seed itself */
}

static uint32_t f_rsqrt0(uint32_t x, uint32_t *fl)
{
    uint32_t m;
    unsigned k;
    int E;
    if (is_nan(x))
        return DNAN;
    if (is_zero(x)) {
        *fl |= FL_Z;
        return (x & SIGN) | PINF;
    }
    if (x & SIGN) {
        *fl |= FL_V;
        return DNAN;
    }
    if (is_inf(x))
        return 0;
    fsplit(x, &m, &k, &E);
    return pack_exact(0, -8 - E / 2, rsqrt8(m, k));
}

static uint32_t f_const(unsigned imm)
{
    /* 0, 1 and 3 are what libgcc's sequences need; 2 is a guess; the rest are unknown. */
    switch (imm) {
    case 1:  return ONE;
    case 2:  return 0x40000000u;
    case 3:  return 0x3F000000u;
    default: return 0;
    }
}

#define ADJ_CLAMP 186

static uint32_t adj_word(int d)
{
    d = d < -ADJ_CLAMP ? -ADJ_CLAMP : d > ADJ_CLAMP ? ADJ_CLAMP : d;
    int h = d < 0;
    return ((uint32_t)(d + 128 * h + 127) & 0xFF) << 23 | ((uint32_t)d & 0xFF) << 14;
}

/* Adjust word for a special result: code 0 zero, 1 infinity, 2 NaN. */
static uint32_t adj_special(unsigned code, uint32_t neg) { return 0x8u | code << 1 | neg; }
static bool adj_is_special(uint32_t a) { return (a & 0xFFFFFFF8u) == 0x8u; }

static uint32_t adj_value(uint32_t a)
{
    switch ((a >> 1) & 3) {
    case 0:  return (a & 1) << 31;
    case 1:  return (a & 1) << 31 | PINF;
    default: return DNAN;
    }
}

/* MKDADJ.S fr = divisor b, fs = dividend a. */
static uint32_t f_mkdadj(uint32_t b, uint32_t a, uint32_t *fl)
{
    uint32_t sg = (a ^ b) >> 31, m;
    unsigned k;
    int ea, eb;
    if (is_nan(a) || is_nan(b))
        return adj_special(2, 0);
    if ((is_inf(a) && is_inf(b)) || (is_zero(a) && is_zero(b))) {
        *fl |= FL_V;
        return adj_special(2, 0);
    }
    if (is_inf(a))
        return adj_special(1, sg);
    if (is_zero(b)) {
        *fl |= FL_Z;
        return adj_special(1, sg);
    }
    if (is_zero(a) || is_inf(b))
        return adj_special(0, sg);
    fsplit(a, &m, &k, &ea);
    fsplit(b, &m, &k, &eb);
    return adj_word(ea - eb) | (m == 0xFFFFFFu);
}

/* MKSADJ.S fs = x. */
static uint32_t f_mksadj(uint32_t x, uint32_t *fl)
{
    uint32_t m;
    unsigned k;
    int E;
    if (is_nan(x))
        return adj_special(2, 0);
    if (is_zero(x))                     /* sqrt(-0) = -0 */
        return adj_special(0, x >> 31);
    if (x & SIGN) {
        *fl |= FL_V;
        return adj_special(2, 0);
    }
    if (is_inf(x))
        return adj_special(1, 0);
    fsplit(x, &m, &k, &E);
    return adj_word(E / 2);
}

static uint32_t f_addexp(uint32_t r, uint32_t s)
{
    if (adj_is_special(s))
        return 63u << 23;               /* marker for DIVN.S */
    uint32_t e = ((r >> 23) + ((s >> 23) & 0xFF) - 127) & 0xFF;
    return ((r ^ s) & SIGN) | e << 23 | ((s & 1) ? 1 : r & 0x7FFFFFu);
}

static uint32_t f_addexpm(uint32_t r, uint32_t s)
{
    if (adj_is_special(s))
        return adj_value(s);
    uint32_t e = ((r >> 23) + (s >> 14)) & 0xFF;
    return ((r ^ (s << 9)) & SIGN) | e << 23 | (r & 0x7FFFFFu);
}

/* DIVN.S: fr = q (exponent wrapped by ADDEXPM.S), fs = remainder, ft = y (exponent
 * wrapped by ADDEXP.S); q + fs * y rounded once. The two wrapped exponent fields differ by
 * rel (h = 0) or 128 + rel (h = 1), |rel| <= 4, which tells the octant q's exponent is
 * in: [-65, 190] or [-190, 65]. */
static uint32_t f_divn(uint32_t r, uint32_t s, uint32_t t, unsigned rm, uint32_t *fl)
{
    unsigned fq = (r >> 23) & 0xFF, fy = (t >> 23) & 0xFF, diff = (fy - fq) & 0xFF;
    int rel, lo, hi;
    if (diff <= 4 || diff >= 252) {
        rel = diff <= 4 ? (int)diff : (int)diff - 256;
        lo = -65;
        hi = 190;
    } else if (diff >= 124 && diff <= 132) {
        rel = (int)diff - 128;
        lo = -190;
        hi = 65;
    } else {
        return r;                       /* special case: fr is the result */
    }
    int eq = (int)fq - 127;
    while (eq < lo)
        eq += 256;
    while (eq > hi)
        eq -= 256;
    if (is_nan(s))
        return DNAN;
    if (is_inf(s))
        return ((s ^ t) & SIGN) | PINF;
    xval q = { r >> 31, eq - 23, (r & 0x7FFFFFu) | 0x800000u };
    xval x = unpack(s);
    xval p = { (s ^ t) >> 31, x.exp + eq + rel - 23, x.sig * ((t & 0x7FFFFFu) | 0x800000u) };
    return add_round(q, p, rm, fl);
}

/* ---- dispatch ------------------------------------------------------------------- */

uam_status xt_fpu_exec(uam_cpu *cpu, unsigned id, uint32_t insn)
{
    unsigned r = (insn >> 12) & 15, s = (insn >> 8) & 15, t = (insn >> 4) & 15;
    unsigned rm = xt_reg(cpu, XT_FCR) & 3;
    uint32_t fr = xt_reg(cpu, XT_F(r)), fs = xt_reg(cpu, XT_F(s)), ft = xt_reg(cpu, XT_F(t));
    uint32_t fl = 0, junk = 0, res = 0;
    int cmp = -1;

    switch (id) {
    case XTO_ADD_S:    res = f_add(fs, ft, rm, &fl); break;
    case XTO_SUB_S:    res = f_add(fs, ft ^ SIGN, rm, &fl); break;
    case XTO_MUL_S:    res = f_mul(fs, ft, rm, &fl); break;
    case XTO_MADD_S:   res = f_fma(fr, fs, ft, 0, rm, &fl); break;
    case XTO_MSUB_S:   res = f_fma(fr, fs, ft, 1, rm, &fl); break;
    case XTO_MADDN_S:  res = f_fma(fr, fs, ft, 0, RM_NEAR, &junk); break;

    case XTO_UN_S:
        cmp = is_nan(fs) || is_nan(ft);
        break;
    case XTO_OEQ_S:
    case XTO_UEQ_S:
        if (is_nan(fs) || is_nan(ft))
            cmp = id == XTO_UEQ_S;
        else
            cmp = fkey(fs) == fkey(ft);
        break;
    case XTO_OLT_S:
    case XTO_ULT_S:
    case XTO_OLE_S:
    case XTO_ULE_S:
        if (is_nan(fs) || is_nan(ft)) {
            fl |= FL_V;
            cmp = id == XTO_ULT_S || id == XTO_ULE_S;
        } else if (id == XTO_OLT_S || id == XTO_ULT_S) {
            cmp = fkey(fs) < fkey(ft);
        } else {
            cmp = fkey(fs) <= fkey(ft);
        }
        break;

    case XTO_FLOAT_S:
    case XTO_UFLOAT_S:
        res = f_from_int(xt_reg(cpu, XT_A(s)), t, id == XTO_UFLOAT_S, rm, &fl);
        break;
    case XTO_TRUNC_S:
    case XTO_ROUND_S:
    case XTO_FLOOR_S:
    case XTO_CEIL_S:
    case XTO_UTRUNC_S: {
        unsigned mode = id == XTO_ROUND_S ? RM_NEAR : id == XTO_FLOOR_S ? RM_DOWN
                      : id == XTO_CEIL_S ? RM_UP : RM_ZERO;
        xt_set_reg(cpu, XT_A(r), f_to_int(fs, t, mode, id == XTO_UTRUNC_S, &fl));
        goto flags;
    }

    case XTO_CONST_S:   res = f_const(s); break;
    case XTO_NEXP01_S:  res = f_nexp01(fs); break;
    case XTO_DIV0_S:    res = f_div0(fs); break;
    case XTO_SQRT0_S:   res = f_sqrt0(fs); break;
    case XTO_RECIP0_S:  res = f_recip0(fs, &fl); break;
    case XTO_RSQRT0_S:  res = f_rsqrt0(fs, &fl); break;
    case XTO_MKDADJ_S:  res = f_mkdadj(fr, fs, &fl); break;
    case XTO_MKSADJ_S:  res = f_mksadj(fs, &fl); break;
    case XTO_ADDEXP_S:  res = f_addexp(fr, fs); break;
    case XTO_ADDEXPM_S: res = f_addexpm(fr, fs); break;
    case XTO_DIVN_S:    res = f_divn(fr, fs, ft, rm, &fl); break;
    default:
        return UAM_ERR_INVALID;
    }

    if (cmp >= 0) {
        uint32_t br = xt_reg(cpu, XT_BR) & ~(1u << r);
        xt_set_reg(cpu, XT_BR, br | (uint32_t)cmp << r);
    } else {
        xt_set_reg(cpu, XT_F(r), res);
    }
flags:
    if (fl)
        xt_set_reg(cpu, XT_FSR, xt_reg(cpu, XT_FSR) | fl);
    return UAM_OK;
}
