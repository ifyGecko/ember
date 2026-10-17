/*
 * uam/isa/mips32.h - Public face of the MIPS32 ISA module.
 *
 * Two descriptors, one per byte order (MIPS is bi-endian and the choice is
 * fixed per image). Boards wire device interrupt lines to CPU hardware
 * interrupt inputs; line numbers are Cause.IP bit positions (2..6 are the
 * six hardware interrupts; 7 is the timer; 0..1 are software).
 *
 * The CPU identity (PRId, Config registers, TLB size, writable Status bits) is
 * a property of the SoC, so the board selects it with uam_mips_configure()
 * after uam_machine_init() and before the first uam_machine_reset(). Without
 * that call the hart is a 24Kc (uam_mips_24kc).
 */
#ifndef UAM_ISA_MIPS32_H
#define UAM_ISA_MIPS32_H

#include "uam/isa.h"

/* MIPS32 release 2, user + privileged (CP0) with an R4K-style joint TLB; no FPU. */
extern const uam_isa uam_isa_mips32be; /* big-endian */
extern const uam_isa uam_isa_mips32le; /* little-endian */

enum uam_mips_irq_line {
    UAM_MIPS_IRQ_SW0 = 0,  /* Cause.IP0 - software */
    UAM_MIPS_IRQ_SW1 = 1,  /* Cause.IP1 - software */
    UAM_MIPS_IRQ_HW0 = 2,  /* Cause.IP2 - hardware interrupt 0 */
    UAM_MIPS_IRQ_HW1 = 3,
    UAM_MIPS_IRQ_HW2 = 4,
    UAM_MIPS_IRQ_HW3 = 5,
    UAM_MIPS_IRQ_HW4 = 6,
    UAM_MIPS_IRQ_HW5 = 7,  /* Cause.IP7 - also the CP0 timer (Count==Compare) */
};

/* CPU model. Reset values of the identification/configuration registers. */
typedef struct uam_mips_config {
    const char *name;
    uint32_t    prid;
    uint32_t    config0;      /* Config.BE is forced to the descriptor's byte order */
    uint32_t    config1;      /* Config1.MMUSize is derived from tlb_entries */
    uint32_t    config2;
    uint32_t    config3;
    uint32_t    status_rw;    /* Status bits software can write */
    unsigned    tlb_entries;  /* joint TLB entries, 1..64 */
} uam_mips_config;

extern const uam_mips_config uam_mips_24kc;  /* MIPS 24Kc: 16-entry TLB, no FPU */

/* Select the CPU model of a MIPS hart. Takes effect at the next reset.
 * Returns UAM_ERR_INVALID if cpu is not a MIPS32 hart or cfg is malformed. */
uam_status uam_mips_configure(uam_cpu *cpu, const uam_mips_config *cfg);

#endif /* UAM_ISA_MIPS32_H */
