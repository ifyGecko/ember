/*
 * mips_dsp.c - The MIPS DSP ASE, revisions 1 and 2 (Config3.DSPP, DSP2P): SIMD
 * arithmetic on four bytes (QB) and two halfwords (PH), Q15/Q31 fractional
 * arithmetic with saturation and rounding, the four 64-bit accumulators (ac0 is
 * HI/LO) with dot products, multiply-accumulate and extraction, compares into
 * DSPControl.ccond and PICK, precision changes, shifts, bit insertion, the
 * indexed loads, and RDDSP/WRDSP.
 *
 * DSPControl: pos 5:0, scount 12:7, c 13, EFI 14, ouflag 23:16 (16+ac: accumulator
 * saturation, 20: add/sub, 21: multiply, 22: shift and precision, 23: extract),
 * ccond 27:24. Status.MX = 0 makes every DSP instruction a DSP Disabled exception
 * (decided at lift time: MX is tagged in cpu->priv). The indexed loads are lifted
 * inline; every other instruction is one helper on the register file.
 */
#include "mips.h"

#define DSP_POS(c)    ((c) & 0x3Fu)
#define DSP_SCOUNT(c) (((c) >> 7) & 0x3Fu)
#define DSP_C         (1u << 13)
#define DSP_EFI       (1u << 14)
#define OU(n)         (1u << (n))       /* ouflag bit n (16..23) */

typedef struct dsp {
    uam_cpu *cpu;
    uint32_t ctl;
    unsigned rs, rt, rd, sa, op, ac;
    uint32_t a, b;     /* GPR[rs], GPR[rt] */
} dsp;

static uint32_t gpr(uam_cpu *cpu, unsigned r) { return r ? (uint32_t)uam_cpu_reg_read(cpu, MIPS_REG_OFF(r), 32) : 0; }
static void set_gpr(uam_cpu *cpu, unsigned r, uint32_t v) { if (r) uam_cpu_reg_write(cpu, MIPS_REG_OFF(r), 32, v); }

static int64_t acc(dsp *x, unsigned ac)
{
    uint64_t hi = uam_cpu_reg_read(x->cpu, MIPS_ACHI_OFF(ac), 32), lo = uam_cpu_reg_read(x->cpu, MIPS_ACLO_OFF(ac), 32);
    return (int64_t)(hi << 32 | lo);
}
static void set_acc(dsp *x, unsigned ac, int64_t v)
{
    uam_cpu_reg_write(x->cpu, MIPS_ACHI_OFF(ac), 32, (uint64_t)v >> 32);
    uam_cpu_reg_write(x->cpu, MIPS_ACLO_OFF(ac), 32, (uint32_t)v);
}

/* lanes */
static uint32_t b_(uint32_t v, unsigned i) { return (v >> (8 * i)) & 0xFF; }
static int32_t  h_(uint32_t v, unsigned i) { return (int16_t)(v >> (16 * i)); }
static uint32_t uh_(uint32_t v, unsigned i) { return (v >> (16 * i)) & 0xFFFF; }
static uint32_t qb(uint32_t b3, uint32_t b2, uint32_t b1, uint32_t b0)
{
    return (b3 & 0xFF) << 24 | (b2 & 0xFF) << 16 | (b1 & 0xFF) << 8 | (b0 & 0xFF);
}
static uint32_t ph(uint32_t h1, uint32_t h0) { return (h1 & 0xFFFF) << 16 | (h0 & 0xFFFF); }

/* saturation, setting ouflag bit `f` when it clamps */
static int64_t sat(dsp *x, int64_t v, int64_t lo, int64_t hi, unsigned f)
{
    if (v < lo) { x->ctl |= OU(f); return lo; }
    if (v > hi) { x->ctl |= OU(f); return hi; }
    return v;
}
#define SAT8U(v)  ((uint32_t)sat(x, (v), 0, 255, 20))
#define SAT16(v)  ((uint32_t)sat(x, (v), -32768, 32767, 20))
#define SAT16U(v) ((uint32_t)sat(x, (v), 0, 65535, 20))
#define SAT32(v)  ((uint32_t)sat(x, (v), INT32_MIN, INT32_MAX, 20))

/* 2*a*b of Q15 halfwords to Q31, 0x8000 * 0x8000 saturating (ouflag f) */
static int32_t q15mul(dsp *x, int32_t a, int32_t b, unsigned f)
{
    if (a == -32768 && b == -32768) {
        x->ctl |= OU(f);
        return INT32_MAX;
    }
    return (a * b) * 2;
}

/* ---- groups ------------------------------------------------------------ */

