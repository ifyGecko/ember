/*
 * uam/exec.h - Execution: interpreter, translation cache, and run loop.
 *
 * The executor drives one hart. Each block goes through the same steps:
 *
 *   fire due timers -> deliver interrupts (isa->irq_take) -> handle WAIT
 *   -> look up / translate block (isa->lift, verify) -> interpret
 *   -> act on the block result (next pc, trap, fault, wait, interrupted)
 *
 * The interpreter is the reference backend: a direct implementation of the
 * semantics in docs/uam-isa.md. The JIT must agree with it. With `chain` on,
 * native code runs from one compiled block into the next while the loop above
 * would do nothing in between (no exit request, deadline not reached); see
 * src/exec/jit.h. Interrupts are taken only between dispatched blocks, so ISA
 * modules must request an exit when an interrupt may newly be taken (isa.h).
 *
 * Optional instrumentation: IR passes (pass.h) run on every new block, edge coverage
 * (uam_exec_set_coverage) is one such pass, and an exception observer sees each
 * synchronous exception before the ISA takes it (uam_exec_set_exc_hook), and an
 * interrupt observer each interrupt the ISA takes (uam_exec_set_irq_hook).
 *
 * Dependencies: ir.h, isa.h, cpu.h, bus.h, sched.h, pass.h, cov.h.
 */
#ifndef UAM_EXEC_H
#define UAM_EXEC_H

#include <stdio.h>

#include "uam/bus.h"
#include "uam/cov.h"
#include "uam/cpu.h"
#include "uam/ir.h"
#include "uam/isa.h"
#include "uam/pass.h"
#include "uam/sched.h"

/* ------------------------------------------------------------------------ */
/* Interpreter                                                               */
/* ------------------------------------------------------------------------ */

typedef enum uam_block_result_kind {
    UAM_BR_NEXT,         /* EXIT taken: continue at pc */
    UAM_BR_TRAP,         /* TRAP op: trap_class + exc */
    UAM_BR_WAIT,         /* WAIT op: resume at pc once woken */
    UAM_BR_FAULT,        /* an op faulted: exc */
    UAM_BR_INTERRUPTED,  /* exit request / deadline at an IMARK: resume at pc */
} uam_block_result_kind;

typedef struct uam_block_result {
    uam_block_result_kind kind;
    uint64_t              pc;
    uam_trap_class        trap_class;
    uam_exception         exc;
} uam_block_result;

/* Execute one block. `temps` must hold at least blk->n_temp entries. */
uam_block_result uam_interp_block(uam_cpu *cpu, const uam_block *blk,
                                  const uam_helper_table *helpers, uint64_t *temps);

/* ------------------------------------------------------------------------ */
/* Translation cache                                                         */
/* ------------------------------------------------------------------------ */

struct uam_tc_entry;

/*
 * Blocks are keyed by guest (virtual) pc. A block stays valid while
 *   - the generation of every (physical) code page it covers is unchanged
 *     (the bus bumps it on any write to a page holding translated code),
 *   - the hart runs at the privilege level the block was translated at, and
 *   - if any of its code was fetched through a non-fixed mapping
 *     (uam_xlate.fixed == false), cpu->xlate_gen is unchanged - or, after it
 *     changed, the `remap` callback confirms that the block's pages still
 *     translate to the same physical pages (then the block is kept and
 *     re-stamped with the new generation). Blocks whose lift saw a failed
 *     translation are never revalidated that way (uam_tcache_insert_ex).
 * Stale blocks are dropped lazily at lookup.
 */
typedef bool (*uam_tcache_remap_fn)(void *ctx, const uam_block *blk);
typedef struct uam_tcache {
    struct uam_tc_entry **buckets;  /* chained hash on guest pc */
    uint32_t              n_buckets;/* power of two */
    uint32_t              n_blocks;
    uint32_t              gen;      /* bumped whenever a block is freed or may have gone
                                     * stale (code write); JIT block links depend on it */
    uam_bus              *bus;
    uam_tcache_remap_fn   remap;    /* optional, see above */
    void                 *remap_ctx;
} uam_tcache;

uam_status uam_tcache_init(uam_tcache *tc, uam_bus *bus);
void       uam_tcache_fini(uam_tcache *tc);
/* Returns the block for pc if present and still valid (see above). */
uam_block *uam_tcache_lookup(uam_tcache *tc, uint64_t pc, unsigned priv, uint32_t xlate_gen);
uam_status uam_tcache_insert(uam_tcache *tc, uam_block *blk, unsigned priv, bool mapped,
                             uint32_t xlate_gen);
/* As uam_tcache_insert; `remappable` false keeps the block from being
 * revalidated through `remap` (it also depended on a translation that failed
 * while it was lifted, e.g. a fetch that cut it short). */
