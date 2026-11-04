/*
 * mips.h - Internal definitions shared by the MIPS32 lifters and system code.
 * Not part of the public interface.
 */
#ifndef UAM_MIPS_INTERNAL_H
#define UAM_MIPS_INTERNAL_H

#include "uam/isa/mips32.h"
#include "uam/sched.h"

/*
 * Register file: 32 GPRs then the HI and LO multiply/divide registers (the DSP
 * ASE's accumulator ac0), all 32-bit; then the DSP ASE's accumulators ac1..ac3
 * (HI, LO each) and DSPControl, the FPU's FCSR, and the 32 floating-point
 * registers, 64 bits each. r0 is hardwired to zero (the lifter folds reads to 0
 * and drops writes). PC lives in uam_cpu, not the register file.
 *
 * With Status.FR = 1 FPR n is the 64-bit slot n (a single in its low word); with
 * FR = 0 the 32-bit register n is word n & 1 of slot n & ~1, so the even/odd pair
 * that holds a double is slot n.
 */
#define MIPS_REG_OFF(i)  ((uint32_t)(i) * 4u)
#define MIPS_HI_OFF      (32u * 4u)
#define MIPS_LO_OFF      (33u * 4u)
#define MIPS_ACHI_OFF(n) ((n) ? 136u + ((uint32_t)(n) - 1u) * 8u : MIPS_HI_OFF)
#define MIPS_ACLO_OFF(n) ((n) ? 140u + ((uint32_t)(n) - 1u) * 8u : MIPS_LO_OFF)
#define MIPS_DSPCTL_OFF  160u
#define MIPS_FCSR_OFF    164u
#define MIPS_FPR_OFF(i)  (168u + (uint32_t)(i) * 8u)
#define MIPS_REGFILE_SIZE (168u + 32u * 8u)

/* Privilege (cpu->priv). Bits 1:0 are the *effective* operating mode: EXL or ERL
 * force kernel mode; otherwise Status.KSU selects it. Supervisor mode exists only
 * if the configured Status write mask lets software set KSU = 01. The bits above
 * record Status bits that change how instructions are lifted (translated code is
 * tagged with cpu->priv): CU1, FR and MX. */
enum { MIPS_KERNEL = 0, MIPS_SUPERVISOR = 1, MIPS_USER = 2 };
#define MIPS_PRIV_MODE(p) ((p) & 3u)
#define MIPS_PRIV_CU1     (1u << 4)
#define MIPS_PRIV_FR      (1u << 5)
#define MIPS_PRIV_MX      (1u << 6)

/* CP0 register numbers, encoded as reg*8 + sel (matches mfc0/mtc0 fields). */
#define CP0(reg, sel) (((reg) << 3) | (sel))
enum {
    CP0_INDEX     = CP0(0, 0),
    CP0_RANDOM    = CP0(1, 0),
    CP0_ENTRYLO0  = CP0(2, 0),
    CP0_ENTRYLO1  = CP0(3, 0),
    CP0_CONTEXT   = CP0(4, 0),
    CP0_PAGEMASK  = CP0(5, 0),
    CP0_WIRED     = CP0(6, 0),
    CP0_BADVADDR  = CP0(8, 0),
    CP0_COUNT     = CP0(9, 0),
    CP0_ENTRYHI   = CP0(10, 0),
    CP0_COMPARE   = CP0(11, 0),
    CP0_STATUS    = CP0(12, 0),
    CP0_INTCTL    = CP0(12, 1),
    CP0_SRSCTL    = CP0(12, 2),
    CP0_SRSMAP    = CP0(12, 3),
    CP0_CAUSE     = CP0(13, 0),
    CP0_EPC       = CP0(14, 0),
    CP0_PRID      = CP0(15, 0),
    CP0_EBASE     = CP0(15, 1),
    CP0_CONFIG    = CP0(16, 0),
    CP0_CONFIG1   = CP0(16, 1),
    CP0_CONFIG2   = CP0(16, 2),
    CP0_CONFIG3   = CP0(16, 3),
    CP0_ERROREPC  = CP0(30, 0),
};

