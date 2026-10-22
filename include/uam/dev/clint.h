/*
 * uam/dev/clint.h - RISC-V machine-level timer and software interrupt device
 * for one or more harts, in the ACLINT MSWI + MTIMER layout of the legacy
 * CLINT (RISC-V ACLINT specification).
 *
 * Register map (offsets from base), for hart h:
 *   0x0000 + 4*h  msip      bit 0 drives hart h's software-interrupt line
 *   0x4000 + 8*h  mtimecmp  64-bit; hart h's timer line is high while mtime >= it
 *   0xBFF8        mtime     64-bit, shared; one tick per virtual-time tick (sched.h)
 */
#ifndef UAM_DEV_CLINT_H
#define UAM_DEV_CLINT_H

#include "uam/device.h"
#include "uam/sched.h"
#include "uam/signal.h"

#define UAM_CLINT_SIZE 0x10000u

#define UAM_CLINT_MAX_HARTS 4095u

/* One hart (uam_clint_create) or n_harts. */
uam_device *uam_clint_create(const char *name, uam_sched *sched);
uam_device *uam_clint_create_harts(const char *name, uam_sched *sched, unsigned n_harts);
/* Hart h's lines (NULL beyond the last); the first two are hart 0's. */
uam_signal *uam_clint_msip_line(uam_device *dev);
uam_signal *uam_clint_mtip_line(uam_device *dev);
uam_signal *uam_clint_hart_msip_line(uam_device *dev, unsigned hart);
uam_signal *uam_clint_hart_mtip_line(uam_device *dev, unsigned hart);

#endif /* UAM_DEV_CLINT_H */
