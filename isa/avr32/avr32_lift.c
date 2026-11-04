/*
 * avr32_lift.c - AVR32 decoder (avr32_dec.c) and lifter.
 *
 * Follows docs/uam-isa.md and the instruction descriptions of the AVR32
 * Architecture Manual (chapter 9):
 *   - R15 is the PC. A read of it is the address of the instruction; a write is a
 *     jump, lifted as an EXIT after the instruction's other effects (guarded by the
 *     instruction's condition, then a fall-through EXIT).
 *   - The flags live in the register file, one byte each (avr32.h); a condition is
 *     one or two pure ops, a conditional instruction guards its effects.
 *   - Faulting operations (loads, stores, system register accesses, traps) come
 *     before an instruction's register writes (the commit rule).
 *   - Halfword, word and doubleword accesses are aligned (ACC_ALIGNED), big-endian.
 *   - Privileged instructions in application mode lift to the Privilege Violation
 *     event; the privilege level tags translated code (cpu->priv).
 *   - Saturating DSP arithmetic and the SIMD instructions call pure helpers
 *     (avr32_dsp.c) that return the result and, above bit 32, the flags they set.
 * Left out: the Java extension (incjosp, popjc, pushjc, retj, the Java state),
 * the secure state (sscall, retss): Unimplemented Instruction; coprocessor
 * instructions (cop, ldc, stc, mvcr, mvrc, ldcm, stcm): Coprocessor Absent;
 * the SIMD instructions on a model without them: Unimplemented Instruction.
 */
#include "avr32.h"
#include "avr32_op.h"

#define NONE UAM_REF_NONE
#define BE   UAM_ACC_BE

typedef enum { LIFT_CONTINUE, LIFT_END } lift_result;

typedef struct a32_lifter {
    uam_builder       *b;
    const avr32_state *s;
    uint32_t           pc, len;
    uint32_t           w;        /* the instruction (a compact one in the upper halfword) */
    bool               priv;     /* lifted for a privileged mode */
    /* a write to R15 this instruction makes */
    bool               jump;
    uam_ref            jtarget, jguard;
    uam_exit_kind      jkind;
} a32_lifter;

static uint32_t F(const a32_lifter *L, unsigned hi, unsigned lo)
{
    return (L->w >> lo) & ((hi - lo == 31) ? 0xFFFFFFFFu : ((1u << (hi - lo + 1)) - 1));
}

static uam_ref imm(a32_lifter *L, uint32_t v) { return uam_imm(L->b, v); }
static uam_ref bin(a32_lifter *L, uam_opcode op, uam_ref a, uam_ref b) { return uam_binop(L->b, op, 32, a, b); }
static uam_ref binw(a32_lifter *L, uam_opcode op, unsigned w, uam_ref a, uam_ref b) { return uam_binop(L->b, op, w, a, b); }
static uam_ref sel(a32_lifter *L, uam_ref c, uam_ref a, uam_ref b) { return uam_select(L->b, 32, c, a, b); }
/* Conversions need a temporary source; immediates are converted as constants. */
static uint64_t imm_val(a32_lifter *L, uam_ref r) { return L->b->imm[uam_ref_index(r)]; }
static uam_ref zext(a32_lifter *L, unsigned w, uam_ref v)
{
    return uam_ref_is_imm(v) ? v : uam_unop(L->b, UAM_OP_ZEXT, w, v);
}
static uam_ref sext(a32_lifter *L, unsigned w, unsigned from, uam_ref v)
{
    if (uam_ref_is_imm(v))
        return uam_imm(L->b, (uint64_t)uam_sext(imm_val(L, v) & uam_mask(from), from) & uam_mask(w));
    return uam_unop(L->b, UAM_OP_SEXT, w, v);
}
static uam_ref trunc_(a32_lifter *L, unsigned w, uam_ref v)
{
    return uam_ref_is_imm(v) ? uam_imm(L->b, imm_val(L, v) & uam_mask(w)) : uam_unop(L->b, UAM_OP_TRUNC, w, v);
}
static uam_ref not32(a32_lifter *L, uam_ref v) { return bin(L, UAM_OP_XOR, v, imm(L, 0xFFFFFFFFu)); }
static uam_ref neg32(a32_lifter *L, uam_ref v) { return bin(L, UAM_OP_SUB, imm(L, 0), v); }
static uam_ref b1and(a32_lifter *L, uam_ref a, uam_ref b) { return binw(L, UAM_OP_AND, 1, a, b); }
static uam_ref b1or(a32_lifter *L, uam_ref a, uam_ref b) { return binw(L, UAM_OP_OR, 1, a, b); }
static uam_ref b1not(a32_lifter *L, uam_ref a) { return binw(L, UAM_OP_XOR, 1, a, imm(L, 1)); }
static uam_ref bit31(a32_lifter *L, uam_ref v) { return bin(L, UAM_OP_SLT, v, imm(L, 0)); }
/* bit n of v, as a boolean */
static uam_ref bitn(a32_lifter *L, uam_ref v, uam_ref n)
{
    return trunc_(L, 1, bin(L, UAM_OP_LSHR, v, n));
}
static uam_ref b2w(a32_lifter *L, uam_ref c) { return zext(L, 32, c); }

/* ---- registers and flags --------------------------------------------------- */

static uam_ref rd(a32_lifter *L, unsigned r)
{
    if (r == A32_PC)
        return imm(L, L->pc);
    return uam_get(L->b, 32, A32_REG_OFF(r));
}

static void wr(a32_lifter *L, unsigned r, uam_ref v, uam_ref guard)
{
    if (r == A32_PC) {
        L->jump = true;
        L->jtarget = v;
        L->jguard = guard;
        return;
    }
    uam_put(L->b, 32, A32_REG_OFF(r), v, guard);
}

static uam_ref flag(a32_lifter *L, uint32_t off) { return trunc_(L, 1, uam_get(L->b, 8, off)); }
static void setf(a32_lifter *L, uint32_t off, uam_ref c, uam_ref guard)
{
    uam_put(L->b, 8, off, zext(L, 8, c), guard);
}

/* N and Z from a 32-bit result. */
static void set_nz(a32_lifter *L, uam_ref res, uam_ref g)
{
    setf(L, A32_F_N, bit31(L, res), g);
    setf(L, A32_F_Z, bin(L, UAM_OP_EQ, res, imm(L, 0)), g);
}

/* C, V, N, Z of res = a + b (no carry in). */
static void flags_add(a32_lifter *L, uam_ref a, uam_ref b, uam_ref res, uam_ref g)
{
    setf(L, A32_F_C, bin(L, UAM_OP_CARRY, a, b), g);
    setf(L, A32_F_V, bin(L, UAM_OP_SCARRY, a, b), g);
    set_nz(L, res, g);
}

/* C (borrow), V, N, Z of res = a - b. */
static void flags_sub(a32_lifter *L, uam_ref a, uam_ref b, uam_ref res, uam_ref g)
{
    setf(L, A32_F_C, bin(L, UAM_OP_ULT, a, b), g);
    setf(L, A32_F_V, bin(L, UAM_OP_SBORROW, a, b), g);
    set_nz(L, res, g);
}

/* The manual's bit-31 formulas, for the forms with a carry in (res is the full
 * result). Z is sticky (Z <- (res == 0) && Z) when `chain`. */
static void flags_addc(a32_lifter *L, uam_ref op1, uam_ref op2, uam_ref res, bool chain)
{
    uam_ref nres = not32(L, res);
    uam_ref c = bin(L, UAM_OP_OR, bin(L, UAM_OP_AND, op1, op2),
                    bin(L, UAM_OP_OR, bin(L, UAM_OP_AND, op1, nres), bin(L, UAM_OP_AND, op2, nres)));
    uam_ref v = bin(L, UAM_OP_OR, bin(L, UAM_OP_AND, bin(L, UAM_OP_AND, op1, op2), nres),
                    bin(L, UAM_OP_AND, bin(L, UAM_OP_AND, not32(L, op1), not32(L, op2)), res));
    uam_ref z = bin(L, UAM_OP_EQ, res, imm(L, 0));
    if (chain)
        z = b1and(L, z, flag(L, A32_F_Z));
    setf(L, A32_F_C, bit31(L, c), NONE);
    setf(L, A32_F_V, bit31(L, v), NONE);
    setf(L, A32_F_N, bit31(L, res), NONE);
    setf(L, A32_F_Z, z, NONE);
}

static void flags_subc(a32_lifter *L, uam_ref op1, uam_ref op2, uam_ref res, bool chain)
{
    uam_ref nop1 = not32(L, op1);
    uam_ref c = bin(L, UAM_OP_OR, bin(L, UAM_OP_AND, nop1, op2),
                    bin(L, UAM_OP_OR, bin(L, UAM_OP_AND, op2, res), bin(L, UAM_OP_AND, nop1, res)));
    uam_ref v = bin(L, UAM_OP_OR, bin(L, UAM_OP_AND, bin(L, UAM_OP_AND, op1, not32(L, op2)), not32(L, res)),
                    bin(L, UAM_OP_AND, bin(L, UAM_OP_AND, nop1, op2), res));
    uam_ref z = bin(L, UAM_OP_EQ, res, imm(L, 0));
    if (chain)
        z = b1and(L, z, flag(L, A32_F_Z));
    setf(L, A32_F_C, bit31(L, c), NONE);
    setf(L, A32_F_V, bit31(L, v), NONE);
    setf(L, A32_F_N, bit31(L, res), NONE);
    setf(L, A32_F_Z, z, NONE);
}

/* cond4: eq ne cc/hs cs/lo ge lt mi pl ls gt le hi vs vc qs al. NONE for "al". */
static uam_ref cond(a32_lifter *L, unsigned c)
{
    uam_ref r;
    switch (c & 15) {
    case 0:  return flag(L, A32_F_Z);
    case 1:  return b1not(L, flag(L, A32_F_Z));
    case 2:  return b1not(L, flag(L, A32_F_C));
    case 3:  return flag(L, A32_F_C);
    case 4:  return binw(L, UAM_OP_EQ, 1, flag(L, A32_F_N), flag(L, A32_F_V));
    case 5:  return binw(L, UAM_OP_NE, 1, flag(L, A32_F_N), flag(L, A32_F_V));
    case 6:  return flag(L, A32_F_N);
    case 7:  return b1not(L, flag(L, A32_F_N));
    case 8:  return b1or(L, flag(L, A32_F_C), flag(L, A32_F_Z));
    case 9:
        r = binw(L, UAM_OP_EQ, 1, flag(L, A32_F_N), flag(L, A32_F_V));
        return b1and(L, r, b1not(L, flag(L, A32_F_Z)));
    case 10:
        r = binw(L, UAM_OP_NE, 1, flag(L, A32_F_N), flag(L, A32_F_V));
        return b1or(L, r, flag(L, A32_F_Z));
    case 11: return b1and(L, b1not(L, flag(L, A32_F_C)), b1not(L, flag(L, A32_F_Z)));
    case 12: return flag(L, A32_F_V);
    case 13: return b1not(L, flag(L, A32_F_V));
    case 14: return flag(L, A32_F_Q);
    default: return NONE;
    }
}

