/*
 * test_link.c - Unit tests for links (uam/link.h): ports and queues, timed
 * delivery and its order, waiting for grants, a node's saved state.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uam/host.h"
#include "uam/isa/riscv.h"
#include "uam/link.h"
#include "uam/machine.h"

static int n_checks, n_failed;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        n_checks++;                                                         \
        if (!(cond)) {                                                      \
            n_failed++;                                                     \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);            \
            fprintf(stderr, __VA_ARGS__);                                   \
            fputc('\n', stderr);                                            \
        }                                                                   \
    } while (0)

/* ---- a frontend that records what it receives -------------------------------- */

typedef struct sink {
    bool     ready;
    unsigned n;
    char     log[256];       /* first payload byte of each message, in order */
    uint64_t last_key, last_time;
    uint32_t last_flags, last_len;
    unsigned ups, downs, rewinds;
    bool    *in_send;        /* set while the test is inside uam_link_send */
    bool     called_in_send;
} sink;

static bool sink_can(void *ctx) { return ((sink *)ctx)->ready; }

static void sink_rx(void *ctx, const uam_link_msg *m)
{
    sink *s = ctx;
    if (s->n < sizeof(s->log) - 1)
        s->log[s->n] = m->len ? (char)m->data[0] : '.';
    s->n++;
    s->last_key = m->key;
    s->last_time = m->time;
    s->last_flags = m->flags;
    s->last_len = m->len;
    if (s->in_send && *s->in_send)
        s->called_in_send = true;
}

static void sink_ev(void *ctx, uam_link_event ev)
{
    sink *s = ctx;
    if (ev == UAM_LINK_EV_UP)
        s->ups++;
    else if (ev == UAM_LINK_EV_DOWN)
        s->downs++;
    else
        s->rewinds++;
}

static uam_link_msg msg(uint64_t time, uint32_t src, uint32_t seq, const char *text)
{
    return (uam_link_msg){ .time = time, .src = src, .seq = seq, .len = (uint32_t)strlen(text),
                           .data = (const uint8_t *)text };
}

/* ---- a transport that plays the hub ------------------------------------------ */

typedef struct fake_hub {
    uam_link_node *node;
    unsigned sent, reports, polls;
    uint64_t reached, quiet;
    uint64_t step;           /* each waiting poll grants this much more (0: nothing) */
    uint64_t granted;
    bool     echo;           /* send: hand the message back on the same port */
    bool     quiesce;        /* a waiting poll declares the world quiet */
    bool     gone;           /* poll reports the hub lost */
    bool     push;           /* a waiting poll delivers one message at the grant */
} fake_hub;

static bool fh_send(void *ctx, unsigned port, const uam_link_msg *m)
{
    fake_hub *h = ctx;
    h->sent++;
    if (h->echo)
        uam_link_node_input(h->node, port, m);
    return true;
}

static void fh_report(void *ctx, uint64_t reached, uint64_t quiet, uint64_t received)
{
    (void)received;
    fake_hub *h = ctx;
    h->reports++;
    h->reached = reached;
    h->quiet = quiet;
}

static bool fh_poll(void *ctx, int timeout_ms)
{
    fake_hub *h = ctx;
    if (h->gone)
        return false;
    if (!timeout_ms)
        return true;
    h->polls++;
    if (h->push) {
        h->push = false;
        uam_link_msg m = msg(h->granted, 7, 0, "p");
        uam_link_node_input(h->node, 0, &m);
    }
    if (h->quiesce)
        uam_link_node_quiescent(h->node);
    if (h->step) {
        h->granted += h->step;
        uam_link_node_grant(h->node, h->granted);
    }
    return true;
}

static const uam_link_transport fh_driven = { .type = "test", .send = fh_send, .report = fh_report };
static const uam_link_transport fh_waiting = { .type = "test", .send = fh_send,
                                               .report = fh_report, .poll = fh_poll };

/* ---- ports and queues -------------------------------------------------------- */

static void test_ports(void)
{
    uam_link_node *n = uam_link_node_create("n0");
    uam_link_port *a = uam_link_node_add_port(n, "a", "bus0", 1000);
    uam_link_port *b = uam_link_node_add_port(n, "b", NULL, 0);
    CHECK(a && b, "ports");
    CHECK(!uam_link_node_add_port(n, "a", "x", 0), "a second port of the same name is refused");
    CHECK(uam_link_node_find_port(n, "b") == b && uam_link_node_ports(n) == 2, "find");
    CHECK(!strcmp(uam_link_port_bus(a), "bus0") && !strcmp(uam_link_port_bus(b), "b"),
          "the bus defaults to the port's name");
    CHECK(uam_link_port_index(b) == 1 && uam_link_node_port(n, 1) == b, "index");
    CHECK(!uam_link_port_up(a), "no hub: down");

    /* Injected messages wait for the frontend, and for it to have room. */
    uam_link_msg m = msg(0, 0, 0, "1");
    CHECK(uam_link_port_inject(a, &m, 0) == UAM_OK, "inject");
    CHECK(uam_link_port_ready(a) == 1, "queued with no frontend");
    sink sa = { .ready = false };
    uam_link_port_set_frontend(a, sink_can, sink_rx, sink_ev, &sa);
    CHECK(sa.n == 0, "frontend not ready");
    m = msg(0, 0, 1, "2");
    uam_link_port_inject(a, &m, 0);
    sa.ready = true;
    uam_link_accept_input(a);
    CHECK(sa.n == 2 && !strncmp(sa.log, "12", 2), "delivered in order: %.4s", sa.log);
    CHECK(uam_link_port_stats(a)->rx == 2, "rx count");

    /* An empty message and a long one keep their exact length. */
    uam_link_msg empty = { .key = 9 };
    uam_link_port_inject(a, &empty, 0);
    CHECK(sa.n == 3 && sa.last_len == 0 && sa.last_key == 9, "empty message");
    static uint8_t big[70000];
    big[0] = 'B';
    uam_link_msg bm = { .len = sizeof(big), .data = big };
    uam_link_port_inject(a, &bm, 0);
    CHECK(sa.last_len == sizeof(big), "long message, exact length");
    uam_link_msg huge = { .len = UAM_LINK_MAX_MSG + 1, .data = big };
    CHECK(uam_link_port_inject(a, &huge, 0) == UAM_ERR_INVALID, "over the limit");

    /* Sending with no hub: nothing takes it. */
    CHECK(!uam_link_send(a, 1, 0, UAM_LINK_DATA, "x", 1), "send without a hub");
    CHECK(uam_link_port_stats(a)->tx_dropped == 1, "counted as dropped");
    CHECK(!uam_link_send(a, 1, 0, UAM_LINK_KIND_RESERVED, "x", 1), "reserved kinds are refused");
    uam_link_node_destroy(n);
}

