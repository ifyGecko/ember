/*
 * mips_lift.c - MIPS32r2 decoder and lifter.
 *
 * Covers the integer user ISA (SPECIAL, SPECIAL2, SPECIAL3, REGIMM, immediate
 * and memory forms, including the unaligned LWL/LWR/SWL/SWR group) plus the
 * CP0 control instructions (MFC0/MTC0, EI/DI, ERET, WAIT, TLBR/TLBWI/TLBWR/TLBP,
 * RDPGPR/WRPGPR), JALX into a compressed ISA, and the MCU ASE's IRET, ASET and
 * ACLR. The FPU (mips_fpu.c) and the DSP ASE (mips_dsp.c) are lifted when the
 * processor has them; otherwise COP1 is a coprocessor-unusable exception and
 * COP2/COP3 always are, with the coprocessor number as tval (it becomes
 * Cause.CE). Blocks whose pc has bit 0 set run MIPS16e (mips16.c) or microMIPS
 * (mips_umips.c); those decoders recode most of their instructions as MIPS32
 * words and lift them here (mips_lift32), with the next pc taken from ml.npc.
 *
 * Branch delay slots are handled structurally (docs/uam-isa.md, UAM_IMARK_CONT):
 * a branch computes its condition and target into temporaries, then the delay
 * slot is lifted under a continuation IMARK, then the block exits. The branch
 * and its delay slot therefore retire together and cannot be split by an
 * interrupt. Branch-likely instructions annul (skip) the delay slot when not
 * taken. An exception raised by the delay slot is reported by the executor as
 * a continuation of the branch (UAM_EXC_F_CONT), which becomes EPC = branch,
 * Cause.BD = 1.
 */
#include "mips.h"

/* Instruction fields. */
#define OP(i)     ((i) >> 26)
#define RS(i)     (((i) >> 21) & 0x1F)
#define RT(i)     (((i) >> 16) & 0x1F)
#define RD(i)     (((i) >> 11) & 0x1F)
#define SA(i)     (((i) >> 6) & 0x1F)
#define FUNCT(i)  ((i) & 0x3F)
#define IMM(i)    ((i) & 0xFFFF)
#define SIMM(i)   ((uint32_t)(int32_t)(int16_t)(i))

/* ---- operand helpers --------------------------------------------------- */

static uam_ref imm(ml *L, uint32_t v) { return uam_imm(L->b, v); }

static uam_ref rs_r(ml *L, uint32_t i) { unsigned r = RS(i); return r ? uam_get(L->b, 32, MIPS_REG_OFF(r)) : imm(L, 0); }
static uam_ref rt_r(ml *L, uint32_t i) { unsigned r = RT(i); return r ? uam_get(L->b, 32, MIPS_REG_OFF(r)) : imm(L, 0); }
static uam_ref reg_r(ml *L, unsigned r) { return r ? uam_get(L->b, 32, MIPS_REG_OFF(r)) : imm(L, 0); }

static void reg_w(ml *L, unsigned r, uam_ref v) { if (r) uam_put(L->b, 32, MIPS_REG_OFF(r), v, NONE); }

static uam_ref bin(ml *L, uam_opcode op, uam_ref a, uam_ref b) { return uam_binop(L->b, op, 32, a, b); }
static uam_ref boolv(ml *L, uam_ref c) { return uam_unop(L->b, UAM_OP_ZEXT, 32, c); } /* 1-bit -> 0/1:32 */

static void trap(ml *L, uam_ref guard, uint32_t exccode, uam_ref tval)
{
    uam_trap(L->b, guard, UAM_TRAP_EXCEPTION, imm(L, exccode), tval);
}

/* Exported for the other decoders. */
uam_ref mips_imm(ml *L, uint32_t v) { return imm(L, v); }
uam_ref mips_reg_r(ml *L, unsigned r) { return reg_r(L, r); }
void    mips_reg_w(ml *L, unsigned r, uam_ref v) { reg_w(L, r, v); }
uam_ref mips_bin(ml *L, uam_opcode op, uam_ref a, uam_ref b) { return bin(L, op, a, b); }
void    mips_trap(ml *L, uam_ref guard, uint32_t exccode, uam_ref tval) { trap(L, guard, exccode, tval); }
void    mips_ri(ml *L) { trap(L, NONE, EXC_RI, imm(L, L->raw)); }

uam_ref mips_helper(ml *L, unsigned id, uam_ref a, uam_ref b, uam_ref c)
{
    return uam_helper(L->b, &L->lc->cpu->isa->helpers, id, a, b, c, NONE);
}

/* DSP ASE instructions need Status.MX; without it they raise DSP Disabled. True
 * if the instruction may proceed (else the trap has been emitted). */
static bool dsp_enabled(ml *L)
{
    if (L->priv & MIPS_PRIV_MX)
        return true;
    trap(L, NONE, EXC_DSPDIS, imm(L, 0));
    return false;
}

/* ---- memory ------------------------------------------------------------ */

static uam_ref ea(ml *L, uint32_t i) { return bin(L, UAM_OP_ADD, rs_r(L, i), imm(L, SIMM(i))); }

/* Truncate a 32-bit value to w bits. Conversions need a temp source, so an
 * immediate (e.g. a $zero operand) is narrowed as a constant instead. */
static uam_ref narrow(ml *L, uam_ref v, unsigned w)
{
    if (uam_ref_is_imm(v))
        return imm(L, (uint32_t)(L->b->imm[uam_ref_index(v)] & uam_mask(w)));
    return uam_unop(L->b, UAM_OP_TRUNC, w, v);
}

