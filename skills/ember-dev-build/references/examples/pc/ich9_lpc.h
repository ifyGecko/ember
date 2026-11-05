/*
 * ich9_lpc.h - ICH9 LPC bridge, PCI 00:1f.0: the chipset's ACPI, SMI, interrupt routing and
 * reset logic, which every PC firmware programs early.
 *
 *   - PMBASE (0x40) + ACPI_CNTL.ACPI_EN (0x44) map the 128-byte ACPI I/O block: ember's
 *     uam_acpi_pm registers (PM1, PM1_CNT, 24-bit PM timer, GPE0) with the chipset's own
 *     registers (SMI_EN/SMI_STS, TCO) through the acpi_pm read/write hooks;
 *   - APM_CNT/APM_STS (ports 0xB2-0xB3, view ich9_apm): a write to APM_CNT raises SMI# when
 *     SMI_EN.APMC_EN and GBL_SMI_EN are set; no further SMI until software sets
 *     SMI_EN.EOS again; GEN_PMCON_1.SMI_LOCK freezes GBL_SMI_EN;
 *   - PIRQA-H routing (0x60-0x63, 0x68-0x6B): PCI interrupts go to the 8259 input each
 *     routing register names, and to I/O APIC pins 16-23;
 *   - RCBA (0xF0) maps the 16 KiB root complex register block (storage; its DnIR registers
 *     route INTA-D of devices 25-31 to PIRQs, ich9_pci_intx is the PCI bus's INTx router);
 *   - PM1_CNT.SLP_EN with SLP_TYP 7 (S5) powers off; reset control at 0xCF9 (view
 *     ich9_reset_control, the PCI bus's CONFIG_ADDRESS pass-through): RST_CPU pulses
 *     the reset line.
 *
 * Adapted from q35_uefi_emu. Register layout: Intel I/O Controller Hub 9 datasheet.
 */
#ifndef ICH9_LPC_H
#define ICH9_LPC_H

#include "uam/bus.h"
#include "uam/dev/pci.h"
#include "uam/device.h"
#include "uam/sched.h"
#include "uam/signal.h"

#define ICH9_LPC_DID  0x2918u
#define ICH9_AHCI_DID 0x2922u   /* ICH9R 6-port SATA AHCI controller, 00:1f.2 */

/* io_base: where the I/O port space is on the system bus (UAM_X86_PIO_BASE). */
uam_device *ich9_lpc_create(const char *name, uam_pci_bus *pci, uam_bus *sys,
                            uam_sched *sched, uint64_t tick_hz, uint64_t io_base);
uam_device *ich9_apm(uam_device *lpc);                 /* 2 bytes: APM_CNT, APM_STS */
/* RST_CNT at 0xCF9: give it to uam_pci_cam_passthrough, which passes it the accesses to
 * CONFIG_ADDRESS (0xCF8-0xCFB) other than a dword. */
uam_device *ich9_reset_control(uam_device *lpc);
uam_signal *ich9_smi_line(uam_device *lpc);            /* to every processor's SMI# */
uam_signal *ich9_sci_line(uam_device *lpc);            /* ACPI SCI (PC: IRQ 9) */
uam_signal *ich9_reset_line(uam_device *lpc);          /* system reset request */
/* Power off (S5): the board stops the machine. */
void        ich9_set_poweroff(uam_device *lpc, void (*fn)(void *ctx), void *ctx);
/* INTx router for the PCI bus (uam_pci_intx_fn): ctx is the LPC device. */
void        ich9_pci_intx(void *lpc, unsigned slot, unsigned pin, int level);
/* PCI interrupt inputs PIRQA-H (0-7): signal sink. */
void        ich9_pirq(void *lpc, unsigned pirq, int level);
/* Where routed PIRQs go: an 8259 input sink and an I/O APIC input sink (pins 16-23). */
void        ich9_set_irq_targets(uam_device *lpc, void (*pic)(void *, unsigned, int),
                                 void *pic_ctx, void (*ioapic)(void *, unsigned, int),
                                 void *ioapic_ctx);

#endif
