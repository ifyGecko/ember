/*
 * mips16.c - The MIPS16e instruction set (MD00076). A block runs MIPS16e when
 * bit 0 of the pc is set.
 *
 * mips16_decode() turns one instruction (with its EXTEND prefix, or the second
 * halfword of JAL/JALX) into either the MIPS32 instruction that does the same
 * thing - lifted by mips_lift32, so the ALU, memory and multiply semantics are
 * shared - or one of the forms MIPS32 cannot express: branches, jumps, the
 * PC-relative ADDIUPC and LWPC, and SAVE/RESTORE. It is a pure function, so a
 * test can compare its field decoding with a disassembler on every encoding.
 *
 * Eight registers are directly addressable; the 3-bit fields map to GPRs
 * {16,17,2,3,4,5,6,7}. T is $24 (the result of SLT/CMP and the condition of
 * BTEQZ/BTNEZ). JAL, JALX, JR and JALR have one delay slot (a 16-bit,
 * non-extended instruction, lifted under a continuation IMARK); their link is
 * the address after the slot, with bit 0 set. JRC, JALRC and the branches have
 * no delay slot. PC-relative instructions use the instruction's address with
 * bits 1:0 cleared (in a jump's delay slot, the jump's address).
 */
#include "mips.h"

static const uint8_t m16_map[8] = { 16, 17, 2, 3, 4, 5, 6, 7 };
#define T_REG 24u
#define SP    29u
#define RA    31u

#define R_TYPE(rs, rt, rd, sa, fn) \
    (((uint32_t)(rs) << 21) | ((uint32_t)(rt) << 16) | ((uint32_t)(rd) << 11) | \
     ((uint32_t)(sa) << 6) | (uint32_t)(fn))
#define I_TYPE(op, rs, rt, imm) \
    (((uint32_t)(op) << 26) | ((uint32_t)(rs) << 21) | ((uint32_t)(rt) << 16) | (uint16_t)(imm))

static uint32_t bits(uint32_t v, unsigned hi, unsigned lo) { return (v >> lo) & ((1u << (hi - lo + 1)) - 1); }
static uint32_t sx(uint32_t v, unsigned n) { return (uint32_t)uam_sext(v, n); }

/* The 16-bit immediate an EXTEND prefix forms with the instruction's low 5 bits. */
static uint32_t ext16(uint16_t ext, uint16_t i)
{
    return (bits(ext, 4, 0) << 11) | (bits(ext, 10, 5) << 5) | bits(i, 4, 0);
}

static bool extendable(unsigned op, uint16_t i)
{
    switch (op) {
    case 0x03: case 0x0F: case 0x17: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
        return false;
    case 0x0C: {
        unsigned f = bits(i, 10, 8);
        return f <= 4;                 /* BTEQZ BTNEZ SWRASP ADJSP SVRS */
    }
    default:
        return true;
    }
}

