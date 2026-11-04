/*
 * x86_f80.c - The x87 80-bit extended format, in software.
 *
 * The x87 model (x86_x87.c) keeps its registers in this format and computes with these
 * functions, so its results do not depend on what the host's long double is (the x87
 * format itself on x86-64, IEEE quadruple on AArch64 Linux, a plain double on macOS on
 * Apple silicon). They follow the x87: a 64-bit significand with an explicit integer
 * bit, round to nearest even (the model does not apply FCW.PC, and rounds arithmetic
 * to nearest whatever FCW.RC says; the integer conversions take RC), gradual underflow
 * and overflow to infinity as with those exceptions masked, the "real indefinite"
 * (negative quiet NaN) for an invalid operation, and NaN operands propagated by the
 * x87 rules (Intel SDM vol. 1, 4.8.3.5): a signalling NaN is quieted, of two NaNs a
 * quiet one wins over a signalling one, else the larger significand (the positive one
 * if they are equal). Unnormals, pseudo-NaNs and pseudo-infinities (an integer bit of
 * 0 with a nonzero exponent) are unsupported operands, which give the indefinite;
 * pseudo-denormals are read as the denormals they equal.
 *
 * The transcendental instructions compute in the host's long double (x86_f80_to_ld,
 * x86_f80_from_ld): exact where that is the x87 format, as precise as the host
 * otherwise. Nothing here sets an exception flag; the x87 model derives the ones it
 * reports.
 */
#include <float.h>
#include <math.h>
#include <string.h>

#include "x86.h"

typedef unsigned __int128 u128;

#define BIAS  16383
#define EMAX  0x7FFF
#define JBIT  (UINT64_C(1) << 63)
#define QBIT  (UINT64_C(1) << 62)

const x86_f80 X86_F80_INDEFINITE = { UINT64_C(0xC000000000000000), 0xFFFF };

static x86_f80 mk(bool neg, unsigned exp, uint64_t m)
{
    return (x86_f80){ m, (uint16_t)((neg ? 0x8000u : 0) | exp) };
}
static unsigned expo(x86_f80 a) { return a.se & EMAX; }

bool x86_f80_sign(x86_f80 a) { return a.se >> 15; }
x86_f80 x86_f80_neg(x86_f80 a) { a.se ^= 0x8000; return a; }
x86_f80 x86_f80_abs(x86_f80 a) { a.se &= EMAX; return a; }

int x86_f80_class(x86_f80 a)
{
    const unsigned e = expo(a);
    if (e == 0)
        return a.m ? X86_F80_SUBNORMAL : X86_F80_ZERO;
    if (!(a.m & JBIT))
        return X86_F80_UNSUPPORTED;
    if (e == EMAX)
        return a.m << 1 ? X86_F80_NAN : X86_F80_INF;
    return X86_F80_NORMAL;
}

/* ---- the memory format --------------------------------------------------- */

x86_f80 x86_f80_load(const uint8_t in[10])
{
    x86_f80 v = { 0, (uint16_t)(in[8] | in[9] << 8) };
    for (unsigned i = 0; i < 8; i++)
        v.m |= (uint64_t)in[i] << (8 * i);
    return v;
}

void x86_f80_store(uint8_t out[10], x86_f80 v)
{
    for (unsigned i = 0; i < 8; i++)
        out[i] = (uint8_t)(v.m >> (8 * i));
    out[8] = (uint8_t)v.se;
    out[9] = (uint8_t)(v.se >> 8);
}

/* ---- unpacking, rounding ------------------------------------------------- */

/* A finite nonzero value: m (integer bit set) * 2^(e - 63). */
typedef struct unp { int cls; bool neg; int32_t e; uint64_t m; } unp;

static unp unpack(x86_f80 a)
{
    unp u = { x86_f80_class(a), x86_f80_sign(a), (int32_t)expo(a) - BIAS, a.m };
    if (u.cls == X86_F80_SUBNORMAL) {           /* denormals and pseudo-denormals */
        const unsigned lz = (unsigned)__builtin_clzll(a.m);
        u.m = a.m << lz;
        u.e = 1 - BIAS - (int32_t)lz;
        u.cls = X86_F80_NORMAL;
    }
    return u;
}

