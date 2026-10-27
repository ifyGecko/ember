/*
 * jit_x86.c - Native x86-64 backend for UAM-IR (jit.h).
 *
 * Compiles one verified, optimized block to a host function
 *
 *     void code(uam_jit_ctx *jc);
 *
 * that leaves cpu/temps in exactly the state uam_interp_block would and writes the
 * block's outcome into jc->res. The hot opcodes are emitted inline; rare pure ops go
 * through the same C evaluator as the interpreter (ir_eval.h), and every memory access
 * the inline data-TLB path cannot do goes through execops.h, so the backends cannot
 * disagree. Every IR op compiles; only a block whose code overflows the scratch buffer
 * runs interpreted (uam_jit_compile sets UAM_JIT_INTERP).
 *
 * Host register use (System V AMD64, all callee-saved so they survive bridge calls):
 *   rbx = jc    rbp = jc->temps    r14 = jc->cpu    r15 = jc->cpu->regfile
 *   r13 = icount (flushed to cpu->icount before bridges and at the epilogue)
 *   r12 = unused (saved; free for values)
 * Every IMARK that may stop compares r13 with cpu->icount_stop (exit_request ? 0 :
 * icount_deadline, set on entry and zeroed by uam_cpu_request_exit) and leaves through
 * an out-of-line stub, so the common path is one compare and an untaken branch.
 * rax/rcx/rdx/rsi/rdi/r8 are scratch. Guest temporaries live in jc->temps[], 64-bit,
 * masked to their width (the interpreter's invariant). Immediates are materialized.
 *
 * Layout of a compiled block: prologue | body | epilogue | link slots. Every block
 * keeps the same frame and register assignment, so a chained jump (jit.h) goes from an
 * EXIT straight to the next block's body, skipping its prologue; the last block's
 * epilogue returns to the run loop.
 *
 * This file builds only on x86-64; elsewhere uam_jit_create returns NULL and the
 * executor uses the interpreter.
 */
#include "jit.h"

#if !defined(__x86_64__)

uam_jit *uam_jit_create(uam_exec *x) { (void)x; return NULL; }
void     uam_jit_destroy(uam_jit *j) { (void)j; }
bool     uam_jit_compile(uam_jit *j, uam_block *blk) { (void)j; (void)blk; blk->backend = UAM_JIT_INTERP; return true; }
void     uam_jit_run(uam_jit *j, const uam_block *blk, uint64_t *t, uam_jit_chain *c,
                     uam_block_result *r)
{ (void)j; (void)blk; (void)t; (void)c; (void)r; }

#else

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#include "uam/bus.h"

#include "../ir/ir_eval.h"
#include "execops.h"

/* ---- runtime context and C bridges ------------------------------------ */

/* A chaining link, one per EXIT, stored after the block's code. */
typedef struct jit_slot {
    uint64_t entry;   /* host address of the target block's body (valid if epoch matches) */
    uint64_t epoch;   /* jc.epoch when linked; LINK_NONE when empty */
    uint64_t pc;      /* guest pc of the target */
} jit_slot;

#define LINK_NONE UINT64_MAX

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
    /* chaining */
    uint64_t                epoch;       /* links made in another epoch are dead */
    jit_slot               *link_slot;   /* out: the EXIT slot that missed (to be linked) */
    uint32_t                chain_left;  /* chained jumps still allowed */
    uint32_t                hops;        /* chained jumps taken */
    uint32_t                dgen;        /* bus->direct_gen at entry */
    uam_sched              *sched;
    uint64_t                next_when;   /* next timer at entry */
    uam_exec_stats         *stats;       /* slow-path counters (jit_*_calls) */
} uam_jit_ctx;

/* After a bridge: if the bus mapping or the timer queue changed, the run loop has work
 * to do (data-TLB flush, a new deadline) before the next block, so stop chaining. A
 * timer armed earlier usually requests an exit too (the machine's scheduler kick). */
static void chain_check(uam_jit_ctx *jc)
{
    if (jc->cpu->bus->direct_gen != jc->dgen || uam_sched_next(jc->sched) != jc->next_when)
        jc->chain_left = 0;
}

static int jit_fault(uam_jit_ctx *jc, uam_status st, uint64_t addr, uint32_t xf, bool store)
{
    uam_block_result *r = &jc->res;
    r->kind = UAM_BR_FAULT;
    r->pc = jc->site->pc;
    r->exc.kind = uam_execops_fault_kind(st, store);
    r->exc.flags = jc->site->cont ? UAM_EXC_F_CONT : 0;
    r->exc.pc = jc->site->pc;
    r->exc.parent_pc = jc->site->parent_pc;
    r->exc.cause = xf;
    r->exc.tval = addr;
    return 1;
}

/*
 * After a successful access, cache the page in the data-TLB so later accesses to it
 * take the inline fast path. Only RAM (both directions) and ROM (reads) are cached:
 * their host bytes are stable for the current generation, and the whole page must be
 * owned by one region, unwatched, and - for writes - not holding translated code
 * (uam_bus_page_host enforces the last three). Device and ROMD pages are never cached.
 */
static void dtlb_fill(uam_cpu *cpu, uint64_t addr, uint32_t flags, unsigned size, bool write)
{
    if (flags & (UAM_ACC_PHYS | UAM_ACC_UNPRIV))
        return;   /* fast path only models the ordinary translation regime */
    uint64_t pa;
    uint32_t xf = 0;
    if (uam_execops_xlate(cpu, addr, write ? UAM_XLATE_STORE : UAM_XLATE_LOAD, flags & ~UAM_ACC_ALIGNED,
                          size, &pa, &xf) != UAM_OK)
        return;
    const uam_region *r = uam_bus_find(cpu->bus, pa);
    if (!r || (r->kind != UAM_REGION_RAM && !(r->kind == UAM_REGION_ROM && !write)))
        return;
    uint8_t *h = uam_bus_page_host(cpu->bus, pa, write);
    if (!h)
        return;
    uint64_t vpage = addr >> UAM_PAGE_SHIFT;
    unsigned idx = (unsigned)vpage & (UAM_DTLB_SIZE - 1);
    uint64_t base = (uint64_t)(uintptr_t)h - (vpage << UAM_PAGE_SHIFT);
    if (write) { cpu->dtlb_wt[idx] = vpage; cpu->dtlb_wb[idx] = base; }
    else       { cpu->dtlb_rt[idx] = vpage; cpu->dtlb_rb[idx] = base; }
}

/* Returns 1 on fault (jc->res filled), 0 on success (jc->scratch = loaded value). */
static int jit_br_load(uam_jit_ctx *jc, uint64_t addr, uint32_t flags, uint32_t size)
{
    uint32_t xf = 0;
    uint64_t fva;
    uam_status st = uam_execops_load(jc->cpu, addr, flags, size, &jc->scratch, &xf, &fva);
    jc->stats->jit_load_calls++;
    chain_check(jc);
    if (st != UAM_OK)
        return jit_fault(jc, st, fva, xf, false);
    if (!(flags & UAM_ACC_X))
        dtlb_fill(jc->cpu, addr, flags, size, false);
    return 0;
}

static int jit_br_store(uam_jit_ctx *jc, uint64_t addr, uint64_t val, uint32_t flags, uint32_t size)
{
    uint32_t xf = 0;
    uint64_t fva;
    uam_status st = uam_execops_store(jc->cpu, addr, val, flags, size, &xf, &fva);
    jc->stats->jit_store_calls++;
    chain_check(jc);
    if (st != UAM_OK)
        return jit_fault(jc, st, fva, xf, true);
    dtlb_fill(jc->cpu, addr, flags, size, true);
    return 0;
}

/* Store-conditional: jc->scratch = 1 if the store was performed, 0 if not. */
static int jit_br_store_x(uam_jit_ctx *jc, uint64_t addr, uint64_t val, uint32_t flags, uint32_t size)
{
    uint32_t xf = 0;
    bool ok = false;
    uint64_t fva;
    uam_status st = uam_execops_store_x(jc->cpu, addr, val, flags, size, &ok, &xf, &fva);
    chain_check(jc);
    if (st != UAM_OK)
        return jit_fault(jc, st, fva, xf, true);
    jc->scratch = ok;
    return 0;
}

/* AMO / CAS: jc->scratch = the old memory value. packed = op | sub << 8 | width << 16. */
static int jit_br_rmw(uam_jit_ctx *jc, uint64_t addr, uint64_t b, uint64_t c, uint32_t flags,
                      uint32_t packed)
{
    uint32_t xf = 0;
    const bool cas = (packed & 0xff) == UAM_OP_CAS;
    uam_status st = uam_execops_rmw(jc->cpu, cas, (uam_amo_op)((packed >> 8) & 0xff), addr, b, c,
                                    flags, (packed >> 16) & 0xff, &jc->scratch, &xf);
    chain_check(jc);
    return st == UAM_OK ? 0 : jit_fault(jc, st, addr, xf, true);
}

static int jit_fault_simple(uam_jit_ctx *jc, uam_exc_kind kind, uint64_t tval)
{
    uam_block_result *r = &jc->res;
    r->kind = UAM_BR_FAULT;
    r->pc = jc->site->pc;
    r->exc.kind = kind;
    r->exc.flags = jc->site->cont ? UAM_EXC_F_CONT : 0;
    r->exc.pc = jc->site->pc;
    r->exc.parent_pc = jc->site->parent_pc;
    r->exc.cause = 0;
    r->exc.tval = tval;
    return 1;
}

static int jit_br_sysrd(uam_jit_ctx *jc, uint32_t id)
{
    uint64_t v = 0;
    uam_status st = jc->cpu->isa->sysreg_read(jc->cpu, id, &v);
    chain_check(jc);
    if (st != UAM_OK)
        return jit_fault_simple(jc, UAM_EXC_SYSREG, id);
    jc->scratch = v;
    return 0;
}

static int jit_br_syswr(uam_jit_ctx *jc, uint32_t id, uint64_t val)
{
    uam_status st = jc->cpu->isa->sysreg_write(jc->cpu, id, val);
    chain_check(jc);
    return st == UAM_OK ? 0 : jit_fault_simple(jc, UAM_EXC_SYSREG, id);
}

/* HELPER: the ISA's C function, as the interpreter calls it (arguments and
 * result masked to their declared widths). Returns 1 on fault. */
static int jit_br_helper(uam_jit_ctx *jc, uint32_t id, uint64_t a, uint64_t b, uint64_t c)
{
    const uam_helper_desc *hd = &jc->helpers->desc[id];
    const uint64_t args[3] = { a, b, c };
    uint64_t v[3] = { 0, 0, 0 };
    for (unsigned k = 0; k < hd->n_args; k++)
        v[k] = args[k] & uam_mask(hd->arg_width[k]);
    uam_status st = UAM_OK;
    uint64_t r = hd->fn(jc->cpu, v[0], v[1], v[2], &st);
    chain_check(jc);
    if (st != UAM_OK)
        return jit_fault_simple(jc, UAM_EXC_HELPER, id);
    jc->scratch = hd->result_width ? r & uam_mask(hd->result_width) : 0;
    return 0;
}

