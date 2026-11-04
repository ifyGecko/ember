/*
 * mips_umips.c - The microMIPS32 instruction set (release 3). A block runs
 * microMIPS when bit 0 of the pc is set and the processor has microMIPS
 * (Config3.ISA != 0).
 *
 * umips_decode() is pure, like mips16_decode: an instruction becomes the MIPS32
 * instruction that does the same (lifted by mips_lift32), or one of the forms
 * MIPS32 cannot express - branches and jumps, ADDIUPC, LWM/SWM, LWP/SWP, MOVEP,
 * JRADDIUSP, LWXS. A halfword whose bits 12:10 are 000 or whose bit 12 is set
 * starts a 32-bit instruction (first halfword high); otherwise it is a 16-bit
 * one. 16-bit forms address eight registers through small maps. Branches and
 * jumps have one delay slot of either size (the "S" forms and JALRS need a
 * 16-bit one), except the compact BEQZC/BNEZC/JRC/JRADDIUSP; a link is the
 * address after the slot, with bit 0 set. Branch offsets count halfwords from
 * the end of the branch.
 */
#include "mips.h"

static const uint8_t m16_map[8] = { 16, 17, 2, 3, 4, 5, 6, 7 };
static const uint8_t q_map[8]   = { 0, 17, 2, 3, 4, 5, 6, 7 };
static const uint8_t mn_map[8]  = { 0, 17, 2, 3, 16, 18, 19, 20 };
static const uint8_t h_map1[8]  = { 5, 5, 6, 4, 4, 4, 4, 4 };
static const uint8_t h_map2[8]  = { 6, 7, 7, 21, 22, 5, 6, 7 };
static const int32_t addiur2_imm[8] = { 1, 4, 8, 12, 16, 20, 24, -1 };
static const uint32_t andi16_imm[16] = { 128, 1, 2, 3, 4, 7, 8, 15, 16, 31, 32, 63, 64, 255,
                                         32768, 65535 };
#define GP 28u
#define SP 29u
#define RA 31u

#define R_TYPE(op, rs, rt, rd, sa, fn) \
    (((uint32_t)(op) << 26) | ((uint32_t)(rs) << 21) | ((uint32_t)(rt) << 16) | \
     ((uint32_t)(rd) << 11) | ((uint32_t)(sa) << 6) | (uint32_t)(fn))
#define I_TYPE(op, rs, rt, imm) \
    (((uint32_t)(op) << 26) | ((uint32_t)(rs) << 21) | ((uint32_t)(rt) << 16) | (uint16_t)(imm))
#define COP0(sub, rt, rd, low) (((uint32_t)0x10 << 26) | ((uint32_t)(sub) << 21) | \
    ((uint32_t)(rt) << 16) | ((uint32_t)(rd) << 11) | (uint32_t)(low))

static uint32_t bits(uint32_t v, unsigned hi, unsigned lo) { return (v >> lo) & ((1u << (hi - lo + 1)) - 1); }
static uint32_t sx(uint32_t v, unsigned n) { return (uint32_t)uam_sext(v, n); }

bool umips_is32(uint16_t hw) { return (hw & 0x1C00) == 0 || (hw & 0x1000); }

static void w32(um_insn *d, uint32_t w) { d->kind = UM_W32; d->w = w; }

static void branch(um_insn *d, uint32_t pcb, um_cond c, unsigned rs, unsigned rt, uint32_t off)
{
    d->kind = UM_BR;
    d->cond = c;
    d->rs = rs;
    d->rt = rt;
    d->target = (pcb + d->len + off) | 1u;
}

/* ---- 16-bit ------------------------------------------------------------ */

