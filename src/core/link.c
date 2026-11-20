/*
 * link.c - Links (uam/link.h): ports, a node's queues and clock, timed
 * delivery, waiting for the hub's grants, and the node's saved state.
 *
 * No host I/O: a transport (src/host) moves the messages.
 */
#include <stdlib.h>
#include <string.h>

#include "uam/link.h"

typedef struct qmsg {
    uam_link_msg m;       /* m.data is owned */
    uint64_t     tick;    /* when to hand it over (pending queue) */
    uint32_t     port;
} qmsg;

struct uam_link_port {
    uam_link_node *node;
    char           name[UAM_LINK_NAME_MAX], bus[UAM_LINK_NAME_MAX];
    uint64_t       latency;
    unsigned       index;
    bool         (*can_receive)(void *ctx);
    void         (*receive)(void *ctx, const uam_link_msg *m);
    void         (*event)(void *ctx, uam_link_event ev);
    void          *fe_ctx;
    qmsg          *ready;          /* due, waiting for the frontend: [r_head, r_head + r_len) */
    size_t         r_head, r_len, r_cap;
    bool           delivering;
    uint32_t       seq;
    uam_link_stats st;
};

struct uam_link_node {
    char            name[UAM_LINK_NAME_MAX];
    uam_link_port **ports;
    unsigned        n_ports;
    uam_sched      *sched;
    uint64_t        tick_hz, unit_hz;
    uam_link_sync   sync;
    const uam_link_transport *ops;
    void           *ops_ctx;
    bool            lost, quiescent, in_send;
    uint64_t        received;
    void          (*lost_fn)(void *ctx);
    void           *lost_ctx;
    qmsg           *pend;          /* not yet due, sorted by (tick, time, src, seq) */
    size_t          n_pend, cap_pend;
    uam_timer       deliver_t, grant_t;
    uint64_t        grant_time, grant_tick;
    uam_link_tap_fn tap;
    void           *tap_ctx;
    bool          (*wait)(void *ctx, uam_link_node *n);
    void           *wait_ctx;
};

/* ---- time -------------------------------------------------------------------- */

uint64_t uam_link_node_from_ticks(const uam_link_node *n, uint64_t ticks)
{
    if (!n->tick_hz)
        return 0;
    unsigned __int128 t = (unsigned __int128)ticks * n->unit_hz / n->tick_hz;
    return t > UINT64_MAX ? UINT64_MAX : (uint64_t)t;
}

uint64_t uam_link_node_to_ticks(const uam_link_node *n, uint64_t time)
{
    if (!n->tick_hz || time == UAM_LINK_NEVER)
        return UINT64_MAX;
    unsigned __int128 t = ((unsigned __int128)time * n->tick_hz + n->unit_hz - 1) / n->unit_hz;
    return t > UINT64_MAX ? UINT64_MAX : (uint64_t)t;
}

uint64_t uam_link_node_time(const uam_link_node *n)
{
    return n->sched ? uam_link_node_from_ticks(n, uam_sched_now(n->sched)) : 0;
}

/* Messages are delivered at their time (not on arrival). */
static bool stamped(const uam_link_node *n)
{
    return n->sched && n->sync != UAM_LINK_FREE;
}

bool uam_link_node_timed(const uam_link_node *n)
{
    return stamped(n) && n->ops && n->ops->poll && !n->lost;
}

/* ---- queues ------------------------------------------------------------------ */

static bool copy_msg(qmsg *q, const uam_link_msg *m)
{
    q->m = *m;
    uint8_t *d = malloc(m->len ? m->len : 1);
    if (!d)
        return false;
    if (m->len)
        memcpy(d, m->data, m->len);
    q->m.data = d;
    return true;
}

static void free_msg(qmsg *q)
{
    free((uint8_t *)(uintptr_t)q->m.data);
    q->m.data = NULL;
}

