/*
 * pic_lift.c - 8-bit PIC decoder and lifter: the baseline (12-bit), mid-range and
 * enhanced mid-range (14-bit) and PIC18 (16-bit) instruction sets.
 *
 * Conventions (docs/uam-isa.md and isa/pic8/pic.h):
 *  - The pc is the bus address of the instruction: twice the word PC on the 12- and 14-bit
 *    families (each word in two little-endian bytes), the byte PC on PIC18.
 *  - STATUS's arithmetic flags live in the register file one byte per flag; its other bits
 *    in PIC_STATUS.
 *  - A file register operand is resolved to a data address. A constant address of a core
 *    register (the bank-independent ones, and the PIC18 access bank) becomes a GET/PUT or a
 *    SYSRD/SYSWR; an address known only at run time (banked through RP/BSR/FSR bits, or
 *    indirect through an FSR) splits into a guarded helper for the core registers, when it
 *    can reach one, and a guarded LD/ST for the rest of the data space. INDF-class
 *    registers named directly are indirect accesses through their FSR.
 *  - Every instruction does its loads first, then its stores, helpers and system writes,
 *    then its register writes (queued and committed at the end: the commit rule). A write
 *    to STATUS by an instruction that sets flags is overridden by those flags, as the data
 *    sheets say, because the flags are committed after the destination.
 *  - A write to PCL is a jump: PCL with PCLATH (and PCLATU, or STATUS PA on baseline).
 *  - Skips end the block with two exits; a PIC18 skip passes one word (the second word of a
 *    two-word instruction is a NOP).
 * An encoding the model does not have stops into the debugger (TRAP DEBUG): these cores
 * have no illegal-instruction exception.
 */
#include "pic.h"

#define NONE UAM_REF_NONE
#define LE   UAM_ACC_LE

typedef enum { LIFT_CONTINUE, LIFT_END } lift_result;

typedef struct pic_qput { uint32_t off; unsigned w; uam_ref v; } pic_qput;

typedef struct pic_lifter {
    uam_lift_ctx         *lc;
    uam_builder          *b;
    const uam_pic_config *cfg;
    uam_pic_core          core;
    uint32_t              pc;      /* bus address of the instruction */
    unsigned              len;     /* bytes: 2 or 4 */
    uint32_t              w, w2;   /* the instruction's words (masked to the family's width) */
    uint32_t              pcmask;
    bool                  xinst;
    /* queued register writes, committed at the end of the instruction */
    pic_qput              q[24];
    unsigned              nq;
    uam_ref               pcl_target;  /* a constant write to PCL: the jump target */
    uam_ref               redirect;    /* a run-time core write's result (PCL written) */
} pic_lifter;

/* ---- IR shorthands ------------------------------------------------------------- */

static uam_ref imm(pic_lifter *L, uint64_t v) { return uam_imm(L->b, v); }
static uam_ref bin(pic_lifter *L, uam_opcode op, unsigned w, uam_ref x, uam_ref y)
{
    return uam_binop(L->b, op, w, x, y);
}
/* Width conversions; an immediate is folded, a value already that wide passes. */
static uam_ref zext(pic_lifter *L, unsigned w, uam_ref x)
{
    if (uam_ref_is_imm(x) || uam_builder_ref_width(L->b, x) == w)
        return x;
    return uam_unop(L->b, UAM_OP_ZEXT, w, x);
}
static uam_ref trunc_(pic_lifter *L, unsigned w, uam_ref x)
{
    if (uam_ref_is_imm(x))
        return uam_imm(L->b, L->b->imm[uam_ref_index(x)] & (w >= 64 ? ~UINT64_C(0) : (UINT64_C(1) << w) - 1));
    if (uam_builder_ref_width(L->b, x) == w)
        return x;
    return uam_unop(L->b, UAM_OP_TRUNC, w, x);
}
static uam_ref not1(pic_lifter *L, uam_ref c) { return bin(L, UAM_OP_XOR, 1, c, imm(L, 1)); }
static uam_ref and1(pic_lifter *L, uam_ref x, uam_ref y)
{
    if (uam_ref_is_none(x)) return y;
    if (uam_ref_is_none(y)) return x;
    return bin(L, UAM_OP_AND, 1, x, y);
}
static uam_ref bit(pic_lifter *L, unsigned w, uam_ref v, unsigned n)
{
    return trunc_(L, 1, n ? bin(L, UAM_OP_LSHR, w, v, imm(L, n)) : v);
}
static uam_ref get8(pic_lifter *L, uint32_t off) { return uam_get(L->b, 8, off); }
static uam_ref get16(pic_lifter *L, uint32_t off) { return uam_get(L->b, 16, off); }

/* Queue a register write. */
static void qput(pic_lifter *L, unsigned w, uint32_t off, uam_ref v)
{
    if (L->nq < UAM_ARRAY_LEN(L->q))
        L->q[L->nq++] = (pic_qput){ off, w, v };
}
static void qflag(pic_lifter *L, uint32_t off, uam_ref c)
{
    if (!uam_ref_is_none(c))
        qput(L, 8, off, zext(L, 8, c));
}
static void commit(pic_lifter *L)
{
    for (unsigned i = 0; i < L->nq; i++)
        uam_put(L->b, L->q[i].w, L->q[i].off, L->q[i].v, NONE);
    L->nq = 0;
}

static lift_result debug_stop(pic_lifter *L)
{
    L->nq = 0;
    uam_trap(L->b, NONE, UAM_TRAP_DEBUG, imm(L, 0), imm(L, L->pc));
    return LIFT_END;
}
#define ILLEGAL() return debug_stop(L)

static bool p18(const pic_lifter *L) { return L->core == UAM_PIC18; }
static bool enh(const pic_lifter *L) { return L->core == UAM_PIC_ENHANCED; }
static bool base(const pic_lifter *L) { return L->core == UAM_PIC_BASELINE; }

static uint32_t next_pc(const pic_lifter *L) { return (L->pc + L->len) & L->pcmask; }
/* The word PC of the next instruction (the PC a PCL read and CALL see). */
static uint32_t next_word(const pic_lifter *L) { return p18(L) ? next_pc(L) : next_pc(L) >> 1; }

/* ---- flags --------------------------------------------------------------------- */

typedef struct flags { uam_ref c, dc, z, ov, n; } flags;
#define NOFLAGS ((flags){ NONE, NONE, NONE, NONE, NONE })

/* a + b + cin (cin a boolean or NONE) with every flag; subtraction is a + ~b + 1 (or + C,
 * the borrow being not-C), as the PIC's ALU does it. */
static uam_ref add8(pic_lifter *L, uam_ref a, uam_ref b, uam_ref cin, flags *f)
{
    uam_ref s = bin(L, UAM_OP_ADD, 16, zext(L, 16, a), zext(L, 16, b));
    if (!uam_ref_is_none(cin))
        s = bin(L, UAM_OP_ADD, 16, s, zext(L, 16, cin));
    uam_ref r = trunc_(L, 8, s);
    uam_ref hx = bin(L, UAM_OP_XOR, 8, bin(L, UAM_OP_XOR, 8, a, b), r);
    uam_ref ov = bin(L, UAM_OP_AND, 8, bin(L, UAM_OP_XOR, 8, a, r), bin(L, UAM_OP_XOR, 8, b, r));
    f->c = bit(L, 16, s, 8);
    f->dc = bit(L, 8, hx, 4);
    f->ov = bit(L, 8, ov, 7);
    f->n = bit(L, 8, r, 7);
    f->z = bin(L, UAM_OP_EQ, 8, r, imm(L, 0));
    return r;
}
static uam_ref inv8(pic_lifter *L, uam_ref v) { return bin(L, UAM_OP_XOR, 8, v, imm(L, 0xFF)); }
static uam_ref flag(pic_lifter *L, uint32_t off) { return trunc_(L, 1, get8(L, off)); }

/* Z (and on PIC18 N) of a result. */
static flags zn(pic_lifter *L, uam_ref r)
{
    flags f = NOFLAGS;
    f.z = bin(L, UAM_OP_EQ, 8, r, imm(L, 0));
    if (p18(L))
        f.n = bit(L, 8, r, 7);
    return f;
}

static void qflags(pic_lifter *L, const flags *f)
{
    qflag(L, PIC_C, f->c);
    qflag(L, PIC_DC, f->dc);
    qflag(L, PIC_Z, f->z);
    if (p18(L)) {
        qflag(L, PIC_OV, f->ov);
        qflag(L, PIC_N, f->n);
    }
}

/* ---- the core's registers ------------------------------------------------------- */

static pic_creg creg(const pic_lifter *L, uint32_t a) { return pic_core_reg(L->cfg, a); }

/* STATUS as one byte. */
static uam_ref status_get(pic_lifter *L)
{
    uam_ref v = bin(L, UAM_OP_OR, 8, get8(L, PIC_STATUS), get8(L, PIC_C));
    v = bin(L, UAM_OP_OR, 8, v, bin(L, UAM_OP_SHL, 8, get8(L, PIC_DC), imm(L, 1)));
    v = bin(L, UAM_OP_OR, 8, v, bin(L, UAM_OP_SHL, 8, get8(L, PIC_Z), imm(L, 2)));
    if (p18(L)) {
        v = bin(L, UAM_OP_OR, 8, v, bin(L, UAM_OP_SHL, 8, get8(L, PIC_OV), imm(L, 3)));
        v = bin(L, UAM_OP_OR, 8, v, bin(L, UAM_OP_SHL, 8, get8(L, PIC_N), imm(L, 4)));
    }
    return v;
}

