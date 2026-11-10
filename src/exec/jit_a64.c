/*
 * jit_a64.c - The AArch64 code generator of the native backend (jit.h, jit_impl.h).
 *
 * Compiles one verified, optimized block, analysed by jit.c, to a host function
 *
 *     void code(uam_jit_ctx *jc);
 *
 * that leaves the cpu in exactly the state uam_interp_block would and writes the
 * block's outcome into jc->res. It follows jit_x86.c op for op: every op is emitted
 * inline except where C must run anyway (memory accesses the inline data-TLB path
 * cannot do, sysregs, helpers, and POPCNT under UAM_JIT_BASELINE), and test_jitdiff
 * checks the two backends agree with the interpreter.
 *
 * Host register use (AAPCS64; x18 is the platform register on macOS and is never
 * touched):
 *   x19 = jc    x20 = jc->temps    x21 = jc->cpu    x22 = jc->cpu->regfile
 *   x23 = icount - jc->ic_base (cpu->icount is brought up to date before bridges and
 *         at the epilogue; see jit_x86.c on the IMARK check)
 *   x24..x28, x9..x13 = guest temporaries (register allocation, jit.c)
 *   x0 = every op's result; x1, x2 (memory address), x3 (store value), x17 = scratch;
 *   x16 = the address of a far field or temp slot, a bridge's address
 *   v0 = POPCNT
 * x19..x28 are callee-saved, so the fixed registers and five temporaries survive
 * bridge calls; x9..x13 are saved around them while they hold a live temp. Temporaries
 * are kept zero-extended to their width in a register or, when spilled, in jc->temps[].
 * An op's template uses only x0, x1, x2, x16 and x17 (and the flags), so a replay on a
 * store's slow path keeps the value in x3.
 *
 * Layout of a compiled block: prologue | body | out-of-line stubs | epilogue | link
 * slots | fault sites, as on x86-64: every block has the same 96-byte frame (x29, x30,
 * x19..x28), so a chained jump goes from an EXIT straight to the next block's body with
 * br. Slots and sites are addressed with adr, which reaches +-1 MiB (a block is at most
 * CBUF_MAX); conditional branches reach as far.
 *
 * This file builds only on little-endian AArch64 (jit_x86.c is the x86-64 one).
 */
#include "jit_impl.h"

#if defined(__aarch64__) && defined(__AARCH64EL__)

#include <stdlib.h>
#include <string.h>

/* ---- the compiler ------------------------------------------------------ */

/* Host registers (31 is the zero register, or sp where an encoding says so). */
enum { X0 = 0, X1 = 1, X2 = 2, X3 = 3, X4 = 4, X5 = 5, X9 = 9, X10 = 10, X11 = 11, X12 = 12,
       X13 = 13, X16 = 16, X17 = 17, X19 = 19, X20 = 20, X21 = 21, X22 = 22, X23 = 23,
       X24 = 24, X25 = 25, X26 = 26, X27 = 27, X28 = 28, X29 = 29, X30 = 30, XZR = 31, SP = 31 };
enum { JC = X19, TMP = X20, CPUR = X21, RF = X22, IC = X23 };

/* Registers that hold temps, the callee-saved ones first (they survive bridge calls).
 * x14 and x15 stay free. */
#define RA_POOL_MAX 10
static const int ra_regs[RA_POOL_MAX] = { X24, X25, X26, X27, X28, X9, X10, X11, X12, X13 };

/* Condition codes. */
enum { CC_EQ = 0, CC_NE = 1, CC_HS = 2, CC_LO = 3, CC_VS = 6, CC_HI = 8, CC_LS = 9, CC_LT = 11,
       CC_GT = 12, CC_LE = 13 };

#define A64_NOP 0xD503201Fu
#define A64_BRK 0xD4200000u
#define A64_RET 0xD65F03C0u

static void g32(uam_jit *j, uint32_t v)
{
    if (j->n + 4 <= CBUF_MAX)
        memcpy(&j->cb[j->n], &v, 4);
    else
        j->overflow = true;
    j->n += 4;
}
static void g64(uam_jit *j, uint64_t v) { g32(j, (uint32_t)v); g32(j, (uint32_t)(v >> 32)); }

/* ---- encodings ------------------------------------------------------------------ */

/* The N:immr:imms field of a logical immediate for v (as a 64-bit or a 32-bit pattern),
 * or false if v has none. */
static bool logimm(uint64_t v, bool sf, uint32_t *field)
{
    if (!sf)
        v = (uint32_t)v | (uint64_t)(uint32_t)v << 32;
    if (v == 0 || v == ~UINT64_C(0))
        return false;
    unsigned e = 64;                                   /* the smallest repeating element */
    while (e > 2) {
        const unsigned h = e / 2;
        const uint64_t m = (UINT64_C(1) << h) - 1;
        if (((v >> h) & m) != (v & m))
            break;
        e = h;
    }
    const uint64_t mask = e == 64 ? ~UINT64_C(0) : (UINT64_C(1) << e) - 1;
    const uint64_t x = v & mask;
    const unsigned ones = (unsigned)__builtin_popcountll(x);
    const uint64_t run = (UINT64_C(1) << ones) - 1;    /* ones < e: x is not all ones */
    for (unsigned r = 0; r < e; r++) {                 /* x = rol(run, r) = ror(run, e - r) */
        const uint64_t rot = r ? ((x >> r) | (x << (e - r))) & mask : x;
        if (rot != run)
            continue;
        const uint32_t immr = (e - r) & (e - 1);
        const uint32_t imms = ((~(2 * e - 1)) & 0x3F) | (ones - 1);
        *field = (uint32_t)(e == 64) << 22 | immr << 16 | imms << 10;
        return true;
    }
    return false;
}

/* add/sub (immediate, imm12 optionally << 12), optionally setting the flags */
static void addsub_imm(uam_jit *j, bool sub, bool sf, bool setf, int rd, int rn, uint32_t imm12, bool sh)
{
    g32(j, (uint32_t)sf << 31 | (uint32_t)sub << 30 | (uint32_t)setf << 29 | 0x11000000u
           | (uint32_t)sh << 22 | (imm12 & 0xFFF) << 10 | (uint32_t)rn << 5 | (uint32_t)rd);
}
/* add/sub (shifted register): rd = rn +- (rm LSL amount) */
static void addsub_reg(uam_jit *j, bool sub, bool sf, bool setf, int rd, int rn, int rm, unsigned lsl)
{
    g32(j, (uint32_t)sf << 31 | (uint32_t)sub << 30 | (uint32_t)setf << 29 | 0x0B000000u
           | (uint32_t)rm << 16 | lsl << 10 | (uint32_t)rn << 5 | (uint32_t)rd);
}
/* subs zr, rn, rm (extended register: rm sign-extended from a byte or a halfword) */
static void cmp_sext(uam_jit *j, int rn, int rm, unsigned w)
{
    const uint32_t option = w == 8 ? 4 : 5;            /* SXTB / SXTH */
    g32(j, 0x6B200000u | (uint32_t)rm << 16 | option << 13 | (uint32_t)rn << 5 | XZR);
}

enum { LG_AND = 0, LG_ORR = 1, LG_EOR = 2, LG_ANDS = 3 };
static void logic_reg(uam_jit *j, int opc, bool sf, int rd, int rn, int rm)
{
    g32(j, (uint32_t)sf << 31 | (uint32_t)opc << 29 | 0x0A000000u | (uint32_t)rm << 16
           | (uint32_t)rn << 5 | (uint32_t)rd);
}
static void logic_imm(uam_jit *j, int opc, bool sf, int rd, int rn, uint32_t field)
{
    g32(j, (uint32_t)sf << 31 | (uint32_t)opc << 29 | 0x12000000u | field | (uint32_t)rn << 5
           | (uint32_t)rd);
}

/* Bitfield moves: SBFM (opc 0) and UBFM (opc 2). */
static void bfm(uam_jit *j, int opc, bool sf, int rd, int rn, unsigned immr, unsigned imms)
{
    g32(j, (uint32_t)sf << 31 | (uint32_t)opc << 29 | 0x13000000u | (uint32_t)sf << 22
           | immr << 16 | imms << 10 | (uint32_t)rn << 5 | (uint32_t)rd);
}
static void lsl_imm(uam_jit *j, bool sf, int rd, int rn, unsigned s)
{
    const unsigned size = sf ? 64 : 32;
    bfm(j, 2, sf, rd, rn, (size - s) & (size - 1), size - 1 - s);
}
static void lsr_imm(uam_jit *j, bool sf, int rd, int rn, unsigned s) { bfm(j, 2, sf, rd, rn, s, sf ? 63 : 31); }
static void asr_imm(uam_jit *j, bool sf, int rd, int rn, unsigned s) { bfm(j, 0, sf, rd, rn, s, sf ? 63 : 31); }
/* rd = the `width` bits of rn from bit lsb, sign-extended to 64 bits */
static void sbfx(uam_jit *j, int rd, int rn, unsigned lsb, unsigned width)
{
    bfm(j, 0, true, rd, rn, lsb, lsb + width - 1);
}
/* extr rd, rn, rm, #lsb (ror rd, rn, #lsb when rn == rm) */
static void extr(uam_jit *j, bool sf, int rd, int rn, int rm, unsigned lsb)
{
    g32(j, (uint32_t)sf << 31 | 0x13800000u | (uint32_t)sf << 22 | (uint32_t)rm << 16 | lsb << 10
           | (uint32_t)rn << 5 | (uint32_t)rd);
}

