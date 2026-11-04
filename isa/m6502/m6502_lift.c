/*
 * m6502_lift.c - 6502 decoder and lifter.
 *
 * Conventions (docs/uam-isa.md and isa/m6502/m6502.h):
 *  - The pc is the 16-bit address; code is fetched a byte at a time and wraps at 64 KiB.
 *    Effective addresses are computed in 16 bits (in 8 for the zero-page modes) and
 *    are bus addresses.
 *  - Blocks are translated for one value of D (cpu->priv) on models with decimal mode,
 *    so ADC and SBC are binary or decimal at lift time. An instruction that can change
 *    D (SED, CLD, PLP), or I (CLI, SEI, PLP), ends the block: the system code applies
 *    the interrupt poll's one-instruction latency at the boundary.
 *  - C, Z, V and N are one byte each; P is assembled when it is pushed.
 *  - Loads, stores and system accesses come before register writes (the commit rule).
 *  - An NMOS read-modify-write instruction stores the byte it read before the result,
 *    as the chip does: a device register sees both writes.
 * Not modelled: the dummy reads of indexed modes that cross a page and the other bus
 * cycles that only timing or a device's read side effects could show.
 */
#include "m6502.h"

#define NONE UAM_REF_NONE
#define LE   UAM_ACC_LE

typedef enum { LIFT_CONTINUE, LIFT_END } lift_result;

/* Addressing modes. */
typedef enum {
    M_IMP, M_ACC, M_IMM, M_ZP, M_ZPX, M_ZPY, M_ABS, M_ABX, M_ABY, M_IZX, M_IZY, M_IZP,
    M_IND, M_IAX, M_REL, M_ZPR,
} mode;

/* Operations. */
typedef enum {
    O_ADC, O_AND, O_ASL, O_BCC, O_BCS, O_BEQ, O_BIT, O_BMI, O_BNE, O_BPL, O_BRK, O_BVC,
    O_BVS, O_CLC, O_CLD, O_CLI, O_CLV, O_CMP, O_CPX, O_CPY, O_DEC, O_DEX, O_DEY, O_EOR,
    O_INC, O_INX, O_INY, O_JMP, O_JSR, O_LDA, O_LDX, O_LDY, O_LSR, O_NOP, O_ORA, O_PHA,
    O_PHP, O_PLA, O_PLP, O_ROL, O_ROR, O_RTI, O_RTS, O_SBC, O_SEC, O_SED, O_SEI, O_STA,
    O_STX, O_STY, O_TAX, O_TAY, O_TSX, O_TXA, O_TXS, O_TYA,
    /* 65C02 */
    O_BRA, O_PHX, O_PHY, O_PLX, O_PLY, O_STZ, O_TRB, O_TSB, O_RMB, O_SMB, O_BBR, O_BBS,
    O_WAI, O_STP,
    /* NMOS undefined opcodes */
    O_SLO, O_RLA, O_SRE, O_RRA, O_SAX, O_LAX, O_DCP, O_ISC, O_ANC, O_ALR, O_ARR, O_SBX,
    O_XAA, O_LXA, O_LAS, O_SHA, O_SHX, O_SHY, O_TAS, O_JAM,
} oper;

typedef struct { uint8_t op, mode; } decoded;

#define E(o, m) { O_##o, M_##m }

