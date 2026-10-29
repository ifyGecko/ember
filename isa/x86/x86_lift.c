/*
 * x86_lift.c - x86 lifter: block driver and the one-byte opcode map.
 *
 * Each instruction is decoded completely first (x86_decode), then emitted.
 * The 0F map is in x86_lift0f.c, string instructions in x86_string.c, and
 * SSE/x87 in x86_sse.c. Helpers named here are in x86_helpers.c.
 */
#include "x86_lift.h"

/* A flag write under a guard (shifts by a zero count leave the flags alone). */
static void gflag(xl *x, unsigned f, uam_ref b1, uam_ref g)
{
    uam_ref v = x_zext(x, 8, 1, b1);
    uam_put(x->b, 8, X86_OFF_FLAG(f), v, g);
}

static uam_ref msb(xl *x, unsigned w, uam_ref v) { return x_bin(x, UAM_OP_SLT, w, v, x_imm(x, 0)); }

static uam_ref bit0(xl *x, unsigned w, uam_ref v)
{
    return x_bin(x, UAM_OP_NE, w, x_bin(x, UAM_OP_AND, w, v, x_imm(x, 1)), x_imm(x, 0));
}

/* ---- ALU group (00-3F, 80-83) -------------------------------------------- */

enum { ALU_ADD = 0, ALU_OR, ALU_ADC, ALU_SBB, ALU_AND, ALU_SUB, ALU_XOR, ALU_CMP };

/* dst op= src. Store/register write first, flags after (commit rule). */
static void alu(xl *x, unsigned op, unsigned w, xop *dst, uam_ref a, uam_ref b)
{
    uam_ref res, c = NONE;
    switch (op) {
    case ALU_ADD: res = x_bin(x, UAM_OP_ADD, w, a, b); break;
    case ALU_OR:  res = x_bin(x, UAM_OP_OR, w, a, b); break;
    case ALU_AND: res = x_bin(x, UAM_OP_AND, w, a, b); break;
    case ALU_XOR: res = x_bin(x, UAM_OP_XOR, w, a, b); break;
    case ALU_SUB: case ALU_CMP: res = x_bin(x, UAM_OP_SUB, w, a, b); break;
    case ALU_ADC:
        c = x_flag(x, X86_F_CF);
        res = x_bin(x, UAM_OP_ADD, w, x_bin(x, UAM_OP_ADD, w, a, b), x_zext(x, w, 1, c));
        break;
    default: /* SBB */
        c = x_flag(x, X86_F_CF);
        res = x_bin(x, UAM_OP_SUB, w, x_bin(x, UAM_OP_SUB, w, a, b), x_zext(x, w, 1, c));
        break;
    }
    if (op != ALU_CMP)
        x_wr(x, dst, w, res);
    switch (op) {
    case ALU_ADD: case ALU_ADC: x_flags_add(x, w, a, b, res, c); break;
    case ALU_SUB: case ALU_CMP: case ALU_SBB: x_flags_sub(x, w, a, b, res, c); break;
    default: x_flags_logic(x, w, res); break;
    }
}

/* ---- shifts and rotates (C0 C1 D0-D3) ------------------------------------ */

/*
 * IR shifts saturate (an amount >= W gives 0 or the sign), so the x86 count
 * is masked explicitly and the out-of-range cases fall out naturally. A
 * masked count of zero leaves the flags unchanged: every flag write is
 * guarded by "count != 0".
 */
static void shift(xl *x, unsigned sub, unsigned w, xop *o, uam_ref cnt_in)
{
    uam_ref a = x_rd(x, o, w);
    uam_ref n = x_bin(x, UAM_OP_AND, 8, x_resize(x, 8, 8, cnt_in), x_imm(x, w == 64 ? 63 : 31));
    if ((sub == 2 || sub == 3) && w < 32)   /* RCL/RCR rotate through w+1 bits */
        n = x_bin(x, UAM_OP_UREM, 8, n, x_imm(x, w + 1));
    uam_ref g = x_bin(x, UAM_OP_NE, 8, n, x_imm(x, 0));
    uam_ref nw = x_zext(x, w, 8, n);
    uam_ref wi = x_imm(x, w);
    uam_ref one = x_imm(x, 1);
    uam_ref res, cf, of = NONE;

    switch (sub) {
    case 0: /* ROL */
        res = x_bin(x, UAM_OP_ROTR, w, a, x_bin(x, UAM_OP_SUB, w, wi, nw));
        cf = bit0(x, w, res);
        of = x_bin(x, UAM_OP_XOR, 1, msb(x, w, res), cf);
        break;
    case 1: /* ROR */
        res = x_bin(x, UAM_OP_ROTR, w, a, nw);
        cf = msb(x, w, res);
        of = x_bin(x, UAM_OP_XOR, 1, cf, msb(x, w, x_bin(x, UAM_OP_SHL, w, res, one)));
        break;
    case 2: { /* RCL */
        uam_ref cin = x_zext(x, w, 1, x_flag(x, X86_F_CF));
        res = x_bin(x, UAM_OP_OR, w, x_bin(x, UAM_OP_SHL, w, a, nw),
                    x_bin(x, UAM_OP_SHL, w, cin, x_bin(x, UAM_OP_SUB, w, nw, one)));
        res = x_bin(x, UAM_OP_OR, w, res,
                    x_bin(x, UAM_OP_LSHR, w, a, x_bin(x, UAM_OP_SUB, w, x_imm(x, w + 1), nw)));
        cf = bit0(x, w, x_bin(x, UAM_OP_LSHR, w, a, x_bin(x, UAM_OP_SUB, w, wi, nw)));
        of = x_bin(x, UAM_OP_XOR, 1, msb(x, w, res), cf);
        break;
    }
    case 3: { /* RCR */
        uam_ref cin = x_zext(x, w, 1, x_flag(x, X86_F_CF));
        res = x_bin(x, UAM_OP_OR, w, x_bin(x, UAM_OP_LSHR, w, a, nw),
                    x_bin(x, UAM_OP_SHL, w, cin, x_bin(x, UAM_OP_SUB, w, wi, nw)));
        res = x_bin(x, UAM_OP_OR, w, res,
                    x_bin(x, UAM_OP_SHL, w, a, x_bin(x, UAM_OP_SUB, w, x_imm(x, w + 1), nw)));
        cf = bit0(x, w, x_bin(x, UAM_OP_LSHR, w, a, x_bin(x, UAM_OP_SUB, w, nw, one)));
        of = x_bin(x, UAM_OP_XOR, 1, msb(x, w, res), msb(x, w, x_bin(x, UAM_OP_SHL, w, res, one)));
        break;
    }
    case 4: case 6: /* SHL/SAL */
        res = x_bin(x, UAM_OP_SHL, w, a, nw);
        cf = bit0(x, w, x_bin(x, UAM_OP_LSHR, w, a, x_bin(x, UAM_OP_SUB, w, wi, nw)));
        of = x_bin(x, UAM_OP_XOR, 1, msb(x, w, res), cf);
        break;
    case 5: /* SHR */
        res = x_bin(x, UAM_OP_LSHR, w, a, nw);
        cf = bit0(x, w, x_bin(x, UAM_OP_LSHR, w, a, x_bin(x, UAM_OP_SUB, w, nw, one)));
        of = msb(x, w, a);
        break;
    default: /* SAR */
        res = x_bin(x, UAM_OP_ASHR, w, a, nw);
        cf = bit0(x, w, x_bin(x, UAM_OP_ASHR, w, a, x_bin(x, UAM_OP_SUB, w, nw, one)));
        of = x_imm(x, 0);
        break;
    }
    x_wr(x, o, w, res);
    gflag(x, X86_F_CF, cf, g);
    gflag(x, X86_F_OF, of, g);
    if (sub >= 4) {
        gflag(x, X86_F_ZF, x_bin(x, UAM_OP_EQ, w, res, x_imm(x, 0)), g);
        gflag(x, X86_F_SF, msb(x, w, res), g);
        gflag(x, X86_F_AF, x_imm(x, 0), g);
        uam_put(x->b, 8, X86_OFF_FLAG(X86_F_PF), x_trunc(x, 8, w, res), g);
    }
}