/* Sign-extend the low `from` bits of a 32-bit value back to 32 bits. */
static uam_ref sext_from(ml *L, uam_ref v, unsigned from)
{
    if (uam_ref_is_imm(v))
        return imm(L, (uint32_t)uam_sext(L->b->imm[uam_ref_index(v)], from));
    return uam_unop(L->b, UAM_OP_SEXT, 32, uam_unop(L->b, UAM_OP_TRUNC, from, v));
}

static lift_result lift_load(ml *L, uint32_t i, unsigned w, bool sign)
{
    unsigned flags = L->endian | (w > 8 ? UAM_ACC_ALIGNED : 0);
    uam_ref v = uam_ld(L->b, w, flags, ea(L, i), NONE);
    if (w < 32)
        v = uam_unop(L->b, sign ? UAM_OP_SEXT : UAM_OP_ZEXT, 32, v);
    reg_w(L, RT(i), v);
    return LIFT_SEQ;
}

static lift_result lift_store(ml *L, uint32_t i, unsigned w)
{
    unsigned flags = L->endian | (w > 8 ? UAM_ACC_ALIGNED : 0);
    uam_ref v = rt_r(L, i);
    if (w < 32)
        v = narrow(L, v, w);
    uam_st(L->b, w, flags, ea(L, i), v, NONE);
    return LIFT_SEQ;
}

/* Common pieces for the unaligned word accessors. */
static uam_ref aligned_addr(ml *L, uam_ref addr) { return bin(L, UAM_OP_AND, addr, imm(L, 0xFFFFFFFCu)); }
static uam_ref byte_off(ml *L, uam_ref addr) { return bin(L, UAM_OP_AND, addr, imm(L, 3)); }
static uam_ref shl(ml *L, uam_ref a, uam_ref n) { return bin(L, UAM_OP_SHL, a, n); }
static uam_ref lshr(ml *L, uam_ref a, uam_ref n) { return bin(L, UAM_OP_LSHR, a, n); }
static uam_ref notv(ml *L, uam_ref a) { return bin(L, UAM_OP_XOR, a, imm(L, 0xFFFFFFFFu)); }

/* shift = 8*sel, where sel is byte_off or (3 - byte_off). */
static uam_ref shift_from(ml *L, uam_ref addr, bool complement)
{
    uam_ref bo = byte_off(L, addr);
    if (complement)
        bo = bin(L, UAM_OP_SUB, imm(L, 3), bo);
    return bin(L, UAM_OP_SHL, bo, imm(L, 3)); /* *8 */
}

/*
 * LWL/LWR/SWL/SWR follow the MIPS manual pseudocode, parameterised by endian.
 * memword is the aligned 32-bit word in the CPU byte order. The partial-word
 * shift amount differs between the "left" and "right" halves and between
 * endiannesses; the shift-by->=width->0 rule of SHL/LSHR gives the right edge
 * behaviour when the shift is zero.
 */
static lift_result lift_lwl(ml *L, uint32_t i, bool left)
{
    bool be = L->endian & UAM_ACC_BE;
    uam_ref addr = ea(L, i);
    uam_ref word = uam_ld(L->b, 32, L->endian | UAM_ACC_ALIGNED, aligned_addr(L, addr), NONE);
    uam_ref old = rt_r(L, i);
    uam_ref res;
    if (left) {
        uam_ref sh = shift_from(L, addr, !be);            /* BE: 8*bo ; LE: 8*(3-bo) */
        uam_ref lowmask = bin(L, UAM_OP_SUB, shl(L, imm(L, 1), sh), imm(L, 1));
        res = bin(L, UAM_OP_OR, shl(L, word, sh), bin(L, UAM_OP_AND, old, lowmask));
    } else {
        uam_ref sh = shift_from(L, addr, be);             /* BE: 8*(3-bo) ; LE: 8*bo */
        uam_ref highmask = notv(L, lshr(L, imm(L, 0xFFFFFFFFu), sh));
        res = bin(L, UAM_OP_OR, lshr(L, word, sh), bin(L, UAM_OP_AND, old, highmask));
    }
    reg_w(L, RT(i), res);
    return LIFT_SEQ;
}

static lift_result lift_swl(ml *L, uint32_t i, bool left)
{
    bool be = L->endian & UAM_ACC_BE;
    uam_ref addr = ea(L, i);
    uam_ref al = aligned_addr(L, addr);
    uam_ref word = uam_ld(L->b, 32, L->endian | UAM_ACC_ALIGNED, al, NONE);
    uam_ref val = rt_r(L, i);
    uam_ref res;
    if (left) {
        uam_ref sh = shift_from(L, addr, !be);
        uam_ref keep = notv(L, lshr(L, imm(L, 0xFFFFFFFFu), sh));
        res = bin(L, UAM_OP_OR, bin(L, UAM_OP_AND, word, keep), lshr(L, val, sh));
    } else {
        uam_ref sh = shift_from(L, addr, be);
        uam_ref keep = notv(L, shl(L, imm(L, 0xFFFFFFFFu), sh));
        res = bin(L, UAM_OP_OR, bin(L, UAM_OP_AND, word, keep), shl(L, val, sh));
    }
    uam_st(L->b, 32, L->endian | UAM_ACC_ALIGNED, al, res, NONE);
    return LIFT_SEQ;
}

/* ---- ALU --------------------------------------------------------------- */

/* rd = rs +/- rt, trapping on signed overflow (ADD/SUB). */
static void alu_trapping(ml *L, uint32_t i, bool sub)
{
    uam_ref a = rs_r(L, i), b = rt_r(L, i);
    uam_ref r = bin(L, sub ? UAM_OP_SUB : UAM_OP_ADD, a, b);
    uam_ref ov = sub ? bin(L, UAM_OP_SBORROW, a, b) : bin(L, UAM_OP_SCARRY, a, b);
    trap(L, ov, EXC_OV, imm(L, 0));
    reg_w(L, RD(i), r);
}