static void decode16(uint32_t pcb, uint16_t i, um_insn *d)
{
    unsigned op = i >> 10;
    unsigned r97 = bits(i, 9, 7), r64 = bits(i, 6, 4), r31 = bits(i, 3, 1);
    switch (op) {
    case 0x01: /* ADDU16 / SUBU16 rd, rs(3:1), rt(6:4) */
        w32(d, R_TYPE(0, m16_map[r31], m16_map[r64], m16_map[r97], 0, (i & 1) ? 0x23 : 0x21));
        return;
    case 0x02: { /* LBU16 rt, off(base): off -1..14 */
        uint32_t off = bits(i, 3, 0) == 15 ? 0xFFFFFFFFu : bits(i, 3, 0);
        w32(d, I_TYPE(0x24, m16_map[r64], m16_map[r97], off));
        return;
    }
    case 0x03: /* MOVE16 rd(9:5), rs(4:0) */
        w32(d, R_TYPE(0, bits(i, 4, 0), 0, bits(i, 9, 5), 0, 0x21));
        return;
    case 0x09: { /* SLL16 / SRL16 rd, rt, sa (1..8) */
        unsigned sa = r31 ? r31 : 8;
        w32(d, R_TYPE(0, 0, m16_map[r64], m16_map[r97], sa, (i & 1) ? 0x02 : 0x00));
        return;
    }
    case 0x0A: /* LHU16 */
        w32(d, I_TYPE(0x25, m16_map[r64], m16_map[r97], bits(i, 3, 0) << 1));
        return;
    case 0x0B: /* ANDI16 */
        w32(d, I_TYPE(0x0C, m16_map[r64], m16_map[r97], andi16_imm[bits(i, 3, 0)]));
        return;
    case 0x11: { /* POOL16C */
        unsigned rd = m16_map[bits(i, 5, 3)], rs = m16_map[bits(i, 2, 0)];
        switch (bits(i, 9, 6)) {
        case 0x0: w32(d, R_TYPE(0, rs, 0, rd, 0, 0x27)); return;   /* NOT16 */
        case 0x1: w32(d, R_TYPE(0, rd, rs, rd, 0, 0x26)); return;  /* XOR16 */
        case 0x2: w32(d, R_TYPE(0, rd, rs, rd, 0, 0x24)); return;  /* AND16 */
        case 0x3: w32(d, R_TYPE(0, rd, rs, rd, 0, 0x25)); return;  /* OR16 */
        case 0x4: case 0x5: { /* LWM16 / SWM16 {s0..s(k), ra}, off(sp) */
            unsigned k = bits(i, 5, 4);
            d->kind = bits(i, 9, 6) == 4 ? UM_LWM : UM_SWM;
            d->rs = SP;
            d->imm = bits(i, 3, 0) << 2;
            d->nregs = 0;
            for (unsigned r = 0; r <= k; r++)
                d->regs[d->nregs++] = (uint8_t)(16 + r);
            d->regs[d->nregs++] = RA;
            return;
        }
        default: break;
        }
        switch (bits(i, 9, 5)) {
        case 0x0C: case 0x0D: case 0x0E: case 0x0F: /* JR16 JRC JALR16 JALRS16 */
            d->kind = UM_JR;
            d->rs = bits(i, 4, 0);
            d->compact = bits(i, 9, 5) == 0x0D;
            d->link = bits(i, 9, 5) >= 0x0E;
            d->short_slot = bits(i, 9, 5) == 0x0F;
            d->rd = RA;
            return;
        case 0x10: w32(d, R_TYPE(0, 0, 0, bits(i, 4, 0), 0, 0x10)); return;  /* MFHI16 */
        case 0x12: w32(d, R_TYPE(0, 0, 0, bits(i, 4, 0), 0, 0x12)); return;  /* MFLO16 */
        case 0x18: /* JRADDIUSP */
            d->kind = UM_JRADDIUSP;
            d->imm = bits(i, 4, 0) << 2;
            return;
        case 0x14: /* BREAK16 / SDBBP16 by bit 4 */
            if (i & 0x10)
                break;
            w32(d, (bits(i, 3, 0) << 16) | 0x0D);
            return;
        case 0x16:
            if (i & 0x10)
                break;
            w32(d, 0x7000003Fu);                     /* SDBBP16 */
            return;
        default: break;
        }
        return;   /* reserved */
    }
    case 0x12: /* LWSP16 rt(9:5), off(sp) */
        w32(d, I_TYPE(0x23, SP, bits(i, 9, 5), bits(i, 4, 0) << 2));
        return;
    case 0x13: /* POOL16D: ADDIUS5 rd(9:5), simm4 / ADDIUSP simm9 << 2 */
        if (!(i & 1)) {
            w32(d, I_TYPE(0x09, bits(i, 9, 5), bits(i, 9, 5), sx(bits(i, 4, 1), 4)));
        } else {
            int32_t v = (int32_t)sx(bits(i, 9, 1), 9);
            if (v >= -2 && v <= 1)
                v += v < 0 ? -256 : 256;               /* -2..1 encode -258,-257,256,257 */
            w32(d, I_TYPE(0x09, SP, SP, (uint32_t)v << 2));
        }
        return;
    case 0x19: /* LWGP rt, off(gp) */
        w32(d, I_TYPE(0x23, GP, m16_map[r97], sx(bits(i, 6, 0), 7) << 2));
        return;
    case 0x1A: /* LW16 */
        w32(d, I_TYPE(0x23, m16_map[r64], m16_map[r97], bits(i, 3, 0) << 2));
        return;
    case 0x1B: /* POOL16E: ADDIUR2 rd, rs, imm / ADDIUR1SP rd, sp, imm */
        if (!(i & 1))
            w32(d, I_TYPE(0x09, m16_map[r64], m16_map[r97], (uint32_t)addiur2_imm[r31]));
        else
            w32(d, I_TYPE(0x09, SP, m16_map[r97], bits(i, 6, 1) << 2));
        return;
    case 0x21: /* MOVEP rd1, rd2, rs(3:1), rt(6:4) */
        if (i & 1)
            return;
        d->kind = UM_MOVEP;
        d->regs[0] = h_map1[r97];
        d->regs[1] = h_map2[r97];
        d->nregs = 2;
        d->rs = mn_map[r31];
        d->rt = mn_map[r64];
        return;
    case 0x22: /* SB16 */
        w32(d, I_TYPE(0x28, m16_map[r64], q_map[r97], bits(i, 3, 0)));
        return;
    case 0x23: case 0x2B: /* BEQZ16 / BNEZ16 */
        branch(d, pcb, op == 0x23 ? UC_EQ : UC_NE, m16_map[r97], 0, sx(bits(i, 6, 0), 7) << 1);
        return;
    case 0x2A: /* SH16 */
        w32(d, I_TYPE(0x29, m16_map[r64], q_map[r97], bits(i, 3, 0) << 1));
        return;
    case 0x32: /* SWSP16 */
        w32(d, I_TYPE(0x2B, SP, bits(i, 9, 5), bits(i, 4, 0) << 2));
        return;
    case 0x33: /* B16 */
        branch(d, pcb, UC_ALWAYS, 0, 0, sx(bits(i, 9, 0), 10) << 1);
        return;
    case 0x3A: /* SW16 */
        w32(d, I_TYPE(0x2B, m16_map[r64], q_map[r97], bits(i, 3, 0) << 2));
        return;
    case 0x3B: { /* LI16 rd, -1..126 */
        uint32_t v = bits(i, 6, 0) == 127 ? 0xFFFFFFFFu : bits(i, 6, 0);
        w32(d, I_TYPE(0x09, 0, m16_map[r97], v));
        return;
    }
    default:
        return;
    }
}

/* ---- 32-bit ------------------------------------------------------------ */

