/*
 * cmp.c - The compare pass (uam/cmp.h).
 *
 * Inserts UAM_OP_CMPLOG in front of each compare it recognises (docs/uam-isa.md section
 * 7.6): directly before the compare, or before the SUB / XOR whose difference a flags
 * ISA tests for zero, so the operands' live ranges grow as little as possible and a
 * compare stays next to the branch or select the JIT fuses it with. Depends on the IR
 * only.
 */
#include <stdlib.h>
#include <string.h>

#include "uam/cmp.h"

struct uam_cmp_pass {
    uam_pass pass;
};

uint32_t uam_cmp_site(uint64_t pc, unsigned k)
{
    uint64_t v = pc * UINT64_C(0x9E3779B97F4A7C15) + k;
    v ^= v >> 31;
    v *= UINT64_C(0xBF58476D1CE4E5B9);
    v ^= v >> 29;
    return (uint32_t)v;
}

/* A record: where it goes and what it says. */
typedef struct rec {
    uint32_t at;        /* insert before this insn */
    uint32_t seq;       /* order found, for a stable sort */
    uam_ref  rx, ry;    /* the first compare's operands, in its own order */
    uint8_t  dirs;      /* ordered compares seen: 1 as (rx, ry), 2 as (ry, rx) */
    uam_insn op;
} rec;

static int rec_cmp(const void *x, const void *y)
{
    const rec *a = x, *b = y;
    if (a->at != b->at)
        return a->at < b->at ? -1 : 1;
    return a->seq < b->seq ? -1 : a->seq > b->seq;
}

static bool is_zero(const uam_block *blk, uam_ref r)
{
    return uam_ref_is_imm(r) && blk->imm[uam_ref_index(r)] == 0;
}

