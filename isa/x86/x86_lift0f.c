/*
 * x86_lift0f.c - x86 lifter: the two-byte (0F) opcode map, except SSE, which
 * x86_sse.c handles. System instructions go through SYSRD/SYSWR or helpers;
 * the ones that can change the mode, paging or the descriptor tables end the
 * block so the next one is looked up under the new cpu->priv.
 */
#include "x86_lift.h"

/* A register write under a guard, with the 32-bit zero-extension rule. */
static void setreg_if(xl *x, unsigned r, unsigned w, uam_ref v, uam_ref g)
{
    if (w == 8 && !x->in.rex && r >= 4 && r < 8)
        uam_put(x->b, 8, X86_OFF_GPR(r - 4) + 1, v, g);
    else if (w == 32)
        uam_put(x->b, 64, X86_OFF_GPR(r), x_zext(x, 64, 32, v), g);
    else
        uam_put(x->b, w, X86_OFF_GPR(r), v, g);
}

static lr gp0(xl *x) { return x_trap_err(x, X86_EXC_GP, 0); }

/* Privileged at CPL > 0: a constant at lift time (CPL is part of cpu->priv). */
#define NEED_CPL0(x) do { if ((x)->cpl > 0) return gp0(x); } while (0)

/* ---- control and debug registers (0F 20-23) ------------------------------ */

static lr mov_cr_dr(xl *x)
{
    NEED_CPL0(x);
    unsigned n = x->in.reg, op = x->in.op;
    unsigned w = (x->code == X86_CODE64) ? 64 : 32;
    uint32_t id;
    if (op & 1) {   /* debug registers */
        if (n > 7)
            return x_ud(x);
        id = X86_SR_DR0 + n;
    } else {
        switch (n) {
        case 0: id = X86_SR_CR0; break;
        case 2: id = X86_SR_CR2; break;
        case 3: id = X86_SR_CR3; break;
        case 4: id = X86_SR_CR4; break;
        case 8: id = X86_SR_CR8; break;
        default: return x_ud(x);
        }
    }
    if (op & 2) {   /* write */
        uam_syswr(x->b, 64, id, x_zext(x, 64, w, x_reg(x, x->in.rm, w)), NONE);
        if (!(op & 1) && n != 2)
            x->end = true;   /* CR0/CR3/CR4/CR8: mode, paging or interrupts */
    } else {
        x_setreg(x, x->in.rm, w, x_resize(x, w, 64, uam_sysrd(x->b, 64, id, NONE)));
    }
    return LR_CONT;
}

/* ---- group 6 (0F 00): SLDT STR LLDT LTR VERR VERW ------------------------ */

static lr grp6(xl *x)
{
    unsigned sub = x->in.reg & 7;
    if (x86_priv_mode(x->priv) == X86_MODE_REAL)
        return x_ud(x);
    xop o = x_rm(x);
    switch (sub) {
    case 0: case 1: {
        uam_ref v = uam_sysrd(x->b, 16, sub ? X86_SR_TR : X86_SR_LDTR, NONE);
        if (o.mem) x_st(x, 16, o.lin, v);
        else       x_setreg(x, o.reg, x->in.osz, x_zext(x, x->in.osz, 16, v));
        return LR_CONT;
    }
    case 2: case 3:
        NEED_CPL0(x);
        x_helper(x, sub == 2 ? X86_H_LLDT : X86_H_LTR, x_zext(x, 64, 16, x_rd(x, &o, 16)),
                 NONE, NONE);
        return LR_CONT;
    case 4: case 5:   /* VERR/VERW: report "not accessible" */
        (void)x_rd(x, &o, 16);
        x_setflag(x, X86_F_ZF, x_imm(x, 0));
        return LR_CONT;
    }
    return x_ud(x);
}

/* ---- group 7 (0F 01) ----------------------------------------------------- */

