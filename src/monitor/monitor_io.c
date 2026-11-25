/*
 * monitor_io.c - The monitor's commands on the machine's ways in and out: the consoles
 * (send, expect, tap), named inputs (input), link ports (link).
 *
 * The program tells the monitor which chardevs and link nodes there are
 * (uam_monitor_add_chardev, uam_monitor_add_link): uam-run names its console "console".
 * Guest output is watched through a chardev tap of the monitor's own
 * (uam_chardev_add_tap); what a client asked for happens at the next host poll, between
 * blocks, never inside the device's write.
 */
#define _POSIX_C_SOURCE 200809L   /* clock_gettime, strdup */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "uam/host.h"
#include "uam/link.h"
#include "monitor_int.h"

#define MAX_CHARDEVS 8u
#define MAX_LINKS    8u
#define MAX_EXPECTS  8u
#define MAX_TAPS     8u
#define TAP_FLUSH    256u   /* a tapped client gets output in pieces of up to this */
#define DATA_MAX     4096u  /* bytes "send", "input" and "link inject" take at most */
#define KEEP         (64u << 10)   /* a chardev's recent output kept for "expect" */
#define MAX_CURSORS  32u

typedef struct mon_chardev {
    char        *name;
    uam_chardev *cd;
    struct mon_io *io;
    uint8_t     *keep;     /* the last KEEP bytes of output, a ring */
    uint64_t     total;    /* bytes of output so far */
} mon_chardev;

/* Where a client has read a chardev's output to, for "expect": what it has not seen
 * yet is what the next expect looks at first. */
typedef struct mon_cursor {
    unsigned serial, cd;
    uint64_t at;
} mon_cursor;

typedef struct mon_expect {
    bool     used;
    unsigned serial;
    unsigned cd;          /* index in chardevs */
    uint8_t *pat;
    size_t   len, matched;
    uint32_t *fail;       /* KMP: fail[i], border of pat[0..i] */
    uint64_t end;         /* matched: the output count just past the match */
    uint64_t deadline_ns; /* 0: none */
    bool     done;        /* matched: answered at the next tick */
} mon_expect;

typedef struct mon_tap {
    bool     used;
    unsigned serial;
    unsigned cd;
    uint8_t  buf[TAP_FLUSH];
    size_t   len;
} mon_tap;

typedef struct mon_link {
    uam_link_node *n;
    bool           tap_free;   /* the node's one tap is not someone else's */
    bool           tapping;
} mon_link;

struct mon_io {
    uam_monitor *mon;
    uam_machine *m;
    mon_chardev  cd[MAX_CHARDEVS];
    unsigned     n_cd;
    mon_expect   ex[MAX_EXPECTS];
    mon_tap      tap[MAX_TAPS];
    mon_link     ln[MAX_LINKS];
    unsigned     n_ln;
    unsigned     link_tap_serial[MAX_TAPS];   /* clients that tap the links */
    mon_cursor   cur[MAX_CURSORS];
};

/* The client's place in chardev k's output (created at the output's end: from now on). */
static mon_cursor *cursor(mon_io *io, unsigned serial, unsigned k)
{
    mon_cursor *free_c = NULL;
    for (unsigned i = 0; i < MAX_CURSORS; i++) {
        if (io->cur[i].serial == serial && io->cur[i].cd == k && serial)
            return &io->cur[i];
        if (!io->cur[i].serial && !free_c)
            free_c = &io->cur[i];
    }
    if (!free_c)
        free_c = &io->cur[serial % MAX_CURSORS];   /* full: reuse one */
    *free_c = (mon_cursor){ .serial = serial, .cd = k, .at = io->cd[k].total };
    return free_c;
}

/* Feed one byte to an expect; true when it completes the match. */
static bool expect_byte(mon_expect *e, uint8_t b)
{
    while (e->matched && e->pat[e->matched] != b)
        e->matched = e->fail[e->matched - 1];
    if (e->pat[e->matched] == b)
        e->matched++;
    return e->matched == e->len;
}

