/*
 * trace.c - The runner's bring-up traces (trace.h).
 */
#include <stdlib.h>
#include <string.h>

#include "uam/exec.h"
#include "uam/isa.h"

#include "trace.h"

unsigned rtrace_flag(const char *tok)
{
    if (!strcmp(tok, "mmio"))       return RT_MMIO;
    if (!strcmp(tok, "sysreg"))     return RT_SYSREG;
    if (!strcmp(tok, "sysreg-all")) return RT_SYSREG_ALL;
    if (!strcmp(tok, "irq"))        return RT_IRQ;
    if (!strcmp(tok, "console"))    return RT_CONSOLE;
    return 0;
}

static void put_origin(FILE *out, bool ok, const uam_origin *o)
{
    if (!ok)
        return;
    fprintf(out, " pc=0x%llx icount=%llu", (unsigned long long)o->pc, (unsigned long long)o->icount);
    if (o->cpu)
        fprintf(out, " cpu=%u", o->cpu);
}

/* Is an event made now inside the --trace-after window? */
static bool in_window(const rtrace *t, bool *ok, uam_origin *o)
{
    *ok = uam_origin_get(o);
    return *ok ? o->icount >= t->after : t->after == 0;
}

/* ---- device accesses --------------------------------------------------------- */

static void on_dev(void *ctx, const uam_region *r, uint64_t addr, unsigned size, bool is_write,
                   uint64_t val, uam_status st)
{
    rtrace *t = ctx;
    if (t->mmio_hi && (addr < t->mmio_lo || addr >= t->mmio_hi))
        return;
    uam_origin o;
    bool ok;
    if (!in_window(t, &ok, &o))
        return;
    if (r)
        fprintf(t->out, "mmio: %c %u 0x%llx %s+0x%llx = 0x%0*llx", is_write ? 'w' : 'r', size,
                (unsigned long long)addr, r->name ? r->name : "?",
                (unsigned long long)(addr - r->base), (int)size * 2, (unsigned long long)val);
    else
        fprintf(t->out, "mmio: %c %u 0x%llx unmapped = 0x%0*llx", is_write ? 'w' : 'r', size,
                (unsigned long long)addr, (int)size * 2, (unsigned long long)val);
    if (st != UAM_OK)
        fprintf(t->out, " (%s)", uam_status_str(st));
    put_origin(t->out, ok, &o);
    fputc('\n', t->out);
}

/* ---- system registers -------------------------------------------------------- */

static rt_sysreg *sr_get(rtrace *t, uint32_t id)
{
    uint32_t h = (id * 2654435761u) & (RT_SYSREG_SLOTS - 1);
    for (uint32_t k = 0; k < RT_SYSREG_SLOTS; k++) {
        rt_sysreg *e = &t->sr[(h + k) & (RT_SYSREG_SLOTS - 1)];
        if (e->used && e->id == id)
            return e;
        if (!e->used) {
            if (t->n_sr * 4 >= RT_SYSREG_SLOTS * 3)
                return NULL;
            *e = (rt_sysreg){ .used = true, .id = id };
            t->n_sr++;
            return e;
        }
    }
    return NULL;
}

static void on_sysreg(void *ctx, uam_cpu *cpu, uint32_t id, bool write, uint64_t val, uam_status st)
{
    rtrace *t = ctx;
    uam_origin o;
    bool ok;
    /* Only accesses in the window count: "first" is the first one from --trace-after on. */
    if (!in_window(t, &ok, &o))
        return;
    rt_sysreg *e = sr_get(t, id);
    bool first = false;
    if (e) {
        first = write ? e->writes == 0 : e->reads == 0;
        if (!e->reads && !e->writes)
            e->first_pc = cpu->insn_pc;
        if (write) e->writes++; else e->reads++;
        if (st != UAM_OK) e->fails++;
    } else if (!t->sr_dropped++) {
        fprintf(t->out, "sysreg: more than %u system registers accessed: the first accesses of "
                "the others are not reported (only those that fault)\n", t->n_sr);
    }
    if (!(t->flags & RT_SYSREG_ALL) && !first && st == UAM_OK)
        return;
    fprintf(t->out, "sysreg: %s%s 0x%x %s 0x%llx", (t->flags & RT_SYSREG_ALL) || !first ? "" : "first ",
            write ? "write" : "read", id, write ? "<-" : "=", (unsigned long long)val);
    if (st != UAM_OK)
        fprintf(t->out, " (%s: the guest takes a system register exception)", uam_status_str(st));
    put_origin(t->out, ok, &o);
    fputc('\n', t->out);
}

