/*
 * netdev.c - Network device plumbing shared by all backends (uam/netdev.h).
 *
 * Queues host -> guest frames until the frontend can take them. No host I/O.
 */
#include <stdlib.h>
#include <string.h>

#include "uam/netdev.h"

void uam_netdev_init(uam_netdev *nd, const uam_netdev_ops *ops)
{
    memset(nd, 0, sizeof(*nd));
    nd->ops = ops;
    nd->link_up = true;
}

static void deliver(uam_netdev *nd)
{
    while (!nd->in_send && nd->q_len && nd->receive && (!nd->can_receive || nd->can_receive(nd->fe_ctx))) {
        uam_netdev_frame f = nd->q[nd->q_head];
        nd->q_head = (nd->q_head + 1) % UAM_NETDEV_QUEUE;
        nd->q_len--;
        nd->receive(nd->fe_ctx, f.data, f.len);
        free(f.data);
    }
}

bool uam_netdev_send(uam_netdev *nd, const void *frame, size_t len)
{
    if (!nd || len > UAM_NETDEV_MTU)
        return false;
    if (nd->tap)
        nd->tap(nd->tap_ctx, frame, len, true);
    nd->in_send = true;
    bool ok = nd->link_up && nd->ops->send && nd->ops->send(nd, frame, len);
    nd->in_send = false;
    if (!ok)
        nd->tx_dropped++;
    deliver(nd);   /* anything the backend answered meanwhile */
    return ok;
}

void uam_netdev_set_frontend(uam_netdev *nd, bool (*can_receive)(void *ctx),
                             void (*receive)(void *ctx, const uint8_t *frame, size_t len),
                             void (*link_changed)(void *ctx, bool up), void *ctx)
{
    if (!nd)
        return;
    nd->can_receive = can_receive;
    nd->receive = receive;
    nd->link_changed = link_changed;
    nd->fe_ctx = ctx;
    if (link_changed)
        link_changed(ctx, nd->link_up);
    deliver(nd);
}

void uam_netdev_accept_input(uam_netdev *nd)
{
    if (nd)
        deliver(nd);
}

void uam_netdev_input(uam_netdev *nd, const void *frame, size_t len)
{
    if (len > UAM_NETDEV_MTU || !nd->link_up) {
        nd->rx_dropped++;
        return;
    }
    if (nd->tap)
        nd->tap(nd->tap_ctx, frame, len, false);
    if (nd->receive && !nd->q_len && !nd->in_send
        && (!nd->can_receive || nd->can_receive(nd->fe_ctx))) {
        nd->receive(nd->fe_ctx, frame, len);
        return;
    }
    uint8_t *copy = malloc(len ? len : 1);
    if (!copy || nd->q_len == UAM_NETDEV_QUEUE) {
        free(copy);
        nd->rx_dropped++;
        return;
    }
    memcpy(copy, frame, len);
    nd->q[(nd->q_head + nd->q_len) % UAM_NETDEV_QUEUE] = (uam_netdev_frame){ (uint16_t)len, copy };
    nd->q_len++;
    deliver(nd);
}

bool uam_netdev_can_input(const uam_netdev *nd)
{
    return nd->q_len < UAM_NETDEV_QUEUE;
}

void uam_netdev_set_link(uam_netdev *nd, bool up)
{
    if (nd->link_up == up)
        return;
    nd->link_up = up;
    if (nd->link_changed)
        nd->link_changed(nd->fe_ctx, up);
}

int uam_netdev_fd(uam_netdev *nd)
{
    return nd && nd->ops->fd ? nd->ops->fd(nd) : -1;
}

void uam_netdev_poll(uam_netdev *nd)
{
    if (!nd)
        return;
    if (nd->ops->poll)
        nd->ops->poll(nd);
    deliver(nd);
}

void uam_netdev_set_tap(uam_netdev *nd,
                        void (*fn)(void *ctx, const uint8_t *frame, size_t len, bool to_host),
                        void (*release)(void *ctx), void *ctx)
{
    if (nd->tap_release)
        nd->tap_release(nd->tap_ctx);
    nd->tap = fn;
    nd->tap_release = release;
    nd->tap_ctx = ctx;
}

void uam_netdev_destroy(uam_netdev *nd)
{
    if (!nd)
        return;
    while (nd->q_len) {
        free(nd->q[nd->q_head].data);
        nd->q_head = (nd->q_head + 1) % UAM_NETDEV_QUEUE;
        nd->q_len--;
    }
    if (nd->tap_release)
        nd->tap_release(nd->tap_ctx);
    nd->tap = NULL;
    nd->tap_release = NULL;
    if (nd->ops->destroy)
        nd->ops->destroy(nd);
}
