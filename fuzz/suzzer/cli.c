/*
 * cli.c - The runner extension for suzzer's formats (uam/runner.h).
 *
 *   --cov-sites LCV3        coverage sites from a suzzer patch list
 *   --cov-module NAME=BASE  a module's load base (repeatable; before the file's S2 table)
 *   --cov-scheme site|pc    coverage model (default site, like libcov)
 *   --cov-ngram N           N-gram length 1..8 (default 2, like libcov)
 *   --cov-rotate            N-gram rotation (libcov's LIBCOV_NGRAM_ROTATE=1)
 *   --cov-mode leapfrog|every  which executions count (default leapfrog, like libcov)
 *   --cov-strict            a site whose bytes differ from the patch list is fatal
 *   --cov-boundary LCVB1    iteration boundaries from a suzzer boundary file (fuzzing)
 *   --fuzz-szr HOST:PORT    take inputs from a suzzer brain over SZR1 (fuzzing)
 *
 * Site bytes are checked at the first begin when fuzzing, else when the run ends.
 */
#include <stdlib.h>
#include <string.h>

#include "suzzer.h"
#include "uam/runner.h"

#define MAX_MODULE_OPTS 32

typedef struct cli {
    const char     *sites_path, *boundary_path, *szr_addr;
    sz_module_base  mod[MAX_MODULE_OPTS];
    char           *mod_names[MAX_MODULE_OPTS];
    size_t          n_mod;
    sz_scheme       scheme;
    bool            every;      /* --cov-mode every: count every execution */
    bool            strict;
    /* after setup */
    sz_patchlist    pl;
    uam_cov_site   *sites;
    sz_namer        namer;
    bool            loaded;
    sz_bounds       bounds;
    sz_szr         *szr;
} cli;

static const uam_runner_opt opts[] = {
    { "cov-sites", true }, { "cov-module", true }, { "cov-scheme", true },
    { "cov-ngram", true }, { "cov-strict", false }, { "cov-boundary", true },
    { "cov-rotate", false }, { "fuzz-szr", true }, { "cov-mode", true },
};
enum { O_SITES, O_MODULE, O_SCHEME, O_NGRAM, O_STRICT, O_BOUNDARY, O_ROTATE, O_SZR, O_MODE };

static void *cli_create(void)
{
    cli *c = calloc(1, sizeof(*c));
    if (c)
        c->scheme = (sz_scheme){ .model = SZ_MODEL_SITE, .ngram = 2 };
    return c;
}

static void cli_destroy(void *st)
{
    cli *c = st;
    if (!c)
        return;
    sz_szr_close(c->szr);
    sz_namer_fini(&c->namer);
    sz_bounds_free(&c->bounds);
    free(c->sites);
    sz_patchlist_free(&c->pl);
    for (size_t i = 0; i < c->n_mod; i++)
        free(c->mod_names[i]);
    free(c);
}

static bool cli_option(void *st, size_t i, const char *arg, char *err, size_t errlen)
{
    cli *c = st;
    switch (i) {
    case O_SITES:    c->sites_path = arg; return true;
    case O_BOUNDARY: c->boundary_path = arg; return true;
    case O_SZR:      c->szr_addr = arg; return true;
    case O_STRICT:   c->strict = true; return true;
    case O_ROTATE:   c->scheme.rotate = true; return true;
    case O_MODE:
        /* libcov's LIBCOV_MODE: leapfrog re-arms the previous site when one fires, so
         * a site counts again only after another one did (FORMATS.md 5.7). oneshot is
         * not offered: it is a sanity-check mode, not a fuzzing one. */
        if (!strcmp(arg, "leapfrog"))
            c->every = false;
        else if (!strcmp(arg, "every"))
            c->every = true;
        else {
            snprintf(err, errlen, "expected leapfrog or every");
            return false;
        }
        return true;
    case O_SCHEME:
        if (!strcmp(arg, "site"))
            c->scheme.model = SZ_MODEL_SITE;
        else if (!strcmp(arg, "pc"))
            c->scheme.model = SZ_MODEL_PC;
        else {
            snprintf(err, errlen, "expected site or pc");
            return false;
        }
        return true;
    case O_NGRAM: {
        char *end;
        unsigned long n = strtoul(arg, &end, 10);
        if (*end || n < 1 || n > SZ_NGRAM_MAX) {
            snprintf(err, errlen, "expected an N-gram length from 1 to %u", SZ_NGRAM_MAX);
            return false;
        }
        c->scheme.ngram = (unsigned)n;
        return true;
    }
    case O_MODULE: {
        const char *eq = strchr(arg, '=');
        char *end;
        if (!eq || eq == arg || !eq[1] || c->n_mod == MAX_MODULE_OPTS) {
            snprintf(err, errlen, "expected NAME=BASE (at most %d)", MAX_MODULE_OPTS);
            return false;
        }
        uint64_t base = strtoull(eq + 1, &end, 0);
        if (*end) {
            snprintf(err, errlen, "bad base '%s'", eq + 1);
            return false;
        }
        char *name = strndup(arg, (size_t)(eq - arg));
        if (!name)
            return false;
        c->mod_names[c->n_mod] = name;
        c->mod[c->n_mod++] = (sz_module_base){ .name = name, .base = base };
        return true;
    }
    }
    return false;
}

