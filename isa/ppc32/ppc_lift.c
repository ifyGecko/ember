/*
 * ppc_lift.c - 32-bit PowerPC decoder and lifter.
 *
 * Covers the user instruction set of 32-bit implementations: integer arithmetic
 * with carry and overflow (XER[CA], XER[OV], XER[SO]) and record forms (CR0),
 * compares, logical, rotate and shift instructions, the condition register
 * instructions, branches with the count register, traps, every integer load and
 * store form (update, indexed, byte-reversed, multiple, string), lwarx/stwcx.,
 * the cache and synchronisation instructions, the time base, and the
 * floating-point loads and stores (arithmetic goes to ppc_fpu.c through one
 * helper). Supervisor instructions (mfmsr/mtmsr, rfi, supervisor SPRs, segment
 * registers, tlbie/tlbia/tlbsync, dcbi) take the privileged-instruction program
 * exception in problem state; cpu->priv carries MSR[PR] and MSR[FP] so that this
 * is decided when the block is lifted. Anything else (64-bit instructions,
 * AltiVec, Book E) is an illegal instruction.
 *
 * The CPU model decides the rest: the classic MMU instructions (mfsr, mtsr,
 * mfsrin, mtsrin, tlbie, tlbia) exist only in the classic model, and the Book E
 * ones (rfci, rfmci, rfdi, wrtee, wrteei, tlbwe, tlbre, tlbsx, tlbivax, mfdcr,
 * mtdcr, mfdcrx, mtdcrx, icbt) only in Book E; isel where the model has it.
 *
 * Instruction fields are extracted with C shifts; comments use the PowerPC
 * books' bit numbering (bit 0 is the most significant).
 */
#include "ppc.h"

#define NONE UAM_REF_NONE

#define OPCD(i)  ((i) >> 26)
#define RD(i)    (((i) >> 21) & 31u)   /* also rS, frD, BO, TO, crbD */
#define RA(i)    (((i) >> 16) & 31u)   /* also BI, crbA */
#define RB(i)    (((i) >> 11) & 31u)   /* also SH, NB, crbB */
#define XO10(i)  (((i) >> 1) & 0x3FFu)
#define XO9(i)   (((i) >> 1) & 0x1FFu)
#define OE(i)    (((i) >> 10) & 1u)
#define RCB(i)   ((i) & 1u)
#define SIMM(i)  ((uint32_t)(int32_t)(int16_t)(i))
#define UIMM(i)  ((i) & 0xFFFFu)
#define CRFD(i)  (((i) >> 23) & 7u)
#define CRFS(i)  (((i) >> 18) & 7u)
#define MB(i)    (((i) >> 6) & 31u)
#define ME(i)    (((i) >> 1) & 31u)
#define SPRN(i)  ((((i) >> 16) & 31u) | (((i) >> 6) & 0x3E0u))

typedef enum { LIFT_SEQ, LIFT_DONE } lift_result;

typedef struct pl {
    uam_lift_ctx    *lc;
    uam_builder     *b;
    const ppc_state *s;
    uint32_t         pc;
    uint32_t         raw;
    unsigned         priv;
} pl;

/* ---- operand helpers --------------------------------------------------- */

static uam_ref imm(pl *L, uint64_t v) { return uam_imm(L->b, v); }
static uint64_t immval(pl *L, uam_ref r) { return L->b->imm[uam_ref_index(r)]; }

static uam_ref bin(pl *L, uam_opcode op, uam_ref a, uam_ref b) { return uam_binop(L->b, op, 32, a, b); }
static uam_ref bin8(pl *L, uam_opcode op, uam_ref a, uam_ref b) { return uam_binop(L->b, op, 8, a, b); }
static uam_ref bool_or(pl *L, uam_ref a, uam_ref b) { return uam_binop(L->b, UAM_OP_OR, 1, a, b); }
static uam_ref bool_and(pl *L, uam_ref a, uam_ref b) { return uam_binop(L->b, UAM_OP_AND, 1, a, b); }
static uam_ref notv(pl *L, uam_ref a) { return bin(L, UAM_OP_XOR, a, imm(L, 0xFFFFFFFFu)); }

/* Width conversions; conversions need a temporary source, so immediates fold here. */
static uam_ref zx(pl *L, unsigned w, uam_ref v)
{
    if (uam_ref_is_imm(v))
        return imm(L, immval(L, v));
    return uam_unop(L->b, UAM_OP_ZEXT, w, v);
}

static uam_ref tr(pl *L, unsigned w, uam_ref v)
{
    if (uam_ref_is_imm(v))
        return imm(L, immval(L, v) & uam_mask(w));
    return uam_unop(L->b, UAM_OP_TRUNC, w, v);
}

/* Sign-extend the low `from` bits of a 32-bit value. */
static uam_ref sx_from(pl *L, uam_ref v, unsigned from)
{
    if (uam_ref_is_imm(v))
        return imm(L, (uint32_t)uam_sext(immval(L, v), from));
    return uam_unop(L->b, UAM_OP_SEXT, 32, uam_unop(L->b, UAM_OP_TRUNC, from, v));
}

static uam_ref gpr(pl *L, unsigned r) { return uam_get(L->b, 32, PPC_GPR_OFF(r)); }
static uam_ref gpr0(pl *L, unsigned r) { return r ? gpr(L, r) : imm(L, 0); }   /* (rA|0) */
static void gpr_w(pl *L, unsigned r, uam_ref v) { uam_put(L->b, 32, PPC_GPR_OFF(r), v, NONE); }

static uam_ref get8(pl *L, uint32_t off) { return uam_get(L->b, 8, off); }
static void put8(pl *L, uint32_t off, uam_ref v) { uam_put(L->b, 8, off, v, NONE); }

static void trap(pl *L, uam_ref guard, uint32_t cause, uam_ref tval)
{
    uam_trap(L->b, guard, UAM_TRAP_EXCEPTION, imm(L, cause), tval);
}

static lift_result illegal(pl *L)
{
    trap(L, NONE, PPC_TRAP_ILLEGAL, imm(L, L->raw));
    return LIFT_DONE;
}

static bool booke(const pl *L) { return ppc_booke(L->s); }

/* True (after emitting the exception) when the instruction is privileged here. */
static bool privileged(pl *L)
{
    if (!(L->priv & PPC_PRIV_PR))
        return false;
    trap(L, NONE, PPC_TRAP_PRIV, imm(L, L->raw));
    return true;
}

static uam_ref helper(pl *L, unsigned id, uam_ref a, uam_ref b)
{
    return uam_helper(L->b, &L->lc->cpu->isa->helpers, id, a, b, NONE, NONE);
}

/* ---- XER and CR -------------------------------------------------------- */

/* A CR field value: LT, GT or EQ of a against b, plus `so` (an 8-bit 0/1). */
static uam_ref cmp_field(pl *L, uam_ref a, uam_ref b, bool sign, uam_ref so)
{
    uam_opcode lt = sign ? UAM_OP_SLT : UAM_OP_ULT;
    uam_ref l = bin(L, lt, a, b), g = bin(L, lt, b, a);
    uam_ref v = uam_select(L->b, 8, l, imm(L, CR_LT), uam_select(L->b, 8, g, imm(L, CR_GT), imm(L, CR_EQ)));
    return bin8(L, UAM_OP_OR, v, so);
}

/* Rc = 1: CR0 from a result and XER[SO] (read after any OE update). */
static void record(pl *L, uam_ref r)
{
    put8(L, PPC_CR_OFF(0), cmp_field(L, r, imm(L, 0), true, get8(L, PPC_SO_OFF)));
}

