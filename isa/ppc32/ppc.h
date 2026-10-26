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
/* Book E MSR bits (where they differ from the classic ones; IS and DS sit where IR
 * and DR do, WE where POW does). */
#define MSR_WE  (1u << 18)
#define MSR_CE  (1u << 17)
#define MSR_DE  (1u << 9)
#define MSR_IS  (1u << 5)
#define MSR_DS  (1u << 4)
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
    /* Book E */
    SPR_PID0 = 48, SPR_DECAR = 54, SPR_CSRR0 = 58, SPR_CSRR1 = 59, SPR_DEAR = 61, SPR_ESR = 62,
    SPR_IVPR = 63, SPR_USPRG0 = 256, SPR_SPRG4_R = 260, SPR_SPRG7_R = 263, SPR_SPRG4_W = 276,
    SPR_SPRG7_W = 279, SPR_PIR_BOOKE = 286, SPR_DBSR = 304, SPR_DBCR0 = 308, SPR_DBCR1 = 309,
    SPR_DBCR2 = 310, SPR_TSR = 336, SPR_TCR = 340, SPR_IVOR0 = 400, SPR_IVOR15 = 415,
    SPR_MCSRR0 = 570, SPR_MCSRR1 = 571, SPR_MCSR = 572, SPR_MCAR = 573, SPR_DSRR0 = 574,
    SPR_DSRR1 = 575, SPR_MAS0 = 624, SPR_MAS4 = 628, SPR_MAS6 = 630, SPR_PID1 = 633,
    SPR_PID2 = 634, SPR_TLB0CFG = 688, SPR_TLB1CFG = 689, SPR_MAS7 = 944, SPR_MMUCSR0 = 1012,
    SPR_MMUCFG = 1015,
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
#define PPC_XF_TLBMISS 0x00000001u   /* Book E: no TLB entry (TLB error interrupt) */

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
    /* Book E */
    PPC_HELPER_RFCI,        /* return from a critical interrupt */
    PPC_HELPER_RFMCI,       /* ... a machine check (MCSRR0/1) */
    PPC_HELPER_RFDI,        /* ... a debug interrupt (DSRR0/1) */
    PPC_HELPER_TLBWE,       /* (insn) */
    PPC_HELPER_TLBRE,       /* (insn) */
    PPC_HELPER_TLBSX,       /* (insn, EA) */
    PPC_HELPER_TLBIVAX,     /* (EA) */
    PPC_HELPER_MFDCR,       /* (dcrn) -> value */
    PPC_HELPER_MTDCR,       /* (dcrn, value) */
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
    uint32_t ppage;         /* real address >> 12 (36-bit real addresses fit) */
    bool     read_ok;       /* a load may use it */
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

/* Book E state cleared by a reset (the TLB arrays are separate). */
#define PPC_BOOKE_TLB_MAX 1024u
typedef struct ppc_booke {
    uint32_t csrr0, csrr1, mcsrr0, mcsrr1, dsrr0, dsrr1;
    uint32_t dear, esr, ivpr, ivor[16], mcsr, mcar;
    uint32_t pid[3];
    uint32_t mas[8];        /* MAS0-MAS7 (MAS5 unused) */
    uint32_t tcr, tsr, decar;
    uint32_t dbcr0, dbsr;
    uint32_t sprg47[4], usprg0;
    uint64_t fit_tb, wdt_tb;  /* time base when the FIT / watchdog bits were last looked at */
    uint32_t victim[2];     /* next replacement way of each TLB array */
} ppc_booke_regs;

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
    /* Book E */
    ppc_booke_regs         bk;
    uam_ppc_tlbe           tlb[2][PPC_BOOKE_TLB_MAX];
    uam_timer              fit_timer, wdt_timer;
    bool                   cint_level;
    uint8_t                wrs_latched;   /* TSR[WRS]: the last watchdog reset survives it */
    const uam_ppc_dcr_ops *dcr_ops;
    void                  *dcr_ctx;
    const uam_ppc_tlb_ops *tlb_ops;
    void                  *tlb_ctx;
    uam_ppc_reset_sink     reset_sink;
    void                  *reset_ctx;
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
void       ppc_msr_write(uam_cpu *cpu, uint32_t v);
uint64_t   ppc_ticks_now(uam_cpu *cpu);
uint64_t   ppc_tb_now(uam_cpu *cpu);
void       ppc_tb_set(uam_cpu *cpu, uint64_t tb);
uam_status ppc_impl_read(uam_cpu *cpu, unsigned spr, uint64_t *val);
uam_status ppc_impl_write(uam_cpu *cpu, unsigned spr, uint32_t v);
void       ppc_checkstop(uam_cpu *cpu, uint32_t pc);
ppc_state *ppc_harts(void);

static inline bool ppc_booke(const ppc_state *s) { return s->cfg.model == UAM_PPC_MODEL_BOOKE; }

/* ppc_booke.c: the Book E operating environment */
bool       ppc_booke_config_ok(const uam_ppc_config *c);
unsigned   ppc_booke_priv(const ppc_state *s);
void       ppc_booke_msr_write(uam_cpu *cpu, uint32_t v);
void       ppc_booke_reset(uam_cpu *cpu);
void       ppc_booke_take_exception(uam_cpu *cpu, const uam_exception *e);
bool       ppc_booke_irq_take(uam_cpu *cpu);
bool       ppc_booke_irq_wakeup(uam_cpu *cpu);
uam_status ppc_booke_sysreg_read(uam_cpu *cpu, uint32_t id, uint64_t *val);
uam_status ppc_booke_sysreg_write(uam_cpu *cpu, uint32_t id, uint32_t v);
uam_status ppc_booke_translate(uam_cpu *cpu, uint32_t ea, bool fetch, bool store, bool debug,
                               uam_xlate *out);
void       ppc_booke_timer_fire(void *ctx);
void       ppc_booke_save(uam_cpu *cpu, uam_state_writer *w);
void       ppc_booke_restore(uam_cpu *cpu, uam_state_reader *r);
void       ppc_booke_dump(uam_cpu *cpu, FILE *out);
uint64_t   ppc_helper_rfci(void *env, uint64_t a, uint64_t b, uint64_t c, uam_status *st);
uint64_t   ppc_helper_rfmci(void *env, uint64_t a, uint64_t b, uint64_t c, uam_status *st);
uint64_t   ppc_helper_rfdi(void *env, uint64_t a, uint64_t b, uint64_t c, uam_status *st);
uint64_t   ppc_helper_tlbwe(void *env, uint64_t insn, uint64_t b, uint64_t c, uam_status *st);
uint64_t   ppc_helper_tlbre(void *env, uint64_t insn, uint64_t b, uint64_t c, uam_status *st);
uint64_t   ppc_helper_tlbsx(void *env, uint64_t insn, uint64_t ea, uint64_t c, uam_status *st);
uint64_t   ppc_helper_tlbivax(void *env, uint64_t ea, uint64_t b, uint64_t c, uam_status *st);
uint64_t   ppc_helper_mfdcr(void *env, uint64_t dcrn, uint64_t b, uint64_t c, uam_status *st);
uint64_t   ppc_helper_mtdcr(void *env, uint64_t dcrn, uint64_t v, uint64_t c, uam_status *st);

#endif /* UAM_PPC_INTERNAL_H */
