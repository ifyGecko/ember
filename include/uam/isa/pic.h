/*
 * uam/isa/pic.h - Public face of the 8-bit PIC ISA module.
 *
 * One descriptor, uam_isa_pic, for the four 8-bit PIC core families; the family and its
 * options are the CPU model, which the board selects with uam_pic_configure() after
 * uam_machine_init() and before the first uam_machine_reset() (without that call the hart
 * is uam_pic18_ref).
 *
 *   UAM_PIC_BASELINE  12-bit instructions (PIC10F, PIC12F5xx, PIC16F5x): 2-level stack,
 *                     no interrupts, OPTION/TRIS, data banks selected by FSR<7:5>,
 *                     program pages by STATUS<7:5> (PA)
 *   UAM_PIC_MID       14-bit instructions (PIC16F8x, PIC16F87x): 8-level stack, banks by
 *                     STATUS RP1:RP0 (IRP for FSR), pages by PCLATH, one interrupt vector
 *   UAM_PIC_ENHANCED  enhanced mid-range (PIC16F1xxx): 16-level stack readable through
 *                     STKPTR/TOS, BSR, two 16-bit FSRs over a linear view of the general
 *                     purpose RAM and of program memory, MOVIW/MOVWI, BRA/BRW/CALLW,
 *                     interrupt context saved in shadow registers
 *   UAM_PIC18         PIC18: 16-bit instructions, 31-level stack, access bank, BSR, three
 *                     FSRs with POSTINC/POSTDEC/PREINC/PLUSW, table reads and writes,
 *                     8x8 multiply, two interrupt priorities and the fast register stack,
 *                     and (UAM_PIC_F_XINST) the extended instruction set with indexed
 *                     literal offset addressing
 *
 * Memory. A PIC is a Harvard machine; ember puts both spaces on the one bus. Program
 * memory is at physical 0 as the GNU-style hex files of the PIC tools lay it out: each
 * 12- or 14-bit word of the baseline and mid-range families takes two bytes, little
 * endian (word n at byte 2n, the upper bits ignored), and PIC18 program memory is byte
 * addressed. The pc is always that bus address: twice the word PC on the 12- and 14-bit
 * families, the PC itself on PIC18. The data space is at UAM_PIC_DATA_BASE: loads and
 * stores reach it through isa->translate; reads of program memory (PIC18 TBLRD, an
 * enhanced FSR at 0x8000 and above) go to the bus directly. A debugger or loader
 * (UAM_XLATE_DEBUG) sees bus addresses as they are.
 *
 * Data addresses are the ones in the data sheets: bank * 32 + f on baseline (f < 0x10
 * always bank 0, as baseline parts map it), bank * 128 + f on the mid-range families,
 * bank * 256 + f on PIC18 (the access bank's upper half is 0xF60-0xFFF). The enhanced
 * core maps an FSR value itself: below 0x1000 a data address, 0x2000-0x29AF the linear
 * view of each bank's 80 general purpose bytes (0x20-0x6F), 0x8000 and above the low byte
 * of program word FSR - 0x8000. The core's own registers - INDF, PCL, STATUS, FSR, PCLATH,
 * INTCON and, by family, BSR, WREG, STKPTR, TOS, the enhanced shadow registers, TBLPTR,
 * TABLAT, PROD, PCLATU and RCON - are part of the hart, at their fixed addresses (in every
 * bank where the family mirrors them); everything else in the data space is the board's.
 * The baseline's write-only OPTION register and the TRIS registers that the OPTION and
 * TRIS instructions write are at data address UAM_PIC_SPECIAL + 0 and UAM_PIC_SPECIAL + f;
 * on the mid-range families those instructions write OPTION_REG and TRISx in the data
 * space (mid-range: 0x81 and 0x80 + f; enhanced: 0x95 and 0x8C + f - 5).
 *
 * Interrupts. The core holds only the global gating and the vectoring: INTCON's GIE and
 * PEIE (PIC18: GIEH/GIEL with RCON.IPEN), and INTCON's three enable/flag pairs (bits 5-3
 * enable the flags in bits 2-0: the timer 0, external and port-change interrupts of every
 * part), whose flags a device sets with uam_pic_intcon_set. Every other source is a
 * peripheral with its flag and enable in the board's registers; the board drives input
 * line UAM_PIC_IRQ_PERIPH while one has both set (PIC18 with IPEN: a high-priority one;
 * UAM_PIC_IRQ_LOW a low-priority one; without IPEN, and on the mid-range families, the two
 * lines are the same). Entry clears GIE (GIEH or GIEL), pushes the pc and
 * jumps to word 4 (PIC18: byte 0x08 high, 0x18 low); the enhanced core saves W, STATUS,
 * BSR, PCLATH and the FSRs in its shadow registers, PIC18 W, STATUS and BSR in the fast
 * register stack. Baseline parts have no interrupts.
 */
