/*
 * q35_mch.h - Q35-class memory controller hub (host bridge), PCI 00:00.0. A host bridge
 * decodes nothing itself: it moves and switches regions on the system bus as firmware
 * programs it, and as the processor enters and leaves SMM.
 *
 *   - PCIEXBAR (0x60) maps the PCI ECAM window (uam_pci_ecam) where software places it,
 *     64, 128 or 256 buses;
 *   - PAM0-6 (0x90-0x96): with a segment's read enable clear, the BIOS area
 *     0xE0000-0xFFFFF reads the top 128 KiB of the boot flash instead of RAM;
 *   - SMRAM (0x9D) and ESMRAMC (0x9E): the A-segment (0xA0000-0xBFFFF) and TSEG (the top
 *     of low memory) are RAM only in SMM (or the A-segment while D_OPEN is set); outside,
 *     they read as all ones. The board reports SMM entry and exit with q35_mch_smm (its
 *     uam_x86_platform.smm_changed hook);
 *   - TOLUD (0xB0), TSEGMB (0xAC) and the other memory-map registers are plain registers.
 *
 * Adapted from q35_uefi_emu (the out-of-tree Q35 board that boots OVMF on ember). Register
 * layout: Intel 3 Series Express chipset datasheet; only what firmware programs.
 */
#ifndef Q35_MCH_H
#define Q35_MCH_H

#include "uam/bus.h"
#include "uam/dev/pci.h"
#include "uam/device.h"

#define Q35_MCH_DID 0x29C0u

typedef struct q35_mch_config {
    uam_bus     *sys;
    uam_pci_bus *pci;
    uint64_t     low_ram_top;   /* end of RAM below 4 GiB (TOLUD's reset value) */
    uam_region  *boot_flash;    /* the flash ending at 4 GiB, for the PAM alias; may be NULL */
} q35_mch_config;

uam_device *q35_mch_create(const char *name, const q35_mch_config *cfg);
/* The processor entered (true) or left SMM (SMIACT#). */
void        q35_mch_smm(uam_device *mch, bool in_smm);

#endif
