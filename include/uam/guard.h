/*
 * uam/guard.h - Guarded and watched guest memory.
 *
 * A guard is a byte range that the processors' data accesses must not touch, in the
 * directions it names (read, write or both). Any access that overlaps the range hits
 * it, so a 4-byte load that starts two bytes before a guard counts. What a hit does
 * is the guard's action:
 *   STOP   the access does not happen and uam_exec_run returns UAM_STOP_GUARD with
 *          the processor at the faulting instruction (resuming re-executes it): a
 *          debugger's watchpoint that fires before the access, whatever the guest's
 *          own exception handling would have done
 *   FAULT  the access does not happen and the processor takes the bus fault an
 *          unmapped address raises (UAM_EXC_LOAD_FAULT / UAM_EXC_STORE_FAULT), for
 *          the guest to handle or an exception observer to catch
 *   LOG    the access happens; it is only recorded, and printed to the machine's
 *          guard log (a watchpoint)
 * Every hit is recorded (uam_machine_guard_last_hit, uam_machine_guard_hits) and
 * passed to the guard observer if there is one.
 *
 * Guards sit on the physical bus. A virtual range is translated as processor 0 maps
 * it when the guard is added, a page at a time; a later change of mapping does not
 * move it. Instruction fetches, debug accesses (gdb, --dump-mem) and DMA
 * (uam_bus_dma_*) are exempt. Every access to a guarded page goes through the bus's
 * slow path (the JIT's direct memory access is off for that page). Guards are
 * configuration, not machine state: snapshots neither save nor restore them, and
 * they stay in place across a restore.
 *
 * Built on bus watches (uam_bus_add_watch, UAM_WATCH_FAULT) and
 * uam_exec_request_fault_stop.
 *
 * Dependencies: machine.h.
 */
#ifndef UAM_GUARD_H
#define UAM_GUARD_H

#include <stdio.h>

#include "uam/machine.h"

typedef enum uam_guard_action {
    UAM_GUARD_STOP,
    UAM_GUARD_FAULT,
    UAM_GUARD_LOG,
} uam_guard_action;

const char *uam_guard_action_str(uam_guard_action a);   /* "stop", "fault", "log" */

/* uam_guard_spec.dir */
enum {
    UAM_GUARD_READ  = UAM_WATCH_READ,
    UAM_GUARD_WRITE = UAM_WATCH_WRITE,
    UAM_GUARD_RW    = UAM_WATCH_READ | UAM_WATCH_WRITE,
};

typedef struct uam_guard_spec {
    uint64_t         addr, len;   /* the range [addr, addr + len); len > 0 */
    bool             phys;        /* addr is physical (else virtual, as processor 0 maps it) */
    unsigned         dir;         /* UAM_GUARD_READ and/or UAM_GUARD_WRITE */
    uam_guard_action action;
    const char      *name;        /* copied; NULL: "guardN" */
} uam_guard_spec;

typedef struct uam_guard_hit {
    int              id;          /* the guard's */
    const char      *name;        /* valid while the guard exists */
    uam_guard_action action;
    unsigned         cpu;         /* the processor that made the access (hartid) */
    uint64_t         pc;          /* STOP, once uam_exec_run has returned: the faulting
                                   * instruction. Otherwise the processor's pc when the
                                   * access was made (the start of its block, which may
                                   * be earlier than the instruction) */
    uint64_t         addr;        /* where the access starts, in the guard's address space
                                   * (virtual if the guard is), so addr - start is how far
                                   * into the guard it is (negative: it starts before) */
    uint64_t         start, len;  /* the guard's range, as given */
    uint64_t         paddr;       /* where the access starts, physically */
    unsigned         size;        /* bytes */
    bool             is_write;
    uint64_t         val;         /* a write's value (stored for LOG, refused otherwise);
                                   * LOG reads: the value read; 0 otherwise */
} uam_guard_hit;

/* Add a guard; returns its id (> 0), or -1 with err saying why (a virtual range
 * processor 0 does not map, len 0, an empty dir, out of memory). */
int  uam_machine_add_guard(uam_machine *m, const uam_guard_spec *spec, char *err, size_t errlen);
bool uam_machine_remove_guard(uam_machine *m, int id);
void uam_machine_remove_guards(uam_machine *m);

/*
 * Guards written as text (the runner's --guard and --watch, a fuzz route):
 *
 *   [p:]ADDR[-BEFORE][+AFTER][:r|w|rw][,stop|,fault|,log][,name=NAME]
 *
 * The range is [ADDR - BEFORE, ADDR + AFTER), around the anchor ADDR: "0x1000+16" is
 * the 16 bytes from 0x1000, "0x1000-16" the 16 before it, "0x1000-16+16" both, and a
 * bare "0x1000" the one byte there. Numbers are C literals (0x.., decimal); BEFORE and
 * AFTER take a K/M suffix. p: makes ADDR physical. The direction defaults to rw, the
 * action to `default_action`, the name to the text before the first ','.
 */
bool uam_guard_parse(const char *text, uam_guard_action default_action, uam_guard_spec *spec,
                     char *name, size_t namelen, char *err, size_t errlen);
int  uam_machine_add_guard_text(uam_machine *m, const char *text, uam_guard_action default_action,
                                char *err, size_t errlen);

/* The most recent hit (NULL before the first), and how many there have been. */
const uam_guard_hit *uam_machine_guard_last_hit(const uam_machine *m);
uint64_t             uam_machine_guard_hits(const uam_machine *m);

/* Called on every hit, after it is recorded, from inside the access: it must not
 * add or remove guards or watches. One observer per machine (NULL removes it). */
typedef void (*uam_guard_observer_fn)(void *ctx, uam_machine *m, const uam_guard_hit *hit);
void uam_machine_set_guard_observer(uam_machine *m, uam_guard_observer_fn fn, void *ctx);

/* Where LOG hits are printed, one line each (default stderr; NULL: nowhere). */
void uam_machine_set_guard_log(uam_machine *m, FILE *out);
/* The one-line description used there, without a newline. */
void uam_guard_hit_format(const uam_guard_hit *hit, char *buf, size_t n);

#endif /* UAM_GUARD_H */