/* ---- INC/DEC (CF is preserved) ------------------------------------------- */

static void incdec(xl *x, bool dec, unsigned w, xop *o)
{
    uam_ref a = x_rd(x, o, w);
    uam_ref one = x_imm(x, 1);
    uam_ref res = x_bin(x, dec ? UAM_OP_SUB : UAM_OP_ADD, w, a, one);
    uam_ref cf = x_flag(x, X86_F_CF);
    x_wr(x, o, w, res);
    if (dec) x_flags_sub(x, w, a, one, res, NONE);
    else     x_flags_add(x, w, a, one, res, NONE);
    x_setflag(x, X86_F_CF, cf);
}

/* ---- multiply and divide (F6/F7 /4-/7) ----------------------------------- */

static void de_if(xl *x, uam_ref cond)
{
    uam_trap(x->b, cond, UAM_TRAP_EXCEPTION, x_imm(x, X86_EXC_DE), x_imm(x, 0));
}

static void mul1(xl *x, bool sgn, unsigned w, uam_ref src)
{
    if (w == 8) {
        unsigned ext = sgn ? 1 : 0;
        uam_ref al = x_reg(x, X86_RAX, 8);
        uam_ref a16 = ext ? x_sext(x, 16, 8, al) : x_zext(x, 16, 8, al);
        uam_ref s16 = ext ? x_sext(x, 16, 8, src) : x_zext(x, 16, 8, src);
        uam_ref p = x_bin(x, UAM_OP_MUL, 16, a16, s16);
        uam_ref lo = x_trunc(x, 8, 16, p);
        uam_ref ovf = ext ? x_bin(x, UAM_OP_NE, 16, p, x_sext(x, 16, 8, lo))
                          : x_bin(x, UAM_OP_NE, 16, x_bin(x, UAM_OP_LSHR, 16, p, x_imm(x, 8)),
                                  x_imm(x, 0));
        x_setreg(x, X86_RAX, 16, p);
        x_setflag(x, X86_F_CF, ovf);
        x_setflag(x, X86_F_OF, ovf);
        x_set_szp(x, 8, lo);
        return;
    }
    uam_ref a = x_reg(x, X86_RAX, w);
    uam_ref lo = x_bin(x, UAM_OP_MUL, w, a, src);
    uam_ref hi = x_bin(x, sgn ? UAM_OP_MULHS : UAM_OP_MULHU, w, a, src);
    uam_ref ovf = sgn ? x_bin(x, UAM_OP_NE, w, hi, x_bin(x, UAM_OP_ASHR, w, lo, x_imm(x, w - 1)))
                      : x_bin(x, UAM_OP_NE, w, hi, x_imm(x, 0));
    x_setreg(x, X86_RAX, w, lo);
    x_setreg(x, X86_RDX, w, hi);
    x_setflag(x, X86_F_CF, ovf);
    x_setflag(x, X86_F_OF, ovf);
    x_set_szp(x, w, lo);
}

static void div1(xl *x, bool sgn, unsigned w, uam_ref src)
{
    if (w == 64) {   /* a 128-bit dividend: the helper writes RAX/RDX or raises #DE */
        x_helper(x, sgn ? X86_H_IDIVQ : X86_H_DIVQ, src, NONE, NONE);
        return;
    }
    unsigned w2 = w * 2;
    uam_ref n;
    if (w == 8) {
        n = x_reg(x, X86_RAX, 16);
    } else {
        uam_ref hi = x_zext(x, w2, w, x_reg(x, X86_RDX, w));
        uam_ref lo = x_zext(x, w2, w, x_reg(x, X86_RAX, w));
        n = x_bin(x, UAM_OP_OR, w2, x_bin(x, UAM_OP_SHL, w2, hi, x_imm(x, w)), lo);
    }
    uam_ref d = sgn ? x_sext(x, w2, w, src) : x_zext(x, w2, w, src);
    uam_ref q = x_bin(x, sgn ? UAM_OP_SDIV : UAM_OP_UDIV, w2, n, d);
    uam_ref r = x_bin(x, sgn ? UAM_OP_SREM : UAM_OP_UREM, w2, n, d);
    uam_ref ql = x_trunc(x, w, w2, q);
    uam_ref rl = x_trunc(x, w, w2, r);
    uam_ref bad = sgn ? x_bin(x, UAM_OP_NE, w2, q, x_sext(x, w2, w, ql))
                      : x_bin(x, UAM_OP_NE, w2, q, x_zext(x, w2, w, ql));
    bad = x_bin(x, UAM_OP_OR, 1, bad, x_bin(x, UAM_OP_EQ, w2, d, x_imm(x, 0)));
    de_if(x, bad);
    if (w == 8) {
        uam_ref ax = x_bin(x, UAM_OP_OR, 16, x_zext(x, 16, 8, ql),
                           x_bin(x, UAM_OP_SHL, 16, x_zext(x, 16, 8, rl), x_imm(x, 8)));
        x_setreg(x, X86_RAX, 16, ax);
    } else {
        x_setreg(x, X86_RAX, w, ql);
        x_setreg(x, X86_RDX, w, rl);
    }
}