static mon_io *IO(uam_monitor *mon) { return *mon_io_slot(mon); }

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/* TEXT with C escapes (\n \r \t \\ \" \0 \xHH), or hex:BYTES. */
static size_t decode(const char *text, uint8_t *out, size_t cap, const char **why)
{
    size_t n = 0;
    if (!strncmp(text, "hex:", 4)) {
        const char *p = text + 4;
        if (strlen(p) % 2) {
            *why = "hex: needs two digits a byte";
            return SIZE_MAX;
        }
        for (; *p; p += 2) {
            unsigned b;
            if (n == cap || sscanf(p, "%2x", &b) != 1) {
                *why = n == cap ? "too long" : "hex: digits only";
                return SIZE_MAX;
            }
            out[n++] = (uint8_t)b;
        }
        return n;
    }
    for (const char *p = text; *p; p++) {
        uint8_t c = (uint8_t)*p;
        if (c == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case '0': c = 0; break;
            case 'x': {
                unsigned b;
                if (sscanf(p + 1, "%2x", &b) != 1) {
                    *why = "\\x needs hex digits";
                    return SIZE_MAX;
                }
                c = (uint8_t)b;
                p += (p[2] && strchr("0123456789abcdefABCDEF", p[2])) ? 2 : 1;
                break;
            }
            default: c = (uint8_t)*p; break;
            }
        }
        if (n == cap) {
            *why = "too long";
            return SIZE_MAX;
        }
        out[n++] = c;
    }
    return n;
}

static int find_cd(mon_io *io, const char *name)
{
    for (unsigned i = 0; i < io->n_cd; i++)
        if (!strcmp(io->cd[i].name, name))
            return (int)i;
    return -1;
}

static int get_cd(uam_monitor_call *c, mon_io *io, int i)
{
    const char *name = uam_monitor_arg(c, i);
    int k = name ? find_cd(io, name) : -1;
    if (k < 0) {
        char names[200] = "";
        for (unsigned j = 0; j < io->n_cd; j++)
            snprintf(names + strlen(names), sizeof(names) - strlen(names), "%s%s",
                     j ? ", " : "", io->cd[j].name);
        uam_monitor_fail(c, "%s: no chardev '%s' (%s)", uam_monitor_arg(c, 0), name ? name : "",
                         io->n_cd ? names : "there is none");
    }
    return k;
}

/* ---- guest output -------------------------------------------------------------------- */

static void on_output(void *ctx, const uint8_t *buf, size_t len)
{
    mon_chardev *mc = ctx;
    mon_io *io = mc->io;
    const unsigned k = (unsigned)(mc - io->cd);
    for (unsigned i = 0; i < MAX_EXPECTS; i++) {
        mon_expect *e = &io->ex[i];
        if (!e->used || e->done || e->cd != k)
            continue;
        for (size_t b = 0; b < len && !e->done; b++)
            if (expect_byte(e, buf[b])) {
                e->done = true;
                e->end = mc->total + b + 1;
            }
    }
    for (size_t b = 0; mc->keep && b < len; b++)
        mc->keep[(mc->total + b) % KEEP] = buf[b];
    mc->total += len;
    for (unsigned i = 0; i < MAX_TAPS; i++) {
        mon_tap *t = &io->tap[i];
        if (!t->used || t->cd != k)
            continue;
        for (size_t b = 0; b < len; b++) {
            t->buf[t->len++] = buf[b];
            if (t->len == TAP_FLUSH || buf[b] == '\n') {
                /* a whole line, or a full piece: out now (only text goes out; nothing
                 * else runs here) */
                mon_event_to(io->mon, t->serial, "output", "text", t->buf, t->len, "chardev",
                             mc->name, NULL);
                t->len = 0;
            }
        }
    }
}

static void expect_free(mon_expect *e)
{
    free(e->pat);
    free(e->fail);
    *e = (mon_expect){ 0 };
}

void mon_io_tick(uam_monitor *mon)
{
    mon_io *io = IO(mon);
    if (!io)
        return;
    const uint64_t now = now_ns();
    for (unsigned i = 0; i < MAX_EXPECTS; i++) {
        mon_expect *e = &io->ex[i];
        if (!e->used)
            continue;
        unsigned serial = e->serial;
        if (e->done) {
            cursor(io, serial, e->cd)->at = e->end;   /* past the match */
            expect_free(e);
            mon_answer_io(mon, serial, NULL, "matched", "yes", NULL);
        } else if (e->deadline_ns && now >= e->deadline_ns) {
            expect_free(e);
            mon_answer_io(mon, serial, "expect: timed out", NULL);
        }
    }
    /* A partial line is out once the host polls (no waiting for its end). */
    for (unsigned i = 0; i < MAX_TAPS; i++) {
        mon_tap *t = &io->tap[i];
        if (t->used && t->len) {
            mon_event_to(mon, t->serial, "output", "text", t->buf, t->len, "chardev",
                         io->cd[t->cd].name, NULL);
            t->len = 0;
        }
    }
}