/* rt = rs + sign-extended immediate, trapping on signed overflow (ADDI). */
static void addi_trapping(ml *L, uint32_t i)
{
    uam_ref a = rs_r(L, i), b = imm(L, SIMM(i));
    uam_ref r = bin(L, UAM_OP_ADD, a, b);
    trap(L, bin(L, UAM_OP_SCARRY, a, b), EXC_OV, imm(L, 0));
    reg_w(L, RT(i), r);
}

static void shift_const(ml *L, uint32_t i, uam_opcode op, bool rotate)
{
    uam_ref v = rt_r(L, i);
    reg_w(L, RD(i), rotate ? bin(L, UAM_OP_ROTR, v, imm(L, SA(i)))
                           : bin(L, op, v, imm(L, SA(i))));
}

static void shift_var(ml *L, uint32_t i, uam_opcode op, bool rotate)
{
    uam_ref amt = bin(L, UAM_OP_AND, rs_r(L, i), imm(L, 31));
    uam_ref v = rt_r(L, i);
    reg_w(L, RD(i), rotate ? bin(L, UAM_OP_ROTR, v, amt) : bin(L, op, v, amt));
}

/* {HI,LO} of accumulator ac (0 unless the DSP ASE is present) = a*b (signed or
 * unsigned), full 64-bit. */
static void mul_full(ml *L, unsigned ac, uam_ref a, uam_ref b, bool sign)
{
    uam_ref lo = bin(L, UAM_OP_MUL, a, b);
    uam_ref hi = bin(L, sign ? UAM_OP_MULHS : UAM_OP_MULHU, a, b);
    uam_put(L->b, 32, MIPS_ACLO_OFF(ac), lo, NONE);
    uam_put(L->b, 32, MIPS_ACHI_OFF(ac), hi, NONE);
}

/* {HI,LO} = {HI,LO} +/- a*b. */
static void mul_acc(ml *L, unsigned ac, uam_ref a, uam_ref b, bool sign, bool sub)
{
    uam_ref plo = bin(L, UAM_OP_MUL, a, b);
    uam_ref phi = bin(L, sign ? UAM_OP_MULHS : UAM_OP_MULHU, a, b);
    uam_ref lo = uam_get(L->b, 32, MIPS_ACLO_OFF(ac)), hi = uam_get(L->b, 32, MIPS_ACHI_OFF(ac));
    uam_ref nlo, carry, nhi;
    if (sub) {
        nlo = bin(L, UAM_OP_SUB, lo, plo);
        carry = bin(L, UAM_OP_ULT, lo, plo);               /* borrow */
        nhi = bin(L, UAM_OP_SUB, bin(L, UAM_OP_SUB, hi, phi), boolv(L, carry));
    } else {
        nlo = bin(L, UAM_OP_ADD, lo, plo);
        carry = bin(L, UAM_OP_CARRY, lo, plo);
        nhi = bin(L, UAM_OP_ADD, bin(L, UAM_OP_ADD, hi, phi), boolv(L, carry));
    }
    uam_put(L->b, 32, MIPS_ACLO_OFF(ac), nlo, NONE);
    uam_put(L->b, 32, MIPS_ACHI_OFF(ac), nhi, NONE);
}

/* The accumulator a DSP ASE form names (field at bit `shift`), or 0 without the
 * ASE (the bits are then ignored, as before). ~0u if the DSP is disabled (the trap
 * has been emitted). */
static unsigned acc_field(ml *L, uint32_t i, unsigned shift)
{
    unsigned ac = mips_has_dsp(L->s) ? (i >> shift) & 3u : 0;
    if (ac && !dsp_enabled(L))
        return ~0u;
    return ac;
}

static void div_op(ml *L, uint32_t i, bool sign)
{
    uam_ref a = rs_r(L, i), b = rt_r(L, i);
    uam_put(L->b, 32, MIPS_LO_OFF, bin(L, sign ? UAM_OP_SDIV : UAM_OP_UDIV, a, b), NONE);
    uam_put(L->b, 32, MIPS_HI_OFF, bin(L, sign ? UAM_OP_SREM : UAM_OP_UREM, a, b), NONE);
}

/* ---- SPECIAL3 bitfield ops --------------------------------------------- */

static void lift_ext(ml *L, uint32_t i)
{
    unsigned pos = SA(i), size = RD(i) + 1;
    uam_ref v = lshr(L, rs_r(L, i), imm(L, pos));
    if (size < 32)
        v = bin(L, UAM_OP_AND, v, imm(L, (uint32_t)((UINT64_C(1) << size) - 1)));
    reg_w(L, RT(i), v);
}

static void lift_ins(ml *L, uint32_t i)
{
    unsigned pos = SA(i), msb = RD(i);
    if (msb < pos)
        return; /* UNPREDICTABLE */
    unsigned size = msb - pos + 1;
    uint32_t mask = (uint32_t)(((UINT64_C(1) << size) - 1) << pos);
    uam_ref src = bin(L, UAM_OP_AND, shl(L, rs_r(L, i), imm(L, pos)), imm(L, mask));
    uam_ref kept = bin(L, UAM_OP_AND, rt_r(L, i), imm(L, ~mask));
    reg_w(L, RT(i), bin(L, UAM_OP_OR, kept, src));
}

static void lift_bshfl(ml *L, uint32_t i)
{
    uam_ref v = rt_r(L, i);
    switch (SA(i)) {
    case 0x02: /* WSBH: swap bytes in each halfword */
        reg_w(L, RD(i), bin(L, UAM_OP_OR,
            bin(L, UAM_OP_AND, shl(L, v, imm(L, 8)), imm(L, 0xFF00FF00u)),
            bin(L, UAM_OP_AND, lshr(L, v, imm(L, 8)), imm(L, 0x00FF00FFu))));
        break;
    case 0x10: reg_w(L, RD(i), sext_from(L, v, 8)); break;  /* SEB */
    case 0x18: reg_w(L, RD(i), sext_from(L, v, 16)); break; /* SEH */
    default: trap(L, NONE, EXC_RI, imm(L, L->raw)); break;
    }
}

