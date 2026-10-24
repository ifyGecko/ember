/*
 * pc_misc.c - System Control Port A, POST-code latch and debug console port
 * (uam/dev/pc_misc.h).
 */
#include <stdio.h>
#include <stdlib.h>

#include "uam/dev/pc_misc.h"

/* ---- System Control Port A ----------------------------------------------- */

typedef struct sysctl {
    uam_device dev;
    uam_signal reset, a20;
    uint8_t    val;
} sysctl;

static sysctl *to_sysctl(uam_device *d) { return UAM_CONTAINER_OF(d, sysctl, dev); }

static uam_status sysctl_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    (void)off; (void)size;
    *val = to_sysctl(d)->val;
    return UAM_OK;
}

static uam_status sysctl_write(uam_device *d, uint64_t off, unsigned size, uint64_t v)
{
    (void)off; (void)size;
    sysctl *s = to_sysctl(d);
    bool rise = (v & 1) && !(s->val & 1);
    s->val = (uint8_t)v & 0x03;
    uam_signal_set(&s->a20, (s->val >> 1) & 1);   /* bit 1: A20 gate */
    if (rise) {
        uam_signal_set(&s->reset, 1);
        uam_signal_set(&s->reset, 0);
    }
    return UAM_OK;
}

static void sysctl_reset(uam_device *d)
{
    sysctl *s = to_sysctl(d);
    s->val = 0;
    uam_signal_set(&s->reset, 0);
    uam_signal_set(&s->a20, 0);
}

static void sysctl_save(uam_device *d, uam_state_writer *w) { uam_state_put_u8(w, to_sysctl(d)->val); }
static void sysctl_restore(uam_device *d, uam_state_reader *r)
{
    sysctl *s = to_sysctl(d);
    s->val = uam_state_get_u8(r);
    s->a20.level = (s->val >> 1) & 1;   /* its receiver keeps its own input level */
}
static void free_dev(uam_device *d) { free(d); }

static const uam_device_ops sysctl_ops = {
    .type = "pc,port92", .impl_min = 1, .impl_max = 1,
    .read = sysctl_read, .write = sysctl_write, .reset = sysctl_reset, .destroy = free_dev,
    .save = sysctl_save, .restore = sysctl_restore,
};

uam_device *uam_pc_sysctl_create(const char *name)
{
    sysctl *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    uam_device_init(&s->dev, &sysctl_ops, name);
    uam_signal_init(&s->reset);
    uam_signal_init(&s->a20);
    return &s->dev;
}

uam_signal *uam_pc_sysctl_reset_line(uam_device *d) { return &to_sysctl(d)->reset; }
uam_signal *uam_pc_sysctl_a20_line(uam_device *d)   { return &to_sysctl(d)->a20; }

/* ---- the A20 gate ---------------------------------------------------------- */

typedef struct a20gate {
    uam_device dev;
    uam_signal a20m;
    uint8_t    in;          /* bit n: enable input n */
} a20gate;

static a20gate *to_a20(uam_device *d) { return UAM_CONTAINER_OF(d, a20gate, dev); }

void uam_a20_gate_input(void *ctx, unsigned n, int level)
{
    a20gate *g = to_a20(ctx);
    if (n >= 8)
        return;
    g->in = level ? (uint8_t)(g->in | (1u << n)) : (uint8_t)(g->in & ~(1u << n));
    uam_signal_set(&g->a20m, g->in == 0);   /* masked while no input enables A20 */
}

static void a20_save(uam_device *d, uam_state_writer *w)
{
    uam_state_put_u8(w, to_a20(d)->in);
    uam_state_put_signal(w, &to_a20(d)->a20m);
}

static void a20_restore(uam_device *d, uam_state_reader *r)
{
    to_a20(d)->in = uam_state_get_u8(r);
    uam_state_get_signal(r, &to_a20(d)->a20m);
}

static const uam_device_ops a20_ops = {
    .type = "pc,a20-gate", .destroy = free_dev, .save = a20_save, .restore = a20_restore,
};

uam_device *uam_a20_gate_create(const char *name)
{
    a20gate *g = calloc(1, sizeof(*g));
    if (!g)
        return NULL;
    uam_device_init(&g->dev, &a20_ops, name);
    uam_signal_init(&g->a20m);
    g->a20m.level = 1;   /* no input enables A20 yet */
    return &g->dev;
}

uam_signal *uam_a20_gate_a20m_line(uam_device *d) { return &to_a20(d)->a20m; }

/* ---- POST codes ---------------------------------------------------------- */

typedef struct postcode {
    uam_device dev;
    uint8_t    last;
} postcode;

static postcode *to_post(uam_device *d) { return UAM_CONTAINER_OF(d, postcode, dev); }

static uam_status post_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    (void)off; (void)size;
    *val = to_post(d)->last;
    return UAM_OK;
}

