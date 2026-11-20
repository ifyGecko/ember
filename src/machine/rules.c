/*
 * rules.c - Rules: a condition and actions at a guest pc (uam/rules.h).
 *
 * Each rule is one PC hook on every processor. Its expressions are parsed once into
 * postfix node lists and evaluated on a small stack at each arrival, against the
 * arriving processor's registers (the ISA's debugger view) and memory (debug accesses:
 * no device side effects, no watches).
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "uam/rules.h"

#define EXPR_MAX  64    /* nodes in one expression: the evaluation stack's depth */
#define DEPTH_MAX 32    /* nesting of ( ) and [ ] */

typedef enum node_op {
    N_CONST, N_REG, N_PC, N_LOAD,
    N_NEG, N_NOT, N_LNOT,
    N_MUL, N_ADD, N_SUB, N_SHL, N_SHR,
    N_LT, N_LE, N_GT, N_GE, N_EQ, N_NE,
    N_AND, N_XOR, N_OR, N_LAND, N_LOR,
} node_op;

typedef struct node {
    uint8_t  op;      /* node_op */
    uint8_t  size;    /* N_LOAD: bytes */
    bool     phys;    /* N_LOAD: the address is physical */
    uint32_t regno;   /* N_REG */
    uint64_t val;     /* N_CONST */
} node;

typedef struct expr {
    node    *n;       /* postfix */
    uint32_t len;
} expr;

typedef enum { LV_REG, LV_PC, LV_MEM } lv_kind;

typedef struct lvalue {
    lv_kind  kind;
    uint32_t regno;
    expr     addr;    /* LV_MEM */
    bool     phys;
    uint8_t  size;
} lvalue;

typedef enum { A_SET, A_LOG, A_STOP } act_kind;

typedef struct action {
    act_kind kind;
    lvalue   dst;        /* A_SET */
    expr     src;
    expr    *log;        /* A_LOG: what to show ... */
    char   **log_text;   /* ... and as what */
    uint32_t n_log;
} action;

typedef struct rule {
    uam_rules *set;
    char      *name;
    uint64_t   pc;
    int        cpu;       /* -1: any */
    uint64_t   limit;     /* count=N; 0: none */
    expr      *cond;
    uint32_t   n_cond;
    action    *act;
    uint32_t   n_act;
    bool       hooked;
    bool       told;      /* its first failure has been logged */
    uint64_t   arrivals, fired, failed;
} rule;

struct uam_rules {
    uam_machine       *m;
    char              *state_name;   /* "rules-NAME" */
    bool               has_state;
    rule             **r;
    uint32_t           n, cap;
    uam_rule_symbol_fn sym;
    void              *sym_ctx;
    FILE              *log;
    bool               stopped;
    uam_rule_stop      stop;
};

/* ---- parsing ---------------------------------------------------------------- */

typedef struct parser {
    const char *p;
    uam_rules  *rs;
    bool        state;    /* registers and memory may appear */
    unsigned    depth;
    expr       *e;
    char       *err;
    size_t      errlen;
    bool        failed;
} parser;

static void fail(parser *ps, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void fail(parser *ps, const char *fmt, ...)
{
    if (ps->failed)
        return;
    ps->failed = true;
    if (!ps->err || !ps->errlen)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ps->err, ps->errlen, fmt, ap);
    va_end(ap);
}

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

static void ws(parser *ps)
{
    while (*ps->p == ' ' || *ps->p == '\t')
        ps->p++;
}

static void emit(parser *ps, node nd)
{
    if (ps->failed)
        return;
    if (ps->e->len == EXPR_MAX) {
        fail(ps, "the expression is too long (at most %d values and operators)", EXPR_MAX);
        return;
    }
    ps->e->n[ps->e->len++] = nd;
}

static bool name_start(char c) { return isalpha((unsigned char)c) || c == '_' || c == '.' || c == '$' || c == '@'; }
static bool name_char(char c)  { return name_start(c) || isdigit((unsigned char)c); }

static void p_lor(parser *ps);

