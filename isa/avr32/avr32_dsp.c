/*
 * avr32_dsp.c - The saturating DSP operations and the SIMD instructions, as pure
 * functions for the A32_H_DSP and A32_H_SIMD helpers (AVR32 Architecture Manual,
 * chapter 9: SATADD, SATSUB, SATS, SATU, SATRNDS, SATRNDU, MULSAT*, MACSATHH.W,
 * and the packed P* instructions).
 *
 * Each returns the 32-bit result in bits 31:0 and above it the flags the
 * instruction sets: bit 32 "saturated" (ORed into Q), and for the saturating
 * add/subtract bits 33..35 V, N, Z; for the packed max/min bits 33..36 C, Z, N, V.
 */
#include "avr32.h"
#include "avr32_op.h"

#define Q_OUT (UINT64_C(1) << 32)

static int32_t s16(uint32_t v) { return (int16_t)(uint16_t)v; }
static int32_t s8(uint32_t v) { return (int8_t)(uint8_t)v; }

/* Saturate v to a signed / unsigned field of `bits` bits; *sat when it had to. */
static int32_t sats(int64_t v, unsigned bits, bool *sat)
{
    int64_t hi = (INT64_C(1) << (bits - 1)) - 1, lo = -(INT64_C(1) << (bits - 1));
    if (v > hi) { *sat = true; return (int32_t)hi; }
    if (v < lo) { *sat = true; return (int32_t)lo; }
    return (int32_t)v;
}
static uint32_t satu(int64_t v, unsigned bits, bool *sat)
{
    int64_t hi = (INT64_C(1) << bits) - 1;
    if (v > hi) { *sat = true; return (uint32_t)hi; }
    if (v < 0) { *sat = true; return 0; }
    return (uint32_t)v;
}

static uint64_t addsub_flags(uint32_t res, bool ovf, unsigned w)
{
    uint64_t r = res;
    if (ovf)
        r |= Q_OUT | UINT64_C(1) << 33;
    if ((res >> (w - 1)) & 1)
        r |= UINT64_C(1) << 34;
    if (!(w == 32 ? res : res & 0xFFFF))
        r |= UINT64_C(1) << 35;
    return r;
}

/* SATS, SATU, SATRNDS, SATRNDU: y = sa | bp << 5. */
static uint64_t saturate(uint32_t rd, uint32_t y, bool sgn, bool rnd)
{
    unsigned sa = y & 31, bp = (y >> 5) & 31;
    int64_t t = (int32_t)rd >> sa;
    if (rnd && sa)
        t += (rd >> (sa - 1)) & 1;
    t = (int32_t)(uint32_t)t;   /* the 32-bit Temp of the manual */
    if (bp == 0)
        return (uint32_t)t;
    bool sat = false;
    uint32_t r;
    if (sgn) {
        int64_t lim = INT64_C(1) << (bp - 1);
        if (t >= lim) { r = (uint32_t)(lim - 1); sat = true; }
        else if (t < -lim) { r = (uint32_t)-lim; sat = true; }
        else r = (uint32_t)t;
    } else {
        uint64_t lim = UINT64_C(1) << bp;
        if (t < 0) { r = 0; sat = true; }
        else if ((uint64_t)t >= lim) { r = (uint32_t)(lim - 1); sat = true; }
        else r = (uint32_t)t;
    }
    return r | (sat ? Q_OUT : 0);
}

