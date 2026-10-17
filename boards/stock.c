/*
 * stock.c - Registration of the stock boards (uam/boards.h).
 */
#include "uam/boards.h"

void uam_boards_register_stock(void)
{
    uam_board_register(&uam_board_virt);
    uam_board_register(&uam_board_mipssim);
    uam_board_register(&uam_board_mipssim_el);
}