static void p_primary(parser *ps)
{
    ws(ps);
    const uam_isa *isa = ps->rs->m->cpu.isa;
    if (isdigit((unsigned char)*ps->p)) {
        char *end;
        uint64_t v = strtoull(ps->p, &end, 0);
        if (name_char(*end)) {
            fail(ps, "bad number at '%s'", ps->p);
            return;
        }
        ps->p = end;
        emit(ps, (node){ .op = N_CONST, .val = v });
    } else if (*ps->p == '(' || *ps->p == '[') {
        const bool mem = *ps->p == '[';
        if (mem && !ps->state) {
            fail(ps, "memory cannot be read here: the value must be a constant");
            return;
        }
        if (++ps->depth > DEPTH_MAX) {
            fail(ps, "the expression nests too deep");
            return;
        }
        ps->p++;
        bool phys = false;
        if (mem) {
            ws(ps);
            if (ps->p[0] == 'p' && ps->p[1] == ':') {
                phys = true;
                ps->p += 2;
            }
        }
        p_lor(ps);
        ws(ps);
        if (ps->failed)
            return;
        if (*ps->p != (mem ? ']' : ')')) {
            fail(ps, "expected '%c' at '%s'", mem ? ']' : ')', ps->p);
            return;
        }
        ps->p++;
        ps->depth--;
        if (mem) {
            unsigned size = isa->addr_width / 8;
            if (*ps->p == ':') {
                const char c = ps->p[1];
                if ((c != '1' && c != '2' && c != '4' && c != '8') || name_char(ps->p[2])) {
                    fail(ps, "the width after ']:' is 1, 2, 4 or 8 bytes");
                    return;
                }
                size = (unsigned)(c - '0');
                ps->p += 2;
            }
            emit(ps, (node){ .op = N_LOAD, .size = (uint8_t)size, .phys = phys });
        }
    } else if (name_start(*ps->p)) {
        char name[128];
        size_t n = 0;
        while (name_char(ps->p[n]))
            n++;
        if (n >= sizeof(name)) {
            fail(ps, "the name at '%.20s...' is too long", ps->p);
            return;
        }
        memcpy(name, ps->p, n);
        name[n] = '\0';
        unsigned regno;
        uint64_t v;
        const bool is_reg = isa->gdb && isa->gdb->read_reg && uam_isa_reg_by_name(isa, name, &regno);
        if (is_reg || !strcmp(name, "pc")) {
            if (!ps->state) {
                fail(ps, "register '%s' cannot be read here: the value must be a constant", name);
                return;
            }
            emit(ps, is_reg ? (node){ .op = N_REG, .regno = regno } : (node){ .op = N_PC });
        } else if (ps->rs->sym && ps->rs->sym(ps->rs->sym_ctx, name, &v)) {
            emit(ps, (node){ .op = N_CONST, .val = v });
        } else {
            fail(ps, "'%s' is neither a register of %s nor a symbol", name, isa->name);
            return;
        }
        ps->p += n;
    } else if (*ps->p) {
        fail(ps, "expected a value at '%s'", ps->p);
    } else {
        fail(ps, "the expression ends where a value is expected");
    }
}

static void p_unary(parser *ps)
{
    ws(ps);
    const char c = *ps->p;
    if (c == '-' || c == '~' || (c == '!' && ps->p[1] != '=')) {
        if (++ps->depth > DEPTH_MAX) {
            fail(ps, "the expression nests too deep");
            return;
        }
        ps->p++;
        p_unary(ps);
        ps->depth--;
        emit(ps, (node){ .op = c == '-' ? N_NEG : c == '~' ? N_NOT : N_LNOT });
    } else {
        p_primary(ps);
    }
}

/* One binary precedence level: `next` operands joined by the operators in ops (longest
 * first). `stop`, if not NULL, is an operator of an outer level that starts like one of
 * these ("&&" for "&"): it ends the level. */
typedef struct binop { const char *text; node_op op; } binop;

static void p_level(parser *ps, void (*next)(parser *), const binop *ops, size_t n_ops,
                    const char *stop)
{
    next(ps);
    while (!ps->failed) {
        ws(ps);
        if (stop && !strncmp(ps->p, stop, strlen(stop)))
            return;
        const binop *hit = NULL;
        for (size_t i = 0; i < n_ops && !hit; i++)
            if (!strncmp(ps->p, ops[i].text, strlen(ops[i].text)))
                hit = &ops[i];
        if (!hit)
            return;
        ps->p += strlen(hit->text);
        next(ps);
        emit(ps, (node){ .op = (uint8_t)hit->op });
    }
}

