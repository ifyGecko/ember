/*
 * monitor_instr.c - The monitor's live instrumentation: breakpoints, guards and
 * watches, rules.
 *
 * What the monitor adds is its own: a breakpoint, a guard or a rule of the monitor that
 * stops the run stops it into the monitor (reason "break N", "guard N", "rule NAME"),
 * where "cont" goes on. Those uam-run was started with (--guard, --on) still end the run.
 *
 * Breakpoints are PC hooks; guards and watches are the machine's (uam/guard.h, STOP for
 * a guard, LOG for a watch); rules are a rule set of the monitor's (uam/rules.h,
 * "rules-monitor" in snapshots when one has a count or a delay).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uam/guard.h"
#include "uam/rules.h"
#include "monitor_int.h"

#define MAX_BREAKS 64u
#define MAX_GUARDS 64u
#define MAX_RULES  256u

typedef struct mon_break {
    bool     used;
    int      id;
    uint64_t pc;
    uint64_t hits;
} mon_break;

typedef struct mon_guard {
    bool     used;
    int      id;          /* the monitor's */
    int      gid;         /* the machine's (uam_machine_add_guard_text); 0 while stepped over */
    bool     watch;       /* LOG; else STOP */
    char     text[160];
    uint64_t hits;
} mon_guard;

struct mon_instr {
    uam_monitor *mon;
    uam_machine *m;
    mon_break    bp[MAX_BREAKS];
    int          next_bp;
    mon_guard    g[MAX_GUARDS];
    int          next_g;
    bool         observing;
    uam_rules   *rules;
    bool         deleted[MAX_RULES];
    uam_rule_symbol_fn rsym;
    void        *rsym_ctx;
    /* the run under way */
    uint64_t     start_icount;
    int          bp_hit, guard_hit;     /* ids; 0: none */
    int          last_guard;            /* the guard the machine last stopped at */
    int          over_guard;            /* stepped over: put back at over_at */
    uint64_t     over_at;
    char         why[96];
};

static mon_instr *I(uam_monitor *mon) { return *mon_instr_slot(mon); }

/* ---- breakpoints --------------------------------------------------------------- */

static uam_hook_action bp_hook(void *ctx, uam_cpu *cpu, uint64_t pc)
{
    mon_instr *in = ctx;
    (void)cpu;
    /* Where the run (re)starts does not count: "cont" from a breakpoint goes on. */
    if (uam_machine_icount(in->m) == in->start_icount)
        return UAM_HOOK_CONTINUE;
    const uint64_t a = pc & ~uam_isa_pc_state_bits(in->m->cpu.isa);
    for (unsigned i = 0; i < MAX_BREAKS; i++)
        if (in->bp[i].used && in->bp[i].pc == a) {
            in->bp[i].hits++;
            if (!in->bp_hit)
                in->bp_hit = in->bp[i].id;
            return UAM_HOOK_STOP;
        }
    return UAM_HOOK_CONTINUE;
}

static bool bp_hooks(mon_instr *in, uint64_t pc, bool add)
{
    const uint64_t sb = uam_isa_pc_state_bits(in->m->cpu.isa);
    if (!add) {
        uam_machine_remove_pc_hook(in->m, pc, bp_hook, in);
        if (sb)
            uam_machine_remove_pc_hook(in->m, pc | sb, bp_hook, in);
        return true;
    }
    if (uam_machine_add_pc_hook(in->m, pc, bp_hook, in) != UAM_OK)
        return false;
    if (sb && uam_machine_add_pc_hook(in->m, pc | sb, bp_hook, in) != UAM_OK) {
        uam_machine_remove_pc_hook(in->m, pc, bp_hook, in);
        return false;
    }
    return true;
}

static bool number(const char *s, uint64_t *v)
{
    char *end;
    if (!s || !*s || *s == '-')
        return false;
    *v = strtoull(s, &end, 0);
    return !*end;
}