static uam_status cmp_run(void *ctx, uam_block *blk)
{
    (void)ctx;
    const uint32_t n = blk->n_insn;
    uint32_t *def = malloc(sizeof(*def) * (blk->n_temp ? blk->n_temp : 1));
    rec *recs = NULL;
    uint32_t n_rec = 0, cap_rec = 0, first_of_insn = 0, k_in_insn = 0;
    uint64_t pc = 0;
    uam_status st = def ? UAM_OK : UAM_ERR_NOMEM;
    for (uint32_t t = 0; def && t < blk->n_temp; t++)
        def[t] = UINT32_MAX;

    for (uint32_t i = 0; i < n && st == UAM_OK; i++) {
        const uam_insn *in = &blk->insn[i];
        if (uam_ref_is_temp(in->dst) && !uam_ref_is_none(in->dst) && in->dst < blk->n_temp)
            def[in->dst] = i;
        if (in->op == UAM_OP_IMARK) {
            pc = blk->imm[uam_ref_index(in->a)];
            first_of_insn = n_rec;
            k_in_insn = 0;
            continue;
        }
        unsigned kind;
        switch ((uam_opcode)in->op) {
        case UAM_OP_EQ: case UAM_OP_NE: kind = UAM_CMP_EQ; break;
        case UAM_OP_ULT: case UAM_OP_ULE: kind = UAM_CMP_ORDER; break;
        case UAM_OP_SLT: case UAM_OP_SLE: case UAM_OP_SBORROW:
            kind = UAM_CMP_ORDER | UAM_CMP_SIGNED;
            break;
        default:
            continue;
        }
        const unsigned W = in->width;
        if (W < 8)
            continue;
        uam_ref x = in->a, y = in->b;
        uint32_t at = i;
        if (in->op != UAM_OP_SBORROW && (is_zero(blk, x) || is_zero(blk, y))) {
            /* a difference tested against zero: the flags of a compare */
            uam_ref d = is_zero(blk, y) ? x : y;
            const uam_insn *dd = uam_ref_is_temp(d) && d < blk->n_temp && def[d] != UINT32_MAX
                                 ? &blk->insn[def[d]] : NULL;
            if (!dd || (dd->op != UAM_OP_SUB && dd->op != UAM_OP_XOR) || dd->width != W)
                continue;                      /* a zero or sign flag of anything else */
            x = dd->a;
            y = dd->b;
            at = def[d];
        }
        if (x == y || (uam_ref_is_imm(x) && uam_ref_is_imm(y)) || is_zero(blk, x) || is_zero(blk, y))
            continue;
        const uam_ref rx = x, ry = y;          /* as compared */
        if (uam_ref_is_imm(x)) {               /* the constant goes to b */
            x = ry;
            y = rx;
        }
        if (uam_ref_is_imm(y)) {
            kind |= UAM_CMP_IMM;
            /* x ^ c1, x + c1 or x - c1 against c2 is x against the constant x must equal
             * (PowerPC's xoris + cmplwi, a compiler's sub + cmp of a range check): the
             * record names x, whose bytes may be in the input */
            const uam_insn *dx = uam_ref_is_temp(x) && x < blk->n_temp && def[x] != UINT32_MAX
                                 ? &blk->insn[def[x]] : NULL;
            if (dx && dx->width == W
                && (dx->op == UAM_OP_XOR || dx->op == UAM_OP_ADD || dx->op == UAM_OP_SUB)
                && uam_ref_is_imm(dx->a) != uam_ref_is_imm(dx->b)) {
                const uint64_t m = uam_mask(W), c2 = blk->imm[uam_ref_index(y)] & m;
                const bool ci = uam_ref_is_imm(dx->a);       /* the constant is dx's a */
                const uint64_t c1 = blk->imm[uam_ref_index(ci ? dx->a : dx->b)] & m;
                uint64_t v;
                if (dx->op == UAM_OP_XOR)
                    v = c1 ^ c2;
                else if (dx->op == UAM_OP_ADD)
                    v = c2 - c1;
                else
                    v = ci ? c1 - c2 : c2 + c1;              /* c1 - x = c2, x - c1 = c2 */
                v &= m;
                if (v) {
                    uam_ref vr = uam_block_imm(blk, v);
                    if (uam_ref_is_none(vr)) {
                        st = UAM_ERR_NOMEM;
                        break;
                    }
                    x = ci ? dx->b : dx->a;
                    y = vr;
                }
            }
        }
        /* one record per pair of operands per instruction; a pair ordered both ways
         * (x < y and y < x: PowerPC's cmpw) is an equality test too */
        bool dup = false;
        for (uint32_t r = first_of_insn; r < n_rec; r++) {
            rec *e = &recs[r];
            if (e->op.width != W || !((e->rx == rx && e->ry == ry) || (e->rx == ry && e->ry == rx)))
                continue;
            if (kind & UAM_CMP_ORDER)
                e->dirs |= e->rx == rx ? 1 : 2;
            e->op.flags |= (uint16_t)(kind | (e->dirs == 3 ? UAM_CMP_EQ : 0));
            if (at < e->at)
                e->at = at;
            dup = true;
            break;
        }
        if (dup)
            continue;
        if (n_rec == cap_rec) {
            uint32_t nc = cap_rec ? cap_rec * 2 : 16;
            rec *nr = realloc(recs, sizeof(*nr) * nc);
            if (!nr) {
                st = UAM_ERR_NOMEM;
                break;
            }
            recs = nr;
            cap_rec = nc;
        }
        recs[n_rec] = (rec){ .at = at, .seq = n_rec, .rx = rx, .ry = ry,
                             .dirs = kind & UAM_CMP_ORDER ? 1 : 0,
                             .op = { .op = UAM_OP_CMPLOG, .width = (uint8_t)W,
                                     .flags = (uint16_t)kind, .aux = uam_cmp_site(pc, k_in_insn++),
                                     .guard = UAM_REF_NONE, .dst = UAM_REF_NONE, .a = x, .b = y,
                                     .c = UAM_REF_NONE } };
        n_rec++;
    }

    if (st == UAM_OK && n_rec) {
        qsort(recs, n_rec, sizeof(*recs), rec_cmp);
        uint32_t *at = malloc(sizeof(*at) * n_rec);
        uam_insn *ops = malloc(sizeof(*ops) * n_rec);
        if (at && ops) {
            for (uint32_t r = 0; r < n_rec; r++) {
                at[r] = recs[r].at;
                ops[r] = recs[r].op;
            }
            st = uam_block_insert(blk, at, ops, n_rec);
        } else {
            st = UAM_ERR_NOMEM;
        }
        free(at);
        free(ops);
    }
    free(recs);
    free(def);
    return st;
}

uam_status uam_cmp_pass_create(uam_cmp_pass **out)
{
    uam_cmp_pass *cp = calloc(1, sizeof(*cp));
    *out = cp;
    if (!cp)
        return UAM_ERR_NOMEM;
    cp->pass = (uam_pass){ .name = "compare", .run = cmp_run, .ctx = cp };
    return UAM_OK;
}

const uam_pass *uam_cmp_pass_get(const uam_cmp_pass *cp) { return &cp->pass; }

void uam_cmp_pass_destroy(uam_cmp_pass *cp) { free(cp); }