uam_status uam_tcache_insert_ex(uam_tcache *tc, uam_block *blk, unsigned priv, bool mapped,
                                uint32_t xlate_gen, bool remappable);
void       uam_tcache_flush(uam_tcache *tc);

/* ------------------------------------------------------------------------ */
/* Run loop                                                                  */
/* ------------------------------------------------------------------------ */

typedef enum uam_stop_reason {
    UAM_STOP_NONE,
    UAM_STOP_REQUEST,     /* uam_exec_stop() (e.g. test finisher device) */
    UAM_STOP_HALT,        /* TRAP HALT */
    UAM_STOP_DEBUG,       /* TRAP DEBUG */
    UAM_STOP_INSN_LIMIT,  /* instruction budget exhausted */
    UAM_STOP_DEADLOCK,    /* WAIT with no pending interrupt, no timer and no host input */
    UAM_STOP_ERROR,       /* internal error (bad IR, lift failure) */
    UAM_STOP_FAULT_LOOP,  /* a fetch fault re-raised itself with no progress */
    UAM_STOP_HOOK,        /* a PC hook returned UAM_HOOK_STOP (breakpoint) */
    UAM_STOP_RESET,       /* reset requested and no reset handler installed */
    UAM_STOP_TIME_LIMIT,  /* virtual time reached max_time or UAM_SCHED_TIME_MAX */
    UAM_STOP_LIVELOCK,    /* timers keep firing, no instruction retires, nothing else can wake the hart */
    UAM_STOP_EXC_HOOK,    /* the exception observer returned UAM_HOOK_STOP */
    UAM_STOP_IRQ_HOOK,    /* the interrupt observer returned UAM_HOOK_STOP */
} uam_stop_reason;

const char *uam_stop_reason_str(uam_stop_reason r);

enum uam_trace_flag {
    UAM_TRACE_BLOCKS = 1u << 0,  /* each executed block's pc (see trace_lo/hi/after) */
    UAM_TRACE_IR     = 1u << 1,  /* IR of each translated block */
    UAM_TRACE_EXC    = 1u << 2,  /* exceptions, interrupts and resets */
};

typedef struct uam_exec_stats {
    uint64_t blocks_translated;
    uint64_t blocks_executed;
    uint64_t exceptions;
    uint64_t interrupts;
    uint64_t idle_ticks;
    uint64_t resets;
    uint64_t hooks;              /* PC hook invocations */
    uint64_t blocks_native;      /* executed via the JIT */
    uint64_t blocks_compiled;    /* compiled to native code */
    uint64_t blocks_interp;      /* marked to run interpreted */
    uint64_t blocks_chained;     /* native blocks entered straight from another (no dispatch) */
} uam_exec_stats;

/* --- reset requests ------------------------------------------------------- */

typedef enum uam_reset_kind {
    UAM_RESET_SYSTEM,  /* whole machine: devices and harts (RAM is preserved) */
    UAM_RESET_CPU,     /* the hart only */
} uam_reset_kind;

/* Performs a requested reset; installed by the machine (machine.h). */
typedef void (*uam_reset_fn)(void *ctx, uam_reset_kind kind);

/* --- PC hooks ---------------------------------------------------------- */

typedef enum uam_hook_action {
    UAM_HOOK_CONTINUE,  /* run on (from cpu->pc, which the hook may have changed) */
    UAM_HOOK_STOP,      /* return UAM_STOP_HOOK from uam_exec_run */
} uam_hook_action;

/*
 * Called when the hart arrives at `pc`, before that instruction executes, with
 * the architectural state precise (all earlier instructions retired). The hook
 * may read and write registers and memory, change cpu->pc (to emulate a
 * function, say), or stop the run. Hooks on an instruction that continues
 * another (a delay slot) are not supported; hook the branch instead.
 */
typedef uam_hook_action (*uam_pc_hook_fn)(void *ctx, uam_cpu *cpu, uint64_t pc);

typedef struct uam_pc_hook {
    uint64_t       pc;
    uam_pc_hook_fn fn;
    void          *ctx;
} uam_pc_hook;

/* --- exception observer --------------------------------------------------- */

/*
 * Called for every synchronous exception (a fault, a TRAP EXCEPTION, a fetch fault)
 * just before the ISA takes it, with precise state: earlier instructions retired,
 * cpu->pc at the faulting instruction (at its parent_pc if it continues another, as
 * in a delay slot). Returning UAM_HOOK_STOP makes uam_exec_run return
 * UAM_STOP_EXC_HOOK without delivering the exception; resuming re-executes the
 * instruction. Interrupts and resets are not reported.
 */
typedef uam_hook_action (*uam_exc_hook_fn)(void *ctx, uam_cpu *cpu, const uam_exception *e);

/* --- interrupt observer --------------------------------------------------- */

