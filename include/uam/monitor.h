/*
 * uam/monitor.h - The monitor: a live command channel into a running machine.
 *
 * A program (uam-run) creates a monitor over its machine and host loop, opens one or
 * more ways in, and runs the machine through uam_monitor_serve instead of
 * uam_exec_run. Commands then pause, inspect and steer the run while it goes:
 *
 *     uam_monitor *mon = uam_monitor_create(m, host, &(uam_monitor_config){ .hold = true });
 *     uam_monitor_listen(mon, "unix:mon.sock", err, sizeof(err));
 *     uam_monitor_use_console(mon, console);      // a raw stdio console: Ctrl-A m
 *     why = uam_monitor_serve(mon, max_insns);
 *
 * Ways in (uam_monitor_listen):
 *   unix:PATH   a Unix socket (a stale socket file there is replaced; removed at the end)
 *   tcp:PORT    a TCP port on 127.0.0.1 only
 *   stdio       stdin/stdout, one line at a time (when the guest console is elsewhere)
 * and, on a raw stdio console, Ctrl-A m (uam_monitor_use_console), which switches the
 * terminal between the guest and the monitor. Several clients may be connected at once.
 *
 * Protocol, one command per line:
 *   [#ID ]COMMAND [ARG...]
 * Arguments are separated by blanks; "double quotes" keep blanks (\" and \\ inside).
 * A command's reply is its output lines, then "ok" or "error: WHY" - with "#ID " in
 * front when the command had one. Lines starting "! " are events, sent to every client
 * between replies: "! hello", "! stopped", "! ended". After "json on" a client gets each
 * reply and event as one JSON object per line instead:
 *   {"id":"ID","ok":true,"lines":[...],"data":{"field":"value",...}}
 *   {"id":"ID","ok":false,"error":"WHY"}
 *   {"event":"stopped","reason":"monitor",...}
 * A line starting "#" that is not an id ("# ...") is a comment.
 *
 * Execution: commands are read by the host loop, between blocks while the machine
 * runs and in a wait while it is stopped. "stop" replies once the machine has
 * stopped; a client's later lines wait for that. A machine stopped by the monitor
 * keeps its state, and "cont" goes on from there as if nothing had happened (real-time
 * pacing starts again from the host clock). With no monitor the run path is unchanged.
 *
 * Commands of a board's own (a button, a device's state) are added with
 * uam_monitor_add_command; ember's own commands are listed by "help".
 *
 * Dependencies: machine.h, host.h.
 */
#ifndef UAM_MONITOR_H
#define UAM_MONITOR_H

#include "uam/host.h"
#include "uam/machine.h"

#define UAM_MONITOR_PROTOCOL 1

typedef struct uam_monitor      uam_monitor;
typedef struct uam_monitor_call uam_monitor_call;

typedef struct uam_monitor_config {
    bool wait;   /* stopped before the first instruction, until "cont" */
    bool hold;   /* when the run ends, stay (stopped) for inspection until "quit" */
} uam_monitor_config;

uam_monitor *uam_monitor_create(uam_machine *m, uam_host *h, const uam_monitor_config *cfg);
/* Closes every client and way in, removes Unix socket files, gives the console back. */
void         uam_monitor_destroy(uam_monitor *mon);
/* Open a way in (see above). UAM_ERR_INVALID: a spec it does not know; UAM_ERR_FAULT:
 * it could not be opened. The reason is in err. */
uam_status   uam_monitor_listen(uam_monitor *mon, const char *spec, char *err, size_t errlen);
/* Ctrl-A m on a raw stdio console. False: not a raw stdio console on a terminal. */
bool         uam_monitor_use_console(uam_monitor *mon, uam_chardev *console);
/* Run the commands in a file, one per line, before the first instruction (the machine
 * is stopped while they run; it starts afterwards unless one said "stop" or the monitor
 * waits). Output goes to stderr. UAM_ERR_REJECTED: a command failed (the reason is in
 * err, and the rest of the file is not run); UAM_ERR_FAULT: the file cannot be read. */
uam_status   uam_monitor_run_script(uam_monitor *mon, const char *path, char *err, size_t errlen);

/*
 * Run the machine for up to max_insns instructions in all (0: no limit), serving the
 * monitor, until the run is over: it ended (without hold), or a client said "quit".
 * Returns why, as uam_exec_run would: a "quit N" is UAM_STOP_REQUEST with
 * exec.stop_status N; a plain "quit" after the run ended returns how it ended, and
 * before that is UAM_STOP_REQUEST with status 0.
 */
uam_stop_reason uam_monitor_serve(uam_monitor *mon, uint64_t max_insns);

