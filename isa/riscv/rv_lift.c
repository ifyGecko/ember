/*
 * rv_lift.c - RISC-V RV32IMAC(+Zicsr, Zifencei) decoder and lifter.
 *
 * Compressed instructions are first expanded to their 32-bit equivalents
 * (rv_expand_compressed), so there is exactly one lifter per operation.
 * Lifting follows docs/uam-isa.md: x0 reads fold to the constant 0, x0
 * writes are dropped, and every instruction performs its faulting operations
 * before its register writes (commit rule).
 */
#include "rv.h"

/* ------------------------------------------------------------------------ */
/* Instruction fields                                                        */
/* ------------------------------------------------------------------------ */

#define OPC(i)    ((i) & 0x7F)
#define RD(i)     (((i) >> 7) & 0x1F)
#define FUNCT3(i) (((i) >> 12) & 0x7)
#define RS1(i)    (((i) >> 15) & 0x1F)
#define RS2(i)    (((i) >> 20) & 0x1F)
#define FUNCT7(i) ((i) >> 25)

static int32_t imm_i(uint32_t i) { return (int32_t)i >> 20; }
static int32_t imm_s(uint32_t i) { return (int32_t)(((int32_t)i >> 25) * 32) | (int32_t)((i >> 7) & 0x1F); }
static int32_t imm_b(uint32_t i)
{
    uint32_t v = ((i >> 31) << 12) | (((i >> 7) & 1) << 11) | (((i >> 25) & 0x3F) << 5)
               | (((i >> 8) & 0xF) << 1);
    return (int32_t)uam_sext(v, 13);
}
static int32_t imm_j(uint32_t i)
{
    uint32_t v = ((i >> 31) << 20) | (i & 0xFF000) | (((i >> 20) & 1) << 11)
               | (((i >> 21) & 0x3FF) << 1);
    return (int32_t)uam_sext(v, 21);
}

/* ------------------------------------------------------------------------ */
/* Compressed expansion (RV32C)                                              */
/* ------------------------------------------------------------------------ */

static uint32_t enc_r(uint32_t op, uint32_t rd, uint32_t f3, uint32_t rs1, uint32_t rs2, uint32_t f7)
{
    return op | rd << 7 | f3 << 12 | rs1 << 15 | rs2 << 20 | f7 << 25;
}
static uint32_t enc_i(uint32_t op, uint32_t rd, uint32_t f3, uint32_t rs1, int32_t imm)
{
    return op | rd << 7 | f3 << 12 | rs1 << 15 | ((uint32_t)imm & 0xFFF) << 20;
}
static uint32_t enc_s(uint32_t op, uint32_t f3, uint32_t rs1, uint32_t rs2, int32_t imm)
{
    uint32_t u = (uint32_t)imm;
    return op | (u & 0x1F) << 7 | f3 << 12 | rs1 << 15 | rs2 << 20 | ((u >> 5) & 0x7F) << 25;
}
static uint32_t enc_b(uint32_t f3, uint32_t rs1, uint32_t rs2, int32_t imm)
{
    uint32_t u = (uint32_t)imm;
    return 0x63 | ((u >> 11) & 1) << 7 | ((u >> 1) & 0xF) << 8 | f3 << 12 | rs1 << 15 | rs2 << 20
         | ((u >> 5) & 0x3F) << 25 | ((u >> 12) & 1) << 31;
}
static uint32_t enc_j(uint32_t rd, int32_t imm)
{
    uint32_t u = (uint32_t)imm;
    return 0x6F | rd << 7 | (u & 0xFF000) | ((u >> 11) & 1) << 20 | ((u >> 1) & 0x3FF) << 21
         | ((u >> 20) & 1) << 31;
}

#define CB(c, hi, lo) (((uint32_t)(c) >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1))

/* Offset of C.J / C.JAL. */
static int32_t cj_off(uint16_t c)
{
    uint32_t v = CB(c, 12, 12) << 11 | CB(c, 11, 11) << 4 | CB(c, 10, 9) << 8 | CB(c, 8, 8) << 10
               | CB(c, 7, 7) << 6 | CB(c, 6, 6) << 7 | CB(c, 5, 3) << 1 | CB(c, 2, 2) << 5;
    return (int32_t)uam_sext(v, 12);
}

/* Offset of C.BEQZ / C.BNEZ. */
static int32_t cb_off(uint16_t c)
{
    uint32_t v = CB(c, 12, 12) << 8 | CB(c, 11, 10) << 3 | CB(c, 6, 5) << 6 | CB(c, 4, 3) << 1
               | CB(c, 2, 2) << 5;
    return (int32_t)uam_sext(v, 9);
}

