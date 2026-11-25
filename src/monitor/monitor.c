/*
 * monitor.c - The monitor (uam/monitor.h): clients and ways in, the line protocol,
 * the run loop, and ember's own commands.
 */
#define _XOPEN_SOURCE 700   /* strndup, strdup */
#define _DARWIN_C_SOURCE    /* macOS: SO_NOSIGPIPE, which _XOPEN_SOURCE alone hides */
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "uam/monitor.h"
#include "monitor_int.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0      /* macOS: SO_NOSIGPIPE on the socket instead */
#endif

#define MAX_LISTENERS 4u
#define MAX_CLIENTS   8u
#define MAX_ARGS      32u
#define LINE_MAX_LEN  8192u
#define OUT_MAX       (1u << 20)   /* a socket client's unsent output; past it, it is dropped */
#define IN_MAX        (1u << 20)   /* input a waiting client may pile up */
#define CONSOLE_PROMPT "ember-mon> "

typedef enum { CL_SOCKET, CL_STDIO, CL_CONSOLE, CL_SCRIPT, CL_EXTERNAL } client_kind;

typedef struct client {
    client_kind kind;
    int     in_fd, out_fd;
    char   *buf;              /* input not yet run */
    size_t  len, cap;
    bool    eof;              /* no more input will come */
    bool    json;
    bool    waiting_stop;     /* a "stop" not answered yet: later lines wait */
    bool    waiting_io;       /* an "expect" not answered yet: later lines wait */
    bool    wait_ok_at_end;   /* "wait": the end of the run is an answer, not an error */
    bool    discarding;       /* a line too long: skip to its end */
    bool    dead;             /* gone or too slow: freed at the next tick */
    char   *pend;             /* socket output the client has not taken yet */
    size_t  pend_len, pend_cap;
    char    stop_id[40];
    unsigned serial;          /* names the client to monitor_io.c */
    bool    active;           /* CL_CONSOLE: the terminal is the monitor's */
    char    edit[512];        /* CL_CONSOLE: the line being typed */
    size_t  edit_len;
    int     esc_seq;          /* CL_CONSOLE: inside an escape sequence (1: ESC, 2: CSI) */
    unsigned cpu;             /* the processor "cpu N" selected */
    bool    failed;           /* CL_SCRIPT: a command failed */
    char    fail_why[256];
} client;

typedef struct listener {
    int  fd;
    bool is_unix;
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
} listener;

typedef struct mon_cmd {
    char          *name, *usage, *help;
    unsigned       flags;
    uam_monitor_fn fn;
    void          *ctx;
} mon_cmd;

typedef enum { ST_STOPPED, ST_RUNNING, ST_ENDED } run_state;

struct uam_monitor {
    uam_machine       *m;
    uam_host          *h;
    uam_monitor_config cfg;
    listener           ls[MAX_LISTENERS];
    unsigned           n_ls;
    client            *cl[MAX_CLIENTS];
    client            *console;
    uam_chardev       *console_cd;
    mon_cmd           *cmds;
    unsigned           n_cmds, cap_cmds;
    run_state          state;
    bool               autostart;
    bool               in_run;        /* inside uam_exec_run */
    bool               stop_wanted;   /* a "stop" asked the executor to stop */
    bool               quit;
    int64_t            quit_code;
    bool               quit_has_code;
    const char        *stop_why;      /* why it is stopped: "start", "monitor", "step", ... */
    uam_stop_reason    end_why;       /* ST_ENDED */
    int64_t            end_status;
    /* what the next run is for (a client waits for it): step, until, run-for */
    uint64_t           step_end;      /* machine icount to stop at; 0: none */
    bool               until_on, until_hit;
    uint64_t           until_pc, until_from;   /* hooked pc; machine icount when it began */
    bool               runfor_on;
    uint64_t           runfor_end, saved_max_time;
    uam_symbolize_fn   sym;
    void              *sym_ctx;
    mon_instr         *instr;         /* monitor_instr.c */
    mon_state         *mstate;        /* monitor_state.c */
    char               why_buf[96];   /* stop_why when it is a claimed reason */
    unsigned           next_serial;
    unsigned           log_cpu;       /* --monitor-log: the processor the log's lines are for */
    bool               last_failed;   /* the last command run_line ran failed */
    mon_io            *io;            /* monitor_io.c */
    FILE              *log;           /* --monitor-log */
    bool               served;        /* uam_monitor_serve has run the machine */
    /* --monitor-script lines "@N COMMAND", in order; next is the one due next */
    struct timed { uint64_t at; char *line; } *timed;
    size_t             n_timed, next_timed;
    client            *timed_cl;      /* runs them: a script client that lives on (its cpu) */
};

/* uam_monitor_offer_command: offered per machine, before a monitor exists. */
typedef struct offered {
    uam_machine   *m;
    char          *name, *usage, *help;
    unsigned       flags;
    uam_monitor_fn fn;
    void          *ctx;
    struct offered *next;
} offered;
static offered *g_offered;
static uam_status take_offered(uam_monitor *mon);
static void       forget_offered(const uam_machine *m);

struct uam_monitor_call {
    uam_monitor *mon;
    client      *cl;
    int          argc;
    char        *argv[MAX_ARGS];
    const char  *rest[MAX_ARGS];
    char        *out;           /* output lines, '\n'-separated */
    size_t       out_len, out_cap;
    char       **names, **values;
    unsigned     n_fields, cap_fields;
    char        *error;
    bool         deferred;      /* answered later ("stop") */
};

/* ---- small helpers ----------------------------------------------------- */

static void write_fd(int fd, bool sock, const char *p, size_t n)
{
    while (n) {
        ssize_t k = sock ? send(fd, p, n, MSG_NOSIGNAL) : write(fd, p, n);
        if (k > 0) {
            p += k;
            n -= (size_t)k;
        } else if (k < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        } else {
            return;   /* the client went away; its read side notices */
        }
    }
}

static bool cl_append(client *cl, const void *p, size_t n);

/* A socket client's output: sent as far as the socket takes it now, the rest kept for
 * later (cl_flush). Nothing ever waits for a client: one that does not read is dropped
 * once OUT_MAX bytes wait for it. */
static void cl_flush(client *c)
{
    size_t done = 0;
    while (done < c->pend_len) {
        ssize_t k = send(c->out_fd, c->pend + done, c->pend_len - done, MSG_NOSIGNAL);
        if (k > 0)
            done += (size_t)k;
        else if (k < 0 && errno == EINTR)
            continue;
        else if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        else {
            c->dead = true;   /* gone */
            done = c->pend_len;
        }
    }
    memmove(c->pend, c->pend + done, c->pend_len - done);
    c->pend_len -= done;
}

static void cl_write(client *c, const char *p, size_t n)
{
    if (c->kind == CL_EXTERNAL) {
        cl_append(c, p, n);   /* uam_monitor_execute: the reply is collected */
        return;
    }
    if (c->out_fd < 0 || c->dead)
        return;
    if (c->kind != CL_SOCKET) {   /* stdout (a terminal, a pipe): written through */
        write_fd(c->out_fd, false, p, n);
        return;
    }
    if (c->pend_len + n > OUT_MAX) {
        c->dead = true;           /* it has not read for a long time: let it go */
        return;
    }
    if (c->pend_len + n > c->pend_cap) {
        size_t cap = c->pend_cap ? c->pend_cap : 4096;
        while (cap < c->pend_len + n)
            cap *= 2;
        char *b = realloc(c->pend, cap);
        if (!b) {
            c->dead = true;
            return;
        }
        c->pend = b;
        c->pend_cap = cap;
    }
    memcpy(c->pend + c->pend_len, p, n);
    c->pend_len += n;
    cl_flush(c);
}

static void cl_puts(client *c, const char *s) { cl_write(c, s, strlen(s)); }

static char *vfmt(const char *fmt, va_list ap)
{
    va_list aq;
    va_copy(aq, ap);
    int n = vsnprintf(NULL, 0, fmt, aq);
    va_end(aq);
    char *s = malloc(n > 0 ? (size_t)n + 1 : 1);
    if (s)
        vsnprintf(s, (size_t)(n > 0 ? n : 0) + 1, fmt, ap);
    return s;
}

/* A growing string. */
typedef struct sbuf { char *p; size_t n, cap; } sbuf;

static void sb_add(sbuf *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->n + n + 1)
            cap *= 2;
        char *p = realloc(b->p, cap);
        if (!p)
            return;
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

static void sb_str(sbuf *b, const char *s) { sb_add(b, s, strlen(s)); }

static void sb_json_str(sbuf *b, const char *s)
{
    sb_add(b, "\"", 1);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        char esc[8];
        if (c == '"' || c == '\\') {
            esc[0] = '\\';
            esc[1] = (char)c;
            sb_add(b, esc, 2);
        } else if (c == '\n') {
            sb_add(b, "\\n", 2);
        } else if (c == '\t') {
            sb_add(b, "\\t", 2);
        } else if (c < 0x20) {
            snprintf(esc, sizeof(esc), "\\u%04x", c);
            sb_str(b, esc);
        } else {
            sb_add(b, (const char *)&c, 1);
        }
    }
    sb_add(b, "\"", 1);
}

/* ---- events -------------------------------------------------------------- */

