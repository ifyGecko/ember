/*
 * uam/dev/apic.h - x86 local APIC (xAPIC register interface) and I/O APIC.
 *
 * Local APIC: one per processor, a 4 KiB register page (PC: 0xFEE00000,
 * relocatable through IA32_APIC_BASE, see uam_lapic_set_base). It owns the
 * processor's INTR and NMI inputs:
 *   - its INTR line goes high while an accepted interrupt outranks the
 *     processor priority, or while LINT0 passes an 8259 request through
 *     (ExtINT, or the APIC disabled); uam_lapic_inta is the acknowledge;
 *   - its NMI line pulses for NMI deliveries and LINT1;
 *   - the LVT timer counts at `bus_hz` (before the divide register).
 *
 * I/O APIC: 24 redirection entries behind an index/data window (PC:
 * 0xFEC00000, 0x20 bytes plus the EOI register at 0x40). Input lines are
 * signal sinks; entries deliver interrupt messages to a local APIC.
 *
 * Messages between them use uam_apic_msg_fn, so a board can put a chipset
 * model in between (SMI delivery, MSI).
 */
#ifndef UAM_DEV_APIC_H
#define UAM_DEV_APIC_H

#include "uam/device.h"
#include "uam/sched.h"
#include "uam/signal.h"

#define UAM_LAPIC_SIZE   0x1000u
#define UAM_IOAPIC_SIZE  0x100u
#define UAM_IOAPIC_PINS  24u

/* Delivery modes (ICR / redirection entry bits 8-10). */
enum uam_apic_mode {
    UAM_APIC_FIXED = 0, UAM_APIC_LOWEST = 1, UAM_APIC_SMI = 2, UAM_APIC_NMI = 4,
    UAM_APIC_INIT = 5, UAM_APIC_SIPI = 6, UAM_APIC_EXTINT = 7,
};

/* An interrupt message: vector, delivery mode, level-triggered, destination. */
typedef void (*uam_apic_msg_fn)(void *ctx, uint8_t vector, unsigned mode, bool level,
                                uint8_t dest, bool logical);
/* End of interrupt for a level-triggered vector (local APIC -> I/O APICs). */
typedef void (*uam_apic_eoi_fn)(void *ctx, uint8_t vector);

/* ---- local APIC ---- */
uam_device *uam_lapic_create(const char *name, uam_sched *sched, uint64_t tick_hz,
                             uint64_t bus_hz, uint8_t apic_id);
uam_signal *uam_lapic_intr_line(uam_device *dev);
uam_signal *uam_lapic_nmi_line(uam_device *dev);
uam_signal *uam_lapic_smi_line(uam_device *dev);   /* SMI messages */
/* Interrupt acknowledge; asks the 8259 (`extint`) for ExtINT/pass-through. */
void        uam_lapic_set_extint(uam_device *dev, int (*inta)(void *ctx), void *ctx);
int         uam_lapic_inta(uam_device *dev);
/* Message sink (uam_apic_msg_fn) for I/O APICs, MSI and IPIs. */
void        uam_lapic_deliver(void *lapic, uint8_t vector, unsigned mode, bool level,
                              uint8_t dest, bool logical);
/* Signal sinks for the LINT0 (input 0) and LINT1 (input 1) pins. */
void        uam_lapic_lint(void *lapic, unsigned pin, int level);
void        uam_lapic_set_eoi_hook(uam_device *dev, uam_apic_eoi_fn fn, void *ctx);
/* IA32_APIC_BASE changed: the board moves/enables the register page; this
 * tells the APIC whether it is globally enabled. */
void        uam_lapic_set_enabled(uam_device *dev, bool enabled);

/* ---- I/O APIC ---- */
uam_device *uam_ioapic_create(const char *name, uint8_t id);
void        uam_ioapic_set_target(uam_device *dev, uam_apic_msg_fn fn, void *ctx);
void        uam_ioapic_input(void *ioapic, unsigned pin, int level);   /* signal sink */
void        uam_ioapic_eoi(void *ioapic, uint8_t vector);              /* uam_apic_eoi_fn */

#endif /* UAM_DEV_APIC_H */
