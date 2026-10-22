/*
 * x86_x87.c - The x87 floating-point unit, behind the X86_H_X87 helper.
 *
 * Registers are kept as host long double (the 80-bit extended format on
 * x86-64 hosts; at least as precise elsewhere) and converted to and from the
 * 80-bit memory format explicitly. Implemented: loads and stores of every
 * memory format, the arithmetic, compare (FCOM/FUCOM/FCOMI/FICOM) and
 * conditional-move instructions, the constants, FXCH/FFREE/FINCSTP/FDECSTP,
 * FCHS/FABS/FTST/FXAM, FSQRT/FRNDINT/FSCALE/FXTRACT/FPREM/FPREM1 and the
 * transcendentals, FNSTSW/FNSTCW/FLDCW/FNCLEX/FNINIT, and FNSTENV/FLDENV/
 * FNSAVE/FRSTOR (32-bit layouts). Exceptions are always treated as masked
 * (their status flags are set; #MF is never raised), and the precision-control
 * field does not round results.
 */
#include <math.h>
#include <string.h>

#include "x86.h"

enum { FSW_IE = 1u << 0, FSW_ZE = 1u << 2, FSW_SF = 1u << 6, FSW_ES = 1u << 7,
       FSW_C0 = 1u << 8, FSW_C1 = 1u << 9, FSW_C2 = 1u << 10, FSW_C3 = 1u << 14 };
#define FSW_CC (FSW_C0 | FSW_C1 | FSW_C2 | FSW_C3)

/* ---- the 80-bit format --------------------------------------------------- */

long double x86_f80_load(const uint8_t in[10])
{
    uint64_t mant = 0;
    for (unsigned i = 0; i < 8; i++)
        mant |= (uint64_t)in[i] << (8 * i);
    unsigned exp = ((unsigned)(in[9] & 0x7F) << 8) | in[8];
    bool neg = in[9] & 0x80;
    long double v;
    if (exp == 0x7FFF)
        v = (mant << 1) ? NAN : INFINITY;
    else if (exp == 0)
        v = ldexpl((long double)mant, -16382 - 63);
    else
        v = ldexpl((long double)mant, (int)exp - 16383 - 63);
    return neg ? -v : v;
}

void x86_f80_store(uint8_t out[10], long double v)
{
    uint64_t mant;
    int exp;
    bool neg = signbit(v);
    if (isnan(v)) {
        mant = 0xC000000000000000ull;   /* the default quiet NaN */
        exp = 0x7FFF;
    } else if (isinf(v)) {
        mant = 0x8000000000000000ull;
        exp = 0x7FFF;
    } else if (v == 0) {
        mant = 0;
        exp = 0;
    } else {
        int e;
        long double m = frexpl(fabsl(v), &e);       /* [0.5, 1) * 2^e */
        mant = (uint64_t)ldexpl(m, 64);
        exp = e - 1 + 16383;
        if (exp <= 0) {                              /* denormal */
            unsigned sh = (unsigned)(1 - exp);
            mant = sh < 64 ? mant >> sh : 0;
            exp = 0;
        } else if (exp >= 0x7FFF) {
            mant = 0x8000000000000000ull;
            exp = 0x7FFF;
        }
    }
    for (unsigned i = 0; i < 8; i++)
        out[i] = (uint8_t)(mant >> (8 * i));
    out[8] = (uint8_t)exp;
    out[9] = (uint8_t)((exp >> 8) | (neg ? 0x80 : 0));
}

/* ---- state --------------------------------------------------------------- */

typedef struct fpu {
    uam_cpu   *cpu;
    x86_state *s;
    uint16_t   fcw, fsw;
    uint8_t    ftw;                 /* abridged: bit n = physical register n valid */
} fpu;

static unsigned top(const fpu *f) { return (f->fsw >> 11) & 7; }
static void set_top(fpu *f, unsigned t) { f->fsw = (uint16_t)((f->fsw & ~0x3800u) | (t & 7) << 11); }
static unsigned phys(const fpu *f, unsigned i) { return (top(f) + i) & 7; }
static bool valid(const fpu *f, unsigned i) { return (f->ftw >> phys(f, i)) & 1; }