#define LEVEL(name, next, stop, ...)                                 \
    static void name(parser *ps)                                     \
    {                                                                \
        static const binop ops[] = { __VA_ARGS__ };                  \
        p_level(ps, next, ops, UAM_ARRAY_LEN(ops), stop);            \
    }

LEVEL(p_mul,   p_unary, NULL, { "*", N_MUL })
LEVEL(p_add,   p_mul,   NULL, { "+", N_ADD }, { "-", N_SUB })
LEVEL(p_shift, p_add,   NULL, { "<<", N_SHL }, { ">>", N_SHR })
LEVEL(p_rel,   p_shift, NULL, { "<=", N_LE }, { ">=", N_GE }, { "<", N_LT }, { ">", N_GT })
LEVEL(p_eq,    p_rel,   NULL, { "==", N_EQ }, { "!=", N_NE })
LEVEL(p_band,  p_eq,    "&&", { "&", N_AND })
LEVEL(p_bxor,  p_band,  NULL, { "^", N_XOR })
LEVEL(p_bor,   p_bxor,  "||", { "|", N_OR })
LEVEL(p_land,  p_bor,   NULL, { "&&", N_LAND })
LEVEL(p_lor0,  p_land,  NULL, { "||", N_LOR })

static void p_lor(parser *ps) { p_lor0(ps); }

static void expr_free(expr *e)
{
    free(e->n);
    *e = (expr){ 0 };
}

static void parser_init(parser *ps, uam_rules *rs, const char *text, bool state, expr *e,
                        char *err, size_t errlen)
{
    *ps = (parser){ .p = text, .rs = rs, .state = state, .e = e, .err = err, .errlen = errlen };
    *e = (expr){ .n = malloc(sizeof(node) * EXPR_MAX) };
    if (!e->n)
        fail(ps, "out of memory");
}

/* The whole of `text` as one expression. */
static bool parse_expr(uam_rules *rs, const char *text, bool state, expr *e, char *err, size_t errlen)
{
    parser ps;
    parser_init(&ps, rs, text, state, e, err, errlen);
    p_lor(&ps);
    ws(&ps);
    if (!ps.failed && *ps.p)
        fail(&ps, "unexpected '%s'", ps.p);
    if (ps.failed)
        expr_free(e);
    return !ps.failed;
}

/* "DST=EXPR": DST a register or [ADDR]. */
static bool parse_set(uam_rules *rs, const char *text, action *a, char *err, size_t errlen)
{
    parser ps;
    expr dst;
    parser_init(&ps, rs, text, true, &dst, err, errlen);
    ws(&ps);
    const bool mem = *ps.p == '[';
    if (!ps.failed && !mem && !name_start(*ps.p))
        fail(&ps, "set wants a register or [ADDR] before '='");
    if (!ps.failed)
        p_primary(&ps);
    ws(&ps);
    if (!ps.failed && (*ps.p != '=' || ps.p[1] == '='))
        fail(&ps, "set wants DST=EXPR, DST a register or [ADDR]");
    if (!ps.failed) {
        const node last = dst.n[dst.len - 1];
        if (mem) {
            /* The load of the address is the store's. */
            a->dst = (lvalue){ .kind = LV_MEM, .phys = last.phys, .size = last.size };
            dst.len--;
        } else if (dst.len == 1 && last.op == N_REG) {
            a->dst = (lvalue){ .kind = LV_REG, .regno = last.regno };
        } else if (dst.len == 1 && last.op == N_PC) {
            a->dst = (lvalue){ .kind = LV_PC };
        } else {
            fail(&ps, "set cannot store into a symbol; write [SYMBOL] for the memory there");
        }
    }
    if (ps.failed) {
        expr_free(&dst);
        return false;
    }
    if (mem)
        a->dst.addr = dst;
    else
        expr_free(&dst);
    if (!parse_expr(rs, ps.p + 1, true, &a->src, err, errlen)) {
        expr_free(&a->dst.addr);
        return false;
    }
    return true;
}

/* ---- evaluation -------------------------------------------------------------- */

