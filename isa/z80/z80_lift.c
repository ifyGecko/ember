/*
 * z80_lift.c - Z80 decoder and lifter.
 *
 * Conventions (docs/uam-isa.md section 10i and isa/z80/z80.h):
 *  - The pc is the 16-bit memory address; memory addresses wrap at 64 KiB, so a 16-bit
 *    access is two byte accesses. I/O accesses go to UAM_Z80_IO_BASE plus the port
 *    address, masked with the board's decoding.
 *  - An instruction is its prefixes and its opcode: DD/FD select IX/IY for HL (H and L
 *    become IXH/IXL except beside an (IX+d) operand), a DD or FD followed by another
 *    prefix is an instruction of its own (a NOP), lifted with the next as a continuation,
 *    since the Z80 takes no interrupt between them.
 *  - Every instruction queues its register writes and commits them after its loads,
 *    stores and system register accesses (the commit rule). Each one adds its opcode
 *    fetches to R and records in QV whether it set the flags (Q, for SCF and CCF).
 *  - Flags are computed whole into F, bits 3 and 5 included.
 *  - EI is a system register write that requests an exit; the instruction after it is a
 *    continuation, so it runs before any interrupt. If the block cannot hold it, the
 *    core's shadow flag (set by EI, cleared by that instruction) makes irq_take wait.
 *  - Repeating block instructions do one iteration and exit to themselves while they
 *    repeat.
 */
#include "z80.h"

#define NONE UAM_REF_NONE
#define LE   UAM_ACC_LE

typedef enum { LIFT_CONTINUE, LIFT_END } lift_result;

typedef struct z80_qput { uint32_t off; unsigned w; uam_ref v, g; } z80_qput;

typedef struct z80_lifter {
    uam_lift_ctx     *lc;
    uam_builder      *b;
    const z80_state  *s;
    uint16_t          pc;     /* address of the instruction (its first prefix) */
    unsigned          len;    /* bytes */
    unsigned          m1;     /* opcode fetches: R's increment */
    bool              fault;  /* a byte could not be fetched */
    /* decoded */
    unsigned          idx;    /* 0 HL, 1 IX, 2 IY */
    unsigned          page;   /* 0 main, 0xCB, 0xED */
    uint8_t           op;
    int8_t            d;      /* (IX+d) displacement */
    uint8_t           n;      /* 8-bit operand */
    uint16_t          nn;     /* 16-bit operand */
    bool              prefix_nop;   /* a DD/FD another prefix follows */
    /* lifting */
    z80_qput          q[24];
    unsigned          nq;
    bool              fset;   /* the instruction wrote F */
    int               qv_prev;  /* QV as the last instruction left it, -1 unknown */
    bool              cont_next;    /* the next instruction is a continuation (EI) */
} z80_lifter;

/* ---- IR shorthands ------------------------------------------------------------- */

static uam_ref imm(z80_lifter *L, uint64_t v) { return uam_imm(L->b, v); }
static uam_ref bin(z80_lifter *L, uam_opcode op, unsigned w, uam_ref x, uam_ref y)
{
    return uam_binop(L->b, op, w, x, y);
}
static uam_ref zext(z80_lifter *L, unsigned w, uam_ref x)
{
    if (uam_ref_is_imm(x) || uam_builder_ref_width(L->b, x) == w)
        return x;
    return uam_unop(L->b, UAM_OP_ZEXT, w, x);
}
static uam_ref trunc_(z80_lifter *L, unsigned w, uam_ref x)
{
    if (uam_ref_is_imm(x))
        return imm(L, L->b->imm[uam_ref_index(x)] & ((UINT64_C(1) << w) - 1));
    if (uam_builder_ref_width(L->b, x) == w)
        return x;
    return uam_unop(L->b, UAM_OP_TRUNC, w, x);
}
static uam_ref not1(z80_lifter *L, uam_ref c) { return bin(L, UAM_OP_XOR, 1, c, imm(L, 1)); }
static uam_ref bit(z80_lifter *L, unsigned w, uam_ref v, unsigned n)
{
    return trunc_(L, 1, n ? bin(L, UAM_OP_LSHR, w, v, imm(L, n)) : v);
}
static uam_ref and8(z80_lifter *L, uam_ref x, uint8_t m) { return bin(L, UAM_OP_AND, 8, x, imm(L, m)); }
static uam_ref or8(z80_lifter *L, uam_ref x, uam_ref y) { return bin(L, UAM_OP_OR, 8, x, y); }
static uam_ref sel8(z80_lifter *L, uam_ref c, uint8_t x, uint8_t y)
{
    return uam_select(L->b, 8, c, imm(L, x), imm(L, y));
}
/* A boolean as flag bit `pos`. */
static uam_ref fb(z80_lifter *L, uam_ref c, unsigned pos) { return sel8(L, c, (uint8_t)(1u << pos), 0); }
static uam_ref add16(z80_lifter *L, uam_ref x, int k)
{
    return k >= 0 ? bin(L, UAM_OP_ADD, 16, x, imm(L, (unsigned)k)) : bin(L, UAM_OP_SUB, 16, x, imm(L, (unsigned)-k));
}

static uam_ref get8(z80_lifter *L, uint32_t off) { return uam_get(L->b, 8, off); }
static uam_ref get16(z80_lifter *L, uint32_t off) { return uam_get(L->b, 16, off); }

static uam_ref fit(z80_lifter *L, unsigned w, uam_ref v)
{
    if (uam_ref_is_imm(v))
        return v;
    return uam_builder_ref_width(L->b, v) > w ? trunc_(L, w, v) : zext(L, w, v);
}
static void qput_g(z80_lifter *L, unsigned w, uint32_t off, uam_ref v, uam_ref g)
{
    if (L->nq < UAM_ARRAY_LEN(L->q))
        L->q[L->nq++] = (z80_qput){ off, w, fit(L, w, v), g };
}
static void qput(z80_lifter *L, unsigned w, uint32_t off, uam_ref v) { qput_g(L, w, off, v, NONE); }
/* End the instruction: its register writes, and QV if it changes. */
static void commit(z80_lifter *L)
{
    if (L->qv_prev != (int)L->fset)
        qput(L, 8, Z80_QV, imm(L, L->fset));
    for (unsigned i = 0; i < L->nq; i++)
        uam_put(L->b, L->q[i].w, L->q[i].off, L->q[i].v, L->q[i].g);
    L->nq = 0;
}

static uint16_t next_pc(const z80_lifter *L) { return (uint16_t)(L->pc + L->len); }

/* ---- flags ------------------------------------------------------------------------ */

