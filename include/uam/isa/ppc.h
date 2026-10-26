/*
 * uam/isa/ppc.h - Public face of the 32-bit PowerPC ISA module.
 *
 * One instruction set, two privileged architectures, chosen per hart by the CPU
 * model (uam_ppc_config.model):
 *
 *   UAM_PPC_MODEL_OEA    the classic operating environment (6xx, 7xx, 74xx, e300,
 *                        e600): MSR with IR/DR, SRR0/SRR1, DAR/DSISR, exception
 *                        vectors at 0 or 0xFFF00000 by MSR[IP], the decrementer
 *                        interrupting when its sign changes, and the MMU of block
 *                        address translation (IBAT/DBAT pairs), sixteen segment
 *                        registers and the hashed page table at SDR1 with its
 *                        referenced and changed bits.
 *   UAM_PPC_MODEL_BOOKE  Book E (Power ISA Book III-E: e500, e200, 440 class): MSR
 *                        with IS/DS address spaces, CE, DE and WE; interrupt vectors
 *                        IVPR | IVORn; SRR0/1, CSRR0/1 for critical interrupts and,
 *                        as configured, MCSRR0/1 (machine check) and DSRR0/1
 *                        (debug); ESR and DEAR; the decrementer with auto-reload, the
 *                        fixed-interval timer and the watchdog (TCR, TSR); debug
 *                        events from DBCR0 (instruction complete, branch taken,
 *                        trap); and an MMU of software-managed TLB arrays searched
 *                        with the PIDs, managed through the architected MAS registers
 *                        (tlbwe, tlbre, tlbsx, tlbivax).
 *
 * Common to both: the user instruction set (integer, branch, condition register,
 * every load/store form, lwarx/stwcx.), the floating-point unit with the FPSCR,
 * the time base, the cache and synchronisation instructions. AltiVec, SPE and the
 * 40x/8xx privileged architectures are not modelled; their instructions take the
 * illegal-instruction program exception. The processor is big-endian.
 *
 * Interrupt inputs (irq_input line numbers): UAM_PPC_IRQ_INT(0..7) are wired-OR
 * sources of the external interrupt pin (taken while MSR[EE] is set, for as long
 * as any of them is high), UAM_PPC_IRQ_MCP the machine-check pin (rising edge;
 * checkstop while MSR[ME] is clear), UAM_PPC_IRQ_SRESET the classic soft-reset pin
 * (vector 0x100; Book E has no such interrupt and ignores it) and
 * UAM_PPC_IRQ_CINT the Book E critical input (level; taken while MSR[CE] is set).
 * The timers are internal to the processor and advance once every `tb_div`
 * virtual ticks.
 *
 * What differs between implementations is chosen by the board: the CPU model
 * (uam_ppc_configure, after uam_machine_init and before the first
 * uam_machine_reset; without it the hart is uam_ppc_ref, a classic reference
 * identity that names no implementation; uam_ppc_booke_ref is the Book E one), the
 * implementation-specific special purpose registers (HID0, L2CR, the performance
 * monitor, ...: uam_ppc_set_spr_ops; an SPR the architecture does not define and
 * the board does not claim reads back the last value written to it), and on Book E
 * the device control registers (uam_ppc_set_dcr_ops), a TLB management interface
 * other than the MAS registers (uam_ppc_set_tlb_ops, with uam_ppc_tlb_read/write/
 * find), and what a watchdog reset does (uam_ppc_set_reset_sink).
 */
#ifndef UAM_ISA_PPC_H
#define UAM_ISA_PPC_H

#include "uam/isa.h"

extern const uam_isa uam_isa_ppc32;   /* big-endian */

/* True for the PowerPC descriptor. */
static inline bool uam_ppc_is(const uam_isa *isa) { return isa == &uam_isa_ppc32; }

#define UAM_PPC_IRQ_INT_LINES 8u
#define UAM_PPC_IRQ_INT(n)    ((unsigned)(n))   /* external interrupt sources, wired-OR */
enum uam_ppc_irq_line {
    UAM_PPC_IRQ_SRESET = 8,   /* classic soft reset (vector 0x100), rising edge */
    UAM_PPC_IRQ_MCP    = 9,   /* machine check, rising edge */
    UAM_PPC_IRQ_CINT   = 10,  /* Book E critical input (IVOR0), level */
};

typedef enum uam_ppc_model {
    UAM_PPC_MODEL_OEA   = 0,
    UAM_PPC_MODEL_BOOKE = 1,
} uam_ppc_model;

/* ---- Book E TLB entries ------------------------------------------------------ */

