/*
 * uam/rules.h - Rules: when the guest arrives somewhere, check a condition and act.
 *
 * A rule names a place in the guest's run, an optional condition on the processor's
 * registers and memory there, and what to do: store a value in a register or in memory,
 * log values, stop the run. It is written as text, so a command line, a file or a board
 * can carry it:
 *
 *   pc=ADDR[,cpu=N][,if COND][,count=N][,name=NAME],ACTION[,ACTION...]
 *
 *   pc=ADDR      the rule is checked each time a processor arrives at ADDR, before that
 *                instruction executes (a PC hook, exec.h): a number, or an expression of
 *                numbers and symbols (main+0x10)
 *   cpu=N        only processor N's arrivals count
 *   if COND      the rule fires only when COND is not zero (several: all of them)
 *   count=N      the rule fires at most N times
 *   name=NAME    what the rule is called in logs and statistics (default: its pc=... text)
 *
 *   set DST=EXPR store EXPR's value in DST: a register, or memory ([ADDR] below)
 *   log [EXPR...]  print a line with each EXPR's value (operands separated by spaces):
 *                  rule NAME: cpu 0 pc 0x8000123c a0=0x1 [sp+8]:4=0x20 icount=4711
 *   stop         stop the run there (uam_exec_run returns UAM_STOP_HOOK), as a breakpoint
 *
 * A rule that fires runs its actions in the order written. Expressions are C's, on
 * unsigned 64-bit values:
 *
 *   0x1f, 42          numbers
 *   a0, rsp, pc       registers, as the ISA's debugger view names them (uam_isa_reg_by_name);
 *                     pc is the processor's pc where the ISA names no such register
 *   main              a symbol, when the rule set has a resolver (uam_rules_set_symbols);
 *                     a register of the same name wins
 *   [EXPR]            the memory at the virtual address EXPR, as the arriving processor maps
 *                     it (as a debugger sees it; x86: a linear address), in its byte order;
 *                     [EXPR]:1 :2 :4 :8 gives the width in bytes (default: the ISA's address
 *                     width). [p:EXPR] is a physical address
 *   ( )  - ~ !  *  + -  << >>  < <= > >=  == !=  &  ^  |  &&  ||
 *                     with C's precedence; comparisons are unsigned and give 0 or 1;
 *                     && and || evaluate both sides
 *
 * So a rule can stand in for something the machine does not model. A firmware that
 * waits at 0x9a144b2 for a coprocessor to store the value it holds at [rsp+0x38] through
 * the 32-bit pointer it holds at [rsp+0x30]:
 *
 *   pc=0x9a144b2,set [[rsp+0x30]:4]:4=[rsp+0x38]:4
 *
 * and the same rules give conditional breakpoints (pc=parse,if a1>0x400,stop), value
 * logs (pc=send,log a0 [a0]:4), and injected results (pc=0x8000456c,count=1,set a0=-1).
 *
 * Memory is read and written the way a debugger does it: reads see RAM, ROM and what a
 * device can show without side effects (its peek handler); writes reach RAM (and a ROMD
 * region's backing store), and translated code made from the written bytes is discarded.
 * A rule never causes a device access, and watches and guards do not see its accesses.
 * An operand that cannot be read (an unmapped address, a device without a peek handler)
 * makes its condition or action fail: a failed condition does not fire the rule, a failed
 * action is skipped, and each is counted (uam_rule_info.failed). Nothing is printed for
 * it but one line at UAM_LOG_INFO for a rule's first failure.
 *
 * Rules are configuration, as PC hooks are: a snapshot does not hold them. The one
 * exception is how often a rule with count=N has fired, which decides what the machine
 * does next: a set with such a rule registers the extra state "rules-NAME"
 * (uam_machine_add_state), so restoring a snapshot brings the counts back with it - and a
 * snapshot taken with counted rules restores only into a machine with the same counted
 * rules, one taken without them only into a machine without. Add counted rules before
 * the first snapshot is taken or restored.
 *
 * A hooked pc costs what a PC hook costs: blocks end there, and are not chained or
 * skipped as a spin loop (so a rule on a wait loop is checked on every pass).
 *
 * Dependencies: machine.h.
 */
#ifndef UAM_RULES_H
#define UAM_RULES_H

#include <stdio.h>

#include "uam/machine.h"

typedef struct uam_rules uam_rules;

/* The value of the symbol `name`; false if there is none. */
typedef bool (*uam_rule_symbol_fn)(void *ctx, const char *name, uint64_t *value);

/*
 * A rule set for machine m, empty. `name` tells the machine's rule sets apart (a board's
 * and a tool's): it names the set's counts in snapshots and must be unique per machine.
 * Create it once the machine has its processors (after uam_machine_add_cpus). NULL: out
 * of memory.
 */
uam_rules *uam_rules_create(uam_machine *m, const char *name);
/* Take the set's hooks and state out of the machine, which must still exist. The rules
 * stay readable (uam_rules_get); they no longer run. */
void       uam_rules_detach(uam_rules *rs);
/* Free the set: after uam_rules_detach, or once the machine is gone (uam_machine_fini). */
void       uam_rules_free(uam_rules *rs);

/* Symbols for the rules added from now on (NULL: none). */
void       uam_rules_set_symbols(uam_rules *rs, uam_rule_symbol_fn fn, void *ctx);
/* Where `log` actions print (default stderr). */
void       uam_rules_set_log(uam_rules *rs, FILE *out);

/* Add the rule `text` (the grammar above). Returns its index in the set, or -1 with the
 * reason in err. */
int        uam_rules_add(uam_rules *rs, const char *text, char *err, size_t errlen);
/* Add the rules in the file `path`: one per line; empty lines and lines starting with '#'
 * are skipped. On a bad line nothing more is added and err names the file and line. */
uam_status uam_rules_add_file(uam_rules *rs, const char *path, char *err, size_t errlen);

typedef struct uam_rule_info {
    const char *name;
    uint64_t    pc;
    uint64_t    arrivals;   /* times a processor it applies to arrived at pc */
    uint64_t    fired;      /* times its conditions held and its actions ran */
    uint64_t    failed;     /* conditions and actions that could not be carried out */
    uint64_t    limit;      /* count=N; 0: none */
} uam_rule_info;

size_t     uam_rules_n(const uam_rules *rs);
bool       uam_rules_get(const uam_rules *rs, size_t i, uam_rule_info *out);

/* The last `stop` action that ran: which rule, and where. False if none has. */
typedef struct uam_rule_stop {
    const char *name;
    unsigned    cpu;
    uint64_t    pc, icount;
} uam_rule_stop;
bool       uam_rules_last_stop(const uam_rules *rs, uam_rule_stop *out);

#endif /* UAM_RULES_H */