static void cmd_break(uam_monitor_call *c, void *ctx)
{
    mon_instr *in = I(ctx);
    const char *sub = uam_monitor_arg(c, 1);
    if (!sub || !strcmp(sub, "list")) {
        for (unsigned i = 0; i < MAX_BREAKS; i++)
            if (in->bp[i].used)
                uam_monitor_print(c, "%d pc=0x%llx hits=%llu", in->bp[i].id,
                                  (unsigned long long)in->bp[i].pc,
                                  (unsigned long long)in->bp[i].hits);
        return;
    }
    if (!strcmp(sub, "add")) {
        uint64_t pc;
        if (uam_monitor_argc(c) != 3 || !number(uam_monitor_arg(c, 2), &pc)) {
            uam_monitor_fail(c, "break add PC");
            return;
        }
        pc &= ~uam_isa_pc_state_bits(in->m->cpu.isa);
        unsigned slot = MAX_BREAKS;
        for (unsigned i = 0; i < MAX_BREAKS; i++) {
            if (in->bp[i].used && in->bp[i].pc == pc) {
                uam_monitor_fail(c, "break: breakpoint %d is at 0x%llx already", in->bp[i].id,
                                 (unsigned long long)pc);
                return;
            }
            if (!in->bp[i].used && slot == MAX_BREAKS)
                slot = i;
        }
        if (slot == MAX_BREAKS) {
            uam_monitor_fail(c, "break: at most %u breakpoints", MAX_BREAKS);
            return;
        }
        if (!bp_hooks(in, pc, true)) {
            uam_monitor_fail(c, "break: cannot hook 0x%llx", (unsigned long long)pc);
            return;
        }
        in->bp[slot] = (mon_break){ .used = true, .id = ++in->next_bp, .pc = pc };
        uam_monitor_field(c, "id", "%d", in->bp[slot].id);
        return;
    }
    if (!strcmp(sub, "del")) {
        const char *which = uam_monitor_arg(c, 2);
        uint64_t id;
        bool all = which && !strcmp(which, "all");
        if (uam_monitor_argc(c) != 3 || (!all && !number(which, &id))) {
            uam_monitor_fail(c, "break del ID|all");
            return;
        }
        bool found = false;
        for (unsigned i = 0; i < MAX_BREAKS; i++)
            if (in->bp[i].used && (all || in->bp[i].id == (int)id)) {
                bp_hooks(in, in->bp[i].pc, false);
                in->bp[i].used = false;
                found = true;
            }
        if (!found && !all)
            uam_monitor_fail(c, "break: no breakpoint %s", which);
        return;
    }
    uam_monitor_fail(c, "break add PC | del ID|all | list");
}

/* ---- guards and watches -------------------------------------------------------- */

static mon_guard *guard_of_gid(mon_instr *in, int gid)
{
    for (unsigned i = 0; i < MAX_GUARDS; i++)
        if (in->g[i].used && in->g[i].gid == gid && gid > 0)
            return &in->g[i];
    return NULL;
}

static void observer(void *ctx, uam_machine *m, const uam_guard_hit *hit)
{
    mon_instr *in = ctx;
    (void)m;
    mon_guard *g = hit->kind == UAM_GUARD_ACCESS ? guard_of_gid(in, hit->id) : NULL;
    if (!g)
        return;
    g->hits++;
    char what[200], id[16], pc[24];
    uam_guard_hit_describe(hit, what, sizeof(what));
    snprintf(id, sizeof(id), "%d", g->id);
    snprintf(pc, sizeof(pc), "0x%llx", (unsigned long long)hit->pc);
    if (g->watch)
        mon_event(in->mon, "watch", "id", id, "pc", pc, "what", what, NULL);
    else if (!in->guard_hit)
        in->guard_hit = g->id;
}