static void status_put(pic_lifter *L, uam_ref v)
{
    flags f = { bit(L, 8, v, 0), bit(L, 8, v, 1), bit(L, 8, v, 2), bit(L, 8, v, 3), bit(L, 8, v, 4) };
    const uint8_t m = pic_status_wmask(L->core);
    if (m)
        qput(L, 8, PIC_STATUS, bin(L, UAM_OP_OR, 8, bin(L, UAM_OP_AND, 8, get8(L, PIC_STATUS), imm(L, 0xFF & ~m)),
                                   bin(L, UAM_OP_AND, 8, v, imm(L, m))));
    qflags(L, &f);
}

/* The register-file byte and write mask of a plain core register, or false. */
static bool creg_plain(const pic_lifter *L, pic_creg cr, uint32_t *off, uint8_t *mask)
{
    *mask = 0xFF;
    switch (cr.kind) {
    case CR_FSRL:    *off = PIC_FSR(cr.n); if (base(L)) *mask = (uint8_t)(L->cfg->banks * 32u - 1u); return true;
    case CR_FSRH:    *off = PIC_FSR(cr.n) + 1; if (p18(L)) *mask = 0x0F; return true;
    case CR_BSR:     *off = PIC_BSR; *mask = p18(L) ? 0x0F : 0x1F; return true;
    case CR_WREG:    *off = PIC_W; return true;
    case CR_PCLATH:  *off = PIC_PCLATH; *mask = L->core == UAM_PIC_MID ? 0x1F : enh(L) ? 0x7F : 0xFF; return true;
    case CR_PCLATU:  *off = PIC_PCLATU; *mask = 0x1F; return true;
    case CR_TBLPTRL: *off = PIC_TBLPTR; return true;
    case CR_TBLPTRH: *off = PIC_TBLPTR + 1; return true;
    case CR_TBLPTRU: *off = PIC_TBLPTR + 2; *mask = 0x3F; return true;
    case CR_TABLAT:  *off = PIC_TABLAT; return true;
    case CR_PRODL:   *off = PIC_PROD; return true;
    case CR_PRODH:   *off = PIC_PROD + 1; return true;
    case CR_SHAD:    *off = PIC_SHAD + cr.n; if (cr.n == 2) *mask = 0x1F; return true;   /* BSR_SHAD */
    default:         return false;
    }
}

/* Read the core register at constant address a (not an INDF). */
static uam_ref creg_read(pic_lifter *L, uint32_t a, pic_creg cr)
{
    uint32_t off;
    uint8_t mask;
    if (creg_plain(L, cr, &off, &mask)) {
        uam_ref v = get8(L, off);
        if (base(L) && cr.kind == CR_FSRL)
            v = bin(L, UAM_OP_OR, 8, v, imm(L, 0xFF & ~mask));   /* unimplemented bits read 1 */
        return v;
    }
    switch (cr.kind) {
    case CR_STATUS:
        return status_get(L);
    case CR_PCL: {
        const uint32_t nw = next_word(L);
        if (p18(L)) {   /* reading PCL latches the upper bytes of the PC */
            qput(L, 8, PIC_PCLATH, imm(L, (nw >> 8) & 0xFF));
            qput(L, 8, PIC_PCLATU, imm(L, (nw >> 16) & 0x1F));
        }
        return imm(L, nw & 0xFF);
    }
    default:
        return uam_sysrd(L->b, 8, PIC_SYS_DATA(a), NONE);
    }
}

/* The jump a write of v to PCL makes (a byte pc). */
static uam_ref pcl_target(pic_lifter *L, uam_ref v)
{
    uam_ref t;
    if (p18(L)) {
        t = bin(L, UAM_OP_OR, 32, zext(L, 32, v), bin(L, UAM_OP_SHL, 32, zext(L, 32, get8(L, PIC_PCLATH)), imm(L, 8)));
        t = bin(L, UAM_OP_OR, 32, t, bin(L, UAM_OP_SHL, 32, zext(L, 32, get8(L, PIC_PCLATU)), imm(L, 16)));
        return bin(L, UAM_OP_AND, 32, t, imm(L, L->pcmask));
    }
    if (base(L))   /* PC<8> is cleared, PA (STATUS<7:5>) gives the page */
        t = bin(L, UAM_OP_SHL, 32, zext(L, 32, bin(L, UAM_OP_LSHR, 8, get8(L, PIC_STATUS), imm(L, 5))), imm(L, 9));
    else
        t = bin(L, UAM_OP_SHL, 32, zext(L, 32, get8(L, PIC_PCLATH)), imm(L, 8));
    t = bin(L, UAM_OP_OR, 32, t, zext(L, 32, v));
    return bin(L, UAM_OP_AND, 32, bin(L, UAM_OP_SHL, 32, t, imm(L, 1)), imm(L, L->pcmask));
}

/* Write v to the core register at constant address a (not an INDF). */
static void creg_write(pic_lifter *L, uint32_t a, pic_creg cr, uam_ref v)
{
    uint32_t off;
    uint8_t mask;
    if (creg_plain(L, cr, &off, &mask)) {
        qput(L, 8, off, mask == 0xFF ? v : bin(L, UAM_OP_AND, 8, v, imm(L, mask)));
        return;
    }
    switch (cr.kind) {
    case CR_STATUS: status_put(L, v); return;
    case CR_PCL:    L->pcl_target = pcl_target(L, v); return;
    default:        uam_syswr(L->b, 8, PIC_SYS_DATA(a), v, NONE); return;
    }
}

/* ---- file register operands ---------------------------------------------------- */

typedef struct access {
    bool     cst;         /* the address is known now */
    uint32_t addr;        /* cst: the data address */
    uam_ref  a;           /* !cst: the data address, 32 bits */
    bool     maybe_core;  /* !cst: may be a core register (tested at run time) */
    bool     via_fsr;     /* !cst: indirect (INDF-class registers read 0, ignore writes) */
    uam_ref  ok;          /* !cst: reaches the data space (NONE: always) */
    uam_ref  prog;        /* !cst: reads program memory instead (NONE: never) */
    uam_ref  paddr;       /* the program memory bus address */
    int      upd;         /* FSR the access updates, or -1 */
    uam_ref  upd_val;     /* its new value (16 bits) */
} access;

static access cst_access(uint32_t a)
{
    return (access){ .cst = true, .addr = a, .ok = NONE, .prog = NONE, .upd = -1 };
}
static access dyn_access(uam_ref a, bool maybe_core)
{
    return (access){ .a = a, .maybe_core = maybe_core, .ok = NONE, .prog = NONE, .upd = -1 };
}

/* Is the run-time data address a (32 bits) one of the core's registers? (pic_core_reg
 * in pic_system.c must agree.) */
static uam_ref is_core(pic_lifter *L, uam_ref a)
{
    uam_ref lo, c;
    switch (L->core) {
    case UAM_PIC_BASELINE:   /* 0, 2-4 of every bank */
        lo = bin(L, UAM_OP_AND, 32, a, imm(L, 0x1F));
        c = bin(L, UAM_OP_EQ, 32, lo, imm(L, 0));
        c = bin(L, UAM_OP_OR, 1, c, bin(L, UAM_OP_ULT, 32, bin(L, UAM_OP_SUB, 32, lo, imm(L, 2)), imm(L, 3)));
        return bin(L, UAM_OP_AND, 1, c, bin(L, UAM_OP_ULT, 32, a, imm(L, 0x100)));
    case UAM_PIC_MID:        /* 0, 2-4, 0xA-0xB of every bank */
        lo = bin(L, UAM_OP_AND, 32, a, imm(L, 0x7F));
        c = bin(L, UAM_OP_EQ, 32, lo, imm(L, 0));
        c = bin(L, UAM_OP_OR, 1, c, bin(L, UAM_OP_ULT, 32, bin(L, UAM_OP_SUB, 32, lo, imm(L, 2)), imm(L, 3)));
        c = bin(L, UAM_OP_OR, 1, c, bin(L, UAM_OP_ULT, 32, bin(L, UAM_OP_SUB, 32, lo, imm(L, 0xA)), imm(L, 2)));
        return bin(L, UAM_OP_AND, 1, c, bin(L, UAM_OP_ULT, 32, a, imm(L, 0x200)));
    case UAM_PIC_ENHANCED:   /* 0x00-0x0B of every bank, bank 31's 0x64-0x6B and 0x6D-0x6F */
        lo = bin(L, UAM_OP_AND, 32, a, imm(L, 0x7F));
        c = bin(L, UAM_OP_ULT, 32, lo, imm(L, 0x0C));
        c = bin(L, UAM_OP_OR, 1, c, bin(L, UAM_OP_ULT, 32, bin(L, UAM_OP_SUB, 32, a, imm(L, 0xFE4)), imm(L, 8)));
        c = bin(L, UAM_OP_OR, 1, c, bin(L, UAM_OP_ULT, 32, bin(L, UAM_OP_SUB, 32, a, imm(L, 0xFED)), imm(L, 3)));
        return bin(L, UAM_OP_AND, 1, c, bin(L, UAM_OP_ULT, 32, a, imm(L, 0x1000)));
    default:                 /* 0xFD0, 0xFD8-0xFEF, 0xFF2-0xFFF */
        c = bin(L, UAM_OP_EQ, 32, a, imm(L, 0xFD0));
        c = bin(L, UAM_OP_OR, 1, c, bin(L, UAM_OP_ULT, 32, bin(L, UAM_OP_SUB, 32, a, imm(L, 0xFD8)), imm(L, 0x18)));
        return bin(L, UAM_OP_OR, 1, c, bin(L, UAM_OP_ULT, 32, bin(L, UAM_OP_SUB, 32, a, imm(L, 0xFF2)), imm(L, 0x0E)));
    }
}

