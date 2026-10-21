/*
 * x86_sse.c - x86 lifter: SSE/SSE2 and the x87 subset firmware uses.
 *
 * The data moves and bitwise operations that compiled firmware runs in its
 * memory and structure copies are inlined as pairs of 64-bit GET/PUT/LD/ST.
 * Every other SSE instruction (unpacks, shuffles, packed compares and
 * arithmetic, 0F38/0F3A) goes to the X86_H_SSE helper with a descriptor of
 * the decoded instruction. MMX forms (no mandatory 66 prefix on an integer
 * op) are not supported and raise #UD there.
 */
#include "x86_lift.h"

static uam_ref xmm(xl *x, unsigned i, unsigned half)
{
    return uam_get(x->b, 64, X86_OFF_XMM(i) + 8u * half);
}

static void set_xmm(xl *x, unsigned i, unsigned half, uam_ref v)
{
    uam_put(x->b, 64, X86_OFF_XMM(i) + 8u * half, v, NONE);
}

static unsigned pfx(xl *x)
{
    if (x->in.last_rep == 0xF3) return X86_PFX_F3;
    if (x->in.last_rep == 0xF2) return X86_PFX_F2;
    return x->in.p66 ? X86_PFX_66 : X86_PFX_NONE;
}

/* SSE needs CR0.EM = 0 and CR4.OSFXSR; the helper checks those it can see. */
static lr sse_helper(xl *x)
{
    const x86_insn *in = &x->in;
    bool mem = in->mod != 3;
    uam_ref lin = mem ? x_rm(x).lin : x_imm(x, 0);
    uint64_t d = X86_SSE_DESC(in->op, in->map, pfx(x), in->reg & 15, in->rm & 15, mem,
                              in->rex_w, in->imm & 0xFF);
    x_helper(x, X86_H_SSE, x_imm(x, d), lin, NONE);
    return LR_CONT;
}

/* 128-bit load or copy into xmm[reg] */
static lr mov128_in(xl *x)
{
    uam_ref lo, hi;
    if (x->in.mod == 3) {
        lo = xmm(x, x->in.rm, 0);
        hi = xmm(x, x->in.rm, 1);
    } else {
        uam_ref lin = x_rm(x).lin;
        lo = x_ld(x, 64, lin);
        hi = x_ld(x, 64, x_bin(x, UAM_OP_ADD, 64, lin, x_imm(x, 8)));
    }
    set_xmm(x, x->in.reg, 0, lo);
    set_xmm(x, x->in.reg, 1, hi);
    return LR_CONT;
}

/* 128-bit store or copy from xmm[reg] */
static lr mov128_out(xl *x)
{
    uam_ref lo = xmm(x, x->in.reg, 0), hi = xmm(x, x->in.reg, 1);
    if (x->in.mod == 3) {
        set_xmm(x, x->in.rm, 0, lo);
        set_xmm(x, x->in.rm, 1, hi);
    } else {
        uam_ref lin = x_rm(x).lin;
        x_st(x, 64, lin, lo);
        x_st(x, 64, x_bin(x, UAM_OP_ADD, 64, lin, x_imm(x, 8)), hi);
    }
    return LR_CONT;
}

/* MOVSS/MOVSD (F3/F2 0F 10/11): scalar of w bits */
static lr movs_scalar(xl *x, unsigned w, bool store)
{
    if (store) {
        uam_ref v = x_trunc(x, w, 64, xmm(x, x->in.reg, 0));
        if (x->in.mod != 3) {
            x_st(x, w, x_rm(x).lin, v);
            return LR_CONT;
        }
        uam_put(x->b, w, X86_OFF_XMM(x->in.rm), v, NONE);
        return LR_CONT;
    }
    if (x->in.mod == 3) {   /* register form merges the low element only */
        uam_put(x->b, w, X86_OFF_XMM(x->in.reg), x_trunc(x, w, 64, xmm(x, x->in.rm, 0)), NONE);
        return LR_CONT;
    }
    uam_ref v = x_ld(x, w, x_rm(x).lin);   /* memory form zeroes the rest */
    set_xmm(x, x->in.reg, 0, x_zext(x, 64, w, v));
    set_xmm(x, x->in.reg, 1, x_imm(x, 0));
    return LR_CONT;
}

/* 64-bit half moves: MOVLPS/MOVHPS/MOVLPD/MOVHPD, MOVHLPS/MOVLHPS (0F 12/13/16/17) */
static lr mov_half(xl *x)
{
    unsigned op = x->in.op, half = (op >= 0x16) ? 1 : 0;
    if (op & 1) {   /* store */
        if (x->in.mod == 3)
            return x_ud(x);
        x_st(x, 64, x_rm(x).lin, xmm(x, x->in.reg, half));
        return LR_CONT;
    }
    uam_ref v = (x->in.mod == 3) ? xmm(x, x->in.rm, half ^ 1) : x_ld(x, 64, x_rm(x).lin);
    set_xmm(x, x->in.reg, half, v);
    return LR_CONT;
}

