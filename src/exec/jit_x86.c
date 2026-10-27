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
 *   r12 = cpu->icount_deadline (fixed while native code runs)
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

typedef struct uam_jit_ctx {
    uam_cpu                *cpu;
    uint64_t               *temps;
    uint8_t                *regfile;
    const uam_block        *blk;
    const uam_helper_table *helpers;
    uint64_t                cur_pc;      /* current guest instruction (for faults) */
    uint64_t                parent_pc;   /* its parent, if a continuation */
    uint64_t                scratch;     /* bridge load destination */
    uint64_t                cont;        /* 1 if the current instruction is a continuation */
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
    r->pc = jc->cur_pc;
    r->exc.kind = uam_execops_fault_kind(st, store);
    r->exc.flags = jc->cont ? UAM_EXC_F_CONT : 0;
    r->exc.pc = jc->cur_pc;
    r->exc.parent_pc = jc->parent_pc;
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
    r->pc = jc->cur_pc;
    r->exc.kind = kind;
    r->exc.flags = jc->cont ? UAM_EXC_F_CONT : 0;
    r->exc.pc = jc->cur_pc;
    r->exc.parent_pc = jc->parent_pc;
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
enum { MK_PROLOGUE, MK_OP, MK_IMARK_PC, MK_SLOW, MK_CHAIN, MK_EPILOGUE, MK_SLOTS };
static const char *const mark_name[] = { "prologue", "op", "imark-pc", "slow", "chain",
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
/* mov [base + disp], reg32 (low 32 bits) */
static void st_mem32(uam_jit *j, int reg, int base, int32_t disp)
{
    rex(j, 0, reg >= 8, 0, base >= 8);
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
static void ld_temp(uam_jit *j, int reg, uint32_t idx) { ld_mem(j, reg, RBP, (int32_t)(8 * idx)); }
static void st_temp(uam_jit *j, uint32_t idx, int reg) { st_mem(j, reg, RBP, (int32_t)(8 * idx)); }

static void load_operand(uam_jit *j, int reg, const uam_block *blk, uam_ref r, unsigned w)
{
    if (uam_ref_is_imm(r))
        movabs(j, reg, blk->imm[uam_ref_index(r)] & uam_mask(w));
    else
        ld_temp(j, reg, r);
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
        movabs(j, RAX, pc);                                /* rax = pc */
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
    movabs(j, RAX, pc);
    st_mem(j, RAX, RBX, RES(pc));
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
    /* if (exit_request || icount >= deadline || !chain_left) goto epilogue */
    g8(j, 0x41); g8(j, 0x0F); g8(j, 0xB6); modrm_mem(j, RAX, R14, CPU(exit_request));
    rex(j, 1, 0, 0, 0); g8(j, 0x85); modrm_reg(j, RAX, RAX);       /* test rax, rax */
    epi[(*n_epi)++] = jcc_fwd(j, 0x85);                             /* jnz */
    g8(j, 0x4D); g8(j, 0x39); modrm_reg(j, R12, R13);               /* cmp r13, r12 */
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
 * Opcodes the native compiler emits inline. A block with any other op runs
 * interpreted. (Pure ops not listed here are emitted as a call to jit_br_eval.)
 */
static bool pure_inline(uam_opcode op)
{
    switch (op) {
    case UAM_OP_ADD: case UAM_OP_SUB: case UAM_OP_AND: case UAM_OP_OR: case UAM_OP_XOR:
    case UAM_OP_MUL: case UAM_OP_EQ: case UAM_OP_NE: case UAM_OP_ULT: case UAM_OP_ULE:
    case UAM_OP_SLT: case UAM_OP_SLE: case UAM_OP_ZEXT: case UAM_OP_SEXT: case UAM_OP_TRUNC:
    case UAM_OP_SELECT: case UAM_OP_SHL: case UAM_OP_LSHR: case UAM_OP_ASHR:
        return true;
    default:
        return false;
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

    if (!pure_inline(op)) {
        /* call jit_br_eval(jc, packed, a, b, c) */
        load_operand(j, RDX, blk, in->a, op == UAM_OP_SELECT ? 1 : src_w);
        if (n_src > 1) load_operand(j, RCX, blk, in->b, W); else movabs(j, RCX, 0);
        if (n_src > 2) load_operand(j, R8, blk, in->c, W); else movabs(j, R8, 0);
        mov_rr(j, RDI, RBX);
        movabs(j, RSI, (uint32_t)op | (W << 8) | (src_w << 16));
        movabs(j, RAX, (uint64_t)(uintptr_t)&jit_br_eval);
        g8(j, 0xFF); modrm_reg(j, 2, RAX);      /* call rax */
        mask_rax(j, W);
        st_temp(j, in->dst, RAX);
        return;
    }

    switch (op) {
    case UAM_OP_ADD: case UAM_OP_SUB: case UAM_OP_AND: case UAM_OP_OR:
    case UAM_OP_XOR: case UAM_OP_MUL: {
        load_operand(j, RAX, blk, in->a, W);
        load_operand(j, RCX, blk, in->b, W);
        rex(j, 1, 0, 0, 0);
        switch (op) {
        case UAM_OP_ADD: g8(j, 0x01); modrm_reg(j, RCX, RAX); break;
        case UAM_OP_SUB: g8(j, 0x29); modrm_reg(j, RCX, RAX); break;
        case UAM_OP_AND: g8(j, 0x21); modrm_reg(j, RCX, RAX); break;
        case UAM_OP_OR:  g8(j, 0x09); modrm_reg(j, RCX, RAX); break;
        case UAM_OP_XOR: g8(j, 0x31); modrm_reg(j, RCX, RAX); break;
        default:         g8(j, 0x0F); g8(j, 0xAF); modrm_reg(j, RAX, RCX); break; /* imul rax,rcx */
        }
        mask_rax(j, W);
        st_temp(j, in->dst, RAX);
        break;
    }
    case UAM_OP_EQ: case UAM_OP_NE: case UAM_OP_ULT: case UAM_OP_ULE:
    case UAM_OP_SLT: case UAM_OP_SLE: {
        load_operand(j, RAX, blk, in->a, W);
        load_operand(j, RCX, blk, in->b, W);
        if (op == UAM_OP_SLT || op == UAM_OP_SLE) { sext_reg(j, RAX, W); sext_reg(j, RCX, W); }
        rex(j, 1, 0, 0, 0); g8(j, 0x39); modrm_reg(j, RCX, RAX);   /* cmp rax, rcx */
        uint8_t cc = op == UAM_OP_EQ ? 0x94 : op == UAM_OP_NE ? 0x95 : op == UAM_OP_ULT ? 0x92
                   : op == UAM_OP_ULE ? 0x96 : op == UAM_OP_SLT ? 0x9C : 0x9E;
        g8(j, 0x0F); g8(j, cc); modrm_reg(j, 0, RAX);              /* setcc al */
        g8(j, 0x0F); g8(j, 0xB6); modrm_reg(j, RAX, RAX);          /* movzx eax, al */
        st_temp(j, in->dst, RAX);
        break;
    }
    case UAM_OP_ZEXT:
        load_operand(j, RAX, blk, in->a, src_w);
        mask_rax(j, W);
        st_temp(j, in->dst, RAX);
        break;
    case UAM_OP_TRUNC:
        load_operand(j, RAX, blk, in->a, src_w);
        mask_rax(j, W);
        st_temp(j, in->dst, RAX);
        break;
    case UAM_OP_SEXT:
        load_operand(j, RAX, blk, in->a, src_w);
        sext_reg(j, RAX, src_w);
        mask_rax(j, W);
        st_temp(j, in->dst, RAX);
        break;
    case UAM_OP_SELECT: {
        load_operand(j, RAX, blk, in->c, W);          /* default = c */
        load_operand(j, RCX, blk, in->b, W);          /* b */
        load_operand(j, RDX, blk, in->a, 1);          /* cond */
        g8(j, 0x84); modrm_reg(j, RDX, RDX);          /* test dl, dl */
        rex(j, 1, 0, 0, 0); g8(j, 0x0F); g8(j, 0x45); modrm_reg(j, RAX, RCX); /* cmovne rax, rcx */
        mask_rax(j, W);
        st_temp(j, in->dst, RAX);
        break;
    }
    case UAM_OP_SHL: case UAM_OP_LSHR: case UAM_OP_ASHR: {
        if (!uam_ref_is_imm(in->b)) {   /* variable amount: use the C evaluator */
            load_operand(j, RDX, blk, in->a, W);
            load_operand(j, RCX, blk, in->b, W);
            movabs(j, R8, 0);
            mov_rr(j, RDI, RBX);
            movabs(j, RSI, (uint32_t)op | (W << 8) | (W << 16));
            movabs(j, RAX, (uint64_t)(uintptr_t)&jit_br_eval);
            g8(j, 0xFF); modrm_reg(j, 2, RAX);
            mask_rax(j, W);
            st_temp(j, in->dst, RAX);
            break;
        }
        uint64_t n = blk->imm[uam_ref_index(in->b)];   /* any amount >= W shifts everything out */
        if (n >= W) {
            if (op == UAM_OP_ASHR) {
                load_operand(j, RAX, blk, in->a, W);
                sext_reg(j, RAX, W);
                rex(j, 1, 0, 0, 0); g8(j, 0xC1); modrm_reg(j, 7, RAX); g8(j, 63); /* sar rax, 63 */
                mask_rax(j, W);
            } else {
                movabs(j, RAX, 0);
            }
            st_temp(j, in->dst, RAX);
            break;
        }
        load_operand(j, RAX, blk, in->a, W);
        if (op == UAM_OP_ASHR)
            sext_reg(j, RAX, W);
        rex(j, 1, 0, 0, 0); g8(j, 0xC1);
        modrm_reg(j, op == UAM_OP_SHL ? 4 : op == UAM_OP_LSHR ? 5 : 7, RAX);
        g8(j, (uint8_t)n);
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
    ld_mem(j, R12, R14, CPU(icount_deadline));     /* r12 = deadline (constant per block) */
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
                    if (!uam_ref_is_none(in->dst)) { movabs(j, RAX, 0); st_temp(j, in->dst, RAX); }
                    continue;   /* guard false: op does nothing */
                }
                /* guard true: fall through unguarded */
            } else {
                ld_temp(j, RAX, in->guard);
                rex(j, 1, 0, 0, 0); g8(j, 0x85); modrm_reg(j, RAX, RAX);  /* test rax, rax */
                guard_skip = jcc_fwd(j, 0x84);    /* jz skip */
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
                } else {
                    /* if (exit_request) goto INT  (exit_request can be set async by a device) */
                    g8(j, 0x41); g8(j, 0x0F); g8(j, 0xB6); modrm_mem(j, RAX, R14, CPU(exit_request));
                    rex(j, 1, 0, 0, 0); g8(j, 0x85); modrm_reg(j, RAX, RAX);
                    size_t to_int = jcc_fwd(j, 0x85);                 /* jnz INT */
                    /* if (icount < deadline) goto AFTER  (both in registers) */
                    g8(j, 0x4D); g8(j, 0x39); modrm_reg(j, R12, R13); /* cmp r13, r12 */
                    size_t to_after = jcc_fwd(j, 0x82);               /* jb AFTER */
                    patch(j, to_int);
                    set_res_pc_imm(j, UAM_BR_INTERRUPTED, pc);
                    epi[n_epi++] = jmp_fwd(j);
                    patch(j, to_after);
                }
            }
            first = false;
            /* cur_pc/parent_pc/cont are read only when an op in this guest instruction can
             * fault or traps (jit_fault / TRAP). Store them only then, so arithmetic and
             * branch instructions - the common case - pay nothing here. */
            bool needs_pc = false;
            for (uint32_t k = i + 1; k < blk->n_insn; k++) {
                uam_opcode o2 = (uam_opcode)blk->insn[k].op;
                if (o2 == UAM_OP_IMARK) {
                    /* A group leader covers its continuation instructions (delay slots): a
                     * fault there needs this instruction's pc as the parent. Keep scanning
                     * across CONT imarks; stop at the next independent instruction. */
                    if (cont || !(blk->insn[k].flags & UAM_IMARK_CONT))
                        break;
                    continue;
                }
                /* a helper may fault (its descriptor says; any one is assumed to) */
                if ((uam_opinfo_table[o2].props & UAM_P_FAULT) || o2 == UAM_OP_TRAP ||
                    o2 == UAM_OP_HELPER) {
                    needs_pc = true;
                    break;
                }
            }
            mark(j, MK_IMARK_PC, i);
            if (needs_pc) {
                movabs(j, RAX, pc);
                st_mem(j, RAX, RBX, OFF(cur_pc));
                if (!cont)
                    st_mem(j, RAX, RBX, OFF(parent_pc));
                movabs(j, RAX, cont ? 1 : 0);
                st_mem(j, RAX, RBX, OFF(cont));
            }
            g8(j, 0x49); g8(j, 0xFF); modrm_reg(j, 0, R13); /* inc r13 (icount) */
            break;
        }
        case UAM_OP_GET:
            if (W == 1) {
                movabs(j, RAX, 0);
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
            if (W != 1) {
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
            mov_rr(j, RDI, RBX);                        /* miss / slow path: the bridge */
            movabs(j, RDX, in->flags);
            movabs(j, RCX, size);
            movabs(j, RAX, (uint64_t)(uintptr_t)&jit_br_load);
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
                mov_rr(j, RDI, RBX);
                load_operand(j, RSI, blk, in->a, aw);
                load_operand(j, RDX, blk, in->b, W);
                movabs(j, RCX, in->flags);
                movabs(j, R8, W / 8);
                movabs(j, RAX, (uint64_t)(uintptr_t)&jit_br_store_x);
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
            mov_rr(j, RDI, RBX);
            load_operand(j, RDX, blk, in->b, W);        /* reload: the bridge call needs it in rdx */
            movabs(j, RCX, in->flags);
            movabs(j, R8, size);
            movabs(j, RAX, (uint64_t)(uintptr_t)&jit_br_store);
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
            mov_rr(j, RDI, RBX);
            load_operand(j, RSI, blk, in->a, aw);       /* addr */
            load_operand(j, RDX, blk, in->b, W);        /* operand / expected */
            if (cas)
                load_operand(j, RCX, blk, in->c, W);    /* new value */
            else
                movabs(j, RCX, 0);
            movabs(j, R8, in->flags);
            movabs(j, R9, (uint32_t)op | (cas ? 0 : (uint32_t)in->aux << 8) | (uint32_t)W << 16);
            movabs(j, RAX, (uint64_t)(uintptr_t)&jit_br_rmw);
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
            mov_rr(j, RDI, RBX);
            movabs(j, RSI, in->aux);
            movabs(j, RAX, (uint64_t)(uintptr_t)&jit_br_sysrd);
            g8(j, 0xFF); modrm_reg(j, 2, RAX);
            g8(j, 0x85); modrm_reg(j, RAX, RAX);
            epi[n_epi++] = jcc_fwd(j, 0x85);          /* jnz epilogue (fault) */
            ld_mem(j, RAX, RBX, OFF(scratch));
            mask_rax(j, W);
            st_temp(j, in->dst, RAX);
            break;
        case UAM_OP_SYSWR:
            st_mem(j, R13, R14, CPU(icount));
            mov_rr(j, RDI, RBX);
            movabs(j, RSI, in->aux);
            load_operand(j, RDX, blk, in->a, W);
            movabs(j, RAX, (uint64_t)(uintptr_t)&jit_br_syswr);
            g8(j, 0xFF); modrm_reg(j, 2, RAX);
            g8(j, 0x85); modrm_reg(j, RAX, RAX);
            epi[n_epi++] = jcc_fwd(j, 0x85);
            break;
        case UAM_OP_HELPER: {
            const uam_ref args[3] = { in->a, in->b, in->c };
            const int regs[3] = { RDX, RCX, R8 };
            st_mem(j, R13, R14, CPU(icount));
            for (unsigned k = 0; k < 3; k++) {
                if (uam_ref_is_none(args[k]))
                    movabs(j, regs[k], 0);
                else
                    load_operand(j, regs[k], blk, args[k], 64);
            }
            mov_rr(j, RDI, RBX);
            movabs(j, RSI, in->aux);
            movabs(j, RAX, (uint64_t)(uintptr_t)&jit_br_helper);
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
            ld_mem(j, RAX, RBX, OFF(cur_pc));   st_mem(j, RAX, RBX, RES(pc));
            st_mem(j, RAX, RBX, REXC(pc));
            ld_mem(j, RAX, RBX, OFF(parent_pc)); st_mem(j, RAX, RBX, REXC(parent_pc));
            ld_mem(j, RAX, RBX, OFF(cont));      st_mem32(j, RAX, RBX, REXC(flags));
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
                movabs(j, RAX, 0);
                st_temp(j, in->dst, RAX);
                patch(j, over);
            } else {
                patch(j, guard_skip);
            }
        }
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
