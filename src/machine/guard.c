/*
 * guard.c - Guarded and watched guest memory (uam/guard.h).
 *
 * Each guard is one bus watch per physically contiguous piece of its range: a fault
 * watch (UAM_WATCH_FAULT) for STOP and FAULT, a plain one for LOG. The watch callback
 * records the hit and, for STOP, asks the running processor's executor to stop at the
 * exception the refused access raises.
 */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "uam/guard.h"

typedef struct guard guard;

typedef struct piece {
    guard   *g;
    uint64_t pa, len;   /* physical [pa, pa + len) */
    uint64_t off;       /* where it starts in the guard's range */
    int      wid;
} piece;

struct guard {
    struct uam_guard_set *set;
    int                   id;
    char                 *name;
    uam_guard_spec        spec;
    piece                *pieces;
    uint32_t              n_pieces;
};

struct uam_guard_set {
    uam_machine          *m;
    guard               **g;
    uint32_t              n, cap;
    int                   next_id;
    uint64_t              hits;
    bool                  has_last;
    uam_guard_hit         last;
    uam_guard_observer_fn observer;
    void                 *observer_ctx;
    FILE                 *log;
};

static void set_err(char *err, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void set_err(char *err, size_t n, const char *fmt, ...)
{
    if (!err || !n)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

const char *uam_guard_action_str(uam_guard_action a)
{
    switch (a) {
    case UAM_GUARD_STOP:  return "stop";
    case UAM_GUARD_FAULT: return "fault";
    case UAM_GUARD_LOG:   return "log";
    }
    return "?";
}

static struct uam_guard_set *guard_set(uam_machine *m)
{
    if (!m->guards) {
        m->guards = calloc(1, sizeof(*m->guards));
        if (m->guards) {
            m->guards->m = m;
            m->guards->next_id = 1;
            m->guards->log = stderr;
        }
    }
    return m->guards;
}

void uam_guard_hit_format(const uam_guard_hit *h, char *buf, size_t n)
{
    int64_t rel = (int64_t)(h->addr - h->start);
    char where[64];
    if (rel < 0)
        snprintf(where, sizeof(where), "%llu before its start", (unsigned long long)-rel);
    else
        snprintf(where, sizeof(where), "+%llu", (unsigned long long)rel);
    int k = snprintf(buf, n, "guard %s (%s): cpu %u pc 0x%llx %s %u at 0x%llx (%s",
                     h->name, uam_guard_action_str(h->action), h->cpu, (unsigned long long)h->pc,
                     h->is_write ? "write" : "read", h->size, (unsigned long long)h->addr, where);
    if (k < 0 || (size_t)k >= n)
        return;
    if (h->paddr != h->addr)
        k += snprintf(buf + k, n - (size_t)k, ", physical 0x%llx", (unsigned long long)h->paddr);
    if (k < 0 || (size_t)k >= n)
        return;
    if (h->is_write || h->action == UAM_GUARD_LOG)
        snprintf(buf + k, n - (size_t)k, ") value 0x%llx", (unsigned long long)h->val);
    else
        snprintf(buf + k, n - (size_t)k, ")");
}

static void on_access(void *ctx, uint64_t pa, unsigned size, bool is_write, uint64_t val)
{
    const piece *p = ctx;
    const guard *g = p->g;
    struct uam_guard_set *s = g->set;
    uam_machine *m = s->m;
    unsigned hart = uam_machine_current_cpu(m);
    const bool refused = g->spec.action != UAM_GUARD_LOG;
    s->last = (uam_guard_hit){
        .id = g->id, .name = g->name, .action = g->spec.action, .cpu = hart,
        .pc = uam_machine_cpu(m, hart)->insn_pc,   /* the accessing instruction */
        .addr = g->spec.addr + p->off + (pa - p->pa),   /* wraps when it starts earlier */
        .start = g->spec.addr, .len = g->spec.len, .paddr = pa, .size = size,
        .is_write = is_write, .val = refused && !is_write ? 0 : val,
    };
    s->has_last = true;
    s->hits++;
    if (g->spec.action == UAM_GUARD_STOP)
        uam_exec_request_fault_stop(uam_machine_exec(m, hart), &s->last.pc);
    if (g->spec.action == UAM_GUARD_LOG && s->log) {
        char line[256];
        uam_guard_hit_format(&s->last, line, sizeof(line));
        fprintf(s->log, "%s\n", line);
    }
    if (s->observer)
        s->observer(s->observer_ctx, m, &s->last);
}

static void free_guard(uam_machine *m, guard *g)
{
    for (uint32_t i = 0; i < g->n_pieces; i++)
        if (g->pieces[i].wid > 0)
            uam_bus_remove_watch(&m->bus, g->pieces[i].wid);
    free(g->pieces);
    free(g->name);
    free(g);
}

/* The physical pieces of g's range: one per page for a virtual range, merged where
 * consecutive pages are physically contiguous. A guard watches loads and stores, so a
 * virtual range is translated as data is: where data and code live in different
 * spaces (AVR, PIC) the debug view is the code space. A store-only guard takes the
 * store translation, any other the load one; if that is refused (privilege,
 * protection) the debug view, which ignores both, decides. */
static bool split(uam_machine *m, guard *g, char *err, size_t errlen)
{
    const uam_guard_spec *s = &g->spec;
    uam_cpu *cpu = uam_machine_cpu(m, 0);
    const bool virt = !s->phys && cpu->isa->translate;
    const uam_xlate_kind kind = s->dir == UAM_GUARD_WRITE ? UAM_XLATE_STORE : UAM_XLATE_LOAD;
    uint64_t cap = virt ? s->len / UAM_PAGE_SIZE + 2 : 1;
    g->pieces = calloc(cap, sizeof(*g->pieces));
    if (!g->pieces) {
        set_err(err, errlen, "out of memory");
        return false;
    }
    for (uint64_t off = 0; off < s->len;) {
        uint64_t va = s->addr + off, pa = va, chunk = s->len - off;
        if (virt) {
            uint64_t room = UAM_PAGE_SIZE - (va & (UAM_PAGE_SIZE - 1));
            if (chunk > room)
                chunk = room;
            uam_xlate xl = { 0 };
            if (cpu->isa->translate(cpu, va, kind, &xl) != UAM_OK
                && cpu->isa->translate(cpu, va, UAM_XLATE_DEBUG, &xl) != UAM_OK) {
                set_err(err, errlen, "processor 0 does not map virtual 0x%llx",
                        (unsigned long long)va);
                return false;
            }
            pa = xl.paddr;
        }
        piece *last = g->n_pieces ? &g->pieces[g->n_pieces - 1] : NULL;
        if (last && last->pa + last->len == pa)
            last->len += chunk;
        else
            g->pieces[g->n_pieces++] = (piece){ .g = g, .pa = pa, .len = chunk, .off = off };
        off += chunk;
    }
    return true;
}

int uam_machine_add_guard(uam_machine *m, const uam_guard_spec *spec, char *err, size_t errlen)
{
    if (err && errlen)
        err[0] = '\0';
    if (!spec->len || spec->addr + spec->len - 1 < spec->addr) {
        set_err(err, errlen, "a guard needs a length that stays within the address space");
        return -1;
    }
    if (!(spec->dir & UAM_GUARD_RW) || (spec->dir & ~(unsigned)UAM_GUARD_RW)) {
        set_err(err, errlen, "a guard's direction is read, write or both");
        return -1;
    }
    if (spec->action != UAM_GUARD_STOP && spec->action != UAM_GUARD_FAULT
        && spec->action != UAM_GUARD_LOG) {
        set_err(err, errlen, "unknown guard action");
        return -1;
    }
    struct uam_guard_set *s = guard_set(m);
    if (s && s->n == s->cap) {
        uint32_t ncap = s->cap ? 2 * s->cap : 4;
        guard **ng = realloc(s->g, sizeof(*ng) * ncap);
        if (ng) {
            s->g = ng;
            s->cap = ncap;
        }
    }
    guard *g = s && s->n < s->cap ? calloc(1, sizeof(*g)) : NULL;
    if (!g) {
        set_err(err, errlen, "out of memory");
        return -1;
    }
    g->set = s;
    g->id = s->next_id++;
    g->spec = *spec;
    char dflt[24];
    snprintf(dflt, sizeof(dflt), "guard%d", g->id);
    g->name = strdup(spec->name ? spec->name : dflt);
    g->spec.name = g->name;
    if (!g->name) {
        set_err(err, errlen, "out of memory");
        free_guard(m, g);
        return -1;
    }
    if (!split(m, g, err, errlen)) {
        free_guard(m, g);
        return -1;
    }
    unsigned kinds = spec->dir | (spec->action == UAM_GUARD_LOG ? 0u : (unsigned)UAM_WATCH_FAULT);
    for (uint32_t i = 0; i < g->n_pieces; i++) {
        piece *p = &g->pieces[i];
        p->wid = uam_bus_add_watch(&m->bus, p->pa, p->len, kinds, on_access, p);
        if (p->wid < 0) {
            set_err(err, errlen, "out of memory");
            free_guard(m, g);
            return -1;
        }
    }
    s->g[s->n++] = g;
    return g->id;
}

bool uam_machine_remove_guard(uam_machine *m, int id)
{
    struct uam_guard_set *s = m->guards;
    for (uint32_t i = 0; s && i < s->n; i++) {
        if (s->g[i]->id != id)
            continue;
        if (s->has_last && s->last.id == id)
            s->last.name = "(removed)";   /* the name goes with the guard */
        free_guard(m, s->g[i]);
        memmove(&s->g[i], &s->g[i + 1], sizeof(*s->g) * (s->n - i - 1));
        s->n--;
        return true;
    }
    return false;
}

void uam_machine_remove_guards(uam_machine *m)
{
    while (m->guards && m->guards->n)
        uam_machine_remove_guard(m, m->guards->g[m->guards->n - 1]->id);
}

/* Called by uam_machine_fini; the bus drops its watches itself. */
void uam_machine_guards_fini(uam_machine *m);
void uam_machine_guards_fini(uam_machine *m)
{
    struct uam_guard_set *s = m->guards;
    if (!s)
        return;
    for (uint32_t i = 0; i < s->n; i++) {
        free(s->g[i]->pieces);
        free(s->g[i]->name);
        free(s->g[i]);
    }
    free(s->g);
    free(s);
    m->guards = NULL;
}

const uam_guard_hit *uam_machine_guard_last_hit(const uam_machine *m)
{
    return m->guards && m->guards->has_last ? &m->guards->last : NULL;
}

uint64_t uam_machine_guard_hits(const uam_machine *m)
{
    return m->guards ? m->guards->hits : 0;
}

void uam_machine_set_guard_observer(uam_machine *m, uam_guard_observer_fn fn, void *ctx)
{
    struct uam_guard_set *s = guard_set(m);
    if (s) {
        s->observer = fn;
        s->observer_ctx = fn ? ctx : NULL;
    }
}

void uam_machine_set_guard_log(uam_machine *m, FILE *out)
{
    struct uam_guard_set *s = guard_set(m);
    if (s)
        s->log = out;
}

/* ---- text form ------------------------------------------------------------ */

static bool parse_num(const char *s, const char **end, uint64_t *out, bool suffix)
{
    char *e;
    if (*s < '0' || *s > '9')
        return false;
    uint64_t v = strtoull(s, &e, 0);
    if (suffix && (*e == 'k' || *e == 'K')) {
        v <<= 10;
        e++;
    } else if (suffix && (*e == 'm' || *e == 'M')) {
        v <<= 20;
        e++;
    }
    *out = v;
    *end = e;
    return true;
}

bool uam_guard_parse(const char *text, uam_guard_action default_action, uam_guard_spec *spec,
                     char *name, size_t namelen, char *err, size_t errlen)
{
    static const char form[] = "[p:]ADDR[-BEFORE][+AFTER][:r|w|rw][,stop|,fault|,log][,name=NAME]";
    if (err && errlen)
        err[0] = '\0';
    *spec = (uam_guard_spec){ .dir = UAM_GUARD_RW, .action = default_action };
    const char *s = text, *comma = strchr(text, ',');
    size_t range_len = comma ? (size_t)(comma - text) : strlen(text);
    if (!strncmp(s, "p:", 2)) {
        spec->phys = true;
        s += 2;
    }
    uint64_t anchor, before = 0, after = 0;
    bool sized = false;
    if (!parse_num(s, &s, &anchor, false)) {
        set_err(err, errlen, "'%s': expected %s", text, form);
        return false;
    }
    if (*s == '-') {
        if (!parse_num(s + 1, &s, &before, true) || !before) {
            set_err(err, errlen, "'%s': expected a byte count after '-'", text);
            return false;
        }
        sized = true;
    }
    if (*s == '+') {
        if (!parse_num(s + 1, &s, &after, true) || !after) {
            set_err(err, errlen, "'%s': expected a byte count after '+'", text);
            return false;
        }
        sized = true;
    }
    if (!sized)
        after = 1;
    if (*s == ':') {
        size_t n = (size_t)(text + range_len - (s + 1));
        if (n == 1 && s[1] == 'r')
            spec->dir = UAM_GUARD_READ;
        else if (n == 1 && s[1] == 'w')
            spec->dir = UAM_GUARD_WRITE;
        else if (n == 2 && !strncmp(s + 1, "rw", 2))
            spec->dir = UAM_GUARD_RW;
        else {
            set_err(err, errlen, "'%s': the direction is r, w or rw", text);
            return false;
        }
        s += 1 + n;
    }
    if (s != text + range_len) {
        set_err(err, errlen, "'%s': expected %s", text, form);
        return false;
    }
    if (before > anchor || (after && after - 1 > UINT64_MAX - anchor)) {
        set_err(err, errlen, "'%s': the range leaves the address space", text);
        return false;
    }
    spec->addr = anchor - before;
    spec->len = before + after;
    if (spec->len < before) {
        set_err(err, errlen, "'%s': the range is too long", text);
        return false;
    }
    /* The name: the range text unless name= says otherwise. */
    size_t nl = range_len < namelen ? range_len : namelen - 1;
    memcpy(name, text, nl);
    name[nl] = '\0';
    while (comma) {
        const char *o = comma + 1;
        comma = strchr(o, ',');
        size_t n = comma ? (size_t)(comma - o) : strlen(o);
        if (n == 4 && !strncmp(o, "stop", 4))
            spec->action = UAM_GUARD_STOP;
        else if (n == 5 && !strncmp(o, "fault", 5))
            spec->action = UAM_GUARD_FAULT;
        else if (n == 3 && !strncmp(o, "log", 3))
            spec->action = UAM_GUARD_LOG;
        else if (n > 5 && !strncmp(o, "name=", 5)) {
            /* the rest of the text: a name may hold commas */
            snprintf(name, namelen, "%s", o + 5);
            break;
        } else {
            set_err(err, errlen, "'%s': unknown option '%.*s' (stop, fault, log, name=NAME)",
                    text, (int)n, o);
            return false;
        }
    }
    spec->name = name;
    return true;
}

int uam_machine_add_guard_text(uam_machine *m, const char *text, uam_guard_action default_action,
                               char *err, size_t errlen)
{
    uam_guard_spec spec;
    char name[128];
    if (!uam_guard_parse(text, default_action, &spec, name, sizeof(name), err, errlen))
        return -1;
    return uam_machine_add_guard(m, &spec, err, errlen);
}
