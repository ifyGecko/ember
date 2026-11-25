/*
 * monitor_inspect.c - The monitor's commands that look at and change the machine:
 * regs, reg, x, write, load, dump, map, history, bt.
 *
 * Memory and registers are accessed as a debugger does (uam/debug.h): a virtual
 * address as the client's processor ("cpu N") translates it, no device side effects.
 * Everything here but "map" needs the machine stopped.
 */
#define _POSIX_C_SOURCE 200809L   /* open_memstream */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uam/debug.h"
#include "monitor_int.h"

#define X_MAX    (64u << 10)    /* bytes "x" shows at most */
#define LOAD_MAX (64u << 20)    /* bytes "load" takes at most */

static bool big_endian(const uam_cpu *cpu)
{
    return (cpu->isa->default_endian & UAM_ACC_BE) != 0;
}

/* Lines a FILE-writing function produced, as output lines of the call. */
typedef void (*to_file_fn)(void *ctx, FILE *out);

static bool print_from(uam_monitor_call *c, to_file_fn fn, void *ctx, const char *skip_prefix,
                       size_t last_n)
{
    char *text = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&text, &len);
    if (!f) {
        uam_monitor_fail(c, "%s: %s", uam_monitor_arg(c, 0), strerror(errno));
        return false;
    }
    fn(ctx, f);
    fclose(f);
    /* Count lines to keep only the last N (0: all). */
    size_t total = 0;
    for (size_t i = 0; i < len; i++)
        total += text[i] == '\n';
    size_t first = last_n && total > last_n ? total - last_n : 0, at = 0;
    for (char *p = text, *nl; p && *p; p = nl ? nl + 1 : NULL, at++) {
        nl = strchr(p, '\n');
        if (nl)
            *nl = 0;
        if (at < first)
            continue;
        size_t k = skip_prefix ? strlen(skip_prefix) : 0;
        uam_monitor_print(c, "%s", k && !strncmp(p, skip_prefix, k) ? p + k : p);
    }
    free(text);
    return true;
}

/* ---- registers --------------------------------------------------------------- */

static void reg_text(uam_cpu *cpu, unsigned r, char *buf, size_t n)
{
    unsigned size = uam_debug_reg_size(cpu, r);
    uint8_t wide[UAM_DEBUG_REG_MAX];
    uint64_t v = 0;
    if (!uam_debug_reg_read(cpu, r, &v, wide)) {
        snprintf(buf, n, "?");
        return;
    }
    if (size <= 8) {
        snprintf(buf, n, "0x%0*llx", (int)(2 * size), (unsigned long long)v);
        return;
    }
    /* Wider: most significant byte first. */
    size_t o = (size_t)snprintf(buf, n, "0x");
    for (unsigned i = 0; i < size && o + 3 <= n; i++) {
        unsigned b = big_endian(cpu) ? i : size - 1 - i;
        o += (size_t)snprintf(buf + o, n - o, "%02x", wide[b]);
    }
}

typedef struct reg_walk {
    uam_monitor_call *c;
    uam_cpu          *cpu;
} reg_walk;

static void put_reg(void *ctx, const char *name, unsigned regno)
{
    reg_walk *w = ctx;
    char text[2 * UAM_DEBUG_REG_MAX + 4];
    reg_text(w->cpu, regno, text, sizeof(text));
    uam_monitor_field(w->c, name, "%s", text);
}

static bool have_regs(uam_monitor_call *c, uam_cpu *cpu)
{
    if (!uam_debug_reg_count(cpu)) {
        uam_monitor_fail(c, "%s: this ISA has no register view", uam_monitor_arg(c, 0));
        return false;
    }
    return true;
}

static void cmd_regs(uam_monitor_call *c, void *ctx)
{
    (void)ctx;
    uam_cpu *cpu = uam_monitor_cpu(c);
    if (!have_regs(c, cpu))
        return;
    reg_walk w = { c, cpu };
    uam_isa_reg_names(cpu->isa, put_reg, &w);
}