/* OE = 1: XER[OV] = ov, XER[SO] |= ov. */
static void set_ov(pl *L, uam_ref ov)
{
    uam_ref o8 = zx(L, 8, ov);
    put8(L, PPC_OV_OFF, o8);
    put8(L, PPC_SO_OFF, bin8(L, UAM_OP_OR, get8(L, PPC_SO_OFF), o8));
}

static void set_ca(pl *L, uam_ref ca) { put8(L, PPC_CA_OFF, zx(L, 8, ca)); }

static uam_ref ca32(pl *L) { return zx(L, 32, get8(L, PPC_CA_OFF)); }

static uam_ref xer_read(pl *L)
{
    uam_ref so = bin(L, UAM_OP_SHL, zx(L, 32, get8(L, PPC_SO_OFF)), imm(L, 31));
    uam_ref ov = bin(L, UAM_OP_SHL, zx(L, 32, get8(L, PPC_OV_OFF)), imm(L, 30));
    uam_ref ca = bin(L, UAM_OP_SHL, ca32(L), imm(L, 29));
    uam_ref bc = zx(L, 32, get8(L, PPC_BC_OFF));
    return bin(L, UAM_OP_OR, bin(L, UAM_OP_OR, so, ov), bin(L, UAM_OP_OR, ca, bc));
}

static void xer_write(pl *L, uam_ref v)
{
    put8(L, PPC_SO_OFF, tr(L, 8, bin(L, UAM_OP_AND, bin(L, UAM_OP_LSHR, v, imm(L, 31)), imm(L, 1))));
    put8(L, PPC_OV_OFF, tr(L, 8, bin(L, UAM_OP_AND, bin(L, UAM_OP_LSHR, v, imm(L, 30)), imm(L, 1))));
    put8(L, PPC_CA_OFF, tr(L, 8, bin(L, UAM_OP_AND, bin(L, UAM_OP_LSHR, v, imm(L, 29)), imm(L, 1))));
    put8(L, PPC_BC_OFF, tr(L, 8, bin(L, UAM_OP_AND, v, imm(L, 0x7F))));
}

/* CR bit `bi` (0 = CR0[LT]) as a 1-bit value. */
static uam_ref cr_bit(pl *L, unsigned bi)
{
    uam_ref f = get8(L, PPC_CR_OFF(bi >> 2));
    return bin8(L, UAM_OP_NE, bin8(L, UAM_OP_AND, f, imm(L, 8u >> (bi & 3))), imm(L, 0));
}

static uam_ref cr_read(pl *L)
{
    uam_ref cr = imm(L, 0);
    for (unsigned f = 0; f < 8; f++) {
        uam_ref v = zx(L, 32, get8(L, PPC_CR_OFF(f)));
        cr = bin(L, UAM_OP_OR, cr, f == 7 ? v : bin(L, UAM_OP_SHL, v, imm(L, 28 - 4 * f)));
    }
    return cr;
}

/* ---- arithmetic -------------------------------------------------------- */

/* r = a + b + cin (cin a 32-bit 0/1); carry out and signed overflow as 1-bit values. */
static uam_ref add3(pl *L, uam_ref a, uam_ref b, uam_ref cin, uam_ref *carry, uam_ref *ov)
{
    uam_ref s = bin(L, UAM_OP_ADD, a, b);
    if (uam_ref_is_imm(cin) && immval(L, cin) == 0) {
        if (carry) *carry = bin(L, UAM_OP_CARRY, a, b);
        if (ov)    *ov = bin(L, UAM_OP_SCARRY, a, b);
        return s;
    }
    uam_ref r = bin(L, UAM_OP_ADD, s, cin);
    if (carry)
        *carry = bool_or(L, bin(L, UAM_OP_CARRY, a, b), bin(L, UAM_OP_CARRY, s, cin));
    if (ov) /* the sign of the result differs from both operands' signs */
        *ov = bin(L, UAM_OP_SLT, bin(L, UAM_OP_AND, bin(L, UAM_OP_XOR, a, r), bin(L, UAM_OP_XOR, b, r)),
                  imm(L, 0));
    return r;
}

/* The XO-form add/subtract family. */
static lift_result lift_addsub(pl *L, uint32_t i, unsigned xo)
{
    uam_ref a = gpr(L, RA(i)), b = gpr(L, RB(i)), cin, ca = NONE, ov = NONE;
    bool sets_ca = true;
    switch (xo) {
    case 266: cin = imm(L, 0); sets_ca = false; break;                      /* add */
    case 10:  cin = imm(L, 0); break;                                       /* addc */
    case 138: cin = ca32(L); break;                                         /* adde */
    case 234: b = imm(L, 0xFFFFFFFFu); cin = ca32(L); break;                /* addme */
    case 202: b = imm(L, 0); cin = ca32(L); break;                          /* addze */
    case 40:  a = notv(L, a); cin = imm(L, 1); sets_ca = false; break;      /* subf */
    case 8:   a = notv(L, a); cin = imm(L, 1); break;                       /* subfc */
    case 136: a = notv(L, a); cin = ca32(L); break;                         /* subfe */
    case 232: a = notv(L, a); b = imm(L, 0xFFFFFFFFu); cin = ca32(L); break; /* subfme */
    case 200: a = notv(L, a); b = imm(L, 0); cin = ca32(L); break;          /* subfze */
    default:  a = notv(L, a); b = imm(L, 0); cin = imm(L, 1); sets_ca = false; break; /* neg */
    }
    uam_ref r = add3(L, a, b, cin, sets_ca ? &ca : NULL, OE(i) ? &ov : NULL);
    gpr_w(L, RD(i), r);
    if (sets_ca)
        set_ca(L, ca);
    if (OE(i))
        set_ov(L, ov);
    if (RCB(i))
        record(L, r);
    return LIFT_SEQ;
}

static lift_result lift_muldiv(pl *L, uint32_t i, unsigned xo)
{
    uam_ref a = gpr(L, RA(i)), b = gpr(L, RB(i)), r, ov = NONE;
    switch (xo) {
    case 235: /* mullw: overflow when the high word is not the low word's sign */
        r = bin(L, UAM_OP_MUL, a, b);
        if (OE(i))
            ov = bin(L, UAM_OP_NE, bin(L, UAM_OP_MULHS, a, b), bin(L, UAM_OP_ASHR, r, imm(L, 31)));
        break;
    case 75:  r = bin(L, UAM_OP_MULHS, a, b); break;   /* mulhw */
    case 11:  r = bin(L, UAM_OP_MULHU, a, b); break;   /* mulhwu */
    case 491: /* divw; the quotient of an overflowing division follows the IR (undefined here) */
        r = bin(L, UAM_OP_SDIV, a, b);
        if (OE(i))
            ov = bool_or(L, bin(L, UAM_OP_EQ, b, imm(L, 0)),
                         bool_and(L, bin(L, UAM_OP_EQ, a, imm(L, 0x80000000u)),
                                  bin(L, UAM_OP_EQ, b, imm(L, 0xFFFFFFFFu))));
        break;
    default:  /* divwu */
        r = bin(L, UAM_OP_UDIV, a, b);
        if (OE(i))
            ov = bin(L, UAM_OP_EQ, b, imm(L, 0));
        break;
    }
    gpr_w(L, RD(i), r);
    if (OE(i) && xo != 75 && xo != 11)
        set_ov(L, ov);
    if (RCB(i))
        record(L, r);
    return LIFT_SEQ;
}

/* ---- rotate and shift -------------------------------------------------- */

