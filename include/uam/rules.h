/*
 * uam/rules.h - Rules: when the guest arrives somewhere or touches something, check a
 * condition and act.
 *
 * A rule names a place in the guest's run (a pc, or accesses to a range of memory), an
 * optional condition on the processor's registers and memory there, and what to do: store
 * a value in a register or in memory, log values, dump memory, stop the run. It is
 * written as text, so a command line, a file or a board can carry it:
 *
 *   WHERE[,cpu=N][,if COND][,count=N][,delay=N][,name=NAME],ACTION[,ACTION...]
 *
 * WHERE is one of
 *   pc=ADDR      each time a processor arrives at ADDR, before that instruction executes
 *                (a PC hook, exec.h): a number, or an expression of numbers and symbols
 *                (main+0x10)
 *   write=[p:]ADDR[,len=N]
 *   read=[p:]ADDR[,len=N]
 *   access=[p:]ADDR[,len=N]
 *                each time an instruction stores to, loads from, or does either to the N
 *                bytes at ADDR (N: 1 unless given; an access that overlaps them counts).
 *                The rule runs when that instruction has completed: memory holds what it
 *                stored, the registers what it loaded, and pc is the next instruction.
 *                ADDR is virtual, as processor 0 (cpu=N: that one) maps data when the rule
 *                is armed; p: makes it physical. As for bus watches, instruction fetches,
 *                debug accesses and DMA do not count, and an instruction that touches the
 *                range more than once fires the rule once, for its first access.
 *
 *   cpu=N        only processor N's arrivals and accesses count
 *   if COND      the rule fires only when COND is not zero (several: all of them)
 *   count=N      the rule fires at most N times
 *   delay=N      the rule's stores happen N ticks of virtual time after it fires: what is
 *                stored, and where, is worked out when it fires (the values then, the
 *                physical addresses then); log and dump act when it fires. Until the
 *                stores have happened the rule does not fire again. This is something
 *                that answers a request after a while. A rule with a delay cannot stop
 *   name=NAME    what the rule is called in logs and statistics (default: its WHERE text)
 *
 *   set DST=EXPR store EXPR's value in DST: a register, or memory ([ADDR] below)
 *   log [EXPR...]  print a line with each EXPR's value (operands separated by spaces):
 *                  rule NAME: cpu 0 pc 0x8000123c a0=0x1 [sp+8]:4=0x20 icount=4711
 *                an access rule's line names the access and its instruction:
 *                  rule NAME: cpu 0 pc 0x80001230 write 4 at 0x80200010 value 0x1 icount=4711
 *   dump [p:]ADDR LEN   print that line with "dump 0xADDR+0xLEN", then the LEN bytes at
 *                ADDR (two expressions; at most 65536 bytes) as a hex dump
 *   stop         stop the run there (uam_exec_run returns UAM_STOP_HOOK), as a breakpoint
 *
 * A rule that fires runs its actions in the order written. Expressions are C's, on
 * unsigned 64-bit values:
 *
 *   0x1f, 42          numbers
 *   a0, rsp, pc       registers, as the ISA's debugger view names them (uam_isa_reg_by_name);
 *                     pc is the processor's pc where the ISA names no such register
 *   addr, value       in an access rule: the address accessed (in the space the rule's
 *                     range was given in) and the value stored or loaded
 *   main              a symbol, when the rule set has a resolver (uam_rules_set_symbols);
 *                     a register of the same name wins, then addr and value
 *   [EXPR]            the memory at the virtual address EXPR, as the processor maps it (as
 *                     a debugger sees it; x86: a linear address), in its byte order;
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
 * one that rings a doorbell register's RAM shadow and expects an answer in a mailbox some
 * time later:
 *
 *   write=p:0x7f000,len=4,if value==1,delay=2000,set [p:0x7f010]:4=0x80000001
 *
 * and the same rules give conditional breakpoints (pc=parse,if a1>0x400,stop), who wrote
 * a variable and what (write=rx_count,len=4,log value), a buffer at a point
 * (pc=send,dump a0 a1), and injected results (pc=0x8000456c,count=1,set a0=-1).
 *
 * Memory is read and written the way a debugger does it: reads see RAM, ROM and what a
 * device can show without side effects (its peek handler); writes reach RAM (and a ROMD
 * region's backing store), and translated code made from the written bytes is discarded.
 * A rule never causes a device access, and watches, guards and other access rules do not
 * see its accesses. An access rule sees the processors' accesses to RAM, ROM and devices
 * alike. An operand that cannot be read (an unmapped address, a device without a peek
 * handler) makes its condition or action fail: a failed condition does not fire the rule,
 * a failed action is skipped, and each is counted (uam_rule_info.failed). Nothing is
 * printed for it but one line at UAM_LOG_INFO for a rule's first failure.
 *
 * Rules are configuration, as PC hooks are: a snapshot does not hold them. The exception
 * is what decides the machine's future: how often a rule with count=N has fired, and the
 * stores of a rule with delay=N that are on their way. A set with such a rule registers
 * the extra state "rules-NAME" (uam_machine_add_state), so restoring a snapshot brings
 * both back - and a snapshot taken with rules that have a count or a delay restores only
 * into a machine with the same such rules, one taken without them only into a machine
 * without. Add them before the first snapshot is taken or restored.
 *
 * A hooked pc costs what a PC hook costs: blocks end there, and are not chained or
 * skipped as a spin loop (so a rule on a wait loop is checked on every pass). A watched
 * range costs what a bus watch costs: accesses to its pages take the bus's slow path.
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

/*
 * Access rules watch physical memory, so a virtual range is translated when its rule is
 * armed: at once when the rule is added, unless uam_rules_arm_later was called - then
 * access rules wait for uam_rules_arm. That is for a caller that adds its rules, restores
 * a snapshot (which may hold their counts) and only then has the mappings the guest runs
 * with. uam_rules_arm (re)arms every access rule as the machine maps memory now, and can
 * be called again whenever the mappings a rule depends on have changed; on failure err
 * names the rule and the address, and that rule is left unarmed.
 */
