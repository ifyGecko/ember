/*
 * suzzer.h - ember's side of suzzer's published formats (fuzz/suzzer/, internal).
 *
 * The only place in ember that knows suzzer's coverage formulas and file formats
 * (docs/FORMATS.md in suzzer; the definitions come from the suzzer checkout the build
 * was given, src/lcv_formats.h - see docs/suzzer.md).
 * It turns an LCV3 patch list into a coverage site table (uam/cov.h) and an LCVB1
 * boundary file into fuzz-harness boundaries (uam/fuzz.h), and checks the sites
 * against guest memory. Everything else in ember sees only those generic forms.
 *
 * Supported schemes: the site-id and hashed-PC models, N = 1..8, rotation off or on,
 * which one UAM_OP_COV per site reproduces bit for bit. The per-site operands are
 *
 *   site, N=1:  A = site_id                      B = 0
 *   site, N>=2: A = site_id                      B = spread(site_id)
 *   pc,   N=1:  A = hash_pc(offset, module)      B = 0
 *   pc,   N>=2: A = hash_pc(offset, module)      B = A >> 1
 *
 * and the coverage state's history (uam_cov_history) does the rest: it folds the last
 * N-1 B values (depth N-1), each rotated by libcov's per-slot amount when rotation is
 * on (sz_cov_history; FORMATS.md 5.4). Conformance: every vector in suzzer's test/vectors.
 */
#ifndef UAM_FUZZ_SUZZER_H
#define UAM_FUZZ_SUZZER_H

#include <stdio.h>

#include "uam/cov.h"
#include "uam/fuzz.h"

/* ---- coverage scheme (FORMATS.md section 5) ---------------------------- */

typedef enum { SZ_MODEL_SITE, SZ_MODEL_PC } sz_model;

typedef struct sz_scheme {
    sz_model model;
    unsigned ngram;      /* 1..SZ_NGRAM_MAX */
    bool     rotate;     /* LIBCOV_NGRAM_ROTATE */
    uint32_t map_size;   /* a power of two */
} sz_scheme;

#define SZ_NGRAM_MAX 8u  /* libcov's cap on LIBCOV_NGRAM_N */

uint64_t sz_mix(uint64_t v);
uint32_t sz_spread(uint32_t site_id, uint32_t mask);                        /* sink_spread */
uint32_t sz_hash_pc(uint64_t offset, uint8_t module_idx, uint32_t mask);    /* sink_hash_pc */
/* libcov's map size for the site-id model (compute_map_size, default constants). */
uint32_t sz_map_size_for(uint64_t n_sites, unsigned ngram);
#define  SZ_PC_MAP_SIZE (1u << 16)
/* The COV operands of one site. `offset` is the site's address minus its module's
 * base, reduced to the target's pointer width. */
void     sz_site_ab(const sz_scheme *s, uint32_t site_id, uint64_t offset, uint8_t module_idx,
                    uint32_t *a, uint32_t *b);
/* The COV history the scheme needs: depth max(N-1, 1); with rotation, libcov's
 * per-slot rotation amounts for s->map_size (FORMATS.md 5.4). */
void     sz_cov_history(const sz_scheme *s, uam_cov_history *h);
/* The scheme bits of SZR1 HELLO flags and of the LCVS header flags. */
uint16_t sz_hello_scheme_flags(const sz_scheme *s);
uint32_t sz_lcvs_scheme_flags(const sz_scheme *s);
/* "site" / "pc" + N (+ "-rot"), e.g. "site-n2", "pc-n3-rot". */
const char *sz_scheme_name(const sz_scheme *s);

/* ---- LCV3 patch list ---------------------------------------------------- */

typedef struct sz_record {
    uint64_t offset;
    uint8_t  len, module;
    uint8_t  orig[4];
} sz_record;

typedef struct sz_patchlist {
    uint32_t   count;
    sz_record *rec;          /* in file order: rec[i] is site i */
    uint8_t    threading, flags, n_modules;
    char     **module;       /* names */
    bool       has_bases;    /* the S2 module base table was present */
    uint64_t  *bases;        /* n_modules link bases, if has_bases */
} sz_patchlist;

/* Parse (FORMATS.md 1.6 reader checklist). false with err on a malformed file. */
bool sz_lcv3_parse(const uint8_t *d, size_t len, sz_patchlist *out, char *err, size_t errlen);
bool sz_lcv3_load(const char *path, sz_patchlist *out, char *err, size_t errlen);
void sz_patchlist_free(sz_patchlist *p);

/* ---- LCVB1 boundary file ------------------------------------------------ */

typedef struct sz_marker {
    uint64_t offset;         /* from module 0's base */
    uint8_t  kind;           /* LIBCOV_BOUNDARY_KIND_* */
    uint8_t  depth_filter;
} sz_marker;

typedef struct sz_boundaries {
    uint32_t   count;
    sz_marker *rec;
} sz_boundaries;

bool sz_lcvb_parse(const uint8_t *d, size_t len, sz_boundaries *out, char *err, size_t errlen);
bool sz_lcvb_load(const char *path, sz_boundaries *out, char *err, size_t errlen);
void sz_boundaries_free(sz_boundaries *b);

/* ---- into ember --------------------------------------------------------- */

typedef struct sz_module_base {
    const char *name;
    uint64_t    base;
} sz_module_base;