static void cmd_guard_common(uam_monitor_call *c, mon_instr *in, bool watch)
{
    const char *word = watch ? "watch" : "guard";
    const char *sub = uam_monitor_arg(c, 1);
    if (!sub || !strcmp(sub, "list")) {
        for (unsigned i = 0; i < MAX_GUARDS; i++)
            if (in->g[i].used && in->g[i].watch == watch)
                uam_monitor_print(c, "%d %s hits=%llu", in->g[i].id, in->g[i].text,
                                  (unsigned long long)in->g[i].hits);
        return;
    }
    if (!strcmp(sub, "add")) {
        const char *text = uam_monitor_rest(c, 2);
        if (!text || strlen(text) >= sizeof(in->g[0].text)) {
            uam_monitor_fail(c, "%s add [p:]ADDR[-BEFORE][+AFTER][:r|w|rw]%s[,name=NAME]", word,
                             watch ? "" : "[,stop|,fault|,log]");
            return;
        }
        unsigned slot = MAX_GUARDS;
        for (unsigned i = 0; i < MAX_GUARDS && slot == MAX_GUARDS; i++)
            if (!in->g[i].used)
                slot = i;
        if (slot == MAX_GUARDS) {
            uam_monitor_fail(c, "%s: at most %u guards and watches", word, MAX_GUARDS);
            return;
        }
        if (!in->observing) {
            uam_machine_set_guard_observer(in->m, observer, in);
            in->observing = true;
        }
        char err[200] = "";
        int gid = uam_machine_add_guard_text(in->m, text, watch ? UAM_GUARD_LOG : UAM_GUARD_STOP,
                                             err, sizeof(err));
        if (gid <= 0) {
            uam_monitor_fail(c, "%s: %s", word, err);
            return;
        }
        mon_guard *g = &in->g[slot];
        *g = (mon_guard){ .used = true, .id = ++in->next_g, .gid = gid, .watch = watch };
        snprintf(g->text, sizeof(g->text), "%s", text);
        uam_monitor_field(c, "id", "%d", g->id);
        return;
    }
    if (!strcmp(sub, "del")) {
        const char *which = uam_monitor_arg(c, 2);
        uint64_t id;
        bool all = which && !strcmp(which, "all");
        if (uam_monitor_argc(c) != 3 || (!all && !number(which, &id))) {
            uam_monitor_fail(c, "%s del ID|all", word);
            return;
        }
        bool found = false;
        for (unsigned i = 0; i < MAX_GUARDS; i++) {
            mon_guard *g = &in->g[i];
            if (!g->used || g->watch != watch || (!all && g->id != (int)id))
                continue;
            if (g->gid > 0)
                uam_machine_remove_guard(in->m, g->gid);
            if (in->over_guard == g->id)
                in->over_guard = 0;
            if (in->last_guard == g->id)
                in->last_guard = 0;
            g->used = false;
            found = true;
        }
        if (!found && !all)
            uam_monitor_fail(c, "%s: no %s %s", word, word, which);
        return;
    }
    uam_monitor_fail(c, "%s add TEXT | del ID|all | list", word);
}

static void cmd_guard(uam_monitor_call *c, void *ctx) { cmd_guard_common(c, I(ctx), false); }
static void cmd_watch(uam_monitor_call *c, void *ctx) { cmd_guard_common(c, I(ctx), true); }

/* ---- rules ----------------------------------------------------------------------- */

static bool have_rules(uam_monitor_call *c, mon_instr *in)
{
    if (!in->rules) {
        in->rules = uam_rules_create(in->m, "monitor");
        if (!in->rules) {
            uam_monitor_fail(c, "rule: out of memory");
            return false;
        }
        if (in->rsym)
            uam_rules_set_symbols(in->rules, in->rsym, in->rsym_ctx);
    }
    return true;
}

static bool rule_id(uam_monitor_call *c, mon_instr *in, size_t *i)
{
    uint64_t id;
    if (uam_monitor_argc(c) != 3 || !number(uam_monitor_arg(c, 2), &id) || !in->rules
        || id >= uam_rules_n(in->rules) || in->deleted[id]) {
        uam_monitor_fail(c, "rule %s: no rule %s (rule list)", uam_monitor_arg(c, 1),
                         uam_monitor_arg(c, 2) ? uam_monitor_arg(c, 2) : "");
        return false;
    }
    *i = (size_t)id;
    return true;
}