static bool ready_push(uam_link_port *p, qmsg q)
{
    if (!p->r_len)
        p->r_head = 0;
    if (p->r_head + p->r_len == p->r_cap) {
        if (p->r_head) {
            memmove(p->ready, p->ready + p->r_head, p->r_len * sizeof(qmsg));
            p->r_head = 0;
        } else {
            size_t cap = p->r_cap ? p->r_cap * 2 : 8;
            qmsg *nq = realloc(p->ready, cap * sizeof(qmsg));
            if (!nq)
                return false;
            p->ready = nq;
            p->r_cap = cap;
        }
    }
    p->ready[p->r_head + p->r_len++] = q;
    return true;
}

static void deliver(uam_link_port *p)
{
    uam_link_node *n = p->node;
    if (n->in_send || p->delivering)
        return;
    p->delivering = true;
    while (p->r_len && p->receive && (!p->can_receive || p->can_receive(p->fe_ctx))) {
        qmsg q = p->ready[p->r_head++];
        p->r_len--;
        p->st.rx++;
        if (n->tap)
            n->tap(n->tap_ctx, p, &q.m, false);
        p->receive(p->fe_ctx, &q.m);
        free_msg(&q);
    }
    p->delivering = false;
}

static void deliver_all(uam_link_node *n)
{
    for (unsigned i = 0; i < n->n_ports; i++)
        deliver(n->ports[i]);
}

static int pend_cmp(const qmsg *a, const qmsg *b)
{
    if (a->tick != b->tick)
        return a->tick < b->tick ? -1 : 1;
    if (a->m.time != b->m.time)
        return a->m.time < b->m.time ? -1 : 1;
    if (a->m.src != b->m.src)
        return a->m.src < b->m.src ? -1 : 1;
    if (a->m.seq != b->m.seq)
        return a->m.seq < b->m.seq ? -1 : 1;
    return a->port < b->port ? -1 : a->port > b->port;
}

static bool pend_insert(uam_link_node *n, qmsg q)
{
    if (n->n_pend == n->cap_pend) {
        size_t cap = n->cap_pend ? n->cap_pend * 2 : 16;
        qmsg *np = realloc(n->pend, cap * sizeof(qmsg));
        if (!np)
            return false;
        n->pend = np;
        n->cap_pend = cap;
    }
    size_t i = n->n_pend;
    while (i && pend_cmp(&q, &n->pend[i - 1]) < 0)
        i--;
    memmove(n->pend + i + 1, n->pend + i, (n->n_pend - i) * sizeof(qmsg));
    n->pend[i] = q;
    n->n_pend++;
    return true;
}

static void arm_delivery(uam_link_node *n)
{
    if (!n->sched)
        return;
    if (!n->n_pend) {
        uam_timer_cancel(n->sched, &n->deliver_t);
        return;
    }
    if (!n->deliver_t.armed || n->deliver_t.when > n->pend[0].tick)
        uam_timer_arm(n->sched, &n->deliver_t, n->pend[0].tick);
}

/* Hand over everything due. A delivery is the last thing to happen at its tick:
 * with another timer due, step behind it, so that the order does not depend on
 * when the message was read from the host. */
static void delivery_fire(void *ctx)
{
    uam_link_node *n = ctx;
    uam_sched *s = n->sched;
    uint64_t now = uam_sched_now(s);
    if (s->head && s->head->when <= now) {
        uam_timer_arm(s, &n->deliver_t, s->head->when);
        return;
    }
    size_t due = 0;
    while (due < n->n_pend && n->pend[due].tick <= now) {
        qmsg q = n->pend[due++];
        if (!ready_push(n->ports[q.port], q))
            free_msg(&q);
    }
    if (due) {
        n->n_pend -= due;
        memmove(n->pend, n->pend + due, n->n_pend * sizeof(qmsg));
    }
    arm_delivery(n);
    deliver_all(n);
}

