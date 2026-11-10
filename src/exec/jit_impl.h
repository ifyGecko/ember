/*
 * jit_impl.h - The JIT's inside: what the host-independent core (jit.c) and a host's
 * code generator (jit_x86.c, jit_a64.c) share. Internal to src/exec/.
 *
 * jit.c owns everything that does not depend on the host instruction set: the runtime
 * context native code works on and the C bridges it calls, the code arena, block linking
 * (uam_jit_run), the analyses every block goes through before it is emitted (deferred register
 * writes and cold ops, docs/jit-dead-puts-design.md; register allocation's bookkeeping;
 * flag fusion), and the profiling output. A code generator turns one analysed block into
 * host code in j->cb, through the jit_host_* functions below, and keeps the conventions
 * jit.h describes: one function per block that leaves the cpu exactly as the interpreter
 * would, every block with the same frame and register assignment so that a chained jump
 * enters the next block's body (j->body_off) directly, link slots (jit_slot) stored after
 * a block's code, fault sites (jit_site) after those.
 *
 * Exactly one code generator is built per host. On a host without one, jit.c's
 * uam_jit_create returns NULL and the executor uses the interpreter.
 */
#ifndef UAM_JIT_IMPL_H
#define UAM_JIT_IMPL_H

#include <stddef.h>
#include <stdio.h>

#include "jit.h"
#include "uam/bus.h"
#include "uam/cpu.h"

#if defined(__x86_64__) || (defined(__aarch64__) && defined(__AARCH64EL__))
#define UAM_JIT_HOST 1
#endif

/* ---- runtime context ----------------------------------------------------------- */

/* A chaining link, one per EXIT, stored after the block's code. A computed EXIT's slot
 * and the jump table's entries are read by native code: (pc, entry), valid in one epoch.
 * A constant EXIT's slot (epoch LINK_DIRECT, for good) is only its record: the EXIT ends
 * in a direct jump, and linking patches that jump (jit_host_link). */
typedef struct jit_slot {
    uint64_t entry;   /* host address of the target block's body (valid if epoch matches);
                       * LINK_DIRECT: the address of the EXIT's jump instruction */
    uint64_t epoch;   /* jc.epoch when linked; LINK_NONE when empty; or LINK_DIRECT */
    uint64_t pc;      /* guest pc of the target */
} jit_slot;

#define LINK_NONE   UINT64_MAX
#define LINK_DIRECT (UINT64_MAX - 1)

/* A write-set data-TLB tag with this bit is a page that holds translated code: the
 * inline probe (tag == vpage) never matches it; the store's miss path does, and stores
 * inline if no byte falls in a line that holds code (the host's code-page store stubs). */
#define DTLB_CODE (UINT64_C(1) << 62)

/* The jump table: links for computed EXITs whose own slot holds another target, shared
 * by every block and indexed by a hash of the target pc. */
#define JTAB_BITS 12u
#define JTAB_HASH 0x9E3779B1u    /* index = (uint32_t)(pc * JTAB_HASH) >> (32 - bits) */

/* Where a bridge call happens, for a precise fault: stored with the block's code, and
 * pointed to by jc->site just before a call that may fault (so the common path pays
 * nothing per instruction). */
typedef struct jit_site {
    uint64_t pc;          /* the guest instruction */
    uint64_t parent_pc;   /* its parent, if a continuation (else pc) */
    uint64_t cont;        /* 1 if it is a continuation */
} jit_site;