bool mips16_decode(uint32_t pc, uint16_t ext, bool has_ext, uint16_t i, uint16_t i2,
                   m16_insn *d)
{
    unsigned op = i >> 11;
    unsigned rx = m16_map[bits(i, 10, 8)], ry = m16_map[bits(i, 7, 5)], rz = m16_map[bits(i, 4, 2)];
    uint32_t pcb = pc & ~1u;           /* the instruction's address */
    *d = (m16_insn){ .kind = M16_RI, .len = has_ext ? 4 : 2 };

    if (op == 0x03) {                  /* JAL / JALX: 32 bits, a delay slot */
        if (has_ext)
            return false;
        uint32_t idx = (bits(i, 4, 0) << 21) | (bits(i, 9, 5) << 16) | i2;
        d->len = 4;
        d->kind = M16_JAL;
        d->link = true;
        d->target = ((pcb + 4) & 0xF0000000u) | (idx << 2) | ((i >> 10) & 1 ? 0u : 1u);
        return true;
    }
    if (has_ext && !extendable(op, i))
        return true;                   /* reserved */

    uint32_t imm;
    switch (op) {
    case 0x00: /* ADDIU rx, sp, imm */
        imm = has_ext ? sx(ext16(ext, i), 16) : bits(i, 7, 0) << 2;
        d->kind = M16_W32;
        d->w = I_TYPE(0x09, SP, rx, imm);
        return true;
    case 0x01: /* ADDIUPC rx, imm */
    case 0x16: /* LWPC rx, imm */
        imm = has_ext ? sx(ext16(ext, i), 16) : bits(i, 7, 0) << 2;
        d->kind = op == 0x01 ? M16_ADDIUPC : M16_LWPC;
        d->rd = rx;
        d->imm = imm;
        d->target = (pcb & ~3u) + imm;   /* when not in a delay slot */
        return true;
    case 0x02: /* B */
        imm = has_ext ? sx(ext16(ext, i), 16) << 1 : sx(bits(i, 10, 0), 11) << 1;
        d->kind = M16_B;
        d->target = (pcb + d->len + imm) | 1u;
        return true;
    case 0x04: case 0x05: /* BEQZ / BNEZ rx */
        imm = has_ext ? sx(ext16(ext, i), 16) << 1 : sx(bits(i, 7, 0), 8) << 1;
        d->kind = op == 0x04 ? M16_BEQZ : M16_BNEZ;
        d->rx = rx;
        d->target = (pcb + d->len + imm) | 1u;
        return true;
    case 0x06: { /* SLL/SRL/SRA rx, ry, sa */
        unsigned f = bits(i, 1, 0), sa;
        if (f == 1)
            return true;               /* DSLL (64-bit) */
        if (has_ext) {
            sa = bits(ext, 10, 6);     /* i[4:2] are don't-care when extended */
        } else {
            sa = bits(i, 4, 2);
            if (!sa)
                sa = 8;
        }
        d->kind = M16_W32;
        d->w = R_TYPE(0, ry, rx, sa, f == 0 ? 0x00 : f == 2 ? 0x02 : 0x03);
        return true;
    }
    case 0x08: /* RRI-A: ADDIU ry, rx, imm */
        if (i & 0x10)
            return true;               /* DADDIU */
        imm = has_ext ? sx((bits(ext, 3, 0) << 11) | (bits(ext, 10, 4) << 4) | bits(i, 3, 0), 15)
                      : sx(bits(i, 3, 0), 4);
        d->kind = M16_W32;
        d->w = I_TYPE(0x09, rx, ry, imm);
        return true;
    case 0x09: /* ADDIU rx, imm */
        imm = has_ext ? sx(ext16(ext, i), 16) : sx(bits(i, 7, 0), 8);
        d->kind = M16_W32;
        d->w = I_TYPE(0x09, rx, rx, imm);
        return true;
    case 0x0A: case 0x0B: /* SLTI / SLTIU rx, imm -> T */
        imm = has_ext ? sx(ext16(ext, i), 16) : bits(i, 7, 0);
        d->kind = M16_W32;
        d->w = I_TYPE(op == 0x0A ? 0x0A : 0x0B, rx, T_REG, imm);
        return true;
    case 0x0C: /* I8 */
        switch (bits(i, 10, 8)) {
        case 0: case 1: /* BTEQZ / BTNEZ */
            imm = has_ext ? sx(ext16(ext, i), 16) << 1 : sx(bits(i, 7, 0), 8) << 1;
            d->kind = bits(i, 10, 8) ? M16_BNEZ : M16_BEQZ;
            d->rx = T_REG;
            d->target = (pcb + d->len + imm) | 1u;
            return true;
        case 2: /* SWRASP: sw ra, imm(sp) */
            imm = has_ext ? sx(ext16(ext, i), 16) : bits(i, 7, 0) << 2;
            d->kind = M16_W32;
            d->w = I_TYPE(0x2B, SP, RA, imm);
            return true;
        case 3: /* ADJSP: addiu sp, imm */
            imm = has_ext ? sx(ext16(ext, i), 16) : sx(bits(i, 7, 0), 8) << 3;
            d->kind = M16_W32;
            d->w = I_TYPE(0x09, SP, SP, imm);
            return true;
        case 4: { /* SAVE / RESTORE */
            static const int8_t args[16]  = { 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 0, 3, 3, 4, -1 };
            static const int8_t stats[16] = { 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 4, 0, 1, 0, -1 };
            unsigned aregs = has_ext ? bits(ext, 3, 0) : 0;
            if (args[aregs] < 0)
                return true;
            d->kind = (i & 0x80) ? M16_SAVE : M16_RESTORE;
            d->args = (unsigned)args[aregs];
            d->astatic = (unsigned)stats[aregs];
            d->xsregs = has_ext ? bits(ext, 10, 8) : 0;
            d->ra = (i >> 6) & 1;
            d->s0 = (i >> 5) & 1;
            d->s1 = (i >> 4) & 1;
            unsigned fs = ((has_ext ? bits(ext, 7, 4) : 0) << 4) | bits(i, 3, 0);
            d->framesize = (fs == 0 && !has_ext) ? 128 : fs * 8;
            return true;
        }
        case 5: { /* MOV32R: move r32, rz */
            unsigned k = bits(i, 7, 3);
            d->kind = M16_W32;
            d->w = R_TYPE(m16_map[bits(i, 2, 0)], 0, (k & 3) * 8 + (k >> 2), 0, 0x21);
            return true;
        }
        case 7: /* MOVR32: move ry, r32 */
            d->kind = M16_W32;
            d->w = R_TYPE(bits(i, 4, 0), 0, ry, 0, 0x21);
            return true;
        default:
            return true;
        }
    case 0x0D: case 0x0E: /* LI rx, imm (ORI rx, 0) / CMPI rx, imm -> T (XORI) */
        imm = has_ext ? ext16(ext, i) : bits(i, 7, 0);
        d->kind = M16_W32;
        d->w = op == 0x0D ? I_TYPE(0x0D, 0, rx, imm) : I_TYPE(0x0E, rx, T_REG, imm);
        return true;
    case 0x10: case 0x11: case 0x13: case 0x14: case 0x15:
    case 0x18: case 0x19: case 0x1B: { /* LB LH LW LBU LHU SB SH SW ry, imm(rx) */
        static const uint8_t mop[0x20] = { [0x10] = 0x20, [0x11] = 0x21, [0x13] = 0x23,
            [0x14] = 0x24, [0x15] = 0x25, [0x18] = 0x28, [0x19] = 0x29, [0x1B] = 0x2B };
        static const uint8_t shift[0x20] = { [0x11] = 1, [0x13] = 2, [0x15] = 1, [0x19] = 1, [0x1B] = 2 };
        imm = has_ext ? sx(ext16(ext, i), 16) : bits(i, 4, 0) << shift[op];
        d->kind = M16_W32;
        d->w = I_TYPE(mop[op], rx, ry, imm);
        return true;
    }
    case 0x12: case 0x1A: /* LWSP / SWSP rx, imm(sp) */
        imm = has_ext ? sx(ext16(ext, i), 16) : bits(i, 7, 0) << 2;
        d->kind = M16_W32;
        d->w = I_TYPE(op == 0x12 ? 0x23 : 0x2B, SP, rx, imm);
        return true;
    case 0x1C: /* RRR: ADDU / SUBU rz, rx, ry */
        if (!(i & 1))
            return true;               /* DADDU / DSUBU */
        d->kind = M16_W32;
        d->w = R_TYPE(rx, ry, rz, 0, (i & 2) ? 0x23 : 0x21);
        return true;
    case 0x1D: { /* RR */
        unsigned f = bits(i, 4, 0), sel = bits(i, 7, 5);
        d->kind = M16_W32;
        switch (f) {
        case 0x00: /* JR / JALR / JRC / JALRC: sel = {compact, link, ra} */
            if (sel == 3 || sel == 7 || ((sel & 1) && bits(i, 10, 8)))
                break;                 /* JR ra / JRC ra name no rx */
            d->kind = M16_JR;
            d->rx = (sel & 1) ? RA : rx;
            d->link = sel & 2;
            d->compact = sel & 4;
            return true;
        case 0x01: d->w = 0x7000003Fu; return true;                  /* SDBBP (MIPS32 SDBBP) */
        case 0x02: d->w = R_TYPE(rx, ry, T_REG, 0, 0x2A); return true; /* SLT -> T */
        case 0x03: d->w = R_TYPE(rx, ry, T_REG, 0, 0x2B); return true; /* SLTU -> T */
        case 0x04: d->w = R_TYPE(rx, ry, ry, 0, 0x04); return true;    /* SLLV ry, rx */
        case 0x05: d->w = (bits(i, 10, 5) << 16) | 0x0D; return true;  /* BREAK code */
        case 0x06: d->w = R_TYPE(rx, ry, ry, 0, 0x06); return true;    /* SRLV ry, rx */
        case 0x07: d->w = R_TYPE(rx, ry, ry, 0, 0x07); return true;    /* SRAV ry, rx */
        case 0x0A: d->w = R_TYPE(rx, ry, T_REG, 0, 0x26); return true; /* CMP -> T (XOR) */
        case 0x0B: d->w = R_TYPE(0, ry, rx, 0, 0x23); return true;     /* NEG rx, ry (SUBU) */
        case 0x0C: d->w = R_TYPE(rx, ry, rx, 0, 0x24); return true;    /* AND */
        case 0x0D: d->w = R_TYPE(rx, ry, rx, 0, 0x25); return true;    /* OR */
        case 0x0E: d->w = R_TYPE(rx, ry, rx, 0, 0x26); return true;    /* XOR */
        case 0x0F: d->w = R_TYPE(ry, 0, rx, 0, 0x27); return true;     /* NOT rx, ry (NOR) */
        case 0x10: if (sel) break; d->w = R_TYPE(0, 0, rx, 0, 0x10); return true; /* MFHI */
        case 0x12: if (sel) break; d->w = R_TYPE(0, 0, rx, 0, 0x12); return true; /* MFLO */
        case 0x11: /* CNVT */
            switch (sel) {
            case 0: d->w = I_TYPE(0x0C, rx, rx, 0xFF); return true;          /* ZEB */
            case 1: d->w = I_TYPE(0x0C, rx, rx, 0xFFFF); return true;        /* ZEH */
            case 4: d->w = (0x1Fu << 26) | R_TYPE(0, rx, rx, 0x10, 0x20); return true; /* SEB */
            case 5: d->w = (0x1Fu << 26) | R_TYPE(0, rx, rx, 0x18, 0x20); return true; /* SEH */
            default: break;
            }
            break;
        case 0x18: d->w = R_TYPE(rx, ry, 0, 0, 0x18); return true;     /* MULT */
        case 0x19: d->w = R_TYPE(rx, ry, 0, 0, 0x19); return true;     /* MULTU */
        case 0x1A: d->w = R_TYPE(rx, ry, 0, 0, 0x1A); return true;     /* DIV */
        case 0x1B: d->w = R_TYPE(rx, ry, 0, 0, 0x1B); return true;     /* DIVU */
        default: break;
        }
        d->kind = M16_RI;
        return true;
    }
    default:
        return true;                   /* reserved */
    }
}