void mon_io_ended(uam_monitor *mon)
{
    mon_io *io = IO(mon);
    if (!io)
        return;
    mon_io_tick(mon);   /* a match in the last output still counts */
    for (unsigned i = 0; i < MAX_EXPECTS; i++)
        if (io->ex[i].used) {
            unsigned serial = io->ex[i].serial;
            expect_free(&io->ex[i]);
            mon_answer_io(mon, serial, "expect: the run ended first", NULL);
        }
}

void mon_io_client_new(uam_monitor *mon, unsigned serial)
{
    mon_io *io = IO(mon);
    for (unsigned k = 0; io && k < io->n_cd; k++)
        cursor(io, serial, k);
}

void mon_io_client_gone(uam_monitor *mon, unsigned serial)
{
    mon_io *io = IO(mon);
    if (!io)
        return;
    for (unsigned i = 0; i < MAX_EXPECTS; i++)
        if (io->ex[i].used && io->ex[i].serial == serial)
            expect_free(&io->ex[i]);
    for (unsigned i = 0; i < MAX_TAPS; i++) {
        if (io->tap[i].used && io->tap[i].serial == serial)
            io->tap[i].used = false;
        if (io->link_tap_serial[i] == serial)
            io->link_tap_serial[i] = 0;
    }
    for (unsigned i = 0; i < MAX_CURSORS; i++)
        if (io->cur[i].serial == serial)
            io->cur[i] = (mon_cursor){ 0 };
}

/* ---- send, expect, tap ---------------------------------------------------------------- */

static void cmd_send(uam_monitor_call *c, void *ctx)
{
    mon_io *io = IO(ctx);
    if (uam_monitor_argc(c) != 3) {
        uam_monitor_fail(c, "send CHARDEV TEXT (\"quoted\" for blanks; \\n \\r \\t \\xHH; or hex:BYTES)");
        return;
    }
    int k = get_cd(c, io, 1);
    if (k < 0)
        return;
    uint8_t buf[DATA_MAX];
    const char *why = NULL;
    size_t n = decode(uam_monitor_arg(c, 2), buf, sizeof(buf), &why);
    if (n == SIZE_MAX) {
        uam_monitor_fail(c, "send: %s", why);
        return;
    }
    cursor(io, mon_call_serial(c), (unsigned)k);   /* an expect after this sees the answer */
    size_t queued = uam_chardev_input(io->cd[k].cd, buf, n);
    if (queued < n)
        uam_monitor_fail(c, "send: the input buffer took %zu of %zu bytes", queued, n);
    else
        uam_monitor_field(c, "sent", "%zu", n);
}

static void cmd_expect(uam_monitor_call *c, void *ctx)
{
    mon_io *io = IO(ctx);
    int argc = uam_monitor_argc(c);
    if (argc != 3 && argc != 4) {
        uam_monitor_fail(c, "expect CHARDEV PATTERN [SECONDS]");
        return;
    }
    int k = get_cd(c, io, 1);
    if (k < 0)
        return;
    double secs = 0;
    if (argc == 4) {
        char *end;
        secs = strtod(uam_monitor_arg(c, 3), &end);
        if (*end || secs <= 0) {
            uam_monitor_fail(c, "expect: SECONDS is a time above 0");
            return;
        }
    }
    unsigned slot = MAX_EXPECTS;
    for (unsigned i = 0; i < MAX_EXPECTS && slot == MAX_EXPECTS; i++)
        if (!io->ex[i].used)
            slot = i;
    if (slot == MAX_EXPECTS) {
        uam_monitor_fail(c, "expect: at most %u at a time", MAX_EXPECTS);
        return;
    }
    uint8_t buf[DATA_MAX];
    const char *why = NULL;
    size_t n = decode(uam_monitor_arg(c, 2), buf, sizeof(buf), &why);
    if (n == SIZE_MAX || !n) {
        uam_monitor_fail(c, "expect: %s", n ? why : "an empty pattern");
        return;
    }
    mon_expect *e = &io->ex[slot];
    e->pat = malloc(n);
    e->fail = malloc(n * sizeof(*e->fail));
    if (!e->pat || !e->fail) {
        expect_free(e);
        uam_monitor_fail(c, "expect: out of memory");
        return;
    }
    memcpy(e->pat, buf, n);
    e->fail[0] = 0;
    for (size_t i = 1, b = 0; i < n; i++) {
        while (b && buf[i] != buf[b])
            b = e->fail[b - 1];
        if (buf[i] == buf[b])
            b++;
        e->fail[i] = (uint32_t)b;
    }
    if (!mon_defer_io(c)) {
        expect_free(e);
        uam_monitor_fail(c, "expect: not in a script (the machine has not started)");
        return;
    }
    e->used = true;
    e->serial = mon_call_serial(c);
    e->cd = (unsigned)k;
    e->len = n;
    /* What came since the client last read this output (still kept) counts first. */
    mon_chardev *mc = &io->cd[k];
    mon_cursor *cu = cursor(io, e->serial, (unsigned)k);
    uint64_t from = cu->at + KEEP < mc->total ? mc->total - KEEP : cu->at;
    for (uint64_t b = from; b < mc->total && !e->done; b++)
        if (expect_byte(e, mc->keep[b % KEEP])) {
            e->done = true;
            e->end = b + 1;
        }
    e->deadline_ns = argc == 4 ? now_ns() + (uint64_t)(secs * 1e9) : 0;
}