/* packed = op | width << 8 | src_w << 16 */
static uint64_t jit_br_eval(uam_jit_ctx *jc, uint32_t packed, uint64_t x, uint64_t y, uint64_t z)
{
    uint64_t out = 0;
    jc->stats->jit_eval_calls++;
    uam_ir_eval_pure((uam_opcode)(packed & 0xff), (packed >> 8) & 0xff, x, y, z,
                     (packed >> 16) & 0xff, &out);
    return out;
}

/* ---- the compiler ------------------------------------------------------ */

#define ARENA_SIZE (64u << 20)
#define CBUF_MAX   (64u << 10)   /* scratch code buffer per block */
#define MAX_SLOTS  64u           /* EXITs per block that get a link (more just return) */

/* Code-region kinds recorded for the op map (UAM_JIT_OPMAP; docs/jit-perf.md). */
enum { MK_PROLOGUE, MK_OP, MK_IMARK_PC, MK_SLOW, MK_CHAIN, MK_STUB, MK_EPILOGUE, MK_SLOTS };
static const char *const mark_name[] = { "prologue", "op", "imark-pc", "slow", "chain", "stub",
                                         "epilogue", "slots" };
typedef struct jit_mark { uint32_t off, idx; uint8_t kind; } jit_mark;

/* Host registers. */
enum { RAX = 0, RCX = 1, RDX = 2, RBX = 3, RSP = 4, RBP = 5, RSI = 6, RDI = 7, R8 = 8, R9 = 9,
       R12 = 12, R13 = 13, R14 = 14, R15 = 15 };

struct uam_jit {
    uam_exec    *x;
    uint8_t     *arena;
    size_t       arena_used;
    uam_jit_ctx  jc;         /* reused across block runs */
    bool         popcnt;     /* the host has POPCNT (else that op uses the C evaluator) */
    size_t       body_off;   /* prologue length: chained jumps enter here */
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
    struct { size_t lea; uint64_t pc; } slot[MAX_SLOTS];  /* lea rel32 position, target */
    unsigned     n_slots;
    jit_site     cur;        /* the guest instruction being compiled */
    bool         cur_emitted;/* cur has a site record (sites[n_sites - 1]) */
    jit_site    *sites;      /* site records, emitted after the slots */
    uint32_t     n_sites, cap_sites;
    struct { size_t lea; uint32_t site; } *site_refs;   /* lea rel32 positions to patch */
    uint32_t     n_site_refs, cap_site_refs;
    struct { size_t jcc; uint64_t pc; } *stubs;        /* out-of-line IMARK stops */
    uint32_t     n_stubs, cap_stubs;
    /* profiling output, from the environment at creation (docs/jit-perf.md) */
    FILE        *perf_map;   /* UAM_PERF_MAP: /tmp/perf-<pid>.map, one line per block */
    FILE        *opmap;      /* UAM_JIT_OPMAP=FILE: code offset -> IR instruction */
    jit_mark    *marks;      /* per-compile, only with opmap */
    uint32_t     n_marks, cap_marks;
};

#define OFF(f)  ((int32_t)offsetof(uam_jit_ctx, f))
#define RES(f)  ((int32_t)(offsetof(uam_jit_ctx, res) + offsetof(uam_block_result, f)))
#define CPU(f)  ((int32_t)offsetof(uam_cpu, f))
#define REXC(f) ((int32_t)(offsetof(uam_jit_ctx, res) + offsetof(uam_block_result, exc) + offsetof(uam_exception, f)))
#define SLOT(f) ((int32_t)offsetof(jit_slot, f))
#define COV(f)  ((int32_t)offsetof(uam_cov_state, f))

/* Note that the code from here on belongs to `kind` (of IR instruction `idx`). */
static void mark(uam_jit *j, unsigned kind, uint32_t idx)
{
    if (!j->opmap)
        return;
    if (j->n_marks == j->cap_marks) {
        uint32_t cap = j->cap_marks ? 2 * j->cap_marks : 256;
        jit_mark *m = realloc(j->marks, sizeof(*m) * cap);
        if (!m)
            return;
        j->marks = m;
        j->cap_marks = cap;
    }
    j->marks[j->n_marks++] = (jit_mark){ (uint32_t)j->n, idx, (uint8_t)kind };
}

static void g8(uam_jit *j, uint8_t b)
{
    if (j->n < CBUF_MAX)
        j->cb[j->n] = b;
    else
        j->overflow = true;
    j->n++;
}
static void g32(uam_jit *j, uint32_t v) { for (int i = 0; i < 4; i++) g8(j, (uint8_t)(v >> (8 * i))); }
static void g64(uam_jit *j, uint64_t v) { for (int i = 0; i < 8; i++) g8(j, (uint8_t)(v >> (8 * i))); }

static void rex(uam_jit *j, int w, int r, int x, int b)
{
    if (w || r || x || b)
        g8(j, (uint8_t)(0x40 | (w << 3) | (r << 2) | (x << 1) | b));
}
static void modrm_reg(uam_jit *j, int reg, int rm) { g8(j, (uint8_t)(0xC0 | ((reg & 7) << 3) | (rm & 7))); }

static void modrm_mem(uam_jit *j, int reg, int base, int32_t disp)
{
    int b = base & 7;
    int mod = (disp == 0 && b != 5) ? 0 : (disp >= -128 && disp <= 127) ? 1 : 2;
    int sib = (b == 4);
    g8(j, (uint8_t)((mod << 6) | ((reg & 7) << 3) | (sib ? 4 : b)));
    if (sib)
        g8(j, (uint8_t)(0x20 | b));         /* scale 0, index none, base b */
    if (mod == 1)
        g8(j, (uint8_t)(int8_t)disp);
    else if (mod == 2)
        g32(j, (uint32_t)disp);
}

/* [base + index<<scale + disp]; caller emits REX (r=reg>=8, x=index>=8, b=base>=8). */
static void modrm_sib(uam_jit *j, int reg, int base, int index, int scale, int32_t disp)
{
    int b = base & 7;
    int mod = (disp == 0 && b != 5) ? 0 : (disp >= -128 && disp <= 127) ? 1 : 2;
    g8(j, (uint8_t)((mod << 6) | ((reg & 7) << 3) | 4));
    g8(j, (uint8_t)((scale << 6) | ((index & 7) << 3) | b));
    if (mod == 1)
        g8(j, (uint8_t)(int8_t)disp);
    else if (mod == 2)
        g32(j, (uint32_t)disp);
}

/* mov reg64, [base + disp] */
static void ld_mem(uam_jit *j, int reg, int base, int32_t disp)
{
    rex(j, 1, reg >= 8, 0, base >= 8);
    g8(j, 0x8B);
    modrm_mem(j, reg, base, disp);
}
/* mov [base + disp], reg64 */
static void st_mem(uam_jit *j, int reg, int base, int32_t disp)
{
    rex(j, 1, reg >= 8, 0, base >= 8);
    g8(j, 0x89);
    modrm_mem(j, reg, base, disp);
}
static void movabs(uam_jit *j, int reg, uint64_t imm)
{
    rex(j, 1, 0, 0, reg >= 8);
    g8(j, (uint8_t)(0xB8 + (reg & 7)));
    g64(j, imm);
}
static void mov_rr(uam_jit *j, int dst, int src)   /* mov dst64, src64 */
{
    rex(j, 1, src >= 8, 0, dst >= 8);
    g8(j, 0x89);
    modrm_reg(j, src, dst);
}
static void st_temp(uam_jit *j, uint32_t idx, int reg) { st_mem(j, reg, RBP, (int32_t)(8 * idx)); }

/* ---- operands ------------------------------------------------------------------- */

/* Where an IR operand lives: an immediate, or a temp's slot in jc->temps. */
enum { O_IMM, O_MEM };
typedef struct opnd { int kind; uint64_t imm; int32_t disp; } opnd;

static opnd opnd_of(const uam_block *blk, uam_ref r, unsigned w)
{
    if (uam_ref_is_imm(r))
        return (opnd){ O_IMM, blk->imm[uam_ref_index(r)] & uam_mask(w), 0 };
    return (opnd){ O_MEM, 0, (int32_t)(8 * r) };
}

static bool fits_s32(uint64_t v) { return (int64_t)v == (int32_t)v; }
static bool fits_s8(int64_t v) { return v >= -128 && v <= 127; }

/* reg = v without touching the host flags (no xor-zeroing): the shortest of
 * mov r32,imm32 (zero-extends), mov r64,simm32 and movabs. */
static void mov_imm(uam_jit *j, int reg, uint64_t v)
{
    if (v <= 0xFFFFFFFFu) {
        rex(j, 0, 0, 0, reg >= 8);
        g8(j, (uint8_t)(0xB8 + (reg & 7)));
        g32(j, (uint32_t)v);
    } else if (fits_s32(v)) {
        rex(j, 1, 0, 0, reg >= 8);
        g8(j, 0xC7); modrm_reg(j, 0, reg);
        g32(j, (uint32_t)v);
    } else {
        movabs(j, reg, v);
    }
}

/* reg = the operand, a value of width w (temps are kept zero-extended, so a 32-bit
 * load of a narrow one is enough). */
static void load(uam_jit *j, int reg, opnd o, unsigned w)
{
    if (o.kind == O_IMM) {
        mov_imm(j, reg, o.imm);
    } else if (w <= 32) {
        rex(j, 0, reg >= 8, 0, 0);
        g8(j, 0x8B); modrm_mem(j, reg, RBP, o.disp);           /* mov r32, [rbp+d] */
    } else {
        ld_mem(j, reg, RBP, o.disp);
    }
}

static void load_operand(uam_jit *j, int reg, const uam_block *blk, uam_ref r, unsigned w)
{
    load(j, reg, opnd_of(blk, r, w), w);
}

/* Prefixes for an instruction at operand size os (8/16/32/64) with ModRM reg `r` and
 * rm `rm` (a register if rm_reg, else a memory base). Byte access to rsp..rdi as
 * registers needs a REX prefix. */
static void opsize(uam_jit *j, unsigned os, int r, int rm, bool rm_reg)
{
    if (os == 16)
        g8(j, 0x66);
    int w = os == 64, R = r >= 8, B = rm >= 8;
    bool byte_rex = os == 8 && ((r >= 4 && r < 8) || (rm_reg && rm >= 4 && rm < 8));
    if (w || R || B || byte_rex)
        g8(j, (uint8_t)(0x40 | (w << 3) | (R << 2) | B));
}

/* x86 group-1 ALU operations (the /digit of opcodes 0x80..0x83). */
enum { ALU_ADD = 0, ALU_OR = 1, ALU_AND = 4, ALU_SUB = 5, ALU_XOR = 6, ALU_CMP = 7 };

static void alu_rr(uam_jit *j, int op, unsigned os, int dst, int src)
{
    opsize(j, os, src, dst, true);
    g8(j, (uint8_t)((op << 3) | (os == 8 ? 0 : 1)));        /* op r/m, r */
    modrm_reg(j, src, dst);
}

