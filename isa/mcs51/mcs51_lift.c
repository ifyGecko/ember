/*
 * mcs51_lift.c - 8051 (MCS-51) decoder and lifter.
 *
 * Conventions (docs/uam-isa.md and isa/mcs51/mcs51.h):
 *  - The pc is the 16-bit code address; code is fetched a byte at a time and relative
 *    targets wrap at 64 KiB.
 *  - Blocks are translated for one register bank and one data pointer (cpu->priv), so
 *    R0-R7 and DPTR are fixed register-file offsets. A write that can change either
 *    (PSW, the data-pointer select) ends the block.
 *  - Direct addresses below 0x80 and bit addresses below 0x80 are internal RAM (GET/PUT);
 *    0x80 and up are SFRs: the core's own become GET/PUT or SYSRD/SYSWR, the rest are
 *    bus accesses in the SFR space (page-relative with SFR paging). Indirect addresses
 *    (@R0, @R1, the stack) go through helpers, since the RAM is in the register file.
 *  - CY, AC and OV are one byte each; P is computed when PSW is read.
 *  - After a write to IE, IP or their extensions the next instruction runs before any
 *    interrupt (it is lifted as a continuation), as the 8051 promises.
 *  - Loads, stores and system accesses come before register writes (the commit rule).
 * The one undefined opcode, 0xA5, stops into the debugger (TRAP DEBUG): the 8051 has no
 * illegal-instruction exception.
 */
#include "mcs51.h"

#define NONE UAM_REF_NONE
#define LE   UAM_ACC_LE

typedef enum { LIFT_CONTINUE, LIFT_END, LIFT_END_NEXT, LIFT_CONT_NEXT } lift_result;
enum { AFTER_NONE, AFTER_CONT, AFTER_END };

typedef struct m51_lifter {
    uam_lift_ctx            *lc;
    uam_builder             *b;
    const uam_mcs51_config  *cfg;
    uint16_t                 pc;      /* address of the instruction */
    unsigned                 len;
    uint8_t                  op, b1, b2;
    unsigned                 bank;    /* PSW.RS the block is translated for */
    unsigned                 dps;     /* the data pointer selected */
    int                      after;   /* what a write asks of the rest of the block */
} m51_lifter;

/* ---- IR shorthands ------------------------------------------------------------- */

static uam_ref imm(m51_lifter *L, uint64_t v) { return uam_imm(L->b, v); }
static uam_ref bin(m51_lifter *L, uam_opcode op, unsigned w, uam_ref x, uam_ref y)
{
    return uam_binop(L->b, op, w, x, y);
}
/* Zero-extend a temp; an immediate is the same value at any width. */
static uam_ref zext(m51_lifter *L, unsigned w, uam_ref x)
{
    return uam_ref_is_imm(x) ? x : uam_unop(L->b, UAM_OP_ZEXT, w, x);
}
static uam_ref trunc_(m51_lifter *L, unsigned w, uam_ref x) { return uam_unop(L->b, UAM_OP_TRUNC, w, x); }
static uam_ref not1(m51_lifter *L, uam_ref c) { return bin(L, UAM_OP_XOR, 1, c, imm(L, 1)); }
static uam_ref bit(m51_lifter *L, unsigned w, uam_ref v, unsigned n)
{
    return trunc_(L, 1, n ? bin(L, UAM_OP_LSHR, w, v, imm(L, n)) : v);
}

static uam_ref get8(m51_lifter *L, uint32_t off) { return uam_get(L->b, 8, off); }
static void put8(m51_lifter *L, uint32_t off, uam_ref v) { uam_put(L->b, 8, off, v, NONE); }

static uam_ref acc(m51_lifter *L) { return get8(L, MCS51_A); }
static void set_acc(m51_lifter *L, uam_ref v) { put8(L, MCS51_A, v); }
static uint32_t rn(const m51_lifter *L, unsigned n) { return MCS51_IRAM(L->bank * 8 + n); }

static uam_ref flag(m51_lifter *L, uint32_t f) { return trunc_(L, 1, get8(L, f)); }
static void setf(m51_lifter *L, uint32_t f, uam_ref c) { put8(L, f, zext(L, 8, c)); }

static uint16_t next_pc(const m51_lifter *L) { return (uint16_t)(L->pc + L->len); }
static uint16_t rel(const m51_lifter *L, uint8_t off) { return (uint16_t)(next_pc(L) + (int8_t)off); }

static lift_result debug_stop(m51_lifter *L)
{
    uam_trap(L->b, NONE, UAM_TRAP_DEBUG, imm(L, 0), imm(L, L->pc));
    return LIFT_END;
}

static void after(m51_lifter *L, int what)
{
    if (what > L->after)
        L->after = what;
}

/* ---- PSW ----------------------------------------------------------------------- */

