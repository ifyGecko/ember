/*
 * arm_thumb.c - Thumb (T16) and Thumb-2 (T32) decoders.
 *
 * Both lower to the emitters of arm_lift.c, so a Thumb instruction means exactly
 * what its ARM counterpart does. Thumb-specific rules handled here: 16-bit data
 * processing sets the flags only outside an IT block, the pc reads as the
 * instruction's address + 4 (word-aligned for literal loads and ADR), branches
 * stay in Thumb state except BX/BLX/LDR pc/POP pc, and IT, CBZ/CBNZ and TBB/TBH.
 */
#include "arm.h"

#define NONE UAM_REF_NONE
#define BITS(i, hi, lo) (((i) >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1))
#define BIT(i, n)       (((i) >> (n)) & 1u)

static uam_ref imm(arm_lift *L, uint32_t v) { return arm_imm(L, v); }
static uam_ref add(arm_lift *L, uam_ref a, uam_ref b) { return arm_bin(L, UAM_OP_ADD, a, b); }
static uam_ref sub(arm_lift *L, uam_ref a, uam_ref b) { return arm_bin(L, UAM_OP_SUB, a, b); }

/* Flags are set by 16-bit data processing only outside IT blocks. */
static bool t16_setflags(const arm_lift *L) { return L->it == 0; }

static arm_shres plain(uam_ref v) { return (arm_shres){ v, NONE }; }

static arm_lift_result t_branch_to(arm_lift *L, uint32_t target, uam_exit_kind kind)
{
    return arm_branch(L, imm(L, target | 1u), kind);
}

/* ---- T16 ---------------------------------------------------------------- */

static arm_lift_result t16_shift_add_sub_mov_cmp(arm_lift *L, uint32_t hw)
{
    unsigned op = BITS(hw, 13, 11);
    bool s = t16_setflags(L);
    if (op < 3) {                                   /* LSL/LSR/ASR #imm */
        unsigned rd = BITS(hw, 2, 0), rm = BITS(hw, 5, 3);
        arm_shres sh = arm_shift_imm(L, arm_reg(L, rm), op, BITS(hw, 10, 6));
        return arm_dp(L, DP_MOV, s, rd, imm(L, 0), sh);
    }
    if (op == 3) {                                  /* ADD/SUB register or imm3 */
        unsigned rd = BITS(hw, 2, 0), rn = BITS(hw, 5, 3);
        uam_ref b = BIT(hw, 10) ? imm(L, BITS(hw, 8, 6)) : arm_reg(L, BITS(hw, 8, 6));
        return arm_dp(L, BIT(hw, 9) ? DP_SUB : DP_ADD, s, rd, arm_reg(L, rn), plain(b));
    }
    unsigned rd = BITS(hw, 10, 8);
    uam_ref v = imm(L, BITS(hw, 7, 0));
    switch (op) {
    case 4: return arm_dp(L, DP_MOV, s, rd, imm(L, 0), plain(v));
    case 5: return arm_dp(L, DP_CMP, true, rd, arm_reg(L, rd), plain(v));
    case 6: return arm_dp(L, DP_ADD, s, rd, arm_reg(L, rd), plain(v));
    default: return arm_dp(L, DP_SUB, s, rd, arm_reg(L, rd), plain(v));
    }
}

static arm_lift_result t16_dp(arm_lift *L, uint32_t hw)
{
    unsigned op = BITS(hw, 9, 6), rdn = BITS(hw, 2, 0), rm = BITS(hw, 5, 3);
    bool s = t16_setflags(L);
    uam_ref a = arm_reg(L, rdn), b = arm_reg(L, rm);
    switch (op) {
    case 0x0: return arm_dp(L, DP_AND, s, rdn, a, plain(b));
    case 0x1: return arm_dp(L, DP_EOR, s, rdn, a, plain(b));
    case 0x2: return arm_dp(L, DP_MOV, s, rdn, imm(L, 0), arm_shift_reg(L, a, 0, b));
    case 0x3: return arm_dp(L, DP_MOV, s, rdn, imm(L, 0), arm_shift_reg(L, a, 1, b));
    case 0x4: return arm_dp(L, DP_MOV, s, rdn, imm(L, 0), arm_shift_reg(L, a, 2, b));
    case 0x5: return arm_dp(L, DP_ADC, s, rdn, a, plain(b));
    case 0x6: return arm_dp(L, DP_SBC, s, rdn, a, plain(b));
    case 0x7: return arm_dp(L, DP_MOV, s, rdn, imm(L, 0), arm_shift_reg(L, a, 3, b));
    case 0x8: return arm_dp(L, DP_TST, true, rdn, a, plain(b));
    case 0x9: return arm_dp(L, DP_RSB, s, rdn, b, plain(imm(L, 0)));   /* NEG: 0 - rm */
    case 0xA: return arm_dp(L, DP_CMP, true, rdn, a, plain(b));
    case 0xB: return arm_dp(L, DP_CMN, true, rdn, a, plain(b));
    case 0xC: return arm_dp(L, DP_ORR, s, rdn, a, plain(b));
    case 0xD: arm_mul(L, rdn, a, b, -1, false, s); return ARM_SEQ;
    case 0xE: return arm_dp(L, DP_BIC, s, rdn, a, plain(b));
    default:  return arm_dp(L, DP_MVN, s, rdn, imm(L, 0), plain(b));
    }
}

