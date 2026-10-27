/*
 * run.c - The hart run loop (uam/exec.h).
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "uam/exec.h"

#include "jit.h"

#define DEFAULT_MAX_BLOCK_INSNS 32u

const char *uam_stop_reason_str(uam_stop_reason r)
{
    switch (r) {
    case UAM_STOP_NONE:       return "running";
    case UAM_STOP_REQUEST:    return "stop requested";
    case UAM_STOP_HALT:       return "halt";
    case UAM_STOP_DEBUG:      return "debug trap";
    case UAM_STOP_INSN_LIMIT: return "instruction limit reached";
    case UAM_STOP_DEADLOCK:   return "deadlock (waiting with no interrupt source)";
    case UAM_STOP_ERROR:      return "internal error";
    case UAM_STOP_FAULT_LOOP: return "fetch fault loop (exception handler not fetchable)";
    case UAM_STOP_HOOK:       return "stopped by a pc hook";
    case UAM_STOP_RESET:      return "reset requested (no reset handler)";
    case UAM_STOP_TIME_LIMIT: return "virtual time limit reached";
    case UAM_STOP_LIVELOCK:   return "livelock (timers keep firing, the hart never wakes)";
    case UAM_STOP_EXC_HOOK:   return "stopped by the exception observer";
    case UAM_STOP_IRQ_HOOK:   return "stopped by the interrupt observer";
    }
    return "?";
}

/* Bus hook: the running block may just have overwritten its own code, and blocks
 * on that page are stale (so are JIT links into them). */
/* uam_tcache_remap_fn: do the block's virtual pages still map where its code
 * was fetched from? (The first instruction's page, and the next page for a
 * block that crosses into it.) */
static bool block_still_mapped(void *ctx, const uam_block *blk)
{
    uam_exec *x = ctx;
    uam_cpu *cpu = x->cpu;
    if (!cpu->isa->translate || blk->n_pages == 0)
        return true;
    uint64_t va = blk->guest_pc & ~uam_isa_pc_state_bits(cpu->isa);
    for (uint32_t i = 0; i < blk->n_pages; i++) {
        uint64_t v = i == 0 ? va : (va & UAM_PAGE_MASK) + UAM_PAGE_SIZE;
        uam_xlate xl = { 0 };
        if (cpu->isa->translate(cpu, v, UAM_XLATE_FETCH, &xl) != UAM_OK
            || (xl.paddr & UAM_PAGE_MASK) != blk->page[i])
            return false;
    }
    return true;
}

static void on_code_write(void *ctx, uint64_t page)
{
    (void)page;
    uam_exec *x = ctx;
    x->tc.gen++;
    uam_cpu_request_exit(x->cpu);
}

uam_status uam_exec_init(uam_exec *x, uam_cpu *cpu, uam_sched *sched)
{
    memset(x, 0, sizeof(*x));
    x->cpu = cpu;
    x->sched = sched;
    x->max_block_insns = DEFAULT_MAX_BLOCK_INSNS;
    x->verify = true;
    x->optimize = true;
    x->chain = true;
    x->trace_out = stderr;
    x->host.poll_interval = 1024;
    x->max_stall = UAM_EXEC_DEFAULT_MAX_STALL;
    uam_builder_init(&x->b, cpu->isa->addr_width);
    uam_bus_set_code_write_hook(cpu->bus, on_code_write, x);
    uam_status st = uam_tcache_init(&x->tc, cpu->bus);
    x->tc.remap = block_still_mapped;
    x->tc.remap_ctx = x;
    return st;
}

void uam_exec_fini(uam_exec *x)
{
    uam_jit_destroy(x->jit);
    x->jit = NULL;
    uam_tcache_fini(&x->tc);
    uam_builder_free(&x->b);
    free(x->temps);
    x->temps = NULL;
    free(x->hooks);
    x->hooks = NULL;
    x->n_hooks = x->cap_hooks = 0;
    free(x->passes);
    x->passes = NULL;
    x->n_passes = x->cap_passes = 0;
    uam_cov_pass_destroy(x->cov_pass);
    x->cov_pass = NULL;
    x->cpu->cov = NULL;
}

bool uam_exec_enable_jit(uam_exec *x)
{
    if (!x->jit)
        x->jit = uam_jit_create(x);
    x->use_jit = x->jit != NULL;
    if (x->use_jit) {
        uam_cpu_dtlb_flush(x->cpu);
        x->seen_dgen = x->cpu->bus->direct_gen;
        x->seen_xgen = x->cpu->xlate_gen;
    }
    uam_tcache_flush(&x->tc);   /* recompile everything through the new backend */
    return x->use_jit;
}

void uam_exec_stop(uam_exec *x, int64_t status)
{
    x->stop_request = true;
    x->stop_status = status;
    uam_cpu_request_exit(x->cpu);
}

void uam_exec_request_reset(uam_exec *x, uam_reset_kind kind)
{
    /* A system reset subsumes a CPU reset requested in the same window. */
    if (!x->reset_request || kind == UAM_RESET_SYSTEM)
        x->reset_kind = kind;
    x->reset_request = true;
    uam_cpu_request_exit(x->cpu);
}