/* ---- memory ---------------------------------------------------------------- */

static unsigned accflags(unsigned w) { return BE | (w > 8 ? UAM_ACC_ALIGNED : 0); }

static uam_ref ld(a32_lifter *L, unsigned w, bool sgn, uam_ref addr, uam_ref guard)
{
    uam_ref v = uam_ld(L->b, w, accflags(w), addr, guard);
    if (w < 32)
        v = uam_unop(L->b, sgn ? UAM_OP_SEXT : UAM_OP_ZEXT, 32, v);
    return v;
}

static void st(a32_lifter *L, unsigned w, uam_ref addr, uam_ref v, uam_ref guard)
{
    if (w < 32)
        v = trunc_(L, w, v);
    uam_st(L->b, w, accflags(w), addr, v, guard);
}

/* ---- events ----------------------------------------------------------------- */

static lift_result trap(a32_lifter *L, uint32_t cause)
{
    uam_trap(L->b, NONE, UAM_TRAP_EXCEPTION, imm(L, cause), imm(L, L->pc));
    return LIFT_END;
}
static lift_result illegal(a32_lifter *L) { return trap(L, EV_ILLEGAL); }
static lift_result unimpl(a32_lifter *L) { return trap(L, EV_UNIMPL); }
static lift_result privileged(a32_lifter *L) { return trap(L, EV_PRIV); }

/* End the block after this instruction (it changed state later code depends on). */
static lift_result end_block(a32_lifter *L)
{
    uam_exit(L->b, NONE, imm(L, L->pc + L->len), UAM_EXIT_JUMP);
    return LIFT_END;
}

/* ---- field helpers -------------------------------------------------------- */

static uint32_t imm21(const a32_lifter *L)
{
    uint32_t v = F(L, 28, 25) << 17 | F(L, 20, 20) << 16 | F(L, 15, 0);
    return (uint32_t)uam_sext(v, 21);
}
static uint32_t sa5_compact(const a32_lifter *L) { return F(L, 28, 25) << 1 | F(L, 20, 20); }
static uint32_t se(uint32_t v, unsigned bits) { return (uint32_t)uam_sext(v, bits); }

/* A halfword of r, selected by a part bit (1: top), sign- or zero-extended. */
static uam_ref half(a32_lifter *L, uam_ref r, unsigned top, bool sgn)
{
    uam_ref h = top ? trunc_(L, 16, bin(L, UAM_OP_LSHR, r, imm(L, 16))) : trunc_(L, 16, r);
    return sgn ? sext(L, 32, 16, h) : zext(L, 32, h);
}

static uam_ref dsp_call(a32_lifter *L, unsigned op, uam_ref x, uam_ref y)
{
    return uam_helper(L->b, &uam_isa_avr32.helpers, A32_H_DSP, imm(L, op), x, y, NONE);
}

/* Q |= bit 32 of a helper result. */
static void q_or(a32_lifter *L, uam_ref h)
{
    uam_ref q = trunc_(L, 1, uam_binop(L->b, UAM_OP_LSHR, 64, h, uam_imm(L->b, 32)));
    setf(L, A32_F_Q, b1or(L, flag(L, A32_F_Q), q), NONE);
}
static uam_ref hbit(a32_lifter *L, uam_ref h, unsigned n)
{
    return trunc_(L, 1, uam_binop(L->b, UAM_OP_LSHR, 64, h, uam_imm(L->b, n)));
}
static uam_ref hres(a32_lifter *L, uam_ref h) { return trunc_(L, 32, h); }

/* Rd+1:Rd <- v (64-bit); Rd even. */
static void wr64(a32_lifter *L, unsigned r, uam_ref v)
{
    wr(L, r, trunc_(L, 32, v), NONE);
    wr(L, (r + 1) & 15, trunc_(L, 32, uam_binop(L->b, UAM_OP_LSHR, 64, v, uam_imm(L->b, 32))), NONE);
}
static uam_ref rd64(a32_lifter *L, unsigned r)
{
    uam_ref lo = zext(L, 64, rd(L, r)), hi = zext(L, 64, rd(L, (r + 1) & 15));
    return uam_binop(L->b, UAM_OP_OR, 64, lo, uam_binop(L->b, UAM_OP_SHL, 64, hi, uam_imm(L->b, 32)));
}

/* ---- load/store multiple ---------------------------------------------------- */

/* ldm / popm / ldm with Rp=PC: the registers of list16 (bit i = Ri) loaded in the
 * order PC, LR, SP, R12, R11..R0 from consecutive words at `addr`. `ret_k` is
 * the popm/ldm "return value" form (PC loaded with Rp == PC or k): R12 gets
 * 0, 1 or -1 from LR/R12 in the list and the flags test R12. */
static lift_result load_multiple(a32_lifter *L, uam_ref base, uint32_t list, bool update,
                                 unsigned upd_reg, bool ret_k)
{
    static const unsigned order[16] = { 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0 };
    uam_ref val[16];
    unsigned n = 0;
    bool pc = list & (1u << 15);
    for (unsigned k = 0; k < 16; k++) {
        unsigned r = order[k];
        if (!(list & (1u << r)))
            continue;
        if (ret_k && (r == 14 || r == 13 || r == 12))
            continue;   /* not loaded: LR and R12 select R12's value instead */
        val[r] = ld(L, 32, false, bin(L, UAM_OP_ADD, base, imm(L, 4 * n)), NONE);
        n++;
    }
    if (update)
        wr(L, upd_reg, bin(L, UAM_OP_ADD, base, imm(L, 4 * n)), NONE);
    for (unsigned k = 0; k < 16; k++) {
        unsigned r = order[k];
        if (!(list & (1u << r)) || r == 15)
            continue;
        if (ret_k && (r == 14 || r == 13 || r == 12))
            continue;
        wr(L, r, val[r], NONE);
    }
    if (ret_k) {
        uint32_t v = (list & (1u << 14)) ? 0xFFFFFFFFu : (list & (1u << 12)) ? 1 : 0;
        wr(L, 12, imm(L, v), NONE);
    }
    if (pc) {
        uam_ref r12 = ret_k ? imm(L, (list & (1u << 14)) ? 0xFFFFFFFFu : (list & (1u << 12)) ? 1 : 0)
                            : (list & (1u << 12)) ? val[12] : rd(L, 12);
        setf(L, A32_F_V, imm(L, 0), NONE);
        setf(L, A32_F_C, imm(L, 0), NONE);
        set_nz(L, r12, NONE);
        L->jump = true;
        L->jtarget = val[15];
        L->jguard = NONE;
        L->jkind = UAM_EXIT_RET;
    }
    return LIFT_CONTINUE;
}

/* stm / pushm: the registers of list16 stored at consecutive words. Decrementing:
 * R0 first at the highest address; incrementing: R15 (PC) first. */
static void store_multiple(a32_lifter *L, unsigned rp, uint32_t list, bool predec)
{
    uam_ref base = rd(L, rp);
    unsigned n = (unsigned)__builtin_popcount(list);
    if (predec) {
        uam_ref lowest = bin(L, UAM_OP_SUB, base, imm(L, 4 * n));
        unsigned k = 0;
        for (int r = 15; r >= 0; r--)   /* the lowest address holds the highest register */
            if (list & (1u << r))
                st(L, 32, bin(L, UAM_OP_ADD, lowest, imm(L, 4 * k++)), rd(L, (unsigned)r), NONE);
        wr(L, rp, lowest, NONE);
    } else {
        unsigned k = 0;
        for (int r = 15; r >= 0; r--)
            if (list & (1u << r))
                st(L, 32, bin(L, UAM_OP_ADD, base, imm(L, 4 * k++)), rd(L, (unsigned)r), NONE);
    }
}

/* The compact pushm/popm register list (Reglist8) as Reglist16. */
static uint32_t list8(uint32_t r8)
{
    uint32_t l = 0;
    if (r8 & 0x01) l |= 0x000F;   /* R0-R3 */
    if (r8 & 0x02) l |= 0x00F0;   /* R4-R7 */
    if (r8 & 0x04) l |= 0x0300;   /* R8-R9 */
    if (r8 & 0x08) l |= 1u << 10;
    if (r8 & 0x10) l |= 1u << 11;
    if (r8 & 0x20) l |= 1u << 12;
    if (r8 & 0x40) l |= 1u << 14;
    if (r8 & 0x80) l |= 1u << 15;
    return l;
}

/* ---- shifts ----------------------------------------------------------------- */

enum { SH_LSL, SH_LSR, SH_ASR };

/* Rd <- shift(op, sh) with sh in 0..31; N, Z, and C the last bit shifted out (0 if sh == 0). */
static void shift(a32_lifter *L, int kind, unsigned rdst, uam_ref op, uam_ref sh)
{
    uam_opcode o = kind == SH_LSL ? UAM_OP_SHL : kind == SH_LSR ? UAM_OP_LSHR : UAM_OP_ASHR;
    uam_ref res = bin(L, o, op, sh);
    uam_ref nz = bin(L, UAM_OP_NE, sh, imm(L, 0));
    uam_ref cbit = kind == SH_LSL ? bitn(L, op, bin(L, UAM_OP_SUB, imm(L, 32), sh))
                                  : bitn(L, op, bin(L, UAM_OP_SUB, sh, imm(L, 1)));
    /* for sh == 0 the amounts above are 32 / -1: select 0 */
    uam_ref c = b1and(L, nz, cbit);
    wr(L, rdst, res, NONE);
    set_nz(L, res, NONE);
    setf(L, A32_F_C, c, NONE);
}

/* ---- the instructions ------------------------------------------------------ */

