/*
 * spin.h - Skipping the passes of a spin loop (internal to the core).
 *
 * A guest that waits for something to change often spins in one block that jumps back
 * to itself: it reads memory, computes, writes registers and branches, and nothing else.
 * While nothing outside it runs (until the next timer, exit request or host poll, which
 * only the run loop delivers), every pass from a state the pass leaves unchanged leaves
 * it unchanged again, so the passes up to that point differ in nothing but icount. The
 * run loop checks that the next pass would be such a pass and adds the whole passes
 * before the deadline to icount at once (run.c), with exactly the effect running them
 * would have had.
 *
 *   uam_spin_candidate: decided once per block from its IR. The block's only effects
 *     are register writes, plain loads and exits; one exit goes back to its own pc;
 *     and its register writes and exits depend on nothing it writes itself (so a loop
 *     that counts is not one: each pass changes its state).
 *   uam_spin_fixed: decided before each run of a candidate. Evaluates one pass from
 *     the current state without performing it (on a copy of the register file): true
 *     if the register file at its end would equal the current one, every load reads
 *     RAM or ROM (no device, watch or page crossing), and the pass ends with the EXIT
 *     back to the block.
 */
#ifndef UAM_SPIN_H
#define UAM_SPIN_H

#include "uam/exec.h"
#include "uam/ir.h"

bool uam_spin_candidate(const uam_block *blk);

/* `t`: temps for the evaluation (as many as the block has). *never is set when the
 * block can never qualify (a load that is not from plain RAM or ROM). */
bool uam_spin_fixed(uam_cpu *cpu, const uam_block *blk, uint64_t *t, bool *never);

/* Spin state of a cached block: 0 undecided, -1 never a candidate, SPIN_CAND + n a
 * candidate whose last n checks failed in a row. After SPIN_GIVE_UP failures it is
 * treated as never: a candidate that does not qualify (whatever the reason) gets its
 * self-link back, so it costs no more than before. */
#define SPIN_CAND    1
#define SPIN_GIVE_UP 16

/* tcache.c: the block for pc, as uam_tcache_lookup, and the spin state of its entry,
 * reset whenever the entry gets a new block. */
uam_block *uam_tcache_find(uam_tcache *tc, uint64_t pc, unsigned priv, uint32_t xlate_gen,
                           int8_t **spin);

#endif /* UAM_SPIN_H */