/* F6/F7: TEST NOT NEG MUL IMUL DIV IDIV */
static lr grp3(xl *x, unsigned w)
{
    xop o = x_rm(x);
    unsigned sub = x->in.reg & 7;
    switch (sub) {
    case 0: case 1: {
        uam_ref a = x_rd(x, &o, w);
        x_flags_logic(x, w, x_bin(x, UAM_OP_AND, w, a, x_imm(x, x->in.imm & xmask(w))));
        break;
    }
    case 2: x_wr(x, &o, w, x_bin(x, UAM_OP_XOR, w, x_rd(x, &o, w), x_imm(x, xmask(w)))); break;
    case 3: {
        uam_ref a = x_rd(x, &o, w);
        uam_ref z = x_imm(x, 0);
        uam_ref res = x_bin(x, UAM_OP_SUB, w, z, a);
        x_wr(x, &o, w, res);
        x_flags_sub(x, w, z, a, res, NONE);
        break;
    }
    case 4: case 5: mul1(x, sub == 5, w, x_rd(x, &o, w)); break;
    default:        div1(x, sub == 7, w, x_rd(x, &o, w)); break;
    }
    return LR_CONT;
}

/* ---- immediates ---------------------------------------------------------- */

uint64_t x_sx(uint64_t v, unsigned from_bits)
{
    if (from_bits >= 64) return v;
    uint64_t m = 1ull << (from_bits - 1);
    v &= (m << 1) - 1;
    return (v ^ m) - m;
}

/* An immediate of `bytes` bytes as an operand of width w (sign-extended). */
static uam_ref simm(xl *x, unsigned w, unsigned bytes)
{
    return x_imm(x, x_sx(x->in.imm, bytes * 8) & xmask(w));
}

/* Iz: 16 bits at osz 16, else 32 bits sign-extended. */
static uam_ref zimm(xl *x, unsigned w) { return simm(x, w, w == 16 ? 2 : 4); }

/* ---- control transfer ---------------------------------------------------- */

/* Return address (rIP after this instruction) at width w. */
static uam_ref ret_ip(xl *x, unsigned w)
{
    return x_resize(x, w, x->code == X86_CODE64 ? 64 : 32, x_ip_of(x, x->next));
}

lr x_jcc(xl *x, unsigned cc, int64_t rel)
{
    uint64_t t = x_rel_target(x, rel, x->in.osz);
    uam_exit(x->b, x_cond(x, cc), x_imm(x, t), UAM_EXIT_JUMP);
    return LR_CONT;
}

static lr jmp_rel(xl *x, int64_t rel)
{
    x_jump(x, x_imm(x, x_rel_target(x, rel, x->in.osz)), UAM_EXIT_JUMP);
    return LR_END;
}

static lr call_rel(xl *x, int64_t rel)
{
    unsigned w = x->in.osz;
    uint64_t t = x_rel_target(x, rel, w);
    x_push(x, w, ret_ip(x, w));
    x_commit_sp(x);
    x_jump(x, x_imm(x, t), UAM_EXIT_CALL);
    return LR_END;
}

static lr call_ind(xl *x, uam_ref ip)
{
    unsigned w = x->in.osz;
    x_push(x, w, ret_ip(x, w));
    x_commit_sp(x);
    x_jump(x, x_ind_target(x, ip), UAM_EXIT_CALL);
    return LR_END;
}

static lr ret_near(xl *x, unsigned imm16)
{
    unsigned w = x->in.osz;
    uam_ref ip = x_pop(x, w);
    if (imm16)
        x->sp = x_bin(x, UAM_OP_ADD, x->sw, x->sp, x_imm(x, imm16));
    x_commit_sp(x);
    x_jump(x, x_ind_target(x, ip), UAM_EXIT_RET);
    return LR_END;
}

/* LOOP/LOOPE/LOOPNE/JrCXZ: the counter is rCX at the address size. */
static lr loop(xl *x, unsigned op)
{
    unsigned aw = x->in.asz;
    uint64_t t = x_rel_target(x, (int64_t)x_sx(x->in.imm, 8), x->in.osz);
    uam_ref c = x_reg(x, X86_RCX, aw);
    uam_ref take;
    if (op == 0xE3) {
        take = x_bin(x, UAM_OP_EQ, aw, c, x_imm(x, 0));
    } else {
        c = x_bin(x, UAM_OP_SUB, aw, c, x_imm(x, 1));
        x_setreg(x, X86_RCX, aw, c);
        take = x_bin(x, UAM_OP_NE, aw, c, x_imm(x, 0));
        if (op != 0xE2) {
            uam_ref zf = x_flag(x, X86_F_ZF);
            if (op == 0xE0) zf = x_bin(x, UAM_OP_XOR, 1, zf, x_imm(x, 1));
            take = x_bin(x, UAM_OP_AND, 1, take, zf);
        }
    }
    uam_exit(x->b, take, x_imm(x, t), UAM_EXIT_JUMP);
    return LR_CONT;
}

/* Software interrupts: the trap's tval is the linear return address. */
static lr soft_int(xl *x, unsigned vec, bool intn)
{
    uint64_t cause = vec | X86_TRAP_SOFT | (intn ? X86_TRAP_INTN : 0);
    uam_trap(x->b, NONE, UAM_TRAP_EXCEPTION, x_imm(x, cause), x_imm(x, x->next));
    x->end = true;
    return LR_END;
}

/* Far transfers go through helpers in every mode; they return the new linear pc. */
static lr far_jmp(xl *x, uam_ref sel, uam_ref off)
{
    uam_ref pc = x_helper(x, X86_H_FAR_JMP, x_zext(x, 64, 16, sel), x_zext(x, 64, 64, off),
                          x_imm(x, x->next));   /* where a task switch saves this task */
    x_jump(x, pc, UAM_EXIT_JUMP);
    return LR_END;
}

