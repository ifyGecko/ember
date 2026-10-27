/*
 * opt.c - Block optimizer (uam/ir.h: uam_optimize).
 *
 * A fast, local, semantics-preserving pass over one verified block, run before the
 * block is executed by any backend:
 *
 *   1. forward pass
 *      - copy/constant propagation: uses of a removed temporary are rewritten to the
 *        value that replaced it (another temporary or an immediate);
 *      - constant folding of pure ops whose operands are all immediates, through the
 *        same evaluator as the interpreter (ir_eval.h), so results are bit-identical;
 *      - algebraic identities (x+0, x|0, x^0, x&-1, x*1, shifts by 0, x&0, x*0,
 *        select on a constant, conversions of constants);
 *      - register forwarding: a GET of a register whose value is known from an earlier
 *        GET or unguarded PUT in the block is replaced by that value. HELPER, SYSRD and
 *        SYSWR forget everything (their native code may touch the register file);
 *      - constant guards: a guard known false removes the op (its result becomes 0), a
 *        guard known true is dropped; an EXIT/TRAP that thereby becomes unconditional
 *        ends the block (everything after it is unreachable);
 *      - commutative operands in a canonical order (an immediate second), peepholes
 *        through an operand's defining op (mostly flag bytes read back: a zero-extended
 *        boolean compared with 0 or 1, truncated, or negated twice; selects of
 *        booleans), and common-subexpression elimination of pure ops.
 *   2. dead register writes: a PUT overwritten by a later unguarded PUT of the same
 *      guest instruction, with nothing in between that could see the register file, is
 *      removed (the commit rule puts every op that may fault before the first PUT, so
 *      within an instruction only a GET, an exit or an escape op could observe it).
 *   3. dead code elimination: pure ops and GETs whose result is unused are removed.
 *
 * Nothing else that has a side effect is moved or removed (except unreachable code and
 * ops whose guard is constant false), IMARKs are kept, and PUTs are never reordered, so the
 * commit rule (section 8.5) and precise exceptions are preserved. COV, an unguarded
 * effect op, therefore stays directly after its IMARK (section 8 rule 8).
 */
#include <stdlib.h>
#include <string.h>

#include "uam/ir.h"

#include "ir_eval.h"

#define MAX_KNOWN 64u   /* register values tracked for forwarding */

typedef struct known_reg {
    uint32_t off;
    uint8_t  width;
    uam_ref  val;
} known_reg;

typedef struct opt {
    uam_block *blk;
    uam_ref   *repl;        /* per temporary: what replaces it (itself if unchanged) */
    uint32_t  *def;         /* per temporary: index of its kept defining insn, or NO_DEF */
    uint32_t  *cse;         /* hash table of kept pure insns (indices), NO_DEF = empty */
    uint32_t   cse_mask;
    known_reg  known[MAX_KNOWN];
    unsigned   n_known;
    bool       oom;
} opt;

#define NO_DEF UINT32_MAX

static uam_ref imm_ref(opt *o, uint64_t v)
{
    uam_block *b = o->blk;
    /* Reuse an existing pool entry when the value is already there. */
    for (uint32_t i = 0; i < b->n_imm; i++)
        if (b->imm[i] == v)
            return UAM_IMM_BIT | i;
    uint64_t *ni = realloc(b->imm, sizeof(*ni) * (b->n_imm + 1));
    if (!ni) {
        o->oom = true;
        return UAM_REF_NONE;
    }
    b->imm = ni;
    b->imm[b->n_imm] = v;
    return UAM_IMM_BIT | b->n_imm++;
}

static uam_ref rewrite(const opt *o, uam_ref r)
{
    return (uam_ref_is_none(r) || uam_ref_is_imm(r)) ? r : o->repl[r];
}

static bool is_imm(uam_ref r) { return uam_ref_is_imm(r); }
static uint64_t imm_val(const opt *o, uam_ref r) { return o->blk->imm[uam_ref_index(r)]; }

