/*
 * xtensa_lift.c - Xtensa to UAM-IR.
 *
 * A block is found first (decode only: where it ends), then emitted. Knowing the extent
 * up front is what zero-overhead loops need: whether the loop end (LEND) is at one of
 * the block's instruction boundaries is decided when the block is lifted, from LEND's
 * value then, and the block checks on entry that the loop end in effect (LENDA) is not
 * at another of its boundaries; if it is, the hart runs the code one instruction at a
 * time (XT_PRIV_STEP) with the loop-back tested after each, and the next lift sees the
 * new LEND. A block ends at its loop end, so a loop body is one block whose last
 * instruction is followed by the loop-back.
 *
 * Every instruction does its loads, stores, system register accesses and faulting
 * helpers before it writes registers (the commit rule): register writes are queued and
 * committed at the end. Window-overflow and coprocessor-disabled checks come first in
 * an instruction, as their exceptions take priority over its own.
 */
#include <string.h>

#include "xtensa.h"
#include "xtensa_opc.h"

#define NONE UAM_REF_NONE
#define MAX_BLOCK 64u

typedef struct xt_insn {
    uint64_t       pc;
    uint32_t       w;      /* the little-endian instruction word */
    unsigned       len;
    const xt_opc  *op;     /* NULL: illegal */
} xt_insn;

typedef struct lifter {
    uam_lift_ctx *lc;
    uam_builder  *b;
    uam_cpu      *cpu;
    const xt_state *s;
    const uam_xtensa_config *cfg;
    unsigned      mem;      /* access flags of a data access */
    bool          be;
    /* the instruction */
    uint64_t      pc;
    uint32_t      w;
    unsigned      len, id;
    const xt_opc *op;
    uint32_t      r, sf, t, op1, op2;
    struct { uint32_t off; uam_ref v; } q[8];
    unsigned      nq;
    uam_ref       br_cond;   /* a conditional branch: its condition and target */
    uam_ref       br_target;
    /* the block */
    uam_ref       wchk;      /* WCHECK (NONE: not read yet) */
    unsigned      wdone;     /* window groups checked so far */
    bool          cp_done;   /* CPENABLE bit 0 checked */
} lifter;

/* What an instruction left for the block loop to do. */
typedef enum {
    R_NEXT,     /* falls through; the block may go on */
    R_END,      /* falls through; the block ends after it */
    R_BRANCH,   /* conditional: [br_cond] EXIT br_target, then falls through, block ends */
    R_DONE,     /* emitted its own terminator */
} lres;

/* ---- emission helpers -------------------------------------------------- */

static uam_ref imm(lifter *L, uint64_t v) { return uam_imm(L->b, v); }
static uam_ref get(lifter *L, uint32_t off) { return uam_get(L->b, 32, off); }
static uam_ref bin(lifter *L, uam_opcode op, unsigned w, uam_ref a, uam_ref b)
{
    return uam_binop(L->b, op, w, a, b);
}
static uam_ref un(lifter *L, uam_opcode op, unsigned w, uam_ref a) { return uam_unop(L->b, op, w, a); }
static uam_ref sel(lifter *L, uam_ref c, uam_ref a, uam_ref b) { return uam_select(L->b, 32, c, a, b); }
static uam_ref add(lifter *L, uam_ref a, uam_ref b) { return bin(L, UAM_OP_ADD, 32, a, b); }
static uam_ref addi(lifter *L, uam_ref a, uint32_t k) { return k ? add(L, a, imm(L, k)) : a; }
static uam_ref and_(lifter *L, uam_ref a, uint32_t k) { return bin(L, UAM_OP_AND, 32, a, imm(L, k)); }
static uam_ref eq(lifter *L, uam_ref a, uam_ref b) { return bin(L, UAM_OP_EQ, 32, a, b); }
static uam_ref ne(lifter *L, uam_ref a, uam_ref b) { return bin(L, UAM_OP_NE, 32, a, b); }
static uam_ref bool32(lifter *L, uam_ref c) { return un(L, UAM_OP_ZEXT, 32, c); }

static uam_ref ar(lifter *L, unsigned n) { return get(L, XT_A(n)); }

/* Queue a register write; commit() emits them, in order, after the faulting ops. */
static void wr(lifter *L, uint32_t off, uam_ref v)
{
    if (L->nq == UAM_ARRAY_LEN(L->q)) {
        L->b->oom = true;
        return;
    }
    L->q[L->nq].off = off;
    L->q[L->nq].v = v;
    L->nq++;
}

static void commit(lifter *L)
{
    for (unsigned i = 0; i < L->nq; i++)
        uam_put(L->b, 32, L->q[i].off, L->q[i].v, NONE);
    L->nq = 0;
}

static void trap(lifter *L, uam_ref guard, uint32_t cause, uam_ref tval)
{
    uam_trap(L->b, guard, UAM_TRAP_EXCEPTION, imm(L, cause), tval);
}

static lres illegal(lifter *L)
{
    L->nq = 0;
    trap(L, NONE, XT_EXC_ILLEGAL, imm(L, 0));
    return R_DONE;
}

static uam_ref ld(lifter *L, unsigned w, uam_ref addr)
{
    return uam_ld(L->b, w, w == 8 ? (L->mem & ~UAM_ACC_ALIGNED) : L->mem, addr, NONE);
}

static void st(lifter *L, unsigned w, uam_ref addr, uam_ref v)
{
    uam_st(L->b, w, w == 8 ? (L->mem & ~UAM_ACC_ALIGNED) : L->mem, addr, v, NONE);
}

static uam_ref trunc_(lifter *L, unsigned w, uam_ref v) { return un(L, UAM_OP_TRUNC, w, v); }
static uam_ref zext32(lifter *L, uam_ref v) { return un(L, UAM_OP_ZEXT, 32, v); }
static uam_ref sext32(lifter *L, uam_ref v) { return un(L, UAM_OP_SEXT, 32, v); }

static uint32_t sext(uint32_t v, unsigned bits)
{
    uint32_t m = 1u << (bits - 1);
    v &= (m << 1) - 1;
    return (v ^ m) - m;
}

/* ---- checks every instruction may need --------------------------------- */

/* The window-overflow check (ISA manual 4.7.1.3) for an instruction that names an
 * address register of window group g (1..3): the frames WB+1..WB+g must not be live.
 * The answer cannot change within a block before something that ends it or resets
 * the cache (ENTRY), so each group is tested once. */
static void wcheck(lifter *L, unsigned g)
{
    if (!(L->cfg->options & UAM_XT_OPT_WINDOWED) || g <= L->wdone)
        return;
    if (L->wchk == NONE)
        L->wchk = get(L, XT_WCHECK);
    uint32_t mask = ((1u << (g + 1)) - 1) & ~1u;
    trap(L, ne(L, and_(L, L->wchk, mask), imm(L, 0)), XT_TRAP_WOVF, imm(L, 0));
    L->wdone = g;
}

/* Coprocessor 0 (the FPU) must be enabled. */
static void cp0check(lifter *L)
{
    if (L->cp_done || !(L->cfg->cp_mask & 1))
        return;
    uam_ref c = and_(L, get(L, XT_CPENABLE), 1);
    trap(L, eq(L, c, imm(L, 0)), XT_EXC_CP_DISABLED + 0, imm(L, 0));
    L->cp_done = true;
}

/* ---- block structure ---------------------------------------------------- */