static lr far_call(xl *x, uam_ref sel, uam_ref off)
{
    uam_ref a = x_bin(x, UAM_OP_OR, 64, x_zext(x, 64, 16, sel), x_imm(x, (uint64_t)x->in.osz << 16));
    uam_ref pc = x_helper(x, X86_H_FAR_CALL, a, x_zext(x, 64, 64, off),
                          x_zext(x, 64, 64, ret_ip(x, 64)));
    x_jump(x, pc, UAM_EXIT_CALL);
    return LR_END;
}

/* Far pointer in memory: offset (osz) then selector. */
static void far_ptr(xl *x, xop *o, uam_ref *sel, uam_ref *off)
{
    unsigned w = x->in.osz;
    *off = x_ld(x, w, o->lin);
    *sel = x_ld(x, 16, x_bin(x, UAM_OP_ADD, 64, o->lin, x_imm(x, w / 8)));
}

/* ---- segment registers --------------------------------------------------- */

/* Load a data/stack segment register. Real and virtual-8086 mode are inline
 * (base = sel << 4); protected and long mode check the descriptor in a helper. */
void x_load_seg(xl *x, unsigned seg, uam_ref sel16)
{
    if (x86_priv_realseg(x->priv)) {
        uam_ref base = x_bin(x, UAM_OP_SHL, 64, x_zext(x, 64, 16, sel16), x_imm(x, 4));
        uam_put(x->b, 16, X86_OFF_SEG_SEL(seg), sel16, NONE);
        uam_put(x->b, 64, X86_OFF_SEG_BASE(seg), base, NONE);
    } else {
        x_helper(x, X86_H_LOAD_SEG, x_imm(x, seg), x_zext(x, 64, 16, sel16), NONE);
        if (seg == X86_SEG_SS)   /* SS.B selects the stack width: re-key the next block */
            x->end = true;
    }
    if (seg == X86_SEG_SS)       /* MOV/POP SS hold off interrupts for one instruction */
        x->sti_shadow = true;
}

uam_ref x_seg_sel(xl *x, unsigned seg) { return uam_get(x->b, 16, X86_OFF_SEG_SEL(seg)); }

lr x_push_seg(xl *x, unsigned seg)
{
    unsigned w = x->in.osz;
    x_push(x, w, x_zext(x, w, 16, x_seg_sel(x, seg)));
    x_commit_sp(x);
    return LR_CONT;
}

/* POP Sreg: the selector is loaded (and may fault) before RSP is committed. */
lr x_pop_seg(xl *x, unsigned seg)
{
    unsigned w = x->in.osz;
    uam_ref v = x_trunc(x, 16, w, x_pop(x, w));
    x_load_seg(x, seg, v);
    x_commit_sp(x);
    return LR_CONT;
}

/* LDS/LES/LSS/LFS/LGS: offset then selector from memory. */
lr x_load_far(xl *x, unsigned seg)
{
    if (x->in.mod == 3)
        return x_ud(x);
    xop o = x_rm(x);
    uam_ref sel, off;
    far_ptr(x, &o, &sel, &off);
    x_load_seg(x, seg, sel);
    x_setreg(x, x->in.reg, x->in.osz, off);
    return LR_CONT;
}

/* ---- flags --------------------------------------------------------------- */

static lr pushf(xl *x)
{
    unsigned w = x->in.osz;
    uam_ref f = uam_sysrd(x->b, 64, X86_SR_RFLAGS, NONE);
    x_push(x, w, x_resize(x, w, 64, f));
    x_commit_sp(x);
    return LR_CONT;
}

/* POPF: the handler applies the IOPL/CPL rules and asks for an exit if IF rose. */
static lr popf(xl *x)
{
    unsigned w = x->in.osz;
    uam_ref v = x_pop(x, w);
    uam_ref cur = uam_sysrd(x->b, 64, X86_SR_RFLAGS, NONE);
    if (w == 16)   /* only the low 16 bits change */
        v = x_bin(x, UAM_OP_OR, 64, x_bin(x, UAM_OP_AND, 64, cur, x_imm(x, ~0xFFFFull)),
                  x_zext(x, 64, 16, v));
    uam_syswr(x->b, 64, X86_SR_RFLAGS, x_zext(x, 64, w, v), NONE);
    x_commit_sp(x);
    x->end = true;   /* TF/IF may have changed */
    return LR_CONT;
}

static lr sahf(xl *x)
{
    uam_ref ah = uam_get(x->b, 8, X86_OFF_GPR(X86_RAX) + 1);   /* AH even with REX */
    static const unsigned bits[] = { 0, 2, 4, 6, 7 };
    static const unsigned flags[] = { X86_F_CF, X86_F_PF, X86_F_AF, X86_F_ZF, X86_F_SF };
    for (unsigned i = 0; i < 5; i++)
        x_setflag(x, flags[i], x_bin(x, UAM_OP_NE, 8,
                                     x_bin(x, UAM_OP_AND, 8, ah, x_imm(x, 1u << bits[i])),
                                     x_imm(x, 0)));
    return LR_CONT;
}

static lr lahf(xl *x)
{
    static const unsigned bits[] = { 0, 2, 4, 6, 7 };
    static const unsigned flags[] = { X86_F_CF, X86_F_PF, X86_F_AF, X86_F_ZF, X86_F_SF };
    uam_ref v = x_imm(x, 2);
    for (unsigned i = 0; i < 5; i++)
        v = x_bin(x, UAM_OP_OR, 8, v,
                  x_bin(x, UAM_OP_SHL, 8, x_zext(x, 8, 1, x_flag(x, flags[i])), x_imm(x, bits[i])));
    uam_put(x->b, 8, X86_OFF_GPR(X86_RAX) + 1, v, NONE);
    return LR_CONT;
}

/* ---- port I/O ------------------------------------------------------------ */

static unsigned io_width(xl *x)
{
    return (x->in.op & 1) ? (x->in.osz == 16 ? 16 : 32) : 8;
}