static uint32_t rot_mask(unsigned mb, unsigned me)
{
    uint32_t a = 0xFFFFFFFFu >> mb, b = 0xFFFFFFFFu << (31 - me);
    return mb <= me ? (a & b) : (a | b);
}

static uam_ref rotl(pl *L, uam_ref v, uam_ref n)
{
    return bin(L, UAM_OP_ROTR, v, bin(L, UAM_OP_SUB, imm(L, 32), n));
}

static lift_result lift_rotate(pl *L, uint32_t i)
{
    uint32_t m = rot_mask(MB(i), ME(i));
    uam_ref rs = gpr(L, RD(i)), r;
    switch (OPCD(i)) {
    case 20: { /* rlwimi */
        uam_ref rot = rotl(L, rs, imm(L, RB(i)));
        r = bin(L, UAM_OP_OR, bin(L, UAM_OP_AND, rot, imm(L, m)),
                bin(L, UAM_OP_AND, gpr(L, RA(i)), imm(L, ~m)));
        break;
    }
    case 21: /* rlwinm */
        r = bin(L, UAM_OP_AND, rotl(L, rs, imm(L, RB(i))), imm(L, m));
        break;
    default: /* rlwnm */
        r = bin(L, UAM_OP_AND, rotl(L, rs, bin(L, UAM_OP_AND, gpr(L, RB(i)), imm(L, 31))), imm(L, m));
        break;
    }
    gpr_w(L, RA(i), r);
    if (RCB(i))
        record(L, r);
    return LIFT_SEQ;
}

/* sraw / srawi: CA is set when the source is negative and 1 bits were shifted out. */
static void lift_sra(pl *L, uint32_t i, uam_ref n)
{
    uam_ref rs = gpr(L, RD(i));
    uam_ref r = bin(L, UAM_OP_ASHR, rs, n);
    uam_ref lost = notv(L, bin(L, UAM_OP_SHL, imm(L, 0xFFFFFFFFu), n));
    uam_ref ca = bool_and(L, bin(L, UAM_OP_SLT, rs, imm(L, 0)),
                          bin(L, UAM_OP_NE, bin(L, UAM_OP_AND, rs, lost), imm(L, 0)));
    gpr_w(L, RA(i), r);
    set_ca(L, ca);
    if (RCB(i))
        record(L, r);
}

/* ---- loads and stores -------------------------------------------------- */

static uam_ref ea_d(pl *L, uint32_t i)
{
    return RA(i) ? bin(L, UAM_OP_ADD, gpr(L, RA(i)), imm(L, SIMM(i))) : imm(L, SIMM(i));
}

static uam_ref ea_x(pl *L, uint32_t i)
{
    return RA(i) ? bin(L, UAM_OP_ADD, gpr(L, RA(i)), gpr(L, RB(i))) : gpr(L, RB(i));
}

/* Load w bits into rD (sign-extended if `sign`), then rA = EA for update forms. */
static lift_result load(pl *L, uint32_t i, uam_ref ea, unsigned w, bool sign, bool update, unsigned order)
{
    if (update && RA(i) == 0)
        return illegal(L);
    uam_ref v = uam_ld(L->b, w, order, ea, NONE);
    if (w < 32)
        v = uam_unop(L->b, sign ? UAM_OP_SEXT : UAM_OP_ZEXT, 32, v);
    gpr_w(L, RD(i), v);
    if (update)
        gpr_w(L, RA(i), ea);
    return LIFT_SEQ;
}

static lift_result store(pl *L, uint32_t i, uam_ref ea, unsigned w, bool update, unsigned order)
{
    if (update && RA(i) == 0)
        return illegal(L);
    uam_ref v = gpr(L, RD(i));
    uam_st(L->b, w, order, ea, w < 32 ? tr(L, w, v) : v, NONE);
    if (update)
        gpr_w(L, RA(i), ea);
    return LIFT_SEQ;
}

/* lmw / stmw: registers rD..r31 from or to consecutive words (word aligned). */
static lift_result lift_multiple(pl *L, uint32_t i, bool st)
{
    uam_ref ea = ea_d(L, i);
    unsigned r0 = RD(i);
    uam_ref v[32];
    for (unsigned r = r0; r < 32; r++) {
        uam_ref a = r == r0 ? ea : bin(L, UAM_OP_ADD, ea, imm(L, 4 * (r - r0)));
        if (st)
            uam_st(L->b, 32, UAM_ACC_BE | UAM_ACC_ALIGNED, a, gpr(L, r), NONE);
        else
            v[r] = uam_ld(L->b, 32, UAM_ACC_BE | UAM_ACC_ALIGNED, a, NONE);
    }
    if (!st)
        for (unsigned r = r0; r < 32; r++)
            gpr_w(L, r, v[r]);
    return LIFT_SEQ;
}

/* lswi / stswi: nb bytes from or to registers rD, rD+1, ... (wrapping to r0), four
 * bytes per register from its most significant end; a load zeroes the bytes of the
 * last register it does not reach. */
static lift_result lift_string_imm(pl *L, uint32_t i, bool st)
{
    unsigned nb = RB(i) ? RB(i) : 32;
    uam_ref ea = gpr0(L, RA(i));
    uam_ref acc[8];
    unsigned nregs = (nb + 3) / 4;
    for (unsigned k = 0; k < nb; k++) {
        unsigned r = (RD(i) + k / 4) & 31, sh = 24 - 8 * (k & 3);
        uam_ref a = k ? bin(L, UAM_OP_ADD, ea, imm(L, k)) : ea;
        if (st) {
            uam_ref byte = bin(L, UAM_OP_AND, bin(L, UAM_OP_LSHR, gpr(L, r), imm(L, sh)), imm(L, 0xFF));
            uam_st(L->b, 8, UAM_ACC_BE, a, tr(L, 8, byte), NONE);
        } else {
            uam_ref byte = bin(L, UAM_OP_SHL, zx(L, 32, uam_ld(L->b, 8, UAM_ACC_BE, a, NONE)), imm(L, sh));
            acc[k / 4] = (k & 3) ? bin(L, UAM_OP_OR, acc[k / 4], byte) : byte;
        }
    }
    if (!st)
        for (unsigned n = 0; n < nregs; n++)
            gpr_w(L, (RD(i) + n) & 31, acc[n]);
    return LIFT_SEQ;
}

/* dcbz: clear the cache block that contains EA. */
static lift_result lift_dcbz(pl *L, uint32_t i)
{
    unsigned line = L->s->cfg.cache_block;
    uam_ref base = bin(L, UAM_OP_AND, ea_x(L, i), imm(L, ~(uint32_t)(line - 1)));
    for (unsigned k = 0; k < line; k += 8)
        uam_st(L->b, 64, UAM_ACC_BE, k ? bin(L, UAM_OP_ADD, base, imm(L, k)) : base, imm(L, 0), NONE);
    return LIFT_SEQ;
}

/* ---- floating point ---------------------------------------------------- */

/* False (after emitting the exception) when FP instructions cannot run here. */
static bool fp_usable(pl *L)
{
    if (!L->s->cfg.fpu) {
        illegal(L);
        return false;
    }
    if (!(L->priv & PPC_PRIV_FP)) {
        trap(L, NONE, PPC_TRAP_FPU, imm(L, 0));
        return false;
    }
    return true;
}

static uam_ref fpr(pl *L, unsigned r) { return uam_get(L->b, 64, PPC_FPR_OFF(r)); }
static void fpr_w(pl *L, unsigned r, uam_ref v) { uam_put(L->b, 64, PPC_FPR_OFF(r), v, NONE); }

