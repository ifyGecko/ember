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
 * that call the hart is uam_mips_ref, a MIPS32 release 2 reference identity
 * that names no company; a board for a specific SoC defines that SoC's
 * identity in its own uam_mips_config.
 *
 * What a processor implements is read from its Config registers, so they always
 * report exactly that:
 *   Config.MT       1: R4K-style joint TLB; 3: fixed mapping translation (FMT, no TLB)
 *   Config1.CA      the MIPS16e instruction set
 *   Config1.FP      the floating-point unit (MIPS32 release 2 FPU, 64-bit FPRs, see fir)
 *   Config3.ISA     microMIPS32: 1 only microMIPS, 2 both (MIPS32 at reset),
 *                   3 both (microMIPS at reset); Config3.ISAOnExc picks the ISA
 *                   of exception handlers (writable when both exist)
 *   Config3.VInt    vectored interrupts (IntCtl.VS, Cause.IV)
 *   Config3.VEIC    an external interrupt controller (uam_mips_eic_input)
 *   Config3.DSPP    the DSP ASE revision 1; Config3.DSP2P revision 2
 *   Config3.MCU     the MCU ASE (IRET, ASET, ACLR)
 * MIPS16e and microMIPS are never both present. In either, bit 0 of the pc (and of
 * EPC, ErrorEPC, jump targets and link addresses) is the ISA mode: 1 runs the
 * compressed instruction set, as on hardware. shadow_sets gives the number of
 * GPR shadow sets (SRSCtl.HSS).
 */
#ifndef UAM_ISA_MIPS32_H
#define UAM_ISA_MIPS32_H

#include "uam/isa.h"
#include "uam/signal.h"

/* MIPS32 release 2, user + privileged (CP0) with an R4K-style joint TLB or fixed
 * mapping; MIPS16e or microMIPS, the FPU and the DSP ASE as configured. */
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
    unsigned    tlb_entries;  /* joint TLB entries, 1..64 (0 with fixed mapping) */
    unsigned    shadow_sets;  /* GPR shadow sets beyond the normal one, 0..15 (SRSCtl.HSS) */
    uint32_t    fir;          /* the FPU's FIR (CP1 register 0) when Config1.FP */
} uam_mips_config;

extern const uam_mips_config uam_mips_ref;  /* MIPS32r2 reference: 16-entry TLB, no FPU */

/*
 * Reference identities for microcontroller-class cores (company 0xFF, like
 * uam_mips_ref). All three have an external interrupt controller and fixed
 * Status.BEV/IPL behaviour as PIC32 parts use them (boards/pic32_ref.c).
 *   uam_mips_m16_ref    MIPS32r2 + MIPS16e, fixed mapping, one shadow set (PIC32MX class)
 *   uam_mips_umips_ref  microMIPS32 only, fixed mapping, MCU ASE, one shadow set (PIC32MM class)
 *   uam_mips_mz_ref     MIPS32r2 + microMIPS, 16-entry TLB, FPU (64-bit FPRs), DSP ASE rev 2,
 *                       MCU ASE, seven shadow sets (PIC32MZ EF class)
 */
extern const uam_mips_config uam_mips_m16_ref;
extern const uam_mips_config uam_mips_umips_ref;
extern const uam_mips_config uam_mips_mz_ref;

/* Select the CPU model of a MIPS hart. Takes effect at the next reset.
 * Returns UAM_ERR_INVALID if cpu is not a MIPS32 hart or cfg is malformed. */
uam_status uam_mips_configure(uam_cpu *cpu, const uam_mips_config *cfg);

/*
 * External interrupt controller (Config3.VEIC). The controller presents its
 * highest request: the requested interrupt priority level (RIPL, 0 = none), the
 * vector number, an optional explicit vector offset from EBase (else
 * UAM_MIPS_EIC_NO_OFFSET: 0x200 + vector * IntCtl.VS spacing), and the shadow set
 * to use. The hart takes it when RIPL > Status.IPL with interrupts enabled; with
 * Cause.IV = 0 it enters at the general exception vector. Call it whenever the
 * request changes (`cpu` is the uam_cpu, as a signal sink's context is).
 */
#define UAM_MIPS_EIC_NO_OFFSET 0xFFFFFFFFu
void uam_mips_eic_input(void *cpu, unsigned ripl, unsigned vector, uint32_t offset,
                        unsigned shadow_set);

/* Requests the processor itself raises, for an external interrupt controller to
 * take as inputs: the Count/Compare timer and the software interrupts Cause.IP0/1.
 * (Without an external controller they are internal and need no wiring.) */
uam_signal *uam_mips_timer_line(uam_cpu *cpu);
uam_signal *uam_mips_swint_line(uam_cpu *cpu, unsigned n);

#endif /* UAM_ISA_MIPS32_H */
