/*
 * uam/dev/mcs51_serial.h - The 8051's serial port: SCON and SBUF, every 8051's UART.
 *
 * Register map (offsets from base; map it at SFR 0x98), bytes:
 *   0 SCON  SM0 SM1 SM2 REN TB8 RB8 TI RI
 *   1 SBUF  write: transmit; read: the byte received
 * Host side: a uam_chardev. A byte written to SBUF goes out at once and sets TI. While
 * REN is set and RI is clear, the next byte received is loaded into SBUF, RB8 is set
 * (the stop bit) and RI is set; received bytes wait in a 256-byte FIFO, and the
 * chardev holds back what does not fit. The mode bits and baud rate only matter to
 * the wire, which is not modelled.
 *
 * The interrupt line is RI | TI (the 8051's serial source, 4); taking it clears
 * neither, as on the chip.
 */
#ifndef UAM_DEV_MCS51_SERIAL_H
#define UAM_DEV_MCS51_SERIAL_H

#include "uam/chardev.h"
#include "uam/device.h"
#include "uam/signal.h"

#define UAM_MCS51_SERIAL_SIZE 2u

uam_device *uam_mcs51_serial_create(const char *name, uam_chardev *cd);
uam_signal *uam_mcs51_serial_irq(uam_device *dev);
/* Queue bytes as if received on the line; returns how many fit the FIFO (the rest
 * are dropped). For machine inputs (fuzzing). */
size_t      uam_mcs51_serial_feed(uam_device *dev, const void *data, size_t len);

#endif /* UAM_DEV_MCS51_SERIAL_H */