static uam_ref f_get(z80_lifter *L) { return get8(L, Z80_F); }
static void set_f(z80_lifter *L, uam_ref f) { qput(L, 8, Z80_F, f); L->fset = true; }
static uam_ref zflag(z80_lifter *L, uam_ref v) { return sel8(L, bin(L, UAM_OP_EQ, 8, v, imm(L, 0)), ZF_Z, 0); }
/* S, Z, Y and X from a result. */
static uam_ref sz53(z80_lifter *L, uam_ref v) { return or8(L, and8(L, v, 0xA8), zflag(L, v)); }
/* P/V as even parity of v. */
static uam_ref par(z80_lifter *L, uam_ref v)
{
    uam_ref odd = bin(L, UAM_OP_AND, 8, uam_unop(L->b, UAM_OP_POPCNT, 8, v), imm(L, 1));
    return bin(L, UAM_OP_SHL, 8, bin(L, UAM_OP_XOR, 8, odd, imm(L, 1)), imm(L, 2));
}
static uam_ref keep(z80_lifter *L, uint8_t m) { return and8(L, f_get(L), m); }

/* ---- memory and ports -------------------------------------------------------------- */

static uam_ref a32(z80_lifter *L, uam_ref a16) { return zext(L, 32, a16); }
static uam_ref rd8_g(z80_lifter *L, uam_ref a16, uam_ref g) { return uam_ld(L->b, 8, LE, a32(L, a16), g); }
static uam_ref rd8(z80_lifter *L, uam_ref a16) { return rd8_g(L, a16, NONE); }
static void wr8_g(z80_lifter *L, uam_ref a16, uam_ref v, uam_ref g) { uam_st(L->b, 8, LE, a32(L, a16), trunc_(L, 8, v), g); }
static void wr8(z80_lifter *L, uam_ref a16, uam_ref v) { wr8_g(L, a16, v, NONE); }
static uam_ref word(z80_lifter *L, uam_ref lo, uam_ref hi)
{
    return bin(L, UAM_OP_OR, 16, zext(L, 16, lo), bin(L, UAM_OP_SHL, 16, zext(L, 16, hi), imm(L, 8)));
}
static uam_ref lo8(z80_lifter *L, uam_ref v16) { return trunc_(L, 8, v16); }
static uam_ref hi8(z80_lifter *L, uam_ref v16) { return trunc_(L, 8, bin(L, UAM_OP_LSHR, 16, v16, imm(L, 8))); }
static uam_ref rd16_g(z80_lifter *L, uam_ref a, uam_ref g)
{
    uam_ref lo = rd8_g(L, a, g);
    return word(L, lo, rd8_g(L, add16(L, a, 1), g));
}
static uam_ref rd16(z80_lifter *L, uam_ref a) { return rd16_g(L, a, NONE); }
static void wr16(z80_lifter *L, uam_ref a, uam_ref v)
{
    wr8(L, a, lo8(L, v));
    wr8(L, add16(L, a, 1), hi8(L, v));
}

static uam_ref io_addr(z80_lifter *L, uam_ref port)
{
    if (uam_ref_is_imm(port))
        return imm(L, UAM_Z80_IO_BASE + (L->b->imm[uam_ref_index(port)] & L->s->io_mask));
    uam_ref p = L->s->io_mask == 0xFFFF ? port : bin(L, UAM_OP_AND, 16, port, imm(L, L->s->io_mask));
    return bin(L, UAM_OP_ADD, 32, zext(L, 32, p), imm(L, UAM_Z80_IO_BASE));
}
static uam_ref in8(z80_lifter *L, uam_ref port) { return uam_ld(L->b, 8, LE, io_addr(L, port), NONE); }
static void out8(z80_lifter *L, uam_ref port, uam_ref v) { uam_st(L->b, 8, LE, io_addr(L, port), v, NONE); }

/* The stack: PUSH writes the high byte first, at SP - 1. */
static void push16_g(z80_lifter *L, uam_ref v, uam_ref g)
{
    uam_ref sp = get16(L, Z80_SP);
    uam_ref a1 = add16(L, sp, -1), a2 = add16(L, sp, -2);
    wr8_g(L, a1, hi8(L, v), g);
    wr8_g(L, a2, lo8(L, v), g);
    qput_g(L, 16, Z80_SP, a2, g);
}
static void push16(z80_lifter *L, uam_ref v) { push16_g(L, v, NONE); }
static uam_ref pop16_g(z80_lifter *L, uam_ref g)
{
    uam_ref sp = get16(L, Z80_SP);
    uam_ref v = rd16_g(L, sp, g);
    qput_g(L, 16, Z80_SP, add16(L, sp, 2), g);
    return v;
}

/* ---- operands --------------------------------------------------------------------- */

/* Register r (0-7 but 6: B C D E H L - A); with `x`, H and L are the index register's. */
static uint32_t r8(const z80_lifter *L, unsigned r, bool x)
{
    static const uint8_t off[8] = { Z80_B, Z80_C, Z80_D, Z80_E, Z80_H, Z80_L, 0, Z80_A };
    if (x && L->idx && (r == 4 || r == 5))
        return (L->idx == 1 ? Z80_IXL : Z80_IYL) + (r == 4);
    return off[r];
}
static uint32_t hl_off(const z80_lifter *L) { return L->idx == 0 ? Z80_HL : L->idx == 1 ? Z80_IX : Z80_IY; }
/* Pair p: BC DE HL(IX/IY) SP, or with af, AF for 3. */
static uint32_t rp(const z80_lifter *L, unsigned p, bool af)
{
    static const uint8_t off[4] = { Z80_BC, Z80_DE, 0, Z80_SP };
    if (p == 2)
        return hl_off(L);
    return p == 3 && af ? Z80_AF : off[p];
}
/* The address of the (HL) operand: HL, or IX+d (which MEMPTR keeps). */
static uam_ref mem_hl(z80_lifter *L)
{
    if (!L->idx)
        return get16(L, Z80_HL);
    uam_ref a = add16(L, get16(L, hl_off(L)), L->d);
    qput(L, 16, Z80_WZ, a);
    return a;
}
/* Read and write operand r (6: memory at `a`, which mem_hl gave). */
static uam_ref opget(z80_lifter *L, unsigned r, bool x, uam_ref a)
{
    return r == 6 ? rd8(L, a) : get8(L, r8(L, r, x));
}
static void opset(z80_lifter *L, unsigned r, bool x, uam_ref a, uam_ref v)
{
    if (r == 6)
        wr8(L, a, v);
    else
        qput(L, 8, r8(L, r, x), v);
}