/* ---- control transfers ------------------------------------------------- */

static bool is_control(const mips_state *s, uint32_t i)
{
    switch (OP(i)) {
    case 0:  return FUNCT(i) == 0x08 || FUNCT(i) == 0x09;         /* JR, JALR */
    case 1:  return RT(i) <= 3 || (RT(i) >= 16 && RT(i) <= 19)    /* REGIMM branches */
                    || (RT(i) == 0x1C && mips_has_dsp(s));        /* BPOSGE32 */
    case 2: case 3:                                               /* J, JAL */
    case 4: case 5: case 6: case 7:                               /* BEQ BNE BLEZ BGTZ */
    case 20: case 21: case 22: case 23:                           /* *L (likely) */
        return true;
    case 0x1D: return mips_compressed(s);                         /* JALX */
    case 0x11: return RS(i) == 8 && mips_has_fpu(s);              /* BC1F/BC1T (+likely) */
    default: return false;
    }
}

static lift_result lift_insn(ml *L, uint32_t i);

/* True if `i` is a legal, non-control instruction usable in a delay slot. */
static bool is_simple_delay(const mips_state *s, uint32_t i)
{
    if (is_control(s, i))
        return false;
    if (OP(i) == 0 && (FUNCT(i) == 0x0C || FUNCT(i) == 0x0D))
        return false; /* SYSCALL, BREAK terminate the block */
    if (OP(i) == 0x10)
        return false; /* COP0 (ERET/WAIT/MTC0) - keep delay slots simple */
    return true;
}

/*
 * Emit the delay slot then the branch exits. `cond` is NONE for an
 * unconditional transfer; `likely` annuls the slot when not taken.
 */
static lift_result finish_branch(ml *L, uam_ref cond, uam_ref target, bool likely)
{
    uint32_t dpc = L->pc + 4;
    uint64_t raw = 0;
    uam_status st = L->lc->fetch(L->lc, dpc, 4, &raw);

    if (likely && !uam_ref_is_none(cond)) {
        /* Not taken: skip the delay slot entirely. */
        uam_exit(L->b, uam_binop(L->b, UAM_OP_XOR, 1, cond, imm(L, 1)),
                 imm(L, L->pc + 8), UAM_EXIT_JUMP);
    }

    uam_imark_cont(L->b, dpc, 4);
    if (st != UAM_OK)
        trap(L, NONE, MIPS_TRAP_SLOT_FETCH, imm(L, dpc)); /* precise cause decided at delivery */
    else if (!is_simple_delay(L->s, (uint32_t)raw))
        trap(L, NONE, EXC_RI, imm(L, (uint32_t)raw));
    else {
        ml slot = *L;
        slot.pc = dpc;
        slot.npc = dpc + 4;
        slot.raw = (uint32_t)raw;
        if (lift_insn(&slot, (uint32_t)raw) == LIFT_DONE)
            return LIFT_DONE;                                     /* the slot always traps */
        if (likely && !uam_ref_is_none(cond))
            uam_exit(L->b, NONE, target, UAM_EXIT_JUMP);          /* taken */
        else if (uam_ref_is_none(cond))
            uam_exit(L->b, NONE, target, UAM_EXIT_JUMP);          /* unconditional */
        else {
            uam_exit(L->b, cond, target, UAM_EXIT_JUMP);
            uam_exit(L->b, NONE, imm(L, L->pc + 8), UAM_EXIT_JUMP);
        }
    }
    return LIFT_DONE;
}

/* The condition of BPOSGE32: DSPControl.pos >= 32. */
static uam_ref pos_ge32(ml *L)
{
    uam_ref pos = bin(L, UAM_OP_AND, uam_get(L->b, 32, MIPS_DSPCTL_OFF), imm(L, 0x3F));
    return bin(L, UAM_OP_ULE, imm(L, 32), pos);
}

/* The condition of BC1F/BC1T on condition code cc (FCSR bit 23, then 25..31). */
uam_ref mips_fcc(ml *L, unsigned cc, bool tf)
{
    uam_ref f = uam_get(L->b, 32, MIPS_FCSR_OFF);
    uam_ref bit = bin(L, UAM_OP_AND, f, imm(L, cc ? 1u << (24 + cc) : 1u << 23));
    return bin(L, tf ? UAM_OP_NE : UAM_OP_EQ, bit, imm(L, 0));
}