/* lfs/lfd and their update and indexed forms. */
static lift_result fp_load(pl *L, uint32_t i, uam_ref ea, bool dbl, bool update)
{
    if (!fp_usable(L))
        return LIFT_DONE;
    if (update && RA(i) == 0)
        return illegal(L);
    uam_ref v = uam_ld(L->b, dbl ? 64 : 32, UAM_ACC_BE, ea, NONE);
    if (!dbl)
        v = helper(L, PPC_HELPER_SP2DP, v, NONE);
    fpr_w(L, RD(i), v);
    if (update)
        gpr_w(L, RA(i), ea);
    return LIFT_SEQ;
}

/* stfs/stfd/stfiwx and their update and indexed forms. */
static lift_result fp_store(pl *L, uint32_t i, uam_ref ea, unsigned kind, bool update)
{
    if (!fp_usable(L))
        return LIFT_DONE;
    if (update && RA(i) == 0)
        return illegal(L);
    uam_ref v = fpr(L, RD(i));
    if (kind == 0)          /* single */
        uam_st(L->b, 32, UAM_ACC_BE, ea, helper(L, PPC_HELPER_DP2SP, v, NONE), NONE);
    else if (kind == 1)     /* double */
        uam_st(L->b, 64, UAM_ACC_BE, ea, v, NONE);
    else                    /* stfiwx: the low word */
        uam_st(L->b, 32, UAM_ACC_BE, ea, uam_unop(L->b, UAM_OP_TRUNC, 32, v), NONE);
    if (update)
        gpr_w(L, RA(i), ea);
    return LIFT_SEQ;
}

/* Opcodes 59 and 63. Moves without Rc are plain bit operations; everything else
 * (arithmetic, rounding, conversions, compares, FPSCR) runs in the FP helper. */
static lift_result lift_fp_arith(pl *L, uint32_t i)
{
    unsigned xo = XO10(i);
    bool a_form = (xo & 0x1F) >= 16;
    if (OPCD(i) == 59) {
        switch (xo & 0x1F) {
        case 18: case 20: case 21: case 22: case 24: case 25: case 28: case 29: case 30: case 31:
            if (a_form)
                break;
            /* fall through */
        default:
            return illegal(L);
        }
    } else if (a_form) {
        switch (xo & 0x1F) {
        case 18: case 20: case 21: case 22: case 23: case 25: case 26: case 28: case 29: case 30: case 31:
            break;
        default:
            return illegal(L);
        }
    } else {
        switch (xo) {
        case 0: case 12: case 14: case 15: case 32: case 38: case 64: case 70: case 134: case 583:
        case 711: case 40: case 72: case 136: case 264:
            break;
        default:
            return illegal(L);
        }
    }
    if (!fp_usable(L))
        return LIFT_DONE;
    if (OPCD(i) == 63 && !a_form && !RCB(i) && (xo == 40 || xo == 72 || xo == 136 || xo == 264)) {
        uam_ref v = fpr(L, RB(i));
        const uint64_t sign = UINT64_C(1) << 63;
        switch (xo) {
        case 40:  v = uam_binop(L->b, UAM_OP_XOR, 64, v, imm(L, sign)); break;   /* fneg */
        case 136: v = uam_binop(L->b, UAM_OP_OR, 64, v, imm(L, sign)); break;    /* fnabs */
        case 264: v = uam_binop(L->b, UAM_OP_AND, 64, v, imm(L, ~sign)); break;  /* fabs */
        default:  break;                                                           /* fmr */
        }
        fpr_w(L, RD(i), v);
        return LIFT_SEQ;
    }
    helper(L, PPC_HELPER_FP, imm(L, i), NONE);
    return LIFT_SEQ;
}

/* ---- branches ---------------------------------------------------------- */

/* The condition of a bc/bclr/bcctr, decrementing CTR when BO says so. NONE means
 * always taken. */
static uam_ref branch_cond(pl *L, unsigned bo, unsigned bi, bool may_decrement)
{
    uam_ref c = NONE;
    if (!(bo & 4) && may_decrement) {
        uam_ref ctr = bin(L, UAM_OP_SUB, uam_get(L->b, 32, PPC_CTR_OFF), imm(L, 1));
        uam_put(L->b, 32, PPC_CTR_OFF, ctr, NONE);
        c = bin(L, (bo & 2) ? UAM_OP_EQ : UAM_OP_NE, ctr, imm(L, 0));
    }
    if (!(bo & 16)) {
        uam_ref bit = cr_bit(L, bi);
        if (!(bo & 8))
            bit = uam_binop(L->b, UAM_OP_XOR, 1, bit, imm(L, 1));
        c = uam_ref_is_none(c) ? bit : bool_and(L, c, bit);
    }
    return c;
}

static lift_result finish_branch(pl *L, uam_ref cond, uam_ref target, uam_exit_kind kind)
{
    if (uam_ref_is_none(cond)) {
        uam_exit(L->b, NONE, target, kind);
    } else {
        uam_exit(L->b, cond, target, kind);
        uam_exit(L->b, NONE, imm(L, L->pc + 4), UAM_EXIT_JUMP);
    }
    return LIFT_DONE;
}

static lift_result lift_b(pl *L, uint32_t i)
{
    uint32_t li = (uint32_t)((int32_t)(i << 6) >> 6) & ~3u;
    uint32_t tgt = ((i & 2) ? 0 : L->pc) + li;
    if (i & 1)
        uam_put(L->b, 32, PPC_LR_OFF, imm(L, L->pc + 4), NONE);
    uam_exit(L->b, NONE, imm(L, tgt), (i & 1) ? UAM_EXIT_CALL : UAM_EXIT_JUMP);
    return LIFT_DONE;
}

static lift_result lift_bc(pl *L, uint32_t i)
{
    uint32_t bd = SIMM(i & 0xFFFCu);
    uint32_t tgt = ((i & 2) ? 0 : L->pc) + bd;
    uam_ref c = branch_cond(L, RD(i), RA(i), true);
    if (i & 1)
        uam_put(L->b, 32, PPC_LR_OFF, imm(L, L->pc + 4), NONE);
    return finish_branch(L, c, imm(L, tgt), (i & 1) ? UAM_EXIT_CALL : UAM_EXIT_JUMP);
}

/* bclr (to LR, read before LK replaces it) and bcctr (no CTR decrement). */
static lift_result lift_bcreg(pl *L, uint32_t i, bool to_lr)
{
    uam_ref tgt = bin(L, UAM_OP_AND, uam_get(L->b, 32, to_lr ? PPC_LR_OFF : PPC_CTR_OFF), imm(L, ~3u));
    uam_ref c = branch_cond(L, RD(i), RA(i), to_lr);
    if (i & 1)
        uam_put(L->b, 32, PPC_LR_OFF, imm(L, L->pc + 4), NONE);
    uam_exit_kind kind = (i & 1) ? UAM_EXIT_CALL : (to_lr && (RD(i) & 0x14) == 0x14) ? UAM_EXIT_RET
                                                                                   : UAM_EXIT_JUMP;
    return finish_branch(L, c, tgt, kind);
}

/* ---- opcode 19: CR logic, branches to registers, rfi, isync ----------- */