static bool to_phys(uam_cpu *cpu, uint64_t addr, bool phys, uint64_t *pa)
{
    if (phys) {
        *pa = addr;
        return true;
    }
    addr &= uam_mask(cpu->isa->addr_width);
    if (!cpu->isa->translate) {
        *pa = addr;
        return true;
    }
    uam_xlate xl = { 0 };
    if (cpu->isa->translate(cpu, addr, UAM_XLATE_DEBUG, &xl) != UAM_OK)
        return false;
    *pa = xl.paddr;
    return true;
}

static const char *space_of(bool phys) { return phys ? "physical" : "virtual"; }

static bool mem_read(uam_cpu *cpu, uint64_t addr, bool phys, unsigned size, uint64_t *val,
                     char *why, size_t n)
{
    const bool be = cpu->isa->default_endian == UAM_ACC_BE;
    uint64_t v = 0;
    for (unsigned i = 0; i < size; i++) {
        uint64_t pa, b;
        if (!to_phys(cpu, addr + i, phys, &pa)) {
            snprintf(why, n, "virtual 0x%llx is not mapped", (unsigned long long)(addr + i));
            return false;
        }
        if (uam_bus_read(cpu->bus, pa, 1, UAM_ACC_LE | UAM_ACC_DEBUG, &b) != UAM_OK) {
            snprintf(why, n, "%s 0x%llx cannot be read", space_of(phys),
                     (unsigned long long)(addr + i));
            return false;
        }
        v |= (b & 0xFF) << 8 * (be ? size - 1 - i : i);
    }
    *val = v;
    return true;
}

static bool mem_write(uam_cpu *cpu, uint64_t addr, bool phys, unsigned size, uint64_t val,
                      char *why, size_t n)
{
    const bool be = cpu->isa->default_endian == UAM_ACC_BE;
    uint64_t pa[8];
    for (unsigned i = 0; i < size; i++) {
        if (!to_phys(cpu, addr + i, phys, &pa[i])) {
            snprintf(why, n, "virtual 0x%llx is not mapped", (unsigned long long)(addr + i));
            return false;
        }
    }
    for (unsigned i = 0; i < size; i++) {
        const uint64_t b = val >> 8 * (be ? size - 1 - i : i) & 0xFF;
        if (uam_bus_write(cpu->bus, pa[i], 1, UAM_ACC_LE | UAM_ACC_DEBUG, b) != UAM_OK) {
            snprintf(why, n, "%s 0x%llx cannot be written (a rule stores to RAM only)",
                     space_of(phys), (unsigned long long)(addr + i));
            return false;
        }
    }
    return true;
}

/* cpu may be NULL for an expression parsed without state (a constant). */
static bool eval(const expr *e, uam_cpu *cpu, uint64_t *out, char *why, size_t n)
{
    uint64_t st[EXPR_MAX];
    unsigned sp = 0;
    for (uint32_t i = 0; i < e->len; i++) {
        const node *nd = &e->n[i];
        uint64_t a, b;
        switch ((node_op)nd->op) {
        case N_CONST: st[sp++] = nd->val; continue;
        case N_PC:    st[sp++] = cpu->pc; continue;
        case N_REG:
            if (!cpu->isa->gdb->read_reg(cpu, nd->regno, &a)) {
                snprintf(why, n, "a register cannot be read");
                return false;
            }
            st[sp++] = a;
            continue;
        case N_LOAD:
            if (!mem_read(cpu, st[sp - 1], nd->phys, nd->size, &st[sp - 1], why, n))
                return false;
            continue;
        case N_NEG:  st[sp - 1] = -st[sp - 1]; continue;
        case N_NOT:  st[sp - 1] = ~st[sp - 1]; continue;
        case N_LNOT: st[sp - 1] = !st[sp - 1]; continue;
        default: break;
        }
        b = st[--sp];
        a = st[sp - 1];
        switch ((node_op)nd->op) {
        case N_MUL:  a *= b; break;
        case N_ADD:  a += b; break;
        case N_SUB:  a -= b; break;
        case N_SHL:  a = b < 64 ? a << b : 0; break;
        case N_SHR:  a = b < 64 ? a >> b : 0; break;
        case N_LT:   a = a < b; break;
        case N_LE:   a = a <= b; break;
        case N_GT:   a = a > b; break;
        case N_GE:   a = a >= b; break;
        case N_EQ:   a = a == b; break;
        case N_NE:   a = a != b; break;
        case N_AND:  a &= b; break;
        case N_XOR:  a ^= b; break;
        case N_OR:   a |= b; break;
        case N_LAND: a = a && b; break;
        case N_LOR:  a = a || b; break;
        default: break;
        }
        st[sp - 1] = a;
    }
    *out = st[0];
    return true;
}

