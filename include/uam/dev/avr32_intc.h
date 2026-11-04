/*
 * uam/dev/avr32_intc.h - AVR32 interrupt controller: groups of level-triggered
 * request lines, each group given one of the four interrupt levels and an
 * autovector (the handler's offset from EVBA).
 *
 * Request line `l` of group `g` is input g*32 + l. A group requests its level
 * while any of its lines is high; the controller drives the processor's INT0..INT3
 * inputs, and tells it the autovector of the group it is to take at a level:
 * among the requesting groups of that level, the lowest-numbered one.
 *
 * Register map (offsets from base, 32-bit registers):
 *   0x000 + 4*g   IPRg   [31:30] INTLEVEL, [13:0] AUTOVECTOR (read/write)
 *   0x100 + 4*g   IRRg   the request lines of group g (read-only)
 *   0x200         ICR3   the group taken at level 3 (read-only)
 *   0x204         ICR2
 *   0x208         ICR1
 *   0x20C         ICR0
 */
#ifndef UAM_DEV_AVR32_INTC_H
#define UAM_DEV_AVR32_INTC_H

#include "uam/device.h"
#include "uam/signal.h"

#define UAM_AVR32_INTC_SIZE       0x400u
#define UAM_AVR32_INTC_MAX_GROUPS 64u

uam_device *uam_avr32_intc_create(const char *name, unsigned n_groups);
/* The output for interrupt level 0..3 (connect to the processor's INTn input). */
uam_signal *uam_avr32_intc_level_line(uam_device *dev, unsigned level);
/* Signal sink for request lines: uam_signal_connect(&irq, uam_avr32_intc_input, intc, g * 32 + l). */
void        uam_avr32_intc_input(void *intc, unsigned line, int level);
/* The autovector for level 0..3 (a uam_avr32_autovector_fn, with the device as ctx). */
uint32_t    uam_avr32_intc_autovector(void *intc, unsigned level);

#endif /* UAM_DEV_AVR32_INTC_H */
