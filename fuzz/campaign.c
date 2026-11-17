/*
 * campaign.c - The campaign engine (uam/fuzz_campaign.h): the record of what runs have
 * shown, judgements against it, and batches of mutations.
 */
#include <stdlib.h>
#include <string.h>

#include "uam/fuzz_campaign.h"

/* An open-addressing table of 64-bit keys and counts, emptied per batch. */
typedef struct counts {
    uint64_t *key;
    uint32_t *n;           /* 0: the slot is free */
    size_t    cap, used;
    uam_fuzz_count *out;   /* the entries as an array (counts_list) */
    size_t    cap_out;
} counts;

struct uam_fuzz_campaign {
    uam_fuzz    *f;
    uam_mutator *mut;
    size_t       map_size;
    uint8_t     *virgin;        /* the count classes seen per bucket (runs that ended normally) */
    uint8_t     *crash_seen;    /* 1: some crash hit this bucket */
    uint64_t     edges;
    uint64_t    *sigs;          /* (verdict << 56 ^ detail) of every crash seen */
    size_t       n_sigs, cap_sigs;
    uint32_t     hang_cap, hang_reports;
    /* the batch */
    uam_fuzz_batch b;
    unsigned       flags;
    const uint8_t *parent;
    size_t         parent_len;
    counts         paths, edge_runs;
    /* a stage: phase 0 is the entry's own run, 1 the inputs derived after it */
    enum { ST_HAVOC, ST_I2S, ST_TRIM } stage;
    unsigned       phase;
    uint8_t       *sbuf;        /* the stage's input being run */
    size_t         sbuf_cap;
    uam_i2s_patch *patches;
    size_t         n_patches, next_patch, cap_patches;
    uint32_t       limit;       /* candidates or probes at most */
    uam_i2s_token  tokens[UAM_FUZZ_I2S_TOKENS];
    uam_fuzz_i2s   i2s;
    uint8_t       *tdata;       /* the entry as trimmed so far */
    size_t         tlen, tcap, remove, floor, pos;
    uint32_t       probes;
    uam_fuzz_trim  trim;
};

uint8_t uam_fuzz_count_class(uint8_t c)
{
    if (c <= 2)   return c;
    if (c == 3)   return 4;
    if (c <= 7)   return 8;
    if (c <= 15)  return 16;
    if (c <= 31)  return 32;
    if (c <= 127) return 64;
    return 128;
}

static uint64_t mix64(uint64_t v)
{
    v ^= v >> 33;
    v *= UINT64_C(0xff51afd7ed558ccd);
    v ^= v >> 33;
    return v;
}

uint64_t uam_fuzz_path_term(uint32_t index, uint8_t count)
{
    /* + 1: the term of bucket 0 with class 0 would otherwise be mix(0) = 0 */
    return mix64(((uint64_t)index << 8 | uam_fuzz_count_class(count)) + 1);
}

uint64_t uam_fuzz_path_end(uint64_t sum) { return mix64(sum); }

/* ---- counts ---------------------------------------------------------------- */

static void counts_free(counts *c)
{
    free(c->key);
    free(c->n);
    free(c->out);
    *c = (counts){ 0 };
}

static void counts_clear(counts *c)
{
    if (c->used)
        memset(c->n, 0, c->cap * sizeof(*c->n));
    c->used = 0;
}

static bool counts_grow(counts *c)
{
    size_t cap = c->cap ? 2 * c->cap : 1024;
    uint64_t *k = calloc(cap, sizeof(*k));
    uint32_t *n = calloc(cap, sizeof(*n));
    if (!k || !n) {
        free(k);
        free(n);
        return false;
    }
    for (size_t i = 0; i < c->cap; i++) {
        if (!c->n[i])
            continue;
        size_t j = (size_t)mix64(c->key[i]) & (cap - 1);
        while (n[j])
            j = (j + 1) & (cap - 1);
        k[j] = c->key[i];
        n[j] = c->n[i];
    }
    free(c->key);
    free(c->n);
    c->key = k;
    c->n = n;
    c->cap = cap;
    return true;
}