static const decoded nmos_table[256] = {
    /* 0x00 */ E(BRK,IMP), E(ORA,IZX), E(JAM,IMP), E(SLO,IZX), E(NOP,ZP),  E(ORA,ZP),  E(ASL,ZP),  E(SLO,ZP),
               E(PHP,IMP), E(ORA,IMM), E(ASL,ACC), E(ANC,IMM), E(NOP,ABS), E(ORA,ABS), E(ASL,ABS), E(SLO,ABS),
    /* 0x10 */ E(BPL,REL), E(ORA,IZY), E(JAM,IMP), E(SLO,IZY), E(NOP,ZPX), E(ORA,ZPX), E(ASL,ZPX), E(SLO,ZPX),
               E(CLC,IMP), E(ORA,ABY), E(NOP,IMP), E(SLO,ABY), E(NOP,ABX), E(ORA,ABX), E(ASL,ABX), E(SLO,ABX),
    /* 0x20 */ E(JSR,ABS), E(AND,IZX), E(JAM,IMP), E(RLA,IZX), E(BIT,ZP),  E(AND,ZP),  E(ROL,ZP),  E(RLA,ZP),
               E(PLP,IMP), E(AND,IMM), E(ROL,ACC), E(ANC,IMM), E(BIT,ABS), E(AND,ABS), E(ROL,ABS), E(RLA,ABS),
    /* 0x30 */ E(BMI,REL), E(AND,IZY), E(JAM,IMP), E(RLA,IZY), E(NOP,ZPX), E(AND,ZPX), E(ROL,ZPX), E(RLA,ZPX),
               E(SEC,IMP), E(AND,ABY), E(NOP,IMP), E(RLA,ABY), E(NOP,ABX), E(AND,ABX), E(ROL,ABX), E(RLA,ABX),
    /* 0x40 */ E(RTI,IMP), E(EOR,IZX), E(JAM,IMP), E(SRE,IZX), E(NOP,ZP),  E(EOR,ZP),  E(LSR,ZP),  E(SRE,ZP),
               E(PHA,IMP), E(EOR,IMM), E(LSR,ACC), E(ALR,IMM), E(JMP,ABS), E(EOR,ABS), E(LSR,ABS), E(SRE,ABS),
    /* 0x50 */ E(BVC,REL), E(EOR,IZY), E(JAM,IMP), E(SRE,IZY), E(NOP,ZPX), E(EOR,ZPX), E(LSR,ZPX), E(SRE,ZPX),
               E(CLI,IMP), E(EOR,ABY), E(NOP,IMP), E(SRE,ABY), E(NOP,ABX), E(EOR,ABX), E(LSR,ABX), E(SRE,ABX),
    /* 0x60 */ E(RTS,IMP), E(ADC,IZX), E(JAM,IMP), E(RRA,IZX), E(NOP,ZP),  E(ADC,ZP),  E(ROR,ZP),  E(RRA,ZP),
               E(PLA,IMP), E(ADC,IMM), E(ROR,ACC), E(ARR,IMM), E(JMP,IND), E(ADC,ABS), E(ROR,ABS), E(RRA,ABS),
    /* 0x70 */ E(BVS,REL), E(ADC,IZY), E(JAM,IMP), E(RRA,IZY), E(NOP,ZPX), E(ADC,ZPX), E(ROR,ZPX), E(RRA,ZPX),
               E(SEI,IMP), E(ADC,ABY), E(NOP,IMP), E(RRA,ABY), E(NOP,ABX), E(ADC,ABX), E(ROR,ABX), E(RRA,ABX),
    /* 0x80 */ E(NOP,IMM), E(STA,IZX), E(NOP,IMM), E(SAX,IZX), E(STY,ZP),  E(STA,ZP),  E(STX,ZP),  E(SAX,ZP),
               E(DEY,IMP), E(NOP,IMM), E(TXA,IMP), E(XAA,IMM), E(STY,ABS), E(STA,ABS), E(STX,ABS), E(SAX,ABS),
    /* 0x90 */ E(BCC,REL), E(STA,IZY), E(JAM,IMP), E(SHA,IZY), E(STY,ZPX), E(STA,ZPX), E(STX,ZPY), E(SAX,ZPY),
               E(TYA,IMP), E(STA,ABY), E(TXS,IMP), E(TAS,ABY), E(SHY,ABX), E(STA,ABX), E(SHX,ABY), E(SHA,ABY),
    /* 0xA0 */ E(LDY,IMM), E(LDA,IZX), E(LDX,IMM), E(LAX,IZX), E(LDY,ZP),  E(LDA,ZP),  E(LDX,ZP),  E(LAX,ZP),
               E(TAY,IMP), E(LDA,IMM), E(TAX,IMP), E(LXA,IMM), E(LDY,ABS), E(LDA,ABS), E(LDX,ABS), E(LAX,ABS),
    /* 0xB0 */ E(BCS,REL), E(LDA,IZY), E(JAM,IMP), E(LAX,IZY), E(LDY,ZPX), E(LDA,ZPX), E(LDX,ZPY), E(LAX,ZPY),
               E(CLV,IMP), E(LDA,ABY), E(TSX,IMP), E(LAS,ABY), E(LDY,ABX), E(LDA,ABX), E(LDX,ABY), E(LAX,ABY),
    /* 0xC0 */ E(CPY,IMM), E(CMP,IZX), E(NOP,IMM), E(DCP,IZX), E(CPY,ZP),  E(CMP,ZP),  E(DEC,ZP),  E(DCP,ZP),
               E(INY,IMP), E(CMP,IMM), E(DEX,IMP), E(SBX,IMM), E(CPY,ABS), E(CMP,ABS), E(DEC,ABS), E(DCP,ABS),
    /* 0xD0 */ E(BNE,REL), E(CMP,IZY), E(JAM,IMP), E(DCP,IZY), E(NOP,ZPX), E(CMP,ZPX), E(DEC,ZPX), E(DCP,ZPX),
               E(CLD,IMP), E(CMP,ABY), E(NOP,IMP), E(DCP,ABY), E(NOP,ABX), E(CMP,ABX), E(DEC,ABX), E(DCP,ABX),
    /* 0xE0 */ E(CPX,IMM), E(SBC,IZX), E(NOP,IMM), E(ISC,IZX), E(CPX,ZP),  E(SBC,ZP),  E(INC,ZP),  E(ISC,ZP),
               E(INX,IMP), E(SBC,IMM), E(NOP,IMP), E(SBC,IMM), E(CPX,ABS), E(SBC,ABS), E(INC,ABS), E(ISC,ABS),
    /* 0xF0 */ E(BEQ,REL), E(SBC,IZY), E(JAM,IMP), E(ISC,IZY), E(NOP,ZPX), E(SBC,ZPX), E(INC,ZPX), E(ISC,ZPX),
               E(SED,IMP), E(SBC,ABY), E(NOP,IMP), E(ISC,ABY), E(NOP,ABX), E(SBC,ABX), E(INC,ABX), E(ISC,ABX),
};

/* The 65C02's (WDC W65C02S). Columns 3 and B are 1-byte NOPs; columns 7 and F are the
 * bit operations (bit = row & 7), which decode() turns into NOPs without them, as it
 * does WAI (0xCB) and STP (0xDB). */
