/*
 * x86_lift_ops.c - Emission helpers for the x86 lifter (see x86_lift.h).
 *
 * Conventions:
 *   - Flags are bytes in the register file. CF/AF/ZF/SF/OF/DF/IF hold 0 or 1;
 *     PF holds the low byte of the last result and is its even parity.
 *     Carry and overflow are computed with plain compares and bit ops (no
 *     CARRY/SCARRY ops), which the JIT emits inline.
 *   - Within one instruction every load, store, SYSRD/SYSWR, trap and
 *     faulting helper precedes every PUT (the commit rule). Stack-pointer
 *     updates are therefore held in xl.sp and written by x_commit_sp().
 *   - Linear addresses are 64-bit values. Outside 64-bit code they are
 *     formed in 32 bits (segment base + offset wraps at 4 GiB).
 */
#include "x86_lift.h"

static const uam_helper_table x86_htab = { x86_helpers, X86_H__COUNT };

/* ---- immediates and arithmetic ------------------------------------------- */

uam_ref x_imm(xl *x, uint64_t v) { return uam_imm(x->b, v); }

uam_ref x_bin(xl *x, uam_opcode op, unsigned w, uam_ref a, uam_ref b)
{
    return uam_binop(x->b, op, w, a, b);
}

uam_ref x_un(xl *x, uam_opcode op, unsigned w, uam_ref a) { return uam_unop(x->b, op, w, a); }

static uint64_t imm_val(xl *x, uam_ref r) { return x->b->imm[uam_ref_index(r)]; }

static unsigned width_of(xl *x, uam_ref v, unsigned from)
{
    unsigned w = uam_builder_ref_width(x->b, v);
    return w ? w : from;
}

uam_ref x_zext(xl *x, unsigned to, unsigned from, uam_ref v)
{
    if (uam_ref_is_imm(v))
        return x_imm(x, imm_val(x, v) & xmask(from));
    if (width_of(x, v, from) == to)
        return v;
    return x_un(x, UAM_OP_ZEXT, to, v);
}

uam_ref x_sext(xl *x, unsigned to, unsigned from, uam_ref v)
{
    if (uam_ref_is_imm(v)) {
        uint64_t u = imm_val(x, v) & xmask(from);
        if (from < 64 && (u >> (from - 1)) & 1)
            u |= ~xmask(from);
        return x_imm(x, u & xmask(to));
    }
    if (width_of(x, v, from) == to)
        return v;
    return x_un(x, UAM_OP_SEXT, to, v);
}

uam_ref x_trunc(xl *x, unsigned to, unsigned from, uam_ref v)
{
    if (uam_ref_is_imm(v))
        return x_imm(x, imm_val(x, v) & xmask(to));
    if (width_of(x, v, from) == to)
        return v;
    return x_un(x, UAM_OP_TRUNC, to, v);
}

uam_ref x_resize(xl *x, unsigned to, unsigned from, uam_ref v)
{
    from = width_of(x, v, from);
    return (to >= from) ? x_zext(x, to, from, v) : x_trunc(x, to, from, v);
}

/* ---- registers ----------------------------------------------------------- */

/* Without a REX prefix, byte registers 4-7 are AH CH DH BH. */
static bool high_byte(xl *x, unsigned r, unsigned w) { return w == 8 && !x->in.rex && r >= 4 && r < 8; }

uam_ref x_reg(xl *x, unsigned r, unsigned w)
{
    if (high_byte(x, r, w))
        return uam_get(x->b, 8, X86_OFF_GPR(r - 4) + 1);
    return uam_get(x->b, w, X86_OFF_GPR(r));
}

void x_setreg(xl *x, unsigned r, unsigned w, uam_ref v)
{
    if (high_byte(x, r, w)) {
        uam_put(x->b, 8, X86_OFF_GPR(r - 4) + 1, x_resize(x, 8, 8, v), NONE);
        return;
    }
    if (w == 32) {   /* a 32-bit write zero-extends into the full register */
        uam_put(x->b, 64, X86_OFF_GPR(r), x_zext(x, 64, 32, v), NONE);
        return;
    }
    uam_put(x->b, w, X86_OFF_GPR(r), v, NONE);
}

