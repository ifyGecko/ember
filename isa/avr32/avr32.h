/*
 * avr32.h - Internal definitions shared by the AVR32 lifter and system code.
 * Not part of the public interface.
 */
#ifndef UAM_AVR32_INTERNAL_H
#define UAM_AVR32_INTERNAL_H

#include "uam/isa/avr32.h"
#include "uam/sched.h"

/*
 * Register file: R0..R15 (4 bytes each; R13 is the SP of the current mode, R14
 * LR; the R15 slot is never used: the PC lives in uam_cpu and the lifter folds
 * reads of R15 to the instruction's address), then the status flags the
 * instructions read and write, one byte each (0 or 1). The rest of SR (mode,
 * masks, debug) is in avr32_state.sr_hi.
 */
#define A32_REG_OFF(i)    ((uint32_t)(i) * 4u)
#define A32_F_C           64u
#define A32_F_Z           65u
#define A32_F_N           66u
#define A32_F_V           67u
#define A32_F_Q           68u
#define A32_F_L           69u
#define A32_F_T           70u
#define A32_REGFILE_SIZE  72u

enum { A32_SP = 13, A32_LR = 14, A32_PC = 15 };

/* SR bits. */
#define SR_C    (1u << 0)
#define SR_Z    (1u << 1)
#define SR_N    (1u << 2)
#define SR_V    (1u << 3)
#define SR_Q    (1u << 4)
#define SR_L    (1u << 5)
#define SR_T    (1u << 14)
#define SR_R    (1u << 15)
#define SR_GM   (1u << 16)
#define SR_I0M  (1u << 17)
#define SR_I1M  (1u << 18)
#define SR_I2M  (1u << 19)
#define SR_I3M  (1u << 20)
#define SR_EM   (1u << 21)
#define SR_M_SHIFT 22
#define SR_M    (7u << SR_M_SHIFT)
#define SR_D    (1u << 26)
#define SR_DM   (1u << 27)
#define SR_J    (1u << 28)
#define SR_H    (1u << 29)
#define SR_SS   (1u << 31)
#define SR_FLAGS (SR_C | SR_Z | SR_N | SR_V | SR_Q | SR_L | SR_T)
/* The upper-halfword bits that exist (J, H, SS: no Java extension, no secure state). */
#define SR_HI_MASK (SR_GM | SR_I0M | SR_I1M | SR_I2M | SR_I3M | SR_EM | SR_M | SR_D | SR_DM)

/* Execution modes (SR[M2:M0]). */
enum {
    A32_MODE_APP = 0, A32_MODE_SUP = 1, A32_MODE_INT0 = 2, A32_MODE_INT1 = 3,
    A32_MODE_INT2 = 4, A32_MODE_INT3 = 5, A32_MODE_EX = 6, A32_MODE_NMI = 7,
};

/* cpu->priv: application (unprivileged) or one of the privileged modes. */
enum { A32_PRIV_APP = 0, A32_PRIV_SYS = 1 };

/* Event handler offsets from EVBA (Table 8-1). */
enum {
    EV_UNRECOVERABLE = 0x00, EV_TLB_MULTI = 0x04, EV_BUS_DATA = 0x08, EV_BUS_INSN = 0x0C,
    EV_NMI = 0x10, EV_INSN_ADDR = 0x14, EV_ITLB_PROT = 0x18, EV_BREAKPOINT = 0x1C,
    EV_ILLEGAL = 0x20, EV_UNIMPL = 0x24, EV_PRIV = 0x28, EV_FP = 0x2C, EV_COP_ABSENT = 0x30,
    EV_DATA_ADDR_R = 0x34, EV_DATA_ADDR_W = 0x38, EV_DTLB_PROT_R = 0x3C, EV_DTLB_PROT_W = 0x40,
    EV_DTLB_MOD = 0x44, EV_ITLB_MISS = 0x50, EV_DTLB_MISS_R = 0x60, EV_DTLB_MISS_W = 0x70,
    EV_SCALL = 0x100,
};

/* TRAP causes the lifter raises (UAM_TRAP_EXCEPTION): an event offset, or one of these. */
#define A32_TRAP_SCALL      0x1000u
#define A32_TRAP_BREAKPOINT 0x1001u

/* Translation fault sub-causes (uam_xlate.fault). */
enum {
    A32_XF_MISS = 1,      /* no TLB entry / no MPU region */
    A32_XF_PROT,          /* access permissions; also privileged segment from application */
    A32_XF_MOD,           /* store to a clean page (DTLB Modified) */
    A32_XF_MULTI,         /* several entries or regions match */
};

/* System register numbers (address >> 2). */
enum {
    SYS_SR = 0, SYS_EVBA = 1, SYS_ACBA = 2, SYS_CPUCR = 3, SYS_ECR = 4,
    SYS_RSR_SUP = 5,   /* .. RSR_NMI = 11: by mode - 1 */
    SYS_RSR_DBG = 12,
    SYS_RAR_SUP = 13,  /* .. RAR_NMI = 19 */
    SYS_RAR_DBG = 20,
    SYS_JECR = 21, SYS_JOSP = 22, SYS_JAVA_LV0 = 23, SYS_JTBA = 31, SYS_JBCR = 32,
    SYS_CONFIG0 = 64, SYS_CONFIG1 = 65, SYS_COUNT = 66, SYS_COMPARE = 67,
    SYS_TLBEHI = 68, SYS_TLBELO = 69, SYS_PTBR = 70, SYS_TLBEAR = 71, SYS_MMUCR = 72,
    SYS_TLBARLO = 73, SYS_TLBARHI = 74, SYS_PCCNT = 75, SYS_PCNT0 = 76, SYS_PCNT1 = 77,
    SYS_PCCR = 78, SYS_BEAR = 79,
    SYS_MPUAR0 = 80, SYS_MPUPSR0 = 88, SYS_MPUCRA = 96, SYS_MPUCRB = 97, SYS_MPUBRA = 98,
    SYS_MPUBRB = 99, SYS_MPUAPRA = 100, SYS_MPUAPRB = 101, SYS_MPUCR = 102,
};