static arm_lift_result t16_special(arm_lift *L, uint32_t hw)
{
    unsigned op = BITS(hw, 9, 8);
    unsigned rd = (BIT(hw, 7) << 3) | BITS(hw, 2, 0), rm = BITS(hw, 6, 3);
    switch (op) {
    case 0: /* ADD (high registers), no flags; to pc: a branch */
        return arm_dp(L, DP_ADD, false, rd, arm_reg(L, rd), plain(arm_reg(L, rm)));
    case 1:
        return arm_dp(L, DP_CMP, true, rd, arm_reg(L, rd), plain(arm_reg(L, rm)));
    case 2:
        return arm_dp(L, DP_MOV, false, rd, imm(L, 0), plain(arm_reg(L, rm)));
    default: {
        uam_ref t = arm_bx_target(L, arm_reg(L, rm));
        if (BIT(hw, 7)) {                           /* BLX register */
            arm_put(L, ARM_LR, imm(L, arm_next_pc(L)));
            return arm_branch(L, t, UAM_EXIT_CALL);
        }
        return arm_branch(L, t, rm == ARM_LR ? UAM_EXIT_RET : UAM_EXIT_JUMP);
    }
    }
}

static arm_lift_result t16_ldst_reg(arm_lift *L, uint32_t hw)
{
    unsigned rt = BITS(hw, 2, 0);
    uam_ref addr = add(L, arm_reg(L, BITS(hw, 5, 3)), arm_reg(L, BITS(hw, 8, 6)));
    switch (BITS(hw, 11, 9)) {
    case 0: return arm_str(L, rt, addr, 4, -1, NONE);
    case 1: return arm_str(L, rt, addr, 2, -1, NONE);
    case 2: return arm_str(L, rt, addr, 1, -1, NONE);
    case 3: return arm_ldr(L, rt, addr, 1, true, -1, NONE);
    case 4: return arm_ldr(L, rt, addr, 4, false, -1, NONE);
    case 5: return arm_ldr(L, rt, addr, 2, false, -1, NONE);
    case 6: return arm_ldr(L, rt, addr, 1, false, -1, NONE);
    default: return arm_ldr(L, rt, addr, 2, true, -1, NONE);
    }
}

static arm_lift_result t16_misc(arm_lift *L, uint32_t hw)
{
    unsigned op = BITS(hw, 11, 5);
    if ((op & 0x7C) == 0x00)                        /* ADD sp, sp, #imm */
        return arm_dp(L, DP_ADD, false, ARM_SP, arm_reg(L, ARM_SP), plain(imm(L, BITS(hw, 6, 0) << 2)));
    if ((op & 0x7C) == 0x04)                        /* SUB sp, sp, #imm */
        return arm_dp(L, DP_SUB, false, ARM_SP, arm_reg(L, ARM_SP), plain(imm(L, BITS(hw, 6, 0) << 2)));
    if ((op & 0x28) == 0x08) {                      /* CBZ / CBNZ */
        uint32_t off = (BIT(hw, 9) << 6) | (BITS(hw, 7, 3) << 1);
        uam_ref z = arm_bin(L, BIT(hw, 11) ? UAM_OP_NE : UAM_OP_EQ, arm_reg(L, BITS(hw, 2, 0)), imm(L, 0));
        uam_exit(L->b, z, imm(L, (L->pc + 4 + off) | 1u), UAM_EXIT_JUMP);
        uam_exit(L->b, NONE, imm(L, arm_next_pc(L)), UAM_EXIT_JUMP);
        return ARM_DONE;
    }
    if ((op & 0x78) == 0x10) {                      /* SXTH SXTB UXTH UXTB */
        unsigned rd = BITS(hw, 2, 0);
        uam_ref m = arm_reg(L, BITS(hw, 5, 3));
        unsigned k = BITS(hw, 7, 6);
        arm_extend(L, rd, ARM_PC, m, 0, k < 2, (k & 1) ? 8 : 16);
        return ARM_SEQ;
    }
    if ((op & 0x70) == 0x20) {                      /* PUSH */
        uint32_t list = BITS(hw, 7, 0) | (BIT(hw, 8) << ARM_LR);
        return arm_stm(L, ARM_SP, list, false, true, true, false);
    }
    if (op == 0x32 && BIT(hw, 4) && !(hw & 7))      /* SETEND */
        return arm_setend(L, BIT(hw, 3));
    if (op == 0x33) {                               /* CPS */
        if (L->user)
            return ARM_SEQ;
        return arm_cps(L, BIT(hw, 4) ? 3 : 2, false, BITS(hw, 2, 0), 0);
    }
    if ((op & 0x78) == 0x50) {                      /* REV REV16 REVSH */
        unsigned k = BITS(hw, 7, 6), rd = BITS(hw, 2, 0);
        if (k == 2)
            return arm_undef(L);
        arm_rev(L, rd, arm_reg(L, BITS(hw, 5, 3)), k == 0 ? 0 : k == 1 ? 1 : 2);
        return ARM_SEQ;
    }
    if ((op & 0x70) == 0x60) {                      /* POP */
        uint32_t list = BITS(hw, 7, 0) | (BIT(hw, 8) << ARM_PC);
        return arm_ldm(L, ARM_SP, list, true, false, true, false);
    }
    if ((op & 0x78) == 0x70) {                      /* BKPT */
        arm_trap(L, ARM_EXC_PABT, ARM_FS_DEBUG);
        return ARM_DONE;
    }
    if ((op & 0x78) == 0x78) {                      /* IT and hints */
        if (BITS(hw, 3, 0)) {
            if (L->it)
                return arm_undef(L);                /* IT inside an IT block */
            L->it_next = (uint8_t)BITS(hw, 7, 0);
            return ARM_SEQ;
        }
        return arm_hint(L, BITS(hw, 7, 4));
    }
    return arm_undef(L);
}