static uam_ref psw_get(m51_lifter *L)
{
    uam_ref p = bin(L, UAM_OP_AND, 8, uam_unop(L->b, UAM_OP_POPCNT, 8, acc(L)), imm(L, 1));
    uam_ref v = bin(L, UAM_OP_OR, 8, get8(L, MCS51_PSWX), p);
    v = bin(L, UAM_OP_OR, 8, v, bin(L, UAM_OP_SHL, 8, get8(L, MCS51_CY), imm(L, 7)));
    v = bin(L, UAM_OP_OR, 8, v, bin(L, UAM_OP_SHL, 8, get8(L, MCS51_AC), imm(L, 6)));
    return bin(L, UAM_OP_OR, 8, v, bin(L, UAM_OP_SHL, 8, get8(L, MCS51_OV), imm(L, 2)));
}

/* PSW from a byte: F0/RS/F1 through the system register (a bank switch), the flags with
 * PUTs. P is read-only. */
static void psw_put(m51_lifter *L, uam_ref v)
{
    uam_syswr(L->b, 8, UAM_MCS51_SFR_PSW, v, NONE);
    put8(L, MCS51_CY, bin(L, UAM_OP_AND, 8, bin(L, UAM_OP_LSHR, 8, v, imm(L, 7)), imm(L, 1)));
    put8(L, MCS51_AC, bin(L, UAM_OP_AND, 8, bin(L, UAM_OP_LSHR, 8, v, imm(L, 6)), imm(L, 1)));
    put8(L, MCS51_OV, bin(L, UAM_OP_AND, 8, bin(L, UAM_OP_LSHR, 8, v, imm(L, 2)), imm(L, 1)));
    after(L, AFTER_END);
}

/* ---- direct addresses ------------------------------------------------------------- */

/* The register-file offset of a core SFR kept there, or ~0. */
static uint32_t sfr_reg(const m51_lifter *L, uint32_t a)
{
    const uam_mcs51_config *c = L->cfg;
    switch (a) {
    case UAM_MCS51_SFR_ACC: return MCS51_A;
    case UAM_MCS51_SFR_B:   return MCS51_B;
    case UAM_MCS51_SFR_SP:  return MCS51_SP;
    default: break;
    }
    if (c->sfrpage && a == c->sfrpage)
        return MCS51_SFRPAGE;
    if (c->dps && c->dpl1) {
        if (a == UAM_MCS51_SFR_DPL || a == UAM_MCS51_SFR_DPH)
            return MCS51_DPTR(0) + (a - UAM_MCS51_SFR_DPL);
        if (a == c->dpl1 || a == c->dpl1 + 1u)
            return MCS51_DPTR(1) + (a - c->dpl1);
    } else if (a == UAM_MCS51_SFR_DPL || a == UAM_MCS51_SFR_DPH) {
        return MCS51_DPTR(L->dps) + (a - UAM_MCS51_SFR_DPL);
    }
    return ~0u;
}

/* The bus address of a peripheral SFR. */
static uam_ref sfr_addr(m51_lifter *L, uint32_t a)
{
    if (!L->cfg->sfrpage)
        return imm(L, UAM_MCS51_SFR_BASE + a);
    uam_ref page = bin(L, UAM_OP_SHL, 32, zext(L, 32, get8(L, MCS51_SFRPAGE)), imm(L, 8));
    return bin(L, UAM_OP_ADD, 32, page, imm(L, UAM_MCS51_SFR_BASE + a));
}

static uam_ref sfr_rd(m51_lifter *L, uint32_t a)
{
    if (a == UAM_MCS51_SFR_PSW)
        return psw_get(L);
    const uint32_t off = sfr_reg(L, a);
    if (off != ~0u)
        return get8(L, off);
    if (mcs51_sys_sfr(L->cfg, a))
        return uam_sysrd(L->b, 8, a, NONE);
    return uam_ld(L->b, 8, LE, sfr_addr(L, a), NONE);
}

static void sfr_wr(m51_lifter *L, uint32_t a, uam_ref v)
{
    const uam_mcs51_config *c = L->cfg;
    if (a == UAM_MCS51_SFR_PSW) {
        psw_put(L, v);
        return;
    }
    const uint32_t off = sfr_reg(L, a);
    if (off != ~0u) {
        put8(L, off, v);
        return;
    }
    if (mcs51_sys_sfr(c, a)) {
        uam_syswr(L->b, 8, a, v, NONE);
        /* PCON (idle) and DPS (the data pointer) need a new block; the interrupt
         * registers let one more instruction run first. */
        after(L, a == UAM_MCS51_SFR_PCON || (c->dps && a == c->dps) ? AFTER_END : AFTER_CONT);
        return;
    }
    uam_st(L->b, 8, LE, sfr_addr(L, a), v, NONE);
}

static uam_ref rd_dir(m51_lifter *L, uint32_t a)
{
    return a < 0x80 ? get8(L, MCS51_IRAM(a)) : sfr_rd(L, a);
}

