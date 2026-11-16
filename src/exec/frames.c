/*
 * frames.c - Call tracking on a processor (uam/frames.h): what UAM_OP_FRAME does, the
 * set of return addresses, and wiring the frame pass (analysis/frames.c) into an executor.
 */
#include <stdlib.h>
#include <string.h>

#include "uam/exec.h"
#include "uam/frames.h"
#include "uam/state.h"

/* ---- the set of return addresses ------------------------------------------ */

#define RS_EMPTY UINT64_MAX   /* no call returns to the last address */

struct uam_ret_sites {
    uint64_t *slot;      /* open addressing, linear probing */
    uint32_t  cap, n;    /* cap: a power of two, or 0 */
    uint64_t  digest;    /* the xor of mix(pc) over the set: the same for the same set */
    uint64_t *scratch;   /* uam_ret_sites_load's buffer */
    uint32_t  scratch_cap;
    bool      loaded;    /* the saved set of digest loaded_digest is in this one */
    uint64_t  loaded_digest;
};

static uint64_t rs_mix(uint64_t x)
{
    x ^= x >> 33;
    x *= UINT64_C(0xff51afd7ed558ccd);
    x ^= x >> 33;
    x *= UINT64_C(0xc4ceb9fe1a85ec53);
    return x ^ (x >> 33);
}

uam_ret_sites *uam_ret_sites_create(void)
{
    return calloc(1, sizeof(uam_ret_sites));
}

void uam_ret_sites_destroy(uam_ret_sites *rs)
{
    if (rs) {
        free(rs->slot);
        free(rs->scratch);
    }
    free(rs);
}

size_t uam_ret_sites_count(const uam_ret_sites *rs)
{
    return rs->n;
}

bool uam_ret_sites_has(const uam_ret_sites *rs, uint64_t pc)
{
    if (!rs->cap || pc == RS_EMPTY)
        return false;
    for (uint32_t i = (uint32_t)rs_mix(pc) & (rs->cap - 1);; i = (i + 1) & (rs->cap - 1)) {
        if (rs->slot[i] == pc)
            return true;
        if (rs->slot[i] == RS_EMPTY)
            return false;
    }
}

static void rs_put(uam_ret_sites *rs, uint64_t pc)
{
    uint32_t i = (uint32_t)rs_mix(pc) & (rs->cap - 1);
    while (rs->slot[i] != RS_EMPTY)
        i = (i + 1) & (rs->cap - 1);
    rs->slot[i] = pc;
}

bool uam_ret_sites_add(uam_ret_sites *rs, uint64_t pc)
{
    if (pc == RS_EMPTY || uam_ret_sites_has(rs, pc))
        return true;
    if (rs->n * 2 >= rs->cap) {
        const uint32_t cap = rs->cap ? rs->cap * 2 : 1024;
        uint64_t *old = rs->slot, *slot = malloc(sizeof(*slot) * cap);
        if (!slot)
            return false;
        const uint32_t old_cap = rs->cap;
        for (uint32_t i = 0; i < cap; i++)
            slot[i] = RS_EMPTY;
        rs->slot = slot;
        rs->cap = cap;
        for (uint32_t i = 0; i < old_cap; i++)
            if (old[i] != RS_EMPTY)
                rs_put(rs, old[i]);
        free(old);
    }
    rs_put(rs, pc);
    rs->n++;
    rs->digest ^= rs_mix(pc);
    return true;
}

/* Saved as one array (host byte order within a snapshot, like memory): a restore that
 * has nothing to add then costs one copy. */
void uam_ret_sites_save(const uam_ret_sites *rs, uam_state_writer *w)
{
    uint64_t *all = malloc(sizeof(*all) * (rs->n ? rs->n : 1));
    uint32_t k = 0;
    for (uint32_t i = 0; all && i < rs->cap; i++)
        if (rs->slot[i] != RS_EMPTY)
            all[k++] = rs->slot[i];
    uam_state_put_u64(w, rs->digest);
    uam_state_put_u32(w, all ? rs->n : 0);
    uam_state_put_bytes(w, all, sizeof(*all) * (all ? rs->n : 0));
    free(all);
}