static const decoded cmos_table[256] = {
    /* 0x00 */ E(BRK,IMP), E(ORA,IZX), E(NOP,IMM), E(NOP,IMP), E(TSB,ZP),  E(ORA,ZP),  E(ASL,ZP),  E(RMB,ZP),
               E(PHP,IMP), E(ORA,IMM), E(ASL,ACC), E(NOP,IMP), E(TSB,ABS), E(ORA,ABS), E(ASL,ABS), E(BBR,ZPR),
    /* 0x10 */ E(BPL,REL), E(ORA,IZY), E(ORA,IZP), E(NOP,IMP), E(TRB,ZP),  E(ORA,ZPX), E(ASL,ZPX), E(RMB,ZP),
               E(CLC,IMP), E(ORA,ABY), E(INC,ACC), E(NOP,IMP), E(TRB,ABS), E(ORA,ABX), E(ASL,ABX), E(BBR,ZPR),
    /* 0x20 */ E(JSR,ABS), E(AND,IZX), E(NOP,IMM), E(NOP,IMP), E(BIT,ZP),  E(AND,ZP),  E(ROL,ZP),  E(RMB,ZP),
               E(PLP,IMP), E(AND,IMM), E(ROL,ACC), E(NOP,IMP), E(BIT,ABS), E(AND,ABS), E(ROL,ABS), E(BBR,ZPR),
    /* 0x30 */ E(BMI,REL), E(AND,IZY), E(AND,IZP), E(NOP,IMP), E(BIT,ZPX), E(AND,ZPX), E(ROL,ZPX), E(RMB,ZP),
               E(SEC,IMP), E(AND,ABY), E(DEC,ACC), E(NOP,IMP), E(BIT,ABX), E(AND,ABX), E(ROL,ABX), E(BBR,ZPR),
    /* 0x40 */ E(RTI,IMP), E(EOR,IZX), E(NOP,IMM), E(NOP,IMP), E(NOP,ZP),  E(EOR,ZP),  E(LSR,ZP),  E(RMB,ZP),
               E(PHA,IMP), E(EOR,IMM), E(LSR,ACC), E(NOP,IMP), E(JMP,ABS), E(EOR,ABS), E(LSR,ABS), E(BBR,ZPR),
    /* 0x50 */ E(BVC,REL), E(EOR,IZY), E(EOR,IZP), E(NOP,IMP), E(NOP,ZPX), E(EOR,ZPX), E(LSR,ZPX), E(RMB,ZP),
               E(CLI,IMP), E(EOR,ABY), E(PHY,IMP), E(NOP,IMP), E(NOP,ABS), E(EOR,ABX), E(LSR,ABX), E(BBR,ZPR),
    /* 0x60 */ E(RTS,IMP), E(ADC,IZX), E(NOP,IMM), E(NOP,IMP), E(STZ,ZP),  E(ADC,ZP),  E(ROR,ZP),  E(RMB,ZP),
               E(PLA,IMP), E(ADC,IMM), E(ROR,ACC), E(NOP,IMP), E(JMP,IND), E(ADC,ABS), E(ROR,ABS), E(BBR,ZPR),
    /* 0x70 */ E(BVS,REL), E(ADC,IZY), E(ADC,IZP), E(NOP,IMP), E(STZ,ZPX), E(ADC,ZPX), E(ROR,ZPX), E(RMB,ZP),
               E(SEI,IMP), E(ADC,ABY), E(PLY,IMP), E(NOP,IMP), E(JMP,IAX), E(ADC,ABX), E(ROR,ABX), E(BBR,ZPR),
    /* 0x80 */ E(BRA,REL), E(STA,IZX), E(NOP,IMM), E(NOP,IMP), E(STY,ZP),  E(STA,ZP),  E(STX,ZP),  E(SMB,ZP),
               E(DEY,IMP), E(BIT,IMM), E(TXA,IMP), E(NOP,IMP), E(STY,ABS), E(STA,ABS), E(STX,ABS), E(BBS,ZPR),
    /* 0x90 */ E(BCC,REL), E(STA,IZY), E(STA,IZP), E(NOP,IMP), E(STY,ZPX), E(STA,ZPX), E(STX,ZPY), E(SMB,ZP),
               E(TYA,IMP), E(STA,ABY), E(TXS,IMP), E(NOP,IMP), E(STZ,ABS), E(STA,ABX), E(STZ,ABX), E(BBS,ZPR),
    /* 0xA0 */ E(LDY,IMM), E(LDA,IZX), E(LDX,IMM), E(NOP,IMP), E(LDY,ZP),  E(LDA,ZP),  E(LDX,ZP),  E(SMB,ZP),
               E(TAY,IMP), E(LDA,IMM), E(TAX,IMP), E(NOP,IMP), E(LDY,ABS), E(LDA,ABS), E(LDX,ABS), E(BBS,ZPR),
    /* 0xB0 */ E(BCS,REL), E(LDA,IZY), E(LDA,IZP), E(NOP,IMP), E(LDY,ZPX), E(LDA,ZPX), E(LDX,ZPY), E(SMB,ZP),
               E(CLV,IMP), E(LDA,ABY), E(TSX,IMP), E(NOP,IMP), E(LDY,ABX), E(LDA,ABX), E(LDX,ABY), E(BBS,ZPR),
    /* 0xC0 */ E(CPY,IMM), E(CMP,IZX), E(NOP,IMM), E(NOP,IMP), E(CPY,ZP),  E(CMP,ZP),  E(DEC,ZP),  E(SMB,ZP),
               E(INY,IMP), E(CMP,IMM), E(DEX,IMP), E(WAI,IMP), E(CPY,ABS), E(CMP,ABS), E(DEC,ABS), E(BBS,ZPR),
    /* 0xD0 */ E(BNE,REL), E(CMP,IZY), E(CMP,IZP), E(NOP,IMP), E(NOP,ZPX), E(CMP,ZPX), E(DEC,ZPX), E(SMB,ZP),
               E(CLD,IMP), E(CMP,ABY), E(PHX,IMP), E(STP,IMP), E(NOP,ABS), E(CMP,ABX), E(DEC,ABX), E(BBS,ZPR),
    /* 0xE0 */ E(CPX,IMM), E(SBC,IZX), E(NOP,IMM), E(NOP,IMP), E(CPX,ZP),  E(SBC,ZP),  E(INC,ZP),  E(SMB,ZP),
               E(INX,IMP), E(SBC,IMM), E(NOP,IMP), E(NOP,IMP), E(CPX,ABS), E(SBC,ABS), E(INC,ABS), E(BBS,ZPR),
    /* 0xF0 */ E(BEQ,REL), E(SBC,IZY), E(SBC,IZP), E(NOP,IMP), E(NOP,ZPX), E(SBC,ZPX), E(INC,ZPX), E(SMB,ZP),
               E(SED,IMP), E(SBC,ABY), E(PLX,IMP), E(NOP,IMP), E(NOP,ABS), E(SBC,ABX), E(INC,ABX), E(BBS,ZPR),
};

#undef E

static decoded decode(const uam_m6502_config *c, uint8_t op)
{
    if (!c->cmos)
        return nmos_table[op];
    decoded d = cmos_table[op];
    if ((!c->bitops && (op & 7) == 7) || (!c->wai_stp && (op == 0xCB || op == 0xDB)))
        d = (decoded){ O_NOP, M_IMP };
    return d;
}

static unsigned mode_len(unsigned m)
{
    switch (m) {
    case M_IMP: case M_ACC: return 1;
    case M_ABS: case M_ABX: case M_ABY: case M_IND: case M_IAX: case M_ZPR: return 3;
    default: return 2;
    }
}

unsigned m6502_insn_len(const uam_m6502_config *c, uint8_t op)
{
    return mode_len(decode(c, op).mode);
}

typedef struct m65_lifter {
    uam_lift_ctx            *lc;
    uam_builder             *b;
    const uam_m6502_config  *cfg;
    uint16_t                 pc;      /* address of the instruction */
    unsigned                 len;
    uint8_t                  op, b1, b2;
    decoded                  d;
    bool                     dec;     /* decimal arithmetic (D, on a model with it) */
    bool                     end;     /* the block ends after this instruction */
} m65_lifter;

/* ---- IR shorthands ------------------------------------------------------------- */

