/*
 * uam/boards.h - The stock boards shipped with UAM.
 *
 * Kept out of the core library: a program that only needs its own board does
 * not link these (or their ISAs and devices). uam-run registers them all.
 */
#ifndef UAM_BOARDS_H
#define UAM_BOARDS_H

#include "uam/machine.h"

extern const uam_board uam_board_rv32_ref;    /* "rv32-ref": RV32IMAC */
extern const uam_board uam_board_mips_ref;    /* "mips-ref": MIPS32 big-endian */
extern const uam_board uam_board_mipsel_ref;  /* "mipsel-ref": MIPS32 little-endian */

/* Register every stock board with uam_board_register(). */
void uam_boards_register_stock(void);

#endif /* UAM_BOARDS_H */