/* Condition y: NZ Z NC C PO PE P M. */
static uam_ref cond(z80_lifter *L, unsigned y)
{
    static const uint8_t pos[8] = { 6, 6, 0, 0, 2, 2, 7, 7 };
    uam_ref c = bit(L, 8, f_get(L), pos[y]);
    return (y & 1) ? c : not1(L, c);
}

/* ---- arithmetic -------------------------------------------------------------------- */

/* ALU operation y (ADD ADC SUB SBC AND XOR OR CP) of A and v: sets F, returns the
 * result (NONE for CP). */
static uam_ref alu(z80_lifter *L, unsigned y, uam_ref a, uam_ref v)
{
    uam_ref r, f;
    if (y <= 3 || y == 7) {
        const bool sub = y >= 2;
        const uam_opcode op = sub ? UAM_OP_SUB : UAM_OP_ADD;
        uam_ref s = bin(L, op, 16, zext(L, 16, a), zext(L, 16, v));
        if (y == 1 || y == 3)
            s = bin(L, op, 16, s, zext(L, 16, and8(L, f_get(L), ZF_C)));
        r = trunc_(L, 8, s);
        uam_ref h = bit(L, 8, bin(L, UAM_OP_XOR, 8, bin(L, UAM_OP_XOR, 8, a, v), r), 4);
        uam_ref ov = sub ? bin(L, UAM_OP_AND, 8, bin(L, UAM_OP_XOR, 8, a, v), bin(L, UAM_OP_XOR, 8, a, r))
                         : bin(L, UAM_OP_AND, 8, bin(L, UAM_OP_XOR, 8, a, r), bin(L, UAM_OP_XOR, 8, v, r));
        f = or8(L, and8(L, r, ZF_S), zflag(L, r));
        f = or8(L, f, and8(L, y == 7 ? v : r, ZF_Y | ZF_X));
        f = or8(L, f, fb(L, h, 4));
        f = or8(L, f, fb(L, bit(L, 8, ov, 7), 2));
        f = or8(L, f, zext(L, 8, bit(L, 16, s, 8)));
        if (sub)
            f = or8(L, f, imm(L, ZF_N));
    } else {
        static const uam_opcode ops[3] = { UAM_OP_AND, UAM_OP_XOR, UAM_OP_OR };
        r = bin(L, ops[y - 4], 8, a, v);
        f = or8(L, sz53(L, r), par(L, r));
        if (y == 4)
            f = or8(L, f, imm(L, ZF_H));
    }
    set_f(L, f);
    return y == 7 ? NONE : r;
}

static uam_ref inc_dec(z80_lifter *L, uam_ref v, bool dec)
{
    uam_ref r = bin(L, dec ? UAM_OP_SUB : UAM_OP_ADD, 8, v, imm(L, 1));
    uam_ref f = or8(L, keep(L, ZF_C), sz53(L, r));
    f = or8(L, f, fb(L, bin(L, UAM_OP_EQ, 8, and8(L, dec ? v : r, 0xF), imm(L, 0)), 4));
    f = or8(L, f, fb(L, bin(L, UAM_OP_EQ, 8, r, imm(L, dec ? 0x7F : 0x80)), 2));
    if (dec)
        f = or8(L, f, imm(L, ZF_N));
    set_f(L, f);
    return r;
}

/* The CB rotates and shifts y (RLC RRC RL RR SLA SRA SLL SRL); `acc`: the RLCA-style
 * forms, which keep S, Z and P/V. */
static uam_ref rot(z80_lifter *L, unsigned y, uam_ref v, bool acc)
{
    const bool left = !(y & 1);
    uam_ref cy = bit(L, 8, v, left ? 7 : 0);   /* the bit shifted out */
    uam_ref in;                                /* the bit shifted in, in place */
    switch (y) {
    case 0: in = zext(L, 8, cy); break;
    case 1: in = bin(L, UAM_OP_SHL, 8, zext(L, 8, cy), imm(L, 7)); break;
    case 2: in = and8(L, f_get(L), ZF_C); break;
    case 3: in = bin(L, UAM_OP_SHL, 8, and8(L, f_get(L), ZF_C), imm(L, 7)); break;
    case 4: in = imm(L, 0); break;
    case 5: in = and8(L, v, 0x80); break;
    case 6: in = imm(L, 1); break;
    default: in = imm(L, 0); break;
    }
    uam_ref r = or8(L, bin(L, left ? UAM_OP_SHL : UAM_OP_LSHR, 8, v, imm(L, 1)), in);
    uam_ref f = acc ? or8(L, keep(L, ZF_S | ZF_Z | ZF_PV), and8(L, r, ZF_Y | ZF_X))
                    : or8(L, sz53(L, r), par(L, r));
    set_f(L, or8(L, f, zext(L, 8, cy)));
    return r;
}

static void daa(z80_lifter *L)
{
    uam_ref a = get8(L, Z80_A), f = f_get(L);
    uam_ref c = bit(L, 8, f, 0), h = bit(L, 8, f, 4), n = bit(L, 8, f, 1);
    uam_ref lo = and8(L, a, 0xF);
    uam_ref lo9 = bin(L, UAM_OP_ULT, 8, imm(L, 9), lo);
    uam_ref d1 = bin(L, UAM_OP_OR, 1, h, lo9);
    uam_ref c2 = bin(L, UAM_OP_OR, 1, c, bin(L, UAM_OP_ULT, 8, imm(L, 0x99), a));
    uam_ref diff = or8(L, sel8(L, d1, 6, 0), sel8(L, c2, 0x60, 0));
    uam_ref r = uam_select(L->b, 8, n, bin(L, UAM_OP_SUB, 8, a, diff), bin(L, UAM_OP_ADD, 8, a, diff));
    uam_ref h2 = uam_select(L->b, 1, n, bin(L, UAM_OP_AND, 1, h, bin(L, UAM_OP_ULT, 8, lo, imm(L, 6))), lo9);
    uam_ref nf = or8(L, sz53(L, r), par(L, r));
    nf = or8(L, nf, fb(L, h2, 4));
    nf = or8(L, nf, and8(L, f, ZF_N));
    nf = or8(L, nf, zext(L, 8, c2));
    set_f(L, nf);
    qput(L, 8, Z80_A, r);
}

/* SCF and CCF: bits 3 and 5 are (Q ^ F) | A, Q being F if the last instruction set the
 * flags, else 0. */
static uam_ref q_xy(z80_lifter *L)
{
    uam_ref f = f_get(L);
    uam_ref qf = uam_select(L->b, 8, bin(L, UAM_OP_EQ, 8, get8(L, Z80_QV), imm(L, 0)), f, imm(L, 0));
    return and8(L, or8(L, qf, get8(L, Z80_A)), ZF_Y | ZF_X);
}