/* The enhanced core's mapping of an FSR value (16 bits) into the access. */
static access enh_fsr(pic_lifter *L, uam_ref fv)
{
    uam_ref f32 = zext(L, 32, fv);
    uam_ref trad = bin(L, UAM_OP_ULT, 32, f32, imm(L, 0x1000));
    uam_ref lin_off = bin(L, UAM_OP_SUB, 32, f32, imm(L, 0x2000));
    uam_ref lin = bin(L, UAM_OP_ULT, 32, lin_off, imm(L, 0x9B0));
    uam_ref q = bin(L, UAM_OP_UDIV, 32, lin_off, imm(L, 80));
    uam_ref r = bin(L, UAM_OP_UREM, 32, lin_off, imm(L, 80));
    uam_ref la = bin(L, UAM_OP_ADD, 32, bin(L, UAM_OP_SHL, 32, q, imm(L, 7)), bin(L, UAM_OP_ADD, 32, r, imm(L, 0x20)));
    access x = dyn_access(uam_select(L->b, 32, trad, f32, la), true);
    x.via_fsr = true;
    x.ok = bin(L, UAM_OP_OR, 1, trad, lin);
    x.prog = bin(L, UAM_OP_ULE, 32, imm(L, 0x8000), f32);
    x.paddr = bin(L, UAM_OP_SHL, 32, bin(L, UAM_OP_AND, 32, f32, imm(L, 0x7FFF)), imm(L, 1));
    return x;
}

/* An indirect access through FSR n in mode `mode` (IND_*). */
static access indirect(pic_lifter *L, unsigned n, unsigned mode)
{
    access x;
    switch (L->core) {
    case UAM_PIC_BASELINE: {
        uam_ref a = zext(L, 32, bin(L, UAM_OP_AND, 8, get8(L, PIC_FSR(0)), imm(L, (L->cfg->banks * 32u - 1u) & 0xFF)));
        uam_ref lo = bin(L, UAM_OP_AND, 32, a, imm(L, 0x1F));
        a = uam_select(L->b, 32, bin(L, UAM_OP_ULT, 32, lo, imm(L, 0x10)), lo, a);   /* maps back to bank 0 */
        x = dyn_access(a, true);
        break;
    }
    case UAM_PIC_MID: {
        uam_ref irp = bin(L, UAM_OP_AND, 32, zext(L, 32, get8(L, PIC_STATUS)), imm(L, 0x80));
        x = dyn_access(bin(L, UAM_OP_OR, 32, bin(L, UAM_OP_SHL, 32, irp, imm(L, 1)), zext(L, 32, get8(L, PIC_FSR(0)))), true);
        break;
    }
    case UAM_PIC_ENHANCED:
        x = enh_fsr(L, get16(L, PIC_FSR(n)));
        break;
    default: {
        uam_ref f = zext(L, 32, get16(L, PIC_FSR(n)));
        uam_ref a = f;
        switch (mode) {
        case IND_PREINC: a = bin(L, UAM_OP_ADD, 32, f, imm(L, 1)); break;
        case IND_PLUSW:  a = bin(L, UAM_OP_ADD, 32, f, uam_unop(L->b, UAM_OP_SEXT, 32, get8(L, PIC_W))); break;
        default: break;
        }
        a = bin(L, UAM_OP_AND, 32, a, imm(L, 0xFFF));
        x = dyn_access(a, true);
        if (mode == IND_POSTINC || mode == IND_POSTDEC || mode == IND_PREINC) {
            uam_ref nv = mode == IND_PREINC ? a : bin(L, mode == IND_POSTINC ? UAM_OP_ADD : UAM_OP_SUB, 32, f, imm(L, 1));
            x.upd = (int)n;
            x.upd_val = trunc_(L, 16, bin(L, UAM_OP_AND, 32, nv, imm(L, 0xFFF)));
        }
        break;
    }
    }
    x.via_fsr = true;
    return x;
}

/* An access at constant address a: an INDF-class register becomes indirect. */
static access at(pic_lifter *L, uint32_t a)
{
    pic_creg cr = creg(L, a);
    if (cr.kind == CR_INDF)
        return indirect(L, cr.n, cr.mode);
    return cst_access(a);
}

/* The baseline/mid-range bank bits as a data address offset (32 bits). */
static uam_ref bank_base(pic_lifter *L)
{
    if (base(L))   /* FSR<7:5>, as many as the part has banks */
        return bin(L, UAM_OP_AND, 32, zext(L, 32, get8(L, PIC_FSR(0))), imm(L, (L->cfg->banks - 1u) << 5));
    if (L->core == UAM_PIC_MID)   /* STATUS RP1:RP0 */
        return bin(L, UAM_OP_SHL, 32, bin(L, UAM_OP_AND, 32, zext(L, 32, get8(L, PIC_STATUS)), imm(L, 0x60)), imm(L, 2));
    return bin(L, UAM_OP_SHL, 32, zext(L, 32, get8(L, PIC_BSR)), imm(L, p18(L) ? 8 : 7));
}

/* Resolve file register operand f (with PIC18's access bit a). */
static access resolve(pic_lifter *L, uint32_t f, unsigned abit)
{
    switch (L->core) {
    case UAM_PIC_BASELINE:
        if (creg(L, f).kind != CR_NONE || f < 0x10 || L->cfg->banks == 1)
            return at(L, f);
        return dyn_access(bin(L, UAM_OP_OR, 32, bank_base(L), imm(L, f)), false);
    case UAM_PIC_MID:
        if (creg(L, f).kind != CR_NONE)
            return at(L, f);
        return dyn_access(bin(L, UAM_OP_OR, 32, bank_base(L), imm(L, f)), false);
    case UAM_PIC_ENHANCED:
        if (f < 0x0C)
            return at(L, f);
        return dyn_access(bin(L, UAM_OP_OR, 32, bank_base(L), imm(L, f)), f >= 0x64 && f <= 0x6F);
    default:
        if (abit == 0) {
            if (L->xinst && f <= 0x5F) {   /* indexed literal offset: [FSR2 + f] */
                uam_ref a = bin(L, UAM_OP_AND, 32, bin(L, UAM_OP_ADD, 32, zext(L, 32, get16(L, PIC_FSR(2))), imm(L, f)),
                                imm(L, 0xFFF));
                return dyn_access(a, true);
            }
            return at(L, f < 0x60 ? f : 0xF00u | f);
        }
        return dyn_access(bin(L, UAM_OP_OR, 32, bank_base(L), imm(L, f)), creg(L, 0xF00u | f).kind != CR_NONE);
    }
}

/* Read the operand. */
static uam_ref rd(pic_lifter *L, const access *x)
{
    if (x->cst) {
        pic_creg cr = creg(L, x->addr);
        if (cr.kind != CR_NONE)
            return creg_read(L, x->addr, cr);
        return uam_ld(L->b, 8, LE, imm(L, x->addr), NONE);
    }
    uam_ref v = NONE;
    uam_ref mem = x->ok;
    if (x->maybe_core) {
        uam_ref ic = is_core(L, x->a);
        uam_ref arg = x->via_fsr ? bin(L, UAM_OP_OR, 32, x->a, imm(L, PIC_VIA_FSR)) : x->a;
        v = uam_helper(L->b, &uam_isa_pic.helpers, PIC_HELPER_CORE_RD, arg, imm(L, next_word(L)), NONE,
                       and1(L, ic, x->ok));
        mem = and1(L, not1(L, ic), x->ok);
    }
    uam_ref m = uam_ld(L->b, 8, LE, x->a, mem);
    v = uam_ref_is_none(v) ? m : bin(L, UAM_OP_OR, 8, v, m);   /* a false guard leaves 0 */
    if (!uam_ref_is_none(x->prog))
        v = bin(L, UAM_OP_OR, 8, v, uam_ld(L->b, 8, LE | UAM_ACC_PHYS, x->paddr, x->prog));
    return v;
}

/* Write the operand (stores now; core registers queued or through the helper). */
static void wr(pic_lifter *L, const access *x, uam_ref v)
{
    if (x->cst) {
        pic_creg cr = creg(L, x->addr);
        if (cr.kind != CR_NONE)
            creg_write(L, x->addr, cr, v);
        else
            uam_st(L->b, 8, LE, imm(L, x->addr), v, NONE);
        return;
    }
    uam_ref mem = x->ok;
    if (x->maybe_core) {
        uam_ref ic = is_core(L, x->a);
        uam_ref arg = x->via_fsr ? bin(L, UAM_OP_OR, 32, x->a, imm(L, PIC_VIA_FSR)) : x->a;
        L->redirect = uam_helper(L->b, &uam_isa_pic.helpers, PIC_HELPER_CORE_WR, arg, v, NONE, and1(L, ic, x->ok));
        mem = and1(L, not1(L, ic), x->ok);
    }
    uam_st(L->b, 8, LE, x->a, v, mem);
}

/* After the destination: the FSR update an access makes. */
static void upd(pic_lifter *L, const access *x)
{
    if (!x->cst && x->upd >= 0)
        qput(L, 16, PIC_FSR(x->upd), x->upd_val);
}