arm_lift_result arm_lift_t16(arm_lift *L, uint32_t hw)
{
    switch (BITS(hw, 15, 12)) {
    case 0x0: case 0x1: case 0x2: case 0x3:
        return t16_shift_add_sub_mov_cmp(L, hw);
    case 0x4:
        if (BITS(hw, 11, 10) == 0)
            return t16_dp(L, hw);
        if (BITS(hw, 11, 10) == 1)
            return t16_special(L, hw);
        /* LDR literal */
        return arm_ldr(L, BITS(hw, 10, 8), imm(L, ((L->pc + 4) & ~3u) + (BITS(hw, 7, 0) << 2)), 4,
                       false, -1, NONE);
    case 0x5:
        return t16_ldst_reg(L, hw);
    case 0x6: case 0x7: case 0x8: {                 /* STR/LDR (B, H) immediate */
        unsigned rt = BITS(hw, 2, 0), op = BITS(hw, 15, 11);
        uam_ref base = arm_reg(L, BITS(hw, 5, 3));
        unsigned off = BITS(hw, 10, 6);
        bool l = BIT(hw, 11);
        unsigned size = op < 0x0E ? 4 : op < 0x10 ? 1 : 2;
        uam_ref addr = add(L, base, imm(L, off * size));
        return l ? arm_ldr(L, rt, addr, size, false, -1, NONE) : arm_str(L, rt, addr, size, -1, NONE);
    }
    case 0x9: {                                     /* STR/LDR sp-relative */
        unsigned rt = BITS(hw, 10, 8);
        uam_ref addr = add(L, arm_reg(L, ARM_SP), imm(L, BITS(hw, 7, 0) << 2));
        return BIT(hw, 11) ? arm_ldr(L, rt, addr, 4, false, -1, NONE) : arm_str(L, rt, addr, 4, -1, NONE);
    }
    case 0xA: {                                     /* ADR / ADD rd, sp, #imm */
        unsigned rd = BITS(hw, 10, 8);
        uint32_t off = BITS(hw, 7, 0) << 2;
        if (BIT(hw, 11))
            arm_put(L, rd, add(L, arm_reg(L, ARM_SP), imm(L, off)));
        else
            arm_put(L, rd, imm(L, ((L->pc + 4) & ~3u) + off));
        return ARM_SEQ;
    }
    case 0xB:
        return t16_misc(L, hw);
    case 0xC: {                                     /* STM / LDM */
        unsigned rn = BITS(hw, 10, 8);
        uint32_t list = BITS(hw, 7, 0);
        if (BIT(hw, 11))
            return arm_ldm(L, rn, list, true, false, !(list & (1u << rn)), false);
        return arm_stm(L, rn, list, true, false, true, false);
    }
    case 0xD: {
        unsigned cond = BITS(hw, 11, 8);
        if (cond == 0xE)
            return arm_undef(L);                    /* UDF */
        if (cond == 0xF) {
            arm_trap(L, ARM_EXC_SVC, BITS(hw, 7, 0));
            return ARM_DONE;
        }
        if (L->it)
            return arm_undef(L);                    /* B<cond> inside an IT block */
        uint32_t target = L->pc + 4 + ((uint32_t)uam_sext(BITS(hw, 7, 0), 8) << 1);
        L->guard = arm_cond(L, cond);
        return t_branch_to(L, target, UAM_EXIT_JUMP);
    }
    default: {                                      /* 0xE: B */
        uint32_t target = L->pc + 4 + ((uint32_t)uam_sext(BITS(hw, 10, 0), 11) << 1);
        return t_branch_to(L, target, UAM_EXIT_JUMP);
    }
    }
}

/* ---- T32 ---------------------------------------------------------------- */

