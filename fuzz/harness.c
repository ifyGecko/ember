/*
 * harness.c - Persistent-mode fuzz harness (uam/fuzz.h).
 *
 * Built only on public interfaces: PC hooks for the iteration boundaries and crash
 * pcs, the exception observer, the machine's reset hook, edge coverage, snapshots and
 * named machine inputs. No device, ISA or fuzzer format is named here.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uam/fuzz.h"
#include "uam/snapshot.h"

typedef enum { PH_IDLE, PH_BOOT, PH_ITER } phase;

typedef struct route {
    char               *name;
    uam_fuzz_deliver_fn fn;
    void               *ctx;
} route;

typedef struct mem_route {
    uint64_t addr, len_addr, max;
    bool     has_len, reject;   /* reject: refuse inputs over max instead of truncating */
    /* guard: after delivery, arm a read-fault guard (UAM_WATCH_FAULT) at addr + L, so a
     * read past the input's legitimate extent faults and is caught as a bus fault. L is the
     * delivered byte count, or - with guard_field - a little/big-endian length field inside
     * the input at [guard_off, guard_off+guard_sz) (e.g. a descriptor's wTotalLength). The
     * guard covers to the end of that page and is re-armed each iteration (guard_wid). */
    bool     guard, guard_field;
    uint64_t guard_off;
    unsigned guard_sz;
    int64_t  guard_delta;       /* guard at addr + field + delta: skip a known N-past read to hunt deeper */
    int      guard_wid;
    /* guard_abs: a FIXED read-fault guard on an absolute virtual range [guard_abs_addr,
     * +guard_abs_n), independent of the input buffer - a redzone BEFORE a buffer catches an
     * under-read (e.g. buf[-1]), which a trailing guard cannot. Re-armed each iteration. */
    bool     guard_abs;
    uint64_t guard_abs_addr, guard_abs_n;
    int      guard_abs_wid;
} mem_route;

struct uam_fuzz {
    uam_machine   *m;
    uam_fuzz_config cfg;
    uint64_t      *crash_pcs;
    uint64_t      *end_pcs;        /* distinct, copied from cfg */
    size_t         n_end_pcs;
    uint64_t      *begin_pcs;      /* cfg.begin_pc, then the distinct more_begin_pcs */
    size_t         n_begin_pcs;
    uint8_t       *map;
    uam_cov_state  cov;
    uam_cov_site  *marker;         /* sites at the begin and end pcs, counted by the hooks */
    size_t         n_marker;
    uint64_t      *snap_last_pc;   /* each processor's cov last_pc when the snapshot was taken */
    unsigned       ncpus;
    uam_cpu       *hit_cpu;        /* the processor a boundary, crash or exception stopped */
    const char    *scheme;
    char           scheme_buf[32];
    bool           has_cov;

    route         *routes;
    size_t         n_routes;
    uam_fuzz_deliver_fn deliver;
    void          *deliver_ctx;
    mem_route      mem;

    uam_snapshot  *snap;
    bool           booted;
    phase          ph;
    volatile sig_atomic_t aborted;

    /* what ended the current run */
    bool           at_begin, ended, oracle, reset, exc_crash, irq_crash;
    uam_interrupt  irq;
    unsigned       depth;          /* nested levels open (paired + nested) */
    uint32_t       boot_arrivals;  /* at the begin pc during boot (cfg.begin_skip) */
    uint64_t       oracle_pc;
    uam_exception  exc;

    uam_machine_reset_hook prev_reset;
    void          *prev_reset_ctx;

    uam_fuzz_boot_fn     boot_fn;
    void                *boot_ctx;
    uam_fuzz_observer_fn obs_fn;
    void                *obs_ctx;

    uam_fuzz_result last;
    uam_fuzz_stats  stats;
};

void uam_fuzz_set_boot_hook(uam_fuzz *f, uam_fuzz_boot_fn fn, void *ctx)
{
    f->boot_fn = fn;
    f->boot_ctx = ctx;
}

void uam_fuzz_set_observer(uam_fuzz *f, uam_fuzz_observer_fn fn, void *ctx)
{
    f->obs_fn = fn;
    f->obs_ctx = ctx;
}

const char *uam_fuzz_verdict_str(uam_fuzz_verdict v)
{
    switch (v) {
    case UAM_FUZZ_OK:           return "ok";
    case UAM_FUZZ_CRASH_FAULT:  return "crash-fault";
    case UAM_FUZZ_CRASH_ORACLE: return "crash-oracle";
    case UAM_FUZZ_HANG:         return "hang";
    case UAM_FUZZ_TIMEOUT:      return "timeout";
    case UAM_FUZZ_REJECTED:     return "rejected";
    }
    return "?";
}

int uam_fuzz_exit_status(uam_fuzz_verdict v)
{
    switch (v) {
    case UAM_FUZZ_OK:           return UAM_FUZZ_EXIT_OK;
    case UAM_FUZZ_CRASH_FAULT:  return UAM_FUZZ_EXIT_CRASH_FAULT;
    case UAM_FUZZ_CRASH_ORACLE: return UAM_FUZZ_EXIT_CRASH_ORACLE;
    case UAM_FUZZ_HANG:         return UAM_FUZZ_EXIT_HANG;
    case UAM_FUZZ_TIMEOUT:      return UAM_FUZZ_EXIT_TIMEOUT;
    case UAM_FUZZ_REJECTED:     return UAM_FUZZ_EXIT_REJECTED;
    }
    return 1;
}