/* Read and decode the instruction at pc; false if its bytes cannot be fetched. */
static bool fetch_insn(lifter *L, uint64_t pc, xt_insn *in)
{
    uint64_t b[4];
    if (L->lc->fetch(L->lc, (uint32_t)pc, 1, &b[0]) != UAM_OK)
        return false;
    unsigned op0 = L->be ? (unsigned)(b[0] >> 4) & 15 : (unsigned)b[0] & 15;
    unsigned len = xt_insn_len(L->cfg, op0);
    for (unsigned i = 1; i < len; i++)
        if (L->lc->fetch(L->lc, (uint32_t)(pc + i), 1, &b[i]) != UAM_OK)
            return false;
    uint32_t w = 0;
    if (L->be) {
        for (unsigned i = 0; i < len; i++)
            w = w << 8 | (uint32_t)b[i];
        if (len <= 3)
            w = xt_be_to_le(w, len);
    } else {
        for (unsigned i = 0; i < len; i++)
            w |= (uint32_t)b[i] << (8 * i);
    }
    in->pc = pc;
    in->w = w;
    in->len = len;
    in->op = len <= 3 ? xt_decode(L->cfg, w, len) : NULL;
    return true;
}

/* Special registers whose writes go straight to the register file, or to system state
 * nothing lifted depends on: the block runs on after them. */
static bool sr_plain(unsigned sr)
{
    switch (sr) {
    case 3: case 4: case 5: case 12: case 16: case 17: case 32: case 33: case 34: case 35:
    case 177: case 178: case 179: case 180: case 181: case 182: case 183:   /* EPC1-7 */
    case 192: case 194: case 195: case 196: case 197: case 198: case 199:   /* DEPC, EPS2-7 */
    case 209: case 210: case 211: case 212: case 213: case 214: case 215:   /* EXCSAVE1-7 */
    case 232: case 238: case 244: case 245: case 246: case 247:   /* EXCCAUSE, EXCVADDR, MISC */
        return true;
    default:
        return false;
    }
}

/* Does the block end after this instruction (decided from the decode alone)? */
static bool ends_block(const uam_xtensa_config *cfg, const xt_insn *in)
{
    if (!in->op)
        return true;
    unsigned sr = (in->w >> 8) & 0xFF;
    switch (in->op->id) {
    case XTO_RSR:
        return !xt_sr_ok(cfg, sr, false, false);
    case XTO_RUR: case XTO_WUR:
    case XTO_ENTRY: case XTO_MOVSP: case XTO_MEMW: case XTO_EXTW: case XTO_NOP: case XTO_NOP_N:
    case XTO_RSYNC: case XTO_ESYNC: case XTO_DSYNC: case XTO_EXCW: case XTO_IITLB: case XTO_IDTLB:
    case XTO_RITLB0: case XTO_RITLB1: case XTO_RDTLB0: case XTO_RDTLB1:
    case XTO_PITLB: case XTO_PDTLB: case XTO_RER: case XTO_WER:
        return false;
    case XTO_WSR: case XTO_XSR:
        return !sr_plain(sr) || !xt_sr_ok(cfg, sr, true, in->op->id == XTO_XSR)
               || (in->op->id == XTO_XSR && !xt_sr_ok(cfg, sr, false, true));
    /* control transfers, traps, system state changes */
    case XTO_J: case XTO_JX: case XTO_CALL0: case XTO_CALL4: case XTO_CALL8: case XTO_CALL12:
    case XTO_CALLX0: case XTO_CALLX4: case XTO_CALLX8: case XTO_CALLX12:
    case XTO_RET: case XTO_RET_N: case XTO_RETW: case XTO_RETW_N:
    case XTO_LOOP: case XTO_LOOPNEZ: case XTO_LOOPGTZ:
    case XTO_ILL: case XTO_ILL_N: case XTO_SYSCALL: case XTO_BREAK: case XTO_BREAK_N:
    case XTO_RFE: case XTO_RFDE: case XTO_RFI: case XTO_RFWO: case XTO_RFWU:
    case XTO_RSIL: case XTO_WAITI: case XTO_ROTW: case XTO_ISYNC:
    case XTO_WITLB: case XTO_WDTLB:
        return true;
    default:
        break;
    }
    /* branches: B (op0 7), and SI's BZ/BI0/BI1 except ENTRY */
    unsigned op0 = in->w & 15;
    if (op0 == 7)
        return true;
    if (op0 == 6)
        return true;
    if (in->op->id == XTO_BEQZ_N || in->op->id == XTO_BNEZ_N)
        return true;
    return false;
}

/* ---- loads and stores --------------------------------------------------- */

static uam_ref l32r_addr(lifter *L)
{
    uint32_t imm16 = (L->w >> 8) & 0xFFFF;
    uint32_t off = (0xFFFF0000u | imm16) << 2;
    uint32_t pcrel = (((uint32_t)L->pc + 3) & ~3u) + off;
    if (!(L->cfg->options & UAM_XT_OPT_LITBASE))
        return imm(L, pcrel);
    uam_ref lb = get(L, XT_LITBASE);
    uam_ref en = ne(L, and_(L, lb, 1), imm(L, 0));
    return sel(L, en, addi(L, and_(L, lb, 0xFFFFF000u), off), imm(L, pcrel));
}

/* ---- the instructions --------------------------------------------------- */

static lres branch(lifter *L, uam_ref cond, uint64_t target)
{
    L->br_cond = cond;
    L->br_target = imm(L, (uint32_t)target);
    return R_BRANCH;
}

static lres jump(lifter *L, uam_ref target, uam_exit_kind kind)
{
    commit(L);
    uam_exit(L->b, NONE, target, kind);
    return R_DONE;
}

static const int32_t b4const[16] = { -1, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 16, 32, 64, 128, 256 };
static const uint32_t b4constu[16] = { 32768, 65536, 2, 3, 4, 5, 6, 7, 8, 10, 12, 16, 32, 64, 128, 256 };

/* Bit b of BR as a 1-bit value. */
static uam_ref brbit(lifter *L, unsigned b)
{
    return ne(L, and_(L, get(L, XT_BR), 1u << b), imm(L, 0));
}

static void set_brbit(lifter *L, unsigned b, uam_ref v1)
{
    uam_ref br = and_(L, get(L, XT_BR), ~(1u << b));
    wr(L, XT_BR, bin(L, UAM_OP_OR, 32, br, bin(L, UAM_OP_SHL, 32, bool32(L, v1), imm(L, b))));
}

/* A bit number of BBC/BBS(I): big-endian cores count from the most significant bit. */
static uam_ref bitno(lifter *L, uam_ref n)
{
    return L->be ? bin(L, UAM_OP_XOR, 32, n, imm(L, 31)) : n;
}