/* POOL32A minor 0x3C (POOL32Axf): the operation in bits 15:6. */
static void decode_axf(uint32_t i, um_insn *d)
{
    unsigned rt = bits(i, 25, 21), rs = bits(i, 20, 16), minor = bits(i, 15, 6);
    /* MFC0 / MTC0 rt, rs, sel: bits 10:6 = 0x03 / 0x0B, sel in 13:11 */
    if (!bits(i, 15, 14) && (bits(i, 10, 6) == 0x03 || bits(i, 10, 6) == 0x0B)) {
        w32(d, COP0(bits(i, 10, 6) == 0x03 ? 0 : 4, rt, rs, bits(i, 13, 11)));
        return;
    }
    switch (minor) {
    case 0x0AC: w32(d, (0x1Fu << 26) | R_TYPE(0, 0, rs, rt, 0x10, 0x20)); return; /* SEB rt, rs */
    case 0x0EC: w32(d, (0x1Fu << 26) | R_TYPE(0, 0, rs, rt, 0x18, 0x20)); return; /* SEH */
    case 0x1EC: w32(d, (0x1Fu << 26) | R_TYPE(0, 0, rs, rt, 0x02, 0x20)); return; /* WSBH */
    case 0x12C: w32(d, R_TYPE(0x1C, rs, rt, rt, 0, 0x21)); return;  /* CLO rt, rs */
    case 0x16C: w32(d, R_TYPE(0x1C, rs, rt, rt, 0, 0x20)); return;  /* CLZ */
    case 0x1AC: w32(d, (0x1Fu << 26) | R_TYPE(0, 0, rt, rs, 0, 0x3B)); return; /* RDHWR rt, rs */
    case 0x22C: case 0x26C: case 0x2AC: case 0x2EC:   /* MULT MULTU DIV DIVU rs, rt */
    case 0x32C: case 0x36C: case 0x3AC: case 0x3EC: { /* MADD MADDU MSUB MSUBU */
        static const uint8_t fn[8] = { 0x18, 0x19, 0x1A, 0x1B, 0x00, 0x01, 0x04, 0x05 };
        unsigned k = (minor - 0x22C) >> 6;
        w32(d, R_TYPE(k < 4 ? 0 : 0x1C, rs, rt, 0, 0, fn[k]));
        return;
    }
    case 0x03C: case 0x07C: case 0x13C: case 0x17C: /* JALR JALR.HB JALRS JALRS.HB rt, rs */
        d->kind = UM_JR;
        d->rs = rs;
        d->rd = rt;
        d->link = rt != 0;
        d->short_slot = minor >= 0x13C;
        return;
    case 0x035: if (!rt) w32(d, R_TYPE(0, 0, 0, rs, 0, 0x10)); return;  /* MFHI rs */
    case 0x075: if (!rt) w32(d, R_TYPE(0, 0, 0, rs, 0, 0x12)); return;  /* MFLO */
    case 0x0B5: if (!rt) w32(d, R_TYPE(0, rs, 0, 0, 0, 0x11)); return;  /* MTHI */
    case 0x0F5: if (!rt) w32(d, R_TYPE(0, rs, 0, 0, 0, 0x13)); return;  /* MTLO */
    case 0x385: w32(d, COP0(0x0A, rs, rt, 0)); return;         /* RDPGPR rt, rs */
    case 0x3C5: w32(d, COP0(0x0E, rs, rt, 0)); return;         /* WRPGPR rt, rs */
    case 0x11D: if (!rt) w32(d, COP0(0x0B, rs, 12, 0)); return;    /* DI rs */
    case 0x15D: if (!rt) w32(d, COP0(0x0B, rs, 12, 0x20)); return; /* EI rs */
    case 0x00D: case 0x04D: case 0x08D: case 0x0CD:            /* TLBP TLBR TLBWI TLBWR */
    case 0x3CD: case 0x38D: case 0x34D: {                      /* ERET DERET IRET */
        static const struct { uint16_t m; uint8_t fn; } t[] = {
            { 0x00D, 0x08 }, { 0x04D, 0x01 }, { 0x08D, 0x02 }, { 0x0CD, 0x06 },
            { 0x3CD, 0x18 }, { 0x38D, 0x1F }, { 0x34D, 0x38 } };
        if (rt || rs)
            return;
        for (unsigned k = 0; k < UAM_ARRAY_LEN(t); k++)
            if (t[k].m == minor)
                w32(d, COP0(0x10, 0, 0, t[k].fn));
        return;
    }
    case 0x24D: w32(d, COP0(0x10, 0, 0, 0x20)); return;        /* WAIT code */
    case 0x22D: w32(d, 0x0000000Cu); return;                   /* SYSCALL code */
    case 0x36D: w32(d, 0x7000003Fu); return;                   /* SDBBP code */
    case 0x1AD: if (!rt) w32(d, 0x0000000Fu); return;          /* SYNC stype */
    default:
        return;
    }
}