/* ThumbExpandImm_C: the value, and the carry out (NONE when C is unchanged). */
static arm_shres t32_modimm(arm_lift *L, uint32_t i)
{
    uint32_t imm12 = (BIT(i, 26) << 11) | (BITS(i, 14, 12) << 8) | BITS(i, 7, 0);
    uint32_t b = imm12 & 0xFF, v;
    if ((imm12 >> 10) == 0) {
        switch ((imm12 >> 8) & 3) {
        case 0: v = b; break;
        case 1: v = (b << 16) | b; break;
        case 2: v = (b << 24) | (b << 8); break;
        default: v = (b << 24) | (b << 16) | (b << 8) | b; break;
        }
        return (arm_shres){ imm(L, v), NONE };
    }
    unsigned rot = imm12 >> 7;
    uint32_t x = 0x80 | (imm12 & 0x7F);
    v = (x >> rot) | (x << (32 - rot));
    return (arm_shres){ imm(L, v), imm(L, v >> 31) };
}

static arm_lift_result t32_dp(arm_lift *L, uint32_t i, arm_shres b)
{
    unsigned op = BITS(i, 24, 21), rn = BITS(i, 19, 16), rd = BITS(i, 11, 8);
    bool s = BIT(i, 20);
    switch (op) {
    case 0x0:
        if (rd == 15 && s) return arm_dp(L, DP_TST, true, 0, arm_reg(L, rn), b);
        return arm_dp(L, DP_AND, s, rd, arm_reg(L, rn), b);
    case 0x1: return arm_dp(L, DP_BIC, s, rd, arm_reg(L, rn), b);
    case 0x2:
        if (rn == 15) return arm_dp(L, DP_MOV, s, rd, imm(L, 0), b);
        return arm_dp(L, DP_ORR, s, rd, arm_reg(L, rn), b);
    case 0x3:
        if (rn == 15) return arm_dp(L, DP_MVN, s, rd, imm(L, 0), b);
        return arm_dp(L, DP_ORN, s, rd, arm_reg(L, rn), b);
    case 0x4:
        if (rd == 15 && s) return arm_dp(L, DP_TEQ, true, 0, arm_reg(L, rn), b);
        return arm_dp(L, DP_EOR, s, rd, arm_reg(L, rn), b);
    case 0x8:
        if (rd == 15 && s) return arm_dp(L, DP_CMN, true, 0, arm_reg(L, rn), b);
        return arm_dp(L, DP_ADD, s, rd, arm_reg(L, rn), b);
    case 0xA: return arm_dp(L, DP_ADC, s, rd, arm_reg(L, rn), b);
    case 0xB: return arm_dp(L, DP_SBC, s, rd, arm_reg(L, rn), b);
    case 0xD:
        if (rd == 15 && s) return arm_dp(L, DP_CMP, true, 0, arm_reg(L, rn), b);
        return arm_dp(L, DP_SUB, s, rd, arm_reg(L, rn), b);
    case 0xE: return arm_dp(L, DP_RSB, s, rd, arm_reg(L, rn), b);
    default: return arm_undef(L);
    }
}

static arm_lift_result t32_plain_imm(arm_lift *L, uint32_t i)
{
    unsigned op = BITS(i, 24, 20), rn = BITS(i, 19, 16), rd = BITS(i, 11, 8);
    uint32_t imm12 = (BIT(i, 26) << 11) | (BITS(i, 14, 12) << 8) | BITS(i, 7, 0);
    uint32_t imm16 = (BITS(i, 19, 16) << 12) | imm12;
    unsigned lsb = (BITS(i, 14, 12) << 2) | BITS(i, 7, 6), w = BITS(i, 4, 0);
    switch (op) {
    case 0x00:                                      /* ADDW / ADR */
        if (rn == 15) { arm_put(L, rd, imm(L, ((L->pc + 4) & ~3u) + imm12)); return ARM_SEQ; }
        return arm_dp(L, DP_ADD, false, rd, arm_reg(L, rn), plain(imm(L, imm12)));
    case 0x0A:                                      /* SUBW / ADR */
        if (rn == 15) { arm_put(L, rd, imm(L, ((L->pc + 4) & ~3u) - imm12)); return ARM_SEQ; }
        return arm_dp(L, DP_SUB, false, rd, arm_reg(L, rn), plain(imm(L, imm12)));
    case 0x04: arm_put(L, rd, imm(L, imm16)); return ARM_SEQ;   /* MOVW */
    case 0x0C:                                                  /* MOVT */
        arm_put(L, rd, arm_bin(L, UAM_OP_OR, arm_bin(L, UAM_OP_AND, arm_reg(L, rd), imm(L, 0xFFFF)),
                               imm(L, imm16 << 16)));
        return ARM_SEQ;
    case 0x10: case 0x12: case 0x18: case 0x1A: {              /* SSAT / USAT */
        bool usat = op >= 0x18;
        unsigned sh = BIT(i, 21) ? 2 : 0;
        if (sh == 2 && lsb == 0)
            return arm_undef(L);                                /* SSAT16 / USAT16 */
        arm_shres v = arm_shift_imm(L, arm_reg(L, rn), sh, lsb);
        arm_sat(L, rd, v.v, usat ? w : w + 1, usat);
        return ARM_SEQ;
    }
    case 0x14: arm_bfx(L, rd, arm_reg(L, rn), lsb, w + 1, true); return ARM_SEQ;   /* SBFX */
    case 0x1C: arm_bfx(L, rd, arm_reg(L, rn), lsb, w + 1, false); return ARM_SEQ;  /* UBFX */
    case 0x16:                                                                     /* BFI/BFC */
        arm_bfi(L, rd, rn == 15 ? imm(L, 0) : arm_reg(L, rn), lsb, w);
        return ARM_SEQ;
    default:
        return arm_undef(L);
    }
}