static void forget_overlapping(opt *o, uint32_t off, unsigned width)
{
    uint32_t end = off + width / 8;
    unsigned w = 0;
    for (unsigned i = 0; i < o->n_known; i++) {
        known_reg *k = &o->known[i];
        if (k->off < end && off < k->off + k->width / 8u)
            continue;
        o->known[w++] = *k;
    }
    o->n_known = w;
}

static void remember(opt *o, uint32_t off, unsigned width, uam_ref val)
{
    forget_overlapping(o, off, width);
    if (o->n_known < MAX_KNOWN)
        o->known[o->n_known++] = (known_reg){ .off = off, .width = (uint8_t)width, .val = val };
}

static uam_ref lookup_known(const opt *o, uint32_t off, unsigned width)
{
    for (unsigned i = 0; i < o->n_known; i++)
        if (o->known[i].off == off && o->known[i].width == width)
            return o->known[i].val;
    return UAM_REF_NONE;
}

/*
 * Try to simplify a pure op whose (rewritten) operands are a, b, c. Returns the
 * replacing ref, or UAM_REF_NONE to keep the op.
 */
/* The kept instruction that defines temp operand r, or NULL. */
static const uam_insn *def_of(const opt *o, uam_ref r)
{
    if (uam_ref_is_none(r) || uam_ref_is_imm(r) || o->def[r] == NO_DEF)
        return NULL;
    return &o->blk->insn[o->def[r]];
}

/* r is the boolean negation (xor x, 1) of some x: returns x, else NONE. */
static uam_ref negated(const opt *o, uam_ref r)
{
    const uam_insn *d = def_of(o, r);
    if (d && d->op == UAM_OP_XOR && d->width == 1 && is_imm(d->b) && (imm_val(o, d->b) & 1))
        return d->a;
    return UAM_REF_NONE;
}

static bool commutative(uam_opcode op)
{
    switch (op) {
    case UAM_OP_ADD: case UAM_OP_MUL: case UAM_OP_AND: case UAM_OP_OR: case UAM_OP_XOR:
    case UAM_OP_EQ: case UAM_OP_NE: case UAM_OP_CARRY: case UAM_OP_SCARRY:
    case UAM_OP_MULHU: case UAM_OP_MULHS:
        return true;
    default:
        return false;
    }
}

/*
 * Peepholes through the defining instruction of an operand. Either returns the
 * replacing ref, or rewrites *in into an equivalent (cheaper) op with the same
 * destination and returns UAM_REF_NONE.
 */
static uam_ref peephole(opt *o, uam_insn *in)
{
    const uam_opcode op = (uam_opcode)in->op;
    const unsigned W = in->width;
    const uam_insn *da = def_of(o, in->a);
    const bool bi = uam_opinfo_table[op].n_src > 1 && is_imm(in->b);
    const uint64_t bv = bi ? imm_val(o, in->b) & uam_mask(W) : 0;

    switch (op) {
    case UAM_OP_EQ: case UAM_OP_NE:
        /* (zext b) != 0 and (zext b) == 1 are b; == 0 and != 1 are !b (b boolean) */
        if (da && da->op == UAM_OP_ZEXT && bi && bv <= 1 && o->blk->temp_width[da->a] == 1) {
            if ((op == UAM_OP_NE) == (bv == 0))
                return da->a;
            *in = (uam_insn){ .op = UAM_OP_XOR, .width = 1, .guard = UAM_REF_NONE, .dst = in->dst,
                              .a = da->a, .b = imm_ref(o, 1), .c = UAM_REF_NONE };
        }
        break;
    case UAM_OP_TRUNC:   /* trunc (zext x) back to x's own width */
        if (da && da->op == UAM_OP_ZEXT && o->blk->temp_width[da->a] == W)
            return da->a;
        break;
    case UAM_OP_ZEXT:    /* zext (zext x) = zext x */
        if (da && da->op == UAM_OP_ZEXT)
            in->a = da->a;
        break;
    case UAM_OP_XOR:     /* !!b = b */
        if (W == 1 && bi && bv == 1) {
            uam_ref x = negated(o, in->a);
            if (!uam_ref_is_none(x))
                return x;
        }
        break;
    case UAM_OP_SELECT: {
        if (W == 1 && is_imm(in->b) && is_imm(in->c)) {
            uint64_t x = imm_val(o, in->b) & 1, y = imm_val(o, in->c) & 1;
            if (x == 1 && y == 0)
                return in->a;
            if (x == 0 && y == 1)
                *in = (uam_insn){ .op = UAM_OP_XOR, .width = 1, .guard = UAM_REF_NONE,
                                  .dst = in->dst, .a = in->a, .b = imm_ref(o, 1),
                                  .c = UAM_REF_NONE };
            break;
        }
        uam_ref c = negated(o, in->a);   /* select !c, x, y = select c, y, x */
        if (!uam_ref_is_none(c)) {
            uam_ref t = in->b;
            in->a = c;
            in->b = in->c;
            in->c = t;
        }
        break;
    }
    default:
        break;
    }
    return UAM_REF_NONE;
}

