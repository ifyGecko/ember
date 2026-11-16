/*
 * mutate.c - ember's input mutator (uam/fuzz_mutate.h): havoc, splice, dictionaries.
 */
#include <stdlib.h>
#include <string.h>

#include "uam/fuzz_mutate.h"

#define ARITH_MAX   35
#define BLK_SMALL   32
#define BLK_MEDIUM  128
#define BLK_LARGE   1500
#define BLK_XL      32768

static const int8_t  interesting_8[]  = { -128, -1, 0, 1, 16, 32, 64, 100, 127 };
static const uint8_t interesting_u8[] = { 0, 1, 16, 32, 64, 100, 127, 128, 255 };
static const uint16_t interesting_16[] = { 0, 1, 128, 255, 256, 512, 1000, 1024, 4096, 32767,
                                           32768, 65535 };
static const uint32_t interesting_32[] = { 0, 1, 32768, 65535, 65536, 100663045, 2147483647,
                                           2147483648u, 4294967295u };

static const struct { const char *name; unsigned weight; } op_info[UAM_MUT_OP_LIMIT] = {
    [UAM_MUT_BIT_FLIP]               = { "BIT_FLIP", 32 },
    [UAM_MUT_BYTE_FLIP]              = { "BYTE_FLIP", 24 },
    [UAM_MUT_BYTE_ARITH]             = { "BYTE_ARITH", 24 },
    [UAM_MUT_WORD_ARITH_LE]          = { "WORD_ARITH_LE", 24 },
    [UAM_MUT_WORD_ARITH_BE]          = { "WORD_ARITH_BE", 16 },
    [UAM_MUT_DWORD_ARITH_LE]         = { "DWORD_ARITH_LE", 16 },
    [UAM_MUT_DWORD_ARITH_BE]         = { "DWORD_ARITH_BE", 12 },
    [UAM_MUT_SET_INTERESTING_U8]     = { "SET_INTERESTING_U8", 32 },
    [UAM_MUT_SET_INTERESTING_U16_LE] = { "SET_INTERESTING_U16_LE", 16 },
    [UAM_MUT_SET_INTERESTING_U16_BE] = { "SET_INTERESTING_U16_BE", 12 },
    [UAM_MUT_SET_INTERESTING_U32_LE] = { "SET_INTERESTING_U32_LE", 12 },
    [UAM_MUT_SET_INTERESTING_U32_BE] = { "SET_INTERESTING_U32_BE", 8 },
    [UAM_MUT_RANDOM_BYTE]            = { "RANDOM_BYTE", 24 },
    [UAM_MUT_DELETE_CHUNK]           = { "DELETE_CHUNK", 24 },
    [UAM_MUT_CLONE_CHUNK]            = { "CLONE_CHUNK", 16 },
    [UAM_MUT_OVERWRITE_CHUNK]        = { "OVERWRITE_CHUNK", 20 },
    [UAM_MUT_SWAP_CHUNKS]            = { "SWAP_CHUNKS", 8 },
    [UAM_MUT_DICT_INSERT]            = { "DICT_INSERT", 32 },
    [UAM_MUT_DICT_OVERWRITE]         = { "DICT_OVERWRITE", 24 },
    [UAM_MUT_DICT_INT_ADJ]           = { "DICT_INT_ADJ", 16 },
    [UAM_MUT_INSERT_BLOCK]           = { "INSERT_BLOCK", 4 },
    [UAM_MUT_RESIZE]                 = { "RESIZE", 1 },
    [UAM_MUT_SPLICE]                 = { "SPLICE", 0 },
};

typedef struct token {
    uint8_t  data[8];      /* an integer token's encoding (byte tokens: bytes below) */
    uint8_t *bytes;        /* the token's bytes (data for an integer) */
    size_t   len;
    bool     is_int, big_endian;
    uint64_t value;
} token;