typedef struct name_buf {
    char  *p;
    size_t n, cap;
} name_buf;

static void list_name(void *ctx, const char *name, unsigned regno)
{
    name_buf *b = ctx;
    (void)regno;
    size_t k = strlen(name);
    if (b->n + k + 2 > b->cap)
        return;
    if (b->n)
        b->p[b->n++] = ' ';
    memcpy(b->p + b->n, name, k + 1);
    b->n += k;
}

static void cmd_reg(uam_monitor_call *c, void *ctx)
{
    (void)ctx;
    uam_cpu *cpu = uam_monitor_cpu(c);
    if (!have_regs(c, cpu))
        return;
    int argc = uam_monitor_argc(c);
    const char *name = uam_monitor_arg(c, 1);
    unsigned r;
    if (argc < 2 || argc > 3) {
        uam_monitor_fail(c, "reg: NAME [VALUE]");
        return;
    }
    if (!uam_isa_reg_by_name(cpu->isa, name, &r)) {
        char names[2048];
        name_buf b = { names, 0, sizeof(names) };
        names[0] = 0;
        uam_isa_reg_names(cpu->isa, list_name, &b);
        uam_monitor_fail(c, "reg: no register '%s' (registers: %s)", name, names);
        return;
    }
    if (argc == 3) {
        char *end;
        const char *t = uam_monitor_arg(c, 2);
        uint64_t v = strtoull(t, &end, 0);
        if (end == t || *end) {
            uam_monitor_fail(c, "reg: '%s' is not a number", t);
            return;
        }
        if (uam_debug_reg_size(cpu, r) > 8) {
            uam_monitor_fail(c, "reg: %s is wider than 64 bits; it cannot be set here", name);
            return;
        }
        if (!uam_debug_reg_write(cpu, r, v, NULL)) {
            uam_monitor_fail(c, "reg: %s cannot be written", name);
            return;
        }
    }
    char text[2 * UAM_DEBUG_REG_MAX + 4];
    reg_text(cpu, r, text, sizeof(text));
    uam_monitor_field(c, name, "%s", text);
}

/* ---- memory -------------------------------------------------------------------- */

static unsigned parse_width(const char *s)
{
    if (!s)
        return 1;
    if (!strcmp(s, "w1")) return 1;
    if (!strcmp(s, "w2")) return 2;
    if (!strcmp(s, "w4")) return 4;
    if (!strcmp(s, "w8")) return 8;
    return 0;
}

static uint64_t word_at(const uint8_t *p, unsigned w, bool big)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < w; i++)
        v |= (uint64_t)p[i] << (8 * (big ? w - 1 - i : i));
    return v;
}

/* Lines of "ADDR: bytes |text|" (width 1) or "ADDR: words" (2, 4, 8 in target order). */
static void hex_lines(uam_monitor_call *c, uint64_t addr, const uint8_t *buf, uint64_t len,
                      unsigned w, bool big)
{
    const unsigned per = 16;
    for (uint64_t i = 0; i < len; i += per) {
        char line[200];
        size_t o = (size_t)snprintf(line, sizeof(line), "%016llx:", (unsigned long long)(addr + i));
        uint64_t n = len - i < per ? len - i : per;
        if (w == 1) {
            char asc[17] = "";
            for (unsigned k = 0; k < per; k++) {
                if (k < n) {
                    o += (size_t)snprintf(line + o, sizeof(line) - o, " %02x", buf[i + k]);
                    asc[k] = buf[i + k] >= 0x20 && buf[i + k] < 0x7f ? (char)buf[i + k] : '.';
                } else {
                    o += (size_t)snprintf(line + o, sizeof(line) - o, "   ");
                }
            }
            snprintf(line + o, sizeof(line) - o, "  |%s|", asc);
        } else {
            for (uint64_t k = 0; k + w <= n; k += w)
                o += (size_t)snprintf(line + o, sizeof(line) - o, " 0x%0*llx", (int)(2 * w),
                                      (unsigned long long)word_at(buf + i + k, w, big));
        }
        uam_monitor_print(c, "%s", line);
    }
}

