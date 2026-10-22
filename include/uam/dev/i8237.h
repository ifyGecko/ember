/*
 * uam/dev/i8237.h - The PC's two cascaded 8237 DMA controllers and their page
 * registers: channels 0-3 move bytes, channels 4-7 move words (channel 4 is
 * the cascade).
 *
 * Views, mapped by the board:
 *   uam_i8237_dma1(dev)   16 bytes: controller 1                   PC: 0x00-0x0F
 *   uam_i8237_dma2(dev)   32 bytes: controller 2, registers at
 *                         even offsets                              PC: 0xC0-0xDF
 *   uam_i8237_page(dev)   15 bytes: page registers, offset n is
 *                         port 0x81 + n (0x80, the POST-code port, is
 *                         another device's)                         PC: 0x81-0x8F
 *
 * Per controller: base/current address and count per channel (16 bits, read
 * and written a byte at a time through the byte-pointer flip-flop), mode,
 * request and mask registers, the status register (terminal count and request
 * bits; reading it clears the TC bits), the command register, and the commands
 * clear flip-flop, master clear, clear mask, write all masks. The page
 * registers give address bits 16-23 (channels 5-7: bits 17-23, the word
 * address taking bit 16 from the address register).
 *
 * Devices move data with uam_i8237_transfer when their DREQ would be active;
 * DREQ itself and the bus timing are not modelled.
 */
#ifndef UAM_DEV_I8237_H
#define UAM_DEV_I8237_H

#include "uam/bus.h"
#include "uam/device.h"

uam_device *uam_i8237_create(const char *name, uam_bus *bus);
uam_device *uam_i8237_dma1(uam_device *dev);   /* the device itself */
uam_device *uam_i8237_dma2(uam_device *dev);
uam_device *uam_i8237_page(uam_device *dev);

/*
 * Move up to `len` bytes for channel `chan` between memory and `buf`, as the
 * channel is programmed: the mode's direction (a write transfer stores `buf`
 * into memory, a read transfer fills `buf` from memory, verify moves nothing),
 * address increment or decrement, and auto-initialisation at terminal count.
 * Word channels move whole words (len is rounded down). Stops at terminal
 * count (unless auto-initialising). Returns the bytes moved; 0 if the channel
 * is masked. *tc, if given, says whether terminal count was reached.
 */
unsigned    uam_i8237_transfer(uam_device *dev, unsigned chan, uint8_t *buf, unsigned len, bool *tc);
/* Whether the channel's mode is a write transfer (device to memory). */
bool        uam_i8237_writes_memory(uam_device *dev, unsigned chan);

#endif /* UAM_DEV_I8237_H */
