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
    uint32_t      mask;    /* block mode with options: the map's mask (0: plain) */
    bool          splits;  /* block mode: mark blocks cut by a limit (UAM_COV_SPLIT) */
    bool          erets;   /* block mode: mark exception returns (UAM_COV_ERET) */
    uam_cov_range *range;  /* block mode: only blocks starting in these (sorted by lo) */
    size_t        n_range;
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

bool uam_cov_pass_covers(const uam_cov_pass *cp, uint64_t pc)
{
    if (!cp->blocks || !cp->n_range)
        return true;
    size_t lo = 0, hi = cp->n_range;   /* the last range with lo <= pc */
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (cp->range[mid].lo <= pc)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo && pc < cp->range[lo - 1].hi;
}

/* The pc after the block when it ends only because ember cut it (its length, a page end,
 * an unfetchable next instruction): the last instruction is no branch (no guarded EXIT
 * after its IMARK) and the final EXIT is a plain jump to the next instruction. A
 * conditional branch's not-taken path also jumps there, but after a guarded EXIT. */
static bool split_end(const uam_block *blk, uint64_t *next)
{
    if (!blk->n_insn)
        return false;
    const uam_insn *fin = &blk->insn[blk->n_insn - 1];
    if (fin->op != UAM_OP_EXIT || !uam_ref_is_none(fin->guard) || fin->aux != UAM_EXIT_JUMP
        || !uam_ref_is_imm(fin->a))
        return false;
    uint32_t last_any = UINT32_MAX, last_main = UINT32_MAX;
    for (uint32_t i = 0; i < blk->n_insn; i++) {
        if (blk->insn[i].op != UAM_OP_IMARK)
            continue;
        last_any = i;
        if (!(blk->insn[i].flags & UAM_IMARK_CONT))
            last_main = i;
    }
    if (last_main == UINT32_MAX)
        return false;
    for (uint32_t i = last_main + 1; i + 1 < blk->n_insn; i++)
        if (blk->insn[i].op == UAM_OP_EXIT && blk->insn[i].aux != UAM_EXIT_RETRY)
            return false;   /* a branch: its fall-through is a real edge */
    const uam_insn *im = &blk->insn[last_any];
    const uint64_t end = blk->imm[uam_ref_index(im->a)] + im->aux;
    if (blk->imm[uam_ref_index(fin->a)] != end)
        return false;
    *next = end;
    return true;
}

/* Where a marker for the EXIT at index e goes: before it and the fences that must
 * directly precede it. */
static uint32_t before_exit(const uam_block *blk, uint32_t e)
{
    while (e > 1 && blk->insn[e - 1].op == UAM_OP_FENCE)
        e--;
    return e;
}

static uam_status run_blocks(const uam_cov_pass *cp, uam_block *blk)
{
    uint32_t n_eret = 0;
    for (uint32_t i = 0; cp->erets && i < blk->n_insn; i++)
        n_eret += blk->insn[i].op == UAM_OP_EXIT && blk->insn[i].aux == UAM_EXIT_EXC_RETURN;
    uam_insn small_ops[4], *ops = small_ops;
    uint32_t small_at[4], *at = small_at, n = 0;
    if (n_eret > 2) {
        ops = malloc(sizeof(*ops) * (n_eret + 2));
        at = malloc(sizeof(*at) * (n_eret + 2));
        if (!ops || !at) {
            free(ops);
            free(at);
            return UAM_ERR_NOMEM;
        }
    }
    uam_status st = UAM_OK;
    if (uam_cov_pass_covers(cp, blk->guest_pc)) {
        uint32_t h = uam_cov_block_hash(blk->guest_pc);
        if (cp->mask)
            h &= cp->mask;
        ops[n] = cov_op(blk, h, h >> 1);
        if (uam_ref_is_none(ops[n].a) || uam_ref_is_none(ops[n].b))
            st = UAM_ERR_NOMEM;
        at[n++] = 1;   /* right after the block's first IMARK */
    }
    /* An exception return, wherever it leads: the next COV may restore a context. */
    for (uint32_t i = 0; n_eret && st == UAM_OK && i < blk->n_insn; i++) {
        if (blk->insn[i].op != UAM_OP_EXIT || blk->insn[i].aux != UAM_EXIT_EXC_RETURN)
            continue;
        ops[n] = cov_op(blk, 0, 0);
        ops[n].flags = UAM_COV_ERET;
        if (uam_ref_is_none(ops[n].a))
            st = UAM_ERR_NOMEM;
        uint32_t k = before_exit(blk, i);
        at[n] = n && k < at[n - 1] ? at[n - 1] : k;   /* never before the block's own COV */
        n++;
    }
    uint64_t next;
    if (st == UAM_OK && cp->splits && split_end(blk, &next) && uam_cov_pass_covers(cp, next)) {
        ops[n] = cov_op(blk, 0, 0);
        ops[n].a = uam_block_imm(blk, next);
        ops[n].flags = UAM_COV_SPLIT;
        if (uam_ref_is_none(ops[n].a) || uam_ref_is_none(ops[n].b))
            st = UAM_ERR_NOMEM;
        at[n++] = before_exit(blk, blk->n_insn - 1);
    }
    if (st == UAM_OK)
        st = uam_block_insert(blk, at, ops, n);
    if (ops != small_ops) {
        free(ops);
        free(at);
    }
    return st;
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
            uam_log(UAM_LOG_ERROR, "cov", "coverage site 0x%llx%s%s%s continues the "
                    "instruction before it (a delay slot, or inside an IT block), so it cannot "
                    "be instrumented",
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
    return cp->blocks ? run_blocks(cp, blk) : run_sites(cp, blk);
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

static int range_cmp(const void *x, const void *y)
{
    const uam_cov_range *a = x, *b = y;
    return a->lo < b->lo ? -1 : a->lo > b->lo;
}

uam_status uam_cov_pass_create_blocks_ex(const uam_cov_block_opts *o, uam_cov_pass **out)
{
    *out = NULL;
    for (size_t i = 0; i < o->n_ranges; i++)
        if (o->ranges[i].lo >= o->ranges[i].hi)
            return UAM_ERR_INVALID;
    uam_cov_pass *cp = alloc_pass();
    if (!cp)
        return UAM_ERR_NOMEM;
    cp->blocks = true;
    cp->mask = o->mask;
    cp->splits = o->splits;
    cp->erets = o->contexts;
    if (o->n_ranges) {
        /* sorted, overlaps merged: covers() then needs one binary search */
        uam_cov_range *r = malloc(sizeof(*r) * o->n_ranges);
        if (!r) {
            free(cp);
            return UAM_ERR_NOMEM;
        }
        memcpy(r, o->ranges, sizeof(*r) * o->n_ranges);
        qsort(r, o->n_ranges, sizeof(*r), range_cmp);
        size_t k = 0;
        for (size_t i = 0; i < o->n_ranges; i++) {
            if (k && r[i].lo <= r[k - 1].hi) {
                if (r[i].hi > r[k - 1].hi)
                    r[k - 1].hi = r[i].hi;
            } else {
                r[k++] = r[i];
            }
        }
        cp->range = r;
        cp->n_range = k;
    }
    *out = cp;
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
    free(cp->range);
    free(cp);
}
