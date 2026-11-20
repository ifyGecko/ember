/*
 * link_hub.c - The link hub (uam/host.h): routes messages between nodes over
 * named buses and, in the timed modes, grants each node the time it may run to.
 *
 * The hub knows members, not machines: something that takes messages and
 * grants (uam_link_member_ops). A node in the same process is one kind of
 * member (uam_link_hub_attach, below); a connection to another process is the
 * other (link_wire.c).
 *
 * Granting. Each member says where it stands (`reached`) and how long it stays
 * quiet if left alone (`quiet`). The earliest time member j can next act is
 *
 *     act(j) = min(quiet(j), min over its neighbours k of act(k) + latency(k))
 *
 * (a message from k can wake it), found by relaxation. Nothing from j then
 * takes effect before act(j) + latency(j), so member i may run to
 *
 *     grant(i) = min over its neighbours j of act(j) + latency(j)
 *
 * In loose mode the configured skew is added, trading late messages for fewer
 * round trips. Members are neighbours when they share a bus.
 */
#include <stdlib.h>
#include <string.h>

#include "uam/host.h"
#include "uam/machine.h"

typedef struct hport {
    unsigned member, port, bus;
    uint64_t latency;
} hport;

typedef struct held {
    uam_link_msg m;          /* data owned */
    unsigned     bus, member, port;
} held;

typedef struct hbus {
    char               name[UAM_LINK_NAME_MAX];
    uam_link_medium    medium;
    uint64_t           slot;
    uam_link_medium_fn fn;
    void              *fn_ctx;
} hbus;

typedef struct member {
    bool     alive, told_quiet;
    char     name[UAM_LINK_NAME_MAX];
    const uam_link_member_ops *ops;
    void    *ctx;
    uint64_t reached, quiet, latency, granted;
    uint64_t act;            /* scratch: the relaxation's result */
} member;

struct uam_link_hub {
    uam_link_hub_config cfg;
    member  *members;
    unsigned n_members, n_alive;
    hport   *ports;
    unsigned n_ports;
    hbus    *buses;
    unsigned n_buses;
    held    *held;
    unsigned n_held, cap_held;
    bool     busy;           /* inside settle(): run it again instead of nesting */
    bool     again;
};

static uint64_t sat_add(uint64_t a, uint64_t b)
{
    return a > UINT64_MAX - b ? UINT64_MAX : a + b;
}

uam_link_hub *uam_link_hub_create(const uam_link_hub_config *cfg)
{
    uam_link_hub *h = calloc(1, sizeof(*h));
    if (!h)
        return NULL;
    if (cfg)
        h->cfg = *cfg;
    if (!h->cfg.unit_hz)
        h->cfg.unit_hz = UAM_LINK_UNIT_HZ;
    return h;
}

void uam_link_hub_destroy(uam_link_hub *h)
{
    if (!h)
        return;
    for (unsigned i = 0; i < h->n_held; i++)
        free((uint8_t *)(uintptr_t)h->held[i].m.data);
    free(h->held);
    free(h->members);
    free(h->ports);
    free(h->buses);
    free(h);
}

const uam_link_hub_config *uam_link_hub_get_config(const uam_link_hub *h) { return &h->cfg; }
unsigned uam_link_hub_members(const uam_link_hub *h) { return h->n_alive; }

/* ---- buses ------------------------------------------------------------------- */

static int find_bus(const uam_link_hub *h, const char *name)
{
    for (unsigned i = 0; i < h->n_buses; i++)
        if (!strcmp(h->buses[i].name, name))
            return (int)i;
    return -1;
}

static int get_bus(uam_link_hub *h, const char *name)
{
    int b = find_bus(h, name);
    if (b >= 0)
        return b;
    if (!name || !*name || strlen(name) >= UAM_LINK_NAME_MAX)
        return -1;
    hbus *nb = realloc(h->buses, (h->n_buses + 1) * sizeof(*nb));
    if (!nb)
        return -1;
    h->buses = nb;
    memset(&nb[h->n_buses], 0, sizeof(*nb));
    strcpy(nb[h->n_buses].name, name);
    nb[h->n_buses].medium = UAM_LINK_BROADCAST;
    return (int)h->n_buses++;
}