static lift_result lift_cr_logic(pl *L, uint32_t i, unsigned xo)
{
    uam_ref a = cr_bit(L, RA(i)), b = cr_bit(L, RB(i)), r;
    switch (xo) {
    case 257: r = bool_and(L, a, b); break;                                              /* crand */
    case 449: r = bool_or(L, a, b); break;                                               /* cror */
    case 193: r = uam_binop(L->b, UAM_OP_XOR, 1, a, b); break;                           /* crxor */
    case 225: r = uam_binop(L->b, UAM_OP_XOR, 1, bool_and(L, a, b), imm(L, 1)); break;   /* crnand */
    case 33:  r = uam_binop(L->b, UAM_OP_XOR, 1, bool_or(L, a, b), imm(L, 1)); break;    /* crnor */
    case 289: r = uam_binop(L->b, UAM_OP_EQ, 1, a, b); break;                            /* creqv */
    case 129: r = bool_and(L, a, uam_binop(L->b, UAM_OP_XOR, 1, b, imm(L, 1))); break;   /* crandc */
    default:  r = bool_or(L, a, uam_binop(L->b, UAM_OP_XOR, 1, b, imm(L, 1))); break;    /* crorc */
    }
    unsigned d = RD(i), bit = 8u >> (d & 3);
    uam_ref f = get8(L, PPC_CR_OFF(d >> 2));
    uam_ref nv = bin8(L, UAM_OP_OR, bin8(L, UAM_OP_AND, f, imm(L, ~bit & 0xFFu)),
                      uam_select(L->b, 8, r, imm(L, bit), imm(L, 0)));
    put8(L, PPC_CR_OFF(d >> 2), nv);
    return LIFT_SEQ;
}

static lift_result lift_op19(pl *L, uint32_t i)
{
    unsigned xo = XO10(i);
    switch (xo) {
    case 0: /* mcrf */
        put8(L, PPC_CR_OFF(CRFD(i)), get8(L, PPC_CR_OFF(CRFS(i))));
        return LIFT_SEQ;
    case 16:  return lift_bcreg(L, i, true);     /* bclr */
    case 528: return lift_bcreg(L, i, false);    /* bcctr */
    case 33: case 129: case 193: case 225: case 257: case 289: case 417: case 449:
        return lift_cr_logic(L, i, xo);
    case 50: /* rfi */
        if (privileged(L))
            return LIFT_DONE;
        uam_exit(L->b, NONE, helper(L, PPC_HELPER_RFI, NONE, NONE), UAM_EXIT_EXC_RETURN);
        return LIFT_DONE;
    case 51: case 38: case 39: { /* rfci, rfmci, rfdi (Book E) */
        const uam_ppc_booke_config *c = &L->s->cfg.booke;
        if (!booke(L) || (xo == 38 && !c->mcheck_mcsrr) || (xo == 39 && !c->debug_dsrr))
            return illegal(L);
        if (privileged(L))
            return LIFT_DONE;
        unsigned h = xo == 51 ? PPC_HELPER_RFCI : xo == 38 ? PPC_HELPER_RFMCI : PPC_HELPER_RFDI;
        uam_exit(L->b, NONE, helper(L, h, NONE, NONE), UAM_EXIT_EXC_RETURN);
        return LIFT_DONE;
    }
    case 150: /* isync: context synchronising */
        uam_fence(L->b, UAM_FENCE_INSN);
        uam_exit(L->b, NONE, imm(L, L->pc + 4), UAM_EXIT_JUMP);
        return LIFT_DONE;
    default:
        return illegal(L);
    }
}

/* ---- opcode 31 --------------------------------------------------------- */

static lift_result lift_tw(pl *L, unsigned to, uam_ref a, uam_ref b)
{
    if (to == 0)
        return LIFT_SEQ;
    if ((to & 0x1C) == 0x1C || (to & 0x07) == 0x07 || to == 0x1F) {   /* always */
        trap(L, NONE, PPC_TRAP_TRAP, imm(L, L->raw));
        return LIFT_DONE;
    }
    uam_ref c = NONE;
    const struct { unsigned bit; uam_opcode op; bool swap; } t[] = {
        { 16, UAM_OP_SLT, false }, { 8, UAM_OP_SLT, true }, { 4, UAM_OP_EQ, false },
        { 2, UAM_OP_ULT, false }, { 1, UAM_OP_ULT, true },
    };
    for (unsigned k = 0; k < UAM_ARRAY_LEN(t); k++) {
        if (!(to & t[k].bit))
            continue;
        uam_ref x = t[k].swap ? bin(L, t[k].op, b, a) : bin(L, t[k].op, a, b);
        c = uam_ref_is_none(c) ? x : bool_or(L, c, x);
    }
    trap(L, c, PPC_TRAP_TRAP, imm(L, L->raw));
    return LIFT_SEQ;
}

static lift_result lift_mfspr(pl *L, uint32_t i, unsigned n)
{
    uam_ref v;
    switch (n) {
    case SPR_XER: v = xer_read(L); break;
    case SPR_LR:  v = uam_get(L->b, 32, PPC_LR_OFF); break;
    case SPR_CTR: v = uam_get(L->b, 32, PPC_CTR_OFF); break;
    default:
        if (SPR_PRIVILEGED(n) && privileged(L))
            return LIFT_DONE;
        v = uam_sysrd(L->b, 32, n, NONE);
        break;
    }
    gpr_w(L, RD(i), v);
    return LIFT_SEQ;
}

static lift_result lift_mtspr(pl *L, uint32_t i, unsigned n)
{
    uam_ref v = gpr(L, RD(i));
    switch (n) {
    case SPR_XER: xer_write(L, v); return LIFT_SEQ;
    case SPR_LR:  uam_put(L->b, 32, PPC_LR_OFF, v, NONE); return LIFT_SEQ;
    case SPR_CTR: uam_put(L->b, 32, PPC_CTR_OFF, v, NONE); return LIFT_SEQ;
    default:
        if (SPR_PRIVILEGED(n) && privileged(L))
            return LIFT_DONE;
        /* May change translation, the decrementer or anything a board models. */
        uam_syswr(L->b, 32, n, v, NONE);
        uam_exit(L->b, NONE, imm(L, L->pc + 4), UAM_EXIT_JUMP);
        return LIFT_DONE;
    }
}

static lift_result lift_mtmsr(pl *L, uint32_t i)
{
    if (privileged(L))
        return LIFT_DONE;
    uam_ref v = gpr(L, RD(i));
    if (i & 0x00010000u) /* L = 1: only EE and RI */
        v = bin(L, UAM_OP_OR, bin(L, UAM_OP_AND, uam_sysrd(L->b, 32, PPC_SR_MSR, NONE),
                                  imm(L, ~(MSR_EE | MSR_RI))),
                bin(L, UAM_OP_AND, v, imm(L, MSR_EE | MSR_RI)));
    uam_syswr(L->b, 32, PPC_SR_MSR, v, NONE);
    /* MSR[POW]: the processor sleeps until an exception (the power-saving mode
     * itself is the implementation's; any interrupt wakes it). */
    uam_ref pow = bin(L, UAM_OP_NE, bin(L, UAM_OP_AND, v, imm(L, MSR_POW)), imm(L, 0));
    uam_exit(L->b, uam_binop(L->b, UAM_OP_XOR, 1, pow, imm(L, 1)), imm(L, L->pc + 4), UAM_EXIT_JUMP);
    uam_wait(L->b, imm(L, L->pc + 4));
    return LIFT_DONE;
}

/* wrtee / wrteei: MSR[EE] from a register or the instruction. */
static lift_result lift_wrtee(pl *L, uam_ref ee)
{
    if (privileged(L))
        return LIFT_DONE;
    uam_ref msr = uam_sysrd(L->b, 32, PPC_SR_MSR, NONE);
    uam_syswr(L->b, 32, PPC_SR_MSR, bin(L, UAM_OP_OR, bin(L, UAM_OP_AND, msr, imm(L, ~MSR_EE)),
                                        bin(L, UAM_OP_AND, ee, imm(L, MSR_EE))), NONE);
    uam_exit(L->b, NONE, imm(L, L->pc + 4), UAM_EXIT_JUMP);
    return LIFT_DONE;
}