typedef struct event {
    const char *kind;
    unsigned    n;
    const char *k[10];
    char        v[10][96];
    const char *data_key;     /* one value of any length, quoted in text (C escapes) */
    const uint8_t *data;
    size_t      data_len;
} event;

static void sb_c_str(sbuf *b, const uint8_t *p, size_t n)
{
    sb_add(b, "\"", 1);
    for (size_t i = 0; i < n; i++) {
        uint8_t c = p[i];
        char esc[8];
        if (c == '"' || c == '\\') {
            esc[0] = '\\';
            esc[1] = (char)c;
            sb_add(b, esc, 2);
        } else if (c == '\n') {
            sb_add(b, "\\n", 2);
        } else if (c == '\r') {
            sb_add(b, "\\r", 2);
        } else if (c == '\t') {
            sb_add(b, "\\t", 2);
        } else if (c < 0x20 || c >= 0x7f) {
            snprintf(esc, sizeof(esc), "\\x%02x", c);
            sb_str(b, esc);
        } else {
            sb_add(b, (const char *)&c, 1);
        }
    }
    sb_add(b, "\"", 1);
}

/* JSON: the bytes as a string (\u00XX for what is not printable ASCII). */
static void sb_json_bytes(sbuf *b, const uint8_t *p, size_t n)
{
    sb_add(b, "\"", 1);
    for (size_t i = 0; i < n; i++) {
        uint8_t c = p[i];
        char esc[8];
        if (c == '"' || c == '\\') {
            esc[0] = '\\';
            esc[1] = (char)c;
            sb_add(b, esc, 2);
        } else if (c < 0x20 || c >= 0x7f) {
            snprintf(esc, sizeof(esc), "\\u%04x", c);
            sb_str(b, esc);
        } else {
            sb_add(b, (const char *)&c, 1);
        }
    }
    sb_add(b, "\"", 1);
}

static void ev_add(event *e, const char *k, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static void ev_add(event *e, const char *k, const char *fmt, ...)
{
    if (e->n == UAM_ARRAY_LEN(e->k))
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->v[e->n], sizeof(e->v[0]), fmt, ap);
    va_end(ap);
    e->k[e->n++] = k;
}

static void console_redraw(client *c)
{
    cl_puts(c, CONSOLE_PROMPT);
    cl_write(c, c->edit, c->edit_len);
}

static void ev_send(client *c, const event *e)
{
    sbuf b = { 0 };
    if (c->json) {
        sb_str(&b, "{\"event\":");
        sb_json_str(&b, e->kind);
        for (unsigned i = 0; i < e->n; i++) {
            sb_str(&b, ",");
            sb_json_str(&b, e->k[i]);
            sb_str(&b, ":");
            sb_json_str(&b, e->v[i]);
        }
        if (e->data_key) {
            sb_str(&b, ",");
            sb_json_str(&b, e->data_key);
            sb_str(&b, ":");
            sb_json_bytes(&b, e->data, e->data_len);
        }
        sb_str(&b, "}\n");
    } else {
        sb_str(&b, "! ");
        sb_str(&b, e->kind);
        for (unsigned i = 0; i < e->n; i++) {
            sb_str(&b, " ");
            sb_str(&b, e->k[i]);
            sb_str(&b, "=");
            for (const char *v = e->v[i]; *v; v++)   /* one word: "instruction-limit-reached" */
                sb_add(&b, *v == ' ' ? "-" : v, 1);
        }
        if (e->data_key) {
            sb_str(&b, " ");
            sb_str(&b, e->data_key);
            sb_str(&b, "=");
            sb_c_str(&b, e->data, e->data_len);
        }
        sb_str(&b, "\n");
    }
    if (c->kind == CL_CONSOLE) {
        cl_puts(c, "\r\033[K");
        cl_write(c, b.p, b.n);
        if (!c->waiting_stop)
            console_redraw(c);
    } else {
        cl_write(c, b.p, b.n);
    }
    free(b.p);
}

static void ev_broadcast(uam_monitor *mon, const event *e)
{
    for (unsigned i = 0; i < MAX_CLIENTS; i++) {
        client *c = mon->cl[i];
        if (!c || c->kind == CL_SCRIPT || (c->kind == CL_CONSOLE && !c->active))
            continue;
        ev_send(c, e);
    }
}

static const uam_cpu *cur_cpu(uam_monitor *mon)
{
    const uam_cpu *cpu = uam_machine_cpu(mon->m, uam_machine_current_cpu(mon->m));
    return cpu ? cpu : &mon->m->cpu;
}

void mon_event(uam_monitor *mon, const char *kind, ...)
{
    event e = { .kind = kind };
    va_list ap;
    va_start(ap, kind);
    for (const char *k; (k = va_arg(ap, const char *));) {
        const char *v = va_arg(ap, const char *);
        ev_add(&e, k, "%s", v ? v : "");
    }
    va_end(ap);
    ev_broadcast(mon, &e);
}

static void ev_where(uam_monitor *mon, event *e)
{
    unsigned cur = uam_machine_current_cpu(mon->m);
    const uam_cpu *cpu = cur_cpu(mon);
    if (uam_machine_ncpus(mon->m) > 1)
        ev_add(e, "cpu", "%u", cur);
    ev_add(e, "pc", "0x%llx", (unsigned long long)cpu->pc);
    ev_add(e, "icount", "%llu", (unsigned long long)uam_machine_icount(mon->m));
}

/* ---- replies ------------------------------------------------------------- */

static void reply_send(client *c, const char *id, const uam_monitor_call *call)
{
    sbuf b = { 0 };
    const char *err = call ? call->error : NULL;
    if (c->json) {
        sb_str(&b, "{");
        if (id && *id) {
            sb_str(&b, "\"id\":");
            sb_json_str(&b, id);
            sb_str(&b, ",");
        }
        if (err) {
            sb_str(&b, "\"ok\":false,\"error\":");
            sb_json_str(&b, err);
        } else {
            sb_str(&b, "\"ok\":true,\"lines\":[");
            const char *p = call && call->out ? call->out : "";
            bool first = true;
            while (*p) {
                const char *nl = strchr(p, '\n');
                size_t n = nl ? (size_t)(nl - p) : strlen(p);
                char *line = strndup(p, n);
                if (line) {
                    if (!first)
                        sb_str(&b, ",");
                    sb_json_str(&b, line);
                    free(line);
                    first = false;
                }
                p += n + (nl ? 1 : 0);
            }
            sb_str(&b, "],\"data\":{");
            for (unsigned i = 0; call && i < call->n_fields; i++) {
                if (i)
                    sb_str(&b, ",");
                sb_json_str(&b, call->names[i]);
                sb_str(&b, ":");
                sb_json_str(&b, call->values[i]);
            }
            sb_str(&b, "}");
        }
        sb_str(&b, "}\n");
    } else {
        if (!err && call) {
            if (call->out)
                sb_add(&b, call->out, call->out_len);
            for (unsigned i = 0; i < call->n_fields; i++) {
                sb_str(&b, call->names[i]);
                sb_str(&b, ": ");
                sb_str(&b, call->values[i]);
                sb_str(&b, "\n");
            }
        }
        if (id && *id) {
            sb_str(&b, "#");
            sb_str(&b, id);
            sb_str(&b, " ");
        }
        if (err) {
            sb_str(&b, "error: ");
            sb_str(&b, err);
            sb_str(&b, "\n");
        } else {
            sb_str(&b, "ok\n");
        }
    }
    if (c->kind == CL_SCRIPT) {
        /* Script output: every line marked as the monitor's, on stderr. */
        const char *p = b.p ? b.p : "";
        while (*p) {
            const char *nl = strchr(p, '\n');
            size_t n = nl ? (size_t)(nl - p) : strlen(p);
            if (!(n == 2 && !strncmp(p, "ok", 2)))
                fprintf(stderr, "uam-run: monitor: %.*s\n", (int)n, p);
            p += n + (nl ? 1 : 0);
        }
    } else {
        cl_write(c, b.p ? b.p : "", b.n);
        if (c->kind == CL_CONSOLE && c->active)
            console_redraw(c);
    }
    free(b.p);
}

static void call_free(uam_monitor_call *c)
{
    free(c->out);
    for (unsigned i = 0; i < c->n_fields; i++) {
        free(c->names[i]);
        free(c->values[i]);
    }
    free(c->names);
    free(c->values);
    free(c->error);
}

/* ---- the call API -------------------------------------------------------- */

int uam_monitor_argc(const uam_monitor_call *c) { return c->argc; }

const char *uam_monitor_arg(const uam_monitor_call *c, int i)
{
    return i >= 0 && i < c->argc ? c->argv[i] : NULL;
}

const char *uam_monitor_rest(const uam_monitor_call *c, int i)
{
    return i >= 0 && i < c->argc ? c->rest[i] : NULL;
}

uam_monitor *uam_monitor_of(const uam_monitor_call *c) { return c->mon; }

uam_machine *uam_monitor_machine(const uam_monitor *mon) { return mon->m; }

unsigned uam_monitor_cpu_index(const uam_monitor_call *c)
{
    return c->cl ? c->cl->cpu : 0;
}

