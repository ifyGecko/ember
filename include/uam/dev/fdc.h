/*
 * uam/dev/fdc.h - An 82077AA-compatible floppy disk controller (PC AT mode)
 * with up to two drives, data moved through an 8237 DMA channel (or by PIO
 * through the FIFO when software selects non-DMA mode).
 *
 * Views, mapped by the board:
 *   uam_fdc_regs(dev)  6 bytes: status A/B (read as 0xFF), digital output,
 *                      tape drive, main status (read) / data rate select
 *                      (write), data FIFO                    PC: 0x3F0-0x3F5
 *   uam_fdc_dir(dev)   1 byte: digital input (disk change) / configuration
 *                      control                                PC: 0x3F7
 * (0x3F6 belongs to the IDE controller on a PC.) The interrupt line is the
 * controller's INT (PC: IRQ 6), gated by DOR bit 3.
 *
 * Commands: SPECIFY, SENSE DRIVE STATUS, RECALIBRATE, SENSE INTERRUPT STATUS,
 * SEEK, READ DATA, WRITE DATA (with multi-track), READ ID, FORMAT TRACK,
 * VERSION, CONFIGURE, LOCK, DUMPREG, PERPENDICULAR MODE; anything else is an
 * invalid command (ST0 0x80). Media geometry comes from the image size (160 KB
 * to 2.88 MB formats); seeks and rotation take no time.
 */
#ifndef UAM_DEV_FDC_H
#define UAM_DEV_FDC_H

#include "uam/blockdev.h"
#include "uam/device.h"
#include "uam/signal.h"

uam_device *uam_fdc_create(const char *name, uam_device *dma, unsigned dma_channel);
uam_device *uam_fdc_regs(uam_device *fdc);   /* the device itself */
uam_device *uam_fdc_dir(uam_device *fdc);
uam_signal *uam_fdc_irq_line(uam_device *fdc);
/* Insert a disk image (512-byte sectors) in drive 0 or 1; NULL ejects it.
 * Not owned. A size no standard format has is refused. */
uam_status  uam_fdc_insert(uam_device *fdc, unsigned drive, uam_blockdev *bd, bool read_only);

#endif /* UAM_DEV_FDC_H */
