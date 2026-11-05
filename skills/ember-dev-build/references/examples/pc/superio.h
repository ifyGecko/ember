/*
 * superio.h - LPC Super I/O configuration (Nuvoton/Winbond/Fintek/ITE/SMSC style): an
 * index/data port pair (PC: 0x2E/0x2F or 0x4E/0x4F) behind an enter key, global registers
 * 0x00-0x2F (0x07 logical device select, 0x20/0x21 chip ID) and per-logical-device
 * registers 0x30-0xFF (0x30 activate, 0x60/0x61 I/O base, 0x70 IRQ).
 *
 * The chip's functions (its UARTs, keyboard controller, hardware monitor, GPIO) are other
 * devices - a UART is ember's uam_uart16550 - whose I/O regions the board maps disabled
 * and hands over with superio_add_ldn: the Super I/O moves and enables each one where the
 * firmware programs its logical device, exactly as the chip's address decoder would.
 */
#ifndef SUPERIO_H
#define SUPERIO_H

#include "uam/bus.h"
#include "uam/device.h"

#define SUPERIO_SIZE    2u    /* index, data */
#define SUPERIO_MAX_LDN 16u

typedef struct superio_config {
    uint8_t  enter_key[4];     /* written to the index port to enter configuration mode:
                                * 0x87 0x87 (Nuvoton, Winbond, Fintek), 0x55 (SMSC),
                                * 0x87 0x01 0x55 0x55 (ITE at 0x2E) */
    unsigned enter_len;
    uint8_t  exit_key;         /* written to the index port to leave it (0xAA); ITE leaves by
                                * setting bit 1 of global register 0x02, also honoured */
    uint16_t chip_id;          /* registers 0x20 (high byte) and 0x21 (low byte) */
} superio_config;

/* io_base: where the I/O port space sits on `sys` (UAM_X86_PIO_BASE). */
uam_device *superio_create(const char *name, const superio_config *cfg, uam_bus *sys,
                           uint64_t io_base);
/* Logical device `ldn` decodes `io` (mapped disabled by the board) at the base in its
 * 0x60/0x61 registers while bit 0 of its 0x30 register is set. reset_base / reset_irq are
 * the power-on values of 0x60/0x61 and 0x70; active says whether it powers on enabled. */
uam_status  superio_add_ldn(uam_device *sio, unsigned ldn, uam_region *io, uint16_t reset_base,
                            uint8_t reset_irq, bool active);

#endif