uam_cpu *uam_monitor_cpu(const uam_monitor_call *c)
{
    uam_cpu *cpu = uam_machine_cpu(c->mon->m, uam_monitor_cpu_index(c));
    return cpu ? cpu : &c->mon->m->cpu;
}

void uam_monitor_set_symbolizer(uam_monitor *mon, uam_symbolize_fn fn, void *ctx)
{
    mon->sym = fn;
    mon->sym_ctx = ctx;
}

uam_symbolize_fn mon_symbolizer(const uam_monitor *mon, void **ctx)
{
    *ctx = mon->sym_ctx;
    return mon->sym;
}

bool mon_stopped(const uam_monitor *mon) { return mon->state != ST_RUNNING; }

mon_instr **mon_instr_slot(uam_monitor *mon) { return &mon->instr; }

mon_state **mon_state_slot(uam_monitor *mon) { return &mon->mstate; }

mon_io **mon_io_slot(uam_monitor *mon) { return &mon->io; }

unsigned mon_call_serial(const uam_monitor_call *c)
{
    return c->cl && c->cl->kind != CL_SCRIPT ? c->cl->serial : 0;
}

bool mon_defer_io(uam_monitor_call *c)
{
    if (!c->cl || c->cl->kind == CL_SCRIPT)
        return false;
    c->cl->waiting_io = true;
    c->deferred = true;
    return true;
}

static void client_lines(uam_monitor *mon, client *cl);
static void client_free(uam_monitor *mon, client *c);

static client *by_serial(uam_monitor *mon, unsigned serial)
{
    for (unsigned i = 0; i < MAX_CLIENTS; i++)
        if (mon->cl[i] && mon->cl[i]->serial == serial)
            return mon->cl[i];
    return NULL;
}

void mon_answer_io(uam_monitor *mon, unsigned serial, const char *error, ...)
{
    client *c = by_serial(mon, serial);
    if (!c || !c->waiting_io)
        return;
    uam_monitor_call call = { .mon = mon, .cl = c };
    if (error) {
        call.error = strdup(error);
    } else {
        va_list ap;
        va_start(ap, error);
        for (const char *k; (k = va_arg(ap, const char *));) {
            const char *v = va_arg(ap, const char *);
            uam_monitor_field(&call, k, "%s", v ? v : "");
        }
        va_end(ap);
    }
    c->waiting_io = false;
    reply_send(c, c->stop_id, &call);
    call_free(&call);
    client_lines(mon, c);
    if (c->kind == CL_STDIO && c->in_fd < 0 && !c->waiting_stop && !c->waiting_io)
        c->dead = true;   /* its input ended and all is answered: freed at the tick */
}

void mon_event_to(uam_monitor *mon, unsigned serial, const char *kind, const char *data_key,
                  const uint8_t *data, size_t len, ...)
{
    event e = { .kind = kind, .data_key = data_key, .data = data, .data_len = len };
    va_list ap;
    va_start(ap, len);
    for (const char *k; (k = va_arg(ap, const char *));) {
        const char *v = va_arg(ap, const char *);
        ev_add(&e, k, "%s", v ? v : "");
    }
    va_end(ap);
    if (!serial) {
        ev_broadcast(mon, &e);
        return;
    }
    client *c = by_serial(mon, serial);
    if (c && !(c->kind == CL_CONSOLE && !c->active))
        ev_send(c, &e);
}

/* Every host poll: output that waits is sent; clients that went away or stopped reading
 * go (here, never inside a device's write or a client's own lines). */
static void mon_tick(void *ctx)
{
    uam_monitor *mon = ctx;
    mon_io_tick(mon);
    for (unsigned i = 0; i < MAX_CLIENTS; i++) {
        client *c = mon->cl[i];
        if (c && c->pend_len && !c->dead)
            cl_flush(c);
        if (c && c->dead)
            client_free(mon, c);
    }
}

/* Can anything reach the monitor: a way in, a client, the console? */
static bool reachable(const uam_monitor *mon)
{
    if (mon->n_ls || mon->console)
        return true;
    for (unsigned i = 0; i < MAX_CLIENTS; i++)
        if (mon->cl[i] && mon->cl[i]->kind != CL_CONSOLE && !mon->cl[i]->dead)
            return true;
    return false;
}

void mon_revive(uam_monitor *mon, const char *why)
{
    mon->state = ST_STOPPED;
    mon->stop_why = why;
    mon->m->exec.stop_request = false;
    event e = { .kind = "stopped" };
    ev_add(&e, "reason", "%s", why);
    ev_where(mon, &e);
    ev_broadcast(mon, &e);
}

void uam_monitor_set_rule_symbols(uam_monitor *mon,
                                  bool (*fn)(void *ctx, const char *name, uint64_t *value),
                                  void *ctx)
{
    mon_instr_set_rule_symbols(mon, fn, ctx);
}

bool uam_monitor_running(const uam_monitor *mon) { return mon->state == ST_RUNNING; }

bool uam_monitor_starts_stopped(const uam_monitor *mon)
{
    return mon->state == ST_STOPPED && !(mon->autostart && !mon->quit);
}

void uam_monitor_print(uam_monitor_call *c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *s = vfmt(fmt, ap);
    va_end(ap);
    if (!s)
        return;
    sbuf b = { c->out, c->out_len, c->out_cap };
    sb_str(&b, s);
    if (!b.n || b.p[b.n - 1] != '\n')
        sb_add(&b, "\n", 1);
    c->out = b.p;
    c->out_len = b.n;
    c->out_cap = b.cap;
    free(s);
}

size_t uam_monitor_lines(const uam_monitor_call *c)
{
    size_t n = 0;
    for (size_t i = 0; i < c->out_len; i++)
        n += c->out[i] == '\n';
    return n;
}

void uam_monitor_field(uam_monitor_call *c, const char *name, const char *fmt, ...)
{
    if (c->n_fields == c->cap_fields) {
        unsigned cap = c->cap_fields ? 2 * c->cap_fields : 16;
        char **n = realloc(c->names, cap * sizeof(*n));
        if (n)
            c->names = n;
        char **v = n ? realloc(c->values, cap * sizeof(*v)) : NULL;
        if (v)
            c->values = v;
        if (!n || !v)
            return;
        c->cap_fields = cap;
    }
    va_list ap;
    va_start(ap, fmt);
    char *v = vfmt(fmt, ap);
    va_end(ap);
    char *n = strdup(name);
    if (!v || !n) {
        free(v);
        free(n);
        return;
    }
    c->names[c->n_fields] = n;
    c->values[c->n_fields++] = v;
}

void uam_monitor_fail(uam_monitor_call *c, const char *fmt, ...)
{
    if (c->error)
        return;   /* the first reason stands */
    va_list ap;
    va_start(ap, fmt);
    c->error = vfmt(fmt, ap);
    va_end(ap);
}

/* ---- command table ------------------------------------------------------- */

static mon_cmd *find_cmd(uam_monitor *mon, const char *name)
{
    for (unsigned i = 0; i < mon->n_cmds; i++)
        if (!strcmp(mon->cmds[i].name, name))
            return &mon->cmds[i];
    return NULL;
}

uam_status uam_monitor_add_command(uam_monitor *mon, const char *name, const char *usage,
                                   const char *help, unsigned flags, uam_monitor_fn fn,
                                   void *ctx)
{
    if (!mon || !name || !*name || !fn || strpbrk(name, " \t#\""))
        return UAM_ERR_INVALID;
    mon_cmd *c = find_cmd(mon, name);
    if (!c) {
        if (mon->n_cmds == mon->cap_cmds) {
            unsigned cap = mon->cap_cmds ? 2 * mon->cap_cmds : 16;
            mon_cmd *p = realloc(mon->cmds, cap * sizeof(*p));
            if (!p)
                return UAM_ERR_NOMEM;
            mon->cmds = p;
            mon->cap_cmds = cap;
        }
        c = &mon->cmds[mon->n_cmds++];
        *c = (mon_cmd){ .name = strdup(name) };
    } else {
        free(c->usage);
        free(c->help);
    }
    c->usage = strdup(usage ? usage : "");
    c->help = strdup(help ? help : "");
    c->flags = flags;
    c->fn = fn;
    c->ctx = ctx;
    return c->name && c->usage && c->help ? UAM_OK : UAM_ERR_NOMEM;
}

/* ---- running a line ------------------------------------------------------ */

/* Split `line` (modified in place: arguments are unquoted into `scratch`). */
static bool split_args(uam_monitor_call *c, const char *line, char *scratch, char **why)
{
    const char *p = line;
    char *o = scratch;
    c->argc = 0;
    for (;;) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            return true;
        if (c->argc == (int)MAX_ARGS) {
            *why = "too many arguments";
            return false;
        }
        c->rest[c->argc] = p;
        c->argv[c->argc++] = o;
        bool quoted = false;
        while (*p && (quoted || (*p != ' ' && *p != '\t'))) {
            if (*p == '"') {
                quoted = !quoted;
                p++;
            } else if (quoted && *p == '\\' && (p[1] == '"' || p[1] == '\\')) {
                *o++ = p[1];
                p += 2;
            } else {
                *o++ = *p++;
            }
        }
        if (quoted) {
            *why = "a quote is not closed";
            return false;
        }
        *o++ = 0;
    }
}