static lift_result lift_control(ml *L, uint32_t i)
{
    uint32_t op = OP(i);
    uint32_t btgt = L->pc + 4 + (SIMM(i) << 2);
    bool likely = (op >= 20 && op <= 23) || (op == 1 && (RT(i) == 2 || RT(i) == 3 || RT(i) >= 18));

    if (op == 2 || op == 3) { /* J / JAL */
        if (op == 3)
            reg_w(L, 31, imm(L, L->pc + 8));
        uint32_t tgt = ((L->pc + 4) & 0xF0000000u) | ((i & 0x03FFFFFFu) << 2);
        return finish_branch(L, NONE, imm(L, tgt), false);
    }
    if (op == 0x1D) { /* JALX: to the compressed ISA */
        reg_w(L, 31, imm(L, L->pc + 8));
        uint32_t tgt = ((L->pc + 4) & 0xF0000000u) | ((i & 0x03FFFFFFu) << 2) | 1u;
        return finish_branch(L, NONE, imm(L, tgt), false);
    }
    if (op == 0x11) { /* BC1F / BC1T / BC1FL / BC1TL */
        if (!(L->priv & MIPS_PRIV_CU1)) {
            trap(L, NONE, EXC_CPU, imm(L, 1));
            return LIFT_DONE;
        }
        return finish_branch(L, mips_fcc(L, RT(i) >> 2, RT(i) & 1), imm(L, btgt), RT(i) & 2);
    }
    if (op == 0) { /* JR / JALR */
        uam_ref t = rs_r(L, i);
        if (FUNCT(i) == 0x09)
            reg_w(L, RD(i) ? RD(i) : 31, imm(L, L->pc + 8));
        return finish_branch(L, NONE, t, false);
    }
    if (op == 1 && RT(i) == 0x1C) { /* BPOSGE32 */
        if (!dsp_enabled(L))
            return LIFT_DONE;
        return finish_branch(L, pos_ge32(L), imm(L, btgt), false);
    }
    if (op == 1) { /* REGIMM: BLTZ/BGEZ (+AL, +likely) */
        bool ge = RT(i) & 1;
        bool link = RT(i) >= 16;
        uam_ref c = bin(L, ge ? UAM_OP_SLE : UAM_OP_SLT,
                        ge ? imm(L, 0) : rs_r(L, i), ge ? rs_r(L, i) : imm(L, 0));
        if (link)
            reg_w(L, 31, imm(L, L->pc + 8));
        return finish_branch(L, c, imm(L, btgt), likely);
    }

    /* BEQ / BNE / BLEZ / BGTZ and their likely forms. */
    unsigned base = op >= 20 ? op - 16 : op;
    uam_ref a = rs_r(L, i), c;
    switch (base) {
    case 4: c = bin(L, UAM_OP_EQ, a, rt_r(L, i)); break;        /* BEQ  */
    case 5: c = bin(L, UAM_OP_NE, a, rt_r(L, i)); break;        /* BNE  */
    case 6: c = bin(L, UAM_OP_SLE, a, imm(L, 0)); break;        /* BLEZ */
    default: c = bin(L, UAM_OP_SLT, imm(L, 0), a); break;       /* BGTZ */
    }
    return finish_branch(L, c, imm(L, btgt), likely);
}

/* ---- CP0 --------------------------------------------------------------- */

static lift_result lift_cop0(ml *L, uint32_t i)
{
    uint32_t rs = RS(i);
    if (rs == 0) { /* MFC0 rt, rd, sel */
        reg_w(L, RT(i), uam_sysrd(L->b, 32, CP0(RD(i), i & 7), NONE));
        return LIFT_SEQ;
    }
    if (rs == 4) { /* MTC0 rt, rd, sel */
        uam_syswr(L->b, 32, CP0(RD(i), i & 7), rt_r(L, i), NONE);
        uam_exit(L->b, NONE, imm(L, L->npc), UAM_EXIT_JUMP); /* may change interrupt state */
        return LIFT_DONE;
    }
    if (rs == 0x0A) { /* RDPGPR rd, rt: rd = rt of the previous shadow set */
        reg_w(L, RD(i), mips_helper(L, MIPS_HELPER_RDPGPR, imm(L, RT(i)), NONE, NONE));
        return LIFT_SEQ;
    }
    if (rs == 0x0E) { /* WRPGPR rd, rt: rd of the previous shadow set = rt */
        mips_helper(L, MIPS_HELPER_WRPGPR, imm(L, RD(i)), rt_r(L, i), NONE);
        return LIFT_SEQ;
    }
    if (rs == 0x0B && RD(i) == 12) { /* DI / EI (MFMC0) */
        bool ei = (i >> 5) & 1;
        uam_ref old = uam_sysrd(L->b, 32, CP0_STATUS, NONE);
        uam_ref nv = ei ? bin(L, UAM_OP_OR, old, imm(L, ST_IE))
                        : bin(L, UAM_OP_AND, old, imm(L, ~ST_IE));
        uam_syswr(L->b, 32, CP0_STATUS, nv, NONE);
        reg_w(L, RT(i), old);
        uam_exit(L->b, NONE, imm(L, L->npc), UAM_EXIT_JUMP);
        return LIFT_DONE;
    }
    if (i & (1u << 25)) { /* CO=1: ERET/WAIT/TLB */
        switch (FUNCT(i)) {
        case 0x18: /* ERET */
        case 0x1F: /* DERET (treat like ERET) */
            uam_exit(L->b, NONE,
                     uam_helper(L->b, &L->lc->cpu->isa->helpers, MIPS_HELPER_ERET, NONE, NONE, NONE, NONE),
                     UAM_EXIT_EXC_RETURN);
            return LIFT_DONE;
        case 0x38: /* IRET (MCU ASE) */
            if (!mips_has_mcu(L->s))
                break;
            uam_exit(L->b, NONE, mips_helper(L, MIPS_HELPER_IRET, NONE, NONE, NONE),
                     UAM_EXIT_EXC_RETURN);
            return LIFT_DONE;
        case 0x20: /* WAIT */
            uam_wait(L->b, imm(L, L->npc));
            return LIFT_DONE;
        case 0x01: /* TLBR */
        case 0x08: /* TLBP */
            if (CFG0_MT(L->s->cfg.config0) != 1)
                break; /* no TLB */
            uam_helper(L->b, &L->lc->cpu->isa->helpers,
                       FUNCT(i) == 0x01 ? MIPS_HELPER_TLBR : MIPS_HELPER_TLBP,
                       NONE, NONE, NONE, NONE);
            return LIFT_SEQ;
        case 0x02: /* TLBWI */
        case 0x06: /* TLBWR */
            if (CFG0_MT(L->s->cfg.config0) != 1)
                break; /* no TLB */
            /* A mapping changed: end the block so the next fetch translates afresh. */
            uam_helper(L->b, &L->lc->cpu->isa->helpers,
                       FUNCT(i) == 0x02 ? MIPS_HELPER_TLBWI : MIPS_HELPER_TLBWR,
                       NONE, NONE, NONE, NONE);
            uam_exit(L->b, NONE, imm(L, L->npc), UAM_EXIT_JUMP);
            return LIFT_DONE;
        default:
            break;
        }
    }
    trap(L, NONE, EXC_RI, imm(L, L->raw));
    return LIFT_DONE;
}