static void cmd_rule(uam_monitor_call *c, void *ctx)
{
    mon_instr *in = I(ctx);
    const char *sub = uam_monitor_arg(c, 1);
    char err[768] = "";
    if (!sub || !strcmp(sub, "list")) {
        uam_rule_info ri;
        for (size_t i = 0; in->rules && uam_rules_get(in->rules, i, &ri); i++)
            if (!in->deleted[i])
                uam_monitor_print(c, "%zu %s %s arrivals=%llu fired=%llu failed=%llu", i, ri.name,
                                  uam_rules_enabled(in->rules, i) ? "on" : "off",
                                  (unsigned long long)ri.arrivals, (unsigned long long)ri.fired,
                                  (unsigned long long)ri.failed);
        return;
    }
    if (!strcmp(sub, "add") || !strcmp(sub, "load")) {
        const char *text = uam_monitor_rest(c, 2);
        if (!text) {
            uam_monitor_fail(c, "rule add TEXT (as --on) | rule load FILE (as --on-file)");
            return;
        }
        if (!have_rules(c, in))
            return;
        size_t before = uam_rules_n(in->rules);
        if (!strcmp(sub, "add")) {
            if (before >= MAX_RULES) {
                uam_monitor_fail(c, "rule: at most %u rules", MAX_RULES);
                return;
            }
            if (uam_rules_add(in->rules, text, err, sizeof(err)) < 0) {
                uam_monitor_fail(c, "rule: %s", err);
                return;
            }
        } else if (uam_rules_add_file(in->rules, uam_monitor_arg(c, 2), err, sizeof(err)) != UAM_OK) {
            uam_monitor_fail(c, "rule: %s", err);
            /* the lines before the bad one were added: they stay, and are listed */
        }
        size_t after = uam_rules_n(in->rules);
        if (after > MAX_RULES) {   /* a file took the set past the limit: the rest go */
            for (size_t i = MAX_RULES; i < after; i++)
                uam_rules_enable(in->rules, i, false, NULL, 0);
            uam_monitor_fail(c, "rule: at most %u rules; rules from %u on are off", MAX_RULES,
                             MAX_RULES);
            after = MAX_RULES;
        }
        for (size_t i = before; i < after; i++)
            uam_monitor_field(c, "id", "%zu", i);
        return;
    }
    size_t i;
    if (!strcmp(sub, "on") || !strcmp(sub, "off") || !strcmp(sub, "del")) {
        if (!rule_id(c, in, &i))
            return;
        if (uam_rules_enable(in->rules, i, !strcmp(sub, "on"), err, sizeof(err)) != UAM_OK) {
            uam_monitor_fail(c, "rule: %s", err);
            return;
        }
        if (!strcmp(sub, "del"))
            in->deleted[i] = true;   /* it keeps its place: ids do not move */
        return;
    }
    uam_monitor_fail(c, "rule add TEXT | load FILE | list | on ID | off ID | del ID");
}

/* ---- the run -------------------------------------------------------------------- */

void mon_instr_before_run(uam_monitor *mon)
{
    mon_instr *in = I(mon);
    in->start_icount = uam_machine_icount(in->m);
    in->bp_hit = in->guard_hit = 0;
    if (in->rules)
        uam_rules_clear_stop(in->rules);
    /* Stopped at a guard: the access did not happen, and going on makes it again. Take
     * the guard away for one instruction. */
    if (in->last_guard && !in->over_guard) {
        for (unsigned i = 0; i < MAX_GUARDS; i++) {
            mon_guard *g = &in->g[i];
            if (g->used && g->id == in->last_guard && g->gid > 0) {
                uam_machine_remove_guard(in->m, g->gid);
                g->gid = 0;
                in->over_guard = g->id;
                in->over_at = in->start_icount + 1;
            }
        }
    }
    in->last_guard = 0;
}

uint64_t mon_instr_limit(uam_monitor *mon, uint64_t limit)
{
    mon_instr *in = I(mon);
    return in->over_guard && in->over_at < limit ? in->over_at : limit;
}