static void run_line(uam_monitor *mon, client *cl, char *line)
{
    char *p = line;
    while (*p == ' ' || *p == '\t')
        p++;
    size_t n = strlen(p);
    while (n && (p[n - 1] == ' ' || p[n - 1] == '\t' || p[n - 1] == '\r'))
        p[--n] = 0;
    if (!*p)
        return;
    char id[40] = "";
    if (*p == '#') {
        size_t k = 1;
        while (k < sizeof(id) && (p[k] == '_' || p[k] == '-' || (p[k] >= '0' && p[k] <= '9')
                                  || ((p[k] | 0x20) >= 'a' && (p[k] | 0x20) <= 'z')))
            k++;
        if (k == 1 || (p[k] != ' ' && p[k] != '\t' && p[k] != 0))
            return;   /* a comment */
        memcpy(id, p + 1, k - 1);
        id[k - 1] = 0;
        p += k;
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            return;
    }

    uam_monitor_call call = { .mon = mon, .cl = cl };
    char *scratch = malloc(strlen(p) + 1);
    char *why = NULL;
    if (!scratch) {
        call.error = strdup("out of memory");
    } else if (!split_args(&call, p, scratch, &why)) {
        call.error = strdup(why);
    } else {
        mon_cmd *cmd = find_cmd(mon, call.argv[0]);
        if (!cmd)
            uam_monitor_fail(&call, "unknown command '%s' (help lists them)", call.argv[0]);
        else if ((cmd->flags & UAM_MONITOR_STOPPED_ONLY) && mon->state == ST_RUNNING)
            uam_monitor_fail(&call, "%s: the machine is running (stop it first)", cmd->name);
        else if ((cmd->flags & UAM_MONITOR_SERVE_ONLY) && cl->kind == CL_EXTERNAL)
            uam_monitor_fail(&call, "%s: not from here (the debugger runs the machine: use its "
                             "own commands)", cmd->name);
        else
            cmd->fn(&call, cmd->ctx);
        if (cmd && (cmd->flags & UAM_MONITOR_CHANGES) && mon->log && !call.error && !call.deferred) {
            const unsigned long long at = (unsigned long long)uam_machine_icount(mon->m);
            if (cl->cpu != mon->log_cpu) {   /* which processor it was for, as the replay needs */
                fprintf(mon->log, "@%llu cpu %u\n", at, cl->cpu);
                mon->log_cpu = cl->cpu;
            }
            fprintf(mon->log, "@%llu %s\n", at, p);
            fflush(mon->log);
        }
    }
    mon->last_failed = call.error != NULL;
    if (call.deferred) {
        snprintf(cl->stop_id, sizeof(cl->stop_id), "%s", id);
    } else {
        reply_send(cl, id, &call);
        if (cl->kind == CL_SCRIPT && call.error && !cl->failed) {
            cl->failed = true;
            snprintf(cl->fail_why, sizeof(cl->fail_why), "%s: %s", call.argv[0] ? call.argv[0] : p,
                     call.error);
        }
    }
    call_free(&call);
    free(scratch);
}

/* Run every complete line a client has sent, unless it waits for a stop. */
static void client_lines(uam_monitor *mon, client *cl)
{
    while (!cl->waiting_stop && !cl->waiting_io && !(cl->kind == CL_SCRIPT && cl->failed)
           && !mon->quit) {
        char *nl = memchr(cl->buf, '\n', cl->len);
        size_t n;
        if (nl) {
            n = (size_t)(nl - cl->buf);
        } else if (cl->eof && cl->len) {
            n = cl->len;
        } else {
            if (cl->len >= LINE_MAX_LEN) {
                cl->len = 0;
                cl->discarding = true;   /* the rest of it, up to its newline, goes too */
                reply_send(cl, NULL, &(uam_monitor_call){ .error = "line too long" });
            }
            return;
        }
        if (cl->discarding) {
            size_t used = n + (nl ? 1 : 0);
            memmove(cl->buf, cl->buf + used, cl->len - used);
            cl->len -= used;
            cl->discarding = !nl;
            continue;
        }
        char *line = strndup(cl->buf, n);
        size_t used = n + (nl ? 1 : 0);
        memmove(cl->buf, cl->buf + used, cl->len - used);
        cl->len -= used;
        if (!line)
            return;
        run_line(mon, cl, line);
        free(line);
    }
}

static bool cl_append(client *cl, const void *p, size_t n)
{
    if (cl->len + n > cl->cap) {
        size_t cap = cl->cap ? cl->cap : 1024;
        while (cap < cl->len + n)
            cap *= 2;
        char *b = realloc(cl->buf, cap);
        if (!b)
            return false;
        cl->buf = b;
        cl->cap = cap;
    }
    memcpy(cl->buf + cl->len, p, n);
    cl->len += n;
    return true;
}

/* ---- clients ------------------------------------------------------------- */

static client *client_new(uam_monitor *mon, client_kind kind, int in_fd, int out_fd)
{
    for (unsigned i = 0; i < MAX_CLIENTS; i++) {
        if (mon->cl[i])
            continue;
        client *c = calloc(1, sizeof(*c));
        if (!c)
            return NULL;
        c->kind = kind;
        c->in_fd = in_fd;
        c->out_fd = out_fd;
        c->serial = ++mon->next_serial;
        mon->cl[i] = c;
        mon_io_client_new(mon, c->serial);
        return c;
    }
    return NULL;
}

static void client_free(uam_monitor *mon, client *c)
{
    mon_io_client_gone(mon, c->serial);
    for (unsigned i = 0; i < MAX_CLIENTS; i++)
        if (mon->cl[i] == c)
            mon->cl[i] = NULL;
    if (c->kind == CL_SOCKET || c->kind == CL_STDIO)
        uam_host_remove_fd(mon->h, c->in_fd);
    if (c->kind == CL_SOCKET)
        close(c->in_fd);
    if (mon->console == c)
        mon->console = NULL;
    free(c->buf);
    free(c->pend);
    free(c);
}

static void hello(uam_monitor *mon, client *c)
{
    event e = { .kind = "hello" };
    ev_add(&e, "protocol", "%d", UAM_MONITOR_PROTOCOL);
    ev_add(&e, "state", "%s", mon->state == ST_RUNNING ? "running"
                              : mon->state == ST_STOPPED ? "stopped" : "ended");
    ev_send(c, &e);
}

static void client_readable(void *ctx, int fd)
{
    uam_monitor *mon = ctx;
    client *c = NULL;
    for (unsigned i = 0; i < MAX_CLIENTS && !c; i++)
        if (mon->cl[i] && mon->cl[i]->in_fd == fd
            && (mon->cl[i]->kind == CL_SOCKET || mon->cl[i]->kind == CL_STDIO))
            c = mon->cl[i];
    if (!c)
        return;
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
        return;
    if (n <= 0) {
        c->eof = true;
        client_lines(mon, c);
        if (c->kind == CL_STDIO) {
            uam_host_remove_fd(mon->h, c->in_fd);   /* stdin ended: answer what is left */
            c->in_fd = -1;
            if (!c->waiting_stop && !c->waiting_io)
                client_free(mon, c);
        } else {
            client_free(mon, c);
        }
        return;
    }
    if (c->len + (size_t)n > IN_MAX) {   /* lines pile up behind a wait: too many */
        reply_send(c, NULL, &(uam_monitor_call){ .error = "too much input while waiting" });
        c->dead = true;
        return;
    }
    cl_append(c, buf, (size_t)n);
    client_lines(mon, c);
}

static void listener_readable(void *ctx, int fd)
{
    uam_monitor *mon = ctx;
    int s = accept(fd, NULL, NULL);
    if (s < 0)
        return;
    fcntl(s, F_SETFD, FD_CLOEXEC);
    fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);   /* never wait for a client */
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    client *c = client_new(mon, CL_SOCKET, s, s);
    if (!c) {
        const char *msg = "error: too many monitor clients\n";
        write_fd(s, true, msg, strlen(msg));
        close(s);
        return;
    }
    if (uam_host_add_fd(mon->h, s, client_readable, mon) != UAM_OK) {
        client_free(mon, c);
        return;
    }
    hello(mon, c);
}