uint64_t avr32_dsp(avr32_state *s, unsigned op, uint32_t x, uint32_t y)
{
    (void)s;
    switch (op) {
    case DSP_SATADD_H: case DSP_SATSUB_H: {
        bool add = op == DSP_SATADD_H;
        uint32_t t = add ? (x & 0xFFFF) + (y & 0xFFFF) : (x & 0xFFFF) - (y & 0xFFFF);
        bool x15 = (x >> 15) & 1, y15 = (y >> 15) & 1, t15 = (t >> 15) & 1;
        bool ovf = add ? ((x15 && y15 && !t15) || (!x15 && !y15 && t15))
                       : ((x15 && !y15 && !t15) || (!x15 && y15 && t15));
        uint32_t r = ovf ? (x15 ? 0xFFFF8000u : 0x00007FFFu) : (uint32_t)s16(t);
        return addsub_flags(r, ovf, 16);
    }
    case DSP_SATADD_W: case DSP_SATSUB_W: case DSP_MACSATHH_W: {
        bool add = op != DSP_SATSUB_W;
        uint32_t t = add ? x + y : x - y;
        bool x31 = x >> 31, y31 = y >> 31, t31 = t >> 31;
        bool ovf = add ? ((x31 && y31 && !t31) || (!x31 && !y31 && t31))
                       : ((x31 && !y31 && !t31) || (!x31 && y31 && t31));
        uint32_t r = ovf ? (x31 ? 0x80000000u : 0x7FFFFFFFu) : t;
        return addsub_flags(r, ovf, 32);
    }
    case DSP_SATS:    return saturate(x, y, true, false);
    case DSP_SATU:    return saturate(x, y, false, false);
    case DSP_SATRNDS: return saturate(x, y, true, true);
    case DSP_SATRNDU: return saturate(x, y, false, true);
    case DSP_MULSATHH_H: case DSP_MULSATHH_W: case DSP_MULSATRNDHH_H: {
        /* x, y: sign-extended halfwords */
        if (x == 0xFFFF8000u && y == 0xFFFF8000u)
            return (op == DSP_MULSATHH_W ? 0x7FFFFFFFu : 0x7FFFu) | Q_OUT;
        int64_t p = (int64_t)(int32_t)x * (int32_t)y;
        if (op == DSP_MULSATHH_W)
            return (uint32_t)(p << 1);
        if (op == DSP_MULSATRNDHH_H)
            p += 0x4000;
        return (uint32_t)s16((uint32_t)(p >> 15));
    }
    case DSP_MULSATWH_W: case DSP_MULSATRNDWH_W: {
        /* x: the word, y: a sign-extended halfword */
        if (x == 0x80000000u && y == 0xFFFF8000u)
            return 0x7FFFFFFFu | Q_OUT;
        int64_t p = (int64_t)(int32_t)x * (int32_t)y;
        if (op == DSP_MULSATRNDWH_W)
            p += 0x4000;
        return (uint32_t)(p >> 15);
    }
    default:
        return 0;
    }
}

/* ---- SIMD ------------------------------------------------------------------ */

static uint32_t lane(uint32_t v, unsigned i, unsigned w) { return (v >> (i * w)) & ((1u << w) - 1); }

