/*
 * monitor_state.c - The monitor's commands on the machine as a whole: snapshots, reset,
 * devices, heaps, interrupt lines, a processor's whole state.
 *
 * Snapshots (uam/snapshot.h) are kept in memory by name, and can be written to and read
 * from files. Restoring one (or resetting) also brings back a run that had ended under
 * --monitor-hold: the machine is stopped there, and "cont" goes on.
 */
#define _POSIX_C_SOURCE 200809L   /* open_memstream, strdup */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "uam/heap.h"
#include "uam/snapshot.h"
#include "monitor_int.h"

#define MAX_SNAPS 32u
#define MAX_IRQ   64u

typedef struct mon_snap {
    char         *name;
    uam_snapshot *s;
} mon_snap;

struct mon_state {
    uam_monitor *mon;
    uam_machine *m;
    mon_snap     snap[MAX_SNAPS];
    /* "irq LINE LEVEL": what the monitor drove, per processor */
    struct { unsigned cpu, line; int level; } irq[MAX_IRQ];
    unsigned     n_irq;
};

static mon_state *S(uam_monitor *mon) { return *mon_state_slot(mon); }

/* ---- snapshots ------------------------------------------------------------------- */

static mon_snap *find_snap(mon_state *st, const char *name)
{
    for (unsigned i = 0; i < MAX_SNAPS; i++)
        if (st->snap[i].name && !strcmp(st->snap[i].name, name))
            return &st->snap[i];
    return NULL;
}

static mon_snap *new_snap(uam_monitor_call *c, mon_state *st, const char *name)
{
    mon_snap *s = find_snap(st, name);
    if (s) {   /* the name is taken again */
        uam_snapshot_free(s->s);
        s->s = NULL;
        return s;
    }
    for (unsigned i = 0; i < MAX_SNAPS; i++)
        if (!st->snap[i].name) {
            if (!(st->snap[i].name = strdup(name))) {
                uam_monitor_fail(c, "snap: out of memory");
                return NULL;
            }
            return &st->snap[i];
        }
    uam_monitor_fail(c, "snap: at most %u snapshots (snap drop NAME)", MAX_SNAPS);
    return NULL;
}

static void drop_snap(mon_snap *s)
{
    uam_snapshot_free(s->s);
    free(s->name);
    *s = (mon_snap){ 0 };
}

static void snap_fields(uam_monitor_call *c, const char *name, const uam_snapshot *s)
{
    uam_snapshot_info in;
    uam_snapshot_get_info(s, &in);
    uam_monitor_field(c, "snapshot", "%s", name);
    uam_monitor_field(c, "pc", "0x%llx", (unsigned long long)in.pc);
    uam_monitor_field(c, "icount", "%llu", (unsigned long long)in.icount);
    uam_monitor_field(c, "time", "%llu", (unsigned long long)in.now);
}

static bool good_name(uam_monitor_call *c, const char *name)
{
    if (!name || !*name || strlen(name) > 64 || strchr(name, '/')) {
        uam_monitor_fail(c, "snap: NAME is a word (no '/'), up to 64 characters");
        return false;
    }
    return true;
}

