/*
 * uam/dev/clint.h - SiFive-compatible Core-Local Interruptor (one hart).
 *
 * Register map (offsets from base):
 *   0x0000  msip      bit 0 drives the software-interrupt line
 *   0x4000  mtimecmp  64-bit; timer line is high while mtime >= mtimecmp
 *   0xBFF8  mtime     64-bit; one tick per virtual-time tick (sched.h)
 */
#ifndef UAM_DEV_CLINT_H
#define UAM_DEV_CLINT_H

#include "uam/device.h"
#include "uam/sched.h"
#include "uam/signal.h"

#define UAM_CLINT_SIZE 0x10000u

uam_device *uam_clint_create(const char *name, uam_sched *sched);
uam_signal *uam_clint_msip_line(uam_device *dev);
uam_signal *uam_clint_mtip_line(uam_device *dev);

#endif /* UAM_DEV_CLINT_H */
