/*
 * cov_cli.c - The runner extension for site lists, boundary files, the coverage region
 * and the fuzz network frontend (uam/runner.h; formats in docs/fuzz-formats.md).
 *
 *   --cov-sites FILE        coverage sites from a site list (fuzz/tools/gen_sites.py)
 *   --cov-module NAME=BASE  a module's load base (repeatable; before the list's base table)
 *   --cov-scheme site|pc    coverage model (default site)
 *   --cov-ngram N           N-gram length 1..8 (default 2)
 *   --cov-rotate            N-gram rotation
 *   --cov-mode leapfrog|every  which executions count (default leapfrog)
 *   --cov-strict            a site whose bytes differ from the site list is fatal
 *   --cov-boundary FILE     iteration boundaries from a boundary file (fuzzing)
 *   --fuzz-net HOST:PORT    take inputs from a fuzz brain over TCP (fuzzing)
 *   --fuzz-net-timeout SEC  give up when the brain sends nothing for SEC seconds
 *   --cov-shm PATH          publish the maps in a coverage region (fuzzing)
 *   --cov-shm-ring N        its ring size (default 8)
 *
 * Site bytes are checked at the first begin when fuzzing, else when the run ends.
 */
#include <stdlib.h>
#include <string.h>

#include "cov_formats.h"
#include "uam/runner.h"

#define MAX_MODULE_OPTS 32

typedef struct cli {
    const char     *sites_path, *boundary_path, *net_addr, *shm_path;
    unsigned        shm_ring;
    unsigned        net_timeout;
    cf_module_base  mod[MAX_MODULE_OPTS];
    char           *mod_names[MAX_MODULE_OPTS];
    size_t          n_mod;
    cf_scheme       scheme;
    bool            every;      /* --cov-mode every: count every execution */
    bool            strict;
    /* after setup */
    cf_sites        pl;
    uam_cov_site   *sites;
    cf_namer        namer;
    bool            loaded;
    cf_bounds       bounds;
    cf_net         *net;
    cf_region      *shm;
} cli;

static const uam_runner_opt opts[] = {
    { "cov-sites", true }, { "cov-module", true }, { "cov-scheme", true },
    { "cov-ngram", true }, { "cov-strict", false }, { "cov-boundary", true },
    { "cov-rotate", false }, { "fuzz-net", true }, { "cov-mode", true },
    { "cov-shm", true }, { "cov-shm-ring", true }, { "fuzz-net-timeout", true },
};
enum { O_SITES, O_MODULE, O_SCHEME, O_NGRAM, O_STRICT, O_BOUNDARY, O_ROTATE, O_NET, O_MODE,
       O_SHM, O_SHM_RING, O_NET_TIMEOUT };

static void *cli_create(void)
{
    cli *c = calloc(1, sizeof(*c));
    if (c) {
        c->scheme = (cf_scheme){ .model = CF_MODEL_SITE, .ngram = 2 };
        c->shm_ring = 8;
    }
    return c;
}

static void cli_destroy(void *st)
{
    cli *c = st;
    if (!c)
        return;
    cf_net_close(c->net);
    cf_region_close(c->shm);
    cf_namer_fini(&c->namer);
    cf_bounds_free(&c->bounds);
    free(c->sites);
    cf_sites_free(&c->pl);
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
    case O_NET:      c->net_addr = arg; return true;
    case O_SHM:      c->shm_path = arg; return true;
    case O_SHM_RING: {
        char *end;
        unsigned long n = strtoul(arg, &end, 10);
        if (*end || n < 2 || n > 0xffff) {
            snprintf(err, errlen, "expected a ring size from 2 to 65535");
            return false;
        }
        c->shm_ring = (unsigned)n;
        return true;
    }
    case O_NET_TIMEOUT: {
        char *end;
        unsigned long n = strtoul(arg, &end, 10);
        if (*end || !*arg || n > 86400) {
            snprintf(err, errlen, "expected seconds from 0 (no limit) to 86400");
            return false;
        }
        c->net_timeout = (unsigned)n;
        return true;
    }
    case O_STRICT:   c->strict = true; return true;
    case O_ROTATE:   c->scheme.rotate = true; return true;
    case O_MODE:
        /* leapfrog: a site counts again only after another one did, so a tight loop
         * counts once per entry (docs/fuzz-formats.md "Which executions count"). */
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
            c->scheme.model = CF_MODEL_SITE;
        else if (!strcmp(arg, "pc"))
            c->scheme.model = CF_MODEL_PC;
        else {
            snprintf(err, errlen, "expected site or pc");
            return false;
        }
        return true;
    case O_NGRAM: {
        char *end;
        unsigned long n = strtoul(arg, &end, 10);
        if (*end || n < 1 || n > CF_NGRAM_MAX) {
            snprintf(err, errlen, "expected an N-gram length from 1 to %u", CF_NGRAM_MAX);
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
        c->mod[c->n_mod++] = (cf_module_base){ .name = name, .base = base };
        return true;
    }
    }
    return false;
}