static void decode_pool32a(uint32_t i, um_insn *d)
{
    unsigned rt = bits(i, 25, 21), rs = bits(i, 20, 16), rd = bits(i, 15, 11), sa = bits(i, 15, 11);
    unsigned f11 = bits(i, 10, 0);
    switch (bits(i, 5, 0)) {
    case 0x00: /* shifts by immediate: SLL SRL SRA ROTR rt, rs, sa (bits 10:6 select) */
        switch (bits(i, 10, 6)) {
        case 0x0: w32(d, R_TYPE(0, 0, rs, rt, sa, 0x00)); return;
        case 0x1: w32(d, R_TYPE(0, 0, rs, rt, sa, 0x02)); return;
        case 0x2: w32(d, R_TYPE(0, 0, rs, rt, sa, 0x03)); return;
        case 0x3: w32(d, R_TYPE(0, 1, rs, rt, sa, 0x02)); return;
        default: break;
        }
        break;
    case 0x07: /* BREAK code */
        w32(d, (bits(i, 25, 16) << 16) | 0x0D);
        return;
    case 0x0C: /* INS rt, rs, pos (10:6), msb (15:11) */
        w32(d, (0x1Fu << 26) | R_TYPE(0, rs, rt, bits(i, 15, 11), bits(i, 10, 6), 0x04));
        return;
    case 0x2C: /* EXT rt, rs, pos, size-1 */
        w32(d, (0x1Fu << 26) | R_TYPE(0, rs, rt, bits(i, 15, 11), bits(i, 10, 6), 0x00));
        return;
    case 0x3C: { /* traps (bits 11:6 select, code in 15:12) or POOL32Axf */
        static const uint8_t tfn[0x40] = { [0x00] = 0x34, [0x08] = 0x30, [0x10] = 0x31,
                                           [0x20] = 0x32, [0x28] = 0x33, [0x30] = 0x36 };
        unsigned t = bits(i, 11, 6);
        if (tfn[t] || t == 0) {
            /* TEQ TGE TGEU TLT TLTU TNE rs, rt, code */
            w32(d, R_TYPE(0, rs, rt, 0, 0, tfn[t]) | (bits(i, 15, 12) << 6));
            return;
        }
        decode_axf(i, d);
        return;
    }
    default:
        break;
    }
    /* three-register forms: rd, rs, rt (fields: rt 25:21, rs 20:16, rd 15:11) */
    switch (f11) {
    /* SLLV rd, rt (value, 25:21), rs (amount, 20:16) */
    case 0x010: w32(d, R_TYPE(0, rs, rt, rd, 0, 0x04)); return;
    case 0x050: w32(d, R_TYPE(0, rs, rt, rd, 0, 0x06)); return;  /* SRLV */
    case 0x090: w32(d, R_TYPE(0, rs, rt, rd, 0, 0x07)); return;  /* SRAV */
    case 0x0D0: w32(d, R_TYPE(0, rs, rt, rd, 1, 0x06)); return;  /* ROTRV */
    case 0x110: w32(d, R_TYPE(0, rs, rt, rd, 0, 0x20)); return;  /* ADD rd, rs, rt */
    case 0x150: w32(d, R_TYPE(0, rs, rt, rd, 0, 0x21)); return;  /* ADDU */
    case 0x190: w32(d, R_TYPE(0, rs, rt, rd, 0, 0x22)); return;  /* SUB */
    case 0x1D0: w32(d, R_TYPE(0, rs, rt, rd, 0, 0x23)); return;  /* SUBU */
    case 0x210: w32(d, R_TYPE(0x1C, rs, rt, rd, 0, 0x02)); return; /* MUL */
    case 0x250: w32(d, R_TYPE(0, rs, rt, rd, 0, 0x24)); return;  /* AND */
    case 0x290: w32(d, R_TYPE(0, rs, rt, rd, 0, 0x25)); return;  /* OR */
    case 0x2D0: w32(d, R_TYPE(0, rs, rt, rd, 0, 0x27)); return;  /* NOR */
    case 0x310: w32(d, R_TYPE(0, rs, rt, rd, 0, 0x26)); return;  /* XOR */
    case 0x350: w32(d, R_TYPE(0, rs, rt, rd, 0, 0x2A)); return;  /* SLT */
    case 0x390: w32(d, R_TYPE(0, rs, rt, rd, 0, 0x2B)); return;  /* SLTU */
    case 0x018: w32(d, R_TYPE(0, rs, rt, rd, 0, 0x0B)); return;  /* MOVN rd, rs, rt */
    case 0x058: w32(d, R_TYPE(0, rs, rt, rd, 0, 0x0A)); return;  /* MOVZ */
    case 0x118: /* LWXS rd, index(rt)(base rs) */
        d->kind = UM_LWXS;
        d->rd = rd;
        d->rs = rs;
        d->rt = rt;
        return;
    default:
        return;
    }
}