void uam_ret_sites_load(uam_ret_sites *rs, uam_state_reader *r)
{
    const uint64_t digest = uam_state_get_u64(r);
    const uint32_t n = uam_state_get_u32(r);
    if (n > rs->scratch_cap) {
        uint64_t *ns = realloc(rs->scratch, sizeof(*ns) * n);
        if (!ns) {
            uam_state_fail(r, "out of memory");
            return;
        }
        rs->scratch = ns;
        rs->scratch_cap = n;
    }
    uam_state_get_bytes(r, rs->scratch, sizeof(*rs->scratch) * n);
    /* A set only grows: the snapshot this one was last brought to (a fuzz harness
     * restores the same one for every input) has nothing to add. */
    if (!uam_state_ok(r) || (rs->loaded && digest == rs->loaded_digest))
        return;
    for (uint32_t i = 0; i < n; i++) {
        if (!uam_ret_sites_add(rs, rs->scratch[i])) {
            uam_state_fail(r, "out of memory");
            return;
        }
    }
    rs->loaded = true;
    rs->loaded_digest = digest;
}

/* ---- frames --------------------------------------------------------------- */

void uam_frame_reset(uam_frame_state *s)
{
    s->n = 0;
}

static void push(uam_frame_state *s, uint64_t ret, uint64_t callee)
{
    if (s->n == UAM_FRAME_DEPTH) {   /* keep the innermost half */
        memmove(s->f, s->f + UAM_FRAME_DEPTH / 2, sizeof(*s->f) * (UAM_FRAME_DEPTH / 2));
        s->n = UAM_FRAME_DEPTH / 2;
    }
    s->f[s->n++] = (uam_frame){ .ret = ret, .callee = callee };
}

void uam_frame_call(uam_frame_state *s, uint64_t ret, uint64_t callee)
{
    s->calls++;
    push(s, ret, callee);
}

void uam_frame_mark(uam_frame_state *s, uint64_t pc)
{
    push(s, UAM_FRAME_MARK, pc);
}

void uam_frame_eret(uam_frame_state *s)
{
    for (uint32_t i = s->n; i-- > 0;) {
        if (s->f[i].ret == UAM_FRAME_MARK) {
            s->n = i;
            return;
        }
    }
}

void uam_frame_hook_return(uam_frame_state *s, uint64_t pc)
{
    if (s->n && s->f[s->n - 1].ret == (pc & s->pc_mask))
        s->n--;
}

void uam_frame_ret(uam_frame_state *s, uint64_t target, uint64_t pc)
{
    const uint64_t t = target & s->pc_mask;
    s->returns++;
    if (s->n && s->f[s->n - 1].ret == t) {
        s->n--;
        return;
    }
    /* A return to an older frame: whatever was above it was left without returning. */
    for (uint32_t i = s->n; i-- > 0;) {
        if (s->f[i].ret == t) {
            s->n = i;
            s->resyncs++;
            return;
        }
    }
    /* No frame: a call made before tracking, another task's, a frame built by hand. */
    if (uam_ret_sites_has(s->sites, t))
        return;
    s->bad_returns++;
    if (s->guard && s->bad_return)
        s->bad_return(s->bad_ctx, s->cpu, pc, target);
}

/* ---- one executor --------------------------------------------------------- */

static void note_site(void *ctx, uint64_t ret)
{
    uam_ret_sites_add(ctx, ret);   /* out of memory: that call's returns may be reported */
}

uam_status uam_exec_set_frames(uam_exec *x, uam_frame_state *s)
{
    if (x->frame_pass) {
        uam_exec_remove_pass(x, uam_frame_pass_get(x->frame_pass));
        uam_frame_pass_destroy(x->frame_pass);
        x->frame_pass = NULL;
    }
    x->cpu->frames = NULL;
    uam_tcache_flush(&x->tc);   /* no block may keep a FRAME once frames is NULL */
    uam_cpu_request_exit(x->cpu);
    if (!s)
        return UAM_OK;
    if (!s->sites)
        return UAM_ERR_INVALID;
    s->pc_mask = ~uam_isa_pc_state_bits(x->cpu->isa);
    s->cpu = x->cpu;
    uam_frame_pass *fp = uam_frame_pass_create(s->pc_mask, note_site, s->sites);
    if (!fp)
        return UAM_ERR_NOMEM;
    uam_status st = uam_exec_add_pass(x, uam_frame_pass_get(fp));
    if (st != UAM_OK) {
        uam_frame_pass_destroy(fp);
        return st;
    }
    x->frame_pass = fp;
    x->cpu->frames = s;
    return UAM_OK;
}
