/*
 * uam/dev/uart16550.h - NS16550A-compatible UART (byte-wide registers).
 *
 * Transmit is instantaneous: bytes written to THR go straight to the chardev
 * backend. Receive bytes come from the chardev (console input) or from
 * uam_uart16550_feed() (a fuzzer). Supports the RX-data-available and
 * THR-empty interrupts.
 */
#ifndef UAM_DEV_UART16550_H
#define UAM_DEV_UART16550_H

#include "uam/chardev.h"
#include "uam/device.h"
#include "uam/signal.h"

#define UAM_UART16550_SIZE 0x100u

/* cd may be NULL: output is discarded and only feed() supplies input. */
uam_device *uam_uart16550_create(const char *name, uam_chardev *cd);
uam_signal *uam_uart16550_irq_line(uam_device *dev);
/* Queue bytes for the guest to receive. Returns how many were accepted. */
size_t      uam_uart16550_feed(uam_device *dev, const void *data, size_t len);

#endif /* UAM_DEV_UART16550_H */
