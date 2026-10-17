/*
 * uam-run - Run a firmware image on a stock UAM board (see uam/runner.h).
 */
#include "uam/boards.h"
#include "uam/runner.h"

int main(int argc, char **argv)
{
    uam_boards_register_stock();
    return uam_run_main(argc, argv);
}
