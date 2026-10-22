/*
 * x86.h - Internal definitions shared by the x86-64 decoder, lifter and system
 * code. Not part of the public interface.
 *
 * Register file layout (IR-visible state, reached by GET/PUT):
 *
 *   off 0   : 16 GPRs, 8 bytes each (rax rcx rdx rbx rsp rbp rsi rdi r8..r15),
 *             indexed by the architectural register number.
 *   off 128 : 6 segment caches, 16 bytes each (ES CS SS DS FS GS), each
 *             { base:u64, limit:u32, sel:u16, attr:u16 }. In real mode
 *             base = sel << 4; protected/long mode load it from a descriptor.
 *             attr is the descriptor's access byte (bits 0-7) and flags nibble
 *             (bits 12-15: AVL L D/B G), the layout of a descriptor's bytes 5-6.
 *   off 224 : 8 flag bytes (CF PF AF ZF SF OF DF IF), the way ARM keeps NZCV.
 *             Each is 0 or 1, except PF, which holds the low byte of the last
 *             result: PF is its even parity, computed only when read (Jp,
 *             PUSHF), so arithmetic does not pay for a population count.
 *   off 232 : the remaining RFLAGS bits (TF IOPL NT RF VM AC VIF VIP ID) as an
 *             image; only system instructions touch them.
 *   off 256 : 16 XMM registers, 16 bytes each (low quadword first).
 *   off 512 : MXCSR (u32), x87 control word (u16), status word (u16), tag (u8).
 *
 * RIP is not in the register file: cpu->pc is the *linear* address of the
 * next instruction (CS.base + RIP), so translated code is keyed by where the
 * bytes are. RIP = pc - CS.base is recovered where software can observe it
 * (CALL, exception frames, the debugger). Privileged and rarely-touched state
 * (CR0/2/3/4/8, EFER, XCR0, the descriptor-table registers, the MSRs, and the
 * SMM save state) lives in x86_state and is reached by SYSRD/SYSWR and
 * helpers, never by GET/PUT.
 */
#ifndef UAM_X86_INTERNAL_H
#define UAM_X86_INTERNAL_H

#include "uam/bus.h"
#include "uam/isa/x86.h"
#include "uam/sched.h"

/* ---- register file layout ------------------------------------------------ */

#define X86_OFF_GPR(i)    ((uint32_t)(i) * 8u)       /* 0..127 */
#define X86_NGPR          16u
enum { X86_RAX = 0, X86_RCX, X86_RDX, X86_RBX, X86_RSP, X86_RBP, X86_RSI, X86_RDI,
       X86_R8, X86_R9, X86_R10, X86_R11, X86_R12, X86_R13, X86_R14, X86_R15 };

/* Segment caches. seg indices use the x86 sreg encoding. */
enum { X86_SEG_ES = 0, X86_SEG_CS = 1, X86_SEG_SS = 2, X86_SEG_DS = 3,
       X86_SEG_FS = 4, X86_SEG_GS = 5, X86_NSEG = 6 };
#define X86_OFF_SEG(s)      (128u + (uint32_t)(s) * 16u)
#define X86_OFF_SEG_BASE(s) (X86_OFF_SEG(s) + 0u)    /* u64 */
#define X86_OFF_SEG_LIMIT(s)(X86_OFF_SEG(s) + 8u)    /* u32 byte-granular limit */
#define X86_OFF_SEG_SEL(s)  (X86_OFF_SEG(s) + 12u)   /* u16 selector */
#define X86_OFF_SEG_ATTR(s) (X86_OFF_SEG(s) + 14u)   /* u16, see above */

/* Segment attribute bits (descriptor bytes 5-6). */
enum {
    SEGA_TYPE   = 0x000Fu,   /* type field; bit 0 accessed, bit 3 code */
    SEGA_S      = 0x0010u,   /* code/data (1) vs system (0) */
    SEGA_DPL    = 0x0060u,
    SEGA_P      = 0x0080u,   /* present */
    SEGA_AVL    = 0x1000u,
    SEGA_L      = 0x2000u,   /* 64-bit code segment */
    SEGA_DB     = 0x4000u,   /* default operand size 32 / stack 32 */
    SEGA_G      = 0x8000u,   /* 4 KiB granularity */
};

