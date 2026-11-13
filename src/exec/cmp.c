/*
 * cmp.c - The state UAM_OP_CMPLOG reports to (uam/cmp.h), and compare instrumentation on
 * a hart (uam_exec_set_cmp, uam/exec.h).
 *
 * The log keeps the distinct (site, a, b) of a run in the order first seen. Two open-
 * addressed tables, sized for the log, make a note O(1): one finds a pair already logged,
 * the other counts the pairs of each site (UAM_CMP_SITE_MAX). Only the slots a run used
 * are cleared by uam_cmp_reset, so a short run costs little whatever the capacity.
 */
#include <stdlib.h>
#include <string.h>

#include "uam/exec.h"

struct uam_cmp_state {
    bool           logging;
    uam_cmp_entry *log;
    uint32_t       cap, n;
    uint64_t       dropped;
    uint32_t       mask;        /* both tables have mask + 1 slots */
    uint32_t      *pair;        /* log index + 1; 0: empty */
    uint32_t      *site_key;    /* site; valid where site_cnt != 0 */
    uint32_t      *site_cnt;
    uint32_t      *used;        /* site-table slots in use (at most cap) */
    uint32_t       n_used;
};

uam_cmp_state *uam_cmp_state_create(uint32_t log_cap)
{
    if (!log_cap)
        log_cap = UAM_CMP_DEFAULT_LOG;
    if (log_cap > (1u << 24))
        return NULL;
    uint32_t slots = 16;
    while (slots < 2 * log_cap)
        slots <<= 1;
    uam_cmp_state *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->cap = log_cap;
    s->mask = slots - 1;
    s->log = malloc(sizeof(*s->log) * log_cap);
    s->pair = calloc(slots, sizeof(*s->pair));
    s->site_key = calloc(slots, sizeof(*s->site_key));
    s->site_cnt = calloc(slots, sizeof(*s->site_cnt));
    s->used = malloc(sizeof(*s->used) * log_cap);
    if (!s->log || !s->pair || !s->site_key || !s->site_cnt || !s->used) {
        uam_cmp_state_destroy(s);
        return NULL;
    }
    return s;
}

void uam_cmp_state_destroy(uam_cmp_state *s)
{
    if (!s)
        return;
    free(s->log);
    free(s->pair);
    free(s->site_key);
    free(s->site_cnt);
    free(s->used);
    free(s);
}

void uam_cmp_set_logging(uam_cmp_state *s, bool on) { s->logging = on; }
bool uam_cmp_logging(const uam_cmp_state *s) { return s->logging; }

static uint32_t mix(uint64_t v)
{
    v ^= v >> 33;
    v *= UINT64_C(0xff51afd7ed558ccd);
    v ^= v >> 33;
    v *= UINT64_C(0xc4ceb9fe1a85ec53);
    v ^= v >> 33;
    return (uint32_t)v;
}

void uam_cmp_reset(uam_cmp_state *s)
{
    for (uint32_t i = 0; i < s->n; i++) {      /* each logged pair's slot */
        const uam_cmp_entry *e = &s->log[i];
        uint32_t h = mix(e->a * UINT64_C(0x9E3779B97F4A7C15) ^ e->b ^ (uint64_t)e->site << 32);
        for (uint32_t k = h & s->mask;; k = (k + 1) & s->mask)
            if (s->pair[k] == i + 1) {
                s->pair[k] = 0;
                break;
            }
    }
    for (uint32_t i = 0; i < s->n_used; i++)
        s->site_cnt[s->used[i]] = 0;
    s->n = 0;
    s->n_used = 0;
    s->dropped = 0;
}

const uam_cmp_entry *uam_cmp_log(const uam_cmp_state *s, size_t *n, uint64_t *dropped)
{
    if (n)
        *n = s->n;
    if (dropped)
        *dropped = s->dropped;
    return s->log;
}

void uam_cmp_note(uam_cmp_state *s, uint32_t site, uint32_t wk, uint64_t a, uint64_t b)
{
    const unsigned w = wk & 0xFFu, kind = (wk >> 8) & UAM_CMP_KIND_MASK;
    if (!s->logging)
        return;

    /* already logged? */
    const uint32_t h = mix(a * UINT64_C(0x9E3779B97F4A7C15) ^ b ^ (uint64_t)site << 32);
    uint32_t pk = h & s->mask;
    for (;; pk = (pk + 1) & s->mask) {
        const uint32_t e = s->pair[pk];
        if (!e)
            break;
        const uam_cmp_entry *le = &s->log[e - 1];
        if (le->site == site && le->a == a && le->b == b)
            return;
    }
    /* room for it? */
    uint32_t sk = mix(site) & s->mask;
    for (; s->site_cnt[sk] && s->site_key[sk] != site; sk = (sk + 1) & s->mask)
        ;
    if (s->n == s->cap || s->site_cnt[sk] >= UAM_CMP_SITE_MAX) {
        s->dropped++;
        return;
    }
    if (!s->site_cnt[sk]) {
        s->site_key[sk] = site;
        s->used[s->n_used++] = sk;
    }
    s->site_cnt[sk]++;
    s->pair[pk] = s->n + 1;
    s->log[s->n++] = (uam_cmp_entry){ .site = site, .width = (uint8_t)w, .kind = (uint8_t)kind,
                                      .a = a, .b = b };
}

/* ---- on a hart ------------------------------------------------------------- */

uam_status uam_exec_set_cmp(uam_exec *x, uam_cmp_state *s)
{
    if (x->cmp_pass) {
        uam_exec_remove_pass(x, uam_cmp_pass_get(x->cmp_pass));
        uam_cmp_pass_destroy(x->cmp_pass);
        x->cmp_pass = NULL;
    }
    x->cmp_state = NULL;
    x->cpu->cmp = NULL;
    uam_tcache_flush(&x->tc);   /* no block may keep a CMPLOG it no longer needs */
    uam_cpu_request_exit(x->cpu);
    if (!s)
        return UAM_OK;
    uam_cmp_pass *cp;
    uam_status st = uam_cmp_pass_create(&cp);
    if (st != UAM_OK)
        return st;
    st = uam_exec_add_pass(x, uam_cmp_pass_get(cp));
    if (st != UAM_OK) {
        uam_cmp_pass_destroy(cp);
        return st;
    }
    x->cmp_pass = cp;
    x->cmp_state = s;
    x->cpu->cmp = s;
    return UAM_OK;
}

void uam_exec_cmp_attach(uam_exec *x, bool on)
{
    x->cpu->cmp = on ? x->cmp_state : NULL;
}
