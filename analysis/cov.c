/*
 * cov.c - The edge-coverage pass (uam/cov.h).
 *
 * Inserts UAM_OP_COV directly after the IMARK of each instrumented instruction
 * (docs/uam-isa.md section 7.5). Depends on the IR only.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uam/cov.h"

struct uam_cov_pass {
    uam_pass      pass;
    bool          blocks;  /* block mode: instrument each block's first instruction */
    uam_cov_site *site;    /* sorted by pc */
    size_t        n_site;
    uam_cov_namer_fn namer;
    void         *namer_ctx;
};

uint32_t uam_cov_block_hash(uint64_t pc)
{
    uint64_t v = pc;
    v ^= v >> 33;
    v *= UINT64_C(0xff51afd7ed558ccd);
    v ^= v >> 33;
    return (uint32_t)v;
}

static const uam_cov_site *find_site(const uam_cov_pass *cp, uint64_t pc)
{
    size_t lo = 0, hi = cp->n_site;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (cp->site[mid].pc < pc)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < cp->n_site && cp->site[lo].pc == pc ? &cp->site[lo] : NULL;
}

static uam_insn cov_op(uam_block *blk, uint32_t a, uint32_t b)
{
    return (uam_insn){ .op = UAM_OP_COV, .width = 32, .guard = UAM_REF_NONE,
                       .dst = UAM_REF_NONE, .a = uam_block_imm(blk, a),
                       .b = uam_block_imm(blk, b), .c = UAM_REF_NONE };
}

static uam_status run_blocks(uam_block *blk)
{
    uint32_t h = uam_cov_block_hash(blk->guest_pc);
    uam_insn op = cov_op(blk, h, h >> 1);
    if (uam_ref_is_none(op.a) || uam_ref_is_none(op.b))
        return UAM_ERR_NOMEM;
    const uint32_t at = 1;   /* right after the block's first IMARK */
    return uam_block_insert(blk, &at, &op, 1);
}

static uam_status run_sites(const uam_cov_pass *cp, uam_block *blk)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < blk->n_insn; i++) {
        const uam_insn *in = &blk->insn[i];
        if (in->op != UAM_OP_IMARK || !find_site(cp, blk->imm[uam_ref_index(in->a)]))
            continue;
        if (in->flags & UAM_IMARK_CONT) {
            uint64_t pc = blk->imm[uam_ref_index(in->a)];
            char name[128] = "";
            if (cp->namer)
                cp->namer(cp->namer_ctx, pc, name, sizeof(name));
            uam_log(UAM_LOG_ERROR, "cov", "coverage site 0x%llx%s%s%s is a continuation "
                    "(delay-slot) instruction, which cannot be instrumented",
                    (unsigned long long)pc, name[0] ? " (" : "", name, name[0] ? ")" : "");
            return UAM_ERR_INVALID;
        }
        n++;
    }
    if (!n)
        return UAM_OK;

    uint32_t *at = malloc(sizeof(*at) * n);
    uam_insn *ops = malloc(sizeof(*ops) * n);
    uam_status st = at && ops ? UAM_OK : UAM_ERR_NOMEM;
    uint32_t k = 0;
    for (uint32_t i = 0; i < blk->n_insn && st == UAM_OK; i++) {
        const uam_insn *in = &blk->insn[i];
        if (in->op != UAM_OP_IMARK)
            continue;
        const uam_cov_site *s = find_site(cp, blk->imm[uam_ref_index(in->a)]);
        if (!s)
            continue;
        at[k] = i + 1;
        ops[k] = cov_op(blk, s->a, s->b);   /* may move blk->imm; `in` stays valid */
        if (uam_ref_is_none(ops[k].a) || uam_ref_is_none(ops[k].b))
            st = UAM_ERR_NOMEM;
        k++;
    }
    if (st == UAM_OK)
        st = uam_block_insert(blk, at, ops, n);
    free(at);
    free(ops);
    return st;
}

static uam_status cov_run(void *ctx, uam_block *blk)
{
    const uam_cov_pass *cp = ctx;
    return cp->blocks ? run_blocks(blk) : run_sites(cp, blk);
}

static int site_cmp(const void *x, const void *y)
{
    const uam_cov_site *a = x, *b = y;
    return a->pc < b->pc ? -1 : a->pc > b->pc;
}

static uam_cov_pass *alloc_pass(void)
{
    uam_cov_pass *cp = calloc(1, sizeof(*cp));
    if (cp)
        cp->pass = (uam_pass){ .name = "coverage", .run = cov_run, .ctx = cp };
    return cp;
}

uam_status uam_cov_pass_create(const uam_cov_site *sites, size_t n, uam_cov_pass **out,
                               char *err, size_t errlen)
{
    *out = NULL;
    if (err && errlen)
        err[0] = '\0';
    uam_cov_pass *cp = alloc_pass();
    if (!cp)
        return UAM_ERR_NOMEM;
    if (n) {
        cp->site = malloc(sizeof(*cp->site) * n);
        if (!cp->site) {
            free(cp);
            return UAM_ERR_NOMEM;
        }
        memcpy(cp->site, sites, sizeof(*cp->site) * n);
        qsort(cp->site, n, sizeof(*cp->site), site_cmp);
        for (size_t i = 1; i < n; i++) {
            if (cp->site[i].pc == cp->site[i - 1].pc) {
                if (err && errlen)
                    snprintf(err, errlen, "two coverage sites at pc 0x%llx",
                             (unsigned long long)cp->site[i].pc);
                uam_cov_pass_destroy(cp);
                return UAM_ERR_INVALID;
            }
        }
    }
    cp->n_site = n;
    *out = cp;
    return UAM_OK;
}

uam_status uam_cov_pass_create_blocks(uam_cov_pass **out)
{
    *out = alloc_pass();
    if (!*out)
        return UAM_ERR_NOMEM;
    (*out)->blocks = true;
    return UAM_OK;
}

const uam_pass *uam_cov_pass_get(const uam_cov_pass *cp) { return &cp->pass; }

void uam_cov_pass_set_namer(uam_cov_pass *cp, uam_cov_namer_fn fn, void *ctx)
{
    cp->namer = fn;
    cp->namer_ctx = ctx;
}

size_t uam_cov_pass_sites(const uam_cov_pass *cp) { return cp->n_site; }

void uam_cov_pass_destroy(uam_cov_pass *cp)
{
    if (!cp)
        return;
    free(cp->site);
    free(cp);
}