/* Flag bytes. */
enum { X86_F_CF = 0, X86_F_PF, X86_F_AF, X86_F_ZF, X86_F_SF, X86_F_OF, X86_F_DF, X86_F_IF,
       X86_NFLAG };
#define X86_OFF_FLAG(f)   (224u + (uint32_t)(f))
#define X86_OFF_RFLAGS_HI 232u    /* u32: RFLAGS bits not kept as flag bytes */

#define X86_OFF_XMM(i)    (256u + (uint32_t)(i) * 16u)
#define X86_OFF_MXCSR     512u
#define X86_OFF_FCW       516u
#define X86_OFF_FSW       518u
#define X86_OFF_FTW       520u

#define X86_REGFILE_SIZE  528u

/* Bit positions in an assembled RFLAGS image. */
enum {
    RFLAGS_CF = 1u << 0,  RFLAGS_PF = 1u << 2,  RFLAGS_AF = 1u << 4,
    RFLAGS_ZF = 1u << 6,  RFLAGS_SF = 1u << 7,  RFLAGS_TF = 1u << 8,
    RFLAGS_IF = 1u << 9,  RFLAGS_DF = 1u << 10, RFLAGS_OF = 1u << 11,
    RFLAGS_IOPL = 3u << 12, RFLAGS_NT = 1u << 14, RFLAGS_RF = 1u << 16,
    RFLAGS_VM = 1u << 17, RFLAGS_AC = 1u << 18, RFLAGS_VIF = 1u << 19,
    RFLAGS_VIP = 1u << 20, RFLAGS_ID = 1u << 21,
};
#define RFLAGS_ALWAYS_1   (1u << 1)   /* bit 1 reads as 1 */
#define RFLAGS_HI_MASK    (RFLAGS_TF | RFLAGS_IOPL | RFLAGS_NT | RFLAGS_RF | RFLAGS_VM | \
                           RFLAGS_AC | RFLAGS_VIF | RFLAGS_VIP | RFLAGS_ID)

/* ---- operating mode ------------------------------------------------------ */

/*
 * cpu->priv encodes everything decoding and translation depend on, so
 * translated code is never reused across a change of any of them:
 *   bits 0-1  CPL
 *   bits 2-3  mode (real / protected / IA-32e / virtual-8086)
 *   bit  4    SMM
 *   bits 5-6  default code size: 0 = 16, 1 = 32, 2 = 64 (CS.D / CS.L)
 *   bit  7    32-bit stack (SS.B), outside 64-bit code
 *   bit  8    paging enabled (CR0.PG)
 *   bit  9    A20M# in effect (physical address bit 20 forced to 0)
 */
enum x86_mode {
    X86_MODE_REAL = 0,   /* real mode (reset state) */
    X86_MODE_PROT,       /* protected mode */
    X86_MODE_LONG,       /* IA-32e: 64-bit or compatibility submode */
    X86_MODE_V86,        /* virtual-8086: protected mode with EFLAGS.VM, CPL 3 */
};
enum { X86_CODE16 = 0, X86_CODE32 = 1, X86_CODE64 = 2 };

#define X86_PRIV_MODE_SHIFT 2
#define X86_PRIV_SMM        (1u << 4)
#define X86_PRIV_CODE_SHIFT 5
#define X86_PRIV_STACK32    (1u << 7)
#define X86_PRIV_PG         (1u << 8)
#define X86_PRIV_A20M       (1u << 9)

static inline enum x86_mode x86_priv_mode(unsigned priv)
{
    return (enum x86_mode)((priv >> X86_PRIV_MODE_SHIFT) & 3u);
}
static inline unsigned x86_priv_cpl(unsigned priv)   { return priv & 3u; }
/* Segment registers load as in real mode (base = selector << 4): real and
 * virtual-8086 mode. */