/* What a hub answers during a send reaches the frontend after the send. */
static void test_send(void)
{
    uam_link_node *n = uam_link_node_create("n0");
    uam_link_port *a = uam_link_node_add_port(n, "a", "bus0", 250);
    fake_hub h = { .node = n, .echo = true };
    bool in_send = false;
    sink sa = { .ready = true, .in_send = &in_send };
    uam_link_port_set_frontend(a, sink_can, sink_rx, sink_ev, &sa);
    uam_link_node_set_transport(n, &fh_driven, &h);
    CHECK(sa.ups == 1 && uam_link_port_up(a), "up once connected");

    in_send = true;
    bool ok = uam_link_send(a, 0x123, 5u << UAM_LINK_F_MODEL_SHIFT, UAM_LINK_DATA, "hi", 2);
    in_send = false;
    CHECK(ok && h.sent == 1, "sent");
    CHECK(sa.n == 1 && sa.last_key == 0x123 && sa.last_len == 2, "the echo arrived");
    CHECK(sa.last_flags == 5u << UAM_LINK_F_MODEL_SHIFT, "model flags pass through");
    CHECK(sa.last_time == 250, "stamped now + latency (no clock: now is 0)");
    /* in_send is the test's flag, cleared after the call: the callback ran before
     * uam_link_send returned but outside the transport's send. */
    CHECK(uam_link_port_stats(a)->tx == 1, "tx count");

    uam_link_node_set_lost(n);
    CHECK(sa.downs == 1 && !uam_link_port_up(a) && uam_link_node_lost(n), "lost: down");
    CHECK(!uam_link_send(a, 0, 0, UAM_LINK_DATA, "x", 1), "nothing is sent to a lost hub");
    uam_link_node_destroy(n);
}

/* ---- timed delivery ---------------------------------------------------------- */

typedef struct other_timer {
    sink *s;
    unsigned seen_at_fire;   /* how many messages the sink had when this timer fired */
    bool fired;
} other_timer;

static void other_fire(void *ctx)
{
    other_timer *o = ctx;
    o->fired = true;
    o->seen_at_fire = o->s->n;
}

static void test_timed(void)
{
    uint64_t icount = 0;
    uam_sched s;
    uam_sched_init(&s, &icount);
    uam_link_node *n = uam_link_node_create("n0");
    uam_link_port *a = uam_link_node_add_port(n, "a", "bus0", 2000);
    fake_hub h = { .node = n };
    sink sa = { .ready = true };
    uam_link_port_set_frontend(a, sink_can, sink_rx, sink_ev, &sa);
    uam_link_node_set_transport(n, &fh_driven, &h);
    /* One tick is a nanosecond; the unit is a picosecond. */
    CHECK(uam_link_node_set_clock(n, &s, 1000000000u) == UAM_OK, "clock");
    CHECK(uam_link_node_to_ticks(n, 5000) == 5 && uam_link_node_to_ticks(n, 5001) == 6,
          "to ticks rounds up");
    CHECK(uam_link_node_from_ticks(n, 7) == 7000, "from ticks");
    CHECK(uam_link_node_to_ticks(n, UAM_LINK_NEVER) == UINT64_MAX, "never stays never");

    /* Free: delivered on arrival whatever the stamp says. */
    uam_link_msg m = msg(9000, 1, 0, "f");
    uam_link_node_input(n, 0, &m);
    CHECK(sa.n == 1, "free: delivered on arrival");

    /* Exact: delivered at its tick, ties by (time, source, sequence). */
    uam_link_node_set_sync(n, UAM_LINK_EXACT, 0);
    m = msg(5000, 2, 0, "c");
    uam_link_node_input(n, 0, &m);
    m = msg(5000, 1, 1, "b");
    uam_link_node_input(n, 0, &m);
    m = msg(5000, 1, 0, "a");
    uam_link_node_input(n, 0, &m);
    m = msg(3000, 9, 0, "0");
    uam_link_node_input(n, 0, &m);
    CHECK(sa.n == 1 && uam_link_node_next_delivery(n) == 3, "held until their time");
    icount = 2;
    uam_sched_run_due(&s);
    CHECK(sa.n == 1, "not yet");
    icount = 3;
    uam_sched_run_due(&s);
    CHECK(sa.n == 2 && sa.log[1] == '0', "the first at tick 3");

    /* A timer armed later for the same tick still fires before the delivery. */
    other_timer o = { .s = &sa };
    uam_timer ot;
    uam_timer_init(&ot, other_fire, &o);
    uam_timer_arm(&s, &ot, 5);
    icount = 5;
    uam_sched_run_due(&s);
    CHECK(o.fired && o.seen_at_fire == 2, "a delivery is the last thing at its tick");
    CHECK(sa.n == 5 && !strncmp(sa.log + 2, "abc", 3), "tie order: %.5s", sa.log);
    CHECK(uam_link_node_next_delivery(n) == UINT64_MAX, "queue empty");
    CHECK(uam_sched_check(&s), "timer list sane");

    /* A send is stamped with the node's time plus the latency. */
    h.echo = true;
    icount = 10;
    uam_link_send(a, 0, 0, UAM_LINK_DATA, "s", 1);
    CHECK(sa.n == 5 && uam_link_node_next_delivery(n) == 12, "the echo is due 2 ns later");
    icount = 12;
    uam_sched_run_due(&s);
    CHECK(sa.n == 6 && sa.last_time == 12000, "stamp: %llu", (unsigned long long)sa.last_time);
    h.echo = false;

    /* Loose: a message whose time has passed is delivered at once, flagged. */
    uam_link_node_set_sync(n, UAM_LINK_LOOSE, 0);
    icount = 100;
    m = msg(50000, 1, 0, "l");
    uam_link_node_input(n, 0, &m);
    CHECK(sa.n == 7 && (sa.last_flags & UAM_LINK_F_LATE), "late message flagged");
    CHECK(uam_link_port_stats(a)->late == 1, "and counted");
    m = msg(100000, 1, 1, "n");
    uam_link_node_input(n, 0, &m);
    CHECK(sa.n == 7, "a message due now waits for the timers");
    uam_sched_run_due(&s);
    CHECK(sa.n == 8 && !(sa.last_flags & UAM_LINK_F_LATE), "due now is not late");

    /* Injection at a tick. */
    m = msg(0, 0, 0, "i");
    uam_link_port_inject(a, &m, 150);
    CHECK(sa.n == 8, "inject at a tick waits");
    icount = 150;
    uam_sched_run_due(&s);
    CHECK(sa.n == 9 && sa.log[8] == 'i', "and arrives then");
    uam_link_node_destroy(n);
}

