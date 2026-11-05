/*
 * testbrd_hle.h - firmware functions replaced by native code (see testbrd_hle.c).
 */
#ifndef TESTBRD_HLE_H
#define TESTBRD_HLE_H

#include "uam/machine.h"

uam_status testbrd_hle_install(uam_machine *m, const uam_board_config *cfg);

#endif