typedef struct uam_interrupt {
    uint64_t from_pc;     /* where the hart was interrupted (it resumes there) */
    uint64_t handler_pc;  /* where the ISA entered the handler */
    uint64_t lines;       /* the lines that caused it (isa->irq_lines; 0 if unknown) */
} uam_interrupt;

/*
 * Called each time the ISA takes an interrupt, after it entered the handler (the
 * state is the handler's first instruction; interrupts are only taken between
 * blocks, so all earlier instructions have retired). Returning UAM_HOOK_STOP makes
 * uam_exec_run return UAM_STOP_IRQ_HOOK there; the interrupt has been taken.
 * Synchronous exceptions go to the exception observer instead.
 */
typedef uam_hook_action (*uam_irq_hook_fn)(void *ctx, uam_cpu *cpu, const uam_interrupt *irq);

/* --- host integration ----------------------------------------------------- */

/*
 * Optional bridge to the host side (host I/O, real-time pacing). The executor
 * stays deterministic unless a host bridge makes it otherwise.
 */
typedef struct uam_exec_host {
    /* Called every `poll_interval` executed blocks (0 = 1024): deliver pending
     * host input to devices, optionally throttle to real time. */
    void (*poll)(void *ctx);
    /* The hart is idle and the next timer is `ticks` away (UINT64_MAX: none).
     * May block on host input. Sets *skip (<= ticks) to the virtual time to
     * advance. Returns false if nothing can ever wake the hart (deadlock). */
    bool (*idle)(void *ctx, uint64_t ticks, uint64_t *skip);
    void    *ctx;
    uint32_t poll_interval;
} uam_exec_host;

#define UAM_EXEC_DEFAULT_MAX_STALL (UINT64_C(1) << 22)

typedef struct uam_exec {
    uam_cpu         *cpu;
    uam_sched       *sched;
    uam_tcache       tc;
    uam_builder      b;
    uint64_t        *temps;
    uint32_t         temps_cap;
    uint32_t         max_block_insns;
    bool             verify;          /* run the IR verifier on every new block */
    bool             optimize;        /* run uam_optimize on every new block (default on) */
    bool             use_jit;         /* compile blocks to native code when available */
    bool             chain;           /* JIT: jump straight from block to block (default on) */
    struct uam_jit  *jit;             /* native backend, or NULL (interpreter only) */
    uint32_t         seen_dgen, seen_xgen;  /* dtlb is flushed when these fall behind */
    unsigned         trace;           /* enum uam_trace_flag */
    FILE            *trace_out;
    uint64_t         trace_lo, trace_hi;  /* UAM_TRACE_BLOCKS: only pcs in [lo, hi) (hi=0: all) */
    uint64_t         trace_after;     /* UAM_TRACE_BLOCKS: only once icount >= this */
    volatile bool    stop_request;    /* set by uam_exec_stop, possibly from a signal handler */
    int64_t          stop_status;     /* status from uam_exec_stop / TRAP HALT */
    uam_exec_stats   stats;

    /* reset */
    bool             reset_request;
    uam_reset_kind   reset_kind;
    uam_reset_fn     reset_fn;
    void            *reset_ctx;

    /* PC hooks */
    uam_pc_hook     *hooks;
    uint32_t         n_hooks, cap_hooks;
    uint64_t         hook_filter[16];     /* 1024-bit pre-filter on hashed pcs */
    bool             hook_fired;          /* hooks already ran for (pc, icount) below */
    uint64_t         hook_fired_pc, hook_fired_icount;

    /* host bridge */
    uam_exec_host    host;
    uint32_t         blocks_since_poll;

    /* IR passes (pass.h), in registration order */
    const uam_pass **passes;
    uint32_t         n_passes, cap_passes;
    /* edge coverage (uam_exec_set_coverage): the pass the executor owns */
    uam_cov_pass    *cov_pass;
    /* exception and interrupt observers: one slot each */
    uam_exc_hook_fn  exc_hook;
    void            *exc_hook_ctx;
    uam_irq_hook_fn  irq_hook;
    void            *irq_hook_ctx;

    /* Bounds on virtual time, which (unlike icount) advances while the hart
     * waits. Both are deterministic: no host time is involved.
     *   max_time:  stop with UAM_STOP_TIME_LIMIT once uam_sched_now() reaches
     *              this absolute tick (0 = only at UAM_SCHED_TIME_MAX).
     *   max_stall: after this many consecutive stall rounds - an idle skip
     *              to the next timer, or a uam_sched_run_due that left timers
     *              due - without a single instruction retiring, the timers
     *              are taken to be unable to wake the hart (a WAIT whose
     *              interrupt line is not wired, say). A waiting hart then
     *              blocks on host input through host.idle(ticks = UINT64_MAX)
     *              instead of skipping time; if nothing could deliver input
     *              (no host bridge, or it reports a deadlock), and for a
     *              timer storm, the run stops with UAM_STOP_LIVELOCK.
     *              0 = never. Default UAM_EXEC_DEFAULT_MAX_STALL. */
    uint64_t         max_time;
    uint64_t         max_stall;
    uint64_t         stall, stall_icount;

    /* fault-loop detection */
    bool             last_fetch_fault_valid;
    uint64_t         last_fetch_fault_pc, last_fetch_fault_icount;

    /* Multiprocessor machines (uam_machine_add_cpus). Processor 0's executor
     * runs them all: `smp` lists every processor's executor, its own first,
     * and uam_exec_run(processor 0's) interleaves them in slices of `quantum`
     * instructions on the shared virtual clock (time advances while any
     * processor runs; idle skips happen only when every one waits). The
     * others point `ctl` at it: stop and reset requests, the host bridge,
     * the instruction budget and the time bounds are its; PC, exception and
     * interrupt observers and IR passes are per executor. Where the
     * interleaving stands (slice_*) is machine state: a run that stops
     * mid-slice resumes there, so splitting a run into several - or saving and
     * restoring a snapshot in between - interleaves exactly as one run would. */
    struct uam_exec **smp;
    uint32_t         n_smp, quantum;
    struct uam_exec *ctl;
    uam_cpu         *running;        /* the processor in its slice (NULL: single) */
    uint64_t         retired;        /* instructions all processors retired */
    uint32_t         slice_cpu;      /* the processor whose slice is current */
    uint64_t         slice_left;     /* instructions left in it (0: a fresh slice) */
    bool             round_ran;      /* a processor has run in the current round */
    uint64_t         slice_base;     /* the running processor's icount when slice_left was set */
    bool             smp_resync;     /* a snapshot was restored mid-slice: resume from slice_* */
} uam_exec;

