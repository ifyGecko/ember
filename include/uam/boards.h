/*
 * uam/boards.h - The stock boards shipped with UAM.
 *
 * Kept out of the core library: a program that only needs its own board does
 * not link these (or their ISAs and devices). uam-run registers them all.
 */
#ifndef UAM_BOARDS_H
#define UAM_BOARDS_H

#include "uam/machine.h"

extern const uam_board uam_board_virt;        /* RV32IMAC, QEMU virt-compatible */
extern const uam_board uam_board_mipssim;     /* MIPS32 BE, QEMU mipssim-compatible */
extern const uam_board uam_board_mipssim_el;  /* MIPS32 LE, QEMU mipssim-compatible */

/* Register every stock board with uam_board_register(). */
void uam_boards_register_stock(void);

#endif /* UAM_BOARDS_H */