/* End an instruction: commit, then any jump a PCL write made. */
static lift_result finish(pic_lifter *L)
{
    commit(L);
    if (!uam_ref_is_none(L->pcl_target)) {
        uam_exit(L->b, NONE, L->pcl_target, UAM_EXIT_JUMP);
        return LIFT_END;
    }
    if (!uam_ref_is_none(L->redirect)) {
        uam_ref r = zext(L, 32, L->redirect);
        uam_exit(L->b, trunc_(L, 1, r), bin(L, UAM_OP_AND, 32, r, imm(L, L->pcmask)), UAM_EXIT_JUMP);
    }
    return LIFT_CONTINUE;
}

/* Skip the next word if `cond` (after committing the instruction). */
static lift_result skip_if(pic_lifter *L, uam_ref cond)
{
    commit(L);
    if (!uam_ref_is_none(L->pcl_target)) {   /* a skip that also wrote PCL: the jump wins */
        uam_exit(L->b, NONE, L->pcl_target, UAM_EXIT_JUMP);
        return LIFT_END;
    }
    if (!uam_ref_is_none(L->redirect)) {
        uam_ref r = zext(L, 32, L->redirect);
        uam_exit(L->b, trunc_(L, 1, r), bin(L, UAM_OP_AND, 32, r, imm(L, L->pcmask)), UAM_EXIT_JUMP);
    }
    uam_exit(L->b, cond, imm(L, (next_pc(L) + 2) & L->pcmask), UAM_EXIT_JUMP);
    uam_exit(L->b, NONE, imm(L, next_pc(L)), UAM_EXIT_JUMP);
    return LIFT_END;
}

/* ---- calls and returns -------------------------------------------------------- */

/* Push `ret` (a pc) on the hardware stack; a stack reset leaves for the reset vector. */
static void push(pic_lifter *L, uam_ref ret)
{
    uam_ref r = uam_helper(L->b, &uam_isa_pic.helpers, PIC_HELPER_PUSH, ret, NONE, NONE, NONE);
    uam_exit(L->b, trunc_(L, 1, r), imm(L, pic_reset_pc(L->cfg)), UAM_EXIT_JUMP);
}

/* Pop a return address (a pc), leaving for the reset vector on an underflow reset. */
static uam_ref pop(pic_lifter *L)
{
    uam_ref r = uam_helper(L->b, &uam_isa_pic.helpers, PIC_HELPER_POP, NONE, NONE, NONE, NONE);
    uam_exit(L->b, trunc_(L, 1, r), imm(L, pic_reset_pc(L->cfg)), UAM_EXIT_JUMP);
    return bin(L, UAM_OP_AND, 32, r, imm(L, L->pcmask));
}

/* A jump or call to a target computed from PCLATH (or PA): a direct exit when the page is
 * the one the code is in, as it usually is. `page_ref` is the selected page bits, `page_now`
 * this code's, `t` the full target pc. */
static void paged_exit(pic_lifter *L, uam_ref page_ref, uint32_t page_now, uam_ref t, uint32_t t_same,
                       uam_exit_kind kind)
{
    uam_exit(L->b, bin(L, UAM_OP_EQ, 32, page_ref, imm(L, page_now)), imm(L, t_same), kind);
    uam_exit(L->b, NONE, t, kind);
}

/* GOTO/CALL with the low `kbits` bits of the word target in k; the upper ones from
 * PCLATH<6:3> (14-bit) or STATUS PA (baseline). */
static lift_result goto_call(pic_lifter *L, uint32_t k, unsigned kbits, bool call)
{
    const uint32_t wmask = L->pcmask >> 1;
    uam_ref page;
    unsigned shift = kbits;
    if (base(L)) {   /* PA<2:0> at STATUS<7:5> -> PC<11:9> (CALL: PC<8> = 0) */
        page = bin(L, UAM_OP_LSHR, 32, zext(L, 32, get8(L, PIC_STATUS)), imm(L, 5));
        shift = 9;
    } else {
        page = bin(L, UAM_OP_LSHR, 32, zext(L, 32, get8(L, PIC_PCLATH)), imm(L, 3));
    }
    page = bin(L, UAM_OP_AND, 32, page, imm(L, wmask >> shift));
    uam_ref t = bin(L, UAM_OP_AND, 32, bin(L, UAM_OP_SHL, 32,
                    bin(L, UAM_OP_OR, 32, bin(L, UAM_OP_SHL, 32, page, imm(L, shift)), imm(L, k)), imm(L, 1)),
                    imm(L, L->pcmask));
    const uint32_t page_now = ((L->pc >> 1) & wmask) >> shift;
    const uint32_t t_same = ((page_now << shift | k) << 1) & L->pcmask;
    if (call)
        push(L, imm(L, next_pc(L)));
    paged_exit(L, page, page_now, t, t_same, call ? UAM_EXIT_CALL : UAM_EXIT_JUMP);
    return LIFT_END;
}

static lift_result do_return(pic_lifter *L, uam_exit_kind kind)
{
    uam_ref t = pop(L);
    commit(L);
    uam_exit(L->b, NONE, t, kind);
    return LIFT_END;
}

/* RETFIE (PIC18: s = FAST). */
static lift_result retfie(pic_lifter *L, bool fast)
{
    uam_ref t = pop(L);
    if (enh(L) || fast) {   /* restore what interrupt entry saved */
        if (enh(L)) {
            uam_ref st = get8(L, PIC_SHAD + 0);
            flags f = { bit(L, 8, st, 0), bit(L, 8, st, 1), bit(L, 8, st, 2), NONE, NONE };
            qflags(L, &f);
            qput(L, 8, PIC_W, get8(L, PIC_SHAD + 1));
            qput(L, 8, PIC_BSR, get8(L, PIC_SHAD + 2));
            qput(L, 8, PIC_PCLATH, get8(L, PIC_SHAD + 3));
            qput(L, 16, PIC_FSR(0), get16(L, PIC_SHAD + 4));
            qput(L, 16, PIC_FSR(1), get16(L, PIC_SHAD + 6));
        } else {
            uam_ref st = get8(L, PIC_FAST + 1);
            flags f = { bit(L, 8, st, 0), bit(L, 8, st, 1), bit(L, 8, st, 2), bit(L, 8, st, 3), bit(L, 8, st, 4) };
            qflags(L, &f);
            qput(L, 8, PIC_W, get8(L, PIC_FAST + 0));
            qput(L, 8, PIC_BSR, get8(L, PIC_FAST + 2));
        }
    }
    /* the interrupt enable (which may let one in) */
    uam_helper(L->b, &uam_isa_pic.helpers, PIC_HELPER_RETFIE, NONE, NONE, NONE, NONE);
    commit(L);
    uam_exit(L->b, NONE, t, UAM_EXIT_EXC_RETURN);
    return LIFT_END;
}

static lift_result sleep_(pic_lifter *L)
{
    /* PD = 0, TO = 1 (PIC18: in RCON) */
    if (p18(L))
        qput(L, 8, PIC_RCON, bin(L, UAM_OP_OR, 8, bin(L, UAM_OP_AND, 8, get8(L, PIC_RCON), imm(L, 0xFF & ~PIC_RCON_PD)),
                                 imm(L, PIC_RCON_TO)));
    else
        qput(L, 8, PIC_STATUS, bin(L, UAM_OP_OR, 8, bin(L, UAM_OP_AND, 8, get8(L, PIC_STATUS), imm(L, 0xFF & ~PIC_ST_PD)),
                                   imm(L, PIC_ST_TO)));
    commit(L);
    uam_wait(L->b, imm(L, next_pc(L)));
    return LIFT_END;
}

static lift_result clrwdt(pic_lifter *L)
{
    if (p18(L))
        qput(L, 8, PIC_RCON, bin(L, UAM_OP_OR, 8, get8(L, PIC_RCON), imm(L, PIC_RCON_TO | PIC_RCON_PD)));
    else
        qput(L, 8, PIC_STATUS, bin(L, UAM_OP_OR, 8, get8(L, PIC_STATUS), imm(L, PIC_ST_TO | PIC_ST_PD)));
    return finish(L);
}

static lift_result reset_insn(pic_lifter *L)
{
    uam_helper(L->b, &uam_isa_pic.helpers, PIC_HELPER_RESET, NONE, NONE, NONE, NONE);
    uam_exit(L->b, NONE, imm(L, pic_reset_pc(L->cfg)), UAM_EXIT_JUMP);
    return LIFT_END;
}

/* ---- 12- and 14-bit byte and bit operations ----------------------------------- */