static bool counts_add(counts *c, uint64_t key)
{
    if (2 * (c->used + 1) > c->cap && !counts_grow(c))
        return false;
    size_t j = (size_t)mix64(key) & (c->cap - 1);
    while (c->n[j] && c->key[j] != key)
        j = (j + 1) & (c->cap - 1);
    if (!c->n[j]) {
        c->key[j] = key;
        c->used++;
    }
    if (c->n[j] < UINT32_MAX)
        c->n[j]++;
    return true;
}

static const uam_fuzz_count *counts_list(counts *c, size_t *n)
{
    *n = 0;
    if (c->used > c->cap_out) {
        uam_fuzz_count *o = realloc(c->out, sizeof(*o) * c->used);
        if (!o)
            return NULL;
        c->out = o;
        c->cap_out = c->used;
    }
    size_t k = 0;
    for (size_t i = 0; i < c->cap; i++)
        if (c->n[i])
            c->out[k++] = (uam_fuzz_count){ .key = c->key[i], .n = c->n[i] };
    *n = k;
    return c->out;
}

/* ---- life cycle ------------------------------------------------------------ */

uam_fuzz_campaign *uam_fuzz_campaign_create(uam_fuzz *f)
{
    uam_fuzz_campaign *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->f = f;
    uam_fuzz_map(f, &c->map_size);
    c->hang_cap = 64;
    c->virgin = calloc(c->map_size ? c->map_size : 1, 1);
    c->crash_seen = calloc(c->map_size ? c->map_size : 1, 1);
    c->mut = uam_mutator_create();
    if (!c->virgin || !c->crash_seen || !c->mut) {
        uam_fuzz_campaign_destroy(c);
        return NULL;
    }
    return c;
}

void uam_fuzz_campaign_destroy(uam_fuzz_campaign *c)
{
    if (!c)
        return;
    uam_mutator_destroy(c->mut);
    free(c->virgin);
    free(c->crash_seen);
    free(c->sigs);
    counts_free(&c->paths);
    counts_free(&c->edge_runs);
    free(c->sbuf);
    free(c->patches);
    free(c->tdata);
    free(c);
}

uam_mutator *uam_fuzz_campaign_mutator(uam_fuzz_campaign *c) { return c->mut; }
void uam_fuzz_campaign_set_hang_cap(uam_fuzz_campaign *c, uint32_t max) { c->hang_cap = max; }
uint64_t uam_fuzz_campaign_edges(const uam_fuzz_campaign *c) { return c->edges; }
const uam_fuzz_batch *uam_fuzz_campaign_batch(const uam_fuzz_campaign *c) { return &c->b; }

/* ---- judging --------------------------------------------------------------- */

/* The buckets the run set: the harness's record, in the order first hit (no sort), or
 * its sorted list when the record overflowed. NULL: out of memory. */
static const uint32_t *run_hits(uam_fuzz_campaign *c, size_t *n)
{
    const uint32_t *h = uam_fuzz_map_touched(c->f, n);
    return h ? h : uam_fuzz_map_hits(c->f, c->map_size, n);
}

static bool sig_new(uam_fuzz_campaign *c, uam_fuzz_verdict v, uint64_t detail)
{
    const uint64_t sig = (uint64_t)v << 56 ^ detail;
    for (size_t i = 0; i < c->n_sigs; i++)
        if (c->sigs[i] == sig)
            return false;
    if (c->n_sigs == c->cap_sigs) {
        size_t cap = c->cap_sigs ? 2 * c->cap_sigs : 64;
        uint64_t *p = realloc(c->sigs, cap * sizeof(*p));
        if (!p)
            return true;   /* novel; the set stays as it is */
        c->sigs = p;
        c->cap_sigs = cap;
    }
    c->sigs[c->n_sigs++] = sig;
    return true;
}

static bool is_crash(uam_fuzz_verdict v)
{
    return v == UAM_FUZZ_CRASH_FAULT || v == UAM_FUZZ_CRASH_ORACLE || v == UAM_FUZZ_CRASH_GUARD;
}