void uam_exec_set_reset_handler(uam_exec *x, uam_reset_fn fn, void *ctx)
{
    x->reset_fn = fn;
    x->reset_ctx = ctx;
}

void uam_exec_set_host(uam_exec *x, const uam_exec_host *host)
{
    if (host)
        x->host = *host;
    else
        memset(&x->host, 0, sizeof(x->host));
    if (!x->host.poll_interval)
        x->host.poll_interval = 1024;
}

/* ---- PC hooks ---------------------------------------------------------- */

static unsigned hook_bit(uint64_t pc)
{
    return (unsigned)(((pc >> 1) * UINT64_C(0x9E3779B97F4A7C15)) >> 54); /* 0..1023 */
}

bool uam_exec_has_pc_hook(const uam_exec *x, uint64_t pc)
{
    if (!x->n_hooks)
        return false;
    unsigned b = hook_bit(pc);
    if (!(x->hook_filter[b >> 6] & (UINT64_C(1) << (b & 63))))
        return false;
    for (uint32_t i = 0; i < x->n_hooks; i++)
        if (x->hooks[i].pc == pc)
            return true;
    return false;
}

static void rebuild_hook_filter(uam_exec *x)
{
    memset(x->hook_filter, 0, sizeof(x->hook_filter));
    for (uint32_t i = 0; i < x->n_hooks; i++) {
        unsigned b = hook_bit(x->hooks[i].pc);
        x->hook_filter[b >> 6] |= UINT64_C(1) << (b & 63);
    }
}

uam_status uam_exec_add_pc_hook(uam_exec *x, uint64_t pc, uam_pc_hook_fn fn, void *ctx)
{
    if (!fn)
        return UAM_ERR_INVALID;
    if (x->n_hooks == x->cap_hooks) {
        uint32_t ncap = x->cap_hooks ? x->cap_hooks * 2 : 8;
        uam_pc_hook *nh = realloc(x->hooks, sizeof(*nh) * ncap);
        if (!nh)
            return UAM_ERR_NOMEM;
        x->hooks = nh;
        x->cap_hooks = ncap;
    }
    x->hooks[x->n_hooks++] = (uam_pc_hook){ .pc = pc, .fn = fn, .ctx = ctx };
    rebuild_hook_filter(x);
    uam_tcache_flush(&x->tc);  /* blocks must now end before pc */
    uam_cpu_request_exit(x->cpu);
    return UAM_OK;
}

bool uam_exec_remove_pc_hook(uam_exec *x, uint64_t pc, uam_pc_hook_fn fn, void *ctx)
{
    for (uint32_t i = 0; i < x->n_hooks; i++) {
        uam_pc_hook *h = &x->hooks[i];
        if (h->pc == pc && h->fn == fn && h->ctx == ctx) {
            memmove(h, h + 1, sizeof(*h) * (x->n_hooks - i - 1));
            x->n_hooks--;
            rebuild_hook_filter(x);
            uam_tcache_flush(&x->tc);
            return true;
        }
    }
    return false;
}

/* ---- IR passes ---------------------------------------------------------- */

uam_status uam_exec_add_pass(uam_exec *x, const uam_pass *p)
{
    if (!p || !p->run)
        return UAM_ERR_INVALID;
    for (uint32_t i = 0; i < x->n_passes; i++)
        if (x->passes[i] == p)
            return UAM_ERR_INVALID;
    if (x->n_passes == x->cap_passes) {
        uint32_t ncap = x->cap_passes ? x->cap_passes * 2 : 4;
        const uam_pass **np = realloc(x->passes, sizeof(*np) * ncap);
        if (!np)
            return UAM_ERR_NOMEM;
        x->passes = np;
        x->cap_passes = ncap;
    }
    x->passes[x->n_passes++] = p;
    uam_tcache_flush(&x->tc);   /* every block must go through the new pass */
    uam_cpu_request_exit(x->cpu);
    return UAM_OK;
}

bool uam_exec_remove_pass(uam_exec *x, const uam_pass *p)
{
    for (uint32_t i = 0; i < x->n_passes; i++) {
        if (x->passes[i] == p) {
            memmove(&x->passes[i], &x->passes[i + 1], sizeof(*x->passes) * (x->n_passes - i - 1));
            x->n_passes--;
            uam_tcache_flush(&x->tc);
            uam_cpu_request_exit(x->cpu);
            return true;
        }
    }
    return false;
}

/* Run the registered passes over a new block. A pass may only insert ops, so the guest
 * instruction count must survive it; the block is verified after each pass. */
