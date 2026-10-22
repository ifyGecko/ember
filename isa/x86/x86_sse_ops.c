/*
 * x86_sse_ops.c - The X86_H_SSE and X86_H_X87 helpers: SSE/SSE2 instructions
 * the lifter does not inline (integer unpack, shuffle, compare, add/sub,
 * shift, pack, min/max, movemask, insert/extract). The x87 is x86_x87.c.
 *
 * Floating-point arithmetic and conversions are x86_sse_fp.c. The integer ops
 * also run on 64-bit MMX registers (the forms without a mandatory prefix);
 * the 0F38/0F3A maps raise #UD (CPUID advertises only SSE2).
 */
#include <string.h>

#include "x86.h"

uint64_t x86_h_sse(void *env, uint64_t a, uint64_t b, uint64_t c, uam_status *st);
bool x86_sse_fp(uam_cpu *cpu, uint64_t desc, uint64_t lin, uam_status *st);

typedef union v128 { uint8_t b[16]; int8_t sb[16]; uint16_t w[8]; int16_t sw[8];
                     uint32_t d[4]; int32_t sd[4]; uint64_t q[2]; int64_t sq[2]; } v128;

static v128 xmm_get(uam_cpu *cpu, unsigned i)
{
    v128 v;
    v.q[0] = uam_cpu_reg_read(cpu, X86_OFF_XMM(i), 64);
    v.q[1] = uam_cpu_reg_read(cpu, X86_OFF_XMM(i) + 8, 64);
    return v;
}

static void xmm_set(uam_cpu *cpu, unsigned i, const v128 *v)
{
    uam_cpu_reg_write(cpu, X86_OFF_XMM(i), 64, v->q[0]);
    uam_cpu_reg_write(cpu, X86_OFF_XMM(i) + 8, 64, v->q[1]);
}

