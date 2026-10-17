/*
 * run.c - The hart run loop (uam/exec.h).
 */
#include <stdlib.h>
#include <string.h>

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
    }
    return "?";
}

/* Bus hook: the running block may just have overwritten its own code. */
static void on_code_write(void *ctx, uint64_t page)
{
    (void)page;
    uam_exec *x = ctx;
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
    x->trace_out = stderr;
    x->host.poll_interval = 1024;
    uam_builder_init(&x->b, cpu->isa->addr_width);
    uam_bus_set_code_write_hook(cpu->bus, on_code_write, x);
    return uam_tcache_init(&x->tc, cpu->bus);
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
    st = uam_bus_read(x->cpu->bus, pa0, size, isa->default_endian | UAM_ACC_FETCH, val);
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
    for (uint32_t i = 0; i < blk->n_pages; i++) {
        uam_bus_mark_code(cpu->bus, blk->page[i]);
        blk->page_gen[i] = uam_bus_page_gen(cpu->bus, blk->page[i]);
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
    if (x->n_hooks)
        mark_hooked_imarks(x, blk);
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
    *st = uam_tcache_insert(&x->tc, blk, cpu->priv, core.mapped, cpu->xlate_gen);
    if (*st != UAM_OK) {
        uam_block_free(blk);
        return NULL;
    }
    x->stats.blocks_translated++;
    return blk;
}

/* ---- run loop ---------------------------------------------------------- */

static void deliver(uam_exec *x, const uam_exception *exc)
{
    x->stats.exceptions++;
    if (x->trace & UAM_TRACE_EXC)
        fprintf(x->trace_out, "exc: kind=%d pc=0x%llx cause=0x%llx tval=0x%llx\n", exc->kind,
                (unsigned long long)exc->pc, (unsigned long long)exc->cause,
                (unsigned long long)exc->tval);
    x->cpu->isa->take_exception(x->cpu, exc);
}

static bool trace_block(const uam_exec *x, uint64_t pc)
{
    return (x->trace & UAM_TRACE_BLOCKS) && x->cpu->icount >= x->trace_after
        && pc >= x->trace_lo && (x->trace_hi == 0 || pc < x->trace_hi);
}

/* The hart is idle: advance virtual time, or block on host input. Returns
 * false on deadlock. */
static bool idle(uam_exec *x)
{
    uint64_t next = uam_sched_next(x->sched), now = uam_sched_now(x->sched);
    uint64_t ticks = next == UINT64_MAX ? UINT64_MAX : (next > now ? next - now : 0);
    uint64_t skip = ticks;
    if (x->host.idle) {
        if (!x->host.idle(x->host.ctx, ticks, &skip))
            return false;
        if (skip > ticks)
            skip = ticks;
    } else if (ticks == UINT64_MAX) {
        return false;
    }
    if (skip && skip != UINT64_MAX) {
        uam_sched_skip_to(x->sched, now + skip);
        x->stats.idle_ticks += skip;
    }
    return true;
}

uam_stop_reason uam_exec_run(uam_exec *x, uint64_t max_insns)
{
    uam_cpu *cpu = x->cpu;
    const uam_isa *isa = cpu->isa;
    const uint64_t limit = max_insns ? cpu->icount + max_insns : UINT64_MAX;

    for (;;) {
        if (x->stop_request)
            return UAM_STOP_REQUEST;

        /* 0. Reset requested (by a device, between instructions). */
        if (x->reset_request) {
            x->reset_request = false;
            if (!x->reset_fn)
                return UAM_STOP_RESET;
            x->stats.resets++;
            if (x->trace & UAM_TRACE_EXC)
                fprintf(x->trace_out, "reset: %s\n",
                        x->reset_kind == UAM_RESET_SYSTEM ? "system" : "cpu");
            x->reset_fn(x->reset_ctx, x->reset_kind);
            if (x->jit)
                uam_cpu_dtlb_flush(cpu);
            x->last_fetch_fault_valid = false;
            x->hook_fired = false;
            continue;
        }
        if (cpu->icount >= limit)
            return UAM_STOP_INSN_LIMIT;

        /* Keep the JIT data-TLB coherent with the bus: any mapping, code-page or
         * ROMD change bumps bus->direct_gen, any TLB/ASID change bumps xlate_gen. */
        if (x->jit && (cpu->bus->direct_gen != x->seen_dgen || cpu->xlate_gen != x->seen_xgen)) {
            uam_cpu_dtlb_flush(cpu);
            x->seen_dgen = cpu->bus->direct_gen;
            x->seen_xgen = cpu->xlate_gen;
        }

        /* 1. Time: fire due timers. Host input is polled periodically. */
        uam_sched_run_due(x->sched);
        if (x->host.poll && ++x->blocks_since_poll >= x->host.poll_interval) {
            x->blocks_since_poll = 0;
            x->host.poll(x->host.ctx);
        }
        cpu->exit_request = false;

        /* 2. Interrupts: only ever taken between blocks. */
        if (isa->irq_take(cpu)) {
            x->stats.interrupts++;
            cpu->waiting = false;
            if (x->trace & UAM_TRACE_EXC)
                fprintf(x->trace_out, "irq: entered handler at 0x%llx\n", (unsigned long long)cpu->pc);
        }

        /* 3. Idle: skip virtual time to the next event (or wait for the host). */
        if (cpu->waiting) {
            if (isa->irq_wakeup(cpu)) {
                cpu->waiting = false;
            } else {
                if (!idle(x))
                    return UAM_STOP_DEADLOCK;
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
            if (cpu->pc != pc || x->stop_request || x->reset_request)
                continue;
        }

        /* 5. Find or translate the block. */
        uam_block *blk = uam_tcache_lookup(&x->tc, cpu->pc, cpu->priv, cpu->xlate_gen);
        if (!blk) {
            uam_status st;
            uam_exception exc;
            blk = translate(x, cpu->pc, &st, &exc);
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
                    deliver(x, &exc);
                    continue;
                }
                uam_log(UAM_LOG_ERROR, "exec", "translation failed at 0x%llx: %s",
                        (unsigned long long)cpu->pc, uam_status_str(st));
                return UAM_STOP_ERROR;
            }
        }

        /* 6. Deadline: stop the block when the next timer comes due. */
        uint64_t next = uam_sched_next(x->sched), now = uam_sched_now(x->sched);
        uint64_t deadline = next == UINT64_MAX ? UINT64_MAX
                          : next <= now        ? cpu->icount
                                               : cpu->icount + (next - now);
        cpu->icount_deadline = deadline < limit ? deadline : limit;

        if (x->trace && trace_block(x, blk->guest_pc))
            fprintf(x->trace_out, "blk 0x%llx icount=%llu\n", (unsigned long long)blk->guest_pc,
                    (unsigned long long)cpu->icount);

        /* 7. Execute (native if compiled, else interpreted) and act on the result. */
        uam_block_result r;
        if (blk->backend && blk->backend != UAM_JIT_INTERP) {
            uam_jit_run(x->jit, blk, x->temps, &r);
            x->stats.blocks_native++;
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
            deliver(x, &r.exc);
            break;
        case UAM_BR_TRAP:
            switch (r.trap_class) {
            case UAM_TRAP_EXCEPTION:
                deliver(x, &r.exc);
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