/* ADD HL,rp (adc/sbc: ADC/SBC HL,rp). */
static void arith16(z80_lifter *L, uint32_t dst, uam_ref x, uam_ref y, int kind /* 0 add, 1 adc, 2 sbc */)
{
    const bool sub = kind == 2;
    const uam_opcode op = sub ? UAM_OP_SUB : UAM_OP_ADD;
    uam_ref s = bin(L, op, 32, zext(L, 32, x), zext(L, 32, y));
    if (kind)
        s = bin(L, op, 32, s, zext(L, 32, and8(L, f_get(L), ZF_C)));
    uam_ref r = trunc_(L, 16, s);
    uam_ref hi = hi8(L, r);
    uam_ref h = bit(L, 16, bin(L, UAM_OP_XOR, 16, bin(L, UAM_OP_XOR, 16, x, y), r), 12);
    uam_ref f = or8(L, and8(L, hi, ZF_Y | ZF_X), fb(L, h, 4));
    f = or8(L, f, zext(L, 8, bit(L, 32, s, 16)));
    if (kind == 0) {
        f = or8(L, f, keep(L, ZF_S | ZF_Z | ZF_PV));
    } else {
        uam_ref ov = sub ? bin(L, UAM_OP_AND, 16, bin(L, UAM_OP_XOR, 16, x, y), bin(L, UAM_OP_XOR, 16, x, r))
                         : bin(L, UAM_OP_AND, 16, bin(L, UAM_OP_XOR, 16, x, r), bin(L, UAM_OP_XOR, 16, y, r));
        f = or8(L, f, and8(L, hi, ZF_S));
        f = or8(L, f, sel8(L, bin(L, UAM_OP_EQ, 16, r, imm(L, 0)), ZF_Z, 0));
        f = or8(L, f, fb(L, bit(L, 16, ov, 15), 2));
        if (sub)
            f = or8(L, f, imm(L, ZF_N));
    }
    set_f(L, f);
    qput(L, 16, Z80_WZ, add16(L, x, 1));
    qput(L, 16, dst, r);
}

/* ---- control flow ------------------------------------------------------------------ */

static lift_result jump(z80_lifter *L, uam_ref t, uam_exit_kind kind)
{
    commit(L);
    uam_exit(L->b, NONE, uam_ref_is_imm(t) ? t : zext(L, 32, t), kind);
    return LIFT_END;
}
/* If c, to t; else on. */
static lift_result branch(z80_lifter *L, uam_ref c, uint16_t t, uam_exit_kind kind)
{
    commit(L);
    uam_exit(L->b, c, imm(L, t), kind);
    uam_exit(L->b, NONE, imm(L, next_pc(L)), UAM_EXIT_JUMP);
    return LIFT_END;
}
static uint16_t rel(const z80_lifter *L) { return (uint16_t)(next_pc(L) + (int8_t)L->n); }

/* ---- decoding ---------------------------------------------------------------------- */

static uint8_t fetch(z80_lifter *L)
{
    uint64_t v = 0;
    if (!L->fault && L->lc->fetch(L->lc, (uint16_t)(L->pc + L->len), 1, &v) != UAM_OK)
        L->fault = true;
    L->len++;
    return (uint8_t)v;
}
static void fetch_nn(z80_lifter *L)
{
    const uint8_t lo = fetch(L);
    L->nn = (uint16_t)(lo | fetch(L) << 8);
}

/* Read the instruction at L->pc: prefixes, opcode and operands. */
static void decode(z80_lifter *L)
{
    uint8_t op = fetch(L);
    L->m1 = 1;
    if (op == 0xDD || op == 0xFD) {
        const uint8_t nx = fetch(L);
        if (nx == 0xDD || nx == 0xFD || nx == 0xED) {   /* this prefix does nothing */
            L->len = 1;
            L->prefix_nop = true;
            return;
        }
        L->idx = op == 0xDD ? 1 : 2;
        L->m1 = 2;
        op = nx;
    }
    if (op == 0xCB) {
        L->page = 0xCB;
        L->m1 = 2;
        if (L->idx)
            L->d = (int8_t)fetch(L);   /* DD CB d op: the opcode is not a fetch R counts */
        L->op = fetch(L);
        return;
    }
    if (op == 0xED) {
        L->page = 0xED;
        L->m1 = 2;
        L->op = op = fetch(L);
        if ((op & 0xC7) == 0x43)       /* LD (nn),rp / LD rp,(nn) */
            fetch_nn(L);
        return;
    }
    L->op = op;
    const unsigned x = op >> 6, y = (op >> 3) & 7, z = op & 7;
    const bool mem = L->idx && op != 0x76
                     && ((x == 0 && (z == 4 || z == 5 || z == 6) && y == 6)
                         || (x == 1 && (y == 6 || z == 6)) || (x == 2 && z == 6));
    if (mem)
        L->d = (int8_t)fetch(L);
    switch (x) {
    case 0:
        if (z == 0 && y >= 2) L->n = fetch(L);                 /* DJNZ, JR */
        else if (z == 1 && !(y & 1)) fetch_nn(L);               /* LD rp,nn */
        else if (z == 2 && y >= 4) fetch_nn(L);                 /* LD (nn),HL/A ... */
        else if (z == 6) L->n = fetch(L);                       /* LD r,n */
        break;
    case 3:
        if (z == 2 || z == 4 || (z == 3 && y == 0) || (z == 5 && y == 1)) fetch_nn(L);   /* JP CALL */
        else if (z == 6 || (z == 3 && (y == 2 || y == 3))) L->n = fetch(L);             /* ALU n, OUT/IN */
        break;
    default:
        break;
    }
}

/* ---- the main page ------------------------------------------------------------------ */

