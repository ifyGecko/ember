/*
 * uam/isa/m6502.h - Public face of the 6502 ISA module.
 *
 * One descriptor, uam_isa_m6502, for the cores that run the 6502 instruction set: the
 * NMOS 6502 (and the parts built on it, the 6510 and the 2A03 among them) and the CMOS
 * 65C02. They share the registers (A, X, Y, the stack pointer S, the status P), the
 * 64 KiB address space with its zero page and the stack in page 1, the addressing
 * modes and the vectors; they differ in the instructions defined beyond the 151
 * documented ones, decimal mode, a handful of bugs the 65C02 fixed and the extra
 * instructions of the Rockwell and WDC 65C02s. Those are the CPU model,
 * uam_m6502_config, which the board selects with uam_m6502_configure() after
 * uam_machine_init() and before the first uam_machine_reset() (without that call the
 * hart is uam_m6502_nmos). Everything else - RAM, ROM, timers, serial ports, video and
 * sound chips, bank switching - is the board's: a 6502 has no on-chip peripheral.
 *
 * Memory. Address a is bus address a: there is one 16-bit space for code, data, the
 * zero page and the stack (0x0100-0x01FF). Effective addresses wrap at 64 KiB, and
 * within the zero page for the zero-page modes (zp,X, zp,Y and the pointers of
 * (zp,X), (zp),Y and (zp)).
 *
 * Reset and vectors. Reset loads the pc from the reset vector at 0xFFFC (the entry
 * given to uam_machine_reset is used only when that address cannot be read), sets I
 * and S = 0xFD (the three dummy pushes of the reset sequence) and, on CMOS, clears D.
 * NMI enters through 0xFFFA, IRQ and BRK through 0xFFFE; they push the pc and P (bit 5
 * set, B clear for an interrupt and set for BRK and PHP), set I, and on CMOS clear D.
 *
 * Interrupts. Input lines 0 .. UAM_M6502_LINE_NMI - 1 are wired-OR into the one
 * level-sensitive IRQ input: any line high requests an interrupt, taken between
 * instructions while I is clear. The device that raised its line clears it (a 6502
 * has no acknowledge cycle). UAM_M6502_LINE_NMI is the edge-triggered NMI: each
 * rising edge requests one. UAM_M6502_LINE_SO is the set-overflow input: a rising edge
 * sets V. As on the chip, CLI, SEI and PLP change what the next interrupt poll sees
 * one instruction late: after CLI (or a PLP that clears I) one more instruction runs
 * before a pending IRQ is taken; after SEI (or a PLP that sets I) an IRQ already
 * pending is still taken, with I set in the pushed P. RTI's I takes effect at once.
 * WAI (WDC) waits for an interrupt request, whether or not I masks it; STP, and the
 * NMOS JAM opcodes, stop the core until the next reset.
 *
 * Time. Every instruction is one tick of virtual time, whatever its cycle count.
 */
#ifndef UAM_ISA_M6502_H
#define UAM_ISA_M6502_H

#include "uam/isa.h"

extern const uam_isa uam_isa_m6502;

#define UAM_M6502_LINE_NMI 30u
#define UAM_M6502_LINE_SO  31u

/* The vectors. */
#define UAM_M6502_VEC_NMI   0xFFFAu
#define UAM_M6502_VEC_RESET 0xFFFCu
#define UAM_M6502_VEC_IRQ   0xFFFEu

/* The status register's bits. */
enum {
    UAM_M6502_P_C = 0x01, UAM_M6502_P_Z = 0x02, UAM_M6502_P_I = 0x04, UAM_M6502_P_D = 0x08,
    UAM_M6502_P_B = 0x10, UAM_M6502_P_U = 0x20, UAM_M6502_P_V = 0x40, UAM_M6502_P_N = 0x80,
};

typedef struct uam_m6502_config {
    const char *name;
    /* The 65C02: its instructions (BRA, PHX/PHY/PLX/PLY, STZ, TRB/TSB, INC/DEC A, BIT
     * #imm and zp,X/abs,X, (zp), JMP (abs,X)), every undefined opcode a NOP, JMP (abs)
     * reading its pointer across a page, valid N and Z after decimal ADC/SBC, D cleared
     * on interrupts and BRK, and a read-modify-write instruction writing memory once.
     * false: the NMOS 6502, whose undefined opcodes are the combined instructions
     * (SLO RLA SRE RRA SAX LAX DCP ISC ANC ALR ARR SBX, SBC 0xEB), NOPs with operands,
     * the JAMs and the unstable XAA, LXA, LAS, SHA, SHX, SHY and TAS; and whose
     * read-modify-write instructions write the unmodified byte back before the result. */
    bool        cmos;
    bool        decimal;   /* D selects BCD arithmetic (false: the 2A03, D is only a flag) */
    bool        bitops;    /* CMOS: RMB, SMB, BBR and BBS (Rockwell and WDC); else 1-byte NOPs */
    bool        wai_stp;   /* CMOS: WAI and STP (WDC); else 1-byte NOPs */
    uint8_t     magic;     /* NMOS: the constant XAA and LXA OR into A (it varies by chip) */
} uam_m6502_config;

extern const uam_m6502_config uam_m6502_nmos;     /* NMOS 6502 / 6510 */
extern const uam_m6502_config uam_m6502_2a03;     /* NMOS without decimal mode */
extern const uam_m6502_config uam_m6502_w65c02;   /* WDC W65C02S: bit operations, WAI, STP */
extern const uam_m6502_config uam_m6502_r65c02;   /* Rockwell R65C02: bit operations */

/* Select the CPU model of a 6502 hart. Takes effect at the next reset. Returns
 * UAM_ERR_INVALID if cpu is not a 6502 hart or cfg is malformed (CMOS options on an
 * NMOS model). */
uam_status uam_m6502_configure(uam_cpu *cpu, const uam_m6502_config *cfg);
/* The model in effect, or NULL if cpu is not a 6502 hart. */
const uam_m6502_config *uam_m6502_config_of(uam_cpu *cpu);

/* True if `isa` is the 6502 descriptor. */
static inline bool uam_m6502_is(const uam_isa *isa) { return isa == &uam_isa_m6502; }

#endif /* UAM_ISA_M6502_H */