/* Data-processing, two sources. */
enum { DP_UDIV = 2, DP_SDIV = 3, DP_LSLV = 8, DP_LSRV = 9, DP_ASRV = 10, DP_RORV = 11 };
static void dp2(uam_jit *j, int op, bool sf, int rd, int rn, int rm)
{
    g32(j, (uint32_t)sf << 31 | 0x1AC00000u | (uint32_t)rm << 16 | (uint32_t)op << 10
           | (uint32_t)rn << 5 | (uint32_t)rd);
}
/* madd / msub: rd = ra +- rn * rm */
static void madd(uam_jit *j, bool sub, bool sf, int rd, int rn, int rm, int ra)
{
    g32(j, (uint32_t)sf << 31 | 0x1B000000u | (uint32_t)rm << 16 | (uint32_t)sub << 15
           | (uint32_t)ra << 10 | (uint32_t)rn << 5 | (uint32_t)rd);
}
static void mulh(uam_jit *j, bool sgn, int rd, int rn, int rm)
{
    g32(j, (sgn ? 0x9B400000u : 0x9BC00000u) | (uint32_t)rm << 16 | XZR << 10 | (uint32_t)rn << 5
           | (uint32_t)rd);
}
/* Data-processing, one source. */
enum { DP_RBIT = 0, DP_REV16 = 1, DP_REV32 = 2, DP_REV64 = 3, DP_CLZ = 4 };
static void dp1(uam_jit *j, int op, bool sf, int rd, int rn)
{
    g32(j, (uint32_t)sf << 31 | 0x5AC00000u | (uint32_t)op << 10 | (uint32_t)rn << 5 | (uint32_t)rd);
}
/* csel (inv = 0, inc = 0), csinc (0, 1), csinv (1, 0) */
static void csel(uam_jit *j, bool inv, bool inc, bool sf, int rd, int rn, int rm, unsigned cc)
{
    g32(j, (uint32_t)sf << 31 | (uint32_t)inv << 30 | 0x1A800000u | (uint32_t)rm << 16 | cc << 12
           | (uint32_t)inc << 10 | (uint32_t)rn << 5 | (uint32_t)rd);
}
/* rd = 1 if cc holds, else 0 */
static void cset(uam_jit *j, int rd, unsigned cc) { csel(j, false, true, false, rd, XZR, XZR, cc ^ 1); }

static void mov_rr(uam_jit *j, int dst, int src) { logic_reg(j, LG_ORR, true, dst, XZR, src); }
static void mov_rr32(uam_jit *j, int dst, int src) { logic_reg(j, LG_ORR, false, dst, XZR, src); }

/* reg = v without touching the flags: one orr if v is a logical immediate and would
 * need more than one move otherwise, else movz or movn (whichever leaves fewer
 * halfwords to fill) and movk. Values that fit 32 bits use the 32-bit forms. */
static void mov_imm(uam_jit *j, int reg, uint64_t v)
{
    const bool sf = v > 0xFFFFFFFFu;
    const unsigned nhw = sf ? 4 : 2;
    unsigned zeros = 0, ones = 0;
    for (unsigned k = 0; k < nhw; k++) {
        const uint32_t h = (uint32_t)(v >> (16 * k)) & 0xFFFF;
        zeros += h == 0;
        ones += h == 0xFFFF;
    }
    const bool neg = ones > zeros;
    uint32_t field;
    if ((neg ? nhw - ones : nhw - zeros) > 1 && logimm(v, sf, &field)) {
        logic_imm(j, LG_ORR, sf, reg, XZR, field);
        return;
    }
    const uint32_t fill = neg ? 0xFFFFu : 0;
    bool first = true;
    for (unsigned k = 0; k < nhw; k++) {
        const uint32_t h = (uint32_t)(v >> (16 * k)) & 0xFFFF;
        if (h == fill)
            continue;
        if (first)                                     /* movn / movz */
            g32(j, (uint32_t)sf << 31 | (neg ? 0x12800000u : 0x52800000u) | k << 21
                   | (neg ? ~h & 0xFFFF : h) << 5 | (uint32_t)reg);
        else                                           /* movk */
            g32(j, (uint32_t)sf << 31 | 0x72800000u | k << 21 | h << 5 | (uint32_t)reg);
        first = false;
    }
    if (first)                                         /* every halfword is the fill */
        g32(j, (uint32_t)sf << 31 | (neg ? 0x12800000u : 0x52800000u) | (uint32_t)reg);
}

/* rd = rn + v (64-bit, no flags); a far v goes through x16 (rn != x16 then). */
static void add_imm(uam_jit *j, int rd, int rn, uint64_t v)
{
    if (v < 0x1000) {
        addsub_imm(j, false, true, false, rd, rn, (uint32_t)v, false);
    } else if (v < 0x1000000) {
        addsub_imm(j, false, true, false, rd, rn, (uint32_t)(v >> 12), true);
        if (v & 0xFFF)
            addsub_imm(j, false, true, false, rd, rd, (uint32_t)v & 0xFFF, false);
    } else {
        mov_imm(j, X16, v);
        addsub_reg(j, false, true, false, rd, rn, X16, 0);
    }
}

/* Set the flags from rn - v at 32 (sf = 0) or 64 bits, as cmp; a v that is not an
 * add/sub immediate goes through x17 (rn != x17 then). */
static void cmp_imm(uam_jit *j, bool sf, int rn, uint64_t v)
{
    const uint64_t m = sf ? ~UINT64_C(0) : 0xFFFFFFFFu;
    v &= m;
    const uint64_t nv = (0 - v) & m;
    if (v < 0x1000)
        addsub_imm(j, true, sf, true, XZR, rn, (uint32_t)v, false);
    else if (nv < 0x1000)
        addsub_imm(j, false, sf, true, XZR, rn, (uint32_t)nv, false);   /* cmn */
    else {
        mov_imm(j, X17, v);
        addsub_reg(j, true, sf, true, XZR, rn, X17, 0);
    }
}

/* Loads and stores of 1 << sz bytes (zero-extending loads) at [rn + off]: scaled
 * unsigned offset, else unscaled, else the offset in x16 (rn != x16 then). */
static void mem(uam_jit *j, unsigned sz, bool load, int rt, int rn, int64_t off)
{
    const uint32_t base = sz << 30 | (uint32_t)load << 22 | (uint32_t)rn << 5 | (uint32_t)rt;
    if (off >= 0 && !(off & ((1 << sz) - 1)) && (off >> sz) < 0x1000)
        g32(j, base | 0x39000000u | (uint32_t)(off >> sz) << 10);
    else if (off >= -256 && off < 256)
        g32(j, base | 0x38000000u | ((uint32_t)off & 0x1FF) << 12);
    else {
        mov_imm(j, X16, (uint64_t)off);
        g32(j, base | 0x38206800u | (uint32_t)X16 << 16);   /* [rn, x16] */
    }
}
/* [rn + rm << (shift ? sz : 0)] */
static void mem_rr(uam_jit *j, unsigned sz, bool load, int rt, int rn, int rm, bool shift)
{
    g32(j, sz << 30 | 0x38206800u | (uint32_t)load << 22 | (uint32_t)rm << 16 | (uint32_t)shift << 12
           | (uint32_t)rn << 5 | (uint32_t)rt);
}
static unsigned sz_of(unsigned bytes) { return bytes == 1 ? 0 : bytes == 2 ? 1 : bytes == 4 ? 2 : 3; }
static void ld64(uam_jit *j, int rt, int rn, int32_t off) { mem(j, 3, true, rt, rn, off); }
static void st64(uam_jit *j, int rt, int rn, int32_t off) { mem(j, 3, false, rt, rn, off); }
static void st32(uam_jit *j, int rt, int rn, int32_t off) { mem(j, 2, false, rt, rn, off); }
static void st_temp(uam_jit *j, uint32_t idx, int reg) { st64(j, reg, TMP, (int32_t)(8 * idx)); }

/* stp / ldp of x registers at [sp + 8 * imm7] (signed offset), or with writeback */
static void stp_ldp(uam_jit *j, bool load, unsigned mode, int rt, int rt2, int imm7)
{
    g32(j, 0xA8000000u | mode << 23 | (uint32_t)load << 22 | ((uint32_t)imm7 & 0x7F) << 15
           | (uint32_t)rt2 << 10 | SP << 5 | (uint32_t)rt);
}
enum { PAIR_POST = 1, PAIR_OFF = 2, PAIR_PRE = 3 };

/* ---- branches ------------------------------------------------------------------- */

static size_t b_fwd(uam_jit *j) { size_t p = j->n; g32(j, 0x14000000u); return p; }
static size_t bcond_fwd(uam_jit *j, unsigned cc) { size_t p = j->n; g32(j, 0x54000000u | cc); return p; }
/* cbz / cbnz on the 32-bit view of rt */
static size_t cbz_fwd(uam_jit *j, bool nz, int rt)
{
    size_t p = j->n;
    g32(j, 0x34000000u | (uint32_t)nz << 24 | (uint32_t)rt);
    return p;
}
/* tbnz / tbz rt, #bit (+-32 KiB: only within the code-page store stubs and an EXIT) */
static size_t tbnz_fwd(uam_jit *j, int rt, unsigned bit)
{
    size_t p = j->n;
    g32(j, 0x37000000u | (bit >> 5) << 31 | (bit & 31) << 19 | (uint32_t)rt);
    return p;
}
static size_t tbz_fwd(uam_jit *j, int rt, unsigned bit)
{
    size_t p = j->n;
    g32(j, 0x36000000u | (bit >> 5) << 31 | (bit & 31) << 19 | (uint32_t)rt);
    return p;
}
static size_t adr_fwd(uam_jit *j, int rd) { size_t p = j->n; g32(j, 0x10000000u | (uint32_t)rd); return p; }

/* Point the branch (or adr) at `pos` to code offset `target`. */
static void patch_to(uam_jit *j, size_t pos, size_t target)
{
    if (pos + 4 > CBUF_MAX || target > CBUF_MAX)
        return;                                        /* j->overflow is set */
    uint32_t in;
    memcpy(&in, &j->cb[pos], 4);
    const uint32_t d = (uint32_t)((int64_t)target - (int64_t)pos);
    if ((in & 0x7C000000u) == 0x14000000u)             /* b, bl */
        in = (in & 0xFC000000u) | ((d >> 2) & 0x3FFFFFFu);
    else if ((in & 0xFF000010u) == 0x54000000u || (in & 0x7E000000u) == 0x34000000u
             || (in & 0xFF000000u) == 0x58000000u)
        in = (in & ~(0x7FFFFu << 5)) | ((d >> 2) & 0x7FFFFu) << 5;   /* b.cond, cbz, cbnz, ldr literal */
    else if ((in & 0x7E000000u) == 0x36000000u)        /* tbz, tbnz */
        in = (in & ~(0x3FFFu << 5)) | ((d >> 2) & 0x3FFFu) << 5;
    else                                               /* adr */
        in = (in & 0x9F00001Fu) | (d & 3) << 29 | ((d >> 2) & 0x7FFFFu) << 5;
    memcpy(&j->cb[pos], &in, 4);
}
static void patch(uam_jit *j, size_t pos) { patch_to(j, pos, j->n); }