/* ---- interrupt inputs --------------------------------------------------------- */

static void on_irq_input(void *ctx, uam_cpu *cpu, unsigned line, int level)
{
    rtrace *t = ctx;
    uam_origin o;
    bool ok;
    if (!in_window(t, &ok, &o))
        return;
    rt_irq_line *l = NULL;
    for (uint32_t i = 0; i < t->n_irq; i++)
        if (t->irq[i].cpu == cpu->hartid && t->irq[i].line == line)
            l = &t->irq[i];
    if (!l && t->n_irq < UAM_ARRAY_LEN(t->irq)) {
        l = &t->irq[t->n_irq++];
        *l = (rt_irq_line){ .cpu = cpu->hartid, .line = line };
    }
    if (l) {
        if (level) l->rises++; else l->falls++;
        l->level = level;
    } else {
        t->irq_dropped++;
    }
    fprintf(t->out, "irq-in: cpu%u line %u = %d", cpu->hartid, line, level);
    if (ok)
        fprintf(t->out, " icount=%llu (cpu%u at pc=0x%llx)", (unsigned long long)o.icount, o.cpu,
                (unsigned long long)o.pc);
    fputc('\n', t->out);
}

/* ---- console ------------------------------------------------------------------ */

static void console_flush(rtrace *t)
{
    if (!t->len && !t->line_origin_ok)
        return;
    if (!t->line_origin_ok || t->line_origin.icount >= t->after) {
        fputs("console:", t->out);
        if (t->line_origin_ok)
            fprintf(t->out, " icount=%llu pc=0x%llx", (unsigned long long)t->line_origin.icount,
                    (unsigned long long)t->line_origin.pc);
        fputs(" \"", t->out);
        for (size_t i = 0; i < t->len; i++) {
            unsigned char c = (unsigned char)t->line[i];
            if (c == '"' || c == '\\')
                fprintf(t->out, "\\%c", c);
            else if (c >= 0x20 && c < 0x7f)
                fputc(c, t->out);
            else
                fprintf(t->out, "\\x%02x", c);
        }
        fputs("\"\n", t->out);
    }
    t->len = 0;
    t->line_origin_ok = false;
}

static void on_console(void *ctx, const uint8_t *buf, size_t len)
{
    rtrace *t = ctx;
    for (size_t i = 0; i < len; i++) {
        if (!t->len && !t->line_origin_ok)
            t->line_origin_ok = uam_origin_get(&t->line_origin);
        if (buf[i] == '\n') {
            console_flush(t);
            continue;
        }
        if (buf[i] == '\r')
            continue;
        t->line[t->len++] = (char)buf[i];
        if (t->len == sizeof(t->line))
            console_flush(t);
    }
}

/* ---- life cycle ---------------------------------------------------------------- */

void rtrace_attach(rtrace *t, uam_machine *m, uam_chardev *console, FILE *out)
{
    t->m = m;
    t->out = out ? out : stderr;
    t->console = console;
    if (t->flags & RT_MMIO)
        uam_bus_set_dev_observer(&m->bus, on_dev, t);
    if (t->flags & (RT_SYSREG | RT_SYSREG_ALL | RT_IRQ)) {
        t->obs = (uam_cpu_observer){
            .sysreg = (t->flags & (RT_SYSREG | RT_SYSREG_ALL)) ? on_sysreg : NULL,
            .irq_input = (t->flags & RT_IRQ) ? on_irq_input : NULL,
            .ctx = t,
        };
        uam_machine_set_cpu_observer(m, &t->obs);
    }
    if ((t->flags & RT_CONSOLE) && console)
        uam_chardev_set_tap(console, on_console, t);
}