static lr grp7_reg(xl *x)
{
    switch (x->in.modrm) {
    case 0xC8: case 0xC9:   /* MONITOR / MWAIT: no-ops */
    case 0xCA: case 0xCB:   /* CLAC / STAC: AC is not modelled */
        return LR_CONT;
    case 0xD0: {            /* XGETBV (ECX = 0 only) */
        uam_ref v = uam_sysrd(x->b, 64, X86_SR_XCR0, NONE);
        x_setreg(x, X86_RAX, 32, x_trunc(x, 32, 64, v));
        x_setreg(x, X86_RDX, 32, x_trunc(x, 32, 64, x_bin(x, UAM_OP_LSHR, 64, v, x_imm(x, 32))));
        return LR_CONT;
    }
    case 0xD1: {            /* XSETBV */
        NEED_CPL0(x);
        uam_ref v = x_bin(x, UAM_OP_OR, 64,
                          x_bin(x, UAM_OP_SHL, 64, x_zext(x, 64, 32, x_reg(x, X86_RDX, 32)), x_imm(x, 32)),
                          x_zext(x, 64, 32, x_reg(x, X86_RAX, 32)));
        uam_syswr(x->b, 64, X86_SR_XCR0, v, NONE);
        return LR_CONT;
    }
    case 0xF8:              /* SWAPGS */
        if (x->code != X86_CODE64)
            return x_ud(x);
        NEED_CPL0(x);
        x_helper(x, X86_H_SWAPGS, NONE, NONE, NONE);
        return LR_CONT;
    case 0xF9: {            /* RDTSCP */
        uam_ref t = uam_sysrd(x->b, 64, X86_SR_TSC, NONE);
        uam_ref aux = uam_sysrd(x->b, 64, X86_SR_TSC_AUX, NONE);
        x_setreg(x, X86_RAX, 32, x_trunc(x, 32, 64, t));
        x_setreg(x, X86_RDX, 32, x_trunc(x, 32, 64, x_bin(x, UAM_OP_LSHR, 64, t, x_imm(x, 32))));
        x_setreg(x, X86_RCX, 32, x_trunc(x, 32, 64, aux));
        return LR_CONT;
    }
    }
    if (((x->in.modrm >> 3) & 7) == 4) {   /* SMSW r */
        uam_ref v = uam_sysrd(x->b, 64, X86_SR_MSW, NONE);
        x_setreg(x, x->in.rm, x->in.osz, x_resize(x, x->in.osz, 64, v));
        return LR_CONT;
    }
    if (((x->in.modrm >> 3) & 7) == 6) {   /* LMSW r */
        NEED_CPL0(x);
        uam_syswr(x->b, 64, X86_SR_MSW, x_zext(x, 64, 16, x_reg(x, x->in.rm, 16)), NONE);
        x->end = true;
        return LR_CONT;
    }
    return x_ud(x);
}

static lr grp7(xl *x)
{
    if (x->in.mod == 3)
        return grp7_reg(x);
    unsigned sub = x->in.reg & 7;
    unsigned bw = (x->code == X86_CODE64) ? 64 : 32;
    xop o = x_rm(x);
    uam_ref lin2 = x_bin(x, UAM_OP_ADD, 64, o.lin, x_imm(x, 2));
    switch (sub) {
    case 0: case 1: {   /* SGDT / SIDT */
        uam_ref lim = uam_sysrd(x->b, 16, sub ? X86_SR_IDTR_LIMIT : X86_SR_GDTR_LIMIT, NONE);
        uam_ref base = uam_sysrd(x->b, 64, sub ? X86_SR_IDTR_BASE : X86_SR_GDTR_BASE, NONE);
        x_st(x, 16, o.lin, lim);
        x_st(x, bw, lin2, x_resize(x, bw, 64, base));
        return LR_CONT;
    }
    case 2: case 3: {   /* LGDT / LIDT */
        NEED_CPL0(x);
        uam_ref lim = x_ld(x, 16, o.lin);
        uam_ref base = x_zext(x, 64, bw, x_ld(x, bw, lin2));
        if (x->in.osz == 16 && x->code != X86_CODE64)
            base = x_bin(x, UAM_OP_AND, 64, base, x_imm(x, 0xFFFFFF));
        uam_syswr(x->b, 64, sub == 3 ? X86_SR_IDTR_BASE : X86_SR_GDTR_BASE, base, NONE);
        uam_syswr(x->b, 64, sub == 3 ? X86_SR_IDTR_LIMIT : X86_SR_GDTR_LIMIT,
                  x_zext(x, 64, 16, lim), NONE);
        return LR_CONT;
    }
    case 4:             /* SMSW m16 */
        x_st(x, 16, o.lin, x_trunc(x, 16, 64, uam_sysrd(x->b, 64, X86_SR_MSW, NONE)));
        return LR_CONT;
    case 6:             /* LMSW m16 */
        NEED_CPL0(x);
        uam_syswr(x->b, 64, X86_SR_MSW, x_zext(x, 64, 16, x_ld(x, 16, o.lin)), NONE);
        x->end = true;
        return LR_CONT;
    case 7:             /* INVLPG */
        NEED_CPL0(x);
        uam_syswr(x->b, 64, X86_SR_INVLPG, o.lin, NONE);
        x->end = true;
        return LR_CONT;
    }
    return x_ud(x);
}

