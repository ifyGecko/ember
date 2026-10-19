/*
 * verify.c - Block well-formedness (docs/uam-isa.md section 8).
 *
 * This is the executable definition of what a valid block is. Lifters and
 * passes must only produce blocks that pass; backends may assume it.
 */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "uam/ir.h"

typedef struct vstate {
    const uam_block        *blk;
    const uam_verify_ctx   *ctx;
    uint8_t                *defined;
    char                   *err;
    size_t                  errlen;
    uint32_t                idx;     /* current instruction index */
} vstate;

static bool fail(vstate *v, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static bool fail(vstate *v, const char *fmt, ...)
{
    if (v->err && v->errlen) {
        int n = snprintf(v->err, v->errlen, "insn %u (%s): ", v->idx,
                         v->idx < v->blk->n_insn && v->blk->insn[v->idx].op < UAM_OP__COUNT
                             ? uam_opinfo_table[v->blk->insn[v->idx].op].mnemonic : "?");
        if (n > 0 && (size_t)n < v->errlen) {
            va_list ap;
            va_start(ap, fmt);
            vsnprintf(v->err + n, v->errlen - (size_t)n, fmt, ap);
            va_end(ap);
        }
    }
    return false;
}

static bool valid_width(unsigned w)
{
    return w == 1 || w == 8 || w == 16 || w == 32 || w == 64;
}

/* Check that `r` is a usable value of width `w`. Immediates must fit. */
static bool use(vstate *v, uam_ref r, unsigned w, const char *what)
{
    if (uam_ref_is_none(r))
        return fail(v, "missing operand %s", what);
    if (uam_ref_is_imm(r)) {
        uint32_t i = uam_ref_index(r);
        if (i >= v->blk->n_imm)
            return fail(v, "operand %s: immediate index %u out of range", what, i);
        if (v->blk->imm[i] & ~uam_mask(w))
            return fail(v, "operand %s: immediate 0x%llx does not fit %u bits", what,
                        (unsigned long long)v->blk->imm[i], w);
        return true;
    }
    if (r >= v->blk->n_temp)
        return fail(v, "operand %s: temp t%u out of range", what, r);
    if (!v->defined[r])
        return fail(v, "operand %s: t%u used before definition", what, r);
    if (v->blk->temp_width[r] != w)
        return fail(v, "operand %s: t%u has width %u, expected %u", what, r,
                    v->blk->temp_width[r], w);
    return true;
}

/* Width of a temp operand (conversions need the source width). */
static unsigned temp_width(vstate *v, uam_ref r)
{
    if (!uam_ref_is_temp(r) || r >= v->blk->n_temp || !v->defined[r])
        return 0;
    return v->blk->temp_width[r];
}

static bool none(vstate *v, uam_ref r, const char *what)
{
    if (!uam_ref_is_none(r))
        return fail(v, "operand %s must be empty", what);
    return true;
}

static bool define(vstate *v, uam_ref dst, unsigned w)
{
    if (w == 0)
        return uam_ref_is_none(dst) ? true : fail(v, "unexpected destination");
    if (!uam_ref_is_temp(dst) || uam_ref_is_none(dst) || dst >= v->blk->n_temp)
        return fail(v, "destination must be a temp");
    if (v->defined[dst])
        return fail(v, "t%u defined twice (SSA)", dst);
    if (v->blk->temp_width[dst] != w)
        return fail(v, "destination t%u has width %u, expected %u", dst,
                    v->blk->temp_width[dst], w);
    v->defined[dst] = 1;
    return true;
}

static bool check_mem_flags(vstate *v, const uam_insn *in)
{
    unsigned endian = in->flags & (UAM_ACC_LE | UAM_ACC_BE);
    if (endian != UAM_ACC_LE && endian != UAM_ACC_BE)
        return fail(v, "memory op needs exactly one of LE/BE");
    if (in->flags & ~UAM_ACC_IR_MASK)
        return fail(v, "core-only access attributes in IR");
    if (in->width < 8)
        return fail(v, "memory width must be 8..64");
    return true;
}

static bool check_insn(vstate *v, const uam_insn *in, unsigned aw)
{
    const unsigned W = in->width;
    const uam_opinfo *oi = &uam_opinfo_table[in->op];
    const uam_helper_desc *hd = NULL;

    if (!valid_width(W))
        return fail(v, "invalid width %u", W);

    /* Guard. */
    if (!uam_ref_is_none(in->guard)) {
        bool ok = oi->props & UAM_P_GUARD;
        if (in->op == UAM_OP_HELPER) {
            const uam_helper_table *ht = v->ctx ? v->ctx->helpers : NULL;
            ok = ht && in->aux < ht->count && ht->desc[in->aux].effect != UAM_HELPER_PURE;
        }
        if (!ok)
            return fail(v, "opcode does not accept a guard");
        if (!use(v, in->guard, 1, "guard"))
            return false;
    }

    switch ((uam_opcode)in->op) {
    case UAM_OP_IMARK:
        if (W != aw || !uam_ref_is_imm(in->a))
            return fail(v, "imark needs an immediate pc of address width");
        if (in->aux == 0)
            return fail(v, "imark length must be non-zero");
        if (in->flags & ~(UAM_IMARK_CONT | UAM_IMARK_BREAK))
            return fail(v, "imark has unknown flags");
        if ((in->flags & UAM_IMARK_CONT) && v->idx == 0)
            return fail(v, "continuation imark cannot start a block");
        return use(v, in->a, aw, "pc") && none(v, in->b, "b") && none(v, in->c, "c")
            && define(v, in->dst, 0);

    case UAM_OP_GET:
    case UAM_OP_PUT:
        if (W < 8)
            return fail(v, "register access width must be 8..64");
        if (v->ctx && (size_t)in->aux + W / 8 > v->ctx->regfile_size)
            return fail(v, "register offset %u out of range", in->aux);
        if (in->op == UAM_OP_GET)
            return none(v, in->a, "a") && none(v, in->b, "b") && none(v, in->c, "c")
                && define(v, in->dst, W);
        return use(v, in->a, W, "value") && none(v, in->b, "b") && none(v, in->c, "c")
            && define(v, in->dst, 0);

    case UAM_OP_LD:
        return check_mem_flags(v, in) && use(v, in->a, aw, "addr") && none(v, in->b, "b")
            && none(v, in->c, "c") && define(v, in->dst, W);
    case UAM_OP_ST:
        return check_mem_flags(v, in) && use(v, in->a, aw, "addr") && use(v, in->b, W, "value")
            && none(v, in->c, "c") && define(v, in->dst, (in->flags & UAM_ACC_X) ? 1 : 0);
    case UAM_OP_AMO:
        if (in->aux >= UAM_AMO__COUNT)
            return fail(v, "bad amo sub-op %u", in->aux);
        return check_mem_flags(v, in) && use(v, in->a, aw, "addr") && use(v, in->b, W, "value")
            && none(v, in->c, "c") && define(v, in->dst, W);
    case UAM_OP_CAS:
        return check_mem_flags(v, in) && use(v, in->a, aw, "addr") && use(v, in->b, W, "expected")
            && use(v, in->c, W, "new") && define(v, in->dst, W);
    case UAM_OP_FENCE:
        if (in->aux > UAM_FENCE_INSN)
            return fail(v, "bad fence kind");
        return none(v, in->a, "a") && none(v, in->b, "b") && none(v, in->c, "c")
            && define(v, in->dst, 0);

    case UAM_OP_AND: case UAM_OP_OR: case UAM_OP_XOR:
    case UAM_OP_EQ: case UAM_OP_NE:
        /* Legal at W = 1 (boolean logic). */
        goto binary;
    case UAM_OP_ADD: case UAM_OP_SUB: case UAM_OP_MUL: case UAM_OP_MULHU: case UAM_OP_MULHS:
    case UAM_OP_UDIV: case UAM_OP_SDIV: case UAM_OP_UREM: case UAM_OP_SREM:
    case UAM_OP_SHL: case UAM_OP_LSHR: case UAM_OP_ASHR: case UAM_OP_ROTR:
    case UAM_OP_ULT: case UAM_OP_ULE: case UAM_OP_SLT: case UAM_OP_SLE:
    case UAM_OP_CARRY: case UAM_OP_SCARRY: case UAM_OP_SBORROW:
        if (W < 8)
            return fail(v, "width must be 8..64");
    binary:
        return use(v, in->a, W, "a") && use(v, in->b, W, "b") && none(v, in->c, "c")
            && define(v, in->dst, oi->result == UAM_RK_BOOL ? 1 : W);

    case UAM_OP_CLZ: case UAM_OP_CTZ: case UAM_OP_POPCNT: case UAM_OP_BSWAP:
        if (W < (in->op == UAM_OP_BSWAP ? 16u : 8u))
            return fail(v, "width too small");
        return use(v, in->a, W, "a") && none(v, in->b, "b") && none(v, in->c, "c")
            && define(v, in->dst, W);

    case UAM_OP_ZEXT: case UAM_OP_SEXT: case UAM_OP_TRUNC: {
        unsigned sw = temp_width(v, in->a);
        if (sw == 0)
            return fail(v, "conversion source must be a defined temp");
        if (in->op == UAM_OP_TRUNC ? !(sw > W) : !(sw < W))
            return fail(v, "bad conversion %u -> %u bits", sw, W);
        return none(v, in->b, "b") && none(v, in->c, "c") && define(v, in->dst, W);
    }

    case UAM_OP_SELECT:
        return use(v, in->a, 1, "cond") && use(v, in->b, W, "a") && use(v, in->c, W, "b")
            && define(v, in->dst, W);

    case UAM_OP_EXIT:
        if (in->aux > UAM_EXIT_EXC_RETURN)
            return fail(v, "bad exit kind");
        return use(v, in->a, aw, "target") && none(v, in->b, "b") && none(v, in->c, "c")
            && define(v, in->dst, 0);

    case UAM_OP_SYSRD:
        if (W < 8)
            return fail(v, "sysreg width must be 8..64");
        return none(v, in->a, "a") && none(v, in->b, "b") && none(v, in->c, "c")
            && define(v, in->dst, W);
    case UAM_OP_SYSWR:
        if (W < 8)
            return fail(v, "sysreg width must be 8..64");
        return use(v, in->a, W, "value") && none(v, in->b, "b") && none(v, in->c, "c")
            && define(v, in->dst, 0);

    case UAM_OP_TRAP:
        if (in->aux > UAM_TRAP_HALT)
            return fail(v, "bad trap class");
        return use(v, in->a, aw, "cause") && use(v, in->b, aw, "tval") && none(v, in->c, "c")
            && define(v, in->dst, 0);

    case UAM_OP_WAIT:
        return use(v, in->a, aw, "next_pc") && none(v, in->b, "b") && none(v, in->c, "c")
            && define(v, in->dst, 0);

    case UAM_OP_HELPER: {
        const uam_helper_table *ht = v->ctx ? v->ctx->helpers : NULL;
        if (!ht || in->aux >= ht->count)
            return fail(v, "unknown helper id %u", in->aux);
        hd = &ht->desc[in->aux];
        const uam_ref args[3] = { in->a, in->b, in->c };
        for (unsigned i = 0; i < 3; i++) {
            if (i < hd->n_args) {
                if (!use(v, args[i], hd->arg_width[i], "arg"))
                    return false;
            } else if (!none(v, args[i], "unused arg")) {
                return false;
            }
        }
        return define(v, in->dst, hd->result_width);
    }

    case UAM_OP_COV: {
        const uam_insn *prev = v->idx ? &v->blk->insn[v->idx - 1] : NULL;
        if (W != 32)
            return fail(v, "cov width must be 32");
        if (!prev || prev->op != UAM_OP_IMARK || (prev->flags & UAM_IMARK_CONT))
            return fail(v, "cov must directly follow a non-continuation imark");
        if (!uam_ref_is_imm(in->a) || !uam_ref_is_imm(in->b))
            return fail(v, "cov operands must be immediates");
        return use(v, in->a, 32, "a") && use(v, in->b, 32, "b") && none(v, in->c, "c")
            && define(v, in->dst, 0);
    }

    case UAM_OP__COUNT:
        break;
    }
    return fail(v, "invalid opcode %u", in->op);
}

static bool may_fault(const uam_insn *in, const uam_verify_ctx *ctx)
{
    if (uam_opinfo_table[in->op].props & UAM_P_FAULT)
        return true;
    if (in->op == UAM_OP_HELPER && ctx && ctx->helpers && in->aux < ctx->helpers->count)
        return ctx->helpers->desc[in->aux].effect & UAM_HELPER_MAY_FAULT;
    return false;
}

static bool is_terminator(const uam_insn *in)
{
    if (in->op == UAM_OP_TRAP || in->op == UAM_OP_EXIT)
        return uam_ref_is_none(in->guard);
    return in->op == UAM_OP_WAIT;
}

bool uam_verify(const uam_block *blk, const uam_verify_ctx *ctx, char *err, size_t errlen)
{
    vstate v = { .blk = blk, .ctx = ctx, .err = err, .errlen = errlen, .idx = 0 };
    if (err && errlen)
        err[0] = '\0';

    if (blk->addr_width != 32 && blk->addr_width != 64)
        return fail(&v, "address width must be 32 or 64");
    if (blk->n_insn == 0 || blk->insn[0].op != UAM_OP_IMARK)
        return fail(&v, "block must start with imark");
    if (blk->n_pages > 2)
        return fail(&v, "block covers more than two pages");

    v.defined = calloc(blk->n_temp ? blk->n_temp : 1, 1);
    if (!v.defined)
        return fail(&v, "out of memory");

    bool ok = true;
    bool put_seen = false;      /* commit rule: a PUT in the current instruction */
    uint32_t imarks = 0;

    for (v.idx = 0; v.idx < blk->n_insn && ok; v.idx++) {
        const uam_insn *in = &blk->insn[v.idx];
        if (in->op >= UAM_OP__COUNT) {
            ok = fail(&v, "invalid opcode %u", in->op);
            break;
        }
        if (in->op == UAM_OP_IMARK) {
            put_seen = false;
            imarks++;
        }
        if (!check_insn(&v, in, blk->addr_width)) {
            ok = false;
            break;
        }
        if (may_fault(in, ctx) && put_seen) {
            ok = fail(&v, "commit rule: may-fault op after a register write in the same instruction");
            break;
        }
        if (in->op == UAM_OP_PUT)
            put_seen = true;
        bool term = is_terminator(in);
        bool last = v.idx + 1 == blk->n_insn;
        if (term != last) {
            ok = fail(&v, last ? "block must end with an unguarded exit, trap or wait"
                               : "terminator before end of block");
            break;
        }
        if (in->op == UAM_OP_FENCE && in->aux == UAM_FENCE_INSN
            && !(v.idx + 1 < blk->n_insn && is_terminator(&blk->insn[v.idx + 1]))) {
            ok = fail(&v, "fence.insn must be followed by the block terminator");
            break;
        }
    }

    if (ok && imarks != blk->n_guest) {
        v.idx = 0;
        ok = fail(&v, "n_guest (%u) does not match imark count (%u)", blk->n_guest, imarks);
    }
    if (ok && ctx && ctx->max_guest_insns && blk->n_guest > ctx->max_guest_insns) {
        v.idx = 0;
        ok = fail(&v, "block has %u guest instructions (max %u)", blk->n_guest,
                  ctx->max_guest_insns);
    }
    free(v.defined);
    return ok;
}
