/*
 * uam/isa/ppc.h - Public face of the 32-bit PowerPC ISA module.
 *
 * The 32-bit PowerPC architecture: the user instruction set (integer, branch,
 * condition register, load/store including string, multiple and byte-reversed
 * forms, lwarx/stwcx., the floating-point unit with the FPSCR and its exception
 * model), the virtual environment (time base, cache and synchronisation
 * instructions) and the operating environment (MSR, SRR0/SRR1, SPRGs, DAR/DSISR,
 * the decrementer, the exception vectors at 0 or 0xFFF00000 by MSR[IP], and the
 * MMU: block address translation with IBAT/DBAT pairs, sixteen segment
 * registers and the hashed page table at SDR1 with its referenced and changed
 * bits). This is the architecture of the 6xx, 7xx and 74xx families and the
 * e300/e600 cores. The Book E, 40x and 8xx privileged architectures, AltiVec and
 * SPE are not modelled; their instructions take the illegal-instruction program
 * exception. The processor is big-endian: MSR[LE] and MSR[ILE] read as zero.
 *
 * Interrupt inputs (irq_input line numbers): UAM_PPC_IRQ_INT(0..7) are wired-OR
 * sources of the external interrupt pin (taken through vector 0x500 while
 * MSR[EE] is set, for as long as any of them is high), UAM_PPC_IRQ_SRESET is the
 * soft-reset pin (vector 0x100, on a rising edge) and UAM_PPC_IRQ_MCP the
 * machine-check pin (vector 0x200 on a rising edge; checkstop while MSR[ME] is
 * clear). The decrementer and the time base are internal to the processor and
 * advance once every `tb_div` virtual ticks.
 *
 * What differs between implementations is chosen by the board: the identity
 * (PVR), whether a floating-point unit exists, the number of BAT pairs, the
 * cache block size dcbz clears, the exception prefix at reset and the time base
 * rate (uam_ppc_configure, after uam_machine_init and before the first
 * uam_machine_reset; without it the hart is uam_ppc_ref, a reference identity
 * that names no implementation), and the implementation-specific special
 * purpose registers (HID0, L2CR, the performance monitor, ...), which the board
 * models with uam_ppc_set_spr_ops. An SPR the architecture does not define and
 * the board does not claim reads back the last value written to it.
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
    UAM_PPC_IRQ_SRESET = 8,   /* soft reset (vector 0x100), rising edge */
    UAM_PPC_IRQ_MCP    = 9,   /* machine check (vector 0x200), rising edge */
};

/* CPU model. */
typedef struct uam_ppc_config {
    const char *name;
    uint32_t    pvr;          /* Processor Version Register */
    bool        fpu;          /* floating-point unit present (else FP instructions are illegal) */
    unsigned    bat_pairs;    /* 4, or 8 with the second set at SPRs 560-575 */
    unsigned    cache_block;  /* bytes dcbz clears: a power of two, 16..128 */
    bool        reset_ip;     /* MSR[IP] at reset: exception vectors at 0xFFF00000 */
    unsigned    tb_div;       /* virtual ticks per time-base / decrementer tick, >= 1 */
} uam_ppc_config;

/* 32-bit PowerPC reference identity: FPU, 4 BAT pairs, 32-byte cache blocks, vectors
 * at 0xFFF00000 at reset, the time base advancing once per virtual tick. */
extern const uam_ppc_config uam_ppc_ref;

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

#endif /* UAM_ISA_PPC_H */