static bool get_addr(uam_monitor_call *c, int i, uint64_t *addr, bool *phys)
{
    const char *t = uam_monitor_arg(c, i);
    if (!t || !uam_debug_parse_addr(t, addr, phys)) {
        uam_monitor_fail(c, "%s: '%s' is not an address ([p:]ADDR)", uam_monitor_arg(c, 0),
                         t ? t : "");
        return false;
    }
    return true;
}

static void cmd_x(uam_monitor_call *c, void *ctx)
{
    (void)ctx;
    uam_machine *m = uam_monitor_machine(uam_monitor_of(c));
    uam_cpu *cpu = uam_monitor_cpu(c);
    int argc = uam_monitor_argc(c);
    uint64_t addr, len = 64;
    bool phys;
    if (argc < 2 || argc > 4) {
        uam_monitor_fail(c, "x: [p:]ADDR [LEN] [w1|w2|w4|w8]");
        return;
    }
    if (!get_addr(c, 1, &addr, &phys))
        return;
    unsigned w = 1;
    for (int i = 2; i < argc; i++) {
        const char *t = uam_monitor_arg(c, i);
        char *end;
        if (t[0] == 'w') {
            if (!(w = parse_width(t))) {
                uam_monitor_fail(c, "x: the width is w1, w2, w4 or w8");
                return;
            }
        } else {
            len = strtoull(t, &end, 0);
            if (end == t || *end || !len || len > X_MAX) {
                uam_monitor_fail(c, "x: LEN is 1 to %u bytes", X_MAX);
                return;
            }
        }
    }
    len -= len % w;
    if (!len) {
        uam_monitor_fail(c, "x: LEN is less than one word");
        return;
    }
    uint8_t *buf = malloc(len);
    if (!buf) {
        uam_monitor_fail(c, "x: out of memory");
        return;
    }
    char why[256] = "";
    uint64_t got = uam_debug_read(m, cpu, addr, phys, buf, len, why, sizeof(why));
    got -= got % w;
    if (!got)
        uam_monitor_fail(c, "x: %s", why);
    else {
        hex_lines(c, addr, buf, got, w, big_endian(cpu));
        if (got < len)
            uam_monitor_field(c, "short", "%llu of %llu bytes: %s", (unsigned long long)got,
                              (unsigned long long)len, why);
    }
    free(buf);
}

static void cmd_write(uam_monitor_call *c, void *ctx)
{
    (void)ctx;
    uam_machine *m = uam_monitor_machine(uam_monitor_of(c));
    uam_cpu *cpu = uam_monitor_cpu(c);
    int argc = uam_monitor_argc(c);
    uint64_t addr;
    bool phys;
    unsigned w;
    if (argc < 4 || !(w = parse_width(uam_monitor_arg(c, 2)))) {
        uam_monitor_fail(c, "write: [p:]ADDR w1|w2|w4|w8 VALUE...");
        return;
    }
    if (!get_addr(c, 1, &addr, &phys))
        return;
    size_t n = (size_t)(argc - 3) * w;
    uint8_t *buf = malloc(n);
    if (!buf) {
        uam_monitor_fail(c, "write: out of memory");
        return;
    }
    const bool big = big_endian(cpu);
    for (int i = 3; i < argc; i++) {
        const char *t = uam_monitor_arg(c, i);
        char *end;
        uint64_t v = strtoull(t, &end, 0);
        if (end == t || *end || (w < 8 && v >> (8 * w))) {
            uam_monitor_fail(c, "write: '%s' is not a %u-byte value", t, w);
            free(buf);
            return;
        }
        uint8_t *p = buf + (size_t)(i - 3) * w;
        for (unsigned k = 0; k < w; k++)
            p[k] = (uint8_t)(v >> (8 * (big ? w - 1 - k : k)));
    }
    char why[256] = "";
    uint64_t done = uam_debug_write(m, cpu, addr, phys, buf, n, why, sizeof(why));
    free(buf);
    if (done < n)
        uam_monitor_fail(c, "write: %llu of %zu bytes written: %s", (unsigned long long)done, n,
                         why);
    else
        uam_monitor_field(c, "written", "%zu", n);
}