/* ---- waiting for grants ------------------------------------------------------ */

static void test_grants(void)
{
    uint64_t icount = 0;
    uam_sched s;
    uam_sched_init(&s, &icount);
    uam_link_node *n = uam_link_node_create("n0");
    uam_link_port *a = uam_link_node_add_port(n, "a", "bus0", 1000);
    fake_hub h = { .node = n, .step = 10000 };     /* 10 ns per grant */
    sink sa = { .ready = true };
    uam_link_port_set_frontend(a, sink_can, sink_rx, sink_ev, &sa);
    uam_link_node_set_clock(n, &s, 1000000000u);
    uam_link_node_set_transport(n, &fh_waiting, &h);
    CHECK(!uam_link_node_timed(n), "free: no grants");
    CHECK(uam_link_node_grant_tick(n) == UINT64_MAX, "free: no limit");
    uam_link_node_set_sync(n, UAM_LINK_EXACT, 0);
    CHECK(uam_link_node_timed(n) && uam_link_node_grant_tick(n) == 0, "exact: nothing granted yet");
    CHECK(uam_sched_next(&s) == 0, "the grant timer stands at the limit");

    /* Running into the grant: report where we stand, wait, go on. */
    uam_sched_run_due(&s);
    CHECK(h.reports == 1 && h.reached == 0 && h.quiet == 0, "reported at 0");
    CHECK(h.polls == 1 && uam_link_node_grant_tick(n) == 10, "granted 10 ns");
    CHECK(uam_sched_next(&s) == 10, "the timer moved to the new limit");
    icount = 10;
    uam_sched_run_due(&s);
    CHECK(h.reports == 2 && h.reached == 10000 && uam_link_node_grant_tick(n) == 20, "next grant");

    /* A grant that arrives while running moves the timer; grants never go back. */
    uam_link_node_grant(n, 50000);
    CHECK(uam_sched_next(&s) == 50, "timer follows the grant");
    uam_link_node_grant(n, 30000);
    CHECK(uam_link_node_grant_tick(n) == 50, "grants do not move back");
    h.granted = 50000;

    /* Idle with the next own event inside the grant: nothing to do. */
    other_timer o = { .s = &sa };
    uam_timer ot;
    uam_timer_init(&ot, other_fire, &o);
    uam_timer_arm(&s, &ot, 40);
    CHECK(uam_link_node_idle(n, UINT64_MAX, true) == 0, "event within the grant");
    uam_timer_cancel(&s, &ot);

    /* Idle with the event beyond the grant: skip to the grant, promise to stay
     * quiet until the event, wait. */
    uam_timer_arm(&s, &ot, 75);
    unsigned reports = h.reports;
    CHECK(uam_link_node_idle(n, UINT64_MAX, true) == 1, "waited");
    CHECK(uam_sched_now(&s) == 50, "time advanced to the grant");
    CHECK(h.reports == reports + 1 && h.reached == 50000 && h.quiet == 75000, "promise until 75 ns");
    CHECK(uam_link_node_grant_tick(n) == 80 && uam_sched_next(&s) == 75,
          "waited until the grant covers the event: %llu",
          (unsigned long long)uam_link_node_grant_tick(n));
    CHECK(uam_link_node_idle(n, UINT64_MAX, true) == 0, "now the event is within the grant");
    uam_timer_cancel(&s, &ot);
    /* Without a promise (host input could wake the machine) quiet = reached, and
     * the node follows every grant. */
    uam_timer_arm(&s, &ot, 200);
    CHECK(uam_link_node_idle(n, UINT64_MAX, false) == 1, "waited again");
    CHECK(h.quiet == h.reached && h.reached == 80000, "no promise");
    CHECK(uam_link_node_grant_tick(n) == 90, "one grant only");
    /* The limit is respected. */
    CHECK(uam_link_node_idle(n, 85, true) == 1 && uam_sched_now(&s) == 85, "stops at the limit");
    uam_timer_cancel(&s, &ot);

    /* A message that arrives while waiting is what the node waits for next. */
    h.push = true;                                 /* arrives stamped with the grant it comes with */
    CHECK(uam_link_node_idle(n, UINT64_MAX, true) == 1, "waited for the message's time");
    uint64_t at = uam_link_node_next_delivery(n);
    CHECK(at == 90 && uam_link_node_grant_tick(n) >= at, "the message is due within the grant");
    CHECK(uam_link_node_idle(n, UINT64_MAX, true) == 0, "nothing more to wait for");
    uam_sched_skip_to(&s, at);
    uam_sched_run_due(&s);
    CHECK(sa.n == 1 && sa.log[0] == 'p', "delivered");
    h.step = 0;

    /* Every node quiet and nothing armed: nothing will ever happen. */
    h.quiesce = true;
    CHECK(uam_link_node_idle(n, UINT64_MAX, true) == -1, "quiescent");
    h.quiesce = false;

    /* The hub goes away: the node runs on alone. */
    h.gone = true;
    CHECK(!uam_link_node_poll(n) && uam_link_node_lost(n), "lost on poll");
    CHECK(!uam_link_node_timed(n) && uam_sched_next(&s) == UINT64_MAX, "no grant timer left");
    CHECK(sa.downs == 1, "down event");
    uam_link_node_destroy(n);
}