/* ---- lifting ----------------------------------------------------------- */

static bool m16_is_ext_or_jal(uint16_t i) { unsigned op = i >> 11; return op == 0x1E || op == 0x03; }

static void lift_save_restore(ml *L, const m16_insn *d)
{
    uint8_t regs[12];
    unsigned nr = 0;
    if (d->ra)
        regs[nr++] = RA;
    for (unsigned k = d->xsregs; k >= 1; k--)
        regs[nr++] = (uint8_t)(k == 7 ? 30 : 17 + k);   /* s(k+1); s8 is $30 */
    if (d->s1)
        regs[nr++] = 17;
    if (d->s0)
        regs[nr++] = 16;
    for (unsigned k = 0; k < d->astatic; k++)
        regs[nr++] = (uint8_t)(7 - k);                  /* a3, a2, ... */

    unsigned fl = L->endian | UAM_ACC_ALIGNED;
    uam_ref sp = mips_reg_r(L, SP);
    if (d->kind == M16_SAVE) {
        for (unsigned k = 0; k < d->args; k++)
            uam_st(L->b, 32, fl, mips_bin(L, UAM_OP_ADD, sp, mips_imm(L, 4 * k)),
                   mips_reg_r(L, 4 + k), NONE);
        for (unsigned k = 0; k < nr; k++)
            uam_st(L->b, 32, fl, mips_bin(L, UAM_OP_SUB, sp, mips_imm(L, 4 * (k + 1))),
                   mips_reg_r(L, regs[k]), NONE);
        mips_reg_w(L, SP, mips_bin(L, UAM_OP_SUB, sp, mips_imm(L, d->framesize)));
    } else {
        /* every load before any register write (the commit rule) */
        uam_ref top = mips_bin(L, UAM_OP_ADD, sp, mips_imm(L, d->framesize));
        uam_ref v[12];
        for (unsigned k = 0; k < nr; k++)
            v[k] = uam_ld(L->b, 32, fl, mips_bin(L, UAM_OP_SUB, top, mips_imm(L, 4 * (k + 1))), NONE);
        for (unsigned k = 0; k < nr; k++)
            mips_reg_w(L, regs[k], v[k]);
        mips_reg_w(L, SP, top);
    }
}