/* Helper ids. */
enum {
    A32_H_RETE = 0,     /* () -> new pc */
    A32_H_RETS,         /* () -> new pc */
    A32_H_RETD,         /* () -> new pc */
    A32_H_SETSR,        /* (bit, value): csrf / ssrf on SR[31:16] */
    A32_H_TLB,          /* (op): 0 tlbr, 1 tlbw, 2 tlbs */
    A32_H_SLEEP,        /* (op8): SR[GM] when bit 7 */
    A32_H_USERREG_RD,   /* (reg) -> application-context register (ldmts/stmts) */
    A32_H_USERREG_WR,   /* (reg, value) */
    A32_H_DSP,          /* (op, x, y) -> result | flags << 32 (avr32_dsp.c) */
    A32_H_SIMD,         /* (insn, x, y) -> result | flags << 32 */
    A32_H__COUNT
};

/* DSP helper operation codes (avr32_dsp.c). */
enum {
    DSP_SATADD_H, DSP_SATADD_W, DSP_SATSUB_H, DSP_SATSUB_W, DSP_SATRNDS, DSP_SATRNDU,
    DSP_SATS, DSP_SATU, DSP_MULSATHH_H, DSP_MULSATHH_W, DSP_MULSATRNDHH_H, DSP_MULSATRNDWH_W,
    DSP_MULSATWH_W, DSP_MACSATHH_W,
};

#define A32_TLB_MAX 64u
#define A32_MPU_REGIONS 8u

typedef struct avr32_tlb_entry {
    uint32_t hi;   /* TLBEHI layout: VPN[31:10] V[9] ASID[7:0] */
    uint32_t lo;   /* TLBELO layout: PFN[31:10] C G B AP SZ D W */
    bool     a;    /* the Accessed bit (set by tlbw, cleared by a match) */
} avr32_tlb_entry;

typedef struct avr32_state {
    uam_avr32_config cfg;          /* chosen by the board; survives reset */
    uam_timer   timer;             /* COUNT == COMPARE, per hart; survives reset */
    uam_signal  compare_line;      /* the compare interrupt request (output) */
    uam_avr32_autovector_fn autovector;
    void       *autovector_ctx;
    uint32_t    lines;             /* INT0..INT3, NMI input levels (bit = line) */

    uint32_t    sr_hi;             /* SR without the flags kept in the register file */
    uint32_t    sp_app, sp_sys;    /* the SP of the mode not current (the current one is R13) */
    uint32_t    shadow[6];         /* B: the other bank of R8-R12, LR (INT3's when not in INT3) */
    uint32_t    rsr[8], rar[8];    /* B: by mode (index 1..7); [0] unused */
    uint32_t    rsr_dbg, rar_dbg;
    uint32_t    evba, acba, cpucr, ecr, bear;
    uint32_t    java[12];          /* JECR, JOSP, JAVA_LV0..7, JTBA, JBCR: storage only */
    uint32_t    pccnt, pcnt[2], pccr;
    uint64_t    count_bias;        /* COUNT = sched_now + count_bias */
    uint32_t    compare;
    /* MMU (B) */
    uint32_t    tlbehi, tlbelo, ptbr, tlbear, mmucr;
    avr32_tlb_entry itlb[A32_TLB_MAX], dtlb[A32_TLB_MAX];
    /* MPU (A) */
    uint32_t    mpuar[A32_MPU_REGIONS], mpupsr[A32_MPU_REGIONS];
    uint32_t    mpucra, mpucrb, mpubra, mpubrb, mpuapra, mpuaprb, mpucr;
} avr32_state;

static inline avr32_state *a32_st(uam_cpu *cpu) { return (avr32_state *)cpu->isa_state; }
static inline unsigned a32_mode(const avr32_state *s) { return (s->sr_hi >> SR_M_SHIFT) & 7; }

/* avr32_lift.c */
uam_status avr32_lift(uam_lift_ctx *lc);

/* avr32_system.c */
extern const uam_helper_desc avr32_helpers[A32_H__COUNT];
uint32_t   avr32_sr_read(uam_cpu *cpu);
void       avr32_sr_write(uam_cpu *cpu, uint32_t sr);
uam_status avr32_mem_rw(uam_cpu *cpu, uint32_t va, unsigned size, bool store, uint64_t *val);

/* avr32_dsp.c */
uint64_t avr32_dsp(avr32_state *s, unsigned op, uint32_t x, uint32_t y);
uint64_t avr32_simd(avr32_state *s, uint32_t insn, uint32_t x, uint32_t y);

#endif /* UAM_AVR32_INTERNAL_H */