/* Judge and fold; *hits (optional) gets the run's bucket list for the caller's counting. */
static uam_status judge(uam_fuzz_campaign *c, uam_fuzz_verdict v, uint64_t detail,
                        uam_fuzz_judgement *j, const uint32_t **hits_out, size_t *n_out)
{
    *j = (uam_fuzz_judgement){ .verdict = v, .detail = detail };
    if (hits_out)
        *hits_out = NULL;
    if (v != UAM_FUZZ_OK && !is_crash(v))
        return UAM_OK;   /* hang, timeout, rejected: nothing to fold */
    size_t n;
    const uint32_t *hits = run_hits(c, &n);
    if (!hits)
        return UAM_ERR_NOMEM;
    const uint8_t *map = uam_fuzz_map(c->f, NULL);
    if (v == UAM_FUZZ_OK) {
        uint64_t sum = 0;
        for (size_t i = 0; i < n; i++) {
            const uint32_t ix = hits[i];
            const uint8_t cl = uam_fuzz_count_class(map[ix]), old = c->virgin[ix];
            sum += uam_fuzz_path_term(ix, map[ix]);
            if (!(old & cl)) {
                c->virgin[ix] = old | cl;
                j->new_classes++;
                j->new_buckets += !old;
            }
        }
        c->edges += j->new_buckets;
        j->path = uam_fuzz_path_end(sum);
    } else {
        bool fresh = false;
        for (size_t i = 0; i < n; i++) {
            if (!c->crash_seen[hits[i]]) {
                c->crash_seen[hits[i]] = 1;
                fresh = true;
            }
        }
        j->novel_crash = sig_new(c, v, detail) || fresh;
    }
    if (hits_out) {
        *hits_out = hits;
        *n_out = n;
    }
    return UAM_OK;
}

uam_status uam_fuzz_campaign_judge(uam_fuzz_campaign *c, uam_fuzz_verdict v, uint64_t detail,
                                   uam_fuzz_judgement *j)
{
    return judge(c, v, detail, j, NULL, NULL);
}

/* ---- stages ---------------------------------------------------------------- */

static bool grow(uint8_t **buf, size_t *cap, size_t need)
{
    if (need <= *cap)
        return true;
    uint8_t *p = realloc(*buf, need);
    if (!p)
        return false;
    *buf = p;
    *cap = need;
    return true;
}

static uam_status stage_begin(uam_fuzz_campaign *c, int stage, uint32_t entry, uint32_t limit,
                              uint32_t max, unsigned flags)
{
    c->b = (uam_fuzz_batch){ 0 };
    counts_clear(&c->paths);
    counts_clear(&c->edge_runs);
    c->flags = flags;
    c->stage = stage;
    c->phase = 0;
    c->limit = limit;
    c->parent = uam_mutator_corpus_get(c->mut, entry, &c->parent_len);
    if (!c->parent || !limit || limit > max)
        return UAM_ERR_INVALID;
    /* room for the entry, and for a fix-up to grow an input up to the mutator's limit */
    size_t cap = uam_mutator_max_size(c->mut);
    if (cap < c->parent_len)
        cap = c->parent_len;
    if (!grow(&c->sbuf, &c->sbuf_cap, cap ? cap : 1))
        return UAM_ERR_NOMEM;
    c->b.left = 1;   /* the entry's own run */
    return UAM_OK;
}

uam_status uam_fuzz_campaign_i2s_begin(uam_fuzz_campaign *c, uint32_t entry,
                                       uint32_t max_candidates, unsigned flags)
{
    c->i2s = (uam_fuzz_i2s){ .tokens = c->tokens };
    uam_status st = stage_begin(c, ST_I2S, entry, max_candidates, UAM_FUZZ_I2S_MAX, flags);
    if (st != UAM_OK)
        return st;
    if (!(uam_fuzz_cmp_modes(c->f) & UAM_FUZZ_CMP_LOG)) {
        c->b.left = 0;
        return UAM_ERR_INVALID;
    }
    if (max_candidates > c->cap_patches) {
        uam_i2s_patch *p = realloc(c->patches, sizeof(*p) * max_candidates);
        if (!p) {
            c->b.left = 0;
            return UAM_ERR_NOMEM;
        }
        c->patches = p;
        c->cap_patches = max_candidates;
    }
    c->n_patches = c->next_patch = 0;
    return UAM_OK;
}