static unsigned clz128(u128 v)
{
    const uint64_t hi = (uint64_t)(v >> 64);
    return hi ? (unsigned)__builtin_clzll(hi) : 64 + (unsigned)__builtin_clzll((uint64_t)v);
}

/* v >> n, with any bit shifted out ORed into bit 0 */
static u128 shr_sticky(u128 v, unsigned n)
{
    if (n == 0)
        return v;
    if (n >= 128)
        return v != 0;
    return v >> n | ((v << (128 - n)) != 0);
}

/* The value sig * 2^(e - 127), rounded to nearest even: normal, denormal (gradual
 * underflow), zero or infinity (overflow). */
static x86_f80 round_pack(bool neg, int32_t e, u128 sig)
{
    if (!sig)
        return mk(neg, 0, 0);
    const unsigned lz = clz128(sig);
    sig <<= lz;
    int32_t E = e - (int32_t)lz + BIAS;
    if (E <= 0) {
        sig = shr_sticky(sig, (unsigned)(1 - E) > 128 ? 128 : (unsigned)(1 - E));
        E = 0;
    }
    uint64_t hi = (uint64_t)(sig >> 64);
    const uint64_t lo = (uint64_t)sig;
    if (lo > JBIT || (lo == JBIT && (hi & 1))) {
        if (++hi == 0) {                         /* carried out: the next binade */
            hi = JBIT;
            E++;
        } else if (E == 0 && (hi & JBIT)) {
            E = 1;                               /* a denormal rounded up to normal */
        }
    }
    if (E >= EMAX)
        return mk(neg, EMAX, JBIT);
    return mk(neg, (unsigned)E, hi);
}

static x86_f80 normal(unp u) { return round_pack(u.neg, u.e, (u128)u.m << 64); }
static x86_f80 inf(bool neg) { return mk(neg, EMAX, JBIT); }
static x86_f80 quiet(x86_f80 a) { a.m |= QBIT; return a; }

/* The NaN an operation on a and b gives when at least one of them is a NaN. */
static x86_f80 pick_nan(x86_f80 a, x86_f80 b)
{
    const bool an = x86_f80_class(a) == X86_F80_NAN, bn = x86_f80_class(b) == X86_F80_NAN;
    if (!bn)
        return quiet(a);
    if (!an)
        return quiet(b);
    const bool aq = a.m & QBIT, bq = b.m & QBIT;
    if (aq != bq)
        return aq ? a : b;
    if (a.m != b.m)
        return quiet(a.m > b.m ? a : b);
    return quiet(x86_f80_sign(a) ? b : a);
}

/* The result if a or b is a NaN or unsupported, with *done set; else nothing. */
static x86_f80 special2(x86_f80 a, x86_f80 b, const unp *x, const unp *y, bool *done)
{
    *done = true;
    if (x->cls == X86_F80_UNSUPPORTED || y->cls == X86_F80_UNSUPPORTED)
        return X86_F80_INDEFINITE;
    if (x->cls == X86_F80_NAN || y->cls == X86_F80_NAN)
        return pick_nan(a, b);
    *done = false;
    return a;
}

/* ---- arithmetic ---------------------------------------------------------- */

static x86_f80 addsub(x86_f80 a, x86_f80 b, bool sub)
{
    unp x = unpack(a), y = unpack(b);
    bool done;
    x86_f80 s = special2(a, b, &x, &y, &done);
    if (done)
        return s;
    y.neg ^= sub;
    if (x.cls == X86_F80_INF)
        return y.cls == X86_F80_INF && x.neg != y.neg ? X86_F80_INDEFINITE : inf(x.neg);
    if (y.cls == X86_F80_INF)
        return inf(y.neg);
    if (x.cls == X86_F80_ZERO)
        return y.cls == X86_F80_ZERO ? mk(x.neg && y.neg, 0, 0) : normal(y);
    if (y.cls == X86_F80_ZERO)
        return normal(x);
    /* significands with the integer bit at bit 126, the smaller one aligned (sticky) */
    if (x.e < y.e || (x.e == y.e && x.m < y.m)) {
        const unp t = x; x = y; y = t;
    }
    const u128 sx = (u128)x.m << 63;
    const u128 sy = shr_sticky((u128)y.m << 63, (unsigned)(x.e - y.e) > 128 ? 128 : (unsigned)(x.e - y.e));
    if (x.neg == y.neg)
        return round_pack(x.neg, x.e + 1, sx + sy);
    if (sx == sy)
        return mk(false, 0, 0);                  /* x - x = +0 (round to nearest) */
    return round_pack(x.neg, x.e + 1, sx - sy);
}

