/*
 * uam-test-run - uam-run with the test boards (test_boards.c) registered too.
 */
#include "uam/boards.h"
#include "uam/runner.h"

#include "test_boards.h"

int main(int argc, char **argv)
{
    uam_boards_register_stock();
    uam_board_register(&test_board_x86_64);
    uam_board_register(&test_board_rv32);
    return uam_run_main(argc, argv);
}