typedef struct entry {
    uint32_t id;
    uint8_t *data;
    size_t   len;
} entry;

struct uam_mutator {
    uint64_t  s[4];                       /* xoshiro256** */
    size_t    max_size;
    unsigned  weight[UAM_MUT_OP_LIMIT];
    unsigned  splice_permille;
    unsigned  stack[16];                  /* the stack depths to draw from */
    unsigned  n_stack;
    /* the enabled operators: cumulative weights, rebuilt when a backing changes */
    uint8_t   pick_op[UAM_MUT_OP_LIMIT];
    uint32_t  pick_cum[UAM_MUT_OP_LIMIT];
    unsigned  n_pick;
    uint32_t  pick_total;
    bool      dirty;
    token    *tokens;
    size_t    n_tokens, cap_tokens, n_int_tokens;
    size_t   *int_tokens;                 /* indices of the integer tokens */
    entry    *corpus;
    size_t    n_corpus, cap_corpus;
    uint8_t  *buf;                        /* max_size bytes: the mutation */
    uint8_t  *tmp;                        /* max_size bytes: chunk copies */
};

/* ---- randomness ------------------------------------------------------------ */

static uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

static uint64_t next64(uam_mutator *m)
{
    uint64_t *s = m->s;
    const uint64_t r = rotl(s[1] * 5, 7) * 9, t = s[1] << 17;
    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rotl(s[3], 45);
    return r;
}

/* Uniform in [0, n), n >= 1 (Lemire's multiply; the bias is below 2^-32 for our n). */
static size_t below(uam_mutator *m, size_t n)
{
    return (size_t)(((unsigned __int128)next64(m) * n) >> 64);
}

/* Uniform in [lo, hi]. */
static size_t range(uam_mutator *m, size_t lo, size_t hi) { return lo + below(m, hi - lo + 1); }