/* reg = reg OP operand, at operand size os. */
static void alu(uam_jit *j, int op, unsigned os, int reg, opnd o)
{
    if (o.kind == O_MEM) {
        opsize(j, os, reg, RBP, false);
        g8(j, (uint8_t)((op << 3) | (os == 8 ? 2 : 3)));    /* op r, [rbp+d] */
        modrm_mem(j, reg, RBP, o.disp);
        return;
    }
    if (os == 64 && !fits_s32(o.imm)) {
        int s = reg == RCX ? RDX : RCX;
        movabs(j, s, o.imm);
        alu_rr(j, op, os, reg, s);
        return;
    }
    opsize(j, os, 0, reg, true);
    if (os == 8) {
        g8(j, 0x80); modrm_reg(j, op, reg); g8(j, (uint8_t)o.imm);
        return;
    }
    int64_t sv = uam_sext(o.imm, os);
    if (fits_s8(sv)) {
        g8(j, 0x83); modrm_reg(j, op, reg); g8(j, (uint8_t)sv);
    } else {
        g8(j, 0x81); modrm_reg(j, op, reg);
        if (os == 16) { g8(j, (uint8_t)o.imm); g8(j, (uint8_t)(o.imm >> 8)); }
        else g32(j, (uint32_t)o.imm);
    }
}

/* reg = reg * operand (imul), at operand size 32 or 64. */
static void imul(uam_jit *j, unsigned os, int reg, opnd o)
{
    if (o.kind == O_MEM) {
        opsize(j, os, reg, RBP, false);
        g8(j, 0x0F); g8(j, 0xAF); modrm_mem(j, reg, RBP, o.disp);
    } else if (os == 64 && !fits_s32(o.imm)) {
        movabs(j, RCX, o.imm);
        opsize(j, os, reg, RCX, true);
        g8(j, 0x0F); g8(j, 0xAF); modrm_reg(j, reg, RCX);
    } else {
        int64_t sv = uam_sext(o.imm, os);
        opsize(j, os, reg, reg, true);
        if (fits_s8(sv)) { g8(j, 0x6B); modrm_reg(j, reg, reg); g8(j, (uint8_t)sv); }
        else { g8(j, 0x69); modrm_reg(j, reg, reg); g32(j, (uint32_t)o.imm); }
    }
}

/* Shift group (C1 /digit ib) by an immediate, at operand size os. */
enum { SH_ROR = 1, SH_SHL = 4, SH_SHR = 5, SH_SAR = 7 };
static void shift_imm(uam_jit *j, int op, unsigned os, int reg, unsigned n)
{
    opsize(j, os, 0, reg, true);
    g8(j, os == 8 ? 0xC0 : 0xC1); modrm_reg(j, op, reg); g8(j, (uint8_t)n);
}

/* Zero-extend the low `w` bits of rax to 64 bits (mask to width). */
static void mask_rax(uam_jit *j, unsigned w)
{
    switch (w) {
    case 8:  g8(j, 0x0F); g8(j, 0xB6); modrm_reg(j, RAX, RAX); break; /* movzx eax,al */
    case 16: g8(j, 0x0F); g8(j, 0xB7); modrm_reg(j, RAX, RAX); break; /* movzx eax,ax */
    case 32: g8(j, 0x89); modrm_reg(j, RAX, RAX); break;              /* mov eax,eax */
    case 1:  g8(j, 0x83); modrm_reg(j, 4, RAX); g8(j, 1); break;      /* and eax,1 */
    default: break;                                                  /* 64: no-op */
    }
}
/* Sign-extend the low `w` bits of `reg` (RAX or RCX) to 64 bits. A 1-bit value is
 * 0 or 1 (temps are masked to their width), so negating it sign-extends it. */
static void sext_reg(uam_jit *j, int reg, unsigned w)
{
    switch (w) {
    case 1:  rex(j, 1, 0, 0, 0); g8(j, 0xF7); modrm_reg(j, 3, reg); break;              /* neg */
    case 8:  rex(j, 1, 0, 0, 0); g8(j, 0x0F); g8(j, 0xBE); modrm_reg(j, reg, reg); break; /* movsx */
    case 16: rex(j, 1, 0, 0, 0); g8(j, 0x0F); g8(j, 0xBF); modrm_reg(j, reg, reg); break;
    case 32: rex(j, 1, 0, 0, 0); g8(j, 0x63); modrm_reg(j, reg, reg); break;              /* movsxd */
    default: break;
    }
}

static size_t jmp_fwd(uam_jit *j);
static size_t jcc_fwd(uam_jit *j, uint8_t cc);
static void   patch(uam_jit *j, size_t pos);

/* rax = [rcx + rsi], `size` bytes, zero-extended; byte-swapped if big-endian. */
static void emit_fast_load(uam_jit *j, unsigned size, bool be)
{
    switch (size) {
    case 1: g8(j, 0x0F); g8(j, 0xB6); modrm_sib(j, RAX, RCX, RSI, 0, 0); break;      /* movzx eax,byte */
    case 2:
        g8(j, 0x0F); g8(j, 0xB7); modrm_sib(j, RAX, RCX, RSI, 0, 0);                 /* movzx eax,word */
        if (be) { g8(j, 0x66); g8(j, 0xC1); modrm_reg(j, 0, RAX); g8(j, 8); }        /* rol ax,8 */
        break;
    case 4:
        g8(j, 0x8B); modrm_sib(j, RAX, RCX, RSI, 0, 0);                              /* mov eax,[..] */
        if (be) { g8(j, 0x0F); g8(j, 0xC8); }                                        /* bswap eax */
        break;
    default:
        rex(j, 1, 0, 0, 0); g8(j, 0x8B); modrm_sib(j, RAX, RCX, RSI, 0, 0);          /* mov rax,[..] */
        if (be) { rex(j, 1, 0, 0, 0); g8(j, 0x0F); g8(j, 0xC8); }                    /* bswap rax */
        break;
    }
}

/* [rcx + rsi] = rdx, `size` bytes; rdx is byte-swapped first if big-endian. */
static void emit_fast_store(uam_jit *j, unsigned size, bool be)
{
    switch (size) {
    case 1: g8(j, 0x88); modrm_sib(j, RDX, RCX, RSI, 0, 0); break;                   /* mov [..],dl */
    case 2:
        if (be) { g8(j, 0x66); g8(j, 0xC1); modrm_reg(j, 0, RDX); g8(j, 8); }        /* rol dx,8 */
        g8(j, 0x66); g8(j, 0x89); modrm_sib(j, RDX, RCX, RSI, 0, 0);                 /* mov [..],dx */
        break;
    case 4:
        if (be) { g8(j, 0x0F); g8(j, 0xCA); }                                        /* bswap edx */
        g8(j, 0x89); modrm_sib(j, RDX, RCX, RSI, 0, 0);                              /* mov [..],edx */
        break;
    default:
        if (be) { rex(j, 1, 0, 0, 0); g8(j, 0x0F); g8(j, 0xCA); }                    /* bswap rdx */
        rex(j, 1, 0, 0, 0); g8(j, 0x89); modrm_sib(j, RDX, RCX, RSI, 0, 0);          /* mov [..],rdx */
        break;
    }
}

/* Emit the data-TLB probe: rsi = addr. On a hit, rcx = host base (data at [rcx+rsi]) and
 * control falls through; on a miss, jumps are taken to be patched to the bridge path.
 * `tag_off`/`base_off` select the read or write set. Returns miss-jump positions in
 * miss[] (up to 3) and their count in *n_miss. */
static void emit_dtlb_probe(uam_jit *j, unsigned size, bool aligned, int32_t tag_off,
                            int32_t base_off, size_t *miss, unsigned *n_miss)
{
    /* Uses only rax and rcx as scratch; rsi (addr) and rdx (store value) are preserved.
     * rcx ends holding the host base, so the value is at [rcx + rsi]. */
    *n_miss = 0;
    mov_rr(j, RAX, RSI);                                   /* rax = addr */
    rex(j, 1, 0, 0, 0); g8(j, 0xC1); modrm_reg(j, 5, RAX); g8(j, UAM_PAGE_SHIFT); /* shr rax,12 (vpage) */
    g8(j, 0x89); modrm_reg(j, RAX, RCX);                   /* mov ecx, eax */
    g8(j, 0x81); modrm_reg(j, 4, RCX); g32(j, UAM_DTLB_SIZE - 1);  /* and ecx, SIZE-1 (index) */
    rex(j, 1, 0, 0, 1); g8(j, 0x3B); modrm_sib(j, RAX, R14, RCX, 3, tag_off);  /* cmp rax,[r14+rcx*8+tag] */
    miss[(*n_miss)++] = jcc_fwd(j, 0x85);                  /* jne miss */
    if (size > 1) {
        g8(j, 0x89); modrm_reg(j, RSI, RAX);               /* mov eax, esi */
        g8(j, 0x25); g32(j, UAM_PAGE_SIZE - 1);            /* and eax, 0xfff */
        g8(j, 0x3D); g32(j, UAM_PAGE_SIZE - size);         /* cmp eax, 0x1000-size */
        miss[(*n_miss)++] = jcc_fwd(j, 0x87);              /* ja miss (page cross) */
        if (aligned) {
            g8(j, 0xF7); modrm_reg(j, 0, RSI); g32(j, size - 1);       /* test esi, size-1 */
            miss[(*n_miss)++] = jcc_fwd(j, 0x85);          /* jne miss (misaligned) */
        }
    }
    rex(j, 1, 0, 0, 1); g8(j, 0x8B); modrm_sib(j, RCX, R14, RCX, 3, base_off);  /* mov rcx,[r14+rcx*8+base] */
}

/* Forward jump: emit opcode(s) + rel32 placeholder; returns the patch position. */
static size_t jmp_fwd(uam_jit *j) { g8(j, 0xE9); size_t p = j->n; g32(j, 0); return p; }
static size_t jcc_fwd(uam_jit *j, uint8_t cc) { g8(j, 0x0F); g8(j, cc); size_t p = j->n; g32(j, 0); return p; }
static void patch(uam_jit *j, size_t pos)
{
    int32_t rel = (int32_t)(j->n - (pos + 4));
    if (pos + 4 <= CBUF_MAX)
        memcpy(&j->cb[pos], &rel, 4);
}

/* Make room for `need` elements of `size` bytes in *p; on failure the block is not
 * compiled (j->overflow). */
static bool grow(uam_jit *j, void *p, uint32_t *cap, uint32_t need, size_t size)
{
    if (need <= *cap)
        return true;
    uint32_t ncap = *cap ? 2 * *cap : 16;
    while (ncap < need)
        ncap *= 2;
    void *n = realloc(*(void **)p, size * ncap);
    if (!n) {
        j->overflow = true;
        return false;
    }
    *(void **)p = n;
    *cap = ncap;
    return true;
}