static lr in_out(xl *x, bool out, uam_ref port)
{
    unsigned w = io_width(x);
    bool imm = x->in.op < 0xEC;                      /* E4-E7: imm8 port; EC-EF: DX */
    unsigned type = out ? (imm ? X86_IO_OUT_IMM : X86_IO_OUT_DX)
                        : (imm ? X86_IO_IN_IMM : X86_IO_IN_DX);
    /* one helper records the SMM I/O state and accesses the port */
    uam_ref v = x_helper(x, X86_H_IO_PORT, x_io_misc(x, port, w, type), x_imm(x, x_io_rip(x)),
                         out ? x_zext(x, 64, w, x_reg(x, X86_RAX, w)) : x_imm(x, 0));
    if (!out)
        x_setreg(x, X86_RAX, w, x_trunc(x, w, 64, v));
    return LR_CONT;
}

/* ---- data movement ------------------------------------------------------- */

static lr xchg(xl *x, xop *o, unsigned r, unsigned w)
{
    uam_ref a = x_rd(x, o, w);
    uam_ref b = x_reg(x, r, w);
    x_wr(x, o, w, b);
    x_setreg(x, r, w, a);
    return LR_CONT;
}

static lr enter(xl *x)
{
    unsigned w = x->in.osz, sw = x->sw;
    unsigned level = x->in.imm2 & 31;
    uam_ref bp = x_reg(x, X86_RBP, sw);
    x_push(x, w, x_reg(x, X86_RBP, w));
    uam_ref frame = x_sp(x);
    for (unsigned i = 1; i < level; i++) {
        bp = x_bin(x, UAM_OP_SUB, sw, bp, x_imm(x, w / 8));
        x_push(x, w, x_ld(x, w, x_lin(x, X86_SEG_SS, bp, sw)));
    }
    if (level > 0)
        x_push(x, w, x_resize(x, w, sw, frame));
    x->sp = x_bin(x, UAM_OP_SUB, sw, x_sp(x), x_imm(x, x->in.imm & 0xFFFF));
    x_commit_sp(x);
    if (sw == 16) x_setreg(x, X86_RBP, 16, frame);
    else          x_setreg(x, X86_RBP, 64, x_zext(x, 64, sw, frame));
    return LR_CONT;
}

static lr leave(xl *x)
{
    unsigned w = x->in.osz;
    x->sp = x_reg(x, X86_RBP, x->sw);
    uam_ref bp = x_pop(x, w);
    x_commit_sp(x);
    x_setreg(x, X86_RBP, w, bp);
    return LR_CONT;
}

static lr pusha(xl *x)
{
    unsigned w = x->in.osz;
    uam_ref sp0 = x_reg(x, X86_RSP, w);
    for (unsigned r = 0; r < 8; r++)
        x_push(x, w, r == X86_RSP ? sp0 : x_reg(x, r, w));
    x_commit_sp(x);
    return LR_CONT;
}

static lr popa(xl *x)
{
    unsigned w = x->in.osz;
    uam_ref v[8];
    for (int r = 7; r >= 0; r--)
        v[r] = x_pop(x, w);
    x_commit_sp(x);
    for (unsigned r = 0; r < 8; r++)
        if (r != X86_RSP)
            x_setreg(x, r, w, v[r]);
    return LR_CONT;
}

static lr xlat(xl *x)
{
    unsigned aw = x->in.asz;
    uam_ref off = x_bin(x, UAM_OP_ADD, aw, x_reg(x, X86_RBX, aw),
                        x_zext(x, aw, 8, uam_get(x->b, 8, X86_OFF_GPR(X86_RAX))));
    uam_ref v = x_ld(x, 8, x_lin(x, x_seg(x, X86_SEG_DS), off, aw));
    uam_put(x->b, 8, X86_OFF_GPR(X86_RAX), v, NONE);
    return LR_CONT;
}

/* 98: CBW/CWDE/CDQE, 99: CWD/CDQ/CQO */
static lr cbw_cwd(xl *x, bool cwd)
{
    unsigned w = x->in.osz;
    if (cwd) {
        uam_ref a = x_reg(x, X86_RAX, w);
        x_setreg(x, X86_RDX, w, x_bin(x, UAM_OP_ASHR, w, a, x_imm(x, w - 1)));
    } else {
        x_setreg(x, X86_RAX, w, x_sext(x, w, w / 2, x_reg(x, X86_RAX, w / 2)));
    }
    return LR_CONT;
}

/* IMUL Gv, Ev, imm (69, 6B) and IMUL Gv, Ev (0F AF) */
void x_imul2(xl *x, unsigned w, uam_ref a, uam_ref b)
{
    uam_ref lo = x_bin(x, UAM_OP_MUL, w, a, b);
    uam_ref hi = x_bin(x, UAM_OP_MULHS, w, a, b);
    uam_ref ovf = x_bin(x, UAM_OP_NE, w, hi, x_bin(x, UAM_OP_ASHR, w, lo, x_imm(x, w - 1)));
    x_setreg(x, x->in.reg, w, lo);
    x_setflag(x, X86_F_CF, ovf);
    x_setflag(x, X86_F_OF, ovf);
    x_set_szp(x, w, lo);
}

static uam_ref moffs_lin(xl *x)
{
    unsigned aw = x->in.asz;
    return x_lin(x, x_seg(x, X86_SEG_DS), x_imm(x, x->in.imm & xmask(aw)), aw);
}

/* AAM/AAD (D4/D5): base in the immediate. */
static lr aam_aad(xl *x, bool aad)
{
    if (!aad && (x->in.imm & 0xFF) == 0)
        return x_trap(x, X86_EXC_DE);
    uam_ref base = x_imm(x, x->in.imm & 0xFF);
    uam_ref al = uam_get(x->b, 8, X86_OFF_GPR(X86_RAX));
    uam_ref res;
    if (aad) {
        uam_ref ah = uam_get(x->b, 8, X86_OFF_GPR(X86_RAX) + 1);
        res = x_bin(x, UAM_OP_ADD, 8, al, x_bin(x, UAM_OP_MUL, 8, ah, base));
        x_setreg(x, X86_RAX, 16, x_zext(x, 16, 8, res));
    } else {
        uam_ref q = x_bin(x, UAM_OP_UDIV, 8, al, base);
        res = x_bin(x, UAM_OP_UREM, 8, al, base);
        x_setreg(x, X86_RAX, 16, x_bin(x, UAM_OP_OR, 16, x_zext(x, 16, 8, res),
                                       x_bin(x, UAM_OP_SHL, 16, x_zext(x, 16, 8, q), x_imm(x, 8))));
    }
    x_flags_logic(x, 8, res);
    return LR_CONT;
}