static void fault(fpu *f, uint16_t bits) { f->fsw |= bits; }

static long double st(fpu *f, unsigned i)
{
    if (!valid(f, i)) {
        fault(f, FSW_IE | FSW_SF);
        f->fsw &= (uint16_t)~FSW_C1;
        return NAN;
    }
    return f->s->fpr[phys(f, i)];
}

static void set_st(fpu *f, unsigned i, long double v)
{
    unsigned p = phys(f, i);
    f->s->fpr[p] = v;
    f->ftw |= (uint8_t)(1u << p);
}

static void push(fpu *f, long double v)
{
    unsigned t = (top(f) - 1) & 7;
    if ((f->ftw >> t) & 1) {        /* stack overflow */
        fault(f, FSW_IE | FSW_SF | FSW_C1);
        v = NAN;
    }
    set_top(f, t);
    set_st(f, 0, v);
}

static void pop(fpu *f)
{
    f->ftw &= (uint8_t)~(1u << top(f));
    set_top(f, top(f) + 1);
}

/* Set C3/C2/C0 from a comparison (C1 cleared). */
static void set_cc(fpu *f, long double a, long double b)
{
    f->fsw &= (uint16_t)~FSW_CC;
    if (isnan(a) || isnan(b))
        f->fsw |= FSW_C3 | FSW_C2 | FSW_C0;
    else if (a < b)
        f->fsw |= FSW_C0;
    else if (a == b)
        f->fsw |= FSW_C3;
}

/* FCOMI/FUCOMI: ZF, PF, CF from the comparison; OF, SF, AF cleared. */
static void set_eflags(fpu *f, long double a, long double b)
{
    bool un = isnan(a) || isnan(b);
    bool zf = un || a == b, cf = un || a < b;
    uam_cpu *cpu = f->cpu;
    uam_cpu_reg_write(cpu, X86_OFF_FLAG(X86_F_ZF), 8, zf);
    uam_cpu_reg_write(cpu, X86_OFF_FLAG(X86_F_CF), 8, cf);
    uam_cpu_reg_write(cpu, X86_OFF_FLAG(X86_F_PF), 8, un ? 0 : 1);   /* byte 0: even parity */
    uam_cpu_reg_write(cpu, X86_OFF_FLAG(X86_F_OF), 8, 0);
    uam_cpu_reg_write(cpu, X86_OFF_FLAG(X86_F_SF), 8, 0);
    uam_cpu_reg_write(cpu, X86_OFF_FLAG(X86_F_AF), 8, 0);
}

/* Integer conversion under FCW.RC; out-of-range and NaN give the indefinite value. */
static int64_t to_int(fpu *f, long double v, unsigned bits, bool trunc)
{
    unsigned rc = trunc ? 3 : (f->fcw >> 10) & 3;
    long double r = rc == 0 ? rintl(v) : rc == 1 ? floorl(v) : rc == 2 ? ceill(v) : truncl(v);
    long double lim = ldexpl(1.0L, (int)bits - 1);
    if (isnan(r) || r >= lim || r < -lim) {
        fault(f, FSW_IE);
        return (int64_t)(UINT64_C(1) << (bits - 1));   /* the integer indefinite */
    }
    if (r != v)
        f->fsw |= 1u << 5;   /* PE */
    return (int64_t)r;
}

static long double rnd_int(fpu *f, long double v)
{
    unsigned rc = (f->fcw >> 10) & 3;
    return rc == 0 ? rintl(v) : rc == 1 ? floorl(v) : rc == 2 ? ceill(v) : truncl(v);
}

/* ---- memory operands ----------------------------------------------------- */

static uam_status rd(fpu *f, uint64_t lin, unsigned size, uint64_t *v)
{
    return x86_lin_read(f->cpu, lin, size, v, false);
}

static uam_status wr(fpu *f, uint64_t lin, unsigned size, uint64_t v)
{
    return x86_lin_write(f->cpu, lin, size, v, false);
}