/* The byte operations numbered as the mid-range opcode's bits 11-8 (baseline: 9-6). */
static lift_result byte_op(pic_lifter *L, unsigned op, bool d, uint32_t f)
{
    access x = resolve(L, f, 0);
    uam_ref v = rd(L, &x), w = get8(L, PIC_W), r;
    flags fl = NOFLAGS;
    bool skip = false;
    switch (op) {
    case 0x2: r = add8(L, v, inv8(L, w), imm(L, 1), &fl); fl.ov = fl.n = NONE; break;   /* SUBWF */
    case 0x3: r = bin(L, UAM_OP_SUB, 8, v, imm(L, 1)); fl = zn(L, r); break;          /* DECF */
    case 0x4: r = bin(L, UAM_OP_OR, 8, v, w); fl = zn(L, r); break;                   /* IORWF */
    case 0x5: r = bin(L, UAM_OP_AND, 8, v, w); fl = zn(L, r); break;                  /* ANDWF */
    case 0x6: r = bin(L, UAM_OP_XOR, 8, v, w); fl = zn(L, r); break;                  /* XORWF */
    case 0x7: r = add8(L, v, w, NONE, &fl); fl.ov = fl.n = NONE; break;               /* ADDWF */
    case 0x8: r = v; fl = zn(L, r); break;                                            /* MOVF */
    case 0x9: r = inv8(L, v); fl = zn(L, r); break;                                   /* COMF */
    case 0xA: r = bin(L, UAM_OP_ADD, 8, v, imm(L, 1)); fl = zn(L, r); break;          /* INCF */
    case 0xB: r = bin(L, UAM_OP_SUB, 8, v, imm(L, 1)); skip = true; break;            /* DECFSZ */
    case 0xC:                                                                         /* RRF */
        r = bin(L, UAM_OP_OR, 8, bin(L, UAM_OP_LSHR, 8, v, imm(L, 1)), bin(L, UAM_OP_SHL, 8, get8(L, PIC_C), imm(L, 7)));
        fl.c = bit(L, 8, v, 0);
        break;
    case 0xD:                                                                         /* RLF */
        r = bin(L, UAM_OP_OR, 8, bin(L, UAM_OP_SHL, 8, v, imm(L, 1)), get8(L, PIC_C));
        fl.c = bit(L, 8, v, 7);
        break;
    case 0xE: r = bin(L, UAM_OP_OR, 8, bin(L, UAM_OP_SHL, 8, v, imm(L, 4)), bin(L, UAM_OP_LSHR, 8, v, imm(L, 4))); break; /* SWAPF */
    default:  r = bin(L, UAM_OP_ADD, 8, v, imm(L, 1)); skip = true; break;            /* INCFSZ */
    }
    if (d)
        wr(L, &x, r);
    else
        qput(L, 8, PIC_W, r);
    qflags(L, &fl);
    upd(L, &x);
    if (skip)
        return skip_if(L, bin(L, UAM_OP_EQ, 8, r, imm(L, 0)));
    return finish(L);
}

/* BCF, BSF, BTFSC, BTFSS (op 0-3), and PIC18 BTG (op 4). */
static lift_result bit_op(pic_lifter *L, unsigned op, unsigned b, uint32_t f, unsigned abit)
{
    access x = resolve(L, f, abit);
    uam_ref v = rd(L, &x);
    switch (op) {
    case 0: wr(L, &x, bin(L, UAM_OP_AND, 8, v, imm(L, 0xFF & ~(1u << b)))); break;
    case 1: wr(L, &x, bin(L, UAM_OP_OR, 8, v, imm(L, 1u << b))); break;
    case 4: wr(L, &x, bin(L, UAM_OP_XOR, 8, v, imm(L, 1u << b))); break;
    default:
        upd(L, &x);
        return skip_if(L, op == 2 ? not1(L, bit(L, 8, v, b)) : bit(L, 8, v, b));
    }
    upd(L, &x);
    return finish(L);
}

/* MOVWF / CLRF / CLRW */
static lift_result move_op(pic_lifter *L, uint32_t f, unsigned abit, uam_ref v, bool z)
{
    access x = resolve(L, f, abit);
    wr(L, &x, v);
    if (z)
        qflag(L, PIC_Z, imm(L, 1));
    upd(L, &x);
    return finish(L);
}

/* IORLW, ANDLW, XORLW, ADDLW, SUBLW, MOVLW (and PIC18's MULLW). */
enum { LIT_IOR, LIT_AND, LIT_XOR, LIT_ADD, LIT_SUB, LIT_MOV };
static lift_result literal(pic_lifter *L, unsigned op, uint32_t k)
{
    uam_ref w = get8(L, PIC_W), r;
    flags fl = NOFLAGS;
    switch (op) {
    case LIT_IOR: r = bin(L, UAM_OP_OR, 8, w, imm(L, k)); fl = zn(L, r); break;
    case LIT_AND: r = bin(L, UAM_OP_AND, 8, w, imm(L, k)); fl = zn(L, r); break;
    case LIT_XOR: r = bin(L, UAM_OP_XOR, 8, w, imm(L, k)); fl = zn(L, r); break;
    case LIT_ADD: r = add8(L, w, imm(L, k), NONE, &fl); break;
    case LIT_SUB: r = add8(L, imm(L, k), inv8(L, w), imm(L, 1), &fl); break;
    default:      r = imm(L, k); break;
    }
    if (!p18(L))
        fl.ov = fl.n = NONE;
    qput(L, 8, PIC_W, r);
    qflags(L, &fl);
    return finish(L);
}

/* ---- baseline (12-bit) --------------------------------------------------------- */

static lift_result lift_baseline(pic_lifter *L)
{
    const uint32_t w = L->w;
    if (w < 0x040) {
        switch (w) {
        case 0x000: return LIFT_CONTINUE;                                          /* NOP */
        case 0x002:                                                                /* OPTION */
            uam_st(L->b, 8, LE, imm(L, UAM_PIC_SPECIAL), get8(L, PIC_W), NONE);
            return LIFT_CONTINUE;
        case 0x003: return sleep_(L);
        case 0x004: return clrwdt(L);
        default: break;
        }
        if (w >= 0x005 && w <= 0x007) {                                            /* TRIS f */
            uam_st(L->b, 8, LE, imm(L, UAM_PIC_SPECIAL + w), get8(L, PIC_W), NONE);
            return LIFT_CONTINUE;
        }
        if (w >= 0x020)                                                            /* MOVWF */
            return move_op(L, w & 0x1F, 0, get8(L, PIC_W), false);
        ILLEGAL();
    }
    if (w < 0x080) {
        if (w >= 0x060)                                                            /* CLRF */
            return move_op(L, w & 0x1F, 0, imm(L, 0), true);
        if (w == 0x040) {                                                          /* CLRW */
            qput(L, 8, PIC_W, imm(L, 0));
            qflag(L, PIC_Z, imm(L, 1));
            return finish(L);
        }
        ILLEGAL();
    }
    if (w < 0x400)
        return byte_op(L, (w >> 6) & 0xF, (w >> 5) & 1, w & 0x1F);
    if (w < 0x800)
        return bit_op(L, (w >> 8) & 3, (w >> 5) & 7, w & 0x1F, 0);
    switch (w >> 8) {
    case 0x8:                                                                      /* RETLW */
        qput(L, 8, PIC_W, imm(L, w & 0xFF));
        return do_return(L, UAM_EXIT_RET);
    case 0x9: return goto_call(L, w & 0xFF, 9, true);                              /* CALL (PC<8> = 0) */
    case 0xA: case 0xB: return goto_call(L, w & 0x1FF, 9, false);                  /* GOTO */
    case 0xC: return literal(L, LIT_MOV, w & 0xFF);
    case 0xD: return literal(L, LIT_IOR, w & 0xFF);
    case 0xE: return literal(L, LIT_AND, w & 0xFF);
    default:  return literal(L, LIT_XOR, w & 0xFF);
    }
}

/* ---- mid-range and enhanced mid-range (14-bit) ---------------------------------- */

/* MOVIW / MOVWI: mode 0 ++FSRn, 1 --FSRn, 2 FSRn++, 3 FSRn--; or (k6) indexed k[n]. */
static lift_result moviw(pic_lifter *L, unsigned n, bool store, int mode, int32_t k)
{
    uam_ref f = get16(L, PIC_FSR(n)), a = f, nf = NONE;
    if (mode < 0) {
        a = bin(L, UAM_OP_ADD, 16, f, imm(L, (uint32_t)k & 0xFFFF));
    } else {
        uam_ref step = bin(L, (mode & 1) ? UAM_OP_SUB : UAM_OP_ADD, 16, f, imm(L, 1));
        nf = step;
        if (mode < 2)
            a = step;
    }
    access x = enh_fsr(L, a);
    if (store) {
        wr(L, &x, get8(L, PIC_W));
    } else {
        uam_ref v = rd(L, &x);
        qput(L, 8, PIC_W, v);
        qflag(L, PIC_Z, bin(L, UAM_OP_EQ, 8, v, imm(L, 0)));
    }
    if (!uam_ref_is_none(nf))
        qput(L, 16, PIC_FSR(n), nf);
    return finish(L);
}

/* LSLF, LSRF, ASRF, ADDWFC, SUBWFB (enhanced) */
static lift_result enh_alu(pic_lifter *L, unsigned op, bool d, uint32_t f)
{
    access x = resolve(L, f, 0);
    uam_ref v = rd(L, &x), w = get8(L, PIC_W), r;
    flags fl = NOFLAGS;
    switch (op) {
    case 0x5: r = bin(L, UAM_OP_SHL, 8, v, imm(L, 1)); fl.c = bit(L, 8, v, 7); fl.z = bin(L, UAM_OP_EQ, 8, r, imm(L, 0)); break;
    case 0x6: r = bin(L, UAM_OP_LSHR, 8, v, imm(L, 1)); fl.c = bit(L, 8, v, 0); fl.z = bin(L, UAM_OP_EQ, 8, r, imm(L, 0)); break;
    case 0x7: r = bin(L, UAM_OP_ASHR, 8, v, imm(L, 1)); fl.c = bit(L, 8, v, 0); fl.z = bin(L, UAM_OP_EQ, 8, r, imm(L, 0)); break;
    case 0xB: r = add8(L, v, inv8(L, w), flag(L, PIC_C), &fl); break;                  /* SUBWFB */
    default:  r = add8(L, v, w, flag(L, PIC_C), &fl); break;                           /* ADDWFC */
    }
    fl.ov = fl.n = NONE;
    if (d)
        wr(L, &x, r);
    else
        qput(L, 8, PIC_W, r);
    qflags(L, &fl);
    upd(L, &x);
    return finish(L);
}