static void cmd_tap(uam_monitor_call *c, void *ctx)
{
    mon_io *io = IO(ctx);
    const char *onoff = uam_monitor_arg(c, 2);
    if (uam_monitor_argc(c) != 3 || (strcmp(onoff, "on") && strcmp(onoff, "off"))) {
        uam_monitor_fail(c, "tap CHARDEV on|off");
        return;
    }
    int k = get_cd(c, io, 1);
    if (k < 0)
        return;
    unsigned serial = mon_call_serial(c);
    if (!serial) {
        uam_monitor_fail(c, "tap: not in a script");
        return;
    }
    unsigned slot = MAX_TAPS, free_slot = MAX_TAPS;
    for (unsigned i = 0; i < MAX_TAPS; i++) {
        if (io->tap[i].used && io->tap[i].serial == serial && io->tap[i].cd == (unsigned)k)
            slot = i;
        if (!io->tap[i].used && free_slot == MAX_TAPS)
            free_slot = i;
    }
    if (!strcmp(onoff, "off")) {
        if (slot < MAX_TAPS)
            io->tap[slot].used = false;
        return;
    }
    if (slot < MAX_TAPS)
        return;   /* on already */
    if (free_slot == MAX_TAPS) {
        uam_monitor_fail(c, "tap: at most %u at a time", MAX_TAPS);
        return;
    }
    io->tap[free_slot] = (mon_tap){ .used = true, .serial = serial, .cd = (unsigned)k };
}

/* ---- named inputs ----------------------------------------------------------------------- */

static void cmd_input(uam_monitor_call *c, void *ctx)
{
    mon_io *io = IO(ctx);
    uam_machine *m = io->m;
    if (uam_monitor_argc(c) == 1) {
        for (uint32_t i = 0; i < m->n_inputs; i++)
            uam_monitor_print(c, "%s", m->inputs[i].name);
        return;
    }
    if (uam_monitor_argc(c) != 3) {
        uam_monitor_fail(c, "input [NAME DATA] (DATA: TEXT with escapes, or hex:BYTES)");
        return;
    }
    const uam_machine_input *in = uam_machine_find_input(m, uam_monitor_arg(c, 1));
    if (!in) {
        uam_monitor_fail(c, "input: no input '%s' (input lists them)", uam_monitor_arg(c, 1));
        return;
    }
    uint8_t buf[DATA_MAX];
    const char *why = NULL;
    size_t n = decode(uam_monitor_arg(c, 2), buf, sizeof(buf), &why);
    if (n == SIZE_MAX) {
        uam_monitor_fail(c, "input: %s", why);
        return;
    }
    if (in->fn(in->ctx, m, buf, n) != UAM_OK)
        uam_monitor_fail(c, "input: %s did not take the %zu bytes", in->name, n);
    else
        uam_monitor_field(c, "delivered", "%zu", n);
}

/* ---- links ------------------------------------------------------------------------------ */

static uam_link_port *find_port(mon_io *io, const char *name)
{
    for (unsigned i = 0; i < io->n_ln; i++) {
        uam_link_port *p = uam_link_node_find_port(io->ln[i].n, name);
        if (p)
            return p;
    }
    return NULL;
}

