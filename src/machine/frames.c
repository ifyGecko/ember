/*
 * frames.c - Call tracking for a whole machine (uam/frames.h) and the return guard
 * (uam/guard.h): a frame state per processor, one set of return addresses, both saved
 * in snapshots.
 */
#include <stdlib.h>
#include <string.h>

#include "uam/frames.h"
#include "uam/guard.h"
#include "uam/machine.h"
#include "uam/state.h"

struct uam_machine_frames {
    uam_machine      *m;
    uam_ret_sites    *sites;
    uam_frame_state **st;       /* one per processor while tracking */
    uint32_t          n;
    bool              on, guard;
    uam_guard_action  action;
};

void uam_machine_frames_fini(uam_machine *m);

static void bad_return(void *ctx, uam_cpu *cpu, uint64_t pc, uint64_t target)
{
    struct uam_machine_frames *f = ctx;
    const uam_guard_hit hit = { .kind = UAM_GUARD_RETURN, .name = "return", .action = f->action,
                                .cpu = cpu->hartid, .pc = pc, .addr = target, .paddr = target };
    uam_machine_guard_report(f->m, &hit, false);
    if (f->action == UAM_GUARD_LOG)   /* once per address */
        uam_ret_sites_add(f->sites, target & ~uam_isa_pc_state_bits(cpu->isa));
}

static void save(void *ctx, uam_state_writer *w)
{
    const struct uam_machine_frames *f = ctx;
    uam_state_put_u32(w, f->on ? f->n : 0);
    for (uint32_t i = 0; f->on && i < f->n; i++) {
        const uint32_t k = uam_frame_depth(f->st[i]);
        uam_state_put_u32(w, k);
        uam_state_put_bytes(w, f->st[i]->f + 1, sizeof(uam_frame) * k);
    }
    uam_ret_sites_save(f->sites, w);
}

static void restore(void *ctx, uam_state_reader *r)
{
    struct uam_machine_frames *f = ctx;
    const uint32_t n = uam_state_get_u32(r);
    for (uint32_t i = 0; i < n && uam_state_ok(r); i++) {
        const uint32_t k = uam_state_get_u32(r);
        uam_frame tmp[UAM_FRAME_DEPTH];
        if (k > UAM_FRAME_DEPTH) {
            uam_state_fail(r, "frames: too deep");
            return;
        }
        uam_state_get_bytes(r, tmp, sizeof(uam_frame) * k);
        if (f->on && i < f->n) {
            memcpy(f->st[i]->f + 1, tmp, sizeof(uam_frame) * k);
            f->st[i]->top = f->st[i]->f + 1 + k;
        }
    }
    for (uint32_t i = n; f->on && i < f->n; i++)
        uam_frame_reset(f->st[i]);
    uam_ret_sites_load(f->sites, r);
}

static struct uam_machine_frames *get(uam_machine *m)
{
    if (m->frames)
        return m->frames;
    struct uam_machine_frames *f = calloc(1, sizeof(*f));
    if (!f || !(f->sites = uam_ret_sites_create())
        || uam_machine_add_state(m, "frames", save, restore, f) != UAM_OK) {
        if (f)
            uam_ret_sites_destroy(f->sites);
        free(f);
        return NULL;
    }
    f->m = m;
    f->action = UAM_GUARD_STOP;
    m->frames = f;
    return f;
}

static void off(uam_machine *m, struct uam_machine_frames *f)
{
    for (uint32_t i = 0; i < f->n; i++) {
        uam_exec_set_frames(uam_machine_exec(m, i), NULL);
        free(f->st[i]);
    }
    free(f->st);
    f->st = NULL;
    f->n = 0;
    f->on = false;
}

static void apply(struct uam_machine_frames *f)
{
    for (uint32_t i = 0; i < f->n; i++) {
        f->st[i]->guard = f->guard;
        f->st[i]->bad_return = bad_return;
        f->st[i]->bad_ctx = f;
    }
}