static void decode32(uint32_t pcb, uint32_t i, um_insn *d)
{
    unsigned op = i >> 26, rt = bits(i, 25, 21), rs = bits(i, 20, 16);
    uint32_t imm = i & 0xFFFF, off12 = sx(bits(i, 11, 0), 12);
    switch (op) {
    case 0x00: decode_pool32a(i, d); return;
    case 0x04: w32(d, I_TYPE(0x08, rs, rt, imm)); return;   /* ADDI */
    case 0x0C: w32(d, I_TYPE(0x09, rs, rt, imm)); return;   /* ADDIU */
    case 0x24: w32(d, I_TYPE(0x0A, rs, rt, imm)); return;   /* SLTI */
    case 0x2C: w32(d, I_TYPE(0x0B, rs, rt, imm)); return;   /* SLTIU */
    case 0x34: w32(d, I_TYPE(0x0C, rs, rt, imm)); return;   /* ANDI */
    case 0x14: w32(d, I_TYPE(0x0D, rs, rt, imm)); return;   /* ORI */
    case 0x1C: w32(d, I_TYPE(0x0E, rs, rt, imm)); return;   /* XORI */
    case 0x07: w32(d, I_TYPE(0x20, rs, rt, imm)); return;   /* LB */
    case 0x05: w32(d, I_TYPE(0x24, rs, rt, imm)); return;   /* LBU */
    case 0x0F: w32(d, I_TYPE(0x21, rs, rt, imm)); return;   /* LH */
    case 0x0D: w32(d, I_TYPE(0x25, rs, rt, imm)); return;   /* LHU */
    case 0x3F: w32(d, I_TYPE(0x23, rs, rt, imm)); return;   /* LW */
    case 0x06: w32(d, I_TYPE(0x28, rs, rt, imm)); return;   /* SB */
    case 0x0E: w32(d, I_TYPE(0x29, rs, rt, imm)); return;   /* SH */
    case 0x3E: w32(d, I_TYPE(0x2B, rs, rt, imm)); return;   /* SW */
    case 0x08: /* POOL32B: LWP SWP LWM32 SWM32 (12-bit offsets) */
        switch (bits(i, 15, 12)) {
        case 0x1: case 0x9: /* LWP / SWP rd, off(base): rd, rd+1 */
            if (rt == 31)
                return;
            d->kind = bits(i, 15, 12) == 1 ? UM_LWP : UM_SWP;
            d->rs = rs;
            d->imm = off12;
            d->nregs = 2;
            d->regs[0] = (uint8_t)rt;
            d->regs[1] = (uint8_t)(rt + 1);
            return;
        case 0x5: case 0xD: { /* LWM32 / SWM32 {s0..s(n-1), fp, ra}, off(base) */
            unsigned n = rt & 0xF;
            if (n > 9 || (!n && !(rt & 0x10)))
                return;
            d->kind = bits(i, 15, 12) == 5 ? UM_LWM : UM_SWM;
            d->rs = rs;
            d->imm = off12;
            d->nregs = 0;
            for (unsigned k = 0; k < n; k++)
                d->regs[d->nregs++] = (uint8_t)(k == 8 ? 30 : 16 + k);
            if (rt & 0x10)
                d->regs[d->nregs++] = RA;
            return;
        }
        case 0x3: case 0xB: /* ASET / ACLR bit, off(base) (MCU ASE): MIPS32 REGIMM rt=7 */
            if (bits(i, 25, 24))
                return;
            w32(d, I_TYPE(0x01, rs, 7, ((bits(i, 15, 12) == 3) << 15) | (bits(i, 23, 21) << 12)
                                         | bits(i, 11, 0)));
            return;
        case 0x6: /* CACHE op, off(base) */
            w32(d, I_TYPE(0x2F, rs, rt, off12));
            return;
        default:
            return;
        }
    case 0x18: /* POOL32C: LWL LWR PREF LL SWL SWR SC */
        switch (bits(i, 15, 12)) {
        case 0x0: w32(d, I_TYPE(0x22, rs, rt, off12)); return;
        case 0x1: w32(d, I_TYPE(0x26, rs, rt, off12)); return;
        case 0x2: w32(d, I_TYPE(0x33, rs, rt, off12)); return;
        case 0x3: w32(d, I_TYPE(0x30, rs, rt, off12)); return;
        case 0x8: w32(d, I_TYPE(0x2A, rs, rt, off12)); return;
        case 0x9: w32(d, I_TYPE(0x2E, rs, rt, off12)); return;
        case 0xB: w32(d, I_TYPE(0x38, rs, rt, off12)); return;
        default: return;
        }
    case 0x10: { /* POOL32I: branches, immediate traps, LUI, SYNCI */
        uint32_t boff = sx(imm, 16) << 1;
        switch (rt) {
        case 0x00: branch(d, pcb, UC_LTZ, rs, 0, boff); return;
        case 0x01: branch(d, pcb, UC_LTZ, rs, 0, boff); d->link = true; return;   /* BLTZAL */
        case 0x02: branch(d, pcb, UC_GEZ, rs, 0, boff); return;
        case 0x03: branch(d, pcb, UC_GEZ, rs, 0, boff); d->link = true; return;   /* BGEZAL */
        case 0x04: branch(d, pcb, UC_LEZ, rs, 0, boff); return;
        case 0x06: branch(d, pcb, UC_GTZ, rs, 0, boff); return;
        case 0x05: branch(d, pcb, UC_NE, rs, 0, boff); d->compact = true; return;  /* BNEZC */
        case 0x07: branch(d, pcb, UC_EQ, rs, 0, boff); d->compact = true; return;  /* BEQZC */
        case 0x11: branch(d, pcb, UC_LTZ, rs, 0, boff); d->link = d->short_slot = true; return;
        case 0x13: branch(d, pcb, UC_GEZ, rs, 0, boff); d->link = d->short_slot = true; return;
        case 0x08: w32(d, I_TYPE(0x01, rs, 0x0A, imm)); return;  /* TLTI */
        case 0x09: w32(d, I_TYPE(0x01, rs, 0x08, imm)); return;  /* TGEI */
        case 0x0A: w32(d, I_TYPE(0x01, rs, 0x0B, imm)); return;  /* TLTIU */
        case 0x0B: w32(d, I_TYPE(0x01, rs, 0x09, imm)); return;  /* TGEIU */
        case 0x0C: w32(d, I_TYPE(0x01, rs, 0x0E, imm)); return;  /* TNEI */
        case 0x0E: w32(d, I_TYPE(0x01, rs, 0x0C, imm)); return;  /* TEQI */
        case 0x0D: w32(d, I_TYPE(0x0F, 0, rs, imm)); return;     /* LUI rs, imm */
        case 0x10: d->kind = UM_NOP; return;                     /* SYNCI */
        case 0x1B: if (!rs) branch(d, pcb, UC_POS32, 0, 0, boff); return; /* BPOSGE32 */
        case 0x1C: case 0x1D: /* BC1F / BC1T cc */
            if (bits(i, 17, 16))
                return;
            branch(d, pcb, UC_FCC, 0, 0, boff);
            d->cc = bits(i, 20, 18);
            d->tf = rt & 1;
            return;
        default: return;
        }
    }
    case 0x25: case 0x2D: /* BEQ / BNE rs, rt */
        branch(d, pcb, op == 0x25 ? UC_EQ : UC_NE, rs, rt, sx(imm, 16) << 1);
        return;
    case 0x35: case 0x3D: case 0x1D: /* J / JAL / JALS: 26-bit index << 1 */
        d->kind = UM_J;
        d->target = ((pcb + 4) & 0xF8000000u) | (bits(i, 25, 0) << 1) | 1u;
        d->link = op != 0x35;
        d->short_slot = op == 0x1D;
        return;
    case 0x3C: /* JALX: to MIPS32, index << 2 */
        d->kind = UM_J;
        d->target = ((pcb + 4) & 0xF0000000u) | (bits(i, 25, 0) << 2);
        d->link = true;
        return;
    case 0x15: { /* POOL32F: the FPU */
        uint32_t w = mips_umips_fpu32(i);
        if (w)
            w32(d, w);
        return;
    }
    case 0x27: w32(d, I_TYPE(0x31, rs, rt, imm)); return;   /* LWC1 ft, off(base) */
    case 0x26: w32(d, I_TYPE(0x39, rs, rt, imm)); return;   /* SWC1 */
    case 0x2F: w32(d, I_TYPE(0x35, rs, rt, imm)); return;   /* LDC1 */
    case 0x2E: w32(d, I_TYPE(0x3D, rs, rt, imm)); return;   /* SDC1 */
    case 0x1E: /* ADDIUPC rs(25:23), imm23 << 2 */
        d->kind = UM_ADDIUPC;
        d->rd = m16_map[bits(i, 25, 23)];
        d->imm = sx(bits(i, 22, 0), 23) << 2;
        return;
    default:
        return;
    }
}

