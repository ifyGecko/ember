/*
 * uam/dev/pc_misc.h - Small PC platform registers.
 *
 *   uam_pc_sysctl_create   System Control Port A (PC: port 0x92). Bit 0
 *                          (fast reset) pulses the reset line on a 0->1
 *                          write; bit 1 ("fast A20") drives the A20 line.
 *   uam_a20_gate_create    The A20 gate: enable inputs (port 92's bit 1, the
 *                          keyboard controller's output port bit 1, ...;
 *                          input n through uam_a20_gate_input) and the
 *                          processor's A20M# output, high (A20 masked) while
 *                          no input enables A20.
 *   uam_postcode_create    POST-code latch (PC: port 0x80): the last byte
 *                          written reads back, and each code is logged at
 *                          debug level.
 *   uam_master_abort_create  Unclaimed accesses: reads all ones.
 *   uam_smi_port_create    Software SMI port (PC: port 0xB2): every write
 *                          keeps the byte and pulses the output line, which
 *                          the board wires to the processors' SMI# - the
 *                          SMI is raised by the OUT itself, as a chipset's
 *                          I/O trap does. Reads return the last byte.
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
uam_signal *uam_pc_sysctl_a20_line(uam_device *dev);

uam_device *uam_a20_gate_create(const char *name);
void        uam_a20_gate_input(void *gate, unsigned n, int level);   /* signal sink, n < 8 */
uam_signal *uam_a20_gate_a20m_line(uam_device *dev);

uam_device *uam_postcode_create(const char *name);
uint8_t     uam_postcode_last(uam_device *dev);

uam_device *uam_debugcon_create(const char *name, uam_chardev *out, uint8_t read_val);

uam_device *uam_smi_port_create(const char *name);
uam_signal *uam_smi_port_line(uam_device *dev);

/* Master abort: what a PC returns for an access no device claims. Reads are
 * all ones, writes are dropped. Boards install it as the bus's unmapped device. */
uam_device *uam_master_abort_create(const char *name);

#endif /* UAM_DEV_PC_MISC_H */