/* ---- saved state ------------------------------------------------------------- */

static void test_state(void)
{
    uint64_t icount = 4;
    uam_sched s;
    uam_sched_init(&s, &icount);
    uam_link_node *n = uam_link_node_create("n0");
    uam_link_port *a = uam_link_node_add_port(n, "a", "bus0", 1000);
    uam_link_port *b = uam_link_node_add_port(n, "b", "bus1", 1000);
    sink sa = { .ready = false }, sb = { .ready = true };
    uam_link_port_set_frontend(a, sink_can, sink_rx, sink_ev, &sa);
    uam_link_port_set_frontend(b, sink_can, sink_rx, sink_ev, &sb);
    fake_hub h = { .node = n };
    uam_link_node_set_transport(n, &fh_driven, &h);
    uam_link_node_set_clock(n, &s, 1000000000u);
    uam_link_node_set_sync(n, UAM_LINK_EXACT, 0);

    uam_link_msg m = msg(0, 0, 0, "r");
    uam_link_port_inject(a, &m, 0);                /* ready, the frontend has no room */
    m = msg(9000, 3, 0, "y");
    uam_link_node_input(n, 1, &m);                 /* pending on b at tick 9 */
    m = msg(7000, 3, 0, "x");
    uam_link_node_input(n, 0, &m);                 /* pending on a at tick 7 */
    uam_link_send(a, 0, 0, UAM_LINK_DATA, "s", 1); /* moves a's sequence number */

    uam_state_writer *w = uam_state_writer_new(&s);
    uam_state_begin(w, "link");
    uam_link_node_save(n, w);
    uam_state_end(w);
    CHECK(uam_state_writer_ok(w), "save");
    CHECK(uam_state_writer_saved_timer(w, s.head), "the delivery timer is saved");
    size_t len;
    const uint8_t *data = uam_state_writer_data(w, &len);

    /* Run on: everything is delivered. */
    sa.ready = true;
    uam_link_accept_input(a);
    icount = 20;
    uam_sched_run_due(&s);
    CHECK(sa.n == 2 && sb.n == 1, "delivered before the restore");

    /* Go back. */
    icount = 4;
    sa = (sink){ .ready = false };
    sb = (sink){ .ready = true };
    uam_sched_cancel_all(&s);
    uam_state_reader *r = uam_state_reader_new(data, len, &s);
    CHECK(r && uam_state_next(r), "record");
    uam_link_node_restore(n, r);
    uam_state_leave(r, "link");
    CHECK(uam_state_ok(r), "restore: %s", uam_state_error(r) ? uam_state_error(r) : "");
    uam_state_arm_timers(r);
    uam_state_reader_free(r);
    CHECK(uam_link_port_ready(a) == 1 && uam_link_node_next_delivery(n) == 7, "queues are back");
    CHECK(uam_sched_next(&s) == 7, "and the timer");
    CHECK(sa.rewinds == 0, "exact: no rewound event");
    sa.ready = true;
    uam_link_accept_input(a);
    icount = 7;
    uam_sched_run_due(&s);
    icount = 9;
    uam_sched_run_due(&s);
    CHECK(sa.n == 2 && !strncmp(sa.log, "rx", 2) && sb.n == 1 && sb.log[0] == 'y',
          "the same deliveries again");

    /* Connected to a hub that does not keep exact time: the frontends are told. */
    uam_link_node_set_sync(n, UAM_LINK_FREE, 0);
    uam_sched_cancel_all(&s);
    r = uam_state_reader_new(data, len, &s);
    uam_state_next(r);
    uam_link_node_restore(n, r);
    uam_state_leave(r, "link");
    uam_state_arm_timers(r);
    uam_state_reader_free(r);
    CHECK(sa.rewinds == 1 && sb.rewinds == 1, "free: rewound event");

    /* State from a node with other ports does not fit. */
    uam_link_node *other = uam_link_node_create("n1");
    uam_link_node_add_port(other, "a", "bus0", 0);
    r = uam_state_reader_new(data, len, NULL);
    uam_state_next(r);
    uam_link_node_restore(other, r);
    CHECK(!uam_state_ok(r), "a different port set is refused");
    uam_state_reader_free(r);
    uam_link_node_destroy(other);

    uam_state_writer_free(w);
    uam_link_node_destroy(n);
}

/* ---- the hub: routing --------------------------------------------------------- */

typedef struct rec {
    unsigned n, last_port;
    uam_link_msg last;
    uint8_t  data[16];
    uint64_t grant;
    unsigned grants, quiets, lost;
} rec;

static void rec_msg(void *ctx, unsigned port, const uam_link_msg *m)
{
    rec *r = ctx;
    r->n++;
    r->last_port = port;
    r->last = *m;
    if (m->flags & UAM_LINK_F_LOST)
        r->lost++;
    memcpy(r->data, m->data, m->len < sizeof(r->data) ? m->len : sizeof(r->data));
}

static void rec_grant(void *ctx, uint64_t time)
{
    rec *r = ctx;
    r->grant = time;
    r->grants++;
}

static void rec_quiet(void *ctx) { ((rec *)ctx)->quiets++; }