uint32_t rv_expand_compressed(uint16_t c)
{
    const uint32_t f3 = CB(c, 15, 13);
    const uint32_t rd = CB(c, 11, 7);         /* full register fields */
    const uint32_t rs2 = CB(c, 6, 2);
    const uint32_t rdp = 8 + CB(c, 4, 2);     /* compressed register fields */
    const uint32_t rs1p = 8 + CB(c, 9, 7);
    const int32_t imm6 = (int32_t)uam_sext(CB(c, 12, 12) << 5 | CB(c, 6, 2), 6);

    switch (c & 3) {
    case 0:
        switch (f3) {
        case 0: { /* C.ADDI4SPN */
            uint32_t nz = CB(c, 12, 11) << 4 | CB(c, 10, 7) << 6 | CB(c, 6, 6) << 2 | CB(c, 5, 5) << 3;
            return nz ? enc_i(0x13, rdp, 0, 2, (int32_t)nz) : 0;
        }
        case 2: { /* C.LW */
            uint32_t off = CB(c, 12, 10) << 3 | CB(c, 6, 6) << 2 | CB(c, 5, 5) << 6;
            return enc_i(0x03, rdp, 2, rs1p, (int32_t)off);
        }
        case 6: { /* C.SW */
            uint32_t off = CB(c, 12, 10) << 3 | CB(c, 6, 6) << 2 | CB(c, 5, 5) << 6;
            return enc_s(0x23, 2, rs1p, rdp, (int32_t)off);
        }
        default:
            return 0; /* floating point or reserved */
        }

    case 1:
        switch (f3) {
        case 0: /* C.ADDI / C.NOP */
            return enc_i(0x13, rd, 0, rd, imm6);
        case 1: /* C.JAL (RV32) */
            return enc_j(1, cj_off(c));
        case 2: /* C.LI */
            return enc_i(0x13, rd, 0, 0, imm6);
        case 3:
            if (rd == 2) { /* C.ADDI16SP */
                uint32_t v = CB(c, 12, 12) << 9 | CB(c, 6, 6) << 4 | CB(c, 5, 5) << 6
                           | CB(c, 4, 3) << 7 | CB(c, 2, 2) << 5;
                int32_t nz = (int32_t)uam_sext(v, 10);
                return nz ? enc_i(0x13, 2, 0, 2, nz) : 0;
            } else { /* C.LUI */
                if (imm6 == 0 || rd == 0)
                    return 0;
                return 0x37 | rd << 7 | ((uint32_t)imm6 & 0xFFFFF) << 12;
            }
        case 4: {
            uint32_t shamt = CB(c, 6, 2);
            switch (CB(c, 11, 10)) {
            case 0: return CB(c, 12, 12) ? 0 : enc_r(0x13, rs1p, 5, rs1p, shamt, 0x00); /* C.SRLI */
            case 1: return CB(c, 12, 12) ? 0 : enc_r(0x13, rs1p, 5, rs1p, shamt, 0x20); /* C.SRAI */
            case 2: return enc_i(0x13, rs1p, 7, rs1p, imm6);                          /* C.ANDI */
            default:
                if (CB(c, 12, 12))
                    return 0; /* RV64 C.SUBW/C.ADDW */
                switch (CB(c, 6, 5)) {
                case 0: return enc_r(0x33, rs1p, 0, rs1p, rdp, 0x20); /* C.SUB */
                case 1: return enc_r(0x33, rs1p, 4, rs1p, rdp, 0x00); /* C.XOR */
                case 2: return enc_r(0x33, rs1p, 6, rs1p, rdp, 0x00); /* C.OR  */
                default: return enc_r(0x33, rs1p, 7, rs1p, rdp, 0x00); /* C.AND */
                }
            }
        }
        case 5: /* C.J */
            return enc_j(0, cj_off(c));
        case 6: /* C.BEQZ */
            return enc_b(0, rs1p, 0, cb_off(c));
        default: /* C.BNEZ */
            return enc_b(1, rs1p, 0, cb_off(c));
        }

    case 2:
        switch (f3) {
        case 0: /* C.SLLI */
            return CB(c, 12, 12) ? 0 : enc_r(0x13, rd, 1, rd, rs2, 0x00);
        case 2: { /* C.LWSP */
            uint32_t off = CB(c, 12, 12) << 5 | CB(c, 6, 4) << 2 | CB(c, 3, 2) << 6;
            return rd ? enc_i(0x03, rd, 2, 2, (int32_t)off) : 0;
        }
        case 4:
            if (!CB(c, 12, 12)) {
                if (rs2 == 0) /* C.JR */
                    return rd ? enc_i(0x67, 0, 0, rd, 0) : 0;
                return enc_r(0x33, rd, 0, 0, rs2, 0); /* C.MV */
            }
            if (rd == 0 && rs2 == 0)
                return 0x00100073; /* C.EBREAK */
            if (rs2 == 0)
                return enc_i(0x67, 1, 0, rd, 0); /* C.JALR */
            return enc_r(0x33, rd, 0, rd, rs2, 0); /* C.ADD */
        case 6: { /* C.SWSP */
            uint32_t off = CB(c, 12, 9) << 2 | CB(c, 8, 7) << 6;
            return enc_s(0x23, 2, 2, rs2, (int32_t)off);
        }
        default:
            return 0;
        }
    }
    return 0; /* quadrant 3 is not compressed */
}