static lres lift_branch(lifter *L)
{
    uint64_t pc = L->pc;
    unsigned s = L->sf, t = L->t, r = L->r;
    uint64_t t8 = pc + 4 + (int32_t)sext(L->w >> 16, 8);
    uint64_t t12 = pc + 4 + (int32_t)sext(L->w >> 12, 12);
    uam_ref as = NONE, at = NONE;
    switch (L->id) {
    case XTO_BEQ: case XTO_BNE: case XTO_BLT: case XTO_BGE: case XTO_BLTU: case XTO_BGEU:
    case XTO_BANY: case XTO_BNONE: case XTO_BALL: case XTO_BNALL: case XTO_BBC: case XTO_BBS:
        as = ar(L, s);
        at = ar(L, t);
        break;
    default:
        break;
    }
    switch (L->id) {
    case XTO_BEQ:  return branch(L, eq(L, as, at), t8);
    case XTO_BNE:  return branch(L, ne(L, as, at), t8);
    case XTO_BLT:  return branch(L, bin(L, UAM_OP_SLT, 32, as, at), t8);
    case XTO_BGE:  return branch(L, bin(L, UAM_OP_SLE, 32, at, as), t8);
    case XTO_BLTU: return branch(L, bin(L, UAM_OP_ULT, 32, as, at), t8);
    case XTO_BGEU: return branch(L, bin(L, UAM_OP_ULE, 32, at, as), t8);
    case XTO_BANY:  return branch(L, ne(L, bin(L, UAM_OP_AND, 32, as, at), imm(L, 0)), t8);
    case XTO_BNONE: return branch(L, eq(L, bin(L, UAM_OP_AND, 32, as, at), imm(L, 0)), t8);
    case XTO_BALL:  return branch(L, eq(L, bin(L, UAM_OP_AND, 32, bin(L, UAM_OP_XOR, 32, as, imm(L, 0xFFFFFFFFu)), at), imm(L, 0)), t8);
    case XTO_BNALL: return branch(L, ne(L, bin(L, UAM_OP_AND, 32, bin(L, UAM_OP_XOR, 32, as, imm(L, 0xFFFFFFFFu)), at), imm(L, 0)), t8);
    case XTO_BBC: case XTO_BBS: {
        uam_ref b = bitno(L, and_(L, at, 31));
        uam_ref bit = and_(L, bin(L, UAM_OP_LSHR, 32, as, b), 1);
        return branch(L, L->id == XTO_BBS ? ne(L, bit, imm(L, 0)) : eq(L, bit, imm(L, 0)), t8);
    }
    case XTO_BBCI: case XTO_BBSI: {
        unsigned n = (r & 1) << 4 | t;
        if (L->be)
            n ^= 31;
        uam_ref bit = and_(L, ar(L, s), 1u << n);
        return branch(L, L->id == XTO_BBSI ? ne(L, bit, imm(L, 0)) : eq(L, bit, imm(L, 0)), t8);
    }
    case XTO_BEQI: return branch(L, eq(L, ar(L, s), imm(L, (uint32_t)b4const[r])), t8);
    case XTO_BNEI: return branch(L, ne(L, ar(L, s), imm(L, (uint32_t)b4const[r])), t8);
    case XTO_BLTI: return branch(L, bin(L, UAM_OP_SLT, 32, ar(L, s), imm(L, (uint32_t)b4const[r])), t8);
    case XTO_BGEI: return branch(L, bin(L, UAM_OP_SLE, 32, imm(L, (uint32_t)b4const[r]), ar(L, s)), t8);
    case XTO_BLTUI: return branch(L, bin(L, UAM_OP_ULT, 32, ar(L, s), imm(L, b4constu[r])), t8);
    case XTO_BGEUI: return branch(L, bin(L, UAM_OP_ULE, 32, imm(L, b4constu[r]), ar(L, s)), t8);
    case XTO_BEQZ: return branch(L, eq(L, ar(L, s), imm(L, 0)), t12);
    case XTO_BNEZ: return branch(L, ne(L, ar(L, s), imm(L, 0)), t12);
    case XTO_BLTZ: return branch(L, bin(L, UAM_OP_SLT, 32, ar(L, s), imm(L, 0)), t12);
    case XTO_BGEZ: return branch(L, bin(L, UAM_OP_SLE, 32, imm(L, 0), ar(L, s)), t12);
    case XTO_BEQZ_N: case XTO_BNEZ_N: {
        uint64_t tn = pc + 4 + ((t & 3) << 4 | r);
        uam_ref a = ar(L, s);
        return branch(L, L->id == XTO_BEQZ_N ? eq(L, a, imm(L, 0)) : ne(L, a, imm(L, 0)), tn);
    }
    case XTO_BT: return branch(L, brbit(L, s), t8);
    case XTO_BF: return branch(L, eq(L, bool32(L, brbit(L, s)), imm(L, 0)), t8);
    default:
        return illegal(L);
    }
}

/* CALLn / CALLXn's link: the window increment in the top two bits. */
static uam_ref link(lifter *L, unsigned n)
{
    return imm(L, (uint32_t)n << 30 | (((uint32_t)L->pc + 3) & 0x3FFFFFFFu));
}

static lres lift_call(lifter *L)
{
    unsigned n = 0;
    switch (L->id) {
    case XTO_CALL0: case XTO_CALLX0: n = 0; break;
    case XTO_CALL4: case XTO_CALLX4: n = 1; break;
    case XTO_CALL8: case XTO_CALLX8: n = 2; break;
    default: n = 3; break;
    }
    bool x = L->id == XTO_CALLX0 || L->id == XTO_CALLX4 || L->id == XTO_CALLX8 || L->id == XTO_CALLX12;
    uam_ref target;
    if (x) {
        target = ar(L, L->sf);   /* read before the link is written */
    } else {
        uint32_t off = sext(L->w >> 6, 18);
        target = imm(L, (((uint32_t)L->pc & ~3u) + (off << 2) + 4) & 0xFFFFFFFFu);
    }
    if (n) {
        wcheck(L, x && (L->sf >> 2) > n ? L->sf >> 2 : n);
        wr(L, XT_CALLINC, imm(L, n));
        wr(L, XT_A(4 * n), link(L, n));
    } else {
        wr(L, XT_A(0), imm(L, ((uint32_t)L->pc + 3) & 0xFFFFFFFFu));
    }
    return jump(L, target, UAM_EXIT_CALL);
}

