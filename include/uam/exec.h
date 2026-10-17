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
 * semantics in docs/uam-isa.md. A JIT, if added, must agree with it.
 *
 * Dependencies: ir.h, isa.h, cpu.h, bus.h, sched.h.
 */
#ifndef UAM_EXEC_H
#define UAM_EXEC_H

#include <stdio.h>

#include "uam/bus.h"
#include "uam/cpu.h"
#include "uam/ir.h"
#include "uam/isa.h"
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
 *     (uam_xlate.fixed == false), cpu->xlate_gen is unchanged.
 * Stale blocks are dropped lazily at lookup.
 */
typedef struct uam_tcache {
    struct uam_tc_entry **buckets;  /* chained hash on guest pc */
    uint32_t              n_buckets;/* power of two */
    uint32_t              n_blocks;
    uam_bus              *bus;
} uam_tcache;

uam_status uam_tcache_init(uam_tcache *tc, uam_bus *bus);
void       uam_tcache_fini(uam_tcache *tc);
/* Returns the block for pc if present and still valid (see above). */
uam_block *uam_tcache_lookup(uam_tcache *tc, uint64_t pc, unsigned priv, uint32_t xlate_gen);
uam_status uam_tcache_insert(uam_tcache *tc, uam_block *blk, unsigned priv, bool mapped,
                             uint32_t xlate_gen);
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
    struct uam_jit  *jit;             /* native backend, or NULL (interpreter only) */
    uint32_t         seen_dgen, seen_xgen;  /* dtlb is flushed when these fall behind */
    unsigned         trace;           /* enum uam_trace_flag */
    FILE            *trace_out;
    uint64_t         trace_lo, trace_hi;  /* UAM_TRACE_BLOCKS: only pcs in [lo, hi) (hi=0: all) */
    uint64_t         trace_after;     /* UAM_TRACE_BLOCKS: only once icount >= this */
    bool             stop_request;
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

    /* fault-loop detection */
    bool             last_fetch_fault_valid;
    uint64_t         last_fetch_fault_pc, last_fetch_fault_icount;
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

/* Enable the native JIT backend. Returns false (and stays on the interpreter) if code
 * generation is unavailable on this host. Call after uam_exec_init, before running. */
bool            uam_exec_enable_jit(uam_exec *x);

#endif /* UAM_EXEC_H */