bool uam_fuzz_parse_exc_classes(const char *s, unsigned *out)
{
    static const struct { const char *name; unsigned bits; } names[] = {
        { "fetch", UAM_FUZZ_EXC_FETCH }, { "load", UAM_FUZZ_EXC_LOAD },
        { "store", UAM_FUZZ_EXC_STORE }, { "align", UAM_FUZZ_EXC_ALIGN },
        { "xlate", UAM_FUZZ_EXC_XLATE }, { "trap", UAM_FUZZ_EXC_TRAP },
        { "sysreg", UAM_FUZZ_EXC_SYSREG }, { "all", UAM_FUZZ_EXC_ALL },
        { "default", UAM_FUZZ_EXC_DEFAULT }, { "none", 0 },
    };
    unsigned bits = 0;
    const char *p = s;
    while (*p) {
        size_t n = strcspn(p, ",");
        bool found = false;
        for (size_t i = 0; i < UAM_ARRAY_LEN(names); i++) {
            if (strlen(names[i].name) == n && !strncmp(p, names[i].name, n)) {
                bits |= names[i].bits;
                found = true;
            }
        }
        if (!found)
            return false;
        p += n;
        if (*p == ',')
            p++;
    }
    *out = bits;
    return true;
}

static unsigned exc_class(uam_exc_kind k)
{
    switch (k) {
    case UAM_EXC_TRAP:             return UAM_FUZZ_EXC_TRAP;
    case UAM_EXC_FETCH_FAULT:      return UAM_FUZZ_EXC_FETCH;
    case UAM_EXC_LOAD_FAULT:       return UAM_FUZZ_EXC_LOAD;
    case UAM_EXC_STORE_FAULT:      return UAM_FUZZ_EXC_STORE;
    case UAM_EXC_LOAD_MISALIGNED:
    case UAM_EXC_STORE_MISALIGNED: return UAM_FUZZ_EXC_ALIGN;
    case UAM_EXC_SYSREG:
    case UAM_EXC_HELPER:           return UAM_FUZZ_EXC_SYSREG;
    case UAM_EXC_FETCH_XLATE:
    case UAM_EXC_LOAD_XLATE:
    case UAM_EXC_STORE_XLATE:      return UAM_FUZZ_EXC_XLATE;
    }
    return 0;
}

/* ---- hooks ------------------------------------------------------------- */

/*
 * A coverage site at a boundary pc is counted here, by its hook, rather than by a COV
 * in the translated code: the hook runs before the instruction, and its hit has to be
 * counted before the map is published and the history reset. So the begin's hit goes
 * to the map it closes (INIT at boot, the previous iteration in begin-only mode), an
 * end's to the iteration it ends, and an iteration's first count is the site after
 * the begin, with an empty history. The snapshot is taken at the begin pc with its
 * hook already run, so the begin instruction there is never counted twice.
 */
static void count_marker(uam_fuzz *f, uam_cpu *cpu, uint64_t pc)
{
    if (!f->has_cov)
        return;
    /* in the processor's own history (uam_machine_set_coverage) */
    uam_cov_state *cs = cpu->cov ? cpu->cov : &f->cov;
    for (size_t i = 0; i < f->n_marker; i++) {
        if (f->marker[i].pc == pc) {
            uam_cov_count(cs, pc, f->marker[i].a, f->marker[i].b);
            return;
        }
    }
}

static uam_hook_action begin_hook(void *ctx, uam_cpu *cpu, uint64_t pc)
{
    uam_fuzz *f = ctx;
    if (f->ph == PH_IDLE)
        return UAM_HOOK_CONTINUE;
    count_marker(f, cpu, pc);
    if (f->ph == PH_BOOT) {
        if (f->boot_arrivals++ < f->cfg.begin_skip)
            return UAM_HOOK_CONTINUE;   /* a start-up pass (begin_skip): part of INIT */
        f->at_begin = true;
        f->hit_cpu = cpu;
        return UAM_HOOK_STOP;
    }
    if (!f->n_end_pcs) {   /* begin-only: back at begin */
        f->ended = true;
        f->hit_cpu = cpu;
        return UAM_HOOK_STOP;
    }
    if (f->cfg.nested)
        f->depth++;
    return UAM_HOOK_CONTINUE;
}

static uam_hook_action end_hook(void *ctx, uam_cpu *cpu, uint64_t pc)
{
    uam_fuzz *f = ctx;
    if (f->ph == PH_IDLE)
        return UAM_HOOK_CONTINUE;
    count_marker(f, cpu, pc);
    if (f->ph != PH_ITER)
        return UAM_HOOK_CONTINUE;
    if (f->cfg.nested && --f->depth > 0)
        return UAM_HOOK_CONTINUE;
    f->ended = true;
    f->hit_cpu = cpu;
    return UAM_HOOK_STOP;
}

static uam_hook_action crash_hook(void *ctx, uam_cpu *cpu, uint64_t pc)
{
    uam_fuzz *f = ctx;
    if (f->ph != PH_ITER)
        return UAM_HOOK_CONTINUE;
    f->oracle = true;
    f->oracle_pc = pc;
    f->hit_cpu = cpu;
    return UAM_HOOK_STOP;
}