/* call fn (through x16; clobbers x0..x17 and x30) */
static void call_bridge(uam_jit *j, const void *fn)
{
    mov_imm(j, X16, (uint64_t)(uintptr_t)fn);
    g32(j, 0xD63F0000u | X16 << 5);                    /* blr x16 */
}

/* ---- operands ------------------------------------------------------------------- */

/* Where an IR operand lives: an immediate, a host register, or a temp's slot in
 * jc->temps (a temp not allocated a register, or spilled). */
enum { O_IMM, O_MEM, O_REG };
typedef struct opnd { int kind; uint64_t imm; int32_t disp; int reg; } opnd;

static opnd opnd_of(const uam_jit *j, const uam_block *blk, uam_ref r, unsigned w)
{
    if (uam_ref_is_imm(r))
        return (opnd){ O_IMM, blk->imm[uam_ref_index(r)] & uam_mask(w), 0, 0 };
    if (j->reg_of[r] >= 0)
        return (opnd){ O_REG, 0, 0, j->reg_of[r] };
    return (opnd){ O_MEM, 0, (int32_t)(8 * r), 0 };
}

/* reg = the operand, a value of width w (temps are kept zero-extended, so a 32-bit
 * load of a narrow one is enough). */
static void load(uam_jit *j, int reg, opnd o, unsigned w)
{
    if (o.kind == O_IMM)
        mov_imm(j, reg, o.imm);
    else if (o.kind == O_REG) {
        if (o.reg != reg)
            mov_rr(j, reg, o.reg);
    } else {
        mem(j, w <= 32 ? 2 : 3, true, reg, TMP, o.disp);
    }
}

static void load_operand(uam_jit *j, int reg, const uam_block *blk, uam_ref r, unsigned w)
{
    load(j, reg, opnd_of(j, blk, r, w), w);
}

/* A register holding the operand: its own, or `scratch` loaded with it. */
static int src(uam_jit *j, opnd o, int scratch, unsigned w)
{
    if (o.kind == O_REG)
        return o.reg;
    load(j, scratch, o, w);
    return scratch;
}

/* Zero-extend the low `w` bits of reg to 64 bits (mask to width). */
static void mask_reg(uam_jit *j, int reg, unsigned w)
{
    if (w == 32)
        mov_rr32(j, reg, reg);
    else if (w < 32)
        bfm(j, 2, false, reg, reg, 0, w - 1);          /* ubfx reg, reg, #0, #w */
}

/* rd = the low `w` bits of rs, zero-extended to 64 bits (w = 64: a move). */
static void zext_rr(uam_jit *j, int rd, int rs, unsigned w)
{
    if (w == 32)
        mov_rr32(j, rd, rs);
    else if (w < 32)
        bfm(j, 2, false, rd, rs, 0, w - 1);            /* ubfx rd, rs, #0, #w */
    else if (rd != rs)
        mov_rr(j, rd, rs);
}

/* Branch if a boolean operand (temp or slot) is 0; returns the patch position. */
static size_t skip_false(uam_jit *j, opnd o)
{
    if (o.kind == O_REG)
        return cbz_fwd(j, false, o.reg);
    mem(j, 0, true, X17, TMP, o.disp);                 /* ldrb w17, [slot] */
    return cbz_fwd(j, false, X17);
}

/* ---- memory fast paths ---------------------------------------------------------- */

/* rd = [x1 + ra], `size` bytes, zero-extended; byte-swapped if big-endian. */
static void emit_fast_load(uam_jit *j, unsigned size, bool be, int rd, int ra)
{
    mem_rr(j, sz_of(size), true, rd, X1, ra, false);
    if (be && size > 1)
        dp1(j, size == 2 ? DP_REV16 : size == 4 ? DP_REV32 : DP_REV64, size == 8, rd, rd);
}

/* [x1 + ra] = rv (a register, or xzr), `size` bytes; byte-swapped (into x17) first if
 * big-endian. */
static void emit_fast_store(uam_jit *j, unsigned size, bool be, int rv, int ra)
{
    int v = rv;
    if (be && size > 1 && rv != XZR) {
        dp1(j, size == 2 ? DP_REV16 : size == 4 ? DP_REV32 : DP_REV64, size == 8, X17, rv);
        v = X17;
    }
    mem_rr(j, sz_of(size), false, v, X1, ra, false);
}

/* The probe reaches an entry's tag and base from one register, x16 = cpu + 8 * index,
 * with an immediate offset. */
#define DTLB_NEAR(f) (offsetof(uam_cpu, f) % 8 == 0 && offsetof(uam_cpu, f) / 8 < 0x1000)
_Static_assert(DTLB_NEAR(dtlb_rt) && DTLB_NEAR(dtlb_rb) && DTLB_NEAR(dtlb_wt) && DTLB_NEAR(dtlb_wb),
               "the data-TLB arrays must be within a load's immediate offset of the cpu");

/* Emit the data-TLB probe of the access at the address in register ra (x2, or a temp's
 * register: preserved). On a hit, x1 = host base (data at [x1 + ra]) and control falls
 * through; on a miss, branches are taken to be patched to the bridge path. `tag_off`/
 * `base_off` select the read or write set. Returns miss-branch positions in miss[] (up
 * to 2) and their count in *n_miss. Uses x0, x1, x16 and x17; ra, x3 and every register
 * that holds a temp are preserved.
 *
 * The entry is that of the first byte's page, and the tag compared with it the page of
 * the last byte: an access that crosses into the next page finds another tag there (an
 * entry's tag is a page of that entry's index, or none) and misses, with no test of its
 * own. An access that must be aligned (ACC_ALIGNED) cannot cross a page once it is
 * aligned, so that test is all it needs. */
static void emit_dtlb_probe(uam_jit *j, unsigned size, bool aligned, int32_t tag_off,
                            int32_t base_off, const opnd *addr, int ra, size_t *miss,
                            unsigned *n_miss)
{
    uint32_t f;
    *n_miss = 0;
    if (addr->kind == O_IMM) {
        /* A constant address: its entry, page and alignment are known, so the probe
         * is one compare with that entry's tag (the caller sends a page-crossing or
         * misaligned constant access straight to the bridge). */
        const uint64_t vpage = addr->imm >> UAM_PAGE_SHIFT;
        const int32_t e = 8 * (int32_t)(vpage & (UAM_DTLB_SIZE - 1));
        ld64(j, X17, CPUR, tag_off + e);
        mov_imm(j, X0, vpage);
        addsub_reg(j, true, true, true, XZR, X17, X0, 0);  /* cmp x17, x0 */
        miss[(*n_miss)++] = bcond_fwd(j, CC_NE);
        ld64(j, X1, CPUR, base_off + e);                   /* x1 = host base */
        return;
    }
    int last = ra;                                         /* the last byte's address */
    if (size > 1 && !aligned) {
        addsub_imm(j, false, true, false, X0, ra, size - 1, false);
        last = X0;
    }
    bfm(j, 2, true, X1, ra, UAM_PAGE_SHIFT, UAM_PAGE_SHIFT + UAM_DTLB_BITS - 1);   /* ubfx: x1 = index */
    addsub_reg(j, false, true, false, X16, CPUR, X1, 3);   /* x16 = cpu + 8 * index */
    ld64(j, X17, X16, tag_off);                            /* x17 = tag[index] */
    g32(j, 0xEB400000u | (uint32_t)last << 16 | UAM_PAGE_SHIFT << 10 | X17 << 5 | XZR);
                                                           /* cmp x17, last, lsr #12 */
    miss[(*n_miss)++] = bcond_fwd(j, CC_NE);
    if (size > 1 && aligned) {
        logimm(size - 1, false, &f);
        logic_imm(j, LG_ANDS, false, XZR, ra, f);          /* tst w<ra>, size-1 */
        miss[(*n_miss)++] = bcond_fwd(j, CC_NE);           /* misaligned */
    }
    ld64(j, X1, X16, base_off);                            /* x1 = base[index] */
}

/*
 * After a write-set miss (x2 = addr, x3 = value): the page may be one that holds
 * translated code (tag vpage | DTLB_CODE, see jit.c's dtlb_fill). Hit if so, and the store
 * neither crosses the page nor (ACC_ALIGNED) is misaligned, and none of its bytes is in
 * a 64-byte line holding code (the bus's mask, read now: a write there must go through
 * the bus, which drops that code). Then x1 = host base as after emit_dtlb_probe; else
 * branches are taken to be patched to the bridge path. Uses x0, x1, x16 and x17.
 */
static void emit_code_probe(uam_jit *j, unsigned size, bool aligned, size_t *miss,
                            unsigned *n_miss)
{
    uint32_t f;
    *n_miss = 0;
    lsr_imm(j, true, X0, X2, UAM_PAGE_SHIFT);              /* x0 = vpage */
    logimm(UAM_DTLB_SIZE - 1, true, &f);
    logic_imm(j, LG_AND, true, X1, X0, f);                 /* x1 = index */
    logimm(DTLB_CODE, true, &f);
    logic_imm(j, LG_ORR, true, X0, X0, f);                 /* x0 = vpage | DTLB_CODE */
    add_imm(j, X16, CPUR, (uint32_t)CPU(dtlb_wt));
    mem_rr(j, 3, true, X17, X16, X1, true);
    addsub_reg(j, true, true, true, XZR, X17, X0, 0);      /* cmp tag, x0 */
    miss[(*n_miss)++] = bcond_fwd(j, CC_NE);
    if (size > 1) {
        logimm(UAM_PAGE_SIZE - 1, false, &f);
        logic_imm(j, LG_AND, false, X17, X2, f);
        cmp_imm(j, false, X17, UAM_PAGE_SIZE - size);
        miss[(*n_miss)++] = bcond_fwd(j, CC_HI);           /* page cross */
        if (aligned) {
            logimm(size - 1, false, &f);
            logic_imm(j, LG_ANDS, false, XZR, X2, f);
            miss[(*n_miss)++] = bcond_fwd(j, CC_NE);       /* misaligned */
        }
    }
    add_imm(j, X16, JC, (uint32_t)OFF(clines));
    mem_rr(j, 3, true, X0, X16, X1, true);                 /* x0 = lines */
    ld64(j, X0, X0, 0);                                    /* x0 = the mask */
    /* the lines of the first and the last byte (lsrv takes the amount mod 64) */
    lsr_imm(j, true, X17, X2, 6);
    dp2(j, DP_LSRV, true, X17, X0, X17);
    miss[(*n_miss)++] = tbnz_fwd(j, X17, 0);               /* code */
    if (size > 1) {
        addsub_imm(j, false, true, false, X17, X2, size - 1, false);
        lsr_imm(j, true, X17, X17, 6);
        dp2(j, DP_LSRV, true, X17, X0, X17);
        miss[(*n_miss)++] = tbnz_fwd(j, X17, 0);           /* code */
    }
    add_imm(j, X16, CPUR, (uint32_t)CPU(dtlb_wb));
    mem_rr(j, 3, true, X1, X16, X1, true);                 /* x1 = base */
}