/* ---- the hook ---------------------------------------------------------------- */

static void note_failure(rule *r, const uam_cpu *cpu, const char *what, const char *why)
{
    r->failed++;
    if (r->told)
        return;
    r->told = true;
    uam_log(UAM_LOG_INFO, "rules", "rule %s: %s failed: %s (cpu %u pc 0x%llx icount %llu); "
            "its later failures are only counted", r->name, what, why, cpu->hartid,
            (unsigned long long)cpu->pc, (unsigned long long)cpu->icount);
}

static void do_set(rule *r, const action *a, uam_cpu *cpu)
{
    char why[96];
    uint64_t v, addr;
    if (!eval(&a->src, cpu, &v, why, sizeof(why))) {
        note_failure(r, cpu, "set", why);
        return;
    }
    switch (a->dst.kind) {
    case LV_PC:
        cpu->pc = v;
        break;
    case LV_REG:
        if (!cpu->isa->gdb->write_reg || !cpu->isa->gdb->write_reg(cpu, a->dst.regno, v))
            note_failure(r, cpu, "set", "the register cannot be written");
        break;
    case LV_MEM:
        if (!eval(&a->dst.addr, cpu, &addr, why, sizeof(why))
            || !mem_write(cpu, addr, a->dst.phys, a->dst.size, v, why, sizeof(why)))
            note_failure(r, cpu, "set", why);
        break;
    }
}

static void do_log(rule *r, const action *a, uam_cpu *cpu, uint64_t pc)
{
    FILE *out = r->set->log ? r->set->log : stderr;
    fprintf(out, "rule %s: cpu %u pc 0x%llx", r->name, cpu->hartid, (unsigned long long)pc);
    for (uint32_t i = 0; i < a->n_log; i++) {
        char why[96];
        uint64_t v;
        if (eval(&a->log[i], cpu, &v, why, sizeof(why))) {
            fprintf(out, " %s=0x%llx", a->log_text[i], (unsigned long long)v);
        } else {
            fprintf(out, " %s=?", a->log_text[i]);
            note_failure(r, cpu, "log", why);
        }
    }
    fprintf(out, " icount=%llu\n", (unsigned long long)cpu->icount);
}

static uam_hook_action rule_hook(void *ctx, uam_cpu *cpu, uint64_t pc)
{
    rule *r = ctx;
    if (r->cpu >= 0 && cpu->hartid != (unsigned)r->cpu)
        return UAM_HOOK_CONTINUE;
    r->arrivals++;
    if (r->limit && r->fired >= r->limit)
        return UAM_HOOK_CONTINUE;
    for (uint32_t i = 0; i < r->n_cond; i++) {
        char why[96];
        uint64_t v;
        if (!eval(&r->cond[i], cpu, &v, why, sizeof(why))) {
            note_failure(r, cpu, "if", why);
            return UAM_HOOK_CONTINUE;
        }
        if (!v)
            return UAM_HOOK_CONTINUE;
    }
    r->fired++;
    uam_hook_action ret = UAM_HOOK_CONTINUE;
    for (uint32_t i = 0; i < r->n_act; i++) {
        const action *a = &r->act[i];
        switch (a->kind) {
        case A_SET: do_set(r, a, cpu); break;
        case A_LOG: do_log(r, a, cpu, pc); break;
        case A_STOP:
            r->set->stopped = true;
            r->set->stop = (uam_rule_stop){ .name = r->name, .cpu = cpu->hartid, .pc = pc,
                                            .icount = cpu->icount };
            ret = UAM_HOOK_STOP;
            break;
        }
    }
    return ret;
}

/* ---- counts in snapshots ------------------------------------------------------ */

