/*
 * i2s.c - Input-to-state candidates and dictionary tokens from a compare log
 * (uam/fuzz_i2s.h).
 */
#include <string.h>

#include "uam/fuzz_i2s.h"
#include "uam/ir.h"

static unsigned bytelen(uint64_t v)
{
    unsigned n = 1;
    while (n < 8 && v >> (8 * n))
        n++;
    return n;
}

static bool fits(uint64_t v, unsigned w) { return w >= 8 || !(v >> (8 * w)); }

static void to_bytes(uint64_t v, unsigned w, bool big, uint8_t *out)
{
    for (unsigned i = 0; i < w; i++)
        out[big ? w - 1 - i : i] = (uint8_t)(v >> (8 * i));
}

/* The first position at or after `from` where pat[0..w) is in data, or len. */
static size_t find(const uint8_t *data, size_t len, const uint8_t *pat, unsigned w, size_t from)
{
    if (len < w)
        return len;
    for (size_t p = from; p + w <= len; p++) {
        const uint8_t *q = memchr(data + p, pat[0], len - w + 1 - p);
        if (!q)
            return len;
        p = (size_t)(q - data);
        if (!memcmp(q, pat, w))
            return p;
    }
    return len;
}

/* Add the patch that writes b[0..w) at pos, unless it changes nothing or is there already. */
static void add_patch(const uint8_t *data, size_t pos, const uint8_t *b, unsigned w,
                      uam_i2s_patch *out, size_t *n)
{
    unsigned lo = 0, hi = w;
    while (lo < hi && b[lo] == data[pos + lo])
        lo++;
    while (hi > lo && b[hi - 1] == data[pos + hi - 1])
        hi--;
    if (lo == hi)
        return;                               /* the input itself */
    uam_i2s_patch p = { .pos = (uint32_t)(pos + lo), .len = (uint8_t)(hi - lo) };
    memcpy(p.bytes, b + lo, hi - lo);
    for (size_t i = 0; i < *n; i++)
        if (out[i].pos == p.pos && out[i].len == p.len && !memcmp(out[i].bytes, p.bytes, p.len))
            return;
    out[(*n)++] = p;
}

size_t uam_i2s_candidates(const uint8_t *data, size_t len, const uam_cmp_entry *log, size_t n,
                          uam_i2s_patch *out, size_t max)
{
    size_t count = 0;
    for (size_t k = 0; k < n && count < max; k++) {
        const uam_cmp_entry *c = &log[k];
        if (c->width != 8 && c->width != 16 && c->width != 32 && c->width != 64)
            continue;
        const unsigned nb = c->width / 8;
        const uint64_t mask = c->width == 64 ? UINT64_MAX : (UINT64_C(1) << c->width) - 1;
        /* a constant of the code (b, with IMM) is never in the input as the compared value */
        const unsigned n_dirs = c->kind & UAM_CMP_IMM ? 1 : 2;
        for (unsigned d = 0; d < n_dirs; d++) {
            const uint64_t x = d ? c->b : c->a, y = d ? c->a : c->b;
            if (x == y)
                continue;
            uint64_t ys[3] = { y, (y + 1) & mask, (y - 1) & mask };
            const unsigned n_ys = c->kind & UAM_CMP_ORDER ? 3 : 1;
            /* the compare's width, and the narrowest of 1, 2, 4 that holds both values */
            unsigned widths[2] = { nb, 0 }, n_w = 1;
            const unsigned m = bytelen(x) > bytelen(y) ? bytelen(x) : bytelen(y);
            for (unsigned w = 1; w <= 4; w *= 2) {
                if (m <= w && w < nb) {
                    widths[n_w++] = w;
                    break;
                }
            }
            for (unsigned wi = 0; wi < n_w; wi++) {
                const unsigned w = widths[wi];
                if (!fits(x, w))
                    continue;
                for (unsigned big = 0; big < (w == 1 ? 1u : 2u); big++) {
                    uint8_t pat[8], zero[8] = { 0 };
                    to_bytes(x, w, big, pat);
                    if (!memcmp(pat, zero, w))
                        continue;             /* zeros are everywhere */
                    unsigned found = 0;
                    for (size_t pos = find(data, len, pat, w, 0);
                         pos < len && found < UAM_I2S_OCCURRENCES;
                         pos = find(data, len, pat, w, pos + 1)) {
                        found++;
                        for (unsigned yi = 0; yi < n_ys; yi++) {
                            if (!fits(ys[yi], w))
                                continue;
                            uint8_t b[8];
                            to_bytes(ys[yi], w, big, b);
                            add_patch(data, pos, b, w, out, &count);
                            if (count >= max)
                                return count;
                        }
                    }
                }
            }
        }
    }
    return count;
}

size_t uam_i2s_tokens(const uint8_t *data, size_t len, const uam_cmp_entry *log, size_t n,
                      uam_i2s_token *out, size_t max)
{
    size_t count = 0;
    for (size_t k = 0; k < n && count < max; k++) {
        const uam_cmp_entry *c = &log[k];
        if (!(c->kind & UAM_CMP_IMM) || !c->b || c->width % 8 || c->width > 64)
            continue;
        const unsigned sig = bytelen(c->b);
        unsigned w = c->width / 8;
        if (sig < 2)
            continue;
        for (unsigned x = 2; x <= 8; x *= 2) {   /* the narrowest of 2, 4, 8 that holds it */
            if (x >= sig && x <= w) {
                w = x;
                break;
            }
        }
        bool seen = false;
        for (size_t i = 0; i < count && !seen; i++)
            seen = out[i].value == c->b && out[i].width == w;
        if (seen)
            continue;
        uint8_t le[8], be[8];
        to_bytes(c->b, w, false, le);
        to_bytes(c->b, w, true, be);
        if (find(data, len, le, w, 0) < len || find(data, len, be, w, 0) < len)
            continue;
        out[count++] = (uam_i2s_token){ .value = c->b, .width = (uint8_t)w };
    }
    return count;
}