static uam_hook_action exc_hook(void *ctx, uam_cpu *cpu, const uam_exception *e)
{
    uam_fuzz *f = ctx;
    if (f->ph != PH_ITER || !(exc_class(e->kind) & f->cfg.crash_exc))
        return UAM_HOOK_CONTINUE;
    f->exc_crash = true;
    f->exc = *e;
    f->hit_cpu = cpu;
    return UAM_HOOK_STOP;
}

static uam_hook_action irq_hook(void *ctx, uam_cpu *cpu, const uam_interrupt *irq)
{
    uam_fuzz *f = ctx;
    if (f->ph != PH_ITER || !(irq->lines & f->cfg.crash_irq_lines))
        return UAM_HOOK_CONTINUE;
    f->irq_crash = true;
    f->irq = *irq;
    f->hit_cpu = cpu;
    return UAM_HOOK_STOP;
}

static void reset_hook(uam_machine *m, uam_reset_kind kind, void *ctx)
{
    uam_fuzz *f = ctx;
    if (f->prev_reset)
        f->prev_reset(m, kind, f->prev_reset_ctx);
    if (f->ph == PH_ITER) {
        f->reset = true;
        uam_exec_stop(&m->exec, 0);
    }
}

/* ---- life cycle -------------------------------------------------------- */

static void set_err(char *err, size_t n, const char *msg)
{
    if (err && n)
        snprintf(err, n, "%s", msg);
}

uam_fuzz *uam_fuzz_create(uam_machine *m, const uam_fuzz_config *cfg, char *err, size_t errlen)
{
    if (err && errlen)
        err[0] = '\0';
    uint32_t map_size = cfg->map_size ? cfg->map_size : 1u << 16;
    if (map_size < 2 || (map_size & (map_size - 1))) {
        set_err(err, errlen, "the coverage map size must be a power of two");
        return NULL;
    }
    for (size_t i = 0; i < cfg->n_end_pcs; i++) {
        bool clash = cfg->end_pcs[i] == cfg->begin_pc;
        for (size_t k = 0; k < cfg->n_more_begin_pcs; k++)
            clash |= cfg->end_pcs[i] == cfg->more_begin_pcs[k];
        if (clash) {
            set_err(err, errlen, "an end pc equals a begin pc");
            return NULL;
        }
    }
    uam_fuzz *f = calloc(1, sizeof(*f));
    if (!f || !(f->map = calloc(1, map_size))
        || (cfg->n_crash_pcs && !(f->crash_pcs = malloc(sizeof(uint64_t) * cfg->n_crash_pcs)))
        || (cfg->n_end_pcs && !(f->end_pcs = malloc(sizeof(uint64_t) * cfg->n_end_pcs)))
        || !(f->begin_pcs = malloc(sizeof(uint64_t) * (1 + cfg->n_more_begin_pcs)))) {
        if (f) {
            free(f->map);
            free(f->crash_pcs);
            free(f->end_pcs);
        }
        free(f);
        set_err(err, errlen, "out of memory");
        return NULL;
    }
    f->m = m;
    f->cfg = *cfg;
    f->cfg.map_size = map_size;
    if (!f->cfg.iter_insns)
        f->cfg.iter_insns = UAM_FUZZ_DEFAULT_ITER_INSNS;
    if (cfg->n_crash_pcs)
        memcpy(f->crash_pcs, cfg->crash_pcs, sizeof(uint64_t) * cfg->n_crash_pcs);
    f->cfg.crash_pcs = f->crash_pcs;
    /* One hook per distinct end pc: a duplicate would close two levels at once. */
    for (size_t i = 0; i < cfg->n_end_pcs; i++) {
        bool dup = false;
        for (size_t k = 0; k < f->n_end_pcs; k++)
            dup |= f->end_pcs[k] == cfg->end_pcs[i];
        if (!dup)
            f->end_pcs[f->n_end_pcs++] = cfg->end_pcs[i];
    }
    f->cfg.end_pcs = f->end_pcs;
    f->cfg.n_end_pcs = f->n_end_pcs;
    /* Likewise one hook per distinct begin pc, cfg.begin_pc first. */
    f->begin_pcs[f->n_begin_pcs++] = cfg->begin_pc;
    for (size_t i = 0; i < cfg->n_more_begin_pcs; i++) {
        bool dup = false;
        for (size_t k = 0; k < f->n_begin_pcs; k++)
            dup |= f->begin_pcs[k] == cfg->more_begin_pcs[i];
        if (!dup)
            f->begin_pcs[f->n_begin_pcs++] = cfg->more_begin_pcs[i];
    }
    f->cfg.more_begin_pcs = f->begin_pcs + 1;
    f->cfg.n_more_begin_pcs = f->n_begin_pcs - 1;
    uam_cov_state_init(&f->cov, f->map, map_size, &cfg->history);
    f->cov.skip_repeat = cfg->skip_repeats;
    f->ncpus = uam_machine_ncpus(m);
    f->snap_last_pc = malloc(sizeof(uint64_t) * f->ncpus);
    if (!f->snap_last_pc) {
        uam_fuzz_destroy(f);
        set_err(err, errlen, "out of memory");
        return NULL;
    }
    for (unsigned i = 0; i < f->ncpus; i++)
        f->snap_last_pc[i] = UAM_COV_NO_PC;
    f->scheme = "none";

    /* Every processor's arrivals, exceptions and interrupts count. */
    uam_status st = UAM_OK;
    for (size_t i = 0; st == UAM_OK && i < f->n_begin_pcs; i++)
        st = uam_machine_add_pc_hook(m, f->begin_pcs[i], begin_hook, f);
    for (size_t i = 0; st == UAM_OK && i < f->n_end_pcs; i++)
        st = uam_machine_add_pc_hook(m, f->end_pcs[i], end_hook, f);
    for (size_t i = 0; st == UAM_OK && i < cfg->n_crash_pcs; i++)
        st = uam_machine_add_pc_hook(m, cfg->crash_pcs[i], crash_hook, f);
    if (st != UAM_OK) {
        uam_fuzz_destroy(f);
        set_err(err, errlen, "cannot install the boundary hooks");
        return NULL;
    }
    uam_machine_set_exc_hook(m, exc_hook, f);
    if (cfg->crash_irq_lines)
        uam_machine_set_irq_hook(m, irq_hook, f);
    f->prev_reset = m->on_reset;
    f->prev_reset_ctx = m->on_reset_ctx;
    m->on_reset = reset_hook;
    m->on_reset_ctx = f;
    return f;
}