/* The Book E instructions of opcode 31 (LIFT_SEQ/DONE), or -1 if xo is none of them. */
static int lift_op31_booke(pl *L, uint32_t i, unsigned xo)
{
    switch (xo) {
    case 131: return lift_wrtee(L, gpr(L, RD(i)));                       /* wrtee */
    case 163: return lift_wrtee(L, imm(L, i & MSR_EE));                  /* wrteei */
    case 22:  return LIFT_SEQ;                                           /* icbt */
    case 978: case 946: case 914: case 786: {  /* tlbwe tlbre tlbsx tlbivax */
        if (privileged(L))
            return LIFT_DONE;
        if (xo == 978) {
            helper(L, PPC_HELPER_TLBWE, imm(L, i), NONE);
        } else if (xo == 946) {
            helper(L, PPC_HELPER_TLBRE, imm(L, i), NONE);
            return LIFT_SEQ;
        } else if (xo == 914) {
            uam_helper(L->b, &L->lc->cpu->isa->helpers, PPC_HELPER_TLBSX, imm(L, i), ea_x(L, i), NONE,
                       NONE);
            return LIFT_SEQ;
        } else {
            helper(L, PPC_HELPER_TLBIVAX, ea_x(L, i), NONE);
        }
        uam_exit(L->b, NONE, imm(L, L->pc + 4), UAM_EXIT_JUMP);   /* mappings changed */
        return LIFT_DONE;
    }
    case 323: case 259: /* mfdcr, mfdcrx */
        if (privileged(L))
            return LIFT_DONE;
        gpr_w(L, RD(i), helper(L, PPC_HELPER_MFDCR, xo == 323 ? imm(L, SPRN(i)) : gpr(L, RA(i)), NONE));
        return LIFT_SEQ;
    case 451: case 387: /* mtdcr, mtdcrx: the device may change interrupt state */
        if (privileged(L))
            return LIFT_DONE;
        helper(L, PPC_HELPER_MTDCR, xo == 451 ? imm(L, SPRN(i)) : gpr(L, RA(i)), gpr(L, RD(i)));
        uam_exit(L->b, NONE, imm(L, L->pc + 4), UAM_EXIT_JUMP);
        return LIFT_DONE;
    default:
        return -1;
    }
}

