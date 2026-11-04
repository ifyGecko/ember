/*
 * pic.h - Internal definitions shared by the 8-bit PIC lifter and system code.
 * Not part of the public interface.
 */
#ifndef UAM_PIC_INTERNAL_H
#define UAM_PIC_INTERNAL_H

#include "uam/isa/pic.h"

/* Register file. STATUS's arithmetic flags are one byte each (0 or 1, so a condition is
 * one TRUNC); PIC_STATUS holds its other bits (PA/IRP/RP, TO, PD) with bits 2-0 zero. */
#define PIC_W        0u
#define PIC_C        1u
#define PIC_DC       2u
#define PIC_Z        3u
#define PIC_OV       4u      /* PIC18 */
#define PIC_N        5u      /* PIC18 */
#define PIC_STATUS   6u
#define PIC_BSR      7u
#define PIC_PCLATH   8u
#define PIC_PCLATU   9u
#define PIC_INTCON   10u
#define PIC_RCON     11u     /* PIC18 */
#define PIC_FSR(n)   (12u + 2u * (n))   /* 16 bits; baseline/mid: FSR in the low byte */
#define PIC_PROD     18u     /* 16 bits */
#define PIC_TABLAT   20u
#define PIC_TBLPTR   24u     /* 32 bits, 22 used */
#define PIC_SHAD     28u     /* enhanced shadows: STATUS, WREG, BSR, PCLATH, FSR0L/H, FSR1L/H */
#define PIC_FAST     36u     /* PIC18 fast register stack: WS, STATUSS, BSRS */
#define PIC_REGFILE_SIZE 40u

/* STATUS bits */
enum { PIC_ST_C = 0, PIC_ST_DC = 1, PIC_ST_Z = 2, PIC_ST_OV = 3, PIC_ST_N = 4 };
#define PIC_ST_PD 0x08u     /* not PIC18 */
#define PIC_ST_TO 0x10u
/* RCON (PIC18) */
#define PIC_RCON_IPEN 0x80u
#define PIC_RCON_RI   0x10u
#define PIC_RCON_TO   0x08u
#define PIC_RCON_PD   0x04u
/* INTCON */
#define PIC_INTCON_GIE  0x80u   /* PIC18 with IPEN: GIEH */
#define PIC_INTCON_PEIE 0x40u   /* PIC18 with IPEN: GIEL */

/* The core's registers in the data space, by kind (pic_core_reg). */
enum pic_creg_kind {
    CR_NONE, CR_INDF, CR_PCL, CR_STATUS, CR_FSRL, CR_FSRH, CR_BSR, CR_WREG, CR_PCLATH,
    CR_PCLATU, CR_INTCON, CR_STKPTR, CR_TOSL, CR_TOSH, CR_TOSU, CR_TBLPTRL, CR_TBLPTRH,
    CR_TBLPTRU, CR_TABLAT, CR_PRODL, CR_PRODH, CR_RCON, CR_SHAD,
};
/* INDF access modes */
enum { IND_PLAIN, IND_POSTINC, IND_POSTDEC, IND_PREINC, IND_PLUSW };
typedef struct pic_creg {
    uint8_t kind;   /* enum pic_creg_kind */
    uint8_t n;      /* FSR number (INDF, FSRL/H) or shadow index */
    uint8_t mode;   /* INDF: IND_* */
} pic_creg;

/* System register ids (SYSRD/SYSWR): a core register by data address. */
#define PIC_SYS_DATA(a)  ((uint32_t)(a))

/* Helper flags in the address argument of CORE_RD/CORE_WR. */
#define PIC_VIA_FSR 0x10000u     /* the access is indirect: INDF-class registers read 0 */

enum { PIC_HELPER_CORE_RD, PIC_HELPER_CORE_WR, PIC_HELPER_PUSH, PIC_HELPER_POP,
       PIC_HELPER_RETFIE, PIC_HELPER_RESET, PIC_HELPER_TBLWT, PIC_HELPER__COUNT };

#define PIC_STACK_MAX 32u
/* PUSH/POP helper results with the core reset (the low bit of a real pc is 0) */
#define PIC_RESET_MARK 1u

/* ISA-private hart state. */
typedef struct pic_state {
    uam_pic_config cfg;
    uint32_t stack[PIC_STACK_MAX];  /* return addresses as bus addresses (pc values) */
    uint8_t  stkptr;     /* baseline/mid: entries in use (circular); enhanced: STKPTR (0x1F
                          * empty); PIC18: STKPTR with STKFUL/STKUNF */
    uint8_t  lines;      /* irq_input levels */
    uint8_t  last_line;  /* the line irq_take entered for (irq_lines) */
    uint8_t  cause;      /* uam_pic_reset_cause of the last reset */
    uint8_t  pending_cause; /* a reset the core asked for, applied by the next reset */
    bool     halted;     /* a bus fault the core has no answer for */
    uam_pic_tblwt_fn tblwt;
    void            *tblwt_ctx;
    uam_pic_reset_fn reset_hook;
    void            *reset_ctx;
} pic_state;

static inline pic_state *pic_st(uam_cpu *cpu) { return (pic_state *)cpu->isa_state; }

/* Properties of a model. */
static inline bool pic_14bit(uam_pic_core c) { return c == UAM_PIC_MID || c == UAM_PIC_ENHANCED; }
static inline unsigned pic_stack_depth(uam_pic_core c)
{
    return c == UAM_PIC_BASELINE ? 2 : c == UAM_PIC_MID ? 8 : c == UAM_PIC_ENHANCED ? 16 : 31;
}
/* Byte addresses the pc can hold: program memory rounded up to a power of two. */
static inline uint32_t pic_pc_mask(const uam_pic_config *c)
{
    uint32_t words = 1;
    while (words < c->prog_words)
        words <<= 1;
    return (2 * words - 1) & ~1u;
}
/* The reset vector as a pc: the last word on baseline, 0 otherwise. */
static inline uint32_t pic_reset_pc(const uam_pic_config *c)
{
    return c->core == UAM_PIC_BASELINE ? pic_pc_mask(c) : 0;
}

/* pic_system.c */
extern const uam_helper_desc pic_helpers[PIC_HELPER__COUNT];
/* The core register at data address a on this model, or kind CR_NONE. */
pic_creg   pic_core_reg(const uam_pic_config *c, uint32_t a);
/* Mask of the bits of the PIC_STATUS byte a STATUS write changes. */
uint8_t    pic_status_wmask(uam_pic_core c);

/* pic_lift.c */
uam_status pic_lift(uam_lift_ctx *lc);

#endif /* UAM_PIC_INTERNAL_H */
