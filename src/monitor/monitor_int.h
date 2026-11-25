/*
 * monitor_int.h - Between the monitor's engine (monitor.c) and its command groups.
 */
#ifndef UAM_MONITOR_INT_H
#define UAM_MONITOR_INT_H

#include "uam/frames.h"
#include "uam/monitor.h"
#include "uam/rules.h"

/* monitor_inspect.c: regs, reg, x, write, load, map, history, bt, dump */
uam_status       mon_add_inspect_commands(uam_monitor *mon);
/* The machine is not running (stopped by the monitor, or ended). */
bool             mon_stopped(const uam_monitor *mon);
/* What names an address for "bt" (uam_monitor_set_symbolizer), or NULL. */
uam_symbolize_fn mon_symbolizer(const uam_monitor *mon, void **ctx);

/* An event to every client: kind, then key/value string pairs, then NULL. */
void             mon_event(uam_monitor *mon, const char *kind, ...);

/* monitor_instr.c: break, guard, watch, rule */
typedef struct mon_instr mon_instr;
mon_instr      **mon_instr_slot(uam_monitor *mon);
uam_status       mon_add_instr_commands(uam_monitor *mon);
void             mon_instr_free(uam_monitor *mon);
void             mon_instr_set_rule_symbols(uam_monitor *mon, uam_rule_symbol_fn fn, void *ctx);
/* Before each run: what it starts from, a guard to step over. */
void             mon_instr_before_run(uam_monitor *mon);
/* The run's instruction limit, lowered to where a stepped-over guard goes back. */
uint64_t         mon_instr_limit(uam_monitor *mon, uint64_t limit);
/* After a run: the reason, if the monitor's own breakpoint, rule or guard stopped it;
 * "" if it was only the step over a guard (go on); NULL otherwise. */
const char      *mon_instr_claim(uam_monitor *mon, uam_stop_reason why);

/* monitor_state.c: snap, reset, devices, dev, heap, irq, cpustate */
typedef struct mon_state mon_state;
mon_state      **mon_state_slot(uam_monitor *mon);
uam_status       mon_add_state_commands(uam_monitor *mon);
void             mon_state_free(uam_monitor *mon);
/* The machine was put back (a snapshot, a reset): it is stopped there, also after the
 * run had ended; `why` is the reason "status" gives. */
void             mon_revive(uam_monitor *mon, const char *why);

/* monitor_io.c: send, expect, tap, input, link */
typedef struct mon_io mon_io;
mon_io         **mon_io_slot(uam_monitor *mon);
uam_status       mon_add_io_commands(uam_monitor *mon);
void             mon_io_free(uam_monitor *mon);
void             mon_io_tick(uam_monitor *mon);          /* every host poll */
void             mon_io_ended(uam_monitor *mon);         /* the run ended */
void             mon_io_client_gone(uam_monitor *mon, unsigned serial);
void             mon_io_client_new(uam_monitor *mon, unsigned serial);   /* reads from now */

/* The client a call came from, by a number that is never reused (0: a script). */
unsigned         mon_call_serial(const uam_monitor_call *c);
/* Answer the call later (mon_answer_io); the client's later lines wait. */
bool             mon_defer_io(uam_monitor_call *c);
/* Answer a deferred call: error, or ok with key/value string pairs, then NULL. */
void             mon_answer_io(uam_monitor *mon, unsigned serial, const char *error, ...);
/* An event to one client (serial; 0: every client), with one quoted data value
 * (data_key NULL: none), then key/value string pairs, then NULL. */
void             mon_event_to(uam_monitor *mon, unsigned serial, const char *kind,
                              const char *data_key, const uint8_t *data, size_t len, ...);

#endif /* UAM_MONITOR_INT_H */
