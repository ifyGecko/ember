/*
 * z80.h - Internal definitions shared by the Z80 lifter and system code.
 * Not part of the public interface.
 */
#ifndef UAM_Z80_INTERNAL_H
#define UAM_Z80_INTERNAL_H

#include "uam/isa/z80.h"

/* Register file. The pairs are little-endian 16-bit words, so a pair is one GET/PUT and
 * its halves are bytes: C at 0, B at 1, and so on; F is the flags byte as the Z80 has it
 * (S Z Y H X P/V N C, bits 7-0) with A above it, so AF is the word PUSH AF writes. R is
 * kept as a counter (RCNT, of which bits 0-6 count) and bit 7 (R7, 0 or 0x80). QV is 1
 * when the last instruction set the flags (Q = F then, else Q = 0: SCF and CCF use it). */
#define Z80_C     0u
#define Z80_B     1u
#define Z80_E     2u
#define Z80_D     3u
#define Z80_L     4u
#define Z80_H     5u
#define Z80_F     6u
#define Z80_A     7u
#define Z80_IXL   8u
#define Z80_IXH   9u
#define Z80_IYL   10u
#define Z80_IYH   11u
#define Z80_SP    12u
#define Z80_BC    Z80_C
#define Z80_DE    Z80_E
#define Z80_HL    Z80_L
#define Z80_AF    Z80_F
#define Z80_IX    Z80_IXL
#define Z80_IY    Z80_IYL
#define Z80_BC2   14u      /* the alternate set: BC' DE' HL' AF' */
#define Z80_DE2   16u
#define Z80_HL2   18u
#define Z80_AF2   20u
#define Z80_I     22u
#define Z80_RCNT  23u
#define Z80_R7    24u
#define Z80_QV    25u
#define Z80_WZ    26u      /* MEMPTR */
#define Z80_REGFILE_SIZE 32u

/* Flag bits. */
enum { ZF_C = 0x01, ZF_N = 0x02, ZF_PV = 0x04, ZF_X = 0x08, ZF_H = 0x10, ZF_Y = 0x20,
       ZF_Z = 0x40, ZF_S = 0x80 };

/* System register ids (SYSRD/SYSWR). */
enum {
    Z80_SYS_IFF,     /* bit 0 IFF1, bit 1 IFF2 (EI writes 3, DI 0; reads for LD A,I/R) */
    Z80_SYS_IM,      /* the interrupt mode, 0-2 */
    Z80_SYS_RETN,    /* write: IFF1 = IFF2 (RETN and RETI); value 1 = RETI */
    Z80_SYS_SHADOW,  /* write: an EI whose shadow the block could not hold (see irq_take) */
};

/* ISA-private hart state. */
typedef struct z80_state {
    uam_z80_config cfg;
    uint16_t io_mask;
    bool     iff1, iff2;
    uint8_t  im;
    bool     int_line;        /* /INT asserted */
    bool     nmi_line, nmi_pending;
    bool     ei_shadow;       /* the instruction after EI has not run yet */
    bool     halted;          /* a bus fault the core has no answer for */
    unsigned last_line;
    uam_z80_inta_fn inta;
    void           *inta_ctx;
    uam_z80_reti_fn reti;
    void           *reti_ctx;
} z80_state;

static inline z80_state *z80_st(uam_cpu *cpu) { return (z80_state *)cpu->isa_state; }

/* z80_lift.c */
uam_status z80_lift(uam_lift_ctx *lc);

#endif /* UAM_Z80_INTERNAL_H */