void uam_mutator_seed(uam_mutator *m, uint64_t seed)
{
    for (int i = 0; i < 4; i++) {   /* splitmix64 */
        uint64_t z = (seed += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        m->s[i] = z ^ (z >> 31);
    }
}

/* ---- set-up ---------------------------------------------------------------- */

const char *uam_mutator_op_name(unsigned op)
{
    return op < UAM_MUT_OP_LIMIT ? op_info[op].name : NULL;
}

unsigned uam_mutator_default_weight(unsigned op)
{
    return op < UAM_MUT_OP_LIMIT ? op_info[op].weight : 0;
}

bool uam_mutator_is_op(unsigned op)
{
    return op < UAM_MUT_OP_LIMIT && op != UAM_MUT_SPLICE && op_info[op].name;
}

void uam_mutator_reset_weights(uam_mutator *m)
{
    for (unsigned op = 0; op < UAM_MUT_OP_LIMIT; op++)
        m->weight[op] = uam_mutator_is_op(op) ? op_info[op].weight : 0;
    m->dirty = true;
}

uam_mutator *uam_mutator_create(void)
{
    uam_mutator *m = calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    uam_mutator_seed(m, 0);
    m->splice_permille = 150;
    uam_mutator_reset_weights(m);
    if (uam_mutator_set_stack(m, 2, 128) != UAM_OK
        || uam_mutator_set_max_size(m, UAM_MUT_DEFAULT_MAX_SIZE) != UAM_OK) {
        uam_mutator_destroy(m);
        return NULL;
    }
    return m;
}

void uam_mutator_destroy(uam_mutator *m)
{
    if (!m)
        return;
    uam_mutator_clear_tokens(m);
    free(m->tokens);
    free(m->int_tokens);
    for (size_t i = 0; i < m->n_corpus; i++)
        free(m->corpus[i].data);
    free(m->corpus);
    free(m->buf);
    free(m->tmp);
    free(m);
}

uam_status uam_mutator_set_max_size(uam_mutator *m, size_t max_size)
{
    if (max_size < 1)
        return UAM_ERR_INVALID;
    if (max_size == m->max_size)
        return UAM_OK;
    uint8_t *b = malloc(max_size), *t = malloc(max_size);
    if (!b || !t) {
        free(b);
        free(t);
        return UAM_ERR_NOMEM;
    }
    free(m->buf);
    free(m->tmp);
    m->buf = b;
    m->tmp = t;
    m->max_size = max_size;
    return UAM_OK;
}

uam_status uam_mutator_set_stack(uam_mutator *m, unsigned lo, unsigned hi)
{
    if (lo < 1 || lo > hi || hi > UAM_MUT_STACK_MAX)
        return UAM_ERR_INVALID;
    /* log-uniform: the powers of two in [lo, hi], or lo alone */
    m->n_stack = 0;
    for (unsigned v = 2; v <= hi; v *= 2)
        if (v >= lo)
            m->stack[m->n_stack++] = v;
    if (!m->n_stack)
        m->stack[m->n_stack++] = lo;
    return UAM_OK;
}

void uam_mutator_set_splice(uam_mutator *m, unsigned permille)
{
    m->splice_permille = permille > 1000 ? 1000 : permille;
}

uam_status uam_mutator_set_weight(uam_mutator *m, unsigned op, unsigned weight)
{
    if (!uam_mutator_is_op(op) || weight > 0xFFFF)
        return UAM_ERR_INVALID;
    m->weight[op] = weight;
    m->dirty = true;
    return UAM_OK;
}

unsigned uam_mutator_weight(const uam_mutator *m, unsigned op)
{
    return op < UAM_MUT_OP_LIMIT ? m->weight[op] : 0;
}

/* ---- dictionary ------------------------------------------------------------ */

void uam_mutator_clear_tokens(uam_mutator *m)
{
    for (size_t i = 0; i < m->n_tokens; i++)
        if (!m->tokens[i].is_int)
            free(m->tokens[i].bytes);
    m->n_tokens = m->n_int_tokens = 0;
    m->dirty = true;
}

static token *new_token(uam_mutator *m)
{
    if (m->n_tokens == m->cap_tokens) {
        size_t cap = m->cap_tokens ? 2 * m->cap_tokens : 16;
        token *t = realloc(m->tokens, cap * sizeof(*t));
        size_t *ix = realloc(m->int_tokens, cap * sizeof(*ix));
        if (t)
            m->tokens = t;
        if (ix)
            m->int_tokens = ix;
        if (!t || !ix)
            return NULL;
        m->cap_tokens = cap;
    }
    m->dirty = true;
    token *t = &m->tokens[m->n_tokens];
    memset(t, 0, sizeof(*t));
    return t;
}

uam_status uam_mutator_add_token(uam_mutator *m, const uint8_t *data, size_t len)
{
    if (!len || len > UAM_MUT_MAX_TOKEN)
        return UAM_ERR_INVALID;
    token *t = new_token(m);
    uint8_t *b = t ? malloc(len) : NULL;
    if (!b)
        return UAM_ERR_NOMEM;
    memcpy(b, data, len);
    t->bytes = b;
    t->len = len;
    m->n_tokens++;
    return UAM_OK;
}

static void encode_int(uint8_t *out, uint64_t v, unsigned width, bool be)
{
    for (unsigned i = 0; i < width; i++)
        out[be ? width - 1 - i : i] = (uint8_t)(v >> (8 * i));
}

static uint64_t width_mask(unsigned width)
{
    return width >= 8 ? ~0ull : (1ull << (8 * width)) - 1;
}

uam_status uam_mutator_add_int_token(uam_mutator *m, uint64_t value, unsigned width,
                                     bool big_endian)
{
    if (width != 1 && width != 2 && width != 4 && width != 8)
        return UAM_ERR_INVALID;
    token *t = new_token(m);
    if (!t)
        return UAM_ERR_NOMEM;
    t->is_int = true;
    t->big_endian = big_endian;
    t->value = value & width_mask(width);
    t->len = width;
    encode_int(t->data, t->value, width, big_endian);
    m->int_tokens[m->n_int_tokens++] = m->n_tokens++;
    return UAM_OK;
}

size_t uam_mutator_n_tokens(const uam_mutator *m) { return m->n_tokens; }

/* The integer tokens' data pointers move with the array: set them on use. */
static const token *pick_token(uam_mutator *m)
{
    token *t = &m->tokens[below(m, m->n_tokens)];
    if (t->is_int)
        t->bytes = t->data;
    return t;
}

/* ---- corpus ---------------------------------------------------------------- */

static entry *find_entry(const uam_mutator *m, uint32_t id)
{
    for (size_t i = 0; i < m->n_corpus; i++)
        if (m->corpus[i].id == id)
            return &m->corpus[i];
    return NULL;
}

uam_status uam_mutator_corpus_put(uam_mutator *m, uint32_t id, const uint8_t *data, size_t len)
{
    uint8_t *copy = malloc(len ? len : 1);
    if (!copy)
        return UAM_ERR_NOMEM;
    if (len)
        memcpy(copy, data, len);
    entry *e = find_entry(m, id);
    if (!e) {
        if (m->n_corpus == m->cap_corpus) {
            size_t cap = m->cap_corpus ? 2 * m->cap_corpus : 64;
            entry *c = realloc(m->corpus, cap * sizeof(*c));
            if (!c) {
                free(copy);
                return UAM_ERR_NOMEM;
            }
            m->corpus = c;
            m->cap_corpus = cap;
        }
        e = &m->corpus[m->n_corpus++];
        e->id = id;
    } else {
        free(e->data);
    }
    e->data = copy;
    e->len = len;
    return UAM_OK;
}

const uint8_t *uam_mutator_corpus_get(const uam_mutator *m, uint32_t id, size_t *len)
{
    const entry *e = find_entry(m, id);
    if (!e)
        return NULL;
    *len = e->len;
    return e->data;
}

size_t uam_mutator_corpus_size(const uam_mutator *m) { return m->n_corpus; }

/* ---- operator choice --------------------------------------------------------- */

static bool backed(const uam_mutator *m, unsigned op)
{
    switch (op) {
    case UAM_MUT_DICT_INSERT:
    case UAM_MUT_DICT_OVERWRITE: return m->n_tokens > 0;
    case UAM_MUT_DICT_INT_ADJ:   return m->n_int_tokens > 0;
    default:                     return true;
    }
}

static void rebuild(uam_mutator *m)
{
    m->n_pick = 0;
    m->pick_total = 0;
    for (unsigned op = 1; op < UAM_MUT_OP_LIMIT; op++) {
        if (!uam_mutator_is_op(op) || !m->weight[op] || !backed(m, op))
            continue;
        m->pick_total += m->weight[op];
        m->pick_op[m->n_pick] = (uint8_t)op;
        m->pick_cum[m->n_pick++] = m->pick_total;
    }
    m->dirty = false;
}

static unsigned pick_op(uam_mutator *m)
{
    uint32_t r = (uint32_t)below(m, m->pick_total);
    unsigned lo = 0, hi = m->n_pick - 1;
    while (lo < hi) {               /* the first cumulative weight above r */
        unsigned mid = (lo + hi) / 2;
        if (m->pick_cum[mid] > r)
            hi = mid;
        else
            lo = mid + 1;
    }
    return m->pick_op[lo];
}

/* AFL's choose_block_len: a length in [1, limit] (limit >= 1), usually small. */
static size_t block_len(uam_mutator *m, size_t limit)
{
    size_t lo, hi;
    switch (below(m, 3)) {
    case 0:  lo = 1; hi = BLK_SMALL; break;
    case 1:  lo = BLK_SMALL; hi = BLK_MEDIUM; break;
    default:
        if (below(m, 10)) {
            lo = BLK_MEDIUM;
            hi = BLK_LARGE;
        } else {
            lo = BLK_LARGE;
            hi = BLK_XL;
        }
    }
    if (lo >= limit)
        lo = 1;
    if (hi > limit)
        hi = limit;
    return range(m, lo, hi);
}

/* ---- operators -------------------------------------------------------------- */

typedef struct mbuf {
    uint8_t *b;
    size_t   len, cap;
} mbuf;

static uint64_t load(const uint8_t *p, unsigned w, bool be)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < w; i++)
        v |= (uint64_t)p[be ? w - 1 - i : i] << (8 * i);
    return v;
}