static const uam_link_member_ops rec_ops = { .msg = rec_msg, .grant = rec_grant,
                                             .quiescent = rec_quiet };

static void only_to_two(void *ctx, uam_link_hub *h, const char *bus, unsigned member,
                        unsigned port, const uam_link_msg *m)
{
    (void)bus; (void)member; (void)port;
    unsigned *calls = ctx;
    (*calls)++;
    uam_link_msg c = *m;
    c.flags |= 1u << UAM_LINK_F_MODEL_SHIFT;
    uam_link_hub_deliver(h, 2, 0, &c);
}

static void test_hub_routing(void)
{
    uam_link_hub *h = uam_link_hub_create(NULL);
    CHECK(uam_link_hub_get_config(h)->sync == UAM_LINK_FREE, "free by default");
    CHECK(uam_link_hub_get_config(h)->unit_hz == UAM_LINK_UNIT_HZ, "picoseconds by default");
    CHECK(uam_link_hub_add_bus(h, "wire", UAM_LINK_POINT_TO_POINT, 0) == UAM_OK, "declare a bus");
    CHECK(uam_link_hub_add_bus(h, "wire", UAM_LINK_BROADCAST, 0) == UAM_ERR_INVALID, "only once");
    rec r[3] = { 0 };
    int id[3];
    for (int i = 0; i < 3; i++) {
        id[i] = uam_link_hub_join(h, "m", &rec_ops, &r[i], 0);
        CHECK(id[i] == i, "join");
        CHECK(uam_link_hub_add_port(h, (unsigned)i, 0, "air", 0) == UAM_OK, "port on air");
    }
    CHECK(uam_link_hub_members(h) == 3, "three members");
    CHECK(uam_link_hub_add_port(h, 0, 0, "air", 0) == UAM_ERR_INVALID, "a port joins once");
    CHECK(uam_link_hub_add_port(h, 0, 1, "wire", 0) == UAM_OK
          && uam_link_hub_add_port(h, 1, 1, "wire", 0) == UAM_OK, "two ends of the wire");
    CHECK(uam_link_hub_add_port(h, 2, 1, "wire", 0) == UAM_ERR_REJECTED, "a third end is refused");

    /* Broadcast: everyone else on the bus, with the exact length. */
    uam_link_msg m = { .time = 77, .key = 3, .len = 3, .data = (const uint8_t *)"abc" };
    CHECK(uam_link_hub_msg(h, 0, 0, &m), "send on air");
    CHECK(r[0].n == 0 && r[1].n == 1 && r[2].n == 1, "the others got it, the sender did not");
    CHECK(r[1].last.len == 3 && !memcmp(r[1].data, "abc", 3) && r[1].last.key == 3, "intact");
    CHECK(r[1].last.src != 0 && r[1].last.src == r[2].last.src, "the hub names the source");
    /* Point to point. */
    m.len = 1;
    uam_link_hub_msg(h, 1, 1, &m);
    CHECK(r[0].n == 1 && r[0].last_port == 1 && r[2].n == 1, "only the other end");
    CHECK(!uam_link_hub_msg(h, 2, 1, &m), "a port that is not on a bus sends nothing");

    /* A medium of the caller's own. */
    unsigned calls = 0;
    CHECK(uam_link_hub_set_medium(h, "air", only_to_two, &calls) == UAM_OK, "custom medium");
    uam_link_hub_msg(h, 0, 0, &m);
    CHECK(calls == 1 && r[1].n == 1 && r[2].n == 2, "it decides who gets what");
    CHECK(r[2].last.flags == 1u << UAM_LINK_F_MODEL_SHIFT, "and may change it");

    /* A member that leaves gets nothing more. */
    uam_link_hub_leave(h, 2);
    uam_link_hub_msg(h, 0, 0, &m);
    CHECK(r[2].n == 2 && uam_link_hub_members(h) == 2, "left");
    uam_link_hub_destroy(h);
}

/* ---- the hub: granting -------------------------------------------------------- */