uam_status uam_monitor_listen(uam_monitor *mon, const char *spec, char *err, size_t errlen)
{
    if (!err || !errlen) {
        static char dummy[2];
        err = dummy;
        errlen = sizeof(dummy);
    }
    err[0] = 0;
    if (!strcmp(spec, "stdio")) {
        client *c = client_new(mon, CL_STDIO, STDIN_FILENO, STDOUT_FILENO);
        if (!c || uam_host_add_fd(mon->h, STDIN_FILENO, client_readable, mon) != UAM_OK) {
            if (c)
                client_free(mon, c);
            snprintf(err, errlen, "stdio: no room for another client");
            return UAM_ERR_NOMEM;
        }
        hello(mon, c);
        return UAM_OK;
    }
    if (mon->n_ls == MAX_LISTENERS) {
        snprintf(err, errlen, "%s: at most %u ways in", spec, MAX_LISTENERS);
        return UAM_ERR_NOMEM;
    }
    listener *l = &mon->ls[mon->n_ls];
    *l = (listener){ .fd = -1 };
    int fd = -1;
    if (!strncmp(spec, "unix:", 5)) {
        const char *path = spec + 5;
        struct sockaddr_un a = { .sun_family = AF_UNIX };
        if (!*path || strlen(path) >= sizeof(a.sun_path)) {
            snprintf(err, errlen, "%s: the path is empty or too long", spec);
            return UAM_ERR_INVALID;
        }
        strcpy(a.sun_path, path);
        struct stat st;
        if (lstat(path, &st) == 0) {
            if (!S_ISSOCK(st.st_mode)) {
                snprintf(err, errlen, "%s: a file that is not a socket is in the way", spec);
                return UAM_ERR_FAULT;
            }
            unlink(path);   /* a stale socket of an earlier run */
        }
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd >= 0)
            fcntl(fd, F_SETFD, FD_CLOEXEC);
        if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(fd, 4) != 0) {
            snprintf(err, errlen, "%s: %s", spec, strerror(errno));
            if (fd >= 0)
                close(fd);
            return UAM_ERR_FAULT;
        }
        l->is_unix = true;
        strcpy(l->path, path);
    } else if (!strncmp(spec, "tcp:", 4)) {
        char *end;
        long port = strtol(spec + 4, &end, 10);
        if (end == spec + 4 || *end || port <= 0 || port > 65535) {
            snprintf(err, errlen, "%s: expected tcp:PORT (127.0.0.1 only)", spec);
            return UAM_ERR_INVALID;
        }
        struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port),
                                 .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
        int one = 1;
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0) {
            fcntl(fd, F_SETFD, FD_CLOEXEC);
            setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        }
        if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(fd, 4) != 0) {
            snprintf(err, errlen, "%s: %s", spec, strerror(errno));
            if (fd >= 0)
                close(fd);
            return UAM_ERR_FAULT;
        }
    } else {
        snprintf(err, errlen, "%s: expected unix:PATH, tcp:PORT or stdio", spec);
        return UAM_ERR_INVALID;
    }
    if (uam_host_add_fd(mon->h, fd, listener_readable, mon) != UAM_OK) {
        close(fd);
        if (l->is_unix)
            unlink(l->path);
        snprintf(err, errlen, "%s: the host loop has no room for it", spec);
        return UAM_ERR_NOMEM;
    }
    l->fd = fd;
    mon->n_ls++;
    uam_log(UAM_LOG_INFO, "monitor", "listening on %s", spec);
    return UAM_OK;
}

/* ---- the console (Ctrl-A m) ------------------------------------------------ */

static void console_show(uam_monitor *mon)
{
    client *c = mon->console;
    c->active = true;
    cl_puts(c, "\n[ember monitor - Ctrl-A m returns to the guest, help lists commands]\n");
    console_redraw(c);
}

static void console_enter(void *ctx)
{
    uam_monitor *mon = ctx;
    if (mon->console)
        console_show(mon);
}

static void console_leave(void *ctx)
{
    uam_monitor *mon = ctx;
    if (!mon->console)
        return;
    mon->console->active = false;
    cl_puts(mon->console, "\r\033[K");
    if (mon->state != ST_RUNNING)
        cl_puts(mon->console, mon->state == ST_STOPPED
                ? "[the machine is stopped: Ctrl-A m, then cont, lets it go on]\n"
                : "[the run has ended: Ctrl-A m, then quit, or Ctrl-A x]\n");
}

static void console_input(void *ctx, const uint8_t *p, size_t n)
{
    uam_monitor *mon = ctx;
    client *c = mon->console;
    if (!c)
        return;
    for (size_t i = 0; i < n; i++) {
        uint8_t ch = p[i];
        if (c->esc_seq == 1) {          /* ESC seen: [ starts a sequence, anything else ends it */
            c->esc_seq = ch == '[' || ch == 'O' ? 2 : 0;
            continue;
        }
        if (c->esc_seq == 2) {          /* a sequence ends with its final byte */
            if (ch >= 0x40 && ch <= 0x7e)
                c->esc_seq = 0;
            continue;
        }
        if (ch == 0x1b) {
            c->esc_seq = 1;
        } else if (ch == '\r' || ch == '\n') {
            cl_puts(c, "\n");
            cl_append(c, c->edit, c->edit_len);
            cl_append(c, "\n", 1);
            bool had = c->edit_len != 0;
            c->edit_len = 0;
            if (!had)
                console_redraw(c);
            client_lines(mon, c);
            if (!mon->console)
                return;
        } else if (ch == 0x7f || ch == 0x08) {
            if (c->edit_len) {
                c->edit_len--;
                cl_puts(c, "\b \b");
            }
        } else if (ch == 0x15) {        /* Ctrl-U: the line goes */
            c->edit_len = 0;
            cl_puts(c, "\r\033[K");
            console_redraw(c);
        } else if (ch == 0x03) {        /* Ctrl-C: the line goes */
            c->edit_len = 0;
            cl_puts(c, "^C\n");
            console_redraw(c);
        } else if (ch >= 0x20 && c->edit_len < sizeof(c->edit)) {
            c->edit[c->edit_len++] = (char)ch;
            cl_write(c, (const char *)&ch, 1);
        }
    }
}

bool uam_monitor_use_console(uam_monitor *mon, uam_chardev *console)
{
    if (mon->console)
        return false;
    uam_chardev_escape e = { .key = 'm', .enter = console_enter, .leave = console_leave,
                             .input = console_input, .ctx = mon };
    if (!uam_chardev_stdio_set_escape(console, &e))
        return false;
    client *c = client_new(mon, CL_CONSOLE, -1, STDOUT_FILENO);
    if (!c) {
        uam_chardev_stdio_set_escape(console, NULL);
        return false;
    }
    mon->console = c;
    mon->console_cd = console;
    return true;
}

/* Stopped or ended: put the console in front, so the prompt is there. */
static void console_to_front(uam_monitor *mon)
{
    if (!mon->console || mon->console->active)
        return;
    uam_chardev_stdio_divert(mon->console_cd, true);
    console_show(mon);
}

/* ---- stop and end ---------------------------------------------------------- */

static void answer_stops(uam_monitor *mon, const char *error)
{
    for (unsigned i = 0; i < MAX_CLIENTS; i++) {
        client *c = mon->cl[i];
        if (!c || !c->waiting_stop)
            continue;
        uam_monitor_call call = { .mon = mon, .cl = c };
        if (error && !(c->wait_ok_at_end && mon->state == ST_ENDED)) {
            call.error = strdup(error);
        } else {
            uam_monitor_field(&call, "state", "%s", mon->state == ST_ENDED ? "ended" : "stopped");
            uam_monitor_field(&call, "reason", "%s", mon->state == ST_ENDED
                              ? uam_stop_reason_str(mon->end_why) : mon->stop_why);
            uam_monitor_field(&call, "pc", "0x%llx", (unsigned long long)cur_cpu(mon)->pc);
            uam_monitor_field(&call, "icount", "%llu",
                              (unsigned long long)uam_machine_icount(mon->m));
        }
        c->waiting_stop = false;
        c->wait_ok_at_end = false;
        reply_send(c, c->stop_id, &call);
        call_free(&call);
    }
}

/* Lines that waited for a stop run now; a stdio client whose input ended goes. */
static void run_waiting_lines(uam_monitor *mon)
{
    for (unsigned i = 0; i < MAX_CLIENTS; i++) {
        client *c = mon->cl[i];
        if (!c)
            continue;
        client_lines(mon, c);
        if (mon->cl[i] == c && c->kind == CL_STDIO && c->in_fd < 0 && !c->waiting_stop
            && !c->waiting_io)
            client_free(mon, c);
    }
}

static void enter_stopped(uam_monitor *mon, const char *why)
{
    mon->state = ST_STOPPED;
    mon->stop_why = why;
    console_to_front(mon);
    answer_stops(mon, NULL);
    event e = { .kind = "stopped" };
    ev_add(&e, "reason", "%s", why);
    ev_where(mon, &e);
    ev_broadcast(mon, &e);
    run_waiting_lines(mon);
}

static void enter_ended(uam_monitor *mon, uam_stop_reason why, int64_t status, bool hold)
{
    mon->state = ST_ENDED;
    mon->end_why = why;
    mon->end_status = status;
    if (hold)
        console_to_front(mon);
    char what[96];
    snprintf(what, sizeof(what), "the run ended (%s)", uam_stop_reason_str(why));
    answer_stops(mon, hold ? NULL : what);
    mon_io_ended(mon);
    event e = { .kind = "ended" };
    ev_add(&e, "reason", "%s", uam_stop_reason_str(why));
    ev_add(&e, "status", "%lld", (long long)status);
    ev_where(mon, &e);
    ev_broadcast(mon, &e);
    if (hold)
        run_waiting_lines(mon);
}

/* ---- ember's own commands -------------------------------------------------- */