static lift_result lift_14bit(pic_lifter *L)
{
    const uint32_t w = L->w;
    const bool e = enh(L);
    switch (w >> 12) {
    case 0x0:
        if (w < 0x100) {
            if (w & 0x80)                                                          /* MOVWF */
                return move_op(L, w & 0x7F, 0, get8(L, PIC_W), false);
            switch (w) {
            case 0x0008: return do_return(L, UAM_EXIT_RET);                        /* RETURN */
            case 0x0009: return retfie(L, false);                                  /* RETFIE */
            case 0x0062:                                                           /* OPTION */
                uam_st(L->b, 8, LE, imm(L, e ? 0x95 : 0x81), get8(L, PIC_W), NONE);
                return LIFT_CONTINUE;
            case 0x0063: return sleep_(L);
            case 0x0064: return clrwdt(L);
            case 0x0065: case 0x0066: case 0x0067:                                 /* TRIS f */
                uam_st(L->b, 8, LE, imm(L, e ? 0x8C + (w & 7) - 5 : 0x80 + (w & 7)), get8(L, PIC_W), NONE);
                return LIFT_CONTINUE;
            default: break;
            }
            if (!e) {
                if ((w & 0x9F) == 0)                                               /* NOP: 00 0000 0xx0 0000 */
                    return LIFT_CONTINUE;
                ILLEGAL();
            }
            switch (w) {
            case 0x0000: case 0x0040: case 0x0060: return LIFT_CONTINUE;          /* NOP (and its old forms) */
            case 0x0001: return reset_insn(L);                                     /* RESET */
            case 0x000A: {                                                         /* CALLW */
                uam_ref t = pcl_target(L, get8(L, PIC_W));
                push(L, imm(L, next_pc(L)));
                uam_exit(L->b, NONE, t, UAM_EXIT_CALL);
                return LIFT_END;
            }
            case 0x000B: {                                                         /* BRW */
                uam_ref t = bin(L, UAM_OP_ADD, 32, imm(L, next_pc(L)), bin(L, UAM_OP_SHL, 32, zext(L, 32, get8(L, PIC_W)), imm(L, 1)));
                uam_exit(L->b, NONE, bin(L, UAM_OP_AND, 32, t, imm(L, L->pcmask)), UAM_EXIT_JUMP);
                return LIFT_END;
            }
            default: break;
            }
            if (w >= 0x10 && w <= 0x1F)                                            /* MOVIW/MOVWI n mm */
                return moviw(L, (w >> 2) & 1, (w >> 3) & 1, (int)(w & 3), 0);
            if (w >= 0x20 && w <= 0x3F) {                                          /* MOVLB */
                qput(L, 8, PIC_BSR, imm(L, w & 0x1F));
                return finish(L);
            }
            ILLEGAL();
        }
        if (w < 0x200) {
            if (w & 0x80)                                                          /* CLRF */
                return move_op(L, w & 0x7F, 0, imm(L, 0), true);
            if (e && w > 0x103)
                ILLEGAL();
            qput(L, 8, PIC_W, imm(L, 0));                                          /* CLRW */
            qflag(L, PIC_Z, imm(L, 1));
            return finish(L);
        }
        return byte_op(L, (w >> 8) & 0xF, (w >> 7) & 1, w & 0x7F);
    case 0x1:
        return bit_op(L, (w >> 10) & 3, (w >> 7) & 7, w & 0x7F, 0);
    case 0x2:
        return goto_call(L, w & 0x7FF, 11, !(w & 0x800));
    default: {
        const uint32_t k = w & 0xFF;
        const unsigned op = (w >> 8) & 0xF;
        if (!e) {
            switch (op) {
            case 0x0: case 0x1: case 0x2: case 0x3: return literal(L, LIT_MOV, k);
            case 0x4: case 0x5: case 0x6: case 0x7:                                /* RETLW */
                qput(L, 8, PIC_W, imm(L, k));
                return do_return(L, UAM_EXIT_RET);
            case 0x8: return literal(L, LIT_IOR, k);
            case 0x9: return literal(L, LIT_AND, k);
            case 0xA: return literal(L, LIT_XOR, k);
            case 0xB: ILLEGAL();
            case 0xC: case 0xD: return literal(L, LIT_SUB, k);
            default:  return literal(L, LIT_ADD, k);
            }
        }
        switch (op) {
        case 0x0: return literal(L, LIT_MOV, k);
        case 0x1:
            if (w & 0x80) {                                                        /* MOVLP */
                qput(L, 8, PIC_PCLATH, imm(L, w & 0x7F));
                return finish(L);
            } else {                                                               /* ADDFSR */
                const unsigned n = (w >> 6) & 1;
                const uint32_t kk = (uint32_t)uam_sext(w & 0x3F, 6) & 0xFFFF;
                qput(L, 16, PIC_FSR(n), bin(L, UAM_OP_ADD, 16, get16(L, PIC_FSR(n)), imm(L, kk)));
                return finish(L);
            }
        case 0x2: case 0x3: {                                                      /* BRA */
            const int32_t kk = (int32_t)uam_sext(w & 0x1FF, 9);
            uam_exit(L->b, NONE, imm(L, (uint32_t)(next_pc(L) + 2 * kk) & L->pcmask), UAM_EXIT_JUMP);
            return LIFT_END;
        }
        case 0x4:                                                                  /* RETLW */
            qput(L, 8, PIC_W, imm(L, k));
            return do_return(L, UAM_EXIT_RET);
        case 0x5: case 0x6: case 0x7: case 0xB: case 0xD:
            return enh_alu(L, op, (w >> 7) & 1, w & 0x7F);
        case 0x8: return literal(L, LIT_IOR, k);
        case 0x9: return literal(L, LIT_AND, k);
        case 0xA: return literal(L, LIT_XOR, k);
        case 0xC: return literal(L, LIT_SUB, k);
        case 0xE: return literal(L, LIT_ADD, k);
        default:                                                                   /* MOVIW/MOVWI k[n] */
            return moviw(L, (w >> 6) & 1, (w >> 7) & 1, -1, (int32_t)uam_sext(w & 0x3F, 6));
        }
    }
    }
}

/* ---- PIC18 ---------------------------------------------------------------------- */

/* The PIC18 byte-oriented operations with f, d, a. */
enum {
    B_ADDWF, B_ADDWFC, B_ANDWF, B_COMF, B_DECF, B_DECFSZ, B_DCFSNZ, B_INCF, B_INCFSZ, B_INFSNZ,
    B_IORWF, B_MOVF, B_RLCF, B_RLNCF, B_RRCF, B_RRNCF, B_SUBFWB, B_SUBWF, B_SUBWFB, B_SWAPF,
    B_XORWF,
};

static lift_result p18_byte(pic_lifter *L, unsigned op)
{
    const bool d = (L->w >> 9) & 1;
    access x = resolve(L, L->w & 0xFF, (L->w >> 8) & 1);
    uam_ref v = rd(L, &x), w = get8(L, PIC_W), r;
    flags fl = NOFLAGS;
    int skip = 0;   /* 1: skip if r == 0, -1: skip if r != 0 */
    switch (op) {
    case B_ADDWF:  r = add8(L, w, v, NONE, &fl); break;
    case B_ADDWFC: r = add8(L, w, v, flag(L, PIC_C), &fl); break;
    case B_ANDWF:  r = bin(L, UAM_OP_AND, 8, w, v); fl = zn(L, r); break;
    case B_COMF:   r = inv8(L, v); fl = zn(L, r); break;
    case B_DECF:   r = add8(L, v, imm(L, 0xFF), NONE, &fl); break;
    case B_INCF:   r = add8(L, v, imm(L, 1), NONE, &fl); break;
    case B_DECFSZ: r = bin(L, UAM_OP_SUB, 8, v, imm(L, 1)); skip = 1; break;
    case B_DCFSNZ: r = bin(L, UAM_OP_SUB, 8, v, imm(L, 1)); skip = -1; break;
    case B_INCFSZ: r = bin(L, UAM_OP_ADD, 8, v, imm(L, 1)); skip = 1; break;
    case B_INFSNZ: r = bin(L, UAM_OP_ADD, 8, v, imm(L, 1)); skip = -1; break;
    case B_IORWF:  r = bin(L, UAM_OP_OR, 8, w, v); fl = zn(L, r); break;
    case B_XORWF:  r = bin(L, UAM_OP_XOR, 8, w, v); fl = zn(L, r); break;
    case B_MOVF:   r = v; fl = zn(L, r); break;
    case B_RLCF:
        r = bin(L, UAM_OP_OR, 8, bin(L, UAM_OP_SHL, 8, v, imm(L, 1)), get8(L, PIC_C));
        fl = zn(L, r);
        fl.c = bit(L, 8, v, 7);
        break;
    case B_RRCF:
        r = bin(L, UAM_OP_OR, 8, bin(L, UAM_OP_LSHR, 8, v, imm(L, 1)), bin(L, UAM_OP_SHL, 8, get8(L, PIC_C), imm(L, 7)));
        fl = zn(L, r);
        fl.c = bit(L, 8, v, 0);
        break;
    case B_RLNCF:  r = bin(L, UAM_OP_OR, 8, bin(L, UAM_OP_SHL, 8, v, imm(L, 1)), bin(L, UAM_OP_LSHR, 8, v, imm(L, 7))); fl = zn(L, r); break;
    case B_RRNCF:  r = bin(L, UAM_OP_OR, 8, bin(L, UAM_OP_LSHR, 8, v, imm(L, 1)), bin(L, UAM_OP_SHL, 8, v, imm(L, 7))); fl = zn(L, r); break;
    case B_SUBFWB: r = add8(L, w, inv8(L, v), flag(L, PIC_C), &fl); break;
    case B_SUBWF:  r = add8(L, v, inv8(L, w), imm(L, 1), &fl); break;
    case B_SUBWFB: r = add8(L, v, inv8(L, w), flag(L, PIC_C), &fl); break;
    default:       r = bin(L, UAM_OP_OR, 8, bin(L, UAM_OP_SHL, 8, v, imm(L, 4)), bin(L, UAM_OP_LSHR, 8, v, imm(L, 4))); break; /* SWAPF */
    }
    if (d)
        wr(L, &x, r);
    else
        qput(L, 8, PIC_W, r);
    qflags(L, &fl);
    upd(L, &x);
    if (skip) {
        uam_ref z = bin(L, UAM_OP_EQ, 8, r, imm(L, 0));
        return skip_if(L, skip > 0 ? z : not1(L, z));
    }
    return finish(L);
}

