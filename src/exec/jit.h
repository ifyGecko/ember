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
 * for the hot opcodes (IMARK, GET, PUT, LD, plain ST, EXIT, COV and the common integer,
 * compare, select and conversion ops) and calling shared C for the rest - rare pure
 * ops go through the same evaluator as the interpreter (ir_eval.h), and memory accesses
 * off the inline data-TLB path, AMO/CAS, SYSRD/SYSWR and HELPER go through execops.h
 * and the ISA's own functions, so the two backends cannot diverge. Every op compiles;
 * a block too large for the code buffer is marked to run on the interpreter.
 *
 * Profiling: UAM_PERF_MAP and UAM_JIT_OPMAP make the JIT describe the code it places
 * (docs/jit-perf.md).
 *
 * The compiled code is attached to the block (uam_block.backend) and freed with it.
 *
 * Chaining. A block that ends with EXIT (to a constant pc, or to a computed one) may
 * jump straight into the compiled code of the next block instead of returning to the
 * run loop. Each EXIT owns a link slot (target pc, entry, epoch) stored with the code.
 * A slot starts empty; when an EXIT misses, uam_jit_run notes the slot, and if the
 * next block the run loop executes natively is the one that EXIT went to, the slot is
 * pointed at it. A computed EXIT's slot caches its first target of the epoch (compared
 * at run time); its other targets go through a jump table shared by all blocks, a
 * direct-mapped cache of (target pc, entry, epoch) indexed by a hash of the pc and
 * linked the same way, so an EXIT with many targets (a switch, a return) chains too.
 *
 * The jump is taken only when returning to the run loop would change nothing: no exit
 * request (stop, reset, interrupt line change, code write, translation change - the
 * same condition an IMARK checks), the deadline not reached (no timer due, no budget
 * exhausted), chain budget left (host polling), and the slot's epoch current. The
 * epoch moves on whenever a linked block may have become invalid: a block freed or
 * gone stale (uam_tcache.gen), a translation change (cpu->xlate_gen) or a privilege
 * change (cpu->priv). Bridges stop the chain if the bus mapping (direct_gen) or the
 * next timer changed during the block. Interrupts are only taken by the run loop, so
 * this relies on everything that can make isa->irq_take() newly succeed (a line
 * change, an interrupt-enable or software-interrupt write) requesting an exit, as an
 * IMARK would need anyway (isa.h).
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

typedef struct uam_jit_chain {
    uint32_t budget;   /* in: blocks that may be entered by chaining (0 = none) */
    bool     link;     /* in: blk may become the target of earlier blocks' links (no
                        * PC hook on its first instruction) */
    uint32_t hops;     /* out: blocks entered by chaining; *res is the last one's */
} uam_jit_chain;

/* Run a block previously compiled (blk->backend is a real entry, not UAM_JIT_INTERP),
 * and possibly the blocks it chains into. */
void     uam_jit_run(uam_jit *j, const uam_block *blk, uint64_t *temps, uam_jit_chain *chain,
                     uam_block_result *res);

#endif /* UAM_JIT_H */