static uam_status ld_f32(fpu *f, uint64_t lin, long double *out)
{
    uint64_t v = 0;
    uam_status st = rd(f, lin, 4, &v);
    uint32_t u = (uint32_t)v;
    float x;
    memcpy(&x, &u, 4);
    *out = x;
    return st;
}

static uam_status ld_f64(fpu *f, uint64_t lin, long double *out)
{
    uint64_t v = 0;
    uam_status st = rd(f, lin, 8, &v);
    double x;
    memcpy(&x, &v, 8);
    *out = x;
    return st;
}

static uam_status ld_f80(fpu *f, uint64_t lin, long double *out)
{
    uint64_t lo = 0, hi = 0;
    uam_status st = rd(f, lin, 8, &lo);
    if (st == UAM_OK)
        st = rd(f, lin + 8, 2, &hi);
    uint8_t b[10];
    for (unsigned i = 0; i < 8; i++) b[i] = (uint8_t)(lo >> (8 * i));
    b[8] = (uint8_t)hi;
    b[9] = (uint8_t)(hi >> 8);
    *out = x86_f80_load(b);
    return st;
}

static uam_status ld_int(fpu *f, uint64_t lin, unsigned bytes, long double *out)
{
    uint64_t v = 0;
    uam_status st = rd(f, lin, bytes, &v);
    int64_t i = bytes == 2 ? (int16_t)v : bytes == 4 ? (int32_t)v : (int64_t)v;
    *out = (long double)i;
    return st;
}

static uam_status st_f32(fpu *f, uint64_t lin, long double v)
{
    float x = (float)v;
    uint32_t u;
    memcpy(&u, &x, 4);
    return wr(f, lin, 4, u);
}

static uam_status st_f64(fpu *f, uint64_t lin, long double v)
{
    double x = (double)v;
    uint64_t u;
    memcpy(&u, &x, 8);
    return wr(f, lin, 8, u);
}

static uam_status st_f80(fpu *f, uint64_t lin, long double v)
{
    uint8_t b[10];
    x86_f80_store(b, v);
    uint64_t lo = 0;
    for (unsigned i = 0; i < 8; i++) lo |= (uint64_t)b[i] << (8 * i);
    uam_status st = wr(f, lin, 8, lo);
    if (st == UAM_OK)
        st = wr(f, lin + 8, 2, b[8] | (uint64_t)b[9] << 8);
    return st;
}

/* Full 16-bit tag word from the abridged one. */
static uint16_t full_tags(fpu *f)
{
    uint16_t t = 0;
    for (unsigned p = 0; p < 8; p++) {
        unsigned tag;
        if (!((f->ftw >> p) & 1)) {
            tag = 3;
        } else {
            long double v = f->s->fpr[p];
            tag = v == 0 ? 1 : (isnan(v) || isinf(v) || fpclassify(v) == FP_SUBNORMAL) ? 2 : 0;
        }
        t |= (uint16_t)(tag << (2 * p));
    }
    return t;
}

/* FNSTENV / FLDENV, 32-bit layout (28 bytes). */
static uam_status store_env(fpu *f, uint64_t lin)
{
    const uint32_t w[7] = { f->fcw | 0xFFFF0000u, f->fsw | 0xFFFF0000u, full_tags(f) | 0xFFFF0000u,
                            (uint32_t)f->s->fpu_ip, (uint32_t)f->s->fpu_op << 16,
                            (uint32_t)f->s->fpu_dp, 0xFFFF0000u };
    for (unsigned i = 0; i < 7; i++) {
        uam_status st = wr(f, lin + 4 * i, 4, w[i]);
        if (st != UAM_OK)
            return st;
    }
    return UAM_OK;
}

static uam_status load_env(fpu *f, uint64_t lin)
{
    uint64_t cw = 0, sw = 0, tw = 0;
    uam_status st;
    if ((st = rd(f, lin, 2, &cw)) != UAM_OK || (st = rd(f, lin + 4, 2, &sw)) != UAM_OK ||
        (st = rd(f, lin + 8, 2, &tw)) != UAM_OK)
        return st;
    f->fcw = (uint16_t)cw;
    f->fsw = (uint16_t)sw;
    f->ftw = 0;
    for (unsigned p = 0; p < 8; p++)
        if (((tw >> (2 * p)) & 3) != 3)
            f->ftw |= (uint8_t)(1u << p);
    return UAM_OK;
}