static inline bool x86_priv_realseg(unsigned priv)
{
    return x86_priv_mode(priv) == X86_MODE_REAL || x86_priv_mode(priv) == X86_MODE_V86;
}
static inline bool x86_priv_smm(unsigned priv)       { return (priv & X86_PRIV_SMM) != 0; }
static inline unsigned x86_priv_code(unsigned priv)  { return (priv >> X86_PRIV_CODE_SHIFT) & 3u; }
/* stack-pointer width in bits */
static inline unsigned x86_priv_stack(unsigned priv)
{
    if (x86_priv_code(priv) == X86_CODE64) return 64;
    return (priv & X86_PRIV_STACK32) ? 32 : 16;
}

/* ---- control-register, EFER and other architectural bits ----------------- */

enum {
    CR0_PE = 1u << 0, CR0_MP = 1u << 1, CR0_EM = 1u << 2, CR0_TS = 1u << 3,
    CR0_ET = 1u << 4, CR0_NE = 1u << 5, CR0_WP = 1u << 16, CR0_AM = 1u << 18,
    CR0_NW = 1u << 29, CR0_CD = 1u << 30, CR0_PG = 1u << 31,
};
enum {
    CR4_VME = 1u << 0, CR4_PVI = 1u << 1, CR4_TSD = 1u << 2, CR4_DE = 1u << 3,
    CR4_PSE = 1u << 4, CR4_PAE = 1u << 5, CR4_MCE = 1u << 6, CR4_PGE = 1u << 7,
    CR4_PCE = 1u << 8, CR4_OSFXSR = 1u << 9, CR4_OSXMMEXCPT = 1u << 10,
    CR4_LA57 = 1u << 12, CR4_FSGSBASE = 1u << 16, CR4_PCIDE = 1u << 17,
    CR4_OSXSAVE = 1u << 18, CR4_SMEP = 1u << 20, CR4_SMAP = 1u << 21,
};
enum { EFER_SCE = 1u << 0, EFER_LME = 1u << 8, EFER_LMA = 1u << 10, EFER_NXE = 1u << 11 };

/* MSR numbers the core implements itself. */
enum {
    MSR_TSC              = 0x00000010,
    MSR_PLATFORM_ID      = 0x00000017,
    MSR_APIC_BASE        = 0x0000001B,
    MSR_FEATURE_CONTROL  = 0x0000003A,
    MSR_TSC_ADJUST       = 0x0000003B,
    MSR_BIOS_SIGN_ID     = 0x0000008B,
    MSR_SMBASE           = 0x0000009E,
    MSR_MTRRCAP          = 0x000000FE,
    MSR_SMRR_PHYSBASE    = 0x000001F2,
    MSR_SMRR_PHYSMASK    = 0x000001F3,
    MSR_SYSENTER_CS      = 0x00000174,
    MSR_SYSENTER_ESP     = 0x00000175,
    MSR_SYSENTER_EIP     = 0x00000176,
    MSR_MCG_CAP          = 0x00000179,
    MSR_MCG_STATUS       = 0x0000017A,
    MSR_MISC_ENABLE      = 0x000001A0,
    MSR_MTRR_PHYSBASE0   = 0x00000200,
    MSR_MTRR_DEF_TYPE    = 0x000002FF,
    MSR_PAT              = 0x00000277,
    MSR_EFER             = 0xC0000080,
    MSR_STAR             = 0xC0000081,
    MSR_LSTAR            = 0xC0000082,
    MSR_CSTAR            = 0xC0000083,
    MSR_FMASK            = 0xC0000084,
    MSR_FS_BASE          = 0xC0000100,
    MSR_GS_BASE          = 0xC0000101,
    MSR_KERNEL_GS_BASE   = 0xC0000102,
    MSR_TSC_AUX          = 0xC0000103,
};

/* ---- exception vectors --------------------------------------------------- */