/* Status register bits. */
#define ST_IE   (1u << 0)   /* interrupts enabled */
#define ST_EXL  (1u << 1)   /* exception level */
#define ST_ERL  (1u << 2)   /* error level */
#define ST_KSU_SHIFT 3      /* KSU[1:0] at bits 4:3: 00 kernel, 01 supervisor, 10 user */
#define ST_KSU_MASK  (3u << ST_KSU_SHIFT)
#define ST_UM   (1u << 4)   /* KSU[1]: user mode */
#define ST_IM_SHIFT 8       /* interrupt mask IM[7:0] at bits 15:8 */
#define ST_IM_MASK  (0xFFu << ST_IM_SHIFT)
#define ST_IPL_SHIFT 10     /* EIC mode: interrupt priority level IPL[5:0] at bits 15:10 */
#define ST_IPL_MASK  (0x3Fu << ST_IPL_SHIFT)
#define ST_BEV  (1u << 22)  /* boot exception vectors (0xbfc00200 region) */
#define ST_MX   (1u << 24)  /* DSP ASE enabled */
#define ST_FR   (1u << 26)  /* 64-bit floating-point registers */
#define ST_CU0  (1u << 28)  /* CP0 usable in user mode */
#define ST_CU1  (1u << 29)  /* CP1 (FPU) usable */

/* Cause register fields. */
#define CAUSE_EXC_SHIFT 2
#define CAUSE_EXC_MASK  (0x1Fu << CAUSE_EXC_SHIFT)
#define CAUSE_IP_SHIFT  8
#define CAUSE_IP_MASK   (0xFFu << CAUSE_IP_SHIFT)
#define CAUSE_RIPL_SHIFT 10         /* EIC mode: requested interrupt priority level */
#define CAUSE_IV        (1u << 23)  /* use special interrupt vector 0x200 */
#define CAUSE_CE_SHIFT  28          /* coprocessor number for Coprocessor Unusable */
#define CAUSE_CE_MASK   (3u << CAUSE_CE_SHIFT)
#define CAUSE_TI        (1u << 30)  /* timer interrupt pending */
#define CAUSE_BD        (1u << 31)  /* exception victim was in a branch delay slot */

/* Config1 / Config3 feature bits. */
#define CFG0_MT(c)      (((c) >> 7) & 7u)   /* 1 TLB, 3 FMT */
#define CFG1_FP         (1u << 0)
#define CFG1_CA         (1u << 2)           /* MIPS16e */
#define CFG3_VINT       (1u << 5)
#define CFG3_VEIC       (1u << 6)
#define CFG3_DSPP       (1u << 10)
#define CFG3_DSP2P      (1u << 11)
#define CFG3_ISA_SHIFT  14                  /* microMIPS: 0 none, 1 only, 2/3 both */
#define CFG3_ISA_MASK   (3u << CFG3_ISA_SHIFT)
#define CFG3_ISAONEXC   (1u << 16)
#define CFG3_MCU        (1u << 17)

/* ExcCode values (Cause bits 6:2). */
enum {
    EXC_INT  = 0,   /* interrupt */
    EXC_MOD  = 1,   /* TLB modified */
    EXC_TLBL = 2,   /* TLB miss (load/fetch) */
    EXC_TLBS = 3,   /* TLB miss (store) */
    EXC_ADEL = 4,   /* address error (load/fetch) */
    EXC_ADES = 5,   /* address error (store) */
    EXC_IBE  = 6,   /* bus error (fetch) */
    EXC_DBE  = 7,   /* bus error (data) */
    EXC_SYS  = 8,   /* syscall */
    EXC_BP   = 9,   /* breakpoint */
    EXC_RI   = 10,  /* reserved instruction */
    EXC_CPU  = 11,  /* coprocessor unusable */
    EXC_OV   = 12,  /* arithmetic overflow */
    EXC_TR   = 13,  /* trap */
    EXC_FPE  = 15,  /* floating-point exception */
    EXC_DSPDIS = 26,/* DSP ASE state disabled */
};

/*
 * Lifter-only TRAP causes (not ExcCodes). SLOT_FETCH: the delay slot of a branch
 * could not be fetched; tval is the slot address, and take_exception re-translates
 * it to raise the precise fetch exception (TLB miss, address error or bus error).
 * FETCH_ADDR: an instruction address the current ISA mode cannot fetch (a MIPS32
 * instruction at a halfword boundary, or the MIPS32 mode a microMIPS-only processor
 * lacks); tval is the address, which becomes BadVAddr.
 */
#define MIPS_TRAP_SLOT_FETCH 0x100u
#define MIPS_TRAP_FETCH_ADDR 0x101u

