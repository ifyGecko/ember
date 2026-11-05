/*
 * reg_stride.h - byte registers spaced 1 << shift bytes apart (see reg_stride.c).
 */
#ifndef REG_STRIDE_H
#define REG_STRIDE_H

#include "uam/access.h"
#include "uam/device.h"

/* shift >= 2 (byte-spaced registers need no adapter). Takes ownership of `inner`
 * (also on failure). Map it with the SoC's byte order:
 *   uam_bus_map_mmio_ex(bus, name, base, 8 << shift, dev, &(uam_map_attrs){ .endian = UAM_ACC_BE }); */
uam_device *reg_stride_create(const char *name, uam_device *inner, unsigned shift);

#endif
