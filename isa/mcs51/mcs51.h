/*
 * mcs51.h - Internal definitions shared by the 8051 lifter and system code.
 * Not part of the public interface.
 */
#ifndef UAM_MCS51_INTERNAL_H
#define UAM_MCS51_INTERNAL_H

#include "uam/isa/mcs51.h"

/* Register file: the internal RAM (256 bytes; a 128-byte model uses the lower half),
 * then the core SFRs. PSW is kept as CY, AC and OV one byte each (0 or 1, so a condition
 * is one TRUNC), the rest of its bits (F0, RS1, RS0, F1) in their places in one byte;
 * P is ACC's parity, computed when PSW is read. */
#define MCS51_IRAM(a)   ((uint32_t)(a))
#define MCS51_A         256u
#define MCS51_B         257u
#define MCS51_CY        258u
#define MCS51_AC        259u
#define MCS51_OV        260u
#define MCS51_PSWX      261u   /* F0 (bit 5), RS1 (4), RS0 (3), F1 (1) */
#define MCS51_SP        262u
#define MCS51_DPTR(n)   (264u + 2u * (n))   /* 16 bits, DPL first */
#define MCS51_SFRPAGE   268u
#define MCS51_REGFILE_SIZE 272u

#define PSWX_MASK 0x3Au

/* cpu->priv, the state translated code depends on: the register bank (PSW.RS) and the
 * data pointer selected. */
#define MCS51_PRIV(rs, dps) ((unsigned)(rs) | (unsigned)(dps) << 2)
static inline unsigned mcs51_priv_bank(unsigned priv) { return priv & 3u; }
static inline unsigned mcs51_priv_dps(unsigned priv) { return (priv >> 2) & 1u; }

/* System register ids (SYSRD/SYSWR) are SFR addresses: PSW (the bits of MCS51_PSWX; a
 * write may switch the register bank), IE, IP, PCON and the model's IPH, EIEn, EIPn,
 * EIPHn and DPS. */

enum { MCS51_HELPER_IRAM_RD, MCS51_HELPER_IRAM_WR, MCS51_HELPER_RETI, MCS51_HELPER__COUNT };

#define MCS51_PAGE_STACK 4u

/* ISA-private hart state. */
typedef struct mcs51_state {
    uam_mcs51_config cfg;
    uint32_t lines;            /* pending sources (irq_input levels) */
    uint8_t  ie, ip, iph, pcon, dps;
    uint8_t  eie[3], eip[3], eiph[3];
    uint8_t  in_progress;      /* priority levels being serviced (bit = level) */
    unsigned last_source;      /* the source irq_take entered (irq_lines) */
    uint8_t  page_stack[MCS51_PAGE_STACK];
    uint8_t  page_depth;
    bool     autopage;
    uint8_t  irq_page[UAM_MCS51_SOURCES];
    uint16_t fetch_bank, movc_bank;
    bool     halted;           /* a bus fault the core has no answer for */
    uam_mcs51_irq_ack_fn ack;
    void                *ack_ctx;
} mcs51_state;

static inline mcs51_state *mcs51_st(uam_cpu *cpu) { return (mcs51_state *)cpu->isa_state; }

/* mcs51_lift.c */
uam_status mcs51_lift(uam_lift_ctx *lc);
/* Bytes of the instruction with opcode op (1..3). */
unsigned   mcs51_insn_len(uint8_t op);

/* mcs51_system.c */
extern const uam_helper_desc mcs51_helpers[MCS51_HELPER__COUNT];
/* Is SFR a (0x80-0xFF) one of the registers SYSRD/SYSWR serve on this model? */
bool       mcs51_sys_sfr(const uam_mcs51_config *c, uint32_t a);

#endif /* UAM_MCS51_INTERNAL_H */
