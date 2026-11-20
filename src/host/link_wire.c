/*
 * link_wire.c - Links between processes (uam/host.h): the wire protocol, the
 * transport that connects a node to a hub in another process, and the server
 * that lets a hub take such connections.
 *
 * The protocol is a stream of frames over a unix or TCP socket, all integers
 * little-endian:
 *
 *     u32 length of the body    u16 type    u16 0    body
 *
 * A frame of a type the reader does not know is skipped, so later versions can
 * add frames. docs/link.md lists the frames; the F_* constants below are the
 * reference.
 */
#define _XOPEN_SOURCE 700
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "uam/host.h"

#define WIRE_VERSION 1u
#define WIRE_HDR     8u
#define WIRE_MAX     (UAM_LINK_MAX_MSG + 256u)

enum {
    F_HELLO = 1,      /* node: u32 version, u32 capabilities (0), name */
    F_WELCOME,        /* hub:  u32 version, u32 capabilities (0), u32 sync, u32 0, u64 unit_hz, u64 skew */
    F_PORT,           /* node: u32 port, u32 0, u64 latency (units), bus name */
    F_READY,          /* node: its ports are declared */
    F_JOINED,         /* hub:  the node is a member */
    F_ERROR,          /* hub:  text; the connection ends */
    F_MSG,            /* both: u32 port, u32 flags, u32 kind, u32 src, u32 seq, u32 0, u64 time, u64 key, payload */
    F_REPORT,         /* node: u64 reached, u64 quiet, u64 messages received so far */
    F_GRANT,          /* hub:  u64 time */
    F_QUIESCENT,      /* hub */
    F_BARRIER_AT,     /* node: u64 time - it stands at the barrier */
    F_BARRIER_DONE,   /* hub:  u64 time - every member does */
    F_BYE,            /* either: the end, on purpose */
};

/* ---- bytes ------------------------------------------------------------------- */

static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> 8 * i);
}

static void put64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> 8 * i);
}

static uint32_t get32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t get64(const uint8_t *p)
{
    return get32(p) | (uint64_t)get32(p + 4) << 32;
}

typedef struct buf {
    uint8_t *data;
    size_t   len, cap;
} buf;

static bool buf_room(buf *b, size_t more)
{
    if (b->len + more <= b->cap)
        return true;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->len + more)
        cap *= 2;
    uint8_t *d = realloc(b->data, cap);
    if (!d)
        return false;
    b->data = d;
    b->cap = cap;
    return true;
}

/* Append a frame: header, a fixed part, and a tail. */
static bool buf_frame(buf *b, unsigned type, const void *fixed, size_t fixed_len,
                      const void *tail, size_t tail_len)
{
    if (!buf_room(b, WIRE_HDR + fixed_len + tail_len))
        return false;
    uint8_t *p = b->data + b->len;
    put32(p, (uint32_t)(fixed_len + tail_len));
    p[4] = (uint8_t)type;
    p[5] = (uint8_t)(type >> 8);
    p[6] = p[7] = 0;
    if (fixed_len)
        memcpy(p + WIRE_HDR, fixed, fixed_len);
    if (tail_len)
        memcpy(p + WIRE_HDR + fixed_len, tail, tail_len);
    b->len += WIRE_HDR + fixed_len + tail_len;
    return true;
}

static bool frame_msg(buf *b, unsigned port, const uam_link_msg *m)
{
    uint8_t f[40];
    put32(f, port);
    put32(f + 4, m->flags);
    put32(f + 8, m->kind);
    put32(f + 12, m->src);
    put32(f + 16, m->seq);
    put32(f + 20, 0);
    put64(f + 24, m->time);
    put64(f + 32, m->key);
    return buf_frame(b, F_MSG, f, sizeof(f), m->data, m->len);
}

static bool parse_msg(const uint8_t *body, size_t len, unsigned *port, uam_link_msg *m)
{
    if (len < 40 || len - 40 > UAM_LINK_MAX_MSG)
        return false;
    *port = get32(body);
    *m = (uam_link_msg){ .flags = get32(body + 4), .kind = get32(body + 8),
                         .src = get32(body + 12), .seq = get32(body + 16),
                         .time = get64(body + 24), .key = get64(body + 32),
                         .len = (uint32_t)(len - 40), .data = body + 40 };
    return true;
}

