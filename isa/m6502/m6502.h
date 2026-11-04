/*
 * m6502.h - Internal definitions shared by the 6502 lifter and system code.
 * Not part of the public interface.
 */
#ifndef UAM_M6502_INTERNAL_H
#define UAM_M6502_INTERNAL_H

#include "uam/isa/m6502.h"

/* Register file. C, Z, V and N are one byte each (0 or 1, so a condition is one TRUNC);
 * PX holds P's I and D bits in their places, which only the system code writes (an I
 * or D change has effects beyond the bit). B and bit 5 exist only on the stack. */
#define M6502_A  0u
#define M6502_X  1u
#define M6502_Y  2u
#define M6502_S  3u
#define M6502_C  4u
#define M6502_Z  5u
#define M6502_V  6u
#define M6502_N  7u
#define M6502_PX 8u
#define M6502_REGFILE_SIZE 16u

#define PX_MASK (UAM_M6502_P_I | UAM_M6502_P_D)

/* cpu->priv, the state translated code depends on: D, on a model with decimal mode. */
#define M6502_PRIV_D 1u

/* System register ids (SYSRD/SYSWR). */
enum {
    M6502_SYS_P,       /* write: I and D from the byte, I seen by the interrupt poll one
                        * instruction late (CLI, SEI, PLP); read: P with B and bit 5 set */
    M6502_SYS_P_NOW,   /* write: I and D, at once (RTI, BRK) */
    M6502_SYS_STOP,    /* write: stop until reset (0: STP, 1: an NMOS JAM) */
};

enum {
    M6502_HELPER_ADC_NMOS, M6502_HELPER_ADC_CMOS, M6502_HELPER_SBC_NMOS, M6502_HELPER_SBC_CMOS,
    M6502_HELPER_ARR_DEC, M6502_HELPER__COUNT
};

/* A decimal helper's result: the byte, then C, Z, N and V. */
#define M6502_DEC_C 8u
#define M6502_DEC_Z 9u
#define M6502_DEC_N 10u
#define M6502_DEC_V 11u

/* ISA-private hart state. */
typedef struct m6502_state {
    uam_m6502_config cfg;
    uint32_t lines;        /* IRQ input levels (lines 0 .. UAM_M6502_LINE_NMI - 1) */
    bool     nmi_level;    /* the NMI line's level, for the edge */
    bool     nmi_pending;
    bool     defer_irq;    /* I was just cleared: the next poll sees it still set */
    bool     poll_i0;      /* I was just set: the next poll sees it still clear */
    uint8_t  stopped;      /* 0, or 1 + the M6502_SYS_STOP value */
    bool     halted;       /* a bus fault the core has no answer for */
    unsigned last_line;    /* what irq_take entered (irq_lines) */
} m6502_state;

static inline m6502_state *m6502_st(uam_cpu *cpu) { return (m6502_state *)cpu->isa_state; }

/* m6502_lift.c */
uam_status m6502_lift(uam_lift_ctx *lc);
/* Bytes of the instruction with opcode op on model c (1..3). */
unsigned   m6502_insn_len(const uam_m6502_config *c, uint8_t op);

/* m6502_system.c */
extern const uam_helper_desc m6502_helpers[M6502_HELPER__COUNT];
/* The decimal arithmetic (packed as M6502_DEC_*), shared with the tests' reference. */
unsigned   m6502_adc_dec(unsigned a, unsigned m, unsigned c, bool cmos);
unsigned   m6502_sbc_dec(unsigned a, unsigned m, unsigned c, bool cmos);
unsigned   m6502_arr_dec(unsigned t, unsigned c);

#endif /* UAM_M6502_INTERNAL_H */