static void finit(fpu *f)
{
    f->fcw = 0x37F;
    f->fsw = 0;
    f->ftw = 0;
    f->s->fpu_ip = f->s->fpu_dp = 0;
    f->s->fpu_op = 0;
}

/* ---- arithmetic ---------------------------------------------------------- */

/* op: 0 add, 1 mul, 4 sub (a-b), 5 subr (b-a), 6 div (a/b), 7 divr (b/a) */
static long double arith(fpu *f, unsigned op, long double a, long double b)
{
    switch (op) {
    case 0: return a + b;
    case 1: return a * b;
    case 4: return a - b;
    case 5: return b - a;
    case 6:
        if (b == 0 && a != 0 && !isnan(a)) fault(f, FSW_ZE);
        return a / b;
    default:
        if (a == 0 && b != 0 && !isnan(b)) fault(f, FSW_ZE);
        return b / a;
    }
}

static bool fcmov_cond(uam_cpu *cpu, unsigned cc)
{
    bool cf = uam_cpu_reg_read(cpu, X86_OFF_FLAG(X86_F_CF), 8) != 0;
    bool zf = uam_cpu_reg_read(cpu, X86_OFF_FLAG(X86_F_ZF), 8) != 0;
    bool pf = !(__builtin_popcount((unsigned)uam_cpu_reg_read(cpu, X86_OFF_FLAG(X86_F_PF), 8)) & 1);
    switch (cc & 3) {
    case 0: return cf;
    case 1: return zf;
    case 2: return cf || zf;
    default: return pf;
    }
}

static void fxam(fpu *f)
{
    f->fsw &= (uint16_t)~FSW_CC;
    if (!valid(f, 0)) {
        f->fsw |= FSW_C3 | FSW_C0;
        return;
    }
    long double v = f->s->fpr[phys(f, 0)];
    if (signbit(v)) f->fsw |= FSW_C1;
    switch (fpclassify(v)) {
    case FP_NAN:       f->fsw |= FSW_C0; break;
    case FP_INFINITE:  f->fsw |= FSW_C2 | FSW_C0; break;
    case FP_ZERO:      f->fsw |= FSW_C3; break;
    case FP_SUBNORMAL: f->fsw |= FSW_C3 | FSW_C2; break;
    default:           f->fsw |= FSW_C2; break;
    }
}

/* FPREM / FPREM1: complete in one step (C2 = 0), quotient bits in C0 C3 C1. */
static void fprem(fpu *f, bool ieee)
{
    long double a = st(f, 0), b = st(f, 1);
    long double q = ieee ? rintl(a / b) : truncl(a / b);
    long double r = ieee ? remainderl(a, b) : fmodl(a, b);
    uint64_t qi = isnan(q) || isinf(q) ? 0 : (uint64_t)fabsl(fmodl(q, 8.0L));
    f->fsw &= (uint16_t)~FSW_CC;
    if (qi & 1) f->fsw |= FSW_C1;
    if (qi & 2) f->fsw |= FSW_C3;
    if (qi & 4) f->fsw |= FSW_C0;
    set_st(f, 0, r);
}