/* ---- sockets ----------------------------------------------------------------- */

static bool unix_addr(const char *path, struct sockaddr_un *sa)
{
    memset(sa, 0, sizeof(*sa));
    sa->sun_family = AF_UNIX;
    if (!*path || strlen(path) >= sizeof(sa->sun_path))
        return false;
    strcpy(sa->sun_path, path);
    return true;
}

/* "tcp:[HOST:]PORT" after the prefix: host (may be empty) and port. */
static bool split_tcp(const char *s, char *host, size_t hostlen, char *port, size_t portlen)
{
    const char *colon = strrchr(s, ':');
    const char *p = colon ? colon + 1 : s;
    size_t hl = colon ? (size_t)(colon - s) : 0;
    if (!*p || strlen(p) >= portlen || hl >= hostlen)
        return false;
    memcpy(host, s, hl);
    host[hl] = 0;
    strcpy(port, p);
    return true;
}

static int open_socket(const char *spec, bool listening)
{
    int fd = -1;
    if (!strncmp(spec, "unix:", 5)) {
        struct sockaddr_un sa;
        if (!unix_addr(spec + 5, &sa))
            return -1;
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0)
            return -1;
        if (listening)
            unlink(sa.sun_path);
        int rc = listening ? bind(fd, (struct sockaddr *)&sa, sizeof(sa))
                           : connect(fd, (struct sockaddr *)&sa, sizeof(sa));
        if (rc != 0 || (listening && listen(fd, 16) != 0)) {
            close(fd);
            return -1;
        }
        return fd;
    }
    if (!strncmp(spec, "tcp:", 4)) {
        char host[128], port[16];
        if (!split_tcp(spec + 4, host, sizeof(host), port, sizeof(port)))
            return -1;
        struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM,
                                  .ai_flags = listening ? AI_PASSIVE : 0 }, *res = NULL;
        if (getaddrinfo(*host ? host : (listening ? "127.0.0.1" : "127.0.0.1"), port, &hints, &res)
            != 0 || !res)
            return -1;
        for (struct addrinfo *ai = res; ai && fd < 0; ai = ai->ai_next) {
            fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (fd < 0)
                continue;
            int one = 1;
            if (listening)
                setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
            int rc = listening ? bind(fd, ai->ai_addr, ai->ai_addrlen)
                               : connect(fd, ai->ai_addr, ai->ai_addrlen);
            if (rc != 0 || (listening && listen(fd, 16) != 0)) {
                close(fd);
                fd = -1;
            }
        }
        freeaddrinfo(res);
        return fd;
    }
    errno = EINVAL;
    return -1;
}

/* Write everything (the socket blocks; a hub always reads, so this ends). */
static bool write_all(int fd, const uint8_t *p, size_t len)
{
    while (len) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pf = { .fd = fd, .events = POLLOUT };
            poll(&pf, 1, 1000);
            continue;
        }
        if (n <= 0)
            return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

/* Read what is there without blocking. False: closed or broken. */
static bool read_some(int fd, buf *b)
{
    for (;;) {
        if (!buf_room(b, 65536))
            return false;
        ssize_t n = recv(fd, b->data + b->len, b->cap - b->len, MSG_DONTWAIT);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return true;
        if (n <= 0)
            return false;
        b->len += (size_t)n;
        if ((size_t)n < 65536)
            return true;
    }
}

/* The next whole frame in b at *off: its type and body. */
static bool next_frame(const buf *b, size_t *off, unsigned *type, const uint8_t **body,
                       size_t *len, bool *bad)
{
    if (b->len - *off < WIRE_HDR)
        return false;
    const uint8_t *p = b->data + *off;
    uint32_t n = get32(p);
    if (n > WIRE_MAX) {
        *bad = true;
        return false;
    }
    if (b->len - *off < WIRE_HDR + n)
        return false;
    *type = p[4] | (unsigned)p[5] << 8;
    *body = p + WIRE_HDR;
    *len = n;
    *off += WIRE_HDR + n;
    return true;
}