static uam_status run_passes(uam_exec *x, uam_block *blk)
{
    const uam_isa *isa = x->cpu->isa;
    for (uint32_t i = 0; i < x->n_passes; i++) {
        const uam_pass *p = x->passes[i];
        const uint32_t n_guest = blk->n_guest;
        uam_status st = p->run(p->ctx, blk);
        if (st != UAM_OK) {
            uam_log(UAM_LOG_ERROR, "exec", "pass '%s' failed on the block at 0x%llx: %s",
                    p->name, (unsigned long long)blk->guest_pc, uam_status_str(st));
            return st;
        }
        char err[256] = "";
        bool ok = blk->n_guest == n_guest;
        if (!ok)
            snprintf(err, sizeof(err), "guest instruction count changed (%u -> %u)",
                     n_guest, blk->n_guest);
        if (ok && x->verify) {
            uam_verify_ctx vc = { .regfile_size = isa->regfile_size, .helpers = &isa->helpers,
                                  .max_guest_insns = x->max_block_insns };
            ok = uam_verify(blk, &vc, err, sizeof(err));
        }
        if (!ok) {
            uam_print_ctx pc_ctx = { .regs = isa->regs, .n_regs = isa->n_regs,
                                     .helpers = &isa->helpers };
            uam_log(UAM_LOG_ERROR, "exec", "pass '%s' produced an invalid block at 0x%llx: %s",
                    p->name, (unsigned long long)blk->guest_pc, err);
            uam_print_block(stderr, blk, &pc_ctx);
            return UAM_ERR_INVALID;
        }
    }
    return UAM_OK;
}

/* Mark the IMARKs of hooked instructions so the block stops before them. */
static void mark_hooked_imarks(uam_exec *x, uam_block *blk)
{
    for (uint32_t i = 1; i < blk->n_insn; i++) {
        uam_insn *in = &blk->insn[i];
        if (in->op == UAM_OP_IMARK && !(in->flags & UAM_IMARK_CONT)
            && uam_exec_has_pc_hook(x, blk->imm[uam_ref_index(in->a)]))
            in->flags |= UAM_IMARK_BREAK;
    }
}

/* Run the hooks registered for cpu->pc. Returns true to stop. */
static bool run_hooks(uam_exec *x)
{
    uam_cpu *cpu = x->cpu;
    const uint64_t pc = cpu->pc;
    x->hook_fired = true;
    x->hook_fired_pc = pc;
    x->hook_fired_icount = cpu->icount;
    bool stop = false;
    /* Iterate by index: a hook may add or remove hooks. */
    for (uint32_t i = 0; i < x->n_hooks && cpu->pc == pc; i++) {
        if (x->hooks[i].pc != pc)
            continue;
        x->stats.hooks++;
        if (x->hooks[i].fn(x->hooks[i].ctx, cpu, pc) == UAM_HOOK_STOP)
            stop = true;
    }
    return stop;
}

/* ---- translation ------------------------------------------------------- */

/* Per-translation fetch bookkeeping (uam_lift_ctx.core). */
typedef struct lift_core {
    uam_exec *x;
    bool      mapped;     /* some code came through a non-fixed mapping */
    uint64_t  fault_addr; /* first failing fetch address */
    uam_status fault_st;  /* its status (UAM_ERR_FAULT / UAM_ERR_XLATE) */
    uint32_t  fault_xf;   /* its translation fault sub-cause */
} lift_core;

static uam_status fetch_xlate(lift_core *lc, uint64_t addr, uint64_t *pa)
{
    uam_cpu *cpu = lc->x->cpu;
    if (!cpu->isa->translate) {
        *pa = addr;
        return UAM_OK;
    }
    uam_xlate xl = { 0 };
    uam_status st = cpu->isa->translate(cpu, addr, UAM_XLATE_FETCH, &xl);
    if (st != UAM_OK) {
        if (lc->fault_st == UAM_OK) {
            lc->fault_st = st;
            lc->fault_addr = addr;
            lc->fault_xf = xl.fault;
        }
        return st;
    }
    lc->mapped |= !xl.fixed;
    *pa = xl.paddr;
    return UAM_OK;
}

static uam_status lift_fetch(uam_lift_ctx *lc, uint64_t addr, unsigned size, uint64_t *val)
{
    lift_core *core = lc->core;
    uam_exec *x = core->x;
    const uam_isa *isa = x->cpu->isa;
    uint64_t pa0, pa1;
    /* Translate both ends of the access when they lie on different pages; code
     * pages are tracked physically so that self-modifying-code invalidation
     * (which sees physical stores) and instruction fetch agree. */
    uam_status st = fetch_xlate(core, addr, &pa0);
    pa1 = pa0 + size - 1;
    if (st == UAM_OK && ((addr ^ (addr + size - 1)) & UAM_PAGE_MASK))
        st = fetch_xlate(core, addr + size - 1, &pa1);
    if (st != UAM_OK)
        return UAM_ERR_FAULT;
    if (!uam_builder_add_page(lc->b, pa0 & UAM_PAGE_MASK)
        || !uam_builder_add_page(lc->b, pa1 & UAM_PAGE_MASK))
        return UAM_ERR_INVALID;
    const unsigned order = isa->fetch_endian ? isa->fetch_endian : isa->default_endian;
    st = uam_bus_read(x->cpu->bus, pa0, size, order | UAM_ACC_FETCH, val);
    if (st != UAM_OK && core->fault_st == UAM_OK) {
        core->fault_st = UAM_ERR_FAULT;
        core->fault_addr = addr;
    }
    return st == UAM_OK ? UAM_OK : UAM_ERR_FAULT;
}

static uam_print_ctx print_ctx(const uam_isa *isa)
{
    return (uam_print_ctx){ .regs = isa->regs, .n_regs = isa->n_regs, .helpers = &isa->helpers };
}

/* Translate the block at pc. On failure returns NULL and sets *st; a fetch
 * fault on the first instruction is described in *exc. */