static arm_lift_result t32_branch_misc(arm_lift *L, uint32_t i)
{
    unsigned op1 = BITS(i, 14, 12), op = BITS(i, 26, 20);
    uint32_t s = BIT(i, 26), j1 = BIT(i, 13), j2 = BIT(i, 11);
    if (op1 & 1 || op1 == 4 || op1 == 6) {         /* B.W, BL, BLX */
        uint32_t i1 = !(j1 ^ s), i2 = !(j2 ^ s);
        uint32_t off = (s << 24) | (i1 << 23) | (i2 << 22) | (BITS(i, 25, 16) << 12) | (BITS(i, 10, 0) << 1);
        uint32_t target = L->pc + 4 + (uint32_t)uam_sext(off, 25);
        if (op1 == 1 || op1 == 3)                   /* B.W */
            return t_branch_to(L, target, UAM_EXIT_JUMP);
        arm_put(L, ARM_LR, imm(L, arm_next_pc(L)));
        if (op1 & 1)                                /* BL */
            return t_branch_to(L, target, UAM_EXIT_CALL);
        if (BIT(i, 0))
            return arm_undef(L);
        return arm_branch(L, imm(L, (L->pc + 4 + (uint32_t)uam_sext(off, 25)) & ~3u), UAM_EXIT_CALL); /* BLX to ARM */
    }
    if ((op & 0x38) != 0x38) {                      /* B<cond>.W */
        if (L->it)
            return arm_undef(L);
        uint32_t off = (s << 20) | (j2 << 19) | (j1 << 18) | (BITS(i, 21, 16) << 12) | (BITS(i, 10, 0) << 1);
        L->guard = arm_cond(L, BITS(i, 25, 22));
        return t_branch_to(L, L->pc + 4 + (uint32_t)uam_sext(off, 21), UAM_EXIT_JUMP);
    }
    switch (op) {
    case 0x38: case 0x39:                           /* MSR */
        if (BIT(i, 5))
            return arm_undef(L);
        return arm_msr(L, arm_reg(L, BITS(i, 19, 16)), BITS(i, 11, 8), BIT(i, 20));
    case 0x3A:                                      /* hints, CPS */
        if (BITS(i, 10, 8) == 0)
            return arm_hint(L, BITS(i, 7, 0) >= 0xF0 ? 0 : BITS(i, 7, 0));
        if (L->user)
            return ARM_SEQ;
        return arm_cps(L, BITS(i, 10, 9), BIT(i, 8), BITS(i, 7, 5), BITS(i, 4, 0));
    case 0x3B:                                      /* CLREX DSB DMB ISB */
        return arm_barrier(L, BITS(i, 7, 4) == 2 ? 1 : BITS(i, 7, 4));
    case 0x3C:                                      /* BXJ */
        return arm_bx(L, arm_reg(L, BITS(i, 19, 16)), UAM_EXIT_JUMP);
    case 0x3D:                                      /* SUBS pc, lr, #imm */
        return arm_dp(L, DP_SUB, true, ARM_PC, arm_reg(L, ARM_LR), plain(imm(L, BITS(i, 7, 0))));
    case 0x3E: case 0x3F:                           /* MRS */
        if (BIT(i, 5))
            return arm_undef(L);
        arm_mrs(L, BITS(i, 11, 8), BIT(i, 20));
        return ARM_SEQ;
    default:
        return arm_undef(L);
    }
}

/* LDM/STM/SRS/RFE (op 00 bits 26:25, bit 22 = 0). */
static arm_lift_result t32_ldm(arm_lift *L, uint32_t i)
{
    unsigned op = BITS(i, 24, 23), rn = BITS(i, 19, 16);
    bool l = BIT(i, 20), w = BIT(i, 21);
    if (op == 0 || op == 3) {                       /* SRS / RFE */
        bool inc = op == 3;
        if (l)
            return arm_rfe(L, rn, inc, !inc, w);
        return arm_srs(L, BITS(i, 4, 0), inc, !inc, w);
    }
    bool inc = op == 1;
    uint32_t list = BITS(i, 15, 0);
    if (l)
        return arm_ldm(L, rn, list, inc, !inc, w, false);
    return arm_stm(L, rn, list, inc, !inc, w, false);
}

