/*
 * xtensa.h - Internal definitions shared by the Xtensa lifter and system code.
 * Not part of the public interface.
 */
#ifndef UAM_XTENSA_INTERNAL_H
#define UAM_XTENSA_INTERNAL_H

#include "uam/isa/xtensa.h"
#include "uam/sched.h"

/*
 * Register file (32-bit words, each at a fixed offset).
 *
 * A0-A15 are the current window: with the Windowed Register option the physical file
 * (AR0-AR63) is the authority for every other frame, and the 16 registers WindowBase
 * selects live at A0-A15 while the hart runs; a window rotation (ENTRY, RETW, ROTW, a
 * window exception or its return, a write of WINDOWBASE) puts them back and loads the
 * new ones (xt_window_rotate), so lifted code always reads the current window at fixed
 * offsets. Without windows A0-A15 are the whole file.
 *
 * WCHECK holds, for the window-overflow test of each instruction, bit i (i = 1..3) =
 * WindowStart[WindowBase + i] when overflow detection is on (PS.WOE and not PS.EXCM),
 * else 0: an instruction that names a register of window group g (a4.. = 1, a8.. = 2,
 * a12.. = 3) overflows when WCHECK & (bits 1..g) is not 0.
 *
 * LENDA is LEND while a loop-back can happen (LCOUNT != 0 and not PS.EXCM), else
 * all ones; system code keeps it in step with LEND, LCOUNT and PS.EXCM (LOOPEN mirrors
 * the latter for the lifted LOOP instructions), and the lifted loop-back writes it when
 * LCOUNT reaches 0.
 */
#define XT_A(n)      (4u * (n))     /* 0..60 */
#define XT_SAR       64u
#define XT_LBEG      68u
#define XT_LEND      72u
#define XT_LCOUNT    76u
#define XT_LENDA     80u
#define XT_BR        84u            /* b0-b15, bit n = bn */
#define XT_SCOMPARE1 88u
#define XT_ACCLO     92u
#define XT_ACCHI     96u            /* ACC[39:32] in the low byte, kept sign-extended */
#define XT_M(n)      (100u + 4u * (n))
#define XT_THREADPTR 116u
#define XT_LITBASE   120u
#define XT_FCR       124u
#define XT_FSR       128u
#define XT_F(n)      (132u + 4u * (n))   /* 132..192 */
#define XT_WCHECK    196u
#define XT_CALLINC   200u           /* PS.CALLINC, 0..3 */
#define XT_CPENABLE  204u
#define XT_LOOPEN    208u           /* 1 while PS.EXCM is clear (loop-backs happen) */
#define XT_AR(n)     (256u + 4u * (n))   /* physical AR0..AR63 (windowed cores) */
#define XT_REGFILE_SIZE (256u + 4u * 64u)

/* PS fields. */
#define PS_INTLEVEL 0x0000000Fu
#define PS_EXCM     0x00000010u
#define PS_UM       0x00000020u
#define PS_RING     0x000000C0u
#define PS_OWB_SH   8
#define PS_OWB      0x00000F00u
#define PS_CALLINC_SH 16
#define PS_CALLINC  0x00030000u
#define PS_WOE      0x00040000u
#define PS_MASK     0x00070FFFu

/* EXCCAUSE values. */
enum {
    XT_EXC_ILLEGAL = 0, XT_EXC_SYSCALL = 1, XT_EXC_IFETCH_ERR = 2, XT_EXC_LS_ERR = 3,
    XT_EXC_LEVEL1 = 4, XT_EXC_ALLOCA = 5, XT_EXC_DIVZERO = 6, XT_EXC_PRIV = 8,
    XT_EXC_ALIGN = 9, XT_EXC_IFETCH_PROHIB = 20, XT_EXC_LOAD_PROHIB = 28,
    XT_EXC_STORE_PROHIB = 29, XT_EXC_CP_DISABLED = 32,   /* + n */
};

/* TRAP EXCEPTION causes the lifter raises (cause operand). Below 64 they are EXCCAUSE
 * values taken through the general vector; above, events with their own entry. */
enum {
    XT_TRAP_WOVF  = 64,    /* window overflow: take_exception picks the vector */
    XT_TRAP_BREAK = 65,    /* BREAK (tval: DEBUGCAUSE bit | length << 8) */
    XT_TRAP_WUNF  = 66,    /* window underflow (pend_cause only: RETW's helper) */
};

/* System register ids for SYSRD/SYSWR: special registers by number (0-255), and the
 * ISA's own operations above. */