static uint32_t n_counted(const uam_rules *rs)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < rs->n; i++)
        n += rs->r[i]->limit != 0;
    return n;
}

static void rules_save(void *ctx, uam_state_writer *w)
{
    const uam_rules *rs = ctx;
    uam_state_put_u32(w, n_counted(rs));
    for (uint32_t i = 0; i < rs->n; i++) {
        const rule *r = rs->r[i];
        if (!r->limit)
            continue;
        uam_state_put_u64(w, r->pc);
        uam_state_put_u64(w, r->limit);
        uam_state_put_u64(w, r->fired);
    }
}

static void rules_restore(void *ctx, uam_state_reader *rd)
{
    uam_rules *rs = ctx;
    const uint32_t n = uam_state_get_u32(rd);
    if (n != n_counted(rs)) {
        uam_state_fail(rd, "the snapshot was taken with %u rules that have a count, the set "
                       "has %u", (unsigned)n, (unsigned)n_counted(rs));
        return;
    }
    for (uint32_t i = 0; i < rs->n; i++) {
        rule *r = rs->r[i];
        if (!r->limit)
            continue;
        const uint64_t pc = uam_state_get_u64(rd), limit = uam_state_get_u64(rd);
        const uint64_t fired = uam_state_get_u64(rd);
        if (pc != r->pc || limit != r->limit) {
            uam_state_fail(rd, "the snapshot was taken with other counted rules (rule %s)",
                           r->name);
            return;
        }
        r->fired = fired;
    }
}

/* ---- the set ------------------------------------------------------------------- */

uam_rules *uam_rules_create(uam_machine *m, const char *name)
{
    uam_rules *rs = calloc(1, sizeof(*rs));
    if (!rs || !name)
        goto bad;
    rs->m = m;
    const size_t n = strlen(name) + sizeof("rules-");
    if (!(rs->state_name = malloc(n)))
        goto bad;
    snprintf(rs->state_name, n, "rules-%s", name);
    return rs;
bad:
    free(rs);
    return NULL;
}

static void rule_free(rule *r)
{
    if (!r)
        return;
    for (uint32_t i = 0; i < r->n_cond; i++)
        expr_free(&r->cond[i]);
    for (uint32_t i = 0; i < r->n_act; i++) {
        action *a = &r->act[i];
        expr_free(&a->dst.addr);
        expr_free(&a->src);
        for (uint32_t k = 0; k < a->n_log; k++) {
            expr_free(&a->log[k]);
            free(a->log_text[k]);
        }
        free(a->log);
        free(a->log_text);
    }
    free(r->cond);
    free(r->act);
    free(r->name);
    free(r);
}

void uam_rules_detach(uam_rules *rs)
{
    if (!rs)
        return;
    for (uint32_t i = 0; i < rs->n; i++) {
        rule *r = rs->r[i];
        if (r->hooked)
            uam_machine_remove_pc_hook(rs->m, r->pc, rule_hook, r);
        r->hooked = false;
    }
    if (rs->has_state)
        uam_machine_remove_state(rs->m, rs->state_name);
    rs->has_state = false;
}

void uam_rules_free(uam_rules *rs)
{
    if (!rs)
        return;
    for (uint32_t i = 0; i < rs->n; i++)
        rule_free(rs->r[i]);
    free(rs->r);
    free(rs->state_name);
    free(rs);
}

void uam_rules_set_symbols(uam_rules *rs, uam_rule_symbol_fn fn, void *ctx)
{
    rs->sym = fn;
    rs->sym_ctx = fn ? ctx : NULL;
}

void uam_rules_set_log(uam_rules *rs, FILE *out)
{
    rs->log = out;
}

/* The rest of `item` after the keyword `word` and the blanks behind it, or NULL if item
 * does not start with that word. */
static const char *after_word(const char *item, const char *word)
{
    const size_t n = strlen(word);
    if (strncmp(item, word, n) || (item[n] && item[n] != ' ' && item[n] != '\t'))
        return NULL;
    item += n;
    while (*item == ' ' || *item == '\t')
        item++;
    return item;
}