uam_status uam_fuzz_campaign_trim_begin(uam_fuzz_campaign *c, uint32_t entry,
                                        uint32_t max_probes, unsigned flags)
{
    c->trim = (uam_fuzz_trim){ 0 };
    uam_status st = stage_begin(c, ST_TRIM, entry, max_probes, UAM_FUZZ_TRIM_MAX, flags);
    if (st != UAM_OK)
        return st;
    if (!grow(&c->tdata, &c->tcap, c->sbuf_cap)) {
        c->b.left = 0;
        return UAM_ERR_NOMEM;
    }
    memcpy(c->tdata, c->parent, c->parent_len);
    c->tlen = c->parent_len;
    c->probes = 0;
    c->trim.data = c->tdata;
    c->trim.len = c->tlen;
    return UAM_OK;
}

const uam_fuzz_i2s  *uam_fuzz_campaign_i2s(const uam_fuzz_campaign *c)  { return &c->i2s; }
const uam_fuzz_trim *uam_fuzz_campaign_trim(const uam_fuzz_campaign *c) { return &c->trim; }

/* The stage's next input into sbuf. */
static bool stage_next(uam_fuzz_campaign *c, const uint8_t **data, size_t *len)
{
    uint8_t *buf = c->sbuf;
    size_t n;
    if (c->phase == 0) {
        memcpy(buf, c->parent, c->parent_len);
        n = c->parent_len;
        if (c->stage == ST_I2S)
            uam_fuzz_cmp_arm(c->f);
    } else if (c->stage == ST_I2S) {
        const uam_i2s_patch *p = &c->patches[c->next_patch++];
        memcpy(buf, c->parent, c->parent_len);
        memcpy(buf + p->pos, p->bytes, p->len);
        n = uam_fuzz_fixup(c->f, buf, c->parent_len, c->sbuf_cap);
    } else {
        /* the entry as trimmed so far, without the chunk at pos */
        const size_t cut = c->tlen - c->pos < c->remove ? c->tlen - c->pos : c->remove;
        memcpy(buf, c->tdata, c->pos);
        memcpy(buf + c->pos, c->tdata + c->pos + cut, c->tlen - c->pos - cut);
        n = uam_fuzz_fixup(c->f, buf, c->tlen - cut, c->sbuf_cap);
        c->probes++;
    }
    c->b.data = buf;
    c->b.len = n;
    c->b.ops = 0;
    c->b.left--;
    *data = buf;
    *len = n;
    return true;
}

/* Trimming: is there another cut to try? Sets the batch's `left`, and the result when not. */
static void trim_seek(uam_fuzz_campaign *c)
{
    while (c->remove >= c->floor && c->probes < c->limit) {
        if (c->pos < c->tlen) {
            c->b.left = 1;
            return;
        }
        c->remove /= 2;
        c->pos = c->remove;
    }
    c->b.left = 0;
    c->trim.len = c->tlen;
    c->trim.removed = (uint32_t)(c->parent_len - c->tlen);
}

/* A stage's run was judged: derive what runs next from it. */
static uam_status stage_judged(uam_fuzz_campaign *c, uam_fuzz_verdict v, const uam_fuzz_judgement *j)
{
    const uam_fuzz_result *r = uam_fuzz_last_result(c->f);
    if (c->stage == ST_I2S) {
        if (c->phase == 0) {
            size_t n = 0;
            uint64_t dropped = 0;
            const uam_cmp_entry *log = uam_fuzz_cmp_log(c->f, &n, &dropped);
            if (!log)
                n = 0;
            c->i2s.compares = (uint32_t)n;
            c->i2s.dropped = dropped;
            c->n_patches = uam_i2s_candidates(c->parent, c->parent_len, log, n, c->patches, c->limit);
            c->i2s.n_tokens = uam_i2s_tokens(c->parent, c->parent_len, log, n, c->tokens,
                                             UAM_FUZZ_I2S_TOKENS);
            c->i2s.candidates = (uint32_t)c->n_patches;
            c->b.left = (uint32_t)c->n_patches;
            c->phase = 1;
        }
        return UAM_OK;
    }
    if (c->phase == 0) {
        c->trim.ran = v == UAM_FUZZ_OK;
        c->trim.path = j->path;
        c->trim.insns = r ? r->insns : 0;
        c->phase = 1;
        if (!c->trim.ran || c->tlen < 5) {
            c->b.left = 0;
            return UAM_OK;
        }
        size_t lp2 = 1;
        while (lp2 < c->tlen)
            lp2 <<= 1;
        c->remove = lp2 / 16 > 4 ? lp2 / 16 : 4;
        c->floor = lp2 / 1024 > 4 ? lp2 / 1024 : 4;
        c->pos = c->remove;
    } else if (v == UAM_FUZZ_OK && j->path == c->trim.path && c->b.len < c->tlen) {
        memcpy(c->tdata, c->b.data, c->b.len);   /* the cut stays; the same place again */
        c->tlen = c->b.len;
        c->trim.insns = r ? r->insns : 0;
    } else {
        c->pos += c->remove;
    }
    trim_seek(c);
    return UAM_OK;
}