#ifndef UAM_ISA_PIC_H
#define UAM_ISA_PIC_H

#include "uam/isa.h"

extern const uam_isa uam_isa_pic;

/* Bus address of data-space address 0. */
#define UAM_PIC_DATA_BASE UINT64_C(0x800000)
/* Baseline: data address of the OPTION register (+0) and of TRIS register f (+f). */
#define UAM_PIC_SPECIAL   0x10000u

typedef enum uam_pic_core {
    UAM_PIC_BASELINE, UAM_PIC_MID, UAM_PIC_ENHANCED, UAM_PIC18,
} uam_pic_core;

/* uam_pic_config.features: configuration-word options of the core. */
enum uam_pic_feature {
    UAM_PIC_F_XINST  = 1u << 0,   /* PIC18: extended instruction set (XINST) */
    UAM_PIC_F_STVREN = 1u << 1,   /* enhanced, PIC18: stack overflow/underflow resets the core */
};

/* Interrupt input lines (irq_input). */
enum {
    UAM_PIC_IRQ_PERIPH = 0,   /* a peripheral interrupt (PIC18 with IPEN: high priority) */
    UAM_PIC_IRQ_LOW    = 1,   /* PIC18 with IPEN: low priority; otherwise a second peripheral line */
};

typedef struct uam_pic_config {
    const char  *name;
    uam_pic_core core;
    unsigned     features;    /* enum uam_pic_feature */
    uint32_t     prog_words;  /* program memory in instruction words (PIC18: bytes / 2); the PC
                               * wraps at the next power of two */
    uint8_t      banks;       /* baseline: data banks of 32 bytes (1, 2, 4 or 8) */
} uam_pic_config;

/* Reference models naming no part: the memory size of a common device of each family. */
extern const uam_pic_config uam_pic_baseline_ref;  /* 2K words, 8 banks (as PIC16F59) */
extern const uam_pic_config uam_pic_mid_ref;       /* 8K words (as PIC16F877) */
extern const uam_pic_config uam_pic_enhanced_ref;  /* 32K words (as PIC16F19197) */
extern const uam_pic_config uam_pic18_ref;         /* 64 KiB (as PIC18F4620), XINST off */

/* Select the CPU model of a PIC hart. Takes effect at the next reset. Returns
 * UAM_ERR_INVALID if cpu is not a PIC hart or cfg is malformed. */
uam_status uam_pic_configure(uam_cpu *cpu, const uam_pic_config *cfg);
/* The model in effect, or NULL if cpu is not a PIC hart. */
const uam_pic_config *uam_pic_config_of(uam_cpu *cpu);

/* Set INTCON flag bits (bits 2-0: TMR0IF, INTF/INT0IF, IOCIF/RBIF), as their sources do. */
void uam_pic_intcon_set(uam_cpu *cpu, uint8_t flags);

/* PIC18 TBLWT hands the board's flash controller TBLPTR and TABLAT; without one it
 * does nothing. */
typedef void (*uam_pic_tblwt_fn)(void *ctx, uint32_t addr, uint8_t byte);
void uam_pic_set_tblwt(uam_cpu *cpu, uam_pic_tblwt_fn fn, void *ctx);

/* Why the core last reset itself (or UAM_PIC_RESET_POWER_ON). A RESET instruction and,
 * with STVREN, a stack overflow or underflow reset the core and call the board's hook,
 * which may reset the rest of the device (PCON's bits on the enhanced parts are the
 * board's to keep). */
typedef enum uam_pic_reset_cause {
    UAM_PIC_RESET_POWER_ON, UAM_PIC_RESET_INSN, UAM_PIC_RESET_STACK_OVERFLOW,
    UAM_PIC_RESET_STACK_UNDERFLOW,
} uam_pic_reset_cause;
typedef void (*uam_pic_reset_fn)(void *ctx, uam_pic_reset_cause cause);
void uam_pic_set_reset_hook(uam_cpu *cpu, uam_pic_reset_fn fn, void *ctx);
uam_pic_reset_cause uam_pic_last_reset(uam_cpu *cpu);

/* True if `isa` is the PIC descriptor. */
static inline bool uam_pic_is(const uam_isa *isa) { return isa == &uam_isa_pic; }

#endif /* UAM_ISA_PIC_H */
