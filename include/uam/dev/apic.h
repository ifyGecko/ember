/*
 * uam/dev/apic.h - x86 local APIC (xAPIC register interface) and I/O APIC.
 *
 * Local APIC: one per processor, a 4 KiB register page (PC: 0xFEE00000,
 * relocatable through IA32_APIC_BASE) or, in x2APIC mode, the MSRs
 * 0x800-0x8FF (uam_lapic_rdmsr/wrmsr, which also serve IA32_TSC_DEADLINE).
 * It owns the processor's INTR and NMI inputs:
 *   - its INTR line goes high while an accepted interrupt outranks the
 *     processor priority, or while LINT0 passes an 8259 request through
 *     (ExtINT, or the APIC disabled); uam_lapic_inta is the acknowledge;
 *   - its NMI line pulses for NMI deliveries and LINT1;
 *   - the LVT timer counts at `bus_hz` (before the divide register), or in
 *     TSC-deadline mode fires when the processor's TSC reaches
 *     IA32_TSC_DEADLINE (the TSC counts one per virtual tick; the board
 *     supplies the processor's value with uam_lapic_set_tsc).
 *
 * I/O APIC: 24 redirection entries behind an index/data window (PC:
 * 0xFEC00000, 0x20 bytes plus the EOI register at 0x40). Input lines are
 * signal sinks; entries deliver interrupt messages to a local APIC.
 *
 * Messages between them use uam_apic_msg_fn, so a board can put a chipset
 * model in between (SMI delivery, MSI).
 *
 * APIC bus (several processors): the interconnect of the local APICs. It
 * routes interprocessor interrupts (ICR: physical and logical flat/cluster
 * destinations, broadcast and the shorthands; fixed, lowest priority -
 * the first matching APIC -, NMI, SMI, INIT and startup), I/O APIC
 * messages (uam_apic_bus_deliver as the I/O APIC's target) and MSI writes
 * (uam_apic_bus_msi as the PCI bus's MSI sink). Each local APIC drives its
 * processor's INIT# input (uam_lapic_init_line) and hands startup IPIs to
 * the callback set with uam_lapic_set_startup. Without a bus, a local APIC
 * takes every message itself (one processor).
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
uam_signal *uam_lapic_init_line(uam_device *dev);  /* INIT messages (pulsed) */
/* Startup IPIs (x86: uam_x86_sipi with the processor). */
void        uam_lapic_set_startup(uam_device *dev, void (*fn)(void *ctx, uint8_t vector),
                                  void *ctx);
/* Interrupt acknowledge; asks the 8259 (`extint`) for ExtINT/pass-through. */
void        uam_lapic_set_extint(uam_device *dev, int (*inta)(void *ctx), void *ctx);
int         uam_lapic_inta(uam_device *dev);
/* Message sink (uam_apic_msg_fn) for I/O APICs, MSI and IPIs. */
void        uam_lapic_deliver(void *lapic, uint8_t vector, unsigned mode, bool level,
                              uint8_t dest, bool logical);
/* Interrupt-message write (MSI): address 0xFEExxxxx with the destination in
 * bits 12-19 and logical mode in bit 2; data holds the vector (0-7),
 * delivery mode (8-10) and trigger mode (15). Other addresses are ignored.
 * Matches uam_pci_msi_fn. */
void        uam_lapic_msi(void *lapic, uint64_t addr, uint32_t data);
/* Signal sinks for the LINT0 (input 0) and LINT1 (input 1) pins. */
void        uam_lapic_lint(void *lapic, unsigned pin, int level);
void        uam_lapic_set_eoi_hook(uam_device *dev, uam_apic_eoi_fn fn, void *ctx);
/* IA32_APIC_BASE changed: the board moves/enables the register page; this
 * tells the APIC whether it is globally enabled. */
void        uam_lapic_set_enabled(uam_device *dev, bool enabled);
/* IA32_APIC_BASE.EXTD: x2APIC mode (the board also stops decoding the
 * register page). The x2APIC ID is the APIC's ID, zero-extended. */
void        uam_lapic_set_x2apic(uam_device *dev, bool on);
/* x2APIC registers (MSR 0x800 + offset/16) and IA32_TSC_DEADLINE (0x6E0):
 * for the processor's platform RDMSR/WRMSR hooks. Return false for any
 * other MSR. The processor faults these MSRs itself when the mode or the
 * feature is off. */
bool        uam_lapic_rdmsr(uam_device *dev, uint32_t msr, uint64_t *val);
bool        uam_lapic_wrmsr(uam_device *dev, uint32_t msr, uint64_t val);
/* The processor's TSC, read by the TSC-deadline timer (default: virtual time). */
void        uam_lapic_set_tsc(uam_device *dev, uint64_t (*read)(void *ctx), void *ctx);

/* ---- APIC bus ---- */
typedef struct uam_apic_bus uam_apic_bus;
/* A stateless device (add it to the machine so it is freed with it). */
uam_apic_bus *uam_apic_bus_create(const char *name);
uam_device   *uam_apic_bus_device(uam_apic_bus *bus);
uam_status    uam_apic_bus_add(uam_apic_bus *bus, uam_device *lapic);
/* uam_apic_msg_fn: an I/O APIC's (or chipset's) interrupt message. */
void          uam_apic_bus_deliver(void *bus, uint8_t vector, unsigned mode, bool level,
                                   uint8_t dest, bool logical);
/* uam_pci_msi_fn: an MSI write (address 0xFEExxxxx). */
void          uam_apic_bus_msi(void *bus, uint64_t addr, uint32_t data);

/*
 * The local APIC register page as every processor sees it at one address:
 * accesses go to the APIC of the processor running (current(ctx) returns its
 * index into `lapics`, e.g. uam_machine_current_cpu). Map this instead of a
 * single APIC on a multiprocessor board.
 */
uam_device   *uam_lapic_window_create(const char *name, uam_device *const *lapics, unsigned n,
                                      unsigned (*current)(void *ctx), void *ctx);

/* ---- I/O APIC ---- */
uam_device *uam_ioapic_create(const char *name, uint8_t id);
void        uam_ioapic_set_target(uam_device *dev, uam_apic_msg_fn fn, void *ctx);
void        uam_ioapic_input(void *ioapic, unsigned pin, int level);   /* signal sink */
void        uam_ioapic_eoi(void *ioapic, uint8_t vector);              /* uam_apic_eoi_fn */

#endif /* UAM_DEV_APIC_H */