static void addsub(dsp *x)   /* SPECIAL3 funct 0x10 */
{
    uint32_t a = x->a, b = x->b, r = 0;
    switch (x->op) {
    case 0x00: case 0x04: /* ADDU.QB / ADDU_S.QB */
    case 0x01: case 0x05: /* SUBU.QB / SUBU_S.QB */
        for (unsigned i = 0; i < 4; i++) {
            int32_t v = (x->op & 1) ? (int32_t)b_(a, i) - (int32_t)b_(b, i) : (int32_t)(b_(a, i) + b_(b, i));
            uint32_t s;
            if (x->op & 4)
                s = SAT8U(v);
            else {
                if (v < 0 || v > 255)
                    x->ctl |= OU(20);
                s = (uint32_t)v & 0xFF;
            }
            r |= s << (8 * i);
        }
        break;
    case 0x08: case 0x0C: /* ADDU.PH / ADDU_S.PH */
    case 0x09: case 0x0D: /* SUBU.PH / SUBU_S.PH */
        for (unsigned i = 0; i < 2; i++) {
            int32_t v = (x->op & 1) ? (int32_t)uh_(a, i) - (int32_t)uh_(b, i) : (int32_t)(uh_(a, i) + uh_(b, i));
            uint32_t s;
            if (x->op & 4)
                s = SAT16U(v);
            else {
                if (v < 0 || v > 65535)
                    x->ctl |= OU(20);
                s = (uint32_t)v & 0xFFFF;
            }
            r |= s << (16 * i);
        }
        break;
    case 0x0A: case 0x0E: /* ADDQ.PH / ADDQ_S.PH */
    case 0x0B: case 0x0F: /* SUBQ.PH / SUBQ_S.PH */
        for (unsigned i = 0; i < 2; i++) {
            int32_t v = (x->op & 1) ? h_(a, i) - h_(b, i) : h_(a, i) + h_(b, i);
            uint32_t s;
            if (x->op & 4)
                s = SAT16(v);
            else {
                if (v < -32768 || v > 32767)
                    x->ctl |= OU(20);
                s = (uint32_t)v;
            }
            r |= (s & 0xFFFF) << (16 * i);
        }
        break;
    case 0x16: r = SAT32((int64_t)(int32_t)a + (int32_t)b); break;   /* ADDQ_S.W */
    case 0x17: r = SAT32((int64_t)(int32_t)a - (int32_t)b); break;   /* SUBQ_S.W */
    case 0x10: { /* ADDSC: carry out to DSPControl.c */
        uint64_t v = (uint64_t)a + b;
        x->ctl = (x->ctl & ~DSP_C) | ((v >> 32) ? DSP_C : 0);
        r = (uint32_t)v;
        break;
    }
    case 0x11: { /* ADDWC: with DSPControl.c; signed overflow sets ouflag 20 */
        int64_t v = (int64_t)(int32_t)a + (int32_t)b + ((x->ctl & DSP_C) ? 1 : 0);
        if (v < INT32_MIN || v > INT32_MAX)
            x->ctl |= OU(20);
        r = (uint32_t)v;
        break;
    }
    case 0x12: /* MODSUB: rs == 0 ? rt[23:8] : rs - rt[7:0] */
        r = a == 0 ? (b >> 8) & 0xFFFF : a - (b & 0xFF);
        break;
    case 0x14: /* RADDU.W.QB rd, rs */
        r = b_(a, 0) + b_(a, 1) + b_(a, 2) + b_(a, 3);
        break;
    case 0x06: case 0x07: { /* MULEU_S.PH.QBL / QBR: byte x halfword, unsigned, sat */
        unsigned hi = x->op == 0x06 ? 3 : 1;
        uint32_t h1 = (uint32_t)sat(x, (int64_t)b_(a, hi) * uh_(b, 1), 0, 65535, 21);
        uint32_t h0 = (uint32_t)sat(x, (int64_t)b_(a, hi - 1) * uh_(b, 0), 0, 65535, 21);
        r = ph(h1, h0);
        break;
    }
    case 0x1C: r = (uint32_t)q15mul(x, h_(a, 1), h_(b, 1), 21); break; /* MULEQ_S.W.PHL */
    case 0x1D: r = (uint32_t)q15mul(x, h_(a, 0), h_(b, 0), 21); break; /* MULEQ_S.W.PHR */
    case 0x1F: case 0x1E: /* MULQ_RS.PH / MULQ_S.PH (r2) */
        for (unsigned i = 0; i < 2; i++) {
            int32_t p = q15mul(x, h_(a, i), h_(b, i), 21);
            int32_t v = x->op == 0x1F ? (int32_t)(((int64_t)p + 0x8000) >> 16) : p >> 16;
            if (p == INT32_MAX)
                v = 0x7FFF;
            r |= ((uint32_t)v & 0xFFFF) << (16 * i);
        }
        break;
    default:
        break;
    }
    set_gpr(x->cpu, x->rd, r);
}

