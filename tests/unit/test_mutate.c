/*
 * test_mutate.c - ember's input mutator (uam/fuzz_mutate.h).
 *
 * Determinism, the size cap, empty inputs, operator weights and the operator set,
 * dictionary tokens (bytes and integers in either byte order, and their off-by-one
 * neighbours), splice, the corpus, and growth: from a seed of one byte and a max_size of
 * thousands, inputs must reach every size class up to max_size, not hover near the seed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uam/fuzz_mutate.h"

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

int main(void)
{
    test_ops_table();
    test_determinism();
    test_cap_and_empty();
    test_weights();
    test_dictionary();
    test_splice_and_corpus();
    test_growth();
    printf("test_mutate: %d/%d checks passed\n", n_checks - n_failed, n_checks);
    return n_failed ? 1 : 0;
}