/* ------------------------------------------------------------------------ */
/* Lifting                                                                   */
/* ------------------------------------------------------------------------ */

#define NONE UAM_REF_NONE
#define LE   UAM_ACC_LE

typedef enum { LIFT_CONTINUE, LIFT_END } lift_result;

typedef struct rv_lifter {
    uam_builder *b;
    uint32_t     pc;
    uint32_t     len;   /* 2 or 4 */
    uint32_t     raw;   /* raw instruction bits as fetched (for tval) */
} rv_lifter;

static uam_ref imm32(rv_lifter *L, uint32_t v) { return uam_imm(L->b, v); }

static uam_ref rreg(rv_lifter *L, unsigned r)
{
    return r == 0 ? imm32(L, 0) : uam_get(L->b, 32, RV_REG_OFF(r));
}

static void wreg(rv_lifter *L, unsigned r, uam_ref v)
{
    if (r != 0)
        uam_put(L->b, 32, RV_REG_OFF(r), v, NONE);
}

static uam_ref bin(rv_lifter *L, uam_opcode op, uam_ref x, uam_ref y)
{
    return uam_binop(L->b, op, 32, x, y);
}

/* Boolean -> 0/1 in 32 bits. */
static uam_ref bool32(rv_lifter *L, uam_ref c)
{
    return uam_unop(L->b, UAM_OP_ZEXT, 32, c);
}

/* Narrow a 32-bit value (temp or immediate) to w bits. */
static uam_ref narrow(rv_lifter *L, uam_ref v, unsigned w)
{
    if (uam_ref_is_imm(v))
        return uam_imm(L->b, L->b->imm[uam_ref_index(v)] & uam_mask(w));
    return uam_unop(L->b, UAM_OP_TRUNC, w, v);
}

static lift_result illegal(rv_lifter *L)
{
    uam_trap(L->b, NONE, UAM_TRAP_EXCEPTION, imm32(L, RV_EXC_ILLEGAL), imm32(L, L->raw));
    return LIFT_END;
}

static uam_exit_kind link_kind(unsigned rd)
{
    return (rd == 1 || rd == 5) ? UAM_EXIT_CALL : UAM_EXIT_JUMP;
}

static lift_result lift_branch(rv_lifter *L, uint32_t i)
{
    uam_ref a = rreg(L, RS1(i)), b = rreg(L, RS2(i)), c;
    switch (FUNCT3(i)) {
    case 0: c = bin(L, UAM_OP_EQ, a, b); break;   /* BEQ  */
    case 1: c = bin(L, UAM_OP_NE, a, b); break;   /* BNE  */
    case 4: c = bin(L, UAM_OP_SLT, a, b); break;  /* BLT  */
    case 5: c = bin(L, UAM_OP_SLE, b, a); break;  /* BGE: b <= a */
    case 6: c = bin(L, UAM_OP_ULT, a, b); break;  /* BLTU */
    case 7: c = bin(L, UAM_OP_ULE, b, a); break;  /* BGEU */
    default: return illegal(L);
    }
    uam_exit(L->b, c, imm32(L, L->pc + (uint32_t)imm_b(i)), UAM_EXIT_JUMP);
    uam_exit(L->b, NONE, imm32(L, L->pc + L->len), UAM_EXIT_JUMP);
    return LIFT_END;
}