static void cmd_snap(uam_monitor_call *c, void *ctx)
{
    mon_state *st = S(ctx);
    const char *sub = uam_monitor_arg(c, 1), *name = uam_monitor_arg(c, 2),
               *file = uam_monitor_arg(c, 3);
    int argc = uam_monitor_argc(c);
    if (!sub || !strcmp(sub, "list")) {
        for (unsigned i = 0; i < MAX_SNAPS; i++)
            if (st->snap[i].name) {
                uam_snapshot_info in;
                uam_snapshot_get_info(st->snap[i].s, &in);
                uam_monitor_print(c, "%s pc=0x%llx icount=%llu time=%llu pages=%llu",
                                  st->snap[i].name, (unsigned long long)in.pc,
                                  (unsigned long long)in.icount, (unsigned long long)in.now,
                                  (unsigned long long)in.mem_pages);
            }
        return;
    }
    if (!strcmp(sub, "save") && (argc == 3 || argc == 4)) {
        if (!good_name(c, name))
            return;
        uam_snapshot *s;
        if (uam_snapshot_take(st->m, &s) != UAM_OK) {
            uam_monitor_fail(c, "snap: the machine cannot be captured (uam-run's log says "
                             "why: a device without save/restore, say)");
            return;
        }
        if (file && uam_snapshot_save(s, file) != UAM_OK) {
            uam_snapshot_free(s);
            uam_monitor_fail(c, "snap: cannot write %s", file);
            return;
        }
        mon_snap *slot = new_snap(c, st, name);
        if (!slot) {
            uam_snapshot_free(s);
            return;
        }
        slot->s = s;
        snap_fields(c, name, s);
        if (file)
            uam_monitor_field(c, "file", "%s", file);
        return;
    }
    if (!strcmp(sub, "load") && argc == 3) {
        mon_snap *slot = find_snap(st, name);
        uam_snapshot *from_file = NULL;
        struct stat sb;
        if (!slot) {
            if (stat(name, &sb) != 0) {
                uam_monitor_fail(c, "snap: no snapshot '%s', and no such file", name);
                return;
            }
            if (uam_snapshot_load(name, &from_file) != UAM_OK) {
                uam_monitor_fail(c, "snap: %s is not a snapshot uam-run can read", name);
                return;
            }
        }
        const uam_snapshot *s = slot ? slot->s : from_file;
        if (uam_snapshot_restore(st->m, s) != UAM_OK) {
            uam_snapshot_free(from_file);
            uam_monitor_fail(c, "snap: '%s' does not fit the machine as it is now (uam-run's "
                             "log says why: a device, or a rule with a count or a delay, "
                             "added since it was taken?)", name);
            return;
        }
        snap_fields(c, name, s);
        uam_snapshot_free(from_file);
        mon_revive(st->mon, "snap");
        return;
    }
    if (!strcmp(sub, "write") && argc == 4) {
        mon_snap *slot = find_snap(st, name);
        if (!slot) {
            uam_monitor_fail(c, "snap: no snapshot '%s'", name);
            return;
        }
        if (uam_snapshot_save(slot->s, file) != UAM_OK) {
            uam_monitor_fail(c, "snap: cannot write %s", file);
            return;
        }
        uam_monitor_field(c, "file", "%s", file);
        return;
    }
    if (!strcmp(sub, "read") && argc == 4) {
        if (!good_name(c, name))
            return;
        uam_snapshot *s;
        if (uam_snapshot_load(file, &s) != UAM_OK) {
            uam_monitor_fail(c, "snap: %s is not a snapshot uam-run can read", file);
            return;
        }
        mon_snap *slot = new_snap(c, st, name);
        if (!slot) {
            uam_snapshot_free(s);
            return;
        }
        slot->s = s;
        snap_fields(c, name, s);
        return;
    }
    if (!strcmp(sub, "drop") && argc == 3) {
        mon_snap *slot = find_snap(st, name);
        if (!slot) {
            uam_monitor_fail(c, "snap: no snapshot '%s'", name);
            return;
        }
        drop_snap(slot);
        return;
    }
    uam_monitor_fail(c, "snap save NAME [FILE] | load NAME|FILE | list | write NAME FILE | "
                     "read NAME FILE | drop NAME");
}

/* ---- reset ------------------------------------------------------------------------- */

static void cmd_reset(uam_monitor_call *c, void *ctx)
{
    mon_state *st = S(ctx);
    uam_machine *m = st->m;
    const char *kind = uam_monitor_arg(c, 1);
    bool cpu_only = kind && !strcmp(kind, "cpu");
    if (kind && !cpu_only && strcmp(kind, "system")) {
        uam_monitor_fail(c, "reset [system|cpu]");
        return;
    }
    /* As a reset the guest asks for: the board's hook first, then the reset. */
    if (m->on_reset)
        m->on_reset(m, cpu_only ? UAM_RESET_CPU : UAM_RESET_SYSTEM, m->on_reset_ctx);
    if (!cpu_only) {
        uam_machine_reset(m, m->reset_pc);
    } else {
        for (unsigned i = 0; i < uam_machine_ncpus(m); i++) {
            uam_cpu *cpu = uam_machine_cpu(m, i);
            cpu->isa->reset(cpu, m->reset_pc);
        }
    }
    uam_monitor_field(c, "pc", "0x%llx", (unsigned long long)m->cpu.pc);
    mon_revive(st->mon, "reset");
}

/* ---- devices ------------------------------------------------------------------------ */

static void cmd_devices(uam_monitor_call *c, void *ctx)
{
    mon_state *st = S(ctx);
    for (const uam_device *d = st->m->devices; d; d = d->next) {
        unsigned regions = 0;
        for (uint32_t i = 0; i < st->m->bus.n_regions; i++)
            regions += st->m->bus.regions[i]->dev == d;
        uam_monitor_print(c, "%s %s regions=%u%s%s", d->name ? d->name : "?",
                          d->ops->type ? d->ops->type : "?", regions,
                          d->ops->peek ? " peek" : "", d->ops->describe ? " describe" : "");
    }
}

typedef struct line_ctx { uam_monitor_call *c; } line_ctx;

static void put_line(void *ctx, const char *text)
{
    uam_monitor_print(((line_ctx *)ctx)->c, "%s", text);
}

