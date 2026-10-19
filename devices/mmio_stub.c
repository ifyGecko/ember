/*
 * mmio_stub.c - Per-register stand-ins for unmodelled registers (uam/dev/mmio_stub.h).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uam/dev/mmio_stub.h"

/* ---- rules -------------------------------------------------------------- */

static unsigned rule_width(const uam_mmio_rule *r) { return r->width ? r->width : 4; }

static void set_err(char *err, size_t errlen, const char *msg)
{
    if (err && errlen)
        snprintf(err, errlen, "%s", msg);
}

uam_status uam_mmio_rule_check(const uam_mmio_rule *r, char *err, size_t errlen)
{
    unsigned w = rule_width(r);
    if (w != 1 && w != 2 && w != 4 && w != 8) {
        set_err(err, errlen, "register width must be 1, 2, 4 or 8");
        return UAM_ERR_INVALID;
    }
    if (r->size == 0 || r->size % w) {
        set_err(err, errlen, "size must be a non-zero multiple of the register width");
        return UAM_ERR_INVALID;
    }
    if (r->addr % w) {
        set_err(err, errlen, "address must be aligned to the register width");
        return UAM_ERR_INVALID;
    }
    if (r->addr + r->size - 1 < r->addr) {
        set_err(err, errlen, "range wraps around the address space");
        return UAM_ERR_INVALID;
    }
    if (r->policy > UAM_MMIO_INPUT) {
        set_err(err, errlen, "unknown policy");
        return UAM_ERR_INVALID;
    }
    return UAM_OK;
}

/* Parse a C number filling the whole of [s, end). */
static bool parse_num(const char *s, const char *end, uint64_t *out)
{
    if (s == end)
        return false;
    char buf[32];
    size_t n = (size_t)(end - s);
    if (n >= sizeof(buf))
        return false;
    memcpy(buf, s, n);
    buf[n] = 0;
    char *e;
    unsigned long long v = strtoull(buf, &e, 0);
    if (*e || buf[0] == '-')
        return false;
    *out = v;
    return true;
}

static bool prefix(const char *s, const char *end, const char *word)
{
    size_t n = strlen(word);
    return (size_t)(end - s) >= n && !memcmp(s, word, n);
}

uam_status uam_mmio_rule_parse(const char *spec, uam_mmio_rule *out, char *err, size_t errlen)
{
    uam_mmio_rule r = { 0 };
    const char *eq = strchr(spec, '=');
    if (!eq) {
        set_err(err, errlen, "expected ADDR[+SIZE]=POLICY");
        return UAM_ERR_INVALID;
    }
    const char *plus = memchr(spec, '+', (size_t)(eq - spec));
    if (!parse_num(spec, plus ? plus : eq, &r.addr)) {
        set_err(err, errlen, "bad address");
        return UAM_ERR_INVALID;
    }
    bool have_size = plus != NULL;
    if (have_size && !parse_num(plus + 1, eq, &r.size)) {
        set_err(err, errlen, "bad size");
        return UAM_ERR_INVALID;
    }

    const char *p = eq + 1;
    const char *end = p + strcspn(p, ",");
    if (end - p == 4 && prefix(p, end, "zero"))
        r.policy = UAM_MMIO_ZERO;
    else if (end - p == 4 && prefix(p, end, "echo"))
        r.policy = UAM_MMIO_ECHO;
    else if (end - p == 5 && prefix(p, end, "fault"))
        r.policy = UAM_MMIO_FAULT;
    else if (end - p == 5 && prefix(p, end, "input"))
        r.policy = UAM_MMIO_INPUT;
    else if (prefix(p, end, "fixed:")) {
        r.policy = UAM_MMIO_FIXED;
        if (!parse_num(p + 6, end, &r.value)) {
            set_err(err, errlen, "bad fixed value (fixed:VALUE)");
            return UAM_ERR_INVALID;
        }
    } else {
        set_err(err, errlen, "policy must be zero, echo, fixed:VALUE, fault or input");
        return UAM_ERR_INVALID;
    }

    while (*end == ',') {
        p = end + 1;
        end = p + strcspn(p, ",");
        uint64_t v;
        const char *val = memchr(p, '=', (size_t)(end - p));
        if (!val || !parse_num(val + 1, end, &v)) {
            set_err(err, errlen, "bad modifier (width=N, set=MASK, clear=MASK)");
            return UAM_ERR_INVALID;
        }
        size_t klen = (size_t)(val - p);
        if (klen == 5 && !memcmp(p, "width", 5))
            r.width = (unsigned)v;
        else if (klen == 3 && !memcmp(p, "set", 3))
            r.set = v;
        else if (klen == 5 && !memcmp(p, "clear", 5))
            r.clear = v;
        else {
            set_err(err, errlen, "unknown modifier (width=N, set=MASK, clear=MASK)");
            return UAM_ERR_INVALID;
        }
    }
    if (r.width > 8) {
        set_err(err, errlen, "register width must be 1, 2, 4 or 8");
        return UAM_ERR_INVALID;
    }
    if (!have_size)
        r.size = rule_width(&r);
    uam_status st = uam_mmio_rule_check(&r, err, errlen);
    if (st == UAM_OK)
        *out = r;
    return st;
}