uam_status uam_machine_track_calls(uam_machine *m, bool on)
{
    struct uam_machine_frames *f = on ? get(m) : m->frames;
    if (!f)
        return on ? UAM_ERR_NOMEM : UAM_OK;
    if (f->on == on)
        return UAM_OK;
    if (!on) {
        off(m, f);
        return UAM_OK;
    }
    const uint32_t n = m->n_aps + 1;
    f->st = calloc(n, sizeof(*f->st));
    if (!f->st)
        return UAM_ERR_NOMEM;
    f->n = n;
    f->on = true;
    uam_status st = UAM_OK;
    for (uint32_t i = 0; i < n && st == UAM_OK; i++) {
        if (!(f->st[i] = calloc(1, sizeof(*f->st[i])))) {
            st = UAM_ERR_NOMEM;
            break;
        }
        uam_frame_state_init(f->st[i]);
        f->st[i]->sites = f->sites;
    }
    apply(f);
    for (uint32_t i = 0; i < n && st == UAM_OK; i++)
        st = uam_exec_set_frames(uam_machine_exec(m, i), f->st[i]);
    if (st != UAM_OK)
        off(m, f);
    return st;
}

uam_status uam_machine_guard_returns(uam_machine *m, bool on, uam_guard_action action)
{
    if (on && action != UAM_GUARD_STOP && action != UAM_GUARD_LOG)
        return UAM_ERR_INVALID;
    if (on) {
        uam_status st = uam_machine_track_calls(m, true);
        if (st != UAM_OK)
            return st;
    }
    struct uam_machine_frames *f = m->frames;
    if (!f)
        return UAM_OK;
    f->guard = on;
    if (on)
        f->action = action;
    apply(f);
    return UAM_OK;
}

uam_status uam_machine_add_return_site(uam_machine *m, uint64_t pc)
{
    struct uam_machine_frames *f = get(m);
    if (!f)
        return UAM_ERR_NOMEM;
    pc &= ~uam_isa_pc_state_bits(m->cpu.isa);
    return uam_ret_sites_add(f->sites, pc) ? UAM_OK : UAM_ERR_NOMEM;
}

uam_frame_state *uam_machine_frames(const uam_machine *m, unsigned cpu)
{
    const struct uam_machine_frames *f = m->frames;
    return f && f->on && cpu < f->n ? f->st[cpu] : NULL;
}

size_t uam_machine_backtrace(const uam_machine *m, unsigned cpu, uam_frame *out, size_t max)
{
    const uam_frame_state *s = uam_machine_frames(m, cpu);
    size_t k = 0;
    for (uint32_t d = s ? uam_frame_depth(s) : 0; k < d && k < max; k++)
        out[k] = *uam_frame_at(s, (uint32_t)k);
    return k;
}

static const char *name_of(uam_symbolize_fn sym, void *ctx, uint64_t pc, char *buf, size_t n)
{
    char nm[160];
    if (sym && sym(ctx, pc, nm, sizeof(nm)))
        snprintf(buf, n, "0x%llx <%s>", (unsigned long long)pc, nm);
    else
        snprintf(buf, n, "0x%llx", (unsigned long long)pc);
    return buf;
}

void uam_machine_backtrace_print(const uam_machine *m, unsigned cpu, FILE *out, const char *prefix,
                                 uam_symbolize_fn sym, void *ctx)
{
    const uam_frame_state *s = uam_machine_frames(m, cpu);
    for (uint32_t k = 0, d = s ? uam_frame_depth(s) : 0; k < d; k++) {
        const uam_frame *fr = uam_frame_at(s, k);
        char a[200], b[200];
        if (fr->ret == UAM_FRAME_MARK)
            fprintf(out, "%s#%u interrupted at %s\n", prefix, k,
                    name_of(sym, ctx, fr->callee, a, sizeof(a)));
        else
            fprintf(out, "%s#%u %s called, returns to %s\n", prefix, k,
                    name_of(sym, ctx, fr->callee, a, sizeof(a)),
                    name_of(sym, ctx, fr->ret, b, sizeof(b)));
    }
}

void uam_machine_frames_fini(uam_machine *m)
{
    struct uam_machine_frames *f = m->frames;
    if (!f)
        return;
    for (uint32_t i = 0; i < f->n; i++)
        free(f->st[i]);   /* the executors are finished with the machine */
    free(f->st);
    uam_ret_sites_destroy(f->sites);
    free(f);
    m->frames = NULL;
}