static void test_hub_grants(void)
{
    uam_link_hub_config cfg = { .sync = UAM_LINK_EXACT, .start_members = 2 };
    uam_link_hub *h = uam_link_hub_create(&cfg);
    rec a = { 0 }, b = { 0 };
    uam_link_hub_join(h, "a", &rec_ops, &a, 0);
    CHECK(uam_link_hub_add_port(h, 0, 0, "bus", 0) == UAM_ERR_REJECTED, "exact: latency 0 refused");
    uam_link_hub_add_port(h, 0, 0, "bus", 1000);
    uam_link_hub_report(h, 0, 0, 0, UINT64_MAX);
    CHECK(a.grants == 0, "nothing is granted before everyone has joined");
    uam_link_hub_join(h, "b", &rec_ops, &b, 0);
    uam_link_hub_add_port(h, 1, 0, "bus", 3000);
    uam_link_hub_report(h, 1, 0, 0, UINT64_MAX);
    /* Each may run to where the other's next message could take effect. */
    CHECK(a.grant == 3000 && b.grant == 1000, "grants: a %llu b %llu",
          (unsigned long long)a.grant, (unsigned long long)b.grant);
    uam_link_hub_report(h, 1, 1000, 1000, UINT64_MAX);
    CHECK(a.grant == 4000, "a follows b: %llu", (unsigned long long)a.grant);
    uam_link_hub_report(h, 0, 3000, 3000, UINT64_MAX);
    CHECK(b.grant == 4000, "b follows a: %llu", (unsigned long long)b.grant);

    /* b idle with nothing armed: a is held only by its own echo path
     * (a's message reaches b one latency later, b's answer another). */
    uam_link_hub_report(h, 1, 4000, UAM_LINK_NEVER, UINT64_MAX);
    CHECK(a.grant == 3000 + 1000 + 3000, "a runs ahead of a quiet b: %llu",
          (unsigned long long)a.grant);
    CHECK(a.quiets == 0 && b.quiets == 0, "not quiescent while a runs");
    /* A message to the quiet member means it may act from then on. */
    unsigned grants = a.grants;
    uam_link_msg m = { .time = 5000, .len = 1, .data = (const uint8_t *)"x" };
    uam_link_hub_msg(h, 0, 0, &m);
    uam_link_hub_report(h, 0, 6000, 6000, UINT64_MAX);
    CHECK(b.n == 1 && a.grant == 5000 + 3000, "a is held by b's wake-up at 5000: %llu",
          (unsigned long long)a.grant);
    CHECK(a.grants == grants + 1, "one new grant");

    /* A report made before the member read a message does not cancel the
     * wake-up: b says "quiet for ever" having taken none of the two sent. */
    m.time = 7000;
    uam_link_hub_msg(h, 0, 0, &m);
    uam_link_hub_report(h, 1, 6000, UAM_LINK_NEVER, 1);
    uam_link_hub_report(h, 0, 7500, UAM_LINK_NEVER, UINT64_MAX);
    CHECK(a.quiets == 0 && b.quiets == 0, "a message under way: not quiescent");
    CHECK(a.grant == 7000 + 3000, "a is still held by b's wake-up at 7000: %llu",
          (unsigned long long)a.grant);
    uam_link_hub_report(h, 1, 7000, 7000, 2);

    /* Everyone quiet: told once. */
    uam_link_hub_report(h, 1, 8000, UAM_LINK_NEVER, UINT64_MAX);
    uam_link_hub_report(h, 0, 8000, UAM_LINK_NEVER, UINT64_MAX);
    CHECK(a.quiets == 1 && b.quiets == 1, "quiescent");
    uam_link_hub_report(h, 0, 8000, UAM_LINK_NEVER, UINT64_MAX);
    CHECK(a.quiets == 1, "only once");

    /* The one left alone has no limit. */
    uam_link_hub_leave(h, 1);
    CHECK(a.grant == UAM_LINK_NEVER, "alone: no limit");
    uam_link_hub_destroy(h);

    /* Loose: the same, plus the skew. */
    uam_link_hub_config loose = { .sync = UAM_LINK_LOOSE, .skew = 50000 };
    h = uam_link_hub_create(&loose);
    rec c = { 0 }, d = { 0 };
    uam_link_hub_join(h, "c", &rec_ops, &c, 0);
    uam_link_hub_join(h, "d", &rec_ops, &d, 0);
    uam_link_hub_add_port(h, 0, 0, "bus", 1000);
    uam_link_hub_add_port(h, 1, 0, "bus", 1000);
    uam_link_hub_report(h, 0, 0, 0, UINT64_MAX);
    CHECK(c.grant == 51000 && d.grant == 51000, "loose: lookahead + skew");
    uam_link_hub_destroy(h);
}

/* An arbitrated bus: messages for the same time contend once nobody can add one. */
static void test_hub_arbitration(void)
{
    uam_link_hub_config cfg = { .sync = UAM_LINK_EXACT };
    uam_link_hub *h = uam_link_hub_create(&cfg);
    uam_link_hub_add_bus(h, "bus", UAM_LINK_ARBITRATED, 1000);
    rec r[3] = { 0 };
    for (unsigned i = 0; i < 3; i++) {
        uam_link_hub_join(h, "m", &rec_ops, &r[i], 0);
        uam_link_hub_add_port(h, i, 0, "bus", 500);
        uam_link_hub_report(h, i, 2000, 2000, UINT64_MAX);
    }
    uam_link_msg m = { .time = 2600, .key = 5, .len = 1, .data = (const uint8_t *)"A" };
    uam_link_hub_msg(h, 0, 0, &m);
    m.key = 2;
    m.time = 2900;
    m.data = (const uint8_t *)"B";
    uam_link_hub_msg(h, 1, 0, &m);
    CHECK(r[0].n + r[1].n + r[2].n == 0, "held: someone could still send for that slot");
    CHECK(r[2].grant == 2500, "granted up to what could still be sent: %llu",
          (unsigned long long)r[2].grant);
    uam_link_hub_report(h, 0, 2500, 2500, UINT64_MAX);
    uam_link_hub_report(h, 1, 2500, 2500, UINT64_MAX);
    uam_link_hub_report(h, 2, 2500, 2500, UINT64_MAX);
    CHECK(r[2].grant == 3000 && r[2].n == 0, "then up to the slot, still held");
    uam_link_hub_report(h, 0, 3000, 3000, UINT64_MAX);
    uam_link_hub_report(h, 1, 3000, 3000, UINT64_MAX);
    CHECK(r[2].n == 0, "one member has not passed the slot yet");
    uam_link_hub_report(h, 2, 3000, 3000, UINT64_MAX);
    CHECK(r[2].n == 1 && r[2].data[0] == 'B' && r[2].last.time == 3000, "the lowest key won");
    CHECK(r[0].n == 2, "the loser hears the winner and gets its own back");
    CHECK(r[0].lost == 1 && r[2].lost == 0, "marked lost");
    CHECK(r[1].n == 0, "the winner hears nothing");
    CHECK(r[2].grant == 3500, "and time moves on: %llu", (unsigned long long)r[2].grant);
    uam_link_hub_destroy(h);

    /* With no time kept every message has the bus to itself. */
    h = uam_link_hub_create(NULL);
    uam_link_hub_add_bus(h, "bus", UAM_LINK_ARBITRATED, 0);
    rec f[2] = { 0 };
    for (unsigned i = 0; i < 2; i++) {
        uam_link_hub_join(h, "m", &rec_ops, &f[i], 0);
        uam_link_hub_add_port(h, i, 0, "bus", 0);
    }
    uam_link_hub_msg(h, 0, 0, &m);
    CHECK(f[1].n == 1 && !(f[1].last.flags & UAM_LINK_F_LOST), "free: passed straight on");
    uam_link_hub_destroy(h);
}

/* ---- two machines in one process ---------------------------------------------- */

/* A device a guest talks through: write a word to send it, poll and read to
 * receive. Each receive is logged with the guest's time. */