static void cmd_load(uam_monitor_call *c, void *ctx)
{
    (void)ctx;
    uam_machine *m = uam_monitor_machine(uam_monitor_of(c));
    uam_cpu *cpu = uam_monitor_cpu(c);
    uint64_t addr;
    bool phys;
    if (uam_monitor_argc(c) != 3) {
        uam_monitor_fail(c, "load: [p:]ADDR FILE");
        return;
    }
    if (!get_addr(c, 1, &addr, &phys))
        return;
    const char *path = uam_monitor_arg(c, 2);
    FILE *f = fopen(path, "rb");
    if (!f) {
        uam_monitor_fail(c, "load: %s: %s", path, strerror(errno));
        return;
    }
    uint8_t *buf = malloc(LOAD_MAX);
    size_t n = buf ? fread(buf, 1, LOAD_MAX, f) : 0;
    bool more = buf && fgetc(f) != EOF;
    fclose(f);
    if (!buf || more) {
        uam_monitor_fail(c, buf ? "load: %s is larger than 64 MiB" : "load: out of memory", path);
        free(buf);
        return;
    }
    char why[256] = "";
    uint64_t done = uam_debug_write(m, cpu, addr, phys, buf, n, why, sizeof(why));
    free(buf);
    if (done < n)
        uam_monitor_fail(c, "load: %llu of %zu bytes written: %s", (unsigned long long)done, n, why);
    else
        uam_monitor_field(c, "written", "%zu", n);
}

static void cmd_dump(uam_monitor_call *c, void *ctx)
{
    (void)ctx;
    uam_machine *m = uam_monitor_machine(uam_monitor_of(c));
    uam_cpu *cpu = uam_monitor_cpu(c);
    uam_debug_range r;
    if (uam_monitor_argc(c) != 2 || !uam_debug_parse_range(uam_monitor_arg(c, 1), &r)) {
        uam_monitor_fail(c, "dump: [p:]ADDR+LEN[=FILE] (LEN with K/M/G, at most 1G)");
        return;
    }
    if (!r.file && r.len > X_MAX) {
        uam_monitor_fail(c, "dump: more than %u bytes goes to a file (=FILE)", X_MAX);
        return;
    }
    uint8_t *buf = malloc(r.len);
    if (!buf) {
        uam_monitor_fail(c, "dump: out of memory");
        return;
    }
    char why[256] = "";
    uint64_t got = uam_debug_read(m, cpu, r.addr, r.phys, buf, r.len, why, sizeof(why));
    if (!got) {
        uam_monitor_fail(c, "dump: %s", why);
    } else if (r.file) {
        FILE *f = fopen(r.file, "wb");
        bool ok = f && fwrite(buf, 1, got, f) == got;
        if (f && fclose(f) != 0)
            ok = false;
        if (!ok)
            uam_monitor_fail(c, "dump: cannot write %s", r.file);
        else
            uam_monitor_field(c, "written", "%llu", (unsigned long long)got);
    } else {
        hex_lines(c, r.addr, buf, got, 1, false);
    }
    if (got && got < r.len && !r.file)
        uam_monitor_field(c, "short", "%llu of %llu bytes: %s", (unsigned long long)got,
                          (unsigned long long)r.len, why);
    else if (got && got < r.len)
        uam_monitor_field(c, "short", "%s", why);
    free(buf);
}

/* ---- the bus, the history, the call chain -------------------------------------- */

static void map_to(void *ctx, FILE *out) { uam_bus_dump_map(&((uam_machine *)ctx)->bus, out); }

static void cmd_map(uam_monitor_call *c, void *ctx)
{
    (void)ctx;
    print_from(c, map_to, uam_monitor_machine(uam_monitor_of(c)), "  ", 0);
}