/* Queue a message for a port: at `tick`, or now if that is 0 or past. */
static uam_status enqueue(uam_link_port *p, const uam_link_msg *m, uint64_t tick, bool may_be_late)
{
    uam_link_node *n = p->node;
    qmsg q = { .tick = tick, .port = p->index };
    if (m->len > UAM_LINK_MAX_MSG)
        return UAM_ERR_INVALID;
    if (!copy_msg(&q, m))
        return UAM_ERR_NOMEM;
    uint64_t now = n->sched ? uam_sched_now(n->sched) : 0;
    if (n->sched && tick >= now && tick != 0) {
        if (!pend_insert(n, q)) {
            free_msg(&q);
            return UAM_ERR_NOMEM;
        }
        arm_delivery(n);
        return UAM_OK;
    }
    if (may_be_late && n->sched && tick && tick < now) {
        q.m.flags |= UAM_LINK_F_LATE;
        p->st.late++;
    }
    if (!ready_push(p, q)) {
        free_msg(&q);
        return UAM_ERR_NOMEM;
    }
    deliver(p);
    return UAM_OK;
}

/* ---- frontend ---------------------------------------------------------------- */

void uam_link_port_set_frontend(uam_link_port *p, bool (*can_receive)(void *ctx),
                                void (*receive)(void *ctx, const uam_link_msg *m),
                                void (*event)(void *ctx, uam_link_event ev), void *ctx)
{
    if (!p)
        return;
    p->can_receive = can_receive;
    p->receive = receive;
    p->event = event;
    p->fe_ctx = ctx;
    if (event && uam_link_port_up(p))
        event(ctx, UAM_LINK_EV_UP);
    deliver(p);
}

bool uam_link_send(uam_link_port *p, uint64_t key, uint32_t flags, uint32_t kind,
                   const void *data, size_t len)
{
    if (!p || len > UAM_LINK_MAX_MSG || kind >= UAM_LINK_KIND_RESERVED)
        return false;
    uam_link_node *n = p->node;
    uint64_t now = uam_link_node_time(n);
    uam_link_msg m = { .time = now > UINT64_MAX - p->latency ? UINT64_MAX : now + p->latency,
                       .key = key, .flags = flags, .kind = kind, .seq = p->seq++,
                       .len = (uint32_t)len, .data = data };
    if (n->tap)
        n->tap(n->tap_ctx, p, &m, true);
    if (!n->ops || n->lost || !n->ops->send) {
        p->st.tx_dropped++;
        return false;
    }
    bool nested = n->in_send;
    n->in_send = true;
    bool ok = n->ops->send(n->ops_ctx, p->index, &m);
    n->in_send = nested;
    if (ok)
        p->st.tx++;
    else
        p->st.tx_dropped++;
    if (!nested)
        deliver_all(n);    /* what came back meanwhile */
    return ok;
}

void uam_link_accept_input(uam_link_port *p)
{
    if (p)
        deliver(p);
}

const char *uam_link_port_name(const uam_link_port *p) { return p->name; }
const char *uam_link_port_bus(const uam_link_port *p) { return p->bus; }
uint64_t uam_link_port_latency(const uam_link_port *p) { return p->latency; }
unsigned uam_link_port_index(const uam_link_port *p) { return p->index; }
uam_link_node *uam_link_port_node(const uam_link_port *p) { return p->node; }
const uam_link_stats *uam_link_port_stats(const uam_link_port *p) { return &p->st; }
size_t uam_link_port_ready(const uam_link_port *p) { return p->r_len; }

bool uam_link_port_up(const uam_link_port *p)
{
    return p->node->ops && !p->node->lost;
}

/* ---- node -------------------------------------------------------------------- */

static void grant_fire(void *ctx);

static void set_name(char *dst, const char *src)
{
    size_t len = src ? strlen(src) : 0;
    if (len >= UAM_LINK_NAME_MAX)
        len = UAM_LINK_NAME_MAX - 1;
    if (len)
        memcpy(dst, src, len);
    dst[len] = 0;
}

uam_link_node *uam_link_node_create(const char *name)
{
    uam_link_node *n = calloc(1, sizeof(*n));
    if (!n)
        return NULL;
    set_name(n->name, name);
    n->unit_hz = UAM_LINK_UNIT_HZ;
    n->grant_tick = 0;
    uam_timer_init(&n->deliver_t, delivery_fire, n);
    uam_timer_init(&n->grant_t, grant_fire, n);
    return n;
}