static void on_link(void *ctx, const uam_link_port *p, const uam_link_msg *msg, bool out)
{
    mon_io *io = ctx;
    char kind[16], key[24], len[16];
    snprintf(kind, sizeof(kind), "%u", msg->kind);
    snprintf(key, sizeof(key), "0x%llx", (unsigned long long)msg->key);
    snprintf(len, sizeof(len), "%u", msg->len);
    size_t shown = msg->len < TAP_FLUSH ? msg->len : TAP_FLUSH;
    for (unsigned i = 0; i < MAX_TAPS; i++)
        if (io->link_tap_serial[i])
            mon_event_to(io->mon, io->link_tap_serial[i], "link", "data", msg->data, shown,
                         "port", uam_link_port_name(p), "dir", out ? "out" : "in", "kind", kind,
                         "key", key, "len", len, NULL);
}

static void cmd_link(uam_monitor_call *c, void *ctx)
{
    mon_io *io = IO(ctx);
    const char *sub = uam_monitor_arg(c, 1);
    if (!sub || !strcmp(sub, "status")) {
        if (!io->n_ln)
            uam_monitor_print(c, "(no links: --link)");
        for (unsigned i = 0; i < io->n_ln; i++) {
            uam_link_node *n = io->ln[i].n;
            uam_monitor_print(c, "node %s %s", uam_link_node_name(n),
                              uam_link_node_connected(n) ? "connected" : "not connected");
            for (unsigned k = 0; k < uam_link_node_ports(n); k++) {
                uam_link_port *p = uam_link_node_port(n, k);
                const uam_link_stats *s = uam_link_port_stats(p);
                uam_monitor_print(c, "port %s bus=%s %s tx=%llu rx=%llu dropped=%llu late=%llu "
                                  "waiting=%zu", uam_link_port_name(p),
                                  uam_link_port_bus(p) ? uam_link_port_bus(p) : "-",
                                  uam_link_port_up(p) ? "up" : "down",
                                  (unsigned long long)s->tx, (unsigned long long)s->rx,
                                  (unsigned long long)s->tx_dropped, (unsigned long long)s->late,
                                  uam_link_port_ready(p));
            }
        }
        return;
    }
    if (!strcmp(sub, "inject")) {
        if (uam_monitor_argc(c) != 4) {
            uam_monitor_fail(c, "link inject PORT DATA (a DATA message, now)");
            return;
        }
        uam_link_port *p = find_port(io, uam_monitor_arg(c, 2));
        if (!p) {
            uam_monitor_fail(c, "link: no port '%s'", uam_monitor_arg(c, 2));
            return;
        }
        uint8_t buf[DATA_MAX];
        const char *why = NULL;
        size_t n = decode(uam_monitor_arg(c, 3), buf, sizeof(buf), &why);
        if (n == SIZE_MAX) {
            uam_monitor_fail(c, "link: %s", why);
            return;
        }
        uam_link_msg msg = { .kind = UAM_LINK_DATA, .len = (uint32_t)n, .data = buf };
        if (uam_link_port_inject(p, &msg, 0) != UAM_OK)
            uam_monitor_fail(c, "link: port %s did not take it", uam_monitor_arg(c, 2));
        else
            uam_monitor_field(c, "injected", "%zu", n);
        return;
    }
    if (!strcmp(sub, "tap") && uam_monitor_argc(c) == 3) {
        const bool on = !strcmp(uam_monitor_arg(c, 2), "on");
        unsigned serial = mon_call_serial(c);
        if (!serial) {
            uam_monitor_fail(c, "link: tap is for a client (not a script, not the debugger)");
            return;
        }
        if (on) {
            for (unsigned i = 0; i < io->n_ln; i++)
                if (!io->ln[i].tap_free) {
                    uam_monitor_fail(c, "link: the links' tap is taken (-t link)");
                    return;
                }
            if (!io->n_ln) {
                uam_monitor_fail(c, "link: no links (--link)");
                return;
            }
            unsigned slot = MAX_TAPS;
            for (unsigned i = 0; i < MAX_TAPS; i++) {
                if (io->link_tap_serial[i] == serial)
                    return;
                if (!io->link_tap_serial[i] && slot == MAX_TAPS)
                    slot = i;
            }
            if (slot == MAX_TAPS) {
                uam_monitor_fail(c, "link: at most %u clients tap", MAX_TAPS);
                return;
            }
            io->link_tap_serial[slot] = serial;
            for (unsigned i = 0; i < io->n_ln; i++)
                if (!io->ln[i].tapping) {
                    uam_link_node_set_tap(io->ln[i].n, on_link, io);
                    io->ln[i].tapping = true;
                }
        } else {
            for (unsigned i = 0; i < MAX_TAPS; i++)
                if (io->link_tap_serial[i] == serial)
                    io->link_tap_serial[i] = 0;
        }
        return;
    }
    uam_monitor_fail(c, "link [status] | inject PORT DATA | tap on|off");
}