/* Before a bridge call that may fault: jc->site = the current instruction's site
 * record (one per instruction, emitted after the code). Clobbers rax. */
static void emit_site(uam_jit *j)
{
    if (!j->cur_emitted) {
        if (!grow(j, &j->sites, &j->cap_sites, j->n_sites + 1, sizeof(jit_site)))
            return;
        j->sites[j->n_sites++] = j->cur;
        j->cur_emitted = true;
    }
    if (!grow(j, &j->site_refs, &j->cap_site_refs, j->n_site_refs + 1, sizeof(*j->site_refs)))
        return;
    g8(j, 0x48); g8(j, 0x8D); g8(j, 0x05);              /* lea rax, [rip + site] */
    j->site_refs[j->n_site_refs].lea = j->n;
    j->site_refs[j->n_site_refs].site = j->n_sites - 1;
    j->n_site_refs++;
    g32(j, 0);
    st_mem(j, RAX, RBX, OFF(site));
}

/*
 * UAM_OP_COV A, B (docs/uam-isa.md section 7.5; uam_cov_count in cpu.h is the
 * reference), inline: no bridge, so chained blocks count too. cpu->cov is read at run
 * time (NULL: nothing), but its history shape (depth, rot, bits) is compiled in: it is
 * fixed while coverage is on, and uam_exec_set_coverage, the only way to change it,
 * flushes every translation. The default shape (depth <= 1, no rotation) is the
 * classic edge, about ten host instructions; deeper histories unroll with immediate
 * shifts. skip_repeat adds a compare of last_pc with the instruction's pc. Clobbers
 * rax, rcx, rdx, rsi (scratch between ops).
 */
static void emit_cov(uam_jit *j, uint64_t pc, uint32_t a, uint32_t b)
{
    const uam_cov_state *cs = j->x->cpu->cov;
    const uint32_t d = cs && cs->depth > 1 ? cs->depth : 1;
    ld_mem(j, RCX, R14, CPU(cov));                         /* rcx = cpu->cov */
    rex(j, 1, 0, 0, 0); g8(j, 0x85); modrm_reg(j, RCX, RCX); /* test rcx, rcx */
    size_t off = jcc_fwd(j, 0x84);                         /* jz skip */
    size_t rep = 0;
    if (cs && cs->skip_repeat) {
        mov_imm(j, RAX, pc);                                /* rax = pc */
        rex(j, 1, 0, 0, 0); g8(j, 0x39); modrm_mem(j, RAX, RCX, COV(last_pc)); /* cmp last_pc, rax */
        rep = jcc_fwd(j, 0x84);                            /* je skip: the same site again */
        st_mem(j, RAX, RCX, COV(last_pc));                 /* last_pc = pc */
    }
    if (d == 1 && !(cs && cs->rot[0])) {
        g8(j, 0x8B); modrm_mem(j, RAX, RCX, COV(prev));    /* eax = prev */
        g8(j, 0x35); g32(j, a);                            /* xor eax, A */
    } else {
        g8(j, 0xB8); g32(j, a);                            /* eax = A */
        for (uint32_t k = 0; k < d; k++) {
            unsigned r = cs->rot[k];
            g8(j, 0x8B); modrm_mem(j, RDX, RCX, COV(hist) + 4 * (int32_t)k); /* edx = hist[k] */
            if (r) {                                       /* rotl within bits, & mask */
                g8(j, 0x89); modrm_reg(j, RDX, RSI);       /* esi = edx */
                g8(j, 0xC1); modrm_reg(j, 4, RDX); g8(j, (uint8_t)r);            /* shl edx, r */
                g8(j, 0xC1); modrm_reg(j, 5, RSI); g8(j, (uint8_t)(cs->bits - r)); /* shr esi, bits-r */
                g8(j, 0x09); modrm_reg(j, RSI, RDX);       /* edx |= esi */
                g8(j, 0x23); modrm_mem(j, RDX, RCX, COV(mask)); /* edx &= mask */
            }
            g8(j, 0x31); modrm_reg(j, RDX, RAX);           /* eax ^= edx */
        }
    }
    g8(j, 0x23); modrm_mem(j, RAX, RCX, COV(mask));        /* and eax, mask (zero-extends) */
    ld_mem(j, RDX, RCX, COV(map));                         /* rdx = map */
    g8(j, 0x80); modrm_sib(j, 0, RDX, RAX, 0, 0); g8(j, 1);  /* add byte [rdx+rax], 1 */
    g8(j, 0x80); modrm_sib(j, 3, RDX, RAX, 0, 0); g8(j, 0);  /* sbb byte [rdx+rax], 0: saturate */
    for (uint32_t k = d - 1; k > 0; k--) {                 /* hist[k] = hist[k-1] */
        g8(j, 0x8B); modrm_mem(j, RDX, RCX, COV(hist) + 4 * (int32_t)(k - 1));
        g8(j, 0x89); modrm_mem(j, RDX, RCX, COV(hist) + 4 * (int32_t)k);
    }
    g8(j, 0xC7); modrm_mem(j, 0, RCX, COV(prev)); g32(j, b); /* hist[0] = B */
    patch(j, off);
    if (rep)
        patch(j, rep);
}

/* Store a block result into jc and jump to the epilogue. */
static void set_res_pc(uam_jit *j, uint32_t kind, const uam_block *blk, uam_ref target, unsigned aw)
{
    load_operand(j, RAX, blk, target, aw);
    st_mem(j, RAX, RBX, RES(pc));
    g8(j, 0xC7); modrm_mem(j, 0, RBX, RES(kind)); g32(j, kind);  /* mov dword[jc+res.kind], kind */
}
static void set_res_pc_imm(uam_jit *j, uint32_t kind, uint64_t pc)
{
    if (fits_s32(pc)) {
        g8(j, 0x48); g8(j, 0xC7); modrm_mem(j, 0, RBX, RES(pc)); g32(j, (uint32_t)pc); /* mov qword */
    } else {
        mov_imm(j, RAX, pc);
        st_mem(j, RAX, RBX, RES(pc));
    }
    g8(j, 0xC7); modrm_mem(j, 0, RBX, RES(kind)); g32(j, kind);
}

/*
 * After an EXIT has stored res (NEXT, pc): jump straight into the next block if its
 * link is current and the run loop has nothing to do (jit.h), else go to the epilogue.
 * A miss records the slot in jc->link_slot so uam_jit_run can link it. `target` is the
 * EXIT's pc operand: for a constant the slot only ever links that pc; for a computed
 * one the slot caches the last target and res.pc is compared against it.
 */
static void emit_chain(uam_jit *j, const uam_block *blk, uam_ref target, size_t *epi,
                       unsigned *n_epi)
{
    bool fixed = uam_ref_is_imm(target);
    /* if (icount >= icount_stop (exit request or deadline) || !chain_left) goto epilogue */
    rex(j, 1, 1, 0, 1); g8(j, 0x3B); modrm_mem(j, R13, R14, CPU(icount_stop)); /* cmp r13, [stop] */
    epi[(*n_epi)++] = jcc_fwd(j, 0x83);                             /* jae */
    g8(j, 0x83); modrm_mem(j, 7, RBX, OFF(chain_left)); g8(j, 0);   /* cmp dword [chain_left], 0 */
    epi[(*n_epi)++] = jcc_fwd(j, 0x84);                             /* je */
    /* rax = &slot (rip-relative; the slot follows the code, patched at the end) */
    g8(j, 0x48); g8(j, 0x8D); g8(j, 0x05);
    j->slot[j->n_slots].lea = j->n;
    j->slot[j->n_slots].pc = fixed ? blk->imm[uam_ref_index(target)] & uam_mask(blk->addr_width)
                                   : LINK_NONE;
    j->n_slots++;
    g32(j, 0);
    size_t miss_pc = 0;
    if (!fixed) {
        ld_mem(j, RCX, RBX, RES(pc));
        rex(j, 1, 0, 0, 0); g8(j, 0x3B); modrm_mem(j, RCX, RAX, SLOT(pc));   /* cmp rcx, [slot.pc] */
        miss_pc = jcc_fwd(j, 0x85);
    }
    ld_mem(j, RCX, RBX, OFF(epoch));
    rex(j, 1, 0, 0, 0); g8(j, 0x3B); modrm_mem(j, RCX, RAX, SLOT(epoch));    /* cmp rcx, [slot.epoch] */
    size_t miss = jcc_fwd(j, 0x85);
    g8(j, 0xFF); modrm_mem(j, 1, RBX, OFF(chain_left));             /* dec dword [chain_left] */
    g8(j, 0xFF); modrm_mem(j, 0, RBX, OFF(hops));                   /* inc dword [hops] */
    g8(j, 0xFF); modrm_mem(j, 4, RAX, SLOT(entry));                 /* jmp [slot.entry] */
    patch(j, miss);
    if (!fixed)
        patch(j, miss_pc);
    st_mem(j, RAX, RBX, OFF(link_slot));
    epi[(*n_epi)++] = jmp_fwd(j);
}

/*
 * Pure opcodes the native compiler emits inline; any other pure op is a call to
 * jit_br_eval. Only POPCNT depends on an optional host feature.
 */
static bool pure_inline(const uam_jit *j, uam_opcode op)
{
    switch (op) {
    case UAM_OP_POPCNT:
        return j->popcnt;
    case UAM_OP_COV: case UAM_OP__COUNT:
        return false;
    default:
        return true;
    }
}

static bool op_supported(uam_opcode op)
{
    switch (op) {
    case UAM_OP_IMARK: case UAM_OP_GET: case UAM_OP_PUT: case UAM_OP_LD: case UAM_OP_ST:
    case UAM_OP_EXIT: case UAM_OP_COV:
        return true;
    case UAM_OP_SYSRD: case UAM_OP_SYSWR: case UAM_OP_TRAP: case UAM_OP_WAIT:
    case UAM_OP_HELPER: case UAM_OP_AMO: case UAM_OP_CAS: case UAM_OP_FENCE:
        return true;
    default:
        break;
    }
    /* pure ops (inline or via jit_br_eval) */
    const uam_opinfo *oi = &uam_opinfo_table[op];
    return !(oi->props & (UAM_P_EFFECT | UAM_P_GUARD)) && op != UAM_OP_GET;
}

