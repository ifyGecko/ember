/*
 * uam/dev/lpt.h - A PC parallel port (SPP): data, status and control
 * registers, with a printer always ready on the other side.
 *
 * View: 3 bytes, data / status / control          PC: 0x378 (or 0x278, 0x3BC)
 * A byte strobed out (control bit 0 pulsed high then low) goes to the
 * character device, if any; then ACK is pulsed and, with control bit 4 set,
 * the interrupt line (PC: IRQ 7 or 5) with it.
 */
#ifndef UAM_DEV_LPT_H
#define UAM_DEV_LPT_H

#include "uam/chardev.h"
#include "uam/device.h"
#include "uam/signal.h"

uam_device *uam_lpt_create(const char *name, uam_chardev *out);
uam_signal *uam_lpt_irq_line(uam_device *lpt);

#endif /* UAM_DEV_LPT_H */