static void halving(dsp *x)   /* funct 0x18 (rev 2) */
{
    uint32_t a = x->a, b = x->b, r = 0;
    bool rnd = x->op & 2, sub = x->op & 1;
    switch (x->op) {
    case 0x00: case 0x01: case 0x02: case 0x03: /* ADDUH SUBUH (_R) .QB */
        for (unsigned i = 0; i < 4; i++) {
            int32_t v = sub ? (int32_t)b_(a, i) - (int32_t)b_(b, i) : (int32_t)(b_(a, i) + b_(b, i));
            r |= ((uint32_t)((v + (rnd ? 1 : 0)) >> 1) & 0xFF) << (8 * i);
        }
        break;
    case 0x08: case 0x09: case 0x0A: case 0x0B: /* ADDQH SUBQH (_R) .PH */
        for (unsigned i = 0; i < 2; i++) {
            int32_t v = sub ? h_(a, i) - h_(b, i) : h_(a, i) + h_(b, i);
            r |= ((uint32_t)((v + (rnd ? 1 : 0)) >> 1) & 0xFFFF) << (16 * i);
        }
        break;
    case 0x10: case 0x11: case 0x12: case 0x13: { /* ADDQH SUBQH (_R) .W */
        int64_t v = sub ? (int64_t)(int32_t)a - (int32_t)b : (int64_t)(int32_t)a + (int32_t)b;
        r = (uint32_t)((v + (rnd ? 1 : 0)) >> 1);
        break;
    }
    case 0x0C: case 0x0E: /* MUL.PH / MUL_S.PH: the low 16 bits of each product */
        for (unsigned i = 0; i < 2; i++) {
            int32_t p = h_(a, i) * h_(b, i);
            uint32_t s;
            if (x->op == 0x0E)
                s = (uint32_t)sat(x, p, -32768, 32767, 21);
            else {
                if (p < -32768 || p > 32767)
                    x->ctl |= OU(21);
                s = (uint32_t)p;
            }
            r |= (s & 0xFFFF) << (16 * i);
        }
        break;
    case 0x16: case 0x17: { /* MULQ_S.W / MULQ_RS.W */
        int32_t ia = (int32_t)a, ib = (int32_t)b;
        if (ia == INT32_MIN && ib == INT32_MIN) {
            x->ctl |= OU(21);
            r = (uint32_t)INT32_MAX;
        } else {
            int64_t p = (int64_t)ia * ib * 2;
            if (x->op == 0x17)
                p += INT64_C(0x80000000);
            r = (uint32_t)(p >> 32);
        }
        break;
    }
    default:
        break;
    }
    set_gpr(x->cpu, x->rd, r);
}

static void cmp_group(dsp *x)   /* funct 0x11 */
{
    uint32_t a = x->a, b = x->b, r = 0, cc = 0;
    switch (x->op) {
    case 0x00: case 0x01: case 0x02:   /* CMPU.cond.QB: ccond */
    case 0x04: case 0x05: case 0x06:   /* CMPGU.cond.QB: rd */
    case 0x18: case 0x19: case 0x1A: { /* CMPGDU.cond.QB: both (rev 2) */
        unsigned c = x->op & 3;
        for (unsigned i = 0; i < 4; i++) {
            uint32_t p = b_(a, i), q = b_(b, i);
            bool t = c == 0 ? p == q : c == 1 ? p < q : p <= q;
            cc |= (uint32_t)t << i;
        }
        if (x->op < 4 || x->op >= 0x18)
            x->ctl = (x->ctl & ~(0xFu << 24)) | (cc << 24);
        if (x->op >= 4)
            set_gpr(x->cpu, x->rd, cc);
        return;
    }
    case 0x08: case 0x09: case 0x0A: { /* CMP.cond.PH: ccond 1:0 */
        unsigned c = x->op & 3;
        for (unsigned i = 0; i < 2; i++) {
            int32_t p = h_(a, i), q = h_(b, i);
            bool t = c == 0 ? p == q : c == 1 ? p < q : p <= q;
            cc |= (uint32_t)t << i;
        }
        x->ctl = (x->ctl & ~(0x3u << 24)) | (cc << 24);
        return;
    }
    case 0x03: /* PICK.QB */
        for (unsigned i = 0; i < 4; i++)
            r |= (((x->ctl >> (24 + i)) & 1) ? b_(a, i) : b_(b, i)) << (8 * i);
        break;
    case 0x0B: /* PICK.PH */
        for (unsigned i = 0; i < 2; i++)
            r |= (((x->ctl >> (24 + i)) & 1) ? uh_(a, i) : uh_(b, i)) << (16 * i);
        break;
    case 0x0C: r = qb(b_(a, 3), b_(a, 1), b_(b, 3), b_(b, 1)); break;   /* PRECRQ.QB.PH */
    case 0x0D: r = qb(b_(a, 2), b_(a, 0), b_(b, 2), b_(b, 0)); break;   /* PRECR.QB.PH (r2) */
    case 0x0E: r = ph(uh_(a, 0), uh_(b, 1)); break;                     /* PACKRL.PH */
    case 0x0F: { /* PRECRQU_S.QB.PH: Q15 to unsigned 8 bits */
        uint32_t o[4];
        const int32_t h[4] = { h_(a, 1), h_(a, 0), h_(b, 1), h_(b, 0) };
        for (unsigned i = 0; i < 4; i++) {
            if (h[i] < 0) { o[i] = 0; x->ctl |= OU(22); }
            else if (h[i] > 0x7F80) { o[i] = 0xFF; x->ctl |= OU(22); }
            else o[i] = ((uint32_t)h[i] >> 7) & 0xFF;
        }
        r = qb(o[0], o[1], o[2], o[3]);
        break;
    }
    case 0x14: r = ph(uh_(a, 1), uh_(b, 1)); break;                     /* PRECRQ.PH.W */
    case 0x15: { /* PRECRQ_RS.PH.W: round, saturate */
        uint32_t o[2];
        const int32_t w[2] = { (int32_t)a, (int32_t)b };
        for (unsigned i = 0; i < 2; i++) {
            int64_t v = (int64_t)w[i] + 0x8000;
            if (v > INT32_MAX) { o[i] = 0x7FFF; x->ctl |= OU(22); }
            else o[i] = (uint32_t)(v >> 16) & 0xFFFF;
        }
        r = ph(o[0], o[1]);
        break;
    }
    case 0x1E: case 0x1F: { /* PRECR_SRA(_R).PH.W rt, rs, sa (rev 2): rt is source and dest */
        unsigned sa = x->rd;
        int64_t hi = (int32_t)x->b, lo = (int32_t)x->a;
        if (x->op == 0x1F && sa) {
            hi += INT64_C(1) << (sa - 1);
            lo += INT64_C(1) << (sa - 1);
        }
        set_gpr(x->cpu, x->rt, ph((uint32_t)(hi >> sa), (uint32_t)(lo >> sa)));
        return;
    }
    default:
        return;
    }
    set_gpr(x->cpu, x->rd, r);
}