static void wr_dir(m51_lifter *L, uint32_t a, uam_ref v)
{
    if (a < 0x80)
        put8(L, MCS51_IRAM(a), v);
    else
        sfr_wr(L, a, v);
}

/* ---- indirect addresses (internal RAM) ---------------------------------------------- */

static uam_ref rd_ind(m51_lifter *L, uam_ref a8)
{
    return uam_helper(L->b, &uam_isa_mcs51.helpers, MCS51_HELPER_IRAM_RD, zext(L, 32, a8), NONE, NONE, NONE);
}

static void wr_ind(m51_lifter *L, uam_ref a8, uam_ref v)
{
    uam_helper(L->b, &uam_isa_mcs51.helpers, MCS51_HELPER_IRAM_WR, zext(L, 32, a8), v, NONE, NONE);
}

/* The stack: SP points at the last byte pushed. */
static void push(m51_lifter *L, uam_ref v)
{
    uam_ref sp = bin(L, UAM_OP_ADD, 8, get8(L, MCS51_SP), imm(L, 1));
    wr_ind(L, sp, v);
    put8(L, MCS51_SP, sp);
}

static uam_ref pop(m51_lifter *L)
{
    uam_ref sp = get8(L, MCS51_SP);
    uam_ref v = rd_ind(L, sp);
    put8(L, MCS51_SP, bin(L, UAM_OP_SUB, 8, sp, imm(L, 1)));
    return v;
}

static void push_pc(m51_lifter *L, uint16_t ret)
{
    push(L, imm(L, ret & 0xFF));
    push(L, imm(L, ret >> 8));
}

static uam_ref pop_pc(m51_lifter *L)
{
    uam_ref hi = zext(L, 32, pop(L));
    uam_ref lo = zext(L, 32, pop(L));
    return bin(L, UAM_OP_OR, 32, bin(L, UAM_OP_SHL, 32, hi, imm(L, 8)), lo);
}

/* ---- operands ------------------------------------------------------------------- */

/* The operand that the low nibble of most opcodes names: 5 a direct address (the byte
 * given), 6/7 @R0/@R1, 8-F R0-R7. */
typedef struct { enum { O_DIR, O_IND, O_REG } k; uint32_t a; uam_ref ptr; } opnd;

static opnd operand(m51_lifter *L, unsigned lo, uint8_t dir)
{
    if (lo == 5)
        return (opnd){ O_DIR, dir, NONE };
    if (lo == 6 || lo == 7)
        return (opnd){ O_IND, 0, get8(L, rn(L, lo & 1)) };
    return (opnd){ O_REG, rn(L, lo & 7), NONE };
}

static uam_ref op_get(m51_lifter *L, const opnd *o)
{
    switch (o->k) {
    case O_DIR: return rd_dir(L, o->a);
    case O_IND: return rd_ind(L, o->ptr);
    default:    return get8(L, o->a);
    }
}

static void op_put(m51_lifter *L, const opnd *o, uam_ref v)
{
    switch (o->k) {
    case O_DIR: wr_dir(L, o->a, v); break;
    case O_IND: wr_ind(L, o->ptr, v); break;
    default:    put8(L, o->a, v); break;
    }
}

/* The source operand of the ALU rows: 4 #data, else operand(). */
static uam_ref alu_src(m51_lifter *L, unsigned lo)
{
    if (lo == 4)
        return imm(L, L->b1);
    opnd o = operand(L, lo, L->b1);
    return op_get(L, &o);
}

/* ---- bits ------------------------------------------------------------------------ */

static uint32_t bit_byte(uint8_t b) { return b < 0x80 ? 0x20u + (b >> 3) : (uint32_t)(b & 0xF8); }

/* Bit b of the byte holding it, read once (v), and that byte with the bit set to c
 * written back: a read-modify-write (on a port, of the latch). */
static uam_ref rd_bit(m51_lifter *L, uint8_t b)
{
    return bit(L, 8, rd_dir(L, bit_byte(b)), b & 7);
}

static void wr_bit_of(m51_lifter *L, uint8_t b, uam_ref v, uam_ref c)
{
    const unsigned m = 1u << (b & 7);
    uam_ref kept = bin(L, UAM_OP_AND, 8, v, imm(L, 0xFF & ~m));
    wr_dir(L, bit_byte(b), bin(L, UAM_OP_OR, 8, kept, uam_select(L->b, 8, c, imm(L, m), imm(L, 0))));
}

static void wr_bit(m51_lifter *L, uint8_t b, uam_ref c)
{
    wr_bit_of(L, b, rd_dir(L, bit_byte(b)), c);
}

/* ---- arithmetic ----------------------------------------------------------------- */

/* a + b + cin or a - b - cin (cin: a boolean or NONE): CY, AC and OV as the 8051 sets
 * them for ADD, ADDC and SUBB (borrows for SUBB). */
