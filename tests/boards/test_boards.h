/*
 * test_boards.h - Boards for ember's own tests (test_boards.c).
 */
#ifndef UAM_TEST_BOARDS_H
#define UAM_TEST_BOARDS_H

#include "uam/machine.h"

extern const uam_board test_board_x86_64;   /* "x86_64-test" */
extern const uam_board test_board_rv32;     /* "rv32-test" */

/* heap_hooks.c: with -o heap=BASE+SIZE, a guarded heap behind the firmware's allocation
 * functions (-o malloc=ADDR, free=ADDR, realloc=ADDR), on a RISC-V board. */
uam_status test_heap_hooks_install(uam_machine *m, const uam_board_config *cfg);

#endif /* UAM_TEST_BOARDS_H */