/* Emit a pure op whose operands have already been chosen. dst gets the W-bit result. */
static void emit_pure(uam_jit *j, const uam_block *blk, const uam_insn *in)
{
    const uam_opcode op = (uam_opcode)in->op;
    const unsigned W = in->width;
    const unsigned n_src = uam_opinfo_table[op].n_src;
    unsigned src_w = W;
    if (op == UAM_OP_ZEXT || op == UAM_OP_SEXT || op == UAM_OP_TRUNC)
        src_w = uam_ref_is_imm(in->a) ? W : blk->temp_width[in->a];

    if (!pure_inline(j, op)) {
        /* call jit_br_eval(jc, packed, a, b, c) */
        load_operand(j, RDX, blk, in->a, op == UAM_OP_SELECT ? 1 : src_w);
        if (n_src > 1) load_operand(j, RCX, blk, in->b, W); else mov_imm(j, RCX, 0);
        if (n_src > 2) load_operand(j, R8, blk, in->c, W); else mov_imm(j, R8, 0);
        mov_rr(j, RDI, RBX);
        mov_imm(j, RSI, (uint32_t)op | (W << 8) | (src_w << 16));
        mov_imm(j, RAX, (uint64_t)(uintptr_t)&jit_br_eval);
        g8(j, 0xFF); modrm_reg(j, 2, RAX);      /* call rax */
        mask_rax(j, W);
        st_temp(j, in->dst, RAX);
        return;
    }

    /* Natural operand size: 32-bit operations zero-extend their result, so only
     * narrower widths need masking after an add, sub or multiply. AND/OR/XOR of
     * zero-extended values never need it. */
    const unsigned os = W == 64 ? 64 : 32;
    switch (op) {
    case UAM_OP_ADD: case UAM_OP_SUB: case UAM_OP_AND: case UAM_OP_OR: case UAM_OP_XOR: {
        static const int alu_op[] = { [UAM_OP_ADD] = ALU_ADD, [UAM_OP_SUB] = ALU_SUB,
                                      [UAM_OP_AND] = ALU_AND, [UAM_OP_OR] = ALU_OR,
                                      [UAM_OP_XOR] = ALU_XOR };
        load_operand(j, RAX, blk, in->a, W);
        alu(j, alu_op[op], os, RAX, opnd_of(blk, in->b, W));
        if ((op == UAM_OP_ADD || op == UAM_OP_SUB) && W < 32)
            mask_rax(j, W);
        st_temp(j, in->dst, RAX);
        break;
    }
    case UAM_OP_MUL:
        load_operand(j, RAX, blk, in->a, W);
        imul(j, os, RAX, opnd_of(blk, in->b, W));
        if (W < 32)
            mask_rax(j, W);
        st_temp(j, in->dst, RAX);
        break;
    case UAM_OP_EQ: case UAM_OP_NE: case UAM_OP_ULT: case UAM_OP_ULE:
    case UAM_OP_SLT: case UAM_OP_SLE: {
        /* compare at width W itself: the signed conditions then see W-bit values */
        load_operand(j, RAX, blk, in->a, W);
        alu(j, ALU_CMP, W == 1 ? 8 : W, RAX, opnd_of(blk, in->b, W));
        uint8_t cc = op == UAM_OP_EQ ? 0x94 : op == UAM_OP_NE ? 0x95 : op == UAM_OP_ULT ? 0x92
                   : op == UAM_OP_ULE ? 0x96 : op == UAM_OP_SLT ? 0x9C : 0x9E;
        g8(j, 0x0F); g8(j, cc); modrm_reg(j, 0, RAX);              /* setcc al */
        g8(j, 0x0F); g8(j, 0xB6); modrm_reg(j, RAX, RAX);          /* movzx eax, al */
        st_temp(j, in->dst, RAX);
        break;
    }
    case UAM_OP_ZEXT:   /* the source temp is already zero-extended */
        load_operand(j, RAX, blk, in->a, src_w);
        st_temp(j, in->dst, RAX);
        break;
    case UAM_OP_TRUNC:
        load_operand(j, RAX, blk, in->a, src_w);
        mask_rax(j, W);
        st_temp(j, in->dst, RAX);
        break;
    case UAM_OP_SEXT: {
        const opnd a = opnd_of(blk, in->a, src_w);
        if (src_w == 1 || a.kind == O_IMM) {
            load(j, RAX, a, src_w);
            sext_reg(j, RAX, src_w);
        } else {                                       /* movsx/movsxd rax, [rbp+d] */
            g8(j, 0x48);
            if (src_w == 32) g8(j, 0x63);
            else { g8(j, 0x0F); g8(j, src_w == 8 ? 0xBE : 0xBF); }
            modrm_mem(j, RAX, RBP, a.disp);
        }
        mask_rax(j, W);
        st_temp(j, in->dst, RAX);
        break;
    }
    case UAM_OP_SELECT: {
        const opnd c = opnd_of(blk, in->a, 1), x = opnd_of(blk, in->b, W);
        const opnd y = opnd_of(blk, in->c, W);
        if (c.kind == O_IMM) {
            load(j, RAX, c.imm ? x : y, W);
        } else {
            load(j, RAX, y, W);                                      /* default: c */
            g8(j, 0x80); modrm_mem(j, 7, RBP, c.disp); g8(j, 0);     /* cmp byte [cond], 0 */
            if (x.kind == O_IMM) {
                mov_imm(j, RCX, x.imm);
                g8(j, 0x48); g8(j, 0x0F); g8(j, 0x45); modrm_reg(j, RAX, RCX);  /* cmovne rax, rcx */
            } else {
                g8(j, 0x48); g8(j, 0x0F); g8(j, 0x45); modrm_mem(j, RAX, RBP, x.disp); /* cmovne rax, [b] */
            }
        }
        st_temp(j, in->dst, RAX);
        break;
    }
    case UAM_OP_SHL: case UAM_OP_LSHR: case UAM_OP_ASHR: {
        if (!uam_ref_is_imm(in->b)) {
            /* Variable amount y (a W-bit value, so possibly >= 64): shift by cl (the
             * host uses cl & 63), then fix up y >= W. For SHL/LSHR an amount in
             * [W, 64) already gives 0 in the low W bits; beyond, select 0. ASHR clamps
             * the amount to 63, which fills with the sign for any y >= W - 1. */
            load_operand(j, RAX, blk, in->a, W);
            load_operand(j, RCX, blk, in->b, W);
            if (op == UAM_OP_ASHR) {
                sext_reg(j, RAX, W);
                g8(j, 0xBA); g32(j, 63);                                  /* mov edx, 63 */
                g8(j, 0x48); g8(j, 0x83); g8(j, 0xF9); g8(j, 63);         /* cmp rcx, 63 */
                g8(j, 0x48); g8(j, 0x0F); g8(j, 0x47); g8(j, 0xCA);       /* cmova rcx, rdx */
                g8(j, 0x48); g8(j, 0xD3); g8(j, 0xF8);                    /* sar rax, cl */
            } else {
                g8(j, 0x48); g8(j, 0xD3); g8(j, op == UAM_OP_SHL ? 0xE0 : 0xE8); /* shl/shr rax, cl */
                g8(j, 0x31); g8(j, 0xD2);                                 /* xor edx, edx */
                g8(j, 0x48); g8(j, 0x83); g8(j, 0xF9); g8(j, (uint8_t)W); /* cmp rcx, W */
                g8(j, 0x48); g8(j, 0x0F); g8(j, 0x43); g8(j, 0xC2);       /* cmovae rax, rdx */
            }
            mask_rax(j, W);
            st_temp(j, in->dst, RAX);
            break;
        }
        uint64_t n = blk->imm[uam_ref_index(in->b)];   /* any amount >= W shifts everything out */
        if (n >= W) {
            if (op == UAM_OP_ASHR) {
                load_operand(j, RAX, blk, in->a, W);
                sext_reg(j, RAX, W);
                shift_imm(j, SH_SAR, 64, RAX, 63);
                mask_rax(j, W);
            } else {
                mov_imm(j, RAX, 0);
            }
            st_temp(j, in->dst, RAX);
            break;
        }
        load_operand(j, RAX, blk, in->a, W);
        if (op == UAM_OP_SHL) {
            shift_imm(j, SH_SHL, os, RAX, (unsigned)n);
            if (W < 32)
                mask_rax(j, W);
        } else if (op == UAM_OP_LSHR) {
            shift_imm(j, SH_SHR, os, RAX, (unsigned)n);     /* zero-extended: nothing to mask */
        } else if (W >= 32) {
            shift_imm(j, SH_SAR, os, RAX, (unsigned)n);     /* sar eax zero-extends the result */
        } else {
            g8(j, 0x0F); g8(j, W == 8 ? 0xBE : 0xBF); modrm_reg(j, RAX, RAX); /* movsx eax, al/ax */
            shift_imm(j, SH_SAR, 32, RAX, (unsigned)n);
            mask_rax(j, W);
        }
        st_temp(j, in->dst, RAX);
        break;
    }
    case UAM_OP_CARRY: case UAM_OP_SCARRY: case UAM_OP_SBORROW: {
        /* The W-bit add/sub sets the host's CF/OF exactly as section 7.2 defines
         * carry and signed overflow (operands are zero-extended W-bit values). */
        load_operand(j, RAX, blk, in->a, W);
        load_operand(j, RCX, blk, in->b, W);
        const bool sub = op == UAM_OP_SBORROW;
        if (W == 16) g8(j, 0x66);
        if (W == 64) g8(j, 0x48);
        g8(j, (uint8_t)((sub ? 0x28 : 0x00) | (W == 8 ? 0 : 1)));    /* add/sub al|ax|eax|rax, cl.. */
        modrm_reg(j, RCX, RAX);
        g8(j, 0x0F); g8(j, op == UAM_OP_CARRY ? 0x92 : 0x90); modrm_reg(j, 0, RAX); /* setc/seto al */
        g8(j, 0x0F); g8(j, 0xB6); modrm_reg(j, RAX, RAX);              /* movzx eax, al */
        st_temp(j, in->dst, RAX);
        break;
    }
    case UAM_OP_POPCNT:
        load_operand(j, RAX, blk, in->a, W);
        g8(j, 0xF3); g8(j, 0x48); g8(j, 0x0F); g8(j, 0xB8); modrm_reg(j, RAX, RAX); /* popcnt rax, rax */
        st_temp(j, in->dst, RAX);
        break;
    case UAM_OP_CLZ:
        load_operand(j, RAX, blk, in->a, W);
        if (W < 64) {
            /* bsr of 2x+1 is (index of x's top bit) + 1, or 0 for x = 0: clz = W - that */
            g8(j, 0x48); g8(j, 0x8D); g8(j, 0x44); g8(j, 0x00); g8(j, 0x01); /* lea rax, [rax+rax+1] */
            g8(j, 0x48); g8(j, 0x0F); g8(j, 0xBD); modrm_reg(j, RAX, RAX);    /* bsr rax, rax */
            g8(j, 0x48); g8(j, 0xF7); g8(j, 0xD8);                            /* neg rax */
            g8(j, 0x48); g8(j, 0x83); g8(j, 0xC0); g8(j, (uint8_t)W);         /* add rax, W */
        } else {
            g8(j, 0x48); g8(j, 0xC7); g8(j, 0xC1); g32(j, 0xFFFFFFFFu);       /* mov rcx, -1 */
            g8(j, 0x48); g8(j, 0x0F); g8(j, 0xBD); modrm_reg(j, RAX, RAX);    /* bsr rax, rax */
            g8(j, 0x48); g8(j, 0x0F); g8(j, 0x44); g8(j, 0xC1);               /* cmovz rax, rcx */
            g8(j, 0x48); g8(j, 0xF7); g8(j, 0xD8);                            /* neg rax */
            g8(j, 0x48); g8(j, 0x83); g8(j, 0xC0); g8(j, 63);                 /* add rax, 63 */
        }
        st_temp(j, in->dst, RAX);
        break;
    case UAM_OP_CTZ:
        load_operand(j, RAX, blk, in->a, W);
        if (W < 64) {
            g8(j, 0x48); g8(j, 0x0F); g8(j, 0xBA); g8(j, 0xE8); g8(j, (uint8_t)W); /* bts rax, W */
            g8(j, 0x48); g8(j, 0x0F); g8(j, 0xBC); modrm_reg(j, RAX, RAX);    /* bsf rax, rax */
        } else {
            g8(j, 0xB9); g32(j, 64);                                          /* mov ecx, 64 */
            g8(j, 0x48); g8(j, 0x0F); g8(j, 0xBC); modrm_reg(j, RAX, RAX);    /* bsf rax, rax */
            g8(j, 0x48); g8(j, 0x0F); g8(j, 0x44); g8(j, 0xC1);               /* cmovz rax, rcx */
        }
        st_temp(j, in->dst, RAX);
        break;
    case UAM_OP_BSWAP:
        load_operand(j, RAX, blk, in->a, W);
        if (W == 16) { g8(j, 0x66); g8(j, 0xC1); modrm_reg(j, 0, RAX); g8(j, 8); } /* rol ax, 8 */
        else if (W == 32) { g8(j, 0x0F); g8(j, 0xC8); }                     /* bswap eax */
        else { g8(j, 0x48); g8(j, 0x0F); g8(j, 0xC8); }                     /* bswap rax */
        st_temp(j, in->dst, RAX);
        break;
    case UAM_OP_ROTR: {
        /* ror at width W: the host rotates by (count & 31 or 63) mod W, which is
         * the amount mod W for every W, as section 7.1 defines */
        load_operand(j, RAX, blk, in->a, W);
        const bool imm = uam_ref_is_imm(in->b);
        if (!imm)
            load_operand(j, RCX, blk, in->b, W);
        if (W == 16) g8(j, 0x66);
        if (W == 64) g8(j, 0x48);
        if (imm) {
            g8(j, W == 8 ? 0xC0 : 0xC1); modrm_reg(j, 1, RAX);
            g8(j, (uint8_t)(blk->imm[uam_ref_index(in->b)] % W));         /* ror r, imm8 */
        } else {
            g8(j, W == 8 ? 0xD2 : 0xD3); modrm_reg(j, 1, RAX);            /* ror r, cl */
        }
        mask_rax(j, W);
        st_temp(j, in->dst, RAX);
        break;
    }
    case UAM_OP_MULHU: case UAM_OP_MULHS:
        load_operand(j, RAX, blk, in->a, W);
        load_operand(j, RCX, blk, in->b, W);
        if (W < 64) {
            /* the 2W-bit product fits in 64 bits */
            if (op == UAM_OP_MULHS) { sext_reg(j, RAX, W); sext_reg(j, RCX, W); }
            g8(j, 0x48); g8(j, 0x0F); g8(j, 0xAF); modrm_reg(j, RAX, RCX);    /* imul rax, rcx */
            g8(j, 0x48); g8(j, 0xC1); modrm_reg(j, op == UAM_OP_MULHS ? 7 : 5, RAX);
            g8(j, (uint8_t)W);                                                /* sar/shr rax, W */
            mask_rax(j, W);
        } else {
            g8(j, 0x48); g8(j, 0xF7); modrm_reg(j, op == UAM_OP_MULHS ? 5 : 4, RCX); /* imul/mul rcx */
            mov_rr(j, RAX, RDX);                                              /* rax = high half */
        }
        st_temp(j, in->dst, RAX);
        break;
    case UAM_OP_UDIV: case UAM_OP_UREM: case UAM_OP_SDIV: case UAM_OP_SREM: {
        /* Section 7.1: x / 0 = all ones, x % 0 = x; SMIN / -1 = SMIN, SMIN % -1 = 0.
         * Signed ops divide the sign-extended operands at 64 bits, where only
         * W = 64 can overflow (y = -1: the quotient is -x mod 2^64, the remainder 0). */
        const bool sgn = op == UAM_OP_SDIV || op == UAM_OP_SREM;
        const bool rem = op == UAM_OP_UREM || op == UAM_OP_SREM;
        load_operand(j, RAX, blk, in->a, W);
        load_operand(j, RCX, blk, in->b, W);
        if (sgn) { sext_reg(j, RAX, W); sext_reg(j, RCX, W); }
        g8(j, 0x48); g8(j, 0x85); modrm_reg(j, RCX, RCX);                    /* test rcx, rcx */
        size_t by_zero = jcc_fwd(j, 0x84);                                    /* jz */
        size_t by_m1 = 0;
        if (sgn && W == 64) {
            g8(j, 0x48); g8(j, 0x83); g8(j, 0xF9); g8(j, 0xFF);               /* cmp rcx, -1 */
            by_m1 = jcc_fwd(j, 0x84);                                         /* je */
        }
        if (sgn) {
            g8(j, 0x48); g8(j, 0x99);                                         /* cqo */
            g8(j, 0x48); g8(j, 0xF7); modrm_reg(j, 7, RCX);                   /* idiv rcx */
        } else {
            g8(j, 0x31); g8(j, 0xD2);                                         /* xor edx, edx */
            if (W == 64) g8(j, 0x48);
            g8(j, 0xF7); modrm_reg(j, 6, RCX);                                /* div ecx / rcx */
        }
        if (rem)
            mov_rr(j, RAX, RDX);
        size_t done = jmp_fwd(j);
        patch(j, by_zero);
        if (!rem)
            mov_imm(j, RAX, uam_mask(W));                                      /* x / 0 */
        size_t done0 = rem ? 0 : jmp_fwd(j);                                  /* x % 0 = x: rax */
        if (by_m1) {
            if (rem) { done0 = jmp_fwd(j); }
            patch(j, by_m1);
            if (rem) { g8(j, 0x31); g8(j, 0xC0); }                            /* xor eax, eax */
            else { g8(j, 0x48); g8(j, 0xF7); g8(j, 0xD8); }                   /* neg rax */
        }
        patch(j, done);
        if (done0)
            patch(j, done0);
        mask_rax(j, W);
        st_temp(j, in->dst, RAX);
        break;
    }
    default:
        break;
    }
}

