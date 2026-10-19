/*
 * uam/fuzz.h - A persistent-mode fuzz harness on top of snapshots and coverage.
 *
 * Lifecycle:
 *   1. Boot until the first arrival at the begin pc. The coverage counted so far is
 *      the INIT map.
 *   2. Take a snapshot there.
 *   3. For every input: restore the snapshot, clear the map and the coverage
 *      predecessor, deliver the input through the selected route, run until the
 *      iteration ends, a crash check fires or the per-iteration budget runs out, and
 *      classify the outcome. A crash never ends the campaign: the next input starts
 *      from the snapshot again, since nothing outside the machine died.
 *
 * Iteration boundaries come in two modes: *paired* (begin pcs and end pcs) and
 * *begin-only* (an iteration ends when a begin pc is reached again). Both are PC
 * hooks. A coverage site at a boundary pc is counted by its hook, before the map is
 * published: the begin's hit belongs to the map it closes (INIT, or the previous
 * iteration in begin-only mode), an end's to the iteration it ends, and an
 * iteration's first count is the site after the begin, with an empty history. Crash checks: crash pcs (an RTOS's fatal-error or exception handler), the
 * exception observer filtered by exception class, interrupts from chosen lines (a
 * watchdog's warning interrupt), a reset during an iteration, and the
 * executor's stop reasons (fault loop, internal error; deadlock and livelock are hangs;
 * the instruction budget is a timeout).
 *
 * The same input after the same snapshot gives the same map and the same result, on
 * the interpreter and on the JIT. Nothing here knows any fuzzer's formats: frontends
 * (a replay tool, a network protocol) sit behind uam_fuzz_frontend.
 *
 * The harness owns the machine's exception and interrupt observers and wraps its reset hook while it
 * exists, and owns the coverage map.
 *
 * Dependencies: machine.h, snapshot.h, exec.h, dev/mmio_stub.h (the input stream type).
 */
#ifndef UAM_FUZZ_H
#define UAM_FUZZ_H

#include <stdio.h>

#include "uam/dev/mmio_stub.h"
#include "uam/machine.h"

typedef enum uam_fuzz_verdict {
    UAM_FUZZ_OK,            /* the iteration ended normally */
    UAM_FUZZ_CRASH_FAULT,   /* detail: pc of the faulting instruction */
    UAM_FUZZ_CRASH_ORACLE,  /* detail: the crash pc reached (0 for a reset, the status
                             * for a non-zero halt, the matching lines for a crashing
                             * interrupt) */
    UAM_FUZZ_HANG,          /* detail: the uam_stop_reason (deadlock, livelock) */
    UAM_FUZZ_TIMEOUT,       /* detail: 0 */
    UAM_FUZZ_REJECTED,      /* the route could not deliver the input (it returned
                             * UAM_ERR_REJECTED): nothing ran. detail: 0 */
} uam_fuzz_verdict;

const char *uam_fuzz_verdict_str(uam_fuzz_verdict v);

/* Exception classes that count as a crash (uam_fuzz_config.crash_exc). */
enum uam_fuzz_exc_class {
    UAM_FUZZ_EXC_FETCH  = 1u << 0,  /* instruction fetch bus fault */
    UAM_FUZZ_EXC_LOAD   = 1u << 1,  /* load bus fault */
    UAM_FUZZ_EXC_STORE  = 1u << 2,  /* store / atomic bus fault */
    UAM_FUZZ_EXC_ALIGN  = 1u << 3,  /* misaligned access */
    UAM_FUZZ_EXC_XLATE  = 1u << 4,  /* translation fault (TLB miss, protection): firmware
                                     * with a TLB takes these routinely */
    UAM_FUZZ_EXC_TRAP   = 1u << 5,  /* TRAP EXCEPTION: illegal instruction, but also
                                     * system calls and breakpoints, per ISA */
    UAM_FUZZ_EXC_SYSREG = 1u << 6,  /* a rejected system register access or helper */
};
#define UAM_FUZZ_EXC_DEFAULT (UAM_FUZZ_EXC_FETCH | UAM_FUZZ_EXC_LOAD | UAM_FUZZ_EXC_STORE \
                              | UAM_FUZZ_EXC_ALIGN)
#define UAM_FUZZ_EXC_ALL     0x7Fu

/* "fetch,load,trap", "all", "none", "default". false on an unknown name. */
bool uam_fuzz_parse_exc_classes(const char *s, unsigned *out);