static uam_status post_write(uam_device *d, uint64_t off, unsigned size, uint64_t v)
{
    (void)off;
    postcode *p = to_post(d);
    /* A new code is boot progress (info, uam-run -v); the same byte again is
     * usually an I/O delay write to port 0x80 (debug, -vv). */
    bool changed = (uint8_t)v != p->last;
    p->last = (uint8_t)v;
    uam_log(changed ? UAM_LOG_INFO : UAM_LOG_DEBUG, "post", "code 0x%0*llx", (int)size * 2,
            (unsigned long long)v);
    return UAM_OK;
}

static void post_save(uam_device *d, uam_state_writer *w) { uam_state_put_u8(w, to_post(d)->last); }
static void post_restore(uam_device *d, uam_state_reader *r) { to_post(d)->last = uam_state_get_u8(r); }

static const uam_device_ops post_ops = {
    .type = "pc,postcode", .read = post_read, .write = post_write, .destroy = free_dev,
    .save = post_save, .restore = post_restore,
};

uam_device *uam_postcode_create(const char *name)
{
    postcode *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    uam_device_init(&p->dev, &post_ops, name);
    return &p->dev;
}

uint8_t uam_postcode_last(uam_device *d) { return to_post(d)->last; }

/* ---- debug console ------------------------------------------------------- */

typedef struct debugcon {
    uam_device   dev;
    uam_chardev *out;
    uint8_t      read_val;
} debugcon;

static debugcon *to_dc(uam_device *d) { return UAM_CONTAINER_OF(d, debugcon, dev); }

static uam_status dc_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    (void)off; (void)size;
    *val = to_dc(d)->read_val;
    return UAM_OK;
}

static uam_status dc_write(uam_device *d, uint64_t off, unsigned size, uint64_t v)
{
    (void)off; (void)size;
    debugcon *c = to_dc(d);
    uint8_t b = (uint8_t)v;
    if (c->out)
        uam_chardev_write(c->out, &b, 1);
    return UAM_OK;
}

static const uam_device_ops dc_ops = {
    .type = "debug-console", .impl_min = 1, .impl_max = 1,
    .read = dc_read, .write = dc_write, .destroy = free_dev, .flags = UAM_DEVICE_STATELESS,
};

uam_device *uam_debugcon_create(const char *name, uam_chardev *out, uint8_t read_val)
{
    debugcon *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    uam_device_init(&c->dev, &dc_ops, name);
    c->out = out;
    c->read_val = read_val;
    return &c->dev;
}

/* ---- master abort -------------------------------------------------------- */

#define ABORT_PLACES 256u   /* places reported and counted; power of two */
#define ABORT_HOT    10u    /* listed at the end */

typedef struct abort_place {
    uint64_t key;           /* memory: the page address; I/O: the port address */
    uint64_t reads, writes;
    bool     used;
} abort_place;

#define ABORT_WINDOWS 4u

typedef struct abort_window {
    uint64_t    base, size;
    const char *what;
    bool        per_address;
} abort_window;

typedef struct master_abort {
    uam_device   dev;
    abort_window win[ABORT_WINDOWS];
    unsigned     n_win;
    uint64_t     total, untracked;
    size_t      n_used;
    abort_place table[ABORT_PLACES];
} master_abort;

static master_abort *to_abort(uam_device *d) { return UAM_CONTAINER_OF(d, master_abort, dev); }

static const abort_window *abort_window_of(const master_abort *a, uint64_t addr)
{
    for (unsigned i = 0; i < a->n_win; i++)
        if (addr - a->win[i].base < a->win[i].size)
            return &a->win[i];
    return NULL;
}

/* "0x1234", or "<window> +0x34" */
static void abort_where(const master_abort *a, uint64_t addr, char *buf, size_t n)
{
    const abort_window *w = abort_window_of(a, addr);
    if (w)
        snprintf(buf, n, "%s +0x%llx", w->what, (unsigned long long)(addr - w->base));
    else
        snprintf(buf, n, "0x%llx", (unsigned long long)addr);
}

/* Count an access; the first one to its place is logged. */
static void abort_note(master_abort *a, uint64_t addr, unsigned size, bool is_write, uint64_t v)
{
    const abort_window *w = abort_window_of(a, addr);
    uint64_t key = w && w->per_address ? addr : addr & ~(uint64_t)0xFFF;
    a->total++;
    uint32_t h = (uint32_t)((key * UINT64_C(0x9E3779B97F4A7C15)) >> 56) & (ABORT_PLACES - 1);
    for (uint32_t probe = 0; probe < ABORT_PLACES; probe++) {
        abort_place *p = &a->table[(h + probe) & (ABORT_PLACES - 1)];
        if (!p->used) {
            p->used = true;
            p->key = key;
            a->n_used++;
            char where[80];
            abort_where(a, addr, where, sizeof(where));
            if (is_write)
                uam_log(UAM_LOG_WARN, "master-abort", "first write of %u bytes to %s: 0x%llx, nothing "
                        "claims it (dropped)", size, where, (unsigned long long)v);
            else
                uam_log(UAM_LOG_WARN, "master-abort", "first read of %u bytes at %s: nothing claims it, "
                        "reads all ones", size, where);
        }
        if (p->key == key) {
            if (is_write) p->writes++; else p->reads++;
            return;
        }
    }
    if (a->untracked++ == 0)
        uam_log(UAM_LOG_WARN, "master-abort", "more than %u places accessed; further ones are only counted",
                ABORT_PLACES);
}