static lift_result lift_load(rv_lifter *L, uint32_t i)
{
    static const struct { uint8_t w; bool sign; } kinds[8] = {
        [0] = { 8, true }, [1] = { 16, true }, [2] = { 32, false },
        [4] = { 8, false }, [5] = { 16, false },
    };
    unsigned f3 = FUNCT3(i);
    if (!kinds[f3].w)
        return illegal(L);
    uam_ref addr = bin(L, UAM_OP_ADD, rreg(L, RS1(i)), imm32(L, (uint32_t)imm_i(i)));
    uam_ref v = uam_ld(L->b, kinds[f3].w, LE, addr, NONE);
    if (kinds[f3].w < 32)
        v = uam_unop(L->b, kinds[f3].sign ? UAM_OP_SEXT : UAM_OP_ZEXT, 32, v);
    wreg(L, RD(i), v);
    return LIFT_CONTINUE;
}

static lift_result lift_store(rv_lifter *L, uint32_t i)
{
    static const uint8_t widths[8] = { [0] = 8, [1] = 16, [2] = 32 };
    unsigned w = widths[FUNCT3(i)];
    if (!w)
        return illegal(L);
    uam_ref addr = bin(L, UAM_OP_ADD, rreg(L, RS1(i)), imm32(L, (uint32_t)imm_s(i)));
    uam_ref v = rreg(L, RS2(i));
    if (w < 32)
        v = narrow(L, v, w);
    uam_st(L->b, w, LE, addr, v, NONE);
    return LIFT_CONTINUE;
}

/* OP-IMM and OP share the ALU; `y` is either the immediate or rs2. */
static lift_result lift_alu(rv_lifter *L, uint32_t i, bool is_imm)
{
    unsigned f3 = FUNCT3(i), f7 = FUNCT7(i);
    uam_ref x = rreg(L, RS1(i));
    uam_ref y = is_imm ? imm32(L, (uint32_t)imm_i(i)) : rreg(L, RS2(i));
    uam_ref r;

    if (!is_imm && f7 == 0x01) { /* M extension */
        switch (f3) {
        case 0: r = bin(L, UAM_OP_MUL, x, y); break;
        case 1: r = bin(L, UAM_OP_MULHS, x, y); break;
        case 2: { /* MULHSU = MULHU - (x < 0 ? y : 0)   (docs/uam-isa.md section 10) */
            uam_ref neg = bin(L, UAM_OP_SLT, x, imm32(L, 0));
            uam_ref corr = uam_select(L->b, 32, neg, y, imm32(L, 0));
            r = bin(L, UAM_OP_SUB, bin(L, UAM_OP_MULHU, x, y), corr);
            break;
        }
        case 3: r = bin(L, UAM_OP_MULHU, x, y); break;
        case 4: r = bin(L, UAM_OP_SDIV, x, y); break;  /* IR edge cases match RISC-V */
        case 5: r = bin(L, UAM_OP_UDIV, x, y); break;
        case 6: r = bin(L, UAM_OP_SREM, x, y); break;
        default: r = bin(L, UAM_OP_UREM, x, y); break;
        }
        wreg(L, RD(i), r);
        return LIFT_CONTINUE;
    }

    /* Shifts: immediate forms encode shamt in rs2 (bit 25 must be 0 on RV32);
     * register forms use rs2[4:0] - RISC-V masks, the IR does not. */
    if (f3 == 1 || f3 == 5) {
        uam_ref amt;
        if (is_imm) {
            if ((f7 & ~0x20u) != 0 || (f3 == 1 && f7 != 0))
                return illegal(L);
            amt = imm32(L, RS2(i));
        } else {
            if (f7 != 0x00 && !(f3 == 5 && f7 == 0x20))
                return illegal(L);
            amt = bin(L, UAM_OP_AND, y, imm32(L, 31));
        }
        uam_opcode op = f3 == 1 ? UAM_OP_SHL : (f7 == 0x20 ? UAM_OP_ASHR : UAM_OP_LSHR);
        wreg(L, RD(i), bin(L, op, x, amt));
        return LIFT_CONTINUE;
    }

    if (!is_imm && f7 != 0x00 && !(f7 == 0x20 && f3 == 0))
        return illegal(L);

    switch (f3) {
    case 0: r = bin(L, (!is_imm && f7 == 0x20) ? UAM_OP_SUB : UAM_OP_ADD, x, y); break;
    case 2: r = bool32(L, bin(L, UAM_OP_SLT, x, y)); break;
    case 3: r = bool32(L, bin(L, UAM_OP_ULT, x, y)); break;
    case 4: r = bin(L, UAM_OP_XOR, x, y); break;
    case 6: r = bin(L, UAM_OP_OR, x, y); break;
    default: r = bin(L, UAM_OP_AND, x, y); break;
    }
    wreg(L, RD(i), r);
    return LIFT_CONTINUE;
}