uam_ref x_flag(xl *x, unsigned f)
{
    uam_ref v = uam_get(x->b, 8, X86_OFF_FLAG(f));
    if (f == X86_F_PF) {
        uam_ref pc = x_un(x, UAM_OP_POPCNT, 8, v);
        return x_bin(x, UAM_OP_EQ, 8, x_bin(x, UAM_OP_AND, 8, pc, x_imm(x, 1)), x_imm(x, 0));
    }
    return x_bin(x, UAM_OP_NE, 8, v, x_imm(x, 0));
}

void x_setflag(xl *x, unsigned f, uam_ref b1)
{
    uam_ref v;
    if (uam_ref_is_imm(b1))
        v = x_imm(x, imm_val(x, b1) & 1);
    else
        v = x_un(x, UAM_OP_ZEXT, 8, b1);
    if (f == X86_F_PF)   /* keep the parity convention: byte 0 = even = PF set */
        v = x_bin(x, UAM_OP_XOR, 8, v, x_imm(x, 1));
    uam_put(x->b, 8, X86_OFF_FLAG(f), v, NONE);
}

static uam_ref sign_bit(xl *x, unsigned w, uam_ref v)
{
    return x_bin(x, UAM_OP_SLT, w, v, x_imm(x, 0));
}

void x_set_szp(xl *x, unsigned w, uam_ref res)
{
    x_setflag(x, X86_F_ZF, x_bin(x, UAM_OP_EQ, w, res, x_imm(x, 0)));
    x_setflag(x, X86_F_SF, sign_bit(x, w, res));
    uam_put(x->b, 8, X86_OFF_FLAG(X86_F_PF), x_trunc(x, 8, w, res), NONE);
}

void x_flags_logic(xl *x, unsigned w, uam_ref res)
{
    x_setflag(x, X86_F_CF, x_imm(x, 0));
    x_setflag(x, X86_F_OF, x_imm(x, 0));
    x_setflag(x, X86_F_AF, x_imm(x, 0));
    x_set_szp(x, w, res);
}

static uam_ref af_of(xl *x, unsigned w, uam_ref a, uam_ref b, uam_ref res)
{
    uam_ref t = x_bin(x, UAM_OP_XOR, w, x_bin(x, UAM_OP_XOR, w, a, b), res);
    return x_bin(x, UAM_OP_NE, w, x_bin(x, UAM_OP_AND, w, t, x_imm(x, 0x10)), x_imm(x, 0));
}

void x_flags_add(xl *x, unsigned w, uam_ref a, uam_ref b, uam_ref res, uam_ref cin)
{
    uam_ref cf = x_bin(x, UAM_OP_ULT, w, res, a);
    if (cin != NONE)
        cf = x_bin(x, UAM_OP_OR, 1, cf,
                   x_bin(x, UAM_OP_AND, 1, cin, x_bin(x, UAM_OP_EQ, w, res, a)));
    uam_ref of = sign_bit(x, w, x_bin(x, UAM_OP_AND, w, x_bin(x, UAM_OP_XOR, w, a, res),
                                      x_bin(x, UAM_OP_XOR, w, b, res)));
    x_setflag(x, X86_F_CF, cf);
    x_setflag(x, X86_F_OF, of);
    x_setflag(x, X86_F_AF, af_of(x, w, a, b, res));
    x_set_szp(x, w, res);
}

void x_flags_sub(xl *x, unsigned w, uam_ref a, uam_ref b, uam_ref res, uam_ref bin)
{
    uam_ref cf = x_bin(x, UAM_OP_ULT, w, a, b);
    if (bin != NONE)
        cf = x_bin(x, UAM_OP_OR, 1, cf,
                   x_bin(x, UAM_OP_AND, 1, bin, x_bin(x, UAM_OP_EQ, w, a, b)));
    uam_ref of = sign_bit(x, w, x_bin(x, UAM_OP_AND, w, x_bin(x, UAM_OP_XOR, w, a, b),
                                      x_bin(x, UAM_OP_XOR, w, a, res)));
    x_setflag(x, X86_F_CF, cf);
    x_setflag(x, X86_F_OF, of);
    x_setflag(x, X86_F_AF, af_of(x, w, a, b, res));
    x_set_szp(x, w, res);
}