static uam_ref arith(m51_lifter *L, bool sub, uam_ref a, uam_ref b, uam_ref cin)
{
    const uam_opcode op = sub ? UAM_OP_SUB : UAM_OP_ADD;
    uam_ref s = bin(L, op, 16, zext(L, 16, a), zext(L, 16, b));
    if (!uam_ref_is_none(cin))
        s = bin(L, op, 16, s, zext(L, 16, cin));
    uam_ref r = trunc_(L, 8, s);
    uam_ref hx = bin(L, UAM_OP_XOR, 8, bin(L, UAM_OP_XOR, 8, a, b), r);
    uam_ref ov = sub ? bin(L, UAM_OP_AND, 8, bin(L, UAM_OP_XOR, 8, a, b), bin(L, UAM_OP_XOR, 8, a, r))
                     : bin(L, UAM_OP_AND, 8, bin(L, UAM_OP_XOR, 8, a, r), bin(L, UAM_OP_XOR, 8, b, r));
    setf(L, MCS51_CY, bit(L, 16, s, 8));
    setf(L, MCS51_AC, bit(L, 8, hx, 4));
    setf(L, MCS51_OV, bit(L, 8, ov, 7));
    return r;
}

/* DA A: decimal adjust after an addition. CY is set by a carry out of either step and
 * never cleared; AC and OV are unchanged. */
static void decimal_adjust(m51_lifter *L)
{
    uam_ref a = acc(L);
    uam_ref lo = bin(L, UAM_OP_AND, 8, a, imm(L, 0xF));
    uam_ref c1 = bin(L, UAM_OP_OR, 1, bin(L, UAM_OP_ULT, 8, imm(L, 9), lo), flag(L, MCS51_AC));
    uam_ref t = bin(L, UAM_OP_ADD, 16, zext(L, 16, a), uam_select(L->b, 16, c1, imm(L, 6), imm(L, 0)));
    uam_ref cy = bin(L, UAM_OP_OR, 1, flag(L, MCS51_CY), bit(L, 16, t, 8));
    uam_ref t8 = trunc_(L, 8, t);
    uam_ref c2 = bin(L, UAM_OP_OR, 1, bin(L, UAM_OP_ULT, 8, imm(L, 9), bin(L, UAM_OP_LSHR, 8, t8, imm(L, 4))), cy);
    uam_ref t2 = bin(L, UAM_OP_ADD, 16, zext(L, 16, t8), uam_select(L->b, 16, c2, imm(L, 0x60), imm(L, 0)));
    setf(L, MCS51_CY, bin(L, UAM_OP_OR, 1, cy, bit(L, 16, t2, 8)));
    set_acc(L, trunc_(L, 8, t2));
}

/* ---- control flow ---------------------------------------------------------------- */

static lift_result jump(m51_lifter *L, uint16_t t)
{
    uam_exit(L->b, NONE, imm(L, t), UAM_EXIT_JUMP);
    return LIFT_END;
}

static lift_result branch(m51_lifter *L, uam_ref cond, uint16_t t)
{
    uam_exit(L->b, cond, imm(L, t), UAM_EXIT_JUMP);
    return jump(L, next_pc(L));
}

static lift_result call(m51_lifter *L, uint16_t t)
{
    push_pc(L, next_pc(L));
    uam_exit(L->b, NONE, imm(L, t), UAM_EXIT_CALL);
    return LIFT_END;
}

/* The DPTR in use, as a 32-bit value. */
static uam_ref dptr(m51_lifter *L) { return zext(L, 32, uam_get(L->b, 16, MCS51_DPTR(L->dps))); }

/* A code byte (MOVC): code space addresses go through isa->translate (code banking). */
static uam_ref code_byte(m51_lifter *L, uam_ref a32)
{
    return uam_ld(L->b, 8, LE, bin(L, UAM_OP_AND, 32, a32, imm(L, 0xFFFF)), NONE);
}

/* MOVX @Ri's address: the model's page register supplies the high byte. */
static uam_ref xdata_ri(m51_lifter *L, unsigned i)
{
    uam_ref a = zext(L, 32, get8(L, rn(L, i)));
    if (L->cfg->xpage)
        a = bin(L, UAM_OP_OR, 32, a, bin(L, UAM_OP_SHL, 32, zext(L, 32, sfr_rd(L, L->cfg->xpage)), imm(L, 8)));
    return bin(L, UAM_OP_ADD, 32, a, imm(L, UAM_MCS51_XDATA_BASE));
}

/* ---- instructions ---------------------------------------------------------------- */