static lift_result lift_amo(rv_lifter *L, uint32_t i)
{
    if (FUNCT3(i) != 2)
        return illegal(L); /* only .W on RV32 */
    unsigned flags = LE | UAM_ACC_ALIGNED;
    if (i & (1u << 26)) flags |= UAM_ACC_ACQ;
    if (i & (1u << 25)) flags |= UAM_ACC_REL;
    uam_ref addr = rreg(L, RS1(i));
    unsigned f5 = i >> 27;
    uam_ref r;

    switch (f5) {
    case 0x02: /* LR.W */
        if (RS2(i) != 0)
            return illegal(L);
        r = uam_ld(L->b, 32, flags | UAM_ACC_X, addr, NONE);
        break;
    case 0x03: { /* SC.W: RISC-V writes 0 on success */
        uam_ref ok = uam_stx(L->b, 32, flags, addr, rreg(L, RS2(i)), NONE);
        r = bin(L, UAM_OP_XOR, bool32(L, ok), imm32(L, 1));
        break;
    }
    default: {
        uam_amo_op sub;
        switch (f5) {
        case 0x01: sub = UAM_AMO_XCHG; break;
        case 0x00: sub = UAM_AMO_ADD;  break;
        case 0x04: sub = UAM_AMO_XOR;  break;
        case 0x0C: sub = UAM_AMO_AND;  break;
        case 0x08: sub = UAM_AMO_OR;   break;
        case 0x10: sub = UAM_AMO_SMIN; break;
        case 0x14: sub = UAM_AMO_SMAX; break;
        case 0x18: sub = UAM_AMO_UMIN; break;
        case 0x1C: sub = UAM_AMO_UMAX; break;
        default: return illegal(L);
        }
        r = uam_amo(L->b, 32, flags, sub, addr, rreg(L, RS2(i)), NONE);
        break;
    }
    }
    wreg(L, RD(i), r);
    return LIFT_CONTINUE;
}

static lift_result lift_system(rv_lifter *L, uint32_t i)
{
    unsigned f3 = FUNCT3(i);

    if (f3 == 0) {
        switch (i) {
        case 0x00000073: /* ECALL: cause resolved by privilege in take_exception */
            uam_trap(L->b, NONE, UAM_TRAP_EXCEPTION, imm32(L, RV_EXC_ECALL_U), imm32(L, 0));
            return LIFT_END;
        case 0x00100073: /* EBREAK */
            uam_trap(L->b, NONE, UAM_TRAP_EXCEPTION, imm32(L, RV_EXC_BREAKPOINT), imm32(L, L->pc));
            return LIFT_END;
        case 0x30200073: { /* MRET */
            uam_ref target = uam_helper(L->b, &uam_isa_rv32.helpers, RV_HELPER_MRET,
                                        NONE, NONE, NONE, NONE);
            uam_exit(L->b, NONE, target, UAM_EXIT_EXC_RETURN);
            return LIFT_END;
        }
        case 0x10500073: /* WFI */
            uam_wait(L->b, imm32(L, L->pc + L->len));
            return LIFT_END;
        default:
            return illegal(L);
        }
    }
    if (f3 == 4)
        return illegal(L);

    /* Zicsr: CSRRW/S/C and immediate forms. */
    const uint32_t csr = i >> 20;
    const unsigned rs1f = RS1(i);
    const unsigned op = f3 & 3;              /* 1 = RW, 2 = RS, 3 = RC */
    uam_ref src = (f3 & 4) ? imm32(L, rs1f) : rreg(L, rs1f);
    bool do_read = !(op == 1 && RD(i) == 0); /* CSRRW to x0 does not read */
    bool do_write = op == 1 || rs1f != 0;    /* RS/RC with x0/zero do not write */

    uam_ref old = do_read ? uam_sysrd(L->b, 32, csr, NONE) : NONE;
    if (do_write) {
        uam_ref nv = src;
        if (op == 2)
            nv = bin(L, UAM_OP_OR, old, src);
        else if (op == 3)
            nv = bin(L, UAM_OP_AND, old, bin(L, UAM_OP_XOR, src, imm32(L, 0xFFFFFFFFu)));
        uam_syswr(L->b, 32, csr, nv, NONE);
    }
    if (do_read)
        wreg(L, RD(i), old);
    if (do_write) {
        /* A CSR write may change interrupt enables or trap state: end the block. */
        uam_exit(L->b, NONE, imm32(L, L->pc + L->len), UAM_EXIT_JUMP);
        return LIFT_END;
    }
    return LIFT_CONTINUE;
}