static lift_result lift_main(z80_lifter *L)
{
    const uint8_t op = L->op;
    const unsigned x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;
    if (x == 1) {
        if (op == 0x76) {                                               /* HALT */
            commit(L);
            uam_wait(L->b, imm(L, next_pc(L)));
            return LIFT_END;
        }
        const bool mem = y == 6 || z == 6;                              /* LD r,r' */
        uam_ref a = mem ? mem_hl(L) : NONE;
        opset(L, y, !mem, a, opget(L, z, !mem, a));
        return LIFT_CONTINUE;
    }
    if (x == 2) {                                                       /* ALU A,r */
        uam_ref a = z == 6 ? mem_hl(L) : NONE;
        uam_ref r = alu(L, y, get8(L, Z80_A), opget(L, z, true, a));
        if (!uam_ref_is_none(r))
            qput(L, 8, Z80_A, r);
        return LIFT_CONTINUE;
    }
    if (x == 0) {
        switch (z) {
        case 0:
            switch (y) {
            case 0: return LIFT_CONTINUE;                               /* NOP */
            case 1: {                                                   /* EX AF,AF' */
                uam_ref af = get16(L, Z80_AF), af2 = get16(L, Z80_AF2);
                qput(L, 16, Z80_AF, af2);
                qput(L, 16, Z80_AF2, af);
                return LIFT_CONTINUE;
            }
            case 2: {                                                   /* DJNZ */
                uam_ref b = bin(L, UAM_OP_SUB, 8, get8(L, Z80_B), imm(L, 1));
                uam_ref c = bin(L, UAM_OP_NE, 8, b, imm(L, 0));
                qput(L, 8, Z80_B, b);
                qput_g(L, 16, Z80_WZ, imm(L, rel(L)), c);
                return branch(L, c, rel(L), UAM_EXIT_JUMP);
            }
            case 3:                                                     /* JR */
                qput(L, 16, Z80_WZ, imm(L, rel(L)));
                return jump(L, imm(L, rel(L)), UAM_EXIT_JUMP);
            default: {                                                  /* JR cc */
                uam_ref c = cond(L, y - 4);
                qput_g(L, 16, Z80_WZ, imm(L, rel(L)), c);
                return branch(L, c, rel(L), UAM_EXIT_JUMP);
            }
            }
        case 1:
            if (!q)                                                     /* LD rp,nn */
                qput(L, 16, rp(L, p, false), imm(L, L->nn));
            else                                                        /* ADD HL,rp */
                arith16(L, hl_off(L), get16(L, hl_off(L)), get16(L, rp(L, p, false)), 0);
            return LIFT_CONTINUE;
        case 2: {
            uam_ref a = get8(L, Z80_A);
            switch (y) {
            case 0: case 2: {                                           /* LD (BC/DE),A */
                uam_ref ad = get16(L, y ? Z80_DE : Z80_BC);
                wr8(L, ad, a);
                qput(L, 16, Z80_WZ, word(L, lo8(L, add16(L, ad, 1)), a));
                break;
            }
            case 1: case 3: {                                           /* LD A,(BC/DE) */
                uam_ref ad = get16(L, y == 3 ? Z80_DE : Z80_BC);
                qput(L, 8, Z80_A, rd8(L, ad));
                qput(L, 16, Z80_WZ, add16(L, ad, 1));
                break;
            }
            case 4:                                                     /* LD (nn),HL */
                wr16(L, imm(L, L->nn), get16(L, hl_off(L)));
                qput(L, 16, Z80_WZ, imm(L, (uint16_t)(L->nn + 1)));
                break;
            case 5:                                                     /* LD HL,(nn) */
                qput(L, 16, hl_off(L), rd16(L, imm(L, L->nn)));
                qput(L, 16, Z80_WZ, imm(L, (uint16_t)(L->nn + 1)));
                break;
            case 6:                                                     /* LD (nn),A */
                wr8(L, imm(L, L->nn), a);
                qput(L, 16, Z80_WZ, word(L, imm(L, (uint8_t)(L->nn + 1)), a));
                break;
            default:                                                    /* LD A,(nn) */
                qput(L, 8, Z80_A, rd8(L, imm(L, L->nn)));
                qput(L, 16, Z80_WZ, imm(L, (uint16_t)(L->nn + 1)));
                break;
            }
            return LIFT_CONTINUE;
        }
        case 3: {                                                       /* INC/DEC rp */
            const uint32_t o = rp(L, p, false);
            qput(L, 16, o, add16(L, get16(L, o), q ? -1 : 1));
            return LIFT_CONTINUE;
        }
        case 4: case 5: {                                               /* INC/DEC r */
            uam_ref a = y == 6 ? mem_hl(L) : NONE;
            opset(L, y, true, a, inc_dec(L, opget(L, y, true, a), z == 5));
            return LIFT_CONTINUE;
        }
        case 6: {                                                       /* LD r,n */
            uam_ref a = y == 6 ? mem_hl(L) : NONE;
            opset(L, y, true, a, imm(L, L->n));
            return LIFT_CONTINUE;
        }
        default:
            switch (y) {
            case 0: case 1: case 2: case 3:                             /* RLCA RRCA RLA RRA */
                qput(L, 8, Z80_A, rot(L, y, get8(L, Z80_A), true));
                return LIFT_CONTINUE;
            case 4: daa(L); return LIFT_CONTINUE;                       /* DAA */
            case 5: {                                                   /* CPL */
                uam_ref r = bin(L, UAM_OP_XOR, 8, get8(L, Z80_A), imm(L, 0xFF));
                set_f(L, or8(L, or8(L, keep(L, ZF_S | ZF_Z | ZF_PV | ZF_C), and8(L, r, ZF_Y | ZF_X)),
                             imm(L, ZF_H | ZF_N)));
                qput(L, 8, Z80_A, r);
                return LIFT_CONTINUE;
            }
            case 6:                                                     /* SCF */
                set_f(L, or8(L, or8(L, keep(L, ZF_S | ZF_Z | ZF_PV), q_xy(L)), imm(L, ZF_C)));
                return LIFT_CONTINUE;
            default: {                                                  /* CCF */
                uam_ref c = bit(L, 8, f_get(L), 0);
                uam_ref f = or8(L, keep(L, ZF_S | ZF_Z | ZF_PV), q_xy(L));
                set_f(L, or8(L, or8(L, f, fb(L, c, 4)), zext(L, 8, not1(L, c))));
                return LIFT_CONTINUE;
            }
            }
        }
    }
    /* x == 3 */
    switch (z) {
    case 0: {                                                           /* RET cc */
        uam_ref c = cond(L, y);
        uam_ref t = pop16_g(L, c);
        qput_g(L, 16, Z80_WZ, t, c);
        commit(L);
        uam_exit(L->b, c, zext(L, 32, t), UAM_EXIT_RET);
        uam_exit(L->b, NONE, imm(L, next_pc(L)), UAM_EXIT_JUMP);
        return LIFT_END;
    }
    case 1:
        if (!q) {                                                       /* POP rp2 */
            qput(L, 16, rp(L, p, true), pop16_g(L, NONE));
            return LIFT_CONTINUE;
        }
        switch (p) {
        case 0: {                                                       /* RET */
            uam_ref t = pop16_g(L, NONE);
            qput(L, 16, Z80_WZ, t);
            return jump(L, t, UAM_EXIT_RET);
        }
        case 1: {                                                       /* EXX */
            static const uint32_t a[3] = { Z80_BC, Z80_DE, Z80_HL }, b[3] = { Z80_BC2, Z80_DE2, Z80_HL2 };
            uam_ref va[3], vb[3];
            for (unsigned i = 0; i < 3; i++) {
                va[i] = get16(L, a[i]);
                vb[i] = get16(L, b[i]);
            }
            for (unsigned i = 0; i < 3; i++) {
                qput(L, 16, a[i], vb[i]);
                qput(L, 16, b[i], va[i]);
            }
            return LIFT_CONTINUE;
        }
        case 2: return jump(L, get16(L, hl_off(L)), UAM_EXIT_JUMP);     /* JP (HL) */
        default: qput(L, 16, Z80_SP, get16(L, hl_off(L))); return LIFT_CONTINUE;   /* LD SP,HL */
        }
    case 2: {                                                           /* JP cc,nn */
        qput(L, 16, Z80_WZ, imm(L, L->nn));
        return branch(L, cond(L, y), L->nn, UAM_EXIT_JUMP);
    }
    case 3:
        switch (y) {
        case 0:                                                         /* JP nn */
            qput(L, 16, Z80_WZ, imm(L, L->nn));
            return jump(L, imm(L, L->nn), UAM_EXIT_JUMP);
        case 2: {                                                       /* OUT (n),A */
            uam_ref a = get8(L, Z80_A);
            out8(L, word(L, imm(L, L->n), a), a);
            qput(L, 16, Z80_WZ, word(L, imm(L, (uint8_t)(L->n + 1)), a));
            return LIFT_CONTINUE;
        }
        case 3: {                                                       /* IN A,(n) */
            uam_ref port = word(L, imm(L, L->n), get8(L, Z80_A));
            qput(L, 8, Z80_A, in8(L, port));
            qput(L, 16, Z80_WZ, add16(L, port, 1));
            return LIFT_CONTINUE;
        }
        case 4: {                                                       /* EX (SP),HL */
            uam_ref sp = get16(L, Z80_SP), hl = get16(L, hl_off(L));
            uam_ref v = rd16(L, sp);
            wr16(L, sp, hl);
            qput(L, 16, hl_off(L), v);
            qput(L, 16, Z80_WZ, v);
            return LIFT_CONTINUE;
        }
        case 5: {                                                       /* EX DE,HL */
            uam_ref de = get16(L, Z80_DE), hl = get16(L, Z80_HL);
            qput(L, 16, Z80_DE, hl);
            qput(L, 16, Z80_HL, de);
            return LIFT_CONTINUE;
        }
        case 6:                                                         /* DI */
            uam_syswr(L->b, 8, Z80_SYS_IFF, imm(L, 0), NONE);
            return LIFT_CONTINUE;
        default:                                                        /* EI */
            uam_syswr(L->b, 8, Z80_SYS_IFF, imm(L, 3), NONE);
            L->cont_next = true;
            return LIFT_CONTINUE;
        }
    case 4: {                                                           /* CALL cc,nn */
        uam_ref c = cond(L, y);
        push16_g(L, imm(L, next_pc(L)), c);
        qput(L, 16, Z80_WZ, imm(L, L->nn));
        return branch(L, c, L->nn, UAM_EXIT_CALL);
    }
    case 5:
        if (!q) {                                                       /* PUSH rp2 */
            push16(L, get16(L, rp(L, p, true)));
            return LIFT_CONTINUE;
        }
        push16(L, imm(L, next_pc(L)));                                  /* CALL nn */
        qput(L, 16, Z80_WZ, imm(L, L->nn));
        return jump(L, imm(L, L->nn), UAM_EXIT_CALL);
    case 6: {                                                           /* ALU A,n */
        uam_ref r = alu(L, y, get8(L, Z80_A), imm(L, L->n));
        if (!uam_ref_is_none(r))
            qput(L, 8, Z80_A, r);
        return LIFT_CONTINUE;
    }
    default:                                                            /* RST */
        push16(L, imm(L, next_pc(L)));
        qput(L, 16, Z80_WZ, imm(L, y * 8u));
        return jump(L, imm(L, y * 8u), UAM_EXIT_CALL);
    }
}