uam_ref x_cond(xl *x, unsigned cc)
{
    uam_ref t;
    switch (cc >> 1) {
    case 0: t = x_flag(x, X86_F_OF); break;
    case 1: t = x_flag(x, X86_F_CF); break;
    case 2: t = x_flag(x, X86_F_ZF); break;
    case 3: t = x_bin(x, UAM_OP_OR, 1, x_flag(x, X86_F_CF), x_flag(x, X86_F_ZF)); break;
    case 4: t = x_flag(x, X86_F_SF); break;
    case 5: t = x_flag(x, X86_F_PF); break;
    case 6: t = x_bin(x, UAM_OP_NE, 1, x_flag(x, X86_F_SF), x_flag(x, X86_F_OF)); break;
    default:
        t = x_bin(x, UAM_OP_OR, 1, x_flag(x, X86_F_ZF),
                  x_bin(x, UAM_OP_NE, 1, x_flag(x, X86_F_SF), x_flag(x, X86_F_OF)));
        break;
    }
    return (cc & 1) ? x_bin(x, UAM_OP_XOR, 1, t, x_imm(x, 1)) : t;
}

/* ---- memory operands ----------------------------------------------------- */

uam_ref x_seg_base(xl *x, unsigned seg)
{
    if (x->code == X86_CODE64 && seg < X86_SEG_FS)
        return x_imm(x, 0);
    return uam_get(x->b, 64, X86_OFF_SEG_BASE(seg));
}

uam_ref x_ea_off(xl *x)
{
    const x86_insn *in = &x->in;
    const unsigned w = in->asz;
    if (in->rip_rel)
        return x_imm(x, (x->next + (uint64_t)in->disp) & xmask(w));
    uam_ref off = NONE;
    if (in->ea_base >= 0)
        off = uam_get(x->b, w, X86_OFF_GPR(in->ea_base));
    if (in->ea_index >= 0) {
        uam_ref idx = uam_get(x->b, w, X86_OFF_GPR(in->ea_index));
        if (in->ea_scale > 1)
            idx = x_bin(x, UAM_OP_SHL, w, idx, x_imm(x, (uint64_t)__builtin_ctz(in->ea_scale)));
        off = (off == NONE) ? idx : x_bin(x, UAM_OP_ADD, w, off, idx);
    }
    uint64_t d = (uint64_t)in->disp & xmask(w);
    if (off == NONE)
        return x_imm(x, d);
    return d ? x_bin(x, UAM_OP_ADD, w, off, x_imm(x, d)) : off;
}

uam_ref x_lin(xl *x, unsigned seg, uam_ref off, unsigned offw)
{
    if (x->code == X86_CODE64) {
        uam_ref o = x_zext(x, 64, offw, off);
        if (seg >= X86_SEG_FS)
            return x_bin(x, UAM_OP_ADD, 64, uam_get(x->b, 64, X86_OFF_SEG_BASE(seg)), o);
        return o;
    }
    uam_ref base = uam_get(x->b, 32, X86_OFF_SEG_BASE(seg));
    uam_ref lin = x_bin(x, UAM_OP_ADD, 32, base, x_zext(x, 32, offw, off));
    return x_zext(x, 64, 32, lin);
}

xop x_rm(xl *x)
{
    xop o = { 0 };
    if (x->in.mod == 3) {
        o.reg = x->in.rm;
        return o;
    }
    o.mem = true;
    o.lin = x_lin(x, x_seg(x, x->in.ea_dseg), x_ea_off(x), x->in.asz);
    return o;
}

uam_ref x_ld(xl *x, unsigned w, uam_ref lin) { return uam_ld(x->b, w, UAM_ACC_LE, lin, NONE); }
void    x_st(xl *x, unsigned w, uam_ref lin, uam_ref v) { uam_st(x->b, w, UAM_ACC_LE, lin, v, NONE); }

uam_ref x_rd(xl *x, xop *o, unsigned w)
{
    return o->mem ? x_ld(x, w, o->lin) : x_reg(x, o->reg, w);
}

void x_wr(xl *x, xop *o, unsigned w, uam_ref v)
{
    if (o->mem) x_st(x, w, o->lin, v);
    else        x_setreg(x, o->reg, w, v);
}

/* ---- stack --------------------------------------------------------------- */

uam_ref x_sp(xl *x)
{
    if (x->sp == NONE)
        x->sp = uam_get(x->b, x->sw, X86_OFF_GPR(X86_RSP));
    return x->sp;
}

static uam_ref stack_lin(xl *x, uam_ref sp) { return x_lin(x, X86_SEG_SS, sp, x->sw); }