enum {
    XT_SYS_STEP = 512,     /* write 1: lift the next block one instruction at a time; 0: stop */
    XT_SYS_RSIL,           /* write: set PS.INTLEVEL (RSIL reads PS first) */
    XT_SYS_WAITI,          /* write: set PS.INTLEVEL, then the hart waits */
    XT_SYS_RFE, XT_SYS_RFDE, XT_SYS_RFWO, XT_SYS_RFWU,   /* read: the return pc */
    XT_SYS_RFI,            /* write: the level (PS = EPSn; XT_SYS_RET_PC = EPCn) */
    XT_SYS_RET_PC,         /* read: the pc the last RFI chose */
    XT_SYS_ROTW,           /* write: the rotation (sign-extended) */
};

/* cpu->priv: XT_PRIV_STEP lifts blocks of one instruction (a loop end the block was not
 * lifted for, see the lifter). */
#define XT_PRIV_STEP 1u

typedef struct xt_state {
    uam_xtensa_config cfg;
    unsigned endian;                 /* UAM_ACC_LE or UAM_ACC_BE */
    uint32_t ps;                     /* PS except CALLINC, which is the register file's */
    uint32_t wb, ws;                 /* WindowBase, WindowStart */
    uint32_t epc[8], eps[8], excsave[8];
    uint32_t depc, exccause, excvaddr, vecbase;
    uint32_t intr;                   /* INTERRUPT: latched bits (edge, software, timer) */
    uint32_t lines;                  /* input line levels */
    uint32_t intenable;
    uint32_t ccount_bias;
    uint32_t ccompare[3];
    uam_timer timer[3];
    uint32_t atomctl, memctl, misc[4], prid, debugcause, icount, icountlevel;
    uint32_t ibreakenable, ibreaka[2], dbreaka[2], dbreakc[2], ddr;
    uint8_t  itlb[8], dtlb[8];       /* region attributes */
    bool     nmi_pending;
    uint32_t ret_pc;                 /* XT_SYS_RET_PC */
    /* A helper's or system register's exception: cause and EXCVADDR (all ones: none). */
    uint32_t pend_cause, pend_vaddr;
    unsigned pend_n;                 /* XT_TRAP_WUNF: the caller's window increment */
    uint64_t taken_lines;            /* irq_lines */
    uam_xtensa_eri_read_fn  eri_rd;
    uam_xtensa_eri_write_fn eri_wr;
    void    *eri_ctx;
    struct xt_timer_ctx { uam_cpu *cpu; unsigned n; } tctx[3];
} xt_state;

static inline xt_state *xt_st(uam_cpu *cpu) { return (xt_state *)cpu->isa_state; }

static inline uint32_t xt_reg(const uam_cpu *cpu, uint32_t off)
{
    return (uint32_t)uam_cpu_reg_read(cpu, off, 32);
}

static inline void xt_set_reg(uam_cpu *cpu, uint32_t off, uint32_t v)
{
    uam_cpu_reg_write(cpu, off, 32, v);
}

/* xtensa_decode.c */
struct xt_opc;
unsigned xt_insn_len(const uam_xtensa_config *cfg, unsigned op0);
uint32_t xt_be_to_le(uint32_t w, unsigned len);
const struct xt_opc *xt_decode(const uam_xtensa_config *cfg, uint32_t w, unsigned len);
const char *xt_opname(unsigned id);

/* xtensa_lift.c */
uam_status xt_lift(uam_lift_ctx *lc);

/* xtensa_system.c */
extern const uam_helper_table xt_helpers;
void xt_window_rotate(uam_cpu *cpu, uint32_t new_wb);
void xt_update_derived(uam_cpu *cpu);   /* WCHECK, LENDA, CPENABLE copy */
/* Does special register sr exist on the model (for RSR, or for WSR/XSR)? */
bool xt_sr_ok(const uam_xtensa_config *cfg, unsigned sr, bool write, bool xsr);

/* xtensa_fpu.c: one FP coprocessor instruction (id: enum xt_opid; insn: the
 * little-endian instruction word) on the register file. */
uam_status xt_fpu_exec(uam_cpu *cpu, unsigned id, uint32_t insn);

/* Helper ids. */
enum {
    XT_H_ENTRY,       /* a: s | imm12 << 4; rotates the window (may fail: overflow) */
    XT_H_RETW,        /* a: pc; returns the target (may fail: underflow) */
    XT_H_MOVSP,       /* fails with Alloca when the caller's frame is not resident */
    XT_H_FPU,         /* a: the instruction word, b: its opcode id */
    XT_H_TLB,         /* a: id, b: AR[t], c: AR[s]; region protection (returns RxTLB) */
    XT_H_ERI_RD,      /* a: address */
    XT_H_ERI_WR,      /* a: address, b: value */
    XT_H__COUNT
};

#endif /* UAM_XTENSA_INTERNAL_H */
