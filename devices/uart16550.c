/*
 * uart16550.c - NS16550A-compatible UART (uam/dev/uart16550.h).
 *
 * Host side: a uam_chardev (transmit is instantaneous; received bytes are
 * buffered in a 4 KiB FIFO and the chardev holds back what does not fit).
 *
 * Registers (DLAB = LCR bit 7):
 *   0 RBR (r) / THR (w) / DLL (DLAB)
 *   1 IER / DLM (DLAB)
 *   2 IIR (r) / FCR (w)
 *   3 LCR   4 MCR   5 LSR   6 MSR   7 SCR
 */
#include <stdlib.h>

#include "uam/dev/uart16550.h"

#define RX_CAP 4096u

#define IER_RDI   0x01u  /* receive data available */
#define IER_THRI  0x02u  /* transmitter holding register empty */
#define LCR_DLAB  0x80u
#define LSR_DR    0x01u
#define LSR_THRE  0x20u
#define LSR_TEMT  0x40u
#define IIR_NONE  0x01u
#define IIR_THRI  0x02u
#define IIR_RDI   0x04u
#define IIR_FIFO  0xC0u
#define FCR_FIFO  0x01u
#define FCR_CLRRX 0x02u

typedef struct uart {
    uam_device dev;
    uam_chardev *cd;
    uam_signal irq;
    uint8_t    ier, lcr, mcr, scr, fcr, dll, dlm;
    bool       thr_pending;   /* THR-empty interrupt condition latched */
    uint8_t    rx[RX_CAP];
    unsigned   rx_head, rx_count;
} uart;

static uart *to_uart(uam_device *d) { return UAM_CONTAINER_OF(d, uart, dev); }

static void update_irq(uart *u)
{
    bool rdi = (u->ier & IER_RDI) && u->rx_count;
    bool thri = (u->ier & IER_THRI) && u->thr_pending;
    uam_signal_set(&u->irq, rdi || thri);
}

static uint8_t iir(uart *u)
{
    uint8_t fifo = (u->fcr & FCR_FIFO) ? IIR_FIFO : 0;
    if ((u->ier & IER_RDI) && u->rx_count)
        return fifo | IIR_RDI;
    if ((u->ier & IER_THRI) && u->thr_pending)
        return fifo | IIR_THRI;
    return fifo | IIR_NONE;
}

static uam_status uart_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    uart *u = to_uart(d);
    (void)size;
    switch (off) {
    case 0:
        if (u->lcr & LCR_DLAB) {
            *val = u->dll;
        } else if (u->rx_count) {
            *val = u->rx[u->rx_head];
            u->rx_head = (u->rx_head + 1) % RX_CAP;
            u->rx_count--;
            uam_chardev_accept_input(u->cd); /* room for more host input */
        } else {
            *val = 0;
        }
        break;
    case 1: *val = (u->lcr & LCR_DLAB) ? u->dlm : u->ier; break;
    case 2: {
        uint8_t v = iir(u);
        if ((v & 0x0F) == IIR_THRI)
            u->thr_pending = false; /* reading IIR acknowledges THRE */
        *val = v;
        break;
    }
    case 3: *val = u->lcr; break;
    case 4: *val = u->mcr; break;
    case 5: *val = LSR_THRE | LSR_TEMT | (u->rx_count ? LSR_DR : 0); break;
    case 6: *val = 0xB0; break; /* CTS, DSR, DCD asserted */
    case 7: *val = u->scr; break;
    default: *val = 0; break;
    }
    update_irq(u);
    return UAM_OK;
}

static uam_status uart_write(uam_device *d, uint64_t off, unsigned size, uint64_t v)
{
    uart *u = to_uart(d);
    uint8_t b = (uint8_t)v;
    (void)size;
    switch (off) {
    case 0:
        if (u->lcr & LCR_DLAB) {
            u->dll = b;
        } else {
            uam_chardev_write(u->cd, &b, 1);
            u->thr_pending = true; /* transmission completes instantly */
        }
        break;
    case 1:
        if (u->lcr & LCR_DLAB) {
            u->dlm = b;
        } else {
            uint8_t enabled = (uint8_t)(b & ~u->ier);
            u->ier = b & 0x0F;
            if (enabled & IER_THRI)
                u->thr_pending = true; /* THR is always empty */
        }
        break;
    case 2:
        u->fcr = b;
        if (b & FCR_CLRRX) {
            u->rx_count = 0;
            uam_chardev_accept_input(u->cd);
        }
        break;
    case 3: u->lcr = b; break;
    case 4: u->mcr = b; break;
    case 7: u->scr = b; break;
    default: break;
    }
    update_irq(u);
    return UAM_OK;
}