static void drop_queues(uam_link_node *n)
{
    for (size_t i = 0; i < n->n_pend; i++)
        free_msg(&n->pend[i]);
    n->n_pend = 0;
    for (unsigned i = 0; i < n->n_ports; i++) {
        uam_link_port *p = n->ports[i];
        for (size_t k = 0; k < p->r_len; k++)
            free_msg(&p->ready[p->r_head + k]);
        p->r_head = p->r_len = 0;
    }
}

void uam_link_node_destroy(uam_link_node *n)
{
    if (!n)
        return;
    if (n->sched) {
        uam_timer_cancel(n->sched, &n->deliver_t);
        uam_timer_cancel(n->sched, &n->grant_t);
    }
    if (n->ops && n->ops->close)
        n->ops->close(n->ops_ctx);
    drop_queues(n);
    for (unsigned i = 0; i < n->n_ports; i++) {
        free(n->ports[i]->ready);
        free(n->ports[i]);
    }
    free(n->ports);
    free(n->pend);
    free(n);
}

const char *uam_link_node_name(const uam_link_node *n) { return n->name; }

uam_link_port *uam_link_node_add_port(uam_link_node *n, const char *name, const char *bus,
                                      uint64_t latency)
{
    if (!n || !name || !*name || uam_link_node_find_port(n, name))
        return NULL;
    uam_link_port *p = calloc(1, sizeof(*p));
    uam_link_port **np = realloc(n->ports, (n->n_ports + 1) * sizeof(*np));
    if (!p || !np) {
        free(p);
        if (np)
            n->ports = np;
        return NULL;
    }
    n->ports = np;
    p->node = n;
    set_name(p->name, name);
    set_name(p->bus, bus && *bus ? bus : name);
    p->latency = latency;
    p->index = n->n_ports;
    n->ports[n->n_ports++] = p;
    return p;
}

uam_link_port *uam_link_node_find_port(const uam_link_node *n, const char *name)
{
    for (unsigned i = 0; n && name && i < n->n_ports; i++)
        if (!strcmp(n->ports[i]->name, name))
            return n->ports[i];
    return NULL;
}

unsigned uam_link_node_ports(const uam_link_node *n) { return n->n_ports; }

uam_link_port *uam_link_node_port(const uam_link_node *n, unsigned index)
{
    return index < n->n_ports ? n->ports[index] : NULL;
}

/* A node that waits for grants keeps a timer at the tick it may not pass. */
static void arm_grant(uam_link_node *n)
{
    if (!n->sched)
        return;
    if (!uam_link_node_timed(n) || n->grant_tick == UINT64_MAX)
        uam_timer_cancel(n->sched, &n->grant_t);
    else
        uam_timer_arm(n->sched, &n->grant_t, n->grant_tick);
}

uam_status uam_link_node_set_clock(uam_link_node *n, uam_sched *sched, uint64_t tick_hz)
{
    if (!n || !sched || !tick_hz || (n->sched && n->sched != sched))
        return UAM_ERR_INVALID;
    n->sched = sched;
    n->tick_hz = tick_hz;
    n->grant_tick = uam_link_node_to_ticks(n, n->grant_time);
    arm_grant(n);
    return UAM_OK;
}

uam_sched *uam_link_node_sched(const uam_link_node *n) { return n->sched; }

uam_status uam_link_port_inject(uam_link_port *p, const uam_link_msg *m, uint64_t at_tick)
{
    if (!p || !m)
        return UAM_ERR_INVALID;
    return enqueue(p, m, at_tick, false);
}

void uam_link_node_set_tap(uam_link_node *n, uam_link_tap_fn fn, void *ctx)
{
    n->tap = fn;
    n->tap_ctx = ctx;
}

bool uam_link_node_lost(const uam_link_node *n) { return n->lost; }