/* Compile the whole block into j->cb. Returns false if it must be interpreted. */
static bool compile_block(uam_jit *j, const uam_block *blk)
{
    const unsigned aw = blk->addr_width;
    j->n = 0;
    j->overflow = false;
    j->n_slots = 0;
    j->n_marks = 0;
    j->n_sites = j->n_site_refs = j->n_stubs = 0;
    j->cur = (jit_site){ blk->guest_pc, blk->guest_pc, 0 };
    j->cur_emitted = false;

    for (uint32_t i = 0; i < blk->n_insn; i++) {
        uam_opcode op = (uam_opcode)blk->insn[i].op;
        if (!op_supported(op))
            return false;
    }

    /* prologue */
    mark(j, MK_PROLOGUE, 0);
    g8(j, 0x53);                                   /* push rbx */
    g8(j, 0x55);                                   /* push rbp */
    g8(j, 0x41); g8(j, 0x54);                      /* push r12 */
    g8(j, 0x41); g8(j, 0x55);                      /* push r13 */
    g8(j, 0x41); g8(j, 0x56);                      /* push r14 */
    g8(j, 0x41); g8(j, 0x57);                      /* push r15 */
    g8(j, 0x48); g8(j, 0x83); g8(j, 0xEC); g8(j, 0x08);  /* sub rsp, 8 (align) */
    mov_rr(j, RBX, RDI);                           /* rbx = jc */
    ld_mem(j, RBP, RBX, OFF(temps));
    ld_mem(j, R14, RBX, OFF(cpu));
    ld_mem(j, R15, RBX, OFF(regfile));
    ld_mem(j, R13, R14, CPU(icount));              /* r13 = icount (kept in a register) */
    j->body_off = j->n;                            /* chained jumps enter here */

    /* epilogue jumps: up to 4 per EXIT (chain checks), 1 per other op */
    size_t *epi = malloc(sizeof(size_t) * (4 * blk->n_insn + 1));
    if (!epi)
        return false;
    unsigned n_epi = 0;
    bool first = true;
    uint64_t imark_pc = blk->guest_pc;   /* the pc of the instruction being emitted */

    for (uint32_t i = 0; i < blk->n_insn && !j->overflow; i++) {
        const uam_insn *in = &blk->insn[i];
        const uam_opcode op = (uam_opcode)in->op;
        const unsigned W = in->width;
        mark(j, MK_OP, i);

        /* Guard on an effectful op. */
        size_t guard_skip = 0;
        bool guarded = false;
        if (!uam_ref_is_none(in->guard)) {
            if (uam_ref_is_imm(in->guard)) {
                if (!(blk->imm[uam_ref_index(in->guard)] & 1)) {
                    if (!uam_ref_is_none(in->dst)) { mov_imm(j, RAX, 0); st_temp(j, in->dst, RAX); }
                    continue;   /* guard false: op does nothing */
                }
                /* guard true: fall through unguarded */
            } else {
                g8(j, 0x80); modrm_mem(j, 7, RBP, (int32_t)(8 * in->guard)); g8(j, 0); /* cmp byte [g], 0 */
                guard_skip = jcc_fwd(j, 0x84);    /* je skip */
                guarded = true;
            }
        }

        switch (op) {
        case UAM_OP_IMARK: {
            uint64_t pc = blk->imm[uam_ref_index(in->a)];
            bool cont = in->flags & UAM_IMARK_CONT;
            imark_pc = pc;
            if (!first && !cont) {
                if (in->flags & UAM_IMARK_BREAK) {
                    set_res_pc_imm(j, UAM_BR_INTERRUPTED, pc);
                    epi[n_epi++] = jmp_fwd(j);
                } else if (grow(j, &j->stubs, &j->cap_stubs, j->n_stubs + 1, sizeof(*j->stubs))) {
                    /* if (icount >= icount_stop) stop here: an out-of-line stub */
                    rex(j, 1, 1, 0, 1); g8(j, 0x3B); modrm_mem(j, R13, R14, CPU(icount_stop));
                    j->stubs[j->n_stubs].jcc = jcc_fwd(j, 0x83);                 /* jae */
                    j->stubs[j->n_stubs].pc = pc;
                    j->n_stubs++;
                }
            }
            first = false;
            /* A continuation keeps the parent of the instruction it continues. */
            j->cur.parent_pc = cont ? j->cur.parent_pc : pc;
            j->cur.pc = pc;
            j->cur.cont = cont;
            j->cur_emitted = false;
            mark(j, MK_IMARK_PC, i);
            g8(j, 0x49); g8(j, 0xFF); modrm_reg(j, 0, R13); /* inc r13 (icount) */
            break;
        }
        case UAM_OP_GET:
            if (W == 1) {
                mov_imm(j, RAX, 0);
            } else {
                int b = R15;
                if (W == 8) { rex(j, 0, 0, 0, 1); g8(j, 0x0F); g8(j, 0xB6); modrm_mem(j, RAX, b, (int32_t)in->aux); }
                else if (W == 16) { rex(j, 0, 0, 0, 1); g8(j, 0x0F); g8(j, 0xB7); modrm_mem(j, RAX, b, (int32_t)in->aux); }
                else if (W == 32) { rex(j, 0, 0, 0, 1); g8(j, 0x8B); modrm_mem(j, RAX, b, (int32_t)in->aux); }
                else { rex(j, 1, 0, 0, 1); g8(j, 0x8B); modrm_mem(j, RAX, b, (int32_t)in->aux); }
            }
            st_temp(j, in->dst, RAX);
            break;
        case UAM_OP_PUT:
            if (uam_ref_is_imm(in->a) && (W < 64 || fits_s32(blk->imm[uam_ref_index(in->a)]))) {
                const uint64_t v = blk->imm[uam_ref_index(in->a)] & uam_mask(W);
                if (W == 16) g8(j, 0x66);
                rex(j, W == 64, 0, 0, 1);
                g8(j, W == 8 ? 0xC6 : 0xC7);                     /* mov [r15+off], imm */
                modrm_mem(j, 0, R15, (int32_t)in->aux);
                if (W == 8) g8(j, (uint8_t)v);
                else if (W == 16) { g8(j, (uint8_t)v); g8(j, (uint8_t)(v >> 8)); }
                else g32(j, (uint32_t)v);
            } else if (W != 1) {
                load_operand(j, RAX, blk, in->a, W);
                int b = R15;
                if (W == 8) { rex(j, 0, 0, 0, 1); g8(j, 0x88); modrm_mem(j, RAX, b, (int32_t)in->aux); }
                else if (W == 16) { g8(j, 0x66); rex(j, 0, 0, 0, 1); g8(j, 0x89); modrm_mem(j, RAX, b, (int32_t)in->aux); }
                else if (W == 32) { rex(j, 0, 0, 0, 1); g8(j, 0x89); modrm_mem(j, RAX, b, (int32_t)in->aux); }
                else { rex(j, 1, 0, 0, 1); g8(j, 0x89); modrm_mem(j, RAX, b, (int32_t)in->aux); }
            }
            break;
        case UAM_OP_LD: {
            unsigned size = W / 8;
            bool be = in->flags & UAM_ACC_BE;
            bool fast = !(in->flags & (UAM_ACC_X | UAM_ACC_PHYS | UAM_ACC_UNPRIV));
            load_operand(j, RSI, blk, in->a, aw);       /* rsi = addr */
            size_t done = 0, miss[3]; unsigned n_miss = 0;
            if (fast) {
                emit_dtlb_probe(j, size, in->flags & UAM_ACC_ALIGNED, CPU(dtlb_rt), CPU(dtlb_rb),
                                miss, &n_miss);
                emit_fast_load(j, size, be);            /* rax = value */
                st_temp(j, in->dst, RAX);
                done = jmp_fwd(j);
                for (unsigned k = 0; k < n_miss; k++)
                    patch(j, miss[k]);
            }
            mark(j, MK_SLOW, i);
            st_mem(j, R13, R14, CPU(icount));           /* flush icount: the bridge may read time */
            emit_site(j);
            mov_rr(j, RDI, RBX);                        /* miss / slow path: the bridge */
            mov_imm(j, RDX, in->flags);
            mov_imm(j, RCX, size);
            mov_imm(j, RAX, (uint64_t)(uintptr_t)&jit_br_load);
            g8(j, 0xFF); modrm_reg(j, 2, RAX);
            g8(j, 0x85); modrm_reg(j, RAX, RAX);
            epi[n_epi++] = jcc_fwd(j, 0x85);            /* jnz epilogue (fault) */
            ld_mem(j, RAX, RBX, OFF(scratch));
            st_temp(j, in->dst, RAX);
            if (fast)
                patch(j, done);
            break;
        }
        case UAM_OP_ST:
            if (in->flags & UAM_ACC_X) {               /* store-conditional: always the bridge */
                st_mem(j, R13, R14, CPU(icount));
                emit_site(j);
                mov_rr(j, RDI, RBX);
                load_operand(j, RSI, blk, in->a, aw);
                load_operand(j, RDX, blk, in->b, W);
                mov_imm(j, RCX, in->flags);
                mov_imm(j, R8, W / 8);
                mov_imm(j, RAX, (uint64_t)(uintptr_t)&jit_br_store_x);
                g8(j, 0xFF); modrm_reg(j, 2, RAX);
                g8(j, 0x85); modrm_reg(j, RAX, RAX);
                epi[n_epi++] = jcc_fwd(j, 0x85);
                if (!uam_ref_is_none(in->dst)) {
                    ld_mem(j, RAX, RBX, OFF(scratch));
                    st_temp(j, in->dst, RAX);
                }
                break;
            }
            {
            unsigned size = W / 8;
            bool be = in->flags & UAM_ACC_BE;
            bool fast = !(in->flags & (UAM_ACC_X | UAM_ACC_PHYS | UAM_ACC_UNPRIV));
            load_operand(j, RSI, blk, in->a, aw);       /* rsi = addr */
            load_operand(j, RDX, blk, in->b, W);        /* rdx = value */
            size_t done = 0, miss[3]; unsigned n_miss = 0;
            if (fast) {
                emit_dtlb_probe(j, size, in->flags & UAM_ACC_ALIGNED, CPU(dtlb_wt), CPU(dtlb_wb),
                                miss, &n_miss);
                emit_fast_store(j, size, be);
                done = jmp_fwd(j);
                for (unsigned k = 0; k < n_miss; k++)
                    patch(j, miss[k]);
            }
            mark(j, MK_SLOW, i);
            st_mem(j, R13, R14, CPU(icount));
            emit_site(j);
            mov_rr(j, RDI, RBX);
            load_operand(j, RDX, blk, in->b, W);        /* reload: the bridge call needs it in rdx */
            mov_imm(j, RCX, in->flags);
            mov_imm(j, R8, size);
            mov_imm(j, RAX, (uint64_t)(uintptr_t)&jit_br_store);
            g8(j, 0xFF); modrm_reg(j, 2, RAX);
            g8(j, 0x85); modrm_reg(j, RAX, RAX);
            epi[n_epi++] = jcc_fwd(j, 0x85);
            if (fast)
                patch(j, done);
            break;
            }
        case UAM_OP_AMO:
        case UAM_OP_CAS: {
            const bool cas = op == UAM_OP_CAS;
            st_mem(j, R13, R14, CPU(icount));
            emit_site(j);
            mov_rr(j, RDI, RBX);
            load_operand(j, RSI, blk, in->a, aw);       /* addr */
            load_operand(j, RDX, blk, in->b, W);        /* operand / expected */
            if (cas)
                load_operand(j, RCX, blk, in->c, W);    /* new value */
            else
                mov_imm(j, RCX, 0);
            mov_imm(j, R8, in->flags);
            mov_imm(j, R9, (uint32_t)op | (cas ? 0 : (uint32_t)in->aux << 8) | (uint32_t)W << 16);
            mov_imm(j, RAX, (uint64_t)(uintptr_t)&jit_br_rmw);
            g8(j, 0xFF); modrm_reg(j, 2, RAX);
            g8(j, 0x85); modrm_reg(j, RAX, RAX);
            epi[n_epi++] = jcc_fwd(j, 0x85);
            if (!uam_ref_is_none(in->dst)) {
                ld_mem(j, RAX, RBX, OFF(scratch));
                st_temp(j, in->dst, RAX);
            }
            break;
        }
        case UAM_OP_FENCE:
            /* Ordering is a no-op: a hart's blocks run one at a time and harts are
             * interleaved only between blocks. FENCE.INSN needs nothing either: code
             * writes invalidate translations and the block ends here (see interp.c). */
            break;
        case UAM_OP_EXIT:
            set_res_pc(j, UAM_BR_NEXT, blk, in->a, aw);
            mark(j, MK_CHAIN, i);
            if (j->n_slots < MAX_SLOTS)
                emit_chain(j, blk, in->a, epi, &n_epi);
            else
                epi[n_epi++] = jmp_fwd(j);
            break;
        case UAM_OP_SYSRD:
            st_mem(j, R13, R14, CPU(icount));
            emit_site(j);
            mov_rr(j, RDI, RBX);
            mov_imm(j, RSI, in->aux);
            mov_imm(j, RAX, (uint64_t)(uintptr_t)&jit_br_sysrd);
            g8(j, 0xFF); modrm_reg(j, 2, RAX);
            g8(j, 0x85); modrm_reg(j, RAX, RAX);
            epi[n_epi++] = jcc_fwd(j, 0x85);          /* jnz epilogue (fault) */
            ld_mem(j, RAX, RBX, OFF(scratch));
            mask_rax(j, W);
            st_temp(j, in->dst, RAX);
            break;
        case UAM_OP_SYSWR:
            st_mem(j, R13, R14, CPU(icount));
            emit_site(j);
            mov_rr(j, RDI, RBX);
            mov_imm(j, RSI, in->aux);
            load_operand(j, RDX, blk, in->a, W);
            mov_imm(j, RAX, (uint64_t)(uintptr_t)&jit_br_syswr);
            g8(j, 0xFF); modrm_reg(j, 2, RAX);
            g8(j, 0x85); modrm_reg(j, RAX, RAX);
            epi[n_epi++] = jcc_fwd(j, 0x85);
            break;
        case UAM_OP_HELPER: {
            const uam_ref args[3] = { in->a, in->b, in->c };
            const int regs[3] = { RDX, RCX, R8 };
            st_mem(j, R13, R14, CPU(icount));
            emit_site(j);
            for (unsigned k = 0; k < 3; k++) {
                if (uam_ref_is_none(args[k]))
                    mov_imm(j, regs[k], 0);
                else
                    load_operand(j, regs[k], blk, args[k], 64);
            }
            mov_rr(j, RDI, RBX);
            mov_imm(j, RSI, in->aux);
            mov_imm(j, RAX, (uint64_t)(uintptr_t)&jit_br_helper);
            g8(j, 0xFF); modrm_reg(j, 2, RAX);
            g8(j, 0x85); modrm_reg(j, RAX, RAX);
            epi[n_epi++] = jcc_fwd(j, 0x85);          /* jnz epilogue (fault) */
            if (!uam_ref_is_none(in->dst)) {
                ld_mem(j, RAX, RBX, OFF(scratch));
                st_temp(j, in->dst, RAX);
            }
            break;
        }
        case UAM_OP_TRAP:
            /* res = { TRAP, class, exc{TRAP, flags=cont, pc, parent_pc, cause, tval} } */
            mov_imm(j, RAX, j->cur.pc);         st_mem(j, RAX, RBX, RES(pc));
            st_mem(j, RAX, RBX, REXC(pc));
            mov_imm(j, RAX, j->cur.parent_pc);  st_mem(j, RAX, RBX, REXC(parent_pc));
            g8(j, 0xC7); modrm_mem(j, 0, RBX, REXC(flags)); g32(j, j->cur.cont ? UAM_EXC_F_CONT : 0);
            load_operand(j, RAX, blk, in->a, aw); st_mem(j, RAX, RBX, REXC(cause));
            load_operand(j, RAX, blk, in->b, aw); st_mem(j, RAX, RBX, REXC(tval));
            g8(j, 0xC7); modrm_mem(j, 0, RBX, REXC(kind)); g32(j, UAM_EXC_TRAP);
            g8(j, 0xC7); modrm_mem(j, 0, RBX, RES(trap_class)); g32(j, in->aux);
            g8(j, 0xC7); modrm_mem(j, 0, RBX, RES(kind)); g32(j, UAM_BR_TRAP);
            epi[n_epi++] = jmp_fwd(j);
            break;
        case UAM_OP_WAIT:
            set_res_pc(j, UAM_BR_WAIT, blk, in->a, aw);
            epi[n_epi++] = jmp_fwd(j);
            break;
        case UAM_OP_COV:
            emit_cov(j, imark_pc, (uint32_t)blk->imm[uam_ref_index(in->a)],
                     (uint32_t)blk->imm[uam_ref_index(in->b)]);
            break;
        default:
            emit_pure(j, blk, in);
            break;
        }

        if (guarded) {
            /* guard false: no effect, dst = 0 (section 4.1) */
            if (!uam_ref_is_none(in->dst)) {
                size_t over = jmp_fwd(j);
                patch(j, guard_skip);
                mov_imm(j, RAX, 0);
                st_temp(j, in->dst, RAX);
                patch(j, over);
            } else {
                patch(j, guard_skip);
            }
        }
    }

    /* out-of-line stops at IMARKs */
    mark(j, MK_STUB, 0);
    for (uint32_t k = 0; k < j->n_stubs; k++) {
        patch(j, j->stubs[k].jcc);
        set_res_pc_imm(j, UAM_BR_INTERRUPTED, j->stubs[k].pc);
        epi[n_epi++] = jmp_fwd(j);
    }

    /* epilogue */
    for (unsigned k = 0; k < n_epi; k++)
        patch(j, epi[k]);
    free(epi);
    mark(j, MK_EPILOGUE, 0);
    st_mem(j, R13, R14, CPU(icount));                   /* flush icount */
    g8(j, 0x48); g8(j, 0x83); g8(j, 0xC4); g8(j, 0x08);  /* add rsp, 8 */
    g8(j, 0x41); g8(j, 0x5F);                            /* pop r15 */
    g8(j, 0x41); g8(j, 0x5E);                            /* pop r14 */
    g8(j, 0x41); g8(j, 0x5D);                            /* pop r13 */
    g8(j, 0x41); g8(j, 0x5C);                            /* pop r12 */
    g8(j, 0x5D);                                         /* pop rbp */
    g8(j, 0x5B);                                         /* pop rbx */
    g8(j, 0xC3);                                         /* ret */

    /* link slots, 8-byte aligned after the code (the code itself starts 16-aligned) */
    mark(j, MK_SLOTS, 0);
    while (j->n & 7)
        g8(j, 0xCC);                                     /* int3 padding */
    for (unsigned k = 0; k < j->n_slots; k++) {
        int32_t rel = (int32_t)(j->n - (j->slot[k].lea + 4));
        if (j->slot[k].lea + 4 <= CBUF_MAX)
            memcpy(&j->cb[j->slot[k].lea], &rel, 4);
        g64(j, 0);                                       /* entry */
        g64(j, LINK_NONE);                               /* epoch: never linked */
        g64(j, j->slot[k].pc);                           /* pc */
    }
    /* fault sites (8-aligned too: slots are 24 bytes) */
    size_t sites_at = j->n;
    for (uint32_t k = 0; k < j->n_sites; k++) {
        g64(j, j->sites[k].pc);
        g64(j, j->sites[k].parent_pc);
        g64(j, j->sites[k].cont);
    }
    for (uint32_t k = 0; k < j->n_site_refs; k++) {
        int32_t rel = (int32_t)(sites_at + sizeof(jit_site) * j->site_refs[k].site
                                - (j->site_refs[k].lea + 4));
        if (j->site_refs[k].lea + 4 <= CBUF_MAX)
            memcpy(&j->cb[j->site_refs[k].lea], &rel, 4);
    }

    return !j->overflow;
}

