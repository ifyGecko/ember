/*
 * pch_spi.h - Intel PCH SPI flash controller, hardware sequencing (the "fast SPI" layout of
 * 100-series and later PCHs), PCI 00:1f.5 with its registers in a 4 KiB memory BAR0.
 *
 * On a real Intel board the boot flash is SPI NOR behind this controller, not a CFI part:
 * the BIOS region is decoded read-only just below 4 GiB, and everything that writes the
 * flash (UEFI variables, capsule updates, the fault-tolerant write area) goes through
 * hardware-sequencing cycles: FADDR = flash linear address, FDATA0-15 = up to 64 bytes,
 * HSFSTS_CTL.FCYCLE/FDBC/FGO to start, FDONE/FCERR to finish.
 *
 * The model maps the BIOS region of the image as a ROMD window ending at `window_top` (the
 * live bytes are that window's backing store, so snapshots cover them); the other regions
 * (descriptor, ME, GbE, PDR) are read from the image as loaded and refuse writes. Writes
 * need BIOS_CONTROL.WPD (config 0xDC bit 0), as on hardware; a flash protected range (PRn
 * with WPE) refuses them too. Without a valid flash descriptor (signature 0x0FF0A55A at
 * 0x10) the whole image is the BIOS region.
 */
#ifndef PCH_SPI_H
#define PCH_SPI_H

#include "uam/bus.h"
#include "uam/dev/pci.h"
#include "uam/device.h"

#define PCH_SPI_BAR_SIZE 0x1000u

/* `image` is the whole flash (copied). The BIOS window is mapped on `sys` at
 * window_top - BIOS region size. JEDEC ID: the 3 bytes a read-ID cycle returns. */
uam_device   *pch_spi_create(const char *name, uam_bus *sys, const uint8_t *image, size_t len,
                             uint64_t window_top, uint16_t vendor, uint16_t device,
                             const uint8_t jedec_id[3]);
uam_pci_func *pch_spi_func(uam_device *spi);      /* the board registers it (00:1f.5) */
uam_region   *pch_spi_window(uam_device *spi);    /* the BIOS window (e.g. for PAM aliases) */

#endif