/* ---- commands ---- */
enum {
    UAM_MONITOR_STOPPED_ONLY = 1u << 0,   /* refused while the machine runs */
    /* Needs the monitor to run the machine (uam_monitor_serve): refused through
     * uam_monitor_execute, where something else (gdb) has run control. */
    UAM_MONITOR_SERVE_ONLY   = 1u << 1,
    /* Changes the machine: recorded by --monitor-log (uam_monitor_set_log). */
    UAM_MONITOR_CHANGES      = 1u << 2,
};
typedef void (*uam_monitor_fn)(uam_monitor_call *c, void *ctx);
/* `usage`: the arguments ("ADDR LEN"); `help`: one line. A name already taken is
 * replaced (a board may take over one of ember's). */
uam_status   uam_monitor_add_command(uam_monitor *mon, const char *name, const char *usage,
                                     const char *help, unsigned flags, uam_monitor_fn fn,
                                     void *ctx);

/* In a command: arguments (0 is the command's name), the text of the line from
 * argument i on as typed (NULL beyond the last), output lines, named results (a line
 * "NAME: VALUE" as text, a member of "data" as JSON), and failure (the reply is then
 * the error, without the output). */
int          uam_monitor_argc(const uam_monitor_call *c);
const char  *uam_monitor_arg(const uam_monitor_call *c, int i);
const char  *uam_monitor_rest(const uam_monitor_call *c, int i);
uam_monitor *uam_monitor_of(const uam_monitor_call *c);
uam_machine *uam_monitor_machine(const uam_monitor *mon);
void         uam_monitor_print(uam_monitor_call *c, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void         uam_monitor_field(uam_monitor_call *c, const char *name, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
void         uam_monitor_fail(uam_monitor_call *c, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
/* Output lines printed so far in this call. */
size_t       uam_monitor_lines(const uam_monitor_call *c);

/*
 * Run one command line where something else runs the machine and has it stopped now (the
 * gdb stub's "monitor CMD"): *out gets the reply as text (output lines, named results,
 * then "error: WHY" if it failed; the caller frees it). Commands flagged
 * UAM_MONITOR_SERVE_ONLY are refused. UAM_ERR_REJECTED: the command failed.
 */
uam_status   uam_monitor_execute(uam_monitor *mon, const char *line, char **out);

/*
 * --monitor-log: every command that changes the machine (UAM_MONITOR_CHANGES) and
 * succeeded, written to `out` as "@ICOUNT LINE", ICOUNT the machine's instruction count
 * when it took effect. Given to uam_monitor_run_script, such a file plays the session
 * again: a line "@N COMMAND" runs when the machine reaches instruction N (at the start,
 * if it is there already), in the order written. NULL: no log.
 */
void         uam_monitor_set_log(uam_monitor *mon, FILE *out);

/*
 * A board's own command, offered before any monitor exists (from the board's build): a
 * monitor created over m later has it, as if uam_monitor_add_command had added it there.
 */
uam_status   uam_monitor_offer_command(uam_machine *m, const char *name, const char *usage,
                                       const char *help, unsigned flags, uam_monitor_fn fn,
                                       void *ctx);

/* Is the machine running (not stopped by the monitor, not ended)? */
bool         uam_monitor_running(const uam_monitor *mon);
/* Before uam_monitor_serve: will it start stopped (--monitor-wait, a script's "stop")? */
bool         uam_monitor_starts_stopped(const uam_monitor *mon);
/* The processor this client looks at ("cpu N"; 0 unless it chose another). */
unsigned     uam_monitor_cpu_index(const uam_monitor_call *c);
uam_cpu     *uam_monitor_cpu(const uam_monitor_call *c);
/* The machine's ways in and out that "send", "expect", "tap" and "link" reach: a chardev
 * by the name commands use for it (uam-run: "console"; the monitor taps its output), and
 * a link node (tap_free: its one tap is not taken, "link tap" may have it). */
uam_status   uam_monitor_add_chardev(uam_monitor *mon, const char *name, uam_chardev *cd);
uam_status   uam_monitor_add_link(uam_monitor *mon, uam_link_node *n, bool tap_free);
/* What symbols the monitor's rules ("rule add") can name, as uam_rules_set_symbols. */
void         uam_monitor_set_rule_symbols(uam_monitor *mon,
                                          bool (*fn)(void *ctx, const char *name, uint64_t *value),
                                          void *ctx);
/* How "bt" names an address (an image's symbols); NULL: addresses only. */
void         uam_monitor_set_symbolizer(uam_monitor *mon,
                                        bool (*fn)(void *ctx, uint64_t pc, char *buf, size_t n),
                                        void *ctx);

#endif /* UAM_MONITOR_H */