/* MAC16: the operands, the operation and the load of an instruction, from its name. */
static lres lift_mac16(lifter *L)
{
    const char *nm = xt_opname(L->id);   /* e.g. "mula.dd.hl.ldinc", "ldinc", "umul.aa.ll" */
    uint32_t w = L->w;
    unsigned s = L->sf, t = L->t;
    unsigned mw = (w >> 12) & 3, mx = (w >> 14) & 1, my = 2 + ((w >> 6) & 1);
    bool ldinc = strstr(nm, "ldinc") != NULL, lddec = strstr(nm, "lddec") != NULL;
    uam_ref vaddr = NONE, loaded = NONE;
    if (ldinc || lddec) {
        vaddr = addi(L, ar(L, s), ldinc ? 4u : (uint32_t)-4);
        loaded = ld(L, 32, vaddr);
    }
    if (!strcmp(nm, "ldinc") || !strcmp(nm, "lddec")) {
        wr(L, XT_A(s), vaddr);
        wr(L, XT_M(mw), loaded);
        return R_NEXT;
    }
    bool umul = !strncmp(nm, "umul", 4);
    int kind = umul ? 0 : !strncmp(nm, "mula", 4) ? 1 : !strncmp(nm, "muls", 4) ? 2 : 0;
    const char *src = strchr(nm, '.') + 1;          /* "aa", "ad", "da", "dd" */
    const char *half = src + 3;                     /* "ll", "hl", "lh", "hh" */
    uam_ref x = src[0] == 'a' ? ar(L, s) : get(L, XT_M(mx));
    uam_ref y = src[1] == 'a' ? ar(L, t) : get(L, XT_M(my));
    uam_ref hx = trunc_(L, 16, half[0] == 'h' ? bin(L, UAM_OP_LSHR, 32, x, imm(L, 16)) : x);
    uam_ref hy = trunc_(L, 16, half[1] == 'h' ? bin(L, UAM_OP_LSHR, 32, y, imm(L, 16)) : y);
    uam_ref p;   /* the product, as a 64-bit value to add to the 40-bit ACC */
    if (umul) {
        p = un(L, UAM_OP_ZEXT, 64, bin(L, UAM_OP_MUL, 32, zext32(L, hx), zext32(L, hy)));
    } else {
        p = un(L, UAM_OP_SEXT, 64, bin(L, UAM_OP_MUL, 32, sext32(L, hx), sext32(L, hy)));
    }
    uam_ref acc;
    if (kind == 0) {
        acc = p;
    } else {
        uam_ref lo = un(L, UAM_OP_ZEXT, 64, get(L, XT_ACCLO));
        uam_ref hi = un(L, UAM_OP_ZEXT, 64, and_(L, get(L, XT_ACCHI), 0xFF));
        uam_ref a = bin(L, UAM_OP_OR, 64, lo, bin(L, UAM_OP_SHL, 64, hi, imm(L, 32)));
        acc = bin(L, kind == 1 ? UAM_OP_ADD : UAM_OP_SUB, 64, a, p);
    }
    wr(L, XT_ACCLO, trunc_(L, 32, acc));
    wr(L, XT_ACCHI, sext32(L, trunc_(L, 8, bin(L, UAM_OP_LSHR, 64, acc, imm(L, 32)))));
    if (ldinc || lddec) {
        wr(L, XT_A(s), vaddr);
        wr(L, XT_M(mw), loaded);
    }
    return R_NEXT;
}

static bool is_mac16(unsigned id)
{
    const char *nm = xt_opname(id);
    return !strncmp(nm, "mul.", 4) || !strncmp(nm, "mula.", 5) || !strncmp(nm, "muls.", 5)
           || !strncmp(nm, "umul.", 5) || !strcmp(nm, "ldinc") || !strcmp(nm, "lddec");
}

/* Special registers held in the register file. */
static int sr_off(unsigned sr)
{
    switch (sr) {
    case 0: return XT_LBEG;
    case 1: return XT_LEND;
    case 2: return XT_LCOUNT;
    case 3: return XT_SAR;
    case 4: return XT_BR;
    case 5: return XT_LITBASE;
    case 12: return XT_SCOMPARE1;
    case 16: return XT_ACCLO;
    case 17: return XT_ACCHI;
    case 32: case 33: case 34: case 35: return (int)XT_M(sr - 32);
    default: return -1;
    }
}

/* The value WSR writes to a register-file SR. */
static uam_ref sr_wvalue(lifter *L, unsigned sr, uam_ref v)
{
    switch (sr) {
    case 3: return and_(L, v, 63);
    case 4: return and_(L, v, 0xFFFF);
    case 5: return and_(L, v, 0xFFFFF001u);
    case 17: return sext32(L, trunc_(L, 8, v));
    default: return v;
    }
}

static lres lift_sr(lifter *L)
{
    unsigned sr = (L->w >> 8) & 0xFF, t = L->t;
    bool rd = L->id == XTO_RSR || L->id == XTO_XSR, wrt = L->id == XTO_WSR || L->id == XTO_XSR;
    if ((rd && !xt_sr_ok(L->cfg, sr, false, L->id == XTO_XSR))
        || (wrt && !xt_sr_ok(L->cfg, sr, true, L->id == XTO_XSR)))
        return illegal(L);
    int off = sr_off(sr);
    /* Loop registers and everything else go through the system code (LENDA, ...) */
    bool inline_w = off >= 0 && sr > 2;
    uam_ref old = NONE, nv = wrt ? ar(L, t) : NONE;
    if (rd)
        old = off >= 0 ? get(L, (uint32_t)off) : uam_sysrd(L->b, 32, sr, NONE);
    if (wrt) {
        if (inline_w)
            wr(L, (uint32_t)off, sr_wvalue(L, sr, nv));
        else
            uam_syswr(L->b, 32, sr, nv, NONE);
    }
    if (rd)
        wr(L, XT_A(t), old);
    return wrt && !sr_plain(sr) ? R_END : R_NEXT;
}

static lres lift_ur(lifter *L)
{
    unsigned ur = L->id == XTO_RUR ? (L->sf << 4 | L->t) : ((L->w >> 8) & 0xFF);
    uint32_t off;
    switch (ur) {
    case 231:
        if (!(L->cfg->options & UAM_XT_OPT_THREADPTR))
            return illegal(L);
        off = XT_THREADPTR;
        break;
    case 232: case 233:
        if (!(L->cfg->options & UAM_XT_OPT_FP))
            return illegal(L);
        cp0check(L);
        off = ur == 232 ? XT_FCR : XT_FSR;
        break;
    default:
        return illegal(L);
    }
    if (L->id == XTO_RUR)
        wr(L, XT_A(L->r), get(L, off));
    else
        wr(L, off, ar(L, L->t));
    return R_NEXT;
}

static uam_ref shamt(lifter *L) { return get(L, XT_SAR); }

/* A 32-bit value at 64 bits (an immediate is one already). */
static uam_ref z64(lifter *L, uam_ref v)
{
    return uam_ref_is_imm(v) ? v : un(L, UAM_OP_ZEXT, 64, v);
}

/* (hi:lo) >> sa, the low 32 bits: the funnel shifter behind every SAR shift. */
static uam_ref funnel(lifter *L, uam_ref hi, uam_ref lo, uam_ref sa)
{
    uam_ref v = bin(L, UAM_OP_OR, 64, bin(L, UAM_OP_SHL, 64, z64(L, hi), imm(L, 32)), z64(L, lo));
    return trunc_(L, 32, bin(L, UAM_OP_LSHR, 64, v, z64(L, sa)));
}