/* ---- CB ------------------------------------------------------------------------------ */

static lift_result lift_cb(z80_lifter *L)
{
    const uint8_t op = L->op;
    const unsigned x = op >> 6, y = (op >> 3) & 7, z = op & 7;
    /* DD CB d op works on (IX+d) and copies the result to register z unless z is 6. */
    const bool mem = L->idx || z == 6;
    uam_ref a = mem ? mem_hl(L) : NONE;
    uam_ref v = mem ? rd8(L, a) : get8(L, r8(L, z, false));
    if (x == 1) {                                                       /* BIT y */
        uam_ref t = and8(L, v, (uint8_t)(1u << y));
        uam_ref xy = !mem ? v : L->idx ? hi8(L, a) : hi8(L, get16(L, Z80_WZ));
        uam_ref nz = bin(L, UAM_OP_NE, 8, t, imm(L, 0));
        uam_ref f = or8(L, keep(L, ZF_C), imm(L, ZF_H));
        f = or8(L, f, sel8(L, nz, y == 7 ? ZF_S : 0, ZF_Z | ZF_PV));
        set_f(L, or8(L, f, and8(L, xy, ZF_Y | ZF_X)));
        return LIFT_CONTINUE;
    }
    uam_ref r;
    if (x == 0)
        r = rot(L, y, v, false);
    else if (x == 2)
        r = and8(L, v, (uint8_t)~(1u << y));                            /* RES */
    else
        r = or8(L, v, imm(L, 1u << y));                                 /* SET */
    if (mem)
        wr8(L, a, r);
    if (!mem || (L->idx && z != 6))
        qput(L, 8, r8(L, z, false), r);
    return LIFT_CONTINUE;
}

/* ---- ED ------------------------------------------------------------------------------ */

/* The repeating block instructions while they repeat: bits 3 and 5 of F from the high byte
 * of the instruction's address. */
static uam_ref pc_xy(z80_lifter *L, uam_ref f)
{
    return or8(L, and8(L, f, (uint8_t)~(ZF_Y | ZF_X)), imm(L, (L->pc >> 8) & (ZF_Y | ZF_X)));
}
static lift_result repeat(z80_lifter *L, uam_ref rep)
{
    qput_g(L, 16, Z80_WZ, imm(L, (uint16_t)(L->pc + 1)), rep);
    return branch(L, rep, L->pc, UAM_EXIT_JUMP);
}

