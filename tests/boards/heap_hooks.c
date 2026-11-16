/*
 * heap_hooks.c - A board's side of a guarded heap (uam/heap.h): the hooks that send a
 * firmware's allocation functions to it. For rv32-test, and a worked example of what a
 * board developer writes.
 *
 * Ember knows neither which functions allocate nor how they take arguments; the board
 * does. Here the firmware is RISC-V code following the standard calling convention
 * (arguments in a0, a1; result in a0; return address in ra), and the board is told where
 * its heap and its functions are:
 *
 *     -o heap=BASE+SIZE      the arena (guest memory the firmware's allocator used)
 *     -o malloc=ADDR         void *f(size)
 *     -o free=ADDR           void f(ptr)
 *     -o realloc=ADDR        void *f(ptr, size)
 *     -o heap-quarantine=N   freed bytes held back from reuse (default: a quarter)
 *
 * Each hook runs when the processor arrives at the function's first instruction: it reads
 * the arguments, calls the heap, writes the result and continues at the return address,
 * so the firmware's own function never runs.
 */
#include <stdlib.h>
#include <string.h>

#include "uam/heap.h"

#include "test_boards.h"

typedef struct heap_hooks {
    uam_heap           *heap;
    const uam_regdesc  *a0, *a1, *ra;
    void               *ref_data;
    void              (*ref_fini)(uam_machine *m);
} heap_hooks;

static const uam_regdesc *reg(uam_cpu *cpu, const char *name)
{
    for (size_t i = 0; i < cpu->isa->n_regs; i++)
        if (strcmp(cpu->isa->regs[i].name, name) == 0)
            return &cpu->isa->regs[i];
    return NULL;
}

static uint64_t rd(uam_cpu *cpu, const uam_regdesc *r)
{
    return uam_cpu_reg_read(cpu, r->offset, r->width);
}

/* "return v" from the hooked function. */
static uam_hook_action ret(heap_hooks *hh, uam_cpu *cpu, uint64_t v)
{
    uam_cpu_reg_write(cpu, hh->a0->offset, hh->a0->width, v);
    cpu->pc = rd(cpu, hh->ra);
    return UAM_HOOK_CONTINUE;
}

static uam_hook_action on_malloc(void *ctx, uam_cpu *cpu, uint64_t pc)
{
    heap_hooks *hh = ctx;
    (void)pc;
    return ret(hh, cpu, uam_heap_alloc(hh->heap, rd(cpu, hh->a0), 0, rd(cpu, hh->ra)));
}

static uam_hook_action on_free(void *ctx, uam_cpu *cpu, uint64_t pc)
{
    heap_hooks *hh = ctx;
    (void)pc;
    uam_heap_free(hh->heap, rd(cpu, hh->a0), rd(cpu, hh->ra));   /* a bad free is reported */
    return ret(hh, cpu, 0);
}

static uam_hook_action on_realloc(void *ctx, uam_cpu *cpu, uint64_t pc)
{
    heap_hooks *hh = ctx;
    (void)pc;
    return ret(hh, cpu, uam_heap_realloc(hh->heap, rd(cpu, hh->a0), rd(cpu, hh->a1), rd(cpu, hh->ra)));
}

static void heap_hooks_fini(uam_machine *m)
{
    heap_hooks *hh = m->board_data;
    uam_heap_destroy(hh->heap);
    m->board_data = hh->ref_data;
    m->board_fini = hh->ref_fini;
    free(hh);
    if (m->board_fini)
        m->board_fini(m);
}

uam_status test_heap_hooks_install(uam_machine *m, const uam_board_config *cfg)
{
    const char *arena = uam_board_config_option(cfg, "heap");
    if (!arena)
        return UAM_OK;
    char *end;
    uam_heap_opts o = UAM_HEAP_OPTS_INIT;
    o.base = strtoull(arena, &end, 0);
    o.size = *end == '+' ? strtoull(end + 1, &end, 0) : 0;
    const char *q = uam_board_config_option(cfg, "heap-quarantine");
    if (q)
        o.quarantine = strtoull(q, NULL, 0);
    if (*end || !o.size) {
        uam_log(UAM_LOG_ERROR, "rv32-test", "heap=%s: expected BASE+SIZE", arena);
        return UAM_ERR_INVALID;
    }
    heap_hooks *hh = calloc(1, sizeof(*hh));
    if (!hh)
        return UAM_ERR_NOMEM;
    hh->ref_data = m->board_data;
    hh->ref_fini = m->board_fini;
    m->board_data = hh;
    m->board_fini = heap_hooks_fini;
    hh->a0 = reg(&m->cpu, "a0");
    hh->a1 = reg(&m->cpu, "a1");
    hh->ra = reg(&m->cpu, "ra");
    char err[256];
    hh->heap = uam_heap_create(m, &o, err, sizeof(err));
    if (!hh->heap || !hh->a0 || !hh->a1 || !hh->ra) {
        uam_log(UAM_LOG_ERROR, "rv32-test", "heap=%s: %s", arena, hh->heap ? "no a0/a1/ra" : err);
        return UAM_ERR_INVALID;
    }
    static const struct { const char *opt; uam_pc_hook_fn fn; } fns[] = {
        { "malloc", on_malloc }, { "free", on_free }, { "realloc", on_realloc },
    };
    for (size_t i = 0; i < sizeof(fns) / sizeof(fns[0]); i++) {
        const char *at = uam_board_config_option(cfg, fns[i].opt);
        if (at && uam_machine_add_pc_hook(m, strtoull(at, NULL, 0), fns[i].fn, hh) != UAM_OK)
            return UAM_ERR_NOMEM;
    }
    return UAM_OK;
}