/*
 * Mark the bytes a block was translated from as code, so that only writes to
 * them (at the bus's 64-byte line granularity) drop it. The guest bytes run
 * contiguously from the first IMARK's pc (less the ISA's pc state bits) to the end
 * of the last instruction; they lie on page[0] and, past its end, page[1].
 * Anything else marks the whole pages.
 */
static void mark_block_code(uam_bus *bus, const uam_block *blk, uint64_t state_bits)
{
    uint64_t lo = UINT64_MAX, hi = 0;
    for (uint32_t i = 0; i < blk->n_insn; i++) {
        const uam_insn *in = &blk->insn[i];
        if (in->op != UAM_OP_IMARK || !uam_ref_is_imm(in->a))
            continue;
        uint64_t pc = blk->imm[uam_ref_index(in->a)] & ~state_bits;
        if (pc < lo) lo = pc;
        if (pc + in->aux > hi) hi = pc + in->aux;
    }
    uint64_t first_room = lo == UINT64_MAX ? 0 : UAM_PAGE_SIZE - (lo & (UAM_PAGE_SIZE - 1));
    if (lo == UINT64_MAX || hi <= lo || blk->n_pages == 0 ||
        (blk->n_pages == 1 && hi - lo > first_room) ||
        (blk->n_pages == 2 && hi - lo <= first_room)) {
        for (uint32_t i = 0; i < blk->n_pages; i++)
            uam_bus_mark_code(bus, blk->page[i]);
        return;
    }
    uint64_t len = hi - lo, n0 = len < first_room ? len : first_room;
    uam_bus_mark_code_range(bus, blk->page[0] + (lo & (UAM_PAGE_SIZE - 1)), n0);
    if (blk->n_pages == 2)
        uam_bus_mark_code_range(bus, blk->page[1], len - n0);
}

static uam_block *translate(uam_exec *x, uint64_t pc, uam_status *st, uam_exception *exc)
{
    uam_cpu *cpu = x->cpu;
    const uam_isa *isa = cpu->isa;

    uam_builder_begin(&x->b, pc);
    lift_core core = { .x = x, .fault_st = UAM_OK };
    uam_lift_ctx lc = { .cpu = cpu, .b = &x->b, .pc = pc,
                        .max_insns = x->max_block_insns, .fetch = lift_fetch, .core = &core };
    *st = isa->lift(&lc);
    if (*st != UAM_OK) {
        if (*st == UAM_ERR_FAULT) {
            bool xl = core.fault_st == UAM_ERR_XLATE;
            *exc = (uam_exception){ .kind = xl ? UAM_EXC_FETCH_XLATE : UAM_EXC_FETCH_FAULT,
                                    .pc = pc, .parent_pc = pc,
                                    .cause = xl ? core.fault_xf : 0,
                                    .tval = core.fault_st != UAM_OK ? core.fault_addr : pc };
        }
        return NULL;
    }

    uam_block *blk = uam_builder_finish(&x->b);
    if (!blk) {
        *st = UAM_ERR_NOMEM;
        return NULL;
    }
    x->stats.ir_ops_lifted += blk->n_insn;
    mark_block_code(cpu->bus, blk, uam_isa_pc_state_bits(cpu->isa));
    for (uint32_t i = 0; i < blk->n_pages; i++)
        blk->page_gen[i] = uam_bus_page_gen(cpu->bus, blk->page[i]);
    /* A page that just became a code page must lose its JIT data-TLB write entry before
     * this block runs, or a store to it (the block's own page, say) takes the inline
     * path past the code-write hook: no invalidation, and the block is not ended. */
    if (x->jit && cpu->bus->direct_gen != x->seen_dgen) {
        uam_cpu_dtlb_flush(cpu);
        x->seen_dgen = cpu->bus->direct_gen;
    }

    uam_print_ctx pc_ctx = print_ctx(isa);
    if (x->verify) {
        char err[256];
        uam_verify_ctx vc = { .regfile_size = isa->regfile_size, .helpers = &isa->helpers,
                              .max_guest_insns = x->max_block_insns };
        if (!uam_verify(blk, &vc, err, sizeof(err))) {
            uam_log(UAM_LOG_ERROR, "exec", "lifter produced an invalid block at 0x%llx: %s",
                    (unsigned long long)pc, err);
            uam_print_block(stderr, blk, &pc_ctx);
            uam_block_free(blk);
            *st = UAM_ERR_INVALID;
            return NULL;
        }
    }
    if (x->optimize) {
        if (!uam_optimize(blk)) {
            uam_block_free(blk);
            *st = UAM_ERR_NOMEM;
            return NULL;
        }
        if (x->verify) {
            char err[256];
            uam_verify_ctx vc = { .regfile_size = isa->regfile_size, .helpers = &isa->helpers,
                                  .max_guest_insns = x->max_block_insns };
            if (!uam_verify(blk, &vc, err, sizeof(err))) {
                uam_log(UAM_LOG_ERROR, "exec", "optimizer produced an invalid block at 0x%llx: %s",
                        (unsigned long long)pc, err);
                uam_print_block(stderr, blk, &pc_ctx);
                uam_block_free(blk);
                *st = UAM_ERR_INVALID;
                return NULL;
            }
        }
    }
    if (x->n_passes) {
        *st = run_passes(x, blk);
        if (*st != UAM_OK) {
            uam_block_free(blk);
            return NULL;
        }
    }
    if (x->n_hooks)
        mark_hooked_imarks(x, blk);
    x->stats.ir_ops_final += blk->n_insn;
    /* A block with a PC hook on a non-first instruction is broken at that IMARK and
     * must stop there; the JIT honours UAM_IMARK_BREAK, so it can still run it. */
    if (x->use_jit && x->jit) {
        if (!uam_jit_compile(x->jit, blk)) {
            uam_block_free(blk);
            *st = UAM_ERR_NOMEM;
            return NULL;
        }
        if (blk->backend == UAM_JIT_INTERP) x->stats.blocks_interp++; else x->stats.blocks_compiled++;
    }
    if (x->trace & UAM_TRACE_IR)
        uam_print_block(x->trace_out, blk, &pc_ctx);

    if (blk->n_temp > x->temps_cap) {
        uint32_t ncap = blk->n_temp * 2;
        uint64_t *nt = realloc(x->temps, sizeof(uint64_t) * ncap);
        if (!nt) {
            uam_block_free(blk);
            *st = UAM_ERR_NOMEM;
            return NULL;
        }
        x->temps = nt;
        x->temps_cap = ncap;
    }
    /* A fetch that failed during the lift (the block stops short of it) also
     * depends on the current mappings: such a block is not revalidated. */
    *st = uam_tcache_insert_ex(&x->tc, blk, cpu->priv, core.mapped, cpu->xlate_gen,
                               core.fault_st == UAM_OK);
    if (*st != UAM_OK) {
        uam_block_free(blk);
        return NULL;
    }
    x->stats.blocks_translated++;
    return blk;
}