void uam_fuzz_destroy(uam_fuzz *f)
{
    if (!f)
        return;
    uam_machine *m = f->m;
    uam_exec *x = &m->exec;
    for (size_t i = 0; i < f->n_begin_pcs; i++)
        uam_machine_remove_pc_hook(m, f->begin_pcs[i], begin_hook, f);
    for (size_t i = 0; i < f->n_end_pcs; i++)
        uam_machine_remove_pc_hook(m, f->end_pcs[i], end_hook, f);
    for (size_t i = 0; i < f->cfg.n_crash_pcs; i++)
        uam_machine_remove_pc_hook(m, f->crash_pcs[i], crash_hook, f);
    if (x->exc_hook == exc_hook)
        uam_machine_set_exc_hook(m, NULL, NULL);
    if (x->irq_hook == irq_hook)
        uam_machine_set_irq_hook(m, NULL, NULL);
    if (m->on_reset == reset_hook) {
        m->on_reset = f->prev_reset;
        m->on_reset_ctx = f->prev_reset_ctx;
    }
    if (f->has_cov)
        uam_machine_set_coverage(m, NULL, 0, NULL, NULL, 0);
    free(f->snap_last_pc);
    uam_snapshot_free(f->snap);
    for (size_t i = 0; i < f->n_routes; i++)
        free(f->routes[i].name);
    free(f->routes);
    free(f->crash_pcs);
    free(f->end_pcs);
    free(f->begin_pcs);
    free(f->marker);
    free(f->map);
    free(f);
}

static void name_scheme(uam_fuzz *f, const char *scheme)
{
    snprintf(f->scheme_buf, sizeof(f->scheme_buf), "%s", scheme ? scheme : "sites");
    f->scheme = f->scheme_buf;
}

static bool is_begin(const uam_fuzz *f, uint64_t pc)
{
    for (size_t i = 0; i < f->n_begin_pcs; i++)
        if (f->begin_pcs[i] == pc)
            return true;
    return false;
}

static bool is_boundary(const uam_fuzz *f, uint64_t pc)
{
    if (is_begin(f, pc))
        return true;
    for (size_t i = 0; i < f->n_end_pcs; i++)
        if (f->end_pcs[i] == pc)
            return true;
    return false;
}

uam_status uam_fuzz_set_sites(uam_fuzz *f, const uam_cov_site *sites, size_t n,
                              const char *scheme, char *err, size_t errlen)
{
    if (f->booted)
        return UAM_ERR_INVALID;
    /* Boundary sites go to the hooks (count_marker), the rest to the coverage pass. */
    uam_cov_site *body = n ? malloc(sizeof(*body) * n) : NULL;
    uam_cov_site *marker = n ? malloc(sizeof(*marker) * n) : NULL;
    if (n && (!body || !marker)) {
        free(body);
        free(marker);
        set_err(err, errlen, "out of memory");
        return UAM_ERR_NOMEM;
    }
    size_t n_body = 0, n_marker = 0;
    for (size_t i = 0; i < n; i++) {
        if (is_boundary(f, sites[i].pc))
            marker[n_marker++] = sites[i];
        else
            body[n_body++] = sites[i];
    }
    free(f->marker);
    f->marker = marker;
    f->n_marker = n_marker;
    /* With only boundary sites there is no pass at all (n_body 0 turns it off). */
    uam_status st = uam_machine_set_coverage(f->m, body, n_body, &f->cov, err, errlen);
    free(body);
    f->has_cov = st == UAM_OK && n;
    if (!f->has_cov)
        f->n_marker = 0;
    name_scheme(f, f->has_cov ? scheme : "none");
    return st;
}

uam_status uam_fuzz_set_block_coverage(uam_fuzz *f)
{
    if (f->booted)
        return UAM_ERR_INVALID;
    uam_status st = uam_machine_set_block_coverage(f->m, &f->cov);
    f->has_cov = st == UAM_OK;
    name_scheme(f, f->has_cov ? "blocks" : "none");
    return st;
}

/* ---- routes ------------------------------------------------------------ */