void uam_link_node_set_lost_handler(uam_link_node *n, void (*fn)(void *ctx), void *ctx)
{
    n->lost_fn = fn;
    n->lost_ctx = ctx;
}

/* ---- transport side ---------------------------------------------------------- */

static void port_events(uam_link_node *n, uam_link_event ev)
{
    for (unsigned i = 0; i < n->n_ports; i++)
        if (n->ports[i]->event)
            n->ports[i]->event(n->ports[i]->fe_ctx, ev);
}

void uam_link_node_set_transport(uam_link_node *n, const uam_link_transport *ops, void *ctx)
{
    bool was_up = n->ops && !n->lost;
    if (n->ops && n->ops->close && (n->ops != ops || n->ops_ctx != ctx))
        n->ops->close(n->ops_ctx);
    n->ops = ops;
    n->ops_ctx = ctx;
    n->lost = false;
    n->quiescent = false;
    if (!ops) {
        n->sync = UAM_LINK_FREE;
        n->grant_time = 0;
        n->grant_tick = 0;
    }
    arm_grant(n);
    bool up = ops != NULL;
    if (up != was_up)
        port_events(n, up ? UAM_LINK_EV_UP : UAM_LINK_EV_DOWN);
}

bool uam_link_node_connected(const uam_link_node *n) { return n->ops && !n->lost; }

void uam_link_node_set_sync(uam_link_node *n, uam_link_sync sync, uint64_t unit_hz)
{
    n->sync = sync;
    if (unit_hz)
        n->unit_hz = unit_hz;
    n->grant_tick = uam_link_node_to_ticks(n, n->grant_time);
    arm_grant(n);
}

uam_link_sync uam_link_node_sync(const uam_link_node *n) { return n->sync; }
uint64_t uam_link_node_unit_hz(const uam_link_node *n) { return n->unit_hz; }

void uam_link_node_input(uam_link_node *n, unsigned port, const uam_link_msg *m)
{
    if (port >= n->n_ports || m->kind >= UAM_LINK_KIND_RESERVED)
        return;
    n->quiescent = false;
    n->received++;
    uint64_t tick = 0;
    if (stamped(n)) {
        tick = uam_link_node_to_ticks(n, m->time);
        if (!tick)
            tick = 1;       /* "now" for enqueue is 0; nothing is due before tick 1 anyway */
    }
    (void)enqueue(n->ports[port], m, tick, true);
}

void uam_link_node_grant(uam_link_node *n, uint64_t time)
{
    if (time <= n->grant_time)
        return;
    n->grant_time = time;
    n->grant_tick = uam_link_node_to_ticks(n, time);
    if (n->sched && n->grant_t.armed)
        arm_grant(n);
}

void uam_link_node_quiescent(uam_link_node *n) { n->quiescent = true; }

void uam_link_node_set_lost(uam_link_node *n)
{
    if (n->lost || !n->ops)
        return;
    n->lost = true;
    if (n->sched)
        uam_timer_cancel(n->sched, &n->grant_t);
    port_events(n, UAM_LINK_EV_DOWN);
    if (n->lost_fn)
        n->lost_fn(n->lost_ctx);
}

/* ---- keeping time ------------------------------------------------------------ */

uint64_t uam_link_node_grant_tick(const uam_link_node *n)
{
    return stamped(n) && n->ops && !n->lost ? n->grant_tick : UINT64_MAX;
}

uint64_t uam_link_node_next_delivery(const uam_link_node *n)
{
    return n->n_pend ? n->pend[0].tick : UINT64_MAX;
}

void uam_link_node_report(uam_link_node *n, uint64_t reached, uint64_t quiet)
{
    if (n->ops && !n->lost && n->ops->report)
        n->ops->report(n->ops_ctx, reached, quiet);
}

uint64_t uam_link_node_received(const uam_link_node *n) { return n->received; }

bool uam_link_node_poll(uam_link_node *n)
{
    if (!n->ops || n->lost)
        return !n->lost;
    if (n->ops->poll && !n->ops->poll(n->ops_ctx, 0)) {
        uam_link_node_set_lost(n);
        return false;
    }
    return true;
}

