/*
 * stock_exts.c - The runner extensions uam_run_main uses (uam/runner.h).
 *
 * Lives with the fuzz layer so that the runner itself names none of them: site lists,
 * boundary files, the coverage region and the fuzz network frontend (fuzz/cov_cli.c).
 */
#include "uam/runner.h"

extern const uam_runner_ext uam_fuzz_lists_runner_ext;   /* fuzz/cov_cli.c */
const uam_runner_ext *const uam_runner_stock_exts[] = { &uam_fuzz_lists_runner_ext };
const size_t uam_runner_n_stock_exts = UAM_ARRAY_LEN(uam_runner_stock_exts);