/* The f, a operations: CPFSLT/CPFSEQ/CPFSGT/TSTFSZ, SETF, CLRF, NEGF, MOVWF, MULWF. */
static lift_result p18_fa(pic_lifter *L, unsigned op)
{
    const uint32_t f = L->w & 0xFF;
    const unsigned a = (L->w >> 8) & 1;
    access x = resolve(L, f, a);
    switch (op) {
    case 0x60: case 0x62: case 0x64: case 0x66: {
        uam_ref v = rd(L, &x), w = get8(L, PIC_W), c;
        if (op == 0x60)      c = bin(L, UAM_OP_ULT, 8, v, w);                 /* CPFSLT */
        else if (op == 0x62) c = bin(L, UAM_OP_EQ, 8, v, w);                  /* CPFSEQ */
        else if (op == 0x64) c = bin(L, UAM_OP_ULT, 8, w, v);                 /* CPFSGT */
        else                 c = bin(L, UAM_OP_EQ, 8, v, imm(L, 0));          /* TSTFSZ */
        upd(L, &x);
        return skip_if(L, c);
    }
    case 0x68: return move_op(L, f, a, imm(L, 0xFF), false);                 /* SETF */
    case 0x6A: return move_op(L, f, a, imm(L, 0), true);                     /* CLRF */
    case 0x6C: {                                                             /* NEGF */
        flags fl;
        uam_ref r = add8(L, imm(L, 0), inv8(L, rd(L, &x)), imm(L, 1), &fl);
        wr(L, &x, r);
        qflags(L, &fl);
        upd(L, &x);
        return finish(L);
    }
    case 0x6E: return move_op(L, f, a, get8(L, PIC_W), false);               /* MOVWF */
    default: {                                                               /* MULWF */
        uam_ref p = bin(L, UAM_OP_MUL, 16, zext(L, 16, get8(L, PIC_W)), zext(L, 16, rd(L, &x)));
        qput(L, 16, PIC_PROD, p);
        upd(L, &x);
        return finish(L);
    }
    }
}

/* TBLRD / TBLWT: mode 0 *, 1 *+, 2 *-, 3 +* */
static lift_result tbl(pic_lifter *L, bool write, unsigned mode)
{
    uam_ref p32 = bin(L, UAM_OP_AND, 32, uam_get(L->b, 32, PIC_TBLPTR), imm(L, 0x3FFFFF));
    uam_ref a = mode == 3 ? bin(L, UAM_OP_AND, 32, bin(L, UAM_OP_ADD, 32, p32, imm(L, 1)), imm(L, 0x3FFFFF)) : p32;
    if (write)
        uam_helper(L->b, &uam_isa_pic.helpers, PIC_HELPER_TBLWT, a, get8(L, PIC_TABLAT), NONE, NONE);
    else
        qput(L, 8, PIC_TABLAT, uam_ld(L->b, 8, LE | UAM_ACC_PHYS, a, NONE));
    if (mode)
        qput(L, 32, PIC_TBLPTR, mode == 3 ? a : bin(L, UAM_OP_AND, 32, bin(L, mode == 1 ? UAM_OP_ADD : UAM_OP_SUB, 32,
                                                                           p32, imm(L, 1)), imm(L, 0x3FFFFF)));
    return finish(L);
}

static lift_result p18_misc(pic_lifter *L)
{
    const uint32_t w = L->w;
    switch (w) {
    case 0x0000: return LIFT_CONTINUE;                                       /* NOP */
    case 0x0003: return sleep_(L);
    case 0x0004: return clrwdt(L);
    case 0x0005:                                                             /* PUSH */
        push(L, imm(L, next_pc(L)));
        return LIFT_CONTINUE;
    case 0x0006:                                                             /* POP */
        pop(L);
        return LIFT_CONTINUE;
    case 0x0007: {                                                           /* DAW */
        uam_ref v = zext(L, 16, get8(L, PIC_W));
        uam_ref lo_adj = bin(L, UAM_OP_OR, 1, bin(L, UAM_OP_ULT, 16, imm(L, 9), bin(L, UAM_OP_AND, 16, v, imm(L, 0xF))),
                             flag(L, PIC_DC));
        v = bin(L, UAM_OP_ADD, 16, v, uam_select(L->b, 16, lo_adj, imm(L, 6), imm(L, 0)));
        uam_ref hi_adj = bin(L, UAM_OP_OR, 1, bin(L, UAM_OP_ULT, 16, imm(L, 0x9F), v), flag(L, PIC_C));
        v = bin(L, UAM_OP_ADD, 16, v, uam_select(L->b, 16, hi_adj, imm(L, 0x60), imm(L, 0)));
        qput(L, 8, PIC_W, trunc_(L, 8, v));
        qflag(L, PIC_C, bin(L, UAM_OP_OR, 1, bit(L, 16, v, 8), flag(L, PIC_C)));
        return finish(L);
    }
    case 0x0010: case 0x0011: return retfie(L, w & 1);
    case 0x0012: case 0x0013:                                                /* RETURN [FAST] */
        if (w & 1) {
            uam_ref st = get8(L, PIC_FAST + 1);
            flags f = { bit(L, 8, st, 0), bit(L, 8, st, 1), bit(L, 8, st, 2), bit(L, 8, st, 3), bit(L, 8, st, 4) };
            qflags(L, &f);
            qput(L, 8, PIC_W, get8(L, PIC_FAST + 0));
            qput(L, 8, PIC_BSR, get8(L, PIC_FAST + 2));
        }
        return do_return(L, UAM_EXIT_RET);
    case 0x0014: {                                                           /* CALLW */
        if (!L->xinst)
            ILLEGAL();
        uam_ref t = pcl_target(L, get8(L, PIC_W));
        push(L, imm(L, next_pc(L)));
        uam_exit(L->b, NONE, t, UAM_EXIT_CALL);
        return LIFT_END;
    }
    case 0x00FF: return reset_insn(L);
    default: break;
    }
    if (w >= 0x0008 && w <= 0x000F)
        return tbl(L, w >= 0x000C, w & 3);
    if (w >= 0x0100 && w <= 0x010F) {                                        /* MOVLB */
        qput(L, 8, PIC_BSR, imm(L, w & 0x0F));
        return finish(L);
    }
    ILLEGAL();
}

/* CALL/GOTO/RCALL targets, saving the fast registers for CALL FAST */
static void fast_save(pic_lifter *L)
{
    qput(L, 8, PIC_FAST + 0, get8(L, PIC_W));
    qput(L, 8, PIC_FAST + 1, status_get(L));
    qput(L, 8, PIC_FAST + 2, get8(L, PIC_BSR));
}

/* ADDFSR/SUBFSR/ADDULNK/SUBULNK, PUSHL, MOVSF/MOVSS (XINST) */
static lift_result p18_xinst(pic_lifter *L)
{
    const uint32_t w = L->w;
    if (!L->xinst)
        ILLEGAL();
    switch ((w >> 8) & 0xFF) {
    case 0xE8: case 0xE9: {
        const unsigned n = (w >> 6) & 3;
        const bool sub = (w >> 8) & 1;
        const unsigned fn = n == 3 ? 2 : n;
        uam_ref nv = bin(L, UAM_OP_AND, 16, bin(L, sub ? UAM_OP_SUB : UAM_OP_ADD, 16, get16(L, PIC_FSR(fn)), imm(L, w & 0x3F)),
                         imm(L, 0xFFF));
        qput(L, 16, PIC_FSR(fn), nv);
        if (n == 3)                                                          /* ADDULNK/SUBULNK */
            return do_return(L, UAM_EXIT_RET);
        return finish(L);
    }
    case 0xEA: {                                                             /* PUSHL */
        uam_ref f = get16(L, PIC_FSR(2));
        access x = dyn_access(zext(L, 32, bin(L, UAM_OP_AND, 16, f, imm(L, 0xFFF))), true);
        wr(L, &x, imm(L, w & 0xFF));
        qput(L, 16, PIC_FSR(2), bin(L, UAM_OP_AND, 16, bin(L, UAM_OP_SUB, 16, f, imm(L, 1)), imm(L, 0xFFF)));
        return finish(L);
    }
    default: {                                                               /* MOVSF / MOVSS */
        uam_ref f2 = zext(L, 32, get16(L, PIC_FSR(2)));
        access src = dyn_access(bin(L, UAM_OP_AND, 32, bin(L, UAM_OP_ADD, 32, f2, imm(L, w & 0x7F)), imm(L, 0xFFF)), true);
        src.via_fsr = true;   /* an INDF source reads 0 */
        uam_ref v = rd(L, &src);
        access dst;
        if (w & 0x80) {
            dst = dyn_access(bin(L, UAM_OP_AND, 32, bin(L, UAM_OP_ADD, 32, f2, imm(L, L->w2 & 0x7F)), imm(L, 0xFFF)), true);
            dst.via_fsr = true;   /* an INDF destination: a NOP */
        } else {
            dst = at(L, L->w2 & 0xFFF);
        }
        wr(L, &dst, v);
        upd(L, &dst);
        return finish(L);
    }
    }
}