void       uam_rules_arm_later(uam_rules *rs);
uam_status uam_rules_arm(uam_rules *rs, char *err, size_t errlen);

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
    uint64_t    pc;         /* a pc rule's */
    uint64_t    addr, len;  /* an access rule's range (len 0: a pc rule) */
    uint64_t    arrivals;   /* times a processor it applies to arrived there or accessed it */
    uint64_t    fired;      /* times its conditions held and its actions ran */
    uint64_t    failed;     /* conditions and actions that could not be carried out */
    uint64_t    limit;      /* count=N; 0: none */
    uint64_t    delay;      /* delay=N; 0: none */
    bool        waiting;    /* stores of its last firing are still on their way */
} uam_rule_info;

size_t     uam_rules_n(const uam_rules *rs);
bool       uam_rules_get(const uam_rules *rs, size_t i, uam_rule_info *out);

/* The last `stop` action that ran: which rule, and where (an access rule: the accessing
 * instruction; the run stops after it). False if none has. */
typedef struct uam_rule_stop {
    const char *name;
    unsigned    cpu;
    uint64_t    pc, icount;
} uam_rule_stop;
bool       uam_rules_last_stop(const uam_rules *rs, uam_rule_stop *out);
/* Forget the last stop, so that uam_rules_last_stop says whether one happens after this. */
void       uam_rules_clear_stop(uam_rules *rs);

/* Turn rule i off (its pc is no longer hooked, its range no longer watched; stores of a
 * delay already on their way still happen) or on again. A rule that is off keeps its
 * place, its counts and its part of the set's snapshot state. Rules are on when added. */
uam_status uam_rules_enable(uam_rules *rs, size_t i, bool on, char *err, size_t errlen);
bool       uam_rules_enabled(const uam_rules *rs, size_t i);

#endif /* UAM_RULES_H */