static void history_to(void *ctx, FILE *out) { uam_exec_history_print(ctx, out, ""); }

static void cmd_history(uam_monitor_call *c, void *ctx)
{
    (void)ctx;
    uam_machine *m = uam_monitor_machine(uam_monitor_of(c));
    uam_exec *x = uam_machine_exec(m, uam_monitor_cpu_index(c));
    uint64_t n = 0;
    if (uam_monitor_argc(c) > 1) {
        char *end;
        const char *t = uam_monitor_arg(c, 1);
        n = strtoull(t, &end, 0);
        if (end == t || *end || !n) {
            uam_monitor_fail(c, "history: N, how many of the last events");
            return;
        }
    }
    if (!x || !x->hist_cap) {
        uam_monitor_fail(c, "history: none is kept (start the run with --history N)");
        return;
    }
    print_from(c, history_to, x, "  ", (size_t)n);
}

typedef struct bt_args {
    uam_machine     *m;
    unsigned         cpu;
    uam_symbolize_fn sym;
    void            *sym_ctx;
} bt_args;

static void bt_to(void *ctx, FILE *out)
{
    bt_args *a = ctx;
    uam_machine_backtrace_print(a->m, a->cpu, out, "", a->sym, a->sym_ctx);
}

static void cmd_bt(uam_monitor_call *c, void *ctx)
{
    (void)ctx;
    uam_monitor *mon = uam_monitor_of(c);
    bt_args a = { uam_monitor_machine(mon), uam_monitor_cpu_index(c), NULL, NULL };
    a.sym = mon_symbolizer(mon, &a.sym_ctx);
    if (!uam_machine_frames(a.m, a.cpu)) {
        uam_monitor_fail(c, "bt: calls are not tracked (start the run with --backtrace)");
        return;
    }
    size_t before = uam_monitor_lines(c);
    print_from(c, bt_to, &a, NULL, 0);
    if (uam_monitor_lines(c) == before)
        uam_monitor_print(c, "(no calls)");
}

uam_status mon_add_inspect_commands(uam_monitor *mon)
{
    static const struct {
        const char *name, *usage, *help;
        unsigned    flags;
        uam_monitor_fn fn;
    } cmds[] = {
        { "regs", "", "the processor's registers, as the debugger view names them",
          UAM_MONITOR_STOPPED_ONLY, cmd_regs },
        { "reg", "NAME [VALUE]", "one register; with VALUE, set it first",
          UAM_MONITOR_STOPPED_ONLY, cmd_reg },
        { "x", "[p:]ADDR [LEN] [w1|w2|w4|w8]", "show memory (64 bytes; p: physical)",
          UAM_MONITOR_STOPPED_ONLY, cmd_x },
        { "write", "[p:]ADDR w1|w2|w4|w8 VALUE...", "write values to memory, in target order",
          UAM_MONITOR_STOPPED_ONLY, cmd_write },
        { "load", "[p:]ADDR FILE", "write a file's bytes to memory", UAM_MONITOR_STOPPED_ONLY,
          cmd_load },
        { "dump", "[p:]ADDR+LEN[=FILE]", "memory as --dump-mem: shown, or raw to FILE",
          UAM_MONITOR_STOPPED_ONLY, cmd_dump },
        { "map", "", "the bus: every region, its kind, priority and name", 0, cmd_map },
        { "history", "[N]", "the last N blocks, exceptions and interrupts (--history)",
          UAM_MONITOR_STOPPED_ONLY, cmd_history },
        { "bt", "", "the call chain (--backtrace)", UAM_MONITOR_STOPPED_ONLY, cmd_bt },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        uam_status st = uam_monitor_add_command(mon, cmds[i].name, cmds[i].usage, cmds[i].help,
                                                cmds[i].flags, cmds[i].fn, mon);
        if (st != UAM_OK)
            return st;
    }
    return UAM_OK;
}