/* Which code-page store stub serves a store of `size` bytes. */
static unsigned code_stub(unsigned size, bool be, bool aligned)
{
    unsigned s = size == 1 ? 0 : size == 2 ? 1 : size == 4 ? 2 : 3;
    return s << 2 | (unsigned)be << 1 | (unsigned)aligned;
}

/*
 * The stores to pages that hold code, shared by all blocks (called from a store's miss
 * path, so a store site costs a call and a branch): emit_code_probe, then the store.
 * Return w0 = 0 if the store was done, 1 if it must take the bridge (x2 and x3
 * unchanged). One stub per size, byte order and alignment rule, placed at the start of
 * the arena by uam_jit_create.
 */
void jit_host_stubs(uam_jit *j, size_t off[16])
{
    for (unsigned v = 0; v < 16; v++) {
        const unsigned size = 1u << (v >> 2);
        const bool be = v & 2, aligned = v & 1;
        while (j->n & 15)
            g32(j, A64_NOP);
        off[v] = j->n;
        size_t miss[5];
        unsigned n_miss;
        emit_code_probe(j, size, aligned, miss, &n_miss);
        emit_fast_store(j, size, be, X3, X2);
        mov_imm(j, X0, 0);
        g32(j, A64_RET);
        for (unsigned k = 0; k < n_miss; k++)
            patch(j, miss[k]);
        mov_imm(j, X0, 1);
        g32(j, A64_RET);
    }
}

/* cpu->icount = x23 + jc->ic_base: before a bridge (it may read time) and at the
 * epilogue. Clobbers x17. */
static void ic_flush(uam_jit *j)
{
    ld64(j, X17, JC, OFF(ic_base));
    addsub_reg(j, false, true, false, X17, X17, IC, 0);
    st64(j, X17, CPUR, CPU(icount));
}
/* After a bridge, which may have asked the block to stop (uam_cpu_request_exit): take
 * cpu->icount_stop as the new jc->ic_base, x23 following. Clobbers x16. */
static void ic_rebase(uam_jit *j)
{
    ld64(j, X16, JC, OFF(ic_base));
    addsub_reg(j, false, true, false, IC, IC, X16, 0);
    ld64(j, X16, CPUR, CPU(icount_stop));
    st64(j, X16, JC, OFF(ic_base));
    addsub_reg(j, true, true, false, IC, IC, X16, 0);
}

/* Before a bridge call that may fault: jc->site = the current instruction's site
 * record (one per instruction, emitted after the code). Clobbers x0. */
static void emit_site(uam_jit *j)
{
    if (!jit_site_ref(j, adr_fwd(j, X0)))                 /* adr x0, site */
        return;
    st64(j, X0, JC, OFF(site));
}

/*
 * UAM_OP_COV A, B (docs/uam-isa.md section 7.5; uam_cov_count in cpu.h is the
 * reference), inline, as on x86-64 (jit_x86.c, emit_cov): cpu->cov is read at run time
 * (NULL: nothing), its history shape is compiled in. The map byte saturates at 255.
 * Clobbers x0, x1, x2, x16 and x17 (scratch between ops).
 */
static void emit_cov(uam_jit *j, uint64_t pc, uint32_t a, uint32_t b)
{
    const uam_cov_state *cs = j->x->cpu->cov;
    const uint32_t d = cs && cs->depth > 1 ? cs->depth : 1;
    ld64(j, X1, CPUR, CPU(cov));                           /* x1 = cpu->cov */
    size_t off = j->n;
    g32(j, 0xB4000000u | X1);                              /* cbz x1, skip */
    size_t rep = 0;
    if (cs && cs->skip_repeat) {
        mov_imm(j, X0, pc);
        ld64(j, X16, X1, COV(last_pc));
        addsub_reg(j, true, true, true, XZR, X16, X0, 0);  /* cmp last_pc, pc */
        rep = bcond_fwd(j, CC_EQ);                         /* the same site again */
        st64(j, X0, X1, COV(last_pc));
    }
    if (d == 1 && !(cs && cs->rot[0])) {
        mem(j, 2, true, X0, X1, COV(prev));                /* w0 = prev */
        mov_imm(j, X16, a);
        logic_reg(j, LG_EOR, false, X0, X0, X16);          /* w0 ^= A */
    } else {
        mov_imm(j, X0, a);
        for (uint32_t k = 0; k < d; k++) {
            unsigned r = cs->rot[k];
            mem(j, 2, true, X2, X1, COV(hist) + 4 * (int32_t)k);   /* w2 = hist[k] */
            if (r) {                                       /* rotl within bits, & mask */
                lsr_imm(j, false, X16, X2, cs->bits - r);
                lsl_imm(j, false, X2, X2, r);
                logic_reg(j, LG_ORR, false, X2, X2, X16);
                mem(j, 2, true, X16, X1, COV(mask));
                logic_reg(j, LG_AND, false, X2, X2, X16);
            }
            logic_reg(j, LG_EOR, false, X0, X0, X2);
        }
    }
    mem(j, 2, true, X16, X1, COV(mask));
    logic_reg(j, LG_AND, false, X0, X0, X16);              /* w0 &= mask (zero-extends) */
    ld64(j, X2, X1, COV(map));                             /* x2 = map */
    mem_rr(j, 0, true, X16, X2, X0, false);                /* w16 = map[w0] */
    cmp_imm(j, false, X16, 255);
    csel(j, false, true, false, X16, X16, X16, CC_EQ);     /* cinc w16, w16, ne: saturate */
    mem_rr(j, 0, false, X16, X2, X0, false);
    for (uint32_t k = d - 1; k > 0; k--) {                 /* hist[k] = hist[k-1] */
        mem(j, 2, true, X16, X1, COV(hist) + 4 * (int32_t)(k - 1));
        mem(j, 2, false, X16, X1, COV(hist) + 4 * (int32_t)k);
    }
    mov_imm(j, X16, b);
    mem(j, 2, false, X16, X1, COV(prev));                  /* hist[0] = B */
    patch(j, off);
    if (rep)
        patch(j, rep);
}

/* Store a block result into jc (the caller then goes to the epilogue). */
static void set_res_pc(uam_jit *j, uint32_t kind, const uam_block *blk, uam_ref target, unsigned aw)
{
    load_operand(j, X0, blk, target, aw);
    st64(j, X0, JC, RES(pc));
    mov_imm(j, X0, kind);
    st32(j, X0, JC, RES(kind));
}
static void set_res_pc_imm(uam_jit *j, uint32_t kind, uint64_t pc)
{
    mov_imm(j, X0, pc);
    st64(j, X0, JC, RES(pc));
    mov_imm(j, X0, kind);
    st32(j, X0, JC, RES(kind));
}

/* chain_left-- (w17 holds it): the hop is taken. */
static void chain_hop(uam_jit *j)
{
    addsub_imm(j, true, false, false, X17, X17, 1, false);
    st32(j, X17, JC, OFF(chain_left));
}
/* Jump to the body at [x0 + entry]. */
static void chain_jump(uam_jit *j)
{
    chain_hop(j);
    ld64(j, X16, X0, SLOT(entry));
    g32(j, 0xD61F0000u | X16 << 5);                        /* br x16 */
}
/* ldr x<rt>, <a field of the newest link slot> (pc-relative, patched at the end) */
static void ld_slot(uam_jit *j, int rt, unsigned off)
{
    jit_slot_ref(j, j->n, off);
    g32(j, 0x58000000u | (uint32_t)rt);
}

/*
 * An EXIT to `target` (its pc operand): jump straight into the next block if its link is
 * current and the run loop has nothing to do (jit.h); else store the block's result,
 * res = (NEXT, pc), and go to the epilogue, as on x86-64 (jit_x86.c, emit_chain). Only
 * those ways out store the result. A miss also records the slot (or the jump table
 * entry) in jc->link_slot so uam_jit_run can link it. A constant target's path reads the
 * slot's two fields pc-relative; a computed one keeps the pc in x1.
 */