static lift_result lift_op31(pl *L, uint32_t i)
{
    unsigned xo = XO10(i);
    if (((i >> 1) & 0x1F) == 15 && L->s->cfg.isel) {   /* isel rD,(rA|0),rB,crb */
        uam_ref c = cr_bit(L, (i >> 6) & 31);
        gpr_w(L, RD(i), uam_select(L->b, 32, c, gpr0(L, RA(i)), gpr(L, RB(i))));
        return LIFT_SEQ;
    }
    if (booke(L)) {
        int r = lift_op31_booke(L, i, xo);
        if (r >= 0)
            return (lift_result)r;
        switch (xo) {   /* the classic MMU's instructions */
        case 595: case 210: case 659: case 242: case 306: case 370:
            return illegal(L);
        default:
            break;
        }
    }
    switch (XO9(i)) {   /* XO-form arithmetic (OE in bit 21) */
    case 266: case 10: case 138: case 234: case 202: case 40: case 8: case 136: case 232: case 200:
    case 104:
        return lift_addsub(L, i, XO9(i));
    case 235: case 75: case 11: case 491: case 459:
        return lift_muldiv(L, i, XO9(i));
    default:
        break;
    }

    switch (xo) {
    /* compares and traps */
    case 0: case 32: { /* cmp, cmpl */
        uam_ref f = cmp_field(L, gpr(L, RA(i)), gpr(L, RB(i)), xo == 0, get8(L, PPC_SO_OFF));
        put8(L, PPC_CR_OFF(CRFD(i)), f);
        return LIFT_SEQ;
    }
    case 4: return lift_tw(L, RD(i), gpr(L, RA(i)), gpr(L, RB(i)));

    /* logical */
    case 28: case 60: case 444: case 412: case 316: case 476: case 124: case 284: {
        uam_ref s = gpr(L, RD(i)), b = gpr(L, RB(i)), r;
        switch (xo) {
        case 28:  r = bin(L, UAM_OP_AND, s, b); break;                   /* and */
        case 60:  r = bin(L, UAM_OP_AND, s, notv(L, b)); break;          /* andc */
        case 444: r = bin(L, UAM_OP_OR, s, b); break;                    /* or */
        case 412: r = bin(L, UAM_OP_OR, s, notv(L, b)); break;           /* orc */
        case 316: r = bin(L, UAM_OP_XOR, s, b); break;                   /* xor */
        case 476: r = notv(L, bin(L, UAM_OP_AND, s, b)); break;          /* nand */
        case 124: r = notv(L, bin(L, UAM_OP_OR, s, b)); break;           /* nor */
        default:  r = notv(L, bin(L, UAM_OP_XOR, s, b)); break;          /* eqv */
        }
        gpr_w(L, RA(i), r);
        if (RCB(i))
            record(L, r);
        return LIFT_SEQ;
    }
    case 954: case 922: case 26: { /* extsb, extsh, cntlzw */
        uam_ref s = gpr(L, RD(i));
        uam_ref r = xo == 26 ? uam_unop(L->b, UAM_OP_CLZ, 32, s) : sx_from(L, s, xo == 954 ? 8 : 16);
        gpr_w(L, RA(i), r);
        if (RCB(i))
            record(L, r);
        return LIFT_SEQ;
    }

    /* shifts */
    case 24: case 536: { /* slw, srw */
        uam_ref n = bin(L, UAM_OP_AND, gpr(L, RB(i)), imm(L, 0x3F));
        uam_ref r = bin(L, xo == 24 ? UAM_OP_SHL : UAM_OP_LSHR, gpr(L, RD(i)), n);
        gpr_w(L, RA(i), r);
        if (RCB(i))
            record(L, r);
        return LIFT_SEQ;
    }
    case 792: lift_sra(L, i, bin(L, UAM_OP_AND, gpr(L, RB(i)), imm(L, 0x3F))); return LIFT_SEQ; /* sraw */
    case 824: lift_sra(L, i, imm(L, RB(i))); return LIFT_SEQ;                                  /* srawi */

    /* condition register, XER, MSR, SPRs */
    case 19: gpr_w(L, RD(i), cr_read(L)); return LIFT_SEQ;      /* mfcr (also mfocrf) */
    case 144: { /* mtcrf (also mtocrf) */
        unsigned fxm = (i >> 12) & 0xFF;
        uam_ref s = gpr(L, RD(i));
        for (unsigned f = 0; f < 8; f++)
            if (fxm & (0x80u >> f))
                put8(L, PPC_CR_OFF(f), tr(L, 8, bin(L, UAM_OP_AND, f == 7 ? s
                                                    : bin(L, UAM_OP_LSHR, s, imm(L, 28 - 4 * f)),
                                                    imm(L, 0xF))));
        return LIFT_SEQ;
    }
    case 512: { /* mcrxr: CR[crfD] = SO OV CA 0; clear them */
        uam_ref f = bin8(L, UAM_OP_OR,
                         bin8(L, UAM_OP_OR, bin8(L, UAM_OP_SHL, get8(L, PPC_SO_OFF), imm(L, 3)),
                              bin8(L, UAM_OP_SHL, get8(L, PPC_OV_OFF), imm(L, 2))),
                         bin8(L, UAM_OP_SHL, get8(L, PPC_CA_OFF), imm(L, 1)));
        put8(L, PPC_CR_OFF(CRFD(i)), f);
        put8(L, PPC_SO_OFF, imm(L, 0));
        put8(L, PPC_OV_OFF, imm(L, 0));
        put8(L, PPC_CA_OFF, imm(L, 0));
        return LIFT_SEQ;
    }
    case 83: /* mfmsr */
        if (privileged(L))
            return LIFT_DONE;
        gpr_w(L, RD(i), uam_sysrd(L->b, 32, PPC_SR_MSR, NONE));
        return LIFT_SEQ;
    case 146: return lift_mtmsr(L, i);
    case 339: return lift_mfspr(L, i, SPRN(i));
    case 467: return lift_mtspr(L, i, SPRN(i));
    case 371: { /* mftb */
        unsigned n = SPRN(i);
        if (n != SPR_TBL_R && n != SPR_TBU_R)
            return illegal(L);
        gpr_w(L, RD(i), uam_sysrd(L->b, 32, n, NONE));
        return LIFT_SEQ;
    }

    /* segment registers and the TLB */
    case 595: /* mfsr */
        if (privileged(L))
            return LIFT_DONE;
        gpr_w(L, RD(i), uam_sysrd(L->b, 32, PPC_SR_SR0 + (RA(i) & 15), NONE));
        return LIFT_SEQ;
    case 210: /* mtsr */
        if (privileged(L))
            return LIFT_DONE;
        uam_syswr(L->b, 32, PPC_SR_SR0 + (RA(i) & 15), gpr(L, RD(i)), NONE);
        uam_exit(L->b, NONE, imm(L, L->pc + 4), UAM_EXIT_JUMP);
        return LIFT_DONE;
    case 659: /* mfsrin */
        if (privileged(L))
            return LIFT_DONE;
        gpr_w(L, RD(i), helper(L, PPC_HELPER_MFSRIN, gpr(L, RB(i)), NONE));
        return LIFT_SEQ;
    case 242: /* mtsrin */
        if (privileged(L))
            return LIFT_DONE;
        helper(L, PPC_HELPER_MTSRIN, gpr(L, RD(i)), gpr(L, RB(i)));
        uam_exit(L->b, NONE, imm(L, L->pc + 4), UAM_EXIT_JUMP);
        return LIFT_DONE;
    case 306: case 370: /* tlbie, tlbia */
        if (privileged(L))
            return LIFT_DONE;
        if (xo == 306)
            helper(L, PPC_HELPER_TLBIE, gpr(L, RB(i)), NONE);
        else
            helper(L, PPC_HELPER_TLBIA, NONE, NONE);
        uam_exit(L->b, NONE, imm(L, L->pc + 4), UAM_EXIT_JUMP);
        return LIFT_DONE;
    case 566: /* tlbsync: the tlbie broadcast is complete at once */
        if (privileged(L))
            return LIFT_DONE;
        return LIFT_SEQ;

    /* synchronisation and caches */
    case 598: case 854: /* sync, eieio */
        uam_fence(L->b, UAM_FENCE_FULL);
        return LIFT_SEQ;
    case 1014: return lift_dcbz(L, i);
    case 470: /* dcbi: memory is always coherent, nothing to discard */
        if (privileged(L))
            return LIFT_DONE;
        return LIFT_SEQ;
    case 54: case 86: case 246: case 278: case 982: case 758: /* dcbst dcbf dcbtst dcbt icbi dcba */
        return LIFT_SEQ;

    /* reservations */
    case 20: /* lwarx */
        gpr_w(L, RD(i), uam_ld(L->b, 32, UAM_ACC_BE | UAM_ACC_ALIGNED | UAM_ACC_X, ea_x(L, i), NONE));
        return LIFT_SEQ;
    case 150: { /* stwcx. */
        uam_ref ok = uam_stx(L->b, 32, UAM_ACC_BE | UAM_ACC_ALIGNED, ea_x(L, i), gpr(L, RD(i)), NONE);
        put8(L, PPC_CR_OFF(0), bin8(L, UAM_OP_OR, uam_select(L->b, 8, ok, imm(L, CR_EQ), imm(L, 0)),
                                    get8(L, PPC_SO_OFF)));
        return LIFT_SEQ;
    }

    /* integer loads and stores, indexed */
    case 23:  return load(L, i, ea_x(L, i), 32, false, false, UAM_ACC_BE);   /* lwzx */
    case 55:  return load(L, i, ea_x(L, i), 32, false, true, UAM_ACC_BE);    /* lwzux */
    case 87:  return load(L, i, ea_x(L, i), 8, false, false, UAM_ACC_BE);    /* lbzx */
    case 119: return load(L, i, ea_x(L, i), 8, false, true, UAM_ACC_BE);     /* lbzux */
    case 279: return load(L, i, ea_x(L, i), 16, false, false, UAM_ACC_BE);   /* lhzx */
    case 311: return load(L, i, ea_x(L, i), 16, false, true, UAM_ACC_BE);    /* lhzux */
    case 343: return load(L, i, ea_x(L, i), 16, true, false, UAM_ACC_BE);    /* lhax */
    case 375: return load(L, i, ea_x(L, i), 16, true, true, UAM_ACC_BE);     /* lhaux */
    case 534: return load(L, i, ea_x(L, i), 32, false, false, UAM_ACC_LE);   /* lwbrx */
    case 790: return load(L, i, ea_x(L, i), 16, false, false, UAM_ACC_LE);   /* lhbrx */
    case 151: return store(L, i, ea_x(L, i), 32, false, UAM_ACC_BE);         /* stwx */
    case 183: return store(L, i, ea_x(L, i), 32, true, UAM_ACC_BE);          /* stwux */
    case 215: return store(L, i, ea_x(L, i), 8, false, UAM_ACC_BE);          /* stbx */
    case 247: return store(L, i, ea_x(L, i), 8, true, UAM_ACC_BE);           /* stbux */
    case 407: return store(L, i, ea_x(L, i), 16, false, UAM_ACC_BE);         /* sthx */
    case 439: return store(L, i, ea_x(L, i), 16, true, UAM_ACC_BE);          /* sthux */
    case 662: return store(L, i, ea_x(L, i), 32, false, UAM_ACC_LE);         /* stwbrx */
    case 918: return store(L, i, ea_x(L, i), 16, false, UAM_ACC_LE);         /* sthbrx */

    /* strings */
    case 597: return lift_string_imm(L, i, false);                           /* lswi */
    case 725: return lift_string_imm(L, i, true);                            /* stswi */
    case 533: case 661: { /* lswx, stswx: XER[BC] bytes, in the helper */
        uint32_t regs = (RD(i) << 10) | (RA(i) << 5) | RB(i);
        helper(L, xo == 533 ? PPC_HELPER_LSWX : PPC_HELPER_STSWX, ea_x(L, i), imm(L, regs));
        return LIFT_SEQ;
    }

    /* floating-point loads and stores, indexed */
    case 535: return fp_load(L, i, ea_x(L, i), false, false);                /* lfsx */
    case 567: return fp_load(L, i, ea_x(L, i), false, true);                 /* lfsux */
    case 599: return fp_load(L, i, ea_x(L, i), true, false);                 /* lfdx */
    case 631: return fp_load(L, i, ea_x(L, i), true, true);                  /* lfdux */
    case 663: return fp_store(L, i, ea_x(L, i), 0, false);                   /* stfsx */
    case 695: return fp_store(L, i, ea_x(L, i), 0, true);                    /* stfsux */
    case 727: return fp_store(L, i, ea_x(L, i), 1, false);                   /* stfdx */
    case 759: return fp_store(L, i, ea_x(L, i), 1, true);                    /* stfdux */
    case 983: return fp_store(L, i, ea_x(L, i), 2, false);                   /* stfiwx */

    default:
        return illegal(L);
    }
}

