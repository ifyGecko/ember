/*
 * uam/isa/z80.h - Public face of the Z80 ISA module.
 *
 * One descriptor, uam_isa_z80, for cores that run the Zilog Z80 instruction set. The CPU
 * model, uam_z80_config, says what differs between them; the board selects it with
 * uam_z80_configure() after uam_machine_init() and before the first uam_machine_reset()
 * (without that call the hart is uam_z80_nmos_ref). Today the models are the NMOS and
 * CMOS Z80; the structure leaves room for relatives (8080, Z180) as further models.
 * Everything outside the core - the CTC, PIO, SIO, memory banking - is a device the board
 * wires.
 *
 * The instruction set is the whole Z80, documented and undocumented: IXH/IXL/IYH/IYL,
 * SLL, the DDCB/FDCB forms that also copy their result to a register, the flags' bits 3
 * and 5 (from the result, from MEMPTR for BIT n,(HL), from the pc for repeating block
 * instructions, from Q for SCF and CCF), MEMPTR itself, chains of DD/FD prefixes (the last
 * one counts), the ED opcodes that do nothing, and R counting every opcode fetch (bit 7
 * kept). Every instruction, prefixes included, is one tick of virtual time.
 *
 * Memory. ember puts the two address spaces on the one bus:
 *   memory (64 KiB)         bus 0x00000 + address
 *   I/O ports               UAM_Z80_IO_BASE + port
 * The core puts a 16-bit port address on the bus, as the Z80 does: IN/OUT (n) put A on the
 * high byte, IN/OUT (C) and the block I/O instructions B. Most boards decode only the low
 * byte; uam_z80_set_io_decode() masks the port address before it reaches the bus.
 *
 * Interrupts. Input line UAM_Z80_LINE_INT is /INT (level: 1 = asserted), taken when IFF1
 * is set, never directly after EI, in the mode IM selected:
 *   IM 0   the core executes the byte the interrupting device puts on the data bus: RST p,
 *          or CALL nn (three bytes)
 *   IM 1   RST 38h
 *   IM 2   a call through the word at (I << 8) | the device's byte
 * The data-bus bytes come from the board's interrupt-acknowledge hook
 * (uam_z80_set_inta), which runs in every mode, as the acknowledge cycle does (in IM 1 the
 * byte is ignored); without one the bus reads 0xFF, which is RST 38h in IM 0. Line
 * UAM_Z80_LINE_NMI is /NMI: a rising edge latches a non-maskable interrupt, a call to
 * 0x0066 that clears IFF1 and keeps IFF2 for RETN. RETI (ED 4D) is reported to the board
 * (uam_z80_set_reti), as Z80 peripherals watch the bus for it to end their interrupt.
 * HALT waits for an interrupt; the address pushed is the instruction after it.
 */
#ifndef UAM_ISA_Z80_H
#define UAM_ISA_Z80_H

#include "uam/isa.h"

extern const uam_isa uam_isa_z80;

#define UAM_Z80_IO_BASE UINT64_C(0x10000)

enum { UAM_Z80_LINE_INT = 0, UAM_Z80_LINE_NMI = 1 };

typedef struct uam_z80_config {
    const char *name;
    uint8_t     out_c_zero;  /* the byte OUT (C),0 (ED 71) writes: 0x00 NMOS, 0xFF CMOS */
} uam_z80_config;

extern const uam_z80_config uam_z80_nmos_ref;   /* the NMOS Z80 */
extern const uam_z80_config uam_z80_cmos_ref;   /* the CMOS Z80 (Z84C00) */

/* Select the CPU model of a Z80 hart. Takes effect at the next reset. Returns
 * UAM_ERR_INVALID if cpu is not a Z80 hart. */
uam_status uam_z80_configure(uam_cpu *cpu, const uam_z80_config *cfg);
/* The model in effect, or NULL if cpu is not a Z80 hart. */
const uam_z80_config *uam_z80_config_of(uam_cpu *cpu);

/* The board's port address decoding: the core ANDs every port address with mask
 * (0xFFFF, the default: all 16 bits; 0x00FF: the low byte only). Set it before the first
 * reset. */
void uam_z80_set_io_decode(uam_cpu *cpu, uint16_t mask);

/* Interrupt acknowledge: byte n (0, then 1 and 2 for an IM 0 CALL) the device on the data
 * bus answers with. */
typedef uint8_t (*uam_z80_inta_fn)(void *ctx, unsigned n);
void uam_z80_set_inta(uam_cpu *cpu, uam_z80_inta_fn fn, void *ctx);
/* Called each time the core executes RETI. */
typedef void (*uam_z80_reti_fn)(void *ctx);
void uam_z80_set_reti(uam_cpu *cpu, uam_z80_reti_fn fn, void *ctx);

/* True if `isa` is the Z80 descriptor. */
static inline bool uam_z80_is(const uam_isa *isa) { return isa == &uam_isa_z80; }

#endif /* UAM_ISA_Z80_H */