void umips_decode(uint32_t pc, uint16_t h0, uint16_t h1, um_insn *d)
{
    uint32_t pcb = pc & ~1u;
    *d = (um_insn){ .kind = UM_RI, .len = umips_is32(h0) ? 4 : 2 };
    if (d->len == 2)
        decode16(pcb, h0, d);
    else {
        uint32_t i = ((uint32_t)h0 << 16) | h1, w;
        decode32(pcb, i, d);
        if (d->kind == UM_RI && (w = mips_umips_dsp32(i)) != 0)
            w32(d, w);
    }
}

/*
 * POOL32F (the FPU, major 0x15) recoded as MIPS32 COP1/COP1X, or 0 if reserved.
 * Fields: a = 25:21, b = 20:16, c = 15:11, d = 10:6.
 */
#define C1(fmt, ft, fs, fd, fn) ((0x11u << 26) | ((uint32_t)(fmt) << 21) | ((uint32_t)(ft) << 16) | \
                                 ((uint32_t)(fs) << 11) | ((uint32_t)(fd) << 6) | (uint32_t)(fn))
#define C1X(r, t, s, d, fn)     ((0x13u << 26) | ((uint32_t)(r) << 21) | ((uint32_t)(t) << 16) | \
                                 ((uint32_t)(s) << 11) | ((uint32_t)(d) << 6) | (uint32_t)(fn))
enum { F_S = 16, F_D = 17, F_W = 20, F_L = 21 };

uint32_t mips_umips_fpu32(uint32_t i)
{
    unsigned a = bits(i, 25, 21), b = bits(i, 20, 16), c = bits(i, 15, 11), d = bits(i, 10, 6);
    switch (bits(i, 5, 0)) {
    case 0x30: /* ADD SUB MUL DIV .fmt fd(c), fs(b), ft(a) */
        if (bits(i, 10, 9))
            return 0;
        return C1(bits(i, 8, 8) ? F_D : F_S, a, b, c, bits(i, 7, 6));
    case 0x01: case 0x09: case 0x21: case 0x29:   /* MADD MSUB .S/.D fd(c), fr(d), fs(b), ft(a) */
    case 0x02: case 0x0A: case 0x22: case 0x2A: { /* NMADD NMSUB */
        unsigned lo = bits(i, 5, 0), kind = ((lo & 2) ? 6 : 4) + ((lo >> 5) & 1);
        return C1X(d, a, b, c, (kind << 3) | ((lo >> 3) & 1));
    }
    case 0x3C: /* C.cond.fmt cc(c[4:2]), fs(b), ft(a) */
        if (bits(i, 12, 11))
            return 0;
        return C1(bits(i, 10, 10) ? F_D : F_S, a, b, bits(i, 15, 13) << 2, 0x30 | bits(i, 9, 6));
    case 0x08: /* indexed loads/stores: index(a), base(b), fd/fs(c) */
        switch (d) {
        case 1: return C1X(b, a, 0, c, 0x00);  /* LWXC1 */
        case 2: return C1X(b, a, c, 0, 0x08);  /* SWXC1 */
        case 3: return C1X(b, a, 0, c, 0x01);  /* LDXC1 */
        case 4: return C1X(b, a, c, 0, 0x09);  /* SDXC1 */
        case 5: return C1X(b, a, 0, c, 0x05);  /* LUXC1 */
        case 6: return C1X(b, a, c, 0, 0x0D);  /* SUXC1 */
        default: return 0;
        }
    case 0x20: /* MOVF/MOVT.fmt fd(a), fs(b), cc(c[4:2]); PREFX hint(c), index(a)(base b) */
        if (d == 6)
            return C1X(b, a, c, 0, 0x0F);
        if (bits(i, 12, 11) || bits(i, 8, 7) || bits(i, 10, 10))
            return 0;                  /* format in 10:9: S, D (PS reserved) */
        return C1(bits(i, 9, 9) ? F_D : F_S, (bits(i, 15, 13) << 2) | bits(i, 6, 6), b, a, 0x11);
    case 0x38: /* MOVZ/MOVN.fmt fd(c), fs(b), rt(a) */
        if (bits(i, 10, 9) || bits(i, 7, 6) > 1)
            return 0;
        return C1(bits(i, 8, 8) ? F_D : F_S, a, b, c, bits(i, 6, 6) ? 0x12 : 0x13);
    case 0x3B: /* POOL32Fxf: fd/rt(a), fs(b) */
        switch (d) {
        case 0: /* moves to and from the FPU: rt(a), fs(b) */
            switch (c) {
            case 2: return C1(0x02, a, b, 0, 0);   /* CFC1 */
            case 3: return C1(0x06, a, b, 0, 0);   /* CTC1 */
            case 4: return C1(0x00, a, b, 0, 0);   /* MFC1 */
            case 5: return C1(0x04, a, b, 0, 0);   /* MTC1 */
            case 6: return C1(0x03, a, b, 0, 0);   /* MFHC1 */
            case 7: return C1(0x07, a, b, 0, 0);   /* MTHC1 */
            default: return 0;
            }
        case 1: /* MOV.fmt */
            if (c != 0 && c != 4)
                return 0;
            return C1(c ? F_D : F_S, 0, b, a, 0x06);
        case 4: /* CVT.L / CVT.W .fmt */
            if (bits(c, 4, 4) || bits(c, 2, 1))
                return 0;
            return C1(bits(c, 3, 3) ? F_D : F_S, 0, b, a, (c & 1) ? 36 : 37);
        case 5: /* MOVF/MOVT rt(a), rs(b), cc: MIPS32 SPECIAL MOVCI */
            if (bits(c, 1, 1))
                return 0;
            return (b << 21) | ((((c >> 2) & 7) << 2 | (c & 1)) << 16) | (a << 11) | 0x01;
        case 8: { /* RSQRT SQRT RECIP .fmt */
            static const uint8_t fn[3] = { 22, 4, 21 };
            if (bits(c, 4, 4) || (c & 7) > 2)
                return 0;
            return C1(bits(c, 3, 3) ? F_D : F_S, 0, b, a, fn[c & 7]);
        }
        case 12: { /* FLOOR CEIL TRUNC ROUND .W/.L .fmt */
            static const uint8_t base[4] = { 3, 2, 1, 0 };   /* floor ceil trunc round -> MIPS32 order */
            if (bits(c, 4, 4))
                return 0;
            return C1(bits(c, 3, 3) ? F_D : F_S, 0, b, a, ((c & 1) ? 12 : 8) + base[bits(c, 2, 1)]);
        }
        case 13: { /* ABS NEG CVT.D CVT.S, format in c[3:2] */
            static const uint8_t f_absneg[2] = { F_S, F_D }, f_cvtd[3] = { F_S, F_W, F_L },
                                 f_cvts[3] = { F_D, F_W, F_L };
            unsigned op = c & 3, f = bits(c, 3, 2);
            if (bits(c, 4, 4))
                return 0;
            switch (op) {
            case 0: case 1: return f < 2 ? C1(f_absneg[f], 0, b, a, op ? 7 : 5) : 0;
            case 2: return f < 3 ? C1(f_cvtd[f], 0, b, a, 33) : 0;
            default: return f < 3 ? C1(f_cvts[f], 0, b, a, 32) : 0;
            }
        }
        default:
            return 0;
        }
    default:
        return 0;
    }
}

