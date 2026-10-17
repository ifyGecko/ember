/*
 * mips.h - Internal definitions shared by the MIPS32 lifter and system code.
 * Not part of the public interface.
 */
#ifndef UAM_MIPS_INTERNAL_H
#define UAM_MIPS_INTERNAL_H

#include "uam/isa/mips32.h"
#include "uam/sched.h"

/*
 * Register file: 32 GPRs then the HI and LO multiply/divide registers, all
 * 32-bit. r0 is hardwired to zero (the lifter folds reads to 0 and drops
 * writes). PC lives in uam_cpu, not the register file.
 */
#define MIPS_REG_OFF(i)  ((uint32_t)(i) * 4u)
#define MIPS_HI_OFF      (32u * 4u)
#define MIPS_LO_OFF      (33u * 4u)
#define MIPS_REGFILE_SIZE (34u * 4u)

/* Privilege (cpu->priv): the *effective* operating mode. EXL or ERL force
 * kernel mode; otherwise Status.KSU selects it. Supervisor mode exists only if
 * the configured Status write mask lets software set KSU = 01. */
enum { MIPS_KERNEL = 0, MIPS_SUPERVISOR = 1, MIPS_USER = 2 };

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
#define ST_BEV  (1u << 22)  /* boot exception vectors (0xbfc00200 region) */
#define ST_CU0  (1u << 28)  /* CP0 usable in user mode */

/* Cause register fields. */
#define CAUSE_EXC_SHIFT 2
#define CAUSE_EXC_MASK  (0x1Fu << CAUSE_EXC_SHIFT)
#define CAUSE_IP_SHIFT  8
#define CAUSE_IP_MASK   (0xFFu << CAUSE_IP_SHIFT)
#define CAUSE_IV        (1u << 23)  /* use special interrupt vector 0x200 */
#define CAUSE_CE_SHIFT  28          /* coprocessor number for Coprocessor Unusable */
#define CAUSE_CE_MASK   (3u << CAUSE_CE_SHIFT)
#define CAUSE_BD        (1u << 31)  /* exception victim was in a branch delay slot */

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
};

/*
 * Lifter-only TRAP cause (not an ExcCode): the delay slot of a branch could not
 * be fetched. tval is the slot address; take_exception re-translates it to
 * raise the precise fetch exception (TLB miss, address error or bus error).
 */
#define MIPS_TRAP_SLOT_FETCH 0x100u

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
    MIPS_HELPER__COUNT
};

#define MIPS_TLB_MAX 64u

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
    uint64_t count_bias;  /* Count = (sched_now >> 1) + count_bias */
    bool     timer_ip;    /* Count==Compare latch drives IP7 */
    mips_tlb_entry tlb[MIPS_TLB_MAX];
} mips_cp0;

typedef struct mips_state {
    uam_mips_config cfg;    /* CPU identity, chosen by the board; survives reset */
    uam_timer       timer;  /* Count/Compare wake-up, per hart; survives reset */
    uint32_t        hwlines;/* hardware-driven Cause.IP bits (from irq_input) */
    uint8_t         endian; /* UAM_ACC_LE or UAM_ACC_BE */
    bool            erl_seen; /* last Status.ERL seen (it changes kuseg's mapping) */
    mips_cp0        c;
} mips_state;

static inline mips_state *mips_st(uam_cpu *cpu) { return (mips_state *)cpu->isa_state; }

/* mips_lift.c */
uam_status mips_lift(uam_lift_ctx *lc);

/* mips_system.c */
extern const uam_helper_desc mips_helpers[MIPS_HELPER__COUNT];
uint32_t mips_count_now(uam_cpu *cpu);

#endif /* UAM_MIPS_INTERNAL_H */