/* ---- integer instructions ------------------------------------------------ */

static lr cmov(xl *x)
{
    unsigned w = x->in.osz;
    xop s = x_rm(x);
    uam_ref v = x_rd(x, &s, w);   /* a memory source is read (and may fault) either way */
    uam_ref old = x_reg(x, x->in.reg, w);
    x_setreg(x, x->in.reg, w, uam_select(x->b, w, x_cond(x, x->in.op & 0xF), v, old));
    return LR_CONT;
}

static lr setcc(xl *x)
{
    xop d = x_rm(x);
    x_wr(x, &d, 8, x_zext(x, 8, 1, x_cond(x, x->in.op & 0xF)));
    return LR_CONT;
}

/* BT/BTS/BTR/BTC: kind 0 test, 1 set, 2 reset, 3 complement. */
static void bt_op(xl *x, unsigned kind, uam_ref off, bool imm_form)
{
    unsigned w = x->in.osz;
    unsigned lg = (w == 16) ? 4 : (w == 32) ? 5 : 6;
    xop o = x_rm(x);
    uam_ref bitn = x_bin(x, UAM_OP_AND, w, off, x_imm(x, w - 1));
    if (o.mem && !imm_form) {   /* a register bit offset reaches beyond the operand */
        uam_ref words = x_bin(x, UAM_OP_ASHR, w, off, x_imm(x, lg));
        uam_ref disp = x_bin(x, UAM_OP_SHL, 64, x_sext(x, 64, w, words), x_imm(x, w / 8 == 2 ? 1 : w / 8 == 4 ? 2 : 3));
        o.lin = x_bin(x, UAM_OP_ADD, 64, o.lin, disp);
        if (x->code != X86_CODE64)
            o.lin = x_zext(x, 64, 32, x_trunc(x, 32, 64, o.lin));
    }
    uam_ref v = x_rd(x, &o, w);
    uam_ref mask = x_bin(x, UAM_OP_SHL, w, x_imm(x, 1), bitn);
    uam_ref cf = x_bin(x, UAM_OP_NE, w, x_bin(x, UAM_OP_AND, w, v, mask), x_imm(x, 0));
    if (kind) {
        uam_ref nv = (kind == 1) ? x_bin(x, UAM_OP_OR, w, v, mask)
                   : (kind == 2) ? x_bin(x, UAM_OP_AND, w, v, x_bin(x, UAM_OP_XOR, w, mask, x_imm(x, xmask(w))))
                   : x_bin(x, UAM_OP_XOR, w, v, mask);
        x_wr(x, &o, w, nv);
    }
    x_setflag(x, X86_F_CF, cf);
}