static void buf_consume(buf *b, size_t off)
{
    memmove(b->data, b->data + off, b->len - off);
    b->len -= off;
}

/* ---- the node's side ---------------------------------------------------------- */

typedef struct wire {
    int            fd;
    uam_link_node *node;
    buf            in, out;
    bool           dead, in_poll;
    uint64_t       barrier_done;     /* the last barrier the hub declared complete, + 1 */
} wire;

static bool wire_flush(wire *w)
{
    if (w->dead)
        return false;
    if (w->out.len && !write_all(w->fd, w->out.data, w->out.len))
        w->dead = true;
    w->out.len = 0;
    return !w->dead;
}

static bool wire_send(void *ctx, unsigned port, const uam_link_msg *m)
{
    wire *w = ctx;
    return frame_msg(&w->out, port, m) && wire_flush(w);
}

static void wire_report(void *ctx, uint64_t reached, uint64_t quiet, uint64_t received)
{
    wire *w = ctx;
    uint8_t f[24];
    put64(f, reached);
    put64(f + 8, quiet);
    put64(f + 16, received);
    if (buf_frame(&w->out, F_REPORT, f, sizeof(f), NULL, 0))
        wire_flush(w);
}

static int wire_fd(void *ctx)
{
    wire *w = ctx;
    return w->dead ? -1 : w->fd;
}

static void wire_dispatch(wire *w)
{
    size_t off = 0, len;
    unsigned type;
    const uint8_t *body;
    bool bad = false;
    while (!w->dead && next_frame(&w->in, &off, &type, &body, &len, &bad)) {
        switch (type) {
        case F_MSG: {
            unsigned port;
            uam_link_msg m;
            if (parse_msg(body, len, &port, &m))
                uam_link_node_input(w->node, port, &m);
            break;
        }
        case F_GRANT:
            if (len >= 8)
                uam_link_node_grant(w->node, get64(body));
            break;
        case F_QUIESCENT:
            uam_link_node_quiescent(w->node);
            break;
        case F_BARRIER_DONE:
            if (len >= 8)
                w->barrier_done = get64(body) + 1;
            break;
        case F_ERROR:
            uam_log(UAM_LOG_ERROR, "link", "%s: hub: %.*s", uam_link_node_name(w->node),
                    (int)len, (const char *)body);
            w->dead = true;
            break;
        case F_BYE:
            w->dead = true;
            break;
        default:
            break;      /* a frame from a later version */
        }
    }
    if (bad)
        w->dead = true;
    buf_consume(&w->in, off);
}

static bool wire_poll(void *ctx, int timeout_ms)
{
    wire *w = ctx;
    if (w->dead)
        return false;
    if (w->in_poll)         /* a delivery made the guest send: nothing to read twice */
        return true;
    if (timeout_ms) {
        struct pollfd pf = { .fd = w->fd, .events = POLLIN };
        int rc = poll(&pf, 1, timeout_ms);
        if (rc < 0 && errno != EINTR)
            w->dead = true;
        if (rc <= 0)
            return !w->dead;
    }
    w->in_poll = true;
    if (!read_some(w->fd, &w->in))
        w->dead = true;     /* still dispatch what arrived before the end */
    bool was_dead = w->dead;
    w->dead = false;
    wire_dispatch(w);
    w->dead = w->dead || was_dead;
    w->in_poll = false;
    return !w->dead;
}

static void wire_close(void *ctx)
{
    wire *w = ctx;
    if (!w->dead && buf_frame(&w->out, F_BYE, NULL, 0, NULL, 0))
        wire_flush(w);
    close(w->fd);
    free(w->in.data);
    free(w->out.data);
    free(w);
}

static const uam_link_transport wire_transport = { .type = "wire", .send = wire_send,
                                                   .report = wire_report, .fd = wire_fd,
                                                   .poll = wire_poll, .close = wire_close };