static void emit_chain(uam_jit *j, const uam_block *blk, uam_ref target, unsigned aw,
                       size_t *epi, unsigned *n_epi)
{
    const bool fixed = uam_ref_is_imm(target);
    const uint64_t pc = fixed ? blk->imm[uam_ref_index(target)] & uam_mask(blk->addr_width) : LINK_NONE;
    size_t miss[4];
    unsigned n_miss = 0;
    j->slot_pc[j->n_slots++] = pc;
    if (!fixed)
        load_operand(j, X1, blk, target, aw);              /* x1 = the target pc */
    /* if (icount >= icount_stop (exit request or deadline) || !chain_left) goto out */
    const size_t stop = tbz_fwd(j, IC, 63);                /* x23 is not negative */
    mem(j, 2, true, X17, JC, OFF(chain_left));
    const size_t spent = cbz_fwd(j, false, X17);
    if (fixed) {
        ld64(j, X0, JC, OFF(epoch));
        ld_slot(j, X16, SLOT(epoch));
        addsub_reg(j, true, true, true, XZR, X0, X16, 0);
        miss[n_miss++] = bcond_fwd(j, CC_NE);
        chain_hop(j);
        ld_slot(j, X16, SLOT(entry));
        g32(j, 0xD61F0000u | X16 << 5);                    /* br x16 */
        patch(j, miss[0]);
        jit_slot_ref(j, adr_fwd(j, X0), 0);                /* x0 = &slot */
    } else {
        jit_slot_ref(j, adr_fwd(j, X0), 0);                /* x0 = &slot */
        ld64(j, X2, JC, OFF(epoch));
        ld64(j, X16, X0, SLOT(pc));
        addsub_reg(j, true, true, true, XZR, X1, X16, 0);  /* cmp pc, slot.pc */
        size_t other = bcond_fwd(j, CC_NE);
        ld64(j, X16, X0, SLOT(epoch));
        addsub_reg(j, true, true, true, XZR, X2, X16, 0);
        miss[n_miss++] = bcond_fwd(j, CC_NE);
        chain_jump(j);
        /* Another target: if the slot is unused this epoch, link it there. */
        patch(j, other);
        ld64(j, X16, X0, SLOT(epoch));
        addsub_reg(j, true, true, true, XZR, X2, X16, 0);
        miss[n_miss++] = bcond_fwd(j, CC_NE);
        /* x0 = &jtab[(uint32_t)(pc * JTAB_HASH) >> (32 - bits)] */
        mov_imm(j, X16, JTAB_HASH);
        madd(j, false, false, X16, X1, X16, XZR);          /* w16 = w1 * HASH */
        lsr_imm(j, false, X16, X16, 32 - j->jtab_bits);
        addsub_reg(j, false, true, false, X16, X16, X16, 1);   /* x16 *= 3 */
        add_imm(j, X0, JC, (uint32_t)OFF(jtab));
        addsub_reg(j, false, true, false, X0, X0, X16, 3);     /* x0 += x16 * 8 */
        ld64(j, X16, X0, SLOT(pc));
        addsub_reg(j, true, true, true, XZR, X1, X16, 0);
        miss[n_miss++] = bcond_fwd(j, CC_NE);
        ld64(j, X16, X0, SLOT(epoch));
        addsub_reg(j, true, true, true, XZR, X2, X16, 0);
        miss[n_miss++] = bcond_fwd(j, CC_NE);
        chain_jump(j);
        for (unsigned k = 0; k < n_miss; k++)
            patch(j, miss[k]);
    }
    st64(j, X0, JC, OFF(link_slot));                       /* a miss: the slot or the entry */
    patch(j, stop);
    patch(j, spent);
    /* out: res = (NEXT, pc) */
    if (fixed) {
        set_res_pc_imm(j, UAM_BR_NEXT, pc);
    } else {
        st64(j, X1, JC, RES(pc));
        mov_imm(j, X0, UAM_BR_NEXT);
        st32(j, X0, JC, RES(kind));
    }
    epi[(*n_epi)++] = b_fwd(j);
}

/* Every pure op has an inline template here; POPCNT (through the vector unit) can be
 * turned off with UAM_JIT_BASELINE, to test the evaluator fallback. */
bool jit_host_pure_inline(const uam_jit *j, uam_opcode op)
{
    switch (op) {
    case UAM_OP_POPCNT:
        return j->popcnt;
    case UAM_OP_COV: case UAM_OP__COUNT:
        return false;
    default:
        return true;
    }
}

static bool op_supported(uam_opcode op)
{
    switch (op) {
    case UAM_OP_IMARK: case UAM_OP_GET: case UAM_OP_PUT: case UAM_OP_LD: case UAM_OP_ST:
    case UAM_OP_EXIT: case UAM_OP_COV:
        return true;
    case UAM_OP_SYSRD: case UAM_OP_SYSWR: case UAM_OP_TRAP: case UAM_OP_WAIT:
    case UAM_OP_HELPER: case UAM_OP_AMO: case UAM_OP_CAS: case UAM_OP_FENCE:
        return true;
    default:
        break;
    }
    /* pure ops (inline or via jit_br_eval) */
    const uam_opinfo *oi = &uam_opinfo_table[op];
    return !(oi->props & (UAM_P_EFFECT | UAM_P_GUARD)) && op != UAM_OP_GET;
}

/* Set the flags as cmp a, b at width W (a in a register, b any operand). */
static void cmp_op(uam_jit *j, unsigned W, int ra, opnd b)
{
    const bool sf = W == 64;
    if (b.kind == O_IMM)
        cmp_imm(j, sf, ra, b.imm);
    else
        addsub_reg(j, true, sf, true, XZR, ra, src(j, b, X1, W), 0);
}

/* The register of the pool an operand is in, or -1. */
static int reg_of_ref(const uam_jit *j, uam_ref r)
{
    return uam_ref_is_none(r) || uam_ref_is_imm(r) ? -1 : j->reg_of[r];
}

/* Emit a pure op; its W-bit result is left in rd: x0, or a register of the pool, which
 * may be that of an operand read here for the last time (jit_ra_dst) - so every template
 * writes rd only in or after the instruction that reads its operands last, and keeps
 * what it computes before that in x0, x1, x16 and x17. A fused compare (j->fuse) leaves
 * only the flags. */