/* Load/store dual, exclusive, table branch (bits 26:25 = 00, bit 22 = 1). */
static arm_lift_result t32_dual(arm_lift *L, uint32_t i)
{
    unsigned op1 = BITS(i, 24, 23), op2 = BITS(i, 21, 20), op3 = BITS(i, 7, 4);
    unsigned rn = BITS(i, 19, 16), rt = BITS(i, 15, 12), rt2 = BITS(i, 11, 8);
    if (op1 == 0 && op2 == 0)                       /* STREX */
        return arm_strex(L, rt2, rt, 0, add(L, arm_reg(L, rn), imm(L, BITS(i, 7, 0) << 2)), 4);
    if (op1 == 0 && op2 == 1)                       /* LDREX */
        return arm_ldrex(L, rt, 0, add(L, arm_reg(L, rn), imm(L, BITS(i, 7, 0) << 2)), 4);
    if (op1 == 1 && op2 == 0) {                     /* STREXB/H/D */
        unsigned rd = BITS(i, 3, 0);
        switch (op3) {
        case 4: return arm_strex(L, rd, rt, 0, arm_reg(L, rn), 1);
        case 5: return arm_strex(L, rd, rt, 0, arm_reg(L, rn), 2);
        case 7: return arm_strex(L, rd, rt, rt2, arm_reg(L, rn), 8);
        default: return arm_undef(L);
        }
    }
    if (op1 == 1 && op2 == 1) {                     /* TBB/TBH, LDREXB/H/D */
        switch (op3) {
        case 0: case 1: {
            bool h = op3 == 1;
            uam_ref idx = arm_reg(L, BITS(i, 3, 0));
            if (h)
                idx = arm_bin(L, UAM_OP_SHL, idx, imm(L, 1));
            uam_ref v = uam_ld(L->b, h ? 16 : 8, L->endian, add(L, arm_reg(L, rn), idx), L->guard);
            v = uam_unop(L->b, UAM_OP_ZEXT, 32, v);
            uam_ref t = add(L, imm(L, (L->pc + 4) | 1u), arm_bin(L, UAM_OP_SHL, v, imm(L, 1)));
            return arm_branch(L, t, UAM_EXIT_JUMP);
        }
        case 4: return arm_ldrex(L, rt, 0, arm_reg(L, rn), 1);
        case 5: return arm_ldrex(L, rt, 0, arm_reg(L, rn), 2);
        case 7: return arm_ldrex(L, rt, rt2, arm_reg(L, rn), 8);
        default: return arm_undef(L);
        }
    }
    /* LDRD / STRD immediate */
    bool p = BIT(i, 24), u = BIT(i, 23), w = BIT(i, 21), l = BIT(i, 20);
    uam_ref base = rn == 15 ? imm(L, (L->pc + 4) & ~3u) : arm_reg(L, rn);
    uam_ref off = imm(L, BITS(i, 7, 0) << 2);
    uam_ref oa = u ? add(L, base, off) : sub(L, base, off);
    uam_ref addr = p ? oa : base;
    int wb = w ? (int)rn : -1;
    return l ? arm_ldrd(L, rt, rt2, addr, wb, oa) : arm_strd(L, rt, rt2, addr, wb, oa);
}

/* Data processing (shifted register). */
static arm_lift_result t32_dp_reg(arm_lift *L, uint32_t i)
{
    unsigned type = BITS(i, 5, 4), amt = (BITS(i, 14, 12) << 2) | BITS(i, 7, 6);
    unsigned op = BITS(i, 24, 21);
    if (op == 6) {                                  /* PKHBT / PKHTB */
        arm_pkh(L, BITS(i, 11, 8), arm_reg(L, BITS(i, 19, 16)), arm_reg(L, BITS(i, 3, 0)),
                BIT(i, 5), amt);
        return ARM_SEQ;
    }
    arm_shres b = arm_shift_imm(L, arm_reg(L, BITS(i, 3, 0)), type, amt);
    return t32_dp(L, i, b);
}

/* Single loads and stores (bits 31:25 = 1111100). */
static arm_lift_result t32_ldst(arm_lift *L, uint32_t i)
{
    unsigned size_bits = BITS(i, 22, 21), rn = BITS(i, 19, 16), rt = BITS(i, 15, 12);
    bool l = BIT(i, 20), sign = BIT(i, 24);
    if (size_bits == 3)
        return arm_undef(L);
    unsigned size = 1u << size_bits;
    if (l && rt == 15 && size < 4)
        return ARM_SEQ;                             /* PLD / PLI */
    uam_ref addr, wbv = NONE;
    int wb = -1;
    if (rn == 15) {                                 /* literal */
        uint32_t base = (L->pc + 4) & ~3u, off = BITS(i, 11, 0);
        addr = imm(L, BIT(i, 23) ? base + off : base - off);
    } else if (BIT(i, 23)) {                        /* imm12 */
        addr = add(L, arm_reg(L, rn), imm(L, BITS(i, 11, 0)));
    } else if (BITS(i, 11, 6) == 0) {               /* register, LSL #0-3 */
        addr = add(L, arm_reg(L, rn), arm_bin(L, UAM_OP_SHL, arm_reg(L, BITS(i, 3, 0)), imm(L, BITS(i, 5, 4))));
    } else if (BIT(i, 11)) {                        /* imm8 with P/U/W (and T) */
        bool p = BIT(i, 10), u = BIT(i, 9), w = BIT(i, 8);
        uam_ref base = arm_reg(L, rn), off = imm(L, BITS(i, 7, 0));
        uam_ref oa = u ? add(L, base, off) : sub(L, base, off);
        addr = p ? oa : base;
        if (!p || w) {
            if (p && u && !w) { /* unprivileged LDRT/STRT: no writeback */ }
            else { wb = (int)rn; wbv = oa; }
        }
    } else {
        return arm_undef(L);
    }
    if (!l) {
        if (sign)
            return arm_undef(L);
        return arm_str(L, rt, addr, size, wb, wbv);
    }
    return arm_ldr(L, rt, addr, size, sign && size < 4, wb, wbv);
}