typedef struct uam_jit_ctx {
    uam_cpu                *cpu;
    uint64_t               *temps;
    uint8_t                *regfile;
    const uam_block        *blk;
    const uam_helper_table *helpers;
    const jit_site         *site;        /* the faulting call's instruction */
    uint64_t                scratch;     /* bridge load destination */
    uam_block_result        res;
    /* Native code keeps icount in a register as icount - ic_base, where ic_base is
     * cpu->icount_stop as it last read it (on entry, after every bridge): negative until
     * the block must stop at an IMARK, so that check is a test of the sign. */
    uint64_t                ic_base;
    /* chaining */
    uint64_t                epoch;       /* links made in another epoch are dead */
    jit_slot               *link_slot;   /* out: the EXIT slot that missed (to be linked) */
    uint32_t                chain_left;  /* chained jumps still allowed */
    uint32_t                chain_budget;/* hops taken = chain_budget - chain_left */
    uint32_t                dgen;        /* bus->direct_gen at entry */
    unsigned                priv;        /* cpu->priv at entry */
    uam_sched              *sched;
    uint64_t                next_when;   /* next timer at entry */
    uam_exec_stats         *stats;       /* slow-path counters (jit_*_calls) */
    /* For a write-set entry tagged DTLB_CODE: the page's code-line mask (jit.c, dtlb_fill) */
    const uint64_t         *clines[UAM_DTLB_SIZE];
    const void             *cstub[16];   /* code-page store stubs (jit_host_stubs) */
    jit_slot                jtab[1u << JTAB_BITS];   /* the jump table (uses 1 << jtab_bits) */
} uam_jit_ctx;

/* The C bridges native code calls (jc first). Those returning int return 1 on a fault,
 * with jc->res filled, else 0 with a loaded value, if any, in jc->scratch. */
int      jit_br_load(uam_jit_ctx *jc, uint64_t addr, uint32_t flags, uint32_t size);
int      jit_br_store(uam_jit_ctx *jc, uint64_t addr, uint64_t val, uint32_t flags, uint32_t size);
/* store-conditional: jc->scratch = 1 if the store was performed, 0 if not */
int      jit_br_store_x(uam_jit_ctx *jc, uint64_t addr, uint64_t val, uint32_t flags, uint32_t size);
/* AMO / CAS: jc->scratch = the old memory value. packed = op | sub << 8 | width << 16. */
int      jit_br_rmw(uam_jit_ctx *jc, uint64_t addr, uint64_t b, uint64_t c, uint32_t flags,
                    uint32_t packed);
int      jit_br_sysrd(uam_jit_ctx *jc, uint32_t id);
int      jit_br_syswr(uam_jit_ctx *jc, uint32_t id, uint64_t val);
/* HELPER: the ISA's C function, as the interpreter calls it */
int      jit_br_helper(uam_jit_ctx *jc, uint32_t id, uint64_t a, uint64_t b, uint64_t c);
/* a pure op through the interpreter's evaluator; packed = op | width << 8 | src_w << 16 */
uint64_t jit_br_eval(uam_jit_ctx *jc, uint32_t packed, uint64_t x, uint64_t y, uint64_t z);

/* ---- the compiler's state ------------------------------------------------------ */

#define ARENA_SIZE (64u << 20)
#define CBUF_MAX   (64u << 10)   /* scratch code buffer per block */
#define MAX_SLOTS  64u           /* EXITs per block that get a link (more just return) */
#define MAX_HOST_REGS 32u
#define NO_TEMP UINT32_MAX

/* Code-region kinds recorded for the op map (UAM_JIT_OPMAP; docs/jit-perf.md). */
enum { MK_PROLOGUE, MK_OP, MK_IMARK_PC, MK_SLOW, MK_CHAIN, MK_STUB, MK_EPILOGUE, MK_SLOTS,
       MK_REPLAY, MK_CODE_ST };
typedef struct jit_opmark { uint32_t off, idx; uint8_t kind; } jit_opmark;

/* An IMARK stop stub's view of a temp it reads: the register holding it there. */
typedef struct jit_snap { uint32_t t; int8_t reg; } jit_snap;

