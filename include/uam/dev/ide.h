/*
 * uam/dev/ide.h - A parallel-ATA (IDE) controller: two channels, each with a
 * master and a slave drive (ATA disk or ATAPI CD/DVD), PIO and bus-master DMA.
 *
 * Views, mapped by the board (the ports are the PC's compatibility ones):
 *   uam_ide_cmd(dev, ch)   8 bytes: data (16-bit), error/features, sector
 *                          count, LBA low/mid/high, device, status/command
 *                                           PC: 0x1F0 (ch 0), 0x170 (ch 1)
 *   uam_ide_ctl(dev, ch)   1 byte: alternate status / device control
 *                                           PC: 0x3F6 (ch 0), 0x376 (ch 1)
 *   uam_ide_bm(dev)        16 bytes: bus-master command, status and PRD table
 *                          address per channel (channel 1 at +8); also BAR 4
 *                          of the PCI function
 * Interrupts: one INTRQ line per channel (PC: IRQ 14 and 15 in compatibility
 * mode). The PCI function (uam_ide_func: class 01:01:80, an IDE controller in
 * compatibility mode with bus mastering) is registered by the board if it has
 * a PCI bus; bus-master DMA goes through it.
 *
 * Commands: IDENTIFY (PACKET) DEVICE, READ/WRITE SECTORS and MULTIPLE (PIO;
 * CHS, 28-bit and 48-bit LBA), SET MULTIPLE MODE, INITIALIZE DEVICE
 * PARAMETERS (the CHS geometry), READ/WRITE DMA (EXT), EXECUTE DEVICE
 * DIAGNOSTIC, PACKET (ATAPI, PIO or DMA), and the rest of the ATA command set
 * the AHCI controller implements (devices/ata_core.c). Software reset
 * (device control SRST) and nIEN work; BSY is never seen (commands complete
 * immediately).
 */
#ifndef UAM_DEV_IDE_H
#define UAM_DEV_IDE_H

#include "uam/blockdev.h"
#include "uam/dev/pci.h"
#include "uam/signal.h"

uam_device   *uam_ide_create(const char *name, uint16_t vendor, uint16_t device, uint8_t revision);
uam_device   *uam_ide_cmd(uam_device *ide, unsigned channel);
uam_device   *uam_ide_ctl(uam_device *ide, unsigned channel);
uam_device   *uam_ide_bm(uam_device *ide);
uam_signal   *uam_ide_irq_line(uam_device *ide, unsigned channel);
uam_pci_func *uam_ide_func(uam_device *ide);
/* Attach a disk (sector size 512) or, with `atapi`, a disc (sector size 2048)
 * as drive `drive` (0 master, 1 slave) of `channel`. Not owned. */
uam_status    uam_ide_attach(uam_device *ide, unsigned channel, unsigned drive, uam_blockdev *bd,
                             bool atapi);

#endif /* UAM_DEV_IDE_H */
