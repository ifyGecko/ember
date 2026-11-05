/*
 * soc_uart.h - <SoC> UART: 32-bit big-endian registers, TX instantaneous, RX via the
 * console chardev, one level-triggered interrupt line.
 */
#ifndef SOC_UART_H
#define SOC_UART_H

#include "uam/chardev.h"
#include "uam/access.h"
#include "uam/device.h"
#include "uam/signal.h"

#define SOC_UART_SIZE 0x10u

/* cd may be NULL (output discarded, no input). */
uam_device *soc_uart_create(const char *name, uam_chardev *cd);
uam_signal *soc_uart_irq_line(uam_device *dev);
/* Queue received bytes directly (ember-fuzz input route); returns how many fit the FIFO. */
size_t      soc_uart_feed(uam_device *dev, const uint8_t *data, size_t len);

#endif