/* ---- registration ----------------------------------------------------------------------- */

uam_status uam_monitor_add_chardev(uam_monitor *mon, const char *name, uam_chardev *cd)
{
    mon_io *io = IO(mon);
    if (!name || !*name || !cd || find_cd(io, name) >= 0)
        return UAM_ERR_INVALID;
    if (io->n_cd == MAX_CHARDEVS)
        return UAM_ERR_NOMEM;
    mon_chardev *mc = &io->cd[io->n_cd];
    if (!(mc->name = strdup(name)))
        return UAM_ERR_NOMEM;
    mc->cd = cd;
    mc->io = io;
    if (!(mc->keep = malloc(KEEP))) {
        free(mc->name);
        mc->name = NULL;
        return UAM_ERR_NOMEM;
    }
    if (!uam_chardev_add_tap(cd, on_output, mc)) {
        free(mc->name);
        free(mc->keep);
        mc->name = NULL;
        mc->keep = NULL;
        return UAM_ERR_NOMEM;
    }
    io->n_cd++;
    return UAM_OK;
}

uam_status uam_monitor_add_link(uam_monitor *mon, uam_link_node *n, bool tap_free)
{
    mon_io *io = IO(mon);
    if (!n)
        return UAM_ERR_INVALID;
    if (io->n_ln == MAX_LINKS)
        return UAM_ERR_NOMEM;
    io->ln[io->n_ln++] = (mon_link){ .n = n, .tap_free = tap_free };
    return UAM_OK;
}

uam_status mon_add_io_commands(uam_monitor *mon)
{
    mon_io *io = calloc(1, sizeof(*io));
    if (!io)
        return UAM_ERR_NOMEM;
    io->mon = mon;
    io->m = uam_monitor_machine(mon);
    *mon_io_slot(mon) = io;
    static const struct {
        const char *name, *usage, *help;
        unsigned    flags;
        uam_monitor_fn fn;
    } cmds[] = {
        { "send", "CHARDEV TEXT", "type TEXT into a console, as if on its host end", UAM_MONITOR_CHANGES,
          cmd_send },
        { "expect", "CHARDEV PATTERN [SECONDS]",
          "answer once the guest has written PATTERN to the console", UAM_MONITOR_SERVE_ONLY,
          cmd_expect },
        { "tap", "CHARDEV on|off", "the guest's output on a console as events to this client",
          UAM_MONITOR_SERVE_ONLY, cmd_tap },
        { "input", "[NAME DATA]", "hand DATA to a named input of the board (a UART's FIFO...)",
          UAM_MONITOR_STOPPED_ONLY | UAM_MONITOR_CHANGES, cmd_input },
        { "link", "[status] | inject PORT DATA | tap on|off",
          "link ports: their state, a message handed in, every message as an event",
          UAM_MONITOR_CHANGES, cmd_link },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        uam_status st = uam_monitor_add_command(mon, cmds[i].name, cmds[i].usage, cmds[i].help,
                                                cmds[i].flags, cmds[i].fn, mon);
        if (st != UAM_OK)
            return st;
    }
    return UAM_OK;
}

void mon_io_free(uam_monitor *mon)
{
    mon_io *io = IO(mon);
    if (!io)
        return;
    for (unsigned i = 0; i < io->n_cd; i++) {
        uam_chardev_remove_tap(io->cd[i].cd, on_output, &io->cd[i]);
        free(io->cd[i].name);
        free(io->cd[i].keep);
    }
    for (unsigned i = 0; i < MAX_EXPECTS; i++)
        if (io->ex[i].used)
            expect_free(&io->ex[i]);
    for (unsigned i = 0; i < io->n_ln; i++)
        if (io->ln[i].tapping)
            uam_link_node_set_tap(io->ln[i].n, NULL, NULL);
    free(io);
    *mon_io_slot(mon) = NULL;
}