struct uam_jit {
    uam_exec    *x;
    uint8_t     *arena;
    size_t       arena_used;
    size_t       arena_base; /* the shared stubs at the start of the arena (kept on recycle) */
    uam_jit_ctx  jc;         /* reused across block runs */
    bool         popcnt;     /* POPCNT is inline (else it uses the C evaluator) */
    /* the registers temps are allocated to (jit_host_init): pool[0 .. pool_kept) survive
     * bridge calls, the others are saved around them while they hold a live temp */
    const int   *pool;
    unsigned     pool_max, pool_kept;
    unsigned     n_pool;     /* registers to allocate temps to (UAM_JIT_REGS, default all) */
    int          res_reg;    /* scratch: where a result goes that gets no register */
    size_t       body_off;   /* prologue length: chained jumps enter here */
    unsigned     jtab_bits;  /* jump table entries used: 1 << jtab_bits (UAM_JIT_JTAB_BITS) */
    uint32_t     arena_gen;  /* bumped when the arena is recycled */
    /* what the link epoch was last computed from */
    bool         key_valid;
    uint32_t     key_tc, key_xgen;
    unsigned     key_priv;
    /* an EXIT that missed its link in the last native run, to link to the next block */
    jit_slot    *pend_slot;
    uint64_t     pend_pc;
    uint32_t     pend_arena;
    /* per-compile scratch */
    uint8_t      cb[CBUF_MAX];
    size_t       n;
    bool         overflow;
    uint64_t     slot_pc[MAX_SLOTS];   /* each link slot's target (LINK_NONE: computed) */
    size_t       slot_site[MAX_SLOTS]; /* a constant EXIT's jump instruction (code offset) */
    unsigned     n_slots;
    size_t       slots_off;  /* where the slots are in the code (set by the code generator) */
    /* the constant EXITs now linked: their jumps are unlinked when the epoch moves */
    jit_slot   **linked;
    uint32_t     n_linked, cap_linked;
    struct { size_t pos; uint16_t slot, off; } slot_ref[3 * MAX_SLOTS];   /* pc-relative
                                        * references to a slot's field, to patch */
    unsigned     n_slot_refs;
    jit_site     cur;        /* the guest instruction being compiled */
    bool         cur_emitted;/* cur has a site record (sites[n_sites - 1]) */
    jit_site    *sites;      /* site records, emitted after the slots */
    uint32_t     n_sites, cap_sites;
    struct { size_t pos; uint32_t site; } *site_refs;   /* site addresses to patch */
    uint32_t     n_site_refs, cap_site_refs;
    struct { size_t pos; uint64_t pc; uint32_t i, snap, n_snap; } *stubs;  /* out-of-line
                                        * IMARK stops: the branch to patch, the imark, its
                                        * temps' registers there */
    uint32_t     n_stubs, cap_stubs;
    /* deferred register writes (jit_defer_analyze; docs/jit-dead-puts-design.md) */
    bool         defer;      /* on unless UAM_JIT_NODEFER */
    uint8_t     *okind;      /* per insn: what it is to the analysis (OK_*) */
    uint8_t     *dfr;        /* per insn: a PUT done only on the cold paths */
    uint32_t    *obs_start;  /* per insn + 1: the writes to replay at insn i (a soft point) */
    uint32_t    *obs_list;   /*   are obs_list[obs_start[i] .. obs_start[i + 1]), in order */
    uint32_t    *pend;       /* scratch: pending writes and their live bytes */
    uint8_t     *pend_live;
    uint8_t     *kbytes;     /* scratch: register file bytes overwritten later (backward) */
    uint8_t     *cold;       /* per insn: a pure op only deferred writes need (not emitted) */
    uint8_t     *forced;     /* per insn: kept hot (its replays would be too long) */
    uint32_t    *clo_start;  /* per insn + 1: the cold ops soft point i recomputes are */
    uint32_t    *clo_list;   /*   clo_list[clo_start[i] .. clo_start[i + 1]), in order */
    uint32_t    *stamp;      /* scratch per insn */
    uint32_t    *def_of;     /* per temp: the insn defining it */
    uint32_t    *bad;        /* scratch per temp: reads by insns that are emitted */
    uint32_t    *nread;      /* scratch per temp: all reads */
    uint32_t     cap_ins, cap_obs, cap_kb, cap_clo, cap_tmp;
    jit_snap    *snaps;      /* the IMARK stubs' snapshots */
    uint32_t     n_snaps, cap_snaps;
    uam_ref     *rd;         /* scratch: one insn's reads (jit_insn_reads) */
    uint32_t     cap_rd;
    /* register allocation, per temp of the block being compiled */
    int8_t      *reg_of;     /* host register holding it, or -1 */
    uint8_t     *in_mem;     /* its jc->temps slot holds its value */
    uint32_t    *last_use;   /* last insn that reads it, or NO_TEMP */
    uint32_t    *use_start;  /* its reads are use_pos[use_start[t] .. use_start[t + 1]) */
    uint32_t    *use_cur;    /* first of them not yet passed */
    uint32_t    *use_pos;
    uint32_t     cap_temp, cap_use;
    uint32_t     holder[MAX_HOST_REGS]; /* temp held by each host register, or NO_TEMP */
    uint32_t     cur_i;      /* the insn being compiled */
    /* flag fusion: a compare whose only reader is the next guard or select condition
     * leaves its result in the host flags instead of a register */
    bool         fuse;       /* the insn being compiled is such a compare */
    uam_ref      flag_temp;  /* the temp now held in the flags, or NONE */
    uint8_t      flag_cc;    /* its host condition code (true when the cc holds) */
    /* profiling output, from the environment at creation (docs/jit-perf.md) */
    FILE        *perf_map;   /* UAM_PERF_MAP: /tmp/perf-<pid>.map, one line per block */
    FILE        *opmap;      /* UAM_JIT_OPMAP=FILE: code offset -> IR instruction */
    jit_opmark  *marks;      /* per-compile, only with opmap */
    uint32_t     n_marks, cap_marks;
};

