/*
 * main.c - An external program: register our board, then run the stock
 * command line (uam/runner.h). The stock boards are not linked in.
 */
#include "uam/runner.h"

#include "ext_board.h"

int main(int argc, char **argv)
{
    uam_board_register(&ext_board_mips);
    return uam_run_main(argc, argv);
}