/* ---- public interface -------------------------------------------------- */

uam_jit *uam_jit_create(uam_exec *x)
{
    void *mem = mmap(NULL, ARENA_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED)
        return NULL;
    uam_jit *j = calloc(1, sizeof(*j));
    if (!j) {
        munmap(mem, ARENA_SIZE);
        return NULL;
    }
    j->x = x;
    j->arena = mem;
    j->jc.cpu = x->cpu;
    j->jc.regfile = x->cpu->regfile;
    j->jc.helpers = &x->cpu->isa->helpers;
    j->jc.sched = x->sched;
    j->jc.stats = &x->stats;
    /* Optional host features. UAM_JIT_BASELINE=1 turns them off (to test the fallbacks). */
    const char *base = getenv("UAM_JIT_BASELINE");
    __builtin_cpu_init();
    j->popcnt = __builtin_cpu_supports("popcnt") && !(base && *base && strcmp(base, "0") != 0);
    /* Profiling output. Every JIT of the process appends to the same files. */
    const char *pm = getenv("UAM_PERF_MAP");
    if (pm && *pm && strcmp(pm, "0") != 0) {
        char path[64];
        snprintf(path, sizeof(path), "/tmp/perf-%d.map", (int)getpid());
        j->perf_map = fopen(path, "a");
    }
    const char *om = getenv("UAM_JIT_OPMAP");
    if (om && *om)
        j->opmap = fopen(om, "a");
    return j;
}