/* ---- batches --------------------------------------------------------------- */

uam_status uam_fuzz_campaign_batch_begin(uam_fuzz_campaign *c, uint32_t entry, uint32_t n,
                                         uint64_t seed, unsigned flags)
{
    c->b = (uam_fuzz_batch){ 0 };
    counts_clear(&c->paths);
    counts_clear(&c->edge_runs);
    c->flags = flags;
    c->stage = ST_HAVOC;
    c->parent = uam_mutator_corpus_get(c->mut, entry, &c->parent_len);
    if (!c->parent || !n)
        return UAM_ERR_INVALID;
    /* A probe: with no operator enabled there is nothing to run. */
    size_t len;
    uam_mutator_seed(c->mut, seed);
    if (!uam_mutator_mutate(c->mut, c->parent, c->parent_len, &len, NULL))
        return UAM_ERR_INVALID;
    uam_mutator_seed(c->mut, seed);
    c->b.left = n;
    return UAM_OK;
}

bool uam_fuzz_campaign_batch_next(uam_fuzz_campaign *c, const uint8_t **data, size_t *len)
{
    if (!c->b.left)
        return false;
    if (c->stage != ST_HAVOC)
        return stage_next(c, data, len);
    uint8_t *buf = uam_mutator_mutate(c->mut, c->parent, c->parent_len, &c->b.len, &c->b.ops);
    if (!buf) {
        c->b.left = 0;
        return false;
    }
    c->b.len = uam_fuzz_fixup(c->f, buf, c->b.len, uam_mutator_max_size(c->mut));
    c->b.data = buf;
    c->b.left--;
    *data = buf;
    *len = c->b.len;
    return true;
}

uam_status uam_fuzz_campaign_batch_judge(uam_fuzz_campaign *c, uam_fuzz_verdict v, uint64_t detail,
                                         uam_fuzz_judgement *j)
{
    const uint32_t *hits;
    size_t n = 0;
    uam_status st = judge(c, v, detail, j, &hits, &n);
    if (st != UAM_OK)
        return st;
    uam_fuzz_batch *b = &c->b;
    b->execs++;
    if ((unsigned)v < UAM_FUZZ_N_VERDICTS)
        b->by_verdict[v]++;
    if (v == UAM_FUZZ_OK) {
        if (!counts_add(&c->paths, j->path))
            return UAM_ERR_NOMEM;
        if (c->flags & UAM_FUZZ_BATCH_EDGES)
            for (size_t i = 0; i < n; i++)
                if (!counts_add(&c->edge_runs, hits[i]))
                    return UAM_ERR_NOMEM;
        j->report = j->new_classes != 0;
    } else if (is_crash(v)) {
        j->report = j->novel_crash;
    } else if (v == UAM_FUZZ_HANG || v == UAM_FUZZ_TIMEOUT) {
        j->report = c->hang_reports < c->hang_cap;
        c->hang_reports += j->report;
    }
    for (unsigned op = 0; op < UAM_MUT_OP_LIMIT; op++) {
        if (b->ops >> op & 1) {
            b->op_used[op]++;
            b->op_found[op] += v == UAM_FUZZ_OK && j->new_classes != 0;
        }
    }
    b->reports += j->report;
    if (c->stage != ST_HAVOC)
        return stage_judged(c, v, j);
    return UAM_OK;
}

const uam_fuzz_count *uam_fuzz_campaign_batch_paths(uam_fuzz_campaign *c, size_t *n)
{
    return counts_list(&c->paths, n);
}

const uam_fuzz_count *uam_fuzz_campaign_batch_edges(uam_fuzz_campaign *c, size_t *n)
{
    return counts_list(&c->edge_runs, n);
}
