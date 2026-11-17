/*
 * test_mutate.c - ember's input mutator (uam/fuzz_mutate.h).
 *
 * Determinism, the size cap, empty inputs, operator weights and the operator set,
 * dictionary tokens (bytes and integers in either byte order, and their off-by-one
 * neighbours), splice, the corpus, and growth: from a seed of one byte and a max_size of
 * thousands, inputs must reach every size class up to max_size, not hover near the seed.
 *
 * Input-to-state (uam/fuzz_i2s.h): the candidates and dictionary tokens derived from a
 * compare log.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uam/fuzz_i2s.h"
#include "uam/fuzz_mutate.h"
#include "uam/ir.h"

static int n_checks, n_failed;

#define CHECK(cond, ...) do {                                              \
        n_checks++;                                                        \
        if (!(cond)) {                                                     \
            n_failed++;                                                    \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);           \
            fprintf(stderr, __VA_ARGS__);                                  \
            fputc('\n', stderr);                                           \
        }                                                                  \
    } while (0)

static bool contains(const uint8_t *h, size_t n, const void *needle, size_t k)
{
    for (size_t i = 0; i + k <= n; i++)
        if (!memcmp(h + i, needle, k))
            return true;
    return false;
}

/* Every weight 0, then op alone at weight 1. */
static void only(uam_mutator *m, unsigned op)
{
    for (unsigned o = 0; o < UAM_MUT_OP_LIMIT; o++)
        if (uam_mutator_is_op(o))
            uam_mutator_set_weight(m, o, 0);
    uam_mutator_set_weight(m, op, 1);
}

static void test_ops_table(void)
{
    CHECK(!uam_mutator_is_op(0) && !uam_mutator_is_op(20) && !uam_mutator_is_op(21)
          && !uam_mutator_is_op(UAM_MUT_SPLICE) && !uam_mutator_is_op(UAM_MUT_OP_LIMIT),
          "only the havoc operators are operators");
    CHECK(uam_mutator_is_op(UAM_MUT_INSERT_BLOCK) && uam_mutator_is_op(UAM_MUT_RESIZE)
          && !strcmp(uam_mutator_op_name(UAM_MUT_DICT_INT_ADJ), "DICT_INT_ADJ")
          && uam_mutator_default_weight(UAM_MUT_BIT_FLIP) > 0, "operator table");
    uam_mutator *m = uam_mutator_create();
    CHECK(uam_mutator_set_weight(m, 20, 1) == UAM_ERR_INVALID
          && uam_mutator_set_weight(m, UAM_MUT_SPLICE, 1) == UAM_ERR_INVALID
          && uam_mutator_set_stack(m, 0, 4) == UAM_ERR_INVALID
          && uam_mutator_set_stack(m, 8, 4) == UAM_ERR_INVALID
          && uam_mutator_set_max_size(m, 0) == UAM_ERR_INVALID
          && uam_mutator_add_int_token(m, 1, 3, false) == UAM_ERR_INVALID
          && uam_mutator_add_token(m, (const uint8_t *)"x", 0) == UAM_ERR_INVALID,
          "bad settings are refused");
    uam_mutator_destroy(m);
}

static void test_determinism(void)
{
    uam_mutator *a = uam_mutator_create(), *b = uam_mutator_create();
    const uint8_t seed[] = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
    uam_mutator_corpus_put(a, 1, (const uint8_t *)"donor bytes here", 16);
    uam_mutator_corpus_put(b, 1, (const uint8_t *)"donor bytes here", 16);
    uam_mutator_add_token(a, (const uint8_t *)"GET ", 4);
    uam_mutator_add_token(b, (const uint8_t *)"GET ", 4);
    uam_mutator_seed(a, 42);
    uam_mutator_seed(b, 42);
    bool same = true, differs = false;
    for (int i = 0; i < 200; i++) {
        size_t la, lb;
        uint64_t oa, ob;
        const uint8_t *pa = uam_mutator_mutate(a, seed, 64, &la, &oa);
        uint8_t keep[UAM_MUT_DEFAULT_MAX_SIZE];
        memcpy(keep, pa, la);
        const uint8_t *pb = uam_mutator_mutate(b, seed, 64, &lb, &ob);
        same &= la == lb && oa == ob && !memcmp(keep, pb, la);
        differs |= la != 64 || memcmp(keep, seed, 64);
    }
    CHECK(same, "the same seed gives the same mutations");
    CHECK(differs, "mutations change the input");
    uam_mutator_seed(b, 43);
    size_t la, lb;
    uint8_t keep[UAM_MUT_DEFAULT_MAX_SIZE];
    uam_mutator_seed(a, 42);
    const uint8_t *pa = uam_mutator_mutate(a, seed, 64, &la, NULL);
    memcpy(keep, pa, la);
    const uint8_t *pb = uam_mutator_mutate(b, seed, 64, &lb, NULL);
    CHECK(la != lb || memcmp(keep, pb, la), "another seed, other mutations");
    uam_mutator_destroy(a);
    uam_mutator_destroy(b);
}