/* Put a stepped-over guard back. */
static void guard_back(mon_instr *in)
{
    if (!in->over_guard)
        return;
    for (unsigned i = 0; i < MAX_GUARDS; i++) {
        mon_guard *g = &in->g[i];
        if (!g->used || g->id != in->over_guard)
            continue;
        char err[200];
        g->gid = uam_machine_add_guard_text(in->m, g->text, g->watch ? UAM_GUARD_LOG
                                                                       : UAM_GUARD_STOP,
                                            err, sizeof(err));
        if (g->gid <= 0) {
            g->gid = 0;
            uam_log(UAM_LOG_WARN, "monitor", "guard %d could not be put back: %s", g->id, err);
        }
    }
    in->over_guard = 0;
}

const char *mon_instr_claim(uam_monitor *mon, uam_stop_reason why)
{
    mon_instr *in = I(mon);
    const uint64_t icount = uam_machine_icount(in->m);
    bool stepped_over = false;
    if (in->over_guard && icount >= in->over_at) {
        guard_back(in);
        stepped_over = true;
    }
    if (why == UAM_STOP_HOOK && in->bp_hit) {
        snprintf(in->why, sizeof(in->why), "break %d", in->bp_hit);
        return in->why;
    }
    uam_rule_stop rs;
    if (why == UAM_STOP_HOOK && in->rules && uam_rules_last_stop(in->rules, &rs)) {
        snprintf(in->why, sizeof(in->why), "rule %s", rs.name);
        return in->why;
    }
    if (why == UAM_STOP_GUARD && in->guard_hit) {
        in->last_guard = in->guard_hit;
        snprintf(in->why, sizeof(in->why), "guard %d", in->guard_hit);
        return in->why;
    }
    if (why == UAM_STOP_INSN_LIMIT && stepped_over)
        return "";   /* the step over a guard: go on */
    return NULL;
}

/* ---- setup ------------------------------------------------------------------------ */

void mon_instr_set_rule_symbols(uam_monitor *mon, uam_rule_symbol_fn fn, void *ctx)
{
    mon_instr *in = I(mon);
    in->rsym = fn;
    in->rsym_ctx = ctx;
    if (in->rules)
        uam_rules_set_symbols(in->rules, fn, ctx);
}

uam_status mon_add_instr_commands(uam_monitor *mon)
{
    mon_instr *in = calloc(1, sizeof(*in));
    if (!in)
        return UAM_ERR_NOMEM;
    in->mon = mon;
    in->m = uam_monitor_machine(mon);
    *mon_instr_slot(mon) = in;
    static const struct {
        const char *name, *usage, *help;
        uam_monitor_fn fn;
    } cmds[] = {
        { "break", "add PC | del ID|all | list",
          "breakpoints: the run stops into the monitor when a processor reaches PC", cmd_break },
        { "guard", "add SPEC | del ID|all | list",
          "guards as --guard takes them: an access stops the run into the monitor before it "
          "happens", cmd_guard },
        { "watch", "add SPEC | del ID|all | list",
          "watches as --watch takes them: each access is an event (! watch)", cmd_watch },
        { "rule", "add TEXT | load FILE | list | on ID | off ID | del ID",
          "rules as --on takes them; a rule's stop stops the run into the monitor", cmd_rule },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        uam_status st = uam_monitor_add_command(mon, cmds[i].name, cmds[i].usage, cmds[i].help,
                                                UAM_MONITOR_STOPPED_ONLY, cmds[i].fn, mon);
        if (st != UAM_OK)
            return st;
    }
    return UAM_OK;
}

void mon_instr_free(uam_monitor *mon)
{
    mon_instr *in = I(mon);
    if (!in)
        return;
    for (unsigned i = 0; i < MAX_BREAKS; i++)
        if (in->bp[i].used)
            bp_hooks(in, in->bp[i].pc, false);
    for (unsigned i = 0; i < MAX_GUARDS; i++)
        if (in->g[i].used && in->g[i].gid > 0)
            uam_machine_remove_guard(in->m, in->g[i].gid);
    if (in->observing)
        uam_machine_set_guard_observer(in->m, NULL, NULL);
    if (in->rules) {
        uam_rules_detach(in->rules);
        uam_rules_free(in->rules);
    }
    free(in);
    *mon_instr_slot(mon) = NULL;
}