typedef struct uam_fuzz_config {
    uint64_t        begin_pc;      /* where an iteration begins (the snapshot is taken there) */
    const uint64_t *more_begin_pcs; /* other begins, each like begin_pc (copied): the snapshot
                                    * is taken at whichever begin the boot reaches first, and
                                    * during an iteration any begin ends it (begin-only) or
                                    * opens a nested level (paired + nested) */
    size_t          n_more_begin_pcs;
    const uint64_t *end_pcs;       /* paired mode: reaching any of these ends an iteration
                                    * (copied; none: begin-only mode) */
    size_t          n_end_pcs;
    bool            nested;        /* paired: a begin during an iteration opens a nested
                                    * level and every end closes one; only the end that
                                    * closes the outermost level ends the iteration
                                    * (recursion stays in one input) */
    uint64_t        iter_insns;    /* per-iteration budget (0: UAM_FUZZ_DEFAULT_ITER_INSNS) */
    uint64_t        boot_insns;    /* budget to reach the first begin (0: unlimited) */
    uint32_t        begin_skip;    /* boot passes the begin pc this many times and takes the
                                    * snapshot at the next arrival: firmware that runs its
                                    * handler during start-up (libcov's
                                    * LIBCOV_BOUNDARY_INIT_GRACE); all of it is INIT */
    unsigned        crash_exc;     /* enum uam_fuzz_exc_class */
    const uint64_t *crash_pcs;     /* copied */
    size_t          n_crash_pcs;
    uint64_t        crash_irq_lines; /* an interrupt from any of these lines (bit i =
                                    * line i, as wired to the ISA) is a crash */
    uint32_t        map_size;      /* coverage map bytes, a power of two (0: 64 KiB) */
    uam_cov_history history;       /* COV history shape for site coverage (zero: classic
                                    * one-predecessor edges; block coverage always is) */
    bool            skip_repeats;  /* a site counts only when another site counted since
                                    * its last count (uam_cov_state.skip_repeat) */
} uam_fuzz_config;

#define UAM_FUZZ_DEFAULT_ITER_INSNS UINT64_C(10000000)

/* Everything known about the last iteration (or the boot). */
typedef struct uam_fuzz_result {
    uam_fuzz_verdict verdict;
    uint64_t         detail;
    uam_stop_reason  stop;        /* how uam_exec_run returned */
    uint64_t         pc;          /* hart pc when it stopped */
    uint64_t         insns;       /* instructions the iteration retired */
    bool             exc_valid;   /* a crashing exception: */
    uam_exception    exc;
    bool             reset;       /* the machine was reset during the iteration */
    bool             irq_valid;   /* a crashing interrupt (crash_irq_lines): */
    uam_interrupt    irq;
} uam_fuzz_result;

/* uam_fuzz_frontend.report kind */
enum { UAM_FUZZ_KIND_INIT = 1, UAM_FUZZ_KIND_ITER = 2 };

typedef struct uam_fuzz_info {
    uint32_t    map_size;
    const char *scheme;     /* coverage scheme ("none", "blocks", or what set_sites named) */
    const char *arch;       /* e.g. "rv32imac", "mips32be" (the ISA's name) */
    const char *isa;
} uam_fuzz_info;

/*
 * A source of inputs and a sink of results. next_input returns > 0 with an input
 * (valid until the next call), 0 when there are no more, < 0 on error. report is
 * called once with kind INIT (id 0, the boot map) and then once per input.
 */
typedef struct uam_fuzz_frontend {
    uam_status (*hello)(void *ctx, const uam_fuzz_info *info);
    int        (*next_input)(void *ctx, uint8_t **data, size_t *len, uint64_t *id);
    uam_status (*report)(void *ctx, uint64_t id, uam_fuzz_verdict v, uint64_t detail,
                         const uint8_t *map, size_t map_size, int kind);
    void       (*bye)(void *ctx);
} uam_fuzz_frontend;

typedef struct uam_fuzz uam_fuzz;

/* Delivers an input to the guest; the same type as a machine input (machine.h).
 * UAM_ERR_REJECTED: this input cannot be delivered (too long for the buffer, not in the
 * shape the route needs): the iteration's verdict is UAM_FUZZ_REJECTED and the campaign
 * goes on. Any other error is the harness's and stops it. */
typedef uam_input_fn uam_fuzz_deliver_fn;

/* NULL on a bad configuration (err says why). */
uam_fuzz  *uam_fuzz_create(uam_machine *m, const uam_fuzz_config *cfg, char *err, size_t errlen);
void       uam_fuzz_destroy(uam_fuzz *f);

/* Coverage source, before uam_fuzz_boot. Without one the map stays zero. `scheme`
 * names it for frontends (uam_fuzz_info.scheme). */
uam_status uam_fuzz_set_sites(uam_fuzz *f, const uam_cov_site *sites, size_t n,
                              const char *scheme, char *err, size_t errlen);
uam_status uam_fuzz_set_block_coverage(uam_fuzz *f);

/*
 * Input routes. Built in, by spec string:
 *   "mem:ADDR[,len=ADDR][,max=N][,reject]"
 *                                  write the input to guest memory at virtual ADDR,
 *                                  optionally its length as a 32-bit word in the CPU's
 *                                  byte order, truncated to N bytes (default 4096), or with
 *                                  "reject" refused when longer (verdict REJECTED)
 * Registered: uam_fuzz_add_route, then the machine's inputs (uam_machine_add_input,
 * e.g. a board's "uart0"). uam_fuzz_mmio_deliver feeds a uam_mmio_input (ctx) for
 * --mmio-rule input registers; the runner registers it as "mmio".
 */