#define PING_DEV   0x10000000u
#define PING_LOG   256u

typedef struct ping_dev {
    uam_device     dev;
    uam_machine   *m;
    uam_link_port *port;
    uint32_t       rx[64];
    unsigned       rx_head, rx_len;
    struct { uint64_t tick; uint32_t value; } log[PING_LOG];
    unsigned       n_log;
    unsigned       lates;
} ping_dev;

static bool ping_can(void *ctx) { return ((ping_dev *)ctx)->rx_len < 64; }

static void ping_rx(void *ctx, const uam_link_msg *m)
{
    ping_dev *d = ctx;
    uint32_t v = 0;
    memcpy(&v, m->data, m->len < 4 ? m->len : 4);
    d->rx[(d->rx_head + d->rx_len++) % 64] = v;
    if (m->flags & UAM_LINK_F_LATE)
        d->lates++;
}

static uam_status ping_read(uam_device *dev, uint64_t off, unsigned size, uint64_t *val)
{
    (void)size;
    ping_dev *d = UAM_CONTAINER_OF(dev, ping_dev, dev);
    *val = 0;
    if (off == 4) {
        *val = d->rx_len != 0;
    } else if (off == 8 && d->rx_len) {
        *val = d->rx[d->rx_head];
        d->rx_head = (d->rx_head + 1) % 64;
        d->rx_len--;
        if (d->n_log < PING_LOG) {
            d->log[d->n_log].tick = uam_sched_now(&d->m->sched);
            d->log[d->n_log].value = (uint32_t)*val;
        }
        d->n_log++;
        uam_link_accept_input(d->port);
    }
    return UAM_OK;
}

static uam_status ping_write(uam_device *dev, uint64_t off, unsigned size, uint64_t val)
{
    (void)size;
    ping_dev *d = UAM_CONTAINER_OF(dev, ping_dev, dev);
    uint32_t v = (uint32_t)val;
    if (off == 0)
        uam_link_send(d->port, v, 0, UAM_LINK_DATA, &v, 4);
    else if (off == 12)
        uam_exec_stop(&d->m->exec, 0);
    return UAM_OK;
}

static const uam_device_ops ping_ops = { .type = "test,ping", .read = ping_read,
                                         .write = ping_write, .flags = UAM_DEVICE_STATELESS };

/* RV32 encoders for the few instructions the guests use. */
static uint32_t rv_i(unsigned op, unsigned f3, unsigned rd, unsigned rs1, int imm)
{
    return ((uint32_t)imm & 0xFFF) << 20 | rs1 << 15 | f3 << 12 | rd << 7 | op;
}
static uint32_t rv_s(unsigned rs2, unsigned rs1, int imm)
{
    uint32_t u = (uint32_t)imm;
    return (u >> 5 & 0x7F) << 25 | rs2 << 20 | rs1 << 15 | 2u << 12 | (u & 0x1F) << 7 | 0x23;
}
static uint32_t rv_b(unsigned f3, unsigned rs1, unsigned rs2, int off)
{
    uint32_t u = (uint32_t)off;
    return (u >> 12 & 1) << 31 | (u >> 5 & 0x3F) << 25 | rs2 << 20 | rs1 << 15 | f3 << 12
         | (u >> 1 & 0xF) << 8 | (u >> 11 & 1) << 7 | 0x63;
}

enum { X0 = 0, T0 = 5, T1 = 6, T2 = 7, S1 = 9, T3 = 28 };

/* Answer every word received with word + 1 until `rounds` is reached, then
 * stop. `first` starts the exchange by sending 0. */
static void ping_program(uint32_t *p, bool first, int rounds)
{
    unsigned i = 0;
    p[i++] = (PING_DEV & 0xFFFFF000u) | S1 << 7 | 0x37;      /* 00: lui  s1, DEV */
    p[i++] = rv_i(0x13, 0, T3, X0, rounds);                  /* 04: li   t3, rounds */
    p[i++] = first ? rv_s(X0, S1, 0) : rv_i(0x13, 0, 0, 0, 0); /* 08: sw zero, 0(s1) / nop */
    p[i++] = rv_i(0x03, 2, T1, S1, 4);                       /* 0c: lw   t1, 4(s1) */
    p[i++] = rv_b(0, T1, X0, -4);                            /* 10: beqz t1, 0c */
    p[i++] = rv_i(0x03, 2, T2, S1, 8);                       /* 14: lw   t2, 8(s1) */
    p[i++] = rv_i(0x13, 0, T2, T2, 1);                       /* 18: addi t2, t2, 1 */
    p[i++] = rv_s(T2, S1, 0);                                /* 1c: sw   t2, 0(s1) */
    p[i++] = rv_b(4, T2, T3, -0x14);                         /* 20: blt  t2, t3, 0c */
    p[i++] = rv_s(T2, S1, 12);                               /* 24: sw   t2, 12(s1) */
    p[i++] = 0x0000006Fu;                                    /* 28: j    . */
}

typedef struct pnode {
    uam_machine    m;
    ping_dev       dev;
    uam_link_node *node;
} pnode;

static void pnode_build(pnode *pn, const char *name, bool first, int rounds, bool jit,
                        uint64_t latency, uint64_t tick_hz)
{
    memset(pn, 0, sizeof(*pn));
    CHECK(uam_machine_init(&pn->m, &uam_isa_rv32) == UAM_OK, "machine init");
    pn->m.board_name = "link-test";
    uam_bus_map_ram(&pn->m.bus, "ram", 0, 1u << 16);
    uam_device_init(&pn->dev.dev, &ping_ops, "ping");
    pn->dev.m = &pn->m;
    CHECK(uam_bus_map_mmio(&pn->m.bus, "ping", PING_DEV, 0x1000, &pn->dev.dev) == UAM_OK, "map");
    uint32_t prog[11];
    ping_program(prog, first, rounds);
    memcpy(uam_bus_region(&pn->m.bus, "ram")->host + 0x1000, prog, sizeof(prog));
    uam_machine_reset(&pn->m, 0x1000);
    if (jit)
        CHECK(uam_exec_enable_jit(&pn->m.exec), "jit available");
    pn->node = uam_link_node_create(name);
    pn->dev.port = uam_link_node_add_port(pn->node, "p0", "wire", latency);
    uam_link_port_set_frontend(pn->dev.port, ping_can, ping_rx, NULL, &pn->dev);
    CHECK(uam_link_node_bind(pn->node, &pn->m, tick_hz) == UAM_OK, "bind");
}