/* SHLD/SHRD Ev, Gv, count */
static lr shxd(xl *x, bool right, uam_ref cnt)
{
    unsigned w = x->in.osz;
    xop o = x_rm(x);
    uam_ref a = x_rd(x, &o, w);
    uam_ref s = x_reg(x, x->in.reg, w);
    uam_ref n = x_bin(x, UAM_OP_AND, 8, x_resize(x, 8, 8, cnt), x_imm(x, w == 64 ? 63 : 31));
    uam_ref g = x_bin(x, UAM_OP_NE, 8, n, x_imm(x, 0));
    uam_ref nw = x_zext(x, w, 8, n);
    uam_ref inv = x_bin(x, UAM_OP_SUB, w, x_imm(x, w), nw);
    uam_ref one = x_imm(x, 1);
    uam_ref res, cfv;
    if (right) {
        res = x_bin(x, UAM_OP_OR, w, x_bin(x, UAM_OP_LSHR, w, a, nw), x_bin(x, UAM_OP_SHL, w, s, inv));
        cfv = x_bin(x, UAM_OP_LSHR, w, a, x_bin(x, UAM_OP_SUB, w, nw, one));
    } else {
        res = x_bin(x, UAM_OP_OR, w, x_bin(x, UAM_OP_SHL, w, a, nw), x_bin(x, UAM_OP_LSHR, w, s, inv));
        cfv = x_bin(x, UAM_OP_LSHR, w, a, inv);
    }
    res = uam_select(x->b, w, g, res, a);
    x_wr(x, &o, w, res);
    uam_ref zero = x_imm(x, 0);
    uam_ref cf = x_bin(x, UAM_OP_NE, w, x_bin(x, UAM_OP_AND, w, cfv, one), zero);
    uam_ref of = x_bin(x, UAM_OP_XOR, 1, x_bin(x, UAM_OP_SLT, w, res, zero), x_bin(x, UAM_OP_SLT, w, a, zero));
    uam_put(x->b, 8, X86_OFF_FLAG(X86_F_CF), x_zext(x, 8, 1, cf), g);
    uam_put(x->b, 8, X86_OFF_FLAG(X86_F_OF), x_zext(x, 8, 1, of), g);
    uam_put(x->b, 8, X86_OFF_FLAG(X86_F_ZF), x_zext(x, 8, 1, x_bin(x, UAM_OP_EQ, w, res, zero)), g);
    uam_put(x->b, 8, X86_OFF_FLAG(X86_F_SF), x_zext(x, 8, 1, x_bin(x, UAM_OP_SLT, w, res, zero)), g);
    uam_put(x->b, 8, X86_OFF_FLAG(X86_F_PF), x_trunc(x, 8, w, res), g);
    return LR_CONT;
}

static lr cmpxchg(xl *x)
{
    unsigned w = (x->in.op & 1) ? x->in.osz : 8;
    xop o = x_rm(x);
    uam_ref d = x_rd(x, &o, w);
    uam_ref acc = x_reg(x, X86_RAX, w);
    uam_ref eq = x_bin(x, UAM_OP_EQ, w, acc, d);
    uam_ref ne = x_bin(x, UAM_OP_XOR, 1, eq, x_imm(x, 1));
    uam_ref src = x_reg(x, x->in.reg, w);
    if (o.mem)   /* the destination is always written back */
        x_st(x, w, o.lin, uam_select(x->b, w, eq, src, d));
    else
        setreg_if(x, o.reg, w, src, eq);
    setreg_if(x, X86_RAX, w, d, ne);
    x_flags_sub(x, w, acc, d, x_bin(x, UAM_OP_SUB, w, acc, d), NONE);
    return LR_CONT;
}

/* CMPXCHG8B / CMPXCHG16B (0F C7 /1) */
static lr cmpxchg_wide(xl *x)
{
    if (x->in.mod == 3)
        return x_ud(x);
    unsigned h = x->in.rex_w ? 64 : 32;
    xop o = x_rm(x);
    uam_ref hi_lin = x_bin(x, UAM_OP_ADD, 64, o.lin, x_imm(x, h / 8));
    uam_ref lo = x_ld(x, h, o.lin), hi = x_ld(x, h, hi_lin);
    uam_ref a = x_reg(x, X86_RAX, h), d = x_reg(x, X86_RDX, h);
    uam_ref eq = x_bin(x, UAM_OP_AND, 1, x_bin(x, UAM_OP_EQ, h, lo, a), x_bin(x, UAM_OP_EQ, h, hi, d));
    uam_ref ne = x_bin(x, UAM_OP_XOR, 1, eq, x_imm(x, 1));
    x_st(x, h, o.lin, uam_select(x->b, h, eq, x_reg(x, X86_RBX, h), lo));
    x_st(x, h, hi_lin, uam_select(x->b, h, eq, x_reg(x, X86_RCX, h), hi));
    setreg_if(x, X86_RAX, h, lo, ne);
    setreg_if(x, X86_RDX, h, hi, ne);
    x_setflag(x, X86_F_ZF, eq);
    return LR_CONT;
}

