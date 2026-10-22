/*
 * uam/dev/hpet.h - High Precision Event Timer (IA-PC HPET specification 1.0a):
 * a 64-bit main counter at 14.31818 MHz and three comparators, in a 1 KiB
 * register block (PC: 0xFED00000).
 *
 * Timer 0 can be periodic; every timer can run in 32-bit mode, edge- or
 * level-triggered. Interrupts go to I/O APIC pins (each timer's route field,
 * through the `route` sink with the pin number as input), or, with legacy
 * replacement enabled, timer 0 to ISA IRQ 0 and timer 1 to IRQ 8 (through
 * the `legacy` sink with the IRQ number). Front-side-bus (MSI) delivery is
 * not offered.
 */
#ifndef UAM_DEV_HPET_H
#define UAM_DEV_HPET_H

#include "uam/device.h"
#include "uam/sched.h"
#include "uam/signal.h"

#define UAM_HPET_SIZE   0x400u
#define UAM_HPET_HZ     14318180u
#define UAM_HPET_TIMERS 3u

uam_device *uam_hpet_create(const char *name, uam_sched *sched, uint64_t tick_hz);
/* Interrupt sinks: `route(ctx, ioapic_pin, level)` and `legacy(ctx, isa_irq, level)`.
 * `route_cap` is the set of I/O APIC pins the board wired (bit n = pin n). */
void        uam_hpet_set_sinks(uam_device *dev, uam_signal_sink route, void *route_ctx,
                               uam_signal_sink legacy, void *legacy_ctx, uint32_t route_cap);
/* Legacy replacement is on: the board may silence the 8254 and RTC lines. */
bool        uam_hpet_legacy(uam_device *dev);

/* The PCI vendor ID the capabilities register reports (default 0). */
void        uam_hpet_set_vendor(uam_device *dev, uint16_t vendor);

#endif /* UAM_DEV_HPET_H */