uam_status uam_fuzz_add_route(uam_fuzz *f, const char *name, uam_fuzz_deliver_fn fn, void *ctx);
uam_status uam_fuzz_select_route(uam_fuzz *f, const char *spec, char *err, size_t errlen);
uam_status uam_fuzz_mmio_deliver(void *ctx, uam_machine *m, const uint8_t *data, size_t len);

/* Called by uam_fuzz_boot at the first begin, before the snapshot (e.g. to check that
 * the code there is what a site list expects). Non-OK fails the boot. */
typedef uam_status (*uam_fuzz_boot_fn)(void *ctx, uam_fuzz *f, uam_machine *m);
void       uam_fuzz_set_boot_hook(uam_fuzz *f, uam_fuzz_boot_fn fn, void *ctx);

/* Sees every map uam_fuzz_run publishes (the INIT map, then one per input), whatever
 * the frontend: a side channel such as a shared-memory exporter. */
typedef void (*uam_fuzz_observer_fn)(void *ctx, uint64_t id, int kind, uam_fuzz_verdict v,
                                     uint64_t detail, const uint8_t *map, size_t map_size);
void       uam_fuzz_set_observer(uam_fuzz *f, uam_fuzz_observer_fn fn, void *ctx);

/* Boot to the first begin and take the snapshot. Fails (with r filled in) if the run
 * stops anywhere else (cfg.boot_insns bounds it). */
uam_status uam_fuzz_boot(uam_fuzz *f, uam_fuzz_result *r);
/* One iteration (after uam_fuzz_boot). The machine is left where the iteration
 * stopped, for inspection. Non-OK only for harness errors (route, snapshot, or an
 * internal error of the executor, UAM_STOP_ERROR, which is never the input's fault). */
uam_status uam_fuzz_run_one(uam_fuzz *f, const uint8_t *data, size_t len, uam_fuzz_result *r);
/* The whole loop with a frontend: boot, hello (after the boot, whether or not it
 * succeeded), INIT report, one report per input, bye. */
uam_status uam_fuzz_run(uam_fuzz *f, const uam_fuzz_frontend *fe, void *ctx);

const uint8_t         *uam_fuzz_map(const uam_fuzz *f, size_t *size);
const uam_fuzz_result *uam_fuzz_last_result(const uam_fuzz *f);
void                   uam_fuzz_get_info(const uam_fuzz *f, uam_fuzz_info *info);

/* Stop at the next instruction boundary and end uam_fuzz_run. Signal-safe. */
void       uam_fuzz_abort(uam_fuzz *f);
bool       uam_fuzz_aborted(const uam_fuzz *f);

/* Counts per verdict over the harness's life. */
typedef struct uam_fuzz_stats {
    uint64_t iterations;
    uint64_t by_verdict[UAM_FUZZ_REJECTED + 1];
} uam_fuzz_stats;
const uam_fuzz_stats *uam_fuzz_get_stats(const uam_fuzz *f);

/* ---- replay frontend (fuzz/frontend_replay.c) --------------------------- */

/*
 * Runs each input file once - FILE, or every regular file in DIR in name order - and
 * prints one line per input to `out`. Its exit status is the most severe verdict of
 * any input: a crash (the kind of the first one), else a hang, else a timeout, else a
 * rejected input, else ok.
 */
#define UAM_FUZZ_EXIT_OK           0
#define UAM_FUZZ_EXIT_CRASH_FAULT  20
#define UAM_FUZZ_EXIT_CRASH_ORACLE 21
#define UAM_FUZZ_EXIT_HANG         22
#define UAM_FUZZ_EXIT_TIMEOUT      23
#define UAM_FUZZ_EXIT_REJECTED     24
int uam_fuzz_exit_status(uam_fuzz_verdict v);

typedef struct uam_fuzz_replay uam_fuzz_replay;
extern const uam_fuzz_frontend uam_fuzz_replay_frontend;
/* NULL if path cannot be read (logged). `f` gives the result details printed. */
uam_fuzz_replay *uam_fuzz_replay_create(const char *path, uam_fuzz *f, FILE *out);
/* The same, but in a directory a file named X<suffix> is skipped when X is there too, for
 * each of the n suffixes: notes a fuzzer saves beside each input (suzzer's brain writes
 * X.info next to crash X), which would otherwise run as inputs. A FILE path always runs. */
uam_fuzz_replay *uam_fuzz_replay_create_ex(const char *path, uam_fuzz *f, FILE *out,
                                           const char *const *sidecars, size_t n_sidecars);
void             uam_fuzz_replay_destroy(uam_fuzz_replay *r);
int              uam_fuzz_replay_exit_status(const uam_fuzz_replay *r);
/* The map of the last input (or the INIT map if there were none). */
const uint8_t   *uam_fuzz_replay_last_map(const uam_fuzz_replay *r, size_t *size);
size_t           uam_fuzz_replay_count(const uam_fuzz_replay *r);

#endif /* UAM_FUZZ_H */