static void cmd_dev(uam_monitor_call *c, void *ctx)
{
    mon_state *st = S(ctx);
    const char *name = uam_monitor_arg(c, 1);
    if (uam_monitor_argc(c) != 2) {
        uam_monitor_fail(c, "dev NAME (devices lists them)");
        return;
    }
    uam_device *d = uam_machine_find_device(st->m, name);
    if (!d) {
        uam_monitor_fail(c, "dev: no device '%s' (devices lists them)", name);
        return;
    }
    uam_monitor_field(c, "type", "%s", d->ops->type ? d->ops->type : "?");
    const uam_bus *bus = &st->m->bus;
    for (uint32_t i = 0; i < bus->n_regions; i++) {
        const uam_region *r = bus->regions[i];
        if (r->dev == d)
            uam_monitor_print(c, "region %s 0x%llx+0x%llx prio %d%s", r->name ? r->name : "?",
                              (unsigned long long)r->base, (unsigned long long)r->size, r->prio,
                              r->disabled ? " (disabled)" : "");
    }
    if (d->ops->describe) {
        line_ctx lc = { c };
        d->ops->describe(d, put_line, &lc);
        return;
    }
    if (!d->ops->peek) {
        uam_monitor_print(c, "(the device can neither describe itself nor be read without "
                          "side effects)");
        return;
    }
    /* No description: its registers as it shows them without side effects, in the first
     * region it has (the first 64 of them). */
    for (uint32_t i = 0; i < bus->n_regions; i++) {
        const uam_region *r = bus->regions[i];
        if (r->dev != d)
            continue;
        unsigned w = d->ops->impl_min ? d->ops->impl_min : 4;
        if (w > 8)
            w = 8;
        uint64_t n = r->size / w < 64 ? r->size / w : 64;
        for (uint64_t k = 0; k < n; k++) {
            uint64_t v;
            if (d->ops->peek(d, k * w, w, &v) == UAM_OK)
                uam_monitor_print(c, "+0x%03llx 0x%0*llx", (unsigned long long)(k * w),
                                  (int)(2 * w), (unsigned long long)v);
            else
                uam_monitor_print(c, "+0x%03llx --", (unsigned long long)(k * w));
        }
        break;
    }
}

/* ---- heaps --------------------------------------------------------------------------- */

static void cmd_heap(uam_monitor_call *c, void *ctx)
{
    mon_state *st = S(ctx);
    uam_heap *hs[16];
    size_t n = uam_machine_heaps(st->m, hs, 16);
    if (n > 16)
        n = 16;
    if (uam_monitor_argc(c) == 2) {
        char *end;
        const char *t = uam_monitor_arg(c, 1);
        uint64_t addr = strtoull(t, &end, 0);
        if (end == t || *end) {
            uam_monitor_fail(c, "heap [ADDR]");
            return;
        }
        for (size_t i = 0; i < n; i++) {
            uam_heap_info in;
            if (uam_heap_block(hs[i], addr, &in)) {
                uam_monitor_field(c, "heap", "%s", uam_heap_name(hs[i]));
                uam_monitor_field(c, "block", "0x%llx+0x%llx", (unsigned long long)in.addr,
                                  (unsigned long long)in.size);
                uam_monitor_field(c, "live", "%s", in.live ? "yes" : "no (freed, in quarantine)");
                uam_monitor_field(c, "allocated-at", "0x%llx", (unsigned long long)in.alloc_site);
                if (!in.live)
                    uam_monitor_field(c, "freed-at", "0x%llx", (unsigned long long)in.free_site);
                return;
            }
        }
        uam_monitor_fail(c, "heap: 0x%llx is in no block of %s", (unsigned long long)addr,
                         n ? "the machine's heaps" : "any heap (the board has none)");
        return;
    }
    if (!n)
        uam_monitor_print(c, "(the board has no guarded heap)");
    for (size_t i = 0; i < n; i++) {
        uam_heap_stats s;
        uint64_t base, size;
        uam_heap_get_stats(hs[i], &s);
        uam_heap_get_arena(hs[i], &base, &size);
        uam_monitor_print(c, "%s 0x%llx+0x%llx allocs=%llu frees=%llu failed=%llu live=%llu/%llu "
                          "peak=%llu quarantined=%llu/%llu hits=%llu", uam_heap_name(hs[i]),
                          (unsigned long long)base, (unsigned long long)size,
                          (unsigned long long)s.allocs, (unsigned long long)s.frees,
                          (unsigned long long)s.failed, (unsigned long long)s.live_blocks,
                          (unsigned long long)s.live_bytes, (unsigned long long)s.peak_bytes,
                          (unsigned long long)s.quarantined_blocks,
                          (unsigned long long)s.quarantined_bytes, (unsigned long long)s.hits);
    }
}