void x_push(xl *x, unsigned w, uam_ref v)
{
    uam_ref nsp = x_bin(x, UAM_OP_SUB, x->sw, x_sp(x), x_imm(x, w / 8));
    x_st(x, w, stack_lin(x, nsp), v);
    x->sp = nsp;
}

uam_ref x_pop(xl *x, unsigned w)
{
    uam_ref sp = x_sp(x);
    uam_ref v = x_ld(x, w, stack_lin(x, sp));
    x->sp = x_bin(x, UAM_OP_ADD, x->sw, sp, x_imm(x, w / 8));
    return v;
}

void x_commit_sp(xl *x)
{
    if (x->sp == NONE)
        return;
    if (x->sw == 16) uam_put(x->b, 16, X86_OFF_GPR(X86_RSP), x->sp, NONE);
    else             uam_put(x->b, 64, X86_OFF_GPR(X86_RSP), x_zext(x, 64, x->sw, x->sp), NONE);
    x->sp = NONE;
}

/* ---- control ------------------------------------------------------------- */

uam_ref x_ip_of(xl *x, uint64_t lin)
{
    if (x->code == X86_CODE64)
        return x_imm(x, lin);
    return x_bin(x, UAM_OP_SUB, 32, x_imm(x, lin & 0xFFFFFFFFu),
                 uam_get(x->b, 32, X86_OFF_SEG_BASE(X86_SEG_CS)));
}

uint64_t x_rel_target(xl *x, int64_t rel, unsigned osz)
{
    if (x->code == X86_CODE64)
        return x->next + (uint64_t)rel;
    uint64_t ip = (x->next - x->cs_base + (uint64_t)rel) & xmask(osz);
    return (x->cs_base + ip) & 0xFFFFFFFFu;
}

uam_ref x_ind_target(xl *x, uam_ref ip)
{
    if (x->code == X86_CODE64)
        return x_zext(x, 64, 64, ip);
    uam_ref lin = x_bin(x, UAM_OP_ADD, 32, uam_get(x->b, 32, X86_OFF_SEG_BASE(X86_SEG_CS)),
                        x_zext(x, 32, 16, ip));
    return x_zext(x, 64, 32, lin);
}

void x_jump(xl *x, uam_ref target, uam_exit_kind k)
{
    uam_exit(x->b, NONE, target, k);
    x->end = true;
}

lr x_trap_err(xl *x, unsigned vec, uint32_t err)
{
    uint64_t cause = vec | X86_TRAP_ERR | ((uint64_t)err << 32);
    uam_trap(x->b, NONE, UAM_TRAP_EXCEPTION, x_imm(x, cause), x_imm(x, 0));
    x->end = true;
    return LR_END;
}

lr x_trap(xl *x, unsigned vec)
{
    uam_trap(x->b, NONE, UAM_TRAP_EXCEPTION, x_imm(x, vec), x_imm(x, 0));
    x->end = true;
    return LR_END;
}

lr x_ud(xl *x) { return x_trap(x, X86_EXC_UD); }

uam_ref x_helper(xl *x, unsigned id, uam_ref a, uam_ref b, uam_ref c)
{
    return uam_helper(x->b, &x86_htab, id, a, b, c, NONE);
}

uam_ref x_io_addr(xl *x, uam_ref port16)
{
    return x_bin(x, UAM_OP_ADD, 64, x_imm(x, UAM_X86_PIO_BASE), x_zext(x, 64, 16, port16));
}

/* X86_H_IO: the SMM I/O state of this instruction (before its access). */
uam_ref x_io_misc(xl *x, uam_ref port16, unsigned width, unsigned type)
{
    uint64_t fixed = 1u | (uint64_t)(width / 8) << 1 | (uint64_t)type << 4;
    return x_bin(x, UAM_OP_OR, 64, x_imm(x, fixed),
                 x_bin(x, UAM_OP_SHL, 64, x_zext(x, 64, 16, port16), x_imm(x, 16)));
}

uint64_t x_io_rip(const xl *x)
{
    uint64_t rip = x->pc - x->cs_base;
    return x->code == X86_CODE64 ? rip : rip & 0xFFFFFFFFu;
}

void x_io_begin(xl *x, uam_ref port16, unsigned width, unsigned type, uam_ref mem)
{
    x_helper(x, X86_H_IO, x_io_misc(x, port16, width, type), x_imm(x, x_io_rip(x)), mem);
}

