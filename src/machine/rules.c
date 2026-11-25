/*
 * rules.c - Rules: a condition and actions at a guest pc or data access (uam/rules.h).
 *
 * A pc rule is one PC hook on every processor. An access rule is one bus watch per
 * physically contiguous piece of its range: the watch notes the access and asks the
 * accessing processor's executor to call back at the next instruction boundary
 * (uam_exec_request_call), where the rule runs with precise state. Expressions are
 * parsed once into postfix node lists and evaluated on a small stack, against that
 * processor's registers (the ISA's debugger view) and memory (debug accesses: no device
 * side effects, no watches). A rule with a delay computes its stores when it fires and
 * carries them out from a scheduler timer.
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "uam/rules.h"

#define EXPR_MAX  64            /* nodes in one expression: the evaluation stack's depth */
#define DEPTH_MAX 32            /* nesting of ( ) and [ ] */
#define DUMP_MAX  65536u        /* bytes one dump action shows */
#define RANGE_MAX (UINT64_C(1) << 28)   /* bytes an access rule covers */

typedef enum node_op {
    N_CONST, N_REG, N_PC, N_LOAD, N_ADDR, N_VALUE,
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

typedef enum { A_SET, A_LOG, A_DUMP, A_STOP } act_kind;

typedef struct action {
    act_kind kind;
    lvalue   dst;        /* A_SET: where; A_DUMP: dst.addr and dst.phys are the address */
    expr     src;        /* A_SET: what; A_DUMP: how many bytes */
    expr    *log;        /* A_LOG: what to show ... */
    char   **log_text;   /* ... and as what */
    uint32_t n_log;
} action;

/* A store worked out when its rule fired: what, and for memory the physical address of
 * each byte. With a delay it waits here for the rule's timer. */
typedef struct held {
    bool     ok;
    uint64_t val;
    uint64_t pa[8];
} held;

typedef struct rule rule;

/* A physically contiguous part of an access rule's range, and its bus watch. */
typedef struct piece {
    rule    *r;
    uint64_t pa, len;
    uint64_t off;        /* where it starts in the range */
    int      wid;
} piece;

/* The access an access rule fires for. */
typedef struct access {
    bool     is_write;
    unsigned size;
    uint64_t addr;       /* in the space the rule's range was given in */
    uint64_t val;        /* read or written */
    uint64_t pc;         /* the accessing instruction */
} access;

struct rule {
    uam_rules *set;
    char      *name;
    uint64_t   id;        /* of its text: tells rules apart in a snapshot */
    bool       on_access; /* an access rule; else a pc rule */
    uint64_t   pc;        /* pc rule */
    unsigned   dir;       /* access rule: UAM_WATCH_READ and/or UAM_WATCH_WRITE */
    uint64_t   addr, len; /* access rule: the range */
    bool       phys;
    piece     *piece;
    uint32_t   n_piece;
    bool       hit;       /* an access waits for its instruction to complete */
    unsigned   hit_cpu;
    access     acc;
    int        cpu;       /* -1: any */
    uint64_t   limit;     /* count=N; 0: none */
    uint64_t   delay;     /* ticks between firing and its stores; 0: none */
    uam_timer  timer;     /* delay: armed while stores wait */
    bool       waiting;
    unsigned   wait_cpu;
    held      *held;      /* delay: one per set action */
    uint32_t   n_set;
    expr      *cond;
    uint32_t   n_cond;
    action    *act;
    uint32_t   n_act;
    bool       hooked;
    bool       off;       /* uam_rules_enable(.., false): neither hooked nor watching */
    bool       told;      /* its first failure has been logged */
    uint64_t   arrivals, fired, failed;
};

struct uam_rules {
    uam_machine       *m;
    char              *state_name;   /* "rules-NAME" */
    bool               has_state;
    bool               arm_later;
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
    bool        access;   /* ... and addr and value: an access rule's */
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
        } else if (ps->access && (!strcmp(name, "addr") || !strcmp(name, "value"))) {
            emit(ps, (node){ .op = name[0] == 'a' ? N_ADDR : N_VALUE });
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

/* What an expression may hold: numbers and symbols; registers and memory too; and an
 * access rule's addr and value. */
typedef enum { M_CONST, M_STATE, M_ACCESS } parse_mode;

static void parser_init(parser *ps, uam_rules *rs, const char *text, parse_mode mode, expr *e,
                        char *err, size_t errlen)
{
    *ps = (parser){ .p = text, .rs = rs, .state = mode != M_CONST, .access = mode == M_ACCESS,
                    .e = e, .err = err, .errlen = errlen };
    *e = (expr){ .n = malloc(sizeof(node) * EXPR_MAX) };
    if (!e->n)
        fail(ps, "out of memory");
}

/* The whole of `text` as one expression. */
static bool parse_expr(uam_rules *rs, const char *text, parse_mode mode, expr *e, char *err,
                       size_t errlen)
{
    parser ps;
    parser_init(&ps, rs, text, mode, e, err, errlen);
    p_lor(&ps);
    ws(&ps);
    if (!ps.failed && *ps.p)
        fail(&ps, "unexpected '%s'", ps.p);
    if (ps.failed)
        expr_free(e);
    return !ps.failed;
}

/* "DST=EXPR": DST a register or [ADDR]. */
static bool parse_set(uam_rules *rs, const char *text, parse_mode mode, action *a, char *err,
                      size_t errlen)
{
    parser ps;
    expr dst;
    parser_init(&ps, rs, text, mode, &dst, err, errlen);
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
            fail(&ps, "set stores into a register or [ADDR]; write [SYMBOL] for the memory at a "
                 "symbol");
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
    if (!parse_expr(rs, ps.p + 1, mode, &a->src, err, errlen)) {
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

/* The physical address of each byte of [addr, addr + size). */
static bool mem_where(uam_cpu *cpu, uint64_t addr, bool phys, unsigned size, uint64_t *pa,
                      char *why, size_t n)
{
    for (unsigned i = 0; i < size; i++) {
        if (!to_phys(cpu, addr + i, phys, &pa[i])) {
            snprintf(why, n, "virtual 0x%llx is not mapped", (unsigned long long)(addr + i));
            return false;
        }
    }
    return true;
}

static bool mem_read(uam_cpu *cpu, uint64_t addr, bool phys, unsigned size, uint64_t *val,
                     char *why, size_t n)
{
    const bool be = cpu->isa->default_endian == UAM_ACC_BE;
    uint64_t pa[8], v = 0;
    if (!mem_where(cpu, addr, phys, size, pa, why, n))
        return false;
    for (unsigned i = 0; i < size; i++) {
        uint64_t b;
        if (uam_bus_read(cpu->bus, pa[i], 1, UAM_ACC_LE | UAM_ACC_DEBUG, &b) != UAM_OK) {
            snprintf(why, n, "%s 0x%llx cannot be read", space_of(phys),
                     (unsigned long long)(addr + i));
            return false;
        }
        v |= (b & 0xFF) << 8 * (be ? size - 1 - i : i);
    }
    *val = v;
    return true;
}

static bool mem_write(uam_cpu *cpu, const uint64_t *pa, unsigned size, uint64_t val, char *why,
                      size_t n)
{
    const bool be = cpu->isa->default_endian == UAM_ACC_BE;
    for (unsigned i = 0; i < size; i++) {
        const uint64_t b = val >> 8 * (be ? size - 1 - i : i) & 0xFF;
        if (uam_bus_write(cpu->bus, pa[i], 1, UAM_ACC_LE | UAM_ACC_DEBUG, b) != UAM_OK) {
            snprintf(why, n, "physical 0x%llx cannot be written (a rule stores to RAM only)",
                     (unsigned long long)pa[i]);
            return false;
        }
    }
    return true;
}

/* What a rule fires on: the processor, the pc it is reported at (a pc rule's pc, the
 * accessing instruction of an access rule) and, for an access rule, the access. */
typedef struct fire {
    uam_cpu      *cpu;
    uint64_t      pc;
    const access *acc;
} fire;

/* f may be NULL for an expression parsed as a constant. */
static bool eval(const expr *e, const fire *f, uint64_t *out, char *why, size_t n)
{
    uint64_t st[EXPR_MAX];
    unsigned sp = 0;
    for (uint32_t i = 0; i < e->len; i++) {
        const node *nd = &e->n[i];
        uint64_t a, b;
        switch ((node_op)nd->op) {
        case N_CONST: st[sp++] = nd->val; continue;
        case N_PC:    st[sp++] = f->cpu->pc; continue;
        case N_ADDR:  st[sp++] = f->acc->addr; continue;
        case N_VALUE: st[sp++] = f->acc->val; continue;
        case N_REG:
            if (!f->cpu->isa->gdb->read_reg(f->cpu, nd->regno, &a)) {
                snprintf(why, n, "a register cannot be read");
                return false;
            }
            st[sp++] = a;
            continue;
        case N_LOAD:
            if (!mem_read(f->cpu, st[sp - 1], nd->phys, nd->size, &st[sp - 1], why, n))
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

/* ---- firing ------------------------------------------------------------------- */

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

/* Work a set action out: the value, and where a store to memory goes. */
static void resolve(rule *r, const action *a, const fire *f, held *h)
{
    char why[96];
    uint64_t addr;
    *h = (held){ .ok = true };
    if (!eval(&a->src, f, &h->val, why, sizeof(why))
        || (a->dst.kind == LV_MEM
            && (!eval(&a->dst.addr, f, &addr, why, sizeof(why))
                || !mem_where(f->cpu, addr, a->dst.phys, a->dst.size, h->pa, why, sizeof(why))))) {
        h->ok = false;
        note_failure(r, f->cpu, "set", why);
    }
}

/* Carry a worked-out set action out on cpu. */
static void apply(rule *r, const action *a, const held *h, uam_cpu *cpu)
{
    char why[96];
    if (!h->ok)
        return;
    switch (a->dst.kind) {
    case LV_PC:
        uam_cpu_set_pc(cpu, h->val);
        break;
    case LV_REG:
        if (!cpu->isa->gdb->write_reg || !cpu->isa->gdb->write_reg(cpu, a->dst.regno, h->val))
            note_failure(r, cpu, "set", "the register cannot be written");
        break;
    case LV_MEM:
        if (!mem_write(cpu, h->pa, a->dst.size, h->val, why, sizeof(why)))
            note_failure(r, cpu, "set", why);
        break;
    }
}

/* "rule NAME: cpu N pc 0x..", and the access of an access rule. */
static void log_head(const rule *r, const fire *f, FILE *out)
{
    fprintf(out, "rule %s: cpu %u pc 0x%llx", r->name, f->cpu->hartid, (unsigned long long)f->pc);
    if (f->acc)
        fprintf(out, " %s %u at 0x%llx value 0x%llx", f->acc->is_write ? "write" : "read",
                f->acc->size, (unsigned long long)f->acc->addr, (unsigned long long)f->acc->val);
}

static void do_log(rule *r, const action *a, const fire *f)
{
    FILE *out = r->set->log ? r->set->log : stderr;
    log_head(r, f, out);
    for (uint32_t i = 0; i < a->n_log; i++) {
        char why[96];
        uint64_t v;
        if (eval(&a->log[i], f, &v, why, sizeof(why))) {
            fprintf(out, " %s=0x%llx", a->log_text[i], (unsigned long long)v);
        } else {
            fprintf(out, " %s=?", a->log_text[i]);
            note_failure(r, f->cpu, "log", why);
        }
    }
    fprintf(out, " icount=%llu\n", (unsigned long long)f->cpu->icount);
}

static void do_dump(rule *r, const action *a, const fire *f)
{
    FILE *out = r->set->log ? r->set->log : stderr;
    char why[96] = "";
    uint64_t addr, len, got = 0;
    if (!eval(&a->dst.addr, f, &addr, why, sizeof(why)) || !eval(&a->src, f, &len, why, sizeof(why))) {
        note_failure(r, f->cpu, "dump", why);
        return;
    }
    uint8_t *buf = len <= DUMP_MAX ? malloc(len ? len : 1) : NULL;
    if (!buf) {
        snprintf(why, sizeof(why), "0x%llx bytes are more than a dump shows (%u)",
                 (unsigned long long)len, DUMP_MAX);
        note_failure(r, f->cpu, "dump", why);
        return;
    }
    for (; got < len; got++) {
        uint64_t b;
        if (!mem_read(f->cpu, addr + got, a->dst.phys, 1, &b, why, sizeof(why)))
            break;
        buf[got] = (uint8_t)b;
    }
    log_head(r, f, out);
    fprintf(out, " dump %s0x%llx+0x%llx icount=%llu\n", a->dst.phys ? "p:" : "",
            (unsigned long long)addr, (unsigned long long)len, (unsigned long long)f->cpu->icount);
    for (uint64_t i = 0; i < got; i += 16) {
        char hex[16 * 3 + 1] = "", asc[17] = "";
        for (unsigned k = 0; k < 16 && i + k < got; k++) {
            const uint8_t c = buf[i + k];
            snprintf(hex + 3 * k, 4, "%02x ", c);
            asc[k] = c >= 0x20 && c < 0x7F ? (char)c : '.';
        }
        fprintf(out, "  %016llx  %-48s |%s|\n", (unsigned long long)(addr + i), hex, asc);
    }
    if (got < len) {
        fprintf(out, "  (%s)\n", why);
        note_failure(r, f->cpu, "dump", why);
    }
    free(buf);
}

/* The rule's place was reached: check its conditions, run its actions. */
static uam_hook_action fire_rule(rule *r, const fire *f)
{
    uam_rules *rs = r->set;
    r->arrivals++;
    if ((r->limit && r->fired >= r->limit) || r->waiting)
        return UAM_HOOK_CONTINUE;
    for (uint32_t i = 0; i < r->n_cond; i++) {
        char why[96];
        uint64_t v;
        if (!eval(&r->cond[i], f, &v, why, sizeof(why))) {
            note_failure(r, f->cpu, "if", why);
            return UAM_HOOK_CONTINUE;
        }
        if (!v)
            return UAM_HOOK_CONTINUE;
    }
    r->fired++;
    uam_hook_action ret = UAM_HOOK_CONTINUE;
    for (uint32_t i = 0, k = 0; i < r->n_act; i++) {
        const action *a = &r->act[i];
        switch (a->kind) {
        case A_SET: {
            held h;
            resolve(r, a, f, &h);
            if (r->delay)
                r->held[k] = h;
            else
                apply(r, a, &h, f->cpu);
            k++;
            break;
        }
        case A_LOG:  do_log(r, a, f); break;
        case A_DUMP: do_dump(r, a, f); break;
        case A_STOP:
            rs->stopped = true;
            rs->stop = (uam_rule_stop){ .name = r->name, .cpu = f->cpu->hartid, .pc = f->pc,
                                        .icount = f->cpu->icount };
            ret = UAM_HOOK_STOP;
            break;
        }
    }
    if (r->delay) {
        r->waiting = true;
        r->wait_cpu = f->cpu->hartid;
        uam_timer_arm(&rs->m->sched, &r->timer, uam_sched_now(&rs->m->sched) + r->delay);
    }
    return ret;
}

/* A delay is over: the stores worked out when the rule fired happen now. */
static void deliver(void *ctx)
{
    rule *r = ctx;
    uam_cpu *cpu = uam_machine_cpu(r->set->m, r->wait_cpu);
    r->waiting = false;
    for (uint32_t i = 0, k = 0; i < r->n_act; i++)
        if (r->act[i].kind == A_SET)
            apply(r, &r->act[i], &r->held[k++], cpu);
}

static uam_hook_action pc_hook(void *ctx, uam_cpu *cpu, uint64_t pc)
{
    rule *r = ctx;
    if (r->cpu >= 0 && cpu->hartid != (unsigned)r->cpu)
        return UAM_HOOK_CONTINUE;
    return fire_rule(r, &(fire){ .cpu = cpu, .pc = pc });
}

/* The instruction that made the noted accesses has completed (uam_exec_request_call). */
static uam_hook_action accessed(void *ctx, uam_cpu *cpu, uint64_t pc)
{
    uam_rules *rs = ctx;
    uam_hook_action ret = UAM_HOOK_CONTINUE;
    (void)pc;
    for (uint32_t i = 0; i < rs->n; i++) {
        rule *r = rs->r[i];
        if (!r->hit || r->hit_cpu != cpu->hartid)
            continue;
        r->hit = false;
        if (fire_rule(r, &(fire){ .cpu = cpu, .pc = r->acc.pc, .acc = &r->acc }) == UAM_HOOK_STOP)
            ret = UAM_HOOK_STOP;
    }
    return ret;
}

/* Bus watch of an access rule: note the instruction's first access to the range, and
 * have the rule run when the instruction is done. */
static void on_access(void *ctx, uint64_t pa, unsigned size, bool is_write, uint64_t val)
{
    const piece *p = ctx;
    rule *r = p->r;
    uam_machine *m = r->set->m;
    uam_origin o;
    if (!uam_origin_get(&o))   /* nothing runs: not the guest's access */
        return;
    const unsigned hart = uam_machine_current_cpu(m);
    if (r->cpu >= 0 && hart != (unsigned)r->cpu)
        return;
    if (!r->hit) {
        r->hit = true;
        r->hit_cpu = hart;
        r->acc = (access){ .is_write = is_write, .size = size, .val = val,
                           .addr = r->addr + p->off + (pa - p->pa),   /* wraps when it starts earlier */
                           .pc = uam_machine_cpu(m, hart)->insn_pc };
    }
    uam_exec_request_call(uam_machine_exec(m, r->hit_cpu), accessed, r->set);
}

/* ---- access rules: the watches -------------------------------------------------- */

static void disarm(rule *r)
{
    for (uint32_t i = 0; i < r->n_piece; i++)
        if (r->piece[i].wid > 0)
            uam_bus_remove_watch(&r->set->m->bus, r->piece[i].wid);
    free(r->piece);
    r->piece = NULL;
    r->n_piece = 0;
}

/* Watch the rule's range: one piece per page of a virtual range, as the rule's processor
 * (processor 0 without cpu=N) maps it now, merged where pages are physically contiguous. */
static bool arm(rule *r, char *err, size_t errlen)
{
    uam_machine *m = r->set->m;
    uam_cpu *cpu = uam_machine_cpu(m, r->cpu >= 0 ? (unsigned)r->cpu : 0);
    const bool virt = !r->phys && cpu->isa->translate;
    const uam_xlate_kind kind = r->dir == UAM_WATCH_WRITE ? UAM_XLATE_STORE : UAM_XLATE_LOAD;
    disarm(r);
    r->piece = calloc(virt ? r->len / UAM_PAGE_SIZE + 2 : 1, sizeof(*r->piece));
    if (!r->piece) {
        set_err(err, errlen, "out of memory");
        return false;
    }
    for (uint64_t off = 0; off < r->len;) {
        uint64_t va = r->addr + off, pa = va, chunk = r->len - off;
        if (virt) {
            const uint64_t room = UAM_PAGE_SIZE - (va & (UAM_PAGE_SIZE - 1));
            if (chunk > room)
                chunk = room;
            uam_xlate xl = { 0 };
            if (cpu->isa->translate(cpu, va, kind, &xl) != UAM_OK
                && cpu->isa->translate(cpu, va, UAM_XLATE_DEBUG, &xl) != UAM_OK) {
                set_err(err, errlen, "rule %s: processor %u does not map virtual 0x%llx (p: gives "
                        "a physical address)", r->name, cpu->hartid, (unsigned long long)va);
                disarm(r);
                return false;
            }
            if (!xl.fixed && !xl.regs_only)
                cpu->xlate_tables = true;
            pa = xl.paddr;
        }
        piece *last = r->n_piece ? &r->piece[r->n_piece - 1] : NULL;
        if (last && last->pa + last->len == pa)
            last->len += chunk;
        else
            r->piece[r->n_piece++] = (piece){ .r = r, .pa = pa, .len = chunk, .off = off };
        off += chunk;
    }
    for (uint32_t i = 0; i < r->n_piece; i++) {
        piece *p = &r->piece[i];
        p->wid = uam_bus_add_watch(&m->bus, p->pa, p->len, r->dir, on_access, p);
        if (p->wid <= 0) {
            set_err(err, errlen, "rule %s: cannot watch physical 0x%llx", r->name,
                    (unsigned long long)p->pa);
            disarm(r);
            return false;
        }
    }
    return true;
}

void uam_rules_arm_later(uam_rules *rs)
{
    rs->arm_later = true;
}

uam_status uam_rules_arm(uam_rules *rs, char *err, size_t errlen)
{
    for (uint32_t i = 0; i < rs->n; i++)
        if (rs->r[i]->on_access && !rs->r[i]->off && !arm(rs->r[i], err, errlen))
            return UAM_ERR_INVALID;
    return UAM_OK;
}

/* ---- what a snapshot holds ------------------------------------------------------- */

/* The rules whose past decides what the machine does next: a count, stores on their way. */
static bool stateful(const rule *r)
{
    return r->limit || r->delay;
}

static uint32_t n_stateful(const uam_rules *rs)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < rs->n; i++)
        n += stateful(rs->r[i]);
    return n;
}

static void rules_save(void *ctx, uam_state_writer *w)
{
    const uam_rules *rs = ctx;
    uam_state_put_u32(w, n_stateful(rs));
    for (uint32_t i = 0; i < rs->n; i++) {
        const rule *r = rs->r[i];
        if (!stateful(r))
            continue;
        uam_state_put_u64(w, r->id);
        uam_state_put_u64(w, r->fired);
        if (!r->delay)
            continue;
        uam_state_put_timer(w, &r->timer);
        uam_state_put_bool(w, r->waiting);
        uam_state_put_u32(w, r->wait_cpu);
        for (uint32_t k = 0; k < r->n_set; k++) {
            uam_state_put_bool(w, r->held[k].ok);
            uam_state_put_u64(w, r->held[k].val);
            for (unsigned b = 0; b < 8; b++)
                uam_state_put_u64(w, r->held[k].pa[b]);
        }
    }
}

static void rules_restore(void *ctx, uam_state_reader *rd)
{
    uam_rules *rs = ctx;
    const uint32_t n = uam_state_get_u32(rd);
    if (n != n_stateful(rs)) {
        uam_state_fail(rd, "the snapshot was taken with %u rules that have a count or a delay, "
                       "the set has %u", (unsigned)n, (unsigned)n_stateful(rs));
        return;
    }
    for (uint32_t i = 0; i < rs->n; i++) {
        rule *r = rs->r[i];
        if (!stateful(r))
            continue;
        if (uam_state_get_u64(rd) != r->id) {
            uam_state_fail(rd, "the snapshot was taken with other rules that have a count or a "
                           "delay (rule %s)", r->name);
            return;
        }
        r->fired = uam_state_get_u64(rd);
        r->hit = false;
        if (!r->delay)
            continue;
        uam_state_get_timer(rd, &r->timer);
        r->waiting = uam_state_get_bool(rd);
        r->wait_cpu = uam_state_get_u32(rd);
        if (r->wait_cpu >= uam_machine_ncpus(rs->m)) {
            uam_state_fail(rd, "rule %s: processor out of range", r->name);
            return;
        }
        for (uint32_t k = 0; k < r->n_set; k++) {
            r->held[k].ok = uam_state_get_bool(rd);
            r->held[k].val = uam_state_get_u64(rd);
            for (unsigned b = 0; b < 8; b++)
                r->held[k].pa[b] = uam_state_get_u64(rd);
        }
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

/* The rule's memory; its watches and timer are the machine's business (uam_rules_detach). */
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
    free(r->piece);
    free(r->held);
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
            uam_machine_remove_pc_hook(rs->m, r->pc, pc_hook, r);
        r->hooked = false;
        disarm(r);
        r->hit = false;
        if (r->delay)
            uam_timer_cancel(&rs->m->sched, &r->timer);
        r->waiting = false;
    }
    for (unsigned i = 0; i < uam_machine_ncpus(rs->m); i++)
        uam_exec_cancel_call(uam_machine_exec(rs->m, i), accessed, rs);
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
static char *after_word(char *item, const char *word)
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

/* `text` as a constant: numbers and symbols. */
static bool constant(uam_rules *rs, const char *text, uint64_t *v, char *err, size_t errlen)
{
    expr e;
    if (!parse_expr(rs, text, M_CONST, &e, err, errlen))
        return false;
    eval(&e, NULL, v, NULL, 0);
    expr_free(&e);
    return true;
}

static action *new_action(rule *r, act_kind kind, char *err, size_t errlen)
{
    action *na = realloc(r->act, sizeof(*na) * (r->n_act + 1));
    if (!na) {
        set_err(err, errlen, "out of memory");
        return NULL;
    }
    r->act = na;
    na[r->n_act] = (action){ .kind = kind };
    return &na[r->n_act];
}

/* "EXPR EXPR ...": what a log action shows. */
static bool parse_log(uam_rules *rs, parse_mode mode, char *list, action *a, char *err, size_t errlen)
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
        if (!parse_expr(rs, tok, mode, &a->log[a->n_log], err, errlen))
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

/* "[p:]ADDR LEN": what a dump action shows. */
static bool parse_dump(uam_rules *rs, parse_mode mode, char *list, action *a, char *err, size_t errlen)
{
    char *addr = strtok(list, " \t"), *len = strtok(NULL, " \t");
    if (!addr || !len || strtok(NULL, " \t")) {
        set_err(err, errlen, "dump wants [p:]ADDR LEN (two expressions, a blank between them)");
        return false;
    }
    if (!strncmp(addr, "p:", 2)) {
        a->dst.phys = true;
        addr += 2;
    }
    if (!parse_expr(rs, addr, mode, &a->dst.addr, err, errlen))
        return false;
    if (!parse_expr(rs, len, mode, &a->src, err, errlen)) {
        expr_free(&a->dst.addr);
        return false;
    }
    return true;
}

static bool parse_item(uam_rules *rs, rule *r, char *item, char *err, size_t errlen)
{
    const parse_mode mode = r->on_access ? M_ACCESS : M_STATE;
    char *rest, *val;
    uint64_t v;
    action *a;
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
    } else if ((val = value_of(item, "delay"))) {
        if (!whole_number(val, &v) || v > UAM_SCHED_TIME_MAX / 2) {
            set_err(err, errlen, "bad '%s': delay=N, N ticks of virtual time", item);
            return false;
        }
        r->delay = v;
    } else if ((val = value_of(item, "len"))) {
        if (!r->on_access) {
            set_err(err, errlen, "len=N goes with read=, write= or access=");
            return false;
        }
        if (!constant(rs, val, &v, err, errlen))
            return false;
        if (!v || v > RANGE_MAX) {
            set_err(err, errlen, "bad '%s': len=N, N from 1 to 0x%llx bytes", item,
                    (unsigned long long)RANGE_MAX);
            return false;
        }
        r->len = v;
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
        if (!parse_expr(rs, rest, mode, &nc[r->n_cond], err, errlen))
            return false;
        r->n_cond++;
    } else if ((rest = after_word(item, "set")) && *rest) {
        if (!(a = new_action(r, A_SET, err, errlen)) || !parse_set(rs, rest, mode, a, err, errlen))
            return false;
        r->n_act++;
        r->n_set++;
    } else if ((rest = after_word(item, "log"))) {
        if (!(a = new_action(r, A_LOG, err, errlen)))
            return false;
        r->n_act++;   /* owns what parse_log allocates, also when it fails */
        if (!parse_log(rs, mode, rest, a, err, errlen))
            return false;
    } else if ((rest = after_word(item, "dump"))) {
        if (!(a = new_action(r, A_DUMP, err, errlen)) || !parse_dump(rs, mode, rest, a, err, errlen))
            return false;
        r->n_act++;
    } else if (!strcmp(item, "stop")) {
        if (!new_action(r, A_STOP, err, errlen))
            return false;
        r->n_act++;
    } else {
        set_err(err, errlen, "unknown item '%s' (cpu=N, if COND, count=N, delay=N, len=N, "
                "name=NAME, set DST=EXPR, log [EXPR...], dump [p:]ADDR LEN, stop)", item);
        return false;
    }
    return true;
}

/* The first item: pc=ADDR, or read=, write= or access= with [p:]ADDR. */
static bool parse_trigger(uam_rules *rs, rule *r, char *item, char *err, size_t errlen)
{
    static const struct { const char *key; unsigned dir; } on[] = {
        { "read", UAM_WATCH_READ }, { "write", UAM_WATCH_WRITE },
        { "access", UAM_WATCH_READ | UAM_WATCH_WRITE },
    };
    char *val = value_of(item, "pc");
    if (!(r->name = strdup(item))) {
        set_err(err, errlen, "out of memory");
        return false;
    }
    if (val)
        return constant(rs, val, &r->pc, err, errlen);
    for (size_t i = 0; i < UAM_ARRAY_LEN(on); i++) {
        if (!(val = value_of(item, on[i].key)))
            continue;
        r->on_access = true;
        r->dir = on[i].dir;
        r->len = 1;
        if (!strncmp(val, "p:", 2)) {
            r->phys = true;
            val += 2;
        }
        return constant(rs, val, &r->addr, err, errlen);
    }
    set_err(err, errlen, "a rule starts with pc=ADDR, or read=, write= or access= and [p:]ADDR");
    return false;
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

/* What the rule's parts must add up to. */
static bool check_rule(const rule *r, char *err, size_t errlen)
{
    bool stop = false;
    for (uint32_t i = 0; i < r->n_act; i++)
        stop |= r->act[i].kind == A_STOP;
    if (!r->n_act)
        set_err(err, errlen, "the rule does nothing: add set DST=EXPR, log [EXPR...], "
                "dump [p:]ADDR LEN or stop");
    else if (r->delay && !r->n_set)
        set_err(err, errlen, "delay=N postpones the rule's stores, and it has none (set DST=EXPR)");
    else if (r->delay && stop)
        set_err(err, errlen, "a rule with delay=N cannot stop: only its stores are postponed");
    else if (r->on_access && r->addr + r->len - 1 < r->addr)
        set_err(err, errlen, "the range runs past the end of the address space");
    else
        return true;
    return false;
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
        r->id = UINT64_C(0xcbf29ce484222325);
        for (const char *c = text; *c; c++)
            r->id = (r->id ^ (uint8_t)*c) * UINT64_C(0x100000001b3);
    }
    /* The items, split at the commas (no expression holds one). */
    bool first = true;
    for (char *item = copy, *next; ok && item; item = next, first = false) {
        next = strchr(item, ',');
        if (next)
            *next++ = '\0';
        item = trim(item);
        if (first) {
            ok = parse_trigger(rs, r, item, err, errlen);
        } else if (!*item) {
            set_err(err, errlen, "an empty item (two commas, or one at the end)");
            ok = false;
        } else {
            ok = parse_item(rs, r, item, err, errlen);
        }
    }
    ok = ok && check_rule(r, err, errlen);
    if (ok && r->delay) {
        uam_timer_init(&r->timer, deliver, r);
        if (!(r->held = calloc(r->n_set, sizeof(*r->held)))) {
            set_err(err, errlen, "out of memory");
            ok = false;
        }
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
    if (ok && stateful(r) && !rs->has_state) {
        if (uam_machine_add_state(rs->m, rs->state_name, rules_save, rules_restore, rs) != UAM_OK) {
            set_err(err, errlen, "cannot register the state '%s' (is another rule set called "
                    "the same?)", rs->state_name);
            ok = false;
        }
        rs->has_state = ok;
    }
    if (ok && r->on_access) {
        ok = rs->arm_later || arm(r, err, errlen);
    } else if (ok) {
        ok = r->hooked = uam_machine_add_pc_hook(rs->m, r->pc, pc_hook, r) == UAM_OK;
        if (!ok)
            set_err(err, errlen, "cannot hook pc 0x%llx", (unsigned long long)r->pc);
    }
    free(copy);
    if (!ok) {
        rule_free(r);
        return -1;
    }
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
    *out = (uam_rule_info){ .name = r->name, .pc = r->pc, .addr = r->addr,
                            .len = r->on_access ? r->len : 0, .arrivals = r->arrivals,
                            .fired = r->fired, .failed = r->failed, .limit = r->limit,
                            .delay = r->delay, .waiting = r->waiting };
    return true;
}

bool uam_rules_last_stop(const uam_rules *rs, uam_rule_stop *out)
{
    if (!rs->stopped)
        return false;
    *out = rs->stop;
    return true;
}

void uam_rules_clear_stop(uam_rules *rs)
{
    rs->stopped = false;
}

bool uam_rules_enabled(const uam_rules *rs, size_t i)
{
    return i < rs->n && !rs->r[i]->off;
}

uam_status uam_rules_enable(uam_rules *rs, size_t i, bool on, char *err, size_t errlen)
{
    if (i >= rs->n) {
        set_err(err, errlen, "no rule %zu", i);
        return UAM_ERR_INVALID;
    }
    rule *r = rs->r[i];
    if (r->off == !on)
        return UAM_OK;
    if (!on) {
        if (r->hooked)
            uam_machine_remove_pc_hook(rs->m, r->pc, pc_hook, r);
        r->hooked = false;
        disarm(r);
        r->hit = false;
        if (r->delay && r->waiting) {   /* stores on their way do not happen either */
            uam_timer_cancel(&rs->m->sched, &r->timer);
            r->waiting = false;
        }
        r->off = true;
        return UAM_OK;
    }
    if (r->on_access) {
        if (!rs->arm_later && !arm(r, err, errlen))
            return UAM_ERR_INVALID;
    } else if (uam_machine_add_pc_hook(rs->m, r->pc, pc_hook, r) != UAM_OK) {
        set_err(err, errlen, "rule %s: cannot hook 0x%llx", r->name, (unsigned long long)r->pc);
        return UAM_ERR_NOMEM;
    } else {
        r->hooked = true;
    }
    r->off = false;
    return UAM_OK;
}