static int arith_delta(uam_mutator *m)
{
    int d = (int)range(m, 0, 2 * ARITH_MAX) - ARITH_MAX;
    return d ? d : 1;
}

static void op_arith(uam_mutator *m, mbuf *x, unsigned w, bool be)
{
    if (x->len < w)
        return;
    uint8_t *p = x->b + below(m, x->len - w + 1);
    encode_int(p, (load(p, w, be) + (uint64_t)(int64_t)arith_delta(m)) & width_mask(w), w, be);
}

static void op_interesting(uam_mutator *m, mbuf *x, unsigned w, bool be)
{
    if (x->len < w)
        return;
    uint8_t *p = x->b + below(m, x->len - w + 1);
    uint64_t v;
    if (w == 1)
        v = below(m, 2) ? interesting_u8[below(m, UAM_ARRAY_LEN(interesting_u8))]
                        : (uint8_t)interesting_8[below(m, UAM_ARRAY_LEN(interesting_8))];
    else if (w == 2)
        v = interesting_16[below(m, UAM_ARRAY_LEN(interesting_16))];
    else
        v = interesting_32[below(m, UAM_ARRAY_LEN(interesting_32))];
    encode_int(p, v, w, be);
}

/* Make room for n bytes at pos (n <= cap - len). */
static uint8_t *open_gap(mbuf *x, size_t pos, size_t n)
{
    memmove(x->b + pos + n, x->b + pos, x->len - pos);
    x->len += n;
    return x->b + pos;
}

