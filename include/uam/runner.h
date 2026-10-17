/*
 * uam/runner.h - The stock command-line runner as a library entry point.
 *
 * uam-run is `uam_boards_register_stock(); return uam_run_main(argc, argv);`.
 * An out-of-tree project registers its own board(s) instead and gets the same
 * command line: board selection, image loading (raw or ELF, plus named images
 * the board places itself), console and network backends, board options,
 * tracing, PC breakpoints, a gdb server, statistics, clean shutdown on
 * SIGINT/SIGTERM (device destroy hooks always run), and exit-status mapping.
 */
#ifndef UAM_RUNNER_H
#define UAM_RUNNER_H

int uam_run_main(int argc, char **argv);

#endif /* UAM_RUNNER_H */