int uam_link_node_fd(const uam_link_node *n)
{
    return n->ops && !n->lost && n->ops->fd ? n->ops->fd(n->ops_ctx) : -1;
}

void uam_link_node_set_waiter(uam_link_node *n, bool (*wait)(void *ctx, uam_link_node *n),
                              void *ctx)
{
    n->wait = wait;
    n->wait_ctx = ctx;
}

/* Wait for the hub once. False: gave up (asked to stop) or the hub is gone. */
static bool wait_once(uam_link_node *n)
{
    if (n->wait)
        return n->wait(n->wait_ctx, n) && !n->lost;
    if (!n->ops->poll(n->ops_ctx, -1)) {
        uam_link_node_set_lost(n);
        return false;
    }
    return true;
}

/* A processor reached the grant while running: say so and wait for more. */
static void grant_fire(void *ctx)
{
    uam_link_node *n = ctx;
    if (!uam_link_node_timed(n))
        return;
    uint64_t now = uam_sched_now(n->sched);
    if (n->grant_tick > now) {          /* moved while the timer was due */
        arm_grant(n);
        return;
    }
    uint64_t t = uam_link_node_from_ticks(n, now);
    n->ops->report(n->ops_ctx, t, t);
    bool ok = true;
    while (ok && n->grant_tick <= now)
        ok = wait_once(n);
    if (!uam_link_node_timed(n))
        return;                          /* the hub is gone: run on alone */
    if (ok)
        arm_grant(n);
    else                                 /* asked to stop: the run ends within a block */
        uam_timer_arm(n->sched, &n->grant_t, now + 1);
}

/* The earliest timer other than the grant timer. */
static uint64_t next_own_event(const uam_link_node *n)
{
    for (const uam_timer *t = n->sched->head; t; t = t->next)
        if (t != &n->grant_t)
            return t->when;
    return UINT64_MAX;
}

int uam_link_node_idle(uam_link_node *n, uint64_t limit, bool promise)
{
    if (!uam_link_node_timed(n))
        return 0;
    uam_sched *s = n->sched;
    uint64_t now = uam_sched_now(s), g = n->grant_tick, event = next_own_event(n);
    if (event <= g)
        return 0;
    uam_timer_cancel(s, &n->grant_t);
    if (now < g) {
        uam_sched_skip_to(s, g < limit ? g : limit);
        now = uam_sched_now(s);
    }
    if (now < g || now >= limit) {
        arm_grant(n);
        return 1;
    }
    n->quiescent = false;
    uint64_t t = uam_link_node_from_ticks(n, now);
    uint64_t quiet = !promise ? t : event == UINT64_MAX ? UAM_LINK_NEVER
                                                         : uam_link_node_from_ticks(n, event);
    n->ops->report(n->ops_ctx, t, quiet);
    /* Having promised, stay put until the grant covers the next thing that can
     * happen here (a message that arrives meanwhile is one). Without a promise
     * the others wait for this node, so follow every grant. */
    bool ok = true;
    while (ok && !n->quiescent
           && (promise ? n->grant_tick < next_own_event(n) : n->grant_tick <= now))
        ok = wait_once(n);
    if (!uam_link_node_timed(n))
        return 1;
    bool dead = n->quiescent && promise && next_own_event(n) == UINT64_MAX;
    arm_grant(n);
    return dead ? -1 : 1;
}

/* ---- state ------------------------------------------------------------------- */

static void put_msg(uam_state_writer *w, const qmsg *q)
{
    uam_state_put_u64(w, q->m.time);
    uam_state_put_u64(w, q->m.key);
    uam_state_put_u32(w, q->m.flags);
    uam_state_put_u32(w, q->m.kind);
    uam_state_put_u32(w, q->m.src);
    uam_state_put_u32(w, q->m.seq);
    uam_state_put_u64(w, q->tick);
    uam_state_put_u32(w, q->port);
    uam_state_put_u32(w, q->m.len);
    if (q->m.len)
        uam_state_put_bytes(w, q->m.data, q->m.len);
}