static void cmd_help(uam_monitor_call *c, void *ctx)
{
    uam_monitor *mon = ctx;
    if (c->argc > 1) {
        mon_cmd *cmd = find_cmd(mon, c->argv[1]);
        if (!cmd) {
            uam_monitor_fail(c, "help: no command '%s'", c->argv[1]);
            return;
        }
        uam_monitor_print(c, "%s%s%s", cmd->name, *cmd->usage ? " " : "", cmd->usage);
        uam_monitor_print(c, "  %s%s", cmd->help,
                          cmd->flags & UAM_MONITOR_STOPPED_ONLY ? " (machine stopped)" : "");
        return;
    }
    for (unsigned i = 0; i < mon->n_cmds; i++) {
        char head[64];
        snprintf(head, sizeof(head), "%s%s%s", mon->cmds[i].name, *mon->cmds[i].usage ? " " : "",
                 mon->cmds[i].usage);
        uam_monitor_print(c, "%-22s %s", head, mon->cmds[i].help);
    }
}

static void cmd_status(uam_monitor_call *c, void *ctx)
{
    uam_monitor *mon = ctx;
    uam_machine *m = mon->m;
    uam_monitor_field(c, "state", "%s", mon->state == ST_RUNNING ? "running"
                                        : mon->state == ST_STOPPED ? "stopped" : "ended");
    if (mon->state == ST_STOPPED)
        uam_monitor_field(c, "reason", "%s", mon->stop_why);
    if (mon->state == ST_ENDED) {
        uam_monitor_field(c, "reason", "%s", uam_stop_reason_str(mon->end_why));
        uam_monitor_field(c, "status", "%lld", (long long)mon->end_status);
    }
    uam_monitor_field(c, "board", "%s", m->board_name ? m->board_name : "?");
    uam_monitor_field(c, "icount", "%llu", (unsigned long long)uam_machine_icount(m));
    uam_monitor_field(c, "time", "%llu", (unsigned long long)uam_sched_now(&m->sched));
    unsigned n = uam_machine_ncpus(m);
    uam_monitor_field(c, "cpus", "%u", n);
    for (unsigned i = 0; i < n; i++) {
        const uam_cpu *cpu = uam_machine_cpu(m, i);
        uam_monitor_print(c, "cpu %u pc=0x%llx icount=%llu%s", i, (unsigned long long)cpu->pc,
                          (unsigned long long)cpu->icount, cpu->waiting ? " waiting" : "");
    }
}

static void cmd_stop(uam_monitor_call *c, void *ctx)
{
    uam_monitor *mon = ctx;
    if (mon->state == ST_ENDED) {
        uam_monitor_fail(c, "the run has ended (%s)", uam_stop_reason_str(mon->end_why));
        return;
    }
    if (mon->state == ST_STOPPED) {
        mon->autostart = false;   /* before the start: stay stopped */
        uam_monitor_field(c, "state", "stopped");
        return;
    }
    if (!mon->in_run) {           /* "cont" earlier on this line batch: not started yet */
        mon->state = ST_STOPPED;
        mon->stop_why = "monitor";
        uam_monitor_field(c, "state", "stopped");
        return;
    }
    /* A stop already on its way (a signal, a quit, the firmware's own) keeps its status:
     * the client hears whatever it turns out to be. */
    if (!mon->m->exec.stop_request) {
        mon->stop_wanted = true;
        uam_exec_stop(&mon->m->exec, 0);
    }
    c->cl->waiting_stop = true;
    c->cl->wait_ok_at_end = false;
    c->deferred = true;
}

static void cmd_cont(uam_monitor_call *c, void *ctx)
{
    uam_monitor *mon = ctx;
    if (mon->state == ST_ENDED) {
        uam_monitor_fail(c, "the run has ended (%s)", uam_stop_reason_str(mon->end_why));
        return;
    }
    mon->state = ST_RUNNING;
    mon->stop_wanted = false;
    uam_monitor_field(c, "state", "running");
    /* From the console: the terminal goes back to the guest. */
    if (c->cl->kind == CL_CONSOLE && c->cl->active) {
        reply_send(c->cl, NULL, &(uam_monitor_call){ 0 });
        c->deferred = true;   /* answered just now, before the terminal changes hands */
        c->cl->stop_id[0] = 0;
        c->cl->active = false;
        cl_puts(c->cl, "\r\033[K");
        uam_chardev_stdio_divert(mon->console_cd, false);
    }
}

static void cmd_wait(uam_monitor_call *c, void *ctx)
{
    uam_monitor *mon = ctx;
    if (mon->state != ST_RUNNING) {
        uam_monitor_field(c, "state", "%s", mon->state == ST_ENDED ? "ended" : "stopped");
        uam_monitor_field(c, "reason", "%s", mon->state == ST_ENDED
                          ? uam_stop_reason_str(mon->end_why) : mon->stop_why);
        uam_monitor_field(c, "pc", "0x%llx", (unsigned long long)cur_cpu(mon)->pc);
        uam_monitor_field(c, "icount", "%llu", (unsigned long long)uam_machine_icount(mon->m));
        return;
    }
    if (c->cl->kind == CL_SCRIPT) {
        uam_monitor_fail(c, "wait: not in a script (the machine has not started)");
        return;
    }
    c->cl->waiting_stop = true;   /* answered by the next stop, or the end */
    c->cl->wait_ok_at_end = true;
    c->deferred = true;
}

static void cmd_quit(uam_monitor_call *c, void *ctx)
{
    uam_monitor *mon = ctx;
    long code = 0;
    if (c->argc > 1) {
        char *end;
        code = strtol(c->argv[1], &end, 0);
        if (*end || code < 0 || code > 123) {
            uam_monitor_fail(c, "quit: the exit status is 0 to 123");
            return;
        }
        mon->quit_has_code = true;
    }
    mon->quit = true;
    mon->quit_code = code;
    if (mon->in_run)
        uam_exec_stop(&mon->m->exec, code);
}

static void cmd_json(uam_monitor_call *c, void *ctx)
{
    (void)ctx;
    const char *a = c->argc > 1 ? c->argv[1] : "";
    if (!strcmp(a, "on"))
        c->cl->json = true;
    else if (!strcmp(a, "off"))
        c->cl->json = false;
    else
        uam_monitor_fail(c, "json: on or off");
}

/* ---- run control: step, until, run-for, cpu -------------------------------- */

static uam_hook_action until_hook(void *ctx, uam_cpu *cpu, uint64_t pc)
{
    uam_monitor *mon = ctx;
    (void)cpu;
    (void)pc;
    /* Where it already stands does not count: "until" means reached again. */
    if (!mon->until_on || uam_machine_icount(mon->m) == mon->until_from)
        return UAM_HOOK_CONTINUE;
    mon->until_hit = true;
    return UAM_HOOK_STOP;
}

static void until_hooks(uam_monitor *mon, bool add)
{
    const uint64_t sb = uam_isa_pc_state_bits(mon->m->cpu.isa);
    if (add) {
        uam_machine_add_pc_hook(mon->m, mon->until_pc, until_hook, mon);
        if (sb)
            uam_machine_add_pc_hook(mon->m, mon->until_pc | sb, until_hook, mon);
    } else {
        uam_machine_remove_pc_hook(mon->m, mon->until_pc, until_hook, mon);
        if (sb)
            uam_machine_remove_pc_hook(mon->m, mon->until_pc | sb, until_hook, mon);
    }
}

/* The run that was for a step, an until or a run-for is over, however it ended. */
static void clear_targets(uam_monitor *mon)
{
    mon->step_end = 0;
    if (mon->until_on) {
        until_hooks(mon, false);
        mon->until_on = false;
    }
    mon->until_hit = false;
    if (mon->runfor_on) {
        mon->m->exec.max_time = mon->saved_max_time;
        mon->runfor_on = false;
    }
}

/* Start a run a client waits for; false (and the call fails) if it cannot. */
static bool begin_targeted(uam_monitor_call *c, uam_monitor *mon)
{
    if (mon->state != ST_STOPPED) {
        uam_monitor_fail(c, "%s: the machine %s", c->argv[0],
                         mon->state == ST_ENDED ? "has ended" : "is running (stop it first)");
        return false;
    }
    if (c->cl->kind == CL_SCRIPT) {
        uam_monitor_fail(c, "%s: not in a script (the machine has not started)", c->argv[0]);
        return false;
    }
    if (mon->step_end || mon->until_on || mon->runfor_on) {
        uam_monitor_fail(c, "%s: another client's step is under way", c->argv[0]);
        return false;
    }
    return true;
}

static void go_targeted(uam_monitor_call *c, uam_monitor *mon)
{
    mon->state = ST_RUNNING;
    c->cl->waiting_stop = true;
    c->cl->wait_ok_at_end = false;
    c->deferred = true;
}

static bool parse_u64(const char *s, uint64_t *v)
{
    char *end;
    if (!s || !*s || *s == '-')
        return false;
    *v = strtoull(s, &end, 0);
    return !*end;
}

static void cmd_step(uam_monitor_call *c, void *ctx)
{
    uam_monitor *mon = ctx;
    uint64_t n = 1;
    if (c->argc > 1 && (!parse_u64(c->argv[1], &n) || !n)) {
        uam_monitor_fail(c, "step: N is a count of instructions, at least 1");
        return;
    }
    if (!begin_targeted(c, mon))
        return;
    mon->step_end = uam_machine_icount(mon->m) + n;
    go_targeted(c, mon);
}