static void unary_group(dsp *x)   /* funct 0x12: rd from rt (or an immediate) */
{
    uint32_t b = x->b, r = 0;
    switch (x->op) {
    case 0x01: /* ABSQ_S.QB (r2) */
        for (unsigned i = 0; i < 4; i++) {
            int32_t v = (int8_t)b_(b, i);
            r |= ((uint32_t)sat(x, v < 0 ? -v : v, -128, 127, 20) & 0xFF) << (8 * i);
        }
        break;
    case 0x09: /* ABSQ_S.PH */
        for (unsigned i = 0; i < 2; i++) {
            int32_t v = h_(b, i);
            r |= (SAT16(v < 0 ? -v : v) & 0xFFFF) << (16 * i);
        }
        break;
    case 0x11: { int64_t v = (int32_t)b; r = SAT32(v < 0 ? -v : v); break; }   /* ABSQ_S.W */
    case 0x02: r = (((x->rs & 7) << 5) | x->rt) * 0x01010101u; break;      /* REPL.QB imm8 (23:16) */
    case 0x03: r = (b & 0xFF) * 0x01010101u; break;                         /* REPLV.QB */
    case 0x0A: { uint32_t k = (uint32_t)uam_sext((x->rs << 5) | x->rt, 10) & 0xFFFF; /* REPL.PH imm10 */
        r = k << 16 | k;
        break; }
    case 0x0B: r = (b & 0xFFFF) * 0x00010001u; break;                       /* REPLV.PH */
    case 0x0C: r = uh_(b, 1) << 16; break;                                  /* PRECEQ.W.PHL */
    case 0x0D: r = uh_(b, 0) << 16; break;                                  /* PRECEQ.W.PHR */
    case 0x04: r = ph(b_(b, 3) << 7, b_(b, 2) << 7); break;                 /* PRECEQU.PH.QBL */
    case 0x05: r = ph(b_(b, 1) << 7, b_(b, 0) << 7); break;                 /* PRECEQU.PH.QBR */
    case 0x06: r = ph(b_(b, 3) << 7, b_(b, 1) << 7); break;                 /* PRECEQU.PH.QBLA */
    case 0x07: r = ph(b_(b, 2) << 7, b_(b, 0) << 7); break;                 /* PRECEQU.PH.QBRA */
    case 0x1C: r = ph(b_(b, 3), b_(b, 2)); break;                           /* PRECEU.PH.QBL */
    case 0x1D: r = ph(b_(b, 1), b_(b, 0)); break;                           /* PRECEU.PH.QBR */
    case 0x1E: r = ph(b_(b, 3), b_(b, 1)); break;                           /* PRECEU.PH.QBLA */
    case 0x1F: r = ph(b_(b, 2), b_(b, 0)); break;                           /* PRECEU.PH.QBRA */
    case 0x1B: /* BITREV: the low 16 bits reversed */
        for (unsigned i = 0; i < 16; i++)
            r |= ((b >> i) & 1) << (15 - i);
        break;
    default:
        return;
    }
    set_gpr(x->cpu, x->rd, r);
}

/* Shift left with the overflow test of SHLL: the bits shifted out, and the result's
 * sign, must all equal the original sign (signed lanes of `w` bits). */
static uint32_t shl_lane(dsp *x, int32_t v, unsigned s, unsigned w, bool satur)
{
    int64_t r = (int64_t)v * ((int64_t)1 << s);
    int64_t lo = -((int64_t)1 << (w - 1)), hi = ((int64_t)1 << (w - 1)) - 1;
    if (r < lo || r > hi) {
        x->ctl |= OU(22);
        if (satur)
            return (uint32_t)(v < 0 ? lo : hi);
    }
    return (uint32_t)r;
}

static uint32_t sra_lane(int32_t v, unsigned s, bool rnd)
{
    if (rnd && s)
        return (uint32_t)(((int64_t)v + ((int64_t)1 << (s - 1))) >> s);
    return (uint32_t)(v >> s);
}