/* The value of "KEY=VALUE" (blanks around the '=' allowed), or NULL if item is not that. */
static char *value_of(char *item, const char *key)
{
    const size_t n = strlen(key);
    if (strncmp(item, key, n))
        return NULL;
    item += n;
    while (*item == ' ' || *item == '\t')
        item++;
    if (item[0] != '=' || item[1] == '=')
        return NULL;
    item++;
    while (*item == ' ' || *item == '\t')
        item++;
    return item;
}

static bool whole_number(const char *s, uint64_t *v)
{
    char *end;
    if (!isdigit((unsigned char)*s))
        return false;
    *v = strtoull(s, &end, 0);
    return !*end;
}

static action *new_action(rule *r, act_kind kind)
{
    action *na = realloc(r->act, sizeof(*na) * (r->n_act + 1));
    if (!na)
        return NULL;
    r->act = na;
    na[r->n_act] = (action){ .kind = kind };
    return &na[r->n_act];
}

/* "EXPR EXPR ...": what a log action shows. */
static bool parse_log(uam_rules *rs, char *list, action *a, char *err, size_t errlen)
{
    for (char *tok = strtok(list, " \t"); tok; tok = strtok(NULL, " \t")) {
        expr *ne = realloc(a->log, sizeof(*ne) * (a->n_log + 1));
        char **nt = ne ? realloc(a->log_text, sizeof(*nt) * (a->n_log + 1)) : NULL;
        if (ne)
            a->log = ne;
        if (nt)
            a->log_text = nt;
        if (!ne || !nt) {
            set_err(err, errlen, "out of memory");
            return false;
        }
        if (!parse_expr(rs, tok, true, &a->log[a->n_log], err, errlen))
            return false;
        if (!(a->log_text[a->n_log] = strdup(tok))) {
            expr_free(&a->log[a->n_log]);
            set_err(err, errlen, "out of memory");
            return false;
        }
        a->n_log++;
    }
    return true;
}

static bool parse_item(uam_rules *rs, rule *r, char *item, char *err, size_t errlen)
{
    const char *rest;
    char *val;
    uint64_t v;
    if ((val = value_of(item, "cpu"))) {
        if (!whole_number(val, &v) || v >= uam_machine_ncpus(rs->m)) {
            set_err(err, errlen, "bad '%s': the machine's processors are 0..%u", item,
                    uam_machine_ncpus(rs->m) - 1);
            return false;
        }
        r->cpu = (int)v;
    } else if ((val = value_of(item, "count"))) {
        if (!whole_number(val, &v) || !v) {
            set_err(err, errlen, "bad '%s': count=N, N at least 1", item);
            return false;
        }
        r->limit = v;
    } else if ((val = value_of(item, "name"))) {
        char *name = *val ? strdup(val) : NULL;
        if (!name) {
            set_err(err, errlen, "bad '%s': name=NAME", item);
            return false;
        }
        free(r->name);
        r->name = name;
    } else if ((rest = after_word(item, "if")) && *rest) {
        expr *nc = realloc(r->cond, sizeof(*nc) * (r->n_cond + 1));
        if (!nc) {
            set_err(err, errlen, "out of memory");
            return false;
        }
        r->cond = nc;
        if (!parse_expr(rs, rest, true, &nc[r->n_cond], err, errlen))
            return false;
        r->n_cond++;
    } else if ((rest = after_word(item, "set")) && *rest) {
        action *a = new_action(r, A_SET);
        if (!a) {
            set_err(err, errlen, "out of memory");
            return false;
        }
        if (!parse_set(rs, rest, a, err, errlen))
            return false;
        r->n_act++;
    } else if ((rest = after_word(item, "log"))) {
        action *a = new_action(r, A_LOG);
        if (!a) {
            set_err(err, errlen, "out of memory");
            return false;
        }
        r->n_act++;   /* owns what parse_log allocates, also when it fails */
        if (!parse_log(rs, item + (rest - item), a, err, errlen))
            return false;
    } else if (!strcmp(item, "stop")) {
        if (!new_action(r, A_STOP)) {
            set_err(err, errlen, "out of memory");
            return false;
        }
        r->n_act++;
    } else {
        set_err(err, errlen, "unknown item '%s' (cpu=N, if COND, count=N, name=NAME, "
                "set DST=EXPR, log [EXPR...], stop)", item);
        return false;
    }
    return true;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n'))
        s[--n] = '\0';
    return s;
}