unsigned mcs51_insn_len(uint8_t op)
{
    const unsigned h = op >> 4, lo = op & 0xF;
    if (lo == 1)
        return 2;                                           /* AJMP / ACALL */
    switch (op) {
    case 0x02: case 0x12: case 0x10: case 0x20: case 0x30: case 0x43: case 0x53: case 0x63:
    case 0x75: case 0x85: case 0x90: case 0xD5:
        return 3;
    case 0x05: case 0x15: case 0x40: case 0x50: case 0x60: case 0x70: case 0x80: case 0x42:
    case 0x52: case 0x62: case 0x72: case 0x82: case 0x92: case 0xA0: case 0xA2: case 0xB0:
    case 0xB2: case 0xC0: case 0xC2: case 0xC5: case 0xD0: case 0xD2: case 0xE5: case 0xF5:
        return 2;
    default:
        break;
    }
    if (h == 0xB && lo >= 4)
        return 3;                                           /* CJNE */
    if ((lo == 4 || lo == 5) && h >= 2 && h <= 9 && op != 0x84)
        return 2;                                           /* A, #data / A, direct */
    if ((h == 0x7 || h == 0x8 || h == 0xA) && lo >= 6)
        return 2;                                           /* MOV with #data or direct */
    if (h == 0xD && lo >= 8)
        return 2;                                           /* DJNZ Rn */
    return 1;
}

/* ADD, ADDC, ORL, ANL, XRL, SUBB A, <src> (rows 2-6 and 9, low nibble 4-F). */
static lift_result lift_alu(m51_lifter *L, unsigned h, unsigned lo)
{
    uam_ref a = acc(L), s = alu_src(L, lo), r;
    switch (h) {
    case 0x2: r = arith(L, false, a, s, NONE); break;
    case 0x3: r = arith(L, false, a, s, flag(L, MCS51_CY)); break;
    case 0x4: r = bin(L, UAM_OP_OR, 8, a, s); break;
    case 0x5: r = bin(L, UAM_OP_AND, 8, a, s); break;
    case 0x6: r = bin(L, UAM_OP_XOR, 8, a, s); break;
    default:  r = arith(L, true, a, s, flag(L, MCS51_CY)); break;
    }
    set_acc(L, r);
    return LIFT_CONTINUE;
}

/* ORL/ANL/XRL direct, A (x2) and direct, #data (x3). */
static lift_result lift_logic_dir(m51_lifter *L, unsigned h, bool data)
{
    static const uam_opcode ops[] = { UAM_OP_OR, UAM_OP_AND, UAM_OP_XOR };
    uam_ref v = rd_dir(L, L->b1);
    uam_ref s = data ? imm(L, L->b2) : acc(L);
    wr_dir(L, L->b1, bin(L, ops[h - 4], 8, v, s));
    return LIFT_CONTINUE;
}