static uam_ref imm(m65_lifter *L, uint64_t v) { return uam_imm(L->b, v); }
static uam_ref bin(m65_lifter *L, uam_opcode op, unsigned w, uam_ref x, uam_ref y)
{
    return uam_binop(L->b, op, w, x, y);
}
/* Zero-extend a temp; an immediate is the same value at any width. */
static uam_ref zext(m65_lifter *L, unsigned w, uam_ref x)
{
    return uam_ref_is_imm(x) ? x : uam_unop(L->b, UAM_OP_ZEXT, w, x);
}
static uam_ref trunc_(m65_lifter *L, unsigned w, uam_ref x) { return uam_unop(L->b, UAM_OP_TRUNC, w, x); }
static uam_ref not1(m65_lifter *L, uam_ref c) { return bin(L, UAM_OP_XOR, 1, c, imm(L, 1)); }
static uam_ref bit(m65_lifter *L, unsigned w, uam_ref v, unsigned n)
{
    return trunc_(L, 1, n ? bin(L, UAM_OP_LSHR, w, v, imm(L, n)) : v);
}
static uam_ref and8(m65_lifter *L, uam_ref x, uam_ref y) { return bin(L, UAM_OP_AND, 8, x, y); }
static uam_ref or8(m65_lifter *L, uam_ref x, uam_ref y) { return bin(L, UAM_OP_OR, 8, x, y); }

static uam_ref get8(m65_lifter *L, uint32_t off) { return uam_get(L->b, 8, off); }
static void put8(m65_lifter *L, uint32_t off, uam_ref v) { uam_put(L->b, 8, off, v, NONE); }
static uam_ref flag(m65_lifter *L, uint32_t f) { return trunc_(L, 1, get8(L, f)); }
static void setf(m65_lifter *L, uint32_t f, uam_ref c) { put8(L, f, zext(L, 8, c)); }

static uint16_t next_pc(const m65_lifter *L) { return (uint16_t)(L->pc + L->len); }
static uint16_t abs16(const m65_lifter *L) { return (uint16_t)(L->b1 | L->b2 << 8); }

/* ---- memory -------------------------------------------------------------------- */

static uam_ref rd(m65_lifter *L, uam_ref a16) { return uam_ld(L->b, 8, LE, zext(L, 32, a16), NONE); }
static void wr(m65_lifter *L, uam_ref a16, uam_ref v) { uam_st(L->b, 8, LE, zext(L, 32, a16), v, NONE); }

/* A 16-bit value from two bytes. */
static uam_ref word(m65_lifter *L, uam_ref lo, uam_ref hi)
{
    return bin(L, UAM_OP_OR, 16, zext(L, 16, lo), bin(L, UAM_OP_SHL, 16, zext(L, 16, hi), imm(L, 8)));
}

/* The pointer in the zero page at z (an 8-bit value): its high byte wraps within the page. */
static uam_ref zp_ptr(m65_lifter *L, uam_ref z)
{
    uam_ref lo = rd(L, zext(L, 16, z));
    uam_ref hi = rd(L, zext(L, 16, bin(L, UAM_OP_ADD, 8, z, imm(L, 1))));
    return word(L, lo, hi);
}

/* The effective address of the instruction's operand, 16 bits; *base, if asked, is the
 * address before indexing (the SHA/SHX/SHY/TAS store depends on it). */
static uam_ref ea(m65_lifter *L, uam_ref *base)
{
    uam_ref bse = NONE, a;
    switch (L->d.mode) {
    case M_ZP: case M_ZPR:
        a = imm(L, L->b1);
        break;
    case M_ZPX: case M_ZPY: {
        uam_ref i = get8(L, L->d.mode == M_ZPX ? M6502_X : M6502_Y);
        a = zext(L, 16, bin(L, UAM_OP_ADD, 8, imm(L, L->b1), i));
        break;
    }
    case M_ABS:
        a = imm(L, abs16(L));
        break;
    case M_ABX: case M_ABY:
        bse = imm(L, abs16(L));
        a = bin(L, UAM_OP_ADD, 16, bse, zext(L, 16, get8(L, L->d.mode == M_ABX ? M6502_X : M6502_Y)));
        break;
    case M_IZX:
        a = zp_ptr(L, bin(L, UAM_OP_ADD, 8, imm(L, L->b1), get8(L, M6502_X)));
        break;
    case M_IZY:
        bse = zp_ptr(L, imm(L, L->b1));
        a = bin(L, UAM_OP_ADD, 16, bse, zext(L, 16, get8(L, M6502_Y)));
        break;
    default:   /* M_IZP */
        a = zp_ptr(L, imm(L, L->b1));
        break;
    }
    if (base)
        *base = bse;
    return a;
}

/* The operand of a read instruction: #imm, A, or the byte at the effective address. */
static uam_ref operand(m65_lifter *L)
{
    if (L->d.mode == M_IMM)
        return imm(L, L->b1);
    if (L->d.mode == M_ACC)
        return get8(L, M6502_A);
    return rd(L, ea(L, NULL));
}

/* The stack: S points at the next free byte of page 1. */
static uam_ref stack_addr(m65_lifter *L, uam_ref s8)
{
    return bin(L, UAM_OP_OR, 16, zext(L, 16, s8), imm(L, 0x100));
}
static uam_ref s_add(m65_lifter *L, uam_ref s8, int k)
{
    return bin(L, UAM_OP_ADD, 8, s8, imm(L, (uint8_t)k));
}
/* Push v at *s, moving *s down (the caller PUTs it). */
static void push(m65_lifter *L, uam_ref *s, uam_ref v)
{
    wr(L, stack_addr(L, *s), v);
    *s = s_add(L, *s, -1);
}
static uam_ref pull(m65_lifter *L, uam_ref *s)
{
    *s = s_add(L, *s, 1);
    return rd(L, stack_addr(L, *s));
}

/* ---- flags --------------------------------------------------------------------- */

/* An instruction's results, written after its memory accesses (the commit rule). */
typedef struct { uam_ref c, z, v, n; } flags;
#define NO_FLAGS ((flags){ NONE, NONE, NONE, NONE })

static void nz(m65_lifter *L, flags *f, uam_ref r)
{
    f->z = bin(L, UAM_OP_EQ, 8, r, imm(L, 0));
    f->n = bit(L, 8, r, 7);
}

