/*
 * uam/isa/avr.h - Public face of the 8-bit AVR ISA module.
 *
 * One descriptor, uam_isa_avr, for every 8-bit AVR core family the AVR Instruction Set
 * Manual describes; the family and the core's options are the CPU model, which the board
 * selects with uam_avr_configure() after uam_machine_init() and before the first
 * uam_machine_reset() (without that call the hart is uam_avr_eplus_ref).
 *
 *   UAM_AVR_E      AVRe: 32 registers, MUL/MOVW/LPM Rd,Z+, 16-bit PC (up to 128 KiB)
 *   UAM_AVR_EPLUS  AVRe+: AVRe with a 22-bit PC (EIND, EIJMP/EICALL, ELPM, RAMPZ)
 *   UAM_AVR_XM     AVRxm (XMEGA): AVRe+ with RAMPD/X/Y, XCH/LAS/LAC/LAT, DES, and the
 *                  level-based interrupt controller (PMIC)
 *   UAM_AVR_XT     AVRxt: the AVRe+ instruction set with the CPUINT interrupt controller
 *   UAM_AVR_RC     AVRrc (reduced core): 16 registers (r16-r31), no MUL/MOVW/ADIW/LDD/LPM,
 *                  7-bit LDS/STS
 *
 * Memory. AVR is a Harvard machine; ember puts both spaces on the one bus, as the GNU tools
 * do in ELF files: program memory at physical 0 (byte addresses: the pc is twice the
 * word-addressed PC), and the data space at UAM_AVR_DATA_BASE. Loads and stores reach
 * the data space through isa->translate; LPM/ELPM read program memory directly. A
 * debugger or loader (UAM_XLATE_DEBUG) sees the bus addresses as they are, as avr-gdb
 * does. On AVRe/AVRe+ the registers r0-r31 are also data addresses 0x00-0x1F and the
 * I/O space starts at data 0x20; on AVRxm, AVRxt and AVRrc the I/O space starts at
 * data 0 and the registers are not in the data space. The core's own I/O registers
 * (SREG, SPH/SPL, RAMPx, EIND, CCP and the PMIC or CPUINT registers) are part of the hart;
 * everything else in the data space is the board's.
 *
 * Interrupts. Input line n is interrupt vector n (1..63; vector 0 is reset). The vector
 * taken is the lowest pending one (AVRe/AVRe+/AVRrc: when SREG.I is set; it clears I).
 * On AVRxm and AVRxt each vector has a level the board sets with uam_avr_set_irq_level
 * (AVRxm: 1 low, 2 medium, 3 high; AVRxt: 0 or 1; either: UAM_AVR_LEVEL_NMI), and the
 * core's interrupt controller decides as the manual and the PMIC/CPUINT descriptions
 * say: a higher level preempts a lower one, RETI ends the highest level running, I
 * gates all but NMI and is left alone, round-robin among level-0/low-level vectors when
 * enabled. Many peripherals clear their interrupt flag when the CPU takes the vector:
 * uam_avr_set_irq_ack names a function the core calls with the vector it takes.
 */
#ifndef UAM_ISA_AVR_H
#define UAM_ISA_AVR_H

#include "uam/isa.h"

extern const uam_isa uam_isa_avr;

/* Bus address of data-space address 0 (the GNU tools' convention). */
#define UAM_AVR_DATA_BASE UINT64_C(0x800000)

typedef enum uam_avr_core {
    UAM_AVR_E, UAM_AVR_EPLUS, UAM_AVR_XM, UAM_AVR_XT, UAM_AVR_RC,
} uam_avr_core;

/* Optional instructions (uam_avr_config.features). Which a part has is in its data sheet
 * and the manual's Appendix A. */
enum uam_avr_feature {
    UAM_AVR_F_MUL   = 1u << 0,   /* MUL, MULS, MULSU, FMUL, FMULS, FMULSU */
    UAM_AVR_F_JMP   = 1u << 1,   /* JMP, CALL (devices over 8 KiB) */
    UAM_AVR_F_ELPM  = 1u << 2,   /* ELPM (RAMPZ) */
    UAM_AVR_F_SPM   = 1u << 3,   /* SPM */
    UAM_AVR_F_BREAK = 1u << 4,   /* BREAK (on-chip debug) */
};

#define UAM_AVR_LEVEL_NMI 4u

typedef struct uam_avr_config {
    const char  *name;
    uam_avr_core core;
    unsigned     features;      /* enum uam_avr_feature */
    uint32_t     flash_size;    /* bytes of program memory; above 128 KiB the PC has 22 bits
                                 * and return addresses take three bytes */
    uint16_t     sp_reset;      /* SP after reset (the end of internal SRAM) */
    uint8_t      vector_size;   /* bytes per interrupt vector: 2 (RJMP) or 4 (JMP) */
    uint32_t     boot_base;     /* AVRxm/AVRxt: the vector base with IVSEL set (byte address) */
} uam_avr_config;

/* Reference models naming no part: the memory layouts of common devices of each family
 * (so their compilers' start-up code runs), with no peripherals of those devices. */
extern const uam_avr_config uam_avr_e_ref;      /* AVRe, 32 KiB flash, SRAM to 0x08FF */
extern const uam_avr_config uam_avr_eplus_ref;  /* AVRe+, 256 KiB flash, SRAM to 0x21FF */
extern const uam_avr_config uam_avr_xm_ref;     /* AVRxm, 128+8 KiB flash, SRAM to 0x3FFF */
extern const uam_avr_config uam_avr_xt_ref;     /* AVRxt, 48 KiB flash, SRAM to 0x3FFF */
extern const uam_avr_config uam_avr_rc_ref;     /* AVRrc, 4 KiB flash, SRAM to 0x013F */

/* Select the CPU model of an AVR hart. Takes effect at the next reset. Returns
 * UAM_ERR_INVALID if cpu is not an AVR hart or cfg is malformed. */
uam_status uam_avr_configure(uam_cpu *cpu, const uam_avr_config *cfg);
/* The model in effect, or NULL if cpu is not an AVR hart. */
const uam_avr_config *uam_avr_config_of(uam_cpu *cpu);

/* AVRxm/AVRxt: the interrupt level of vector `vector`. */
uam_status uam_avr_set_irq_level(uam_cpu *cpu, unsigned vector, unsigned level);

/* Called with each vector the core takes (a peripheral that clears its flag then). */
typedef void (*uam_avr_irq_ack_fn)(void *ctx, unsigned vector);
void uam_avr_set_irq_ack(uam_cpu *cpu, uam_avr_irq_ack_fn fn, void *ctx);

/* Self-programming: SPM hands the board's flash controller the Z pointer (byte address,
 * with RAMPZ) and the word in r1:r0; without one SPM does nothing. */
typedef void (*uam_avr_spm_fn)(void *ctx, uint32_t addr, uint16_t word);
void uam_avr_set_spm(uam_cpu *cpu, uam_avr_spm_fn fn, void *ctx);

/* True if `isa` is the AVR descriptor. */
static inline bool uam_avr_is(const uam_isa *isa) { return isa == &uam_isa_avr; }

#endif /* UAM_ISA_AVR_H */