static lres lift_fp(lifter *L)
{
    unsigned r = L->r, s = L->sf, t = L->t;
    cp0check(L);
    switch (L->id) {
    case XTO_LSI: case XTO_SSI: case XTO_LSIP: case XTO_SSIP: {
        uint32_t off = ((L->w >> 16) & 0xFF) << 2;
        uam_ref base = ar(L, s);
        bool post = L->id == XTO_LSIP || L->id == XTO_SSIP;
        uam_ref a = post ? base : addi(L, base, off);
        if (L->id == XTO_LSI || L->id == XTO_LSIP)
            wr(L, XT_F(t), ld(L, 32, a));
        else
            st(L, 32, a, get(L, XT_F(t)));
        if (post)
            wr(L, XT_A(s), addi(L, base, off));
        return R_NEXT;
    }
    case XTO_LSX: case XTO_SSX: case XTO_LSXP: case XTO_SSXP: {
        uam_ref base = ar(L, s), idx = ar(L, t);
        bool post = L->id == XTO_LSXP || L->id == XTO_SSXP;
        uam_ref a = post ? base : add(L, base, idx);
        if (L->id == XTO_LSX || L->id == XTO_LSXP)
            wr(L, XT_F(r), ld(L, 32, a));
        else
            st(L, 32, a, get(L, XT_F(r)));
        if (post)
            wr(L, XT_A(s), add(L, base, idx));
        return R_NEXT;
    }
    case XTO_MOV_S: wr(L, XT_F(r), get(L, XT_F(s))); return R_NEXT;
    case XTO_NEG_S: wr(L, XT_F(r), bin(L, UAM_OP_XOR, 32, get(L, XT_F(s)), imm(L, 0x80000000u))); return R_NEXT;
    case XTO_ABS_S: wr(L, XT_F(r), and_(L, get(L, XT_F(s)), 0x7FFFFFFFu)); return R_NEXT;
    case XTO_RFR: wr(L, XT_A(r), get(L, XT_F(s))); return R_NEXT;
    case XTO_WFR: wr(L, XT_F(r), ar(L, s)); return R_NEXT;
    case XTO_MOVEQZ_S: case XTO_MOVNEZ_S: case XTO_MOVLTZ_S: case XTO_MOVGEZ_S:
    case XTO_MOVT_S: case XTO_MOVF_S: {
        uam_ref c;
        switch (L->id) {
        case XTO_MOVEQZ_S: c = eq(L, ar(L, t), imm(L, 0)); break;
        case XTO_MOVNEZ_S: c = ne(L, ar(L, t), imm(L, 0)); break;
        case XTO_MOVLTZ_S: c = bin(L, UAM_OP_SLT, 32, ar(L, t), imm(L, 0)); break;
        case XTO_MOVGEZ_S: c = bin(L, UAM_OP_SLE, 32, imm(L, 0), ar(L, t)); break;
        case XTO_MOVT_S: c = brbit(L, t); break;
        default: c = eq(L, bool32(L, brbit(L, t)), imm(L, 0)); break;
        }
        wr(L, XT_F(r), sel(L, c, get(L, XT_F(s)), get(L, XT_F(r))));
        return R_NEXT;
    }
    default:
        uam_helper(L->b, &xt_helpers, XT_H_FPU, imm(L, L->w), imm(L, L->id), NONE, NONE);
        return R_NEXT;
    }
}

