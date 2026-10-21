/*
 * stock.c - Registration of the stock boards (uam/boards.h).
 */
#include "uam/boards.h"
#include "uam/dev/uart16550.h"

void uam_boards_register_stock(void)
{
    uam_board_register(&uam_board_rv32_ref);
    uam_board_register(&uam_board_mips_ref);
    uam_board_register(&uam_board_mipsel_ref);
    uam_board_register(&uam_board_arm_ref);
    uam_board_register(&uam_board_armeb_ref);
    uam_board_register(&uam_board_x86_64_ref);
}

static uam_status uart_input(void *ctx, uam_machine *m, const uint8_t *data, size_t len)
{
    (void)m;
    uam_uart16550_feed(ctx, data, len);   /* the excess is dropped, as on a real line */
    return UAM_OK;
}

uam_status uam_board_add_uart_input(uam_machine *m, const char *name, uam_device *uart)
{
    return uam_machine_add_input(m, name, uart_input, uart);
}