static void put_flags(m65_lifter *L, const flags *f)
{
    if (!uam_ref_is_none(f->c)) setf(L, M6502_C, f->c);
    if (!uam_ref_is_none(f->z)) setf(L, M6502_Z, f->z);
    if (!uam_ref_is_none(f->v)) setf(L, M6502_V, f->v);
    if (!uam_ref_is_none(f->n)) setf(L, M6502_N, f->n);
}

/* P as pushed: B and bit 5 set (PHP, BRK). */
static uam_ref p_get(m65_lifter *L)
{
    uam_ref v = or8(L, get8(L, M6502_PX), imm(L, UAM_M6502_P_B | UAM_M6502_P_U));
    v = or8(L, v, get8(L, M6502_C));
    v = or8(L, v, bin(L, UAM_OP_SHL, 8, get8(L, M6502_Z), imm(L, 1)));
    v = or8(L, v, bin(L, UAM_OP_SHL, 8, get8(L, M6502_V), imm(L, 6)));
    return or8(L, v, bin(L, UAM_OP_SHL, 8, get8(L, M6502_N), imm(L, 7)));
}

/* C, Z, V and N from a pulled P. */
static flags p_flags(m65_lifter *L, uam_ref p)
{
    return (flags){ bit(L, 8, p, 0), bit(L, 8, p, 1), bit(L, 8, p, 6), bit(L, 8, p, 7) };
}

/* Write I and D: through the poll latency (CLI, SEI, SED, CLD, PLP) or at once. */
static void px_write(m65_lifter *L, uam_ref p, bool now)
{
    uam_syswr(L->b, 8, now ? M6502_SYS_P_NOW : M6502_SYS_P, p, NONE);
    L->end = true;
}

/* ---- arithmetic ----------------------------------------------------------------- */

/* a + m + c (sub: a - m - !c). Binary, or decimal through the model's helper. */
static uam_ref add(m65_lifter *L, bool sub, uam_ref a, uam_ref m, uam_ref c, flags *f)
{
    if (L->dec) {
        const unsigned h = sub ? (L->cfg->cmos ? M6502_HELPER_SBC_CMOS : M6502_HELPER_SBC_NMOS)
                               : (L->cfg->cmos ? M6502_HELPER_ADC_CMOS : M6502_HELPER_ADC_NMOS);
        uam_ref p = uam_helper(L->b, &uam_isa_m6502.helpers, h, a, m, zext(L, 8, c), NONE);
        f->c = bit(L, 16, p, M6502_DEC_C);
        f->z = bit(L, 16, p, M6502_DEC_Z);
        f->n = bit(L, 16, p, M6502_DEC_N);
        f->v = bit(L, 16, p, M6502_DEC_V);
        return trunc_(L, 8, p);
    }
    uam_ref mm = sub ? bin(L, UAM_OP_XOR, 8, m, imm(L, 0xFF)) : m;
    uam_ref s = bin(L, UAM_OP_ADD, 16, bin(L, UAM_OP_ADD, 16, zext(L, 16, a), zext(L, 16, mm)), zext(L, 16, c));
    uam_ref r = trunc_(L, 8, s);
    f->c = bit(L, 16, s, 8);
    f->v = bit(L, 8, and8(L, bin(L, UAM_OP_XOR, 8, a, r), bin(L, UAM_OP_XOR, 8, mm, r)), 7);
    nz(L, f, r);
    return r;
}

/* CMP, CPX, CPY (and DCP, SBX): r - m. */
static void compare(m65_lifter *L, uam_ref r, uam_ref m, flags *f)
{
    f->c = not1(L, bin(L, UAM_OP_ULT, 8, r, m));
    nz(L, f, bin(L, UAM_OP_SUB, 8, r, m));
}

/* ASL, LSR, ROL, ROR of v: the result and the bit shifted out. */
static uam_ref shift(m65_lifter *L, unsigned which, uam_ref v, uam_ref *out)
{
    const bool left = which == O_ASL || which == O_ROL;
    const bool rot = which == O_ROL || which == O_ROR;
    *out = bit(L, 8, v, left ? 7 : 0);
    uam_ref r = bin(L, left ? UAM_OP_SHL : UAM_OP_LSHR, 8, v, imm(L, 1));
    if (rot) {
        uam_ref c = zext(L, 8, flag(L, M6502_C));
        r = or8(L, r, left ? c : bin(L, UAM_OP_SHL, 8, c, imm(L, 7)));
    }
    return r;
}

/* ---- control flow ---------------------------------------------------------------- */

static lift_result jump(m65_lifter *L, uint16_t t)
{
    uam_exit(L->b, NONE, imm(L, t), UAM_EXIT_JUMP);
    return LIFT_END;
}

static lift_result branch(m65_lifter *L, uam_ref cond, uint16_t t)
{
    uam_exit(L->b, cond, imm(L, t), UAM_EXIT_JUMP);
    return jump(L, next_pc(L));
}

static uint16_t rel(const m65_lifter *L, uint8_t off) { return (uint16_t)(next_pc(L) + (int8_t)off); }

/* ---- instructions ---------------------------------------------------------------- */

/* A read-modify-write of memory: read, (NMOS) write the byte back, write the result. */
static uam_ref rmw_read(m65_lifter *L, uam_ref *a)
{
    *a = ea(L, NULL);
    return rd(L, *a);
}
static void rmw_write(m65_lifter *L, uam_ref a, uam_ref old, uam_ref r)
{
    if (!L->cfg->cmos)
        wr(L, a, old);
    wr(L, a, r);
}

/* SHA, SHX, SHY, TAS: store v & (H + 1), H the high byte of the unindexed address; when
 * indexing carried into the high byte, the stored value is that byte too. */
static void store_hi(m65_lifter *L, uam_ref v)
{
    uam_ref base, a = ea(L, &base);
    uam_ref h1 = bin(L, UAM_OP_ADD, 8, trunc_(L, 8, bin(L, UAM_OP_LSHR, 16, base, imm(L, 8))), imm(L, 1));
    uam_ref val = and8(L, v, h1);
    uam_ref cross = bin(L, UAM_OP_NE, 16, bin(L, UAM_OP_AND, 16, bin(L, UAM_OP_XOR, 16, a, base), imm(L, 0xFF00)),
                        imm(L, 0));
    uam_ref crossed = bin(L, UAM_OP_OR, 16, bin(L, UAM_OP_AND, 16, a, imm(L, 0xFF)),
                          bin(L, UAM_OP_SHL, 16, zext(L, 16, val), imm(L, 8)));
    wr(L, uam_select(L->b, 16, cross, crossed, a), val);
}