uam_status uam_fuzz_add_route(uam_fuzz *f, const char *name, uam_fuzz_deliver_fn fn, void *ctx)
{
    if (!name || !fn)
        return UAM_ERR_INVALID;
    for (size_t i = 0; i < f->n_routes; i++)
        if (!strcmp(f->routes[i].name, name))
            return UAM_ERR_INVALID;
    route *nr = realloc(f->routes, sizeof(*nr) * (f->n_routes + 1));
    if (!nr)
        return UAM_ERR_NOMEM;
    f->routes = nr;
    char *nm = strdup(name);
    if (!nm)
        return UAM_ERR_NOMEM;
    f->routes[f->n_routes++] = (route){ .name = nm, .fn = fn, .ctx = ctx };
    return UAM_OK;
}

uam_status uam_fuzz_mmio_deliver(void *ctx, uam_machine *m, const uint8_t *data, size_t len)
{
    (void)m;
    uam_mmio_input_set(ctx, data, len);
    return UAM_OK;
}

/* Copy bytes to guest virtual memory, a page at a time through the ISA's translation. */
static uam_status write_virt(uam_machine *m, uint64_t va, const uint8_t *data, size_t len)
{
    uam_cpu *cpu = &m->cpu;
    while (len) {
        size_t chunk = UAM_PAGE_SIZE - (va & (UAM_PAGE_SIZE - 1));
        if (chunk > len)
            chunk = len;
        uint64_t pa = va;
        if (cpu->isa->translate) {
            uam_xlate xl = { 0 };
            if (cpu->isa->translate(cpu, va, UAM_XLATE_STORE, &xl) != UAM_OK)
                return UAM_ERR_XLATE;
            pa = xl.paddr;
        }
        uam_status st = uam_bus_dma_write(&m->bus, pa, data, chunk);
        if (st != UAM_OK)
            return st;
        va += chunk;
        data += chunk;
        len -= chunk;
    }
    return UAM_OK;
}

/* No-op observer for a guard watch: a guarded access faults in the bus before fire_watches,
 * so this is never actually called; it only satisfies uam_bus_add_watch's non-NULL fn. */
static void guard_noop(void *ctx, uint64_t a, unsigned s, bool w, uint64_t v)
{
    (void)ctx; (void)a; (void)s; (void)w; (void)v;
}

static uam_status virt_to_phys(uam_machine *m, uint64_t va, uint64_t *pa)
{
    uam_cpu *cpu = &m->cpu;
    if (!cpu->isa->translate) { *pa = va; return UAM_OK; }
    uam_xlate xl = { 0 };
    if (cpu->isa->translate(cpu, va, UAM_XLATE_LOAD, &xl) != UAM_OK)
        return UAM_ERR_XLATE;
    *pa = xl.paddr;
    return UAM_OK;
}

/* Re-arm the read-fault guard at addr + L for this iteration (see mem_route.guard). */
static void arm_guard(mem_route *r, uam_machine *m, const uint8_t *data, size_t len)
{
    if (r->guard_wid > 0) {
        uam_bus_remove_watch(&m->bus, r->guard_wid);
        r->guard_wid = 0;
    }
    uint64_t L;
    if (r->guard_field) {
        if (r->guard_off + r->guard_sz > len)
            return;                         /* the length field is not in the input */
        bool be = m->cpu.isa->default_endian & UAM_ACC_BE;
        uint64_t field = 0;
        for (unsigned i = 0; i < r->guard_sz; i++)
            field |= (uint64_t)data[r->guard_off + i] << (be ? 8 * (r->guard_sz - 1 - i) : 8 * i);
        int64_t LL = (int64_t)field + r->guard_delta;   /* delta skips a known N-past read */
        if (LL <= 0)
            return;
        L = (uint64_t)LL;
    } else {
        L = len;
    }
    if (L == 0 || L > r->max + UAM_PAGE_SIZE)
        return;                             /* nothing legitimate to read, or absurdly past our window */
    uint64_t pa;
    if (virt_to_phys(m, r->addr + L, &pa) != UAM_OK)
        return;
    uint64_t span = UAM_PAGE_SIZE - (pa & (UAM_PAGE_SIZE - 1));   /* stay within one page (PA) */
    int id = uam_bus_add_watch(&m->bus, pa, span, UAM_WATCH_FAULT | UAM_WATCH_READ,
                               guard_noop, NULL);
    if (id > 0)
        r->guard_wid = id;
}

/* Re-arm the fixed absolute read-fault guard [guard_abs_addr, +guard_abs_n) (see mem_route). */
static void arm_guard_abs(mem_route *r, uam_machine *m)
{
    if (r->guard_abs_wid > 0) {
        uam_bus_remove_watch(&m->bus, r->guard_abs_wid);
        r->guard_abs_wid = 0;
    }
    uint64_t pa;
    if (virt_to_phys(m, r->guard_abs_addr, &pa) != UAM_OK)
        return;
    uint64_t rem = UAM_PAGE_SIZE - (pa & (UAM_PAGE_SIZE - 1));
    uint64_t n = r->guard_abs_n < rem ? r->guard_abs_n : rem;    /* stay within one page (PA) */
    int id = uam_bus_add_watch(&m->bus, pa, n, UAM_WATCH_FAULT | UAM_WATCH_READ, guard_noop, NULL);
    if (id > 0)
        r->guard_abs_wid = id;
}