static lift_result lift_insn(m51_lifter *L)
{
    const uint8_t op = L->op;
    const unsigned h = op >> 4, lo = op & 0xF;

    if (lo == 1) {                                          /* AJMP / ACALL addr11 */
        const uint16_t t = (uint16_t)((next_pc(L) & 0xF800) | ((op >> 5) << 8) | L->b1);
        return (h & 1) ? call(L, t) : jump(L, t);
    }
    if (lo >= 4 && ((h >= 0x2 && h <= 0x6) || h == 0x9))
        return lift_alu(L, h, lo);

    switch (op) {
    case 0x00: return LIFT_CONTINUE;                                        /* NOP */
    case 0x02: return jump(L, (uint16_t)(L->b1 << 8 | L->b2));              /* LJMP */
    case 0x12: return call(L, (uint16_t)(L->b1 << 8 | L->b2));              /* LCALL */
    case 0x22: case 0x32: {                                                 /* RET / RETI */
        uam_ref t = pop_pc(L);
        if (op == 0x32)
            uam_helper(L->b, &uam_isa_mcs51.helpers, MCS51_HELPER_RETI, NONE, NONE, NONE, NONE);
        uam_exit(L->b, NONE, t, op == 0x32 ? UAM_EXIT_EXC_RETURN : UAM_EXIT_RET);
        return LIFT_END;
    }
    case 0x03: case 0x13: case 0x23: case 0x33: {                          /* RR RRC RL RLC */
        uam_ref a = acc(L);
        const bool left = op >= 0x23, thru = op == 0x13 || op == 0x33;
        uam_ref in = thru ? flag(L, MCS51_CY) : bit(L, 8, a, left ? 7 : 0);   /* the bit shifted in */
        uam_ref r = left ? bin(L, UAM_OP_OR, 8, bin(L, UAM_OP_SHL, 8, a, imm(L, 1)), zext(L, 8, in))
                         : bin(L, UAM_OP_OR, 8, bin(L, UAM_OP_LSHR, 8, a, imm(L, 1)),
                               bin(L, UAM_OP_SHL, 8, zext(L, 8, in), imm(L, 7)));
        if (thru)
            setf(L, MCS51_CY, bit(L, 8, a, left ? 7 : 0));
        set_acc(L, r);
        return LIFT_CONTINUE;
    }
    case 0x04: set_acc(L, bin(L, UAM_OP_ADD, 8, acc(L), imm(L, 1))); return LIFT_CONTINUE;   /* INC A */
    case 0x14: set_acc(L, bin(L, UAM_OP_SUB, 8, acc(L), imm(L, 1))); return LIFT_CONTINUE;   /* DEC A */
    case 0x10: case 0x20: case 0x30: {                                     /* JBC JB JNB bit, rel */
        uam_ref v = rd_dir(L, bit_byte(L->b1));
        uam_ref c = bit(L, 8, v, L->b1 & 7);
        if (op == 0x10)     /* clear it (writing the byte back unchanged when it is clear) */
            wr_bit_of(L, L->b1, v, imm(L, 0));
        return branch(L, op == 0x30 ? not1(L, c) : c, rel(L, L->b2));
    }
    case 0x40: return branch(L, flag(L, MCS51_CY), rel(L, L->b1));                    /* JC */
    case 0x50: return branch(L, not1(L, flag(L, MCS51_CY)), rel(L, L->b1));           /* JNC */
    case 0x60: return branch(L, bin(L, UAM_OP_EQ, 8, acc(L), imm(L, 0)), rel(L, L->b1));   /* JZ */
    case 0x70: return branch(L, bin(L, UAM_OP_NE, 8, acc(L), imm(L, 0)), rel(L, L->b1));   /* JNZ */
    case 0x80: return jump(L, rel(L, L->b1));                                         /* SJMP */
    case 0x73: {                                                                       /* JMP @A+DPTR */
        uam_ref t = bin(L, UAM_OP_AND, 32, bin(L, UAM_OP_ADD, 32, zext(L, 32, acc(L)), dptr(L)), imm(L, 0xFFFF));
        uam_exit(L->b, NONE, t, UAM_EXIT_JUMP);
        return LIFT_END;
    }
    case 0x42: case 0x52: case 0x62: return lift_logic_dir(L, h, false);
    case 0x43: case 0x53: case 0x63: return lift_logic_dir(L, h, true);
    case 0x72: case 0x82: case 0xA0: case 0xB0: {                          /* ORL/ANL C, [/]bit */
        uam_ref c = rd_bit(L, L->b1);
        if (op == 0xA0 || op == 0xB0)
            c = not1(L, c);
        const bool orl = op == 0x72 || op == 0xA0;
        setf(L, MCS51_CY, bin(L, orl ? UAM_OP_OR : UAM_OP_AND, 1, flag(L, MCS51_CY), c));
        return LIFT_CONTINUE;
    }
    case 0x92: wr_bit(L, L->b1, flag(L, MCS51_CY)); return LIFT_CONTINUE;             /* MOV bit, C */
    case 0xA2: setf(L, MCS51_CY, rd_bit(L, L->b1)); return LIFT_CONTINUE;             /* MOV C, bit */
    case 0xB2: {                                                                       /* CPL bit */
        uam_ref v = rd_dir(L, bit_byte(L->b1));
        wr_bit_of(L, L->b1, v, not1(L, bit(L, 8, v, L->b1 & 7)));
        return LIFT_CONTINUE;
    }
    case 0xC2: wr_bit(L, L->b1, imm(L, 0)); return LIFT_CONTINUE;                     /* CLR bit */
    case 0xD2: wr_bit(L, L->b1, imm(L, 1)); return LIFT_CONTINUE;                     /* SETB bit */
    case 0xB3: setf(L, MCS51_CY, not1(L, flag(L, MCS51_CY))); return LIFT_CONTINUE;   /* CPL C */
    case 0xC3: put8(L, MCS51_CY, imm(L, 0)); return LIFT_CONTINUE;                    /* CLR C */
    case 0xD3: put8(L, MCS51_CY, imm(L, 1)); return LIFT_CONTINUE;                    /* SETB C */
    case 0x74: set_acc(L, imm(L, L->b1)); return LIFT_CONTINUE;                       /* MOV A, #data */
    case 0x75: wr_dir(L, L->b1, imm(L, L->b2)); return LIFT_CONTINUE;                 /* MOV dir, #data */
    case 0x85: wr_dir(L, L->b2, rd_dir(L, L->b1)); return LIFT_CONTINUE;              /* MOV dir, dir */
    case 0x83: case 0x93: {                                                            /* MOVC */
        uam_ref base = op == 0x83 ? imm(L, next_pc(L)) : dptr(L);
        set_acc(L, code_byte(L, bin(L, UAM_OP_ADD, 32, zext(L, 32, acc(L)), base)));
        return LIFT_CONTINUE;
    }
    case 0x84: {                                                                       /* DIV AB */
        uam_ref a = acc(L), b = get8(L, MCS51_B);
        uam_ref z = bin(L, UAM_OP_EQ, 8, b, imm(L, 0));
        uam_ref d = uam_select(L->b, 8, z, imm(L, 1), b);
        set_acc(L, uam_select(L->b, 8, z, a, bin(L, UAM_OP_UDIV, 8, a, d)));
        put8(L, MCS51_B, uam_select(L->b, 8, z, b, bin(L, UAM_OP_UREM, 8, a, d)));
        put8(L, MCS51_CY, imm(L, 0));
        setf(L, MCS51_OV, z);
        return LIFT_CONTINUE;
    }
    case 0xA4: {                                                                       /* MUL AB */
        uam_ref p = bin(L, UAM_OP_MUL, 16, zext(L, 16, acc(L)), zext(L, 16, get8(L, MCS51_B)));
        uam_ref hi = trunc_(L, 8, bin(L, UAM_OP_LSHR, 16, p, imm(L, 8)));
        set_acc(L, trunc_(L, 8, p));
        put8(L, MCS51_B, hi);
        put8(L, MCS51_CY, imm(L, 0));
        setf(L, MCS51_OV, bin(L, UAM_OP_NE, 8, hi, imm(L, 0)));
        return LIFT_CONTINUE;
    }
    case 0x90:                                                                         /* MOV DPTR, #data16 */
        uam_put(L->b, 16, MCS51_DPTR(L->dps), imm(L, (uint32_t)L->b1 << 8 | L->b2), NONE);
        return LIFT_CONTINUE;
    case 0xA3:                                                                         /* INC DPTR */
        uam_put(L->b, 16, MCS51_DPTR(L->dps),
                bin(L, UAM_OP_ADD, 16, uam_get(L->b, 16, MCS51_DPTR(L->dps)), imm(L, 1)), NONE);
        return LIFT_CONTINUE;
    case 0xA5:
        return debug_stop(L);
    case 0xB4: case 0xB5: {                                                            /* CJNE A, #/dir, rel */
        uam_ref a = acc(L), s = op == 0xB4 ? imm(L, L->b1) : rd_dir(L, L->b1);
        setf(L, MCS51_CY, bin(L, UAM_OP_ULT, 8, a, s));
        return branch(L, bin(L, UAM_OP_NE, 8, a, s), rel(L, L->b2));
    }
    case 0xC0: push(L, rd_dir(L, L->b1)); return LIFT_CONTINUE;                       /* PUSH */
    case 0xD0: {                                                                       /* POP */
        uam_ref sp = get8(L, MCS51_SP);
        uam_ref v = rd_ind(L, sp);
        if (L->b1 != UAM_MCS51_SFR_SP) {
            wr_dir(L, L->b1, v);
            put8(L, MCS51_SP, bin(L, UAM_OP_SUB, 8, sp, imm(L, 1)));
        } else {
            put8(L, MCS51_SP, v);
        }
        return LIFT_CONTINUE;
    }
    case 0xC4: {                                                                       /* SWAP A */
        uam_ref a = acc(L);
        set_acc(L, bin(L, UAM_OP_OR, 8, bin(L, UAM_OP_SHL, 8, a, imm(L, 4)), bin(L, UAM_OP_LSHR, 8, a, imm(L, 4))));
        return LIFT_CONTINUE;
    }
    case 0xD4: decimal_adjust(L); return LIFT_CONTINUE;                              /* DA A */
    case 0xD5: {                                                                       /* DJNZ dir, rel */
        uam_ref v = bin(L, UAM_OP_SUB, 8, rd_dir(L, L->b1), imm(L, 1));
        wr_dir(L, L->b1, v);
        return branch(L, bin(L, UAM_OP_NE, 8, v, imm(L, 0)), rel(L, L->b2));
    }
    case 0xD6: case 0xD7: {                                                            /* XCHD A, @Ri */
        uam_ref p = get8(L, rn(L, lo & 1));
        uam_ref m = rd_ind(L, p), a = acc(L);
        wr_ind(L, p, bin(L, UAM_OP_OR, 8, bin(L, UAM_OP_AND, 8, m, imm(L, 0xF0)), bin(L, UAM_OP_AND, 8, a, imm(L, 0xF))));
        set_acc(L, bin(L, UAM_OP_OR, 8, bin(L, UAM_OP_AND, 8, a, imm(L, 0xF0)), bin(L, UAM_OP_AND, 8, m, imm(L, 0xF))));
        return LIFT_CONTINUE;
    }
    case 0xE0:                                                                         /* MOVX A, @DPTR */
        set_acc(L, uam_ld(L->b, 8, LE, bin(L, UAM_OP_ADD, 32, dptr(L), imm(L, UAM_MCS51_XDATA_BASE)), NONE));
        return LIFT_CONTINUE;
    case 0xF0:                                                                         /* MOVX @DPTR, A */
        uam_st(L->b, 8, LE, bin(L, UAM_OP_ADD, 32, dptr(L), imm(L, UAM_MCS51_XDATA_BASE)), acc(L), NONE);
        return LIFT_CONTINUE;
    case 0xE2: case 0xE3:                                                              /* MOVX A, @Ri */
        set_acc(L, uam_ld(L->b, 8, LE, xdata_ri(L, lo & 1), NONE));
        return LIFT_CONTINUE;
    case 0xF2: case 0xF3:                                                              /* MOVX @Ri, A */
        uam_st(L->b, 8, LE, xdata_ri(L, lo & 1), acc(L), NONE);
        return LIFT_CONTINUE;
    case 0xE4: set_acc(L, imm(L, 0)); return LIFT_CONTINUE;                           /* CLR A */
    case 0xF4: set_acc(L, bin(L, UAM_OP_XOR, 8, acc(L), imm(L, 0xFF))); return LIFT_CONTINUE;   /* CPL A */
    default:
        break;
    }

    /* The operand rows: low nibble 5 direct (byte 1), 6/7 @Ri, 8-F Rn. */
    opnd o = operand(L, lo, L->b1);
    switch (h) {
    case 0x0: case 0x1:                                                                /* INC / DEC */
        op_put(L, &o, bin(L, h ? UAM_OP_SUB : UAM_OP_ADD, 8, op_get(L, &o), imm(L, 1)));
        return LIFT_CONTINUE;
    case 0x7:                                                                          /* MOV op, #data */
        op_put(L, &o, imm(L, L->b1));
        return LIFT_CONTINUE;
    case 0x8:                                                                          /* MOV dir, op */
        wr_dir(L, L->b1, op_get(L, &o));
        return LIFT_CONTINUE;
    case 0xA:                                                                          /* MOV op, dir */
        op_put(L, &o, rd_dir(L, L->b1));
        return LIFT_CONTINUE;
    case 0xB: {                                                                        /* CJNE op, #data, rel */
        uam_ref v = op_get(L, &o), s = imm(L, L->b1);
        setf(L, MCS51_CY, bin(L, UAM_OP_ULT, 8, v, s));
        return branch(L, bin(L, UAM_OP_NE, 8, v, s), rel(L, L->b2));
    }
    case 0xC: {                                                                        /* XCH A, op */
        uam_ref v = op_get(L, &o), a = acc(L);
        op_put(L, &o, a);
        set_acc(L, v);
        return LIFT_CONTINUE;
    }
    case 0xD: {                                                                        /* DJNZ Rn, rel */
        uam_ref v = bin(L, UAM_OP_SUB, 8, op_get(L, &o), imm(L, 1));
        op_put(L, &o, v);
        return branch(L, bin(L, UAM_OP_NE, 8, v, imm(L, 0)), rel(L, L->b1));
    }
    case 0xE: set_acc(L, op_get(L, &o)); return LIFT_CONTINUE;                       /* MOV A, op */
    default:  op_put(L, &o, acc(L)); return LIFT_CONTINUE;                           /* MOV op, A */
    }
}