static int sr_cmp(const void *a, const void *b)
{
    const rt_sysreg *x = a, *y = b;
    return x->id < y->id ? -1 : x->id > y->id;
}

void rtrace_finish(rtrace *t)
{
    if (!t->m)
        return;
    uam_machine *m = t->m;
    if (t->flags & RT_CONSOLE)
        console_flush(t);
    if ((t->flags & (RT_SYSREG | RT_SYSREG_ALL)) && t->n_sr) {
        rt_sysreg *list = malloc(sizeof(*list) * t->n_sr);
        uint32_t n = 0;
        for (uint32_t i = 0; list && i < RT_SYSREG_SLOTS; i++)
            if (t->sr[i].used)
                list[n++] = t->sr[i];
        if (list) {
            qsort(list, n, sizeof(*list), sr_cmp);
            fprintf(t->out, "sysreg: %u system registers accessed%s (ids as uam/isa/<arch>.h numbers them):\n",
                    n, t->sr_dropped ? " and more not tracked" : "");
            for (uint32_t i = 0; i < n; i++)
                fprintf(t->out, "sysreg:   0x%x: %llu reads, %llu writes%s, first at pc=0x%llx\n",
                        list[i].id, (unsigned long long)list[i].reads,
                        (unsigned long long)list[i].writes, list[i].fails ? " (some faulted)" : "",
                        (unsigned long long)list[i].first_pc);
        }
        free(list);
    }
    if (t->flags & RT_IRQ) {
        for (uint32_t i = 0; i < t->n_irq; i++)
            fprintf(t->out, "irq-in: cpu%u line %u: %llu rises, %llu falls, level %d at the stop\n",
                    t->irq[i].cpu, t->irq[i].line, (unsigned long long)t->irq[i].rises,
                    (unsigned long long)t->irq[i].falls, t->irq[i].level);
        if (t->irq_dropped)
            fprintf(t->out, "irq-in: %llu changes on lines beyond the first %u not counted\n",
                    (unsigned long long)t->irq_dropped, (unsigned)UAM_ARRAY_LEN(t->irq));
        if (!t->n_irq)
            fprintf(t->out, "irq-in: no interrupt input line of a processor changed level\n");
        for (unsigned c = 0; c < uam_machine_ncpus(m); c++) {
            uam_cpu *cpu = uam_machine_cpu(m, c);
            if (cpu->isa->irq_wakeup && cpu->isa->irq_wakeup(cpu))
                fprintf(t->out, "irq-in: at the stop cpu%u has an interrupt pending and enabled%s\n", c,
                        cpu->waiting ? " (waiting: it wakes)" : " (it is taken at the next instruction "
                        "boundary unless interrupts are globally off)");
        }
    }
    fflush(t->out);
}

void rtrace_restart(rtrace *t)
{
    if (!t->m)
        return;
    if (t->flags & RT_CONSOLE)
        console_flush(t);
    memset(t->sr, 0, sizeof(t->sr));
    t->n_sr = 0;
    t->sr_dropped = 0;
    t->n_irq = 0;
    t->irq_dropped = 0;
}

void rtrace_detach(rtrace *t)
{
    if (!t->m)
        return;
    if (t->flags & RT_MMIO)
        uam_bus_set_dev_observer(&t->m->bus, NULL, NULL);
    if (t->flags & (RT_SYSREG | RT_SYSREG_ALL | RT_IRQ))
        uam_machine_set_cpu_observer(t->m, NULL);
    if ((t->flags & RT_CONSOLE) && t->console)
        uam_chardev_set_tap(t->console, NULL, NULL);
    t->m = NULL;
}
