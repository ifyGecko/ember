/*
 * rv.h - Internal definitions shared by the RISC-V lifter and system code.
 * Not part of the public interface.
 */
#ifndef UAM_RV_INTERNAL_H
#define UAM_RV_INTERNAL_H

#include "uam/isa/riscv.h"

/* Register file layout: x0..x31, 4 bytes each. x0's slot exists but is
 * never written (the lifter folds it to the constant 0). */
#define RV_REG_OFF(i)     ((uint32_t)(i) * 4u)
#define RV_REGFILE_SIZE   (32u * 4u)

/* Privilege levels (mstatus.MPP encoding). */
enum { RV_PRIV_U = 0, RV_PRIV_S = 1, RV_PRIV_M = 3 };

/* Exception causes (mcause, interrupt bit clear). */
enum {
    RV_EXC_INSN_MISALIGNED  = 0,
    RV_EXC_INSN_FAULT       = 1,
    RV_EXC_ILLEGAL          = 2,
    RV_EXC_BREAKPOINT       = 3,
    RV_EXC_LOAD_MISALIGNED  = 4,
    RV_EXC_LOAD_FAULT       = 5,
    RV_EXC_STORE_MISALIGNED = 6,
    RV_EXC_STORE_FAULT      = 7,
    RV_EXC_ECALL_U          = 8,   /* +priv gives the ecall cause for that mode */
    RV_EXC_INSN_PAGE_FAULT  = 12,
    RV_EXC_LOAD_PAGE_FAULT  = 13,
    RV_EXC_STORE_PAGE_FAULT = 15,
};

/* CSR numbers used by this module. */
enum {
    CSR_MSTATUS = 0x300, CSR_MISA = 0x301, CSR_MIE = 0x304, CSR_MTVEC = 0x305,
    CSR_MCOUNTEREN = 0x306, CSR_MCOUNTINHIBIT = 0x320,
    CSR_MSCRATCH = 0x340, CSR_MEPC = 0x341, CSR_MCAUSE = 0x342, CSR_MTVAL = 0x343,
    CSR_MIP = 0x344,
    CSR_PMPCFG0 = 0x3A0, CSR_PMPADDR0 = 0x3B0, CSR_PMPADDR63 = 0x3EF,
    CSR_MCYCLE = 0xB00, CSR_MINSTRET = 0xB02, CSR_MCYCLEH = 0xB80, CSR_MINSTRETH = 0xB82,
    CSR_CYCLE = 0xC00, CSR_INSTRET = 0xC02, CSR_CYCLEH = 0xC80, CSR_INSTRETH = 0xC82,
    CSR_MVENDORID = 0xF11, CSR_MARCHID = 0xF12, CSR_MIMPID = 0xF13, CSR_MHARTID = 0xF14,
    CSR_MCONFIGPTR = 0xF15,
};

#define MSTATUS_MIE   (1u << 3)
#define MSTATUS_MPIE  (1u << 7)
#define MSTATUS_MPP   (3u << 11)

/* Helper ids (index into the ISA helper table). */
enum { RV_HELPER_MRET = 0, RV_HELPER__COUNT };

/* ISA-private hart state. */
typedef struct rv_state {
    uint32_t mstatus;
    uint32_t mie;
    uint32_t lines;      /* hardware-driven mip bits (from irq_input) */
    uint32_t mtvec;
    uint32_t mscratch;
    uint32_t mepc;
    uint32_t mcause;
    uint32_t mtval;
    uint32_t mcounteren;
    uint32_t mcountinhibit;
    uint64_t counter_bias; /* written mcycle/minstret value minus icount */
} rv_state;

static inline rv_state *rv_st(uam_cpu *cpu) { return (rv_state *)cpu->isa_state; }

/* rv_lift.c */
uam_status rv_lift(uam_lift_ctx *lc);
uint32_t   rv_expand_compressed(uint16_t c); /* 0 if illegal */

/* rv_system.c */
extern const uam_helper_desc rv_helpers[RV_HELPER__COUNT];

#endif /* UAM_RV_INTERNAL_H */