x86_f80 x86_f80_add(x86_f80 a, x86_f80 b) { return addsub(a, b, false); }
x86_f80 x86_f80_sub(x86_f80 a, x86_f80 b) { return addsub(a, b, true); }

x86_f80 x86_f80_mul(x86_f80 a, x86_f80 b)
{
    unp x = unpack(a), y = unpack(b);
    bool done;
    x86_f80 s = special2(a, b, &x, &y, &done);
    if (done)
        return s;
    const bool neg = x.neg != y.neg;
    if (x.cls == X86_F80_INF || y.cls == X86_F80_INF)
        return x.cls == X86_F80_ZERO || y.cls == X86_F80_ZERO ? X86_F80_INDEFINITE : inf(neg);
    if (x.cls == X86_F80_ZERO || y.cls == X86_F80_ZERO)
        return mk(neg, 0, 0);
    return round_pack(neg, x.e + y.e + 1, (u128)x.m * y.m);
}

x86_f80 x86_f80_div(x86_f80 a, x86_f80 b)
{
    unp x = unpack(a), y = unpack(b);
    bool done;
    x86_f80 s = special2(a, b, &x, &y, &done);
    if (done)
        return s;
    const bool neg = x.neg != y.neg;
    if (x.cls == X86_F80_INF)
        return y.cls == X86_F80_INF ? X86_F80_INDEFINITE : inf(neg);
    if (y.cls == X86_F80_INF)
        return mk(neg, 0, 0);
    if (y.cls == X86_F80_ZERO)
        return x.cls == X86_F80_ZERO ? X86_F80_INDEFINITE : inf(neg);
    if (x.cls == X86_F80_ZERO)
        return mk(neg, 0, 0);
    /* 128 quotient bits by two 128/64 divisions, the first one in [2^63, 2^64) */
    const bool ge = x.m >= y.m;
    const u128 n1 = ge ? (u128)x.m << 63 : (u128)x.m << 64;
    const uint64_t q1 = (uint64_t)(n1 / y.m), r1 = (uint64_t)(n1 % y.m);
    const u128 n2 = (u128)r1 << 64;
    const uint64_t q2 = (uint64_t)(n2 / y.m), r2 = (uint64_t)(n2 % y.m);
    const u128 q = (u128)q1 << 64 | q2 | (r2 != 0);
    return round_pack(neg, x.e - y.e - (ge ? 0 : 1), q);
}

x86_f80 x86_f80_sqrt(x86_f80 a)
{
    const unp x = unpack(a);
    switch (x.cls) {
    case X86_F80_UNSUPPORTED: return X86_F80_INDEFINITE;
    case X86_F80_NAN:         return quiet(a);
    case X86_F80_ZERO:        return a;
    case X86_F80_INF:         return x.neg ? X86_F80_INDEFINITE : a;
    default:                  break;
    }
    if (x.neg)
        return X86_F80_INDEFINITE;
    /* R = m * 2^64 or 2^63 (so that e - 127 or e - 126 is even): a 64-bit root */
    const bool odd = x.e & 1;
    const u128 R = (u128)x.m << (odd ? 64 : 63);
    const int32_t half = (x.e - (odd ? 127 : 126)) / 2;
    u128 rem = 0, root = 0;
    for (int i = 63; i >= 0; i--) {              /* two bits of R per step */
        rem = rem << 2 | (R >> (2 * i) & 3);
        const u128 t = root << 2 | 1;
        root <<= 1;
        if (rem >= t) {
            rem -= t;
            root |= 1;
        }
    }
    uint64_t r = (uint64_t)root;
    int32_t E = half + 63 + BIAS;
    if (rem > root) {                            /* R - r^2 > r: above the half (never on it) */
        if (++r == 0) {
            r = JBIT;
            E++;
        }
    }
    return mk(false, (unsigned)E, r);
}

