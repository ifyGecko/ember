/*
 * uam/isa/avr32.h - Public face of the AVR32 ISA module.
 *
 * One descriptor covers both microarchitectures of the AVR32 Architecture
 * Manual; the CPU model a board selects with uam_avr32_configure() decides
 * which one a hart is:
 *
 *   - AVR32A (the microcontroller class): no register shadowing, events save
 *     SR, PC (and for interrupts R8-R12 and LR) on the system stack, reset at
 *     0x80000000, an optional MPU (MMU type 3);
 *   - AVR32B (the application-processor class): return status/address
 *     registers per context, INT3 may shadow R8-R12 and LR, reset at
 *     0xA0000000, the segmented memory map (P0-P4) with an optional MMU
 *     (separate ITLB and DTLB), and the SIMD instructions.
 *
 * Big-endian, 32-bit. Interrupts arrive on five input lines: INT0..INT3 (the
 * four priority levels) and NMI. Where an interrupt handler starts is EVBA plus
 * an autovector the external interrupt controller supplies; the board installs
 * a function that answers it (uam_avr32_set_autovector). The COUNT/COMPARE
 * timer's request is an output line of the CPU (uam_avr32_compare_line), which
 * the board routes to its interrupt controller, as the architecture does.
 */
#ifndef UAM_ISA_AVR32_H
#define UAM_ISA_AVR32_H

#include "uam/isa.h"
#include "uam/signal.h"

extern const uam_isa uam_isa_avr32;

enum uam_avr32_irq_line {
    UAM_AVR32_IRQ_INT0 = 0,
    UAM_AVR32_IRQ_INT1 = 1,
    UAM_AVR32_IRQ_INT2 = 2,
    UAM_AVR32_IRQ_INT3 = 3,
    UAM_AVR32_IRQ_NMI  = 4,
};

enum uam_avr32_arch { UAM_AVR32_A = 0, UAM_AVR32_B = 1 };

/* CPU model: the identity and the implementation-defined parts. */
typedef struct uam_avr32_config {
    const char *name;
    unsigned    arch;          /* UAM_AVR32_A or UAM_AVR32_B */
    uint32_t    config0;       /* AT, MMUT and the S/D/R feature bits are made to agree with
                                * arch, mmu_entries and the fields below */
    uint32_t    config1;       /* unused: CONFIG1 reports the TLB / MPU sizes from mmu_entries
                                * and no caches */
    unsigned    mmu_entries;   /* A: MPU regions (0 = no MPU, else 8); B: entries per TLB
                                * (0 = no MMU, else 1..64, an ITLB and a DTLB) */
    bool        int3_shadow;   /* B: INT3 has its own R8-R12 and LR */
    bool        simd;          /* the SIMD instructions (B only) */
} uam_avr32_config;

extern const uam_avr32_config uam_avr32a_ref;  /* AVR32A, rev. 2, MPU with 8 regions, DSP, RMW */
extern const uam_avr32_config uam_avr32b_ref;  /* AVR32B, rev. 2, 32-entry ITLB and DTLB,
                                                * INT3 shadow registers, DSP, SIMD */

/* Select the CPU model of an AVR32 hart (before its reset, normally while the board is built).
 * Returns UAM_ERR_INVALID if cpu is not an AVR32 hart or cfg is malformed. */
uam_status uam_avr32_configure(uam_cpu *cpu, const uam_avr32_config *cfg);

/* The autovector (offset from EVBA, bits 13:0) of the interrupt the controller
 * presents at `level` (0..3), asked when the CPU takes it. */
typedef uint32_t (*uam_avr32_autovector_fn)(void *ctx, unsigned level);
void uam_avr32_set_autovector(uam_cpu *cpu, uam_avr32_autovector_fn fn, void *ctx);

/* The COUNT == COMPARE interrupt request, high until COMPARE is written. */
uam_signal *uam_avr32_compare_line(uam_cpu *cpu);

/* Whether isa is the AVR32 module. */
bool uam_avr32_is(const uam_isa *isa);

#endif /* UAM_ISA_AVR32_H */