/* Data processing (register): shifts, extends, parallel, misc. */
static arm_lift_result t32_dp_misc(arm_lift *L, uint32_t i)
{
    unsigned op1 = BITS(i, 23, 20), op2 = BITS(i, 7, 4);
    unsigned rn = BITS(i, 19, 16), rd = BITS(i, 11, 8), rm = BITS(i, 3, 0);
    if (op1 < 8 && op2 == 0) {                      /* LSL/LSR/ASR/ROR register */
        arm_shres sh = arm_shift_reg(L, arm_reg(L, rn), BITS(i, 22, 21), arm_reg(L, rm));
        return arm_dp(L, DP_MOV, BIT(i, 20), rd, imm(L, 0), sh);
    }
    if (op1 < 8 && (op2 & 8)) {                     /* extends */
        static const unsigned from[] = { 16, 16, 0, 0, 8, 8 };
        unsigned k = BITS(i, 22, 20);
        if (k > 5)
            return arm_undef(L);
        arm_extend(L, rd, rn, arm_reg(L, rm), BITS(i, 5, 4) * 8, !(k & 1), from[k]);
        return ARM_SEQ;
    }
    if (op1 >= 8 && !(op2 & 8)) {                  /* parallel add/subtract */
        /* T32 bits 22:20 to the A32 op2 numbering (5: unallocated) */
        static const unsigned a32op2[] = { 4, 0, 1, 5, 7, 3, 2, 5 };
        unsigned kind = BITS(i, 5, 4);
        if (kind == 3)
            return arm_undef(L);
        unsigned a32op1 = (BIT(i, 6) ? 4 : 0) + 1 + kind;
        if (!arm_parallel(L, rd, arm_reg(L, rn), arm_reg(L, rm), a32op1, a32op2[BITS(i, 22, 20)]))
            return arm_undef(L);
        return ARM_SEQ;
    }
    if ((op1 & 0xC) == 0x8 && (op2 & 0xC) == 0x8) { /* misc */
        unsigned k = (BITS(i, 21, 20) << 2) | BITS(i, 5, 4);
        uam_ref m = arm_reg(L, rm);
        switch (k) {
        case 0x0: arm_qaddsub(L, rd, m, arm_reg(L, rn), false, false); return ARM_SEQ;
        case 0x1: arm_qaddsub(L, rd, m, arm_reg(L, rn), false, true); return ARM_SEQ;
        case 0x2: arm_qaddsub(L, rd, m, arm_reg(L, rn), true, false); return ARM_SEQ;
        case 0x3: arm_qaddsub(L, rd, m, arm_reg(L, rn), true, true); return ARM_SEQ;
        case 0x4: arm_rev(L, rd, m, 0); return ARM_SEQ;
        case 0x5: arm_rev(L, rd, m, 1); return ARM_SEQ;
        case 0x6: arm_rev(L, rd, m, 3); return ARM_SEQ;
        case 0x7: arm_rev(L, rd, m, 2); return ARM_SEQ;
        case 0x8: arm_sel(L, rd, arm_reg(L, rn), m); return ARM_SEQ;
        case 0xC: arm_put(L, rd, uam_unop(L->b, UAM_OP_CLZ, 32, m)); return ARM_SEQ;
        default: return arm_undef(L);
        }
    }
    return arm_undef(L);
}