static void test_cap_and_empty(void)
{
    uam_mutator *m = uam_mutator_create();
    uint8_t src[200];
    memset(src, 'x', sizeof(src));
    uam_mutator_set_max_size(m, 128);
    bool capped = true;
    for (int i = 0; i < 2000; i++) {
        size_t n;
        uam_mutator_mutate(m, src, sizeof(src), &n, NULL);
        capped &= n <= 128;
    }
    CHECK(capped, "max_size caps every mutation (and a longer parent)");
    uam_mutator_set_max_size(m, 1);
    capped = true;
    for (int i = 0; i < 500; i++) {
        size_t n;
        uam_mutator_mutate(m, src, 1, &n, NULL);
        capped &= n <= 1;
    }
    CHECK(capped, "max_size 1");
    uam_mutator_set_max_size(m, 4096);
    unsigned grew = 0;
    for (int i = 0; i < 500; i++) {
        size_t n;
        CHECK(uam_mutator_mutate(m, NULL, 0, &n, NULL) != NULL, "an empty parent");
        grew += n > 0;
    }
    CHECK(grew > 450, "an empty parent grows (%u of 500)", grew);
    uam_mutator_destroy(m);
}

static void test_weights(void)
{
    uam_mutator *m = uam_mutator_create();
    uint8_t src[32] = { 0 };
    only(m, UAM_MUT_BIT_FLIP);
    bool ok = true;
    for (int i = 0; i < 200; i++) {
        size_t n;
        uint64_t ops;
        uam_mutator_mutate(m, src, sizeof(src), &n, &ops);
        ok &= n == sizeof(src) && ops == (1ull << UAM_MUT_BIT_FLIP);
    }
    CHECK(ok, "one enabled operator is the only one applied");
    only(m, UAM_MUT_DICT_INSERT);
    size_t n;
    CHECK(uam_mutator_mutate(m, src, sizeof(src), &n, NULL) == NULL,
          "dictionary operators without tokens: nothing enabled");
    uam_mutator_set_weight(m, UAM_MUT_DICT_INSERT, 0);
    CHECK(uam_mutator_mutate(m, src, sizeof(src), &n, NULL) == NULL, "every weight 0");
    uam_mutator_reset_weights(m);
    CHECK(uam_mutator_weight(m, UAM_MUT_BIT_FLIP) == uam_mutator_default_weight(UAM_MUT_BIT_FLIP),
          "reset_weights");
    /* The arithmetic and interesting-value operators keep the length. */
    static const unsigned fixed[] = {
        UAM_MUT_BYTE_FLIP, UAM_MUT_BYTE_ARITH, UAM_MUT_WORD_ARITH_LE, UAM_MUT_WORD_ARITH_BE,
        UAM_MUT_DWORD_ARITH_LE, UAM_MUT_DWORD_ARITH_BE, UAM_MUT_SET_INTERESTING_U8,
        UAM_MUT_SET_INTERESTING_U16_LE, UAM_MUT_SET_INTERESTING_U16_BE,
        UAM_MUT_SET_INTERESTING_U32_LE, UAM_MUT_SET_INTERESTING_U32_BE, UAM_MUT_RANDOM_BYTE,
        UAM_MUT_OVERWRITE_CHUNK, UAM_MUT_SWAP_CHUNKS,
    };
    for (size_t k = 0; k < sizeof(fixed) / sizeof(fixed[0]); k++) {
        only(m, fixed[k]);
        ok = true;
        for (int i = 0; i < 100; i++) {
            uam_mutator_mutate(m, src, sizeof(src), &n, NULL);
            ok &= n == sizeof(src);
        }
        CHECK(ok, "%s keeps the length", uam_mutator_op_name(fixed[k]));
    }
    /* Deletes shrink, inserts grow. */
    only(m, UAM_MUT_DELETE_CHUNK);
    uam_mutator_mutate(m, src, sizeof(src), &n, NULL);
    CHECK(n < sizeof(src) && n >= 1, "DELETE_CHUNK shrinks and leaves a byte (%zu)", n);
    static const unsigned grow[] = { UAM_MUT_CLONE_CHUNK, UAM_MUT_INSERT_BLOCK };
    for (size_t k = 0; k < 2; k++) {
        only(m, grow[k]);
        uam_mutator_mutate(m, src, sizeof(src), &n, NULL);
        CHECK(n > sizeof(src), "%s grows (%zu)", uam_mutator_op_name(grow[k]), n);
    }
    uam_mutator_destroy(m);
}

