/*
 * jit.h - Optional native-code backend for UAM-IR (internal to the core).
 *
 * The JIT compiles a verified, optimized block into host machine code that produces
 * exactly the result uam_interp_block would (docs/uam-isa.md section 9), and the run
 * loop runs that code instead of interpreting. It is a second backend, never the
 * contract: the interpreter remains the reference, blocks it cannot compile run
 * interpreted, and a host without a code generator disables it transparently.
 *
 * Design (see jit_x86.c): one compiled function per block, emitting inline host code
 * for the hot opcodes (IMARK, GET, PUT, LD, plain ST, EXIT and the common integer,
 * compare, select and conversion ops) and calling shared C for the rest - rare pure
 * ops go through the same evaluator as the interpreter (ir_eval.h), and every memory
 * access goes through execops.h, so the two backends cannot diverge. A block using an
 * op the JIT does not emit (AMO, CAS, exclusive ST, SYSRD/SYSWR, TRAP, WAIT, FENCE,
 * HELPER) is marked to run on the interpreter.
 *
 * The compiled code is attached to the block (uam_block.backend) and freed with it.
 */
#ifndef UAM_JIT_H
#define UAM_JIT_H

#include "uam/exec.h"
#include "uam/ir.h"

typedef struct uam_jit uam_jit;

/* backend sentinel: this block must run on the interpreter. */
#define UAM_JIT_INTERP ((void *)1)

/* Create a JIT for `x` (its tcache is flushed when the code arena fills), or NULL if
 * native code generation is unavailable on this host. */
uam_jit *uam_jit_create(uam_exec *x);
void     uam_jit_destroy(uam_jit *j);

/* Compile `blk` (verified and optimized). Sets blk->backend to the entry point, or to
 * UAM_JIT_INTERP if the block must be interpreted. Returns false only on a hard error
 * (out of memory). */
bool     uam_jit_compile(uam_jit *j, uam_block *blk);

/* Run a block previously compiled (blk->backend is a real entry, not UAM_JIT_INTERP). */
void     uam_jit_run(uam_jit *j, const uam_block *blk, uint64_t *temps, uam_block_result *res);

#endif /* UAM_JIT_H */
