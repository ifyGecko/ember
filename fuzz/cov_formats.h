/*
 * cov_formats.h - Site lists, boundary files, the coverage region and the fuzz network
 * frontend (fuzz/, internal).
 *
 * The formats are defined in fuzz/formats.h and specified in docs/fuzz-formats.md. This
 * layer turns a site list into a coverage site table (uam/cov.h) and a boundary file
 * into harness boundaries (uam/fuzz.h), checks the sites against guest memory, publishes
 * maps in a coverage region and serves inputs from a fuzz brain. Everything else in ember
 * sees only those generic forms.
 *
 * Coverage schemes: the site-id and hashed-pc models, N = 1..8, rotation off or on, which
 * one UAM_OP_COV per site computes. The per-site operands are
 *
 *   site, N=1:  A = site_id                      B = 0
 *   site, N>=2: A = site_id                      B = spread(site_id)
 *   pc,   N=1:  A = hash_pc(offset, module)      B = 0
 *   pc,   N>=2: A = hash_pc(offset, module)      B = A >> 1
 *
 * and the coverage state's history (uam_cov_history) does the rest: it folds the last N-1
 * B values (depth N-1), each rotated by its slot's amount when rotation is on
 * (cf_cov_history; docs/fuzz-formats.md "Coverage buckets").
 */
#ifndef UAM_FUZZ_COV_FORMATS_H
#define UAM_FUZZ_COV_FORMATS_H

#include <stdio.h>

#include "uam/cov.h"
#include "uam/fuzz.h"

/* ---- coverage scheme ------------------------------------------------------ */

typedef enum { CF_MODEL_SITE, CF_MODEL_PC } cf_model;

typedef struct cf_scheme {
    cf_model model;
    unsigned ngram;      /* 1..CF_NGRAM_MAX */
    bool     rotate;
    uint32_t map_size;   /* a power of two */
} cf_scheme;

#define CF_NGRAM_MAX 8u

uint64_t cf_mix(uint64_t v);
uint32_t cf_spread(uint32_t site_id, uint32_t mask);
uint32_t cf_hash_pc(uint64_t offset, uint8_t module_idx, uint32_t mask);
/* The default map size for the site-id model with n_sites sites and N-gram length ngram. */
uint32_t cf_map_size_for(uint64_t n_sites, unsigned ngram);
#define  CF_PC_MAP_SIZE (1u << 16)
/* The COV operands of one site. `offset` is the site's address minus its module's
 * base, reduced to the target's pointer width. */
void     cf_site_ab(const cf_scheme *s, uint32_t site_id, uint64_t offset, uint8_t module_idx,
                    uint32_t *a, uint32_t *b);
/* The COV history the scheme needs: depth max(N-1, 1); with rotation, the per-slot
 * rotation amounts for s->map_size. */
void     cf_cov_history(const cf_scheme *s, uam_cov_history *h);
/* The scheme bits of HELLO flags and of the coverage region's header flags. */
uint32_t cf_scheme_flags(const cf_scheme *s);
/* "site" / "pc" + N (+ "-rot"), e.g. "site-n2", "pc-n3-rot". */
const char *cf_scheme_name(const cf_scheme *s);

/* ---- site list ------------------------------------------------------------ */

typedef struct cf_site {
    uint64_t offset;
    uint8_t  module, len, flags;   /* flags: UAM_SITE_ALT_STATE */
    uint8_t  orig[4];
} cf_site;

typedef struct cf_sites {
    uint32_t  count;
    cf_site  *rec;           /* in file order: rec[i] is site i */
    uint8_t   n_modules;
    char    **module;        /* names */
    bool      has_bases;     /* the module base table was present */
    uint64_t *bases;         /* n_modules link bases, if has_bases */
} cf_sites;

/* false with err on a malformed file (docs/fuzz-formats.md "Reading a site list"). */
bool cf_sites_parse(const uint8_t *d, size_t len, cf_sites *out, char *err, size_t errlen);
bool cf_sites_load(const char *path, cf_sites *out, char *err, size_t errlen);
void cf_sites_free(cf_sites *p);

/* ---- boundary file -------------------------------------------------------- */

typedef struct cf_marker {
    uint64_t offset;         /* from module 0's base */
    uint8_t  kind;           /* UAM_BOUNDS_BEGIN / _END */
    uint8_t  depth;          /* UAM_BOUNDS_DEPTH_* */
    uint8_t  flags;          /* UAM_SITE_ALT_STATE */
} cf_marker;

typedef struct cf_markers {
    uint32_t   count;
    cf_marker *rec;
} cf_markers;

bool cf_bounds_parse(const uint8_t *d, size_t len, cf_markers *out, char *err, size_t errlen);
bool cf_bounds_load(const char *path, cf_markers *out, char *err, size_t errlen);
void cf_markers_free(cf_markers *b);