static unsigned cli_provides(void *st)
{
    const cli *c = st;
    return (c->sites_path ? UAM_RUNNER_EXT_SITES : 0) | (c->boundary_path ? UAM_RUNNER_EXT_BEGIN : 0)
         | (c->net_addr ? UAM_RUNNER_EXT_FRONTEND : 0) | (c->shm_path ? UAM_RUNNER_EXT_OBSERVER : 0);
}

/* Last in setup: the brain hears HELLO with the scheme the sites were built for. */
static uam_status connect_brain(cli *c, uam_runner_setup *s, char *err, size_t errlen)
{
    if (!c->net_addr)
        return UAM_OK;
    char e[256];
    c->net = cf_net_connect(c->net_addr, c->loaded ? (uint16_t)cf_scheme_flags(&c->scheme) : 0,
                            c->net_timeout, e, sizeof(e));
    if (!c->net) {
        snprintf(err, errlen, "--fuzz-net: %s", e);
        return UAM_ERR_INVALID;
    }
    s->frontend = &cf_net_frontend;
    s->frontend_ctx = c->net;
    return UAM_OK;
}

/* The brain saves each crash or hang X with its notes in X.info: replaying its crashes/
 * or hangs/ directory runs the inputs, not the notes. */
static const char *const brain_sidecars[] = { ".info" };

static uam_status cli_setup(void *st, uam_runner_setup *s, char *err, size_t errlen)
{
    cli *c = st;
    s->replay_sidecars = brain_sidecars;
    s->n_replay_sidecars = UAM_ARRAY_LEN(brain_sidecars);
    if (c->net_timeout && !c->net_addr) {
        snprintf(err, errlen, "--fuzz-net-timeout needs --fuzz-net");
        return UAM_ERR_INVALID;
    }
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
        if (!cf_sites_load(c->sites_path, &c->pl, e, sizeof(e))) {
            snprintf(err, errlen, "%s: %s", c->sites_path, e);
            return UAM_ERR_INVALID;
        }
        c->loaded = true;
        uint64_t *bases = calloc(c->pl.n_modules, sizeof(*bases));
        if (!bases)
            return UAM_ERR_NOMEM;
        if (!cf_resolve_bases(&c->pl, c->mod, c->n_mod, have_base0, image_base, bases, err, errlen)) {
            free(bases);
            return UAM_ERR_INVALID;
        }
        base0 = bases[0];
        c->scheme.map_size = s->map_size_option ? s->map_size_option
                           : c->scheme.model == CF_MODEL_SITE ? cf_map_size_for(c->pl.count, c->scheme.ngram)
                                                              : CF_PC_MAP_SIZE;
        if (c->scheme.model == CF_MODEL_SITE && c->pl.count > c->scheme.map_size)
            uam_log(UAM_LOG_WARN, "cov", "%u sites in a %u-byte map: site ids collide",
                    c->pl.count, c->scheme.map_size);
        bool ok = cf_build_sites(&c->pl, bases, &c->scheme, aw, uam_isa_pc_state_bits(s->m->cpu.isa),
                                 &c->sites, err, errlen);
        ok = ok && cf_namer_init(&c->namer, &c->pl, c->sites);
        free(bases);
        if (!ok)
            return UAM_ERR_INVALID;
        s->sites = c->sites;
        s->n_sites = c->pl.count;
        s->scheme = cf_scheme_name(&c->scheme);
        s->map_size = c->scheme.map_size;
        cf_cov_history(&c->scheme, &s->history);
        s->skip_repeats = !c->every;
        s->namer = cf_namer_describe;
        s->namer_ctx = &c->namer;
        uam_log(UAM_LOG_INFO, "cov", "%s: %u sites in %u modules, scheme %s, map %u bytes",
                c->sites_path, c->pl.count, c->pl.n_modules, s->scheme, s->map_size);
    }
    if (c->boundary_path) {
        if (!c->sites_path && !have_base0) {
            snprintf(err, errlen, "--cov-boundary needs module 0's base: an image or --cov-sites");
            return UAM_ERR_INVALID;
        }
        cf_markers b;
        char e[256];
        if (!cf_bounds_load(c->boundary_path, &b, e, sizeof(e))) {
            snprintf(err, errlen, "%s: %s", c->boundary_path, e);
            return UAM_ERR_INVALID;
        }
        bool ok = cf_bounds_apply(&b, base0, aw, uam_isa_pc_state_bits(s->m->cpu.isa), &c->bounds,
                                  e, sizeof(e));
        cf_markers_free(&b);
        if (!ok) {
            snprintf(err, errlen, "%s: %s", c->boundary_path, e);
            return UAM_ERR_INVALID;
        }
        s->have_begin = true;
        s->begin_pc = c->bounds.begin_pc;
        s->more_begin_pcs = c->bounds.more_begin_pcs;
        s->n_more_begin_pcs = c->bounds.n_more_begin_pcs;
        s->end_pcs = c->bounds.end_pcs;
        s->n_end_pcs = c->bounds.n_end_pcs;
        s->nested = c->bounds.nested;
    }
    return connect_brain(c, s, err, errlen);
}