static lift_result lift_one(rv_lifter *L, uint32_t i)
{
    switch (OPC(i)) {
    case 0x37: /* LUI */
        wreg(L, RD(i), imm32(L, i & 0xFFFFF000u));
        return LIFT_CONTINUE;
    case 0x17: /* AUIPC */
        wreg(L, RD(i), imm32(L, L->pc + (i & 0xFFFFF000u)));
        return LIFT_CONTINUE;
    case 0x6F: /* JAL */
        wreg(L, RD(i), imm32(L, L->pc + L->len));
        uam_exit(L->b, NONE, imm32(L, L->pc + (uint32_t)imm_j(i)), link_kind(RD(i)));
        return LIFT_END;
    case 0x67: { /* JALR: compute target before writing rd (rd may equal rs1) */
        if (FUNCT3(i) != 0)
            return illegal(L);
        uam_ref t = bin(L, UAM_OP_ADD, rreg(L, RS1(i)), imm32(L, (uint32_t)imm_i(i)));
        t = bin(L, UAM_OP_AND, t, imm32(L, 0xFFFFFFFEu));
        wreg(L, RD(i), imm32(L, L->pc + L->len));
        uam_exit_kind k = (RD(i) == 0 && (RS1(i) == 1 || RS1(i) == 5)) ? UAM_EXIT_RET
                                                                       : link_kind(RD(i));
        uam_exit(L->b, NONE, t, k);
        return LIFT_END;
    }
    case 0x63: return lift_branch(L, i);
    case 0x03: return lift_load(L, i);
    case 0x23: return lift_store(L, i);
    case 0x13: return lift_alu(L, i, true);
    case 0x33: return lift_alu(L, i, false);
    case 0x2F: return lift_amo(L, i);
    case 0x0F:
        if (FUNCT3(i) == 0) { /* FENCE */
            uam_fence(L->b, UAM_FENCE_FULL);
            return LIFT_CONTINUE;
        }
        if (FUNCT3(i) == 1) { /* FENCE.I */
            uam_fence(L->b, UAM_FENCE_INSN);
            uam_exit(L->b, NONE, imm32(L, L->pc + L->len), UAM_EXIT_JUMP);
            return LIFT_END;
        }
        return illegal(L);
    case 0x73: return lift_system(L, i);
    default:   return illegal(L);
    }
}

uam_status rv_lift(uam_lift_ctx *lc)
{
    uint32_t pc = (uint32_t)lc->pc;

    for (uint32_t n = 0;; n++) {
        if (n == lc->max_insns) {
            uam_exit(lc->b, NONE, uam_imm(lc->b, pc), UAM_EXIT_JUMP);
            return UAM_OK;
        }

        /* Fetch: one 16-bit parcel, a second one for 32-bit instructions. */
        uint64_t lo = 0, hi = 0;
        uam_status st = lc->fetch(lc, pc, 2, &lo);
        if (st == UAM_OK && (lo & 3) == 3)
            st = lc->fetch(lc, pc + 2, 2, &hi);
        if (st != UAM_OK) {
            if (n == 0)
                return UAM_ERR_FAULT; /* the executor raises the fetch fault */
            /* End the block before the unfetchable instruction; it faults
             * (or is translated) when execution actually reaches it. */
            uam_exit(lc->b, NONE, uam_imm(lc->b, pc), UAM_EXIT_JUMP);
            return UAM_OK;
        }

        rv_lifter L = { .b = lc->b, .pc = pc };
        uint32_t insn;
        if ((lo & 3) == 3) {
            L.len = 4;
            L.raw = insn = (uint32_t)(lo | hi << 16);
        } else {
            L.len = 2;
            L.raw = (uint32_t)lo;
            insn = rv_expand_compressed((uint16_t)lo);
        }

        uam_imark(lc->b, pc, L.len);
        lift_result r = insn ? lift_one(&L, insn) : illegal(&L);
        pc += L.len;
        if (r == LIFT_END)
            return UAM_OK;
    }
}