/* uam_ppc_tlbe.perm */
enum {
    UAM_PPC_TLB_SR = 0x01, UAM_PPC_TLB_UR = 0x02,   /* read: supervisor, user */
    UAM_PPC_TLB_SW = 0x04, UAM_PPC_TLB_UW = 0x08,   /* write */
    UAM_PPC_TLB_SX = 0x10, UAM_PPC_TLB_UX = 0x20,   /* execute */
};

/* One translation: the effective page (epn, 2^size_log2 bytes) in address space ts,
 * for processes whose PID equals tid (tid 0: all), maps to the real page rpn. */
typedef struct uam_ppc_tlbe {
    bool     valid;
    bool     iprot;      /* survives tlbivax and flash invalidation */
    uint8_t  ts;         /* translation space, matched against MSR[IS] / MSR[DS] */
    uint8_t  size_log2;  /* 12 (4 KiB) .. 32 (4 GiB) */
    uint32_t tid;        /* translation ID (14 bits); 0 matches every PID */
    uint32_t epn;        /* effective page address, aligned to the size */
    uint64_t rpn;        /* real page address (up to 36 bits), aligned to the size */
    uint8_t  wimge;      /* W I M G E in bits 4..0 */
    uint8_t  perm;       /* UAM_PPC_TLB_* */
    uint8_t  user;       /* U0-U3 in bits 3..0, X0 X1 in bits 5..4: kept for software */
} uam_ppc_tlbe;

/* A TLB array. Set-associative arrays are indexed by the low page-number bits of the
 * effective address (4 KiB granules); an entry's index is set * ways + way. */
typedef struct uam_ppc_tlb_geom {
    unsigned entries;    /* 0: the array does not exist; at most 1024 */
    unsigned ways;       /* entries per set (== entries: fully associative) */
    uint64_t page_sizes; /* bit n set: pages of 2^n bytes (12 <= n <= 32) */
    bool     iprot;      /* entries can be protected (IPROT) */
} uam_ppc_tlb_geom;

typedef struct uam_ppc_booke_config {
    uam_ppc_tlb_geom tlb[2];     /* TLB0, TLB1 */
    unsigned         pids;       /* PID registers: 1 (PID, SPR 48) or 3 (PID0-2: 48, 633, 634) */
    bool             tsize_pow2; /* MAS1[TSIZE] is 2^n KiB (MMU v2); else 4^n KiB (v1) */
    bool             mcheck_mcsrr; /* machine check: MCSRR0/1 and rfmci; else CSRR0/1 */
    bool             debug_dsrr; /* debug: DSRR0/1 and rfdi; else CSRR0/1 */
    uint8_t          fit_bit[4]; /* time-base bit (0 = least significant) TCR[FP] selects */
    uint8_t          wdt_bit[4]; /* time-base bit TCR[WP] selects */
    bool             period_ext; /* TCR[FPEXT]/[WPEXT] extend FP/WP: bit 63 - (EXT || FP) */
    uam_ppc_tlbe     boot;       /* written to TLB1 entry 0 at reset when valid */
} uam_ppc_booke_config;

/* CPU model. */
typedef struct uam_ppc_config {
    const char   *name;
    uam_ppc_model model;
    uint32_t      pvr;          /* Processor Version Register */
    bool          fpu;          /* floating-point unit present (else FP instructions are illegal) */
    bool          isel;         /* the isel instruction */
    unsigned      cache_block;  /* bytes dcbz clears: a power of two, 16..128 */
    unsigned      tb_div;       /* virtual ticks per time-base / timer tick, >= 1 */
    /* classic (OEA) */
    unsigned      bat_pairs;    /* 4, or 8 with the second set at SPRs 560-575 */
    bool          reset_ip;     /* MSR[IP] at reset: exception vectors at 0xFFF00000 */
    /* Book E */
    uam_ppc_booke_config booke;
} uam_ppc_config;

/* Classic reference identity: FPU, 4 BAT pairs, 32-byte cache blocks, vectors at
 * 0xFFF00000 at reset, the time base advancing once per virtual tick. */
extern const uam_ppc_config uam_ppc_ref;

/* Book E reference identity: FPU, isel, a 512-entry 4-way TLB0 of 4 KiB pages and a
 * 16-entry fully associative TLB1 of 4 KiB .. 1 GiB pages (MAS interface v1), one
 * PID, MCSRR0/1 for machine checks, CSRR0/1 for debug, and the boot page mapped:
 * TLB1 entry 0 maps 0xFFFFF000 (4 KiB, supervisor read/write/execute, I, G, IPROT)
 * to itself, around the reset vector 0xFFFFFFFC. */
extern const uam_ppc_config uam_ppc_booke_ref;

/* Select the CPU model of a PowerPC hart. Takes effect at the next reset.
 * Returns UAM_ERR_INVALID if cpu is not a PowerPC hart or cfg is malformed. */
uam_status uam_ppc_configure(uam_cpu *cpu, const uam_ppc_config *cfg);