static void emit_pure(uam_jit *j, const uam_block *blk, const uam_insn *in, int rd)
{
    const uam_opcode op = (uam_opcode)in->op;
    const unsigned W = in->width;
    const unsigned n_src = uam_opinfo_table[op].n_src;
    unsigned src_w = W;
    if (op == UAM_OP_ZEXT || op == UAM_OP_SEXT || op == UAM_OP_TRUNC)
        src_w = uam_ref_is_imm(in->a) ? W : blk->temp_width[in->a];

    if (!jit_host_pure_inline(j, op)) {
        /* x0 = jit_br_eval(jc, packed, a, b, c) */
        jit_ra_save(j, false);
        load_operand(j, X2, blk, in->a, op == UAM_OP_SELECT ? 1 : src_w);
        if (n_src > 1) load_operand(j, X3, blk, in->b, W); else mov_imm(j, X3, 0);
        if (n_src > 2) load_operand(j, X4, blk, in->c, W); else mov_imm(j, X4, 0);
        mov_rr(j, X0, JC);
        mov_imm(j, X1, (uint32_t)op | (W << 8) | (src_w << 16));
        call_bridge(j, jit_br_eval);
        jit_ra_save(j, true);
        zext_rr(j, rd, X0, W);
        return;
    }

    /* Natural operand size: 32-bit operations zero-extend their result, so only
     * narrower widths need masking after an add, sub or multiply. AND/OR/XOR of
     * zero-extended values never need it. */
    const bool sf = W == 64;
    switch (op) {
    case UAM_OP_ADD: case UAM_OP_SUB: {
        const int ra = src(j, opnd_of(j, blk, in->a, W), X0, W);
        const opnd b = opnd_of(j, blk, in->b, W);
        bool sub = op == UAM_OP_SUB;
        if (b.kind == O_IMM) {
            const uint64_t m = sf ? ~UINT64_C(0) : 0xFFFFFFFFu;
            const uint64_t nv = (0 - b.imm) & m;
            if (b.imm < 0x1000) {
                addsub_imm(j, sub, sf, false, rd, ra, (uint32_t)b.imm, false);
            } else if (nv < 0x1000) {
                addsub_imm(j, !sub, sf, false, rd, ra, (uint32_t)nv, false);
            } else {
                mov_imm(j, X1, b.imm);
                addsub_reg(j, sub, sf, false, rd, ra, X1, 0);
            }
        } else {
            addsub_reg(j, sub, sf, false, rd, ra, src(j, b, X1, W), 0);
        }
        if (W < 32)
            mask_reg(j, rd, W);
        break;
    }
    case UAM_OP_AND: case UAM_OP_OR: case UAM_OP_XOR: {
        const int opc = op == UAM_OP_AND ? LG_AND : op == UAM_OP_OR ? LG_ORR : LG_EOR;
        const int ra = src(j, opnd_of(j, blk, in->a, W), X0, W);
        const opnd b = opnd_of(j, blk, in->b, W);
        uint32_t f;
        if (b.kind == O_IMM && logimm(b.imm, sf, &f))
            logic_imm(j, opc, sf, rd, ra, f);
        else
            logic_reg(j, opc, sf, rd, ra, src(j, b, X1, W));
        break;
    }
    case UAM_OP_MUL: {
        const int ra = src(j, opnd_of(j, blk, in->a, W), X0, W);
        madd(j, false, sf, rd, ra, src(j, opnd_of(j, blk, in->b, W), X1, W), XZR);
        if (W < 32)
            mask_reg(j, rd, W);
        break;
    }
    case UAM_OP_EQ: case UAM_OP_NE: case UAM_OP_ULT: case UAM_OP_ULE:
    case UAM_OP_SLT: case UAM_OP_SLE: {
        /* Zero-extended narrow values compare as 32-bit ones, except signed: then both
         * sides are sign-extended (the immediate here, a register by the compare). */
        const opnd b = opnd_of(j, blk, in->b, W);
        if ((op == UAM_OP_SLT || op == UAM_OP_SLE) && W < 32) {
            bfm(j, 0, false, X0, src(j, opnd_of(j, blk, in->a, W), X0, W), 0, W - 1);  /* sxt w0 */
            if (b.kind == O_IMM)
                cmp_imm(j, false, X0, (uint32_t)uam_sext(b.imm, W));
            else
                cmp_sext(j, X0, src(j, b, X1, W), W);
        } else {
            cmp_op(j, W, src(j, opnd_of(j, blk, in->a, W), X0, W), b);
        }
        const uint8_t cc = op == UAM_OP_EQ ? CC_EQ : op == UAM_OP_NE ? CC_NE : op == UAM_OP_ULT ? CC_LO
                         : op == UAM_OP_ULE ? CC_LS : op == UAM_OP_SLT ? CC_LT : CC_LE;
        if (j->fuse) {
            j->flag_cc = cc;                                       /* the reader tests the flags */
            break;
        }
        cset(j, rd, cc);
        break;
    }
    case UAM_OP_ZEXT:   /* the source temp is already zero-extended */
        load_operand(j, rd, blk, in->a, src_w);
        break;
    case UAM_OP_TRUNC:
        zext_rr(j, rd, src(j, opnd_of(j, blk, in->a, src_w), X0, src_w), W);
        break;
    case UAM_OP_SEXT:
        sbfx(j, rd, src(j, opnd_of(j, blk, in->a, src_w), X0, src_w), 0, src_w);
        if (W < 64)
            mask_reg(j, rd, W);
        break;
    case UAM_OP_SELECT: {
        const opnd c = opnd_of(j, blk, in->a, 1);
        const opnd x = opnd_of(j, blk, in->b, W);
        const opnd y = opnd_of(j, blk, in->c, W);
        if (c.kind == O_IMM) {
            load(j, rd, c.imm ? x : y, W);
            break;
        }
        unsigned cc = CC_NE;                                     /* cond != 0 */
        if (in->a == j->flag_temp) {
            cc = j->flag_cc;                                     /* cond is in the flags */
            j->flag_temp = UAM_REF_NONE;
        } else {
            cmp_imm(j, false, src(j, c, X17, 1), 0);
        }
        const int rx = src(j, x, X1, W), ry = src(j, y, X2, W);  /* loads leave the flags */
        csel(j, false, false, true, rd, rx, ry, cc);
        break;
    }
    case UAM_OP_SHL: case UAM_OP_LSHR: case UAM_OP_ASHR: {
        if (!uam_ref_is_imm(in->b)) {
            /* Variable amount y (a W-bit value, so possibly >= 64): shift at 64 bits
             * (the host uses y mod 64), then fix up y >= W. For SHL/LSHR an amount in
             * [W, 64) already gives 0 in the low W bits; beyond, select 0. ASHR clamps
             * the amount to 63, which fills with the sign for any y >= W - 1. */
            const int ra = src(j, opnd_of(j, blk, in->a, W), X0, W);
            const int rb = src(j, opnd_of(j, blk, in->b, W), X1, W);
            if (op == UAM_OP_ASHR) {
                if (W < 64) sbfx(j, X0, ra, 0, W); else mov_rr(j, X0, ra);
                cmp_imm(j, true, rb, 63);
                mov_imm(j, X17, 63);
                csel(j, false, false, true, X17, X17, rb, CC_HI);   /* x17 = min(y, 63) */
                dp2(j, DP_ASRV, true, rd, X0, X17);
            } else {
                dp2(j, op == UAM_OP_SHL ? DP_LSLV : DP_LSRV, true, X0, ra, rb);
                cmp_imm(j, true, rb, W);
                csel(j, false, false, true, rd, XZR, X0, CC_HS);    /* y >= W: 0 */
            }
            mask_reg(j, rd, W);
            break;
        }
        uint64_t n = blk->imm[uam_ref_index(in->b)];   /* any amount >= W shifts everything out */
        if (n >= W) {
            if (op == UAM_OP_ASHR) {
                sbfx(j, rd, src(j, opnd_of(j, blk, in->a, W), X0, W), W - 1, 1);  /* the sign */
                mask_reg(j, rd, W);
            } else {
                mov_imm(j, rd, 0);
            }
            break;
        }
        const int ra = src(j, opnd_of(j, blk, in->a, W), X0, W);
        if (op == UAM_OP_SHL) {
            lsl_imm(j, sf, rd, ra, (unsigned)n);
            if (W < 32)
                mask_reg(j, rd, W);
        } else if (op == UAM_OP_LSHR) {
            lsr_imm(j, sf, rd, ra, (unsigned)n);           /* zero-extended: nothing to mask */
        } else if (W >= 32) {
            asr_imm(j, sf, rd, ra, (unsigned)n);           /* asr w zero-extends the result */
        } else {
            sbfx(j, rd, ra, (unsigned)n, W - (unsigned)n); /* bits [n, W), sign-extended */
            mask_reg(j, rd, W);
        }
        break;
    }
    case UAM_OP_CARRY: case UAM_OP_SCARRY: case UAM_OP_SBORROW: {
        /* The add/sub sets C and V exactly as section 7.2 defines carry and signed
         * overflow; narrow operands are moved to the top of 32 bits first, so that the
         * flags are those of the W-bit operation. */
        const bool sub = op == UAM_OP_SBORROW;
        const int ra = src(j, opnd_of(j, blk, in->a, W), X0, W);
        const int rb = src(j, opnd_of(j, blk, in->b, W), X1, W);
        if (W >= 32) {
            addsub_reg(j, sub, sf, true, XZR, ra, rb, 0);
        } else {
            lsl_imm(j, false, X0, ra, 32 - W);
            addsub_reg(j, sub, false, true, XZR, X0, rb, 32 - W);
        }
        const uint8_t cc = op == UAM_OP_CARRY ? CC_HS : CC_VS;   /* c / v */
        if (j->fuse) {
            j->flag_cc = cc;
            break;
        }
        cset(j, rd, cc);
        break;
    }
    case UAM_OP_POPCNT:
        g32(j, 0x9E670000u | (uint32_t)src(j, opnd_of(j, blk, in->a, W), X0, W) << 5);  /* fmov d0, x */
        g32(j, 0x0E205800u);                               /* cnt v0.8b, v0.8b */
        g32(j, 0x0E31B800u);                               /* addv b0, v0.8b */
        g32(j, 0x0E013C00u | (uint32_t)rd);                /* umov w<rd>, v0.b[0] */
        break;
    case UAM_OP_CLZ: {
        const int ra = src(j, opnd_of(j, blk, in->a, W), X0, W);
        dp1(j, DP_CLZ, sf, rd, ra);
        if (W < 32)                                        /* counted at 32 bits */
            addsub_imm(j, true, false, false, rd, rd, 32 - W, false);
        break;
    }
    case UAM_OP_CTZ: {
        int ra = src(j, opnd_of(j, blk, in->a, W), X0, W);
        if (W < 64) {                                      /* bit W set: ctz(0) = W */
            uint32_t f;
            logimm(UINT64_C(1) << W, true, &f);
            logic_imm(j, LG_ORR, true, X0, ra, f);
            ra = X0;
        }
        dp1(j, DP_RBIT, true, X0, ra);
        dp1(j, DP_CLZ, true, rd, X0);
        break;
    }
    case UAM_OP_BSWAP: {
        const int ra = src(j, opnd_of(j, blk, in->a, W), X0, W);
        dp1(j, W == 16 ? DP_REV16 : W == 32 ? DP_REV32 : DP_REV64, sf, rd, ra);
        break;
    }
    case UAM_OP_ROTR: {
        /* the amount mod W, as section 7.1 defines */
        const bool imm = uam_ref_is_imm(in->b);
        const unsigned k = imm ? (unsigned)(blk->imm[uam_ref_index(in->b)] % W) : 0;
        if (W >= 32) {
            const int ra = src(j, opnd_of(j, blk, in->a, W), X0, W);
            if (imm)
                extr(j, sf, rd, ra, ra, k);
            else
                dp2(j, DP_RORV, sf, rd, ra, src(j, opnd_of(j, blk, in->b, W), X1, W));
            break;
        }
        if (imm && !k) {
            load_operand(j, rd, blk, in->a, W);
            break;
        }
        load_operand(j, X0, blk, in->a, W);
        if (imm) {
            lsr_imm(j, false, X17, X0, k);
            lsl_imm(j, false, X0, X0, W - k);
        } else {
            uint32_t f;
            logimm(W - 1, false, &f);
            logic_imm(j, LG_AND, false, X17, src(j, opnd_of(j, blk, in->b, W), X1, W), f);
            dp2(j, DP_LSRV, false, X16, X0, X17);          /* x >> k */
            mov_imm(j, X1, W);
            addsub_reg(j, true, false, false, X17, X1, X17, 0);   /* W - k, in 1..W */
            dp2(j, DP_LSLV, false, X0, X0, X17);           /* x << (W - k) */
            mov_rr32(j, X17, X16);
        }
        logic_reg(j, LG_ORR, false, X0, X0, X17);
        zext_rr(j, rd, X0, W);
        break;
    }
    case UAM_OP_MULHU: case UAM_OP_MULHS: {
        const bool sgn = op == UAM_OP_MULHS;
        const int ra = src(j, opnd_of(j, blk, in->a, W), X0, W);
        const int rb = src(j, opnd_of(j, blk, in->b, W), X1, W);
        if (W == 64) {
            mulh(j, sgn, rd, ra, rb);
            break;
        }
        /* the 2W-bit product fits in 64 bits */
        if (sgn) {
            sbfx(j, X0, ra, 0, W);
            sbfx(j, X1, rb, 0, W);
            madd(j, false, true, X0, X0, X1, XZR);
            asr_imm(j, true, X0, X0, W);
        } else {
            madd(j, false, true, X0, ra, rb, XZR);
            lsr_imm(j, true, X0, X0, W);
        }
        zext_rr(j, rd, X0, W);
        break;
    }
    case UAM_OP_UDIV: case UAM_OP_UREM: case UAM_OP_SDIV: case UAM_OP_SREM: {
        /* Section 7.1: x / 0 = all ones, x % 0 = x; SMIN / -1 = SMIN, SMIN % -1 = 0.
         * Divide at 64 bits (signed ops sign-extend first), where the host gives x / 0 = 0
         * (fixed up for the quotient; x - 0 * 0 is the remainder) and SMIN / -1 = SMIN
         * without a trap. */
        const bool sgn = op == UAM_OP_SDIV || op == UAM_OP_SREM;
        const bool rem = op == UAM_OP_UREM || op == UAM_OP_SREM;
        const int ra = src(j, opnd_of(j, blk, in->a, W), X0, W);
        const int rb = src(j, opnd_of(j, blk, in->b, W), X1, W);
        if (sgn && W < 64) {
            sbfx(j, X0, ra, 0, W);
            sbfx(j, X1, rb, 0, W);
        } else {
            if (ra != X0) mov_rr(j, X0, ra);
            if (rb != X1) mov_rr(j, X1, rb);
        }
        dp2(j, sgn ? DP_SDIV : DP_UDIV, true, X16, X0, X1);
        if (rem) {
            madd(j, true, true, X0, X16, X1, X0);          /* x0 - q * y */
        } else {
            cmp_imm(j, true, X1, 0);
            csel(j, true, false, true, X0, X16, XZR, CC_NE);   /* y == 0: all ones */
        }
        zext_rr(j, rd, X0, W);
        break;
    }
    default:
        break;
    }
}

/* [x22 + aux] = v (width w) */
static void emit_put(uam_jit *j, uint32_t aux, unsigned w, opnd v)
{
    int rt;
    if (v.kind == O_IMM && v.imm == 0)
        rt = XZR;
    else if (v.kind == O_REG)
        rt = v.reg;
    else {
        load(j, X0, v, w);
        rt = X0;
    }
    mem(j, sz_of(w / 8), false, rt, RF, (int32_t)aux);
}