/* ---- into ember ----------------------------------------------------------- */

typedef struct cf_module_base {
    const char *name;
    uint64_t    base;
} cf_module_base;

/*
 * Each module's base, by priority: `cli` (--cov-module NAME=BASE), the file's base
 * table, then for module 0 only `image_base` if have_image_base (the ELF's lowest PT_LOAD
 * vaddr, or --raw-base). Fails, naming the module, if one stays unknown or a cli name is
 * not in the file.
 */
bool cf_resolve_bases(const cf_sites *p, const cf_module_base *cli, size_t n_cli,
                      bool have_image_base, uint64_t image_base, uint64_t *bases,
                      char *err, size_t errlen);

/* The site table: record i at bases[module] + offset (mod 2^addr_width), with `state`
 * (the ISA's pc state bits, uam_isa_pc_state_bits) set in each UAM_SITE_ALT_STATE site. */
bool cf_build_sites(const cf_sites *p, const uint64_t *bases, const cf_scheme *s,
                    unsigned addr_width, uint64_t state, uam_cov_site **out,
                    char *err, size_t errlen);

/* Describes the record at `pc` ("site-list record 12, module fw, ..."), for
 * uam_cov_pass_set_namer. ctx is a cf_namer built by cf_namer_init. */
typedef struct cf_namer {
    const cf_sites *p;
    uint64_t       *pc;      /* sorted pcs ... */
    uint32_t       *idx;     /* ... and their record indices */
} cf_namer;
bool cf_namer_init(cf_namer *n, const cf_sites *p, const uam_cov_site *sites);
void cf_namer_fini(cf_namer *n);
void cf_namer_describe(void *ctx, uint64_t pc, char *buf, size_t len);

/* A boundary file as harness boundaries (uam_fuzz_config): one or more BEGIN markers
 * (the first in the file is begin_pc, the others more_begin_pcs; the harness snapshots at
 * whichever the boot reaches first) and any number of END markers (none: begin-only
 * mode). All markers share one depth filter; depth-zero markers make the boundaries
 * nested. `state` is set in each UAM_SITE_ALT_STATE marker's pc. */
typedef struct cf_bounds {
    uint64_t  begin_pc;
    uint64_t *more_begin_pcs;  /* owned; cf_bounds_free */
    size_t    n_more_begin_pcs;
    uint64_t *end_pcs;         /* owned; cf_bounds_free */
    size_t    n_end_pcs;
    bool      nested;
} cf_bounds;
bool cf_bounds_apply(const cf_markers *b, uint64_t base0, unsigned addr_width, uint64_t state,
                     cf_bounds *out, char *err, size_t errlen);
void cf_bounds_free(cf_bounds *bd);

/* Compare each site's bytes in guest memory (through the ISA's translation, debug reads)
 * with orig[]. Prints a summary and the first `max_report` mismatches to `log`; returns
 * the number of mismatches (unreadable sites count as mismatches). */
size_t cf_check_sites(uam_machine *m, const cf_sites *p, const uam_cov_site *sites,
                      FILE *log, unsigned max_report);

/* ---- the fuzz network frontend (frontend_net.c) ------------------------------ */

/* A connection to a brain at HOST:PORT ([V6ADDR]:PORT), retried for a few seconds while
 * the brain starts, with TCP keepalive on. `flags` are HELLO's flags (cf_scheme_flags, or
 * 0 for block coverage). timeout_s > 0: waiting longer than that for the brain's next
 * frame (or for it to take ours) is an error. Use it with cf_net_frontend as the
 * frontend context. */
typedef struct cf_net cf_net;
cf_net *cf_net_connect(const char *hostport, uint16_t flags, unsigned timeout_s, char *err,
                       size_t errlen);
void    cf_net_close(cf_net *z);
extern const uam_fuzz_frontend cf_net_frontend;

/* ---- the coverage region (cov_region.c) ---------------------------------------- */

/* A region at PATH (a file if PATH has a '/' after its first character, else a POSIX
 * shared-memory name) for maps of map_size bytes and ring_size (2..65535) slots;
 * scheme_flags are cf_scheme_flags, or 0. Refuses a region another live process owns.
 * cf_region_observe is a uam_fuzz_observer_fn with the region as ctx: it publishes INIT
 * and each ok iteration, and keeps the live bitmap as their union. */
typedef struct cf_region cf_region;
cf_region *cf_region_create(const char *path, uint32_t map_size, unsigned ring_size,
                            uint32_t scheme_flags, char *err, size_t errlen);
void       cf_region_close(cf_region *l);
uint64_t   cf_region_published(const cf_region *l);
void       cf_region_observe(void *ctx, uint64_t id, int kind, uam_fuzz_verdict v,
                             uint64_t detail, const uint8_t *map, size_t map_size);

#endif /* UAM_FUZZ_COV_FORMATS_H */