static uint8_t  sat_u8(int v)  { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
static int8_t   sat_s8(int v)  { return (int8_t)(v < -128 ? -128 : v > 127 ? 127 : v); }
static uint16_t sat_u16(int v) { return (uint16_t)(v < 0 ? 0 : v > 65535 ? 65535 : v); }
static int16_t  sat_s16(int v) { return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }

/* Shifts by a count (shared by the immediate and xmm forms). kind: 0 srl, 1 sra, 2 sll */
static void vshift(v128 *d, unsigned esz, unsigned kind, uint64_t n)
{
    unsigned bits = esz * 8;
    for (unsigned i = 0; i < 16 / esz; i++) {
        uint64_t v = esz == 2 ? d->w[i] : esz == 4 ? d->d[i] : d->q[i];
        int64_t sv = esz == 2 ? d->sw[i] : esz == 4 ? d->sd[i] : d->sq[i];
        uint64_t r;
        if (kind == 1) r = (uint64_t)(n >= bits ? (sv < 0 ? -1 : 0) : sv >> n);
        else if (n >= bits) r = 0;
        else r = kind == 0 ? v >> n : v << n;
        if (esz == 2) d->w[i] = (uint16_t)r;
        else if (esz == 4) d->d[i] = (uint32_t)r;
        else d->q[i] = r;
    }
}

/* Interleave elements of a vector of `vb` bytes (16, or 8 for MMX). */
static void unpack(v128 *d, const v128 *s, unsigned esz, bool high, unsigned vb)
{
    v128 r = *d;
    unsigned n = vb / esz / 2, off = high ? n : 0;
    for (unsigned i = 0; i < n; i++) {
        memcpy(&r.b[(2 * i) * esz], &d->b[(off + i) * esz], esz);
        memcpy(&r.b[(2 * i + 1) * esz], &s->b[(off + i) * esz], esz);
    }
    *d = r;
}

/* Integer ops on vectors of `vb` bytes: SSE2 with a 66 prefix (16), MMX (8; the
 * upper half is computed and ignored). Returns false if not implemented. */
static bool int_op(uam_cpu *cpu, unsigned op, v128 *d, const v128 *s, unsigned imm, unsigned reg,
                   unsigned vb)
{
    v128 r = *d;
    const unsigned hw = vb / 2, hd = vb / 4;   /* words, dwords per vector */
    switch (op) {
    case 0x60: unpack(&r, s, 1, false, vb); break;
    case 0x61: unpack(&r, s, 2, false, vb); break;
    case 0x62: unpack(&r, s, 4, false, vb); break;
    case 0x68: unpack(&r, s, 1, true, vb); break;
    case 0x69: unpack(&r, s, 2, true, vb); break;
    case 0x6A: unpack(&r, s, 4, true, vb); break;
    case 0x6C: unpack(&r, s, 8, false, vb); break;
    case 0x6D: unpack(&r, s, 8, true, vb); break;
    case 0x63: for (unsigned i = 0; i < hw; i++) r.sb[i] = sat_s8(d->sw[i]), r.sb[i + hw] = sat_s8(s->sw[i]); break;
    case 0x67: for (unsigned i = 0; i < hw; i++) r.b[i] = sat_u8(d->sw[i]), r.b[i + hw] = sat_u8(s->sw[i]); break;
    case 0x6B: for (unsigned i = 0; i < hd; i++) r.sw[i] = sat_s16(d->sd[i]), r.sw[i + hd] = sat_s16(s->sd[i]); break;
    case 0x64: for (int i = 0; i < 16; i++) r.b[i] = d->sb[i] > s->sb[i] ? 0xFF : 0; break;
    case 0x65: for (int i = 0; i < 8; i++) r.w[i] = d->sw[i] > s->sw[i] ? 0xFFFF : 0; break;
    case 0x66: for (int i = 0; i < 4; i++) r.d[i] = d->sd[i] > s->sd[i] ? ~0u : 0; break;
    case 0x74: for (int i = 0; i < 16; i++) r.b[i] = d->b[i] == s->b[i] ? 0xFF : 0; break;
    case 0x75: for (int i = 0; i < 8; i++) r.w[i] = d->w[i] == s->w[i] ? 0xFFFF : 0; break;
    case 0x76: for (int i = 0; i < 4; i++) r.d[i] = d->d[i] == s->d[i] ? ~0u : 0; break;
    case 0x70:   /* PSHUFD, or PSHUFW on MMX */
        for (int i = 0; i < 4; i++) {
            if (vb == 16) r.d[i] = s->d[(imm >> (2 * i)) & 3];
            else          r.w[i] = s->w[(imm >> (2 * i)) & 3];
        }
        break;
    case 0xDB: r.q[0] &= s->q[0]; r.q[1] &= s->q[1]; break;               /* PAND (MMX) */
    case 0xDF: r.q[0] = ~r.q[0] & s->q[0]; r.q[1] = ~r.q[1] & s->q[1]; break;
    case 0xEB: r.q[0] |= s->q[0]; r.q[1] |= s->q[1]; break;
    case 0xEF: r.q[0] ^= s->q[0]; r.q[1] ^= s->q[1]; break;
    case 0xD1: vshift(&r, 2, 0, s->q[0]); break;
    case 0xD2: vshift(&r, 4, 0, s->q[0]); break;
    case 0xD3: vshift(&r, 8, 0, s->q[0]); break;
    case 0xE1: vshift(&r, 2, 1, s->q[0]); break;
    case 0xE2: vshift(&r, 4, 1, s->q[0]); break;
    case 0xF1: vshift(&r, 2, 2, s->q[0]); break;
    case 0xF2: vshift(&r, 4, 2, s->q[0]); break;
    case 0xF3: vshift(&r, 8, 2, s->q[0]); break;
    case 0xD4: r.q[0] += s->q[0]; r.q[1] += s->q[1]; break;
    case 0xFB: r.q[0] -= s->q[0]; r.q[1] -= s->q[1]; break;
    case 0xFC: for (int i = 0; i < 16; i++) r.b[i] = (uint8_t)(d->b[i] + s->b[i]); break;
    case 0xFD: for (int i = 0; i < 8; i++) r.w[i] = (uint16_t)(d->w[i] + s->w[i]); break;
    case 0xFE: for (int i = 0; i < 4; i++) r.d[i] = d->d[i] + s->d[i]; break;
    case 0xF8: for (int i = 0; i < 16; i++) r.b[i] = (uint8_t)(d->b[i] - s->b[i]); break;
    case 0xF9: for (int i = 0; i < 8; i++) r.w[i] = (uint16_t)(d->w[i] - s->w[i]); break;
    case 0xFA: for (int i = 0; i < 4; i++) r.d[i] = d->d[i] - s->d[i]; break;
    case 0xD8: for (int i = 0; i < 16; i++) r.b[i] = sat_u8(d->b[i] - s->b[i]); break;
    case 0xD9: for (int i = 0; i < 8; i++) r.w[i] = sat_u16(d->w[i] - s->w[i]); break;
    case 0xDC: for (int i = 0; i < 16; i++) r.b[i] = sat_u8(d->b[i] + s->b[i]); break;
    case 0xDD: for (int i = 0; i < 8; i++) r.w[i] = sat_u16(d->w[i] + s->w[i]); break;
    case 0xE8: for (int i = 0; i < 16; i++) r.sb[i] = sat_s8(d->sb[i] - s->sb[i]); break;
    case 0xE9: for (int i = 0; i < 8; i++) r.sw[i] = sat_s16(d->sw[i] - s->sw[i]); break;
    case 0xEC: for (int i = 0; i < 16; i++) r.sb[i] = sat_s8(d->sb[i] + s->sb[i]); break;
    case 0xED: for (int i = 0; i < 8; i++) r.sw[i] = sat_s16(d->sw[i] + s->sw[i]); break;
    case 0xDA: for (int i = 0; i < 16; i++) r.b[i] = d->b[i] < s->b[i] ? d->b[i] : s->b[i]; break;
    case 0xDE: for (int i = 0; i < 16; i++) r.b[i] = d->b[i] > s->b[i] ? d->b[i] : s->b[i]; break;
    case 0xEA: for (int i = 0; i < 8; i++) r.sw[i] = d->sw[i] < s->sw[i] ? d->sw[i] : s->sw[i]; break;
    case 0xEE: for (int i = 0; i < 8; i++) r.sw[i] = d->sw[i] > s->sw[i] ? d->sw[i] : s->sw[i]; break;
    case 0xE0: for (int i = 0; i < 16; i++) r.b[i] = (uint8_t)((d->b[i] + s->b[i] + 1) >> 1); break;
    case 0xE3: for (int i = 0; i < 8; i++) r.w[i] = (uint16_t)((d->w[i] + s->w[i] + 1) >> 1); break;
    case 0xD5: for (int i = 0; i < 8; i++) r.w[i] = (uint16_t)(d->w[i] * s->w[i]); break;
    case 0xE4: for (int i = 0; i < 8; i++) r.w[i] = (uint16_t)(((uint32_t)d->w[i] * s->w[i]) >> 16); break;
    case 0xE5: for (int i = 0; i < 8; i++) r.w[i] = (uint16_t)(((int32_t)d->sw[i] * s->sw[i]) >> 16); break;
    case 0xF4: r.q[0] = (uint64_t)d->d[0] * s->d[0]; r.q[1] = (uint64_t)d->d[2] * s->d[2]; break;
    case 0xF5: for (int i = 0; i < 4; i++)
                   r.sd[i] = d->sw[2 * i] * s->sw[2 * i] + d->sw[2 * i + 1] * s->sw[2 * i + 1];
               break;
    case 0xF6: for (int h = 0; h < 2; h++) {
                   unsigned sum = 0;
                   for (int i = 0; i < 8; i++) {
                       int df = d->b[h * 8 + i] - s->b[h * 8 + i];
                       sum += (unsigned)(df < 0 ? -df : df);
                   }
                   r.q[h] = sum;
               }
               break;
    case 0xD7: {   /* PMOVMSKB r32, xmm/mm */
        uint32_t m = 0;
        for (unsigned i = 0; i < vb; i++) m |= (uint32_t)(s->b[i] >> 7) << i;
        x86_set_gpr(cpu, reg, m);
        return true;
    }
    case 0xC5:     /* PEXTRW r32, xmm/mm, imm8 */
        x86_set_gpr(cpu, reg, s->w[imm & (hw - 1)]);
        return true;
    default:
        return false;
    }
    *d = r;
    return true;
}

static uam_status load_src(uam_cpu *cpu, uint64_t desc, uint64_t lin, v128 *s, unsigned bytes)
{
    memset(s, 0, sizeof(*s));
    if (!X86_SSE_MEM(desc)) {
        *s = xmm_get(cpu, X86_SSE_RM(desc));
        return UAM_OK;
    }
    for (unsigned i = 0; i < bytes; i += 8) {
        uint64_t v = 0;
        uam_status st = x86_lin_read(cpu, lin + i, bytes - i >= 8 ? 8 : bytes - i, &v, false);
        if (st != UAM_OK)
            return st;
        s->q[i / 8] = v;
    }
    return UAM_OK;
}

/* MMX: the 0F integer forms without a prefix, on MM0-MM7. Returns false if
 * `op` is not one; otherwise *st tells how it went. */
static bool is_mmx(unsigned op)
{
    return (op >= 0x60 && op <= 0x6B) || op == 0x6E || op == 0x6F || (op >= 0x70 && op <= 0x77) ||
           op == 0x7E || op == 0x7F || op == 0xC4 || op == 0xC5 || (op >= 0xD1 && op <= 0xFE &&
           op != 0xD6 && op != 0xE6 && op != 0xF0 && op != 0xF7);
}

static bool mmx_op(uam_cpu *cpu, uint64_t desc, uint64_t lin, uam_status *st)
{
    x86_state *xs = x86_st(cpu);
    unsigned op = X86_SSE_OP(desc), reg = X86_SSE_REG(desc) & 7, rm = X86_SSE_RM(desc) & 7;
    unsigned imm = X86_SSE_IMM(desc);
    bool mem = X86_SSE_MEM(desc);
    uam_status r;
    if (xs->cr0 & CR0_EM) { *st = x86_raise(cpu, X86_EXC_UD, false, 0); return true; }
    if (xs->cr0 & CR0_TS) { *st = x86_raise(cpu, X86_EXC_NM, false, 0); return true; }
    if (op == 0x77) {   /* EMMS: the x87 registers are empty again */
        uam_cpu_reg_write(cpu, X86_OFF_FTW, 8, 0);
        xs->mmx = false;
        return true;
    }
    /* any other MMX instruction: every x87 register valid, TOP = 0 */
    uam_cpu_reg_write(cpu, X86_OFF_FTW, 8, 0xFF);
    uam_cpu_reg_write(cpu, X86_OFF_FSW, 16, uam_cpu_reg_read(cpu, X86_OFF_FSW, 16) & ~0x3800u);
    xs->mmx = true;
    uint64_t v = 0;
    switch (op) {
    case 0x6E:   /* MOVD/MOVQ mm, r/m32/64 */
        if (mem) {
            if ((r = x86_lin_read(cpu, lin, X86_SSE_REXW(desc) ? 8 : 4, &v, false)) != UAM_OK) { *st = r; return true; }
        } else {
            v = x86_gpr(cpu, X86_SSE_RM(desc));
            if (!X86_SSE_REXW(desc)) v &= 0xFFFFFFFFu;
        }
        xs->mm[reg] = v;
        return true;
    case 0x7E:   /* MOVD/MOVQ r/m32/64, mm */
        v = X86_SSE_REXW(desc) ? xs->mm[reg] : (xs->mm[reg] & 0xFFFFFFFFu);
        if (mem)
            *st = x86_lin_write(cpu, lin, X86_SSE_REXW(desc) ? 8 : 4, v, false);
        else
            x86_set_gpr(cpu, X86_SSE_RM(desc), v);
        return true;
    case 0x6F:   /* MOVQ mm, mm/m64 */
        if (mem) {
            if ((r = x86_lin_read(cpu, lin, 8, &v, false)) != UAM_OK) { *st = r; return true; }
            xs->mm[reg] = v;
        } else {
            xs->mm[reg] = xs->mm[rm];
        }
        return true;
    case 0x7F: case 0xE7:   /* MOVQ / MOVNTQ mm/m64, mm */
        if (mem)
            *st = x86_lin_write(cpu, lin, 8, xs->mm[reg], false);
        else if (op == 0x7F)
            xs->mm[rm] = xs->mm[reg];
        else
            *st = x86_raise(cpu, X86_EXC_UD, false, 0);
        return true;
    case 0x71: case 0x72: case 0x73: {   /* shift groups by imm8 */
        unsigned sub = (unsigned)((desc >> 12) & 7);
        unsigned esz = op == 0x71 ? 2 : op == 0x72 ? 4 : 8;
        if (mem || !(sub == 2 || sub == 4 || sub == 6) || (sub == 4 && esz == 8)) {
            *st = x86_raise(cpu, X86_EXC_UD, false, 0);
            return true;
        }
        v128 t = { .q = { xs->mm[rm], 0 } };
        vshift(&t, esz, sub == 2 ? 0 : sub == 4 ? 1 : 2, imm);
        xs->mm[rm] = t.q[0];
        return true;
    }
    case 0xC4: {   /* PINSRW mm, r32/m16, imm8 */
        if (mem) {
            if ((r = x86_lin_read(cpu, lin, 2, &v, false)) != UAM_OK) { *st = r; return true; }
        } else {
            v = x86_gpr(cpu, X86_SSE_RM(desc));
        }
        v128 t = { .q = { xs->mm[reg], 0 } };
        t.w[imm & 3] = (uint16_t)v;
        xs->mm[reg] = t.q[0];
        return true;
    }
    }
    v128 d = { .q = { xs->mm[reg], 0 } }, s = { .q = { 0, 0 } };
    if (mem) {
        if ((r = x86_lin_read(cpu, lin, 8, &s.q[0], false)) != UAM_OK) { *st = r; return true; }
    } else {
        s.q[0] = xs->mm[rm];
    }
    if ((op == 0xC5 || op == 0xD7) && mem) {
        *st = x86_raise(cpu, X86_EXC_UD, false, 0);
        return true;
    }
    if (!int_op(cpu, op, &d, &s, imm, X86_SSE_REG(desc), 8)) {
        *st = x86_raise(cpu, X86_EXC_UD, false, 0);
        return true;
    }
    if (op != 0xC5 && op != 0xD7)   /* those write a general register */
        xs->mm[reg] = d.q[0];
    return true;
}

uint64_t x86_h_sse(void *env, uint64_t desc, uint64_t lin, uint64_t c, uam_status *st)
{
    uam_cpu *cpu = env;
    x86_state *xs = x86_st(cpu);
    (void)c;
    unsigned op = X86_SSE_OP(desc), pfx = X86_SSE_PFX(desc), reg = X86_SSE_REG(desc);
    unsigned imm = X86_SSE_IMM(desc);
    v128 d = xmm_get(cpu, reg), s;
    uam_status r;

    if (X86_SSE_MAP(desc) == 1 && pfx == X86_PFX_NONE && is_mmx(op) && mmx_op(cpu, desc, lin, st))
        return 0;
    if ((xs->cr0 & CR0_EM) || !(xs->cr4 & CR4_OSFXSR) || X86_SSE_MAP(desc) != 1)
        goto ud;
    if (xs->cr0 & CR0_TS) { *st = x86_raise(cpu, X86_EXC_NM, false, 0); return 0; }
    if (x86_sse_fp(cpu, desc, lin, st))
        return 0;

    if (pfx == X86_PFX_66) {
        if (op == 0x71 || op == 0x72 || op == 0x73) {   /* shift groups by imm8 */
            if (X86_SSE_MEM(desc)) goto ud;
            unsigned sub = (unsigned)((desc >> 12) & 7);   /* ModRM.reg without REX.R */
            unsigned rm = X86_SSE_RM(desc);
            v128 v = xmm_get(cpu, rm);
            unsigned esz = op == 0x71 ? 2 : op == 0x72 ? 4 : 8;
            if (op == 0x73 && (sub == 3 || sub == 7)) {   /* PSRLDQ / PSLLDQ */
                v128 o; memset(&o, 0, sizeof(o));
                unsigned n = imm > 16 ? 16 : imm;
                for (unsigned i = 0; i < 16; i++) {
                    int src = sub == 3 ? (int)(i + n) : (int)i - (int)n;
                    if (src >= 0 && src < 16) o.b[i] = v.b[src];
                }
                v = o;
            } else if (sub == 2 || sub == 4 || sub == 6) {
                if (sub == 4 && esz == 8) goto ud;
                vshift(&v, esz, sub == 2 ? 0 : sub == 4 ? 1 : 2, imm);
            } else {
                goto ud;
            }
            xmm_set(cpu, rm, &v);
            return 0;
        }
        if (op == 0xC4) {   /* PINSRW xmm, r32/m16, imm8 */
            uint64_t v = 0;
            if (X86_SSE_MEM(desc)) {
                if ((r = x86_lin_read(cpu, lin, 2, &v, false)) != UAM_OK) { *st = r; return 0; }
            } else {
                v = x86_gpr(cpu, X86_SSE_RM(desc));
            }
            d.w[imm & 7] = (uint16_t)v;
            xmm_set(cpu, reg, &d);
            return 0;
        }
        if ((r = load_src(cpu, desc, lin, &s, 16)) != UAM_OK) { *st = r; return 0; }
        if (op == 0xC5 || op == 0xD7) {
            if (X86_SSE_MEM(desc)) goto ud;
            int_op(cpu, op, &d, &s, imm, reg, 16);
            return 0;
        }
        if (op == 0x14 || op == 0x15) { unpack(&d, &s, 8, op == 0x15, 16); xmm_set(cpu, reg, &d); return 0; }
        if (op == 0x50) {   /* MOVMSKPD */
            x86_set_gpr(cpu, reg, (s.q[0] >> 63) | (s.q[1] >> 63) << 1);
            return 0;
        }
        if (op == 0xC6) {   /* SHUFPD */
            v128 o;
            o.q[0] = d.q[imm & 1];
            o.q[1] = s.q[(imm >> 1) & 1];
            xmm_set(cpu, reg, &o);
            return 0;
        }
        if (!int_op(cpu, op, &d, &s, imm, reg, 16)) goto ud;
        xmm_set(cpu, reg, &d);
        return 0;
    }

    if (pfx == X86_PFX_NONE) {
        if ((r = load_src(cpu, desc, lin, &s, 16)) != UAM_OK) { *st = r; return 0; }
        switch (op) {
        case 0x14: case 0x15: unpack(&d, &s, 4, op == 0x15, 16); xmm_set(cpu, reg, &d); return 0;
        case 0x50: {
            if (X86_SSE_MEM(desc)) goto ud;
            uint32_t m = 0;
            for (int i = 0; i < 4; i++) m |= (s.d[i] >> 31) << i;
            x86_set_gpr(cpu, reg, m);
            return 0;
        }
        case 0xC6: {
            v128 o;
            o.d[0] = d.d[imm & 3]; o.d[1] = d.d[(imm >> 2) & 3];
            o.d[2] = s.d[(imm >> 4) & 3]; o.d[3] = s.d[(imm >> 6) & 3];
            xmm_set(cpu, reg, &o);
            return 0;
        }
        }
        goto ud;   /* floating point not handled above */
    }

    if (op == 0x70) {   /* PSHUFHW (F3) / PSHUFLW (F2) */
        if ((r = load_src(cpu, desc, lin, &s, 16)) != UAM_OK) { *st = r; return 0; }
        v128 o = s;
        unsigned base = pfx == X86_PFX_F3 ? 4 : 0;
        for (unsigned i = 0; i < 4; i++)
            o.w[base + i] = s.w[base + ((imm >> (2 * i)) & 3)];
        xmm_set(cpu, reg, &o);
        return 0;
    }
ud:
    *st = x86_raise(cpu, X86_EXC_UD, false, 0);
    return 0;
}