/* ---- dispatch ---------------------------------------------------------- */

static lift_result lift_insn(pl *L, uint32_t i)
{
    switch (OPCD(i)) {
    case 3: return lift_tw(L, RD(i), gpr(L, RA(i)), imm(L, SIMM(i)));      /* twi */
    case 7: { /* mulli */
        gpr_w(L, RD(i), bin(L, UAM_OP_MUL, gpr(L, RA(i)), imm(L, SIMM(i))));
        return LIFT_SEQ;
    }
    case 8: { /* subfic: ~rA + SIMM + 1 */
        uam_ref ca;
        uam_ref r = add3(L, notv(L, gpr(L, RA(i))), imm(L, SIMM(i)), imm(L, 1), &ca, NULL);
        gpr_w(L, RD(i), r);
        set_ca(L, ca);
        return LIFT_SEQ;
    }
    case 10: case 11: { /* cmpli, cmpi */
        bool sign = OPCD(i) == 11;
        uam_ref f = cmp_field(L, gpr(L, RA(i)), imm(L, sign ? SIMM(i) : UIMM(i)), sign, get8(L, PPC_SO_OFF));
        put8(L, PPC_CR_OFF(CRFD(i)), f);
        return LIFT_SEQ;
    }
    case 12: case 13: { /* addic, addic. */
        uam_ref ca;
        uam_ref r = add3(L, gpr(L, RA(i)), imm(L, SIMM(i)), imm(L, 0), &ca, NULL);
        gpr_w(L, RD(i), r);
        set_ca(L, ca);
        if (OPCD(i) == 13)
            record(L, r);
        return LIFT_SEQ;
    }
    case 14: /* addi */
        gpr_w(L, RD(i), RA(i) ? bin(L, UAM_OP_ADD, gpr(L, RA(i)), imm(L, SIMM(i))) : imm(L, SIMM(i)));
        return LIFT_SEQ;
    case 15: /* addis */
        gpr_w(L, RD(i), RA(i) ? bin(L, UAM_OP_ADD, gpr(L, RA(i)), imm(L, UIMM(i) << 16))
                              : imm(L, UIMM(i) << 16));
        return LIFT_SEQ;
    case 16: return lift_bc(L, i);
    case 17: /* sc */
        if (!(i & 2))
            return illegal(L);
        trap(L, NONE, PPC_TRAP_SC, imm(L, 0));
        return LIFT_DONE;
    case 18: return lift_b(L, i);
    case 19: return lift_op19(L, i);
    case 20: case 21: case 23: return lift_rotate(L, i);
    case 24: case 25: case 26: case 27: case 28: case 29: { /* ori oris xori xoris andi. andis. */
        static const uam_opcode ops[] = { UAM_OP_OR, UAM_OP_XOR, UAM_OP_AND };
        unsigned k = (OPCD(i) - 24) / 2;
        uint32_t v = (OPCD(i) & 1) ? UIMM(i) << 16 : UIMM(i);
        uam_ref r = bin(L, ops[k], gpr(L, RD(i)), imm(L, v));
        gpr_w(L, RA(i), r);
        if (k == 2)
            record(L, r);
        return LIFT_SEQ;
    }
    case 31: return lift_op31(L, i);
    case 32: return load(L, i, ea_d(L, i), 32, false, false, UAM_ACC_BE);   /* lwz */
    case 33: return load(L, i, ea_d(L, i), 32, false, true, UAM_ACC_BE);    /* lwzu */
    case 34: return load(L, i, ea_d(L, i), 8, false, false, UAM_ACC_BE);    /* lbz */
    case 35: return load(L, i, ea_d(L, i), 8, false, true, UAM_ACC_BE);     /* lbzu */
    case 36: return store(L, i, ea_d(L, i), 32, false, UAM_ACC_BE);         /* stw */
    case 37: return store(L, i, ea_d(L, i), 32, true, UAM_ACC_BE);          /* stwu */
    case 38: return store(L, i, ea_d(L, i), 8, false, UAM_ACC_BE);          /* stb */
    case 39: return store(L, i, ea_d(L, i), 8, true, UAM_ACC_BE);           /* stbu */
    case 40: return load(L, i, ea_d(L, i), 16, false, false, UAM_ACC_BE);   /* lhz */
    case 41: return load(L, i, ea_d(L, i), 16, false, true, UAM_ACC_BE);    /* lhzu */
    case 42: return load(L, i, ea_d(L, i), 16, true, false, UAM_ACC_BE);    /* lha */
    case 43: return load(L, i, ea_d(L, i), 16, true, true, UAM_ACC_BE);     /* lhau */
    case 44: return store(L, i, ea_d(L, i), 16, false, UAM_ACC_BE);         /* sth */
    case 45: return store(L, i, ea_d(L, i), 16, true, UAM_ACC_BE);          /* sthu */
    case 46: return lift_multiple(L, i, false);                             /* lmw */
    case 47: return lift_multiple(L, i, true);                              /* stmw */
    case 48: return fp_load(L, i, ea_d(L, i), false, false);                /* lfs */
    case 49: return fp_load(L, i, ea_d(L, i), false, true);                 /* lfsu */
    case 50: return fp_load(L, i, ea_d(L, i), true, false);                 /* lfd */
    case 51: return fp_load(L, i, ea_d(L, i), true, true);                  /* lfdu */
    case 52: return fp_store(L, i, ea_d(L, i), 0, false);                   /* stfs */
    case 53: return fp_store(L, i, ea_d(L, i), 0, true);                    /* stfsu */
    case 54: return fp_store(L, i, ea_d(L, i), 1, false);                   /* stfd */
    case 55: return fp_store(L, i, ea_d(L, i), 1, true);                    /* stfdu */
    case 59: case 63: return lift_fp_arith(L, i);
    default:
        return illegal(L);
    }
}

/* Branches end a trace-enabled block with a branch trace (MSR[BE]). */
static bool is_branch(uint32_t i)
{
    return OPCD(i) == 16 || OPCD(i) == 18 || (OPCD(i) == 19 && (XO10(i) == 16 || XO10(i) == 528));
}

uam_status ppc_lift(uam_lift_ctx *lc)
{
    pl L = { .lc = lc, .b = lc->b, .s = ppc_st(lc->cpu), .priv = lc->cpu->priv };
    uint32_t pc = (uint32_t)lc->pc;
    bool trace = L.priv & (PPC_PRIV_SE | PPC_PRIV_BE);
    uint32_t limit = trace ? 1 : lc->max_insns;

    for (uint32_t n = 0;; n++) {
        if (n >= limit) {
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
        uam_imark(lc->b, pc, 4);
        L.pc = pc;
        L.raw = i;
        /* Trace (Book E: the instruction-complete and branch-taken debug events):
         * armed before the instruction; an exception it raises disarms it. sc and
         * the returns from interrupts are not traced. */
        if (trace && ((L.priv & PPC_PRIV_SE) || is_branch(i)) && OPCD(i) != 17
            && !(OPCD(i) == 19 && (XO10(i) == 50 || XO10(i) == 51 || XO10(i) == 38 || XO10(i) == 39)))
            helper(&L, PPC_HELPER_TRACE, NONE, NONE);
        if (lift_insn(&L, i) == LIFT_DONE)
            return UAM_OK;
        pc += 4;
    }
}