/* Multiply, multiply-accumulate, absolute difference (bits 26:23 = 0110). */
static arm_lift_result t32_mul(arm_lift *L, uint32_t i)
{
    unsigned op1 = BITS(i, 22, 20), op2 = BITS(i, 5, 4);
    unsigned rn = BITS(i, 19, 16), ra = BITS(i, 15, 12), rd = BITS(i, 11, 8), rm = BITS(i, 3, 0);
    uam_ref a = arm_reg(L, rn), b = arm_reg(L, rm);
    int acc = ra == 15 ? -1 : (int)ra;
    switch (op1) {
    case 0:
        if (op2 == 0) { arm_mul(L, rd, a, b, acc, false, false); return ARM_SEQ; }
        if (op2 == 1) { arm_mul(L, rd, a, b, (int)ra, true, false); return ARM_SEQ; }
        break;
    case 1: arm_mulxy(L, rd, a, b, BIT(i, 5), BIT(i, 4), acc); return ARM_SEQ;
    case 2:                                         /* SMLAD / SMUAD */
    case 4:                                         /* SMLSD / SMUSD */
        if (op2 < 2) { arm_dual_mul(L, rd, ra, rm, rn, BIT(i, 4), op1 == 4, false); return ARM_SEQ; }
        break;
    case 3:
        if (op2 < 2) { arm_mulwy(L, rd, a, b, BIT(i, 4), acc); return ARM_SEQ; }
        break;
    case 5:
        if (op2 < 2) { arm_mmul(L, rd, a, b, acc, false, BIT(i, 4)); return ARM_SEQ; }
        break;
    case 6:
        if (op2 < 2) { arm_mmul(L, rd, a, b, (int)ra, true, BIT(i, 4)); return ARM_SEQ; }
        break;
    case 7:                                         /* USAD8 / USADA8 */
        if (op2 == 0) { arm_usad8(L, rd, a, b, ra); return ARM_SEQ; }
        break;
    default:
        break;
    }
    return arm_undef(L);
}

/* Long multiply, divide (bits 26:23 = 0111). */
static arm_lift_result t32_lmul(arm_lift *L, uint32_t i)
{
    unsigned op1 = BITS(i, 22, 20), op2 = BITS(i, 7, 4);
    unsigned rn = BITS(i, 19, 16), rdlo = BITS(i, 15, 12), rdhi = BITS(i, 11, 8), rm = BITS(i, 3, 0);
    uam_ref a = arm_reg(L, rn), b = arm_reg(L, rm);
    switch (op1) {
    case 0: if (op2 == 0) { arm_mull(L, rdlo, rdhi, a, b, true, false, false); return ARM_SEQ; } break;
    case 1: if (op2 == 0xF) { arm_div(L, rdhi, a, b, true); return ARM_SEQ; } break;
    case 2: if (op2 == 0) { arm_mull(L, rdlo, rdhi, a, b, false, false, false); return ARM_SEQ; } break;
    case 3: if (op2 == 0xF) { arm_div(L, rdhi, a, b, false); return ARM_SEQ; } break;
    case 4:
        if (op2 == 0) { arm_mull(L, rdlo, rdhi, a, b, true, true, false); return ARM_SEQ; }
        if ((op2 & 0xE) == 0xC) { arm_dual_mul(L, rdhi, rdlo, rm, rn, op2 & 1, false, true); return ARM_SEQ; }
        break;
    case 5:
        if ((op2 & 0xE) == 0xC) { arm_dual_mul(L, rdhi, rdlo, rm, rn, op2 & 1, true, true); return ARM_SEQ; }
        break;
    case 6:
        if (op2 == 0) { arm_mull(L, rdlo, rdhi, a, b, false, true, false); return ARM_SEQ; }
        if (op2 == 6) { arm_umaal(L, rdlo, rdhi, a, b); return ARM_SEQ; }
        break;
    default: break;
    }
    return arm_undef(L);
}

static arm_lift_result t32_coproc(arm_lift *L, uint32_t i)
{
    unsigned cp = BITS(i, 11, 8);
    if (BITS(i, 25, 21) == 0x02)                    /* MCRR / MRRC */
        return arm_coproc64(L, BIT(i, 20), cp, BITS(i, 7, 4), BITS(i, 3, 0), BITS(i, 15, 12),
                            BITS(i, 19, 16));
    if (BITS(i, 25, 24) == 2 && BIT(i, 4))          /* MCR / MRC */
        return arm_coproc(L, BIT(i, 20), cp, BITS(i, 23, 21), BITS(i, 19, 16), BITS(i, 3, 0),
                          BITS(i, 7, 5), BITS(i, 15, 12));
    return arm_undef(L);
}

arm_lift_result arm_lift_t32(arm_lift *L, uint32_t i)
{
    unsigned op1 = BITS(i, 28, 27), op2 = BITS(i, 26, 20);
    if (op1 == 1) {
        if ((op2 & 0x64) == 0x00) return t32_ldm(L, i);
        if ((op2 & 0x64) == 0x04) return t32_dual(L, i);
        if ((op2 & 0x60) == 0x20) return t32_dp_reg(L, i);
        return t32_coproc(L, i);
    }
    if (op1 == 2) {
        if (!BIT(i, 15)) {
            if (!BIT(i, 25))
                return t32_dp(L, i, t32_modimm(L, i));
            return t32_plain_imm(L, i);
        }
        return t32_branch_misc(L, i);
    }
    if (op1 == 3) {
        if ((op2 & 0x70) == 0x00 || (op2 & 0x70) == 0x10)
            return t32_ldst(L, i);
        if ((op2 & 0x70) == 0x20)
            return t32_dp_misc(L, i);
        if ((op2 & 0x78) == 0x30)
            return t32_mul(L, i);
        if ((op2 & 0x78) == 0x38)
            return t32_lmul(L, i);
        return t32_coproc(L, i);
    }
    return arm_undef(L);
}