static uam_status check(cli *c, uam_machine *m)
{
    size_t bad = cf_check_sites(m, &c->pl, c->sites, stderr, 5);
    if (bad && c->strict) {
        uam_log(UAM_LOG_ERROR, "cov", "--cov-strict: %zu sites do not match the site list (wrong "
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
    cli *c = st;
    if (c->net)
        cf_net_attach(c->net, f, m->cpu.isa->default_endian == UAM_ACC_BE);
    if (c->loaded)
        uam_fuzz_set_boot_hook(f, boot_check, c);
    if (c->shm_path) {
        size_t map_size;
        uam_fuzz_map(f, &map_size);
        char e[300];
        c->shm = cf_region_create(c->shm_path, (uint32_t)map_size, c->shm_ring,
                                  c->loaded ? cf_scheme_flags(&c->scheme) : 0, e, sizeof(e));
        if (!c->shm) {
            uam_log(UAM_LOG_ERROR, "cov-region", "--cov-shm: %s", e);
            return UAM_ERR_INVALID;
        }
        uam_fuzz_set_observer(f, cf_region_observe, c->shm);
        uam_log(UAM_LOG_INFO, "cov-region", "%s: %zu-byte maps, %u ring slots", c->shm_path, map_size,
                c->shm_ring);
    }
    return UAM_OK;
}

static int cli_finish(void *st, uam_machine *m, bool fuzzed, int rc)
{
    cli *c = st;
    if (c->loaded && !fuzzed && check(c, m) != UAM_OK)
        return 2;   /* --cov-strict: wrong sites make every result meaningless */
    return rc;
}

const uam_runner_ext uam_fuzz_lists_runner_ext = {
    .name = "fuzz-lists",
    .opts = opts,
    .n_opts = UAM_ARRAY_LEN(opts),
    .help =
        "  site lists, boundary files, the coverage region and the fuzz brain:\n"
        "      --cov-sites FILE    coverage sites from a site list (gen_sites.py: site ids,\n"
        "                          instruction bytes)\n"
        "      --cov-module NAME=BASE  load base of the site-list module NAME (repeatable; default:\n"
        "                          the list's base table, then the image's link base for module 0)\n"
        "      --cov-scheme M      site (default: buckets from site ids) or pc (hashed pcs)\n"
        "      --cov-ngram N       N-gram length 1..8 (default 2)\n"
        "      --cov-rotate        rotate each history slot, so the order of sites counts\n"
        "      --cov-mode M        leapfrog (default: a site counts again only after another\n"
        "                          one) or every (each execution counts)\n"
        "      --cov-strict        fail if a site's bytes in memory differ from the list\n"
        "      --cov-boundary FILE iteration begin/end from a boundary file (gen_boundaries.py;\n"
        "                          any number of BEGIN and END markers)\n"
        "      --fuzz-net HOST:PORT  fuzz with inputs from a fuzz brain over TCP (fuzz_brain.py;\n"
        "                          TCP keepalive notices a vanished brain host)\n"
        "      --fuzz-net-timeout S  give up when the brain sends nothing for S seconds\n"
        "                          (default 0: wait as long as it takes)\n"
        "      --cov-shm PATH      publish INIT and each ok iteration's map in a coverage region\n"
        "                          (a file if PATH has a '/' after its start, else /dev/shm)\n"
        "      --cov-shm-ring N    ring slots in that region (default 8)\n",
    .create = cli_create,
    .destroy = cli_destroy,
    .option = cli_option,
    .provides = cli_provides,
    .setup = cli_setup,
    .attach = cli_attach,
    .finish = cli_finish,
};
