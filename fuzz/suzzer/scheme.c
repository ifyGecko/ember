/*
 * scheme.c - suzzer's coverage bucket formula (FORMATS.md section 5), as COV operands.
 *
 * Reimplemented from the specification and checked against suzzer's conformance
 * vectors (its test/vectors; tests/unit/test_suzzer.c).
 */
#include "lcv_formats.h"
#include "suzzer.h"

/* The format versions fuzz/suzzer/ implements. A suzzer checkout that speaks other
 * versions stops the build here instead of being misread (docs/suzzer.md). */
_Static_assert(LIBCOV_VERSION == 3, "this suzzer's LCV3 version is not the one ember supports (3)");
_Static_assert(LIBCOV_BOUNDARY_VERSION == 1,
               "this suzzer's LCVB version is not the one ember supports (1)");
_Static_assert(LIBCOV_SHM_VERSION == 1, "this suzzer's LCVS version is not the one ember supports (1)");
_Static_assert(SZR_PROTO_VERSION == 1, "this suzzer's SZR1 version is not the one ember supports (1)");

uint64_t sz_mix(uint64_t v)
{
    v ^= v >> 33;
    v *= UINT64_C(0xff51afd7ed558ccd);
    v ^= v >> 33;
    return v;
}

uint32_t sz_spread(uint32_t site_id, uint32_t mask)
{
    return (uint32_t)sz_mix(site_id) & mask;
}

uint32_t sz_hash_pc(uint64_t offset, uint8_t module_idx, uint32_t mask)
{
    return (uint32_t)sz_mix(offset ^ ((uint64_t)module_idx * UINT64_C(0x9E3779B97F4A7C15))) & mask;
}

static uint64_t next_pow2(uint64_t v)
{
    uint64_t p = 1;
    while (p < v)
        p <<= 1;
    return p;
}

uint32_t sz_map_size_for(uint64_t n_sites, unsigned ngram)
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

void sz_site_ab(const sz_scheme *s, uint32_t site_id, uint64_t offset, uint8_t module_idx,
                uint32_t *a, uint32_t *b)
{
    const uint32_t mask = s->map_size - 1;
    if (s->model == SZ_MODEL_SITE) {
        *a = site_id;
        *b = s->ngram >= 2 ? sz_spread(site_id, mask) : 0;
    } else {
        *a = sz_hash_pc(offset, module_idx, mask);
        *b = s->ngram >= 2 ? *a >> 1 : 0;
    }
}

void sz_cov_history(const sz_scheme *s, uam_cov_history *h)
{
    *h = (uam_cov_history){ .depth = s->ngram >= 2 ? s->ngram - 1 : 1 };
    if (!s->rotate || s->ngram < 2)
        return;                              /* N=1 folds no history: nothing to rotate */
    unsigned bits = 0;
    while (bits < 32 && (UINT64_C(1) << bits) < s->map_size)
        bits++;
    /* libcov's sink_set_map_dims: WIN distinct amounts spread across [1, bits-1]. */
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

uint16_t sz_hello_scheme_flags(const sz_scheme *s)
{
    return (uint16_t)(SZR_HELLO_FLAG_SCHEME | (s->model == SZ_MODEL_PC ? SZR_HELLO_FLAG_MODEL_PC : 0)
                      | (s->rotate ? SZR_HELLO_FLAG_ROTATE : 0) | (s->ngram << SZR_HELLO_NGRAM_SHIFT));
}

uint32_t sz_lcvs_scheme_flags(const sz_scheme *s)
{
    return LIBCOV_SHM_FLAG_SCHEME | (s->model == SZ_MODEL_PC ? LIBCOV_SHM_FLAG_MODEL_PC : 0)
         | (s->rotate ? LIBCOV_SHM_FLAG_ROTATE : 0) | (s->ngram << LIBCOV_SHM_NGRAM_SHIFT);
}

const char *sz_scheme_name(const sz_scheme *s)
{
    static const char *const names[2][SZ_NGRAM_MAX][2] = {
        { { "site-n1", "site-n1-rot" }, { "site-n2", "site-n2-rot" }, { "site-n3", "site-n3-rot" },
          { "site-n4", "site-n4-rot" }, { "site-n5", "site-n5-rot" }, { "site-n6", "site-n6-rot" },
          { "site-n7", "site-n7-rot" }, { "site-n8", "site-n8-rot" } },
        { { "pc-n1", "pc-n1-rot" }, { "pc-n2", "pc-n2-rot" }, { "pc-n3", "pc-n3-rot" },
          { "pc-n4", "pc-n4-rot" }, { "pc-n5", "pc-n5-rot" }, { "pc-n6", "pc-n6-rot" },
          { "pc-n7", "pc-n7-rot" }, { "pc-n8", "pc-n8-rot" } },
    };
    unsigned n = s->ngram < 1 ? 1 : s->ngram > SZ_NGRAM_MAX ? SZ_NGRAM_MAX : s->ngram;
    return names[s->model == SZ_MODEL_PC][n - 1][s->rotate ? 1 : 0];
}