static lift_result lift_block(z80_lifter *L, unsigned y, unsigned z)
{
    const bool dec = y & 1, rpt = y >= 6;
    const int step = dec ? -1 : 1;
    uam_ref hl = get16(L, Z80_HL), bc = get16(L, Z80_BC), f = f_get(L);
    uam_ref hl2 = add16(L, hl, step);
    if (z == 0) {                                                       /* LDI LDD LDIR LDDR */
        uam_ref de = get16(L, Z80_DE);
        uam_ref v = rd8(L, hl);
        wr8(L, de, v);
        uam_ref bc2 = add16(L, bc, -1);
        uam_ref pv = bin(L, UAM_OP_NE, 16, bc2, imm(L, 0));
        uam_ref n = bin(L, UAM_OP_ADD, 8, v, get8(L, Z80_A));
        uam_ref nf = or8(L, and8(L, f, ZF_S | ZF_Z | ZF_C), fb(L, pv, 2));
        nf = or8(L, nf, and8(L, n, ZF_X));
        nf = or8(L, nf, bin(L, UAM_OP_SHL, 8, and8(L, n, 0x02), imm(L, 4)));
        if (rpt)
            nf = uam_select(L->b, 8, pv, pc_xy(L, nf), nf);
        set_f(L, nf);
        qput(L, 16, Z80_HL, hl2);
        qput(L, 16, Z80_DE, add16(L, de, step));
        qput(L, 16, Z80_BC, bc2);
        return rpt ? repeat(L, pv) : LIFT_CONTINUE;
    }
    if (z == 1) {                                                       /* CPI CPD CPIR CPDR */
        uam_ref a = get8(L, Z80_A), v = rd8(L, hl);
        uam_ref r = bin(L, UAM_OP_SUB, 8, a, v);
        uam_ref h = bit(L, 8, bin(L, UAM_OP_XOR, 8, bin(L, UAM_OP_XOR, 8, a, v), r), 4);
        uam_ref n = bin(L, UAM_OP_SUB, 8, r, zext(L, 8, h));
        uam_ref bc2 = add16(L, bc, -1);
        uam_ref pv = bin(L, UAM_OP_NE, 16, bc2, imm(L, 0));
        uam_ref nf = or8(L, or8(L, and8(L, f, ZF_C), imm(L, ZF_N)), and8(L, r, ZF_S));
        nf = or8(L, nf, zflag(L, r));
        nf = or8(L, nf, fb(L, h, 4));
        nf = or8(L, nf, fb(L, pv, 2));
        nf = or8(L, nf, and8(L, n, ZF_X));
        nf = or8(L, nf, bin(L, UAM_OP_SHL, 8, and8(L, n, 0x02), imm(L, 4)));
        uam_ref rep = rpt ? bin(L, UAM_OP_AND, 1, pv, bin(L, UAM_OP_NE, 8, r, imm(L, 0))) : NONE;
        if (rpt)
            nf = uam_select(L->b, 8, rep, pc_xy(L, nf), nf);
        set_f(L, nf);
        qput(L, 16, Z80_HL, hl2);
        qput(L, 16, Z80_BC, bc2);
        qput(L, 16, Z80_WZ, add16(L, get16(L, Z80_WZ), step));
        return rpt ? repeat(L, rep) : LIFT_CONTINUE;
    }
    /* INI IND INIR INDR, OUTI OUTD OTIR OTDR */
    uam_ref b = hi8(L, bc), c = lo8(L, bc);
    uam_ref b2 = bin(L, UAM_OP_SUB, 8, b, imm(L, 1));
    uam_ref v, k;
    if (z == 2) {
        v = in8(L, bc);
        wr8(L, hl, v);
        qput(L, 16, Z80_WZ, add16(L, bc, step));
        k = bin(L, UAM_OP_ADD, 16, zext(L, 16, v), zext(L, 16, bin(L, UAM_OP_ADD, 8, c, imm(L, dec ? 0xFF : 1))));
    } else {
        v = rd8(L, hl);
        uam_ref bc2 = word(L, c, b2);
        out8(L, bc2, v);
        qput(L, 16, Z80_WZ, add16(L, bc2, step));
        k = bin(L, UAM_OP_ADD, 16, zext(L, 16, v), zext(L, 16, lo8(L, hl2)));
    }
    uam_ref kc = bit(L, 16, k, 8);
    uam_ref nv = bit(L, 8, v, 7);
    uam_ref pvb = par(L, bin(L, UAM_OP_XOR, 8, and8(L, trunc_(L, 8, k), 7), b2));
    uam_ref base = or8(L, sz53(L, b2), fb(L, nv, 1));
    base = or8(L, base, sel8(L, kc, ZF_H | ZF_C, 0));
    uam_ref nf = or8(L, base, pvb);
    uam_ref rep = NONE;
    if (rpt) {
        /* While repeating, P/V also takes the parity of B's low bits after a further step
         * (with the carry, up when the byte's bit 7 is clear, else down) and H whether that
         * step carries out of B's low nibble. */
        rep = bin(L, UAM_OP_NE, 8, b2, imm(L, 0));
        uam_ref up = bin(L, UAM_OP_ADD, 8, b2, imm(L, 1)), down = bin(L, UAM_OP_SUB, 8, b2, imm(L, 1));
        uam_ref t = uam_select(L->b, 8, kc, uam_select(L->b, 8, nv, down, up), b2);
        uam_ref pv2 = bin(L, UAM_OP_XOR, 8, pvb, bin(L, UAM_OP_XOR, 8, par(L, and8(L, t, 7)), imm(L, ZF_PV)));
        uam_ref hn = uam_select(L->b, 1, nv, bin(L, UAM_OP_EQ, 8, and8(L, b2, 0xF), imm(L, 0)),
                                bin(L, UAM_OP_EQ, 8, and8(L, b2, 0xF), imm(L, 0xF)));
        uam_ref h2 = bin(L, UAM_OP_AND, 1, kc, hn);
        uam_ref rf = or8(L, and8(L, base, (uint8_t)~ZF_H), pv2);
        rf = pc_xy(L, or8(L, rf, fb(L, h2, 4)));
        nf = uam_select(L->b, 8, rep, rf, nf);
    }
    set_f(L, nf);
    qput(L, 8, Z80_B, b2);
    qput(L, 16, Z80_HL, hl2);
    return rpt ? repeat(L, rep) : LIFT_CONTINUE;
}