static uam_status mem_deliver(void *ctx, uam_machine *m, const uint8_t *data, size_t len)
{
    mem_route *r = ctx;
    if (len > r->max && r->reject)
        return UAM_ERR_REJECTED;
    if (len > r->max)
        len = (size_t)r->max;
    uam_status st = write_virt(m, r->addr, data, len);
    if (st == UAM_OK && r->has_len) {
        uint8_t w[4];
        bool be = m->cpu.isa->default_endian & UAM_ACC_BE;
        for (int i = 0; i < 4; i++)
            w[i] = (uint8_t)((uint32_t)len >> (be ? 24 - 8 * i : 8 * i));
        st = write_virt(m, r->len_addr, w, 4);
    }
    if (st == UAM_OK && r->guard)
        arm_guard(r, m, data, len);
    if (st == UAM_OK && r->guard_abs)
        arm_guard_abs(r, m);
    return st;
}

static bool parse_u64(const char *s, size_t n, uint64_t *out)
{
    char buf[32];
    if (!n || n >= sizeof(buf))
        return false;
    memcpy(buf, s, n);
    buf[n] = '\0';
    char *end;
    *out = strtoull(buf, &end, 0);
    return *end == '\0';
}

static bool parse_mem(mem_route *r, const char *spec)
{
    *r = (mem_route){ .max = 4096 };
    size_t n = strcspn(spec, ",");
    if (!parse_u64(spec, n, &r->addr))
        return false;
    for (const char *p = spec + n; *p == ',';) {
        p++;
        n = strcspn(p, ",");
        if (n > 4 && !strncmp(p, "len=", 4)) {
            if (!parse_u64(p + 4, n - 4, &r->len_addr))
                return false;
            r->has_len = true;
        } else if (n > 4 && !strncmp(p, "max=", 4)) {
            if (!parse_u64(p + 4, n - 4, &r->max))
                return false;
        } else if (n == 6 && !strncmp(p, "reject", 6)) {
            r->reject = true;
        } else if (n == 5 && !strncmp(p, "guard", 5)) {
            r->guard = true;                 /* guard at addr + delivered byte count */
        } else if (n > 7 && !strncmp(p, "guard=@", 7)) {
            /* guard=@OFF:SZ - the legit length is the SZ-byte field at input offset OFF */
            char *e;
            r->guard_off = strtoull(p + 7, &e, 0);
            if (*e != ':')
                return false;
            r->guard_sz = (unsigned)strtoul(e + 1, &e, 0);
            if (r->guard_sz != 1 && r->guard_sz != 2 && r->guard_sz != 4)
                return false;
            if (*e == '+' || *e == '-')     /* optional +DELTA/-DELTA: guard at field + delta */
                r->guard_delta = (int64_t)strtoll(e, &e, 0);
            if (e != p + n)
                return false;
            r->guard = r->guard_field = true;
        } else if (n > 9 && !strncmp(p, "guardabs=", 9)) {
            /* guardabs=ADDR:N - a fixed read-fault redzone at absolute [ADDR, ADDR+N) */
            char *e;
            r->guard_abs_addr = strtoull(p + 9, &e, 0);
            if (*e != ':')
                return false;
            r->guard_abs_n = strtoull(e + 1, &e, 0);
            if (r->guard_abs_n == 0 || e != p + n)
                return false;
            r->guard_abs = true;
        } else {
            return false;
        }
        p += n;
    }
    return true;
}

uam_status uam_fuzz_select_route(uam_fuzz *f, const char *spec, char *err, size_t errlen)
{
    if (err && errlen)
        err[0] = '\0';
    if (!strncmp(spec, "mem:", 4)) {
        if (!parse_mem(&f->mem, spec + 4)) {
            set_err(err, errlen, "expected mem:ADDR[,len=ADDR][,max=N][,reject]"
                                 "[,guard|,guard=@OFF:SZ[+-D]][,guardabs=ADDR:N]");
            return UAM_ERR_INVALID;
        }
        f->deliver = mem_deliver;
        f->deliver_ctx = &f->mem;
        return UAM_OK;
    }
    for (size_t i = 0; i < f->n_routes; i++) {
        if (!strcmp(f->routes[i].name, spec)) {
            f->deliver = f->routes[i].fn;
            f->deliver_ctx = f->routes[i].ctx;
            return UAM_OK;
        }
    }
    const uam_machine_input *in = uam_machine_find_input(f->m, spec);
    if (in) {
        f->deliver = in->fn;
        f->deliver_ctx = in->ctx;
        return UAM_OK;
    }
    if (err && errlen) {
        int k = snprintf(err, errlen, "unknown input route '%s' (known: mem:ADDR", spec);
        for (size_t i = 0; i < f->n_routes && k > 0 && (size_t)k < errlen; i++)
            k += snprintf(err + k, errlen - (size_t)k, ", %s", f->routes[i].name);
        for (uint32_t i = 0; i < f->m->n_inputs && k > 0 && (size_t)k < errlen; i++)
            k += snprintf(err + k, errlen - (size_t)k, ", %s", f->m->inputs[i].name);
        if (k > 0 && (size_t)k < errlen)
            snprintf(err + k, errlen - (size_t)k, ")");
    }
    return UAM_ERR_INVALID;
}

/* ---- running ----------------------------------------------------------- */

static void clear_flags(uam_fuzz *f)
{
    f->at_begin = f->ended = f->oracle = f->reset = f->exc_crash = f->irq_crash = false;
    f->depth = 1;   /* an iteration starts inside its begin */
}