enum {
    X86_EXC_DE = 0,   /* divide error */
    X86_EXC_DB = 1,   /* debug */
    X86_EXC_NMI = 2,
    X86_EXC_BP = 3,   /* int3 */
    X86_EXC_OF = 4,   /* into */
    X86_EXC_BR = 5,   /* bound */
    X86_EXC_UD = 6,   /* invalid opcode */
    X86_EXC_NM = 7,   /* device not available */
    X86_EXC_DF = 8,   /* double fault */
    X86_EXC_TS = 10,  /* invalid TSS */
    X86_EXC_NP = 11,  /* segment not present */
    X86_EXC_SS = 12,  /* stack fault */
    X86_EXC_GP = 13,  /* general protection */
    X86_EXC_PF = 14,  /* page fault */
    X86_EXC_MF = 16,  /* x87 FP */
    X86_EXC_AC = 17,  /* alignment check */
    X86_EXC_MC = 18,  /* machine check */
    X86_EXC_XM = 19,  /* SIMD FP */
};

/*
 * TRAP EXCEPTION cause encoding. Bits 0-7: vector. X86_TRAP_ERR: bits 32-63
 * carry an error code to push. X86_TRAP_SOFT: a software interrupt (INT n,
 * INT3, INTO, INT1): it returns past the instruction, whose next linear pc is
 * the trap's tval, and INT n checks the gate's DPL.
 */
#define X86_TRAP_ERR    (1ull << 8)
#define X86_TRAP_SOFT   (1ull << 9)
#define X86_TRAP_INTN   (1ull << 10)   /* INT n (with SOFT): gate DPL check applies */

/* #PF error-code bits; also the uam_xlate.fault sub-cause of a translation fault. */
enum { PFEC_P = 1u << 0, PFEC_W = 1u << 1, PFEC_U = 1u << 2, PFEC_RSVD = 1u << 3,
       PFEC_I = 1u << 4 };

/* ---- system-register ids (SYSRD/SYSWR aux) ------------------------------- */

enum {
    X86_SR_CR0 = 0, X86_SR_CR2, X86_SR_CR3, X86_SR_CR4, X86_SR_CR8,
    X86_SR_DR0, X86_SR_DR1, X86_SR_DR2, X86_SR_DR3, X86_SR_DR4, X86_SR_DR5,
    X86_SR_DR6, X86_SR_DR7,
    X86_SR_RFLAGS,       /* PUSHF (read) / POPF (write; privilege rules applied) */
    X86_SR_RFLAGS_INT,   /* write: IRET-style unrestricted RFLAGS write (helpers) */
    X86_SR_STI,          /* write: STI (IOPL check); value ignored */
    X86_SR_CLI,          /* write: CLI (IOPL check); value ignored */
    X86_SR_GDTR_BASE, X86_SR_GDTR_LIMIT, X86_SR_IDTR_BASE, X86_SR_IDTR_LIMIT,
    X86_SR_LDTR, X86_SR_TR,                  /* SLDT / STR selectors (read) */
    X86_SR_MSW,          /* SMSW read / LMSW write */
    X86_SR_CLTS,         /* write: clear CR0.TS */
    X86_SR_INVLPG,       /* write: invalidate the page of the linear address */
    X86_SR_TSC,          /* RDTSC */
    X86_SR_TSC_AUX,      /* RDTSCP / RDPID */
    X86_SR_XCR0,         /* XGETBV / XSETBV with ECX = 0 */
    X86_SR_RDRAND,       /* RDRAND / RDSEED */
    X86_SR_CPL0_CHECK,   /* read: faults #GP unless CPL 0 (HLT, INVD, ...) */
    X86_SR__COUNT
};

/* An I/O instruction as the SMRAM save state describes it. */
typedef struct x86_io {
    uint64_t misc;      /* IOMisc: SMI flag, length (1/2/4) << 1, type << 4, port << 16 */
    uint64_t rip;       /* the instruction's RIP */
    uint64_t mem;       /* INS/OUTS: the linear memory address */
    uint64_t rcx, rsi, rdi;   /* before the instruction (string restart) */
} x86_io;

/* IOMisc instruction types */
enum { X86_IO_OUT_DX = 0x0, X86_IO_IN_DX = 0x1, X86_IO_OUTS = 0x2, X86_IO_INS = 0x3,
       X86_IO_REP = 0x4, X86_IO_OUT_IMM = 0x8, X86_IO_IN_IMM = 0x9 };

