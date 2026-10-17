/*
 * uam/dev/unknown_mmio.h - Fallback for accesses that hit no mapped region.
 *
 * Installed with uam_bus_set_unmapped(). It receives absolute addresses and
 * logs the first access to each address. That log is the to-do list during
 * firmware bring-up: every entry is a peripheral register the board does not
 * model yet.
 *
 * Policies:
 *   ZERO  reads return 0, writes are dropped
 *   ECHO  reads return the last value written to that address (else 0)
 */
#ifndef UAM_DEV_UNKNOWN_MMIO_H
#define UAM_DEV_UNKNOWN_MMIO_H

#include "uam/device.h"

typedef enum uam_unknown_policy { UAM_UNKNOWN_POLICY_ZERO, UAM_UNKNOWN_POLICY_ECHO } uam_unknown_policy;

uam_device *uam_unknown_mmio_create(const char *name, uam_unknown_policy policy);
/* Number of distinct unmapped addresses touched so far. */
size_t      uam_unknown_mmio_count(uam_device *dev);

#endif /* UAM_DEV_UNKNOWN_MMIO_H */