static lift_result lift_insn(m65_lifter *L)
{
    const unsigned o = L->d.op;
    flags f = NO_FLAGS;
    uam_ref a, v, r, c, s;

    switch (o) {
    /* loads, stores, transfers */
    case O_LDA: case O_LDX: case O_LDY: case O_LAX:
        v = operand(L);
        nz(L, &f, v);
        if (o != O_LDX && o != O_LDY) put8(L, M6502_A, v);
        if (o == O_LDX || o == O_LAX) put8(L, M6502_X, v);
        if (o == O_LDY) put8(L, M6502_Y, v);
        break;
    case O_STA: wr(L, ea(L, NULL), get8(L, M6502_A)); return LIFT_CONTINUE;
    case O_STX: wr(L, ea(L, NULL), get8(L, M6502_X)); return LIFT_CONTINUE;
    case O_STY: wr(L, ea(L, NULL), get8(L, M6502_Y)); return LIFT_CONTINUE;
    case O_STZ: wr(L, ea(L, NULL), imm(L, 0)); return LIFT_CONTINUE;
    case O_SAX: wr(L, ea(L, NULL), and8(L, get8(L, M6502_A), get8(L, M6502_X))); return LIFT_CONTINUE;
    case O_TAX: case O_TAY: case O_TXA: case O_TYA: case O_TSX: {
        static const uint8_t src[] = { [O_TAX] = M6502_A, [O_TAY] = M6502_A, [O_TXA] = M6502_X,
                                       [O_TYA] = M6502_Y, [O_TSX] = M6502_S };
        static const uint8_t dst[] = { [O_TAX] = M6502_X, [O_TAY] = M6502_Y, [O_TXA] = M6502_A,
                                       [O_TYA] = M6502_A, [O_TSX] = M6502_X };
        v = get8(L, src[o]);
        nz(L, &f, v);
        put8(L, dst[o], v);
        break;
    }
    case O_TXS: put8(L, M6502_S, get8(L, M6502_X)); return LIFT_CONTINUE;

    /* arithmetic and logic */
    case O_ADC: case O_SBC:
        v = operand(L);
        r = add(L, o == O_SBC, get8(L, M6502_A), v, flag(L, M6502_C), &f);
        put8(L, M6502_A, r);
        break;
    case O_AND: case O_ORA: case O_EOR: {
        static const uam_opcode ops[] = { [O_AND] = UAM_OP_AND, [O_ORA] = UAM_OP_OR, [O_EOR] = UAM_OP_XOR };
        v = operand(L);
        r = bin(L, ops[o], 8, get8(L, M6502_A), v);
        nz(L, &f, r);
        put8(L, M6502_A, r);
        break;
    }
    case O_CMP: case O_CPX: case O_CPY:
        v = operand(L);
        compare(L, get8(L, o == O_CMP ? M6502_A : o == O_CPX ? M6502_X : M6502_Y), v, &f);
        break;
    case O_BIT:
        v = operand(L);
        f.z = bin(L, UAM_OP_EQ, 8, and8(L, get8(L, M6502_A), v), imm(L, 0));
        if (L->d.mode != M_IMM) {   /* BIT #imm (65C02) sets only Z */
            f.v = bit(L, 8, v, 6);
            f.n = bit(L, 8, v, 7);
        }
        break;
    case O_ASL: case O_LSR: case O_ROL: case O_ROR:
        if (L->d.mode == M_ACC) {
            r = shift(L, o, get8(L, M6502_A), &f.c);
            put8(L, M6502_A, r);
        } else {
            v = rmw_read(L, &a);
            r = shift(L, o, v, &f.c);
            rmw_write(L, a, v, r);
        }
        nz(L, &f, r);
        break;
    case O_INC: case O_DEC: {
        const uam_opcode op = o == O_INC ? UAM_OP_ADD : UAM_OP_SUB;
        if (L->d.mode == M_ACC) {
            r = bin(L, op, 8, get8(L, M6502_A), imm(L, 1));
            put8(L, M6502_A, r);
        } else {
            v = rmw_read(L, &a);
            r = bin(L, op, 8, v, imm(L, 1));
            rmw_write(L, a, v, r);
        }
        nz(L, &f, r);
        break;
    }
    case O_INX: case O_INY: case O_DEX: case O_DEY: {
        const uint32_t reg = o == O_INX || o == O_DEX ? M6502_X : M6502_Y;
        r = bin(L, o == O_INX || o == O_INY ? UAM_OP_ADD : UAM_OP_SUB, 8, get8(L, reg), imm(L, 1));
        nz(L, &f, r);
        put8(L, reg, r);
        break;
    }

    /* flags */
    case O_CLC: put8(L, M6502_C, imm(L, 0)); return LIFT_CONTINUE;
    case O_SEC: put8(L, M6502_C, imm(L, 1)); return LIFT_CONTINUE;
    case O_CLV: put8(L, M6502_V, imm(L, 0)); return LIFT_CONTINUE;
    case O_CLI: case O_SEI: case O_CLD: case O_SED: {
        const unsigned m = o == O_CLI || o == O_SEI ? UAM_M6502_P_I : UAM_M6502_P_D;
        const bool set = o == O_SEI || o == O_SED;
        if (m == UAM_M6502_P_D && L->dec == set && L->cfg->decimal) {   /* D as translated */
            return LIFT_CONTINUE;
        }
        v = get8(L, M6502_PX);
        px_write(L, set ? or8(L, v, imm(L, m)) : and8(L, v, imm(L, 0xFF & ~m)), false);
        if (m == UAM_M6502_P_D && !L->cfg->decimal)
            L->end = false;   /* a flag only: nothing translated depends on it */
        return LIFT_CONTINUE;
    }

    /* the stack */
    case O_PHA: case O_PHP: case O_PHX: case O_PHY:
        s = get8(L, M6502_S);
        v = o == O_PHP ? p_get(L) : get8(L, o == O_PHA ? M6502_A : o == O_PHX ? M6502_X : M6502_Y);
        push(L, &s, v);
        put8(L, M6502_S, s);
        return LIFT_CONTINUE;
    case O_PLA: case O_PLX: case O_PLY:
        s = get8(L, M6502_S);
        v = pull(L, &s);
        nz(L, &f, v);
        put8(L, M6502_S, s);
        put8(L, o == O_PLA ? M6502_A : o == O_PLX ? M6502_X : M6502_Y, v);
        break;
    case O_PLP:
        s = get8(L, M6502_S);
        v = pull(L, &s);
        px_write(L, v, false);
        f = p_flags(L, v);
        put8(L, M6502_S, s);
        break;

    /* control flow */
    case O_JMP:
        if (L->d.mode == M_ABS)
            return jump(L, abs16(L));
        if (L->d.mode == M_IND) {
            const uint16_t p = abs16(L);
            /* NMOS: the pointer's high byte comes from the start of its page */
            const uint16_t p1 = L->cfg->cmos ? (uint16_t)(p + 1) : (uint16_t)((p & 0xFF00) | ((p + 1) & 0xFF));
            a = word(L, rd(L, imm(L, p)), rd(L, imm(L, p1)));
        } else {   /* (abs,X) */
            uam_ref p = bin(L, UAM_OP_ADD, 16, imm(L, abs16(L)), zext(L, 16, get8(L, M6502_X)));
            a = word(L, rd(L, p), rd(L, bin(L, UAM_OP_ADD, 16, p, imm(L, 1))));
        }
        uam_exit(L->b, NONE, zext(L, 32, a), UAM_EXIT_JUMP);
        return LIFT_END;
    case O_JSR: {
        const uint16_t ret = (uint16_t)(L->pc + 2);   /* the address of its last byte */
        s = get8(L, M6502_S);
        push(L, &s, imm(L, ret >> 8));
        push(L, &s, imm(L, ret & 0xFF));
        put8(L, M6502_S, s);
        uam_exit(L->b, NONE, imm(L, abs16(L)), UAM_EXIT_CALL);
        return LIFT_END;
    }
    case O_RTS: {
        s = get8(L, M6502_S);
        uam_ref lo = pull(L, &s), hi = pull(L, &s);
        put8(L, M6502_S, s);
        uam_ref t = bin(L, UAM_OP_ADD, 16, word(L, lo, hi), imm(L, 1));
        uam_exit(L->b, NONE, zext(L, 32, t), UAM_EXIT_RET);
        return LIFT_END;
    }
    case O_RTI: {
        s = get8(L, M6502_S);
        uam_ref p = pull(L, &s);
        uam_ref lo = pull(L, &s), hi = pull(L, &s);
        px_write(L, p, true);
        f = p_flags(L, p);
        put_flags(L, &f);
        put8(L, M6502_S, s);
        uam_exit(L->b, NONE, zext(L, 32, word(L, lo, hi)), UAM_EXIT_EXC_RETURN);
        return LIFT_END;
    }
    case O_BRK: {
        const uint16_t ret = (uint16_t)(L->pc + 2);   /* BRK skips a signature byte */
        s = get8(L, M6502_S);
        push(L, &s, imm(L, ret >> 8));
        push(L, &s, imm(L, ret & 0xFF));
        push(L, &s, p_get(L));
        uam_ref t = word(L, rd(L, imm(L, UAM_M6502_VEC_IRQ)), rd(L, imm(L, UAM_M6502_VEC_IRQ + 1)));
        uam_ref px = or8(L, get8(L, M6502_PX), imm(L, UAM_M6502_P_I));
        if (L->cfg->cmos)
            px = and8(L, px, imm(L, 0xFF & ~UAM_M6502_P_D));
        px_write(L, px, true);
        put8(L, M6502_S, s);
        uam_exit(L->b, NONE, zext(L, 32, t), UAM_EXIT_CALL);
        return LIFT_END;
    }
    case O_BPL: return branch(L, not1(L, flag(L, M6502_N)), rel(L, L->b1));
    case O_BMI: return branch(L, flag(L, M6502_N), rel(L, L->b1));
    case O_BVC: return branch(L, not1(L, flag(L, M6502_V)), rel(L, L->b1));
    case O_BVS: return branch(L, flag(L, M6502_V), rel(L, L->b1));
    case O_BCC: return branch(L, not1(L, flag(L, M6502_C)), rel(L, L->b1));
    case O_BCS: return branch(L, flag(L, M6502_C), rel(L, L->b1));
    case O_BNE: return branch(L, not1(L, flag(L, M6502_Z)), rel(L, L->b1));
    case O_BEQ: return branch(L, flag(L, M6502_Z), rel(L, L->b1));
    case O_BRA: return jump(L, rel(L, L->b1));

    case O_NOP:
        /* The NMOS NOPs with an operand read it (a device register sees the read). */
        if (!L->cfg->cmos && L->d.mode != M_IMP && L->d.mode != M_IMM)
            (void)rd(L, ea(L, NULL));
        return LIFT_CONTINUE;

    /* 65C02 */
    case O_TRB: case O_TSB:
        v = rmw_read(L, &a);
        r = o == O_TSB ? or8(L, v, get8(L, M6502_A))
                       : and8(L, v, bin(L, UAM_OP_XOR, 8, get8(L, M6502_A), imm(L, 0xFF)));
        f.z = bin(L, UAM_OP_EQ, 8, and8(L, get8(L, M6502_A), v), imm(L, 0));
        wr(L, a, r);
        break;
    case O_RMB: case O_SMB: {
        const unsigned m = 1u << ((L->op >> 4) & 7);
        a = imm(L, L->b1);
        v = rd(L, a);
        wr(L, a, o == O_SMB ? or8(L, v, imm(L, m)) : and8(L, v, imm(L, 0xFF & ~m)));
        return LIFT_CONTINUE;
    }
    case O_BBR: case O_BBS: {
        uam_ref b = bit(L, 8, rd(L, imm(L, L->b1)), (L->op >> 4) & 7);
        return branch(L, o == O_BBS ? b : not1(L, b), rel(L, L->b2));
    }
    case O_WAI:
        uam_wait(L->b, imm(L, next_pc(L)));
        return LIFT_END;
    case O_STP: case O_JAM:
        uam_syswr(L->b, 8, M6502_SYS_STOP, imm(L, o == O_JAM), NONE);
        uam_wait(L->b, imm(L, L->pc));
        return LIFT_END;

    /* NMOS combined instructions */
    case O_SLO: case O_RLA: case O_SRE: case O_RRA: {
        static const uint8_t sh[] = { [O_SLO] = O_ASL, [O_RLA] = O_ROL, [O_SRE] = O_LSR, [O_RRA] = O_ROR };
        v = rmw_read(L, &a);
        r = shift(L, sh[o], v, &c);
        rmw_write(L, a, v, r);
        uam_ref acc = get8(L, M6502_A), res;
        if (o == O_RRA) {
            res = add(L, false, acc, r, c, &f);
        } else {
            res = bin(L, o == O_SLO ? UAM_OP_OR : o == O_RLA ? UAM_OP_AND : UAM_OP_XOR, 8, acc, r);
            nz(L, &f, res);
            f.c = c;
        }
        put8(L, M6502_A, res);
        break;
    }
    case O_DCP: case O_ISC:
        v = rmw_read(L, &a);
        r = bin(L, o == O_ISC ? UAM_OP_ADD : UAM_OP_SUB, 8, v, imm(L, 1));
        rmw_write(L, a, v, r);
        if (o == O_DCP) {
            compare(L, get8(L, M6502_A), r, &f);
        } else {
            put8(L, M6502_A, add(L, true, get8(L, M6502_A), r, flag(L, M6502_C), &f));
        }
        break;
    case O_ANC:
        r = and8(L, get8(L, M6502_A), imm(L, L->b1));
        nz(L, &f, r);
        f.c = f.n;
        put8(L, M6502_A, r);
        break;
    case O_ALR:
        v = and8(L, get8(L, M6502_A), imm(L, L->b1));
        f.c = bit(L, 8, v, 0);
        r = bin(L, UAM_OP_LSHR, 8, v, imm(L, 1));
        nz(L, &f, r);
        put8(L, M6502_A, r);
        break;
    case O_ARR:
        v = and8(L, get8(L, M6502_A), imm(L, L->b1));
        if (L->dec) {
            uam_ref p = uam_helper(L->b, &uam_isa_m6502.helpers, M6502_HELPER_ARR_DEC, v, get8(L, M6502_C),
                                   NONE, NONE);
            f = (flags){ bit(L, 16, p, M6502_DEC_C), bit(L, 16, p, M6502_DEC_Z), bit(L, 16, p, M6502_DEC_V),
                         bit(L, 16, p, M6502_DEC_N) };
            r = trunc_(L, 8, p);
        } else {
            r = or8(L, bin(L, UAM_OP_LSHR, 8, v, imm(L, 1)),
                    bin(L, UAM_OP_SHL, 8, get8(L, M6502_C), imm(L, 7)));
            nz(L, &f, r);
            f.c = bit(L, 8, r, 6);
            f.v = bin(L, UAM_OP_XOR, 1, bit(L, 8, r, 6), bit(L, 8, r, 5));
        }
        put8(L, M6502_A, r);
        break;
    case O_SBX: {
        uam_ref t = and8(L, get8(L, M6502_A), get8(L, M6502_X));
        compare(L, t, imm(L, L->b1), &f);
        put8(L, M6502_X, bin(L, UAM_OP_SUB, 8, t, imm(L, L->b1)));
        break;
    }
    case O_XAA: case O_LXA: {
        r = and8(L, or8(L, get8(L, M6502_A), imm(L, L->cfg->magic)), imm(L, L->b1));
        if (o == O_XAA)
            r = and8(L, r, get8(L, M6502_X));
        nz(L, &f, r);
        put8(L, M6502_A, r);
        if (o == O_LXA)
            put8(L, M6502_X, r);
        break;
    }
    case O_LAS:
        r = and8(L, rd(L, ea(L, NULL)), get8(L, M6502_S));
        nz(L, &f, r);
        put8(L, M6502_A, r);
        put8(L, M6502_X, r);
        put8(L, M6502_S, r);
        break;
    case O_SHA: store_hi(L, and8(L, get8(L, M6502_A), get8(L, M6502_X))); return LIFT_CONTINUE;
    case O_SHX: store_hi(L, get8(L, M6502_X)); return LIFT_CONTINUE;
    case O_SHY: store_hi(L, get8(L, M6502_Y)); return LIFT_CONTINUE;
    case O_TAS:
        v = and8(L, get8(L, M6502_A), get8(L, M6502_X));
        store_hi(L, v);
        put8(L, M6502_S, v);
        return LIFT_CONTINUE;
    default:
        return LIFT_CONTINUE;
    }
    put_flags(L, &f);
    return LIFT_CONTINUE;
}

