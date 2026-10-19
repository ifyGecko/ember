/*
 * uam/cov.h - The edge-coverage pass: UAM_OP_COV at chosen guest pcs.
 *
 * The pass takes a plain table of sites - a guest virtual pc and the two COV
 * immediates (docs/uam-isa.md section 7.5) - and inserts COV right after the IMARK of
 * every instruction at one of those pcs. It knows nothing about where the table came
 * from (a site list file, a block hash, a test): choosing A and B is the caller's
 * coverage scheme. All the work is done when a block is translated: the table is
 * sorted once and each instruction costs one binary search.
 *
 * Most users enable coverage through the executor (uam_exec_set_coverage in exec.h),
 * which owns a pass made here and points the hart at the coverage state. The pass
 * itself only edits IR, so it works unchanged for every ISA.
 *
 * Dependencies: pass.h.
 */
#ifndef UAM_COV_H
#define UAM_COV_H

#include "uam/pass.h"

typedef struct uam_cov_site {
    uint64_t pc;     /* guest virtual pc of the instruction */
    uint32_t a, b;   /* COV operands */
} uam_cov_site;

typedef struct uam_cov_pass uam_cov_pass;

/*
 * A pass for `n` sites (copied). Two sites at one pc are UAM_ERR_INVALID, with the pc
 * in err. A site that turns out to be a continuation instruction (a branch delay
 * slot) cannot be instrumented: translating it fails the pass, naming the pc.
 */
uam_status uam_cov_pass_create(const uam_cov_site *sites, size_t n, uam_cov_pass **out,
                               char *err, size_t errlen);

/*
 * Block mode, for use without any site list: a site at the start of every translated
 * block, A = uam_cov_block_hash(pc), B = A >> 1 (AFL's edge scheme). Approximate: an
 * ember block is not a basic block (it may end early - size limit, hooks, page ends -
 * or start mid-way when an interrupted block resumes), so which edges exist depends
 * on how the code was split, though never on the execution backend.
 */
uam_status uam_cov_pass_create_blocks(uam_cov_pass **out);

/* Optional: describes a site in error messages (e.g. "record 12 of module fw"), so a
 * site that cannot be instrumented is reported in its source's terms. */
typedef void (*uam_cov_namer_fn)(void *ctx, uint64_t pc, char *buf, size_t len);
void            uam_cov_pass_set_namer(uam_cov_pass *cp, uam_cov_namer_fn fn, void *ctx);

const uam_pass *uam_cov_pass_get(const uam_cov_pass *cp);
void            uam_cov_pass_destroy(uam_cov_pass *cp);

/* Number of sites (0 in block mode). */
size_t          uam_cov_pass_sites(const uam_cov_pass *cp);

/* The block-mode hash of a pc. */
uint32_t        uam_cov_block_hash(uint64_t pc);

#endif /* UAM_COV_H */
