/*
 * uam/netdev.h - Network device interface: Ethernet frames between a guest
 * NIC/switch model (the frontend) and the host (the backend: a tap interface,
 * a UDP socket pair standing in for a cable or the radio "air", an
 * in-process pair; see uam/host.h). Capture (pcap) attaches as a tap.
 *
 * Devices depend only on this header:
 *
 *     uam_netdev_set_frontend(nd, can_receive, receive, link_changed, mac);
 *     uam_netdev_send(nd, frame, len);           guest transmits
 *     uam_netdev_accept_input(nd);               the guest freed RX descriptors
 *
 * Frames the frontend cannot take yet are queued (up to UAM_NETDEV_QUEUE,
 * then dropped, as a real link would) and delivered on accept_input.
 *
 * Dependencies: common.h.
 */
#ifndef UAM_NETDEV_H
#define UAM_NETDEV_H

#include "uam/common.h"

#define UAM_NETDEV_MTU   9216u   /* largest frame carried (jumbo-safe) */
#define UAM_NETDEV_QUEUE 64u

typedef struct uam_netdev uam_netdev;

typedef struct uam_netdev_ops {
    const char *type;
    /* Guest -> host. Returns false if the frame was dropped. */
    bool (*send)(uam_netdev *nd, const void *frame, size_t len);
    int  (*fd)(uam_netdev *nd);            /* -1 if none */
    void (*poll)(uam_netdev *nd);          /* read frames, pass to uam_netdev_input() */
    void (*destroy)(uam_netdev *nd);
} uam_netdev_ops;

typedef struct uam_netdev_frame {
    uint16_t len;
    uint8_t *data;
} uam_netdev_frame;

struct uam_netdev {
    const uam_netdev_ops *ops;
    bool                  link_up;
    /* frontend */
    bool  (*can_receive)(void *ctx);
    void  (*receive)(void *ctx, const uint8_t *frame, size_t len);
    void  (*link_changed)(void *ctx, bool up);
    void   *fe_ctx;
    /* queued host -> guest frames */
    uam_netdev_frame q[UAM_NETDEV_QUEUE];
    unsigned         q_head, q_len;
    uint64_t         rx_dropped, tx_dropped;
    /* optional observer of every frame in both directions (e.g. pcap capture) */
    void           (*tap)(void *ctx, const uint8_t *frame, size_t len, bool to_host);
    void           (*tap_release)(void *ctx);   /* called on destroy / replacement */
    void            *tap_ctx;
};

void uam_netdev_init(uam_netdev *nd, const uam_netdev_ops *ops);

/* ---- frontend (device) side ---- */
bool uam_netdev_send(uam_netdev *nd, const void *frame, size_t len);
void uam_netdev_set_frontend(uam_netdev *nd, bool (*can_receive)(void *ctx),
                             void (*receive)(void *ctx, const uint8_t *frame, size_t len),
                             void (*link_changed)(void *ctx, bool up), void *ctx);
void uam_netdev_accept_input(uam_netdev *nd);

/* ---- backend side ---- */
void uam_netdev_input(uam_netdev *nd, const void *frame, size_t len);
void uam_netdev_set_link(uam_netdev *nd, bool up);

/* ---- owner side ---- */
int  uam_netdev_fd(uam_netdev *nd);
void uam_netdev_poll(uam_netdev *nd);
void uam_netdev_set_tap(uam_netdev *nd,
                        void (*fn)(void *ctx, const uint8_t *frame, size_t len, bool to_host),
                        void (*release)(void *ctx), void *ctx);
void uam_netdev_destroy(uam_netdev *nd);

#endif /* UAM_NETDEV_H */
