/*
 * uam/dev/cfi_flash.h - NOR flash with the Intel/Sharp command set (CFI
 * "command set 0001"), byte-wide, mapped as a ROM device: the array executes
 * in place and reads at memory speed while in read-array mode; the command
 * interface switches it to status, identifier and CFI query modes.
 *
 * Commands: 0xFF read array, 0x70 read status, 0x50 clear status, 0x90 read
 * identifier, 0x98 CFI query, 0x40/0x10 program byte, 0x20/0xD0 block erase,
 * 0x60/0x01 and 0x60/0xD0 block lock/unlock (accepted, not enforced). Program
 * and erase complete at once; the status register then reads 0x80 (ready), or
 * with an error bit if the operation was refused.
 */
#ifndef UAM_DEV_CFI_FLASH_H
#define UAM_DEV_CFI_FLASH_H

#include "uam/bus.h"
#include "uam/device.h"

/* Create the device and map it at [base, base+size) with `data` as contents.
 * `block_size` is the erase block (a power of two dividing size). */
uam_device *uam_cfi_flash_map(uam_bus *bus, const char *name, uint64_t base, uint64_t size,
                              uint64_t block_size, const void *data, size_t len);
uam_region *uam_cfi_flash_region(uam_device *dev);
/* Write-protect the whole array (programs and erases fail with a status error). */
void        uam_cfi_flash_set_protected(uam_device *dev, bool on);

#endif /* UAM_DEV_CFI_FLASH_H */
