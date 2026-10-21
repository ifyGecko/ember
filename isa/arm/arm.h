/*
 * arm.h - Internal definitions shared by the ARM lifters (A32, T32) and the
 * system code. Not part of the public interface.
 */
#ifndef UAM_ARM_INTERNAL_H
#define UAM_ARM_INTERNAL_H

#include "uam/isa/arm.h"
#include "uam/sched.h"

/*
 * Register file: the current mode's view of r0-r14 (the system code swaps banked
 * registers in and out on mode changes), then the flags, each in its own byte
 * holding 0 or 1, and GE[3:0]. The pc lives in uam_cpu (bit 0 = Thumb state);
 * slot 15 is not used.
 */
#define ARM_REG_OFF(i)   ((uint32_t)(i) * 4u)
#define ARM_N_OFF        64u
#define ARM_Z_OFF        65u
#define ARM_C_OFF        66u
#define ARM_V_OFF        67u
#define ARM_Q_OFF        68u
#define ARM_GE_OFF       69u   /* GE[3:0] in bits 3:0 */
#define ARM_REGFILE_SIZE 72u

#define ARM_SP 13
#define ARM_LR 14
#define ARM_PC 15

/* Processor modes (CPSR.M). */
enum {
    ARM_MODE_USR = 0x10, ARM_MODE_FIQ = 0x11, ARM_MODE_IRQ = 0x12, ARM_MODE_SVC = 0x13,
    ARM_MODE_ABT = 0x17, ARM_MODE_UND = 0x1B, ARM_MODE_SYS = 0x1F,
};

/* Register banks: usr/sys share one. */
enum { ARM_BANK_USR, ARM_BANK_FIQ, ARM_BANK_IRQ, ARM_BANK_SVC, ARM_BANK_ABT, ARM_BANK_UND,
       ARM_BANK__COUNT };

/* CPSR bits. */
#define CPSR_M_MASK  0x1Fu
#define CPSR_T       (1u << 5)
#define CPSR_F       (1u << 6)
#define CPSR_I       (1u << 7)
#define CPSR_A       (1u << 8)
#define CPSR_E       (1u << 9)
#define CPSR_GE_SHIFT 16
#define CPSR_Q       (1u << 27)
#define CPSR_V       (1u << 28)
#define CPSR_C       (1u << 29)
#define CPSR_Z       (1u << 30)
#define CPSR_N       (1u << 31)
#define CPSR_IT_LO_SHIFT 25    /* IT[1:0] */
#define CPSR_IT_HI_SHIFT 10    /* IT[7:2] */
#define CPSR_IT_MASK ((3u << CPSR_IT_LO_SHIFT) | (0x3Fu << CPSR_IT_HI_SHIFT))

/*
 * cpu->priv tags translated code with everything the lifted code depends on
 * besides the pc (whose bit 0 is the instruction set): bit 0 is set in User mode
 * (translation permissions), bits 15:8 hold the IT state at the start of the block.
 * The IT state is non-zero only when a block resumes inside an IT block, after an
 * exception return into one.
 */
#define ARM_PRIV_USER      1u
#define ARM_PRIV_IT_SHIFT  8
#define ARM_PRIV_IT(p)     (((p) >> ARM_PRIV_IT_SHIFT) & 0xFFu)

/* System register ids (SYSRD/SYSWR aux). Coprocessor registers are
 * ARM_SR_CP(cp, opc1, CRn, CRm, opc2) for MRC/MCR and ARM_SR_CP64(cp, opc1, CRm) for
 * MRRC/MCRR (64-bit). The rest are pseudo registers for the lifters. */
#define ARM_SR_CP(cp, o1, n, m, o2) \
    (((uint32_t)(cp) << 16) | ((uint32_t)(o1) << 12) | ((uint32_t)(n) << 8) | ((uint32_t)(m) << 4) \
     | (uint32_t)(o2))
#define ARM_SR_CP64(cp, o1, m)  (0x00100000u | ARM_SR_CP(cp, o1, 0, m, 0))
#define ARM_SR_IS_CP(id)        ((id) < 0x00200000u)
enum {
    ARM_SR_CPSR    = 0x00200000u, /* read: the CPSR's mode, A, I, F and E bits (MRS adds the
                                   * flags); write (| field mask << 0): CPSR_c/x/s fields */
    ARM_SR_SPSR    = 0x00210000u, /* read: SPSR of the current mode; write: | field mask */
    ARM_SR_CPS     = 0x00220000u, /* write: CPS (bits: imod << 6 | M << 5 | A I F << 2..0 |
                                   * mode << 8) */
    ARM_SR_USRREG  = 0x00230000u, /* | n: User-mode r8-r14 (LDM/STM with ^) */
    ARM_SR_BANKSP  = 0x00240000u, /* | mode: the banked SP of a mode (SRS) */
};

