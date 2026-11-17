/*
 * uam/fuzz_mutate.h - ember's input mutator: havoc, splice and dictionaries.
 *
 * A mutation copies a parent input and applies a stack of 2..128 (log-uniform) havoc
 * operators to it, each picked by weight; with probability splice/1000 the parent is
 * first spliced with a donor from the mutator's corpus (AFL's rule: a crossover point
 * between the first and last byte in which the two differ). Everything is deterministic
 * given uam_mutator_seed.
 *
 * The length moves both ways and evenly. The chunk operators draw their lengths the way
 * AFL's havoc does (usually 1-32 bytes, sometimes 32-128, sometimes 128-1500 and now and
 * then more), within a step that scales with the input: a growth (CLONE_CHUNK,
 * INSERT_BLOCK) adds up to the input's own length, a cut (DELETE_CHUNK) removes up to two
 * thirds of it. Whenever an operator that changes the length is drawn, a coin decides
 * between growing and cutting, whatever the weights: one of the three chunk operators
 * becomes a cut or a growth, a DICT_INSERT inserts its token or cuts as many bytes as a
 * token has (so does DICT_INT_ADJ where it would insert), and RESIZE, the long step, goes
 * to a length between the input's and max_size or between the input's and one byte,
 * log-uniform on that side. So a mutation is as likely to come out shorter as longer
 * wherever the input stands between one byte and max_size, a seed of a few bytes reaches
 * max_size and one of max_size a few bytes, and a campaign's inputs do not settle at
 * either end. At the ends the move without room does nothing. A direction whose
 * operators have weight 0 stays off (DELETE_CHUNK for cuts; CLONE_CHUNK and INSERT_BLOCK
 * for the chunk growths).
 *
 * Dictionary tokens are byte strings or integers (1, 2, 4 or 8 bytes, little- or
 * big-endian); DICT_INT_ADJ writes an integer token plus or minus 1, the off-by-one
 * neighbour of a boundary check.
 *
 * The operator ids are stable: the fuzz protocol and the brain's statistics carry them.
 * 20 and 21 (structured inserts, which ran Python code) are retired; new ones append.
 * UAM_MUT_SPLICE is no havoc operator: it marks, in a mutation's operator set, that the
 * mutation spliced (its weight is the splice rate, not an entry of the weight table).
 *
 * Not thread-safe; one mutator per fuzzing thread.
 *
 * Dependencies: common.h.
 */
#ifndef UAM_FUZZ_MUTATE_H
#define UAM_FUZZ_MUTATE_H

#include "uam/common.h"

enum uam_mut_op {
    UAM_MUT_BIT_FLIP               = 1,
    UAM_MUT_BYTE_FLIP              = 2,
    UAM_MUT_BYTE_ARITH             = 3,
    UAM_MUT_WORD_ARITH_LE          = 4,
    UAM_MUT_WORD_ARITH_BE          = 5,
    UAM_MUT_DWORD_ARITH_LE         = 6,
    UAM_MUT_DWORD_ARITH_BE         = 7,
    UAM_MUT_SET_INTERESTING_U8     = 8,
    UAM_MUT_SET_INTERESTING_U16_LE = 9,
    UAM_MUT_SET_INTERESTING_U16_BE = 10,
    UAM_MUT_SET_INTERESTING_U32_LE = 11,
    UAM_MUT_SET_INTERESTING_U32_BE = 12,
    UAM_MUT_RANDOM_BYTE            = 13,
    UAM_MUT_DELETE_CHUNK           = 14,
    UAM_MUT_CLONE_CHUNK            = 15,
    UAM_MUT_OVERWRITE_CHUNK        = 16,
    UAM_MUT_SWAP_CHUNKS            = 17,
    UAM_MUT_DICT_INSERT            = 18,
    UAM_MUT_DICT_OVERWRITE         = 19,
    /* 20, 21: retired */
    UAM_MUT_DICT_INT_ADJ           = 22,
    UAM_MUT_INSERT_BLOCK           = 23,
    UAM_MUT_RESIZE                 = 24,
    UAM_MUT_SPLICE                 = 25,   /* a marker in the operator set, not weighted */
    UAM_MUT_OP_LIMIT               = 26    /* ids are below this */
};

#define UAM_MUT_DEFAULT_MAX_SIZE 65536u
#define UAM_MUT_MAX_TOKEN        256u      /* bytes in one dictionary token */
#define UAM_MUT_STACK_MAX        1024u

typedef struct uam_mutator uam_mutator;

uam_mutator *uam_mutator_create(void);
void         uam_mutator_destroy(uam_mutator *m);

/* The operator's name ("BIT_FLIP", ...) and default weight; NULL / 0 for an id that is
 * no operator. uam_mutator_is_op: a weighted havoc operator (not SPLICE, not retired). */
const char  *uam_mutator_op_name(unsigned op);
unsigned     uam_mutator_default_weight(unsigned op);
bool         uam_mutator_is_op(unsigned op);

/* Configuration. Every setter can be called again at any time. */
uam_status   uam_mutator_set_max_size(uam_mutator *m, size_t max_size);   /* >= 1 */
size_t       uam_mutator_max_size(const uam_mutator *m);
uam_status   uam_mutator_set_stack(uam_mutator *m, unsigned lo, unsigned hi); /* 1 <= lo <= hi */
void         uam_mutator_set_splice(uam_mutator *m, unsigned permille);  /* 0..1000, default 150 */
uam_status   uam_mutator_set_weight(uam_mutator *m, unsigned op, unsigned weight);
unsigned     uam_mutator_weight(const uam_mutator *m, unsigned op);
void         uam_mutator_reset_weights(uam_mutator *m);
void         uam_mutator_seed(uam_mutator *m, uint64_t seed);

/* Dictionary. A byte token of 1..UAM_MUT_MAX_TOKEN bytes; an integer token of 1, 2, 4 or
 * 8 bytes (value reduced to that width). */
void         uam_mutator_clear_tokens(uam_mutator *m);
uam_status   uam_mutator_add_token(uam_mutator *m, const uint8_t *data, size_t len);
uam_status   uam_mutator_add_int_token(uam_mutator *m, uint64_t value, unsigned width,
                                       bool big_endian);
size_t       uam_mutator_n_tokens(const uam_mutator *m);

/* The corpus: parents and splice donors, by the caller's id. put adds or replaces. */
uam_status     uam_mutator_corpus_put(uam_mutator *m, uint32_t id, const uint8_t *data,
                                      size_t len);
const uint8_t *uam_mutator_corpus_get(const uam_mutator *m, uint32_t id, size_t *len);
size_t         uam_mutator_corpus_size(const uam_mutator *m);

/* One mutation of src[0..len): the result is in a buffer of the mutator's (max_size
 * bytes, which the caller may write: a fix-up), valid until the next call or
 * reconfiguration, and *out_len <= max_size. *ops (optional) gets bit
 * k set for every operator k that was applied (and UAM_MUT_SPLICE for a splice). NULL
 * when no operator is enabled (every weight 0, or only dictionary operators and no
 * tokens). */
uint8_t       *uam_mutator_mutate(uam_mutator *m, const uint8_t *src, size_t len,
                                  size_t *out_len, uint64_t *ops);

#endif /* UAM_FUZZ_MUTATE_H */