/* bitwise ops on 128 bits: kind 0 AND, 1 ANDN, 2 OR, 3 XOR */
static lr logic128(xl *x, unsigned kind)
{
    uam_ref s[2];
    if (x->in.mod == 3) {
        s[0] = xmm(x, x->in.rm, 0);
        s[1] = xmm(x, x->in.rm, 1);
    } else {
        uam_ref lin = x_rm(x).lin;
        s[0] = x_ld(x, 64, lin);
        s[1] = x_ld(x, 64, x_bin(x, UAM_OP_ADD, 64, lin, x_imm(x, 8)));
    }
    for (unsigned h = 0; h < 2; h++) {
        uam_ref d = xmm(x, x->in.reg, h), r;
        if (kind == 3 && x->in.mod == 3 && x->in.rm == x->in.reg) {
            r = x_imm(x, 0);   /* the xor-zero idiom */
        } else switch (kind) {
        case 0: r = x_bin(x, UAM_OP_AND, 64, d, s[h]); break;
        case 1: r = x_bin(x, UAM_OP_AND, 64, x_bin(x, UAM_OP_XOR, 64, d, x_imm(x, ~0ull)), s[h]); break;
        case 2: r = x_bin(x, UAM_OP_OR, 64, d, s[h]); break;
        default: r = x_bin(x, UAM_OP_XOR, 64, d, s[h]); break;
        }
        set_xmm(x, x->in.reg, h, r);
    }
    return LR_CONT;
}

/* MOVD/MOVQ between xmm and GPR/memory */
static lr movd_q(xl *x, unsigned p)
{
    unsigned op = x->in.op;
    unsigned w = x->in.rex_w ? 64 : 32;
    if (op == 0x6E && p == X86_PFX_66) {          /* xmm <- r/m32/64 */
        xop s = x_rm(x);
        set_xmm(x, x->in.reg, 0, x_zext(x, 64, w, x_rd(x, &s, w)));
        set_xmm(x, x->in.reg, 1, x_imm(x, 0));
        return LR_CONT;
    }
    if (op == 0x7E && p == X86_PFX_66) {          /* r/m32/64 <- xmm */
        xop d = x_rm(x);
        x_wr(x, &d, w, x_trunc(x, w, 64, xmm(x, x->in.reg, 0)));
        return LR_CONT;
    }
    if (op == 0x7E && p == X86_PFX_F3) {          /* MOVQ xmm, xmm/m64 */
        uam_ref v = (x->in.mod == 3) ? xmm(x, x->in.rm, 0) : x_ld(x, 64, x_rm(x).lin);
        set_xmm(x, x->in.reg, 0, v);
        set_xmm(x, x->in.reg, 1, x_imm(x, 0));
        return LR_CONT;
    }
    if (op == 0xD6 && p == X86_PFX_66) {          /* MOVQ xmm/m64, xmm */
        uam_ref v = xmm(x, x->in.reg, 0);
        if (x->in.mod != 3) {
            x_st(x, 64, x_rm(x).lin, v);
        } else {
            set_xmm(x, x->in.rm, 0, v);
            set_xmm(x, x->in.rm, 1, x_imm(x, 0));
        }
        return LR_CONT;
    }
    return sse_helper(x);
}

lr x_lift_sse(xl *x)
{
    unsigned p = pfx(x);
    if (x->in.map != X86_MAP_0F)
        return sse_helper(x);
    switch (x->in.op) {
    case 0x10: case 0x11:
        if (p == X86_PFX_F3) return movs_scalar(x, 32, x->in.op & 1);
        if (p == X86_PFX_F2) return movs_scalar(x, 64, x->in.op & 1);
        return (x->in.op & 1) ? mov128_out(x) : mov128_in(x);
    case 0x12: case 0x13: case 0x16: case 0x17:
        if (p == X86_PFX_NONE || p == X86_PFX_66) return mov_half(x);
        break;
    case 0x28: case 0x29:
        if (p == X86_PFX_NONE || p == X86_PFX_66) return (x->in.op & 1) ? mov128_out(x) : mov128_in(x);
        break;
    case 0x2B:
        if ((p == X86_PFX_NONE || p == X86_PFX_66) && x->in.mod != 3) return mov128_out(x);
        break;
    case 0x54: case 0x55: case 0x56: case 0x57:
        if (p == X86_PFX_NONE || p == X86_PFX_66) return logic128(x, x->in.op - 0x54);
        break;
    case 0x6F:
        if (p == X86_PFX_66 || p == X86_PFX_F3) return mov128_in(x);
        break;
    case 0x7F:
        if (p == X86_PFX_66 || p == X86_PFX_F3) return mov128_out(x);
        break;
    case 0xE7:
        if (p == X86_PFX_66 && x->in.mod != 3) return mov128_out(x);
        break;
    case 0x6E: case 0x7E: case 0xD6:
        return movd_q(x, p);
    case 0xDB: if (p == X86_PFX_66) return logic128(x, 0); break;
    case 0xDF: if (p == X86_PFX_66) return logic128(x, 1); break;
    case 0xEB: if (p == X86_PFX_66) return logic128(x, 2); break;
    case 0xEF: if (p == X86_PFX_66) return logic128(x, 3); break;
    }
    return sse_helper(x);
}

/* x87: FWAIT is a no-op; the rest is the helper's (FNINIT, FNSTSW, FNSTCW, FLDCW, ...). */
lr x_lift_x87(xl *x)
{
    if (x->in.op == 0x9B)
        return LR_CONT;
    bool mem = x->in.mod != 3;
    uam_ref lin = mem ? x_rm(x).lin : x_imm(x, 0);
    x_helper(x, X86_H_X87, x_imm(x, X86_X87_DESC(x->in.op, x->in.modrm, mem)), lin, NONE);
    return LR_CONT;
}