/* ---- helper ids (index into x86_helpers) --------------------------------- */

enum {
    X86_H_LOAD_SEG = 0,  /* (seg, sel): MOV/POP Sreg, LDS..., protected mode */
    X86_H_FAR_JMP,       /* (sel, off, -) -> new linear pc */
    X86_H_FAR_CALL,      /* (sel | osz << 16, off, return rip) -> new linear pc */
    X86_H_RETF,          /* (osz, imm16) -> new linear pc */
    X86_H_IRET,          /* (osz) -> new linear pc */
    X86_H_CPUID,         /* reads EAX/ECX, writes EAX..EDX */
    X86_H_RDMSR,         /* ECX -> EDX:EAX */
    X86_H_WRMSR,         /* EDX:EAX -> MSR[ECX] */
    X86_H_LLDT,          /* (sel) */
    X86_H_LTR,           /* (sel) */
    X86_H_RSM,           /* -> new linear pc */
    X86_H_DIVQ,          /* (divisor): RDX:RAX / divisor, unsigned, 64-bit */
    X86_H_IDIVQ,         /* (divisor): signed */
    X86_H_FXSAVE,        /* (linear address) */
    X86_H_FXRSTOR,       /* (linear address) */
    X86_H_LAR,           /* (sel, is_lsl) -> value; sets ZF */
    X86_H_SYSCALL,       /* (next linear pc) -> new linear pc */
    X86_H_SYSRET,        /* (osz) -> new linear pc */
    X86_H_SWAPGS,
    X86_H_SSE,           /* (descriptor, linear address of a memory operand): see x86_sse.c */
    X86_H_X87,           /* (descriptor, linear address): the x87 control/status subset */
    X86_H_IO,            /* (IOMisc, rip, memory address): INS/OUTS begin their access */
    X86_H_IO_PORT,       /* (IOMisc, rip, value) -> value: IN/OUT, recorded and done */
    X86_H__COUNT
};
extern const uam_helper_desc x86_helpers[X86_H__COUNT];

/*
 * X86_H_SSE descriptor: everything the helper needs to execute one SSE
 * instruction that the lifter does not inline. The memory operand (if any)
 * is the helper's second argument, as a linear address.
 */
enum { X86_PFX_NONE = 0, X86_PFX_66, X86_PFX_F3, X86_PFX_F2 };
#define X86_SSE_DESC(op, map, pfx, reg, rm, mem, rexw, imm8)                       \
    ((uint64_t)(op) | (uint64_t)(map) << 8 | (uint64_t)(pfx) << 10 |               \
     (uint64_t)(reg) << 12 | (uint64_t)(rm) << 16 | (uint64_t)(mem) << 20 |        \
     (uint64_t)(rexw) << 21 | (uint64_t)(imm8) << 24)
#define X86_SSE_OP(d)   ((unsigned)(d) & 0xFFu)
#define X86_SSE_MAP(d)  ((unsigned)((d) >> 8) & 3u)
#define X86_SSE_PFX(d)  ((unsigned)((d) >> 10) & 3u)
#define X86_SSE_REG(d)  ((unsigned)((d) >> 12) & 15u)
#define X86_SSE_RM(d)   ((unsigned)((d) >> 16) & 15u)
#define X86_SSE_MEM(d)  (((d) >> 20) & 1u)
#define X86_SSE_REXW(d) (((d) >> 21) & 1u)
#define X86_SSE_IMM(d)  ((unsigned)((d) >> 24) & 0xFFu)

/* X86_H_X87 descriptor: opcode byte (D8-DF, 9B) | ModRM << 8 | mem << 16. */
#define X86_X87_DESC(op, modrm, mem) ((uint64_t)(op) | (uint64_t)(modrm) << 8 | (uint64_t)(mem) << 16)

/* ---- ISA-private hart state ---------------------------------------------- */

typedef struct x86_dtr { uint64_t base; uint32_t limit; } x86_dtr;
typedef struct x86_sysseg { uint64_t base; uint32_t limit; uint16_t sel, attr; } x86_sysseg;