/* Replay, here, what soft point i needs: compute its cold ops into their slots (in
 * program order, so each finds its cold operands there), then do its pending writes,
 * each under its guard. The temps it reads are where opnd_of says (an IMARK stub sets
 * that up). Uses x0, x1, x2, x16, x17 and the flags (not x3); returns whether x1 and x2
 * were touched. `back` is the region kind to resume in the op map. */
static bool emit_replays(uam_jit *j, const uam_block *blk, uint32_t i, unsigned back)
{
    if (j->obs_start[i] == j->obs_start[i + 1])
        return false;
    jit_mark(j, MK_REPLAY, i);
    const bool fuse = j->fuse;
    const uam_ref ft = j->flag_temp;
    j->fuse = false;                   /* every result to x0, nothing held in the flags */
    j->flag_temp = UAM_REF_NONE;
    for (uint32_t e = j->clo_start[i]; e < j->clo_start[i + 1]; e++) {
        const uam_insn *c = &blk->insn[j->clo_list[e]];
        emit_pure(j, blk, c, X0);
        st_temp(j, c->dst, X0);
    }
    j->fuse = fuse;
    j->flag_temp = ft;
    for (uint32_t e = j->obs_start[i]; e < j->obs_start[i + 1]; e++) {
        const uam_insn *d = &blk->insn[j->obs_list[e]];
        size_t skip = 0;
        if (jit_guard_temp(d))
            skip = skip_false(j, opnd_of(j, blk, d->guard, 1));
        emit_put(j, d->aux, d->width, opnd_of(j, blk, d->a, d->width));
        if (skip)
            patch(j, skip);
    }
    jit_mark(j, back, i);
    return j->clo_start[i] != j->clo_start[i + 1];
}

/* Before a bridge call: flush icount (the bridge may read time), the fault site, the
 * call-clobbered temps. */
static void bridge_enter(uam_jit *j)
{
    ic_flush(j);
    emit_site(j);
    jit_ra_save(j, false);
}

/* After a bridge returning int: to the epilogue on a fault, then the temps back. */
static void bridge_leave(uam_jit *j, size_t *epi, unsigned *n_epi)
{
    epi[(*n_epi)++] = cbz_fwd(j, true, X0);               /* cbnz w0: fault */
    ic_rebase(j);
    jit_ra_save(j, true);
}