static void test_dictionary(void)
{
    uam_mutator *m = uam_mutator_create();
    uint8_t src[24];
    memset(src, '.', sizeof(src));
    uam_mutator_add_token(m, (const uint8_t *)"MAGIC", 5);
    only(m, UAM_MUT_DICT_INSERT);
    uam_mutator_set_stack(m, 1, 1);
    size_t n;
    const uint8_t *p = uam_mutator_mutate(m, src, sizeof(src), &n, NULL);
    CHECK(n == sizeof(src) + 5 && contains(p, n, "MAGIC", 5), "DICT_INSERT inserts a token");
    only(m, UAM_MUT_DICT_OVERWRITE);
    p = uam_mutator_mutate(m, src, sizeof(src), &n, NULL);
    CHECK(n == sizeof(src) && contains(p, n, "MAGIC", 5), "DICT_OVERWRITE writes over");

    /* Integer tokens: the token itself in its order, and DICT_INT_ADJ's neighbours. */
    uam_mutator_clear_tokens(m);
    CHECK(uam_mutator_n_tokens(m) == 0, "clear_tokens");
    uam_mutator_add_int_token(m, 0x100, 4, true);
    uam_mutator_add_int_token(m, 0x1234, 2, false);
    only(m, UAM_MUT_DICT_INSERT);
    bool be = false, le = false;
    for (int i = 0; i < 100; i++) {
        p = uam_mutator_mutate(m, src, sizeof(src), &n, NULL);
        be |= contains(p, n, "\x00\x00\x01\x00", 4);
        le |= contains(p, n, "\x34\x12", 2);
    }
    CHECK(be && le, "integer tokens in their own byte order (be %d le %d)", be, le);
    only(m, UAM_MUT_DICT_INT_ADJ);
    bool up = false, down = false;
    for (int i = 0; i < 200; i++) {
        p = uam_mutator_mutate(m, src, sizeof(src), &n, NULL);
        up |= contains(p, n, "\x00\x00\x01\x01", 4);
        down |= contains(p, n, "\x00\x00\x00\xff", 4);
    }
    CHECK(up && down, "DICT_INT_ADJ writes value + 1 and value - 1 (%d %d)", up, down);
    uam_mutator_clear_tokens(m);
    uam_mutator_add_int_token(m, 0, 1, false);
    only(m, UAM_MUT_DICT_INT_ADJ);
    down = false;
    for (int i = 0; i < 100; i++) {
        p = uam_mutator_mutate(m, src, sizeof(src), &n, NULL);
        down |= contains(p, n, "\xff", 1);
    }
    CHECK(down, "DICT_INT_ADJ wraps within the token's width");
    uam_mutator_destroy(m);
}

