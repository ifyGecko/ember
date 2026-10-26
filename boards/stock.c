/*
 * stock.c - Registration of the stock boards (uam/boards.h).
 */
#include <stdio.h>
#include <string.h>

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
    uam_board_register(&uam_board_ppc_ref);
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

uam_status uam_board_open_disk(const uam_board_config *cfg, const char *key,
                               uint32_t sector_size, uam_blockdev **out)
{
    *out = NULL;
    const char *path = uam_board_config_option(cfg, key);
    if (!path)
        return UAM_OK;
    char mkey[64];
    snprintf(mkey, sizeof(mkey), "%s-mode", key);
    const char *mode = uam_board_config_option(cfg, mkey);
    unsigned flags = 0;
    if (mode && strcmp(mode, "ro") == 0)
        flags = UAM_BLK_READONLY;
    else if (mode && strcmp(mode, "volatile") == 0)
        flags = UAM_BLK_VOLATILE;
    else if (mode && strcmp(mode, "rw") != 0)
        return UAM_ERR_INVALID;
    *out = uam_blockdev_open_file(path, sector_size, flags);
    return *out ? UAM_OK : UAM_ERR_INVALID;
}
