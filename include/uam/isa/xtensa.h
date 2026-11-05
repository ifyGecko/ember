/*
 * uam/isa/xtensa.h - Public face of the Xtensa ISA module.
 *
 * Xtensa is a configurable processor: every core is the base instruction set plus the
 * options its designer chose. Two descriptors, one per byte order (an Xtensa core is
 * built little- or big-endian, and big-endian cores lay instruction fields out in the
 * opposite order); the CPU model, uam_xtensa_config, says which options a hart has and
 * how its interrupts and vectors are configured. The board selects it with
 * uam_xtensa_configure() after uam_machine_init() and before the first
 * uam_machine_reset() (without that call the hart is uam_xtensa_lx6).
 *
 * The options ember implements (UAM_XT_OPT_*): Code Density, Windowed Registers (32 or 64
 * physical address registers), Loops, Booleans, MUL16, MUL32 and its high half, 32-bit
 * Divide, NSA, MIN/MAX, SEXT, CLAMPS, MAC16, the single-precision Floating-Point
 * coprocessor (CP0, with the divide and square-root step instructions), Conditional Store
 * (S32C1I, SCOMPARE1, ATOMCTL), Multiprocessor Sync (L32AI, S32RI), Thread Pointer,
 * Extended L32R (LITBASE), the Unaligned Exception option or hardware alignment, Region
 * Protection (eight 512 MiB regions), the Coprocessor option (CPENABLE), Miscellaneous
 * Special Registers, Processor ID, and the XEA2 exception architecture with the Interrupt,
 * High-Priority Interrupt, Timer Interrupt (CCOUNT, CCOMPARE) and Debug options (BREAK,
 * DEBUGCAUSE). A missing option's instructions and special registers are illegal
 * instructions, as on hardware. Designer-defined TIE instructions are not modelled: they
 * are IllegalInstruction (the ESP32-S3's AI instructions, its CP3, whatever CPENABLE says;
 * the ESP32's double-precision helpers); cp_mask only gives CPENABLE its bits.
 *
 * Interrupts. Input line n is interrupt n (0 .. ninterrupt-1) as the model's table
 * configures it: a level-triggered line is read as it is, an edge-triggered or NMI line
 * latches its rising edge. Timer and software interrupts are the core's own (CCOMPARE
 * matches, INTSET); profiling and write-error lines are external inputs too. CCOUNT counts
 * one per tick of virtual time.
 *
 * External registers (RER/WER) reach the board's ERI bus through uam_xtensa_set_eri();
 * without it RER reads 0 and WER does nothing.
 */
#ifndef UAM_ISA_XTENSA_H
#define UAM_ISA_XTENSA_H

#include "uam/isa.h"

extern const uam_isa uam_isa_xtensa;     /* little-endian */
extern const uam_isa uam_isa_xtensaeb;   /* big-endian */

/* Options (uam_xtensa_config.options). */
enum {
    UAM_XT_OPT_DENSITY     = 1u << 0,   /* 16-bit .N instructions */
    UAM_XT_OPT_WINDOWED    = 1u << 1,   /* register windows (nareg 32 or 64) */
    UAM_XT_OPT_LOOPS       = 1u << 2,   /* LOOP, LBEG/LEND/LCOUNT */
    UAM_XT_OPT_BOOLEANS    = 1u << 3,   /* BR b0-b15 */
    UAM_XT_OPT_MUL16       = 1u << 4,
    UAM_XT_OPT_MUL32       = 1u << 5,   /* MULL */
    UAM_XT_OPT_MUL32_HIGH  = 1u << 6,   /* MULUH, MULSH */
    UAM_XT_OPT_DIV32       = 1u << 7,   /* QUOS, QUOU, REMS, REMU */
    UAM_XT_OPT_NSA         = 1u << 8,
    UAM_XT_OPT_MINMAX      = 1u << 9,
    UAM_XT_OPT_SEXT        = 1u << 10,
    UAM_XT_OPT_CLAMPS      = 1u << 11,
    UAM_XT_OPT_MAC16       = 1u << 12,
    UAM_XT_OPT_FP          = 1u << 13,  /* single-precision FPU as coprocessor 0 */
    UAM_XT_OPT_S32C1I      = 1u << 14,  /* SCOMPARE1, ATOMCTL */
    UAM_XT_OPT_SYNC        = 1u << 15,  /* L32AI, S32RI */
    UAM_XT_OPT_THREADPTR   = 1u << 16,
    UAM_XT_OPT_LITBASE     = 1u << 17,  /* Extended L32R */
    UAM_XT_OPT_UNALIGNED_EXC = 1u << 18,/* misaligned accesses raise LoadStoreAlignment
                                         * (otherwise the hardware handles them) */
    UAM_XT_OPT_REGION_PROT = 1u << 19,  /* 8 x 512 MiB region attributes (WITLB/WDTLB) */
    UAM_XT_OPT_COPROC      = 1u << 20,  /* CPENABLE */
    UAM_XT_OPT_DEBUG       = 1u << 21,  /* BREAK, DEBUGCAUSE, IBREAK/DBREAK registers */
    UAM_XT_OPT_PRID        = 1u << 22,
    UAM_XT_OPT_EXTERN_REGS = 1u << 23,  /* RER, WER */
    UAM_XT_OPT_LONG_INSN   = 1u << 24,  /* op0 0xE/0xF are 4-byte instructions (TIE) */
    UAM_XT_OPT_FP_EXT      = 1u << 25,  /* FP divide/square-root steps (DIV0.S, ...) and
                                         * post-increment FP loads/stores (LSIP, ...) */
    UAM_XT_OPT_SALT        = 1u << 26,  /* SALT, SALTU (LX7) */
    UAM_XT_OPT_S32NB       = 1u << 27,  /* S32NB (store, not buffered) */
    UAM_XT_OPT_MEMCTL      = 1u << 28,  /* the MEMCTL special register (LX6, LX7) */
};