static void shift_group(dsp *x)   /* funct 0x13 */
{
    uint32_t b = x->b, r = 0;
    bool var = x->op & 2;
    unsigned s;
    switch (x->op) {
    case 0x00: case 0x02: /* SHLL(V).QB: unsigned bytes; any bit shifted out overflows */
        s = (var ? x->a : x->rs) & 7;
        for (unsigned i = 0; i < 4; i++) {
            uint32_t v = b_(b, i) << s;
            if (v > 0xFF)
                x->ctl |= OU(22);
            r |= (v & 0xFF) << (8 * i);
        }
        break;
    case 0x01: case 0x03: /* SHRL(V).QB */
        s = (var ? x->a : x->rs) & 7;
        for (unsigned i = 0; i < 4; i++)
            r |= (b_(b, i) >> s) << (8 * i);
        break;
    case 0x04: case 0x05: case 0x06: case 0x07: /* SHRA(_R)(V).QB (r2) */
        s = (var ? x->a : x->rs) & 7;
        for (unsigned i = 0; i < 4; i++)
            r |= (sra_lane((int8_t)b_(b, i), s, x->op & 1) & 0xFF) << (8 * i);
        break;
    case 0x08: case 0x0A: case 0x0C: case 0x0E: /* SHLL(V)(_S).PH */
        s = (var ? x->a : x->rs) & 15;
        for (unsigned i = 0; i < 2; i++)
            r |= (shl_lane(x, h_(b, i), s, 16, x->op & 4) & 0xFFFF) << (16 * i);
        break;
    case 0x09: case 0x0B: case 0x0D: case 0x0F: /* SHRA(V)(_R).PH */
        s = (var ? x->a : x->rs) & 15;
        for (unsigned i = 0; i < 2; i++)
            r |= (sra_lane(h_(b, i), s, x->op & 4) & 0xFFFF) << (16 * i);
        break;
    case 0x19: case 0x1B: /* SHRL(V).PH (r2) */
        s = (var ? x->a : x->rs) & 15;
        r = ph(uh_(b, 1) >> s, uh_(b, 0) >> s);
        break;
    case 0x14: case 0x16: /* SHLL(V)_S.W */
        s = (var ? x->a : x->rs) & 31;
        r = shl_lane(x, (int32_t)b, s, 32, true);
        break;
    case 0x15: case 0x17: /* SHRA(V)_R.W */
        s = (var ? x->a : x->rs) & 31;
        r = sra_lane((int32_t)b, s, true);
        break;
    default:
        return;
    }
    set_gpr(x->cpu, x->rd, r);
}

static void dot_group(dsp *x)   /* funct 0x30: accumulator ops, ac in 12:11 */
{
    uint32_t a = x->a, b = x->b;
    unsigned ac = x->ac;
    int64_t A = acc(x, ac), p0, p1;
    unsigned f = 16 + ac;
    switch (x->op) {
    case 0x03: case 0x07: case 0x0B: case 0x0F: { /* DPAU/DPSU.H.QBL/QBR */
        unsigned hi = (x->op & 4) ? 1 : 3;
        int64_t p = (int64_t)b_(a, hi) * b_(b, hi) + (int64_t)b_(a, hi - 1) * b_(b, hi - 1);
        A = (x->op & 8) ? A - p : A + p;
        break;
    }
    case 0x04: case 0x05: /* DPAQ_S / DPSQ_S .W.PH */
        p1 = q15mul(x, h_(a, 1), h_(b, 1), f);
        p0 = q15mul(x, h_(a, 0), h_(b, 0), f);
        A = x->op == 4 ? A + p1 + p0 : A - p1 - p0;
        break;
    case 0x06: /* MULSAQ_S.W.PH */
        p1 = q15mul(x, h_(a, 1), h_(b, 1), f);
        p0 = q15mul(x, h_(a, 0), h_(b, 0), f);
        A = A + p1 - p0;
        break;
    case 0x0C: case 0x0D: { /* DPAQ_SA / DPSQ_SA .L.W: Q63 product, saturating sum */
        int32_t ia = (int32_t)a, ib = (int32_t)b;
        int64_t p;
        if (ia == INT32_MIN && ib == INT32_MIN) {
            p = INT64_MAX;
            x->ctl |= OU(f);
        } else {
            p = (int64_t)ia * ib * 2;
        }
        int64_t r;
        bool ov = x->op == 0x0C ? __builtin_add_overflow(A, p, &r) : __builtin_sub_overflow(A, p, &r);
        if (ov) {
            r = (x->op == 0x0C ? p > 0 : p < 0) ? INT64_MAX : INT64_MIN;
            x->ctl |= OU(f);
        }
        A = r;
        break;
    }
    case 0x14: case 0x16: /* MAQ_S.W.PHL / PHR */
        A += q15mul(x, h_(a, x->op == 0x14), h_(b, x->op == 0x14), f);
        break;
    case 0x10: case 0x12: { /* MAQ_SA.W.PHL / PHR: the sum saturated to Q31 */
        int64_t v = A + q15mul(x, h_(a, x->op == 0x10), h_(b, x->op == 0x10), f);
        A = sat(x, v, INT32_MIN, INT32_MAX, f);
        break;
    }
    case 0x00: case 0x01: /* DPA / DPS .W.PH (r2): integer */
        p1 = (int64_t)h_(a, 1) * h_(b, 1) + (int64_t)h_(a, 0) * h_(b, 0);
        A = x->op == 0 ? A + p1 : A - p1;
        break;
    case 0x02: /* MULSA.W.PH (r2) */
        A += (int64_t)h_(a, 1) * h_(b, 1) - (int64_t)h_(a, 0) * h_(b, 0);
        break;
    case 0x08: case 0x09: /* DPAX / DPSX .W.PH (r2): crossed */
        p1 = (int64_t)h_(a, 1) * h_(b, 0) + (int64_t)h_(a, 0) * h_(b, 1);
        A = x->op == 8 ? A + p1 : A - p1;
        break;
    case 0x18: case 0x19: case 0x1A: case 0x1B: { /* DPAQX_S DPSQX_S DPAQX_SA DPSQX_SA (r2) */
        p1 = q15mul(x, h_(a, 1), h_(b, 0), f);
        p0 = q15mul(x, h_(a, 0), h_(b, 1), f);
        int64_t v = (x->op & 1) ? A - p1 - p0 : A + p1 + p0;
        A = (x->op & 2) ? sat(x, v, INT32_MIN, INT32_MAX, f) : v;
        break;
    }
    default:
        return;
    }
    set_acc(x, ac, A);
}

