/*
 * uam/chardev.h - Character device interface: a byte stream between a guest
 * device (the frontend: a UART, a debug console) and the host (the backend:
 * stdio, a file, a pty, a socket, a fixed input string).
 *
 * Devices depend only on this header. The frontend writes guest output with
 * uam_chardev_write() and registers handlers for input:
 *
 *     uam_chardev_set_frontend(cd, my_can_receive, my_receive, dev);
 *     ...when the guest drains the RX FIFO:  uam_chardev_accept_input(cd);
 *
 * Input is never dropped: the chardev buffers what the frontend cannot take
 * yet and delivers it when the frontend calls uam_chardev_accept_input().
 *
 * Backends are created in src/host (uam/host.h). Input from a host file
 * descriptor arrives when the host loop polls it; a backend created with
 * fixed input (uam_chardev_buffer_create) delivers it as soon as a frontend
 * is attached, which keeps runs deterministic.
 *
 * Dependencies: common.h.
 */
#ifndef UAM_CHARDEV_H
#define UAM_CHARDEV_H

#include "uam/common.h"

#define UAM_CHARDEV_RXBUF 4096u

typedef struct uam_chardev uam_chardev;

typedef struct uam_chardev_ops {
    const char *type;
    /* Guest -> host. Returns bytes accepted (backends never block the guest). */
    size_t (*write)(uam_chardev *cd, const void *buf, size_t len);
    /* Host file descriptor to wait on for input, or -1. */
    int    (*fd)(uam_chardev *cd);
    /* The fd is readable (or a poll timed out): read without blocking and
     * pass bytes to uam_chardev_input(). */
    void   (*poll)(uam_chardev *cd);
    void   (*destroy)(uam_chardev *cd);
} uam_chardev_ops;

struct uam_chardev {
    const uam_chardev_ops *ops;
    /* frontend */
    size_t (*can_receive)(void *ctx);
    void   (*receive)(void *ctx, const uint8_t *buf, size_t len);
    void    *fe_ctx;
    /* host input not yet taken by the frontend */
    uint8_t  rx[UAM_CHARDEV_RXBUF];
    size_t   rx_head, rx_len;
    /* the backend asks to end the session (escape key, EOF on a script) */
    void   (*on_quit)(void *ctx);
    void    *quit_ctx;
};

void   uam_chardev_init(uam_chardev *cd, const uam_chardev_ops *ops);

/* ---- frontend (device) side ---- */
size_t uam_chardev_write(uam_chardev *cd, const void *buf, size_t len);
void   uam_chardev_set_frontend(uam_chardev *cd, size_t (*can_receive)(void *ctx),
                                void (*receive)(void *ctx, const uint8_t *buf, size_t len),
                                void *ctx);
void   uam_chardev_accept_input(uam_chardev *cd);

/* ---- backend side ---- */
/* Queue host input and deliver what the frontend accepts. Returns bytes
 * queued (less than len only if the buffer is full). */
size_t uam_chardev_input(uam_chardev *cd, const void *buf, size_t len);
void   uam_chardev_quit(uam_chardev *cd);

/* ---- owner side ---- */
void   uam_chardev_set_quit_handler(uam_chardev *cd, void (*fn)(void *ctx), void *ctx);
int    uam_chardev_fd(uam_chardev *cd);
void   uam_chardev_poll(uam_chardev *cd);
void   uam_chardev_destroy(uam_chardev *cd);

#endif /* UAM_CHARDEV_H */