/* Interrupt types (uam_xtensa_irq.type). */
enum {
    UAM_XT_INT_NONE,       /* not configured */
    UAM_XT_INT_LEVEL,      /* external, level-triggered */
    UAM_XT_INT_EDGE,       /* external, rising edge */
    UAM_XT_INT_NMI,        /* external, rising edge, not maskable (level nlevel + 1) */
    UAM_XT_INT_SOFTWARE,   /* INTSET / INTCLEAR */
    UAM_XT_INT_TIMER,      /* a CCOMPARE match (uam_xtensa_config.timer_int) */
    UAM_XT_INT_PROFILING,  /* external, level (performance monitor) */
    UAM_XT_INT_WRITE_ERR,  /* external, rising edge (bus error on a write) */
};

/* The dynamic vectors, as offsets from VECBASE (uam_xtensa_config.vec). */
enum {
    UAM_XT_VEC_WO4, UAM_XT_VEC_WU4, UAM_XT_VEC_WO8, UAM_XT_VEC_WU8,
    UAM_XT_VEC_WO12, UAM_XT_VEC_WU12,
    UAM_XT_VEC_LEVEL2, UAM_XT_VEC_LEVEL3, UAM_XT_VEC_LEVEL4, UAM_XT_VEC_LEVEL5,
    UAM_XT_VEC_LEVEL6, UAM_XT_VEC_LEVEL7,
    UAM_XT_VEC_KERNEL, UAM_XT_VEC_USER, UAM_XT_VEC_DOUBLE,
    UAM_XT_VEC__COUNT
};

/* Region Protection rights of a region attribute (uam_xtensa_config.ca_rights). */
enum { UAM_XT_CA_X = 1u << 0, UAM_XT_CA_R = 1u << 1, UAM_XT_CA_W = 1u << 2 };

typedef struct uam_xtensa_irq {
    uint8_t type;    /* UAM_XT_INT_* */
    uint8_t level;   /* 1 .. nlevel; nlevel + 1 for the NMI */
} uam_xtensa_irq;

typedef struct uam_xtensa_config {
    const char    *name;
    uint32_t       options;        /* UAM_XT_OPT_* */
    unsigned       nareg;          /* physical address registers: 16 (no windows), 32, 64 */
    unsigned       ninterrupt;     /* 0..32 */
    uam_xtensa_irq irq[32];
    unsigned       nlevel;         /* interrupt levels, level 1 included (NLEVEL) */
    unsigned       excm_level;     /* EXCMLEVEL */
    unsigned       debug_level;    /* DEBUGLEVEL (with UAM_XT_OPT_DEBUG) */
    bool           nmi;            /* an NMI at level nlevel + 1 */
    unsigned       nccompare;      /* 0..3 */
    uint8_t        timer_int[3];   /* the interrupt each CCOMPARE match raises */
    unsigned       nibreak, ndbreak;
    unsigned       nmisc;          /* MISC0.. (0..4) */
    uint8_t        cp_mask;        /* coprocessors CPENABLE gates (bit 0: the FPU) */
    uint32_t       reset_vector;   /* where a reset starts (the static vector the chip selects) */
    uint32_t       vecbase;        /* VECBASE at reset */
    uint16_t       vec[UAM_XT_VEC__COUNT];  /* offsets from VECBASE (unused levels: 0) */
    uint8_t        ca_rights[16];  /* rights of each region attribute (UAM_XT_CA_*) */
    uint32_t       prid;
    uint32_t       configid0, configid1;   /* 0: the registers do not exist */
} uam_xtensa_config;

/* The cores of the Espressif chips, from their published configuration headers. */
extern const uam_xtensa_config uam_xtensa_lx106;  /* LX106: ESP8266 */
extern const uam_xtensa_config uam_xtensa_lx6;    /* LX6 as configured in the ESP32 */
extern const uam_xtensa_config uam_xtensa_lx7;    /* LX7 as configured in the ESP32-S3 */

/* Select the CPU model of an Xtensa hart. Takes effect at the next reset. Returns
 * UAM_ERR_INVALID if cpu is not an Xtensa hart or the configuration is inconsistent. */
uam_status uam_xtensa_configure(uam_cpu *cpu, const uam_xtensa_config *cfg);
/* The model in effect, or NULL if cpu is not an Xtensa hart. */
const uam_xtensa_config *uam_xtensa_config_of(uam_cpu *cpu);
/* PRID of this hart (default: the model's), e.g. per processor of a multicore board. */
void uam_xtensa_set_prid(uam_cpu *cpu, uint32_t prid);

/* The External Register Interface (RER, WER): 32-bit registers at 32-bit addresses. */
typedef uint32_t (*uam_xtensa_eri_read_fn)(void *ctx, uint32_t addr);
typedef void (*uam_xtensa_eri_write_fn)(void *ctx, uint32_t addr, uint32_t val);
void uam_xtensa_set_eri(uam_cpu *cpu, uam_xtensa_eri_read_fn rd, uam_xtensa_eri_write_fn wr,
                        void *ctx);

/* True if `isa` is one of the Xtensa descriptors. */
static inline bool uam_xtensa_is(const uam_isa *isa)
{
    return isa == &uam_isa_xtensa || isa == &uam_isa_xtensaeb;
}

#endif /* UAM_ISA_XTENSA_H */