void uam_mmio_rule_describe(const uam_mmio_rule *r, char *buf, size_t len)
{
    static const char *const names[] = { "zero", "echo", "fixed", "fault", "input" };
    int n = r->policy == UAM_MMIO_FIXED
          ? snprintf(buf, len, "fixed 0x%llx", (unsigned long long)r->value)
          : snprintf(buf, len, "%s", r->policy <= UAM_MMIO_INPUT ? names[r->policy] : "?");
    if (n < 0 || (size_t)n >= len)
        return;
    if (r->set)
        n += snprintf(buf + n, len - (size_t)n, " set=0x%llx", (unsigned long long)r->set);
    if (n >= 0 && (size_t)n < len && r->clear)
        snprintf(buf + n, len - (size_t)n, " clear=0x%llx", (unsigned long long)r->clear);
}

/* ---- input stream ------------------------------------------------------- */

struct uam_mmio_input {
    const uint8_t *data;
    size_t         len, pos;
    bool           told;          /* on_exhausted already called for this input */
    void         (*on_exhausted)(void *ctx);
    void          *ctx;
};

uam_mmio_input *uam_mmio_input_create(void)
{
    return calloc(1, sizeof(uam_mmio_input));
}

void uam_mmio_input_free(uam_mmio_input *in)
{
    free(in);
}

void uam_mmio_input_set(uam_mmio_input *in, const void *data, size_t len)
{
    in->data = data;
    in->len = data ? len : 0;
    in->pos = 0;
    in->told = false;
}

size_t uam_mmio_input_pos(const uam_mmio_input *in) { return in->pos; }
bool   uam_mmio_input_exhausted(const uam_mmio_input *in) { return in->pos >= in->len; }

void uam_mmio_input_on_exhausted(uam_mmio_input *in, void (*fn)(void *ctx), void *ctx)
{
    in->on_exhausted = fn;
    in->ctx = ctx;
}

/* The next n bytes, first byte least significant; bytes past the end read 0. */
static uint64_t input_take(uam_mmio_input *in, unsigned n)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < n; i++) {
        if (in->pos < in->len) {
            v |= (uint64_t)in->data[in->pos++] << (8 * i);
        } else if (!in->told) {
            in->told = true;
            if (in->on_exhausted)
                in->on_exhausted(in->ctx);
        }
    }
    return v;
}

void uam_mmio_input_save(void *ctx, uam_state_writer *w)
{
    const uam_mmio_input *in = ctx;
    uam_state_put_u64(w, in->pos);
    uam_state_put_bool(w, in->told);
}

void uam_mmio_input_restore(void *ctx, uam_state_reader *r)
{
    uam_mmio_input *in = ctx;
    uint64_t pos = uam_state_get_u64(r);
    bool told = uam_state_get_bool(r);
    if (!uam_state_ok(r))
        return;
    /* The data is the caller's: a position past its end just means exhausted. */
    in->pos = pos < in->len ? (size_t)pos : in->len;
    in->told = told;
}

/* ---- the stub ----------------------------------------------------------- */

#define REGS_MAX (1u << 20)   /* registers tracked per stub (echo values, access log) */

typedef struct reg {
    uint64_t off;      /* register offset in the stub */
    uint64_t value;    /* ECHO: last value written */
    bool     used;
} reg;

