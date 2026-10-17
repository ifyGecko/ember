/*
 * stock.c - Registration of the stock boards (uam/boards.h).
 */
#include "uam/boards.h"

void uam_boards_register_stock(void)
{
    uam_board_register(&uam_board_rv32_ref);
    uam_board_register(&uam_board_mips_ref);
    uam_board_register(&uam_board_mipsel_ref);
}
