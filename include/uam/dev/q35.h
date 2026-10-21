/*
 * uam/dev/q35.h - Intel Q35-class chipset: the memory controller hub (MCH,
 * PCI 00:00.0) and the ICH9 LPC bridge (PCI 00:1f.0) with its ACPI and SMI
 * logic. Register layouts follow the Intel 3 Series Express chipset and ICH9
 * datasheets; only registers firmware programs are modelled.
 *
 * MCH:
 *   - PCIEXBAR (0x60) maps the PCI ECAM window (uam_pci_ecam) where software
 *     places it, 64, 128 or 256 buses;
 *   - PAM0-6 (0x90-0x96): with a segment's read enable clear, the BIOS area
 *     0xE0000-0xFFFFF reads the top 128 KiB of the boot flash instead of RAM;
 *   - SMRAM (0x9D) and ESMRAMC (0x9E): the A-segment (0xA0000-0xBFFFF) and
 *     TSEG (the top of low memory) are visible as RAM only in SMM, or for the
 *     A-segment while D_OPEN is set; outside, the A-segment is legacy video
 *     space and TSEG reads as all ones. The board reports SMM entry and exit
 *     with uam_q35_mch_smm (the processor's smm_changed hook);
 *   - TOLUD (0xB0), TSEGMB (0xAC) and the other memory-map registers are
 *     plain registers firmware fills in.
 *
 * ICH9 LPC:
 *   - PMBASE (0x40) + ACPI_CNTL.ACPI_EN (0x44) map the 128-byte ACPI I/O
 *     block: PM1 status/enable/control, the 24-bit PM timer (3.579545 MHz),
 *     GPE0, SMI_EN/SMI_STS, the TCO registers;
 *   - APM_CNT/APM_STS (PC ports 0xB2-0xB3, view uam_ich9_apm): a write to
 *     APM_CNT raises SMI# when SMI_EN.APMC_EN and GBL_SMI_EN are set; SMI#
 *     stays off until software sets SMI_EN.EOS again;
 *   - PIRQA-H routing (0x60-0x63, 0x68-0x6B): PCI interrupt inputs go to the
 *     8259 input each routing register names, and to I/O APIC pins 16-23;
 *   - RCBA (0xF0) maps the 16 KiB root complex register block (storage);
 *   - PM1_CNT.SLP_EN with SLP_TYP S5 powers off; the reset-control register
 *     (0xCF9, uam_ich9_reset_control as the PCI bus's CF9 hook) resets.
 */
#ifndef UAM_DEV_Q35_H
#define UAM_DEV_Q35_H

#include "uam/bus.h"
#include "uam/dev/pci.h"
#include "uam/device.h"
#include "uam/sched.h"
#include "uam/signal.h"

#define UAM_Q35_MCH_DID   0x29C0u
#define UAM_ICH9_LPC_DID  0x2918u
#define UAM_ICH9_PM_HZ    3579545u

typedef struct uam_q35_mch_config {
    uam_bus     *sys;
    uam_pci_bus *pci;
    uint64_t     low_ram_top;   /* end of RAM below 4 GiB (TOLUD's reset value) */
    uam_region  *boot_flash;    /* the flash ending at 4 GiB, for the PAM alias; may be NULL */
} uam_q35_mch_config;

uam_device *uam_q35_mch_create(const char *name, const uam_q35_mch_config *cfg);
/* The processor entered (true) or left SMM. */
void        uam_q35_mch_smm(uam_device *mch, bool in_smm);

uam_device *uam_ich9_lpc_create(const char *name, uam_pci_bus *pci, uam_bus *sys,
                                uam_sched *sched, uint64_t tick_hz, uint64_t io_base);
uam_device *uam_ich9_apm(uam_device *lpc);                 /* 2 bytes: APM_CNT, APM_STS */
uam_signal *uam_ich9_smi_line(uam_device *lpc);            /* to the processor's SMI# */
uam_signal *uam_ich9_sci_line(uam_device *lpc);            /* ACPI SCI (PC: IRQ 9) */
uam_signal *uam_ich9_reset_line(uam_device *lpc);          /* system reset request */
/* Power off (S5): the board stops the machine. */
void        uam_ich9_set_poweroff(uam_device *lpc, void (*fn)(void *ctx), void *ctx);
/* PCI interrupt inputs PIRQA-H (0-7): signal sink. */
void        uam_ich9_pirq(void *lpc, unsigned pirq, int level);
/* Where routed PIRQs go: an 8259 input sink and an I/O APIC input sink (pins 16-23). */
void        uam_ich9_set_irq_targets(uam_device *lpc, void (*pic)(void *, unsigned, int),
                                     void *pic_ctx, void (*ioapic)(void *, unsigned, int),
                                     void *ioapic_ctx);
/* 0xCF9 hook (uam_pci_set_cf9_hook). */
void        uam_ich9_reset_control(void *lpc, uint8_t val);

#endif /* UAM_DEV_Q35_H */