/* A non-control instruction. pcbase: the address PC-relative forms use. */
static lift_result lift_simple(ml *L, const m16_insn *d, uint32_t pcbase)
{
    switch (d->kind) {
    case M16_W32:
        return mips_lift32(L, d->w);
    case M16_ADDIUPC:
    case M16_LWPC: {
        uint32_t a = (pcbase & ~3u) + d->imm;
        if (d->kind == M16_ADDIUPC)
            mips_reg_w(L, d->rd, mips_imm(L, a));
        else
            mips_reg_w(L, d->rd, uam_ld(L->b, 32, L->endian | UAM_ACC_ALIGNED, mips_imm(L, a), NONE));
        return LIFT_SEQ;
    }
    case M16_SAVE:
    case M16_RESTORE:
        lift_save_restore(L, d);
        return LIFT_SEQ;
    default:
        mips_ri(L);
        return LIFT_DONE;
    }
}

static bool is_control(const m16_insn *d)
{
    return d->kind == M16_B || d->kind == M16_BEQZ || d->kind == M16_BNEZ || d->kind == M16_JR
        || d->kind == M16_JAL;
}

static bool has_slot(const m16_insn *d)
{
    return d->kind == M16_JAL || (d->kind == M16_JR && !d->compact);
}

/* The delay slot at `spc` (even), then the jump to `target`. */
static lift_result jump_with_slot(ml *L, uint32_t spc, uam_ref target)
{
    uint64_t raw = 0;
    uint32_t jpc = L->pc;
    uam_imark_cont(L->b, spc | 1u, 2);
    if (L->lc->fetch(L->lc, spc, 2, &raw) != UAM_OK) {
        mips_trap(L, NONE, MIPS_TRAP_SLOT_FETCH, mips_imm(L, spc | 1u));
        return LIFT_DONE;
    }
    m16_insn s;
    uint16_t si = (uint16_t)raw;
    if (m16_is_ext_or_jal(si) || !mips16_decode(spc | 1u, 0, false, si, 0, &s) || is_control(&s)) {
        mips_trap(L, NONE, EXC_RI, mips_imm(L, si));
        return LIFT_DONE;
    }
    ml slot = *L;
    slot.pc = spc | 1u;
    slot.npc = (spc + 2) | 1u;
    slot.raw = si;
    slot.in_slot = true;
    if (lift_simple(&slot, &s, jpc & ~1u) == LIFT_DONE)
        return LIFT_DONE;
    uam_exit(L->b, NONE, target, UAM_EXIT_JUMP);
    return LIFT_DONE;
}