static bool get_msg(uam_state_reader *r, uam_link_node *n, qmsg *q)
{
    memset(q, 0, sizeof(*q));
    q->m.time = uam_state_get_u64(r);
    q->m.key = uam_state_get_u64(r);
    q->m.flags = uam_state_get_u32(r);
    q->m.kind = uam_state_get_u32(r);
    q->m.src = uam_state_get_u32(r);
    q->m.seq = uam_state_get_u32(r);
    q->tick = uam_state_get_u64(r);
    q->port = uam_state_get_u32(r);
    q->m.len = uam_state_get_u32(r);
    if (!uam_state_ok(r))
        return false;
    if (q->m.len > UAM_LINK_MAX_MSG || q->port >= n->n_ports) {
        uam_state_fail(r, "link %s: a saved message does not fit this node", n->name);
        return false;
    }
    uint8_t *d = malloc(q->m.len ? q->m.len : 1);
    if (!d) {
        uam_state_fail(r, "link %s: out of memory", n->name);
        return false;
    }
    if (q->m.len)
        uam_state_get_bytes(r, d, q->m.len);
    q->m.data = d;
    return true;
}

void uam_link_node_save(uam_link_node *n, uam_state_writer *w)
{
    uam_state_put_u32(w, n->n_ports);
    uam_state_put_u64(w, n->grant_time);
    for (unsigned i = 0; i < n->n_ports; i++) {
        const uam_link_port *p = n->ports[i];
        uam_state_put_u32(w, p->seq);
        uam_state_put_u64(w, p->r_len);
        for (size_t k = 0; k < p->r_len; k++)
            put_msg(w, &p->ready[p->r_head + k]);
    }
    uam_state_put_u64(w, n->n_pend);
    for (size_t i = 0; i < n->n_pend; i++)
        put_msg(w, &n->pend[i]);
    uam_state_put_timer(w, &n->deliver_t);
    uam_state_put_bool(w, n->grant_t.armed);
    uam_state_put_timer(w, &n->grant_t);
}

void uam_link_node_restore(uam_link_node *n, uam_state_reader *r)
{
    drop_queues(n);
    uam_state_expect_u32(r, n->n_ports, "link ports");
    uint64_t grant = uam_state_get_u64(r);
    for (unsigned i = 0; i < n->n_ports && uam_state_ok(r); i++) {
        uam_link_port *p = n->ports[i];
        p->seq = uam_state_get_u32(r);
        uint64_t count = uam_state_get_u64(r);
        for (uint64_t k = 0; k < count && uam_state_ok(r); k++) {
            qmsg q;
            if (get_msg(r, n, &q) && !ready_push(p, q))
                free_msg(&q);
        }
    }
    uint64_t count = uam_state_get_u64(r);
    for (uint64_t k = 0; k < count && uam_state_ok(r); k++) {
        qmsg q;
        if (get_msg(r, n, &q) && !pend_insert(n, q))
            free_msg(&q);
    }
    uam_state_get_timer(r, &n->deliver_t);
    bool grant_armed = uam_state_get_bool(r);
    uam_state_get_timer(r, &n->grant_t);
    if (!uam_state_ok(r))
        return;
    n->quiescent = false;
    n->in_send = false;
    if (uam_link_node_connected(n)) {
        /* A live hub does not go back with us: keep what it has granted since. */
        if (grant > n->grant_time)
            n->grant_time = grant;
        if (n->sync != UAM_LINK_EXACT)
            port_events(n, UAM_LINK_EV_REWOUND);
    } else {
        n->grant_time = grant;
    }
    n->grant_tick = uam_link_node_to_ticks(n, n->grant_time);
    /* Saved without a hub, restored with one that keeps time: start waiting. */
    if (!grant_armed && uam_link_node_timed(n) && n->grant_tick != UINT64_MAX)
        uam_timer_arm(n->sched, &n->grant_t, n->grant_tick);
}