static unsigned cli_provides(void *st)
{
    const cli *c = st;
    return (c->sites_path ? UAM_RUNNER_EXT_SITES : 0) | (c->boundary_path ? UAM_RUNNER_EXT_BEGIN : 0)
         | (c->szr_addr ? UAM_RUNNER_EXT_FRONTEND : 0);
}

/* Last in setup: the brain hears HELLO with the scheme the sites were built for. */
static uam_status connect_brain(cli *c, uam_runner_setup *s, char *err, size_t errlen)
{
    if (!c->szr_addr)
        return UAM_OK;
    char e[256];
    c->szr = sz_szr_connect(c->szr_addr, c->loaded ? sz_hello_scheme_flags(&c->scheme) : 0,
                            e, sizeof(e));
    if (!c->szr) {
        snprintf(err, errlen, "--fuzz-szr: %s", e);
        return UAM_ERR_INVALID;
    }
    s->frontend = &sz_szr_frontend;
    s->frontend_ctx = c->szr;
    return UAM_OK;
}

static uam_status cli_setup(void *st, uam_runner_setup *s, char *err, size_t errlen)
{
    cli *c = st;
    if (!c->sites_path && !c->boundary_path) {
        if (c->n_mod || c->strict) {
            snprintf(err, errlen, "--cov-module and --cov-strict need --cov-sites");
            return UAM_ERR_INVALID;
        }
        return connect_brain(c, s, err, errlen);
    }
    const unsigned aw = s->m->cpu.isa->addr_width;
    const bool have_base0 = s->have_image || s->have_raw_base;
    const uint64_t image_base = s->have_image ? s->image.link_base : s->raw_base;
    uint64_t base0 = image_base;

    if (c->sites_path) {
        char e[256];
        if (!sz_lcv3_load(c->sites_path, &c->pl, e, sizeof(e))) {
            snprintf(err, errlen, "%s: %s", c->sites_path, e);
            return UAM_ERR_INVALID;
        }
        c->loaded = true;
        uint64_t *bases = calloc(c->pl.n_modules, sizeof(*bases));
        if (!bases)
            return UAM_ERR_NOMEM;
        if (!sz_resolve_bases(&c->pl, c->mod, c->n_mod, have_base0, image_base, bases, err, errlen)) {
            free(bases);
            return UAM_ERR_INVALID;
        }
        base0 = bases[0];
        c->scheme.map_size = s->map_size_option ? s->map_size_option
                           : c->scheme.model == SZ_MODEL_SITE ? sz_map_size_for(c->pl.count, c->scheme.ngram)
                                                              : SZ_PC_MAP_SIZE;
        if (c->scheme.model == SZ_MODEL_SITE && c->pl.count > c->scheme.map_size)
            uam_log(UAM_LOG_WARN, "cov", "%u sites in a %u-byte map: site ids collide, and the map "
                    "differs from libcov's", c->pl.count, c->scheme.map_size);
        bool ok = sz_build_sites(&c->pl, bases, &c->scheme, aw, &c->sites, err, errlen)
               && sz_namer_init(&c->namer, &c->pl, c->sites);
        free(bases);
        if (!ok)
            return UAM_ERR_INVALID;
        s->sites = c->sites;
        s->n_sites = c->pl.count;
        s->scheme = sz_scheme_name(&c->scheme);
        s->map_size = c->scheme.map_size;
        sz_cov_history(&c->scheme, &s->history);
        s->skip_repeats = !c->every;
        s->namer = sz_namer_describe;
        s->namer_ctx = &c->namer;
        uam_log(UAM_LOG_INFO, "cov", "%s: %u sites in %u modules, scheme %s, map %u bytes",
                c->sites_path, c->pl.count, c->pl.n_modules, s->scheme, s->map_size);
    }
    if (c->boundary_path) {
        if (!c->sites_path && !have_base0) {
            snprintf(err, errlen, "--cov-boundary needs module 0's base: an image or --cov-sites");
            return UAM_ERR_INVALID;
        }
        sz_boundaries b;
        char e[256];
        if (!sz_lcvb_load(c->boundary_path, &b, e, sizeof(e))) {
            snprintf(err, errlen, "%s: %s", c->boundary_path, e);
            return UAM_ERR_INVALID;
        }
        bool ok = sz_boundaries_apply(&b, base0, aw, &c->bounds, e, sizeof(e));
        sz_boundaries_free(&b);
        if (!ok) {
            snprintf(err, errlen, "%s: %s", c->boundary_path, e);
            return UAM_ERR_INVALID;
        }
        s->have_begin = true;
        s->begin_pc = c->bounds.begin_pc;
        s->end_pcs = c->bounds.end_pcs;
        s->n_end_pcs = c->bounds.n_end_pcs;
        s->nested = c->bounds.nested;
    }
    return connect_brain(c, s, err, errlen);
}