/*
 * The DSP ASE (all in POOL32A) recoded as MIPS32, or 0 if not one: a table lookup,
 * the table generated from the assembler (tests/isa-mips/gen-umips-dsp.py).
 */
#include "umips_dsp.h"

uint32_t mips_umips_dsp32(uint32_t i)
{
    for (size_t k = 0; k < sizeof umips_dsp / sizeof umips_dsp[0]; k++) {
        const struct umips_dsp *e = &umips_dsp[k];
        if ((i & e->umask) != e->u)
            continue;
        uint32_t w = e->w;
        for (unsigned b = 0; b < e->n; b++)
            w |= ((i >> e->bit[b][0]) & 1u) << e->bit[b][1];
        return w;
    }
    return 0;
}

/* ---- lifting ----------------------------------------------------------- */

static bool um_control(const um_insn *d)
{
    return d->kind == UM_BR || d->kind == UM_J || d->kind == UM_JR || d->kind == UM_JRADDIUSP;
}

static bool um_has_slot(const um_insn *d)
{
    return (d->kind == UM_BR || d->kind == UM_J || d->kind == UM_JR) && !d->compact;
}

static void lift_multi(ml *L, const um_insn *d)
{
    unsigned fl = L->endian | UAM_ACC_ALIGNED;
    uam_ref base = mips_bin(L, UAM_OP_ADD, mips_reg_r(L, d->rs), mips_imm(L, d->imm));
    if (d->kind == UM_SWM || d->kind == UM_SWP) {
        for (unsigned k = 0; k < d->nregs; k++)
            uam_st(L->b, 32, fl, mips_bin(L, UAM_OP_ADD, base, mips_imm(L, 4 * k)),
                   mips_reg_r(L, d->regs[k]), NONE);
        return;
    }
    uam_ref v[16];                      /* loads first: the commit rule */
    for (unsigned k = 0; k < d->nregs; k++)
        v[k] = uam_ld(L->b, 32, fl, mips_bin(L, UAM_OP_ADD, base, mips_imm(L, 4 * k)), NONE);
    for (unsigned k = 0; k < d->nregs; k++)
        mips_reg_w(L, d->regs[k], v[k]);
}

static lift_result lift_simple(ml *L, const um_insn *d, uint32_t pcbase)
{
    switch (d->kind) {
    case UM_W32:
        return mips_lift32(L, d->w);
    case UM_NOP:
        return LIFT_SEQ;
    case UM_ADDIUPC:
        mips_reg_w(L, d->rd, mips_imm(L, (pcbase & ~3u) + d->imm));
        return LIFT_SEQ;
    case UM_LWM: case UM_SWM: case UM_LWP: case UM_SWP:
        lift_multi(L, d);
        return LIFT_SEQ;
    case UM_MOVEP: {
        uam_ref a = mips_reg_r(L, d->rs), b = mips_reg_r(L, d->rt);
        mips_reg_w(L, d->regs[0], a);
        mips_reg_w(L, d->regs[1], b);
        return LIFT_SEQ;
    }
    case UM_LWXS: {
        uam_ref a = mips_bin(L, UAM_OP_ADD, mips_reg_r(L, d->rs),
                             mips_bin(L, UAM_OP_SHL, mips_reg_r(L, d->rt), mips_imm(L, 2)));
        mips_reg_w(L, d->rd, uam_ld(L->b, 32, L->endian | UAM_ACC_ALIGNED, a, NONE));
        return LIFT_SEQ;
    }
    default:
        mips_ri(L);
        return LIFT_DONE;
    }
}

static uam_ref cond(ml *L, const um_insn *d)
{
    uam_ref a = mips_reg_r(L, d->rs);
    switch (d->cond) {
    case UC_EQ:  return mips_bin(L, UAM_OP_EQ, a, mips_reg_r(L, d->rt));
    case UC_NE:  return mips_bin(L, UAM_OP_NE, a, mips_reg_r(L, d->rt));
    case UC_LTZ: return mips_bin(L, UAM_OP_SLT, a, mips_imm(L, 0));
    case UC_GEZ: return mips_bin(L, UAM_OP_SLE, mips_imm(L, 0), a);
    case UC_LEZ: return mips_bin(L, UAM_OP_SLE, a, mips_imm(L, 0));
    case UC_GTZ: return mips_bin(L, UAM_OP_SLT, mips_imm(L, 0), a);
    case UC_FCC: return mips_fcc(L, d->cc, d->tf);
    case UC_POS32:
        return mips_bin(L, UAM_OP_ULE, mips_imm(L, 32),
                        mips_bin(L, UAM_OP_AND, uam_get(L->b, 32, MIPS_DSPCTL_OFF), mips_imm(L, 0x3F)));
    default:     return NONE;
    }
}

