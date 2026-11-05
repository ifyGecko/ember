/*
 * nor_flash.h - parallel NOR flash (AMD/JEDEC command set subset) as a ROMD region:
 * reads and instruction fetches come straight from the backing store (XIP works); command
 * writes go to this device.
 */
#ifndef NOR_FLASH_H
#define NOR_FLASH_H

#include "uam/access.h"
#include "uam/bus.h"
#include "uam/device.h"

/* sector: erase block size. id: manufacturer << 16 | device id, as the driver compares it. */
uam_device *nor_flash_create(const char *name, uam_bus *bus, uint64_t sector, uint32_t id);
/* The board maps the ROMD region (uam_bus_map_romd(..., &region)) and hands it back. */
void        nor_flash_set_region(uam_device *dev, uam_region *region);

#endif