static void extr_group(dsp *x)   /* funct 0x38 */
{
    unsigned ac = x->ac;
    int64_t A = acc(x, ac);
    bool var = x->op & 1;
    unsigned s = (var ? x->a : x->rs) & 31;
    switch (x->op) {
    case 0x00: case 0x01: case 0x04: case 0x05: case 0x06: case 0x07: { /* EXTR(V)(_R)(_RS).W */
        int64_t v = A >> s;
        if (x->op >= 4 && s)
            v = (A + (INT64_C(1) << (s - 1))) >> s;
        else if (x->op >= 4)
            v = A;
        bool fits = v >= INT32_MIN && v <= INT32_MAX;
        if (!fits)
            x->ctl |= OU(23);
        uint32_t r = (uint32_t)v;
        if ((x->op & 6) == 6 && !fits)
            r = v < 0 ? (uint32_t)INT32_MIN : (uint32_t)INT32_MAX;
        set_gpr(x->cpu, x->rt, r);
        return;
    }
    case 0x0E: case 0x0F: { /* EXTR(V)_S.H */
        int64_t v = A >> s;
        set_gpr(x->cpu, x->rt, (uint32_t)sat(x, v, -32768, 32767, 23));
        return;
    }
    case 0x02: case 0x03: case 0x0A: case 0x0B: { /* EXTP(V) / EXTPDP(V): size+1 bits at pos */
        unsigned size = s, pos = DSP_POS(x->ctl);
        int start = (int)pos - (int)size;
        if (start < 0) {
            x->ctl |= DSP_EFI;
            return;   /* rt UNPREDICTABLE: left as it is */
        }
        x->ctl &= ~DSP_EFI;
        uint64_t m = size == 31 ? 0xFFFFFFFFu : (UINT64_C(1) << (size + 1)) - 1;
        set_gpr(x->cpu, x->rt, (uint32_t)(((uint64_t)A >> start) & m));
        if (x->op & 8)
            x->ctl = (x->ctl & ~0x3Fu) | (uint32_t)((pos - (size + 1)) & 0x3F);
        return;
    }
    case 0x1A: case 0x1B: { /* SHILO ac, shift (25:20, signed) / SHILOV ac, rs */
        int32_t sh = var ? (int32_t)uam_sext(x->a & 0x3F, 6) : (int32_t)uam_sext((x->rs << 1) | (x->rt >> 4), 6);
        A = sh >= 0 ? (int64_t)((uint64_t)A >> sh) : (int64_t)((uint64_t)A << -sh);
        set_acc(x, ac, A);
        return;
    }
    case 0x1F: { /* MTHLIP rs, ac */
        set_acc(x, ac, (int64_t)((uint64_t)A << 32 | x->a));
        x->ctl = (x->ctl & ~0x3Fu) | ((DSP_POS(x->ctl) + 32) & 0x3F);
        return;
    }
    case 0x12: { /* RDDSP rd, mask (21:16) */
        unsigned m = (x->rs << 5 | x->rt) & 0x3F;
        uint32_t r = 0;
        if (m & 1)  r |= x->ctl & 0x3Fu;
        if (m & 2)  r |= x->ctl & (0x3Fu << 7);
        if (m & 4)  r |= x->ctl & DSP_C;
        if (m & 8)  r |= x->ctl & (0xFFu << 16);
        if (m & 16) r |= x->ctl & (0xFu << 24);
        if (m & 32) r |= x->ctl & DSP_EFI;
        set_gpr(x->cpu, x->rd, r);
        return;
    }
    case 0x13: { /* WRDSP rs, mask (16:11) */
        unsigned m = ((x->rt & 1) << 5) | x->rd;
        uint32_t v = x->a, keep = 0;
        if (m & 1)  keep |= 0x3Fu;
        if (m & 2)  keep |= 0x3Fu << 7;
        if (m & 4)  keep |= DSP_C;
        if (m & 8)  keep |= 0xFFu << 16;
        if (m & 16) keep |= 0xFu << 24;
        if (m & 32) keep |= DSP_EFI;
        x->ctl = (x->ctl & ~keep) | (v & keep);
        return;
    }
    default:
        return;
    }
}

static void append_group(dsp *x)   /* funct 0x31 (rev 2): rt, rs, sa(15:11) */
{
    uint32_t t = x->b, s = x->a, sa = x->rd, r;
    switch (x->op) {
    case 0x00: r = sa ? (t << sa) | (s & ((1u << sa) - 1)) : t; break;          /* APPEND */
    case 0x01: r = sa ? (s << (32 - sa)) | (t >> sa) : t; break;                 /* PREPEND */
    case 0x10: { unsigned bp = sa & 3;                                           /* BALIGN */
        r = bp ? (t << (8 * bp)) | (s >> (32 - 8 * bp)) : t;
        break; }
    default: return;
    }
    set_gpr(x->cpu, x->rt, r);
}

