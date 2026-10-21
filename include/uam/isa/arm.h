/*
 * uam/isa/arm.h - Public face of the ARM (AArch32) ISA module.
 *
 * ARMv7-A, little-endian: the ARM (A32) and Thumb/Thumb-2 (T32) instruction
 * sets with interworking, conditional execution and IT blocks, the processor
 * modes with their banked registers, the exception model (vectors at 0,
 * 0xffff0000 or VBAR), CP15 with a short-descriptor MMU (sections,
 * supersections, large and small pages, domains, access permissions, XN) and
 * the generic timer. No VFP/NEON, Jazelle, security or virtualization
 * extensions; their instructions take the Undefined Instruction exception.
 *
 * Code addresses follow the ARM convention for interworking: bit 0 of the pc
 * set means Thumb state. cpu->pc, block and IMARK pcs, PC hooks, coverage
 * sites and --fuzz-begin addresses for Thumb code therefore carry bit 0
 * (e.g. 0x8001 for Thumb code at 0x8000, as in a Thumb function symbol), and
 * an ELF entry point with bit 0 set starts the hart in Thumb state.
 *
 * Interrupt input lines are UAM_ARM_IRQ and UAM_ARM_FIQ (the core's nIRQ and
 * nFIQ, active high here). The generic timer is internal to the CPU; a board
 * routes its outputs (usually to GIC PPIs) with uam_arm_set_timer_sink().
 *
 * The CPU identity (MIDR and the other identification registers, counter
 * frequency) is a property of the SoC, so the board selects it with
 * uam_arm_configure() after uam_machine_init() and before the first
 * uam_machine_reset(). Without that call the hart is uam_arm_cortex_a7.
 */
#ifndef UAM_ISA_ARM_H
#define UAM_ISA_ARM_H

#include "uam/isa.h"

/* ARMv7-A, ARM + Thumb-2, CP15 with the short-descriptor MMU; little-endian. */
extern const uam_isa uam_isa_arm;

enum uam_arm_irq_line {
    UAM_ARM_IRQ = 0,   /* IRQ input (taken when CPSR.I = 0) */
    UAM_ARM_FIQ = 1,   /* FIQ input (taken when CPSR.F = 0; above IRQ) */
};

/* Generic timer outputs (uam_arm_set_timer_sink). */
enum uam_arm_timer {
    UAM_ARM_TIMER_PHYS = 0,   /* CNTP_*: usually GIC PPI 30 */
    UAM_ARM_TIMER_VIRT = 1,   /* CNTV_*: usually GIC PPI 27 */
};

/* CPU model: reset values of the identification registers. */
typedef struct uam_arm_config {
    const char *name;
    uint32_t    midr;         /* Main ID */
    uint32_t    ctr;          /* Cache Type */
    uint32_t    mpidr;        /* Multiprocessor Affinity */
    uint32_t    id_pfr0, id_pfr1;
    uint32_t    id_mmfr0;
    uint32_t    id_isar0;
    uint32_t    cntfrq;       /* reset CNTFRQ; the counter advances one per virtual tick */
} uam_arm_config;

extern const uam_arm_config uam_arm_cortex_a7;  /* Cortex-A7 identity, no FPU */

/* Select the CPU model of an ARM hart. Takes effect at the next reset.
 * Returns UAM_ERR_INVALID if cpu is not an ARM hart or cfg is NULL. */
uam_status uam_arm_configure(uam_cpu *cpu, const uam_arm_config *cfg);

/* Called whenever a generic timer's interrupt output changes level
 * (enum uam_arm_timer). Not saved in snapshots: the receiver saves its own input
 * levels. Pass fn = NULL to disconnect. */
typedef void (*uam_arm_timer_sink)(void *ctx, unsigned timer, int level);
uam_status uam_arm_set_timer_sink(uam_cpu *cpu, uam_arm_timer_sink fn, void *ctx);

#endif /* UAM_ISA_ARM_H */