/* D9 E0-FF */
static bool d9_reg(fpu *f, unsigned modrm)
{
    long double a;
    switch (modrm) {
    case 0xD0: return true;                                  /* FNOP */
    case 0xE0: set_st(f, 0, -st(f, 0)); return true;         /* FCHS */
    case 0xE1: set_st(f, 0, fabsl(st(f, 0))); return true;   /* FABS */
    case 0xE4: set_cc(f, st(f, 0), 0.0L); return true;       /* FTST */
    case 0xE5: fxam(f); return true;
    case 0xE8: push(f, 1.0L); return true;
    case 0xE9: push(f, log2l(10.0L)); return true;
    case 0xEA: push(f, log2l(expl(1.0L))); return true;
    case 0xEB: push(f, acosl(-1.0L)); return true;
    case 0xEC: push(f, log10l(2.0L)); return true;
    case 0xED: push(f, logl(2.0L)); return true;
    case 0xEE: push(f, 0.0L); return true;
    case 0xF0: set_st(f, 0, exp2l(st(f, 0)) - 1.0L); return true;            /* F2XM1 */
    case 0xF1: a = st(f, 1) * log2l(st(f, 0)); pop(f); set_st(f, 0, a); return true;   /* FYL2X */
    case 0xF2: set_st(f, 0, tanl(st(f, 0))); f->fsw &= (uint16_t)~FSW_C2; push(f, 1.0L); return true;
    case 0xF3: a = atan2l(st(f, 1), st(f, 0)); pop(f); set_st(f, 0, a); return true;   /* FPATAN */
    case 0xF4: {                                             /* FXTRACT */
        int e;
        long double v = st(f, 0);
        long double m = frexpl(v, &e);
        set_st(f, 0, v == 0 ? -INFINITY : (long double)(e - 1));
        push(f, v == 0 ? v : m * 2.0L);
        return true;
    }
    case 0xF5: fprem(f, true); return true;
    case 0xF6: set_top(f, top(f) - 1); return true;          /* FDECSTP */
    case 0xF7: set_top(f, top(f) + 1); return true;          /* FINCSTP */
    case 0xF8: fprem(f, false); return true;
    case 0xF9: a = st(f, 1) * log2l(st(f, 0) + 1.0L); pop(f); set_st(f, 0, a); return true;
    case 0xFA: set_st(f, 0, sqrtl(st(f, 0))); return true;
    case 0xFB: a = st(f, 0); set_st(f, 0, sinl(a)); f->fsw &= (uint16_t)~FSW_C2; push(f, cosl(a)); return true;
    case 0xFC: set_st(f, 0, rnd_int(f, st(f, 0))); return true;
    case 0xFD: set_st(f, 0, ldexpl(st(f, 0), (int)truncl(st(f, 1)))); return true;   /* FSCALE */
    case 0xFE: set_st(f, 0, sinl(st(f, 0))); f->fsw &= (uint16_t)~FSW_C2; return true;
    case 0xFF: set_st(f, 0, cosl(st(f, 0))); f->fsw &= (uint16_t)~FSW_C2; return true;
    }
    return false;
}