static lift_result lift_control(ml *L, const m16_insn *d)
{
    uint32_t pcb = L->pc & ~1u;
    switch (d->kind) {
    case M16_B:
        uam_exit(L->b, NONE, mips_imm(L, d->target), UAM_EXIT_JUMP);
        return LIFT_DONE;
    case M16_BEQZ:
    case M16_BNEZ: {
        uam_ref c = mips_bin(L, d->kind == M16_BEQZ ? UAM_OP_EQ : UAM_OP_NE,
                             mips_reg_r(L, d->rx), mips_imm(L, 0));
        uam_exit(L->b, c, mips_imm(L, d->target), UAM_EXIT_JUMP);
        uam_exit(L->b, NONE, mips_imm(L, L->npc), UAM_EXIT_JUMP);
        return LIFT_DONE;
    }
    case M16_JAL:   /* 4 bytes + a 2-byte slot */
        mips_reg_w(L, RA, mips_imm(L, (pcb + 6) | 1u));
        return jump_with_slot(L, pcb + 4, mips_imm(L, d->target));
    case M16_JR: {
        uam_ref t = mips_reg_r(L, d->rx);
        if (d->compact) {
            if (d->link)
                mips_reg_w(L, RA, mips_imm(L, (pcb + 2) | 1u));
            uam_exit(L->b, NONE, t, UAM_EXIT_JUMP);
            return LIFT_DONE;
        }
        if (d->link)
            mips_reg_w(L, RA, mips_imm(L, (pcb + 4) | 1u));
        return jump_with_slot(L, pcb + 2, t);
    }
    default:
        mips_ri(L);
        return LIFT_DONE;
    }
}