int uam_rules_add(uam_rules *rs, const char *text, char *err, size_t errlen)
{
    char *copy = strdup(text);
    rule *r = calloc(1, sizeof(*r));
    bool ok = copy && r;
    if (!ok)
        set_err(err, errlen, "out of memory");
    if (ok) {
        r->set = rs;
        r->cpu = -1;
    }
    /* The items, split at the commas (no expression holds one). */
    bool first = true;
    for (char *item = copy, *next; ok && item; item = next, first = false) {
        next = strchr(item, ',');
        if (next)
            *next++ = '\0';
        item = trim(item);
        if (first) {
            expr e;
            const char *addr = value_of(item, "pc");
            if (!addr) {
                set_err(err, errlen, "a rule starts with pc=ADDR");
                ok = false;
            } else if (!(r->name = strdup(item))) {
                set_err(err, errlen, "out of memory");
                ok = false;
            } else if ((ok = parse_expr(rs, addr, false, &e, err, errlen))) {
                eval(&e, NULL, &r->pc, NULL, 0);
                expr_free(&e);
            }
        } else if (!*item) {
            set_err(err, errlen, "an empty item (two commas, or one at the end)");
            ok = false;
        } else {
            ok = parse_item(rs, r, item, err, errlen);
        }
    }
    if (ok && !r->n_act) {
        set_err(err, errlen, "the rule does nothing: add set DST=EXPR, log [EXPR...] or stop");
        ok = false;
    }
    if (ok && rs->n == rs->cap) {
        const uint32_t ncap = rs->cap ? rs->cap * 2 : 8;
        rule **nr = realloc(rs->r, sizeof(*nr) * ncap);
        if (nr) {
            rs->r = nr;
            rs->cap = ncap;
        } else {
            set_err(err, errlen, "out of memory");
            ok = false;
        }
    }
    if (ok && r->limit && !rs->has_state) {
        if (uam_machine_add_state(rs->m, rs->state_name, rules_save, rules_restore, rs) != UAM_OK) {
            set_err(err, errlen, "cannot register the state '%s' (is another rule set called "
                    "the same?)", rs->state_name);
            ok = false;
        }
        rs->has_state = ok;
    }
    if (ok && uam_machine_add_pc_hook(rs->m, r->pc, rule_hook, r) != UAM_OK) {
        set_err(err, errlen, "cannot hook pc 0x%llx", (unsigned long long)r->pc);
        ok = false;
    }
    free(copy);
    if (!ok) {
        rule_free(r);
        return -1;
    }
    r->hooked = true;
    rs->r[rs->n] = r;
    return (int)rs->n++;
}

uam_status uam_rules_add_file(uam_rules *rs, const char *path, char *err, size_t errlen)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        set_err(err, errlen, "cannot read %s", path);
        return UAM_ERR_INVALID;
    }
    char line[4096], why[512];
    uam_status st = UAM_OK;
    for (unsigned no = 1; st == UAM_OK && fgets(line, sizeof(line), f); no++) {
        const size_t n = strlen(line);
        if (n == sizeof(line) - 1 && line[n - 1] != '\n') {
            set_err(err, errlen, "%s:%u: the line is too long", path, no);
            st = UAM_ERR_INVALID;
            break;
        }
        const char *text = trim(line);
        if (!*text || *text == '#')
            continue;
        if (uam_rules_add(rs, text, why, sizeof(why)) < 0) {
            set_err(err, errlen, "%s:%u: %s", path, no, why);
            st = UAM_ERR_INVALID;
        }
    }
    fclose(f);
    return st;
}

size_t uam_rules_n(const uam_rules *rs)
{
    return rs->n;
}

bool uam_rules_get(const uam_rules *rs, size_t i, uam_rule_info *out)
{
    if (i >= rs->n)
        return false;
    const rule *r = rs->r[i];
    *out = (uam_rule_info){ .name = r->name, .pc = r->pc, .arrivals = r->arrivals,
                            .fired = r->fired, .failed = r->failed, .limit = r->limit };
    return true;
}

bool uam_rules_last_stop(const uam_rules *rs, uam_rule_stop *out)
{
    if (!rs->stopped)
        return false;
    *out = rs->stop;
    return true;
}