int x86_f80_cmp(x86_f80 a, x86_f80 b)
{
    const unp x = unpack(a), y = unpack(b);
    if (x.cls == X86_F80_NAN || y.cls == X86_F80_NAN || x.cls == X86_F80_UNSUPPORTED
        || y.cls == X86_F80_UNSUPPORTED)
        return 2;
    if (x.cls == X86_F80_ZERO && y.cls == X86_F80_ZERO)
        return 0;
    if (x.cls == X86_F80_ZERO)
        return y.neg ? 1 : -1;
    if (y.cls == X86_F80_ZERO)
        return x.neg ? -1 : 1;
    if (x.neg != y.neg)
        return x.neg ? -1 : 1;
    int mag;                                     /* |x| against |y| */
    if (x.cls == X86_F80_INF || y.cls == X86_F80_INF)
        mag = x.cls == y.cls ? 0 : x.cls == X86_F80_INF ? 1 : -1;
    else if (x.e != y.e)
        mag = x.e > y.e ? 1 : -1;
    else
        mag = x.m == y.m ? 0 : x.m > y.m ? 1 : -1;
    return x.neg ? -mag : mag;
}

/* ---- conversions --------------------------------------------------------- */

x86_f80 x86_f80_from_i64(int64_t v)
{
    if (!v)
        return mk(false, 0, 0);
    const uint64_t u = v < 0 ? 0 - (uint64_t)v : (uint64_t)v;
    const unsigned lz = (unsigned)__builtin_clzll(u);
    return mk(v < 0, (unsigned)(63 - lz + BIAS), u << lz);
}

/* From an IEEE format of `fb` fraction bits and exponent bias `eb`: exact. */
static x86_f80 from_ieee(uint64_t bits, unsigned fb, unsigned ebits)
{
    const bool neg = bits >> (fb + ebits);
    const unsigned emax = (1u << ebits) - 1, eb = emax >> 1;
    const unsigned e = (unsigned)(bits >> fb) & emax;
    const uint64_t frac = bits & ((UINT64_C(1) << fb) - 1);
    if (e == emax)                               /* a signalling NaN is quieted */
        return frac ? mk(neg, EMAX, JBIT | QBIT | frac << (63 - fb)) : inf(neg);
    if (e == 0) {
        if (!frac)
            return mk(neg, 0, 0);
        const unsigned lz = (unsigned)__builtin_clzll(frac);
        return mk(neg, (unsigned)(BIAS + 1 - (int)eb - (int)(lz - (63 - fb))), frac << lz);
    }
    return mk(neg, e - eb + BIAS, JBIT | frac << (63 - fb));
}

x86_f80 x86_f80_from_f32(uint32_t bits) { return from_ieee(bits, 23, 8); }
x86_f80 x86_f80_from_f64(uint64_t bits) { return from_ieee(bits, 52, 11); }

/* To an IEEE format, rounding to nearest even; the indefinite for an unsupported value. */
static uint64_t to_ieee(x86_f80 a, unsigned fb, unsigned ebits)
{
    const unsigned emax = (1u << ebits) - 1, eb = emax >> 1;
    const unp x = unpack(a);
    const uint64_t sign = (uint64_t)x.neg << (fb + ebits);
    const uint64_t inf_bits = sign | (uint64_t)emax << fb;
    switch (x.cls) {
    case X86_F80_UNSUPPORTED:
        return (UINT64_C(1) << (fb + ebits)) | (uint64_t)emax << fb | UINT64_C(1) << (fb - 1);
    case X86_F80_NAN:                            /* the top fraction bits, quieted */
        return inf_bits | ((a.m | QBIT) & ~JBIT) >> (63 - fb);
    case X86_F80_INF:
        return inf_bits;
    case X86_F80_ZERO:
        return sign;
    default:
        break;
    }
    int32_t E = x.e + (int32_t)eb;
    if (E >= (int32_t)emax)
        return inf_bits;                         /* overflow */
    unsigned sh = 63 - fb;                       /* bits of m below the result's */
    if (E <= 0) {
        sh += (unsigned)(1 - E) > 64 ? 64 : (unsigned)(1 - E);
        E = 0;
    }
    uint64_t mant, rest, halfway;
    if (sh >= 64) {
        mant = 0;
        rest = x.m;                              /* compared against 2^(sh - 1) >= 2^63 */
        halfway = sh == 64 ? JBIT : 0;
        if (sh > 64)
            rest = 1;                            /* below the half: rounds to 0 */
    } else {
        mant = x.m >> sh;
        rest = x.m & ((UINT64_C(1) << sh) - 1);
        halfway = UINT64_C(1) << (sh - 1);
    }
    if (halfway && (rest > halfway || (rest == halfway && (mant & 1))))
        mant++;
    /* a normal's mantissa carries its integer bit into the exponent field */
    const uint64_t v = E ? ((uint64_t)(E - 1) << fb) + mant : mant;
    if (v >= (uint64_t)emax << fb)
        return inf_bits;
    return sign | v;
}