static lift_result lift_special(ml *L, uint32_t i)
{
    switch (FUNCT(i)) {
    case 0x00: shift_const(L, i, UAM_OP_SHL, false); return LIFT_SEQ; /* SLL (NOP when rd=0) */
    case 0x02: shift_const(L, i, UAM_OP_LSHR, RS(i) & 1); return LIFT_SEQ; /* SRL / ROTR */
    case 0x03: shift_const(L, i, UAM_OP_ASHR, false); return LIFT_SEQ; /* SRA */
    case 0x04: shift_var(L, i, UAM_OP_SHL, false); return LIFT_SEQ;   /* SLLV */
    case 0x06: shift_var(L, i, UAM_OP_LSHR, (i >> 6) & 1); return LIFT_SEQ; /* SRLV / ROTRV */
    case 0x07: shift_var(L, i, UAM_OP_ASHR, false); return LIFT_SEQ;  /* SRAV */
    case 0x0A: { /* MOVZ rd,rs,rt: rd = (rt==0) ? rs : rd */
        uam_ref c = bin(L, UAM_OP_EQ, rt_r(L, i), imm(L, 0));
        reg_w(L, RD(i), uam_select(L->b, 32, c, rs_r(L, i), reg_r(L, RD(i))));
        return LIFT_SEQ;
    }
    case 0x0B: { /* MOVN */
        uam_ref c = bin(L, UAM_OP_NE, rt_r(L, i), imm(L, 0));
        reg_w(L, RD(i), uam_select(L->b, 32, c, rs_r(L, i), reg_r(L, RD(i))));
        return LIFT_SEQ;
    }
    case 0x0C: trap(L, NONE, EXC_SYS, imm(L, 0)); return LIFT_DONE;      /* SYSCALL */
    case 0x0D: trap(L, NONE, EXC_BP, imm(L, 0)); return LIFT_DONE;       /* BREAK */
    case 0x0F: uam_fence(L->b, UAM_FENCE_FULL); return LIFT_SEQ;         /* SYNC */
    case 0x10: case 0x12: { /* MFHI / MFLO rd(, ac) */
        unsigned ac = acc_field(L, i, 21);
        if (ac == ~0u)
            return LIFT_DONE;
        reg_w(L, RD(i), uam_get(L->b, 32, FUNCT(i) == 0x10 ? MIPS_ACHI_OFF(ac) : MIPS_ACLO_OFF(ac)));
        return LIFT_SEQ;
    }
    case 0x11: case 0x13: { /* MTHI / MTLO rs(, ac) */
        unsigned ac = acc_field(L, i, 11);
        if (ac == ~0u)
            return LIFT_DONE;
        uam_put(L->b, 32, FUNCT(i) == 0x11 ? MIPS_ACHI_OFF(ac) : MIPS_ACLO_OFF(ac), rs_r(L, i), NONE);
        return LIFT_SEQ;
    }
    case 0x18: case 0x19: { /* MULT / MULTU (ac,) rs, rt */
        unsigned ac = acc_field(L, i, 11);
        if (ac == ~0u)
            return LIFT_DONE;
        mul_full(L, ac, rs_r(L, i), rt_r(L, i), FUNCT(i) == 0x18);
        return LIFT_SEQ;
    }
    case 0x1A: div_op(L, i, true); return LIFT_SEQ;                     /* DIV */
    case 0x1B: div_op(L, i, false); return LIFT_SEQ;                    /* DIVU */
    case 0x20: alu_trapping(L, i, false); return LIFT_SEQ;              /* ADD */
    case 0x21: reg_w(L, RD(i), bin(L, UAM_OP_ADD, rs_r(L, i), rt_r(L, i))); return LIFT_SEQ; /* ADDU */
    case 0x22: alu_trapping(L, i, true); return LIFT_SEQ;              /* SUB */
    case 0x23: reg_w(L, RD(i), bin(L, UAM_OP_SUB, rs_r(L, i), rt_r(L, i))); return LIFT_SEQ; /* SUBU */
    case 0x24: reg_w(L, RD(i), bin(L, UAM_OP_AND, rs_r(L, i), rt_r(L, i))); return LIFT_SEQ; /* AND */
    case 0x25: reg_w(L, RD(i), bin(L, UAM_OP_OR, rs_r(L, i), rt_r(L, i))); return LIFT_SEQ;  /* OR */
    case 0x26: reg_w(L, RD(i), bin(L, UAM_OP_XOR, rs_r(L, i), rt_r(L, i))); return LIFT_SEQ; /* XOR */
    case 0x27: reg_w(L, RD(i), notv(L, bin(L, UAM_OP_OR, rs_r(L, i), rt_r(L, i)))); return LIFT_SEQ; /* NOR */
    case 0x2A: reg_w(L, RD(i), boolv(L, bin(L, UAM_OP_SLT, rs_r(L, i), rt_r(L, i)))); return LIFT_SEQ; /* SLT */
    case 0x2B: reg_w(L, RD(i), boolv(L, bin(L, UAM_OP_ULT, rs_r(L, i), rt_r(L, i)))); return LIFT_SEQ; /* SLTU */
    case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x36: { /* TGE/TGEU/TLT/TLTU/TEQ/TNE */
        uam_ref a = rs_r(L, i), b = rt_r(L, i), c;
        switch (FUNCT(i)) {
        case 0x30: c = bin(L, UAM_OP_SLE, b, a); break;
        case 0x31: c = bin(L, UAM_OP_ULE, b, a); break;
        case 0x32: c = bin(L, UAM_OP_SLT, a, b); break;
        case 0x33: c = bin(L, UAM_OP_ULT, a, b); break;
        case 0x34: c = bin(L, UAM_OP_EQ, a, b); break;
        default:   c = bin(L, UAM_OP_NE, a, b); break;
        }
        trap(L, c, EXC_TR, imm(L, 0));
        return LIFT_SEQ;
    }
    default:
        trap(L, NONE, EXC_RI, imm(L, L->raw));
        return LIFT_DONE;
    }
}