/* Describe the block just placed at `code` in the perf map and the op map. */
static void write_profile_maps(uam_jit *j, const uam_block *blk, const uint8_t *code)
{
    const char *isa = j->x->cpu->isa->name;
    if (j->perf_map) {
        fprintf(j->perf_map, "%llx %zx uam:%s:0x%llx\n", (unsigned long long)(uintptr_t)code,
                j->n, isa, (unsigned long long)blk->guest_pc);
        fflush(j->perf_map);
    }
    if (!j->opmap)
        return;
    fprintf(j->opmap, "B %llx %zx %llx %s\n", (unsigned long long)(uintptr_t)code, j->n,
            (unsigned long long)blk->guest_pc, isa);
    for (uint32_t k = 0; k < j->n_marks; k++) {
        const jit_mark *m = &j->marks[k];
        fprintf(j->opmap, "M %x %s %u", m->off, mark_name[m->kind], m->idx);
        if (m->kind == MK_OP || m->kind == MK_SLOW) {
            const uam_insn *in = &blk->insn[m->idx];
            fprintf(j->opmap, " %s.%u", uam_opinfo_table[in->op].mnemonic, in->width);
            if (in->op == UAM_OP_GET || in->op == UAM_OP_PUT)
                fprintf(j->opmap, "@%u", in->aux);
            if (in->op == UAM_OP_IMARK)
                fprintf(j->opmap, " 0x%llx", (unsigned long long)blk->imm[uam_ref_index(in->a)]);
        }
        fputc('\n', j->opmap);
    }
    fflush(j->opmap);
}

void uam_jit_destroy(uam_jit *j)
{
    if (!j)
        return;
    munmap(j->arena, ARENA_SIZE);
    if (j->perf_map)
        fclose(j->perf_map);
    if (j->opmap)
        fclose(j->opmap);
    free(j->marks);
    free(j->sites);
    free(j->site_refs);
    free(j->stubs);
    free(j);
}

bool uam_jit_compile(uam_jit *j, uam_block *blk)
{
    if (!compile_block(j, blk) || j->overflow) {
        blk->backend = UAM_JIT_INTERP;
        return true;
    }
    if (j->n > ARENA_SIZE)
        return false;   /* a single block larger than the arena: impossible in practice */
    if (j->arena_used + j->n > ARENA_SIZE) {
        /* Arena full: drop every compiled block (and every link), then restart this one. */
        uam_tcache_flush(&j->x->tc);
        j->arena_used = 0;
        j->arena_gen++;
        if (j->opmap)
            fputs("R\n", j->opmap);   /* the addresses below are reused from here on */
    }
    uint8_t *code = j->arena + j->arena_used;
    memcpy(code, j->cb, j->n);
    j->x->stats.code_bytes += j->n;
    write_profile_maps(j, blk, code);
    j->arena_used += (j->n + 15) & ~(size_t)15;   /* keep 16-byte alignment */
    blk->backend = code;
    blk->backend_free = NULL;                      /* bulk-freed with the arena */
    return true;
}

void uam_jit_run(uam_jit *j, const uam_block *blk, uint64_t *temps, uam_jit_chain *chain,
                 uam_block_result *res)
{
    uam_jit_ctx *jc = &j->jc;
    uam_exec *x = j->x;
    uam_cpu *cpu = x->cpu;

    /* A linked block may have become invalid since the epoch was set: kill all links. */
    if (!j->key_valid || x->tc.gen != j->key_tc || cpu->xlate_gen != j->key_xgen
        || cpu->priv != j->key_priv) {
        jc->epoch++;
        j->key_valid = true;
        j->key_tc = x->tc.gen;
        j->key_xgen = cpu->xlate_gen;
        j->key_priv = cpu->priv;
    }
    /* The last native block's EXIT missed its link and led here: link it. The run loop
     * just validated blk for the current epoch and has nothing to do before entering it
     * that it would not also check at the EXIT (no hook on blk: chain->link). */
    if (j->pend_slot) {
        if (chain->link && j->pend_arena == j->arena_gen && blk->guest_pc == j->pend_pc) {
            j->pend_slot->entry = (uint64_t)(uintptr_t)blk->backend + j->body_off;
            j->pend_slot->pc = blk->guest_pc;
            j->pend_slot->epoch = jc->epoch;
        }
        j->pend_slot = NULL;
    }

    jc->temps = temps;   /* the executor may grow it between runs */
    jc->chain_left = chain->budget;
    jc->hops = 0;
    jc->link_slot = NULL;
    jc->dgen = cpu->bus->direct_gen;
    jc->next_when = uam_sched_next(x->sched);
    cpu->icount_stop = cpu->exit_request ? 0 : cpu->icount_deadline;
    ((void (*)(uam_jit_ctx *))blk->backend)(jc);
    *res = jc->res;
    chain->hops = jc->hops;
    if (jc->link_slot && res->kind == UAM_BR_NEXT) {
        j->pend_slot = jc->link_slot;
        j->pend_pc = res->pc;
        j->pend_arena = j->arena_gen;
    }
}

#endif /* __x86_64__ */