#define OFF(f)  ((int32_t)offsetof(uam_jit_ctx, f))
#define RES(f)  ((int32_t)(offsetof(uam_jit_ctx, res) + offsetof(uam_block_result, f)))
#define CPU(f)  ((int32_t)offsetof(uam_cpu, f))
#define REXC(f) ((int32_t)(offsetof(uam_jit_ctx, res) + offsetof(uam_block_result, exc) + offsetof(uam_exception, f)))
#define SLOT(f) ((int32_t)offsetof(jit_slot, f))
#define COV(f)  ((int32_t)offsetof(uam_cov_state, f))

/* ---- what jit.c provides to a code generator ----------------------------------- */

/* Note that the code from here on belongs to `kind` (of IR instruction `idx`). */
void     jit_mark(uam_jit *j, unsigned kind, uint32_t idx);
/* Make room for `need` elements of `size` bytes in *p; on failure the block is not
 * compiled (j->overflow). */
bool     jit_grow(uam_jit *j, void *p, uint32_t *cap, uint32_t need, size_t size);
/* A constant access the inline path can never take: it crosses a page, or it is
 * misaligned and must fault (ACC_ALIGNED). */
bool     jit_const_slow(uint64_t addr, unsigned size, unsigned flags);
/* The insn's guard is a temp (not none, not an immediate). */
bool     jit_guard_temp(const uam_insn *in);

/* Per block, before emitting: decide which PUTs are deferred (j->dfr), what each soft
 * point replays (j->obs_start / obs_list) and which pure ops only those writes need
 * (j->cold, j->clo_start / clo_list); then the register allocator's use lists. Both
 * return false on allocation failure. */
bool     jit_defer_analyze(uam_jit *j, const uam_block *blk);
bool     jit_ra_begin(uam_jit *j, const uam_block *blk);
/* The temps insn i reads where it is emitted (its own operands, and those of the cold
 * ops and writes its replay needs), into j->rd; returns how many. */