static void pnode_free(pnode *pn)
{
    uam_link_node_destroy(pn->node);
    uam_machine_fini(&pn->m);
}

/* Run a pair to the end; the two receive logs go to `out` (a then b). */
static unsigned run_pair(uam_link_sync sync, bool jit_a, bool jit_b, uint64_t slice,
                         uint64_t tick_hz_b, bool in_steps, ping_dev *out_a, ping_dev *out_b)
{
    static pnode a, b;
    uam_link_hub_config cfg = { .sync = sync, .skew = 200000 };
    uam_link_hub *h = uam_link_hub_create(&cfg);
    uam_link_hub_add_bus(h, "wire", UAM_LINK_POINT_TO_POINT, 0);
    pnode_build(&a, "a", true, 40, jit_a, 50000, 1000000000u);
    pnode_build(&b, "b", false, 40, jit_b, 50000, tick_hz_b);
    CHECK(uam_link_hub_attach(h, a.node) == UAM_OK && uam_link_hub_attach(h, b.node) == UAM_OK,
          "attach");
    uam_link_world *w = uam_link_world_create(h);
    uam_link_world_add(w, &a.m, a.node);
    uam_link_world_add(w, &b.m, b.node);
    uam_link_world_set_slice(w, slice);
    bool done = false;
    if (in_steps)       /* stopping at arbitrary times on the way changes nothing */
        for (uint64_t t = 777777; !done && t < UINT64_C(4000000000); t += 777777)
            done = uam_link_world_run(w, t);
    else
        done = uam_link_world_run(w, UAM_LINK_NEVER);
    CHECK(done, "the pair ran to the end");
    CHECK(uam_link_world_reason(w, 0) == UAM_STOP_REQUEST
          && uam_link_world_reason(w, 1) == UAM_STOP_REQUEST, "both stopped themselves");
    *out_a = a.dev;
    *out_b = b.dev;
    unsigned total = a.dev.n_log + b.dev.n_log;
    uam_link_world_destroy(w);
    pnode_free(&a);
    pnode_free(&b);
    uam_link_hub_destroy(h);
    return total;
}

static bool same_log(const ping_dev *x, const ping_dev *y)
{
    return x->n_log == y->n_log && !memcmp(x->log, y->log, x->n_log * sizeof(x->log[0]));
}

static void test_world(void)
{
    static ping_dev a0, b0, a1, b1;

    /* Exact: the words go back and forth, each received one latency after it
     * was sent, and nothing depends on the backend or on where runs are cut. */
    unsigned n = run_pair(UAM_LINK_EXACT, false, false, 4096, 1000000000u, false, &a0, &b0);
    CHECK(n == 41 && b0.n_log == 21 && a0.n_log == 20, "41 words received: %u", n);
    for (unsigned i = 0; i < b0.n_log; i++)
        CHECK(b0.log[i].value == 2 * i, "b receives the even words");
    CHECK(b0.log[0].tick >= 50 && b0.log[0].tick < 60, "first word after one latency: tick %llu",
          (unsigned long long)b0.log[0].tick);
    CHECK(a0.log[0].tick >= b0.log[0].tick + 50, "the answer one latency later");
    CHECK(a0.lates == 0 && b0.lates == 0, "exact: nothing is late");

    run_pair(UAM_LINK_EXACT, true, true, 4096, 1000000000u, false, &a1, &b1);
    CHECK(same_log(&a0, &a1) && same_log(&b0, &b1), "exact: interpreter and JIT agree");
    run_pair(UAM_LINK_EXACT, true, false, 4096, 1000000000u, false, &a1, &b1);
    CHECK(same_log(&a0, &a1) && same_log(&b0, &b1), "exact: mixed backends agree");
    run_pair(UAM_LINK_EXACT, false, false, 4096, 1000000000u, true, &a1, &b1);
    CHECK(same_log(&a0, &a1) && same_log(&b0, &b1), "exact: cutting the run changes nothing");

    /* Machines whose ticks are worth different amounts still meet in link time. */
    n = run_pair(UAM_LINK_EXACT, false, false, 4096, 250000000u, false, &a1, &b1);
    CHECK(n == 41 && a1.lates == 0 && b1.lates == 0, "different tick rates");
    CHECK(b1.log[0].tick >= 14 && b1.log[0].tick < 20, "b's ticks are 4 ns: tick %llu",
          (unsigned long long)b1.log[0].tick);

    /* Loose: everything arrives, perhaps late. */
    n = run_pair(UAM_LINK_LOOSE, false, false, 4096, 1000000000u, false, &a1, &b1);
    CHECK(n == 41, "loose: all words received");

    /* Free: no time is kept, but one process and a fixed slice are repeatable. */
    n = run_pair(UAM_LINK_FREE, false, false, 64, 1000000000u, false, &a0, &b0);
    CHECK(n == 41, "free: all words received");
    run_pair(UAM_LINK_FREE, false, false, 64, 1000000000u, false, &a1, &b1);
    CHECK(same_log(&a0, &a1) && same_log(&b0, &b1), "free: repeatable in one process");
}

int main(void)
{
    test_ports();
    test_send();
    test_timed();
    test_grants();
    test_state();
    test_hub_routing();
    test_hub_grants();
    test_hub_arbitration();
    test_world();
    printf("test_link: %d/%d checks passed\n", n_checks - n_failed, n_checks);
    return n_failed ? 1 : 0;
}
