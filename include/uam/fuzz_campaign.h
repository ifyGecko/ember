/*
 * uam/fuzz_campaign.h - The campaign engine: what a fuzzer keeps between runs.
 *
 * The harness (fuzz.h) runs one input and says how it ended. A campaign remembers what
 * every run so far has shown and judges each new one against it:
 *   - coverage: the bucket/count classes seen (a count's class is 1, 2, 4 for 1, 2, 3,
 *     then 8 (4-7), 16 (8-15), 32 (16-31), 64 (32-127), 128 (128-255)); a run that ended
 *     normally is interesting when it sets a bucket never hit or a class a bucket never had;
 *   - paths: a 64-bit name for a run's set of (bucket, class) pairs, the same whatever
 *     order they were hit in: mix(sum of mix(index << 8 | class)), with the coverage
 *     formula's mix (docs/fuzz-formats.md);
 *   - crashes: novel when their (verdict, detail) is new or when they ran code no earlier
 *     crash ran (a bucket no crash has hit), so that two bugs at one pc are told apart.
 * It owns a mutator (fuzz_mutate.h) and runs batches with it: n mutations of one corpus
 * entry, each fixed up by the route's fix-up (uam_fuzz_fixup), with the batch's totals.
 *
 * The campaign is the one owner of this state. A frontend (the fuzz protocol,
 * fuzz/frontend_net.c; a program of your own) reads judgements from it instead of keeping
 * a second record. Every run of the harness must be judged, in order, for the record to be
 * whole: uam_fuzz_campaign_judge after uam_fuzz_run_one, or the batch calls.
 *
 * Dependencies: fuzz.h, fuzz_mutate.h.
 */
#ifndef UAM_FUZZ_CAMPAIGN_H
#define UAM_FUZZ_CAMPAIGN_H

#include "uam/fuzz.h"
#include "uam/fuzz_mutate.h"

typedef struct uam_fuzz_campaign uam_fuzz_campaign;

/* How many verdicts there are (by_verdict below is indexed by uam_fuzz_verdict). */
#define UAM_FUZZ_N_VERDICTS 7u

typedef struct uam_fuzz_judgement {
    uam_fuzz_verdict verdict;
    uint64_t         detail;
    /* a run that ended normally */
    uint32_t         new_buckets;   /* buckets never hit before */
    uint32_t         new_classes;   /* (bucket, class) pairs never seen before (>= new_buckets) */
    uint64_t         path;          /* the path's name */
    /* a crash (fault, oracle, guard) */
    bool             novel_crash;
    /* in a batch: worth reporting - new coverage, a novel crash, or a hang or timeout
     * while the campaign has reported fewer than its cap (uam_fuzz_campaign_set_hang_cap) */
    bool             report;
} uam_fuzz_judgement;

uam_fuzz_campaign *uam_fuzz_campaign_create(uam_fuzz *f);
void               uam_fuzz_campaign_destroy(uam_fuzz_campaign *c);
uam_mutator       *uam_fuzz_campaign_mutator(uam_fuzz_campaign *c);
/* Hangs and timeouts a campaign's batches report in all (default 64). */
void               uam_fuzz_campaign_set_hang_cap(uam_fuzz_campaign *c, uint32_t max);
/* Buckets ever hit by a run that ended normally. */
uint64_t           uam_fuzz_campaign_edges(const uam_fuzz_campaign *c);

/* Judge the run the harness just finished (not one of a batch) and fold it into the
 * record. UAM_ERR_NOMEM only. */
uam_status uam_fuzz_campaign_judge(uam_fuzz_campaign *c, uam_fuzz_verdict v, uint64_t detail,
                                   uam_fuzz_judgement *j);

/* ---- batches ------------------------------------------------------------ */

#define UAM_FUZZ_BATCH_EDGES (1u << 0)   /* also count the runs that hit each bucket */

typedef struct uam_fuzz_count {
    uint64_t key;    /* a path's name, or a bucket index */
    uint32_t n;
} uam_fuzz_count;

typedef struct uam_fuzz_batch {
    uint32_t execs, reports, left;
    uint32_t by_verdict[UAM_FUZZ_N_VERDICTS];
    uint32_t op_used[UAM_MUT_OP_LIMIT];    /* runs each operator took part in */
    uint32_t op_found[UAM_MUT_OP_LIMIT];   /* ... that found new coverage */
    /* the mutation last handed out */
    const uint8_t *data;
    size_t         len;
    uint64_t       ops;                    /* bit k: operator k was applied */
} uam_fuzz_batch;

/* Start a batch of n mutations of the mutator's corpus entry `entry`, seeded with `seed`.
 * UAM_ERR_INVALID: no such entry, n is 0, or no operator is enabled. */
uam_status uam_fuzz_campaign_batch_begin(uam_fuzz_campaign *c, uint32_t entry, uint32_t n,
                                         uint64_t seed, unsigned flags);
/* The next mutation (fixed up), valid until the next call; false when the batch is done. */
bool       uam_fuzz_campaign_batch_next(uam_fuzz_campaign *c, const uint8_t **data, size_t *len);
/* Judge the run of that mutation; counts it in the batch. UAM_ERR_NOMEM only. */
uam_status uam_fuzz_campaign_batch_judge(uam_fuzz_campaign *c, uam_fuzz_verdict v, uint64_t detail,
                                         uam_fuzz_judgement *j);
/* The batch so far (or the last one). */
const uam_fuzz_batch *uam_fuzz_campaign_batch(const uam_fuzz_campaign *c);
/* Runs per path, and per bucket with UAM_FUZZ_BATCH_EDGES: arrays of the campaign's, valid
 * until the next batch begins. */
const uam_fuzz_count *uam_fuzz_campaign_batch_paths(uam_fuzz_campaign *c, size_t *n);
const uam_fuzz_count *uam_fuzz_campaign_batch_edges(uam_fuzz_campaign *c, size_t *n);

/* The hit-count class of a count, and the path name of a list of n (index, count) pairs. */
uint8_t  uam_fuzz_count_class(uint8_t count);
uint64_t uam_fuzz_path_term(uint32_t index, uint8_t count);   /* summed, then uam_fuzz_path_end */
uint64_t uam_fuzz_path_end(uint64_t sum);

#endif /* UAM_FUZZ_CAMPAIGN_H */
