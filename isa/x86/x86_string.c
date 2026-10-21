/*
 * x86_string.c - x86 lifter: string instructions (MOVS CMPS STOS LODS SCAS
 * INS OUTS) with REP/REPE/REPNE.
 *
 * A REP instruction runs one iteration per execution: if rCX is zero it
 * falls through, otherwise it does one element, decrements rCX and, when the
 * repeat condition still holds, exits back to its own pc. Interrupts are
 * therefore taken between iterations, as on hardware, and a fault leaves
 * rSI/rDI/rCX describing the element that faulted.
 */
#include "x86_lift.h"

static uam_ref idx_lin(xl *x, unsigned seg, unsigned r)
{
    return x_lin(x, seg, x_reg(x, r, x->in.asz), x->in.asz);
}

/* rSI/rDI += step, where step = DF ? -size : size, at the address size. */
static void advance(xl *x, unsigned r, unsigned size)
{
    unsigned aw = x->in.asz;
    uam_ref step = uam_select(x->b, aw, x_flag(x, X86_F_DF), x_imm(x, (0 - (uint64_t)size) & xmask(aw)),
                              x_imm(x, size));
    uam_ref v = x_bin(x, UAM_OP_ADD, aw, x_reg(x, r, aw), step);
    if (aw == 16) uam_put(x->b, 16, X86_OFF_GPR(r), v, NONE);
    else          uam_put(x->b, 64, X86_OFF_GPR(r), x_zext(x, 64, aw, v), NONE);
}

lr x_lift_string(xl *x)
{
    unsigned op = x->in.op;
    unsigned w = (op & 1) ? (x->in.osz) : 8;
    unsigned aw = x->in.asz;
    bool rep = x->in.rep || x->in.repne;
    bool cmp = (op == 0xA6 || op == 0xA7 || op == 0xAE || op == 0xAF);
    unsigned src = x_seg(x, X86_SEG_DS);
    uam_ref zero = x_imm(x, 0);
    bool io = op >= 0x6C && op <= 0x6F;
    if (io && w == 64)
        w = 32;
    uam_ref cnt = NONE;

    if (io)
        x_iopl_check(x);
    if (rep) {
        cnt = x_reg(x, X86_RCX, aw);
        uam_exit(x->b, x_bin(x, UAM_OP_EQ, aw, cnt, zero), x_imm(x, x->next), UAM_EXIT_JUMP);
    }

    bool adv_si = false, adv_di = false;
    switch (op) {
    case 0x6C: case 0x6D: {   /* INS */
        uam_ref a = x_io_addr(x, x_reg(x, X86_RDX, 16));
        uam_ref v = uam_ld(x->b, w, UAM_ACC_LE | UAM_ACC_PHYS, a, NONE);
        x_st(x, w, idx_lin(x, X86_SEG_ES, X86_RDI), v);
        adv_di = true;
        break;
    }
    case 0x6E: case 0x6F: {   /* OUTS */
        uam_ref v = x_ld(x, w, idx_lin(x, src, X86_RSI));
        uam_st(x->b, w, UAM_ACC_LE | UAM_ACC_PHYS, x_io_addr(x, x_reg(x, X86_RDX, 16)), v, NONE);
        adv_si = true;
        break;
    }
    case 0xA4: case 0xA5:     /* MOVS */
        x_st(x, w, idx_lin(x, X86_SEG_ES, X86_RDI), x_ld(x, w, idx_lin(x, src, X86_RSI)));
        adv_si = adv_di = true;
        break;
    case 0xA6: case 0xA7: {   /* CMPS */
        uam_ref a = x_ld(x, w, idx_lin(x, src, X86_RSI));
        uam_ref b = x_ld(x, w, idx_lin(x, X86_SEG_ES, X86_RDI));
        x_flags_sub(x, w, a, b, x_bin(x, UAM_OP_SUB, w, a, b), NONE);
        adv_si = adv_di = true;
        break;
    }
    case 0xAA: case 0xAB:     /* STOS */
        x_st(x, w, idx_lin(x, X86_SEG_ES, X86_RDI), x_reg(x, X86_RAX, w));
        adv_di = true;
        break;
    case 0xAC: case 0xAD:     /* LODS */
        x_setreg(x, X86_RAX, w, x_ld(x, w, idx_lin(x, src, X86_RSI)));
        adv_si = true;
        break;
    default: {                /* SCAS */
        uam_ref a = x_reg(x, X86_RAX, w);
        uam_ref b = x_ld(x, w, idx_lin(x, X86_SEG_ES, X86_RDI));
        x_flags_sub(x, w, a, b, x_bin(x, UAM_OP_SUB, w, a, b), NONE);
        adv_di = true;
        break;
    }
    }
    if (adv_si) advance(x, X86_RSI, w / 8);
    if (adv_di) advance(x, X86_RDI, w / 8);

    if (rep) {
        uam_ref c = x_bin(x, UAM_OP_SUB, aw, cnt, x_imm(x, 1));
        if (aw == 16) uam_put(x->b, 16, X86_OFF_GPR(X86_RCX), c, NONE);
        else          uam_put(x->b, 64, X86_OFF_GPR(X86_RCX), x_zext(x, 64, aw, c), NONE);
        uam_ref more = x_bin(x, UAM_OP_NE, aw, c, zero);
        if (cmp) {
            uam_ref zf = x_flag(x, X86_F_ZF);
            if (x->in.repne)
                zf = x_bin(x, UAM_OP_XOR, 1, zf, x_imm(x, 1));
            more = x_bin(x, UAM_OP_AND, 1, more, zf);
        }
        uam_exit(x->b, more, x_imm(x, x->pc), UAM_EXIT_JUMP);
    }
    return LR_CONT;
}
