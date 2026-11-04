/*
 * avr.h - Internal definitions shared by the 8-bit AVR lifter and system code.
 * Not part of the public interface.
 */
#ifndef UAM_AVR_INTERNAL_H
#define UAM_AVR_INTERNAL_H

#include "uam/isa/avr.h"

/* Register file: r0..r31 one byte each, then SREG one byte per flag (0 or 1, so a
 * condition is one TRUNC), SP, the RAMP registers and EIND. */
#define AVR_R(n)       ((uint32_t)(n))
enum { AVR_SREG_C = 0, AVR_SREG_Z, AVR_SREG_N, AVR_SREG_V, AVR_SREG_S, AVR_SREG_H, AVR_SREG_T,
       AVR_SREG_I };
#define AVR_FLAG(b)    (32u + (uint32_t)(b))      /* SREG bit b */
#define AVR_SP         40u                        /* 16 bits */
#define AVR_RAMPD      42u
#define AVR_RAMPX      43u
#define AVR_RAMPY      44u
#define AVR_RAMPZ      45u
#define AVR_EIND       46u
#define AVR_REGFILE_SIZE 48u

/* The core's I/O registers (I/O addresses; data address = I/O address + io_offset). */
enum {
    AVR_IO_CCP = 0x34, AVR_IO_RAMPD = 0x38, AVR_IO_RAMPX = 0x39, AVR_IO_RAMPY = 0x3A,
    AVR_IO_RAMPZ = 0x3B, AVR_IO_EIND = 0x3C, AVR_IO_SPL = 0x3D, AVR_IO_SPH = 0x3E,
    AVR_IO_SREG = 0x3F,
};
/* The interrupt controllers' registers (data addresses). */
enum {
    AVR_PMIC_STATUS = 0xA0, AVR_PMIC_INTPRI = 0xA1, AVR_PMIC_CTRL = 0xA2,           /* AVRxm */
    AVR_CPUINT_CTRLA = 0x110, AVR_CPUINT_STATUS = 0x111, AVR_CPUINT_LVL0PRI = 0x112,
    AVR_CPUINT_LVL1VEC = 0x113,                                                       /* AVRxt */
};

/* System register ids (SYSRD/SYSWR): a core register by data address, plus these. */
#define AVR_SYS_DATA(a)  ((uint32_t)(a))           /* a core register at data address a */
#define AVR_SYS_SET_I    0x10000u                  /* write: SREG.I (requests an exit) */

enum { AVR_HELPER_CORE_RD, AVR_HELPER_CORE_WR, AVR_HELPER_RETI, AVR_HELPER_SPM, AVR_HELPER_DES,
       AVR_HELPER__COUNT };

#define AVR_VECTORS 64u

/* ISA-private hart state. */
typedef struct avr_state {
    uam_avr_config cfg;
    uint64_t lines;              /* pending vectors (irq_input levels) */
    uint8_t  level[AVR_VECTORS]; /* AVRxm/AVRxt interrupt level per vector */
    uint8_t  status;             /* PMIC.STATUS / CPUINT.STATUS: levels running (bit = level) */
    uint8_t  pri;                /* PMIC.INTPRI / CPUINT.LVL0PRI */
    uint8_t  ctrl;               /* PMIC.CTRL / CPUINT.CTRLA */
    uint8_t  lvl1vec;            /* CPUINT.LVL1VEC */
    uint8_t  ccp;
    unsigned last_vector;        /* the vector irq_take entered (irq_lines) */
    bool     halted;             /* a bus fault the core has no answer for */
    uam_avr_irq_ack_fn ack;
    void              *ack_ctx;
    uam_avr_spm_fn     spm;
    void              *spm_ctx;
} avr_state;

static inline avr_state *avr_st(uam_cpu *cpu) { return (avr_state *)cpu->isa_state; }

/* Properties of a core family. */
static inline bool avr_regs_mapped(uam_avr_core c) { return c == UAM_AVR_E || c == UAM_AVR_EPLUS; }
static inline unsigned avr_io_offset(uam_avr_core c) { return avr_regs_mapped(c) ? 0x20u : 0u; }
static inline bool avr_levels(uam_avr_core c) { return c == UAM_AVR_XM || c == UAM_AVR_XT; }
static inline bool avr_pc22(const uam_avr_config *c) { return c->flash_size > 0x20000u; }

/* avr_lift.c */
uam_status avr_lift(uam_lift_ctx *lc);
/* Words of the instruction whose first word is w (2 for LDS/STS/JMP/CALL), on core c. */
unsigned   avr_insn_words(uint16_t w, uam_avr_core c);

/* avr_system.c */
extern const uam_helper_desc avr_helpers[AVR_HELPER__COUNT];
/* Is data address a one of the hart's own registers on this model? */
bool       avr_core_addr(const uam_avr_config *c, uint32_t a);

/* avr_des.c */
void avr_des_round(uint8_t regs[16], unsigned k, bool decrypt);

#endif /* UAM_AVR_INTERNAL_H */