/*
 * Each module's base, by priority: `cli` (--cov-module NAME=BASE), the file's S2 base
 * table, then for module 0 only `image_base` if have_image_base (the ELF's lowest
 * PT_LOAD vaddr, or --raw-base). Fails, naming the module, if one stays unknown or a
 * cli name is not in the file.
 */
bool sz_resolve_bases(const sz_patchlist *p, const sz_module_base *cli, size_t n_cli,
                      bool have_image_base, uint64_t image_base, uint64_t *bases,
                      char *err, size_t errlen);

/* The site table: record i at bases[module] + offset (mod 2^addr_width). */
bool sz_build_sites(const sz_patchlist *p, const uint64_t *bases, const sz_scheme *s,
                    unsigned addr_width, uam_cov_site **out, char *err, size_t errlen);

/* Describes the record at `pc` ("record 12 (module fw)"), for uam_cov_pass_set_namer.
 * ctx is an sz_namer built by sz_namer_init. */
typedef struct sz_namer {
    const sz_patchlist *p;
    uint64_t           *pc;      /* sorted pcs ... */
    uint32_t           *idx;     /* ... and their record indices */
} sz_namer;
bool sz_namer_init(sz_namer *n, const sz_patchlist *p, const uam_cov_site *sites);
void sz_namer_fini(sz_namer *n);
void sz_namer_describe(void *ctx, uint64_t pc, char *buf, size_t len);

/* An LCVB1 file as harness boundaries (uam_fuzz_config): one or more BEGIN markers (the
 * first in the file is begin_pc, the others more_begin_pcs; the harness snapshots at
 * whichever the boot reaches first) and any number of END markers (none: begin-only
 * mode). All markers must share one depth filter; depth-zero-only markers make the
 * boundaries nested. */
typedef struct sz_bounds {
    uint64_t  begin_pc;
    uint64_t *more_begin_pcs;  /* owned; sz_bounds_free */
    size_t    n_more_begin_pcs;
    uint64_t *end_pcs;     /* owned; sz_bounds_free */
    size_t    n_end_pcs;
    bool      nested;
} sz_bounds;
bool sz_boundaries_apply(const sz_boundaries *b, uint64_t base0, unsigned addr_width,
                         sz_bounds *out, char *err, size_t errlen);
void sz_bounds_free(sz_bounds *bd);

/*
 * ARM code addresses carry the instruction set in bit 0 (uam/isa/arm.h), while suzzer
 * marks a Thumb site by its length (2, at the instruction's even address). For an ARM
 * hart: set bit 0 of every Thumb site's pc, and of each boundary pc whose closest site
 * at or below it is a Thumb site (a marker in a Thumb function; with no sites, markers
 * stay ARM). Call before sz_namer_init.
 */
void sz_arm_thumb_sites(const sz_patchlist *p, uam_cov_site *sites);
void sz_arm_thumb_bounds(const sz_patchlist *p, const uam_cov_site *sites, sz_bounds *bd);

/* Compare each site's bytes in guest memory (through the ISA's translation, debug
 * reads) with orig[]. Prints a summary and the first `max_report` mismatches to `log`;
 * returns the number of mismatches (unreadable sites count as mismatches). */
size_t sz_check_sites(uam_machine *m, const sz_patchlist *p, const uam_cov_site *sites,
                      FILE *log, unsigned max_report);

/* ---- SZR1 executor (FORMATS.md section 4; frontend_szr1.c) ---------- */

/* A connection to a brain at HOST:PORT ([V6ADDR]:PORT), retried for a few seconds while
 * the brain starts, with TCP keepalive on. `flags` are HELLO's flags
 * (sz_hello_scheme_flags, or 0 when the maps follow no suzzer scheme). timeout_s > 0:
 * waiting longer than that for the brain's next frame (or for it to take ours) is an
 * error. Use it with sz_szr_frontend as the frontend context. */
typedef struct sz_szr sz_szr;
sz_szr *sz_szr_connect(const char *hostport, uint16_t flags, unsigned timeout_s, char *err,
                       size_t errlen);
void    sz_szr_close(sz_szr *z);
extern const uam_fuzz_frontend sz_szr_frontend;

/* ---- LCVS export (FORMATS.md section 3; lcvs.c) -------------------------- */

/* A region at PATH (LIBCOV_SHM_PATH's rule: a '/' after the first character means a
 * file, else a shared-memory name) for maps of map_size bytes and ring_size (2..65535)
 * slots; scheme_flags are sz_lcvs_scheme_flags, or 0. Refuses a region another live
 * process owns. sz_lcvs_observe is a uam_fuzz_observer_fn with the region as ctx: it
 * publishes INIT and each ok iteration, and keeps the live bitmap as their union. */
typedef struct sz_lcvs sz_lcvs;
sz_lcvs *sz_lcvs_create(const char *path, uint32_t map_size, unsigned ring_size, uint32_t scheme_flags,
                        char *err, size_t errlen);
void     sz_lcvs_close(sz_lcvs *l);
uint64_t sz_lcvs_published(const sz_lcvs *l);
void     sz_lcvs_observe(void *ctx, uint64_t id, int kind, uam_fuzz_verdict v, uint64_t detail,
                         const uint8_t *map, size_t map_size);

#endif /* UAM_FUZZ_SUZZER_H */