static void insv(dsp *x)   /* INSV rt, rs: size bits of rs at pos (DSPControl) */
{
    unsigned pos = DSP_POS(x->ctl), size = DSP_SCOUNT(x->ctl);
    if (!size || pos + size > 32)
        return;   /* UNPREDICTABLE: unchanged */
    uint32_t m = (size == 32 ? 0xFFFFFFFFu : ((1u << size) - 1)) << pos;
    set_gpr(x->cpu, x->rt, (x->b & ~m) | ((x->a << pos) & m));
}

uint64_t mips_dsp_op(void *env, uint64_t insn, uint64_t b, uint64_t c, uam_status *st)
{
    (void)b; (void)c; (void)st;
    uam_cpu *cpu = env;
    uint32_t i = (uint32_t)insn;
    dsp x = { .cpu = cpu, .ctl = (uint32_t)uam_cpu_reg_read(cpu, MIPS_DSPCTL_OFF, 32),
              .rs = (i >> 21) & 31, .rt = (i >> 16) & 31, .rd = (i >> 11) & 31,
              .op = (i >> 6) & 31, .ac = (i >> 11) & 3 };
    x.a = gpr(cpu, x.rs);
    x.b = gpr(cpu, x.rt);
    switch (i & 0x3F) {
    case 0x10: addsub(&x); break;
    case 0x18: halving(&x); break;
    case 0x11: cmp_group(&x); break;
    case 0x12: unary_group(&x); break;
    case 0x13: shift_group(&x); break;
    case 0x30: dot_group(&x); break;
    case 0x38: extr_group(&x); break;
    case 0x31: append_group(&x); break;
    case 0x0C: insv(&x); break;
    default: break;
    }
    uam_cpu_reg_write(cpu, MIPS_DSPCTL_OFF, 32, x.ctl);
    return 0;
}

/* ---- lifting ----------------------------------------------------------- */

/* 0: reserved; 1: DSP rev 1; 2: rev 2. Per SPECIAL3 funct, the sub-opcodes (bits 10:6)
 * of each revision. */