static lres lift_insn(lifter *L)
{
    unsigned r = L->r, s = L->sf, t = L->t;
    uint32_t w = L->w;
    switch (L->id) {
    /* ---- arithmetic and logic ---- */
    case XTO_ADD: case XTO_ADD_N: wr(L, XT_A(r), add(L, ar(L, s), ar(L, t))); return R_NEXT;
    case XTO_ADDX2: case XTO_ADDX4: case XTO_ADDX8: {
        unsigned sh = L->id == XTO_ADDX2 ? 1 : L->id == XTO_ADDX4 ? 2 : 3;
        wr(L, XT_A(r), add(L, bin(L, UAM_OP_SHL, 32, ar(L, s), imm(L, sh)), ar(L, t)));
        return R_NEXT;
    }
    case XTO_SUB: wr(L, XT_A(r), bin(L, UAM_OP_SUB, 32, ar(L, s), ar(L, t))); return R_NEXT;
    case XTO_SUBX2: case XTO_SUBX4: case XTO_SUBX8: {
        unsigned sh = L->id == XTO_SUBX2 ? 1 : L->id == XTO_SUBX4 ? 2 : 3;
        wr(L, XT_A(r), bin(L, UAM_OP_SUB, 32, bin(L, UAM_OP_SHL, 32, ar(L, s), imm(L, sh)), ar(L, t)));
        return R_NEXT;
    }
    case XTO_ADDI: wr(L, XT_A(t), addi(L, ar(L, s), sext(w >> 16, 8))); return R_NEXT;
    case XTO_ADDMI: wr(L, XT_A(t), addi(L, ar(L, s), sext(w >> 16, 8) << 8)); return R_NEXT;
    case XTO_ADDI_N: wr(L, XT_A(r), addi(L, ar(L, s), t ? t : 0xFFFFFFFFu)); return R_NEXT;
    case XTO_AND: wr(L, XT_A(r), bin(L, UAM_OP_AND, 32, ar(L, s), ar(L, t))); return R_NEXT;
    case XTO_OR: wr(L, XT_A(r), bin(L, UAM_OP_OR, 32, ar(L, s), ar(L, t))); return R_NEXT;
    case XTO_XOR: wr(L, XT_A(r), bin(L, UAM_OP_XOR, 32, ar(L, s), ar(L, t))); return R_NEXT;
    case XTO_NEG: wr(L, XT_A(r), bin(L, UAM_OP_SUB, 32, imm(L, 0), ar(L, t))); return R_NEXT;
    case XTO_ABS: {
        uam_ref x = ar(L, t);
        wr(L, XT_A(r), sel(L, bin(L, UAM_OP_SLT, 32, x, imm(L, 0)), bin(L, UAM_OP_SUB, 32, imm(L, 0), x), x));
        return R_NEXT;
    }
    case XTO_MOVI: wr(L, XT_A(t), imm(L, sext(s << 8 | ((w >> 16) & 0xFF), 12))); return R_NEXT;
    case XTO_MOVI_N: {
        uint32_t i7 = (t & 7) << 4 | r;
        wr(L, XT_A(s), imm(L, i7 >= 96 ? i7 - 128u : i7));
        return R_NEXT;
    }
    case XTO_MOV_N: wr(L, XT_A(t), ar(L, s)); return R_NEXT;
    case XTO_MOVEQZ: case XTO_MOVNEZ: case XTO_MOVLTZ: case XTO_MOVGEZ: case XTO_MOVT: case XTO_MOVF: {
        uam_ref c;
        switch (L->id) {
        case XTO_MOVEQZ: c = eq(L, ar(L, t), imm(L, 0)); break;
        case XTO_MOVNEZ: c = ne(L, ar(L, t), imm(L, 0)); break;
        case XTO_MOVLTZ: c = bin(L, UAM_OP_SLT, 32, ar(L, t), imm(L, 0)); break;
        case XTO_MOVGEZ: c = bin(L, UAM_OP_SLE, 32, imm(L, 0), ar(L, t)); break;
        case XTO_MOVT: c = brbit(L, t); break;
        default: c = eq(L, bool32(L, brbit(L, t)), imm(L, 0)); break;
        }
        wr(L, XT_A(r), sel(L, c, ar(L, s), ar(L, r)));
        return R_NEXT;
    }
    case XTO_EXTUI: {
        unsigned sh = (L->op1 & 1) << 4 | s, bits = L->op2 + 1;
        wr(L, XT_A(r), and_(L, bin(L, UAM_OP_LSHR, 32, ar(L, t), imm(L, sh)), (uint32_t)((1ull << bits) - 1)));
        return R_NEXT;
    }
    /* ---- shifts ---- */
    case XTO_SLLI: {
        unsigned sa = (L->op2 & 1) << 4 | t;   /* the field holds 32 - shift */
        wr(L, XT_A(r), funnel(L, ar(L, s), imm(L, 0), imm(L, sa)));
        return R_NEXT;
    }
    case XTO_SRLI: wr(L, XT_A(r), bin(L, UAM_OP_LSHR, 32, ar(L, t), imm(L, s))); return R_NEXT;
    case XTO_SRAI: wr(L, XT_A(r), bin(L, UAM_OP_ASHR, 32, ar(L, t), imm(L, (L->op2 & 1) << 4 | s))); return R_NEXT;
    case XTO_SRC: wr(L, XT_A(r), funnel(L, ar(L, s), ar(L, t), shamt(L))); return R_NEXT;
    case XTO_SRL: wr(L, XT_A(r), funnel(L, imm(L, 0), ar(L, t), shamt(L))); return R_NEXT;
    case XTO_SLL: wr(L, XT_A(r), funnel(L, ar(L, s), imm(L, 0), shamt(L))); return R_NEXT;
    case XTO_SRA: {
        uam_ref x = ar(L, t);
        wr(L, XT_A(r), funnel(L, bin(L, UAM_OP_ASHR, 32, x, imm(L, 31)), x, shamt(L)));
        return R_NEXT;
    }
    case XTO_SSL: wr(L, XT_SAR, bin(L, UAM_OP_SUB, 32, imm(L, 32), and_(L, ar(L, s), 31))); return R_NEXT;
    case XTO_SSR: wr(L, XT_SAR, and_(L, ar(L, s), 31)); return R_NEXT;
    case XTO_SSA8L: wr(L, XT_SAR, bin(L, UAM_OP_SHL, 32, and_(L, ar(L, s), 3), imm(L, 3))); return R_NEXT;
    case XTO_SSA8B: wr(L, XT_SAR, bin(L, UAM_OP_SUB, 32, imm(L, 32), bin(L, UAM_OP_SHL, 32, and_(L, ar(L, s), 3), imm(L, 3)))); return R_NEXT;
    case XTO_SSAI: wr(L, XT_SAR, imm(L, (t & 1) << 4 | s)); return R_NEXT;
    /* ---- multiply, divide, miscellaneous ---- */
    case XTO_MUL16U: case XTO_MUL16S: {
        bool sg = L->id == XTO_MUL16S;
        uam_ref a = trunc_(L, 16, ar(L, s)), b2 = trunc_(L, 16, ar(L, t));
        a = sg ? sext32(L, a) : zext32(L, a);
        b2 = sg ? sext32(L, b2) : zext32(L, b2);
        wr(L, XT_A(r), bin(L, UAM_OP_MUL, 32, a, b2));
        return R_NEXT;
    }
    case XTO_MULL: wr(L, XT_A(r), bin(L, UAM_OP_MUL, 32, ar(L, s), ar(L, t))); return R_NEXT;
    case XTO_MULUH: wr(L, XT_A(r), bin(L, UAM_OP_MULHU, 32, ar(L, s), ar(L, t))); return R_NEXT;
    case XTO_MULSH: wr(L, XT_A(r), bin(L, UAM_OP_MULHS, 32, ar(L, s), ar(L, t))); return R_NEXT;
    case XTO_QUOS: case XTO_QUOU: case XTO_REMS: case XTO_REMU: {
        uam_ref a = ar(L, s), d = ar(L, t);
        trap(L, eq(L, d, imm(L, 0)), XT_EXC_DIVZERO, imm(L, 0));
        uam_opcode op = L->id == XTO_QUOS ? UAM_OP_SDIV : L->id == XTO_QUOU ? UAM_OP_UDIV
                        : L->id == XTO_REMS ? UAM_OP_SREM : UAM_OP_UREM;
        wr(L, XT_A(r), bin(L, op, 32, a, d));
        return R_NEXT;
    }
    case XTO_NSA: {
        uam_ref x = ar(L, s);
        uam_ref u = bin(L, UAM_OP_XOR, 32, x, bin(L, UAM_OP_ASHR, 32, x, imm(L, 31)));
        wr(L, XT_A(t), bin(L, UAM_OP_SUB, 32, un(L, UAM_OP_CLZ, 32, u), imm(L, 1)));
        return R_NEXT;
    }
    case XTO_NSAU: wr(L, XT_A(t), un(L, UAM_OP_CLZ, 32, ar(L, s))); return R_NEXT;
    case XTO_MIN: case XTO_MAX: case XTO_MINU: case XTO_MAXU: {
        uam_ref a = ar(L, s), b2 = ar(L, t);
        uam_ref lt = bin(L, L->id == XTO_MIN || L->id == XTO_MAX ? UAM_OP_SLT : UAM_OP_ULT, 32, a, b2);
        bool mn = L->id == XTO_MIN || L->id == XTO_MINU;
        wr(L, XT_A(r), mn ? sel(L, lt, a, b2) : sel(L, lt, b2, a));
        return R_NEXT;
    }
    case XTO_SEXT: {
        unsigned sh = 32 - (t + 8);
        wr(L, XT_A(r), bin(L, UAM_OP_ASHR, 32, bin(L, UAM_OP_SHL, 32, ar(L, s), imm(L, sh)), imm(L, sh)));
        return R_NEXT;
    }
    case XTO_CLAMPS: {
        unsigned n = t + 7;
        uam_ref x = ar(L, s);
        uint32_t hi = (1u << n) - 1, lo = (uint32_t)-(int32_t)(1u << n);
        uam_ref v = sel(L, bin(L, UAM_OP_SLT, 32, x, imm(L, lo)), imm(L, lo), x);
        wr(L, XT_A(r), sel(L, bin(L, UAM_OP_SLT, 32, imm(L, hi), x), imm(L, hi), v));
        return R_NEXT;
    }
    case XTO_SALT: wr(L, XT_A(r), bool32(L, bin(L, UAM_OP_SLT, 32, ar(L, s), ar(L, t)))); return R_NEXT;
    case XTO_SALTU: wr(L, XT_A(r), bool32(L, bin(L, UAM_OP_ULT, 32, ar(L, s), ar(L, t)))); return R_NEXT;
    /* ---- loads and stores ---- */
    case XTO_L8UI: wr(L, XT_A(t), zext32(L, ld(L, 8, addi(L, ar(L, s), (w >> 16) & 0xFF)))); return R_NEXT;
    case XTO_L16UI: wr(L, XT_A(t), zext32(L, ld(L, 16, addi(L, ar(L, s), ((w >> 16) & 0xFF) << 1)))); return R_NEXT;
    case XTO_L16SI: wr(L, XT_A(t), sext32(L, ld(L, 16, addi(L, ar(L, s), ((w >> 16) & 0xFF) << 1)))); return R_NEXT;
    case XTO_L32I: case XTO_L32AI:
        wr(L, XT_A(t), uam_ld(L->b, 32, L->mem | (L->id == XTO_L32AI ? UAM_ACC_ACQ : 0),
                              addi(L, ar(L, s), ((w >> 16) & 0xFF) << 2), NONE));
        return R_NEXT;
    case XTO_L32I_N: wr(L, XT_A(t), ld(L, 32, addi(L, ar(L, s), r << 2))); return R_NEXT;
    case XTO_S8I: st(L, 8, addi(L, ar(L, s), (w >> 16) & 0xFF), trunc_(L, 8, ar(L, t))); return R_NEXT;
    case XTO_S16I: st(L, 16, addi(L, ar(L, s), ((w >> 16) & 0xFF) << 1), trunc_(L, 16, ar(L, t))); return R_NEXT;
    case XTO_S32I: case XTO_S32RI: case XTO_S32NB:
        uam_st(L->b, 32, L->mem | (L->id == XTO_S32RI ? UAM_ACC_REL : 0),
               addi(L, ar(L, s), (L->id == XTO_S32NB ? r : (w >> 16) & 0xFF) << 2), ar(L, t), NONE);
        return R_NEXT;
    case XTO_S32I_N: st(L, 32, addi(L, ar(L, s), r << 2), ar(L, t)); return R_NEXT;
    case XTO_L32R: wr(L, XT_A(t), ld(L, 32, l32r_addr(L))); return R_NEXT;
    case XTO_L32E: case XTO_S32E: {
        uam_ref a = addi(L, ar(L, s), (r << 2) - 64u);
        if (L->id == XTO_L32E)
            wr(L, XT_A(t), ld(L, 32, a));
        else
            st(L, 32, a, ar(L, t));
        return R_NEXT;
    }
    case XTO_S32C1I: {
        uam_ref a = addi(L, ar(L, s), ((w >> 16) & 0xFF) << 2);
        uam_ref old = uam_cas(L->b, 32, L->mem | UAM_ACC_ACQ | UAM_ACC_REL, a, get(L, XT_SCOMPARE1), ar(L, t), NONE);
        wr(L, XT_A(t), old);
        return R_NEXT;
    }
    /* ---- jumps, calls, returns ---- */
    case XTO_J: return jump(L, imm(L, (uint32_t)(L->pc + 4 + (int32_t)sext(w >> 6, 18))), UAM_EXIT_JUMP);
    case XTO_JX: return jump(L, ar(L, s), UAM_EXIT_JUMP);
    case XTO_RET: case XTO_RET_N: return jump(L, ar(L, 0), UAM_EXIT_RET);
    case XTO_CALL0: case XTO_CALL4: case XTO_CALL8: case XTO_CALL12:
    case XTO_CALLX0: case XTO_CALLX4: case XTO_CALLX8: case XTO_CALLX12:
        return lift_call(L);
    /* ---- windows ---- */
    case XTO_ENTRY: {
        /* The helper checks the window (group CALLINC) and rotates it; afterwards the
         * cached WCHECK is stale. */
        uam_helper(L->b, &xt_helpers, XT_H_ENTRY, imm(L, s | ((w >> 12) & 0xFFF) << 4), NONE, NONE, NONE);
        L->wchk = NONE;
        L->wdone = 0;
        return R_NEXT;
    }
    case XTO_RETW: case XTO_RETW_N: {
        uam_ref target = uam_helper(L->b, &xt_helpers, XT_H_RETW, imm(L, (uint32_t)L->pc), NONE, NONE, NONE);
        uam_exit(L->b, NONE, target, UAM_EXIT_RET);
        return R_DONE;
    }
    case XTO_MOVSP: {
        uam_helper(L->b, &xt_helpers, XT_H_MOVSP, NONE, NONE, NONE, NONE);
        wr(L, XT_A(t), ar(L, s));
        return R_NEXT;
    }
    case XTO_ROTW:
        uam_syswr(L->b, 32, XT_SYS_ROTW, imm(L, sext(t, 4)), NONE);
        return R_END;
    case XTO_RFWO: case XTO_RFWU: case XTO_RFE: case XTO_RFDE: {
        uint32_t id = L->id == XTO_RFWO ? XT_SYS_RFWO : L->id == XTO_RFWU ? XT_SYS_RFWU
                      : L->id == XTO_RFE ? XT_SYS_RFE : XT_SYS_RFDE;
        uam_exit(L->b, NONE, uam_sysrd(L->b, 32, id, NONE), UAM_EXIT_EXC_RETURN);
        return R_DONE;
    }
    case XTO_RFI:
        if (s < 2 || s > L->cfg->nlevel + (L->cfg->nmi ? 1u : 0u))
            return illegal(L);
        uam_syswr(L->b, 32, XT_SYS_RFI, imm(L, s), NONE);
        uam_exit(L->b, NONE, uam_sysrd(L->b, 32, XT_SYS_RET_PC, NONE), UAM_EXIT_EXC_RETURN);
        return R_DONE;
    /* ---- loops ---- */
    case XTO_LOOP: case XTO_LOOPNEZ: case XTO_LOOPGTZ: {
        uint32_t lend = (uint32_t)L->pc + 4 + ((w >> 16) & 0xFF);
        uam_ref n = ar(L, s);
        wr(L, XT_LCOUNT, bin(L, UAM_OP_SUB, 32, n, imm(L, 1)));
        wr(L, XT_LBEG, imm(L, (uint32_t)L->pc + 3));
        wr(L, XT_LEND, imm(L, lend));
        /* The loop end in effect: LEND while LCOUNT != 0 and PS.EXCM is clear. */
        uam_ref on = bin(L, UAM_OP_AND, 1, ne(L, n, imm(L, 1)), ne(L, get(L, XT_LOOPEN), imm(L, 0)));
        wr(L, XT_LENDA, sel(L, on, imm(L, lend), imm(L, 0xFFFFFFFFu)));
        if (L->id == XTO_LOOP)
            return R_END;
        L->br_cond = L->id == XTO_LOOPNEZ ? eq(L, n, imm(L, 0)) : bin(L, UAM_OP_SLE, 32, n, imm(L, 0));
        L->br_target = imm(L, lend);
        return R_BRANCH;
    }
    /* ---- booleans ---- */
    case XTO_ANDB: case XTO_ANDBC: case XTO_ORB: case XTO_ORBC: case XTO_XORB: {
        uam_ref a = brbit(L, s), b2 = brbit(L, t);
        if (L->id == XTO_ANDBC || L->id == XTO_ORBC)
            b2 = un(L, UAM_OP_TRUNC, 1, bin(L, UAM_OP_XOR, 32, bool32(L, b2), imm(L, 1)));
        uam_opcode op = L->id == XTO_ANDB || L->id == XTO_ANDBC ? UAM_OP_AND
                        : L->id == XTO_XORB ? UAM_OP_XOR : UAM_OP_OR;
        set_brbit(L, r, bin(L, op, 1, a, b2));
        return R_NEXT;
    }
    case XTO_ANY4: case XTO_ALL4: case XTO_ANY8: case XTO_ALL8: {
        bool eight = L->id == XTO_ANY8 || L->id == XTO_ALL8;
        uint32_t m = eight ? 0xFF : 0xF;
        uam_ref g = and_(L, bin(L, UAM_OP_LSHR, 32, get(L, XT_BR), imm(L, s & (eight ? 8 : 12))), m);
        bool any = L->id == XTO_ANY4 || L->id == XTO_ANY8;
        set_brbit(L, t, any ? ne(L, g, imm(L, 0)) : eq(L, g, imm(L, m)));
        return R_NEXT;
    }
    /* ---- special and user registers ---- */
    case XTO_RSR: case XTO_WSR: case XTO_XSR: return lift_sr(L);
    case XTO_RUR: case XTO_WUR: return lift_ur(L);
    case XTO_RER:
        wr(L, XT_A(t), uam_helper(L->b, &xt_helpers, XT_H_ERI_RD, ar(L, s), NONE, NONE, NONE));
        return R_NEXT;
    case XTO_WER:
        uam_helper(L->b, &xt_helpers, XT_H_ERI_WR, ar(L, s), ar(L, t), NONE, NONE);
        return R_NEXT;
    /* ---- exceptions, interrupts, debug ---- */
    case XTO_ILL: case XTO_ILL_N: return illegal(L);
    case XTO_SYSCALL: trap(L, NONE, XT_EXC_SYSCALL, imm(L, 0)); return R_DONE;
    case XTO_BREAK: case XTO_BREAK_N:
        trap(L, NONE, XT_TRAP_BREAK, imm(L, (L->id == XTO_BREAK ? 8u : 16u) | L->len << 8));
        return R_DONE;
    case XTO_RSIL: {
        uam_ref old = uam_sysrd(L->b, 32, 230, NONE);
        uam_syswr(L->b, 32, XT_SYS_RSIL, imm(L, s), NONE);
        wr(L, XT_A(t), old);
        return R_END;
    }
    case XTO_WAITI:
        uam_syswr(L->b, 32, XT_SYS_WAITI, imm(L, s), NONE);
        uam_wait(L->b, imm(L, (uint32_t)L->pc + L->len));
        return R_DONE;
    /* ---- synchronisation ---- */
    case XTO_MEMW: case XTO_EXTW: uam_fence(L->b, UAM_FENCE_FULL); return R_NEXT;
    case XTO_ISYNC:
        uam_fence(L->b, UAM_FENCE_INSN);
        return R_END;
    case XTO_NOP: case XTO_NOP_N: case XTO_RSYNC: case XTO_ESYNC: case XTO_DSYNC: case XTO_EXCW:
    case XTO_IITLB: case XTO_IDTLB:
        return R_NEXT;
    /* ---- region protection ---- */
    case XTO_WITLB: case XTO_WDTLB:
        uam_helper(L->b, &xt_helpers, XT_H_TLB, imm(L, L->id), ar(L, t), ar(L, s), NONE);
        return R_END;
    case XTO_RITLB0: case XTO_RITLB1: case XTO_RDTLB0: case XTO_RDTLB1: case XTO_PITLB: case XTO_PDTLB:
        wr(L, XT_A(t), uam_helper(L->b, &xt_helpers, XT_H_TLB, imm(L, L->id), imm(L, 0), ar(L, s), NONE));
        return R_NEXT;
    default:
        break;
    }
    if (L->op->group == XTG_FP || L->op->group == XTG_FP_EXT)
        return lift_fp(L);
    if (is_mac16(L->id))
        return lift_mac16(L);
    if ((L->w & 15) == 7 || (L->w & 15) == 6 || L->id == XTO_BEQZ_N || L->id == XTO_BNEZ_N)
        return lift_branch(L);
    return illegal(L);
}

