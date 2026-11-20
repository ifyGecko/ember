/*
 * link_adapters.c - Chardev and netdev backends on a link port (uam/host.h):
 * a device that knows only the chardev or netdev interface talks over a link.
 *
 * chardev: the bytes of one write are one message; a message's bytes are the
 *          device's input.
 * netdev:  a frame is a message, at its exact length (nothing is padded).
 *
 * Neither interface tells its backend when the device has room again, so what
 * does not fit stays in the port's queue and the adapter looks again a fixed
 * number of ticks later - a timer, so that it happens at the same guest time
 * in every run. The chardev's and netdev's own queues are saved with the port.
 */
#include <stdlib.h>
#include <string.h>

#include "uam/host.h"

/* How long to wait before looking whether a full device has room again. */
#define RETRY_TICKS 2000u

/* ---- chardev ----------------------------------------------------------------- */

typedef struct link_cd {
    uam_chardev    cd;
    uam_link_port *port;
    uint8_t       *rest;         /* the part of a message the chardev had no room for */
    size_t         rest_len, rest_off;
} link_cd;

static uam_sched *port_sched(const uam_link_port *p)
{
    return uam_link_node_sched(uam_link_port_node(p));
}

/* Push what is left of the last message; true once it has all gone. */
static bool cd_flush(link_cd *l)
{
    if (l->rest_off < l->rest_len)
        l->rest_off += uam_chardev_input(&l->cd, l->rest + l->rest_off, l->rest_len - l->rest_off);
    if (l->rest_off < l->rest_len)
        return false;
    l->rest_len = l->rest_off = 0;
    return true;
}

static bool cd_can_receive(void *ctx)
{
    link_cd *l = ctx;
    uam_chardev_accept_input(&l->cd);    /* the device may have made room since */
    if (cd_flush(l) && l->cd.rx_len < UAM_CHARDEV_RXBUF)
        return true;
    uam_link_port_retry(l->port, RETRY_TICKS);
    return false;
}

static void cd_receive(void *ctx, const uam_link_msg *m)
{
    link_cd *l = ctx;
    if (m->kind != UAM_LINK_DATA || !m->len)
        return;
    size_t took = uam_chardev_input(&l->cd, m->data, m->len);
    if (took == m->len)
        return;
    uint8_t *r = realloc(l->rest, m->len - took);
    if (!r)
        return;
    l->rest = r;
    l->rest_len = m->len - took;
    l->rest_off = 0;
    memcpy(r, m->data + took, l->rest_len);
}

static size_t cd_write(uam_chardev *cd, const void *buf, size_t len)
{
    link_cd *l = UAM_CONTAINER_OF(cd, link_cd, cd);
    uam_link_send(l->port, 0, 0, UAM_LINK_DATA, buf, len);
    return len;                          /* a guest is never blocked */
}

static void cd_poll(uam_chardev *cd)
{
    link_cd *l = UAM_CONTAINER_OF(cd, link_cd, cd);
    if (!port_sched(l->port))            /* no clock, no timer: look whenever asked */
        uam_link_accept_input(l->port);
}

static void cd_destroy(uam_chardev *cd)
{
    link_cd *l = UAM_CONTAINER_OF(cd, link_cd, cd);
    uam_link_port_set_frontend(l->port, NULL, NULL, NULL, NULL);
    uam_link_port_set_state(l->port, NULL, NULL, NULL);
    free(l->rest);
    free(l);
}

static void cd_save(void *ctx, uam_state_writer *w)
{
    link_cd *l = ctx;
    uam_state_put_u32(w, (uint32_t)l->cd.rx_len);
    for (size_t i = 0; i < l->cd.rx_len; i++)
        uam_state_put_u8(w, l->cd.rx[(l->cd.rx_head + i) % UAM_CHARDEV_RXBUF]);
    uam_state_put_u32(w, (uint32_t)(l->rest_len - l->rest_off));
    if (l->rest_len > l->rest_off)
        uam_state_put_bytes(w, l->rest + l->rest_off, l->rest_len - l->rest_off);
}

static void cd_restore(void *ctx, uam_state_reader *r)
{
    link_cd *l = ctx;
    uint32_t n = uam_state_get_u32(r);
    if (n > UAM_CHARDEV_RXBUF) {
        uam_state_fail(r, "link console: saved input does not fit");
        return;
    }
    l->cd.rx_head = 0;
    l->cd.rx_len = n;
    for (uint32_t i = 0; i < n; i++)
        l->cd.rx[i] = uam_state_get_u8(r);
    uint32_t rest = uam_state_get_u32(r);
    l->rest_len = l->rest_off = 0;
    if (rest && uam_state_ok(r)) {
        uint8_t *b = rest <= UAM_LINK_MAX_MSG ? realloc(l->rest, rest) : NULL;
        if (!b) {
            uam_state_fail(r, "link console: saved message does not fit");
            return;
        }
        l->rest = b;
        l->rest_len = rest;
        uam_state_get_bytes(r, b, rest);
    }
}

