/*
 * uam/isa/riscv.h - Public face of the RISC-V ISA module.
 *
 * Boards use this to instantiate a RISC-V hart and to wire interrupt lines
 * into it. Line numbers equal the mip bit positions.
 */
#ifndef UAM_ISA_RISCV_H
#define UAM_ISA_RISCV_H

#include "uam/isa.h"

/* RV32IMAC + Zicsr + Zifencei, machine mode only. */
extern const uam_isa uam_isa_rv32;

enum uam_rv_irq_line {
    UAM_RV_IRQ_MSI = 3,   /* machine software interrupt (CLINT msip) */
    UAM_RV_IRQ_MTI = 7,   /* machine timer interrupt (CLINT mtimecmp) */
    UAM_RV_IRQ_MEI = 11,  /* machine external interrupt (PLIC context) */
};

#endif /* UAM_ISA_RISCV_H */
