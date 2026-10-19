/*
 * uam/runner.h - The stock command-line runner as a library entry point.
 *
 * uam-run is `uam_boards_register_stock(); return uam_run_main(argc, argv);`.
 * An out-of-tree project registers its own board(s) instead and gets the same
 * command line: board selection, image loading (raw or ELF, plus named images
 * the board places itself), console and network backends, board options,
 * per-register MMIO rules and fuzz input (--mmio-rule, --mmio-input),
 * the interpreter or the JIT (--jit, --no-chain), instruction / virtual-time /
 * stall bounds, snapshots (--save-snapshot, --load-snapshot, --snapshot-at),
 * tracing, PC breakpoints, a gdb server, coverage and the fuzz harness
 * (--cov-*, --fuzz-*), statistics, clean shutdown on SIGINT/SIGTERM (device
 * destroy hooks always run), and exit-status mapping.
 * The board must support snapshots (device save/restore) for the snapshot
 * and fuzz options; everything else works with any board.
 *
 * Extensions add options without the runner knowing what they are for: a
 * coverage-site source, iteration boundaries from a file, a fuzz frontend, a
 * side channel watching the harness. uam_run_main uses the stock extensions
 * (uam_runner_stock_exts, built with the fuzz library).
 *
 * Dependencies: machine.h, loader.h, fuzz.h.
 */
#ifndef UAM_RUNNER_H
#define UAM_RUNNER_H

#include "uam/fuzz.h"
#include "uam/loader.h"
#include "uam/machine.h"

typedef struct uam_runner_opt {
    const char *name;      /* long option, without "--" */
    bool        has_arg;
} uam_runner_opt;

/* What an extension contributes, decided once the machine is built and the image
 * loaded (uam_runner_ext.setup). Fields left zero contribute nothing. */
typedef struct uam_runner_setup {
    /* in */
    uam_machine          *m;
    bool                  have_image;
    uam_image_info        image;           /* the loaded image (link_base: module 0's base) */
    bool                  have_raw_base;
    uint64_t              raw_base;
    uint32_t              map_size_option; /* --cov-map-size if given, else 0 */
    /* out: coverage sites (owned by the extension, valid until it is destroyed) */
    const uam_cov_site   *sites;
    size_t                n_sites;
    const char           *scheme;          /* names the scheme for frontends */
    uint32_t              map_size;        /* 0: the runner's --cov-map-size */
    uam_cov_history       history;         /* COV history shape (zero: classic edges) */
    uam_cov_namer_fn      namer;           /* optional, see uam_cov_pass_set_namer */
    void                 *namer_ctx;
    /* out: iteration boundaries (turn on the fuzz harness) */
    bool                  have_begin;
    uint64_t              begin_pc;
    const uint64_t       *end_pcs;         /* none: begin-only (owned by the extension) */
    size_t                n_end_pcs;
    bool                  nested;
    /* out: a fuzz frontend in place of --fuzz-replay */
    const uam_fuzz_frontend *frontend;
    void                 *frontend_ctx;
} uam_runner_setup;

/* What an extension will contribute, known right after option parsing. */
enum uam_runner_ext_provides {
    UAM_RUNNER_EXT_SITES    = 1u << 0,
    UAM_RUNNER_EXT_BEGIN    = 1u << 1,
    UAM_RUNNER_EXT_FRONTEND = 1u << 2,
};

typedef struct uam_runner_ext {
    const char           *name;
    const uam_runner_opt *opts;
    size_t                n_opts;
    const char           *help;            /* lines for --help */
    void      *(*create)(void);
    void       (*destroy)(void *st);
    /* opts[i] was given (arg is NULL without an argument). false: usage error. */
    bool       (*option)(void *st, size_t i, const char *arg, char *err, size_t errlen);
    unsigned   (*provides)(void *st);      /* enum uam_runner_ext_provides */
    /* The machine is built and the image loaded. */
    uam_status (*setup)(void *st, uam_runner_setup *s, char *err, size_t errlen);
    /* Fuzzing: the harness exists and has not booted (hooks, observers). Optional. */
    uam_status (*attach)(void *st, uam_machine *m, uam_fuzz *f);
    /* After the run, with the exit status so far; returns the final one. Optional. */
    int        (*finish)(void *st, uam_machine *m, bool fuzzed, int rc);
} uam_runner_ext;

/* The stock runner with the stock extensions. */
int uam_run_main(int argc, char **argv);
/* The stock runner with the given extensions. */
int uam_run_main_ext(int argc, char **argv, const uam_runner_ext *const *exts, size_t n_exts);

/* The stock extensions (fuzz/stock_exts.c). */
extern const uam_runner_ext *const uam_runner_stock_exts[];
extern const size_t                uam_runner_n_stock_exts;

#endif /* UAM_RUNNER_H */
