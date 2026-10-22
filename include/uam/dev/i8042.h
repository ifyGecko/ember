/*
 * uam/dev/i8042.h - 8042-compatible PS/2 keyboard controller with a keyboard
 * and a mouse attached.
 *
 * Views, mapped by the board:
 *   uam_i8042_data(dev)     1 byte: data (PC: port 0x60)
 *   uam_i8042_cmd(dev)      1 byte: status (read), command (write) (PC: 0x64)
 *
 * Controller: the configuration byte (0x20/0x60), self tests (0xAA, 0xAB,
 * 0xA9), keyboard/mouse interface enable and disable (0xAD/0xAE, 0xA7/0xA8),
 * the output port (0xD0/0xD1: bit 0 low resets the system, bit 1 is the A20
 * gate; 0xDD/0xDF turn A20 off/on), writes to the
 * keyboard/mouse output buffers (0xD2/0xD3), commands to the mouse (0xD4),
 * and output-line pulses (0xF0-0xFF; 0xFE resets). Keyboard: reset, identify,
 * enable/disable, LEDs, typematic, scan-code set (2 native; the controller
 * translates to set 1 when configuration bit 6 is set), echo. Mouse: reset,
 * identify, enable/disable and the parameter commands (no movement reports).
 *
 * The host types through uam_i8042_key (set-2 scan codes) or
 * uam_i8042_type (ASCII text).
 */
#ifndef UAM_DEV_I8042_H
#define UAM_DEV_I8042_H

#include "uam/device.h"
#include "uam/signal.h"

uam_device *uam_i8042_create(const char *name);
uam_device *uam_i8042_data(uam_device *dev);   /* the device itself */
uam_device *uam_i8042_cmd(uam_device *dev);
uam_signal *uam_i8042_kbd_irq(uam_device *dev);     /* PC: IRQ 1 */
uam_signal *uam_i8042_aux_irq(uam_device *dev);     /* PC: IRQ 12 */
uam_signal *uam_i8042_reset_line(uam_device *dev);  /* pulsed for a system reset */
uam_signal *uam_i8042_a20_line(uam_device *dev);    /* output port bit 1: A20 gate (high = on) */
/* Queue keyboard bytes as the keyboard sends them (set-2 scan codes:
 * make codes, 0xF0 break prefixes, 0xE0 extended prefixes). */
void        uam_i8042_key(uam_device *dev, const uint8_t *codes, size_t n);
/* Type printable ASCII, '\n' and '\b' (make and break of each key, with
 * Shift where needed). Returns the number of characters it could type. */
size_t      uam_i8042_type(uam_device *dev, const char *text, size_t n);

#endif /* UAM_DEV_I8042_H */