/* Translation fault sub-causes (uam_xlate.fault, carried in uam_exception.cause). */
enum {
    MIPS_XF_ADDR = 1,     /* address error: segment not accessible in this mode, or misaligned fetch */
    MIPS_XF_REFILL,       /* no matching TLB entry (refill vector when EXL = 0) */
    MIPS_XF_INVALID,      /* matching entry with V = 0 */
    MIPS_XF_MOD,          /* store to a matching valid entry with D = 0 */
};

/* Helper ids. */
enum {
    MIPS_HELPER_ERET = 0,
    MIPS_HELPER_TLBR,
    MIPS_HELPER_TLBWI,
    MIPS_HELPER_TLBWR,
    MIPS_HELPER_TLBP,
    MIPS_HELPER_RDPGPR,   /* (rt) -> GPR[PSS][rt] */
    MIPS_HELPER_WRPGPR,   /* (rd, value): GPR[PSS][rd] = value */
    MIPS_HELPER_IRET,     /* MCU ASE: like ERET */
    MIPS_HELPER_FPU,      /* (MIPS32 COP1/COP1X word): an FPU operation, operands in the regfile */
    MIPS_HELPER_CFC1,     /* (fs) -> FPU control register */
    MIPS_HELPER_CTC1,     /* (fs, value) */
    MIPS_HELPER_DSP,      /* (MIPS32 DSP word): a DSP ASE operation, operands in the regfile */
    MIPS_HELPER__COUNT
};

/* Why a helper failed (mips_state.hfault), for take_exception. */
enum { MIPS_HF_NONE, MIPS_HF_FPE, MIPS_HF_RI };

#define MIPS_TLB_MAX 64u
#define MIPS_SRS_MAX 16u

/* One joint-TLB entry (an even/odd page pair). */
typedef struct mips_tlb_entry {
    uint32_t vpn2;    /* EntryHi[31:13] as an address (low 13 bits zero) */
    uint32_t mask;    /* PageMask[28:13] */
    uint8_t  asid;
    bool     g;       /* global: ignore ASID */
    uint32_t lo[2];   /* EntryLo0/1 without the G bit: PFN[25:6] C[5:3] D[2] V[1] */
} mips_tlb_entry;

/* CP0 state cleared by a reset. */
typedef struct mips_cp0 {
    uint32_t status;
    uint32_t cause;
    uint32_t epc;
    uint32_t errorepc;
    uint32_t badvaddr;
    uint32_t ebase;       /* exception base (bits 31:12) */
    uint32_t compare;
    uint32_t config;      /* Config0 (K0 is writable) */
    uint32_t intctl;
    uint32_t index, entrylo0, entrylo1, context, pagemask, wired, entryhi;
    uint32_t srsctl;      /* ESS, PSS and CSS (HSS and EICSS are filled in on read) */
    uint32_t srsmap;
    uint32_t config3;     /* Config3 (ISAOnExc is writable) */
    uint64_t count_bias;  /* Count = (sched_now >> 1) + count_bias */
    bool     timer_ip;    /* Count==Compare latch drives IP7 */
    mips_tlb_entry tlb[MIPS_TLB_MAX];
    uint32_t srs[MIPS_SRS_MAX][32]; /* GPR sets; the current one lives in the regfile */
} mips_cp0;

/* The request an external interrupt controller presents (uam_mips_eic_input). */
typedef struct mips_eic {
    uint32_t ripl, vector, offset, ss;
} mips_eic;

typedef struct mips_state {
    uam_mips_config cfg;    /* CPU identity, chosen by the board; survives reset */
    uam_timer       timer;  /* Count/Compare wake-up, per hart; survives reset */
    uint32_t        hwlines;/* hardware-driven Cause.IP bits (from irq_input) */
    uint8_t         endian; /* UAM_ACC_LE or UAM_ACC_BE */
    bool            erl_seen; /* last Status.ERL seen (it changes kuseg's mapping) */
    uint8_t         hfault; /* MIPS_HF_*: why the last helper failed */
    mips_eic        eic;    /* the external controller's request (Config3.VEIC) */
    uam_signal      timer_line, swint_line[2];
    mips_cp0        c;
} mips_state;

static inline mips_state *mips_st(uam_cpu *cpu) { return (mips_state *)cpu->isa_state; }