int mips_dsp_rev(uint32_t i)
{
    static const struct { uint32_t r1, r2; } ops[0x40] = {
        [0x0A] = { 0x00000051u, 0x00000000u },
        [0x0C] = { 0x00000001u, 0x00000000u },
        [0x10] = { 0xB0D7CCF3u, 0x40003300u },
        [0x11] = { 0x0030DF7Fu, 0xC7002000u },
        [0x12] = { 0xF8023EFCu, 0x00000002u },
        [0x13] = { 0x00F0FF0Fu, 0x0A0000F0u },
        [0x18] = { 0x00000000u, 0x00CF5F0Fu },
        [0x30] = { 0x0055B8F8u, 0x0F000307u },
        [0x31] = { 0x00000000u, 0x00010003u },
        [0x38] = { 0x8C0CCCFFu, 0x00000000u },
    };
    /* Fields an encoding must have zero (unused register fields, the bits above a
     * shift amount, an accumulator number or a mask). */
    static const struct { uint8_t fn, op; uint32_t zero; } zero[] = {
        { 0x0C,  0, 0x0000F800u },   /* insv */
        { 0x10, 20, 0x001F0000u },   /* raddu.w.qb */
        { 0x11,  0, 0x0000F800u },   /* cmpu.eq.qb */
        { 0x11,  1, 0x0000F800u },   /* cmpu.lt.qb */
        { 0x11,  2, 0x0000F800u },   /* cmpu.le.qb */
        { 0x11,  8, 0x0000F800u },   /* cmp.eq.ph */
        { 0x11,  9, 0x0000F800u },   /* cmp.lt.ph */
        { 0x11, 10, 0x0000F800u },   /* cmp.le.ph */
        { 0x12,  1, 0x03E00000u },   /* absq_s.qb */
        { 0x12,  2, 0x03000000u },   /* repl.qb */
        { 0x12,  3, 0x03E00000u },   /* replv.qb */
        { 0x12,  4, 0x03E00000u },   /* precequ.ph.qbl */
        { 0x12,  5, 0x03E00000u },   /* precequ.ph.qbr */
        { 0x12,  6, 0x03E00000u },   /* precequ.ph.qbla */
        { 0x12,  7, 0x03E00000u },   /* precequ.ph.qbra */
        { 0x12,  9, 0x03E00000u },   /* absq_s.ph */
        { 0x12, 11, 0x03E00000u },   /* replv.ph */
        { 0x12, 12, 0x03E00000u },   /* preceq.w.phl */
        { 0x12, 13, 0x03E00000u },   /* preceq.w.phr */
        { 0x12, 17, 0x03E00000u },   /* absq_s.w */
        { 0x12, 27, 0x03E00000u },   /* bitrev */
        { 0x12, 28, 0x03E00000u },   /* preceu.ph.qbl */
        { 0x12, 29, 0x03E00000u },   /* preceu.ph.qbr */
        { 0x12, 30, 0x03E00000u },   /* preceu.ph.qbla */
        { 0x12, 31, 0x03E00000u },   /* preceu.ph.qbra */
        { 0x13,  0, 0x03000000u },   /* shll.qb */
        { 0x13,  1, 0x03000000u },   /* shrl.qb */
        { 0x13,  4, 0x03000000u },   /* shra.qb */
        { 0x13,  5, 0x03000000u },   /* shra_r.qb */
        { 0x13,  8, 0x02000000u },   /* shll.ph */
        { 0x13,  9, 0x02000000u },   /* shra.ph */
        { 0x13, 12, 0x02000000u },   /* shll_s.ph */
        { 0x13, 13, 0x02000000u },   /* shra_r.ph */
        { 0x13, 25, 0x02000000u },   /* shrl.ph */
        { 0x30,  0, 0x0000E000u },   /* dpa.w.ph */
        { 0x30,  1, 0x0000E000u },   /* dps.w.ph */
        { 0x30,  2, 0x0000E000u },   /* mulsa.w.ph */
        { 0x30,  3, 0x0000E000u },   /* dpau.h.qbl */
        { 0x30,  4, 0x0000E000u },   /* dpaq_s.w.ph */
        { 0x30,  5, 0x0000E000u },   /* dpsq_s.w.ph */
        { 0x30,  6, 0x0000E000u },   /* mulsaq_s.w.ph */
        { 0x30,  7, 0x0000E000u },   /* dpau.h.qbr */
        { 0x30,  8, 0x0000E000u },   /* dpax.w.ph */
        { 0x30,  9, 0x0000E000u },   /* dpsx.w.ph */
        { 0x30, 11, 0x0000E000u },   /* dpsu.h.qbl */
        { 0x30, 12, 0x0000E000u },   /* dpaq_sa.l.w */
        { 0x30, 13, 0x0000E000u },   /* dpsq_sa.l.w */
        { 0x30, 15, 0x0000E000u },   /* dpsu.h.qbr */
        { 0x30, 16, 0x0000E000u },   /* maq_sa.w.phl */
        { 0x30, 18, 0x0000E000u },   /* maq_sa.w.phr */
        { 0x30, 20, 0x0000E000u },   /* maq_s.w.phl */
        { 0x30, 22, 0x0000E000u },   /* maq_s.w.phr */
        { 0x30, 24, 0x0000E000u },   /* dpaqx_s.w.ph */
        { 0x30, 25, 0x0000E000u },   /* dpsqx_s.w.ph */
        { 0x30, 26, 0x0000E000u },   /* dpaqx_sa.w.ph */
        { 0x30, 27, 0x0000E000u },   /* dpsqx_sa.w.ph */
        { 0x31, 16, 0x0000E000u },   /* balign */
        { 0x38,  0, 0x0000E000u },   /* extr.w */
        { 0x38,  1, 0x0000E000u },   /* extrv.w */
        { 0x38,  2, 0x0000E000u },   /* extp */
        { 0x38,  3, 0x0000E000u },   /* extpv */
        { 0x38,  4, 0x0000E000u },   /* extr_r.w */
        { 0x38,  5, 0x0000E000u },   /* extrv_r.w */
        { 0x38,  6, 0x0000E000u },   /* extr_rs.w */
        { 0x38,  7, 0x0000E000u },   /* extrv_rs.w */
        { 0x38, 10, 0x0000E000u },   /* extpdp */
        { 0x38, 11, 0x0000E000u },   /* extpdpv */
        { 0x38, 14, 0x0000E000u },   /* extr_s.h */
        { 0x38, 15, 0x0000E000u },   /* extrv_s.h */
        { 0x38, 18, 0x03C00000u },   /* rddsp */
        { 0x38, 19, 0x001E0000u },   /* wrdsp */
        { 0x38, 26, 0x000FE000u },   /* shilo */
        { 0x38, 27, 0x001FE000u },   /* shilov */
        { 0x38, 31, 0x001FE000u },   /* mthlip */
    };
    unsigned fn = i & 0x3F, op = (i >> 6) & 31;
    int rev = (ops[fn].r1 & (1u << op)) ? 1 : (ops[fn].r2 & (1u << op)) ? 2 : 0;
    for (unsigned k = 0; rev && k < UAM_ARRAY_LEN(zero); k++)
        if (zero[k].fn == fn && zero[k].op == op && (i & zero[k].zero))
            return 0;
    return rev;
}

lift_result mips_lift_dsp(ml *L, uint32_t i)
{
    int rev = mips_dsp_rev(i);
    if (!rev || (rev == 2 && !mips_has_dsp2(L->s))) {
        mips_ri(L);
        return LIFT_DONE;
    }
    if (!(L->priv & MIPS_PRIV_MX)) {
        mips_trap(L, NONE, EXC_DSPDIS, mips_imm(L, 0));
        return LIFT_DONE;
    }
    if ((i & 0x3F) == 0x0A) {   /* LWX / LHX / LBUX rd, index(base) */
        unsigned op = (i >> 6) & 31, w = op == 0 ? 32 : op == 4 ? 16 : 8;
        uam_ref a = mips_bin(L, UAM_OP_ADD, mips_reg_r(L, (i >> 21) & 31), mips_reg_r(L, (i >> 16) & 31));
        uam_ref v = uam_ld(L->b, w, L->endian | (w > 8 ? UAM_ACC_ALIGNED : 0), a, NONE);
        if (w < 32)
            v = uam_unop(L->b, w == 16 ? UAM_OP_SEXT : UAM_OP_ZEXT, 32, v);
        mips_reg_w(L, (i >> 11) & 31, v);
        return LIFT_SEQ;
    }
    mips_helper(L, MIPS_HELPER_DSP, mips_imm(L, i), NONE, NONE);
    return LIFT_SEQ;
}