/* ---- run loop ---------------------------------------------------------- */

/* Take a synchronous exception. Returns false if the observer stopped the run
 * instead (the exception is not delivered; cpu->pc is where it would restart). */
static bool deliver(uam_exec *x, const uam_exception *exc)
{
    if (x->trace & UAM_TRACE_EXC)
        fprintf(x->trace_out, x->cpu->hartid ? "exc: kind=%d pc=0x%llx cause=0x%llx tval=0x%llx cpu=%u\n"
                                             : "exc: kind=%d pc=0x%llx cause=0x%llx tval=0x%llx\n",
                exc->kind, (unsigned long long)exc->pc, (unsigned long long)exc->cause,
                (unsigned long long)exc->tval, x->cpu->hartid);
    if (x->exc_hook) {
        x->cpu->pc = (exc->flags & UAM_EXC_F_CONT) ? exc->parent_pc : exc->pc;
        if (x->exc_hook(x->exc_hook_ctx, x->cpu, exc) == UAM_HOOK_STOP)
            return false;
    }
    x->stats.exceptions++;
    x->cpu->isa->take_exception(x->cpu, exc);
    return true;
}

void uam_exec_set_exc_hook(uam_exec *x, uam_exc_hook_fn fn, void *ctx)
{
    x->exc_hook = fn;
    x->exc_hook_ctx = fn ? ctx : NULL;
}

void uam_exec_set_irq_hook(uam_exec *x, uam_irq_hook_fn fn, void *ctx)
{
    x->irq_hook = fn;
    x->irq_hook_ctx = fn ? ctx : NULL;
}

static bool trace_block(const uam_exec *x, uint64_t pc)
{
    return (x->trace & UAM_TRACE_BLOCKS) && x->cpu->icount >= x->trace_after
        && pc >= x->trace_lo && (x->trace_hi == 0 || pc < x->trace_hi);
}

/* Virtual time at which a run stops with UAM_STOP_TIME_LIMIT. */
static uint64_t time_limit(const uam_exec *x)
{
    return x->max_time && x->max_time < UAM_SCHED_TIME_MAX ? x->max_time : UAM_SCHED_TIME_MAX;
}

/* Stall rounds (timers fired or time advanced) in a row with no instruction
 * retired; the count restarts whenever one has. */
static uint64_t stall_rounds(uam_exec *x)
{
    uint64_t progress = x->cpu->icount;
    for (uint32_t i = 1; i < x->n_smp; i++)
        progress += x->smp[i]->cpu->icount;   /* every processor's (saved in snapshots) */
    if (progress != x->stall_icount) {
        x->stall_icount = progress;
        x->stall = 0;
    }
    return x->stall;
}

static bool stalled(const uam_exec *x)
{
    return x->max_stall && x->stall >= x->max_stall;
}

/* The hart is idle: advance virtual time (never past tlimit, which is above
 * now), or block on host input. Returns UAM_STOP_NONE to keep running. */