static lift_result lift_ed(z80_lifter *L)
{
    const uint8_t op = L->op;
    const unsigned x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;
    if (x == 2 && z <= 3 && y >= 4)
        return lift_block(L, y, z);
    if (x != 1)
        return LIFT_CONTINUE;                                           /* no operation */
    switch (z) {
    case 0: {                                                           /* IN r,(C) */
        uam_ref bc = get16(L, Z80_BC);
        uam_ref v = in8(L, bc);
        set_f(L, or8(L, or8(L, keep(L, ZF_C), sz53(L, v)), par(L, v)));
        if (y != 6)
            qput(L, 8, r8(L, y, false), v);
        qput(L, 16, Z80_WZ, add16(L, bc, 1));
        return LIFT_CONTINUE;
    }
    case 1: {                                                           /* OUT (C),r */
        uam_ref bc = get16(L, Z80_BC);
        out8(L, bc, y == 6 ? imm(L, L->s->cfg.out_c_zero) : get8(L, r8(L, y, false)));
        qput(L, 16, Z80_WZ, add16(L, bc, 1));
        return LIFT_CONTINUE;
    }
    case 2:                                                             /* SBC/ADC HL,rp */
        arith16(L, Z80_HL, get16(L, Z80_HL), get16(L, rp(L, p, false)), q ? 1 : 2);
        return LIFT_CONTINUE;
    case 3:
        if (!q)                                                         /* LD (nn),rp */
            wr16(L, imm(L, L->nn), get16(L, rp(L, p, false)));
        else                                                            /* LD rp,(nn) */
            qput(L, 16, rp(L, p, false), rd16(L, imm(L, L->nn)));
        qput(L, 16, Z80_WZ, imm(L, (uint16_t)(L->nn + 1)));
        return LIFT_CONTINUE;
    case 4: {                                                           /* NEG */
        uam_ref r = alu(L, 2, imm(L, 0), get8(L, Z80_A));
        qput(L, 8, Z80_A, r);
        return LIFT_CONTINUE;
    }
    case 5: {                                                           /* RETN, RETI */
        uam_ref t = pop16_g(L, NONE);
        uam_syswr(L->b, 8, Z80_SYS_RETN, imm(L, y == 1), NONE);
        qput(L, 16, Z80_WZ, t);
        return jump(L, t, UAM_EXIT_EXC_RETURN);
    }
    case 6: {                                                           /* IM */
        static const uint8_t mode[8] = { 0, 0, 1, 2, 0, 0, 1, 2 };
        uam_syswr(L->b, 8, Z80_SYS_IM, imm(L, mode[y]), NONE);
        return LIFT_CONTINUE;
    }
    default:
        break;
    }
    switch (y) {
    case 0: qput(L, 8, Z80_I, get8(L, Z80_A)); return LIFT_CONTINUE;    /* LD I,A */
    case 1: {                                                           /* LD R,A */
        uam_ref a = get8(L, Z80_A);
        qput(L, 8, Z80_RCNT, a);
        qput(L, 8, Z80_R7, and8(L, a, 0x80));
        return LIFT_CONTINUE;
    }
    case 2: case 3: {                                                   /* LD A,I / LD A,R */
        uam_ref iff = uam_sysrd(L->b, 8, Z80_SYS_IFF, NONE);
        uam_ref v = y == 2 ? get8(L, Z80_I)
                           : or8(L, get8(L, Z80_R7),
                                 and8(L, bin(L, UAM_OP_ADD, 8, get8(L, Z80_RCNT), imm(L, L->m1)), 0x7F));
        uam_ref f = or8(L, keep(L, ZF_C), sz53(L, v));
        set_f(L, or8(L, f, bin(L, UAM_OP_SHL, 8, and8(L, iff, 2), imm(L, 1))));
        qput(L, 8, Z80_A, v);
        return LIFT_CONTINUE;
    }
    case 4: case 5: {                                                   /* RRD, RLD */
        uam_ref hl = get16(L, Z80_HL), a = get8(L, Z80_A);
        uam_ref t = rd8(L, hl), m, na;
        if (y == 4) {
            m = or8(L, bin(L, UAM_OP_SHL, 8, a, imm(L, 4)), bin(L, UAM_OP_LSHR, 8, t, imm(L, 4)));
            na = or8(L, and8(L, a, 0xF0), and8(L, t, 0x0F));
        } else {
            m = or8(L, bin(L, UAM_OP_SHL, 8, t, imm(L, 4)), and8(L, a, 0x0F));
            na = or8(L, and8(L, a, 0xF0), bin(L, UAM_OP_LSHR, 8, t, imm(L, 4)));
        }
        wr8(L, hl, m);
        set_f(L, or8(L, or8(L, keep(L, ZF_C), sz53(L, na)), par(L, na)));
        qput(L, 8, Z80_A, na);
        qput(L, 16, Z80_WZ, add16(L, hl, 1));
        return LIFT_CONTINUE;
    }
    default:
        return LIFT_CONTINUE;                                           /* no operation */
    }
}

/* ---- blocks -------------------------------------------------------------------------- */

uam_status z80_lift(uam_lift_ctx *lc)
{
    const z80_state *s = z80_st(lc->cpu);
    uint16_t pc = (uint16_t)lc->pc;
    bool cont = false;         /* this instruction continues the last (EI, a prefix) */
    bool clear_shadow = false; /* it follows an EI */
    int qv = -1;               /* QV as the last instruction left it, -1 unknown */
    for (uint32_t n = 0;; n++) {
        /* Also in the middle of a run of EIs or prefixes, which could otherwise grow
         * without bound: an EI's shadow is carried by the core's flag (irq_take). */
        if (n >= lc->max_insns) {
            uam_exit(lc->b, NONE, uam_imm(lc->b, pc), UAM_EXIT_JUMP);
            return UAM_OK;
        }
        z80_lifter L = { .lc = lc, .b = lc->b, .s = s, .pc = pc, .qv_prev = qv };
        decode(&L);
        if (L.fault) {
            if (n == 0)
                return UAM_ERR_FAULT;
            uam_exit(lc->b, NONE, uam_imm(lc->b, pc), UAM_EXIT_JUMP);
            return UAM_OK;
        }
        if (cont)
            uam_imark_cont(lc->b, pc, L.len);
        else
            uam_imark(lc->b, pc, L.len);
        if (clear_shadow)
            uam_syswr(lc->b, 8, Z80_SYS_SHADOW, uam_imm(lc->b, 0), NONE);
        /* R counts the opcode fetches. */
        qput(&L, 8, Z80_RCNT, bin(&L, UAM_OP_ADD, 8, get8(&L, Z80_RCNT), imm(&L, L.m1)));
        lift_result r;
        if (L.prefix_nop) {
            L.cont_next = true;
            r = LIFT_CONTINUE;
        } else {
            r = L.page == 0xCB ? lift_cb(&L) : L.page == 0xED ? lift_ed(&L) : lift_main(&L);
        }
        if (r == LIFT_END)          /* it committed its writes before leaving */
            return UAM_OK;
        commit(&L);
        qv = L.fset;
        clear_shadow = L.cont_next && !L.prefix_nop;
        cont = L.cont_next;
        pc = next_pc(&L);
    }
}