static lift_result lift_p18(pic_lifter *L)
{
    const uint32_t w = L->w;
    const unsigned hi = w >> 8;
    switch (w >> 12) {
    case 0x0:
        if (hi == 0x00 || hi == 0x01)
            return p18_misc(L);
        if (hi == 0x02 || hi == 0x03)
            return p18_fa(L, 0x02);                                          /* MULWF */
        if (hi < 0x08)
            return p18_byte(L, B_DECF);
        switch (hi) {
        case 0x08: return literal(L, LIT_SUB, w & 0xFF);
        case 0x09: return literal(L, LIT_IOR, w & 0xFF);
        case 0x0A: return literal(L, LIT_XOR, w & 0xFF);
        case 0x0B: return literal(L, LIT_AND, w & 0xFF);
        case 0x0C:                                                           /* RETLW */
            qput(L, 8, PIC_W, imm(L, w & 0xFF));
            return do_return(L, UAM_EXIT_RET);
        case 0x0D:                                                           /* MULLW */
            qput(L, 16, PIC_PROD, bin(L, UAM_OP_MUL, 16, zext(L, 16, get8(L, PIC_W)), imm(L, w & 0xFF)));
            return finish(L);
        case 0x0E: return literal(L, LIT_MOV, w & 0xFF);
        default:   return literal(L, LIT_ADD, w & 0xFF);
        }
    case 0x1: {
        static const unsigned ops[] = { B_IORWF, B_ANDWF, B_XORWF, B_COMF };
        return p18_byte(L, ops[(w >> 10) & 3]);
    }
    case 0x2: {
        static const unsigned ops[] = { B_ADDWFC, B_ADDWF, B_INCF, B_DECFSZ };
        return p18_byte(L, ops[(w >> 10) & 3]);
    }
    case 0x3: {
        static const unsigned ops[] = { B_RRCF, B_RLCF, B_SWAPF, B_INCFSZ };
        return p18_byte(L, ops[(w >> 10) & 3]);
    }
    case 0x4: {
        static const unsigned ops[] = { B_RRNCF, B_RLNCF, B_INFSNZ, B_DCFSNZ };
        return p18_byte(L, ops[(w >> 10) & 3]);
    }
    case 0x5: {
        static const unsigned ops[] = { B_MOVF, B_SUBFWB, B_SUBWFB, B_SUBWF };
        return p18_byte(L, ops[(w >> 10) & 3]);
    }
    case 0x6:
        return p18_fa(L, hi & 0xFE);
    case 0x7: return bit_op(L, 4, (w >> 9) & 7, w & 0xFF, (w >> 8) & 1);    /* BTG */
    case 0x8: return bit_op(L, 1, (w >> 9) & 7, w & 0xFF, (w >> 8) & 1);    /* BSF */
    case 0x9: return bit_op(L, 0, (w >> 9) & 7, w & 0xFF, (w >> 8) & 1);    /* BCF */
    case 0xA: return bit_op(L, 3, (w >> 9) & 7, w & 0xFF, (w >> 8) & 1);    /* BTFSS */
    case 0xB: return bit_op(L, 2, (w >> 9) & 7, w & 0xFF, (w >> 8) & 1);    /* BTFSC */
    case 0xC: {                                                              /* MOVFF */
        access src = at(L, w & 0xFFF);
        uam_ref v = rd(L, &src);
        access dst = at(L, L->w2 & 0xFFF);
        wr(L, &dst, v);
        upd(L, &src);
        upd(L, &dst);
        return finish(L);
    }
    case 0xD: {                                                              /* BRA / RCALL */
        const int32_t k = (int32_t)uam_sext(w & 0x7FF, 11);
        const uint32_t t = (uint32_t)(next_pc(L) + 2 * k) & L->pcmask;
        const bool call = (w >> 11) & 1;
        if (call)
            push(L, imm(L, next_pc(L)));
        uam_exit(L->b, NONE, imm(L, t), call ? UAM_EXIT_CALL : UAM_EXIT_JUMP);
        return LIFT_END;
    }
    case 0xE:
        if (hi < 0xE8) {                                                     /* Bcc */
            static const uint32_t fl[] = { PIC_Z, PIC_C, PIC_OV, PIC_N };
            uam_ref c = flag(L, fl[(hi >> 1) & 3]);
            if (hi & 1)
                c = not1(L, c);
            const int32_t k = (int32_t)uam_sext(w & 0xFF, 8);
            uam_exit(L->b, c, imm(L, (uint32_t)(next_pc(L) + 2 * k) & L->pcmask), UAM_EXIT_JUMP);
            uam_exit(L->b, NONE, imm(L, next_pc(L)), UAM_EXIT_JUMP);
            return LIFT_END;
        }
        switch (hi) {
        case 0xEC: case 0xED: case 0xEF: {                                   /* CALL / GOTO */
            const uint32_t t = ((L->w2 & 0xFFF) << 9 | (w & 0xFF) << 1) & L->pcmask;
            if (hi == 0xEF) {
                uam_exit(L->b, NONE, imm(L, t), UAM_EXIT_JUMP);
                return LIFT_END;
            }
            if (hi & 1) {                                                    /* CALL FAST */
                fast_save(L);
                commit(L);
            }
            push(L, imm(L, next_pc(L)));
            uam_exit(L->b, NONE, imm(L, t), UAM_EXIT_CALL);
            return LIFT_END;
        }
        case 0xEE:                                                           /* LFSR */
            if ((w & 0xC0) || (w & 0x30) == 0x30)
                ILLEGAL();
            qput(L, 16, PIC_FSR((w >> 4) & 3), imm(L, (w & 0xF) << 8 | (L->w2 & 0xFF)));
            return finish(L);
        default:
            return p18_xinst(L);
        }
    default:                                                                 /* 1111: NOP */
        return LIFT_CONTINUE;
    }
}

/* ---- blocks ------------------------------------------------------------------- */

/* Words of the instruction whose first word is w. */
static unsigned insn_words(const pic_state *s, uint32_t w)
{
    if (s->cfg.core != UAM_PIC18)
        return 1;
    const unsigned hi = w >> 8;
    if ((w >> 12) == 0xC || hi == 0xEC || hi == 0xED || hi == 0xEE || hi == 0xEF)
        return 2;
    if (hi == 0xEB && (s->cfg.features & UAM_PIC_F_XINST))
        return 2;
    return 1;
}

uam_status pic_lift(uam_lift_ctx *lc)
{
    const pic_state *s = pic_st(lc->cpu);
    const uint32_t pcmask = pic_pc_mask(&s->cfg);
    const uint32_t wmask = s->cfg.core == UAM_PIC_BASELINE ? 0xFFF : pic_14bit(s->cfg.core) ? 0x3FFF : 0xFFFF;
    uint32_t pc = (uint32_t)lc->pc & pcmask;

    for (uint32_t n = 0;; n++) {
        if (n >= lc->max_insns) {
            uam_exit(lc->b, NONE, uam_imm(lc->b, pc), UAM_EXIT_JUMP);
            return UAM_OK;
        }
        uint64_t w = 0, w2 = 0;
        uam_status st = lc->fetch(lc, pc, 2, &w);
        unsigned words = st == UAM_OK ? insn_words(s, (uint32_t)w & wmask) : 1;
        if (st == UAM_OK && words == 2)
            st = lc->fetch(lc, (pc + 2) & pcmask, 2, &w2);
        if (st != UAM_OK) {
            if (n == 0)
                return UAM_ERR_FAULT;
            uam_exit(lc->b, NONE, uam_imm(lc->b, pc), UAM_EXIT_JUMP);
            return UAM_OK;
        }
        pic_lifter L = { .lc = lc, .b = lc->b, .cfg = &s->cfg, .core = s->cfg.core, .pc = pc,
                         .len = 2 * words, .w = (uint32_t)w & wmask, .w2 = (uint32_t)w2 & 0xFFFF,
                         .pcmask = pcmask, .xinst = (s->cfg.features & UAM_PIC_F_XINST) != 0,
                         .pcl_target = NONE, .redirect = NONE };
        uam_imark(lc->b, pc, L.len);
        lift_result r;
        switch (L.core) {
        case UAM_PIC_BASELINE: r = lift_baseline(&L); break;
        case UAM_PIC18:        r = lift_p18(&L); break;
        default:               r = lift_14bit(&L); break;
        }
        pc = (pc + L.len) & pcmask;
        if (r == LIFT_END)
            return UAM_OK;
    }
}