typedef struct mmio_stub {
    uam_device      dev;
    char            name[48];      /* dev.name points here */
    uam_mmio_rule   rule;
    unsigned        width;
    uam_mmio_input *in;
    reg            *regs;          /* open addressing, power-of-two size */
    uint32_t        cap, n_regs;
    bool            full_warned;
    uint64_t        reads, writes;
} mmio_stub;

static mmio_stub *to_stub(uam_device *d) { return UAM_CONTAINER_OF(d, mmio_stub, dev); }

static uint32_t reg_hash(uint64_t off, uint32_t cap)
{
    return (uint32_t)((off * UINT64_C(0x9E3779B97F4A7C15)) >> 40) & (cap - 1);
}

static reg *reg_slot(reg *regs, uint32_t cap, uint64_t off)
{
    for (uint32_t i = reg_hash(off, cap);; i = (i + 1) & (cap - 1))
        if (!regs[i].used || regs[i].off == off)
            return &regs[i];
}

static bool regs_grow(mmio_stub *s)
{
    uint32_t ncap = s->cap ? s->cap * 2 : 64;
    reg *nr = calloc(ncap, sizeof(*nr));
    if (!nr)
        return false;
    for (uint32_t i = 0; i < s->cap; i++)
        if (s->regs[i].used)
            *reg_slot(nr, ncap, s->regs[i].off) = s->regs[i];
    free(s->regs);
    s->regs = nr;
    s->cap = ncap;
    return true;
}

/* The register at off, created (and its first access logged) if new; NULL when the
 * table is full (the stub then behaves as if the register was never written). */
static reg *reg_get(mmio_stub *s, uint64_t off, bool is_write, unsigned size)
{
    if (s->cap) {
        reg *r = reg_slot(s->regs, s->cap, off);
        if (r->used)
            return r;
    }
    if (s->n_regs >= REGS_MAX) {
        if (!s->full_warned) {
            s->full_warned = true;
            uam_log(UAM_LOG_WARN, "mmio-stub", "%s: more than %u registers used; no longer "
                    "tracking new ones", s->dev.name, REGS_MAX);
        }
        return NULL;
    }
    if ((s->n_regs + 1) * 4 > s->cap * 3 && !regs_grow(s))
        return NULL;
    reg *r = reg_slot(s->regs, s->cap, off);
    r->used = true;
    r->off = off;
    r->value = 0;
    s->n_regs++;
    char how[96];
    uam_mmio_rule_describe(&s->rule, how, sizeof(how));
    uam_log(UAM_LOG_INFO, "mmio-stub", "%s (%s): first %s of %u bytes at 0x%llx", s->dev.name,
            how, is_write ? "write" : "read", size, (unsigned long long)(s->rule.addr + off));
    return r;
}

static uam_status stub_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    mmio_stub *s = to_stub(d);
    s->reads++;
    reg *r = reg_get(s, off, false, size);
    uint64_t v = 0;
    switch (s->rule.policy) {
    case UAM_MMIO_ZERO:  v = 0; break;
    case UAM_MMIO_ECHO:  v = r ? r->value : 0; break;
    case UAM_MMIO_FIXED: v = s->rule.value; break;
    case UAM_MMIO_FAULT: return UAM_ERR_FAULT;
    case UAM_MMIO_INPUT: v = s->in ? input_take(s->in, size) : 0; break;
    }
    *val = ((v & ~s->rule.clear) | s->rule.set) & uam_mask(size * 8);
    return UAM_OK;
}

static uam_status stub_write_masked(uam_device *d, uint64_t off, unsigned size, uint64_t v,
                                    uint64_t mask)
{
    mmio_stub *s = to_stub(d);
    s->writes++;
    if (s->rule.policy == UAM_MMIO_FAULT)
        return UAM_ERR_FAULT;
    reg *r = reg_get(s, off, true, size);
    if (r && s->rule.policy == UAM_MMIO_ECHO)
        r->value = ((r->value & ~mask) | (v & mask)) & uam_mask(size * 8);
    return UAM_OK;
}

static uam_status stub_write(uam_device *d, uint64_t off, unsigned size, uint64_t v)
{
    return stub_write_masked(d, off, size, v, uam_mask(size * 8));
}