uam_status uam_link_hub_add_bus(uam_link_hub *h, const char *name, uam_link_medium medium,
                                uint64_t slot)
{
    if (find_bus(h, name) >= 0 || medium == UAM_LINK_CUSTOM)
        return UAM_ERR_INVALID;
    int b = get_bus(h, name);
    if (b < 0)
        return UAM_ERR_INVALID;
    h->buses[b].medium = medium;
    h->buses[b].slot = slot;
    return UAM_OK;
}

uam_status uam_link_hub_set_medium(uam_link_hub *h, const char *name, uam_link_medium_fn fn,
                                   void *ctx)
{
    int b = fn ? get_bus(h, name) : -1;
    if (b < 0)
        return UAM_ERR_INVALID;
    h->buses[b].medium = UAM_LINK_CUSTOM;
    h->buses[b].fn = fn;
    h->buses[b].fn_ctx = ctx;
    return UAM_OK;
}

/* ---- granting ---------------------------------------------------------------- */

static bool neighbours(const uam_link_hub *h, unsigned a, unsigned b)
{
    for (unsigned i = 0; i < h->n_ports; i++) {
        if (h->ports[i].member != a)
            continue;
        for (unsigned k = 0; k < h->n_ports; k++)
            if (h->ports[k].member == b && h->ports[k].bus == h->ports[i].bus)
                return true;
    }
    return false;
}

static bool on_bus(const uam_link_hub *h, unsigned mem, unsigned bus)
{
    for (unsigned i = 0; i < h->n_ports; i++)
        if (h->ports[i].member == mem && h->ports[i].bus == bus)
            return true;
    return false;
}

static void send_to(uam_link_hub *h, unsigned mem, unsigned port, const uam_link_msg *m)
{
    member *d = &h->members[mem];
    if (!d->alive)
        return;
    if (m->time < d->quiet)
        d->quiet = m->time;          /* it may act from then on */
    d->told_quiet = false;
    d->ops->msg(d->ctx, port, m);
}

void uam_link_hub_deliver(uam_link_hub *h, unsigned member_id, unsigned port,
                          const uam_link_msg *m)
{
    if (member_id < h->n_members)
        send_to(h, member_id, port, m);
}

static void broadcast(uam_link_hub *h, unsigned bus, unsigned from, unsigned from_port,
                      const uam_link_msg *m)
{
    for (unsigned i = 0; i < h->n_ports; i++) {
        const hport *p = &h->ports[i];
        if (p->bus == bus && !(p->member == from && p->port == from_port))
            send_to(h, p->member, p->port, m);
    }
}

static void relax(uam_link_hub *h)
{
    for (unsigned i = 0; i < h->n_members; i++)
        h->members[i].act = h->members[i].alive ? h->members[i].quiet : UAM_LINK_NEVER;
    for (unsigned round = 0; round < h->n_members; round++) {
        bool changed = false;
        for (unsigned j = 0; j < h->n_members; j++) {
            if (!h->members[j].alive)
                continue;
            for (unsigned k = 0; k < h->n_members; k++) {
                if (k == j || !h->members[k].alive || !neighbours(h, j, k))
                    continue;
                uint64_t v = sat_add(h->members[k].act, h->members[k].latency);
                if (v < h->members[j].act) {
                    h->members[j].act = v;
                    changed = true;
                }
            }
        }
        if (!changed)
            break;
    }
}

/* Messages held on an arbitrated bus for time `t` are settled once nothing more
 * can be sent for that time: the lowest key takes the bus, the others go back
 * to their senders marked lost. */