/* Common subexpressions: an earlier kept pure insn with the same op and operands. */
static uint32_t cse_hash(const uam_insn *in)
{
    uint64_t h = (uint64_t)in->op * 0x9E3779B97F4A7C15ULL ^ in->width;
    h = (h ^ in->a) * 0xBF58476D1CE4E5B9ULL;
    h = (h ^ in->b) * 0x94D049BB133111EBULL;
    h = (h ^ in->c) * 0x9E3779B97F4A7C15ULL;
    return (uint32_t)(h >> 32);
}

static uam_ref cse_find(const opt *o, const uam_insn *in)
{
    for (uint32_t k = cse_hash(in) & o->cse_mask;; k = (k + 1) & o->cse_mask) {
        uint32_t i = o->cse[k];
        if (i == NO_DEF)
            return UAM_REF_NONE;
        const uam_insn *e = &o->blk->insn[i];
        if (e->op == in->op && e->width == in->width && e->a == in->a && e->b == in->b
            && e->c == in->c)
            return e->dst;
    }
}

static void cse_add(opt *o, const uam_insn *in, uint32_t idx)
{
    uint32_t k = cse_hash(in) & o->cse_mask;
    while (o->cse[k] != NO_DEF)
        k = (k + 1) & o->cse_mask;
    o->cse[k] = idx;
}

static uam_ref simplify(opt *o, uam_insn *in, unsigned src_w)
{
    const uam_opcode op = (uam_opcode)in->op;
    const unsigned W = in->width;
    const unsigned n_src = uam_opinfo_table[op].n_src;
    const uint64_t m = uam_mask(W);

    /* Constant folding. */
    bool all_imm = true;
    const uam_ref src[3] = { in->a, in->b, in->c };
    for (unsigned i = 0; i < n_src; i++)
        all_imm &= is_imm(src[i]);
    if (all_imm) {
        uint64_t x = n_src > 0 ? imm_val(o, in->a) & uam_mask(op == UAM_OP_SELECT ? 1 : src_w) : 0;
        uint64_t y = n_src > 1 ? imm_val(o, in->b) & m : 0;
        uint64_t z = n_src > 2 ? imm_val(o, in->c) & m : 0;
        uint64_t r;
        if (uam_ir_eval_pure(op, W, x, y, z, src_w, &r))
            return imm_ref(o, r);
        return UAM_REF_NONE;
    }

    /* Identities with one constant operand. */
    const bool bi = n_src > 1 && is_imm(in->b);
    const uint64_t bv = bi ? imm_val(o, in->b) & m : 0;
    const bool ai = n_src > 0 && is_imm(in->a);
    const uint64_t av = ai ? imm_val(o, in->a) & m : 0;
    switch (op) {
    case UAM_OP_ADD: case UAM_OP_OR: case UAM_OP_XOR:
        if (bi && bv == 0) return in->a;
        if (ai && av == 0) return in->b;
        break;
    case UAM_OP_SUB:
    case UAM_OP_SHL: case UAM_OP_LSHR: case UAM_OP_ASHR: case UAM_OP_ROTR:
        if (bi && bv == 0) return in->a;
        break;
    case UAM_OP_AND:
        if (bi && bv == m) return in->a;
        if (ai && av == m) return in->b;
        if ((bi && bv == 0) || (ai && av == 0)) return imm_ref(o, 0);
        break;
    case UAM_OP_MUL:
        if (bi && bv == 1) return in->a;
        if (ai && av == 1) return in->b;
        if ((bi && bv == 0) || (ai && av == 0)) return imm_ref(o, 0);
        break;
    case UAM_OP_SELECT:
        if (is_imm(in->a)) return (imm_val(o, in->a) & 1) ? in->b : in->c;
        if (in->b == in->c) return in->b;
        break;
    default:
        break;
    }
    return peephole(o, in);
}