uam_status m6502_lift(uam_lift_ctx *lc)
{
    const m6502_state *st = m6502_st(lc->cpu);
    const uam_m6502_config *cfg = &st->cfg;
    const bool dec = cfg->decimal && (lc->cpu->priv & M6502_PRIV_D);
    uint16_t pc = (uint16_t)lc->pc;

    for (uint32_t n = 0;; n++) {
        if (n >= lc->max_insns) {
            uam_exit(lc->b, NONE, uam_imm(lc->b, pc), UAM_EXIT_JUMP);
            return UAM_OK;
        }
        uint64_t byte[3] = { 0, 0, 0 };
        uam_status s = lc->fetch(lc, pc, 1, &byte[0]);
        const unsigned len = s == UAM_OK ? m6502_insn_len(cfg, (uint8_t)byte[0]) : 1;
        for (unsigned k = 1; s == UAM_OK && k < len; k++)
            s = lc->fetch(lc, (uint16_t)(pc + k), 1, &byte[k]);
        if (s != UAM_OK) {
            if (n == 0)
                return UAM_ERR_FAULT;
            uam_exit(lc->b, NONE, uam_imm(lc->b, pc), UAM_EXIT_JUMP);
            return UAM_OK;
        }
        m65_lifter L = { .lc = lc, .b = lc->b, .cfg = cfg, .pc = pc, .len = len,
                         .op = (uint8_t)byte[0], .b1 = (uint8_t)byte[1], .b2 = (uint8_t)byte[2],
                         .d = decode(cfg, (uint8_t)byte[0]), .dec = dec };
        uam_imark(lc->b, pc, len);
        const lift_result r = lift_insn(&L);
        pc = next_pc(&L);
        if (r == LIFT_END)
            return UAM_OK;
        if (L.end) {
            uam_exit(lc->b, NONE, uam_imm(lc->b, pc), UAM_EXIT_JUMP);
            return UAM_OK;
        }
    }
}
