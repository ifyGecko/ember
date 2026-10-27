/*
 * uam/dev/uart16550.h - NS16550A-compatible UART (byte registers, packed or spaced).
 *
 * Transmit is instantaneous: bytes written to THR go straight to the chardev
 * backend. Receive bytes come from the chardev (console input) or from
 * uam_uart16550_feed() (a fuzzer). Supports the RX-data-available and
 * THR-empty interrupts.
 */
#ifndef UAM_DEV_UART16550_H
#define UAM_DEV_UART16550_H

#include "uam/access.h"
#include "uam/chardev.h"
#include "uam/device.h"
#include "uam/signal.h"

#define UAM_UART16550_SIZE 0x100u
#define UAM_UART16550_REGS 8u

/* Register layout. All zero is the PC's: eight byte registers at consecutive
 * offsets. SoCs often space them 4 bytes apart (reg_shift 2): each register then
 * fills a slot of 2^reg_shift bytes in `endian` byte order, the register in its
 * least significant byte (a whole-slot access, or a byte access to that byte,
 * reaches it; other bytes read 0). Map UAM_UART16550_REGS << reg_shift bytes, or
 * UAM_UART16550_SIZE, which holds reg_shift up to 5. */
typedef struct uam_uart16550_cfg {
    unsigned reg_shift;
    unsigned endian;    /* UAM_ACC_LE (0) or UAM_ACC_BE */
} uam_uart16550_cfg;

/* cd may be NULL: output is discarded and only feed() supplies input. */
uam_device *uam_uart16550_create(const char *name, uam_chardev *cd);
/* With a register layout (NULL: the PC's). NULL if reg_shift is above 5. */
uam_device *uam_uart16550_create_cfg(const char *name, uam_chardev *cd,
                                     const uam_uart16550_cfg *cfg);
uam_signal *uam_uart16550_irq_line(uam_device *dev);
/* Queue bytes for the guest to receive. Returns how many were accepted. */
size_t      uam_uart16550_feed(uam_device *dev, const void *data, size_t len);

#endif /* UAM_DEV_UART16550_H */