typedef struct x86_msr_ent { uint32_t msr; uint64_t val; } x86_msr_ent;

/* A tiny direct-mapped cache of page walks (non-architectural; flushed with
 * every uam_cpu_xlate_changed() through xlate_gen). */
#define X86_TLB_SIZE 256u
typedef struct x86_tlb_ent {
    uint64_t vpage;      /* linear page number, UINT64_MAX = invalid */
    uint64_t ppage;      /* physical page base */
    uint8_t  perm;       /* bit0 writable, bit1 user, bit2 executable */
    uint32_t gen;
} x86_tlb_ent;

typedef struct x86_state {
    uint64_t cr0, cr2, cr3, cr4, cr8;
    uint64_t efer, xcr0;
    uint64_t dr[8];
    x86_dtr  gdtr, idtr;
    x86_sysseg ldtr, tr;
    uint64_t smbase;              /* SMRAM base; reset value 0x30000 */
    uint64_t smrr_base, smrr_mask;/* IA32_SMRR_PHYSBASE / PHYSMASK (bit 11: valid) */
    uint64_t apic_base;           /* IA32_APIC_BASE */
    uint64_t kernel_gs_base, star, lstar, cstar, fmask, tsc_aux, pat;
    uint64_t sysenter_cs, sysenter_esp, sysenter_eip;
    uint64_t misc_enable, feature_control;
    uint64_t tsc_offset;          /* TSC = icount + tsc_offset */
    uint64_t mtrr_def_type;
    uint64_t rng;                 /* RDRAND state (deterministic) */

    /* interrupts */
    uint32_t irq_lines;           /* input-line levels, by uam_x86_irq_line */
    bool     nmi_pending, smi_pending, init_pending;
    bool     nmi_blocked;         /* in an NMI handler, until IRET */
    bool     in_smm;
    bool     halted_in_smm;       /* auto-HALT restart in the save state */
    bool     shutdown;            /* triple fault: the hart stops */
    bool     wait_sipi;           /* an application processor waiting for a startup IPI */
    /* SMM I/O state (I/O instruction restart): the I/O instruction executing
     * (recorded by X86_H_IO, valid while icount == io_icount), what an SMI
     * it raised latched, and what the SMM handler may restart */
    x86_io   io_cur, io_smi, io_smm;
    uint64_t io_icount;
    bool     smi_io, smm_io;
    bool     sipi_pending;
    uint8_t  sipi_vector;

    /* a fault raised by a helper or SYSRD/SYSWR: delivered by take_exception */
    bool     pend_valid;
    uint8_t  pend_vec;
    bool     pend_has_err;
    uint32_t pend_err;
    /* ... raised after a task switch committed: it belongs to the new task,
     * whose instruction at pend_pc it is reported at */
    bool     pend_in_task;
    uint64_t pend_pc;
    unsigned exc_depth;           /* nesting while delivering (for #DF) */

    /* MSRs nobody implements, stored per the policy */
    x86_msr_ent *msrs; size_t n_msrs, cap_msrs;
    uam_x86_msr_policy msr_policy;
    bool     msr_trace;

    /* x87 register stack, by physical register number (ST(i) is
     * fpr[(TOP + i) & 7]); FCW/FSW/FTW live in the register file. */
    long double fpr[8];
    uint64_t    fpu_ip, fpu_dp;   /* last instruction / operand pointers (FNSTENV) */
    uint16_t    fpu_op;
    /* MMX registers. They alias the x87 registers' significands on hardware;
     * here they are kept apart and FXSAVE/FXRSTOR carry them in the x87 slots
     * while MMX is in use (`mmx`: an MMX instruction ran since EMMS) */
    uint64_t    mm[8];
    bool        mmx;

    uam_x86_platform   plat;
    uam_x86_cpuid_info cpuid;

    x86_tlb_ent tlb[X86_TLB_SIZE];
} x86_state;

static inline x86_state *x86_st(uam_cpu *cpu) { return (x86_state *)cpu->isa_state; }