static uam_status abort_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    abort_note(to_abort(d), off, size, false, 0);
    *val = size >= 8 ? ~0ull : (1ull << (8 * size)) - 1;
    return UAM_OK;
}

static uam_status abort_write(uam_device *d, uint64_t off, unsigned size, uint64_t v)
{
    abort_note(to_abort(d), off, size, true, v);
    return UAM_OK;
}

static void abort_destroy(uam_device *d)
{
    master_abort *a = to_abort(d);
    if (a->total) {
        uam_log(UAM_LOG_WARN, "master-abort", "%llu unclaimed accesses at %zu places%s; most frequent:", (unsigned long long)a->total, a->n_used, a->untracked ? " (and more)" : "");
        bool listed[ABORT_PLACES] = { false };
        for (unsigned n = 0; n < ABORT_HOT; n++) {
            int best = -1;
            for (unsigned i = 0; i < ABORT_PLACES; i++) {
                const abort_place *p = &a->table[i];
                if (p->used && !listed[i]
                    && (best < 0 || p->reads + p->writes > a->table[best].reads + a->table[best].writes))
                    best = (int)i;
            }
            if (best < 0)
                break;
            listed[best] = true;
            const abort_place *p = &a->table[best];
            const abort_window *w = abort_window_of(a, p->key);
            char where[80];
            abort_where(a, p->key, where, sizeof(where));
            uam_log(UAM_LOG_WARN, "master-abort", "  %s%s: %llu reads, %llu writes", where,
                    w && w->per_address ? "" : " (4 KiB page)", (unsigned long long)p->reads,
                    (unsigned long long)p->writes);
        }
    }
    free(a);
}

static const uam_device_ops abort_ops = {
    .type = "pc,master-abort", .read = abort_read, .write = abort_write, .destroy = abort_destroy,
    .flags = UAM_DEVICE_STATELESS,
};

uam_device *uam_master_abort_create(const char *name)
{
    master_abort *a = calloc(1, sizeof(*a));
    if (!a)
        return NULL;
    uam_device_init(&a->dev, &abort_ops, name);
    return &a->dev;
}

bool uam_master_abort_name_window(uam_device *d, uint64_t base, uint64_t size, const char *what,
                                  bool per_address)
{
    master_abort *a = to_abort(d);
    if (a->n_win == ABORT_WINDOWS)
        return false;
    a->win[a->n_win++] = (abort_window){ .base = base, .size = size, .what = what,
                                         .per_address = per_address };
    return true;
}

/* ---- software SMI port ------------------------------------------------------ */

typedef struct smi_port {
    uam_device dev;
    uam_signal line;
    uint8_t    val;
} smi_port;

static smi_port *to_smi(uam_device *d) { return UAM_CONTAINER_OF(d, smi_port, dev); }

static uam_status smi_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    (void)off; (void)size;
    *val = to_smi(d)->val;
    return UAM_OK;
}

static uam_status smi_write(uam_device *d, uint64_t off, unsigned size, uint64_t v)
{
    (void)off; (void)size;
    smi_port *p = to_smi(d);
    p->val = (uint8_t)v;
    uam_signal_set(&p->line, 1);
    uam_signal_set(&p->line, 0);
    return UAM_OK;
}

static void smi_reset(uam_device *d) { to_smi(d)->val = 0; }

static void smi_save(uam_device *d, uam_state_writer *w) { uam_state_put_u8(w, to_smi(d)->val); }
static void smi_restore(uam_device *d, uam_state_reader *r) { to_smi(d)->val = uam_state_get_u8(r); }

static const uam_device_ops smi_ops = {
    .type = "pc,smi-port", .impl_min = 1, .impl_max = 1, .read = smi_read, .write = smi_write,
    .reset = smi_reset, .destroy = free_dev, .save = smi_save, .restore = smi_restore,
};

uam_device *uam_smi_port_create(const char *name)
{
    smi_port *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    uam_device_init(&p->dev, &smi_ops, name);
    uam_signal_init(&p->line);
    return &p->dev;
}

uam_signal *uam_smi_port_line(uam_device *d) { return &to_smi(d)->line; }