uint64_t avr32_simd(avr32_state *s, uint32_t insn, uint32_t x, uint32_t y)
{
    (void)s;
    const unsigned op = avr32_decode(insn);
    const unsigned px = (insn >> 5) & 1, py = (insn >> 4) & 1;   /* the :t/:b operand parts */
    uint32_t r = 0;
    bool sat = false;
    uint64_t fl = 0;

    switch (op) {
    case A32_PADD_B_RRR_32: case A32_PSUB_B_RRR_32:
        for (unsigned i = 0; i < 4; i++) {
            uint32_t a = lane(x, i, 8), b = lane(y, i, 8);
            r |= ((op == A32_PADD_B_RRR_32 ? a + b : a - b) & 0xFF) << (8 * i);
        }
        break;
    case A32_PADD_H_RRR_32: case A32_PSUB_H_RRR_32:
        for (unsigned i = 0; i < 2; i++) {
            uint32_t a = lane(x, i, 16), b = lane(y, i, 16);
            r |= ((op == A32_PADD_H_RRR_32 ? a + b : a - b) & 0xFFFF) << (16 * i);
        }
        break;
    case A32_PADDX_H_RRR_32: case A32_PSUBX_H_RRR_32: {
        bool add = op == A32_PADDX_H_RRR_32;
        uint32_t hi = add ? lane(x, 1, 16) + lane(y, 0, 16) : lane(x, 1, 16) - lane(y, 0, 16);
        uint32_t lo = add ? lane(x, 0, 16) + lane(y, 1, 16) : lane(x, 0, 16) - lane(y, 1, 16);
        r = (hi & 0xFFFF) << 16 | (lo & 0xFFFF);
        break;
    }
    case A32_PADDS_UB_RRR_32: case A32_PADDS_SB_RRR_32: case A32_PSUBS_UB_RRR_32: case A32_PSUBS_SB_RRR_32: {
        bool add = op == A32_PADDS_UB_RRR_32 || op == A32_PADDS_SB_RRR_32;
        bool sg = op == A32_PADDS_SB_RRR_32 || op == A32_PSUBS_SB_RRR_32;
        for (unsigned i = 0; i < 4; i++) {
            int64_t a = sg ? (int64_t)s8(lane(x, i, 8)) : (int64_t)lane(x, i, 8), b = sg ? (int64_t)s8(lane(y, i, 8)) : (int64_t)lane(y, i, 8);
            int64_t v = add ? a + b : a - b;
            uint32_t q = sg ? (uint32_t)sats(v, 8, &sat) : satu(v, 8, &sat);
            r |= (q & 0xFF) << (8 * i);
        }
        break;
    }
    case A32_PADDS_UH_RRR_32: case A32_PADDS_SH_RRR_32: case A32_PSUBS_UH_RRR_32: case A32_PSUBS_SH_RRR_32: {
        bool add = op == A32_PADDS_UH_RRR_32 || op == A32_PADDS_SH_RRR_32;
        bool sg = op == A32_PADDS_SH_RRR_32 || op == A32_PSUBS_SH_RRR_32;
        for (unsigned i = 0; i < 2; i++) {
            int64_t a = sg ? (int64_t)s16(lane(x, i, 16)) : (int64_t)lane(x, i, 16), b = sg ? (int64_t)s16(lane(y, i, 16)) : (int64_t)lane(y, i, 16);
            int64_t v = add ? a + b : a - b;
            uint32_t q = sg ? (uint32_t)sats(v, 16, &sat) : satu(v, 16, &sat);
            r |= (q & 0xFFFF) << (16 * i);
        }
        break;
    }
    case A32_PADDXS_UH_RRR_32: case A32_PADDXS_SH_RRR_32: case A32_PSUBXS_UH_RRR_32: case A32_PSUBXS_SH_RRR_32: {
        bool add = op == A32_PADDXS_UH_RRR_32 || op == A32_PADDXS_SH_RRR_32;
        bool sg = op == A32_PADDXS_SH_RRR_32 || op == A32_PSUBXS_SH_RRR_32;
        for (unsigned i = 0; i < 2; i++) {
            int64_t a = sg ? (int64_t)s16(lane(x, i, 16)) : (int64_t)lane(x, i, 16);
            int64_t b = sg ? (int64_t)s16(lane(y, 1 - i, 16)) : (int64_t)lane(y, 1 - i, 16);
            int64_t v = add ? a + b : a - b;
            uint32_t q = sg ? (uint32_t)sats(v, 16, &sat) : satu(v, 16, &sat);
            r |= (q & 0xFFFF) << (16 * i);
        }
        break;
    }
    case A32_PADDH_UB_RRR_32: case A32_PSUBH_UB_RRR_32: case A32_PAVG_UB_RRR_32:
        for (unsigned i = 0; i < 4; i++) {
            uint32_t a = lane(x, i, 8), b = lane(y, i, 8);
            uint32_t v = op == A32_PSUBH_UB_RRR_32 ? a - b : a + b + (op == A32_PAVG_UB_RRR_32);
            r |= ((v & 0x1FF) >> 1) << (8 * i);
        }
        break;
    case A32_PADDH_SH_RRR_32: case A32_PSUBH_SH_RRR_32: case A32_PAVG_SH_RRR_32:
        for (unsigned i = 0; i < 2; i++) {
            int32_t a = s16(lane(x, i, 16)), b = s16(lane(y, i, 16));
            int32_t v = op == A32_PSUBH_SH_RRR_32 ? a - b : a + b + (op == A32_PAVG_SH_RRR_32);
            r |= ((uint32_t)(v >> 1) & 0xFFFF) << (16 * i);
        }
        break;
    case A32_PADDXH_SH_RRR_32: case A32_PSUBXH_SH_RRR_32:
        for (unsigned i = 0; i < 2; i++) {
            int32_t a = s16(lane(x, i, 16)), b = s16(lane(y, 1 - i, 16));
            int32_t v = op == A32_PADDXH_SH_RRR_32 ? a + b : a - b;
            r |= ((uint32_t)(v >> 1) & 0xFFFF) << (16 * i);
        }
        break;
    case A32_PADDSUB_H_RRR_32: case A32_PSUBADD_H_RRR_32: case A32_PADDSUBH_SH_RRR_32:
    case A32_PSUBADDH_SH_RRR_32: case A32_PADDSUBS_UH_RRR_32: case A32_PADDSUBS_SH_RRR_32:
    case A32_PSUBADDS_UH_RRR_32: case A32_PSUBADDS_SH_RRR_32: {
        /* the top result is op1 + op2 (paddsub) or op1 - op2 (psubadd), the bottom the other */
        bool addfirst = op == A32_PADDSUB_H_RRR_32 || op == A32_PADDSUBH_SH_RRR_32
                     || op == A32_PADDSUBS_UH_RRR_32 || op == A32_PADDSUBS_SH_RRR_32;
        uint32_t o1 = lane(x, px, 16), o2 = lane(y, py, 16);
        int64_t hi, lo;
        bool sg = op == A32_PADDSUBH_SH_RRR_32 || op == A32_PSUBADDH_SH_RRR_32
               || op == A32_PADDSUBS_SH_RRR_32 || op == A32_PSUBADDS_SH_RRR_32;
        int64_t a = sg ? (int64_t)s16(o1) : (int64_t)o1, b = sg ? (int64_t)s16(o2) : (int64_t)o2;
        hi = addfirst ? a + b : a - b;
        lo = addfirst ? a - b : a + b;
        if (op == A32_PADDSUBH_SH_RRR_32 || op == A32_PSUBADDH_SH_RRR_32) {
            hi >>= 1;
            lo >>= 1;
        } else if (op != A32_PADDSUB_H_RRR_32 && op != A32_PSUBADD_H_RRR_32) {
            if (sg) {
                hi = sats(hi, 16, &sat);
                lo = sats(lo, 16, &sat);
            } else {
                hi = satu(hi, 16, &sat);
                lo = satu(lo, 16, &sat);
            }
        }
        r = ((uint32_t)hi & 0xFFFF) << 16 | ((uint32_t)lo & 0xFFFF);
        break;
    }
    case A32_PABS_SB_RR_32:
        for (unsigned i = 0; i < 4; i++) {
            int32_t a = s8(lane(y, i, 8));
            r |= ((uint32_t)(a < 0 ? -a : a) & 0xFF) << (8 * i);
        }
        break;
    case A32_PABS_SH_RR_32:
        for (unsigned i = 0; i < 2; i++) {
            int32_t a = s16(lane(y, i, 16));
            r |= ((uint32_t)(a < 0 ? -a : a) & 0xFFFF) << (16 * i);
        }
        break;
    case A32_PACKSH_UB_RRR_32: case A32_PACKSH_SB_RRR_32: {
        bool sg = op == A32_PACKSH_SB_RRR_32;
        int32_t v[4] = { s16(lane(y, 0, 16)), s16(lane(y, 1, 16)), s16(lane(x, 0, 16)), s16(lane(x, 1, 16)) };
        for (unsigned i = 0; i < 4; i++) {
            uint32_t q = sg ? (uint32_t)sats(v[i], 8, &sat) : satu(v[i], 8, &sat);
            r |= (q & 0xFF) << (8 * i);
        }
        break;
    }
    case A32_PACKW_SH_RRR_32:
        r = ((uint32_t)sats((int32_t)x, 16, &sat) & 0xFFFF) << 16 | ((uint32_t)sats((int32_t)y, 16, &sat) & 0xFFFF);
        break;
    case A32_PUNPCKUB_H_RR_32: case A32_PUNPCKSB_H_RR_32: {
        unsigned top = (insn >> 4) & 1;
        uint32_t b1 = lane(x, top ? 3 : 1, 8), b0 = lane(x, top ? 2 : 0, 8);
        if (op == A32_PUNPCKSB_H_RR_32) {
            b1 = (uint32_t)s8(b1) & 0xFFFF;
            b0 = (uint32_t)s8(b0) & 0xFFFF;
        }
        r = b1 << 16 | b0;
        break;
    }
    case A32_PASR_B_RRI_32: case A32_PLSL_B_RRI_32: case A32_PLSR_B_RRI_32: {
        unsigned sa = (insn >> 16) & 7;
        for (unsigned i = 0; i < 4; i++) {
            uint32_t a = lane(x, i, 8), v;
            if (op == A32_PASR_B_RRI_32) v = (uint32_t)(s8(a) >> sa);
            else if (op == A32_PLSL_B_RRI_32) v = a << sa;
            else v = a >> sa;
            r |= (v & 0xFF) << (8 * i);
        }
        break;
    }
    case A32_PASR_H_RRI_32: case A32_PLSL_H_RRI_32: case A32_PLSR_H_RRI_32: {
        unsigned sa = (insn >> 16) & 15;
        for (unsigned i = 0; i < 2; i++) {
            uint32_t a = lane(x, i, 16), v;
            if (op == A32_PASR_H_RRI_32) v = (uint32_t)(s16(a) >> sa);
            else if (op == A32_PLSL_H_RRI_32) v = a << sa;
            else v = a >> sa;
            r |= (v & 0xFFFF) << (16 * i);
        }
        break;
    }
    case A32_PMAX_UB_RRR_32: case A32_PMIN_UB_RRR_32:
        for (unsigned i = 0; i < 4; i++) {
            uint32_t a = lane(x, i, 8), b = lane(y, i, 8);
            bool c = op == A32_PMAX_UB_RRR_32 ? a > b : a < b;
            r |= (c ? a : b) << (8 * i);
            /* lane 3 -> C, 2 -> Z, 1 -> N, 0 -> V */
            if (c)
                fl |= UINT64_C(1) << (33 + (3 - i));
        }
        break;
    case A32_PMAX_SH_RRR_32: case A32_PMIN_SH_RRR_32:
        for (unsigned i = 0; i < 2; i++) {
            int32_t a = s16(lane(x, i, 16)), b = s16(lane(y, i, 16));
            bool c = op == A32_PMAX_SH_RRR_32 ? a > b : a < b;
            r |= ((uint32_t)(c ? a : b) & 0xFFFF) << (16 * i);
            if (c)                        /* lane 1 -> C, lane 0 -> Z */
                fl |= UINT64_C(1) << (33 + (1 - i));
        }
        break;
    case A32_PSAD_RRR_32:
        for (unsigned i = 0; i < 4; i++) {
            int32_t d = (int32_t)lane(x, i, 8) - (int32_t)lane(y, i, 8);
            r += (uint32_t)(d < 0 ? -d : d);
        }
        break;
    default:
        break;
    }
    return r | fl | (sat ? Q_OUT : 0);
}
