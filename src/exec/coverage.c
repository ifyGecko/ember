/*
 * coverage.c - Edge coverage on a hart (uam_exec_set_coverage, uam/exec.h).
 *
 * Wires the coverage pass (analysis/cov.c, which only edits IR) into the executor
 * and points the hart at the coverage state that UAM_OP_COV updates.
 */
#include <stdio.h>

#include "uam/exec.h"

static void coverage_off(uam_exec *x)
{
    if (x->cov_pass) {
        uam_exec_remove_pass(x, uam_cov_pass_get(x->cov_pass));
        uam_cov_pass_destroy(x->cov_pass);
        x->cov_pass = NULL;
    }
    x->cpu->cov = NULL;
    uam_tcache_flush(&x->tc);   /* no block may keep a COV once cov is NULL */
    uam_cpu_request_exit(x->cpu);
}

/* NULL if `s` is usable by UAM_OP_COV, else why not. */
static const char *state_problem(const uam_cov_state *s)
{
    if (!s->map || (s->mask & (s->mask + 1)))
        return "coverage map size must be a power of two";
    if (s->depth > UAM_COV_HIST_MAX)
        return "coverage history is deeper than UAM_COV_HIST_MAX";
    const uint32_t d = s->depth ? s->depth : 1;
    for (uint32_t j = 0; j < d; j++) {
        if (!s->rot[j])
            continue;
        if (s->mask + 1 == 0 || (UINT64_C(1) << s->bits) != (uint64_t)s->mask + 1)
            return "coverage rotation needs bits = log2(map size) (uam_cov_state_init)";
        if (s->rot[j] >= s->bits)
            return "coverage rotation must be below the map's width in bits";
    }
    return NULL;
}

static uam_status coverage_on(uam_exec *x, uam_cov_pass *cp, uam_cov_state *state)
{
    uam_status st = uam_exec_add_pass(x, uam_cov_pass_get(cp));
    if (st != UAM_OK) {
        uam_cov_pass_destroy(cp);
        return st;
    }
    x->cov_pass = cp;
    x->cpu->cov = state;
    return UAM_OK;
}

uam_status uam_exec_set_coverage(uam_exec *x, const uam_cov_site *sites, size_t n,
                                 uam_cov_state *state, char *err, size_t errlen)
{
    if (err && errlen)
        err[0] = '\0';
    coverage_off(x);
    if (!sites || !n || !state)
        return UAM_OK;
    const char *why = state_problem(state);
    if (why) {
        if (err && errlen)
            snprintf(err, errlen, "%s", why);
        return UAM_ERR_INVALID;
    }
    uam_cov_pass *cp;
    uam_status st = uam_cov_pass_create(sites, n, &cp, err, errlen);
    if (st != UAM_OK)
        return st;
    return coverage_on(x, cp, state);
}

uam_status uam_exec_set_block_coverage(uam_exec *x, uam_cov_state *state)
{
    coverage_off(x);
    if (!state)
        return UAM_OK;
    if (state_problem(state))
        return UAM_ERR_INVALID;
    uam_cov_pass *cp;
    uam_status st = uam_cov_pass_create_blocks(&cp);
    if (st != UAM_OK)
        return st;
    return coverage_on(x, cp, state);
}

uam_status uam_exec_set_block_coverage_ex(uam_exec *x, uam_cov_state *state,
                                          const uam_cov_block_opts *o)
{
    coverage_off(x);
    if (!state)
        return UAM_OK;
    if (state_problem(state) || (o->mask && o->mask != state->mask))
        return UAM_ERR_INVALID;
    uam_cov_pass *cp;
    uam_status st = uam_cov_pass_create_blocks_ex(o, &cp);
    if (st != UAM_OK)
        return st;
    state->contexts = o->contexts;
    uam_cov_reset_prev(state);
    return coverage_on(x, cp, state);
}

/* ---- block contexts (cpu.h, uam_cov_irq_ctx) --------------------------------- */

void uam_cov_eret(uam_cov_state *s)
{
    s->eret = 1;
    s->pending = 1;
}

/* After an exception return to `pc`: the newest context saved there, if any, is the
 * code's own again (its history, and whether its block was itself entered mid-way). */
static void ctx_return(uam_cov_state *s, uint64_t pc)
{
    for (unsigned i = s->n_irq; i-- > 0;) {
        if (s->irq[i].resume_pc != pc)
            continue;
        const uam_cov_irq_ctx *c = &s->irq[i];
        for (unsigned j = 0; j < UAM_COV_HIST_MAX; j++)
            s->hist[j] = c->hist[j];
        s->last_pc = c->last_pc;
        s->skip_pc = c->skip_pc;
        for (unsigned k = i + 1; k < s->n_irq; k++)
            s->irq[k - 1] = s->irq[k];
        s->n_irq--;
        return;
    }
}

void uam_cov_skip_at(uam_cov_state *s, uint64_t pc)
{
    s->skip_pc = pc;
    s->pending = 1;
}

void uam_cov_irq_enter(uam_cov_state *s, uint64_t from_pc)
{
    if (s->eret) {
        /* Interrupted again before the code returned to ran a block: it is that code,
         * with its own history, that this interrupt suspends. */
        s->eret = 0;
        ctx_return(s, from_pc);
    }
    if (s->n_irq == UAM_COV_IRQ_MAX) {
        /* Contexts never returned to (a task switch from the handler): drop the oldest. */
        for (unsigned i = 1; i < UAM_COV_IRQ_MAX; i++)
            s->irq[i - 1] = s->irq[i];
        s->n_irq--;
    }
    uam_cov_irq_ctx *c = &s->irq[s->n_irq++];
    c->resume_pc = from_pc;
    c->last_pc = s->last_pc;
    c->skip_pc = s->skip_pc;
    for (unsigned j = 0; j < UAM_COV_HIST_MAX; j++) {
        c->hist[j] = s->hist[j];
        s->hist[j] = 0;
    }
    s->last_pc = UAM_COV_NO_PC;
    s->skip_pc = UAM_COV_NO_PC;
    s->pending = 0;
}

void uam_cov_count_slow(uam_cov_state *s, uint64_t pc, uint32_t a, uint32_t b)
{
    if (s->eret) {
        s->eret = 0;
        ctx_return(s, pc);
    }
    const bool skip = s->skip_pc == pc;
    s->skip_pc = UAM_COV_NO_PC;   /* consumed, or the hart went elsewhere */
    s->pending = 0;
    if (!skip)
        uam_cov_count(s, pc, a, b);
}