/* ---- one-byte map: 00-7F ------------------------------------------------- */

static xop reg_op(unsigned r) { xop o = { false, r, NONE }; return o; }

static lr alu_form(xl *x, unsigned opc)
{
    unsigned op = (opc >> 3) & 7, form = opc & 7;
    unsigned w = (form & 1) ? x->in.osz : 8;
    xop d;
    uam_ref a, b;
    switch (form) {
    case 0: case 1:
        d = x_rm(x);
        a = x_rd(x, &d, w);
        b = x_reg(x, x->in.reg, w);
        break;
    case 2: case 3: {
        xop s = x_rm(x);
        b = x_rd(x, &s, w);
        d = reg_op(x->in.reg);
        a = x_reg(x, x->in.reg, w);
        break;
    }
    default:
        d = reg_op(X86_RAX);
        a = x_reg(x, X86_RAX, w);
        b = (w == 8) ? x_imm(x, x->in.imm & 0xFF) : zimm(x, w);
        break;
    }
    alu(x, op, w, &d, a, b);
    return LR_CONT;
}

static lr lift_lo(xl *x)
{
    unsigned op = x->in.op, w = x->in.osz;
    if (op < 0x40 && (op & 7) < 6)
        return alu_form(x, op);
    switch (op) {
    case 0x06: return x_push_seg(x, X86_SEG_ES);
    case 0x07: return x_pop_seg(x, X86_SEG_ES);
    case 0x0E: return x_push_seg(x, X86_SEG_CS);
    case 0x16: return x_push_seg(x, X86_SEG_SS);
    case 0x17: return x_pop_seg(x, X86_SEG_SS);
    case 0x1E: return x_push_seg(x, X86_SEG_DS);
    case 0x1F: return x_pop_seg(x, X86_SEG_DS);
    case 0x27: case 0x2F: case 0x37: case 0x3F:   /* DAA DAS AAA AAS: not modelled */
        return x_ud(x);
    }
    if (op >= 0x40 && op <= 0x4F) {   /* INC/DEC r (not 64-bit: those are REX) */
        xop o = reg_op(op & 7);
        incdec(x, op >= 0x48, w, &o);
        return LR_CONT;
    }
    if (op >= 0x50 && op <= 0x57) {
        x_push(x, w, x_reg(x, (op & 7) | x->in.rex_b, w));
        x_commit_sp(x);
        return LR_CONT;
    }
    if (op >= 0x58 && op <= 0x5F) {
        uam_ref v = x_pop(x, w);
        x_commit_sp(x);
        x_setreg(x, (op & 7) | x->in.rex_b, w, v);
        return LR_CONT;
    }
    if (op >= 0x70)
        return x_jcc(x, op & 0xF, (int64_t)x_sx(x->in.imm, 8));
    switch (op) {
    case 0x60: return pusha(x);
    case 0x61: return popa(x);
    case 0x63:
        if (x->code != X86_CODE64)
            return x_ud(x);   /* ARPL */
        else {
            xop s = x_rm(x);
            uam_ref v = x_rd(x, &s, 32);
            x_setreg(x, x->in.reg, w, w == 64 ? x_sext(x, 64, 32, v) : x_resize(x, w, 32, v));
            return LR_CONT;
        }
    case 0x68: case 0x6A:
        x_push(x, w, op == 0x6A ? simm(x, w, 1) : zimm(x, w));
        x_commit_sp(x);
        return LR_CONT;
    case 0x69: case 0x6B: {
        xop s = x_rm(x);
        x_imul2(x, w, x_rd(x, &s, w), op == 0x6B ? simm(x, w, 1) : zimm(x, w));
        return LR_CONT;
    }
    case 0x6C: case 0x6D: case 0x6E: case 0x6F:
        return x_lift_string(x);
    }
    return x_ud(x);
}

/* ---- one-byte map: 80-BF ------------------------------------------------- */