static lift_result lift_one(a32_lifter *L, unsigned op)
{
    const unsigned RD = F(L, 19, 16);   /* the compact Rd, and extended Ry */
    const unsigned RS = F(L, 28, 25);   /* the compact Rs/Rp, and extended Rx */
    const unsigned R3 = F(L, 3, 0);     /* the extended Rd */
    uam_ref a, b, res, g, addr, t;
    unsigned w;
    bool sgn;

    switch (op) {
    /* ---- arithmetic, compact two-register forms ---- */
    case A32_ADD_RR_16:
        a = rd(L, RD); b = rd(L, RS); res = bin(L, UAM_OP_ADD, a, b);
        wr(L, RD, res, NONE); flags_add(L, a, b, res, NONE);
        return LIFT_CONTINUE;
    case A32_SUB_RR_16:
        a = rd(L, RD); b = rd(L, RS); res = bin(L, UAM_OP_SUB, a, b);
        wr(L, RD, res, NONE); flags_sub(L, a, b, res, NONE);
        return LIFT_CONTINUE;
    case A32_RSUB_RR_16:
        a = rd(L, RS); b = rd(L, RD); res = bin(L, UAM_OP_SUB, a, b);
        wr(L, RD, res, NONE); flags_sub(L, a, b, res, NONE);
        return LIFT_CONTINUE;
    case A32_CP_W_RR_16:
        a = rd(L, RD); b = rd(L, RS); flags_sub(L, a, b, bin(L, UAM_OP_SUB, a, b), NONE);
        return LIFT_CONTINUE;
    case A32_AND_RR_16: case A32_OR_RR_16: case A32_EOR_RR_16: case A32_ANDN_RR_16: case A32_TST_RR_16: {
        b = rd(L, RS);
        uam_opcode o = op == A32_OR_RR_16 ? UAM_OP_OR : op == A32_EOR_RR_16 ? UAM_OP_XOR : UAM_OP_AND;
        if (op == A32_ANDN_RR_16)
            b = not32(L, b);
        res = bin(L, o, rd(L, RD), b);
        if (op != A32_TST_RR_16)
            wr(L, RD, res, NONE);
        set_nz(L, res, NONE);
        return LIFT_CONTINUE;
    }
    case A32_MOV_RR_16:
        wr(L, RD, rd(L, RS), NONE);
        if (RD == A32_PC && RS == A32_LR)
            L->jkind = UAM_EXIT_RET;
        return LIFT_CONTINUE;
    case A32_MUL_RR_16:
        wr(L, RD, bin(L, UAM_OP_MUL, rd(L, RD), rd(L, RS)), NONE);
        return LIFT_CONTINUE;

    /* ---- arithmetic, single register ---- */
    case A32_ABS_R_16:
        a = rd(L, RD);
        res = sel(L, bit31(L, a), neg32(L, a), a);
        wr(L, RD, res, NONE);
        setf(L, A32_F_Z, bin(L, UAM_OP_EQ, res, imm(L, 0)), NONE);
        return LIFT_CONTINUE;
    case A32_ACR_R_16: case A32_SCR_R_16: case A32_CPC_R_16: {
        a = rd(L, RD);
        b = b2w(L, flag(L, A32_F_C));
        bool add = op == A32_ACR_R_16;
        res = bin(L, add ? UAM_OP_ADD : UAM_OP_SUB, a, b);
        if (op != A32_CPC_R_16)
            wr(L, RD, res, NONE);
        if (add)
            flags_addc(L, a, imm(L, 0), res, true);
        else
            flags_subc(L, a, imm(L, 0), res, true);
        return LIFT_CONTINUE;
    }
    case A32_NEG_R_16:
        a = rd(L, RD); res = neg32(L, a);
        wr(L, RD, res, NONE);
        flags_sub(L, imm(L, 0), a, res, NONE);
        return LIFT_CONTINUE;
    case A32_COM_R_16:
        res = not32(L, rd(L, RD)); wr(L, RD, res, NONE);
        setf(L, A32_F_Z, bin(L, UAM_OP_EQ, res, imm(L, 0)), NONE);
        return LIFT_CONTINUE;
    case A32_TNBZ_R_16: {
        a = rd(L, RD);
        uam_ref z = NONE;
        for (unsigned k = 0; k < 4; k++) {
            uam_ref byte = trunc_(L, 8, bin(L, UAM_OP_LSHR, a, imm(L, 8 * k)));
            uam_ref e = binw(L, UAM_OP_EQ, 8, byte, uam_imm(L->b, 0));
            z = k ? b1or(L, z, e) : e;
        }
        setf(L, A32_F_Z, z, NONE);
        return LIFT_CONTINUE;
    }
    case A32_CASTS_B_R_16: case A32_CASTS_H_R_16: case A32_CASTU_B_R_16: case A32_CASTU_H_R_16: {
        w = (op == A32_CASTS_B_R_16 || op == A32_CASTU_B_R_16) ? 8 : 16;
        sgn = op == A32_CASTS_B_R_16 || op == A32_CASTS_H_R_16;
        t = trunc_(L, w, rd(L, RD));
        res = sgn ? sext(L, 32, w, t) : zext(L, 32, t);
        wr(L, RD, res, NONE);
        set_nz(L, res, NONE);
        setf(L, A32_F_C, bit31(L, res), NONE);
        return LIFT_CONTINUE;
    }
    case A32_SWAP_B_R_16:
        wr(L, RD, uam_unop(L->b, UAM_OP_BSWAP, 32, rd(L, RD)), NONE);
        return LIFT_CONTINUE;
    case A32_SWAP_BH_R_16:
        a = rd(L, RD);
        wr(L, RD, bin(L, UAM_OP_OR, bin(L, UAM_OP_AND, bin(L, UAM_OP_SHL, a, imm(L, 8)), imm(L, 0xFF00FF00u)),
                      bin(L, UAM_OP_AND, bin(L, UAM_OP_LSHR, a, imm(L, 8)), imm(L, 0x00FF00FFu))), NONE);
        return LIFT_CONTINUE;
    case A32_SWAP_H_R_16:
        wr(L, RD, bin(L, UAM_OP_ROTR, rd(L, RD), imm(L, 16)), NONE);
        return LIFT_CONTINUE;
    case A32_BREV_R_16: {
        a = rd(L, RD);
        static const uint32_t m[5] = { 0x55555555u, 0x33333333u, 0x0F0F0F0Fu, 0x00FF00FFu, 0x0000FFFFu };
        for (unsigned k = 0; k < 5; k++) {
            uint32_t s = 1u << k;
            a = bin(L, UAM_OP_OR, bin(L, UAM_OP_AND, bin(L, UAM_OP_LSHR, a, imm(L, s)), imm(L, m[k])),
                    bin(L, UAM_OP_SHL, bin(L, UAM_OP_AND, a, imm(L, m[k])), imm(L, s)));
        }
        wr(L, RD, a, NONE);
        setf(L, A32_F_Z, bin(L, UAM_OP_EQ, a, imm(L, 0)), NONE);
        return LIFT_CONTINUE;
    }
    case A32_ROL_R_16: case A32_ROR_R_16: {
        a = rd(L, RD);
        uam_ref c = b2w(L, flag(L, A32_F_C));
        uam_ref out;
        if (op == A32_ROL_R_16) {
            res = bin(L, UAM_OP_OR, bin(L, UAM_OP_SHL, a, imm(L, 1)), c);
            out = bit31(L, a);
        } else {
            res = bin(L, UAM_OP_OR, bin(L, UAM_OP_LSHR, a, imm(L, 1)), bin(L, UAM_OP_SHL, c, imm(L, 31)));
            out = trunc_(L, 1, a);
        }
        wr(L, RD, res, NONE);
        set_nz(L, res, NONE);
        setf(L, A32_F_C, out, NONE);
        return LIFT_CONTINUE;
    }

    /* ---- arithmetic, extended ---- */
    case A32_ADD_RRR_32: case A32_SUB_RRR_32:
        a = rd(L, RS);
        b = bin(L, UAM_OP_SHL, rd(L, RD), imm(L, F(L, 5, 4)));
        res = bin(L, op == A32_ADD_RRR_32 ? UAM_OP_ADD : UAM_OP_SUB, a, b);
        wr(L, R3, res, NONE);
        if (op == A32_ADD_RRR_32)
            flags_add(L, a, b, res, NONE);
        else
            flags_sub(L, a, b, res, NONE);
        return LIFT_CONTINUE;
    case A32_ADC_RRR_32: case A32_SBC_RRR_32:
        a = rd(L, RS); b = rd(L, RD);
        if (op == A32_ADC_RRR_32) {
            res = bin(L, UAM_OP_ADD, bin(L, UAM_OP_ADD, a, b), b2w(L, flag(L, A32_F_C)));
            wr(L, R3, res, NONE);
            flags_addc(L, a, b, res, true);
        } else {
            res = bin(L, UAM_OP_SUB, bin(L, UAM_OP_SUB, a, b), b2w(L, flag(L, A32_F_C)));
            wr(L, R3, res, NONE);
            flags_subc(L, a, b, res, true);
        }
        return LIFT_CONTINUE;
    case A32_CPC_RR_32:
        a = rd(L, RD); b = rd(L, RS);
        res = bin(L, UAM_OP_SUB, bin(L, UAM_OP_SUB, a, b), b2w(L, flag(L, A32_F_C)));
        flags_subc(L, a, b, res, true);
        return LIFT_CONTINUE;
    case A32_ADDABS_RRR_32:
        b = rd(L, RD);
        res = bin(L, UAM_OP_ADD, rd(L, RS), sel(L, bit31(L, b), neg32(L, b), b));
        wr(L, R3, res, NONE);
        setf(L, A32_F_Z, bin(L, UAM_OP_EQ, res, imm(L, 0)), NONE);
        return LIFT_CONTINUE;
    case A32_CP_B_RR_32: case A32_CP_H_RR_32: {
        w = op == A32_CP_B_RR_32 ? 8 : 16;
        a = trunc_(L, w, rd(L, RD)); b = trunc_(L, w, rd(L, RS));
        res = binw(L, UAM_OP_SUB, w, a, b);
        setf(L, A32_F_C, binw(L, UAM_OP_ULT, w, a, b), NONE);
        setf(L, A32_F_V, binw(L, UAM_OP_SBORROW, w, a, b), NONE);
        setf(L, A32_F_N, binw(L, UAM_OP_SLT, w, res, uam_imm(L->b, 0)), NONE);
        setf(L, A32_F_Z, binw(L, UAM_OP_EQ, w, res, uam_imm(L->b, 0)), NONE);
        return LIFT_CONTINUE;
    }
    case A32_MAX_RRR_32: case A32_MIN_RRR_32:
        a = rd(L, RS); b = rd(L, RD);
        wr(L, R3, op == A32_MAX_RRR_32 ? sel(L, bin(L, UAM_OP_SLT, b, a), a, b)
                                       : sel(L, bin(L, UAM_OP_SLT, a, b), a, b), NONE);
        return LIFT_CONTINUE;
    case A32_MUL_RRR_32:
        wr(L, R3, bin(L, UAM_OP_MUL, rd(L, RS), rd(L, RD)), NONE);
        return LIFT_CONTINUE;
    case A32_MAC_RRR_32:
        wr(L, R3, bin(L, UAM_OP_ADD, bin(L, UAM_OP_MUL, rd(L, RS), rd(L, RD)), rd(L, R3)), NONE);
        return LIFT_CONTINUE;
    case A32_MUL_RRI_32:
        wr(L, RD, bin(L, UAM_OP_MUL, rd(L, RS), imm(L, se(F(L, 7, 0), 8))), NONE);
        return LIFT_CONTINUE;
    case A32_DIVS_RRR_32: case A32_DIVU_RRR_32: {
        bool s = op == A32_DIVS_RRR_32;
        a = rd(L, RS); b = rd(L, RD);
        uam_ref q = bin(L, s ? UAM_OP_SDIV : UAM_OP_UDIV, a, b);
        uam_ref r = bin(L, s ? UAM_OP_SREM : UAM_OP_UREM, a, b);
        wr(L, R3, q, NONE);
        wr(L, (R3 + 1) & 15, r, NONE);
        return LIFT_CONTINUE;
    }
    case A32_MULS_D_RRR_32: case A32_MULU_D_RRR_32: case A32_MACS_D_RRR_32: case A32_MACU_D_RRR_32: {
        bool s = op == A32_MULS_D_RRR_32 || op == A32_MACS_D_RRR_32;
        uam_ref x = uam_unop(L->b, s ? UAM_OP_SEXT : UAM_OP_ZEXT, 64, rd(L, RS));
        uam_ref y = uam_unop(L->b, s ? UAM_OP_SEXT : UAM_OP_ZEXT, 64, rd(L, RD));
        res = uam_binop(L->b, UAM_OP_MUL, 64, x, y);
        if (op == A32_MACS_D_RRR_32 || op == A32_MACU_D_RRR_32)
            res = uam_binop(L->b, UAM_OP_ADD, 64, res, rd64(L, R3 & ~1u));
        wr64(L, R3 & ~1u, res);
        return LIFT_CONTINUE;
    }
    case A32_CLZ_RR_32:
        res = uam_unop(L->b, UAM_OP_CLZ, 32, rd(L, RS));
        wr(L, RD, res, NONE);
        setf(L, A32_F_Z, bin(L, UAM_OP_EQ, res, imm(L, 0)), NONE);
        setf(L, A32_F_C, bin(L, UAM_OP_EQ, res, imm(L, 32)), NONE);
        return LIFT_CONTINUE;

    /* ---- immediates ---- */
    case A32_MOV_RI_16:
        wr(L, RD, imm(L, se(F(L, 27, 20), 8)), NONE);
        return LIFT_CONTINUE;
    case A32_MOV_RI_32:
        wr(L, RD, imm(L, imm21(L)), NONE);
        return LIFT_CONTINUE;
    case A32_MOVH_RI_32:
        wr(L, RD, imm(L, F(L, 15, 0) << 16), NONE);
        return LIFT_CONTINUE;
    case A32_SUB_RI_16: case A32_SUB_RI_32: case A32_SUB_RRI_32: {
        uint32_t k;
        unsigned src = RD;
        if (op == A32_SUB_RI_16)
            k = RD == A32_SP ? se(F(L, 27, 20), 8) << 2 : se(F(L, 27, 20), 8);
        else if (op == A32_SUB_RI_32)
            k = imm21(L);
        else {
            k = se(F(L, 15, 0), 16);
            src = RS;
        }
        a = rd(L, src); b = imm(L, k);
        res = bin(L, UAM_OP_SUB, a, b);
        wr(L, RD, res, NONE);
        flags_sub(L, a, b, res, NONE);
        return LIFT_CONTINUE;
    }
    case A32_CP_W_RI_16: case A32_CP_W_RI_32:
        a = rd(L, RD);
        b = imm(L, op == A32_CP_W_RI_16 ? se(F(L, 25, 20), 6) : imm21(L));
        flags_sub(L, a, b, bin(L, UAM_OP_SUB, a, b), NONE);
        return LIFT_CONTINUE;
    case A32_RSUB_RRI_32:
        a = imm(L, se(F(L, 7, 0), 8)); b = rd(L, RS);
        res = bin(L, UAM_OP_SUB, a, b);
        wr(L, RD, res, NONE);
        flags_sub(L, a, b, res, NONE);
        return LIFT_CONTINUE;
    case A32_ANDH_RI_32: case A32_ANDH_RICOH_32: case A32_ANDL_RI_32: case A32_ANDL_RICOH_32:
    case A32_ORH_RI_32: case A32_ORL_RI_32: case A32_EORH_RI_32: case A32_EORL_RI_32: {
        bool high = op == A32_ANDH_RI_32 || op == A32_ANDH_RICOH_32 || op == A32_ORH_RI_32 || op == A32_EORH_RI_32;
        uint32_t k = F(L, 15, 0);
        a = rd(L, RD);
        if (op == A32_ANDH_RI_32 || op == A32_ANDL_RI_32)
            res = bin(L, UAM_OP_AND, a, imm(L, high ? (k << 16) | 0xFFFF : k | 0xFFFF0000u));
        else if (op == A32_ANDH_RICOH_32 || op == A32_ANDL_RICOH_32)
            res = bin(L, UAM_OP_AND, a, imm(L, high ? k << 16 : k));
        else
            res = bin(L, (op == A32_ORH_RI_32 || op == A32_ORL_RI_32) ? UAM_OP_OR : UAM_OP_XOR, a,
                      imm(L, high ? k << 16 : k));
        wr(L, RD, res, NONE);
        set_nz(L, res, NONE);
        return LIFT_CONTINUE;
    }

    /* ---- logic with shifted operand ---- */
    case A32_AND_LSL_32: case A32_AND_LSR_32: case A32_OR_LSL_32: case A32_OR_LSR_32:
    case A32_EOR_LSL_32: case A32_EOR_LSR_32: {
        bool left = op == A32_AND_LSL_32 || op == A32_OR_LSL_32 || op == A32_EOR_LSL_32;
        uam_opcode o = (op == A32_AND_LSL_32 || op == A32_AND_LSR_32) ? UAM_OP_AND
                     : (op == A32_OR_LSL_32 || op == A32_OR_LSR_32) ? UAM_OP_OR : UAM_OP_XOR;
        b = bin(L, left ? UAM_OP_SHL : UAM_OP_LSHR, rd(L, RD), imm(L, F(L, 8, 4)));
        res = bin(L, o, rd(L, RS), b);
        wr(L, R3, res, NONE);
        set_nz(L, res, NONE);
        return LIFT_CONTINUE;
    }

    /* ---- conditional (revision 2 and the cond forms) ---- */
    case A32_MOVC_RR_32:
        wr(L, RD, rd(L, RS), cond(L, F(L, 7, 4)));
        return LIFT_CONTINUE;
    case A32_MOVC_RI_32:
        wr(L, RD, imm(L, se(F(L, 7, 0), 8)), cond(L, F(L, 11, 8)));
        return LIFT_CONTINUE;
    case A32_SUBC_RI_32: case A32_SUBFC_RI_32: case A32_RSUBC_RI_32:
        g = cond(L, F(L, 11, 8));
        if (op == A32_RSUBC_RI_32) {
            a = imm(L, se(F(L, 7, 0), 8)); b = rd(L, RD);
        } else {
            a = rd(L, RD); b = imm(L, se(F(L, 7, 0), 8));
        }
        res = bin(L, UAM_OP_SUB, a, b);
        wr(L, RD, res, g);
        if (op == A32_SUBFC_RI_32)
            flags_sub(L, a, b, res, g);
        return LIFT_CONTINUE;
    case A32_ADDC_RRR_32: case A32_SUBC_RRR_32: case A32_ANDC_RRR_32: case A32_ORC_RRR_32:
    case A32_EORC_RRR_32: {
        static const uam_opcode o[] = { UAM_OP_ADD, UAM_OP_SUB, UAM_OP_AND, UAM_OP_OR, UAM_OP_XOR };
        unsigned k = op == A32_ADDC_RRR_32 ? 0 : op == A32_SUBC_RRR_32 ? 1 : op == A32_ANDC_RRR_32 ? 2
                   : op == A32_ORC_RRR_32 ? 3 : 4;
        wr(L, R3, bin(L, o[k], rd(L, RS), rd(L, RD)), cond(L, F(L, 11, 8)));
        return LIFT_CONTINUE;
    }
    case A32_SRC_R_16: {
        g = cond(L, F(L, 23, 20));
        wr(L, RD, g == NONE ? imm(L, 1) : b2w(L, g), NONE);
        return LIFT_CONTINUE;
    }
    case A32_RETC_R_16: {
        g = cond(L, F(L, 23, 20));
        uam_ref v = RD == A32_LR ? imm(L, 0xFFFFFFFFu) : RD == A32_SP ? imm(L, 0)
                  : RD == A32_PC ? imm(L, 1) : rd(L, RD);
        uam_ref lr = rd(L, A32_LR);
        wr(L, 12, v, g);
        setf(L, A32_F_V, imm(L, 0), g);
        setf(L, A32_F_C, imm(L, 0), g);
        set_nz(L, v, g);
        L->jump = true;
        L->jtarget = lr;
        L->jguard = g;
        L->jkind = UAM_EXIT_RET;
        return LIFT_CONTINUE;
    }

    /* ---- shifts ---- */
    case A32_ASR_RRR_32: case A32_LSL_RRR_32: case A32_LSR_RRR_32:
        shift(L, op == A32_LSL_RRR_32 ? SH_LSL : op == A32_LSR_RRR_32 ? SH_LSR : SH_ASR, R3, rd(L, RS),
              bin(L, UAM_OP_AND, rd(L, RD), imm(L, 31)));
        return LIFT_CONTINUE;
    case A32_ASR_RRI_32: case A32_LSL_RRI_32: case A32_LSR_RRI_32:
        shift(L, op == A32_LSL_RRI_32 ? SH_LSL : op == A32_LSR_RRI_32 ? SH_LSR : SH_ASR, RD, rd(L, RS),
              imm(L, F(L, 4, 0)));
        return LIFT_CONTINUE;
    case A32_ASR_RI_16: case A32_LSL_RI_16: case A32_LSR_RI_16:
        shift(L, op == A32_LSL_RI_16 ? SH_LSL : op == A32_LSR_RI_16 ? SH_LSR : SH_ASR, RD, rd(L, RD),
              imm(L, sa5_compact(L)));
        return LIFT_CONTINUE;

    /* ---- bits ---- */
    case A32_SBR_RI_16: case A32_CBR_RI_16: {
        uint32_t m = 1u << sa5_compact(L);
        a = rd(L, RD);
        if (op == A32_SBR_RI_16) {
            res = bin(L, UAM_OP_OR, a, imm(L, m));
            setf(L, A32_F_Z, imm(L, 0), NONE);
        } else {
            res = bin(L, UAM_OP_AND, a, imm(L, ~m));
            setf(L, A32_F_Z, bin(L, UAM_OP_EQ, res, imm(L, 0)), NONE);
        }
        wr(L, RD, res, NONE);
        return LIFT_CONTINUE;
    }
    case A32_BLD_RI_32:
        t = bitn(L, rd(L, RD), imm(L, F(L, 4, 0)));
        setf(L, A32_F_C, t, NONE);
        setf(L, A32_F_Z, t, NONE);
        return LIFT_CONTINUE;
    case A32_BST_RI_32: {
        uint32_t m = 1u << F(L, 4, 0);
        a = bin(L, UAM_OP_AND, rd(L, RD), imm(L, ~m));
        wr(L, RD, bin(L, UAM_OP_OR, a, bin(L, UAM_OP_SHL, b2w(L, flag(L, A32_F_C)), imm(L, F(L, 4, 0)))), NONE);
        return LIFT_CONTINUE;
    }
    case A32_BFEXTS_RRII_32: case A32_BFEXTU_RRII_32: case A32_BFINS_RRII_32: {
        unsigned bp = F(L, 9, 5), wd = F(L, 4, 0);
        unsigned dst = RS, srcr = RD;   /* Rd at bits 28:25, Rs at 19:16 */
        uint64_t fm = wd ? ((UINT64_C(1) << wd) - 1) : 0;
        if (op == A32_BFINS_RRII_32) {
            uint32_t m = (uint32_t)(fm << bp);
            res = bin(L, UAM_OP_OR, bin(L, UAM_OP_AND, rd(L, dst), imm(L, ~m)),
                      bin(L, UAM_OP_AND, bin(L, UAM_OP_SHL, rd(L, srcr), imm(L, bp)), imm(L, m)));
        } else {
            /* the field, moved to the top, then shifted down (arithmetic for bfexts) */
            unsigned up = 32 - (bp + wd) > 32 ? 0 : 32 - (bp + wd);
            if (wd == 0 || bp + wd > 32) {
                res = imm(L, 0);
                if (wd && bp + wd > 32)   /* the field runs off the top: what is there */
                    res = bin(L, op == A32_BFEXTS_RRII_32 ? UAM_OP_ASHR : UAM_OP_LSHR, rd(L, srcr), imm(L, bp));
            } else {
                res = bin(L, op == A32_BFEXTS_RRII_32 ? UAM_OP_ASHR : UAM_OP_LSHR,
                          bin(L, UAM_OP_SHL, rd(L, srcr), imm(L, up)), imm(L, 32 - wd));
            }
        }
        wr(L, dst, res, NONE);
        set_nz(L, res, NONE);
        setf(L, A32_F_C, bit31(L, res), NONE);
        return LIFT_CONTINUE;
    }

    /* ---- loads and stores ---- */
    case A32_LD_W_POSTINC_16: case A32_LD_SH_POSTINC_16: case A32_LD_UH_POSTINC_16: case A32_LD_UB_POSTINC_16:
    case A32_LD_W_PREDEC_16: case A32_LD_SH_PREDEC_16: case A32_LD_UH_PREDEC_16: case A32_LD_UB_PREDEC_16: {
        bool post = op == A32_LD_W_POSTINC_16 || op == A32_LD_SH_POSTINC_16 || op == A32_LD_UH_POSTINC_16
                 || op == A32_LD_UB_POSTINC_16;
        w = (op == A32_LD_W_POSTINC_16 || op == A32_LD_W_PREDEC_16) ? 32
          : (op == A32_LD_UB_POSTINC_16 || op == A32_LD_UB_PREDEC_16) ? 8 : 16;
        sgn = op == A32_LD_SH_POSTINC_16 || op == A32_LD_SH_PREDEC_16;
        a = rd(L, RS);
        uam_ref na = post ? bin(L, UAM_OP_ADD, a, imm(L, w / 8)) : bin(L, UAM_OP_SUB, a, imm(L, w / 8));
        t = ld(L, w, sgn, post ? a : na, NONE);
        wr(L, RS, na, NONE);
        wr(L, RD, t, NONE);
        if (RD == A32_PC)
            L->jkind = RS == A32_SP ? UAM_EXIT_RET : UAM_EXIT_JUMP;
        return LIFT_CONTINUE;
    }
    case A32_LD_W_DISP_16: case A32_LD_SH_DISP_16: case A32_LD_UH_DISP_16: case A32_LD_UB_DISP_16: {
        uint32_t d;
        if (op == A32_LD_W_DISP_16) { w = 32; d = F(L, 24, 20) << 2; }
        else if (op == A32_LD_UB_DISP_16) { w = 8; d = F(L, 22, 20); }
        else { w = 16; d = F(L, 22, 20) << 1; }
        t = ld(L, w, op == A32_LD_SH_DISP_16, bin(L, UAM_OP_ADD, rd(L, RS), imm(L, d)), NONE);
        wr(L, RD, t, NONE);
        return LIFT_CONTINUE;
    }
    case A32_LD_W_DISP_32: case A32_LD_SH_DISP_32: case A32_LD_UH_DISP_32: case A32_LD_SB_DISP_32:
    case A32_LD_UB_DISP_32:
        w = op == A32_LD_W_DISP_32 ? 32 : (op == A32_LD_SB_DISP_32 || op == A32_LD_UB_DISP_32) ? 8 : 16;
        sgn = op == A32_LD_SH_DISP_32 || op == A32_LD_SB_DISP_32;
        t = ld(L, w, sgn, bin(L, UAM_OP_ADD, rd(L, RS), imm(L, se(F(L, 15, 0), 16))), NONE);
        wr(L, RD, t, NONE);
        return LIFT_CONTINUE;
    case A32_LD_W_IDX_32: case A32_LD_SH_IDX_32: case A32_LD_UH_IDX_32: case A32_LD_SB_IDX_32:
    case A32_LD_UB_IDX_32:
        w = op == A32_LD_W_IDX_32 ? 32 : (op == A32_LD_SB_IDX_32 || op == A32_LD_UB_IDX_32) ? 8 : 16;
        sgn = op == A32_LD_SH_IDX_32 || op == A32_LD_SB_IDX_32;
        addr = bin(L, UAM_OP_ADD, rd(L, RS), bin(L, UAM_OP_SHL, rd(L, RD), imm(L, F(L, 5, 4))));
        wr(L, R3, ld(L, w, sgn, addr, NONE), NONE);
        return LIFT_CONTINUE;
    case A32_LD_W_RRRSHI_32: {
        uam_ref idx = bin(L, UAM_OP_AND, bin(L, UAM_OP_LSHR, rd(L, RD), imm(L, 8 * F(L, 5, 4))), imm(L, 0xFF));
        addr = bin(L, UAM_OP_ADD, rd(L, RS), bin(L, UAM_OP_SHL, idx, imm(L, 2)));
        wr(L, R3, ld(L, 32, false, addr, NONE), NONE);
        return LIFT_CONTINUE;
    }
    case A32_LDDPC_RI_16:
        t = ld(L, 32, false, imm(L, (L->pc & ~3u) + (F(L, 26, 20) << 2)), NONE);
        wr(L, RD, t, NONE);
        return LIFT_CONTINUE;
    case A32_LDDSP_DISP_16:
        addr = bin(L, UAM_OP_ADD, bin(L, UAM_OP_AND, rd(L, A32_SP), imm(L, ~3u)), imm(L, F(L, 26, 20) << 2));
        wr(L, RD, ld(L, 32, false, addr, NONE), NONE);
        return LIFT_CONTINUE;
    case A32_STDSP_DISP_16:
        addr = bin(L, UAM_OP_ADD, bin(L, UAM_OP_AND, rd(L, A32_SP), imm(L, ~3u)), imm(L, F(L, 26, 20) << 2));
        st(L, 32, addr, rd(L, RD), NONE);
        return LIFT_CONTINUE;
    case A32_ST_W_POSTINC_16: case A32_ST_H_POSTINC_16: case A32_ST_B_POSTINC_16:
    case A32_ST_W_PREDEC_16: case A32_ST_H_PREDEC_16: case A32_ST_B_PREDEC_16: {
        bool post = op == A32_ST_W_POSTINC_16 || op == A32_ST_H_POSTINC_16 || op == A32_ST_B_POSTINC_16;
        w = (op == A32_ST_W_POSTINC_16 || op == A32_ST_W_PREDEC_16) ? 32
          : (op == A32_ST_H_POSTINC_16 || op == A32_ST_H_PREDEC_16) ? 16 : 8;
        a = rd(L, RS);
        uam_ref na = post ? bin(L, UAM_OP_ADD, a, imm(L, w / 8)) : bin(L, UAM_OP_SUB, a, imm(L, w / 8));
        st(L, w, post ? a : na, rd(L, RD), NONE);
        wr(L, RS, na, NONE);
        return LIFT_CONTINUE;
    }
    case A32_ST_W_DISP_16: case A32_ST_H_DISP_16: case A32_ST_B_DISP_16: {
        uint32_t d;
        if (op == A32_ST_W_DISP_16) { w = 32; d = F(L, 23, 20) << 2; }
        else if (op == A32_ST_H_DISP_16) { w = 16; d = F(L, 22, 20) << 1; }
        else { w = 8; d = F(L, 22, 20); }
        st(L, w, bin(L, UAM_OP_ADD, rd(L, RS), imm(L, d)), rd(L, RD), NONE);
        return LIFT_CONTINUE;
    }
    case A32_ST_W_DISP_32: case A32_ST_H_DISP_32: case A32_ST_B_DISP_32:
        w = op == A32_ST_W_DISP_32 ? 32 : op == A32_ST_H_DISP_32 ? 16 : 8;
        st(L, w, bin(L, UAM_OP_ADD, rd(L, RS), imm(L, se(F(L, 15, 0), 16))), rd(L, RD), NONE);
        return LIFT_CONTINUE;
    case A32_ST_W_IDX_32: case A32_ST_H_IDX_32: case A32_ST_B_IDX_32:
        w = op == A32_ST_W_IDX_32 ? 32 : op == A32_ST_H_IDX_32 ? 16 : 8;
        addr = bin(L, UAM_OP_ADD, rd(L, RS), bin(L, UAM_OP_SHL, rd(L, RD), imm(L, F(L, 5, 4))));
        st(L, w, addr, rd(L, R3), NONE);
        return LIFT_CONTINUE;
    case A32_LDSTC_32: {
        /* type: 0 ld.w 1 ld.sh 2 ld.uh 3 ld.sb 4 ld.ub 5 st.w 6 st.h 7 st.b; disp9 scaled */
        static const uint8_t wd[8] = { 32, 16, 16, 8, 8, 32, 16, 8 };
        unsigned ty = F(L, 11, 9);
        w = wd[ty];
        g = cond(L, F(L, 15, 12));
        addr = bin(L, UAM_OP_ADD, rd(L, RS), imm(L, F(L, 8, 0) * (w / 8)));
        if (ty < 5) {
            t = ld(L, w, ty == 1 || ty == 3, addr, g);
            wr(L, RD, t, g);
        } else {
            st(L, w, addr, rd(L, RD), g);
        }
        return LIFT_CONTINUE;
    }
    case A32_LD_D_POSTINC_16: case A32_LD_D_PREDEC_16: case A32_LD_D_RR_16:
    case A32_LD_D_DISP_32: case A32_LD_D_IDX_32: {
        unsigned rdd = op == A32_LD_D_IDX_32 ? F(L, 3, 1) * 2 : F(L, 19, 17) * 2;
        uam_ref base = rd(L, RS), na = NONE;
        if (op == A32_LD_D_POSTINC_16) { addr = base; na = bin(L, UAM_OP_ADD, base, imm(L, 8)); }
        else if (op == A32_LD_D_PREDEC_16) { na = bin(L, UAM_OP_SUB, base, imm(L, 8)); addr = na; }
        else if (op == A32_LD_D_RR_16) addr = base;
        else if (op == A32_LD_D_DISP_32) addr = bin(L, UAM_OP_ADD, base, imm(L, se(F(L, 15, 0), 16)));
        else addr = bin(L, UAM_OP_ADD, base, bin(L, UAM_OP_SHL, rd(L, RD), imm(L, F(L, 5, 4))));
        uam_ref hi = ld(L, 32, false, addr, NONE);
        uam_ref lo = ld(L, 32, false, bin(L, UAM_OP_ADD, addr, imm(L, 4)), NONE);
        if (na != NONE)
            wr(L, RS, na, NONE);
        wr(L, rdd + 1, hi, NONE);
        wr(L, rdd, lo, NONE);
        return LIFT_CONTINUE;
    }
    case A32_ST_D_POSTINC_16: case A32_ST_D_PREDEC_16: case A32_ST_D_RR_16:
    case A32_ST_D_DISP_32: case A32_ST_D_IDX_32: {
        unsigned rs = op == A32_ST_D_IDX_32 ? F(L, 3, 1) * 2 : F(L, 19, 17) * 2;
        uam_ref base = rd(L, RS), na = NONE;
        if (op == A32_ST_D_POSTINC_16) { addr = base; na = bin(L, UAM_OP_ADD, base, imm(L, 8)); }
        else if (op == A32_ST_D_PREDEC_16) { na = bin(L, UAM_OP_SUB, base, imm(L, 8)); addr = na; }
        else if (op == A32_ST_D_RR_16) addr = base;
        else if (op == A32_ST_D_DISP_32) addr = bin(L, UAM_OP_ADD, base, imm(L, se(F(L, 15, 0), 16)));
        else addr = bin(L, UAM_OP_ADD, base, bin(L, UAM_OP_SHL, rd(L, RD), imm(L, F(L, 5, 4))));
        st(L, 32, addr, rd(L, rs + 1), NONE);
        st(L, 32, bin(L, UAM_OP_ADD, addr, imm(L, 4)), rd(L, rs), NONE);
        if (na != NONE)
            wr(L, RS, na, NONE);
        return LIFT_CONTINUE;
    }
    case A32_LDINS_B_RRI_32: case A32_LDINS_H_RRI_32: {
        bool byte = op == A32_LDINS_B_RRI_32;
        unsigned part = byte ? F(L, 13, 12) : F(L, 12, 12);   /* b l u t: 0..3 (byte), b t (half) */
        w = byte ? 8 : 16;
        addr = bin(L, UAM_OP_ADD, rd(L, RS), imm(L, se(F(L, 11, 0), 12) * (w / 8)));
        t = ld(L, w, false, addr, NONE);
        unsigned sh = part * w;
        uint32_t m = (uint32_t)(((UINT64_C(1) << w) - 1) << sh);
        res = bin(L, UAM_OP_OR, bin(L, UAM_OP_AND, rd(L, RD), imm(L, ~m)), bin(L, UAM_OP_SHL, t, imm(L, sh)));
        wr(L, RD, res, NONE);
        return LIFT_CONTINUE;
    }
    case A32_LDSWP_SH_DISP_32: case A32_LDSWP_UH_DISP_32: case A32_LDSWP_W_DISP_32: {
        bool word = op == A32_LDSWP_W_DISP_32;
        w = word ? 32 : 16;
        addr = bin(L, UAM_OP_ADD, rd(L, RS), imm(L, se(F(L, 11, 0), 12) * (w / 8)));
        t = uam_ld(L->b, w, UAM_ACC_LE | UAM_ACC_ALIGNED, addr, NONE);   /* swapped: little-endian */
        res = word ? t : uam_unop(L->b, op == A32_LDSWP_SH_DISP_32 ? UAM_OP_SEXT : UAM_OP_ZEXT, 32, t);
        wr(L, RD, res, NONE);
        return LIFT_CONTINUE;
    }
    case A32_STSWP_H_DISP_32: case A32_STSWP_W_DISP_32: {
        w = op == A32_STSWP_W_DISP_32 ? 32 : 16;
        addr = bin(L, UAM_OP_ADD, rd(L, RS), imm(L, se(F(L, 11, 0), 12) * (w / 8)));
        uam_ref v = rd(L, RD);
        uam_st(L->b, w, UAM_ACC_LE | UAM_ACC_ALIGNED, addr, w < 32 ? trunc_(L, w, v) : v, NONE);
        return LIFT_CONTINUE;
    }
    case A32_STHH_W_RIRR_32: case A32_STHH_W_RRSHIRR_32: {
        uam_ref hi = half(L, rd(L, RS), F(L, 13, 13), false);
        uam_ref lo = half(L, rd(L, RD), F(L, 12, 12), false);
        if (op == A32_STHH_W_RIRR_32)
            addr = bin(L, UAM_OP_ADD, rd(L, R3), imm(L, F(L, 11, 4) << 2));
        else
            addr = bin(L, UAM_OP_ADD, rd(L, R3), bin(L, UAM_OP_SHL, rd(L, F(L, 11, 8)), imm(L, F(L, 5, 4))));
        st(L, 32, addr, bin(L, UAM_OP_OR, bin(L, UAM_OP_SHL, hi, imm(L, 16)), lo), NONE);
        return LIFT_CONTINUE;
    }
    case A32_STCOND_DISP_32: {
        uam_ref l = flag(L, A32_F_L);
        addr = bin(L, UAM_OP_ADD, rd(L, RS), imm(L, se(F(L, 15, 0), 16)));
        st(L, 32, addr, rd(L, RD), l);
        setf(L, A32_F_Z, l, NONE);
        return LIFT_CONTINUE;
    }
    case A32_XCHG_RRR_32:
        t = uam_amo(L->b, 32, BE | UAM_ACC_ALIGNED, UAM_AMO_XCHG, rd(L, RS), rd(L, RD), NONE);
        wr(L, R3, t, NONE);
        return LIFT_CONTINUE;
    case A32_MEMC_II_32: case A32_MEMS_II_32: case A32_MEMT_II_32: {
        addr = imm(L, se(F(L, 14, 0) << 2, 17));
        uint32_t m = 1u << F(L, 19, 15);
        uam_amo_op sub = op == A32_MEMC_II_32 ? UAM_AMO_AND : op == A32_MEMS_II_32 ? UAM_AMO_OR : UAM_AMO_XOR;
        uam_amo(L->b, 32, BE | UAM_ACC_ALIGNED, sub, addr, imm(L, op == A32_MEMC_II_32 ? ~m : m), NONE);
        return LIFT_CONTINUE;
    }

    /* ---- load / store multiple ---- */
    case A32_LDM_RRR_32: {
        unsigned rp = RD;
        uint32_t list = F(L, 15, 0);
        bool upd = F(L, 25, 25);
        bool pcbase = rp == A32_PC;
        /* Rp == PC with PC in the list: the stack, with the return-value rule */
        uam_ref base = pcbase && (list & (1u << 15)) ? rd(L, A32_SP) : rd(L, rp);
        return load_multiple(L, base, list, upd, pcbase ? A32_SP : rp, pcbase && (list & (1u << 15)));
    }
    case A32_LDM_POSTINC_32:   /* ldm sp++, list (the popm alias) */
        return load_multiple(L, rd(L, A32_SP), F(L, 15, 0), true, A32_SP, false);
    case A32_POPM_RR_16: {
        uint32_t list = list8(F(L, 27, 20));
        bool k = F(L, 19, 19);
        return load_multiple(L, rd(L, A32_SP), list, true, A32_SP, k && (list & (1u << 15)));
    }
    case A32_STM_RR_32: case A32_STM_PREDEC_32:
        store_multiple(L, RD, F(L, 15, 0), op == A32_STM_PREDEC_32);
        return LIFT_CONTINUE;
    case A32_PUSHM_R_32:
        store_multiple(L, A32_SP, F(L, 15, 0), true);
        return LIFT_CONTINUE;
    case A32_PUSHM_RR_16:
        store_multiple(L, A32_SP, list8(F(L, 27, 20)), true);
        return LIFT_CONTINUE;
    case A32_LDMTS_RR_32: case A32_LDMTS_POSTINC_32: case A32_STMTS_RR_32: case A32_STMTS_PREDEC_32: {
        /* the application context's registers: through helpers (SP and the shadowed
         * ones differ from the current context's in a privileged mode) */
        uint32_t list = F(L, 15, 0) & 0x7FFF;
        unsigned rp = RD;
        uam_ref base = rd(L, rp);
        unsigned n = (unsigned)__builtin_popcount(list);
        const uam_helper_table *ht = &uam_isa_avr32.helpers;
        if (op == A32_LDMTS_RR_32 || op == A32_LDMTS_POSTINC_32) {
            uam_ref val[15];
            unsigned k = 0;
            for (int r = 14; r >= 0; r--)
                if (list & (1u << r))
                    val[r] = ld(L, 32, false, bin(L, UAM_OP_ADD, base, imm(L, 4 * k++)), NONE);
            if (op == A32_LDMTS_POSTINC_32)
                wr(L, rp, bin(L, UAM_OP_ADD, base, imm(L, 4 * n)), NONE);
            for (int r = 14; r >= 0; r--)
                if (list & (1u << r))
                    uam_helper(L->b, ht, A32_H_USERREG_WR, imm(L, (uint32_t)r), val[r], NONE, NONE);
            return end_block(L);
        }
        uam_ref lowest = op == A32_STMTS_PREDEC_32 ? bin(L, UAM_OP_SUB, base, imm(L, 4 * n)) : base;
        unsigned k = 0;
        for (int r = 14; r >= 0; r--)
            if (list & (1u << r)) {
                uam_ref v = uam_helper(L->b, ht, A32_H_USERREG_RD, imm(L, (uint32_t)r), NONE, NONE, NONE);
                st(L, 32, bin(L, UAM_OP_ADD, lowest, imm(L, 4 * k++)), v, NONE);
            }
        if (op == A32_STMTS_PREDEC_32)
            wr(L, rp, lowest, NONE);
        return LIFT_CONTINUE;
    }

    /* ---- control flow ---- */
    case A32_RJMP_I_16: {
        uint32_t d = se(F(L, 17, 16) << 8 | F(L, 27, 20), 10);
        uam_exit(L->b, NONE, imm(L, L->pc + (d << 1)), UAM_EXIT_JUMP);
        return LIFT_END;
    }
    case A32_RCALL_I_16: case A32_RCALL_I_32: {
        uint32_t d = op == A32_RCALL_I_16 ? se(F(L, 17, 16) << 8 | F(L, 27, 20), 10) : imm21(L);
        wr(L, A32_LR, imm(L, L->pc + L->len), NONE);
        uam_exit(L->b, NONE, imm(L, L->pc + (d << 1)), UAM_EXIT_CALL);
        return LIFT_END;
    }
    case A32_BRC_I_16: case A32_BRC_I_32: {
        uint32_t d = op == A32_BRC_I_16 ? se(F(L, 27, 20), 8) : imm21(L);
        g = cond(L, op == A32_BRC_I_16 ? F(L, 18, 16) : F(L, 19, 16));
        uam_exit(L->b, g, imm(L, L->pc + (d << 1)), UAM_EXIT_JUMP);
        if (g != NONE)
            uam_exit(L->b, NONE, imm(L, L->pc + L->len), UAM_EXIT_JUMP);
        return LIFT_END;
    }
    case A32_ICALL_R_16:
        a = rd(L, RD);
        wr(L, A32_LR, imm(L, L->pc + 2), NONE);
        uam_exit(L->b, NONE, a, UAM_EXIT_CALL);
        return LIFT_END;
    case A32_ACALL_I_16:
        a = uam_sysrd(L->b, 32, SYS_ACBA | 0x10000u, NONE);   /* ACBA, readable from every mode */
        t = ld(L, 32, false, bin(L, UAM_OP_ADD, a, imm(L, F(L, 27, 20) << 2)), NONE);
        wr(L, A32_LR, imm(L, L->pc + 2), NONE);
        uam_exit(L->b, NONE, t, UAM_EXIT_CALL);
        return LIFT_END;
    case A32_MCALL_MI_32:
        addr = bin(L, UAM_OP_ADD, bin(L, UAM_OP_AND, rd(L, RD), imm(L, ~3u)), imm(L, se(F(L, 15, 0), 16) << 2));
        t = ld(L, 32, false, addr, NONE);
        wr(L, A32_LR, imm(L, L->pc + 4), NONE);
        uam_exit(L->b, NONE, t, UAM_EXIT_CALL);
        return LIFT_END;

    /* ---- system ---- */
    case A32_SCALL_16:
        uam_trap(L->b, NONE, UAM_TRAP_EXCEPTION, imm(L, A32_TRAP_SCALL), imm(L, L->pc));
        return LIFT_END;
    case A32_BREAKPOINT_16:
        uam_trap(L->b, NONE, UAM_TRAP_EXCEPTION, imm(L, A32_TRAP_BREAKPOINT), imm(L, L->pc));
        return LIFT_END;
    case A32_RETE_16: case A32_RETS_16: case A32_RETD_16: {
        unsigned h = op == A32_RETE_16 ? A32_H_RETE : op == A32_RETS_16 ? A32_H_RETS : A32_H_RETD;
        t = uam_helper(L->b, &uam_isa_avr32.helpers, h, NONE, NONE, NONE, NONE);
        uam_exit(L->b, NONE, t, UAM_EXIT_EXC_RETURN);
        return LIFT_END;
    }
    case A32_MFSR_RI_32:
        if (!L->priv)
            return privileged(L);
        wr(L, RD, uam_sysrd(L->b, 32, F(L, 7, 0), NONE), NONE);
        return LIFT_CONTINUE;
    case A32_MTSR_IR_32:
        if (!L->priv)
            return privileged(L);
        uam_syswr(L->b, 32, F(L, 7, 0), rd(L, RD), NONE);
        return end_block(L);
    case A32_MFDR_RI_32:
        if (!L->priv)
            return privileged(L);
        wr(L, RD, imm(L, 0), NONE);   /* no debug registers */
        return LIFT_CONTINUE;
    case A32_MTDR_IR_32:
        return L->priv ? LIFT_CONTINUE : privileged(L);
    case A32_CSRF_I_16: case A32_SSRF_I_16: {
        unsigned bp = F(L, 24, 20);
        uint32_t v = op == A32_SSRF_I_16;
        static const uint8_t off[16] = { A32_F_C, A32_F_Z, A32_F_N, A32_F_V, A32_F_Q, A32_F_L, 0, 0,
                                         0, 0, 0, 0, 0, 0, A32_F_T, 0 };
        if (bp < 16) {
            if (off[bp])
                setf(L, off[bp], imm(L, v), NONE);
            return LIFT_CONTINUE;
        }
        if (!L->priv)
            return privileged(L);
        uam_helper(L->b, &uam_isa_avr32.helpers, A32_H_SETSR, imm(L, bp), imm(L, v), NONE, NONE);
        return end_block(L);
    }
    case A32_CSRFCZ_I_16: {
        unsigned bp = F(L, 24, 20);
        static const uint8_t off[16] = { A32_F_C, A32_F_Z, A32_F_N, A32_F_V, A32_F_Q, A32_F_L, 0, 0,
                                         0, 0, 0, 0, 0, 0, A32_F_T, 0 };
        if (bp < 16)
            t = off[bp] ? flag(L, off[bp]) : imm(L, 0);
        else
            t = bitn(L, uam_sysrd(L->b, 32, SYS_SR | 0x10000u, NONE), imm(L, bp));
        if (bp < 16 && !off[bp])
            t = trunc_(L, 1, t);
        setf(L, A32_F_C, t, NONE);
        setf(L, A32_F_Z, t, NONE);
        return LIFT_CONTINUE;
    }
    case A32_MUSFR_R_16:
        a = rd(L, RD);
        setf(L, A32_F_C, bitn(L, a, imm(L, 0)), NONE);
        setf(L, A32_F_Z, bitn(L, a, imm(L, 1)), NONE);
        setf(L, A32_F_N, bitn(L, a, imm(L, 2)), NONE);
        setf(L, A32_F_V, bitn(L, a, imm(L, 3)), NONE);
        return LIFT_CONTINUE;
    case A32_MUSTR_R_16:
        res = bin(L, UAM_OP_OR, bin(L, UAM_OP_OR, b2w(L, flag(L, A32_F_C)),
                                    bin(L, UAM_OP_SHL, b2w(L, flag(L, A32_F_Z)), imm(L, 1))),
                  bin(L, UAM_OP_OR, bin(L, UAM_OP_SHL, b2w(L, flag(L, A32_F_N)), imm(L, 2)),
                      bin(L, UAM_OP_SHL, b2w(L, flag(L, A32_F_V)), imm(L, 3))));
        wr(L, RD, res, NONE);
        return LIFT_CONTINUE;
    case A32_NOP_16: case A32_FRS_16: case A32_PREF_MI_32:
        return LIFT_CONTINUE;
    case A32_SYNC_I_32:
        uam_fence(L->b, UAM_FENCE_FULL);
        return LIFT_CONTINUE;
    case A32_CACHE_RII_32:
        return L->priv ? LIFT_CONTINUE : privileged(L);   /* no caches are modelled */
    case A32_SLEEP_I_32:
        if (!L->priv)
            return privileged(L);
        uam_helper(L->b, &uam_isa_avr32.helpers, A32_H_SLEEP, imm(L, F(L, 7, 0)), NONE, NONE, NONE);
        uam_wait(L->b, imm(L, L->pc + L->len));
        return LIFT_END;
    case A32_TLBR_16: case A32_TLBW_16: case A32_TLBS_16:
        if (!L->priv)
            return privileged(L);
        uam_helper(L->b, &uam_isa_avr32.helpers, A32_H_TLB,
                   imm(L, op == A32_TLBR_16 ? 0 : op == A32_TLBW_16 ? 1 : 2), NONE, NONE, NONE);
        return end_block(L);

    /* ---- DSP ---- */
    case A32_ADDHH_W_RRR_32: case A32_SUBHH_W_RRR_32:
        a = half(L, rd(L, RS), F(L, 5, 5), true);
        b = half(L, rd(L, RD), F(L, 4, 4), true);
        res = bin(L, op == A32_ADDHH_W_RRR_32 ? UAM_OP_ADD : UAM_OP_SUB, a, b);
        wr(L, R3, res, NONE);
        if (op == A32_ADDHH_W_RRR_32)
            flags_add(L, a, b, res, NONE);
        else
            flags_sub(L, a, b, res, NONE);
        return LIFT_CONTINUE;
    case A32_MULHH_W_RRR_32: case A32_MULNHH_W_RRR_32: case A32_MACHH_W_RRR_32:
        a = half(L, rd(L, RS), F(L, 5, 5), true);
        b = half(L, rd(L, RD), F(L, 4, 4), true);
        res = bin(L, UAM_OP_MUL, a, b);
        if (op == A32_MULNHH_W_RRR_32)
            res = neg32(L, res);
        if (op == A32_MACHH_W_RRR_32)
            res = bin(L, UAM_OP_ADD, res, rd(L, R3));
        wr(L, R3, res, NONE);
        return LIFT_CONTINUE;
    case A32_MACHH_D_RRR_32: case A32_MULWH_D_RRR_32: case A32_MULNWH_D_RRR_32: case A32_MACWH_D_RRR_32: {
        /* the product, placed at bits 63:16 of Rd+1:Rd; Rd[15:0] <- 0 */
        unsigned rdd = R3 & ~1u;
        uam_ref x = op == A32_MACHH_D_RRR_32 ? half(L, rd(L, RS), F(L, 5, 5), true) : rd(L, RS);
        uam_ref y = half(L, rd(L, RD), F(L, 4, 4), true);
        uam_ref p = uam_binop(L->b, UAM_OP_MUL, 64, sext(L, 64, 32, x), sext(L, 64, 32, y));
        if (op == A32_MACHH_D_RRR_32)   /* the 32-bit product, extended */
            p = sext(L, 64, 32, trunc_(L, 32, p));
        if (op == A32_MULNWH_D_RRR_32)
            p = uam_binop(L->b, UAM_OP_SUB, 64, uam_imm(L->b, 0), p);
        p = uam_binop(L->b, UAM_OP_SHL, 64, p, uam_imm(L->b, 16));
        if (op == A32_MACHH_D_RRR_32 || op == A32_MACWH_D_RRR_32) {
            uam_ref acc = uam_binop(L->b, UAM_OP_AND, 64, rd64(L, rdd), uam_imm(L->b, ~UINT64_C(0xFFFF)));
            p = uam_binop(L->b, UAM_OP_ADD, 64, p, acc);
        }
        wr64(L, rdd, p);
        return LIFT_CONTINUE;
    }
    case A32_MULSATHH_H_RRR_32: case A32_MULSATHH_W_RRR_32: case A32_MULSATRNDHH_H_RRR_32:
    case A32_MACSATHH_W_RRR_32: {
        unsigned dop = op == A32_MULSATHH_H_RRR_32 ? DSP_MULSATHH_H : op == A32_MULSATHH_W_RRR_32 ? DSP_MULSATHH_W
                     : op == A32_MULSATRNDHH_H_RRR_32 ? DSP_MULSATRNDHH_H : DSP_MULSATHH_W;
        a = half(L, rd(L, RS), F(L, 5, 5), true);
        b = half(L, rd(L, RD), F(L, 4, 4), true);
        uam_ref h = dsp_call(L, dop, a, b);
        if (op == A32_MACSATHH_W_RRR_32) {
            uam_ref h2 = dsp_call(L, DSP_MACSATHH_W, hres(L, h), rd(L, R3));
            wr(L, R3, hres(L, h2), NONE);
            uam_ref q = b1or(L, hbit(L, h, 32), hbit(L, h2, 32));
            setf(L, A32_F_Q, b1or(L, flag(L, A32_F_Q), q), NONE);
            return LIFT_CONTINUE;
        }
        wr(L, R3, hres(L, h), NONE);
        q_or(L, h);
        return LIFT_CONTINUE;
    }
    case A32_MULSATRNDWH_W_RRR_32: case A32_MULSATWH_W_RRR_32: {
        b = half(L, rd(L, RD), F(L, 4, 4), true);
        uam_ref h = dsp_call(L, op == A32_MULSATWH_W_RRR_32 ? DSP_MULSATWH_W : DSP_MULSATRNDWH_W, rd(L, RS), b);
        wr(L, R3, hres(L, h), NONE);
        q_or(L, h);
        return LIFT_CONTINUE;
    }
    case A32_SATADD_H_RRR_32: case A32_SATADD_W_RRR_32: case A32_SATSUB_H_RRR_32:
    case A32_SATSUB_W_RRR_32: case A32_SATSUB_W_RRI_32: {
        unsigned dop = op == A32_SATADD_H_RRR_32 ? DSP_SATADD_H : op == A32_SATADD_W_RRR_32 ? DSP_SATADD_W
                     : op == A32_SATSUB_H_RRR_32 ? DSP_SATSUB_H : DSP_SATSUB_W;
        unsigned dst = op == A32_SATSUB_W_RRI_32 ? RD : R3;
        b = op == A32_SATSUB_W_RRI_32 ? imm(L, se(F(L, 15, 0), 16)) : rd(L, RD);
        uam_ref h = dsp_call(L, dop, rd(L, RS), b);
        wr(L, dst, hres(L, h), NONE);
        q_or(L, h);
        setf(L, A32_F_V, hbit(L, h, 33), NONE);
        setf(L, A32_F_N, hbit(L, h, 34), NONE);
        setf(L, A32_F_Z, hbit(L, h, 35), NONE);
        setf(L, A32_F_C, imm(L, 0), NONE);
        return LIFT_CONTINUE;
    }
    case A32_SATS_RII_32: case A32_SATU_RII_32: case A32_SATRNDS_RII_32: case A32_SATRNDU_RII_32: {
        unsigned dop = op == A32_SATS_RII_32 ? DSP_SATS : op == A32_SATU_RII_32 ? DSP_SATU
                     : op == A32_SATRNDS_RII_32 ? DSP_SATRNDS : DSP_SATRNDU;
        /* y = sa5 | bp5 << 5 */
        uam_ref h = dsp_call(L, dop, rd(L, RD), imm(L, F(L, 4, 0) | F(L, 9, 5) << 5));
        wr(L, RD, hres(L, h), NONE);
        q_or(L, h);
        return LIFT_CONTINUE;
    }

    /* ---- SIMD (AVR32B) ---- */
    case A32_PABS_SB_RR_32: case A32_PABS_SH_RR_32: case A32_PACKSH_SB_RRR_32: case A32_PACKSH_UB_RRR_32:
    case A32_PACKW_SH_RRR_32: case A32_PADDH_SH_RRR_32: case A32_PADDH_UB_RRR_32:
    case A32_PADDSUBH_SH_RRR_32: case A32_PADDSUBS_SH_RRR_32: case A32_PADDSUBS_UH_RRR_32:
    case A32_PADDSUB_H_RRR_32: case A32_PADDS_SB_RRR_32: case A32_PADDS_SH_RRR_32: case A32_PADDS_UB_RRR_32:
    case A32_PADDS_UH_RRR_32: case A32_PADDXH_SH_RRR_32: case A32_PADDXS_SH_RRR_32: case A32_PADDXS_UH_RRR_32:
    case A32_PADDX_H_RRR_32: case A32_PADD_B_RRR_32: case A32_PADD_H_RRR_32: case A32_PASR_B_RRI_32:
    case A32_PASR_H_RRI_32: case A32_PAVG_SH_RRR_32: case A32_PAVG_UB_RRR_32: case A32_PLSL_B_RRI_32:
    case A32_PLSL_H_RRI_32: case A32_PLSR_B_RRI_32: case A32_PLSR_H_RRI_32: case A32_PMAX_SH_RRR_32:
    case A32_PMAX_UB_RRR_32: case A32_PMIN_SH_RRR_32: case A32_PMIN_UB_RRR_32: case A32_PSAD_RRR_32:
    case A32_PSUBADDH_SH_RRR_32: case A32_PSUBADDS_SH_RRR_32: case A32_PSUBADDS_UH_RRR_32:
    case A32_PSUBADD_H_RRR_32: case A32_PSUBH_SH_RRR_32: case A32_PSUBH_UB_RRR_32: case A32_PSUBS_SB_RRR_32:
    case A32_PSUBS_SH_RRR_32: case A32_PSUBS_UB_RRR_32: case A32_PSUBS_UH_RRR_32: case A32_PSUBXH_SH_RRR_32:
    case A32_PSUBXS_SH_RRR_32: case A32_PSUBXS_UH_RRR_32: case A32_PSUBX_H_RRR_32: case A32_PSUB_B_RRR_32:
    case A32_PSUB_H_RRR_32: case A32_PUNPCKSB_H_RR_32: case A32_PUNPCKUB_H_RR_32: {
        if (!L->s->cfg.simd)
            return unimpl(L);
        uam_ref h = uam_helper(L->b, &uam_isa_avr32.helpers, A32_H_SIMD, imm(L, L->w), rd(L, RS), rd(L, RD), NONE);
        wr(L, R3, hres(L, h), NONE);
        if (op == A32_PMAX_UB_RRR_32 || op == A32_PMIN_UB_RRR_32 || op == A32_PMAX_SH_RRR_32
            || op == A32_PMIN_SH_RRR_32) {
            bool bytes = op == A32_PMAX_UB_RRR_32 || op == A32_PMIN_UB_RRR_32;
            setf(L, A32_F_C, hbit(L, h, 33), NONE);
            setf(L, A32_F_Z, hbit(L, h, 34), NONE);
            if (bytes) {
                setf(L, A32_F_N, hbit(L, h, 35), NONE);
                setf(L, A32_F_V, hbit(L, h, 36), NONE);
            }
        } else {
            q_or(L, h);
        }
        return LIFT_CONTINUE;
    }

    /* ---- left out ---- */
    case A32_INCJOSP_I_16: case A32_POPJC_16: case A32_PUSHJC_16: case A32_RETJ_16:
    case A32_SSCALL_16: case A32_RETSS_16:
        return unimpl(L);
    case A32_COPROC_32:
        return trap(L, EV_COP_ABSENT);
    default:
        return illegal(L);
    }
}