static uam_stop_reason idle(uam_exec *x, uint64_t tlimit)
{
    uint64_t next = uam_sched_next(x->sched), now = uam_sched_now(x->sched);
    uint64_t ticks = next == UINT64_MAX ? UINT64_MAX : (next > now ? next - now : 0);
    uint64_t skip = ticks;
    stall_rounds(x);
    if (stalled(x)) {
        /* max_stall timer events have not woken the hart: only host input
         * can. Wait for it as if no timer were armed (no CPU spent; a signal
         * interrupts the wait), then give the timers another max_stall rounds,
         * since the input may need them to reach the hart. With nothing that
         * could deliver input this is a livelock, decided in virtual time. */
        if (!x->host.idle || !x->host.idle(x->host.ctx, UINT64_MAX, &skip))
            return UAM_STOP_LIVELOCK;
        x->stall = 0;
        if (skip > ticks)
            skip = ticks;
    } else if (x->host.idle) {
        if (!x->host.idle(x->host.ctx, ticks, &skip))
            return UAM_STOP_DEADLOCK;
        if (skip > ticks)
            skip = ticks;
    } else if (ticks == UINT64_MAX) {
        return UAM_STOP_DEADLOCK;
    }
    /* Reaching the next timer is a stall round: it fires, and if that cannot
     * wake the hart we are back here with nothing retired. Waiting on host
     * input alone (no timer) is not. */
    bool to_timer = ticks != UINT64_MAX && skip == ticks;
    if (skip == UINT64_MAX)
        skip = 0;
    if (skip > tlimit - now)
        skip = tlimit - now;
    if (skip) {
        uam_sched_skip_to(x->sched, now + skip);
        x->stats.idle_ticks += skip;
    }
    if (to_timer)
        x->stall++;
    return UAM_STOP_NONE;
}

/* Internal results of run_loop in a multiprocessor slice. */
#define STOP_SLICE ((uam_stop_reason)0x100)   /* the slice's instructions ran */
#define STOP_IDLE  ((uam_stop_reason)0x101)   /* the processor waits */
#define STOP_RESYNC ((uam_stop_reason)0x102)  /* a snapshot was restored: slice state is new */

/* Run x's processor until `limit` (its icount) or a stop. In a slice (smp),
 * reaching the limit is STOP_SLICE and waiting is STOP_IDLE: the caller
 * decides. Requests, the host bridge and resets are the controlling
 * executor's. */