static bool is_pure(uam_opcode op)
{
    const uam_opinfo *oi = &uam_opinfo_table[op];
    return !(oi->props & (UAM_P_EFFECT | UAM_P_GUARD)) && op != UAM_OP_GET;
}

/* Effectful ops whose native code might read or write the register file. */
static bool clobbers_regfile(uam_opcode op)
{
    return op == UAM_OP_HELPER || op == UAM_OP_SYSRD || op == UAM_OP_SYSWR;
}

static uam_ref *ref_slot(uam_insn *in, unsigned i)
{
    switch (i) {
    case 0:  return &in->a;
    case 1:  return &in->b;
    default: return &in->c;
    }
}

bool uam_optimize(uam_block *blk)
{
    if (!blk->n_insn)
        return true;
    opt o = { .blk = blk };
    o.repl = malloc(sizeof(*o.repl) * (blk->n_temp ? blk->n_temp : 1));
    if (!o.repl)
        return false;
    for (uint32_t t = 0; t < blk->n_temp; t++)
        o.repl[t] = t;
    uint32_t cse_size = 16;
    while (cse_size < 2 * blk->n_insn)
        cse_size *= 2;
    o.def = malloc(sizeof(*o.def) * (blk->n_temp ? blk->n_temp : 1));
    o.cse = malloc(sizeof(*o.cse) * cse_size);
    if (!o.def || !o.cse) {
        free(o.def);
        free(o.cse);
        free(o.repl);
        return false;
    }
    o.cse_mask = cse_size - 1;
    memset(o.def, 0xFF, sizeof(*o.def) * blk->n_temp);
    memset(o.cse, 0xFF, sizeof(*o.cse) * cse_size);

    /* ---- 1. forward pass (compacts in place) ---- */
    uint32_t w = 0;
    bool ended = false;
    for (uint32_t i = 0; i < blk->n_insn && !ended && !o.oom; i++) {
        uam_insn in = blk->insn[i];
        const uam_opcode op = (uam_opcode)in.op;
        const uam_ref orig_a = in.a;

        in.guard = rewrite(&o, in.guard);
        for (unsigned k = 0; k < 3; k++)
            *ref_slot(&in, k) = rewrite(&o, *ref_slot(&in, k));

        /* Constant guards. */
        if (!uam_ref_is_none(in.guard) && is_imm(in.guard)) {
            if (!(imm_val(&o, in.guard) & 1)) {
                /* The op does nothing (a skipped PUT leaves known registers valid). */
                if (!uam_ref_is_none(in.dst))
                    o.repl[in.dst] = imm_ref(&o, 0);
                continue;
            }
            in.guard = UAM_REF_NONE;
        }

        if (is_pure(op)) {
            unsigned src_w = in.width;
            if (op == UAM_OP_ZEXT || op == UAM_OP_SEXT || op == UAM_OP_TRUNC)
                src_w = uam_ref_is_imm(orig_a) ? in.width : blk->temp_width[orig_a];
            if (commutative(op) && is_imm(in.a) && !is_imm(in.b)) {
                uam_ref t = in.a;
                in.a = in.b;
                in.b = t;
            }
            uam_ref r = simplify(&o, &in, src_w);
            if (uam_ref_is_none(r))
                r = cse_find(&o, &in);
            if (!uam_ref_is_none(r)) {
                o.repl[in.dst] = r;
                continue;
            }
            cse_add(&o, &in, w);
        } else if (op == UAM_OP_GET) {
            uam_ref k = lookup_known(&o, in.aux, in.width);
            if (!uam_ref_is_none(k)) {
                o.repl[in.dst] = k;
                continue;
            }
            remember(&o, in.aux, in.width, in.dst);
        } else if (op == UAM_OP_PUT) {
            if (uam_ref_is_none(in.guard))
                remember(&o, in.aux, in.width, in.a);
            else
                forget_overlapping(&o, in.aux, in.width);
        } else if (clobbers_regfile(op)) {
            o.n_known = 0;
        }

        /* An unconditional EXIT or TRAP ends the block. */
        if ((op == UAM_OP_EXIT || op == UAM_OP_TRAP || op == UAM_OP_WAIT) && uam_ref_is_none(in.guard))
            ended = true;
        if (!uam_ref_is_none(in.dst))
            o.def[in.dst] = w;
        blk->insn[w++] = in;
    }
    free(o.def);
    free(o.cse);
    if (o.oom) {
        free(o.repl);
        return false;
    }
    blk->n_insn = w;

    /* ---- 2. dead register writes within an instruction (backward) ---- */
    uint32_t reg_end = 1;
    for (uint32_t i = 0; i < blk->n_insn; i++) {
        const uam_insn *in = &blk->insn[i];
        if ((in->op == UAM_OP_PUT || in->op == UAM_OP_GET) && in->aux + in->width / 8u > reg_end)
            reg_end = in->aux + in->width / 8u;
    }
    bool *dead = calloc(blk->n_insn, sizeof(*dead));
    uint8_t *written = calloc(reg_end, 1);   /* bytes a later PUT of this instruction writes */
    if (!dead || !written) {
        free(dead);
        free(written);
        free(o.repl);
        return false;
    }
    for (uint32_t i = blk->n_insn; i-- > 0;) {
        const uam_insn *in = &blk->insn[i];
        const unsigned nb = in->width / 8u;
        if (in->op == UAM_OP_PUT) {
            bool covered = true;
            for (unsigned k = 0; k < nb; k++)
                covered &= written[in->aux + k];
            if (covered)
                dead[i] = true;
            else if (uam_ref_is_none(in->guard))
                memset(&written[in->aux], 1, nb);
        } else if (in->op == UAM_OP_GET) {
            memset(&written[in->aux], 0, nb);
        } else if (!is_pure((uam_opcode)in->op)) {
            memset(written, 0, reg_end);   /* an instruction boundary or anything else */
        }
    }
    free(written);

    /* ---- 3. dead code elimination ---- */
    uint32_t *uses = calloc(blk->n_temp ? blk->n_temp : 1, sizeof(*uses));
    if (!uses) {
        free(dead);
        free(o.repl);
        return false;
    }
    for (uint32_t i = 0; i < blk->n_insn; i++) {
        const uam_insn *in = &blk->insn[i];
        if (dead[i])
            continue;
        const uam_ref refs[4] = { in->guard, in->a, in->b, in->c };
        for (unsigned k = 0; k < 4; k++)
            if (!uam_ref_is_none(refs[k]) && !is_imm(refs[k]))
                uses[refs[k]]++;
    }
    for (uint32_t i = blk->n_insn; i-- > 0;) {
        const uam_insn *in = &blk->insn[i];
        const uam_opcode op = (uam_opcode)in->op;
        if ((is_pure(op) || op == UAM_OP_GET) && !uam_ref_is_none(in->dst) && uses[in->dst] == 0) {
            dead[i] = true;
            const uam_ref refs[3] = { in->a, in->b, in->c };
            for (unsigned k = 0; k < 3; k++)
                if (!uam_ref_is_none(refs[k]) && !is_imm(refs[k]))
                    uses[refs[k]]--;
        }
    }
    w = 0;
    uint32_t n_guest = 0;
    for (uint32_t i = 0; i < blk->n_insn; i++) {
        if (dead[i])
            continue;
        if (blk->insn[i].op == UAM_OP_IMARK)
            n_guest++;
        blk->insn[w++] = blk->insn[i];
    }
    blk->n_insn = w;
    blk->n_guest = n_guest;
    free(dead);
    free(uses);
    free(o.repl);
    return true;
}
