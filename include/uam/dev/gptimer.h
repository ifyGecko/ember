/*
 * uam/dev/gptimer.h - A general-purpose timer: one down-counter in virtual-time ticks,
 * one-shot or periodic, with an interrupt line. For boards whose CPU architecture has
 * no timer of its own (8-bit AVR).
 *
 * Register map (offsets from base), 32 bits each, little-endian; any access width
 * (an 8-bit CPU programs a register a byte at a time):
 *   0x0 CTRL    bit 0 EN (counting), bit 1 PERIODIC (reload on expiry), bit 2 IE.
 *               Setting EN starts a period of LOAD ticks; clearing it stops the count.
 *   0x4 STATUS  bit 0 IF: set when the count expires; write 1 to clear.
 *   0x8 LOAD    ticks per period (0: never expires). Writing it while counting starts
 *               a new period.
 *   0xC COUNT   ticks left in the current period (read-only; 0 when stopped)
 * The line is IF & IE. A one-shot timer clears EN when it expires.
 */
#ifndef UAM_DEV_GPTIMER_H
#define UAM_DEV_GPTIMER_H

#include "uam/device.h"
#include "uam/sched.h"
#include "uam/signal.h"

#define UAM_GPTIMER_SIZE 0x10u

uam_device *uam_gptimer_create(const char *name, uam_sched *sched);
uam_signal *uam_gptimer_irq(uam_device *dev);
/* Clear IF, as a CPU that acknowledges the interrupt in hardware does (AVR vectors). */
void        uam_gptimer_ack(uam_device *dev);

#endif /* UAM_DEV_GPTIMER_H */