static bool release_held(uam_link_hub *h)
{
    bool any = false;
    for (unsigned b = 0; b < h->n_buses; b++) {
        for (;;) {
            uint64_t t = UAM_LINK_NEVER;
            for (unsigned i = 0; i < h->n_held; i++)
                if (h->held[i].bus == b && h->held[i].m.time < t)
                    t = h->held[i].m.time;
            if (t == UAM_LINK_NEVER)
                break;
            uint64_t safe = UAM_LINK_NEVER;
            for (unsigned k = 0; k < h->n_members; k++)
                if (h->members[k].alive && on_bus(h, k, b)) {
                    uint64_t v = sat_add(h->members[k].act, h->members[k].latency);
                    if (v < safe)
                        safe = v;
                }
            if (safe <= t)
                break;
            int win = -1;
            for (unsigned i = 0; i < h->n_held; i++) {
                const held *c = &h->held[i];
                if (c->bus != b || c->m.time != t)
                    continue;
                const held *w = win < 0 ? NULL : &h->held[win];
                if (!w || c->m.key < w->m.key || (c->m.key == w->m.key && c->m.src < w->m.src))
                    win = (int)i;
            }
            unsigned kept = 0;
            for (unsigned i = 0; i < h->n_held; i++) {
                held c = h->held[i];
                if (c.bus != b || c.m.time != t) {
                    h->held[kept++] = c;
                    continue;
                }
                if ((int)i == win) {
                    broadcast(h, b, c.member, c.port, &c.m);
                } else {
                    c.m.flags |= UAM_LINK_F_LOST;
                    send_to(h, c.member, c.port, &c.m);
                }
                free((uint8_t *)(uintptr_t)c.m.data);
            }
            h->n_held = kept;
            any = true;
        }
    }
    return any;
}

static void settle_once(uam_link_hub *h)
{
    if (h->cfg.sync == UAM_LINK_FREE || h->n_alive < h->cfg.start_members)
        return;
    relax(h);
    if (release_held(h))
        relax(h);
    bool all_quiet = h->n_alive > 0 && !h->n_held;
    for (unsigned i = 0; i < h->n_members; i++)
        if (h->members[i].alive && h->members[i].act != UAM_LINK_NEVER)
            all_quiet = false;
    for (unsigned i = 0; i < h->n_members; i++) {
        member *m = &h->members[i];
        if (!m->alive)
            continue;
        uint64_t g = UAM_LINK_NEVER;
        for (unsigned j = 0; j < h->n_members; j++) {
            if (j == i || !h->members[j].alive || !neighbours(h, i, j))
                continue;
            uint64_t v = sat_add(h->members[j].act, h->members[j].latency);
            if (v < g)
                g = v;
        }
        if (h->cfg.sync == UAM_LINK_LOOSE)
            g = sat_add(g, h->cfg.skew);
        /* Not past a message still held on one of its buses. */
        for (unsigned k = 0; k < h->n_held; k++)
            if (on_bus(h, i, h->held[k].bus) && h->held[k].m.time < g)
                g = h->held[k].m.time;
        if (g > m->granted) {
            m->granted = g;
            m->ops->grant(m->ctx, g);
        }
        if (all_quiet && !m->told_quiet) {
            m->told_quiet = true;
            if (m->ops->quiescent)
                m->ops->quiescent(m->ctx);
        }
    }
}

/* Granting can make a member (one in this process) send at once; fold that into
 * one more pass instead of nesting. */
static void settle(uam_link_hub *h)
{
    if (h->busy) {
        h->again = true;
        return;
    }
    h->busy = true;
    do {
        h->again = false;
        settle_once(h);
    } while (h->again);
    h->busy = false;
}

/* ---- members ----------------------------------------------------------------- */

int uam_link_hub_join(uam_link_hub *h, const char *name, const uam_link_member_ops *ops,
                      void *ctx, uint64_t time)
{
    if (!ops || !ops->msg || !ops->grant)
        return -1;
    member *nm = realloc(h->members, (h->n_members + 1) * sizeof(*nm));
    if (!nm)
        return -1;
    h->members = nm;
    member *m = &nm[h->n_members];
    memset(m, 0, sizeof(*m));
    m->alive = true;
    m->ops = ops;
    m->ctx = ctx;
    m->reached = m->quiet = time;
    m->latency = UAM_LINK_NEVER;
    size_t len = name ? strlen(name) : 0;
    if (len >= UAM_LINK_NAME_MAX)
        len = UAM_LINK_NAME_MAX - 1;
    if (len)
        memcpy(m->name, name, len);
    h->n_alive++;
    return (int)h->n_members++;
}