static void stub_destroy(uam_device *d)
{
    mmio_stub *s = to_stub(d);
    if (s->reads || s->writes)
        uam_log(UAM_LOG_INFO, "mmio-stub", "%s: %llu reads, %llu writes, %u registers",
                s->dev.name, (unsigned long long)s->reads, (unsigned long long)s->writes,
                (unsigned)s->n_regs);
    free(s->regs);
    free(s);
}

static void stub_save(uam_device *d, uam_state_writer *w)
{
    mmio_stub *s = to_stub(d);
    uam_state_put_u64(w, s->reads);
    uam_state_put_u64(w, s->writes);
    uam_state_put_u32(w, s->n_regs);
    for (uint32_t i = 0; i < s->cap; i++) {
        if (!s->regs[i].used)
            continue;
        uam_state_put_u64(w, s->regs[i].off);
        uam_state_put_u64(w, s->regs[i].value);
    }
}

static void stub_restore(uam_device *d, uam_state_reader *r)
{
    mmio_stub *s = to_stub(d);
    uint64_t reads = uam_state_get_u64(r), writes = uam_state_get_u64(r);
    uint32_t n = uam_state_get_u32(r);
    if (uam_state_ok(r) && n > REGS_MAX)
        uam_state_fail(r, "%u registers saved, at most %u are tracked", (unsigned)n, REGS_MAX);
    if (!uam_state_ok(r))
        return;
    free(s->regs);
    s->regs = NULL;
    s->cap = s->n_regs = 0;
    s->full_warned = false;
    s->reads = reads;
    s->writes = writes;
    while (s->cap * 3 < n * 4)
        if (!regs_grow(s)) {
            uam_state_fail(r, "out of memory");
            return;
        }
    for (uint32_t k = 0; k < n && uam_state_ok(r); k++) {
        uint64_t off = uam_state_get_u64(r), value = uam_state_get_u64(r);
        if (!uam_state_ok(r))
            return;
        if (off >= s->rule.size) {
            uam_state_fail(r, "register offset 0x%llx is outside the stub",
                           (unsigned long long)off);
            return;
        }
        reg *e = reg_slot(s->regs, s->cap, off);
        if (!e->used)
            s->n_regs++;
        *e = (reg){ .off = off, .value = value, .used = true };
    }
}

static void stub_reset(uam_device *d)
{
    /* Power-on: registers forget what was written (the access log is kept). */
    mmio_stub *s = to_stub(d);
    for (uint32_t i = 0; i < s->cap; i++)
        s->regs[i].value = 0;
}

#define STUB_OPS(w) {                                                           \
        .type = "uam,mmio-stub", .impl_min = (w), .impl_max = (w),              \
        .read = stub_read, .write = stub_write, .write_masked = stub_write_masked, \
        .reset = stub_reset, .destroy = stub_destroy,                           \
        .save = stub_save, .restore = stub_restore,                             \
    }

static const uam_device_ops stub_ops[4] = { STUB_OPS(1), STUB_OPS(2), STUB_OPS(4), STUB_OPS(8) };

uam_device *uam_mmio_stub_create(const char *name, const uam_mmio_rule *rule, uam_mmio_input *in)
{
    char err[128];
    if (uam_mmio_rule_check(rule, err, sizeof(err)) != UAM_OK) {
        uam_log(UAM_LOG_ERROR, "mmio-stub", "%s: %s", name, err);
        return NULL;
    }
    if (rule->policy == UAM_MMIO_INPUT && !in) {
        uam_log(UAM_LOG_ERROR, "mmio-stub", "%s: an input rule needs an input stream", name);
        return NULL;
    }
    mmio_stub *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->rule = *rule;
    s->width = rule_width(rule);
    s->rule.width = s->width;
    s->in = in;
    unsigned idx = s->width == 1 ? 0 : s->width == 2 ? 1 : s->width == 4 ? 2 : 3;
    snprintf(s->name, sizeof(s->name), "%s", name);
    uam_device_init(&s->dev, &stub_ops[idx], s->name);
    return &s->dev;
}

const uam_mmio_rule *uam_mmio_stub_rule(uam_device *d) { return &to_stub(d)->rule; }
uint64_t uam_mmio_stub_reads(uam_device *d)  { return to_stub(d)->reads; }
uint64_t uam_mmio_stub_writes(uam_device *d) { return to_stub(d)->writes; }