static uam_status check(cli *c, uam_machine *m)
{
    size_t bad = sz_check_sites(m, &c->pl, c->sites, stderr, 5);
    if (bad && c->strict) {
        uam_log(UAM_LOG_ERROR, "cov", "--cov-strict: %zu sites do not match the patch list (wrong "
                "base or byte order?)", bad);
        return UAM_ERR_INVALID;
    }
    return UAM_OK;
}

static uam_status boot_check(void *ctx, uam_fuzz *f, uam_machine *m)
{
    (void)f;
    return check(ctx, m);
}

static uam_status cli_attach(void *st, uam_machine *m, uam_fuzz *f)
{
    (void)m;
    cli *c = st;
    if (c->loaded)
        uam_fuzz_set_boot_hook(f, boot_check, c);
    return UAM_OK;
}

static int cli_finish(void *st, uam_machine *m, bool fuzzed, int rc)
{
    cli *c = st;
    if (c->loaded && !fuzzed && check(c, m) != UAM_OK)
        return 2;   /* --cov-strict: wrong sites make every result meaningless */
    return rc;
}

const uam_runner_ext uam_suzzer_runner_ext = {
    .name = "suzzer",
    .opts = opts,
    .n_opts = UAM_ARRAY_LEN(opts),
    .help =
        "  suzzer formats:\n"
        "      --cov-sites LCV3    coverage sites from a suzzer patch list (site ids, orig bytes)\n"
        "      --cov-module N=BASE load base of patch-list module N (repeatable; default: the\n"
        "                          list's base table, then the image's link base for module 0)\n"
        "      --cov-scheme M      site (default) or pc: libcov's coverage model\n"
        "      --cov-ngram N       N-gram length 1..8 (default 2, like libcov)\n"
        "      --cov-rotate        N-gram rotation, as libcov's LIBCOV_NGRAM_ROTATE=1\n"
        "      --cov-mode M        leapfrog (default, like libcov: a site counts again only\n"
        "                          after another one) or every (each execution counts)\n"
        "      --cov-strict        fail if a site's bytes in memory differ from the list\n"
        "      --cov-boundary LCVB1  iteration begin/end from a suzzer boundary file\n"
        "      --fuzz-szr HOST:PORT  fuzz with inputs from a suzzer brain (SZR1 over TCP)\n",
    .create = cli_create,
    .destroy = cli_destroy,
    .option = cli_option,
    .provides = cli_provides,
    .setup = cli_setup,
    .attach = cli_attach,
    .finish = cli_finish,
};