uint32_t x86_f80_to_f32(x86_f80 a) { return (uint32_t)to_ieee(a, 23, 8); }
uint64_t x86_f80_to_f64(x86_f80 a) { return to_ieee(a, 52, 11); }

x86_f80 x86_f80_round(x86_f80 a, unsigned rc, bool *inexact)
{
    *inexact = false;
    const unp x = unpack(a);
    switch (x.cls) {
    case X86_F80_UNSUPPORTED: return X86_F80_INDEFINITE;
    case X86_F80_NAN:         return quiet(a);
    case X86_F80_ZERO: case X86_F80_INF: return a;
    default:                  break;
    }
    if (x.e >= 63)
        return normal(x);                        /* already an integer */
    /* fraction bits, 1 .. 65 (|a| < 1/2 behaves alike from 65 on) */
    const unsigned sh = x.e < -2 ? 65 : (unsigned)(63 - x.e);
    const u128 m = x.m;
    const u128 ip = m >> sh;
    const u128 frac = m & (((u128)1 << sh) - 1);
    const u128 half = (u128)1 << (sh - 1);
    *inexact = frac != 0;
    bool up;
    switch (rc & 3) {
    case 0:  up = frac > half || (frac == half && (ip & 1)); break;   /* nearest even */
    case 1:  up = x.neg && frac; break;                               /* down */
    case 2:  up = !x.neg && frac; break;                              /* up */
    default: up = false; break;                                       /* toward zero */
    }
    const uint64_t n = (uint64_t)ip + up;
    if (!n)
        return mk(x.neg, 0, 0);
    const x86_f80 r = x86_f80_from_i64((int64_t)n);   /* n < 2^63 */
    return x.neg ? x86_f80_neg(r) : r;
}

bool x86_f80_to_int(x86_f80 a, unsigned rc, unsigned bits, int64_t *out, bool *inexact)
{
    const x86_f80 r = x86_f80_round(a, rc, inexact);
    const unp x = unpack(r);
    if (x.cls == X86_F80_ZERO) {
        *out = 0;
        return true;
    }
    if (x.cls != X86_F80_NORMAL || x.e > (int32_t)bits - 1)
        return false;
    const uint64_t mag = x.m >> (63 - x.e);
    const uint64_t lim = UINT64_C(1) << (bits - 1);
    if (mag > lim || (mag == lim && !x.neg))
        return false;
    *out = x.neg ? (int64_t)(0 - mag) : (int64_t)mag;
    return true;
}

x86_f80 x86_f80_rem(x86_f80 a, x86_f80 b, bool ieee, unsigned *q)
{
    *q = 0;
    unp x = unpack(a), y = unpack(b);
    bool done;
    x86_f80 s = special2(a, b, &x, &y, &done);
    if (done)
        return s;
    if (x.cls == X86_F80_INF || y.cls == X86_F80_ZERO)
        return X86_F80_INDEFINITE;
    if (x.cls == X86_F80_ZERO)
        return a;
    if (y.cls == X86_F80_INF)
        return normal(x);                        /* a pseudo-denormal comes out normal */
    /* |a| = x.m * 2^(x.e - 63), |b| = y.m * 2^(y.e - 63) */
    const int32_t d = x.e - y.e;
    if (d < 0) {
        /* |a| < |b|: the truncated quotient is 0. Rounded to nearest it is 1 only if
         * |a| > |b| / 2, which takes d = -1: the remainder is then |a| - |b|, of
         * magnitude (2 * y.m - x.m) * 2^(y.e - 64). */
        if (ieee && d == -1 && x.m > y.m) {
            *q = 1;
            return round_pack(!x.neg, y.e + 63, ((u128)y.m << 1) - x.m);
        }
        return normal(x);
    }
    /* the remainder of x.m * 2^d by y.m, up to 63 bits of d at a time, and the
     * quotient's low bits */
    uint64_t r = x.m % y.m, qlo = x.m >= y.m;
    for (int32_t left = d; left > 0;) {
        const unsigned k = left > 63 ? 63 : (unsigned)left;
        const u128 n = (u128)r << k;
        qlo = ((k >= 3 ? 0 : qlo << k) + (uint64_t)(n / y.m)) & 7;
        r = (uint64_t)(n % y.m);
        left -= (int32_t)k;
    }
    /* r * 2^(y.e - 63), of a's sign; for FPREM1 one |b| more if r is above half of it
     * (or on it with an odd quotient) */
    bool neg = x.neg;
    if (ieee && (((u128)r << 1) > y.m || (((u128)r << 1) == y.m && (qlo & 1)))) {
        r = y.m - r;
        neg = !neg;
        qlo = (qlo + 1) & 7;
    }
    *q = (unsigned)qlo;
    if (!r)
        return mk(neg, 0, 0);
    return round_pack(neg, y.e, (u128)r << 64);
}

