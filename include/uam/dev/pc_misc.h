/*
 * uam/dev/pc_misc.h - Small PC platform registers.
 *
 *   uam_pc_sysctl_create   System Control Port A (PC: port 0x92). Bit 0
 *                          (fast reset) pulses the reset line on a 0->1
 *                          write; bit 1 (A20 gate) is kept but A20 is never
 *                          masked.
 *   uam_postcode_create    POST-code latch (PC: port 0x80): the last byte
 *                          written reads back, and each code is logged at
 *                          debug level.
 *   uam_master_abort_create  Unclaimed accesses: reads all ones.
 *   uam_debugcon_create    Debug console port: bytes written go to a
 *                          character device; reads return `read_val`. The
 *                          board maps it where its firmware writes debug
 *                          output, if anywhere.
 */
#ifndef UAM_DEV_PC_MISC_H
#define UAM_DEV_PC_MISC_H

#include "uam/chardev.h"
#include "uam/device.h"
#include "uam/signal.h"

uam_device *uam_pc_sysctl_create(const char *name);
uam_signal *uam_pc_sysctl_reset_line(uam_device *dev);

uam_device *uam_postcode_create(const char *name);
uint8_t     uam_postcode_last(uam_device *dev);

uam_device *uam_debugcon_create(const char *name, uam_chardev *out, uint8_t read_val);

/* Master abort: what a PC returns for an access no device claims. Reads are
 * all ones, writes are dropped. Boards install it as the bus's unmapped device. */
uam_device *uam_master_abort_create(const char *name);

#endif /* UAM_DEV_PC_MISC_H */