unsigned jit_insn_reads(const uam_jit *j, const uam_block *blk, uint32_t i);
/* After an insn: free the registers of the temps it read for the last time. */
void     jit_ra_free_dead(uam_jit *j, const uam_block *blk);
/* Before the code of an insn that defines temp dst (and before its guard's branch: this
 * may emit a spill): the register its result is to be left in. That is want0 or want1 (a
 * register of the pool, or -1) if it is free or holds a temp the insn reads for the last
 * time, else a free register, else one of a temp read here for the last time - so the
 * result's register may be an operand's, and the code must write it only after its last
 * read of that operand - else that of the temp read furthest in the future, spilled now.
 * j->res_reg if the temp is never read or is to stay in its slot. */
int      jit_ra_dst(uam_jit *j, uam_ref dst, int want0, int want1);
/* After the insn's code and jit_ra_free_dead: temp dst is in `reg`, as jit_ra_dst said
 * (j->res_reg: store it to its slot). */
void     jit_ra_def(uam_jit *j, uam_ref dst, int reg);
/* Around a call: store / reload the call-clobbered registers whose temps are still read
 * after the current insn. */
void     jit_ra_save(uam_jit *j, bool restore);
/* Can insn i, a compare, leave its result in the host flags? Only if its one reader is
 * the guard or the select condition of the next insn (past a continuation IMARK, whose
 * icount increment must then leave the flags alone). */
bool     jit_fusable(const uam_jit *j, const uam_block *blk, uint32_t i);
/* The current instruction's site record (one per instruction, emitted after the code),
 * and a reference to it at code offset `pos` to patch. Returns false on failure. */
bool     jit_site_ref(uam_jit *j, size_t pos);
/* Record an IMARK stop stub: the branch at `pos` leaves for it, at insn i of pc `pc`. */
void     jit_stub_add(uam_jit *j, const uam_block *blk, size_t pos, uint64_t pc, uint32_t i);
/* Around a stub's replay: make reg_of show the temps' registers at the stub's IMARK
 * (enter), then restore the current allocation (leave). */
void     jit_stub_regs(uam_jit *j, uint32_t k, bool enter);

/* Note that the code at offset `pos` refers, pc-relative, to the field at offset `off` of
 * the newest link slot (the slots follow the code: the code generator patches these). */
static inline void jit_slot_ref(uam_jit *j, size_t pos, unsigned off)
{
    j->slot_ref[j->n_slot_refs].pos = pos;
    j->slot_ref[j->n_slot_refs].slot = (uint16_t)(j->n_slots - 1);
    j->slot_ref[j->n_slot_refs].off = (uint16_t)off;
    j->n_slot_refs++;
}

/* ---- what a code generator provides (jit_x86.c, jit_a64.c) --------------------- */

/* Host features, the temp register pool and res_reg; false if it cannot run here. */
bool     jit_host_init(uam_jit *j);
/* The code-page store stubs, into j->cb, with each one's offset in off[] (placed at the
 * start of the arena; native code finds them in jc->cstub). */
void     jit_host_stubs(uam_jit *j, size_t off[16]);
/* Compile the whole block into j->cb. Returns false if it must be interpreted. */
bool     jit_host_compile(uam_jit *j, const uam_block *blk);
/* Pure opcodes the code generator emits inline; any other is a call to jit_br_eval. */
bool     jit_host_pure_inline(const uam_jit *j, uam_opcode op);
/* Point a constant EXIT's jump instruction, at `site` in the arena, to `target` (a
 * block's body), or with target NULL back to the miss path that follows it. jit.c makes
 * the arena writable around it and syncs the instruction cache after. */
void     jit_host_link(uint8_t *site, const uint8_t *target);
/* For the register allocator: jc->temps[t] = reg; reg = jc->temps[t]. */
void     jit_host_spill(uam_jit *j, uint32_t t, int reg);
void     jit_host_fill(uam_jit *j, uint32_t t, int reg);

#endif /* UAM_JIT_IMPL_H */