static lift_result lift_special2(ml *L, uint32_t i)
{
    switch (FUNCT(i)) {
    case 0x00: case 0x01: case 0x04: case 0x05: { /* MADD MADDU MSUB MSUBU (ac,) rs, rt */
        unsigned ac = acc_field(L, i, 11);
        if (ac == ~0u)
            return LIFT_DONE;
        mul_acc(L, ac, rs_r(L, i), rt_r(L, i), !(FUNCT(i) & 1), FUNCT(i) & 4);
        return LIFT_SEQ;
    }
    case 0x02: reg_w(L, RD(i), bin(L, UAM_OP_MUL, rs_r(L, i), rt_r(L, i))); return LIFT_SEQ; /* MUL */
    case 0x20: reg_w(L, RD(i), uam_unop(L->b, UAM_OP_CLZ, 32, rs_r(L, i))); return LIFT_SEQ; /* CLZ */
    case 0x21: reg_w(L, RD(i), uam_unop(L->b, UAM_OP_CLZ, 32, notv(L, rs_r(L, i)))); return LIFT_SEQ; /* CLO */
    default: trap(L, NONE, EXC_RI, imm(L, L->raw)); return LIFT_DONE;
    }
}

static lift_result lift_special3(ml *L, uint32_t i)
{
    switch (FUNCT(i)) {
    case 0x00: lift_ext(L, i); return LIFT_SEQ;
    case 0x04: lift_ins(L, i); return LIFT_SEQ;
    case 0x20: lift_bshfl(L, i); return LIFT_SEQ;
    case 0x3B: { /* RDHWR */
        unsigned hw = RD(i);
        if (hw == 2)
            reg_w(L, RT(i), uam_sysrd(L->b, 32, CP0_COUNT, NONE)); /* CC = cycle counter */
        else
            reg_w(L, RT(i), imm(L, hw == 1 ? 32 : hw == 3 ? 1 : 0)); /* SYNCI_Step / CCRes / 0 */
        return LIFT_SEQ;
    }
    default:
        if (mips_has_dsp(L->s))
            return mips_lift_dsp(L, i);
        trap(L, NONE, EXC_RI, imm(L, L->raw));
        return LIFT_DONE;
    }
}

/* REGIMM instructions that do not branch: the immediate traps, and the MCU ASE's
 * ASET/ACLR (atomic set/clear of one bit of a byte). */
static lift_result lift_regimm(ml *L, uint32_t i)
{
    unsigned rt = RT(i);
    if (rt >= 8 && rt <= 14 && rt != 13) { /* TGEI TGEIU TLTI TLTIU TEQI - TNEI */
        uam_ref a = rs_r(L, i), b = imm(L, SIMM(i)), c;
        switch (rt) {
        case 8:  c = bin(L, UAM_OP_SLE, b, a); break;
        case 9:  c = bin(L, UAM_OP_ULE, b, a); break;
        case 10: c = bin(L, UAM_OP_SLT, a, b); break;
        case 11: c = bin(L, UAM_OP_ULT, a, b); break;
        case 12: c = bin(L, UAM_OP_EQ, a, b); break;
        default: c = bin(L, UAM_OP_NE, a, b); break;
        }
        trap(L, c, EXC_TR, imm(L, 0));
        return LIFT_SEQ;
    }
    if (rt == 7 && mips_has_mcu(L->s)) {
        bool set = i & 0x8000u;
        uint32_t bit = 1u << ((i >> 12) & 7);
        uam_ref addr = bin(L, UAM_OP_ADD, rs_r(L, i), imm(L, (uint32_t)uam_sext(i & 0xFFF, 12)));
        uam_ref v = uam_ld(L->b, 8, L->endian, addr, NONE);
        v = set ? uam_binop(L->b, UAM_OP_OR, 8, v, imm(L, bit))
                : uam_binop(L->b, UAM_OP_AND, 8, v, imm(L, ~bit & 0xFF));
        uam_st(L->b, 8, L->endian, addr, v, NONE);
        return LIFT_SEQ;
    }
    trap(L, NONE, EXC_RI, imm(L, L->raw));
    return LIFT_DONE;
}

lift_result mips_lift32(ml *L, uint32_t i) { return lift_insn(L, i); }