x86_f80 x86_f80_scale(x86_f80 a, x86_f80 b)
{
    unp x = unpack(a), y = unpack(b);
    bool done;
    x86_f80 s = special2(a, b, &x, &y, &done);
    if (done)
        return s;
    if (y.cls == X86_F80_INF) {                  /* 0 * 2^inf and inf * 2^-inf are invalid */
        if (!y.neg)
            return x.cls == X86_F80_ZERO ? X86_F80_INDEFINITE : x.cls == X86_F80_INF ? a : inf(x.neg);
        return x.cls == X86_F80_INF ? X86_F80_INDEFINITE : mk(x.neg, 0, 0);
    }
    if (x.cls == X86_F80_ZERO || x.cls == X86_F80_INF)
        return a;
    int64_t n = 0;
    bool inexact;
    if (!x86_f80_to_int(b, 3, 64, &n, &inexact))
        n = y.neg ? INT64_MIN : INT64_MAX;
    if (n > 70000) n = 70000;                    /* past either end of the range */
    if (n < -70000) n = -70000;
    return round_pack(x.neg, x.e + (int32_t)n, (u128)x.m << 64);
}

x86_f80 x86_f80_xtract_sig(x86_f80 a, int32_t *exp)
{
    const unp x = unpack(a);
    *exp = x.e;
    return mk(x.neg, BIAS, x.m);
}

/* ---- the host's long double, for the transcendentals ----------------------- */

#if LDBL_MANT_DIG == 64 && LDBL_MAX_EXP == 16384
/* The x87 format itself (x86-64 hosts): the first 10 bytes, little-endian. */
long double x86_f80_to_ld(x86_f80 a)
{
    long double v = 0;
    uint8_t b[10];
    x86_f80_store(b, a);
    memcpy(&v, b, sizeof(b));
    return v;
}

x86_f80 x86_f80_from_ld(long double v)
{
    uint8_t b[10];
    memcpy(b, &v, sizeof(b));
    return x86_f80_load(b);
}
#else
long double x86_f80_to_ld(x86_f80 a)
{
    const unp x = unpack(a);
    switch (x.cls) {
    case X86_F80_UNSUPPORTED: return -NAN;
    case X86_F80_NAN:  return x.neg ? -NAN : NAN;
    case X86_F80_INF:  return x.neg ? -INFINITY : INFINITY;
    case X86_F80_ZERO: return x.neg ? -0.0L : 0.0L;
    default: {
        /* the significand in two halves, each exact in any binary long double */
        const long double v = ldexpl((long double)(x.m >> 32), x.e - 31)
                            + ldexpl((long double)(x.m & 0xFFFFFFFFu), x.e - 63);
        return x.neg ? -v : v;
    }
    }
}

x86_f80 x86_f80_from_ld(long double v)
{
    const bool neg = signbit(v);
    if (isnan(v))
        return mk(neg, EMAX, JBIT | QBIT);
    if (isinf(v))
        return inf(neg);
    if (v == 0)
        return mk(neg, 0, 0);
    int e;
    const long double m = frexpl(fabsl(v), &e);  /* [0.5, 1) * 2^e */
    const long double t = ldexpl(m, 64);         /* [2^63, 2^64) */
    const uint64_t hi = (uint64_t)t;
    const uint64_t lo = (uint64_t)ldexpl(t - (long double)hi, 64);  /* the bits below */
    return round_pack(neg, e - 1, (u128)hi << 64 | lo);
}
#endif