uam_status uam_fuzz_boot(uam_fuzz *f, uam_fuzz_result *r)
{
    uam_machine *m = f->m;
    memset(r, 0, sizeof(*r));
    if (f->booted)
        return UAM_ERR_INVALID;
    clear_flags(f);
    f->boot_arrivals = 0;
    uint64_t ic0 = uam_machine_icount(m);
    f->hit_cpu = &m->cpu;
    if (is_begin(f, m->cpu.pc)) {
        /* Already there (e.g. resumed from a snapshot taken at begin): an arrival whose
         * hook must not run again when the hart starts. */
        count_marker(f, &m->cpu, m->cpu.pc);
        if (f->boot_arrivals++ >= f->cfg.begin_skip) {
            f->at_begin = true;
            r->stop = UAM_STOP_HOOK;
        } else {
            m->exec.hook_fired = true;
            m->exec.hook_fired_pc = m->cpu.pc;
            m->exec.hook_fired_icount = m->cpu.icount;
        }
    }
    if (!f->at_begin) {
        f->ph = PH_BOOT;
        r->stop = uam_exec_run(&m->exec, f->cfg.boot_insns);
        f->ph = PH_IDLE;
    }
    r->pc = f->hit_cpu->pc;
    r->insns = uam_machine_icount(m) - ic0;
    if (!f->at_begin || f->aborted) {
        if (!f->aborted)
            uam_log(UAM_LOG_ERROR, "fuzz", "boot did not reach the begin pc 0x%llx%s%s: %s at pc "
                    "0x%llx (%u arrivals)", (unsigned long long)f->cfg.begin_pc,
                    f->n_begin_pcs > 1 ? " (or another begin)" : "",
                    f->cfg.begin_skip ? " often enough" : "", uam_stop_reason_str(r->stop),
                    (unsigned long long)r->pc, f->boot_arrivals);
        return UAM_ERR_INVALID;
    }
    /* The hooks have run for this arrival at begin: iterations start past them, in the
     * snapshot as well, so begin-only mode does not end an iteration at its first pc. */
    uam_exec *hx = uam_machine_exec(m, f->hit_cpu->hartid);
    hx->hook_fired = true;
    hx->hook_fired_pc = f->hit_cpu->pc;
    hx->hook_fired_icount = f->hit_cpu->icount;
    uam_status st = f->boot_fn ? f->boot_fn(f->boot_ctx, f, m) : UAM_OK;
    if (st != UAM_OK)
        return st;
    st = uam_snapshot_take(m, &f->snap);
    if (st != UAM_OK) {
        uam_log(UAM_LOG_ERROR, "fuzz", "cannot snapshot the machine at the begin pc");
        return st;
    }
    for (unsigned i = 0; i < f->ncpus; i++) {
        const uam_cov_state *cs = uam_machine_cov_state(m, i);
        f->snap_last_pc[i] = (cs ? cs : &f->cov)->last_pc;
    }
    f->booted = true;
    f->last = *r;
    return UAM_OK;
}

static void classify(uam_fuzz *f, uam_fuzz_result *r)
{
    const uam_machine *m = f->m;
    r->verdict = UAM_FUZZ_OK;
    r->detail = 0;
    if (f->ended && r->stop == UAM_STOP_HOOK)
        return;
    if (f->exc_crash) {
        r->verdict = UAM_FUZZ_CRASH_FAULT;
        r->exc_valid = true;
        r->exc = f->exc;
        r->detail = f->exc.pc;
        return;
    }
    if (f->oracle) {
        r->verdict = UAM_FUZZ_CRASH_ORACLE;
        r->detail = f->oracle_pc;
        return;
    }
    if (f->irq_crash) {
        r->verdict = UAM_FUZZ_CRASH_ORACLE;
        r->irq_valid = true;
        r->irq = f->irq;
        r->detail = f->irq.lines & f->cfg.crash_irq_lines;
        return;
    }
    if (f->reset) {
        r->verdict = UAM_FUZZ_CRASH_ORACLE;
        r->reset = true;
        return;
    }
    switch (r->stop) {
    case UAM_STOP_REQUEST:
    case UAM_STOP_HALT:
        /* The firmware stopped the machine (a test finisher, a halt): a clean end of
         * the iteration with status 0, an assertion otherwise. */
        if (m->exec.stop_status != 0) {
            r->verdict = UAM_FUZZ_CRASH_ORACLE;
            r->detail = (uint64_t)m->exec.stop_status;
        }
        return;
    case UAM_STOP_INSN_LIMIT:
    case UAM_STOP_TIME_LIMIT:
        r->verdict = UAM_FUZZ_TIMEOUT;
        return;
    case UAM_STOP_DEADLOCK:
    case UAM_STOP_LIVELOCK:
        r->verdict = UAM_FUZZ_HANG;
        r->detail = (uint64_t)r->stop;
        return;
    case UAM_STOP_FAULT_LOOP:
        r->verdict = UAM_FUZZ_CRASH_FAULT;
        r->detail = r->pc;
        return;
    default:
        /* A debug trap (a breakpoint instruction), or a hook that is not ours. */
        r->verdict = UAM_FUZZ_CRASH_ORACLE;
        r->detail = r->pc;
        return;
    }
}