static lift_result lift_control(ml *L, const um_insn *d)
{
    uint32_t pcb = L->pc & ~1u, after = pcb + d->len;
    if (d->kind == UM_BR && d->cond == UC_FCC && !(L->priv & MIPS_PRIV_CU1)) {
        mips_trap(L, NONE, EXC_CPU, mips_imm(L, 1));
        return LIFT_DONE;
    }
    if (d->kind == UM_BR && d->cond == UC_FCC && !mips_has_fpu(L->s)) {
        mips_ri(L);
        return LIFT_DONE;
    }
    if (d->kind == UM_BR && d->cond == UC_POS32 && !mips_has_dsp(L->s)) {
        mips_ri(L);
        return LIFT_DONE;
    }
    if (d->kind == UM_BR && d->cond == UC_POS32 && !(L->priv & MIPS_PRIV_MX)) {
        mips_trap(L, NONE, EXC_DSPDIS, mips_imm(L, 0));
        return LIFT_DONE;
    }

    uam_ref c = d->kind == UM_BR && d->cond != UC_ALWAYS ? cond(L, d) : NONE;
    uam_ref target = d->kind == UM_JR ? mips_reg_r(L, d->rs)
                   : d->kind == UM_JRADDIUSP ? mips_reg_r(L, RA)
                   : mips_imm(L, d->target);

    if (d->kind == UM_JRADDIUSP) {
        mips_reg_w(L, SP, mips_bin(L, UAM_OP_ADD, mips_reg_r(L, SP), mips_imm(L, d->imm)));
        uam_exit(L->b, NONE, target, UAM_EXIT_JUMP);
        return LIFT_DONE;
    }
    if (d->compact) {   /* BEQZC / BNEZC / JRC */
        if (uam_ref_is_none(c)) {
            uam_exit(L->b, NONE, target, UAM_EXIT_JUMP);
        } else {
            uam_exit(L->b, c, target, UAM_EXIT_JUMP);
            uam_exit(L->b, NONE, mips_imm(L, after | 1u), UAM_EXIT_JUMP);
        }
        return LIFT_DONE;
    }

    /* The delay slot (16 or 32 bits). */
    uint64_t s0 = 0, s1 = 0;
    uam_status st = L->lc->fetch(L->lc, after, 2, &s0);
    unsigned slen = st == UAM_OK && umips_is32((uint16_t)s0) ? 4 : 2;
    if (st == UAM_OK && slen == 4)
        st = L->lc->fetch(L->lc, after + 2, 2, &s1);
    uint32_t link = (after + slen) | 1u;
    if (d->link)
        mips_reg_w(L, d->kind == UM_JR ? d->rd : RA, mips_imm(L, link));
    uam_imark_cont(L->b, after | 1u, slen);
    if (st != UAM_OK) {
        mips_trap(L, NONE, MIPS_TRAP_SLOT_FETCH, mips_imm(L, after | 1u));
        return LIFT_DONE;
    }
    um_insn sd;
    umips_decode(after | 1u, (uint16_t)s0, (uint16_t)s1, &sd);
    if (um_control(&sd) || (d->short_slot && slen != 2)) {
        mips_ri(L);
        return LIFT_DONE;
    }
    ml slot = *L;
    slot.pc = after | 1u;
    slot.npc = link;
    slot.raw = slen == 4 ? ((uint32_t)s0 << 16) | (uint16_t)s1 : (uint16_t)s0;
    if (lift_simple(&slot, &sd, pcb) == LIFT_DONE)
        return LIFT_DONE;
    if (uam_ref_is_none(c)) {
        uam_exit(L->b, NONE, target, UAM_EXIT_JUMP);
    } else {
        uam_exit(L->b, c, target, UAM_EXIT_JUMP);
        uam_exit(L->b, NONE, mips_imm(L, link), UAM_EXIT_JUMP);
    }
    return LIFT_DONE;
}

uam_status mips_umips_lift(uam_lift_ctx *lc)
{
    const mips_state *s = mips_st(lc->cpu);
    ml L = { .lc = lc, .b = lc->b, .s = s, .endian = s->endian, .priv = lc->cpu->priv };
    uint32_t pc = (uint32_t)lc->pc & ~1u;

    for (uint32_t n = 0;; n++) {
        if (n >= lc->max_insns) {
            uam_exit(lc->b, NONE, uam_imm(lc->b, pc | 1u), UAM_EXIT_JUMP);
            return UAM_OK;
        }
        uint64_t h0 = 0, h1 = 0;
        uam_status st = lc->fetch(lc, pc, 2, &h0);
        if (st == UAM_OK && umips_is32((uint16_t)h0))
            st = lc->fetch(lc, pc + 2, 2, &h1);
        if (st != UAM_OK) {
            if (n == 0)
                return UAM_ERR_FAULT;
            uam_exit(lc->b, NONE, uam_imm(lc->b, pc | 1u), UAM_EXIT_JUMP);
            return UAM_OK;
        }
        um_insn d;
        umips_decode(pc | 1u, (uint16_t)h0, (uint16_t)h1, &d);

        /* A branch needs its delay slot in the same block. */
        if (um_has_slot(&d) && n > 0) {
            uint64_t t;
            if (n + 2 > lc->max_insns || lc->fetch(lc, pc + d.len, 2, &t) != UAM_OK
                || (umips_is32((uint16_t)t) && lc->fetch(lc, pc + d.len + 2, 2, &t) != UAM_OK)) {
                uam_exit(lc->b, NONE, uam_imm(lc->b, pc | 1u), UAM_EXIT_JUMP);
                return UAM_OK;
            }
        }

        uam_imark(lc->b, pc | 1u, d.len);
        L.pc = pc | 1u;
        L.npc = (pc + d.len) | 1u;
        L.raw = d.len == 4 ? ((uint32_t)h0 << 16) | (uint16_t)h1 : (uint16_t)h0;
        lift_result r = um_control(&d) ? lift_control(&L, &d) : lift_simple(&L, &d, pc);
        if (r == LIFT_DONE)
            return UAM_OK;
        pc += d.len;
    }
}
