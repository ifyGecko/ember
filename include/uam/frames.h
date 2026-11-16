/*
 * uam/frames.h - Call tracking: a record of the calls a processor is inside.
 *
 * The frame pass inserts UAM_OP_FRAME (ir.h) before every EXIT a lifter marked as a call,
 * a return or an exception return. While a processor has a uam_frame_state, those ops
 * keep a stack of frames in it: a call pushes the address it returns to, a return pops
 * the frame it returns to, an interrupt or exception pushes a marker that its return
 * pops. The stack is a best-effort history, not a model of the guest's stack: code that
 * leaves functions without returning (longjmp, a task switch, a tail of hand-written
 * assembly) leaves frames behind, and a return to an older frame drops the ones above it.
 *
 * The return guard is a check on top that does not depend on the stack being exact:
 * **a return to an address that no call returns to is reported**. The pass records the
 * return address of every call it instruments, when the block is translated, in a set
 * (uam_ret_sites) shared by a machine's processors; a return that matches no frame and
 * is not in the set goes to the state's bad_return callback. A switch to another task, a
 * longjmp and a frame built by hand all return to some call's return address; a return
 * address overwritten with data does not. What it cannot see: a return address
 * overwritten with another call's return address, and code that jumps through a return
 * instruction on purpose (a pushed address followed by RET), which needs its targets
 * added with uam_ret_sites_add.
 *
 * The exit kinds are the lifters' (docs/uam-isa.md section 7.3). Nothing here knows a
 * calling convention.
 *
 * Machines: uam_machine_set_frames (machine.h) gives every processor a state and saves
 * the stacks and the set in snapshots. Dependencies: ir.h, pass.h.
 */
#ifndef UAM_FRAMES_H
#define UAM_FRAMES_H

#include <stdio.h>

#include "uam/pass.h"

struct uam_cpu;
struct uam_exec;
struct uam_state_writer;
struct uam_state_reader;

/* Frames kept per processor; a deeper call drops the oldest half. */
#define UAM_FRAME_DEPTH 512u
/* `ret` of a marker: an interrupt or exception was taken at `callee`. */
#define UAM_FRAME_MARK UINT64_MAX

typedef struct uam_frame {
    uint64_t ret;      /* where the call returns to (pc state bits cleared) */
    uint64_t callee;   /* where it went */
} uam_frame;

/* The return addresses of the calls translated so far, plus any added by hand. */
typedef struct uam_ret_sites uam_ret_sites;
uam_ret_sites *uam_ret_sites_create(void);
void           uam_ret_sites_destroy(uam_ret_sites *rs);
bool           uam_ret_sites_add(uam_ret_sites *rs, uint64_t pc);   /* false: out of memory */
bool           uam_ret_sites_has(const uam_ret_sites *rs, uint64_t pc);
size_t         uam_ret_sites_count(const uam_ret_sites *rs);
/* Snapshot form. Loading adds to the set (it only ever grows). */
void           uam_ret_sites_save(const uam_ret_sites *rs, struct uam_state_writer *w);
void           uam_ret_sites_load(uam_ret_sites *rs, struct uam_state_reader *r);

/* A return at `pc` to `target`, which no frame and no call's return address matches.
 * Called from inside the block; it may ask the executor to stop. */
typedef void (*uam_bad_return_fn)(void *ctx, struct uam_cpu *cpu, uint64_t pc, uint64_t target);

typedef struct uam_frame_state {
    uint32_t           n;          /* frames in use: f[0] oldest, f[n - 1] innermost */
    bool               guard;      /* report bad returns */
    uint64_t           pc_mask;    /* clears the ISA's pc state bits (set by uam_exec_set_frames) */
    uam_ret_sites     *sites;
    uam_bad_return_fn  bad_return;
    void              *bad_ctx;
    struct uam_cpu    *cpu;        /* the processor it is attached to */
    uint64_t           calls, returns, resyncs, bad_returns;
    uam_frame          f[UAM_FRAME_DEPTH];
} uam_frame_state;

/* What UAM_OP_FRAME does (the interpreter and both JITs call these). */
void uam_frame_call(uam_frame_state *s, uint64_t ret, uint64_t callee);
void uam_frame_ret(uam_frame_state *s, uint64_t target, uint64_t pc);
void uam_frame_eret(uam_frame_state *s);
/* An interrupt or exception taken at `pc`: push a marker. */
void uam_frame_mark(uam_frame_state *s, uint64_t pc);
/* A PC hook moved the processor to `pc`: if that is where the innermost call returns to,
 * the hook returned for the function it replaced, and the frame goes. */
void uam_frame_hook_return(uam_frame_state *s, uint64_t pc);
/* Forget the frames (not the set). */
void uam_frame_reset(uam_frame_state *s);

/* ---- the pass ------------------------------------------------------------ */

typedef struct uam_frame_pass uam_frame_pass;
/* `site(ctx, ret)` is called for every call instrumented, with its return address;
 * `pc_mask` clears the pc state bits before the pass compares addresses. */
uam_frame_pass *uam_frame_pass_create(uint64_t pc_mask, void (*site)(void *ctx, uint64_t ret),
                                      void *ctx);
void            uam_frame_pass_destroy(uam_frame_pass *fp);
const uam_pass *uam_frame_pass_get(const uam_frame_pass *fp);

/* ---- one executor -------------------------------------------------------- */

/* Track x's processor in `s` (s->sites set; s->guard, s->bad_return as wanted); NULL
 * stops tracking. Discards translated code. */
uam_status uam_exec_set_frames(struct uam_exec *x, uam_frame_state *s);

/* ---- a machine ----------------------------------------------------------- */

struct uam_machine;

/*
 * Track calls on every processor of `m` (false: stop). The frame stacks and the set of
 * return addresses become part of the machine's state: snapshots carry them, and a
 * snapshot taken without tracking cannot be restored into a machine that tracks (nor the
 * other way round). Call it after the processors are added and before the machine runs
 * or a snapshot is taken or loaded. Discards translated code.
 */
uam_status uam_machine_track_calls(struct uam_machine *m, bool on);
/* Processor `cpu`'s state (NULL: not tracking). */
uam_frame_state *uam_machine_frames(const struct uam_machine *m, unsigned cpu);
/* Its frames, innermost first; returns how many were written (at most max). */
size_t uam_machine_backtrace(const struct uam_machine *m, unsigned cpu, uam_frame *out, size_t max);
/*
 * Print them, one line each after `prefix`, innermost first: "#0 0x... called from
 * 0x..." for a call (the address is the callee, "from" the return address), "#1
 * interrupted at 0x..." for a marker. `sym`, if not NULL, names an address: it writes
 * "name+0xoff" for `pc` into buf and returns true, or returns false.
 */
typedef bool (*uam_symbolize_fn)(void *ctx, uint64_t pc, char *buf, size_t n);
void uam_machine_backtrace_print(const struct uam_machine *m, unsigned cpu, FILE *out,
                                 const char *prefix, uam_symbolize_fn sym, void *ctx);

#endif /* UAM_FRAMES_H */