uam_status uam_link_hub_add_port(uam_link_hub *h, unsigned member_id, unsigned port,
                                 const char *bus, uint64_t latency)
{
    if (member_id >= h->n_members || !h->members[member_id].alive)
        return UAM_ERR_INVALID;
    if (h->cfg.sync != UAM_LINK_FREE && !latency)
        return UAM_ERR_REJECTED;     /* nothing could ever be granted */
    int b = get_bus(h, bus);
    if (b < 0)
        return UAM_ERR_INVALID;
    unsigned on = 0;
    for (unsigned i = 0; i < h->n_ports; i++) {
        if (h->ports[i].member == member_id && h->ports[i].port == port)
            return UAM_ERR_INVALID;
        if (h->ports[i].bus == (unsigned)b)
            on++;
    }
    if (h->buses[b].medium == UAM_LINK_POINT_TO_POINT && on >= 2)
        return UAM_ERR_REJECTED;
    hport *np = realloc(h->ports, (h->n_ports + 1) * sizeof(*np));
    if (!np)
        return UAM_ERR_NOMEM;
    h->ports = np;
    np[h->n_ports++] = (hport){ .member = member_id, .port = port, .bus = (unsigned)b,
                                .latency = latency };
    if (latency < h->members[member_id].latency)
        h->members[member_id].latency = latency;
    return UAM_OK;
}

void uam_link_hub_leave(uam_link_hub *h, unsigned member_id)
{
    if (member_id >= h->n_members || !h->members[member_id].alive)
        return;
    h->members[member_id].alive = false;
    h->n_alive--;
    unsigned kept = 0;
    for (unsigned i = 0; i < h->n_ports; i++)
        if (h->ports[i].member != member_id)
            h->ports[kept++] = h->ports[i];
    h->n_ports = kept;
    kept = 0;
    for (unsigned i = 0; i < h->n_held; i++) {
        if (h->held[i].member == member_id)
            free((uint8_t *)(uintptr_t)h->held[i].m.data);
        else
            h->held[kept++] = h->held[i];
    }
    h->n_held = kept;
    /* The others no longer wait for it. */
    if (h->cfg.start_members > h->n_alive)
        h->cfg.start_members = h->n_alive;
    settle(h);
}

static bool hold(uam_link_hub *h, unsigned bus, unsigned mem, unsigned port, const uam_link_msg *m)
{
    if (h->n_held == h->cap_held) {
        unsigned cap = h->cap_held ? h->cap_held * 2 : 16;
        held *nh = realloc(h->held, cap * sizeof(*nh));
        if (!nh)
            return false;
        h->held = nh;
        h->cap_held = cap;
    }
    uint8_t *d = malloc(m->len ? m->len : 1);
    if (!d)
        return false;
    if (m->len)
        memcpy(d, m->data, m->len);
    held *c = &h->held[h->n_held++];
    c->m = *m;
    c->m.data = d;
    c->bus = bus;
    c->member = mem;
    c->port = port;
    /* Whoever is on the bus may act when it is settled. */
    for (unsigned k = 0; k < h->n_members; k++)
        if (h->members[k].alive && on_bus(h, k, bus)) {
            if (m->time < h->members[k].quiet)
                h->members[k].quiet = m->time;
            h->members[k].told_quiet = false;
        }
    return true;
}

bool uam_link_hub_msg(uam_link_hub *h, unsigned member_id, unsigned port, const uam_link_msg *in)
{
    const hport *from = NULL;
    for (unsigned i = 0; i < h->n_ports && !from; i++)
        if (h->ports[i].member == member_id && h->ports[i].port == port)
            from = &h->ports[i];
    if (!from || in->len > UAM_LINK_MAX_MSG)
        return false;
    uam_link_msg m = *in;
    m.src = member_id * 256u + port + 1;
    unsigned bus = from->bus;
    const hbus *b = &h->buses[bus];
    bool ok = true;
    if (b->medium == UAM_LINK_CUSTOM) {
        b->fn(b->fn_ctx, h, b->name, member_id, port, &m);
        settle(h);
    } else if (b->medium == UAM_LINK_ARBITRATED && h->cfg.sync != UAM_LINK_FREE) {
        if (b->slot > 1 && m.time % b->slot)
            m.time = sat_add(m.time, b->slot - m.time % b->slot);
        ok = hold(h, bus, member_id, port, &m);
        settle(h);
    } else {
        /* with no time kept, whatever arrives on an arbitrated bus has it */
        broadcast(h, bus, member_id, port, &m);
    }
    return ok;
}

