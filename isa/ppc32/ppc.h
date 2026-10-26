/*
 * ppc.h - Internal definitions shared by the 32-bit PowerPC lifter, the
 * floating-point unit and the system code. Not part of the public interface.
 *
 * Bits are named as the PowerPC books number them (bit 0 is the most significant
 * of a 32-bit register); the constants below are the C values.
 */
#ifndef UAM_PPC_INTERNAL_H
#define UAM_PPC_INTERNAL_H

#include "uam/isa/ppc.h"
#include "uam/sched.h"

/*
 * Register file: the 32 GPRs, LR, CTR, FPSCR, the XER split into SO, OV, CA (one
 * byte each, 0 or 1) and the string byte count, the eight CR fields (one byte
 * each, LT GT EQ SO in bits 3..0), then the 32 FPRs (IEEE double bits). The pc
 * lives in uam_cpu.
 */
#define PPC_GPR_OFF(i)   ((uint32_t)(i) * 4u)
#define PPC_LR_OFF       128u
#define PPC_CTR_OFF      132u
#define PPC_FPSCR_OFF    136u
#define PPC_SO_OFF       140u
#define PPC_OV_OFF       141u
#define PPC_CA_OFF       142u
#define PPC_BC_OFF       143u   /* XER[25:31]: the lswx/stswx byte count */
#define PPC_CR_OFF(f)    (144u + (uint32_t)(f))
#define PPC_FPR_OFF(i)   (152u + (uint32_t)(i) * 8u)
#define PPC_REGFILE_SIZE (152u + 32u * 8u)

/* CR field bits (in a field's byte). */
#define CR_LT 8u
#define CR_GT 4u
#define CR_EQ 2u
#define CR_SO 1u

/* MSR bits. */
#define MSR_POW (1u << 18)
#define MSR_ILE (1u << 16)
#define MSR_EE  (1u << 15)
#define MSR_PR  (1u << 14)
#define MSR_FP  (1u << 13)
#define MSR_ME  (1u << 12)
#define MSR_FE0 (1u << 11)
#define MSR_SE  (1u << 10)
#define MSR_BE  (1u << 9)
#define MSR_FE1 (1u << 8)
#define MSR_IP  (1u << 6)
#define MSR_IR  (1u << 5)
#define MSR_DR  (1u << 4)
#define MSR_RI  (1u << 1)
#define MSR_LE  (1u << 0)
/* MSR bits saved in SRR1 by an interrupt and restored by rfi: 16-23, 25-27, 30-31. */
#define MSR_SRR1_MASK 0x0000FF73u

/* SRR1 bits an interrupt sets besides the MSR copy. */
#define SRR1_ISI_NOPTE  0x40000000u   /* ISI: no translation */
#define SRR1_ISI_NOEXEC 0x10000000u   /* ISI: direct-store segment, N or guarded page */
#define SRR1_ISI_PROT   0x08000000u   /* ISI: protection */
#define SRR1_PROG_FP    0x00100000u   /* program: floating-point enabled exception */
#define SRR1_PROG_ILL   0x00080000u   /* program: illegal instruction */
#define SRR1_PROG_PRIV  0x00040000u   /* program: privileged instruction */
#define SRR1_PROG_TRAP  0x00020000u   /* program: trap */

/* DSISR bits for a DSI. */
#define DSISR_NOPTE  0x40000000u
#define DSISR_PROT   0x08000000u
#define DSISR_DS     0x04000000u      /* direct-store segment */
#define DSISR_STORE  0x02000000u

/*
 * cpu->priv tags translated code with the MSR bits lifted code depends on:
 * problem state (privileged instructions), floating point available, and single
 * step / branch trace (one instruction per block, then the trace exception).
 */
#define PPC_PRIV_PR 1u
#define PPC_PRIV_FP 2u
#define PPC_PRIV_SE 4u
#define PPC_PRIV_BE 8u

/* Special purpose registers the architecture defines. */
enum {
    SPR_XER = 1, SPR_LR = 8, SPR_CTR = 9, SPR_DSISR = 18, SPR_DAR = 19, SPR_DEC = 22,
    SPR_SDR1 = 25, SPR_SRR0 = 26, SPR_SRR1 = 27, SPR_TBL_R = 268, SPR_TBU_R = 269,
    SPR_SPRG0 = 272, SPR_SPRG3 = 275, SPR_EAR = 282, SPR_TBL_W = 284, SPR_TBU_W = 285,
    SPR_PVR = 287, SPR_IBAT0U = 528, SPR_DBAT3L = 543, SPR_IBAT4U = 560, SPR_DBAT7L = 575,
    SPR_PIR = 1023,
};
#define SPR_PRIVILEGED(n) (((n) >> 4) & 1u)   /* spr[0] of the instruction field */

/* System register ids (SYSRD/SYSWR aux): an SPR number, or one of these. */
enum {
    PPC_SR_MSR = 0x1000u,
    PPC_SR_SR0 = 0x1100u,   /* + n: segment register n */
};

/* Exception causes carried by TRAP (uam_exception.cause), tval as noted. */
enum {
    PPC_TRAP_ILLEGAL = 1,   /* program, illegal instruction; tval = the instruction */
    PPC_TRAP_PRIV,          /* program, privileged instruction; tval = the instruction */
    PPC_TRAP_TRAP,          /* program, trap (tw/twi) */
    PPC_TRAP_SC,            /* system call */
    PPC_TRAP_FPU,           /* floating point unavailable */
};

