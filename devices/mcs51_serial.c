/*
 * mcs51_serial.c - The 8051's serial port (uam/dev/mcs51_serial.h).
 */
#include <stdlib.h>

#include "uam/dev/mcs51_serial.h"

enum { REN = 0x10, RB8 = 0x04, TI = 0x02, RI = 0x01 };
#define RX_CAP 256u

typedef struct m51serial {
    uam_device   dev;
    uam_chardev *cd;
    uam_signal   irq;
    uint8_t      scon, rbuf;
    uint8_t      rx[RX_CAP];
    unsigned     rx_head, rx_count;
} m51serial;

static m51serial *to_s(uam_device *d) { return UAM_CONTAINER_OF(d, m51serial, dev); }

/* Move the next received byte into SBUF if the receiver may take it. `host`: called
 * from the chardev's delivery, which must not be re-entered. */
static void deliver(m51serial *s, bool host)
{
    if ((s->scon & REN) && !(s->scon & RI) && s->rx_count) {
        s->rbuf = s->rx[s->rx_head];
        s->rx_head = (s->rx_head + 1) % RX_CAP;
        s->rx_count--;
        s->scon |= RI | RB8;
        if (!host)
            uam_chardev_accept_input(s->cd);   /* room for more host input */
    }
    uam_signal_set(&s->irq, (s->scon & (RI | TI)) != 0);
}

static uam_status ser_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    (void)size;
    m51serial *s = to_s(d);
    *val = off == 0 ? s->scon : off == 1 ? s->rbuf : 0;
    return UAM_OK;
}

static uam_status ser_write(uam_device *d, uint64_t off, unsigned size, uint64_t val)
{
    (void)size;
    m51serial *s = to_s(d);
    const uint8_t b = (uint8_t)val;
    if (off == 0) {
        s->scon = b;
    } else if (off == 1) {
        uam_chardev_write(s->cd, &b, 1);
        s->scon |= TI;   /* transmission completes instantly */
    }
    deliver(s, false);
    return UAM_OK;
}

static void ser_reset(uam_device *d)
{
    m51serial *s = to_s(d);
    s->scon = s->rbuf = 0;
    uam_signal_set(&s->irq, 0);
}

static void ser_destroy(uam_device *d)
{
    m51serial *s = to_s(d);
    uam_chardev_set_frontend(s->cd, NULL, NULL, NULL);
    free(s);
}

static void ser_save(uam_device *d, uam_state_writer *w)
{
    m51serial *s = to_s(d);
    uam_state_put_u8(w, s->scon);
    uam_state_put_u8(w, s->rbuf);
    uint8_t fifo[RX_CAP];
    for (unsigned i = 0; i < s->rx_count; i++)
        fifo[i] = s->rx[(s->rx_head + i) % RX_CAP];
    uam_state_put_u32(w, s->rx_count);
    uam_state_put_bytes(w, fifo, s->rx_count);
    uam_state_put_signal(w, &s->irq);
}

static void ser_restore(uam_device *d, uam_state_reader *r)
{
    m51serial *s = to_s(d);
    s->scon = uam_state_get_u8(r);
    s->rbuf = uam_state_get_u8(r);
    const uint32_t n = uam_state_get_u32(r);
    if (n > RX_CAP)
        uam_state_fail(r, "receive FIFO holds %u bytes, more than its %u", (unsigned)n, RX_CAP);
    if (!uam_state_ok(r))
        return;
    uam_state_get_bytes(r, s->rx, n);
    s->rx_head = 0;
    s->rx_count = n;
    uam_state_get_signal(r, &s->irq);
    uam_chardev_accept_input(s->cd);
}

static const uam_device_ops ser_ops = {
    .type = "uam,mcs51-serial", .impl_min = 1, .impl_max = 1,
    .read = ser_read, .peek = ser_read, .write = ser_write, .reset = ser_reset,
    .destroy = ser_destroy, .save = ser_save, .restore = ser_restore,
};

static size_t ser_can_receive(void *ctx)
{
    m51serial *s = ctx;
    return RX_CAP - s->rx_count;
}

static void ser_receive(void *ctx, const uint8_t *buf, size_t len)
{
    uam_mcs51_serial_feed(&((m51serial *)ctx)->dev, buf, len);
}

uam_device *uam_mcs51_serial_create(const char *name, uam_chardev *cd)
{
    m51serial *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    uam_device_init(&s->dev, &ser_ops, name);
    s->cd = cd;
    uam_signal_init(&s->irq);
    uam_chardev_set_frontend(cd, ser_can_receive, ser_receive, s);
    return &s->dev;
}

uam_signal *uam_mcs51_serial_irq(uam_device *d) { return &to_s(d)->irq; }

size_t uam_mcs51_serial_feed(uam_device *d, const void *data, size_t len)
{
    m51serial *s = to_s(d);
    const uint8_t *p = data;
    size_t n = 0;
    for (; n < len && s->rx_count < RX_CAP; n++)
        s->rx[(s->rx_head + s->rx_count++) % RX_CAP] = p[n];
    deliver(s, true);
    return n;
}