static void test_splice_and_corpus(void)
{
    uam_mutator *m = uam_mutator_create();
    const uint8_t a[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", d[] = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    CHECK(uam_mutator_corpus_put(m, 7, (const uint8_t *)"old", 3) == UAM_OK
          && uam_mutator_corpus_put(m, 7, d, 40) == UAM_OK && uam_mutator_corpus_size(m) == 1,
          "corpus_put replaces by id");
    size_t n;
    const uint8_t *g = uam_mutator_corpus_get(m, 7, &n);
    CHECK(g && n == 40 && !memcmp(g, d, 40) && !uam_mutator_corpus_get(m, 8, &n), "corpus_get");
    uam_mutator_set_splice(m, 1000);
    only(m, UAM_MUT_BIT_FLIP);
    uam_mutator_set_stack(m, 1, 1);
    bool spliced = true;
    for (int i = 0; i < 100; i++) {
        uint64_t ops;
        const uint8_t *p = uam_mutator_mutate(m, a, 32, &n, &ops);
        /* a's head, the donor's tail (one bit flipped somewhere) */
        size_t as = 0, bs = 0;
        for (size_t k = 0; k < n; k++)
            as += p[k] == 'a', bs += p[k] == 'b';
        spliced &= (ops & (1ull << UAM_MUT_SPLICE)) && n == 40 && as >= 1 && bs >= 8
                   && as + bs >= n - 1;
    }
    CHECK(spliced, "splice: a head of the parent, the donor's tail");
    /* Identical parent and donor: no crossover point, no splice. */
    uint64_t ops;
    uam_mutator_mutate(m, d, 40, &n, &ops);
    CHECK(!(ops & (1ull << UAM_MUT_SPLICE)), "no splice with an identical donor");
    uam_mutator_destroy(m);
}

/* Inputs must grow from a tiny seed toward max_size. A lineage that keeps every child
 * (what a queue does when each child finds something) and single mutations of a small
 * seed both have to reach the size classes a fuzzer with a large max_size should try. */
static void test_growth(void)
{
    enum { MAX = 9000, GENS = 3000 };
    uam_mutator *m = uam_mutator_create();
    uam_mutator_set_max_size(m, MAX);
    uam_mutator_seed(m, 1);
    static uint8_t cur[MAX];
    size_t len = 1;
    cur[0] = 'A';
    size_t peak = 0, over1k = 0, over4k = 0;
    double sum = 0;
    for (int g = 0; g < GENS; g++) {
        size_t n;
        const uint8_t *p = uam_mutator_mutate(m, cur, len, &n, NULL);
        memcpy(cur, p, n);
        len = n;
        sum += (double)n;
        peak = n > peak ? n : peak;
        over1k += n > 1000;
        over4k += n > 4000;
    }
    printf("test_mutate: lineage from 1 byte, max %u: mean %.0f, peak %zu, %.0f%% > 1000, "
           "%.0f%% > 4000\n", MAX, sum / GENS, peak, 100.0 * (double)over1k / GENS,
           100.0 * (double)over4k / GENS);
    CHECK(peak > 7000, "the lineage reaches near max_size (peak %zu)", peak);
    CHECK(over1k > GENS / 5, "the lineage spends time above 1000 bytes (%zu of %d)", over1k, GENS);

    /* Single mutations of a 16-byte seed: most stay small (cheap to run), some grow far. */
    uint8_t seed[16];
    memset(seed, 'A', sizeof(seed));
    size_t bigger = 0, over256 = 0, over2k = 0;
    enum { N = 20000 };
    for (int i = 0; i < N; i++) {
        size_t n;
        uam_mutator_mutate(m, seed, sizeof(seed), &n, NULL);
        bigger += n > sizeof(seed);
        over256 += n > 256;
        over2k += n > 2000;
    }
    printf("test_mutate: one mutation of 16 bytes: %.0f%% larger, %.1f%% > 256, %.1f%% > 2000\n",
           100.0 * (double)bigger / N, 100.0 * (double)over256 / N, 100.0 * (double)over2k / N);
    CHECK(bigger > N / 3, "a third of the mutations grow the seed (%zu of %d)", bigger, N);
    CHECK(over2k > N / 50 && over256 < N * 3 / 4,
          "some mutations reach thousands of bytes, most stay small (%zu, %zu)", over2k, over256);
    uam_mutator_destroy(m);
}

/* ---- input-to-state --------------------------------------------------------- */

#define I2S_MAX 256

static uam_i2s_patch i2s_out[I2S_MAX];

/* The candidates of data for one record, at most max. */
static size_t i2s(const void *data, size_t len, unsigned width, unsigned kind, uint64_t a,
                  uint64_t b, size_t max)
{
    const uam_cmp_entry e = { .site = 1, .width = (uint8_t)width, .kind = (uint8_t)kind, .a = a, .b = b };
    return uam_i2s_candidates(data, len, &e, 1, i2s_out, max);
}