static uam_stop_reason run_loop(uam_exec *x, uint64_t limit, uint64_t tlimit, bool smp)
{
    uam_cpu *cpu = x->cpu;
    const uam_isa *isa = cpu->isa;
    uam_exec *c = x->ctl ? x->ctl : x;

    for (;;) {
        if (c->stop_request)
            return UAM_STOP_REQUEST;
        if (smp && c->smp_resync)
            return STOP_RESYNC;

        /* 0. Reset requested (by a device, between instructions). */
        if (c->reset_request) {
            c->reset_request = false;
            if (!c->reset_fn)
                return UAM_STOP_RESET;
            c->stats.resets++;
            if (c->trace & UAM_TRACE_EXC)
                fprintf(c->trace_out, "reset: %s\n",
                        c->reset_kind == UAM_RESET_SYSTEM ? "system" : "cpu");
            c->reset_fn(c->reset_ctx, c->reset_kind);
            for (uint32_t i = 0; i < (c->n_smp ? c->n_smp : 1); i++) {
                uam_exec *e = c->n_smp ? c->smp[i] : c;
                if (e->jit)
                    uam_cpu_dtlb_flush(e->cpu);
                e->last_fetch_fault_valid = false;
                e->hook_fired = false;
            }
            continue;
        }
        if (cpu->icount >= limit)
            return smp ? STOP_SLICE : UAM_STOP_INSN_LIMIT;
        if (uam_sched_now(x->sched) >= tlimit)
            return UAM_STOP_TIME_LIMIT;

        /* Keep the JIT data-TLB coherent with the bus: any mapping, code-page or
         * ROMD change bumps bus->direct_gen, any TLB/ASID change bumps xlate_gen. */
        if (x->jit && (cpu->bus->direct_gen != x->seen_dgen || cpu->xlate_gen != x->seen_xgen)) {
            uam_cpu_dtlb_flush(cpu);
            x->seen_dgen = cpu->bus->direct_gen;
            x->seen_xgen = cpu->xlate_gen;
        }

        /* 1. Time: fire due timers. Host input is polled periodically. A timer
         * that keeps re-arming itself in the past fills the firing budget:
         * go round again, so that stop requests (signals) are still seen. */
        if (uam_sched_run_due(x->sched) == UAM_SCHED_MAX_FIRE
            && uam_sched_next(x->sched) <= uam_sched_now(x->sched)) {
            /* No host input is delivered in here: a storm is stuck either way. */
            x->stall = stall_rounds(x) + 1;
            if (stalled(x))
                return UAM_STOP_LIVELOCK;
            continue;
        }
        if (c->host.poll && ++c->blocks_since_poll >= c->host.poll_interval) {
            c->blocks_since_poll = 0;
            c->host.poll(c->host.ctx);
        }
        cpu->exit_request = false;

        /* 2. Interrupts: only ever taken between blocks. */
        const uint64_t from_pc = cpu->pc;
        if (isa->irq_take(cpu)) {
            x->stats.interrupts++;
            cpu->waiting = false;
            if (x->trace & UAM_TRACE_EXC)
                fprintf(x->trace_out, cpu->hartid ? "irq: entered handler at 0x%llx cpu=%u\n"
                                                  : "irq: entered handler at 0x%llx\n",
                        (unsigned long long)cpu->pc, cpu->hartid);
            if (x->irq_hook) {
                const uam_interrupt irq = { .from_pc = from_pc, .handler_pc = cpu->pc,
                                            .lines = isa->irq_lines ? isa->irq_lines(cpu) : 0 };
                if (x->irq_hook(x->irq_hook_ctx, cpu, &irq) == UAM_HOOK_STOP)
                    return UAM_STOP_IRQ_HOOK;
            }
        }

        /* 3. Idle: skip virtual time to the next event (or wait for the host). */
        if (cpu->waiting) {
            if (isa->irq_wakeup(cpu)) {
                cpu->waiting = false;
            } else {
                if (smp)
                    return STOP_IDLE;
                uam_stop_reason why = idle(x, tlimit);
                if (why != UAM_STOP_NONE)
                    return why;
                continue;
            }
        }

        /* 4. PC hooks, once per arrival at a hooked pc. */
        if (x->n_hooks && uam_exec_has_pc_hook(x, cpu->pc)
            && !(x->hook_fired && x->hook_fired_pc == cpu->pc
                 && x->hook_fired_icount == cpu->icount)) {
            uint64_t pc = cpu->pc;
            if (run_hooks(x))
                return UAM_STOP_HOOK;
            if (cpu->pc != pc || x->stop_request || x->reset_request || c->smp_resync)
                continue;
        }

        /* 5. Find or translate the block. */
        uam_block *blk = uam_tcache_lookup(&x->tc, cpu->pc, cpu->priv, cpu->xlate_gen);
        if (!blk) {
            uam_status st;
            uam_exception exc;
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            blk = translate(x, cpu->pc, &st, &exc);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            x->stats.translate_ns += (uint64_t)((t1.tv_sec - t0.tv_sec) * 1000000000LL
                                                + (t1.tv_nsec - t0.tv_nsec));
            if (!blk) {
                if (st == UAM_ERR_FAULT) {
                    /* The same fetch fault again without a single instruction
                     * in between: the handler itself is unfetchable and the
                     * hart can never make progress. */
                    if (x->last_fetch_fault_valid && x->last_fetch_fault_pc == cpu->pc
                        && x->last_fetch_fault_icount == cpu->icount)
                        return UAM_STOP_FAULT_LOOP;
                    x->last_fetch_fault_valid = true;
                    x->last_fetch_fault_pc = cpu->pc;
                    x->last_fetch_fault_icount = cpu->icount;
                    if (!deliver(x, &exc))
                        return UAM_STOP_EXC_HOOK;
                    continue;
                }
                uam_log(UAM_LOG_ERROR, "exec", "translation failed at 0x%llx: %s",
                        (unsigned long long)cpu->pc, uam_status_str(st));
                return UAM_STOP_ERROR;
            }
        }

        /* 6. Deadline: stop the block when the next timer comes due, or at
         * the time limit (now < tlimit <= UAM_SCHED_TIME_MAX: no overflow). */
        uint64_t next = uam_sched_next(x->sched), now = uam_sched_now(x->sched);
        uint64_t deadline = next == UINT64_MAX ? UINT64_MAX
                          : next <= now        ? cpu->icount
                                               : cpu->icount + (next - now);
        if (deadline - cpu->icount > tlimit - now)
            deadline = cpu->icount + (tlimit - now);
        cpu->icount_deadline = deadline < limit ? deadline : limit;

        if (x->trace && trace_block(x, blk->guest_pc))
            fprintf(x->trace_out, "blk 0x%llx icount=%llu\n", (unsigned long long)blk->guest_pc,
                    (unsigned long long)cpu->icount);

        /* 7. Execute (native if compiled, else interpreted) and act on the result.
         * Native code may run on into further blocks without coming back here
         * (jit.h, chaining) while nothing would make this loop act differently:
         * up to the next host poll, and never while tracing blocks. */
        uam_block_result r;
        if (blk->backend && blk->backend != UAM_JIT_INTERP) {
            uam_jit_chain ch = { .budget = 0, .link = false };
            if (x->chain && !(x->trace & UAM_TRACE_BLOCKS)) {
                ch.budget = !c->host.poll ? UINT32_C(1) << 30
                          : c->blocks_since_poll < c->host.poll_interval
                              ? c->host.poll_interval - 1 - c->blocks_since_poll : 0;
                ch.link = !(x->n_hooks && uam_exec_has_pc_hook(x, blk->guest_pc));
            }
            uam_jit_run(x->jit, blk, x->temps, &ch, &r);
            x->stats.blocks_native += 1 + ch.hops;
            x->stats.blocks_executed += ch.hops;
            x->stats.blocks_chained += ch.hops;
            if (c->host.poll)
                c->blocks_since_poll += ch.hops;
        }
        else
            r = uam_interp_block(cpu, blk, &isa->helpers, x->temps);
        x->stats.blocks_executed++;

        switch (r.kind) {
        case UAM_BR_NEXT:
        case UAM_BR_INTERRUPTED:
            cpu->pc = r.pc;
            break;
        case UAM_BR_WAIT:
            cpu->pc = r.pc;
            cpu->waiting = true;
            break;
        case UAM_BR_FAULT:
            if (!deliver(x, &r.exc))
                return UAM_STOP_EXC_HOOK;
            break;
        case UAM_BR_TRAP:
            switch (r.trap_class) {
            case UAM_TRAP_EXCEPTION:
                if (!deliver(x, &r.exc))
                    return UAM_STOP_EXC_HOOK;
                break;
            case UAM_TRAP_DEBUG:
                cpu->pc = r.pc;
                return UAM_STOP_DEBUG;
            case UAM_TRAP_HALT:
                cpu->pc = r.pc;
                x->stop_status = (int64_t)r.exc.cause;
                return UAM_STOP_HALT;
            }
            break;
        }
    }
}