uam_status mips16_lift(uam_lift_ctx *lc)
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
        if (lc->fetch(lc, pc, 2, &h0) != UAM_OK) {
            if (n == 0)
                return UAM_ERR_FAULT;
            uam_exit(lc->b, NONE, uam_imm(lc->b, pc | 1u), UAM_EXIT_JUMP);
            return UAM_OK;
        }
        uint16_t i = (uint16_t)h0, ext = 0;
        bool has_ext = false, two = m16_is_ext_or_jal(i);
        if (two && lc->fetch(lc, pc + 2, 2, &h1) != UAM_OK) {
            if (n == 0)
                return UAM_ERR_FAULT;
            uam_exit(lc->b, NONE, uam_imm(lc->b, pc | 1u), UAM_EXIT_JUMP);
            return UAM_OK;
        }
        if ((i >> 11) == 0x1E) {
            has_ext = true;
            ext = i;
            i = (uint16_t)h1;
            if (m16_is_ext_or_jal(i))
                i = 0xF800;            /* EXTEND of EXTEND/JAL: reserved (op 0x1F) */
        }
        m16_insn d;
        mips16_decode(pc | 1u, ext, has_ext, i, (uint16_t)h1, &d);

        /* A jump needs its delay slot in the same block. */
        if (is_control(&d) && has_slot(&d) && n > 0) {
            uint64_t slot;
            if (n + 2 > lc->max_insns || lc->fetch(lc, pc + d.len, 2, &slot) != UAM_OK) {
                uam_exit(lc->b, NONE, uam_imm(lc->b, pc | 1u), UAM_EXIT_JUMP);
                return UAM_OK;
            }
        }

        uam_imark(lc->b, pc | 1u, d.len);
        L.pc = pc | 1u;
        L.npc = (pc + d.len) | 1u;
        L.raw = two ? ((uint32_t)h0 << 16) | (uint16_t)h1 : (uint16_t)h0;
        lift_result r = is_control(&d) ? lift_control(&L, &d) : lift_simple(&L, &d, pc);
        if (r == LIFT_DONE)
            return UAM_OK;
        pc += d.len;
    }
}