static void insert(mbuf *x, size_t pos, const uint8_t *src, size_t n)
{
    memcpy(open_gap(x, pos, n), src, n);
}

static void fill(uam_mutator *m, uint8_t *p, size_t n, const mbuf *x)
{
    if (below(m, 2)) {                      /* a constant: random, or one the input has */
        int v = x->len && !below(m, 4) ? x->b[below(m, x->len)] : (int)below(m, 256);
        memset(p, v, n);
    } else {
        for (size_t i = 0; i < n; i++)
            p[i] = (uint8_t)next64(m);
    }
}

static void op_delete(uam_mutator *m, mbuf *x)
{
    if (x->len < 2)
        return;
    size_t n = block_len(m, x->len - 1), pos = below(m, x->len - n + 1);
    memmove(x->b + pos, x->b + pos + n, x->len - pos - n);
    x->len -= n;
}

static void op_clone(uam_mutator *m, mbuf *x, uint8_t *tmp)
{
    if (x->len == x->cap)
        return;
    if (!x->len) {
        x->b[x->len++] = (uint8_t)next64(m);
        return;
    }
    size_t n = block_len(m, x->len);
    if (n > x->cap - x->len)
        n = x->cap - x->len;
    size_t from = below(m, x->len - n + 1), to = below(m, x->len + 1);
    memcpy(tmp, x->b + from, n);
    insert(x, to, tmp, n);
}

static void op_insert_block(uam_mutator *m, mbuf *x)
{
    if (x->len == x->cap)
        return;
    size_t n = block_len(m, x->cap - x->len), pos = below(m, x->len + 1);
    mbuf before = *x;
    fill(m, open_gap(x, pos, n), n, &before);
}

