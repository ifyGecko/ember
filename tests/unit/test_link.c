/*
 * test_link.c - Unit tests for links (uam/link.h): ports and queues, timed
 * delivery and its order, waiting for grants, a node's saved state.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uam/link.h"

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

static void fh_report(void *ctx, uint64_t reached, uint64_t quiet)
{
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
    CHECK(uam_link_node_grant_tick(n) == 60 && uam_sched_next(&s) == 60, "granted and re-armed");
    /* Without a promise (host input could wake the machine) quiet = reached. */
    CHECK(uam_link_node_idle(n, UINT64_MAX, false) == 1, "waited again");
    CHECK(h.quiet == h.reached && h.reached == 60000, "no promise");
    /* The limit is respected. */
    CHECK(uam_link_node_idle(n, 65, true) == 1 && uam_sched_now(&s) == 65, "stops at the limit");
    uam_timer_cancel(&s, &ot);

    /* A message that arrives while waiting ends the wait. */
    uam_link_node_idle(n, UINT64_MAX, true);       /* to the grant (70) and one more step */
    h.step = 0;
    h.push = true;
    uint64_t at = uam_link_node_grant_tick(n);
    uam_link_node_idle(n, UINT64_MAX, true);
    CHECK(uam_link_node_next_delivery(n) == at, "the message is due at the grant");
    h.step = 10000;
    uam_sched_run_due(&s);                         /* the grant timer waits again; then the delivery */
    CHECK(sa.n == 1 && sa.log[0] == 'p', "delivered");

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

int main(void)
{
    test_ports();
    test_send();
    test_timed();
    test_grants();
    test_state();
    printf("test_link: %d/%d checks passed\n", n_checks - n_failed, n_checks);
    return n_failed ? 1 : 0;
}
