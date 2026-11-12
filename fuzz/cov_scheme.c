/*
 * cov_scheme.c - The coverage bucket formula (docs/fuzz-formats.md "Coverage buckets"),
 * as COV operands. tests/unit/test_cov_formats.c pins it with golden maps.
 */
#include "formats.h"
#include "cov_formats.h"

uint64_t cf_mix(uint64_t v)
{
    v ^= v >> 33;
    v *= UINT64_C(0xff51afd7ed558ccd);
    v ^= v >> 33;
    return v;
}

uint32_t cf_spread(uint32_t site_id, uint32_t mask)
{
    return (uint32_t)cf_mix(site_id) & mask;
}

uint32_t cf_hash_pc(uint64_t offset, uint8_t module_idx, uint32_t mask)
{
    return (uint32_t)cf_mix(offset ^ ((uint64_t)module_idx * UINT64_C(0x9E3779B97F4A7C15))) & mask;
}

static uint64_t next_pow2(uint64_t v)
{
    uint64_t p = 1;
    while (p < v)
        p <<= 1;
    return p;
}

uint32_t cf_map_size_for(uint64_t n_sites, unsigned ngram)
{
    const uint64_t fudge = ngram <= 1 ? 2 : UINT64_C(16) << (ngram - 2);
    const uint64_t min = UINT64_C(1) << 12, max = UINT64_C(1) << 24;
    uint64_t n = n_sites ? n_sites : 1;
    uint64_t size = next_pow2(n * fudge);
    if (size > max)
        size = max;
    if (size < min)
        size = min;
    if (size < next_pow2(n))
        size = next_pow2(n);   /* every site id fits, even above the cap */
    return size > (UINT64_C(1) << 31) ? UINT32_C(1) << 31 : (uint32_t)size;
}

void cf_site_ab(const cf_scheme *s, uint32_t site_id, uint64_t offset, uint8_t module_idx,
                uint32_t *a, uint32_t *b)
{
    const uint32_t mask = s->map_size - 1;
    if (s->model == CF_MODEL_SITE) {
        *a = site_id;
        *b = s->ngram >= 2 ? cf_spread(site_id, mask) : 0;
    } else {
        *a = cf_hash_pc(offset, module_idx, mask);
        *b = s->ngram >= 2 ? *a >> 1 : 0;
    }
}

void cf_cov_history(const cf_scheme *s, uam_cov_history *h)
{
    *h = (uam_cov_history){ .depth = s->ngram >= 2 ? s->ngram - 1 : 1 };
    if (!s->rotate || s->ngram < 2)
        return;                              /* N=1 folds no history: nothing to rotate */
    unsigned bits = 0;
    while (bits < 32 && (UINT64_C(1) << bits) < s->map_size)
        bits++;
    /* depth distinct amounts spread across [1, bits-1] */
    unsigned step = (bits - 1) / h->depth;
    if (step == 0)
        step = 1;
    for (unsigned j = 0; j < h->depth; j++) {
        unsigned r = 1 + j * step;
        if (r >= bits)
            r = 1 + (r % (bits - 1));
        h->rot[j] = (uint8_t)r;
    }
}

uint32_t cf_scheme_flags(const cf_scheme *s)
{
    return UAM_COV_SCHEME | (s->model == CF_MODEL_PC ? UAM_COV_SCHEME_PC : 0)
         | (s->rotate ? UAM_COV_SCHEME_ROTATE : 0) | (s->ngram << UAM_COV_NGRAM_SHIFT);
}

const char *cf_scheme_name(const cf_scheme *s)
{
    static const char *const names[2][CF_NGRAM_MAX][2] = {
        { { "site-n1", "site-n1-rot" }, { "site-n2", "site-n2-rot" }, { "site-n3", "site-n3-rot" },
          { "site-n4", "site-n4-rot" }, { "site-n5", "site-n5-rot" }, { "site-n6", "site-n6-rot" },
          { "site-n7", "site-n7-rot" }, { "site-n8", "site-n8-rot" } },
        { { "pc-n1", "pc-n1-rot" }, { "pc-n2", "pc-n2-rot" }, { "pc-n3", "pc-n3-rot" },
          { "pc-n4", "pc-n4-rot" }, { "pc-n5", "pc-n5-rot" }, { "pc-n6", "pc-n6-rot" },
          { "pc-n7", "pc-n7-rot" }, { "pc-n8", "pc-n8-rot" } },
    };
    unsigned n = s->ngram < 1 ? 1 : s->ngram > CF_NGRAM_MAX ? CF_NGRAM_MAX : s->ngram;
    return names[s->model == CF_MODEL_PC][n - 1][s->rotate ? 1 : 0];
}