/* ---- shared functions ---------------------------------------------------- */

/* lift entry point (x86_lift.c) */
uam_status x86_lift(uam_lift_ctx *lc);

/* registers (x86_system.c) */
uint32_t x86_get_rflags(uam_cpu *cpu);
void     x86_set_rflags(uam_cpu *cpu, uint32_t v);   /* every bit, no checks */
void     x86_update_priv(uam_cpu *cpu);              /* recompute cpu->priv from state */
uint64_t x86_seg_base(uam_cpu *cpu, unsigned seg);
uint16_t x86_seg_sel(uam_cpu *cpu, unsigned seg);
uint16_t x86_seg_attr(uam_cpu *cpu, unsigned seg);
void     x86_seg_set(uam_cpu *cpu, unsigned seg, uint16_t sel, uint64_t base,
                     uint32_t limit, uint16_t attr);
void     x86_seg_set_real(uam_cpu *cpu, unsigned seg, uint16_t sel);
/* The time-stamp counter before the offset: virtual time (one count per
 * tick, idle time included), or the instruction count without a scheduler. */
static inline uint64_t x86_tsc_base(const uam_cpu *cpu)
{
    return cpu->sched ? uam_sched_now(cpu->sched) : cpu->icount;
}

static inline uint64_t x86_gpr(uam_cpu *cpu, unsigned r) { return uam_cpu_reg_read(cpu, X86_OFF_GPR(r), 64); }
static inline void x86_set_gpr(uam_cpu *cpu, unsigned r, uint64_t v) { uam_cpu_reg_write(cpu, X86_OFF_GPR(r), 64, v); }
/* current rIP (pc minus CS base) */
static inline uint64_t x86_rip(uam_cpu *cpu) { return cpu->pc - x86_seg_base(cpu, X86_SEG_CS); }

/* Record a fault for take_exception (helpers / sysreg handlers); returns UAM_ERR_FAULT. */
uam_status x86_raise(uam_cpu *cpu, unsigned vec, bool has_err, uint32_t err);

/* exceptions and interrupts (x86_system.c) */
void x86_deliver(uam_cpu *cpu, unsigned vec, bool has_err, uint32_t err,
                 bool soft, uint64_t return_pc, bool intn);

/* paging and linear-address memory access from C (x86_mmu.c) */
uam_status x86_translate(uam_cpu *cpu, uint64_t vaddr, uam_xlate_kind kind, uam_xlate *out);
/* Access `size` bytes at a linear address, translating each page. `kind` is LOAD,
 * STORE or DEBUG; `sup` forces a supervisor access (descriptor tables, the
 * interrupt stack frame). On a translation fault, records #PF (CR2 is set by
 * delivery) and returns UAM_ERR_FAULT. */
uam_status x86_lin_read(uam_cpu *cpu, uint64_t lin, unsigned size, uint64_t *val, bool sup);
uam_status x86_lin_write(uam_cpu *cpu, uint64_t lin, unsigned size, uint64_t val, bool sup);
void       x86_tlb_flush(uam_cpu *cpu);
/* A physical address as the processor emits it: bit 20 cleared under A20M#,
 * and the SMRR range outside SMM goes to UAM_X86_SMRR_SINK. */
uint64_t   x86_smrr_filter(uam_cpu *cpu, uint64_t pa);

/* descriptors and far transfers (x86_seg.c) */
uam_status x86_load_seg(uam_cpu *cpu, unsigned seg, uint16_t sel);   /* MOV/POP Sreg rules */
/* Load a segment register from its descriptor with only the present check
 * (interrupt delivery, far transfers after their own checks). */
uam_status x86_load_seg_nocheck(uam_cpu *cpu, unsigned seg, uint16_t sel);
/* Read the 8-byte (or, `wide`, 16-byte system) descriptor `sel` names in
 * the GDT or LDT; #GP(sel) if it lies outside the table. */