static lr lift_80(xl *x)
{
    unsigned op = x->in.op, w = x->in.osz;
    unsigned bw = (op & 1) ? w : 8;
    switch (op) {
    case 0x80: case 0x81: case 0x82: case 0x83: {
        xop d = x_rm(x);
        uam_ref a = x_rd(x, &d, bw);
        uam_ref b = (op == 0x81) ? zimm(x, w) : simm(x, bw, 1);
        alu(x, x->in.reg & 7, bw, &d, a, b);
        return LR_CONT;
    }
    case 0x84: case 0x85: {
        xop s = x_rm(x);
        uam_ref a = x_rd(x, &s, bw);
        x_flags_logic(x, bw, x_bin(x, UAM_OP_AND, bw, a, x_reg(x, x->in.reg, bw)));
        return LR_CONT;
    }
    case 0x86: case 0x87: {
        xop s = x_rm(x);
        return xchg(x, &s, x->in.reg, bw);
    }
    case 0x88: case 0x89: {
        xop d = x_rm(x);
        x_wr(x, &d, bw, x_reg(x, x->in.reg, bw));
        return LR_CONT;
    }
    case 0x8A: case 0x8B: {
        xop s = x_rm(x);
        x_setreg(x, x->in.reg, bw, x_rd(x, &s, bw));
        return LR_CONT;
    }
    case 0x8C: {   /* MOV Ev, Sreg */
        unsigned sr = (x->in.modrm >> 3) & 7;
        if (sr >= X86_NSEG)
            return x_ud(x);
        xop d = x_rm(x);
        uam_ref v = x_seg_sel(x, sr);
        if (d.mem) x_st(x, 16, d.lin, v);
        else       x_setreg(x, d.reg, w, x_zext(x, w, 16, v));
        return LR_CONT;
    }
    case 0x8D:     /* LEA */
        if (x->in.mod == 3)
            return x_ud(x);
        x_setreg(x, x->in.reg, w, x_resize(x, w, x->in.asz, x_ea_off(x)));
        return LR_CONT;
    case 0x8E: {   /* MOV Sreg, Ew */
        unsigned sr = (x->in.modrm >> 3) & 7;
        if (sr >= X86_NSEG || sr == X86_SEG_CS)
            return x_ud(x);
        xop s = x_rm(x);
        x_load_seg(x, sr, x_rd(x, &s, 16));
        return LR_CONT;
    }
    case 0x8F: {   /* POP Ev: rSP is incremented before the address is formed */
        uam_ref v = x_pop(x, w);
        if (x->in.mod == 3) {
            x_commit_sp(x);
            x_setreg(x, x->in.rm, w, v);
            return LR_CONT;
        }
        /* POP m: the Intel SDM forms the destination address *after* rSP is
         * incremented, so when rSP is the operand's base the address must use
         * the post-pop value. x_ea_off() reads the pre-pop rSP from the register
         * file; add the pop width (in the address-size domain) when the base is
         * rSP (rSP can never be a SIB index, so the base is the only place it
         * appears). Without this, `pop disp(%rsp)` writes the wrong slot — e.g.
         * Linux's `pushf; pop 8(%rsp)` local_irq_save stores the flags one slot
         * low, and the matching local_irq_restore then reads a stale byte and
         * re-enables interrupts inside a hard IRQ. rSP is committed only after
         * the store, which may fault and must precede any register write. */
        uam_ref off = x_ea_off(x);
        if (x->in.ea_base == X86_RSP)
            off = x_bin(x, UAM_OP_ADD, x->in.asz, off, x_imm(x, w / 8));
        x_st(x, w, x_lin(x, x_seg(x, x->in.ea_dseg), off, x->in.asz), v);
        x_commit_sp(x);
        return LR_CONT;
    }
    case 0x90:
        if (x->in.rex_b) {
            xop o = reg_op(X86_RAX);
            return xchg(x, &o, 8, w);
        }
        return LR_CONT;   /* NOP / PAUSE */
    case 0x98: return cbw_cwd(x, false);
    case 0x99: return cbw_cwd(x, true);
    case 0x9A: return far_call(x, x_imm(x, x->in.imm2), x_imm(x, x->in.imm & xmask(w)));
    case 0x9B: return x_lift_x87(x);
    case 0x9C: return pushf(x);
    case 0x9D: return popf(x);
    case 0x9E: return sahf(x);
    case 0x9F: return lahf(x);
    case 0xA0: case 0xA1:
        x_setreg(x, X86_RAX, bw, x_ld(x, bw, moffs_lin(x)));
        return LR_CONT;
    case 0xA2: case 0xA3:
        x_st(x, bw, moffs_lin(x), x_reg(x, X86_RAX, bw));
        return LR_CONT;
    case 0xA8: case 0xA9: {
        uam_ref b = (bw == 8) ? x_imm(x, x->in.imm & 0xFF) : zimm(x, w);
        x_flags_logic(x, bw, x_bin(x, UAM_OP_AND, bw, x_reg(x, X86_RAX, bw), b));
        return LR_CONT;
    }
    case 0xA4: case 0xA5: case 0xA6: case 0xA7:
    case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF:
        return x_lift_string(x);
    }
    if (op >= 0x91 && op <= 0x97) {
        xop o = reg_op(X86_RAX);
        return xchg(x, &o, (op & 7) | x->in.rex_b, w);
    }
    if (op >= 0xB0 && op <= 0xB7) {
        x_setreg(x, (op & 7) | x->in.rex_b, 8, x_imm(x, x->in.imm & 0xFF));
        return LR_CONT;
    }
    if (op >= 0xB8) {
        x_setreg(x, (op & 7) | x->in.rex_b, w, x_imm(x, x->in.imm & xmask(w)));
        return LR_CONT;
    }
    return x_ud(x);
}

/* ---- one-byte map: C0-FF ------------------------------------------------- */

static lr grp5(xl *x, unsigned bw)
{
    unsigned sub = x->in.reg & 7, w = x->in.osz;
    xop o = x_rm(x);
    if (x->in.op == 0xFE && sub > 1)
        return x_ud(x);
    switch (sub) {
    case 0: case 1: incdec(x, sub == 1, bw, &o); return LR_CONT;
    case 2: return call_ind(x, x_rd(x, &o, w));
    case 4: x_jump(x, x_ind_target(x, x_rd(x, &o, w)), UAM_EXIT_JUMP); return LR_END;
    case 3: case 5: {
        if (!o.mem)
            return x_ud(x);
        uam_ref sel, off;
        far_ptr(x, &o, &sel, &off);
        return sub == 3 ? far_call(x, sel, off) : far_jmp(x, sel, off);
    }
    case 6:
        x_push(x, w, x_rd(x, &o, w));
        x_commit_sp(x);
        return LR_CONT;
    }
    return x_ud(x);
}