static lr xadd(xl *x)
{
    unsigned w = (x->in.op & 1) ? x->in.osz : 8;
    xop o = x_rm(x);
    uam_ref d = x_rd(x, &o, w);
    uam_ref s = x_reg(x, x->in.reg, w);
    uam_ref sum = x_bin(x, UAM_OP_ADD, w, d, s);
    if (o.mem) {
        x_st(x, w, o.lin, sum);
        x_setreg(x, x->in.reg, w, d);
    } else {
        x_setreg(x, x->in.reg, w, d);
        x_setreg(x, o.reg, w, sum);
    }
    x_flags_add(x, w, d, s, sum, NONE);
    return LR_CONT;
}

static lr movx(xl *x, bool sgn)
{
    unsigned sw = (x->in.op & 1) ? 16 : 8, w = x->in.osz;
    xop s = x_rm(x);
    uam_ref v = x_rd(x, &s, sw);
    x_setreg(x, x->in.reg, w, sgn ? x_sext(x, w, sw, v) : x_zext(x, w, sw, v));
    return LR_CONT;
}

/* BSF/BSR, and with F3 TZCNT/LZCNT; F3 0F B8 POPCNT */
static lr bitscan(xl *x)
{
    unsigned w = x->in.osz, op = x->in.op;
    xop s = x_rm(x);
    uam_ref v = x_rd(x, &s, w);
    uam_ref zero = x_imm(x, 0);
    uam_ref isz = x_bin(x, UAM_OP_EQ, w, v, zero);
    if (op == 0xB8) {
        uam_ref c = x_un(x, UAM_OP_POPCNT, w, v);
        x_setreg(x, x->in.reg, w, c);
        x_flags_logic(x, w, c);
        x_setflag(x, X86_F_ZF, isz);
        return LR_CONT;
    }
    bool bsr = op == 0xBD;
    if (x->in.rep) {   /* TZCNT / LZCNT */
        uam_ref c = x_un(x, bsr ? UAM_OP_CLZ : UAM_OP_CTZ, w, v);
        x_setreg(x, x->in.reg, w, c);
        x_setflag(x, X86_F_CF, isz);
        x_setflag(x, X86_F_ZF, x_bin(x, UAM_OP_EQ, w, c, zero));
        return LR_CONT;
    }
    uam_ref c = bsr ? x_bin(x, UAM_OP_SUB, w, x_imm(x, w - 1), x_un(x, UAM_OP_CLZ, w, v))
                    : x_un(x, UAM_OP_CTZ, w, v);
    setreg_if(x, x->in.reg, w, c, x_bin(x, UAM_OP_XOR, 1, isz, x_imm(x, 1)));
    x_setflag(x, X86_F_ZF, isz);
    return LR_CONT;
}

static lr bswap(xl *x)
{
    unsigned r = (x->in.op & 7) | x->in.rex_b;
    unsigned w = x->in.osz == 64 ? 64 : 32;
    x_setreg(x, r, w, x_un(x, UAM_OP_BSWAP, w, x_reg(x, r, w)));
    return LR_CONT;
}

/* 0F C7: CMPXCHG8B/16B (/1), RDRAND (/6), RDSEED (/7) */
static lr grp9(xl *x)
{
    unsigned sub = x->in.reg & 7;
    if (sub == 1)
        return cmpxchg_wide(x);
    if ((sub == 6 || sub == 7) && x->in.mod == 3) {
        unsigned w = x->in.osz;
        uam_ref v = uam_sysrd(x->b, 64, X86_SR_RDRAND, NONE);
        x_setreg(x, x->in.rm, w, x_resize(x, w, 64, v));
        x_flags_logic(x, w, x_imm(x, 1));
        x_setflag(x, X86_F_CF, x_imm(x, 1));
        x_setflag(x, X86_F_ZF, x_imm(x, 0));
        return LR_CONT;
    }
    return x_ud(x);
}