/* Patch k applied to data gives want[0..len). */
static bool gives(const void *data, size_t len, size_t k, const void *want)
{
    uint8_t buf[256];
    memcpy(buf, data, len);
    memcpy(buf + i2s_out[k].pos, i2s_out[k].bytes, i2s_out[k].len);
    return !memcmp(buf, want, len);
}

static bool any_gives(const void *data, size_t len, size_t n, const void *want)
{
    for (size_t k = 0; k < n; k++)
        if (gives(data, len, k, want))
            return true;
    return false;
}

static void le32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static void test_i2s(void)
{
    const uint32_t magic = 0x6A3C91E5u, near = magic ^ 0x01000000u;
    uint8_t data[12], want[12];
    memcpy(data, "Mabc", 4);
    le32(data + 4, near);
    memcpy(data + 8, "tail", 4);
    memcpy(want, data, 12);
    le32(want + 4, magic);

    /* a 32-bit equality of the input's little-endian word with a register holding magic */
    size_t n = i2s(data, 12, 32, UAM_CMP_EQ, near, magic, I2S_MAX);
    CHECK(any_gives(data, 12, n, want), "the LE word replaced by magic (%zu candidates)", n);
    bool minimal = n > 0;
    for (size_t k = 0; k < n; k++)
        minimal = minimal && i2s_out[k].len >= 1
                  && i2s_out[k].bytes[0] != data[i2s_out[k].pos]
                  && i2s_out[k].bytes[i2s_out[k].len - 1] != data[i2s_out[k].pos + i2s_out[k].len - 1];
    CHECK(minimal, "a patch differs from the input in its first and last byte");
    /* the same compare on a big-endian core that loaded the word as is: swapped values */
    n = i2s(data, 12, 32, UAM_CMP_EQ | UAM_CMP_IMM, __builtin_bswap32(near), __builtin_bswap32(magic),
            I2S_MAX);
    CHECK(any_gives(data, 12, n, want), "the big-endian view");
    /* IMM: only a is searched, b written ('tail' as a big-endian word) */
    n = i2s(data, 12, 32, UAM_CMP_EQ | UAM_CMP_IMM, 0x7461696Cu, 0x41414141u, I2S_MAX);
    uint8_t w2[12];
    memcpy(w2, data, 12);
    memcpy(w2 + 8, "AAAA", 4);
    CHECK(n == 1 && gives(data, 12, 0, w2), "IMM: only a is searched, b written (%zu)", n);
    n = i2s(data, 12, 32, UAM_CMP_EQ | UAM_CMP_IMM, 0x41414141u, 0x7461696Cu, I2S_MAX);
    CHECK(n == 0, "IMM: the constant is not searched for");
    /* without IMM both operands are */
    n = i2s(data, 12, 32, UAM_CMP_EQ, 0x41414141u, 0x7461696Cu, I2S_MAX);
    CHECK(n == 1 && gives(data, 12, 0, w2), "no IMM: either operand may be the input's");
    /* a byte compared in a 32-bit register is one byte of input */
    n = i2s("xax", 3, 32, UAM_CMP_EQ | UAM_CMP_IMM, 'a', 'Z', I2S_MAX);
    CHECK(n == 1 && gives("xax", 3, 0, "xZx") && i2s_out[0].len == 1, "a byte in a wide compare");
    /* ordered: y, y + 1, y - 1 */
    const uint8_t h[2] = { 0x34, 0x12 };
    n = i2s(h, 2, 16, UAM_CMP_ORDER | UAM_CMP_IMM, 0x1234, 0x2000, I2S_MAX);
    CHECK(n == 3 && any_gives(h, 2, n, "\x00\x20") && any_gives(h, 2, n, "\x01\x20")
          && any_gives(h, 2, n, "\xff\x1f"), "ORDER: y and y +- 1 (%zu)", n);
    /* zero patterns are not searched; equal operands give nothing */
    CHECK(i2s("\0\0\0\0", 4, 32, UAM_CMP_EQ, 0, 5, I2S_MAX) == 0, "zeros are not searched");
    CHECK(i2s(data, 12, 32, UAM_CMP_EQ, 9, 9, I2S_MAX) == 0, "equal operands: nothing");
    /* every occurrence, up to UAM_I2S_OCCURRENCES; the limit */
    char many[60];
    for (int k = 0; k < 20; k++)
        memcpy(many + 3 * k, "ab-", 3);
    n = i2s(many, 60, 16, UAM_CMP_EQ | UAM_CMP_IMM, 0x6261, 0x7a7a, I2S_MAX);
    CHECK(n == UAM_I2S_OCCURRENCES, "at most %u positions (%zu)", UAM_I2S_OCCURRENCES, n);
    CHECK(i2s(many, 60, 16, UAM_CMP_EQ | UAM_CMP_IMM, 0x6261, 0x7a7a, 3) == 3, "the limit");
    /* distinct: two records that give the same bytes give one candidate */
    const uam_cmp_entry two[2] = {
        { .site = 1, .width = 32, .kind = UAM_CMP_EQ | UAM_CMP_IMM, .a = 'a', .b = 'Z' },
        { .site = 2, .width = 8, .kind = UAM_CMP_EQ | UAM_CMP_IMM, .a = 'a', .b = 'Z' },
    };
    CHECK(uam_i2s_candidates((const uint8_t *)"xax", 3, two, 2, i2s_out, I2S_MAX) == 1, "distinct");
    /* a 64-bit compare, and a width that is none */
    uint8_t q[8] = { 1, 2, 3, 4, 5, 6, 7, 8 }, qw[8] = { 8, 7, 6, 5, 4, 3, 2, 1 };
    n = i2s(q, 8, 64, UAM_CMP_EQ, UINT64_C(0x0807060504030201), UINT64_C(0x0102030405060708), I2S_MAX);
    CHECK(any_gives(q, 8, n, qw), "a 64-bit operand");
    CHECK(i2s(q, 8, 12, UAM_CMP_EQ, 1, 2, I2S_MAX) == 0, "a record with no such width is skipped");
    CHECK(i2s(q, 0, 32, UAM_CMP_EQ, 1, 2, I2S_MAX) == 0 && i2s(q, 1, 32, UAM_CMP_EQ, 0x0201, 5, I2S_MAX) == 0,
          "inputs shorter than the pattern");

    /* tokens: the constants the input lacks, two or more significant bytes */
    const uam_cmp_entry recs[] = {
        { .width = 32, .kind = UAM_CMP_EQ | UAM_CMP_IMM, .a = 1, .b = magic },
        { .width = 32, .kind = UAM_CMP_EQ | UAM_CMP_IMM, .a = 1, .b = 0x7461 },      /* "at" is in it */
        { .width = 32, .kind = UAM_CMP_EQ | UAM_CMP_IMM, .a = 1, .b = 0x41 },        /* one byte */
        { .width = 32, .kind = UAM_CMP_EQ, .a = 1, .b = 0xBEEF },                    /* no constant */
        { .width = 32, .kind = UAM_CMP_EQ | UAM_CMP_IMM, .a = 2, .b = magic },       /* again */
        { .width = 32, .kind = UAM_CMP_EQ | UAM_CMP_IMM, .a = 1, .b = 0xBEEF },
        { .width = 64, .kind = UAM_CMP_EQ | UAM_CMP_IMM, .a = 1, .b = UINT64_C(0x1122334455) },
    };
    uam_i2s_token tok[8];
    n = uam_i2s_tokens(data, 12, recs, 7, tok, 8);
    CHECK(n == 3 && tok[0].value == magic && tok[0].width == 4 && tok[1].value == 0xBEEF
          && tok[1].width == 2 && tok[2].value == UINT64_C(0x1122334455) && tok[2].width == 8,
          "tokens: the missing multi-byte constants, each once, at their narrowest (%zu)", n);
    CHECK(uam_i2s_tokens(data, 12, recs, 7, tok, 1) == 1, "the token limit");
}

int main(void)
{
    test_ops_table();
    test_determinism();
    test_cap_and_empty();
    test_weights();
    test_dictionary();
    test_splice_and_corpus();
    test_growth();
    test_i2s();
    printf("test_mutate: %d/%d checks passed\n", n_checks - n_failed, n_checks);
    return n_failed ? 1 : 0;
}