uam_status uam_fuzz_run_one(uam_fuzz *f, const uint8_t *data, size_t len, uam_fuzz_result *r)
{
    uam_machine *m = f->m;
    memset(r, 0, sizeof(*r));
    if (!f->booted || f->aborted)
        return UAM_ERR_INVALID;
    if (!f->deliver) {
        uam_log(UAM_LOG_ERROR, "fuzz", "no input route selected");
        return UAM_ERR_INVALID;
    }
    uam_status st = uam_snapshot_restore(m, f->snap);
    if (st != UAM_OK) {
        uam_log(UAM_LOG_ERROR, "fuzz", "restoring the begin snapshot failed");
        return st;
    }
    memset(f->map, 0, f->cfg.map_size);
    uam_cov_reset_prev(&f->cov);
    f->cov.last_pc = f->snap_last_pc[0];
    for (unsigned i = 1; i < f->ncpus; i++) {
        uam_cov_state *cs = uam_machine_cov_state(m, i);
        if (cs) {
            uam_cov_reset_prev(cs);
            cs->last_pc = f->snap_last_pc[i];
        }
    }
    clear_flags(f);
    st = f->deliver(f->deliver_ctx, m, data, len);
    if (st == UAM_ERR_REJECTED) {
        /* The input's fault, not the harness's: nothing ran, the next input goes on. */
        r->verdict = UAM_FUZZ_REJECTED;
        r->pc = m->cpu.pc;
        f->stats.iterations++;
        f->stats.by_verdict[r->verdict]++;
        f->last = *r;
        return UAM_OK;
    }
    if (st != UAM_OK) {
        uam_log(UAM_LOG_ERROR, "fuzz", "delivering an input of %zu bytes failed: %s", len,
                uam_status_str(st));
        return st;
    }
    uint64_t ic0 = uam_machine_icount(m);
    f->hit_cpu = &m->cpu;
    f->ph = PH_ITER;
    r->stop = uam_exec_run(&m->exec, f->cfg.iter_insns);
    f->ph = PH_IDLE;
    r->pc = f->hit_cpu->pc;   /* the processor that ended it (processor 0 otherwise) */
    r->insns = uam_machine_icount(m) - ic0;
    if (f->aborted)
        return UAM_ERR_INVALID;
    if (r->stop == UAM_STOP_ERROR) {
        /* ember itself failed (bad IR, a site it cannot instrument): not the input's
         * doing, and every later input would fail the same way. */
        uam_log(UAM_LOG_ERROR, "fuzz", "internal error at pc 0x%llx: stopping",
                (unsigned long long)r->pc);
        return UAM_ERR_INVALID;
    }
    classify(f, r);
    f->stats.iterations++;
    f->stats.by_verdict[r->verdict]++;
    f->last = *r;
    return UAM_OK;
}

void uam_fuzz_get_info(const uam_fuzz *f, uam_fuzz_info *info)
{
    *info = (uam_fuzz_info){ .map_size = f->cfg.map_size, .scheme = f->scheme,
                             .arch = f->m->cpu.isa->name, .isa = f->m->cpu.isa->name };
}

uam_status uam_fuzz_run(uam_fuzz *f, const uam_fuzz_frontend *fe, void *ctx)
{
    /* Boot first: it may take long (a whole firmware start-up), and a frontend's peer
     * should not wait on it between hello and the first input. */
    uam_fuzz_result r;
    uam_status st = uam_fuzz_boot(f, &r);
    uam_fuzz_info info;
    uam_fuzz_get_info(f, &info);
    uam_status hs = fe->hello ? fe->hello(ctx, &info) : UAM_OK;
    if (st == UAM_OK)
        st = hs;
    if (st == UAM_OK && f->obs_fn)
        f->obs_fn(f->obs_ctx, 0, UAM_FUZZ_KIND_INIT, UAM_FUZZ_OK, 0, f->map, f->cfg.map_size);
    if (st == UAM_OK)
        st = fe->report(ctx, 0, UAM_FUZZ_OK, 0, f->map, f->cfg.map_size, UAM_FUZZ_KIND_INIT);
    while (st == UAM_OK && !f->aborted) {
        uint8_t *data;
        size_t len;
        uint64_t id;
        int n = fe->next_input(ctx, &data, &len, &id);
        if (n == 0)
            break;
        if (n < 0) {
            st = UAM_ERR_INVALID;
            break;
        }
        st = uam_fuzz_run_one(f, data, len, &r);
        if (st != UAM_OK)
            break;
        if (f->obs_fn)
            f->obs_fn(f->obs_ctx, id, UAM_FUZZ_KIND_ITER, r.verdict, r.detail, f->map,
                      f->cfg.map_size);
        st = fe->report(ctx, id, r.verdict, r.detail, f->map, f->cfg.map_size, UAM_FUZZ_KIND_ITER);
    }
    if (fe->bye)
        fe->bye(ctx);
    return f->aborted ? UAM_OK : st;
}

const uint8_t *uam_fuzz_map(const uam_fuzz *f, size_t *size)
{
    if (size)
        *size = f->cfg.map_size;
    return f->map;
}

const uam_fuzz_result *uam_fuzz_last_result(const uam_fuzz *f) { return &f->last; }
const uam_fuzz_stats  *uam_fuzz_get_stats(const uam_fuzz *f) { return &f->stats; }

void uam_fuzz_abort(uam_fuzz *f)
{
    f->aborted = 1;
    uam_exec_stop(&f->m->exec, 130);
}

bool uam_fuzz_aborted(const uam_fuzz *f) { return f->aborted; }