/*
 * Implementation-specific special purpose registers. mfspr/mtspr of an SPR the
 * architecture does not define, in supervisor state, calls these (either may be
 * NULL). Return UAM_OK when handled, UAM_ERR_ILLEGAL when the SPR does not exist
 * (the instruction takes the illegal-instruction program exception), and
 * UAM_ERR_INVALID to leave the SPR to the default: a plain register that reads
 * back what was written (zero after reset), saved in snapshots. A board saves the
 * state of the SPRs it claims itself (uam_machine_add_state). The SPR number is
 * the architectural one (e.g. 1008 for HID0 on many implementations).
 */
typedef struct uam_ppc_spr_ops {
    uam_status (*read)(void *ctx, uam_cpu *cpu, unsigned spr, uint32_t *val);
    uam_status (*write)(void *ctx, uam_cpu *cpu, unsigned spr, uint32_t val);
} uam_ppc_spr_ops;

/* Install the SPR handlers of one hart (ops NULL: none). Not saved in snapshots. */
uam_status uam_ppc_set_spr_ops(uam_cpu *cpu, const uam_ppc_spr_ops *ops, void *ctx);

/* ---- Book E board interfaces --------------------------------------------------- */

/*
 * Device control registers (mfdcr, mtdcr, mfdcrx, mtdcrx; supervisor only). Return
 * UAM_OK when handled; anything else makes the instruction illegal. Without ops
 * every DCR access is illegal.
 */
typedef struct uam_ppc_dcr_ops {
    uam_status (*read)(void *ctx, uam_cpu *cpu, unsigned dcrn, uint32_t *val);
    uam_status (*write)(void *ctx, uam_cpu *cpu, unsigned dcrn, uint32_t val);
} uam_ppc_dcr_ops;

uam_status uam_ppc_set_dcr_ops(uam_cpu *cpu, const uam_ppc_dcr_ops *ops, void *ctx);

/*
 * TLB management with another interface than the MAS registers (an implementation
 * whose tlbwe/tlbre take a word select and an index in GPRs, say). Each gets the
 * instruction word (tlbsx: also its effective address) and does the work through
 * uam_ppc_gpr / uam_ppc_set_gpr / uam_ppc_set_cr_field and uam_ppc_tlb_*. Return
 * UAM_OK when done, UAM_ERR_ILLEGAL for an illegal instruction, UAM_ERR_INVALID to
 * let the MAS registers handle it.
 */
typedef struct uam_ppc_tlb_ops {
    uam_status (*tlbwe)(void *ctx, uam_cpu *cpu, uint32_t insn);
    uam_status (*tlbre)(void *ctx, uam_cpu *cpu, uint32_t insn);
    uam_status (*tlbsx)(void *ctx, uam_cpu *cpu, uint32_t insn, uint32_t ea);
} uam_ppc_tlb_ops;

uam_status uam_ppc_set_tlb_ops(uam_cpu *cpu, const uam_ppc_tlb_ops *ops, void *ctx);

/* TLB entry access by array (0, 1) and index (set * ways + way). A write takes effect
 * for the next access (it invalidates cached translations). */
uam_status uam_ppc_tlb_read(uam_cpu *cpu, unsigned array, unsigned index, uam_ppc_tlbe *out);
uam_status uam_ppc_tlb_write(uam_cpu *cpu, unsigned array, unsigned index, const uam_ppc_tlbe *e);
/* The entry that translates ea in space ts for process pid (tid 0 entries match any
 * pid): UAM_OK with *array and *index, or UAM_ERR_XLATE. */
uam_status uam_ppc_tlb_find(uam_cpu *cpu, uint32_t ea, unsigned ts, uint32_t pid,
                            unsigned *array, unsigned *index);

/* Register access for the handlers above. */
uint32_t uam_ppc_gpr(uam_cpu *cpu, unsigned n);
void     uam_ppc_set_gpr(uam_cpu *cpu, unsigned n, uint32_t v);
void     uam_ppc_set_cr_field(uam_cpu *cpu, unsigned field, unsigned lt_gt_eq_so);

/*
 * The watchdog's reset (TCR[WRC] != 0 when the third time-out comes): `kind` is the
 * WRC value (1 core, 2 chip, 3 system reset), which TSR[WRS] also reports after the
 * reset. The board decides what is reset, usually the machine (uam_machine_reset_sink).
 * Without a sink the reset is only reported in TSR[WRS]. Not saved in snapshots.
 */
typedef void (*uam_ppc_reset_sink)(void *ctx, uam_cpu *cpu, unsigned kind);
uam_status uam_ppc_set_reset_sink(uam_cpu *cpu, uam_ppc_reset_sink fn, void *ctx);

#endif /* UAM_ISA_PPC_H */