static void cmd_until(uam_monitor_call *c, void *ctx)
{
    uam_monitor *mon = ctx;
    uint64_t pc;
    if (c->argc != 2 || !parse_u64(c->argv[1], &pc)) {
        uam_monitor_fail(c, "until: PC, an address");
        return;
    }
    if (!begin_targeted(c, mon))
        return;
    mon->until_pc = pc & ~uam_isa_pc_state_bits(mon->m->cpu.isa);
    mon->until_from = uam_machine_icount(mon->m);
    mon->until_on = true;
    until_hooks(mon, true);
    go_targeted(c, mon);
}

static void cmd_run_for(uam_monitor_call *c, void *ctx)
{
    uam_monitor *mon = ctx;
    uint64_t ticks;
    if (c->argc != 2 || !parse_u64(c->argv[1], &ticks) || !ticks) {
        uam_monitor_fail(c, "run-for: TICKS, scheduler ticks of virtual time (status: time)");
        return;
    }
    if (!begin_targeted(c, mon))
        return;
    uam_exec *x = &mon->m->exec;
    uint64_t now = uam_sched_now(&mon->m->sched);
    uint64_t until = ticks < UAM_SCHED_TIME_MAX - now ? now + ticks : UAM_SCHED_TIME_MAX;
    mon->saved_max_time = x->max_time;
    mon->runfor_end = until;
    mon->runfor_on = true;
    if (!x->max_time || until < x->max_time)
        x->max_time = until;
    go_targeted(c, mon);
}

static void cmd_cpu(uam_monitor_call *c, void *ctx)
{
    uam_monitor *mon = ctx;
    unsigned n = uam_machine_ncpus(mon->m);
    if (c->argc > 1) {
        uint64_t i;
        if (!parse_u64(c->argv[1], &i) || i >= n) {
            uam_monitor_fail(c, "cpu: 0 to %u", n - 1);
            return;
        }
        c->cl->cpu = (unsigned)i;
    }
    uam_monitor_field(c, "cpu", "%u", c->cl->cpu);
    uam_monitor_field(c, "cpus", "%u", n);
}

/* ---- create, script, serve ------------------------------------------------- */

uam_monitor *uam_monitor_create(uam_machine *m, uam_host *h, const uam_monitor_config *cfg)
{
    if (!m || !h)
        return NULL;
    uam_monitor *mon = calloc(1, sizeof(*mon));
    if (!mon)
        return NULL;
    mon->m = m;
    mon->h = h;
    if (cfg)
        mon->cfg = *cfg;
    mon->state = ST_STOPPED;
    mon->stop_why = "start";
    mon->autostart = !mon->cfg.wait;
    static const struct {
        const char *name, *usage, *help;
        unsigned    flags;
        uam_monitor_fn fn;
    } own[] = {
        { "help",   "[COMMAND]", "list the commands, or say what one does", 0, cmd_help },
        { "status", "",          "the machine's state: running, stopped or ended, and where", 0,
          cmd_status },
        { "stop",   "",          "stop the machine (answers once it has stopped)", UAM_MONITOR_SERVE_ONLY, cmd_stop },
        { "cont",   "",          "let a stopped machine go on", UAM_MONITOR_SERVE_ONLY, cmd_cont },
        { "wait",   "",          "answer once the machine stops (a breakpoint, a guard...) or "
          "the run ends", UAM_MONITOR_SERVE_ONLY, cmd_wait },
        { "quit",   "[STATUS]",  "end the run (uam-run exits with STATUS, 0-123)", UAM_MONITOR_SERVE_ONLY, cmd_quit },
        { "json",   "on|off",    "answer this client in JSON, one object per line", 0, cmd_json },
        { "step",   "[N]",       "run N instructions (1), then stop (answers then)", UAM_MONITOR_SERVE_ONLY,
          cmd_step },
        { "until",  "PC",        "run until a processor reaches PC again, then stop", UAM_MONITOR_SERVE_ONLY,
          cmd_until },
        { "run-for", "TICKS",    "run for TICKS of virtual time (scheduler ticks), then stop", UAM_MONITOR_SERVE_ONLY,
          cmd_run_for },
        { "cpu",    "[N]",       "select the processor later commands look at (this client)", 0,
          cmd_cpu },
    };
    for (size_t i = 0; i < UAM_ARRAY_LEN(own); i++)
        if (uam_monitor_add_command(mon, own[i].name, own[i].usage, own[i].help, own[i].flags,
                                    own[i].fn, mon) != UAM_OK) {
            uam_monitor_destroy(mon);
            return NULL;
        }
    if (mon_add_inspect_commands(mon) != UAM_OK || mon_add_instr_commands(mon) != UAM_OK
        || mon_add_state_commands(mon) != UAM_OK || mon_add_io_commands(mon) != UAM_OK) {
        uam_monitor_destroy(mon);
        return NULL;
    }
    if (take_offered(mon) != UAM_OK) {
        uam_monitor_destroy(mon);
        return NULL;
    }
    uam_host_set_tick(h, mon_tick, mon);
    return mon;
}

void uam_monitor_destroy(uam_monitor *mon)
{
    if (!mon)
        return;
    clear_targets(mon);
    uam_host_set_tick(mon->h, NULL, NULL);
    mon_instr_free(mon);
    mon_state_free(mon);
    for (unsigned i = 0; i < MAX_CLIENTS; i++)
        if (mon->cl[i])
            client_free(mon, mon->cl[i]);
    for (unsigned i = 0; i < mon->n_ls; i++) {
        uam_host_remove_fd(mon->h, mon->ls[i].fd);
        close(mon->ls[i].fd);
        if (mon->ls[i].is_unix)
            unlink(mon->ls[i].path);
    }
    mon_io_free(mon);   /* after the clients: their expects and taps went with them */
    if (mon->console_cd) {
        uam_chardev_stdio_divert(mon->console_cd, false);
        uam_chardev_stdio_set_escape(mon->console_cd, NULL);
    }
    forget_offered(mon->m);
    for (size_t i = 0; i < mon->n_timed; i++)
        free(mon->timed[i].line);
    free(mon->timed);
    if (mon->timed_cl) {
        free(mon->timed_cl->buf);
        free(mon->timed_cl);
    }
    for (unsigned i = 0; i < mon->n_cmds; i++) {
        free(mon->cmds[i].name);
        free(mon->cmds[i].usage);
        free(mon->cmds[i].help);
    }
    free(mon->cmds);
    free(mon);
}

uam_status uam_monitor_run_script(uam_monitor *mon, const char *path, char *err, size_t errlen)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errlen, "%s: %s", path, strerror(errno));
        return UAM_ERR_FAULT;
    }
    /* One script client for the whole session, so a "cpu N" carries over to later lines. */
    if (!mon->timed_cl && !(mon->timed_cl = calloc(1, sizeof(*mon->timed_cl)))) {
        fclose(f);
        return UAM_ERR_NOMEM;
    }
    client sc = { .kind = CL_SCRIPT, .in_fd = -1, .out_fd = -1, .eof = true };
    char line[LINE_MAX_LEN];
    unsigned lineno = 0;
    const uint64_t now = uam_machine_icount(mon->m);
    /* "@N COMMAND": when the machine reaches instruction N; the rest at once. */
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        if (line[0] != '@' || (mon->n_timed == 0 && strtoull(line + 1, NULL, 0) <= now
                               && line[1] >= '0' && line[1] <= '9')) {
            char *text = line[0] == '@' ? strpbrk(line, " \t") : line;
            if (text)
                cl_append(&sc, text, strlen(text));
            continue;
        }
        char *end;
        uint64_t at = strtoull(line + 1, &end, 0);
        if (end == line + 1 || (*end != ' ' && *end != '\t')) {
            snprintf(err, errlen, "%s:%u: expected @ICOUNT COMMAND", path, lineno);
            fclose(f);
            free(sc.buf);
            return UAM_ERR_REJECTED;
        }
        struct timed *t = realloc(mon->timed, (mon->n_timed + 1) * sizeof(*t));
        if (!t) {
            fclose(f);
            free(sc.buf);
            return UAM_ERR_NOMEM;
        }
        mon->timed = t;
        t[mon->n_timed].at = at;
        end[strcspn(end, "\r\n")] = 0;
        t[mon->n_timed].line = strdup(end + 1);
        if (!t[mon->n_timed].line) {
            fclose(f);
            free(sc.buf);
            return UAM_ERR_NOMEM;
        }
        mon->n_timed++;
    }
    fclose(f);
    sc.cpu = mon->timed_cl->cpu;
    client_lines(mon, &sc);
    mon->timed_cl->cpu = sc.cpu;
    free(sc.buf);
    if (sc.failed) {
        snprintf(err, errlen, "%s: %s", path, sc.fail_why);
        return UAM_ERR_REJECTED;
    }
    return UAM_OK;
}

/* ---- uam_monitor_execute, the log, offered commands, timed lines ---------------- */