static lr lift_c0(xl *x)
{
    unsigned op = x->in.op, w = x->in.osz;
    unsigned bw = (op & 1) ? w : 8;
    switch (op) {
    case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
        xop o = x_rm(x);
        uam_ref cnt = (op <= 0xC1) ? x_imm(x, x->in.imm & 0xFF)
                    : (op <= 0xD1) ? x_imm(x, 1)
                    : uam_get(x->b, 8, X86_OFF_GPR(X86_RCX));
        shift(x, x->in.reg & 7, bw, &o, cnt);
        return LR_CONT;
    }
    case 0xC2: return ret_near(x, x->in.imm & 0xFFFF);
    case 0xC3: return ret_near(x, 0);
    case 0xC4: return x_load_far(x, X86_SEG_ES);
    case 0xC5: return x_load_far(x, X86_SEG_DS);
    case 0xC6: case 0xC7: {
        if ((x->in.reg & 7) != 0)
            return x_ud(x);
        xop d = x_rm(x);
        x_wr(x, &d, bw, bw == 8 ? x_imm(x, x->in.imm & 0xFF) : zimm(x, w));
        return LR_CONT;
    }
    case 0xC8: return enter(x);
    case 0xC9: return leave(x);
    case 0xCA: case 0xCB: {
        uam_ref pc = x_helper(x, X86_H_RETF, x_imm(x, w),
                              x_imm(x, op == 0xCA ? (x->in.imm & 0xFFFF) : 0), NONE);
        x_jump(x, pc, UAM_EXIT_RET);
        return LR_END;
    }
    case 0xCC: return soft_int(x, X86_EXC_BP, false);
    case 0xCD: return soft_int(x, x->in.imm & 0xFF, true);
    case 0xCE: {   /* INTO (not 64-bit) */
        uam_ref cause = x_imm(x, X86_EXC_OF | X86_TRAP_SOFT);
        uam_trap(x->b, x_flag(x, X86_F_OF), UAM_TRAP_EXCEPTION, cause, x_imm(x, x->next));
        return LR_CONT;
    }
    case 0xCF: {
        uam_ref pc = x_helper(x, X86_H_IRET, x_imm(x, w), x_imm(x, x->next), NONE);
        x_jump(x, pc, UAM_EXIT_EXC_RETURN);
        return LR_END;
    }
    case 0xD4: return aam_aad(x, false);
    case 0xD5: return aam_aad(x, true);
    case 0xD7: return xlat(x);
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: return loop(x, op);
    case 0xE4: case 0xE5: return in_out(x, false, x_imm(x, x->in.imm & 0xFF));
    case 0xE6: case 0xE7: return in_out(x, true, x_imm(x, x->in.imm & 0xFF));
    case 0xEC: case 0xED: return in_out(x, false, x_reg(x, X86_RDX, 16));
    case 0xEE: case 0xEF: return in_out(x, true, x_reg(x, X86_RDX, 16));
    case 0xE8: return call_rel(x, (int64_t)x_sx(x->in.imm, w == 16 ? 16 : 32));
    case 0xE9: return jmp_rel(x, (int64_t)x_sx(x->in.imm, w == 16 ? 16 : 32));
    case 0xEB: return jmp_rel(x, (int64_t)x_sx(x->in.imm, 8));
    case 0xEA: return far_jmp(x, x_imm(x, x->in.imm2), x_imm(x, x->in.imm & xmask(w)));
    case 0xF1: return soft_int(x, X86_EXC_DB, false);
    case 0xF4:     /* HLT */
        if (x->cpl > 0)
            return x_trap_err(x, X86_EXC_GP, 0);
        uam_wait(x->b, x_imm(x, x->next));
        x->end = true;
        return LR_END;
    case 0xF5: x_setflag(x, X86_F_CF, x_bin(x, UAM_OP_XOR, 1, x_flag(x, X86_F_CF), x_imm(x, 1)));
               return LR_CONT;
    case 0xF6: case 0xF7: return grp3(x, bw);
    case 0xF8: case 0xF9: x_setflag(x, X86_F_CF, x_imm(x, op & 1)); return LR_CONT;
    case 0xFC: case 0xFD: x_setflag(x, X86_F_DF, x_imm(x, op & 1)); return LR_CONT;
    case 0xFA:     /* CLI */
        if (x->cpl > 0) uam_syswr(x->b, 64, X86_SR_CLI, x_imm(x, 0), NONE);
        else            x_setflag(x, X86_F_IF, x_imm(x, 0));
        return LR_CONT;
    case 0xFB:     /* STI: the handler requests an exit; the next insn is shadowed */
        uam_syswr(x->b, 64, X86_SR_STI, x_imm(x, 0), NONE);
        x->sti_shadow = true;
        return LR_CONT;
    case 0xFE: case 0xFF: return grp5(x, bw);
    }
    if (op >= 0xD8 && op <= 0xDF)
        return x_lift_x87(x);
    return x_ud(x);
}

/* ---- block driver -------------------------------------------------------- */

typedef struct fetch_ctx {
    uam_lift_ctx *lc;
    uint64_t      pc;
    bool          wrap32;
    uam_status    st;
} fetch_ctx;

static bool fetch_byte(void *ctx, unsigned off, uint8_t *byte)
{
    fetch_ctx *f = ctx;
    uint64_t a = f->pc + off;
    if (f->wrap32)
        a &= 0xFFFFFFFFu;
    uint64_t v = 0;
    f->st = f->lc->fetch(f->lc, a, 1, &v);
    *byte = (uint8_t)v;
    return f->st == UAM_OK;
}

static lr lift_insn(xl *x)
{
    switch (x->in.map) {
    case X86_MAP_1:
        if (x->in.op < 0x80) return lift_lo(x);
        if (x->in.op < 0xC0) return lift_80(x);
        return lift_c0(x);
    case X86_MAP_0F:
        return x_lift_0f(x);
    default:
        return x_lift_sse(x);
    }
}

uam_status x86_lift(uam_lift_ctx *lc)
{
    uam_cpu *cpu = lc->cpu;
    xl x = { .lc = lc, .b = lc->b, .priv = cpu->priv };
    x.code = x86_priv_code(x.priv);
    x.cpl = x86_priv_cpl(x.priv);
    x.sw = x86_priv_stack(x.priv);
    x.cs_base = x86_seg_base(cpu, X86_SEG_CS);
    bool wrap32 = x.code != X86_CODE64;
    uint64_t pc = lc->pc;
    bool shadow = false;   /* the previous instruction holds off interrupts */

    for (uint32_t n = 0;; n++) {
        /* one slot stays free: an instruction that opens an interrupt shadow
         * (STI, MOV SS) takes the next one into the block */
        if (n + 1 >= lc->max_insns && !shadow) {
            uam_exit(lc->b, NONE, uam_imm(lc->b, pc), UAM_EXIT_JUMP);
            return UAM_OK;
        }
        fetch_ctx fc = { lc, pc, wrap32, UAM_OK };
        x86_dec_status ds = x86_decode(&x.in, x.code, fetch_byte, &fc);
        if (ds == X86_DEC_FAULT) {
            if (n == 0)
                return fc.st != UAM_OK ? fc.st : UAM_ERR_FAULT;
            uam_exit(lc->b, NONE, uam_imm(lc->b, pc), UAM_EXIT_JUMP);
            return UAM_OK;
        }
        if (shadow)
            uam_imark_cont(lc->b, pc, x.in.len);
        else
            uam_imark(lc->b, pc, x.in.len);
        x.pc = pc;
        x.next = pc + x.in.len;
        if (wrap32)
            x.next &= 0xFFFFFFFFu;
        x.sp = NONE;
        x.end = false;
        x.sti_shadow = false;

        lr r = (ds == X86_DEC_UD) ? x_ud(&x) : lift_insn(&x);
        if (r == LR_END)
            return UAM_OK;
        if (x.end) {
            uam_exit(lc->b, NONE, uam_imm(lc->b, x.next), UAM_EXIT_JUMP);
            return UAM_OK;
        }
        shadow = x.sti_shadow;
        pc = x.next;
    }
}