void uam_link_hub_report(uam_link_hub *h, unsigned member_id, uint64_t reached, uint64_t quiet)
{
    if (member_id >= h->n_members || !h->members[member_id].alive)
        return;
    member *m = &h->members[member_id];
    if (reached > m->reached)
        m->reached = reached;
    m->quiet = quiet < m->reached ? m->reached : quiet;
    if (m->quiet != UAM_LINK_NEVER)
        for (unsigned i = 0; i < h->n_members; i++)
            h->members[i].told_quiet = false;
    settle(h);
}

void uam_link_hub_start(uam_link_hub *h)
{
    h->cfg.start_members = 0;
    settle(h);
}

/* ---- a node in this process as a member --------------------------------------- */

typedef struct local {
    uam_link_hub  *hub;
    uam_link_node *node;
    unsigned       id;
} local;

static void local_msg(void *ctx, unsigned port, const uam_link_msg *m)
{
    uam_link_node_input(((local *)ctx)->node, port, m);
}

static void local_grant(void *ctx, uint64_t time)
{
    uam_link_node_grant(((local *)ctx)->node, time);
}

static void local_quiescent(void *ctx)
{
    uam_link_node_quiescent(((local *)ctx)->node);
}

static const uam_link_member_ops local_member = { .msg = local_msg, .grant = local_grant,
                                                  .quiescent = local_quiescent };

static bool local_send(void *ctx, unsigned port, const uam_link_msg *m)
{
    local *l = ctx;
    return uam_link_hub_msg(l->hub, l->id, port, m);
}

static void local_report(void *ctx, uint64_t reached, uint64_t quiet)
{
    local *l = ctx;
    uam_link_hub_report(l->hub, l->id, reached, quiet);
}

static void local_close(void *ctx)
{
    local *l = ctx;
    uam_link_hub_leave(l->hub, l->id);
    free(l);
}

/* No poll: the owner drives the nodes (uam_link_world_run), nothing waits. */
static const uam_link_transport local_transport = { .type = "hub", .send = local_send,
                                                    .report = local_report, .close = local_close };

uam_status uam_link_hub_attach(uam_link_hub *h, uam_link_node *n)
{
    local *l = calloc(1, sizeof(*l));
    if (!l)
        return UAM_ERR_NOMEM;
    l->hub = h;
    l->node = n;
    uam_link_node_set_sync(n, h->cfg.sync, h->cfg.unit_hz);
    int id = uam_link_hub_join(h, uam_link_node_name(n), &local_member, l, uam_link_node_time(n));
    if (id < 0) {
        free(l);
        return UAM_ERR_NOMEM;
    }
    l->id = (unsigned)id;
    for (unsigned i = 0; i < uam_link_node_ports(n); i++) {
        const uam_link_port *p = uam_link_node_port(n, i);
        uam_status st = uam_link_hub_add_port(h, l->id, i, uam_link_port_bus(p),
                                              uam_link_port_latency(p));
        if (st != UAM_OK) {
            uam_log(UAM_LOG_ERROR, "link", "%s: port %s cannot join bus %s%s",
                    uam_link_node_name(n), uam_link_port_name(p), uam_link_port_bus(p),
                    st == UAM_ERR_REJECTED ? (uam_link_port_latency(p)
                        ? " (a point-to-point bus has two ends)"
                        : " (a latency of 0 cannot keep time)") : "");
            uam_link_hub_leave(h, l->id);
            free(l);
            return st;
        }
    }
    uam_link_node_set_transport(n, &local_transport, l);
    return UAM_OK;
}

/* ---- several machines in one process ----------------------------------------- */

typedef struct wnode {
    uam_machine    *m;
    uam_link_node  *n;
    bool            done, parked;
    uint64_t        seen;        /* messages it had received when it parked */
    uam_stop_reason reason;
} wnode;

struct uam_link_world {
    uam_link_hub *hub;
    wnode        *nodes;
    unsigned      n_nodes;
    uint64_t      slice;
};

