/*
 * uam/pass.h - IR passes: transformations applied to every newly translated block.
 *
 * A pass sees each block once, after the lifter, the optimizer and the verifier,
 * and before PC-hook marking and native compilation (exec.h). Passes run in the
 * order they were registered; the block is verified again after each one (unless
 * verification is off), and a pass that fails or leaves invalid IR stops the run
 * with UAM_STOP_ERROR, the log naming the pass.
 *
 * Rules for a pass:
 *   - it may only insert ops: guest-semantic ops and IMARKs are never removed,
 *     reordered or changed, so the block still behaves as the guest code does;
 *   - it must work for every ISA (it sees only IR);
 *   - it is not machine state: passes are not saved in snapshots (like PC hooks).
 * Registering or removing a pass discards all translated code.
 *
 * Passes under analysis/ include only ir.h and this header (docs/architecture.md
 * section 3); the uam_exec they attach to stays opaque to them.
 *
 * Dependencies: ir.h.
 */
#ifndef UAM_PASS_H
#define UAM_PASS_H

#include "uam/ir.h"

struct uam_exec;

typedef struct uam_pass {
    const char *name;
    uam_status (*run)(void *ctx, uam_block *blk);
    void       *ctx;
} uam_pass;

/* Register `p` (the struct is referenced, not copied: it must outlive the
 * registration). Registering the same pass twice is UAM_ERR_INVALID. */
uam_status uam_exec_add_pass(struct uam_exec *x, const uam_pass *p);
/* Returns false if `p` was not registered. */
bool       uam_exec_remove_pass(struct uam_exec *x, const uam_pass *p);

/* ---- editing a block --------------------------------------------------- */

/* An immediate operand holding `v` (the pool entry is shared if v is already
 * there). UAM_REF_NONE on allocation failure. */
uam_ref    uam_block_imm(uam_block *blk, uint64_t v);

/*
 * Insert n ops in one go: ops[k] goes immediately before the op that was at index
 * at[k] (at[k] == n_insn appends). `at` must be non-decreasing; ops sharing an
 * index keep their order. IMARKs may not be inserted (n_guest is unchanged).
 * Inserted ops may use existing temps and immediates; they may not define temps.
 */
uam_status uam_block_insert(uam_block *blk, const uint32_t *at, const uam_insn *ops, uint32_t n);

#endif /* UAM_PASS_H */