/* Exception vectors (offsets) and the cause carried by TRAP / uam_exception.cause. */
enum {
    ARM_EXC_UNDEF = 1,    /* Undefined Instruction; tval = the instruction */
    ARM_EXC_SVC,          /* Supervisor Call */
    ARM_EXC_PABT,         /* Prefetch Abort (BKPT: debug event) */
    ARM_EXC_DABT,
};

/* Fault status, as carried in uam_xlate.fault: FS[4:0] (short-descriptor encoding)
 * plus the domain at bits 11:8. */
enum {
    ARM_FS_ALIGN      = 0x01,
    ARM_FS_DEBUG      = 0x02,
    ARM_FS_XLATE_SEC  = 0x05,
    ARM_FS_XLATE_PAGE = 0x07,
    ARM_FS_EXT        = 0x08,   /* synchronous external abort */
    ARM_FS_DOM_SEC    = 0x09,
    ARM_FS_DOM_PAGE   = 0x0B,
    ARM_FS_PERM_SEC   = 0x0D,
    ARM_FS_PERM_PAGE  = 0x0F,
};
#define ARM_XF(fs, dom) ((uint32_t)(fs) | ((uint32_t)(dom) << 8))

/* Helper ids. */
enum {
    ARM_HELPER_ERET = 0,   /* CPSR = SPSR, return to a (from SUBS pc / LDM ^ / RFE) */
    ARM_HELPER_SET_IT,     /* set the IT state the next block starts with */
    ARM_HELPER_CLREX,      /* clear the exclusive monitor */
    ARM_HELPER__COUNT
};

/* SCTLR bits. */
#define SCTLR_M   (1u << 0)
#define SCTLR_A   (1u << 1)
#define SCTLR_C   (1u << 2)
#define SCTLR_Z   (1u << 11)
#define SCTLR_I   (1u << 12)
#define SCTLR_V   (1u << 13)
#define SCTLR_AFE (1u << 29)
#define SCTLR_TE  (1u << 30)

/* CP15 and banked state cleared by a reset. */
typedef struct arm_sys {
    uint32_t cpsr;                       /* mode, A, I, F (flags live in the register file,
                                          * T in the pc, IT in cpu->priv) */
    uint32_t spsr[ARM_BANK__COUNT];      /* [ARM_BANK_USR] unused */
    uint32_t r13[ARM_BANK__COUNT], r14[ARM_BANK__COUNT];
    uint32_t usr_r8[5], fiq_r8[5];       /* r8-r12: whichever set is not in the regfile */
    uint32_t sctlr, actlr, cpacr;
    uint32_t ttbr0, ttbr1, ttbcr, dacr;
    uint32_t dfsr, ifsr, dfar, ifar, adfsr, aifsr;
    uint32_t vbar, contextidr, tpidrurw, tpidruro, tpidrprw;
    uint32_t prrr, nmrr, par, csselr;
    /* generic timer */
    uint32_t cntfrq, cntkctl;
    uint32_t cnt_ctl[2];                 /* [UAM_ARM_TIMER_PHYS / _VIRT] */
    uint64_t cnt_cval[2];
    uint64_t cntvoff;
    bool     timer_out[2];               /* last output level reported to the sink */
} arm_sys;

typedef struct arm_state {
    uam_arm_config     cfg;       /* CPU identity, chosen by the board; survives reset */
    uam_timer          timer[2];  /* generic timer deadlines; survive reset */
    uam_arm_timer_sink sink;      /* timer outputs; survives reset */
    void              *sink_ctx;
    bool               irq_line, fiq_line;
    uint64_t           last_irq;  /* the line irq_take last entered (irq_lines) */
    arm_sys            s;
} arm_state;

static inline arm_state *arm_st(uam_cpu *cpu) { return (arm_state *)cpu->isa_state; }

/* ---- lifter context (arm_lift.c, arm_thumb.c) ---------------------------- */

typedef enum { ARM_SEQ, ARM_DONE } arm_lift_result;

typedef struct arm_lift {
    uam_lift_ctx *lc;
    uam_builder  *b;
    uint32_t      pc;      /* address of the current instruction (bit 0 clear) */
    bool          thumb;
    unsigned      len;     /* 2 or 4 */
    uint32_t      raw;     /* the instruction (T32: first halfword in bits 31:16) */
    uam_ref       guard;   /* condition of the current instruction, NONE if always */
    bool          user;    /* lifted for User mode */
    uint8_t       it;      /* IT state of the instruction being lifted (Thumb) */
    uint8_t       it_next; /* IT state after it */
    bool          end;     /* end the block after this instruction */
} arm_lift;