/* ---- group 15 (0F AE): FXSAVE FXRSTOR LDMXCSR STMXCSR fences CLFLUSH ----- */

static lr grp15(xl *x)
{
    unsigned sub = x->in.reg & 7;
    if (x->in.mod == 3)
        return (sub >= 5 && !x->in.rep) ? LR_CONT : x_ud(x);   /* LFENCE MFENCE SFENCE */
    if (x->in.rep || x->in.p66)
        return x_ud(x);
    xop o = x_rm(x);
    switch (sub) {
    case 0: x_helper(x, X86_H_FXSAVE, o.lin, NONE, NONE); return LR_CONT;
    case 1: x_helper(x, X86_H_FXRSTOR, o.lin, NONE, NONE); return LR_CONT;
    case 2: uam_put(x->b, 32, X86_OFF_MXCSR, x_ld(x, 32, o.lin), NONE); return LR_CONT;
    case 3: x_st(x, 32, o.lin, uam_get(x->b, 32, X86_OFF_MXCSR)); return LR_CONT;
    case 7: return LR_CONT;   /* CLFLUSH */
    }
    return x_ud(x);
}

/* ---- MSRs, TSC, CPUID ---------------------------------------------------- */

static lr rdtsc(xl *x)
{
    uam_ref t = uam_sysrd(x->b, 64, X86_SR_TSC, NONE);
    x_setreg(x, X86_RAX, 32, x_trunc(x, 32, 64, t));
    x_setreg(x, X86_RDX, 32, x_trunc(x, 32, 64, x_bin(x, UAM_OP_LSHR, 64, t, x_imm(x, 32))));
    return LR_CONT;
}

/* LAR / LSL: the helper sets ZF and returns the value to load when ZF = 1. */
static lr lar_lsl(xl *x)
{
    if (x86_priv_mode(x->priv) == X86_MODE_REAL)
        return x_ud(x);
    unsigned w = x->in.osz;
    xop s = x_rm(x);
    uam_ref sel = x_zext(x, 64, 16, x_rd(x, &s, 16));
    uam_ref v = x_helper(x, X86_H_LAR, sel, x_imm(x, x->in.op == 0x03), NONE);
    setreg_if(x, x->in.reg, w, x_resize(x, w, 64, v), x_flag(x, X86_F_ZF));
    return LR_CONT;
}

static lr system_0f(xl *x)
{
    switch (x->in.op) {
    case 0x00: return grp6(x);
    case 0x01: return grp7(x);
    case 0x02: case 0x03: return lar_lsl(x);
    case 0x05: {
        if (x->code != X86_CODE64)
            return x_ud(x);
        x_jump(x, x_helper(x, X86_H_SYSCALL, x_imm(x, x->next), NONE, NONE), UAM_EXIT_CALL);
        return LR_END;
    }
    case 0x06: NEED_CPL0(x); uam_syswr(x->b, 64, X86_SR_CLTS, x_imm(x, 0), NONE); return LR_CONT;
    case 0x07: {
        NEED_CPL0(x);
        if (x->code != X86_CODE64)
            return x_ud(x);
        x_jump(x, x_helper(x, X86_H_SYSRET, x_imm(x, x->in.osz), NONE, NONE), UAM_EXIT_RET);
        return LR_END;
    }
    case 0x08: case 0x09: NEED_CPL0(x); return LR_CONT;   /* INVD / WBINVD */
    case 0x20: case 0x21: case 0x22: case 0x23: return mov_cr_dr(x);
    case 0x30:
        NEED_CPL0(x);
        x_helper(x, X86_H_WRMSR, NONE, NONE, NONE);
        x->end = true;   /* EFER, FS/GS base, APIC base, ... */
        return LR_CONT;
    case 0x31: return rdtsc(x);
    case 0x32: NEED_CPL0(x); x_helper(x, X86_H_RDMSR, NONE, NONE, NONE); return LR_CONT;
    case 0xA2: x_helper(x, X86_H_CPUID, NONE, NONE, NONE); return LR_CONT;
    case 0xAA: {   /* RSM: only in SMM */
        if (!x86_priv_smm(x->priv))
            return x_ud(x);
        x_jump(x, x_helper(x, X86_H_RSM, NONE, NONE, NONE), UAM_EXIT_EXC_RETURN);
        return LR_END;
    }
    }
    return x_ud(x);
}