static void op_overwrite(uam_mutator *m, mbuf *x)
{
    if (x->len < 2)
        return;
    size_t n = block_len(m, x->len), to = below(m, x->len - n + 1);
    if (below(m, 10) < 7)
        memmove(x->b + to, x->b + below(m, x->len - n + 1), n);
    else
        memset(x->b + to, (int)below(m, 256), n);
}

static void op_swap(uam_mutator *m, mbuf *x, uint8_t *tmp)
{
    if (x->len < 4)
        return;
    size_t n = range(m, 1, x->len / 2 < BLK_SMALL / 2 ? x->len / 2 : BLK_SMALL / 2);
    size_t a = below(m, x->len - n + 1), b = below(m, x->len - n + 1);
    if ((a > b ? a - b : b - a) < n)
        return;
    memcpy(tmp, x->b + a, n);
    memmove(x->b + a, x->b + b, n);
    memcpy(x->b + b, tmp, n);
}

/* A new length, log-uniform in [1, cap]: each power-of-two size class as likely as the
 * next, so a large max_size is reached without most resizes landing near it. */
static void op_resize(uam_mutator *m, mbuf *x)
{
    unsigned bits = 0;
    while (bits < 63 && (2ull << bits) <= x->cap)
        bits++;
    unsigned k = (unsigned)below(m, bits + 1);   /* the size class [2^k, 2^(k+1)) */
    size_t lo = (size_t)1 << k, hi = (size_t)((2ull << k) - 1);
    size_t n = range(m, lo, hi < x->cap ? hi : x->cap);
    if (n > x->len)
        fill(m, x->b + x->len, n - x->len, x);
    x->len = n;
}

static void op_dict_insert(uam_mutator *m, mbuf *x)
{
    const token *t = pick_token(m);
    if (t->len > x->cap - x->len)
        return;
    insert(x, below(m, x->len + 1), t->bytes, t->len);
}

static void op_dict_overwrite(uam_mutator *m, mbuf *x)
{
    const token *t = pick_token(m);
    if (t->len > x->len)
        return;
    memcpy(x->b + below(m, x->len - t->len + 1), t->bytes, t->len);
}

static void op_dict_int_adj(uam_mutator *m, mbuf *x)
{
    const token *t = &m->tokens[m->int_tokens[below(m, m->n_int_tokens)]];
    uint8_t enc[8];
    encode_int(enc, (t->value + (below(m, 2) ? 1 : (uint64_t)-1)) & width_mask((unsigned)t->len),
               (unsigned)t->len, t->big_endian);
    if (x->len >= t->len && below(m, 2))
        memcpy(x->b + below(m, x->len - t->len + 1), enc, t->len);
    else if (t->len <= x->cap - x->len)
        insert(x, below(m, x->len + 1), enc, t->len);
}

