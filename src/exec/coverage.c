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