uam_status avr32_lift(uam_lift_ctx *lc)
{
    const avr32_state *s = a32_st(lc->cpu);
    uint32_t pc = (uint32_t)lc->pc;
    for (uint32_t n = 0;; n++) {
        if (n == lc->max_insns) {
            uam_exit(lc->b, NONE, uam_imm(lc->b, pc), UAM_EXIT_JUMP);
            return UAM_OK;
        }
        a32_lifter L = { .b = lc->b, .s = s, .pc = pc, .priv = lc->cpu->priv != A32_PRIV_APP,
                         .jkind = UAM_EXIT_JUMP, .jtarget = NONE, .jguard = NONE };
        if (pc & 1) {   /* misaligned instruction address */
            uam_imark(lc->b, pc, 2);
            trap(&L, EV_INSN_ADDR);
            return UAM_OK;
        }
        uint64_t hi = 0, lo = 0;
        uam_status st = lc->fetch(lc, pc, 2, &hi);
        if (st == UAM_OK && (hi >> 13) == 7)
            st = lc->fetch(lc, pc + 2, 2, &lo);
        if (st != UAM_OK) {
            if (n == 0)
                return UAM_ERR_FAULT;
            uam_exit(lc->b, NONE, uam_imm(lc->b, pc), UAM_EXIT_JUMP);
            return UAM_OK;
        }
        L.len = (hi >> 13) == 7 ? 4 : 2;
        L.w = (uint32_t)(hi << 16 | lo);
        uam_imark(lc->b, pc, L.len);
        unsigned op = avr32_decode(L.w);
        lift_result r = lift_one(&L, op);
        if (r == LIFT_CONTINUE && L.jump) {
            uam_exit(lc->b, L.jguard, L.jtarget, L.jkind);
            if (L.jguard != NONE)
                uam_exit(lc->b, NONE, uam_imm(lc->b, pc + L.len), UAM_EXIT_JUMP);
            r = LIFT_END;
        }
        pc += L.len;
        if (r == LIFT_END)
            return UAM_OK;
    }
}