uam_status x86_read_desc(uam_cpu *cpu, uint16_t sel, uint64_t *lo, uint64_t *hi, bool wide);
void       x86_desc_unpack(uint64_t d, uint64_t *base, uint32_t *limit, uint16_t *attr);
/* Far transfers; each returns the new linear pc. */
/* next_lin: the linear pc after the JMP (where a task switch saves the old task). */
uam_status x86_far_jmp(uam_cpu *cpu, uint16_t sel, uint64_t off, uint64_t next_lin, uint64_t *pc);
uam_status x86_far_call(uam_cpu *cpu, uint16_t sel, unsigned osz, uint64_t off,
                        uint64_t ret_rip, uint64_t *pc);
uam_status x86_retf(uam_cpu *cpu, unsigned osz, unsigned imm16, uint64_t *pc);
uam_status x86_iret(uam_cpu *cpu, unsigned osz, uint64_t next_lin, uint64_t *pc);
uam_status x86_lldt(uam_cpu *cpu, uint16_t sel);
uam_status x86_ltr(uam_cpu *cpu, uint16_t sel);
/* The inner stack for `cpl` (or 64-bit IST slot `ist`) from the current TSS:
 * 64-bit, 32-bit or 16-bit. */
uam_status x86_tss_stack(uam_cpu *cpu, bool lm, unsigned cpl, unsigned ist, uint64_t *sp,
                         uint16_t *ss);
/* Null DS/ES/FS/GS where they name data the new (outer) CPL may not use. */
void       x86_null_outer_segs(uam_cpu *cpu);

/* task switches and the I/O permission bitmap (x86_task.c) */
enum x86_task_reason { X86_TASK_JMP, X86_TASK_CALL, X86_TASK_IRET, X86_TASK_INT };
/* Switch to the task whose TSS selector is `sel` (already found through a
 * gate, if any). `ret` is the linear pc the old task resumes at. For an
 * exception through a task gate, has_err/err is pushed on the new task's
 * stack. On success *pc is the new task's linear pc. */
uam_status x86_task_switch(uam_cpu *cpu, uint16_t sel, enum x86_task_reason why, uint64_t ret,
                           bool has_err, uint32_t err, uint64_t *pc);
/* IN/OUT/INS/OUTS permission: CPL <= IOPL outside virtual-8086 mode, else
 * the TSS I/O permission bitmap; #GP(0) if denied. */
uam_status x86_io_permitted(uam_cpu *cpu, unsigned port, unsigned size);
/* Load the six segment registers of virtual-8086 mode from selectors. */
void       x86_v86_segs(uam_cpu *cpu, const uint16_t sel[X86_NSEG]);
/* LAR (lsl = false) / LSL: sets ZF; the value is valid when ZF = 1 */
uint64_t   x86_lar(uam_cpu *cpu, uint16_t sel, bool lsl);

/* CPUID and MSRs (x86_msr.c) */
void       x86_cpuid(uam_cpu *cpu, uint32_t leaf, uint32_t sub, uint32_t r[4]);
uam_status x86_rdmsr(uam_cpu *cpu, uint32_t msr, uint64_t *val);
uam_status x86_wrmsr(uam_cpu *cpu, uint32_t msr, uint64_t val);
void       x86_cpuid_defaults(uam_x86_cpuid_info *info);

/* x87 (x86_x87.c): 80-bit memory format <-> host value, and FXSAVE support */
void        x86_f80_store(uint8_t out[10], long double v);
long double x86_f80_load(const uint8_t in[10]);
uint64_t    x86_h_x87(void *env, uint64_t desc, uint64_t lin, uint64_t c, uam_status *st);

/* SMM (x86_smm.c) */
void       x86_smm_enter(uam_cpu *cpu);
uam_status x86_smm_rsm(uam_cpu *cpu, uint64_t *new_pc);

/* the system-register handlers (x86_system.c), shared with helpers */
uam_status x86_sysreg_read(uam_cpu *cpu, uint32_t id, uint64_t *val);
uam_status x86_sysreg_write(uam_cpu *cpu, uint32_t id, uint64_t val);

/* debugger view (x86_gdb.c) */
extern const uam_gdb_arch x86_gdb;

#endif /* UAM_X86_INTERNAL_H */
