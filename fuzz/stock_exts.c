/*
 * stock_exts.c - The runner extensions uam_run_main uses (uam/runner.h).
 *
 * Lives with the fuzz layer so that the runner itself names no fuzzer. The suzzer
 * extension is included when the build found a suzzer checkout (UAM_HAVE_SUZZER;
 * see docs/suzzer.md); without one ember runs on its own and the list is empty.
 */
#include "uam/runner.h"

#ifdef UAM_HAVE_SUZZER
extern const uam_runner_ext uam_suzzer_runner_ext;   /* fuzz/suzzer/cli.c */
const uam_runner_ext *const uam_runner_stock_exts[] = { &uam_suzzer_runner_ext };
const size_t uam_runner_n_stock_exts = UAM_ARRAY_LEN(uam_runner_stock_exts);
#else
const uam_runner_ext *const uam_runner_stock_exts[] = { NULL };
const size_t uam_runner_n_stock_exts = 0;
#endif