static void uart_reset(uam_device *d)
{
    uart *u = to_uart(d);
    u->ier = u->lcr = u->mcr = u->scr = u->fcr = u->dll = u->dlm = 0;
    u->thr_pending = false;
    update_irq(u);
}

static void uart_destroy(uam_device *d)
{
    uart *u = to_uart(d);
    uam_chardev_set_frontend(u->cd, NULL, NULL, NULL);
    free(u);
}

static void uart_save(uam_device *d, uam_state_writer *w)
{
    uart *u = to_uart(d);
    const uint8_t regs[] = { u->ier, u->lcr, u->mcr, u->scr, u->fcr, u->dll, u->dlm };
    uam_state_put_bytes(w, regs, sizeof(regs));
    uam_state_put_bool(w, u->thr_pending);
    /* The receive FIFO, oldest byte first. */
    uint8_t fifo[RX_CAP];
    for (unsigned i = 0; i < u->rx_count; i++)
        fifo[i] = u->rx[(u->rx_head + i) % RX_CAP];
    uam_state_put_u32(w, u->rx_count);
    uam_state_put_bytes(w, fifo, u->rx_count);
    uam_state_put_signal(w, &u->irq);
}

static void uart_restore(uam_device *d, uam_state_reader *r)
{
    uart *u = to_uart(d);
    uint8_t regs[7];
    uam_state_get_bytes(r, regs, sizeof(regs));
    u->ier = regs[0]; u->lcr = regs[1]; u->mcr = regs[2]; u->scr = regs[3];
    u->fcr = regs[4]; u->dll = regs[5]; u->dlm = regs[6];
    u->thr_pending = uam_state_get_bool(r);
    uint32_t n = uam_state_get_u32(r);
    if (n > RX_CAP)
        uam_state_fail(r, "receive FIFO holds %u bytes, more than its %u", (unsigned)n, RX_CAP);
    if (!uam_state_ok(r))
        return;
    uam_state_get_bytes(r, u->rx, n);
    u->rx_head = 0;
    u->rx_count = n;
    uam_state_get_signal(r, &u->irq);
    uam_chardev_accept_input(u->cd);   /* the FIFO's free space changed */
}

static const uam_device_ops uart_ops = {
    .type = "16550a-uart",
    .read = uart_read,
    .write = uart_write,
    .reset = uart_reset,
    .destroy = uart_destroy,
    .save = uart_save,
    .restore = uart_restore,
};

static size_t uart_can_receive(void *ctx)
{
    uart *u = ctx;
    return RX_CAP - u->rx_count;
}

static void uart_receive(void *ctx, const uint8_t *buf, size_t len)
{
    uam_uart16550_feed(&((uart *)ctx)->dev, buf, len);
}

uam_device *uam_uart16550_create(const char *name, uam_chardev *cd)
{
    uart *u = calloc(1, sizeof(*u));
    if (!u)
        return NULL;
    uam_device_init(&u->dev, &uart_ops, name);
    u->cd = cd;
    uam_signal_init(&u->irq);
    uam_chardev_set_frontend(cd, uart_can_receive, uart_receive, u);
    return &u->dev;
}

uam_signal *uam_uart16550_irq_line(uam_device *d) { return &to_uart(d)->irq; }

size_t uam_uart16550_feed(uam_device *d, const void *data, size_t len)
{
    uart *u = to_uart(d);
    const uint8_t *p = data;
    size_t n = 0;
    while (n < len && u->rx_count < RX_CAP) {
        u->rx[(u->rx_head + u->rx_count) % RX_CAP] = p[n++];
        u->rx_count++;
    }
    update_irq(u);
    return n;
}