uam_status uam_monitor_execute(uam_monitor *mon, const char *line, char **out)
{
    client ex = { .kind = CL_EXTERNAL, .in_fd = -1, .out_fd = -1 };
    if (!mon->served && mon->state == ST_STOPPED)
        mon->stop_why = "debugger";
    char *copy = strdup(line ? line : "");
    if (!copy)
        return UAM_ERR_NOMEM;
    size_t n = strlen(copy);
    if (n && copy[n - 1] == '\n')
        copy[n - 1] = 0;
    run_line(mon, &ex, copy);
    free(copy);
    /* The closing "ok" says nothing a debugger's user needs. */
    if (ex.len >= 3 && !memcmp(ex.buf + ex.len - 3, "ok\n", 3)
        && (ex.len == 3 || ex.buf[ex.len - 4] == '\n'))
        ex.len -= 3;
    cl_append(&ex, "", 1);   /* the text's end */
    const bool failed = mon->last_failed;
    if (out)
        *out = ex.buf;
    else
        free(ex.buf);
    return failed ? UAM_ERR_REJECTED : UAM_OK;
}

void uam_monitor_set_log(uam_monitor *mon, FILE *out)
{
    mon->log = out;
    if (out) {
        fprintf(out, "# monitor log: give it to --monitor-script with the same command line "
                "to run the session again\n");
        fflush(out);
    }
}

uam_status uam_monitor_offer_command(uam_machine *m, const char *name, const char *usage,
                                     const char *help, unsigned flags, uam_monitor_fn fn,
                                     void *ctx)
{
    if (!m || !name || !fn)
        return UAM_ERR_INVALID;
    offered *o = calloc(1, sizeof(*o));
    if (!o)
        return UAM_ERR_NOMEM;
    *o = (offered){ .m = m, .name = strdup(name), .usage = strdup(usage ? usage : ""),
                    .help = strdup(help ? help : ""), .flags = flags, .fn = fn, .ctx = ctx };
    if (!o->name || !o->usage || !o->help) {
        free(o->name);
        free(o->usage);
        free(o->help);
        free(o);
        return UAM_ERR_NOMEM;
    }
    offered **tail = &g_offered;
    while (*tail)
        tail = &(*tail)->next;
    *tail = o;
    return UAM_OK;
}

/* The board's offered commands, once the monitor's own are in (a board may replace one). */
static uam_status take_offered(uam_monitor *mon)
{
    for (offered *o = g_offered; o; o = o->next)
        if (o->m == mon->m) {
            uam_status st = uam_monitor_add_command(mon, o->name, o->usage, o->help, o->flags,
                                                    o->fn, o->ctx);
            if (st != UAM_OK)
                return st;
        }
    return UAM_OK;
}

/* The machine's offered commands go with its monitor. */
static void forget_offered(const uam_machine *m)
{
    for (offered **p = &g_offered; *p;) {
        offered *o = *p;
        if (o->m != m) {
            p = &o->next;
            continue;
        }
        *p = o->next;
        free(o->name);
        free(o->usage);
        free(o->help);
        free(o);
    }
}

/* The next timed line's instruction count (UINT64_MAX: none). */
static uint64_t next_timed_at(const uam_monitor *mon)
{
    return mon->next_timed < mon->n_timed ? mon->timed[mon->next_timed].at : UINT64_MAX;
}

/* Run the timed lines that are due. True if one ran. */
static bool run_timed(uam_monitor *mon)
{
    bool ran = false;
    while (mon->next_timed < mon->n_timed
           && uam_machine_icount(mon->m) >= mon->timed[mon->next_timed].at) {
        if (!mon->timed_cl && !(mon->timed_cl = calloc(1, sizeof(*mon->timed_cl))))
            return ran;
        client *c = mon->timed_cl;
        c->kind = CL_SCRIPT;
        c->in_fd = c->out_fd = -1;
        c->failed = false;
        char *line = mon->timed[mon->next_timed++].line;
        /* The machine stands still between two runs here: as stopped, for the line. */
        const run_state was = mon->state;
        const char *was_why = mon->stop_why;
        if (was == ST_RUNNING) {
            mon->state = ST_STOPPED;
            mon->stop_why = "script";
        }
        run_line(mon, c, line);
        if (mon->state == ST_STOPPED && was == ST_RUNNING) {
            mon->state = ST_RUNNING;
            mon->stop_why = was_why;
        }
        if (c->failed)
            fprintf(stderr, "uam-run: monitor: @%llu: %s (the session goes on)\n",
                    (unsigned long long)uam_machine_icount(mon->m), c->fail_why);
        ran = true;
    }
    return ran;
}

uam_stop_reason uam_monitor_serve(uam_monitor *mon, uint64_t max_insns)
{
    uam_exec *x = &mon->m->exec;
    const uint64_t end = max_insns ? uam_machine_icount(mon->m) + max_insns : UINT64_MAX;
    mon->served = true;
    run_timed(mon);   /* lines due at the start (and those of a log replayed) */
    if (mon->state == ST_STOPPED && mon->autostart && !mon->quit)
        mon->state = ST_RUNNING;
    if (mon->state == ST_STOPPED && !mon->quit)
        enter_stopped(mon, mon->stop_why);
    else if (mon->state == ST_RUNNING)
        mon_event(mon, "running", NULL);   /* a client that heard "stopped" at its hello */

    for (;;) {
        bool waited = false;
        /* The waits poll with a bound: a signal that lands just before the poll is seen
         * within a tenth of a second, and output that waits for a slow client goes out. */
        while (mon->state == ST_STOPPED && !mon->quit && !x->stop_request) {
            if (!reachable(mon)) {
                uam_log(UAM_LOG_ERROR, "monitor", "the machine is stopped and nothing can reach "
                        "the monitor to let it go on (a script's stop with no --monitor?)");
                return UAM_STOP_ERROR;
            }
            uam_host_poll(mon->h, 100);
            waited = true;
        }
        if (mon->quit || x->stop_request) {
            /* "quit", or a signal / the console's Ctrl-A x while stopped */
            if (mon->quit)
                x->stop_status = mon->quit_code;
            return UAM_STOP_REQUEST;
        }
        if (waited)
            mon_event(mon, "running", NULL);

        uam_host_resync(mon->h);
        uam_stop_reason why;
        uint64_t done = uam_machine_icount(mon->m);
        uint64_t limit = mon->step_end && mon->step_end < end ? mon->step_end : end;
        mon_instr_before_run(mon);
        limit = mon_instr_limit(mon, limit);
        if (next_timed_at(mon) > done && next_timed_at(mon) < limit)
            limit = next_timed_at(mon);
        if (limit != UINT64_MAX && done >= limit) {
            why = UAM_STOP_INSN_LIMIT;
        } else {
            mon->in_run = true;
            why = uam_exec_run(x, limit == UINT64_MAX ? 0 : limit - done);
            mon->in_run = false;
        }
        if (mon->quit) {
            clear_targets(mon);
            x->stop_request = false;
            x->stop_status = mon->quit_code;
            answer_stops(mon, "the run was ended");
            return UAM_STOP_REQUEST;
        }
        /* Lines of a replayed session that were due here (after instruction N). */
        const bool timed = run_timed(mon);
        if (mon->quit) {
            clear_targets(mon);
            x->stop_request = false;
            x->stop_status = mon->quit_code;
            return UAM_STOP_REQUEST;
        }
        /* Stopped for the monitor, or for good? */
        const uint64_t icount = uam_machine_icount(mon->m);
        const uint64_t now = uam_sched_now(&mon->m->sched);
        const char *mine = NULL, *claimed = mon_instr_claim(mon, why);
        if (why == UAM_STOP_REQUEST && mon->stop_wanted && x->stop_status == 0) {
            mine = "monitor";
            x->stop_request = false;
        } else if (why == UAM_STOP_INSN_LIMIT && mon->step_end && icount >= mon->step_end
                   && icount < end) {
            mine = "step";
        } else if (claimed && *claimed) {
            snprintf(mon->why_buf, sizeof(mon->why_buf), "%s", claimed);
            mine = mon->why_buf;
        } else if (why == UAM_STOP_HOOK && mon->until_hit) {
            mine = "until";
        } else if (why == UAM_STOP_TIME_LIMIT && mon->runfor_on && now >= mon->runfor_end
                   && (!mon->saved_max_time || now < mon->saved_max_time)) {
            mine = "run-for";
        }
        if (!mine && (claimed || timed) && why == UAM_STOP_INSN_LIMIT && icount < end)
            continue;   /* only the step over a guard, or a timed line: the run goes on (a
                         * stop asked for meanwhile is still pending, and comes next) */
        clear_targets(mon);
        if (mine && mon->stop_wanted && x->stop_status == 0)
            x->stop_request = false;   /* that stop is this one: it must not end the run */
        mon->stop_wanted = false;
        if (mine) {
            enter_stopped(mon, mine);
            continue;
        }

        const int64_t status = x->stop_status;
        enter_ended(mon, why, status, mon->cfg.hold);
        if (!mon->cfg.hold)
            return why;
        x->stop_request = false;
        while (mon->state == ST_ENDED && !mon->quit && !x->stop_request)
            uam_host_poll(mon->h, 100);
        if (mon->state == ST_STOPPED && !mon->quit && !x->stop_request)
            continue;   /* a snapshot or a reset brought the machine back: it goes on */
        if (x->stop_request && !mon->quit)
            return UAM_STOP_REQUEST;   /* SIGINT (130) or Ctrl-A x (0) while held */
        if (mon->quit && mon->quit_has_code) {
            x->stop_status = mon->quit_code;
            return UAM_STOP_REQUEST;
        }
        x->stop_status = status;
        return why;
    }
}
