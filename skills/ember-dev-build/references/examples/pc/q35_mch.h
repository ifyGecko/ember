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
 *     of low memory) are RAM only to a processor in SMM (or the A-segment to everyone while
 *     D_OPEN is set); to everyone else the A-segment is video space and TSEG reads all
 *     ones. Decoded per access, by SMIACT#: the device mirrors low RAM in the SMM view
 *     (at cfg->smm_view, UAM_X86_SMM_VIEW) and has the board give every processor the two
 *     SMRAM windows (cfg->smram_window -> uam_x86_set_smram_window; D_CLS closes the
 *     A-segment's for data). No smm_changed hook is needed for it;
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
    uam_region  *low_ram;       /* the RAM region at 0, mirrored in the SMM view */
    uint64_t     low_ram_top;   /* end of RAM below 4 GiB (TOLUD's reset value) */
    uam_region  *boot_flash;    /* the flash ending at 4 GiB, for the PAM alias; may be NULL */
    uint64_t     smm_view;      /* the processors' SMM view aperture (UAM_X86_SMM_VIEW) */
    /* Set SMRAM window `slot` of the SMM view on every processor (the board calls
     * uam_x86_set_smram_window; data_closed: UAM_X86_SMRAM_DATA_CLOSED). size 0: none. */
    void       (*smram_window)(void *ctx, unsigned slot, uint64_t base, uint64_t size,
                               bool data_closed);
    void        *smram_ctx;
} q35_mch_config;

uam_device *q35_mch_create(const char *name, const q35_mch_config *cfg);

#endif
