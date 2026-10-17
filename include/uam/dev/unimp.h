/*
 * uam/dev/unimp.h - "Unimplemented" catch-all window for a block of MMIO.
 *
 * Map it at a low priority (UAM_PRIO_BACKGROUND) over a whole peripheral block
 * and map real devices on top as they are written; it then sees only the
 * gaps. Reads return zero, writes are ignored (RAZ/WI). It logs the first
 * access to each offset (relative to its base, named after the window), counts
 * accesses per offset, and prints the most-accessed offsets when destroyed -
 * the bring-up to-do list for that block.
 *
 *     uam_device *u = uam_unimp_create("gpio", 0x18040000);
 *     uam_bus_map_mmio_ex(bus, "gpio", 0x18040000, 0x10000, u,
 *                         &(uam_map_attrs){ .prio = UAM_PRIO_BACKGROUND });
 *
 * The global fallback for addresses no window covers stays unknown_mmio.h.
 */
#ifndef UAM_DEV_UNIMP_H
#define UAM_DEV_UNIMP_H

#include "uam/device.h"

/* `base` is used only to print absolute addresses next to offsets. */
uam_device *uam_unimp_create(const char *name, uint64_t base);
/* Accesses so far (all offsets), and distinct offsets touched. */
uint64_t    uam_unimp_accesses(uam_device *dev);
size_t      uam_unimp_offsets(uam_device *dev);

#endif /* UAM_DEV_UNIMP_H */