uam_status mcs51_lift(uam_lift_ctx *lc)
{
    const mcs51_state *s = mcs51_st(lc->cpu);
    const unsigned priv = lc->cpu->priv;
    uint16_t pc = (uint16_t)lc->pc;
    bool cont = false;

    for (uint32_t n = 0;; n++) {
        if (n >= lc->max_insns && !cont) {
            uam_exit(lc->b, NONE, uam_imm(lc->b, pc), UAM_EXIT_JUMP);
            return UAM_OK;
        }
        uint64_t byte[3] = { 0, 0, 0 };
        uam_status st = lc->fetch(lc, pc, 1, &byte[0]);
        const unsigned len = st == UAM_OK ? mcs51_insn_len((uint8_t)byte[0]) : 1;
        for (unsigned k = 1; st == UAM_OK && k < len; k++)
            st = lc->fetch(lc, (uint16_t)(pc + k), 1, &byte[k]);
        if (st != UAM_OK) {
            if (n == 0)
                return UAM_ERR_FAULT;
            uam_exit(lc->b, NONE, uam_imm(lc->b, pc), UAM_EXIT_JUMP);
            return UAM_OK;
        }
        m51_lifter L = { .lc = lc, .b = lc->b, .cfg = &s->cfg, .pc = pc, .len = len,
                         .op = (uint8_t)byte[0], .b1 = (uint8_t)byte[1], .b2 = (uint8_t)byte[2],
                         .bank = mcs51_priv_bank(priv), .dps = mcs51_priv_dps(priv) };
        if (cont)
            uam_imark_cont(lc->b, pc, len);
        else
            uam_imark(lc->b, pc, len);
        lift_result r = lift_insn(&L);
        pc = next_pc(&L);
        if (r == LIFT_END)
            return UAM_OK;
        if (r == LIFT_CONTINUE && L.after == AFTER_END)
            r = LIFT_END_NEXT;
        else if (r == LIFT_CONTINUE && L.after == AFTER_CONT)
            r = LIFT_CONT_NEXT;
        if (r == LIFT_END_NEXT || cont) {   /* a new block, or the instruction after an
                                             * interrupt-register write ran */
            uam_exit(lc->b, NONE, uam_imm(lc->b, pc), UAM_EXIT_JUMP);
            return UAM_OK;
        }
        cont = r == LIFT_CONT_NEXT;
    }
}