/* Register forms (mod = 3). Returns false for an invalid encoding. */
static bool reg_form(fpu *f, unsigned op, unsigned modrm)
{
    unsigned r = (modrm >> 3) & 7, i = modrm & 7;
    long double a, b;
    switch (op) {
    case 0xD8:
        if (r == 2 || r == 3) {
            set_cc(f, st(f, 0), st(f, i));
            if (r == 3) pop(f);
        } else {
            set_st(f, 0, arith(f, r, st(f, 0), st(f, i)));
        }
        return true;
    case 0xD9:
        if (r == 0) { a = st(f, i); push(f, a); return true; }                 /* FLD ST(i) */
        if (r == 1) { a = st(f, 0); b = st(f, i); set_st(f, 0, b); set_st(f, i, a); return true; }
        if (r == 3) { set_st(f, i, st(f, 0)); pop(f); return true; }          /* FSTP1 alias */
        return d9_reg(f, modrm);
    case 0xDA:
        if (modrm == 0xE9) {                                                  /* FUCOMPP */
            set_cc(f, st(f, 0), st(f, 1));
            pop(f);
            pop(f);
            return true;
        }
        if (r < 4) {
            if (fcmov_cond(f->cpu, r))
                set_st(f, 0, st(f, i));
            return true;
        }
        return false;
    case 0xDB:
        if (modrm == 0xE2) { f->fsw &= 0x7F00; return true; }                 /* FNCLEX */
        if (modrm == 0xE3) { finit(f); return true; }
        if (modrm == 0xE0 || modrm == 0xE1 || modrm == 0xE4) return true;     /* FENI FDISI FSETPM */
        if (r < 4) {
            if (!fcmov_cond(f->cpu, r))
                set_st(f, 0, st(f, i));
            return true;
        }
        if (r == 5 || r == 6) { set_eflags(f, st(f, 0), st(f, i)); return true; }
        return false;
    case 0xDC: case 0xDE:
        if (r == 2 || r == 3) {                                               /* FCOM/FCOMP aliases */
            if (op == 0xDE && r == 3 && i != 1) return false;
            set_cc(f, st(f, 0), st(f, i));
            pop(f);
            if (op == 0xDE) pop(f);                                           /* FCOMPP */
            return true;
        }
        /* ST(i) = ST(i) op ST(0), with the subtract/divide directions swapped */
        {
            unsigned o = r == 4 ? 5 : r == 5 ? 4 : r == 6 ? 7 : r == 7 ? 6 : r;
            set_st(f, i, arith(f, o, st(f, i), st(f, 0)));
        }
        if (op == 0xDE) pop(f);
        return true;
    case 0xDD:
        switch (r) {
        case 0: f->ftw &= (uint8_t)~(1u << phys(f, i)); return true;           /* FFREE */
        case 2: set_st(f, i, st(f, 0)); return true;
        case 3: set_st(f, i, st(f, 0)); pop(f); return true;
        case 4: set_cc(f, st(f, 0), st(f, i)); return true;                   /* FUCOM */
        case 5: set_cc(f, st(f, 0), st(f, i)); pop(f); return true;
        default: return false;
        }
    default: /* DF */
        if (modrm == 0xE0) {                                                  /* FNSTSW AX */
            uam_cpu_reg_write(f->cpu, X86_OFF_GPR(X86_RAX), 16, f->fsw);
            return true;
        }
        if (r == 0) { f->ftw &= (uint8_t)~(1u << phys(f, i)); pop(f); return true; }   /* FFREEP */
        if (r == 5 || r == 6) { set_eflags(f, st(f, 0), st(f, i)); pop(f); return true; }
        return false;
    }
}

