/*
 * uam/dev/mc146818.h - MC146818-compatible real-time clock with 256 bytes of
 * CMOS RAM (two banks of 128), as in PC chipsets.
 *
 * Views, mapped by the board:
 *   uam_mc146818_bank0(dev)  2 bytes: index (bit 7 = NMI mask), data   PC: 0x70-0x71
 *   uam_mc146818_bank1(dev)  2 bytes: index, data (bytes 128-255)      PC: 0x72-0x73
 *
 * Time is deterministic: the clock reads `epoch` (seconds since 1970-01-01
 * UTC) at virtual time 0 and advances with virtual time; the guest may set it.
 * Periodic, update-ended and alarm interrupts drive the IRQ line (PC: IRQ 8).
 */
#ifndef UAM_DEV_MC146818_H
#define UAM_DEV_MC146818_H

#include "uam/device.h"
#include "uam/sched.h"
#include "uam/signal.h"

uam_device *uam_mc146818_create(const char *name, uam_sched *sched, uint64_t tick_hz,
                                uint64_t epoch);
uam_device *uam_mc146818_bank0(uam_device *dev);   /* the device itself */
uam_device *uam_mc146818_bank1(uam_device *dev);
uam_signal *uam_mc146818_irq_line(uam_device *dev);
/* CMOS RAM as firmware will find it (the board's memory-size bytes and the
 * like); kept across device resets, as battery-backed RAM is. */
void        uam_mc146818_nvram_set(uam_device *dev, unsigned index, uint8_t val);
uint8_t     uam_mc146818_nvram_get(uam_device *dev, unsigned index);
/* The NMI-mask bit of the last index write (port 70h bit 7). */
bool        uam_mc146818_nmi_masked(uam_device *dev);

#endif /* UAM_DEV_MC146818_H */