uam_link_world *uam_link_world_create(uam_link_hub *hub)
{
    uam_link_world *w = calloc(1, sizeof(*w));
    if (w) {
        w->hub = hub;
        w->slice = 4096;
    }
    return w;
}

void uam_link_world_destroy(uam_link_world *w)
{
    if (w)
        free(w->nodes);
    free(w);
}

uam_status uam_link_world_add(uam_link_world *w, uam_machine *m, uam_link_node *n)
{
    if (!m || !n || uam_link_node_sched(n) != &m->sched)
        return UAM_ERR_INVALID;
    wnode *nn = realloc(w->nodes, (w->n_nodes + 1) * sizeof(*nn));
    if (!nn)
        return UAM_ERR_NOMEM;
    w->nodes = nn;
    nn[w->n_nodes++] = (wnode){ .m = m, .n = n };
    return UAM_OK;
}

void uam_link_world_set_slice(uam_link_world *w, uint64_t insns)
{
    w->slice = insns ? insns : 4096;
}

uam_stop_reason uam_link_world_reason(const uam_link_world *w, unsigned index)
{
    return index < w->n_nodes && w->nodes[index].done ? w->nodes[index].reason : UAM_STOP_NONE;
}

/* The earliest timer of a machine, or UINT64_MAX. */
static uint64_t next_event(const wnode *wn)
{
    return uam_sched_next(&wn->m->sched);
}

bool uam_link_world_run(uam_link_world *w, uint64_t until_time)
{
    bool timed = w->hub->cfg.sync != UAM_LINK_FREE;
    uam_link_hub_start(w->hub);
    for (;;) {
        /* Tell the hub where everyone stands; a parked machine does nothing
         * until a message (which arms its delivery timer) wakes it. */
        int pick = -1;
        uint64_t pick_time = UINT64_MAX;
        bool any = false;
        for (unsigned i = 0; i < w->n_nodes; i++) {
            wnode *wn = &w->nodes[i];
            if (!wn->done && wn->parked
                && (next_event(wn) != UINT64_MAX || uam_link_node_received(wn->n) != wn->seen))
                wn->parked = false;
            uint64_t t = uam_link_node_time(wn->n);
            if (timed)   /* a machine that has stopped holds nobody back */
                uam_link_node_report(wn->n, t, wn->done || wn->parked ? UAM_LINK_NEVER : t);
        }
        for (unsigned i = 0; i < w->n_nodes; i++) {
            wnode *wn = &w->nodes[i];
            if (wn->done || wn->parked)
                continue;
            any = true;
            uint64_t now = uam_sched_now(&wn->m->sched);
            uint64_t t = uam_link_node_time(wn->n);
            if (until_time != UAM_LINK_NEVER && t >= until_time)
                continue;
            if (uam_link_node_grant_tick(wn->n) <= now)
                continue;
            if (t < pick_time) {
                pick_time = t;
                pick = (int)i;
            }
        }
        if (pick < 0)
            return !any;         /* true: every machine stopped or waits for ever */
        wnode *wn = &w->nodes[pick];
        uam_exec *x = &wn->m->exec;
        uint64_t saved = x->max_time, limit = uam_link_node_grant_tick(wn->n);
        uint64_t until_tick = until_time == UAM_LINK_NEVER ? UINT64_MAX
                                                           : uam_link_node_to_ticks(wn->n, until_time);
        if (until_tick < limit)
            limit = until_tick;
        if (saved && saved < limit)
            limit = saved;
        x->max_time = limit == UINT64_MAX ? 0 : limit;
        uam_stop_reason r = uam_exec_run(x, timed ? 0 : w->slice);   /* 0: no instruction budget */
        x->max_time = saved;
        switch (r) {
        case UAM_STOP_INSN_LIMIT:
            break;
        case UAM_STOP_TIME_LIMIT:
            if (saved && uam_sched_now(&wn->m->sched) >= saved) {
                wn->done = true;
                wn->reason = r;
            }
            break;
        case UAM_STOP_DEADLOCK:
            wn->parked = true;   /* only a message can wake it */
            wn->seen = uam_link_node_received(wn->n);
            break;
        default:
            wn->done = true;
            wn->reason = r;
            break;
        }
    }
}
