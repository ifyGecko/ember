/*
 * uam/dev/mcs51_port.h - Single-byte SFRs of 8051 parts: an I/O port and a plain
 * register.
 *
 * Port (P0-P3 and the like): one byte, the output latch, 0xFF after reset as on the
 * chip. A read returns latch & pins (an input pin reads low only when driven low);
 * nothing drives the pins unless the board calls uam_mcs51_port_set_pins.
 *
 * Register: one plain byte, `reset_value` after reset, optionally reporting each value
 * written (and the reset value) to a callback: a code-bank select whose value the board
 * turns into banks (IFBANK in bits 1:0 and COBANK in bits 5:4, say) for
 * uam_mcs51_set_code_bank, or a page register MOVX @Ri reads.
 */
#ifndef UAM_DEV_MCS51_PORT_H
#define UAM_DEV_MCS51_PORT_H

#include "uam/device.h"

uam_device *uam_mcs51_port_create(const char *name);
void        uam_mcs51_port_set_pins(uam_device *dev, uint8_t pins);

typedef void (*uam_mcs51_reg_fn)(void *ctx, uint8_t value);
uam_device *uam_mcs51_reg_create(const char *name, uint8_t reset_value, uam_mcs51_reg_fn fn, void *ctx);

#endif /* UAM_DEV_MCS51_PORT_H */
