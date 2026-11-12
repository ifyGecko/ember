/*
 * trace.h - The runner's bring-up traces (internal to src/runner): what the guest does
 * to devices, system registers, interrupt lines and the console, on top of the
 * observers the core offers (bus.h uam_bus_set_dev_observer, cpu.h uam_cpu_observer,
 * chardev.h uam_chardev_set_tap). Everything here is off unless a
 * --trace kind asks for it.
 */
#ifndef UAM_RUNNER_TRACE_H
#define UAM_RUNNER_TRACE_H

#include <stdio.h>

#include "uam/chardev.h"
#include "uam/machine.h"

enum {
    RT_MMIO       = 1u << 0,   /* every access a device serves */
    RT_SYSREG     = 1u << 1,   /* the first read and write of each system register, and failures */
    RT_SYSREG_ALL = 1u << 2,   /* every system register access */
    RT_IRQ        = 1u << 3,   /* interrupt input lines of the processors */
    RT_CONSOLE    = 1u << 4,   /* console output lines, stamped */
};

#define RT_SYSREG_SLOTS 1024u

typedef struct rt_sysreg {
    bool     used;
    uint32_t id;
    uint64_t reads, writes, fails;
    uint64_t first_pc;
} rt_sysreg;

typedef struct rt_irq_line {
    unsigned cpu, line;
    uint64_t rises, falls;
    int      level;
} rt_irq_line;

typedef struct rtrace {
    unsigned          flags;          /* RT_* */
    FILE             *out;
    uam_machine      *m;
    uam_chardev      *console;
    uint64_t          mmio_lo, mmio_hi;   /* RT_MMIO: physical addresses in [lo, hi) (hi = 0: all) */
    uint64_t          after;              /* only events once the icount reaches this */
    uam_cpu_observer  obs;
    rt_sysreg         sr[RT_SYSREG_SLOTS];
    uint32_t          n_sr;
    uint64_t          sr_dropped;         /* accesses to registers the table had no room for */
    rt_irq_line       irq[64];
    uint32_t          n_irq;
    uint64_t          irq_dropped;        /* changes on lines the table had no room for */
    char              line[200];          /* RT_CONSOLE: the line being printed */
    size_t            len;
    bool              line_origin_ok;
    uam_origin        line_origin;
} rtrace;

/* A --trace token this module owns: its RT_* bit, or 0. */
unsigned rtrace_flag(const char *tok);
/* Install the observers the flags ask for. */
void     rtrace_attach(rtrace *t, uam_machine *m, uam_chardev *console, FILE *out);
/* The run stopped: a partial console line, the summaries (system registers, interrupt
 * lines). TODO: a summary of the device polling loops a run waited in, once a trace
 * can detect polling loops of any shape (the removed --trace polls could not). */
void     rtrace_finish(rtrace *t);
/* Start over: a fuzz input begins (the machine is back at the snapshot). Flushes a
 * partial console line and forgets the first accesses and the counts, so that what
 * follows, and the next rtrace_finish, is this input's alone. */
void     rtrace_restart(rtrace *t);
/* Remove the observers (before the machine goes). */
void     rtrace_detach(rtrace *t);

#endif /* UAM_RUNNER_TRACE_H */