/* arm_lift.c: the ARM decoder and the emitters both decoders share. */
uam_status arm_lift_block(uam_lift_ctx *lc);
arm_lift_result arm_lift_a32(arm_lift *L, uint32_t i);

uam_ref  arm_imm(arm_lift *L, uint32_t v);
uam_ref  arm_bin(arm_lift *L, uam_opcode op, uam_ref a, uam_ref b);
uam_ref  arm_reg(arm_lift *L, unsigned r);          /* r15 reads as pc+8 / pc+4 */
uam_ref  arm_reg_align(arm_lift *L, unsigned r);    /* r15 reads as Align(pc+N, 4) */
void     arm_put(arm_lift *L, unsigned r, uam_ref v); /* r0-r14, guarded */
uam_ref  arm_flag(arm_lift *L, uint32_t off);       /* 1-bit */
void     arm_put_flag(arm_lift *L, uint32_t off, uam_ref v1);
uam_ref  arm_cond(arm_lift *L, unsigned cond);      /* NONE for AL */
void     arm_trap(arm_lift *L, uint32_t cause, uint32_t tval);
arm_lift_result arm_undef(arm_lift *L);
uint32_t arm_next_pc(const arm_lift *L);            /* with the Thumb bit */

/* Shifter operands: value and carry out (carry NONE when the shift leaves C alone). */
typedef struct arm_shres { uam_ref v, c; } arm_shres;
arm_shres arm_shift_imm(arm_lift *L, uam_ref x, unsigned type, unsigned amt);
arm_shres arm_shift_reg(arm_lift *L, uam_ref x, unsigned type, uam_ref amt);

/* Data processing (ARM opcode numbering; ORN = 16 for Thumb). With rd = 15 the
 * result is a branch (ARM: interworking; Thumb: within Thumb), or with s an
 * exception return. Returns ARM_DONE if it ended the block. */
enum { DP_AND, DP_EOR, DP_SUB, DP_RSB, DP_ADD, DP_ADC, DP_SBC, DP_RSC, DP_TST, DP_TEQ,
       DP_CMP, DP_CMN, DP_ORR, DP_MOV, DP_BIC, DP_MVN, DP_ORN };
arm_lift_result arm_dp(arm_lift *L, unsigned op, bool s, unsigned rd, uam_ref a, arm_shres b);

/* Branches. */
arm_lift_result arm_branch(arm_lift *L, uam_ref target, uam_exit_kind kind); /* exits to target as is */
arm_lift_result arm_bx(arm_lift *L, uam_ref addr, uam_exit_kind kind);       /* BXWritePC */
uam_ref  arm_bx_target(arm_lift *L, uam_ref addr);

/* Loads and stores. size 1/2/4; sign for LDRSB/LDRSH. */
arm_lift_result arm_ldr(arm_lift *L, unsigned rt, uam_ref addr, unsigned size, bool sign,
                        int wb_rn, uam_ref wb_val);
arm_lift_result arm_str(arm_lift *L, unsigned rt, uam_ref addr, unsigned size,
                        int wb_rn, uam_ref wb_val);
arm_lift_result arm_ldrd(arm_lift *L, unsigned rt, unsigned rt2, uam_ref addr,
                         int wb_rn, uam_ref wb_val);
arm_lift_result arm_strd(arm_lift *L, unsigned rt, unsigned rt2, uam_ref addr,
                         int wb_rn, uam_ref wb_val);
/* LDM/STM: increment/before, writeback, user bank (^). */
arm_lift_result arm_ldm(arm_lift *L, unsigned rn, uint32_t list, bool inc, bool before,
                        bool wb, bool user);
arm_lift_result arm_stm(arm_lift *L, unsigned rn, uint32_t list, bool inc, bool before,
                        bool wb, bool user);
arm_lift_result arm_ldrex(arm_lift *L, unsigned rt, unsigned rt2, uam_ref addr, unsigned size);
arm_lift_result arm_strex(arm_lift *L, unsigned rd, unsigned rt, unsigned rt2, uam_ref addr,
                          unsigned size);

/* Media and multiply groups shared by both decoders. */
void     arm_extend(arm_lift *L, unsigned rd, unsigned rn, uam_ref x, unsigned rot,
                    bool sign, unsigned from);         /* SXTB/UXTH/... (+add if rn != 15) */