static const uam_chardev_ops link_cd_ops = { .type = "link", .write = cd_write, .poll = cd_poll,
                                             .destroy = cd_destroy };

uam_chardev *uam_chardev_link_create(uam_link_port *port)
{
    link_cd *l = port ? calloc(1, sizeof(*l)) : NULL;
    if (!l)
        return NULL;
    uam_chardev_init(&l->cd, &link_cd_ops);
    l->port = port;
    uam_link_port_set_state(port, cd_save, cd_restore, l);
    uam_link_port_set_frontend(port, cd_can_receive, cd_receive, NULL, l);
    return &l->cd;
}

/* ---- netdev ------------------------------------------------------------------ */

typedef struct link_nd {
    uam_netdev     nd;
    uam_link_port *port;
} link_nd;

static bool nd_can_receive(void *ctx)
{
    link_nd *l = ctx;
    uam_netdev_accept_input(&l->nd);
    if (uam_netdev_can_input(&l->nd))
        return true;
    uam_link_port_retry(l->port, RETRY_TICKS);
    return false;
}

static void nd_receive(void *ctx, const uam_link_msg *m)
{
    link_nd *l = ctx;
    if (m->kind == UAM_LINK_DATA && !(m->flags & UAM_LINK_F_LOST))
        uam_netdev_input(&l->nd, m->data, m->len);   /* counts what is too long as dropped */
}

static void nd_event(void *ctx, uam_link_event ev)
{
    link_nd *l = ctx;
    if (ev == UAM_LINK_EV_REWOUND) {     /* the far end did not go back: as a cable pulled */
        uam_netdev_set_link(&l->nd, false);
        uam_netdev_set_link(&l->nd, true);
    } else {
        uam_netdev_set_link(&l->nd, ev == UAM_LINK_EV_UP);
    }
}

static bool nd_send(uam_netdev *nd, const void *frame, size_t len)
{
    link_nd *l = UAM_CONTAINER_OF(nd, link_nd, nd);
    return uam_link_send(l->port, 0, 0, UAM_LINK_DATA, frame, len);
}

static void nd_poll(uam_netdev *nd)
{
    link_nd *l = UAM_CONTAINER_OF(nd, link_nd, nd);
    if (!port_sched(l->port))
        uam_link_accept_input(l->port);
}

static void nd_destroy(uam_netdev *nd)
{
    link_nd *l = UAM_CONTAINER_OF(nd, link_nd, nd);
    uam_link_port_set_frontend(l->port, NULL, NULL, NULL, NULL);
    uam_link_port_set_state(l->port, NULL, NULL, NULL);
    free(l);
}

static void nd_drop_queue(uam_netdev *nd)
{
    while (nd->q_len) {
        free(nd->q[nd->q_head].data);
        nd->q_head = (nd->q_head + 1) % UAM_NETDEV_QUEUE;
        nd->q_len--;
    }
    nd->q_head = 0;
}

static void nd_save(void *ctx, uam_state_writer *w)
{
    link_nd *l = ctx;
    uam_state_put_bool(w, l->nd.link_up);
    uam_state_put_u32(w, l->nd.q_len);
    for (unsigned i = 0; i < l->nd.q_len; i++) {
        const uam_netdev_frame *f = &l->nd.q[(l->nd.q_head + i) % UAM_NETDEV_QUEUE];
        uam_state_put_u16(w, f->len);
        if (f->len)
            uam_state_put_bytes(w, f->data, f->len);
    }
}

static void nd_restore(void *ctx, uam_state_reader *r)
{
    link_nd *l = ctx;
    nd_drop_queue(&l->nd);
    bool up = uam_state_get_bool(r);
    uint32_t n = uam_state_get_u32(r);
    if (n > UAM_NETDEV_QUEUE) {
        uam_state_fail(r, "link nic: saved frames do not fit");
        return;
    }
    for (uint32_t i = 0; i < n && uam_state_ok(r); i++) {
        uint16_t len = uam_state_get_u16(r);
        uint8_t *d = malloc(len ? len : 1);
        if (!d) {
            uam_state_fail(r, "link nic: out of memory");
            return;
        }
        if (len)
            uam_state_get_bytes(r, d, len);
        l->nd.q[l->nd.q_len++] = (uam_netdev_frame){ len, d };
    }
    /* The device restored its own view of the line; a hub that is there now
     * keeps it up, whatever was saved. */
    l->nd.link_up = uam_link_port_up(l->port) ? true : up;
}

static const uam_netdev_ops link_nd_ops = { .type = "link", .send = nd_send, .poll = nd_poll,
                                            .destroy = nd_destroy };

uam_netdev *uam_netdev_link_create(uam_link_port *port)
{
    link_nd *l = port ? calloc(1, sizeof(*l)) : NULL;
    if (!l)
        return NULL;
    uam_netdev_init(&l->nd, &link_nd_ops);
    l->port = port;
    l->nd.link_up = uam_link_port_up(port);
    uam_link_port_set_state(port, nd_save, nd_restore, l);
    uam_link_port_set_frontend(port, nd_can_receive, nd_receive, nd_event, l);
    return &l->nd;
}
