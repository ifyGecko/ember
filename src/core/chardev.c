/*
 * chardev.c - Character device plumbing shared by all backends (uam/chardev.h).
 *
 * Buffers host input until the frontend can take it. No host I/O here.
 */
#include <string.h>

#include "uam/chardev.h"

void uam_chardev_init(uam_chardev *cd, const uam_chardev_ops *ops)
{
    memset(cd, 0, sizeof(*cd));
    cd->ops = ops;
}

size_t uam_chardev_write(uam_chardev *cd, const void *buf, size_t len)
{
    if (!cd)
        return len; /* no backend: output is discarded */
    return cd->ops->write ? cd->ops->write(cd, buf, len) : len;
}

/* Hand queued input to the frontend, as much as it accepts. */
static void deliver(uam_chardev *cd)
{
    if (!cd->receive)
        return;
    while (cd->rx_len) {
        size_t room = cd->can_receive ? cd->can_receive(cd->fe_ctx) : cd->rx_len;
        if (!room)
            return;
        size_t chunk = UAM_CHARDEV_RXBUF - cd->rx_head;   /* contiguous run */
        if (chunk > cd->rx_len)
            chunk = cd->rx_len;
        if (chunk > room)
            chunk = room;
        const uint8_t *p = cd->rx + cd->rx_head;
        cd->rx_head = (cd->rx_head + chunk) % UAM_CHARDEV_RXBUF;
        cd->rx_len -= chunk;
        cd->receive(cd->fe_ctx, p, chunk);
    }
}

void uam_chardev_set_frontend(uam_chardev *cd, size_t (*can_receive)(void *ctx),
                              void (*receive)(void *ctx, const uint8_t *buf, size_t len),
                              void *ctx)
{
    if (!cd)
        return;
    cd->can_receive = can_receive;
    cd->receive = receive;
    cd->fe_ctx = ctx;
    deliver(cd);
}

void uam_chardev_accept_input(uam_chardev *cd)
{
    if (!cd)
        return;
    /* Backends without a host fd (fixed input) refill right away. */
    if (!cd->ops->fd && cd->ops->poll && cd->rx_len < UAM_CHARDEV_RXBUF)
        cd->ops->poll(cd);
    deliver(cd);
}

size_t uam_chardev_input(uam_chardev *cd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    size_t n = 0;
    while (n < len && cd->rx_len < UAM_CHARDEV_RXBUF) {
        cd->rx[(cd->rx_head + cd->rx_len) % UAM_CHARDEV_RXBUF] = p[n++];
        cd->rx_len++;
    }
    deliver(cd);
    return n;
}

void uam_chardev_quit(uam_chardev *cd)
{
    if (cd->on_quit)
        cd->on_quit(cd->quit_ctx);
}

void uam_chardev_set_quit_handler(uam_chardev *cd, void (*fn)(void *ctx), void *ctx)
{
    cd->on_quit = fn;
    cd->quit_ctx = ctx;
}

int uam_chardev_fd(uam_chardev *cd)
{
    return cd && cd->ops->fd ? cd->ops->fd(cd) : -1;
}

void uam_chardev_poll(uam_chardev *cd)
{
    if (!cd)
        return;
    /* Only read more from the host when there is room to keep it. */
    if (cd->ops->poll && cd->rx_len < UAM_CHARDEV_RXBUF)
        cd->ops->poll(cd);
    deliver(cd);
}

void uam_chardev_destroy(uam_chardev *cd)
{
    if (cd && cd->ops->destroy)
        cd->ops->destroy(cd);
}