static void apply(uam_mutator *m, mbuf *x, unsigned op)
{
    switch (op) {
    case UAM_MUT_BIT_FLIP:
        if (x->len)
            x->b[below(m, x->len)] ^= (uint8_t)(1u << below(m, 8));
        break;
    case UAM_MUT_BYTE_FLIP:
        if (x->len)
            x->b[below(m, x->len)] ^= 0xFF;
        break;
    case UAM_MUT_BYTE_ARITH:              op_arith(m, x, 1, false); break;
    case UAM_MUT_WORD_ARITH_LE:           op_arith(m, x, 2, false); break;
    case UAM_MUT_WORD_ARITH_BE:           op_arith(m, x, 2, true); break;
    case UAM_MUT_DWORD_ARITH_LE:          op_arith(m, x, 4, false); break;
    case UAM_MUT_DWORD_ARITH_BE:          op_arith(m, x, 4, true); break;
    case UAM_MUT_SET_INTERESTING_U8:      op_interesting(m, x, 1, false); break;
    case UAM_MUT_SET_INTERESTING_U16_LE:  op_interesting(m, x, 2, false); break;
    case UAM_MUT_SET_INTERESTING_U16_BE:  op_interesting(m, x, 2, true); break;
    case UAM_MUT_SET_INTERESTING_U32_LE:  op_interesting(m, x, 4, false); break;
    case UAM_MUT_SET_INTERESTING_U32_BE:  op_interesting(m, x, 4, true); break;
    case UAM_MUT_RANDOM_BYTE:
        if (x->len)
            x->b[below(m, x->len)] = (uint8_t)next64(m);
        else
            x->b[x->len++] = (uint8_t)next64(m);
        break;
    case UAM_MUT_DELETE_CHUNK:            op_delete(m, x); break;
    case UAM_MUT_CLONE_CHUNK:             op_clone(m, x, m->tmp); break;
    case UAM_MUT_OVERWRITE_CHUNK:         op_overwrite(m, x); break;
    case UAM_MUT_SWAP_CHUNKS:             op_swap(m, x, m->tmp); break;
    case UAM_MUT_DICT_INSERT:             op_dict_insert(m, x); break;
    case UAM_MUT_DICT_OVERWRITE:          op_dict_overwrite(m, x); break;
    case UAM_MUT_DICT_INT_ADJ:            op_dict_int_adj(m, x); break;
    case UAM_MUT_INSERT_BLOCK:            op_insert_block(m, x); break;
    case UAM_MUT_RESIZE:                  op_resize(m, x); break;
    default:                              break;
    }
}

/* ---- a mutation ------------------------------------------------------------- */

/* AFL's splice: a crossover between the first and last byte in which a and the donor
 * differ, within their common length. false: no such point. */
static bool splice(uam_mutator *m, mbuf *x)
{
    const entry *d = &m->corpus[below(m, m->n_corpus)];
    size_t n = x->len < d->len ? x->len : d->len, first = SIZE_MAX, last = 0;
    for (size_t i = 0; i < n; i++) {
        if (x->b[i] != d->data[i]) {
            if (first == SIZE_MAX)
                first = i;
            last = i;
        }
    }
    if (first == SIZE_MAX || first == last)
        return false;
    size_t p = range(m, first + 1, last);
    size_t tail = d->len - p;
    if (tail > x->cap - p)
        tail = x->cap - p;
    memcpy(x->b + p, d->data + p, tail);
    x->len = p + tail;
    return true;
}

size_t uam_mutator_max_size(const uam_mutator *m) { return m->max_size; }

uint8_t *uam_mutator_mutate(uam_mutator *m, const uint8_t *src, size_t len,
                            size_t *out_len, uint64_t *ops)
{
    if (m->dirty)
        rebuild(m);
    if (!m->n_pick)
        return NULL;
    mbuf x = { .b = m->buf, .len = len < m->max_size ? len : m->max_size, .cap = m->max_size };
    memcpy(x.b, src, x.len);
    uint64_t used = 0;
    if (m->n_corpus && below(m, 1000) < m->splice_permille && splice(m, &x))
        used |= 1ull << UAM_MUT_SPLICE;
    for (unsigned k = m->stack[below(m, m->n_stack)]; k; k--) {
        unsigned op = pick_op(m);
        /* An empty input: only an operator that adds bytes does anything. */
        if (!x.len && op != UAM_MUT_DICT_INSERT && op != UAM_MUT_DICT_INT_ADJ
            && op != UAM_MUT_CLONE_CHUNK && op != UAM_MUT_RANDOM_BYTE
            && op != UAM_MUT_INSERT_BLOCK && op != UAM_MUT_RESIZE)
            op = m->n_tokens ? UAM_MUT_DICT_INSERT : UAM_MUT_INSERT_BLOCK;
        used |= 1ull << op;
        apply(m, &x, op);
    }
    *out_len = x.len;
    if (ops)
        *ops = used;
    return x.b;
}
