/*
 * uam/dev/ahci.h - AHCI 1.3 SATA host bus adapter (a PCI function).
 *
 * The HBA is a PCI mass-storage function (class 01:06:01) with its register
 * block (ABAR) in BAR5, an MSI capability (one vector, 64-bit) and a power
 * management capability, interrupt pin INTA. The board registers the
 * function (uam_ahci_func) wherever its chipset puts it, and may adjust the
 * IDs or add capabilities first.
 *
 * Ports: up to 32, each with an attached ATA disk or ATAPI CD/DVD drive, or
 * empty. Commands execute when software issues them (PxCI), so a command is
 * complete by the time the write returns. Supported:
 *   - ATA: IDENTIFY DEVICE, READ/WRITE (DMA, PIO, 28- and 48-bit), FLUSH
 *     CACHE (EXT), SET FEATURES, power-mode and verify commands, READ NATIVE
 *     MAX ADDRESS (EXT), device control (SRST) FISes; NCQ is not advertised;
 *   - ATAPI (PACKET): the SCSI multimedia commands firmware and operating
 *     systems use to find and read a disc (INQUIRY, TEST UNIT READY, REQUEST
 *     SENSE, READ CAPACITY, READ(10/12), READ TOC, MODE SENSE, GET
 *     CONFIGURATION, GET EVENT STATUS NOTIFICATION, START STOP UNIT,
 *     PREVENT ALLOW MEDIUM REMOVAL).
 * Received FISes (signature, PIO setup, D2H register) are posted to PxFB
 * while FIS receive is enabled.
 */
#ifndef UAM_DEV_AHCI_H
#define UAM_DEV_AHCI_H

#include "uam/blockdev.h"
#include "uam/dev/pci.h"

#define UAM_AHCI_MAX_PORTS 32u

uam_device   *uam_ahci_create(const char *name, unsigned nports, uint16_t vendor,
                              uint16_t device, uint8_t revision);
uam_pci_func *uam_ahci_func(uam_device *ahci);
/* Attach a disk (sector size 512) or, with `atapi`, a disc (sector size
 * 2048) to an empty port. The HBA does not take ownership of `bd`. */
uam_status    uam_ahci_attach(uam_device *ahci, unsigned port, uam_blockdev *bd, bool atapi);

#endif /* UAM_DEV_AHCI_H */