/* ---- dispatch ------------------------------------------------------------ */

static bool is_sse(unsigned op)
{
    return (op >= 0x10 && op <= 0x17) || (op >= 0x28 && op <= 0x2F) ||
           (op >= 0x50 && op <= 0x7F) || (op >= 0xC2 && op <= 0xC6) || op >= 0xD0;
}

lr x_lift_0f(xl *x)
{
    unsigned op = x->in.op;
    if (is_sse(op) || (op == 0xB8 && !x->in.rep))
        return op == 0xB8 ? x_ud(x) : x_lift_sse(x);
    if (op >= 0x80 && op <= 0x8F) {
        unsigned w = x->in.osz;
        return x_jcc(x, op & 0xF, (int64_t)x_sx(x->in.imm, w == 16 ? 16 : 32));
    }
    if (op >= 0x40 && op <= 0x4F) return cmov(x);
    if (op >= 0x90 && op <= 0x9F) return setcc(x);
    if (op >= 0xC8 && op <= 0xCF) return bswap(x);
    if (op == 0x0D || (op >= 0x18 && op <= 0x1F))
        return LR_CONT;   /* prefetches, hint NOPs, ENDBR */
    switch (op) {
    case 0x0B: return x_ud(x);   /* UD2 */
    case 0xA0: return x_push_seg(x, X86_SEG_FS);
    case 0xA1: return x_pop_seg(x, X86_SEG_FS);
    case 0xA8: return x_push_seg(x, X86_SEG_GS);
    case 0xA9: return x_pop_seg(x, X86_SEG_GS);
    case 0xA3: bt_op(x, 0, x_reg(x, x->in.reg, x->in.osz), false); return LR_CONT;
    case 0xAB: bt_op(x, 1, x_reg(x, x->in.reg, x->in.osz), false); return LR_CONT;
    case 0xB3: bt_op(x, 2, x_reg(x, x->in.reg, x->in.osz), false); return LR_CONT;
    case 0xBB: bt_op(x, 3, x_reg(x, x->in.reg, x->in.osz), false); return LR_CONT;
    case 0xBA: {
        unsigned sub = x->in.reg & 7;
        if (sub < 4)
            return x_ud(x);
        bt_op(x, sub - 4, x_imm(x, x->in.imm & 0xFF), true);
        return LR_CONT;
    }
    case 0xA4: return shxd(x, false, x_imm(x, x->in.imm & 0xFF));
    case 0xA5: return shxd(x, false, uam_get(x->b, 8, X86_OFF_GPR(X86_RCX)));
    case 0xAC: return shxd(x, true, x_imm(x, x->in.imm & 0xFF));
    case 0xAD: return shxd(x, true, uam_get(x->b, 8, X86_OFF_GPR(X86_RCX)));
    case 0xAE: return grp15(x);
    case 0xAF: {
        xop s = x_rm(x);
        x_imul2(x, x->in.osz, x_reg(x, x->in.reg, x->in.osz), x_rd(x, &s, x->in.osz));
        return LR_CONT;
    }
    case 0xB0: case 0xB1: return cmpxchg(x);
    case 0xB2: return x_load_far(x, X86_SEG_SS);
    case 0xB4: return x_load_far(x, X86_SEG_FS);
    case 0xB5: return x_load_far(x, X86_SEG_GS);
    case 0xB6: case 0xB7: return movx(x, false);
    case 0xBE: case 0xBF: return movx(x, true);
    case 0xB8: case 0xBC: case 0xBD: return bitscan(x);
    case 0xC0: case 0xC1: return xadd(x);
    case 0xC7: return grp9(x);
    }
    return system_0f(x);
}