/* ---- multiprocessor scheduling ------------------------------------------ */

/* Point the shared clock at `cpu` without moving it: now = icount + idle. */
static void clock_to(uam_sched *s, uam_cpu *cpu)
{
    uint64_t now = uam_sched_now(s);
    s->icount = &cpu->icount;
    s->idle = now - cpu->icount;   /* modulo 2^64 */
}

/* Bring the other processors' executors in line with processor 0's. */
static void sync_settings(uam_exec *x)
{
    for (uint32_t i = 1; i < x->n_smp; i++) {
        uam_exec *e = x->smp[i];
        e->max_block_insns = x->max_block_insns;
        e->verify = x->verify;
        e->optimize = x->optimize;
        e->chain = x->chain;
        e->trace = x->trace;
        e->trace_out = x->trace_out;
        e->trace_lo = x->trace_lo;
        e->trace_hi = x->trace_hi;
        e->trace_after = x->trace_after;
        if (x->use_jit && !e->use_jit)
            uam_exec_enable_jit(e);
    }
}

static uam_stop_reason run_smp(uam_exec *x, uint64_t max_insns)
{
    const uint64_t tlimit = time_limit(x);
    const uint64_t start = x->retired;
    const uint32_t q = x->quantum ? x->quantum : 4096;
    uam_sched *s = x->sched;
    uam_stop_reason why = UAM_STOP_NONE;
    sync_settings(x);

    while (why == UAM_STOP_NONE) {
        /* The rest of the round, from where the last run left it. */
        while (x->slice_cpu < x->n_smp && why == UAM_STOP_NONE) {
            const uint32_t i = x->slice_cpu;
            uam_exec *e = x->smp[i];
            uam_cpu *cpu = e->cpu;
            uint64_t left = max_insns ? max_insns - (x->retired - start) : UINT64_MAX;
            if (left == 0) {
                why = UAM_STOP_INSN_LIMIT;
                break;
            }
            if (!x->slice_left)
                x->slice_left = q;
            clock_to(s, cpu);
            x->running = cpu;
            if (i && cpu->isa->switch_in)
                cpu->isa->switch_in(cpu);
            uint64_t before = cpu->icount;
            x->slice_base = before;
            uam_stop_reason r = run_loop(e, before + (left < x->slice_left ? left : x->slice_left),
                                         tlimit, true);
            if (i && cpu->isa->switch_in)
                x->cpu->isa->switch_in(x->cpu);   /* the platform view is processor 0's between slices */
            if (r == STOP_RESYNC) {
                /* A hook restored a snapshot: every processor and slice_* are the
                 * snapshot's now. Carry on from there. */
                x->smp_resync = false;
                continue;
            }
            uint64_t ran = cpu->icount - before;
            x->retired += ran;
            x->round_ran |= ran != 0;
            x->slice_left = ran < x->slice_left ? x->slice_left - ran : 0;
            if (r == STOP_IDLE || !x->slice_left) {
                /* The slice is over and other processors run next: they may write
                 * what this one holds a reservation on, so it is lost (a
                 * store-conditional may always fail; the next attempt runs within
                 * one slice). Only here, so split runs interleave identically. */
                cpu->resv.valid = false;
                x->slice_cpu++;
                x->slice_left = 0;
            }
            if (r != STOP_SLICE && r != STOP_IDLE)
                why = r;
        }
        if (why != UAM_STOP_NONE)
            break;
        /* The round is over. */
        bool ran = x->round_ran;
        x->slice_cpu = 0;
        x->round_ran = false;
        if (!ran) {
            /* every processor waits: skip time (or wait for the host) */
            clock_to(s, x->cpu);
            x->running = x->cpu;
            uam_sched_run_due(s);
            bool woke = false;
            for (uint32_t i = 0; i < x->n_smp && !woke; i++)
                woke = x->smp[i]->cpu->isa->irq_wakeup(x->smp[i]->cpu) ||
                       !x->smp[i]->cpu->waiting;
            if (!woke)
                why = idle(x, tlimit);
            if (why == UAM_STOP_NONE && uam_sched_now(s) >= tlimit)
                why = UAM_STOP_TIME_LIMIT;
        }
    }
    clock_to(s, x->cpu);
    x->running = NULL;
    return why;
}

uam_stop_reason uam_exec_run(uam_exec *x, uint64_t max_insns)
{
    if (x->n_smp > 1)
        return run_smp(x, max_insns);
    uint64_t before = x->cpu->icount;
    uam_stop_reason r = run_loop(x, max_insns ? x->cpu->icount + max_insns : UINT64_MAX,
                                 time_limit(x), false);
    x->retired += x->cpu->icount - before;
    return r;
}