/* ---- interrupt lines ------------------------------------------------------------------ */

static void cmd_irq(uam_monitor_call *c, void *ctx)
{
    mon_state *st = S(ctx);
    unsigned cpu = uam_monitor_cpu_index(c);
    if (uam_monitor_argc(c) == 1) {
        for (unsigned i = 0; i < st->n_irq; i++)
            uam_monitor_print(c, "cpu %u line %u level %d", st->irq[i].cpu, st->irq[i].line,
                              st->irq[i].level);
        return;
    }
    char *end;
    const char *lt = uam_monitor_arg(c, 1), *vt = uam_monitor_arg(c, 2);
    unsigned long line = strtoul(lt, &end, 0);
    if (uam_monitor_argc(c) != 3 || end == lt || *end || line > 1023 || !vt
        || (strcmp(vt, "0") && strcmp(vt, "1"))) {
        uam_monitor_fail(c, "irq [LINE 0|1]");
        return;
    }
    int level = vt[0] == '1';
    uam_cpu_irq_sink(uam_monitor_cpu(c), (unsigned)line, level);
    unsigned k = 0;
    while (k < st->n_irq && !(st->irq[k].cpu == cpu && st->irq[k].line == line))
        k++;
    if (k == st->n_irq && k < MAX_IRQ)
        st->n_irq++;
    if (k < MAX_IRQ)
        st->irq[k].cpu = cpu, st->irq[k].line = (unsigned)line, st->irq[k].level = level;
    uam_monitor_field(c, "line", "%lu", line);
    uam_monitor_field(c, "level", "%d", level);
}

/* ---- a processor's whole state (the ISA's own dump, as -d prints it) ------------------- */

static void cmd_cpustate(uam_monitor_call *c, void *ctx)
{
    (void)ctx;
    uam_cpu *cpu = uam_monitor_cpu(c);
    if (!cpu->isa->dump_state) {
        uam_monitor_fail(c, "cpustate: this ISA has no dump of its state");
        return;
    }
    char *text = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&text, &len);
    if (!f) {
        uam_monitor_fail(c, "cpustate: %s", strerror(errno));
        return;
    }
    cpu->isa->dump_state(cpu, f);
    fclose(f);
    for (char *p = text, *nl; p && *p; p = nl ? nl + 1 : NULL) {
        nl = strchr(p, '\n');
        if (nl)
            *nl = 0;
        uam_monitor_print(c, "%s", p);
    }
    free(text);
}

/* ---- setup --------------------------------------------------------------------------- */

uam_status mon_add_state_commands(uam_monitor *mon)
{
    mon_state *st = calloc(1, sizeof(*st));
    if (!st)
        return UAM_ERR_NOMEM;
    st->mon = mon;
    st->m = uam_monitor_machine(mon);
    *mon_state_slot(mon) = st;
    static const struct {
        const char *name, *usage, *help;
        uam_monitor_fn fn;
        unsigned    flags;
    } cmds[] = {
        { "snap", "save NAME [FILE] | load NAME|FILE | list | write NAME FILE | read NAME FILE | "
          "drop NAME", "snapshots: keep the machine as it is, and go back to it", cmd_snap,
          UAM_MONITOR_CHANGES },
        { "reset", "[system|cpu]", "reset the machine (or the processors only), as the guest "
          "asking would", cmd_reset, UAM_MONITOR_CHANGES },
        { "devices", "", "the machine's devices: name, type, regions mapped", cmd_devices, 0 },
        { "dev", "NAME", "one device: its regions, and what state it is in", cmd_dev, 0 },
        { "heap", "[ADDR]", "the board's guarded heaps; with ADDR, the block it lies in", cmd_heap, 0 },
        { "irq", "[LINE 0|1]", "drive an interrupt input line of the processor (cpu N)", cmd_irq,
          UAM_MONITOR_CHANGES },
        { "cpustate", "", "the processor's whole state, system registers too, as -d dumps it",
          cmd_cpustate, 0 },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        uam_status s = uam_monitor_add_command(mon, cmds[i].name, cmds[i].usage, cmds[i].help,
                                               UAM_MONITOR_STOPPED_ONLY | cmds[i].flags,
                                               cmds[i].fn, mon);
        if (s != UAM_OK)
            return s;
    }
    return UAM_OK;
}

void mon_state_free(uam_monitor *mon)
{
    mon_state *st = S(mon);
    if (!st)
        return;
    for (unsigned i = 0; i < MAX_SNAPS; i++)
        if (st->snap[i].name)
            drop_snap(&st->snap[i]);
    free(st);
    *mon_state_slot(mon) = NULL;
}