static lift_result lift_insn(ml *L, uint32_t i)
{
    switch (OP(i)) {
    case 0x00: return lift_special(L, i);
    case 0x1C: return lift_special2(L, i);
    case 0x1F: return lift_special3(L, i);
    case 0x10: return lift_cop0(L, i);
    case 0x01: return lift_regimm(L, i);

    case 0x08: addi_trapping(L, i); return LIFT_SEQ; /* ADDI */
    case 0x09: reg_w(L, RT(i), bin(L, UAM_OP_ADD, rs_r(L, i), imm(L, SIMM(i)))); return LIFT_SEQ; /* ADDIU */
    case 0x0A: reg_w(L, RT(i), boolv(L, bin(L, UAM_OP_SLT, rs_r(L, i), imm(L, SIMM(i))))); return LIFT_SEQ; /* SLTI */
    case 0x0B: reg_w(L, RT(i), boolv(L, bin(L, UAM_OP_ULT, rs_r(L, i), imm(L, SIMM(i))))); return LIFT_SEQ; /* SLTIU */
    case 0x0C: reg_w(L, RT(i), bin(L, UAM_OP_AND, rs_r(L, i), imm(L, IMM(i)))); return LIFT_SEQ; /* ANDI */
    case 0x0D: reg_w(L, RT(i), bin(L, UAM_OP_OR, rs_r(L, i), imm(L, IMM(i)))); return LIFT_SEQ;  /* ORI */
    case 0x0E: reg_w(L, RT(i), bin(L, UAM_OP_XOR, rs_r(L, i), imm(L, IMM(i)))); return LIFT_SEQ; /* XORI */
    case 0x0F: reg_w(L, RT(i), imm(L, IMM(i) << 16)); return LIFT_SEQ;                           /* LUI */

    case 0x20: return lift_load(L, i, 8, true);    /* LB  */
    case 0x21: return lift_load(L, i, 16, true);   /* LH  */
    case 0x22: return lift_lwl(L, i, true);        /* LWL */
    case 0x23: return lift_load(L, i, 32, false);  /* LW  */
    case 0x24: return lift_load(L, i, 8, false);   /* LBU */
    case 0x25: return lift_load(L, i, 16, false);  /* LHU */
    case 0x26: return lift_lwl(L, i, false);       /* LWR */
    case 0x28: return lift_store(L, i, 8);         /* SB  */
    case 0x29: return lift_store(L, i, 16);        /* SH  */
    case 0x2A: return lift_swl(L, i, true);        /* SWL */
    case 0x2B: return lift_store(L, i, 32);        /* SW  */
    case 0x2E: return lift_swl(L, i, false);       /* SWR */
    case 0x2F: return LIFT_SEQ;                    /* CACHE: no-op */
    case 0x33: return LIFT_SEQ;                    /* PREF: no-op */

    case 0x30: { /* LL */
        reg_w(L, RT(i), uam_ld(L->b, 32, L->endian | UAM_ACC_ALIGNED | UAM_ACC_X, ea(L, i), NONE));
        return LIFT_SEQ;
    }
    case 0x38: { /* SC */
        uam_ref ok = uam_stx(L->b, 32, L->endian | UAM_ACC_ALIGNED, ea(L, i), rt_r(L, i), NONE);
        reg_w(L, RT(i), boolv(L, ok));
        return LIFT_SEQ;
    }

    case 0x11: case 0x13:                       /* COP1, COP1X (FPU) */
    case 0x31: case 0x35: case 0x39: case 0x3D: /* LWC1/LDC1/SWC1/SDC1 */
        if (mips_has_fpu(L->s))
            return mips_lift_fpu(L, i);
        trap(L, NONE, EXC_CPU, imm(L, 1));      /* tval = coprocessor number (Cause.CE) */
        return LIFT_DONE;
    case 0x12:                                  /* COP2 */
    case 0x32: case 0x36: case 0x3A: case 0x3E: /* LWC2/LDC2/SWC2/SDC2 */
        trap(L, NONE, EXC_CPU, imm(L, 2));
        return LIFT_DONE;

    default:
        trap(L, NONE, EXC_RI, imm(L, L->raw));
        return LIFT_DONE;
    }
}

uam_status mips_lift(uam_lift_ctx *lc)
{
    const mips_state *s = mips_st(lc->cpu);
    uint32_t pc = (uint32_t)lc->pc;
    if (pc & 1) {
        if (mips_has_m16(s))
            return mips16_lift(lc);
        if (mips_has_umips(s))
            return mips_umips_lift(lc);
        /* MIPS32 only: the fetch below fails (misaligned), an address error. */
    }
    ml L = { .lc = lc, .b = lc->b, .s = s, .endian = s->endian, .priv = lc->cpu->priv };

    if (mips_compressed(s) && ((pc & 2) || !mips_has_mips32(s))) {
        /* No MIPS32 instruction here: misaligned, or a microMIPS-only processor. */
        uam_imark(lc->b, pc, 4);
        uam_trap(lc->b, NONE, UAM_TRAP_EXCEPTION, uam_imm(lc->b, MIPS_TRAP_FETCH_ADDR),
                 uam_imm(lc->b, pc));
        return UAM_OK;
    }

    for (uint32_t n = 0;; n++) {
        if (n >= lc->max_insns) {
            uam_exit(lc->b, NONE, uam_imm(lc->b, pc), UAM_EXIT_JUMP);
            return UAM_OK;
        }

        uint64_t raw = 0;
        uam_status st = lc->fetch(lc, pc, 4, &raw);
        if (st != UAM_OK) {
            if (n == 0)
                return UAM_ERR_FAULT;
            uam_exit(lc->b, NONE, uam_imm(lc->b, pc), UAM_EXIT_JUMP);
            return UAM_OK;
        }
        uint32_t i = (uint32_t)raw;

        /* A branch needs its delay slot in the same block; defer to a fresh
         * block if only one slot of budget is left, or if the slot cannot be
         * fetched as part of this block (another page, or a fetch fault). A
         * block that starts at the branch reports a slot fault precisely. */
        bool ctl = is_control(s, i);
        if (ctl && n > 0) {
            uint64_t slot;
            if (n + 2 > lc->max_insns || lc->fetch(lc, pc + 4, 4, &slot) != UAM_OK) {
                uam_exit(lc->b, NONE, uam_imm(lc->b, pc), UAM_EXIT_JUMP);
                return UAM_OK;
            }
        }

        uam_imark(lc->b, pc, 4);
        L.pc = pc;
        L.npc = pc + 4;
        L.raw = i;
        lift_result r = ctl ? lift_control(&L, i) : lift_insn(&L, i);
        if (r == LIFT_DONE)
            return UAM_OK;
        pc += 4;
    }
}