/* The loop-back after an instruction that falls through to `next`: taken when the loop
 * end in effect is there (ISA manual 4.3.2.4). */
static void loopback(lifter *L, uint64_t next)
{
    uam_ref la = get(L, XT_LENDA);
    uam_ref g = eq(L, la, imm(L, (uint32_t)next));
    uam_ref c1 = bin(L, UAM_OP_SUB, 32, get(L, XT_LCOUNT), imm(L, 1));
    uam_put(L->b, 32, XT_LCOUNT, c1, g);
    uam_put(L->b, 32, XT_LENDA, sel(L, eq(L, c1, imm(L, 0)), imm(L, 0xFFFFFFFFu), la), g);
    uam_exit(L->b, g, get(L, XT_LBEG), UAM_EXIT_JUMP);
}

uam_status xt_lift(uam_lift_ctx *lc)
{
    uam_cpu *cpu = lc->cpu;
    const xt_state *s = xt_st(cpu);
    lifter L = { .lc = lc, .b = lc->b, .cpu = cpu, .s = s, .cfg = &s->cfg };
    L.be = s->endian == UAM_ACC_BE;
    L.mem = s->endian | ((s->cfg.options & UAM_XT_OPT_UNALIGNED_EXC) ? UAM_ACC_ALIGNED : 0);
    L.wchk = NONE;
    bool step = cpu->priv & XT_PRIV_STEP;
    bool loops = s->cfg.options & UAM_XT_OPT_LOOPS;
    uint32_t lend = xt_reg(cpu, XT_LEND);

    /* Pass 1: the instructions of the block. */
    xt_insn ins[MAX_BLOCK];
    unsigned n = 0, max = step ? 1 : lc->max_insns < MAX_BLOCK ? lc->max_insns : MAX_BLOCK;
    uint64_t pc = lc->pc;
    bool at_lend = false;
    while (n < max) {
        if (!fetch_insn(&L, pc, &ins[n])) {
            if (n == 0)
                return UAM_ERR_FAULT;
            break;
        }
        uint64_t next = (pc + ins[n].len) & 0xFFFFFFFFu;
        bool end = ends_block(&s->cfg, &ins[n]);
        n++;
        pc = next;
        if (loops && !step && next == lend) {
            at_lend = true;
            break;
        }
        if (end)
            break;
    }
    uint64_t start = lc->pc, endpc = pc;

    /* Pass 2: emit. */
    for (unsigned i = 0; i < n; i++) {
        xt_insn *in = &ins[i];
        bool last = i == n - 1;
        L.pc = in->pc;
        L.w = in->w;
        L.len = in->len;
        L.nq = 0;
        L.br_cond = NONE;
        uam_imark(L.b, in->pc, in->len);
        if (i == 0) {
            if (step) {
                uam_syswr(L.b, 32, XT_SYS_STEP, imm(&L, 0), NONE);
            } else if (loops) {
                /* The loop end in effect must be where this block was lifted for. */
                uint32_t range = (uint32_t)(endpc - start) - (at_lend ? 1u : 0u);
                if (range) {
                    uam_ref d = bin(&L, UAM_OP_SUB, 32, get(&L, XT_LENDA), imm(&L, (uint32_t)start + 1));
                    uam_ref bad = bin(&L, UAM_OP_ULT, 32, d, imm(&L, range));
                    uam_syswr(L.b, 32, XT_SYS_STEP, imm(&L, 1), bad);
                    uam_exit(L.b, bad, imm(&L, (uint32_t)start), UAM_EXIT_JUMP);
                }
            }
        }
        lres res;
        if (!in->op) {
            res = illegal(&L);
        } else {
            L.op = in->op;
            L.id = in->op->id;
            L.r = (in->w >> 12) & 15;
            L.sf = (in->w >> 8) & 15;
            L.t = (in->w >> 4) & 15;
            L.op1 = (in->w >> 16) & 15;
            L.op2 = (in->w >> 20) & 15;
            /* Window overflow comes before everything the instruction does. ENTRY,
             * CALLn and CALLXn check their own way. */
            unsigned arf = in->op->arf, g = 0;
            if (arf && L.id != XTO_ENTRY && L.id != XTO_CALLX4 && L.id != XTO_CALLX8 && L.id != XTO_CALLX12) {
                if ((arf & 4) && (L.r >> 2) > g) g = L.r >> 2;
                if ((arf & 2) && (L.sf >> 2) > g) g = L.sf >> 2;
                if ((arf & 1) && (L.t >> 2) > g) g = L.t >> 2;
                if (g)
                    wcheck(&L, g);
            }
            res = lift_insn(&L);
        }
        uint64_t next = (in->pc + in->len) & 0xFFFFFFFFu;
        /* An instruction pass 1 let the block go on after may still end it (an
         * illegal register number, say): then the block ends there. */
        switch (res) {
        case R_DONE:
            return L.b->oom ? UAM_ERR_NOMEM : UAM_OK;
        case R_BRANCH:
            commit(&L);
            uam_exit(L.b, L.br_cond, L.br_target, UAM_EXIT_JUMP);
            break;
        case R_NEXT:
        case R_END:
            commit(&L);
            break;
        }
        if (!last && res == R_NEXT)
            continue;
        if (step || (last && at_lend))
            loopback(&L, next);
        uam_exit(L.b, NONE, imm(&L, (uint32_t)next), UAM_EXIT_JUMP);
        break;
    }
    return L.b->oom ? UAM_ERR_NOMEM : UAM_OK;
}