uam_status      uam_exec_init(uam_exec *x, uam_cpu *cpu, uam_sched *sched);
void            uam_exec_fini(uam_exec *x);
/* Run until something stops the hart. max_insns = 0 means no limit. */
uam_stop_reason uam_exec_run(uam_exec *x, uint64_t max_insns);
/* Request a stop at the next instruction boundary. Safe to call from a
 * device, a hook, or a signal handler. */
void            uam_exec_stop(uam_exec *x, int64_t status);

/* Request a reset at the next instruction boundary. uam_exec_run performs it
 * through the reset handler and keeps running; without a handler it returns
 * UAM_STOP_RESET. */
void            uam_exec_request_reset(uam_exec *x, uam_reset_kind kind);
void            uam_exec_set_reset_handler(uam_exec *x, uam_reset_fn fn, void *ctx);

/* PC hooks. Several hooks may share a pc; they run in insertion order.
 * Adding or removing a hook discards translated code. */
uam_status      uam_exec_add_pc_hook(uam_exec *x, uint64_t pc, uam_pc_hook_fn fn, void *ctx);
bool            uam_exec_remove_pc_hook(uam_exec *x, uint64_t pc, uam_pc_hook_fn fn, void *ctx);
bool            uam_exec_has_pc_hook(const uam_exec *x, uint64_t pc);

void            uam_exec_set_host(uam_exec *x, const uam_exec_host *host);

/*
 * Edge coverage. Instrument the given sites (copied; see cov.h) and count into
 * `state`, which must stay valid while coverage is on; cpu->cov points at it.
 * sites = NULL / n = 0 or state = NULL turns coverage off. Every call replaces the
 * previous configuration and discards translated code. With coverage off no block
 * contains COV, so runs are exactly as without this feature. The map is not part of
 * any snapshot. On UAM_ERR_INVALID (two sites at one pc) err says why and coverage
 * is off.
 */
uam_status      uam_exec_set_coverage(uam_exec *x, const uam_cov_site *sites, size_t n,
                                      uam_cov_state *state, char *err, size_t errlen);
/* Block mode (uam_cov_pass_create_blocks): a site at every block start. NULL turns
 * coverage off. */
uam_status      uam_exec_set_block_coverage(uam_exec *x, uam_cov_state *state);

/* Install the exception observer (NULL removes it). There is one slot; the fuzz
 * harness owns it while it runs, so it cannot be shared with the gdb stub. */
void            uam_exec_set_exc_hook(uam_exec *x, uam_exc_hook_fn fn, void *ctx);
/* Install the interrupt observer (NULL removes it). One slot, owned by the fuzz
 * harness while it runs. */
void            uam_exec_set_irq_hook(uam_exec *x, uam_irq_hook_fn fn, void *ctx);

/* Enable the native JIT backend. Returns false (and stays on the interpreter) if code
 * generation is unavailable on this host. Call after uam_exec_init, before running. */
bool            uam_exec_enable_jit(uam_exec *x);

#endif /* UAM_EXEC_H */