void     arm_rev(arm_lift *L, unsigned rd, uam_ref x, unsigned kind); /* 0 REV 1 REV16 2 REVSH 3 RBIT */
void     arm_bfx(arm_lift *L, unsigned rd, uam_ref x, unsigned lsb, unsigned width, bool sign);
void     arm_bfi(arm_lift *L, unsigned rd, uam_ref src, unsigned lsb, unsigned msb);
void     arm_sat(arm_lift *L, unsigned rd, uam_ref x, unsigned bits, bool usat);
void     arm_qaddsub(arm_lift *L, unsigned rd, uam_ref a, uam_ref b, bool sub, bool dbl);
void     arm_mul(arm_lift *L, unsigned rd, uam_ref a, uam_ref b, int acc_reg, bool sub, bool s);
void     arm_mull(arm_lift *L, unsigned rdlo, unsigned rdhi, uam_ref a, uam_ref b, bool sign,
                  bool acc, bool s);
void     arm_umaal(arm_lift *L, unsigned rdlo, unsigned rdhi, uam_ref a, uam_ref b);
void     arm_mulxy(arm_lift *L, unsigned rd, uam_ref a, uam_ref b, bool nhi, bool mhi,
                   int acc_reg);                       /* SMULxy / SMLAxy */
void     arm_mulwy(arm_lift *L, unsigned rd, uam_ref a, uam_ref b, bool mhi, int acc_reg);
void     arm_mmul(arm_lift *L, unsigned rd, uam_ref a, uam_ref b, int acc_reg, bool sub,
                  bool round);                         /* SMMUL / SMMLA / SMMLS */
void     arm_div(arm_lift *L, unsigned rd, uam_ref a, uam_ref b, bool sign);
/* SMLAD/SMUAD/SMLSD/SMUSD (ra = 15: no accumulate); lng: SMLALD/SMLSLD (ra = RdLo). */
void     arm_dual_mul(arm_lift *L, unsigned rd, unsigned ra, unsigned rm, unsigned rn, bool swap,
                      bool subtract, bool lng);
void     arm_usad8(arm_lift *L, unsigned rd, uam_ref x, uam_ref y, unsigned ra);
void     arm_sel(arm_lift *L, unsigned rd, uam_ref a, uam_ref b);
void     arm_pkh(arm_lift *L, unsigned rd, uam_ref a, uam_ref b, bool tb, unsigned sh);
/* Parallel add/subtract: op1 1 S, 2 Q, 3 SH, 5 U, 6 UQ, 7 UH; op2 0 ADD16 1 ASX 2 SAX
 * 3 SUB16 4 ADD8 7 SUB8. Returns false for an unallocated encoding. */
bool     arm_parallel(arm_lift *L, unsigned rd, uam_ref a, uam_ref b, unsigned op1, unsigned op2);

/* Status registers and coprocessors. */
void     arm_mrs(arm_lift *L, unsigned rd, bool spsr);
arm_lift_result arm_msr(arm_lift *L, uam_ref v, unsigned mask, bool spsr);
arm_lift_result arm_cps(arm_lift *L, unsigned imod, bool m, unsigned aif, unsigned mode);
arm_lift_result arm_coproc(arm_lift *L, bool load, unsigned cp, unsigned opc1, unsigned crn,
                           unsigned crm, unsigned opc2, unsigned rt); /* MRC / MCR */
arm_lift_result arm_coproc64(arm_lift *L, bool load, unsigned cp, unsigned opc1, unsigned crm,
                             unsigned rt, unsigned rt2);              /* MRRC / MCRR */
arm_lift_result arm_hint(arm_lift *L, unsigned op);   /* NOP YIELD WFE WFI SEV */
arm_lift_result arm_barrier(arm_lift *L, unsigned op); /* 4 DSB 5 DMB 6 ISB; 1 CLREX */
arm_lift_result arm_srs(arm_lift *L, unsigned mode, bool inc, bool before, bool wb);
arm_lift_result arm_rfe(arm_lift *L, unsigned rn, bool inc, bool before, bool wb);

/* arm_thumb.c */
arm_lift_result arm_lift_t16(arm_lift *L, uint32_t hw);
arm_lift_result arm_lift_t32(arm_lift *L, uint32_t i);
static inline bool arm_t32_prefix(uint32_t hw) { return (hw >> 11) >= 0x1D; }

/* arm_system.c */
extern const uam_helper_desc arm_helpers[ARM_HELPER__COUNT];

#endif /* UAM_ARM_INTERNAL_H */
