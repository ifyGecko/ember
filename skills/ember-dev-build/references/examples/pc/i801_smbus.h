/*
 * i801_smbus.h - Intel PCH/ICH SMBus host controller ("i801"), PCI 00:1f.3, with SPD
 * EEPROMs on the bus. Memory initialization (the MRC, FSP-M, coreboot raminit) reads each
 * DIMM's SPD through it before it trains DRAM; a board whose firmware does that needs this
 * device with the SPD bytes of the memory it claims to have.
 *
 * Registers (I/O BAR4, SMBASE, 32 bytes): HST_STS 0x00 (BUSY 0, INTR 1, DEV_ERR 2, BUS_ERR
 * 3, FAILED 4, INUSE 6, BYTE_DONE 7; write 1 to clear), HST_CNT 0x02 (INTREN 0, KILL 1,
 * SMB_CMD [4:2], LAST_BYTE 5, START 6), HST_CMD 0x03, XMIT_SLVA 0x04 (address << 1 | read),
 * HST_D0 0x05, HST_D1 0x06, HOST_BLOCK_DB 0x07. Configuration 0x40 HOSTC: HST_EN (bit 0)
 * enables the host. Commands complete when START is written.
 *
 * SPD: EEPROMs at 0x50-0x57. DDR4 parts are 512 bytes in two pages, selected by writing to
 * the page addresses 0x36 (page 0) and 0x37 (page 1); smaller parts are one page.
 */
#ifndef I801_SMBUS_H
#define I801_SMBUS_H

#include "uam/dev/pci.h"
#include "uam/device.h"

#define I801_SMBUS_IO_SIZE 32u

uam_device   *i801_smbus_create(const char *name, uint16_t vendor, uint16_t device);
uam_pci_func *i801_smbus_func(uam_device *smb);   /* the board registers it (00:1f.3) */
/* An SPD EEPROM at 7-bit address 0x50-0x57 holding `len` bytes (copied; up to 512). */
uam_status    i801_smbus_add_spd(uam_device *smb, uint8_t addr, const uint8_t *data, size_t len);

#endif