bool jit_host_compile(uam_jit *j, const uam_block *blk)
{
    const unsigned aw = blk->addr_width;
    for (uint32_t i = 0; i < blk->n_insn; i++) {
        uam_opcode op = (uam_opcode)blk->insn[i].op;
        if (!op_supported(op))
            return false;
    }

    /* prologue: the frame every block shares */
    jit_mark(j, MK_PROLOGUE, 0);
    stp_ldp(j, false, PAIR_PRE, X29, X30, -12);            /* stp x29, x30, [sp, #-96]! */
    addsub_imm(j, false, true, false, X29, SP, 0, false);  /* mov x29, sp */
    for (int r = 0; r < 5; r++)
        stp_ldp(j, false, PAIR_OFF, X19 + 2 * r, X20 + 2 * r, 2 + 2 * r);
    mov_rr(j, JC, X0);                                     /* x19 = jc */
    ld64(j, TMP, JC, OFF(temps));
    ld64(j, CPUR, JC, OFF(cpu));
    ld64(j, RF, JC, OFF(regfile));
    ld64(j, IC, CPUR, CPU(icount));                        /* x23 = icount - ic_base (kept in a register) */
    ld64(j, X17, JC, OFF(ic_base));
    addsub_reg(j, true, true, false, IC, IC, X17, 0);
    j->body_off = j->n;                                    /* chained jumps enter here */

    /* epilogue branches: at most 1 per op */
    size_t *epi = malloc(sizeof(size_t) * (blk->n_insn + 1));
    if (!epi || !jit_defer_analyze(j, blk) || !jit_ra_begin(j, blk)) {
        free(epi);
        return false;
    }
    unsigned n_epi = 0;
    bool first = true;
    uint64_t imark_pc = blk->guest_pc;   /* the pc of the instruction being emitted */

    for (uint32_t i = 0; i < blk->n_insn && !j->overflow; i++) {
        const uam_insn *in = &blk->insn[i];
        const uam_opcode op = (uam_opcode)in->op;
        const unsigned W = in->width;
        const bool has_dst = !uam_ref_is_none(in->dst);
        j->cur_i = i;
        if (j->dfr[i] || j->cold[i])
            continue;                          /* replayed on the cold paths, if at all */
        j->fuse = jit_fusable(j, blk, i);
        jit_mark(j, MK_OP, i);
        /* where the result goes: picked now, as it may spill a register; an operand's
         * own register, if it is free for it, keeps a ZEXT from moving anything */
        int rd = X0;
        if (has_dst && !j->fuse)
            rd = jit_ra_dst(j, in->dst, reg_of_ref(j, in->a), reg_of_ref(j, in->b));

        /* Guard on an effectful op. */
        size_t guard_skip = 0;
        bool guarded = false;
        if (!uam_ref_is_none(in->guard)) {
            if (uam_ref_is_imm(in->guard)) {
                if (!(blk->imm[uam_ref_index(in->guard)] & 1)) {
                    jit_ra_free_dead(j, blk);  /* guard false: op does nothing, dst = 0 */
                    if (has_dst) {
                        mov_imm(j, rd, 0);
                        jit_ra_def(j, in->dst, rd);
                    }
                    continue;
                }
                /* guard true: fall through unguarded */
            } else {
                if (in->guard == j->flag_temp) {  /* the guard is in the flags */
                    guard_skip = bcond_fwd(j, j->flag_cc ^ 1u);
                    j->flag_temp = UAM_REF_NONE;
                } else {
                    guard_skip = skip_false(j, opnd_of(j, blk, in->guard, 1));
                }
                guarded = true;
            }
        }

        switch (op) {
        case UAM_OP_IMARK: {
            uint64_t pc = blk->imm[uam_ref_index(in->a)];
            bool cont = in->flags & UAM_IMARK_CONT;
            imark_pc = pc;
            const bool brk = in->flags & UAM_IMARK_BREAK;
            const bool check = !first && !cont && !brk;
            if (!first && !cont && brk) {
                set_res_pc_imm(j, UAM_BR_INTERRUPTED, pc);
                epi[n_epi++] = b_fwd(j);
            }
            first = false;
            /* A continuation keeps the parent of the instruction it continues. */
            j->cur.parent_pc = cont ? j->cur.parent_pc : pc;
            j->cur.pc = pc;
            j->cur.cont = cont;
            j->cur_emitted = false;
            jit_mark(j, MK_IMARK_PC, i);
            /* icount++; and if (icount >= icount_stop) stop here, before this instruction:
             * x23 was not negative, so it is positive now. An out-of-line stub, which
             * takes the count back and replays the deferred writes still pending. Without
             * the check the add keeps the flags (a CONT imark). */
            addsub_imm(j, false, true, check, IC, IC, 1, false);
            if (check)
                jit_stub_add(j, blk, bcond_fwd(j, CC_GT), pc, i);
            break;
        }
        case UAM_OP_GET:
            if (W == 1)
                mov_imm(j, rd, 0);
            else
                mem(j, sz_of(W / 8), true, rd, RF, (int32_t)in->aux);
            break;
        case UAM_OP_PUT:
            emit_put(j, in->aux, W, opnd_of(j, blk, in->a, W));
            break;
        case UAM_OP_LD: {
            unsigned size = W / 8;
            bool be = in->flags & UAM_ACC_BE;
            bool fast = !(in->flags & (UAM_ACC_X | UAM_ACC_PHYS | UAM_ACC_UNPRIV));
            const opnd ao = opnd_of(j, blk, in->a, aw);
            fast = fast && !(ao.kind == O_IMM && jit_const_slow(ao.imm, size, in->flags));
            /* the address: in its own register on the inline path, in x2 for the bridge */
            const int ra = fast && ao.kind == O_REG ? ao.reg : X2;
            if (ra == X2)
                load(j, X2, ao, aw);
            size_t done = 0, miss[2]; unsigned n_miss = 0;
            if (fast) {
                emit_dtlb_probe(j, size, in->flags & UAM_ACC_ALIGNED, CPU(dtlb_rt), CPU(dtlb_rb),
                                &ao, ra, miss, &n_miss);
                emit_fast_load(j, size, be, rd, ra);    /* rd = value: written last, as rd
                                                         * may be the address's register */
                done = b_fwd(j);
                for (unsigned k = 0; k < n_miss; k++)
                    patch(j, miss[k]);
            }
            jit_mark(j, MK_SLOW, i);
            ic_flush(j);                                /* the bridge may read time */
            if (emit_replays(j, blk, i, MK_SLOW) || ra != X2)
                load(j, X2, ao, aw);                    /* x2 = addr (again) */
            emit_site(j);
            jit_ra_save(j, false);
            mov_rr(j, X1, X2);                          /* miss / slow path: the bridge */
            mov_rr(j, X0, JC);
            mov_imm(j, X2, in->flags);
            mov_imm(j, X3, size);
            call_bridge(j, jit_br_load);
            bridge_leave(j, epi, &n_epi);
            ld64(j, rd, JC, OFF(scratch));
            if (fast)
                patch(j, done);
            break;
        }
        case UAM_OP_ST:
            if (in->flags & UAM_ACC_X) {               /* store-conditional: always the bridge */
                load_operand(j, X3, blk, in->b, W);
                load_operand(j, X2, blk, in->a, aw);
                bridge_enter(j);
                mov_rr(j, X1, X2);
                mov_rr(j, X2, X3);
                mov_rr(j, X0, JC);
                mov_imm(j, X3, in->flags);
                mov_imm(j, X4, W / 8);
                call_bridge(j, jit_br_store_x);
                bridge_leave(j, epi, &n_epi);
                if (has_dst)
                    ld64(j, rd, JC, OFF(scratch));
                break;
            }
            {
            unsigned size = W / 8;
            bool be = in->flags & UAM_ACC_BE;
            bool fast = !(in->flags & (UAM_ACC_X | UAM_ACC_PHYS | UAM_ACC_UNPRIV));
            const opnd vo = opnd_of(j, blk, in->b, W);
            const opnd ao = opnd_of(j, blk, in->a, aw);
            fast = fast && !(ao.kind == O_IMM && jit_const_slow(ao.imm, size, in->flags));
            /* On the inline path the address and the value stay where they are: a
             * temp's register, the zero register. The stubs and the bridge take them
             * in x2 and x3. */
            const int ra = fast && ao.kind == O_REG ? ao.reg : X2;
            const int rv = !fast ? X3 : vo.kind == O_REG ? vo.reg
                         : vo.kind == O_IMM && vo.imm == 0 ? XZR : X3;
            if (rv == X3)
                load(j, X3, vo, W);
            if (ra == X2)
                load(j, X2, ao, aw);
            size_t done = 0, done_code = 0, miss[2]; unsigned n_miss = 0;
            if (fast) {
                emit_dtlb_probe(j, size, in->flags & UAM_ACC_ALIGNED, CPU(dtlb_wt), CPU(dtlb_wb),
                                &ao, ra, miss, &n_miss);
                emit_fast_store(j, size, be, rv, ra);
                done = b_fwd(j);
                for (unsigned k = 0; k < n_miss; k++)
                    patch(j, miss[k]);
                /* not in the write set: maybe a page that holds code, away from it */
                jit_mark(j, MK_CODE_ST, i);
                if (rv != X3)
                    load(j, X3, vo, W);
                if (ra != X2)
                    load(j, X2, ao, aw);
                ld64(j, X16, JC, OFF(cstub) + 8 * (int32_t)code_stub(size, be, in->flags & UAM_ACC_ALIGNED));
                g32(j, 0xD63F0000u | X16 << 5);         /* blr x16 */
                done_code = cbz_fwd(j, false, X0);
            }
            jit_mark(j, MK_SLOW, i);
            ic_flush(j);
            if (emit_replays(j, blk, i, MK_SLOW))       /* x3 (the value) is kept */
                load(j, X2, ao, aw);                    /* x2 = addr again */
            emit_site(j);
            jit_ra_save(j, false);
            mov_rr(j, X1, X2);
            mov_rr(j, X2, X3);
            mov_rr(j, X0, JC);
            mov_imm(j, X3, in->flags);
            mov_imm(j, X4, size);
            call_bridge(j, jit_br_store);
            bridge_leave(j, epi, &n_epi);
            if (fast) {
                patch(j, done);
                patch(j, done_code);
            }
            break;
            }
        case UAM_OP_AMO:
        case UAM_OP_CAS: {
            const bool cas = op == UAM_OP_CAS;
            load_operand(j, X2, blk, in->b, W);         /* operand / expected */
            if (cas)
                load_operand(j, X3, blk, in->c, W);     /* new value */
            else
                mov_imm(j, X3, 0);
            load_operand(j, X1, blk, in->a, aw);        /* addr */
            bridge_enter(j);
            mov_imm(j, X4, in->flags);
            mov_imm(j, X5, (uint32_t)op | (cas ? 0 : (uint32_t)in->aux << 8) | (uint32_t)W << 16);
            mov_rr(j, X0, JC);
            call_bridge(j, jit_br_rmw);
            bridge_leave(j, epi, &n_epi);
            if (has_dst)
                ld64(j, rd, JC, OFF(scratch));
            break;
        }
        case UAM_OP_FENCE:
            /* Ordering is a no-op: a hart's blocks run one at a time and harts are
             * interleaved only between blocks. FENCE.INSN needs nothing either: code
             * writes invalidate translations and the block ends here (see interp.c). */
            break;
        case UAM_OP_EXIT:
            jit_mark(j, MK_CHAIN, i);
            if (j->n_slots < MAX_SLOTS) {
                emit_chain(j, blk, in->a, aw, epi, &n_epi);
            } else {
                set_res_pc(j, UAM_BR_NEXT, blk, in->a, aw);
                epi[n_epi++] = b_fwd(j);
            }
            break;
        case UAM_OP_SYSRD:
            bridge_enter(j);
            mov_rr(j, X0, JC);
            mov_imm(j, X1, in->aux);
            call_bridge(j, jit_br_sysrd);
            bridge_leave(j, epi, &n_epi);
            ld64(j, rd, JC, OFF(scratch));
            mask_reg(j, rd, W);
            break;
        case UAM_OP_SYSWR:
            load_operand(j, X2, blk, in->a, W);
            bridge_enter(j);
            mov_rr(j, X0, JC);
            mov_imm(j, X1, in->aux);
            call_bridge(j, jit_br_syswr);
            bridge_leave(j, epi, &n_epi);
            break;
        case UAM_OP_HELPER: {
            const uam_ref args[3] = { in->a, in->b, in->c };
            const int regs[3] = { X2, X3, X4 };
            bridge_enter(j);
            for (unsigned k = 0; k < 3; k++) {
                if (uam_ref_is_none(args[k]))
                    mov_imm(j, regs[k], 0);
                else
                    load_operand(j, regs[k], blk, args[k], 64);
            }
            mov_rr(j, X0, JC);
            mov_imm(j, X1, in->aux);
            call_bridge(j, jit_br_helper);
            bridge_leave(j, epi, &n_epi);
            if (has_dst)
                ld64(j, rd, JC, OFF(scratch));
            break;
        }
        case UAM_OP_TRAP:
            /* res = { TRAP, class, exc{TRAP, flags=cont, pc, parent_pc, cause, tval} } */
            mov_imm(j, X0, j->cur.pc);         st64(j, X0, JC, RES(pc));
            st64(j, X0, JC, REXC(pc));
            mov_imm(j, X0, j->cur.parent_pc);  st64(j, X0, JC, REXC(parent_pc));
            mov_imm(j, X0, j->cur.cont ? UAM_EXC_F_CONT : 0); st32(j, X0, JC, REXC(flags));
            load_operand(j, X0, blk, in->a, aw); st64(j, X0, JC, REXC(cause));
            load_operand(j, X0, blk, in->b, aw); st64(j, X0, JC, REXC(tval));
            mov_imm(j, X0, UAM_EXC_TRAP);      st32(j, X0, JC, REXC(kind));
            mov_imm(j, X0, in->aux);           st32(j, X0, JC, RES(trap_class));
            mov_imm(j, X0, UAM_BR_TRAP);       st32(j, X0, JC, RES(kind));
            epi[n_epi++] = b_fwd(j);
            break;
        case UAM_OP_WAIT:
            set_res_pc(j, UAM_BR_WAIT, blk, in->a, aw);
            epi[n_epi++] = b_fwd(j);
            break;
        case UAM_OP_COV:
            emit_cov(j, imark_pc, (uint32_t)blk->imm[uam_ref_index(in->a)],
                     (uint32_t)blk->imm[uam_ref_index(in->b)]);
            break;
        default:
            emit_pure(j, blk, in, rd);
            break;
        }

        if (guarded) {
            /* guard false: no effect, dst = 0 (section 4.1) */
            if (has_dst) {
                size_t over = b_fwd(j);
                patch(j, guard_skip);
                mov_imm(j, rd, 0);
                patch(j, over);
            } else {
                patch(j, guard_skip);
            }
        }
        /* the paths have joined: free what this insn read last, define its result */
        jit_ra_free_dead(j, blk);
        if (j->fuse)
            j->flag_temp = in->dst;              /* in the flags, not in a register */
        else if (has_dst)
            jit_ra_def(j, in->dst, rd);
    }

    /* out-of-line stops at IMARKs */
    jit_mark(j, MK_STUB, 0);
    for (uint32_t k = 0; k < j->n_stubs; k++) {
        patch(j, j->stubs[k].pos);
        addsub_imm(j, true, true, false, IC, IC, 1, false);    /* x23--: not executed */
        /* the replay, with the temps it reads where they were at the IMARK (a replay of
         * immediates reads none) */
        jit_stub_regs(j, k, true);
        emit_replays(j, blk, j->stubs[k].i, MK_STUB);
        jit_stub_regs(j, k, false);
        set_res_pc_imm(j, UAM_BR_INTERRUPTED, j->stubs[k].pc);
        epi[n_epi++] = b_fwd(j);
    }

    /* epilogue */
    for (unsigned k = 0; k < n_epi; k++)
        patch(j, epi[k]);
    free(epi);
    jit_mark(j, MK_EPILOGUE, 0);
    ic_flush(j);
    for (int r = 4; r >= 0; r--)
        stp_ldp(j, true, PAIR_OFF, X19 + 2 * r, X20 + 2 * r, 2 + 2 * r);
    stp_ldp(j, true, PAIR_POST, X29, X30, 12);             /* ldp x29, x30, [sp], #96 */
    g32(j, A64_RET);

    /* link slots, 8-byte aligned after the code */
    jit_mark(j, MK_SLOTS, 0);
    while (j->n & 7)
        g32(j, A64_BRK);
    for (unsigned k = 0; k < j->n_slot_refs; k++)
        patch_to(j, j->slot_ref[k].pos,
                 j->n + sizeof(jit_slot) * j->slot_ref[k].slot + j->slot_ref[k].off);
    for (unsigned k = 0; k < j->n_slots; k++) {
        g64(j, 0);                                         /* entry */
        g64(j, LINK_NONE);                                 /* epoch: never linked */
        g64(j, j->slot_pc[k]);                             /* pc */
    }
    /* fault sites (8-aligned too: slots are 24 bytes) */
    size_t sites_at = j->n;
    for (uint32_t k = 0; k < j->n_sites; k++) {
        g64(j, j->sites[k].pc);
        g64(j, j->sites[k].parent_pc);
        g64(j, j->sites[k].cont);
    }
    for (uint32_t k = 0; k < j->n_site_refs; k++)
        patch_to(j, j->site_refs[k].pos, sites_at + sizeof(jit_site) * j->site_refs[k].site);

    return !j->overflow;
}

/* ---- the interface to jit.c ---------------------------------------------------- */

bool jit_host_init(uam_jit *j)
{
    const char *base = getenv("UAM_JIT_BASELINE");
    j->popcnt = !(base && *base && strcmp(base, "0") != 0);   /* Advanced SIMD is always there */
    j->pool = ra_regs;
    j->pool_max = RA_POOL_MAX;
    j->pool_kept = 5;                                      /* x24..x28 */
    j->res_reg = X0;
    return true;
}

void jit_host_spill(uam_jit *j, uint32_t t, int reg) { st_temp(j, t, reg); }
void jit_host_fill(uam_jit *j, uint32_t t, int reg) { ld64(j, reg, TMP, (int32_t)(8 * t)); }

#endif /* __aarch64__ */
