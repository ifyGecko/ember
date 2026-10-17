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
 *        ends the block (everything after it is unreachable).
 *   2. dead code elimination: pure ops and GETs whose result is unused are removed.
 *
 * Nothing that has a side effect is moved or removed (except unreachable code and ops
 * whose guard is constant false), IMARKs are kept, and PUTs are never reordered, so the
 * commit rule (section 8.5) and precise exceptions are preserved.
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
    known_reg  known[MAX_KNOWN];
    unsigned   n_known;
    bool       oom;
} opt;

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
static uam_ref simplify(opt *o, const uam_insn *in, unsigned src_w)
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
    return UAM_REF_NONE;
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
            uam_ref r = simplify(&o, &in, src_w);
            if (!uam_ref_is_none(r)) {
                o.repl[in.dst] = r;
                continue;
            }
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
        blk->insn[w++] = in;
    }
    if (o.oom) {
        free(o.repl);
        return false;
    }
    blk->n_insn = w;

    /* ---- 2. dead code elimination ---- */
    uint32_t *uses = calloc(blk->n_temp ? blk->n_temp : 1, sizeof(*uses));
    if (!uses) {
        free(o.repl);
        return false;
    }
    for (uint32_t i = 0; i < blk->n_insn; i++) {
        const uam_insn *in = &blk->insn[i];
        const uam_ref refs[4] = { in->guard, in->a, in->b, in->c };
        for (unsigned k = 0; k < 4; k++)
            if (!uam_ref_is_none(refs[k]) && !is_imm(refs[k]))
                uses[refs[k]]++;
    }
    bool *dead = calloc(blk->n_insn, sizeof(*dead));
    if (!dead) {
        free(uses);
        free(o.repl);
        return false;
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