/* Features, from the Config registers (uam/isa/mips32.h). */
static inline bool mips_has_m16(const mips_state *s)   { return s->cfg.config1 & CFG1_CA; }
static inline unsigned mips_umips(const mips_state *s) { return (s->cfg.config3 & CFG3_ISA_MASK) >> CFG3_ISA_SHIFT; }
static inline bool mips_has_umips(const mips_state *s) { return mips_umips(s) != 0; }
static inline bool mips_has_mips32(const mips_state *s){ return mips_umips(s) != 1; }
static inline bool mips_has_fpu(const mips_state *s)   { return s->cfg.config1 & CFG1_FP; }
static inline bool mips_has_dsp(const mips_state *s)   { return s->cfg.config3 & CFG3_DSPP; }
static inline bool mips_has_dsp2(const mips_state *s)  { return s->cfg.config3 & CFG3_DSP2P; }
static inline bool mips_has_mcu(const mips_state *s)   { return s->cfg.config3 & CFG3_MCU; }
static inline bool mips_has_eic(const mips_state *s)   { return s->cfg.config3 & CFG3_VEIC; }
static inline bool mips_compressed(const mips_state *s){ return mips_has_m16(s) || mips_has_umips(s); }

/*
 * Lifter state, shared by the MIPS32, MIPS16e and microMIPS decoders. pc is the
 * instruction's address with the ISA mode in bit 0 (as in cpu->pc); npc the
 * address of the next instruction, likewise.
 */
typedef enum { LIFT_SEQ, LIFT_DONE } lift_result;

typedef struct ml {
    uam_lift_ctx *lc;
    uam_builder  *b;
    const mips_state *s;
    uint32_t      pc;
    uint32_t      npc;
    unsigned      endian; /* UAM_ACC_LE / UAM_ACC_BE for data accesses */
    unsigned      priv;   /* cpu->priv the block is translated at */
    uint32_t      raw;    /* the instruction as fetched (Reserved Instruction reports it) */
} ml;

#define NONE UAM_REF_NONE

/* mips_lift.c: MIPS32 instructions and pieces the other decoders reuse. */
uam_status  mips_lift(uam_lift_ctx *lc);
lift_result mips_lift32(ml *L, uint32_t i);  /* a non-control MIPS32 instruction */
uam_ref mips_imm(ml *L, uint32_t v);
uam_ref mips_reg_r(ml *L, unsigned r);
void    mips_reg_w(ml *L, unsigned r, uam_ref v);
uam_ref mips_bin(ml *L, uam_opcode op, uam_ref a, uam_ref b);
void    mips_trap(ml *L, uam_ref guard, uint32_t exccode, uam_ref tval);
void    mips_ri(ml *L);                      /* Reserved Instruction, ends the block */
uam_ref mips_helper(ml *L, unsigned id, uam_ref a, uam_ref b, uam_ref c);
uam_ref mips_fcc(ml *L, unsigned cc, bool tf);   /* FCSR condition code cc is tf */

/* mips16.c / mips_umips.c: one block in the compressed ISA (lc->pc has bit 0 set). */
uam_status mips16_lift(uam_lift_ctx *lc);
uam_status mips_umips_lift(uam_lift_ctx *lc);
/* microMIPS FPU (POOL32F) and DSP words recoded as their MIPS32 equivalents, or 0. */
uint32_t mips_umips_fpu32(uint32_t i);
uint32_t mips_umips_dsp32(uint32_t i);

/* mips_fpu.c: COP1, COP1X and the FPU loads/stores in MIPS32 encoding. */
lift_result mips_lift_fpu(ml *L, uint32_t i);
uint64_t mips_fpu_op(void *env, uint64_t insn, uint64_t b, uint64_t c, uam_status *st);
uint64_t mips_cfc1(void *env, uint64_t fs, uint64_t b, uint64_t c, uam_status *st);
uint64_t mips_ctc1(void *env, uint64_t fs, uint64_t v, uint64_t c, uam_status *st);

/* mips_dsp.c: the DSP ASE in MIPS32 encoding (SPECIAL3 and REGIMM BPOSGE32 aside). */
lift_result mips_lift_dsp(ml *L, uint32_t i);
uint64_t    mips_dsp_op(void *env, uint64_t insn, uint64_t b, uint64_t c, uam_status *st);

/* mips_system.c */
extern const uam_helper_desc mips_helpers[MIPS_HELPER__COUNT];
uint32_t mips_count_now(uam_cpu *cpu);

#endif /* UAM_MIPS_INTERNAL_H */
