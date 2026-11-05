/*
 * soc_uart.c - <SoC> UART @ phys 0x........ Semantics from the firmware driver:
 *   putc 0x........: spin until STATUS & TX_READY, then DATA = ch (sb to the low byte)
 *   getc 0x........: if STATUS & RX_VALID { ch = DATA }   (reading DATA pops the byte)
 *   isr  0x........: INT = INT (W1C ack), then getc until !RX_VALID
 * Local emulation model; not part of ember.
 */
#include <stdlib.h>

#include "soc_uart.h"

#define R_DATA   0x00u
#define R_STATUS 0x04u
#define  ST_RX_VALID  (1u << 0)
#define  ST_TX_READY  (1u << 5)
#define R_INT    0x08u          /* W1C */
#define  INT_RX       (1u << 0)
#define R_INT_EN 0x0cu

#define RX_CAP 64u

typedef struct soc_uart {
    uam_device   dev;          /* first member: the bus hands us &dev */
    uam_chardev *cd;
    uam_signal   irq;
    uint32_t     int_stat, int_en;
    uint8_t      rx[RX_CAP];
    unsigned     rx_head, rx_count;
} soc_uart;

static soc_uart *to_uart(uam_device *d) { return UAM_CONTAINER_OF(d, soc_uart, dev); }

static void update_irq(soc_uart *s)
{
    if (s->rx_count)
        s->int_stat |= INT_RX;          /* level: re-asserts while data is pending */
    uam_signal_set(&s->irq, (s->int_stat & s->int_en) != 0);
}

static uam_status uart_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    soc_uart *s = to_uart(d);
    (void)size;
    switch (off) {
    case R_DATA:
        *val = 0;
        if (s->rx_count) {
            *val = s->rx[s->rx_head];
            s->rx_head = (s->rx_head + 1) % RX_CAP;
            s->rx_count--;
            uam_chardev_accept_input(s->cd);   /* room again: the chardev delivers more */
        }
        break;
    case R_STATUS: *val = ST_TX_READY | (s->rx_count ? ST_RX_VALID : 0); break; /* TX never busy */
    case R_INT:    *val = s->int_stat; break;
    case R_INT_EN: *val = s->int_en; break;
    default:
        uam_log(UAM_LOG_WARN, "soc-uart", "%s: read of unmodelled +0x%llx", d->name,
                (unsigned long long)off);
        *val = 0;
        break;
    }
    update_irq(s);
    return UAM_OK;
}

static uam_status uart_write(uam_device *d, uint64_t off, unsigned size, uint64_t v)
{
    soc_uart *s = to_uart(d);
    uint8_t ch = (uint8_t)v;
    (void)size;
    switch (off) {
    case R_DATA:   uam_chardev_write(s->cd, &ch, 1); break;   /* output appears */
    case R_INT:    s->int_stat &= ~(uint32_t)v; break;
    case R_INT_EN: s->int_en = (uint32_t)v; break;
    default:
        uam_log(UAM_LOG_WARN, "soc-uart", "%s: write of unmodelled +0x%llx = 0x%llx", d->name,
                (unsigned long long)off, (unsigned long long)v);
        break;
    }
    update_irq(s);
    return UAM_OK;
}

/* Byte stores (sb) to a 32-bit register: without this hook the bus would read DATA to
 * merge the other bytes - and a DATA read pops the RX FIFO. */
static uam_status uart_write_masked(uam_device *d, uint64_t off, unsigned size, uint64_t v,
                                    uint64_t mask)
{
    return uart_write(d, off, size, v & mask);
}

static void uart_reset(uam_device *d)
{
    soc_uart *s = to_uart(d);
    s->int_stat = s->int_en = 0;
    s->rx_head = s->rx_count = 0;
    uam_chardev_accept_input(s->cd);
    update_irq(s);
}

static void uart_destroy(uam_device *d)
{
    soc_uart *s = to_uart(d);
    uam_chardev_set_frontend(s->cd, NULL, NULL, NULL);
    free(s);
}

/* Snapshots: everything the guest can observe - registers, the RX FIFO (oldest byte
 * first), the line level. Not the chardev: wiring and host side are rebuilt by the board. */
static void uart_save(uam_device *d, uam_state_writer *w)
{
    soc_uart *s = to_uart(d);
    uint8_t fifo[RX_CAP];
    for (unsigned i = 0; i < s->rx_count; i++)
        fifo[i] = s->rx[(s->rx_head + i) % RX_CAP];
    uam_state_put_u32(w, s->int_stat);
    uam_state_put_u32(w, s->int_en);
    uam_state_put_u32(w, s->rx_count);
    uam_state_put_bytes(w, fifo, s->rx_count);
    uam_state_put_signal(w, &s->irq);
}

static void uart_restore(uam_device *d, uam_state_reader *r)
{
    soc_uart *s = to_uart(d);
    s->int_stat = uam_state_get_u32(r);
    s->int_en = uam_state_get_u32(r);
    uint32_t n = uam_state_get_u32(r);
    if (n > RX_CAP)
        uam_state_fail(r, "RX FIFO holds %u bytes, more than its %u", (unsigned)n, RX_CAP);
    if (!uam_state_ok(r))
        return;
    uam_state_get_bytes(r, s->rx, n);
    s->rx_head = 0;
    s->rx_count = n;
    uam_state_get_signal(r, &s->irq);   /* the level only; the sink restores its own view */
    uam_chardev_accept_input(s->cd);    /* the FIFO's free space changed */
}

static const uam_device_ops soc_uart_ops = {
    .type = "soc-uart",
    .endian = UAM_ACC_BE,         /* the SoC's register byte order (big-endian MIPS SoC) */
    .impl_min = 4, .impl_max = 4, /* 32-bit registers: the bus narrows/splits other widths */
    .read = uart_read,
    .write = uart_write,
    .write_masked = uart_write_masked,
    .reset = uart_reset,
    .destroy = uart_destroy,
    .save = uart_save,            /* snapshots (uam/state.h); or .flags = UAM_DEVICE_STATELESS */
    .restore = uart_restore,
};

static size_t uart_can_receive(void *ctx)
{
    soc_uart *s = ctx;
    return RX_CAP - s->rx_count;
}

static void uart_receive(void *ctx, const uint8_t *buf, size_t len)
{
    soc_uart *s = ctx;
    for (size_t i = 0; i < len && s->rx_count < RX_CAP; i++)
        s->rx[(s->rx_head + s->rx_count++) % RX_CAP] = buf[i];
    update_irq(s);
}

uam_device *soc_uart_create(const char *name, uam_chardev *cd)
{
    soc_uart *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    uam_device_init(&s->dev, &soc_uart_ops, name);
    s->cd = cd;
    uam_signal_init(&s->irq);
    uam_chardev_set_frontend(cd, uart_can_receive, uart_receive, s);
    return &s->dev;
}

uam_signal *soc_uart_irq_line(uam_device *d) { return &to_uart(d)->irq; }

/* Queue bytes as if received on the line: what does not fit the FIFO is dropped, as on a
 * real line. Not needed for bring-up: the ember-fuzz skill publishes it as a fuzz input. */
size_t soc_uart_feed(uam_device *d, const uint8_t *data, size_t len)
{
    soc_uart *s = to_uart(d);
    size_t n = len < RX_CAP - s->rx_count ? len : RX_CAP - s->rx_count;
    uart_receive(s, data, n);
    return n;
}