static void set_err(char *err, size_t errlen, const char *fmt, ...)
{
    if (!err || !errlen)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

/* Wait for one frame of the handshake (10 s). */
static bool handshake_frame(wire *w, unsigned *type, const uint8_t **body, size_t *len)
{
    for (int tries = 0; tries < 100; tries++) {
        size_t off = 0;
        bool bad = false;
        if (next_frame(&w->in, &off, type, body, len, &bad)) {
            /* The body stays valid until the buffer is touched again: the caller
             * consumes it with handshake_done. */
            w->barrier_done = off;
            return true;
        }
        if (bad)
            return false;
        struct pollfd pf = { .fd = w->fd, .events = POLLIN };
        if (poll(&pf, 1, 100) > 0 && !read_some(w->fd, &w->in))
            return false;
    }
    return false;
}

static void handshake_done(wire *w)
{
    buf_consume(&w->in, (size_t)w->barrier_done);
    w->barrier_done = 0;
}

uam_status uam_link_node_connect(uam_link_node *n, const char *spec, char *err, size_t errlen)
{
    if (!n || !spec)
        return UAM_ERR_INVALID;
    int fd = open_socket(spec, false);
    if (fd < 0) {
        set_err(err, errlen, "cannot reach a hub at %s: %s", spec,
                errno == EINVAL ? "use unix:PATH or tcp:HOST:PORT" : strerror(errno));
        return UAM_ERR_FAULT;
    }
    wire *w = calloc(1, sizeof(*w));
    if (!w) {
        close(fd);
        return UAM_ERR_NOMEM;
    }
    w->fd = fd;
    w->node = n;
    uam_status st = UAM_ERR_FAULT;
    unsigned type;
    const uint8_t *body;
    size_t len;
    uint8_t f[16];
    const char *name = uam_link_node_name(n);

    put32(f, WIRE_VERSION);
    put32(f + 4, 0);
    if (!buf_frame(&w->out, F_HELLO, f, 8, name, strlen(name)) || !wire_flush(w)
        || !handshake_frame(w, &type, &body, &len)) {
        set_err(err, errlen, "the hub at %s did not answer", spec);
        goto fail;
    }
    if (type == F_ERROR) {
        set_err(err, errlen, "hub: %.*s", (int)len, (const char *)body);
        goto fail;
    }
    if (type != F_WELCOME || len < 32 || get32(body + 8) > UAM_LINK_EXACT || !get64(body + 16)) {
        set_err(err, errlen, "%s is not a link hub", spec);
        goto fail;
    }
    uam_link_node_set_sync(n, (uam_link_sync)get32(body + 8), get64(body + 16));
    handshake_done(w);

    for (unsigned i = 0; i < uam_link_node_ports(n); i++) {
        const uam_link_port *p = uam_link_node_port(n, i);
        put32(f, i);
        put32(f + 4, 0);
        put64(f + 8, uam_link_port_latency_units(p));
        const char *bus = uam_link_port_bus(p);
        if (!buf_frame(&w->out, F_PORT, f, 16, bus, strlen(bus)))
            goto fail;
    }
    if (!buf_frame(&w->out, F_READY, NULL, 0, NULL, 0) || !wire_flush(w)
        || !handshake_frame(w, &type, &body, &len)) {
        set_err(err, errlen, "the hub at %s went away", spec);
        goto fail;
    }
    if (type != F_JOINED) {
        set_err(err, errlen, "hub: %.*s", type == F_ERROR ? (int)len : 22,
                type == F_ERROR ? (const char *)body : "unexpected answer");
        st = UAM_ERR_REJECTED;
        goto fail;
    }
    handshake_done(w);
    uam_link_node_set_transport(n, &wire_transport, w);
    return UAM_OK;

fail:
    uam_link_node_set_sync(n, UAM_LINK_FREE, 0);
    close(fd);
    free(w->in.data);
    free(w->out.data);
    free(w);
    return st;
}

/* ---- the hub's side ----------------------------------------------------------- */

typedef struct pending_port {
    unsigned port;
    uint64_t latency;
    char     bus[UAM_LINK_NAME_MAX];
} pending_port;

typedef struct client {
    struct uam_link_server *srv;
    int           fd;
    buf           in, out;
    int           member;            /* -1 until it has joined */
    bool          closing;
    char          name[UAM_LINK_NAME_MAX];
    pending_port *ports;
    unsigned      n_ports;
    bool          at_barrier;
    uint64_t      barrier;
} client;

struct uam_link_server {
    uam_link_hub *hub;
    int           lfd;
    char          path[108];         /* unix socket to remove at the end */
    client      **clients;
    unsigned      n_clients;
    bool          ever;
    FILE         *log;
    uint64_t      n_msgs;
};

static void client_msg(void *ctx, unsigned port, const uam_link_msg *m)
{
    client *c = ctx;
    if (!frame_msg(&c->out, port, m))
        c->closing = true;
}

static void client_grant(void *ctx, uint64_t time)
{
    client *c = ctx;
    uint8_t f[8];
    put64(f, time);
    if (!buf_frame(&c->out, F_GRANT, f, 8, NULL, 0))
        c->closing = true;
}

static void client_quiescent(void *ctx)
{
    client *c = ctx;
    buf_frame(&c->out, F_QUIESCENT, NULL, 0, NULL, 0);
}

static const uam_link_member_ops client_member = { .msg = client_msg, .grant = client_grant,
                                                   .quiescent = client_quiescent };

static void client_error(client *c, const char *fmt, ...)
{
    char text[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    buf_frame(&c->out, F_ERROR, NULL, 0, text, strlen(text));
    c->closing = true;
    if (c->srv->log)
        fprintf(c->srv->log, "hub: refused %s: %s\n", c->name[0] ? c->name : "a connection", text);
}

static void barrier_check(uam_link_server *s)
{
    uint64_t t = 0;
    unsigned members = 0;
    for (unsigned i = 0; i < s->n_clients; i++) {
        const client *c = s->clients[i];
        if (c->member < 0 || c->closing)
            continue;
        if (!c->at_barrier || (members && c->barrier != t))
            return;
        t = c->barrier;
        members++;
    }
    if (!members)
        return;
    uint8_t f[8];
    put64(f, t);
    for (unsigned i = 0; i < s->n_clients; i++) {
        client *c = s->clients[i];
        if (c->member >= 0 && !c->closing) {
            buf_frame(&c->out, F_BARRIER_DONE, f, 8, NULL, 0);
            c->at_barrier = false;
        }
    }
    if (s->log)
        fprintf(s->log, "hub: barrier at %llu\n", (unsigned long long)t);
}

static void client_frame(client *c, unsigned type, const uint8_t *body, size_t len)
{
    uam_link_server *s = c->srv;
    const uam_link_hub_config *cfg = uam_link_hub_get_config(s->hub);
    switch (type) {
    case F_HELLO: {
        if (len < 8 || get32(body) != WIRE_VERSION) {
            client_error(c, "protocol version %u is not spoken here (%u is)",
                         len >= 4 ? get32(body) : 0, WIRE_VERSION);
            return;
        }
        size_t nl = len - 8 < UAM_LINK_NAME_MAX - 1 ? len - 8 : UAM_LINK_NAME_MAX - 1;
        memcpy(c->name, body + 8, nl);
        c->name[nl] = 0;
        uint8_t f[32];
        put32(f, WIRE_VERSION);
        put32(f + 4, 0);
        put32(f + 8, (uint32_t)cfg->sync);
        put32(f + 12, 0);
        put64(f + 16, cfg->unit_hz);
        put64(f + 24, cfg->skew);
        buf_frame(&c->out, F_WELCOME, f, sizeof(f), NULL, 0);
        break;
    }
    case F_PORT: {
        if (len < 16 || len - 16 >= UAM_LINK_NAME_MAX || c->member >= 0)
            return;
        pending_port *np = realloc(c->ports, (c->n_ports + 1) * sizeof(*np));
        if (!np)
            return;
        c->ports = np;
        pending_port *p = &np[c->n_ports++];
        p->port = get32(body);
        p->latency = get64(body + 8);
        memcpy(p->bus, body + 16, len - 16);
        p->bus[len - 16] = 0;
        break;
    }
    case F_READY: {
        if (c->member >= 0 || !c->name[0])
            return;
        for (unsigned i = 0; i < s->n_clients; i++)
            if (s->clients[i] != c && s->clients[i]->member >= 0
                && !strcmp(s->clients[i]->name, c->name)) {
                client_error(c, "a node called %s is already connected (give each its own "
                             "name)", c->name);
                return;
            }
        int id = uam_link_hub_join(s->hub, c->name, &client_member, c, 0);
        if (id < 0) {
            client_error(c, "out of memory");
            return;
        }
        for (unsigned i = 0; i < c->n_ports; i++) {
            uam_status st = uam_link_hub_add_port(s->hub, (unsigned)id, c->ports[i].port,
                                                  c->ports[i].bus, c->ports[i].latency);
            if (st != UAM_OK) {
                uam_link_hub_leave(s->hub, (unsigned)id);
                client_error(c, "port %u cannot join bus %s%s", c->ports[i].port, c->ports[i].bus,
                             st != UAM_ERR_REJECTED ? ""
                             : c->ports[i].latency ? ": a point-to-point bus has two ends"
                             : ": this hub keeps time, so the port needs a latency (latency=...)");
                return;
            }
        }
        c->member = id;
        s->ever = true;
        buf_frame(&c->out, F_JOINED, NULL, 0, NULL, 0);
        if (s->log)
            fprintf(s->log, "hub: %s joined with %u port(s)\n", c->name, c->n_ports);
        /* Count it towards the start only now that its ports are known. */
        uam_link_hub_report(s->hub, (unsigned)id, 0, 0, UINT64_MAX);
        break;
    }
    case F_MSG: {
        unsigned port;
        uam_link_msg m;
        if (c->member < 0 || !parse_msg(body, len, &port, &m))
            return;
        s->n_msgs++;
        if (s->log)
            fprintf(s->log, "hub: %s port %u time %llu key 0x%llx len %u\n", c->name, port,
                    (unsigned long long)m.time, (unsigned long long)m.key, m.len);
        uam_link_hub_msg(s->hub, (unsigned)c->member, port, &m);
        break;
    }
    case F_REPORT:
        if (c->member >= 0 && len >= 24)
            uam_link_hub_report(s->hub, (unsigned)c->member, get64(body), get64(body + 8),
                                get64(body + 16));
        break;
    case F_BARRIER_AT:
        if (c->member >= 0 && len >= 8) {
            c->at_barrier = true;
            c->barrier = get64(body);
            uam_link_hub_report(s->hub, (unsigned)c->member, c->barrier, c->barrier, UINT64_MAX);
            barrier_check(s);
        }
        break;
    case F_BYE:
        c->closing = true;
        break;
    default:
        break;
    }
}

uam_link_server *uam_link_server_create(uam_link_hub *hub, const char *spec)
{
    int fd = open_socket(spec, true);
    if (fd < 0) {
        uam_log(UAM_LOG_ERROR, "link", "cannot listen on %s: %s", spec,
                errno == EINVAL ? "use unix:PATH or tcp:[HOST:]PORT" : strerror(errno));
        return NULL;
    }
    uam_link_server *s = calloc(1, sizeof(*s));
    if (!s) {
        close(fd);
        return NULL;
    }
    s->hub = hub;
    s->lfd = fd;
    if (!strncmp(spec, "unix:", 5))
        snprintf(s->path, sizeof(s->path), "%s", spec + 5);
    return s;
}

static void client_free(client *c)
{
    if (c->member >= 0)
        uam_link_hub_leave(c->srv->hub, (unsigned)c->member);
    if (c->srv->log && c->member >= 0)
        fprintf(c->srv->log, "hub: %s left\n", c->name);
    close(c->fd);
    free(c->in.data);
    free(c->out.data);
    free(c->ports);
    free(c);
}

void uam_link_server_destroy(uam_link_server *s)
{
    if (!s)
        return;
    for (unsigned i = 0; i < s->n_clients; i++)
        client_free(s->clients[i]);
    free(s->clients);
    close(s->lfd);
    if (s->path[0])
        unlink(s->path);
    free(s);
}

void uam_link_server_set_log(uam_link_server *s, FILE *log) { s->log = log; }
uint64_t uam_link_server_messages(const uam_link_server *s) { return s->n_msgs; }

bool uam_link_server_step(uam_link_server *s, int timeout_ms)
{
    struct pollfd *pf = calloc(s->n_clients + 1, sizeof(*pf));
    if (!pf)
        return false;
    pf[0] = (struct pollfd){ .fd = s->lfd, .events = POLLIN };
    for (unsigned i = 0; i < s->n_clients; i++)
        pf[i + 1] = (struct pollfd){ .fd = s->clients[i]->fd,
                                     .events = (short)(POLLIN | (s->clients[i]->out.len ? POLLOUT : 0)) };
    unsigned n = s->n_clients;
    int rc = poll(pf, n + 1, timeout_ms);
    if (rc > 0 && (pf[0].revents & POLLIN)) {
        int fd = accept(s->lfd, NULL, NULL);
        client *c = fd >= 0 ? calloc(1, sizeof(*c)) : NULL;
        client **nc = c ? realloc(s->clients, (s->n_clients + 1) * sizeof(*nc)) : NULL;
        if (nc) {
            s->clients = nc;
            c->srv = s;
            c->fd = fd;
            c->member = -1;
            nc[s->n_clients++] = c;
        } else {
            free(c);
            if (fd >= 0)
                close(fd);
        }
    }
    for (unsigned i = 0; rc > 0 && i < n; i++) {
        client *c = s->clients[i];
        if (!(pf[i + 1].revents & (POLLIN | POLLHUP | POLLERR)))
            continue;
        bool open = read_some(c->fd, &c->in);
        size_t off = 0, len;
        unsigned type;
        const uint8_t *body;
        bool bad = false;
        while (!c->closing && next_frame(&c->in, &off, &type, &body, &len, &bad))
            client_frame(c, type, body, len);
        buf_consume(&c->in, off);
        if (!open || bad)
            c->closing = true;
    }
    free(pf);
    /* Send what the hub decided; drop who is done. */
    unsigned kept = 0;
    bool left = false;
    for (unsigned i = 0; i < s->n_clients; i++) {
        client *c = s->clients[i];
        while (c->out.len) {
            ssize_t w = send(c->fd, c->out.data, c->out.len, MSG_NOSIGNAL | MSG_DONTWAIT);
            if (w < 0 && errno == EINTR)
                continue;
            if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                break;
            if (w <= 0) {
                c->closing = true;
                c->out.len = 0;
                break;
            }
            buf_consume(&c->out, (size_t)w);
        }
        if (c->closing && !c->out.len) {
            left = left || c->member >= 0;
            client_free(c);
        } else {
            s->clients[kept++] = c;
        }
    }
    s->n_clients = kept;
    if (left)
        barrier_check(s);    /* the others may have been waiting for it */
    unsigned members = 0;
    for (unsigned i = 0; i < s->n_clients; i++)
        members += s->clients[i]->member >= 0;
    return !(s->ever && !members);
}

/* ---- barrier ------------------------------------------------------------------ */

bool uam_link_node_barrier(uam_link_node *n, uint64_t time)
{
    void *ctx = NULL;
    if (uam_link_node_transport(n, &ctx) != &wire_transport)
        return true;        /* no hub in another process: nothing to wait for */
    wire *w = ctx;
    uint8_t f[8];
    put64(f, time);
    if (!buf_frame(&w->out, F_BARRIER_AT, f, 8, NULL, 0) || !wire_flush(w))
        return false;
    while (w->barrier_done != time + 1)
        if (!wire_poll(w, 1000) && w->dead) {
            uam_link_node_set_lost(n);
            return false;
        }
    return true;
}