/* Translation fault sub-causes (uam_xlate.fault): DSISR-style bits. */
#define PPC_XF_NOPTE  0x40000000u
#define PPC_XF_NOEXEC 0x10000000u
#define PPC_XF_PROT   0x08000000u
#define PPC_XF_DS     0x04000000u

/* Helper ids. */
enum {
    PPC_HELPER_RFI = 0,
    PPC_HELPER_FP,          /* a floating-point arithmetic, compare or FPSCR instruction */
    PPC_HELPER_SP2DP,       /* single bits -> double bits (lfs) */
    PPC_HELPER_DP2SP,       /* double bits -> single bits (stfs) */
    PPC_HELPER_MFSRIN,
    PPC_HELPER_MTSRIN,
    PPC_HELPER_TLBIE,
    PPC_HELPER_TLBIA,
    PPC_HELPER_LSWX,
    PPC_HELPER_STSWX,
    PPC_HELPER_TRACE,       /* arm the trace exception after this instruction */
    PPC_HELPER__COUNT
};

/* Why a helper failed (ppc_state.hfault), for take_exception. */
enum {
    PPC_HF_NONE = 0,
    PPC_HF_FP_ENABLED,      /* program: floating-point enabled exception */
    PPC_HF_PRIV,            /* program: privileged instruction */
    PPC_HF_ILLEGAL,         /* program: illegal instruction */
    PPC_HF_DSI,             /* data storage: hf_ea, hf_dsisr */
    PPC_HF_ALIGN,           /* alignment: hf_ea */
    PPC_HF_MCHECK,          /* bus error: machine check */
};

/* The software TLB: recent page-table and BAT translations (direct mapped). */
#define PPC_TLB_SETS 256u
typedef struct ppc_tlb_entry {
    uint32_t vpage;         /* EA >> 12, or PPC_TLB_NONE */
    uint32_t ppage;
    bool     write_ok;      /* a store may use it (permitted, C already set) */
} ppc_tlb_entry;
#define PPC_TLB_NONE 0xFFFFFFFFu

/* Architected state cleared by a reset. */
typedef struct ppc_sys {
    uint32_t msr;
    uint32_t srr0, srr1, dar, dsisr, ear, sdr1;
    uint32_t sprg[4];
    uint32_t ibat[8][2], dbat[8][2];   /* [n][0] upper, [n][1] lower */
    uint32_t sr[16];
    uint64_t tb_bias;       /* TB = ticks + tb_bias */
    uint32_t dec;           /* DEC as last written ... */
    uint64_t dec_ticks;     /* ... at this tick count */
    uint64_t dec_next;      /* tick count of its next change from 0 to -1 */
    bool     dec_pending;   /* the decrementer exception is pending (latched) */
    bool     sreset_pending, mcp_pending;
    bool     trace_pending;
    bool     checkstop;
} ppc_sys;

typedef struct ppc_state {
    uam_ppc_config         cfg;       /* chosen by the board; survives reset */
    const uam_ppc_spr_ops *spr_ops;
    void                  *spr_ctx;
    uam_timer              dec_timer;
    uint8_t                intlines;  /* levels of UAM_PPC_IRQ_INT(0..7) */
    bool                   sreset_level, mcp_level;
    uint64_t               taken_lines;  /* irq_lines: what the last interrupt came from */
    unsigned               hfault;    /* PPC_HF_*, for the failing helper */
    uint32_t               hf_ea, hf_dsisr;
    ppc_tlb_entry          itlb[PPC_TLB_SETS], dtlb[PPC_TLB_SETS];
    uint32_t               impl_spr[1024];   /* SPRs left to the default */
    ppc_sys                s;
    uam_cpu               *cpu;
    struct ppc_state      *next;      /* every PowerPC hart (tlbie reaches all on a bus) */
} ppc_state;

static inline ppc_state *ppc_st(uam_cpu *cpu) { return (ppc_state *)cpu->isa_state; }

/* ppc_lift.c */
uam_status ppc_lift(uam_lift_ctx *lc);

/* ppc_fpu.c */
uint64_t ppc_helper_fp(void *env, uint64_t insn, uint64_t b, uint64_t c, uam_status *st);
uint64_t ppc_helper_sp2dp(void *env, uint64_t a, uint64_t b, uint64_t c, uam_status *st);
uint64_t ppc_helper_dp2sp(void *env, uint64_t a, uint64_t b, uint64_t c, uam_status *st);
uint64_t ppc_sp_to_dp(uint32_t s);
uint32_t ppc_dp_to_sp(uint64_t d);

/* ppc_mmu.c */
uam_status ppc_translate(uam_cpu *cpu, uint64_t vaddr, uam_xlate_kind kind, uam_xlate *out);
void       ppc_tlb_flush(ppc_state *s);
void       ppc_tlb_flush_page(ppc_state *s, uint32_t ea);
uam_status ppc_mem_rw(uam_cpu *cpu, uint32_t ea, unsigned size, bool store, uint64_t *val);
bool       ppc_peek_insn(uam_cpu *cpu, uint32_t pc, uint32_t *insn);

/* ppc_system.c */
extern const uam_helper_desc ppc_helpers[PPC_HELPER__COUNT];
void ppc_msr_write(uam_cpu *cpu, uint32_t v);

#endif /* UAM_PPC_INTERNAL_H */
