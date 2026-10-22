/*
 * uam/dev/vga.h - A VGA-compatible display adapter: the register set of the
 * IBM VGA (miscellaneous output, sequencer, CRT controller, graphics and
 * attribute controllers, DAC) and 256 KiB of display memory in four planes,
 * which is what a VGA BIOS (an option ROM the board supplies) programs.
 *
 * Views, mapped by the board:
 *   uam_vga_io(dev)    48 bytes: ports 0x3B0-0x3DF (offset = port - 0x3B0); the
 *                      CRT controller and input status 1 answer at 0x3B4/0x3B5/
 *                      0x3BA or 0x3D4/0x3D5/0x3DA as miscellaneous output bit 0
 *                      selects (monochrome or colour addressing)
 *   uam_vga_mem(dev)   128 KiB: the legacy window, PC 0xA0000-0xBFFFF; the
 *                      graphics controller's memory map decides which part of
 *                      it answers (unmapped parts read 0xFF)
 * The PCI function (uam_vga_func: class 03:00:00, VGA-compatible with legacy
 * decoding and no BARs) is registered by a board with a PCI bus; the board
 * gives it the VGA BIOS with uam_pci_func_rom, or maps the BIOS at 0xC0000.
 *
 * CPU access to display memory follows the VGA rules: write modes 0-3 with the
 * data rotate, logical operation, set/reset, enable set/reset and bit mask;
 * read modes 0 (read map select) and 1 (colour compare and don't care); the
 * latches; the sequencer's map mask; chain-4 and odd/even addressing. Input
 * status 1 toggles display enable and vertical retrace on successive reads, so
 * polling loops finish deterministically.
 *
 * For the host: the text screen (characters and attributes) and an RGB
 * rendering of the current mode (text with the loaded font, 16-colour planar,
 * 256-colour chain-4), through the attribute palette and the DAC.
 */
#ifndef UAM_DEV_VGA_H
#define UAM_DEV_VGA_H

#include <stdio.h>

#include "uam/dev/pci.h"

#define UAM_VGA_IO_SIZE   0x30u
#define UAM_VGA_MEM_SIZE  0x20000u
#define UAM_VGA_VRAM_SIZE 0x40000u

uam_device   *uam_vga_create(const char *name, uint16_t vendor, uint16_t device, uint8_t revision);
uam_device   *uam_vga_io(uam_device *vga);
uam_device   *uam_vga_mem(uam_device *vga);
uam_pci_func *uam_vga_func(uam_device *vga);

/* Whether the adapter is in a text mode, and its size in character cells. */
bool          uam_vga_text_mode(uam_device *vga, unsigned *cols, unsigned *rows);
/* The text screen: up to `max` cells, each character | attribute << 8. */
unsigned      uam_vga_text(uam_device *vga, uint16_t *cells, unsigned max);
/* The text screen as lines of characters (code page 437 bytes 0x20-0x7E kept,
 * others shown as '.'), trailing blanks removed. */
void          uam_vga_text_dump(uam_device *vga, FILE *out);
/* Render the display into `rgb` (0x00RRGGBB, `maxw` x `maxh`); returns false
 * if the mode is not one it renders. *w, *h receive the size used. */
bool          uam_vga_render(uam_device *vga, uint32_t *rgb, unsigned maxw, unsigned maxh,
                             unsigned *w, unsigned *h);
/* Display memory as stored: plane p's byte at offset o is vram[o * 4 + p]. */
const uint8_t *uam_vga_vram(uam_device *vga);

#endif /* UAM_DEV_VGA_H */