/* Memory forms. Returns false for an invalid encoding; *st is the access status. */
static bool mem_form(fpu *f, unsigned op, unsigned r, uint64_t lin, uam_status *ast)
{
    long double v = 0;
    uam_status rs = UAM_OK;
    switch (op) {
    case 0xD8: case 0xDA: case 0xDC: case 0xDE:
        rs = op == 0xD8 ? ld_f32(f, lin, &v) : op == 0xDC ? ld_f64(f, lin, &v)
           : ld_int(f, lin, op == 0xDA ? 4 : 2, &v);
        if (rs != UAM_OK)
            break;
        if (r == 2 || r == 3) {
            set_cc(f, st(f, 0), v);
            if (r == 3) pop(f);
        } else {
            set_st(f, 0, arith(f, r, st(f, 0), v));
        }
        break;
    case 0xD9:
        switch (r) {
        case 0: if ((rs = ld_f32(f, lin, &v)) == UAM_OK) push(f, v); break;
        case 2: rs = st_f32(f, lin, st(f, 0)); break;
        case 3: if ((rs = st_f32(f, lin, st(f, 0))) == UAM_OK) pop(f); break;
        case 4: rs = load_env(f, lin); break;
        case 5: { uint64_t cw = 0; if ((rs = rd(f, lin, 2, &cw)) == UAM_OK) f->fcw = (uint16_t)cw; break; }
        case 6: rs = store_env(f, lin); f->fcw |= 0x3F; break;
        case 7: rs = wr(f, lin, 2, f->fcw); break;
        default: return false;
        }
        break;
    case 0xDB:
        switch (r) {
        case 0: if ((rs = ld_int(f, lin, 4, &v)) == UAM_OK) push(f, v); break;
        case 1: case 2: case 3: {
            int64_t iv = to_int(f, st(f, 0), 32, r == 1);
            if ((rs = wr(f, lin, 4, (uint32_t)iv)) == UAM_OK && r != 2) pop(f);
            break;
        }
        case 5: if ((rs = ld_f80(f, lin, &v)) == UAM_OK) push(f, v); break;
        case 7: if ((rs = st_f80(f, lin, st(f, 0))) == UAM_OK) pop(f); break;
        default: return false;
        }
        break;
    case 0xDD:
        switch (r) {
        case 0: if ((rs = ld_f64(f, lin, &v)) == UAM_OK) push(f, v); break;
        case 1: {
            int64_t iv = to_int(f, st(f, 0), 64, true);
            if ((rs = wr(f, lin, 8, (uint64_t)iv)) == UAM_OK) pop(f);
            break;
        }
        case 2: rs = st_f64(f, lin, st(f, 0)); break;
        case 3: if ((rs = st_f64(f, lin, st(f, 0))) == UAM_OK) pop(f); break;
        case 4:                                                                /* FRSTOR */
            if ((rs = load_env(f, lin)) != UAM_OK)
                break;
            for (unsigned i = 0; i < 8 && rs == UAM_OK; i++)
                if ((rs = ld_f80(f, lin + 28 + 10 * i, &v)) == UAM_OK)
                    f->s->fpr[phys(f, i)] = v;
            break;
        case 6:                                                                /* FNSAVE */
            if ((rs = store_env(f, lin)) != UAM_OK)
                break;
            for (unsigned i = 0; i < 8 && rs == UAM_OK; i++)
                rs = st_f80(f, lin + 28 + 10 * i, f->s->fpr[phys(f, i)]);
            if (rs == UAM_OK)
                finit(f);
            break;
        case 7: rs = wr(f, lin, 2, f->fsw); break;
        default: return false;
        }
        break;
    default: /* DF */
        switch (r) {
        case 0: if ((rs = ld_int(f, lin, 2, &v)) == UAM_OK) push(f, v); break;
        case 1: case 2: case 3: {
            int64_t iv = to_int(f, st(f, 0), 16, r == 1);
            if ((rs = wr(f, lin, 2, (uint16_t)iv)) == UAM_OK && r != 2) pop(f);
            break;
        }
        case 5: if ((rs = ld_int(f, lin, 8, &v)) == UAM_OK) push(f, v); break;
        case 7: {
            int64_t iv = to_int(f, st(f, 0), 64, false);
            if ((rs = wr(f, lin, 8, (uint64_t)iv)) == UAM_OK) pop(f);
            break;
        }
        default: return false;   /* FBLD / FBSTP (packed BCD) */
        }
        break;
    }
    *ast = rs;
    return true;
}

uint64_t x86_h_x87(void *env, uint64_t desc, uint64_t lin, uint64_t c, uam_status *ast)
{
    uam_cpu *cpu = env;
    x86_state *s = x86_st(cpu);
    (void)c;
    if (s->cr0 & (CR0_EM | CR0_TS)) {
        *ast = x86_raise(cpu, X86_EXC_NM, false, 0);
        return 0;
    }
    unsigned op = desc & 0xFF, modrm = (desc >> 8) & 0xFF;
    bool mem = (desc >> 16) & 1;
    fpu f = { cpu, s, (uint16_t)uam_cpu_reg_read(cpu, X86_OFF_FCW, 16),
              (uint16_t)uam_cpu_reg_read(cpu, X86_OFF_FSW, 16),
              (uint8_t)uam_cpu_reg_read(cpu, X86_OFF_FTW, 8) };
    uam_status st = UAM_OK;
    bool ok = mem ? mem_form(&f, op, (modrm >> 3) & 7, lin, &st) : reg_form(&f, op, modrm);
    if (!ok) {
        *ast = x86_raise(cpu, X86_EXC_UD, false, 0);
        return 0;
    }
    if (st != UAM_OK) {   /* a faulting access leaves the FPU as it was */
        *ast = st;
        return 0;
    }
    if (f.fsw & ~f.fcw & 0x3F)
        f.fsw |= FSW_ES;
    s->fpu_op = (uint16_t)(((op & 7) << 8) | modrm);
    if (mem)
        s->fpu_dp = lin;
    uam_cpu_reg_write(cpu, X86_OFF_FCW, 16, f.fcw);
    uam_cpu_reg_write(cpu, X86_OFF_FSW, 16, f.fsw);
    uam_cpu_reg_write(cpu, X86_OFF_FTW, 8, f.ftw);
    return 0;
}
