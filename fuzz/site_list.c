/*
 * site_list.c - Site lists and boundary files into ember's site tables and harness
 * boundaries; the site-byte check (cov_formats.h, docs/fuzz-formats.md).
 *
 * Every field is decoded explicitly as little-endian, so the readers work on any host.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "formats.h"
#include "cov_formats.h"

#define SITES_HDR  sizeof(struct uam_sites_header)
#define SITES_REC  sizeof(struct uam_sites_record)
#define BOUNDS_HDR sizeof(struct uam_bounds_header)
#define BOUNDS_REC sizeof(struct uam_bounds_record)

static bool fail(char *err, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static bool fail(char *err, size_t n, const char *fmt, ...)
{
    if (err && n) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, n, fmt, ap);
        va_end(ap);
    }
    return false;
}

static uint64_t le(const uint8_t *p, unsigned n)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < n; i++)
        v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static bool read_all(const char *path, uint8_t **d, size_t *len, char *err, size_t errlen)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return fail(err, errlen, "cannot open %s: %s", path, strerror(errno));
    size_t cap = 0, n = 0;
    uint8_t *buf = NULL;
    for (;;) {
        if (n == cap) {
            size_t ncap = cap ? cap * 2 : 65536;
            uint8_t *nb = realloc(buf, ncap);
            if (!nb) {
                free(buf);
                fclose(f);
                return fail(err, errlen, "out of memory");
            }
            buf = nb;
            cap = ncap;
        }
        size_t got = fread(buf + n, 1, cap - n, f);
        n += got;
        if (!got)
            break;
    }
    bool bad = ferror(f);
    fclose(f);
    if (bad) {
        free(buf);
        return fail(err, errlen, "reading %s failed", path);
    }
    *d = buf;
    *len = n;
    return true;
}

/* ---- site list ------------------------------------------------------------ */

void cf_sites_free(cf_sites *p)
{
    if (p->module)
        for (unsigned i = 0; i < p->n_modules; i++)
            free(p->module[i]);
    free(p->module);
    free(p->rec);
    free(p->bases);
    memset(p, 0, sizeof(*p));
}

static bool zero(const uint8_t *p, size_t n)
{
    while (n--)
        if (*p++)
            return false;
    return true;
}

bool cf_sites_parse(const uint8_t *d, size_t len, cf_sites *out, char *err, size_t errlen)
{
    memset(out, 0, sizeof(*out));
    if (len < 4 || memcmp(d, UAM_SITES_MAGIC, 4))
        return fail(err, errlen, "not a site list (no %s magic): make one with "
                    "fuzz/tools/gen_sites.py", UAM_SITES_MAGIC);
    if (len < SITES_HDR)
        return fail(err, errlen, "too short for a site list header (%zu bytes)", len);
    uint32_t version = (uint32_t)le(d + 4, 4);
    if (version != UAM_SITES_VERSION)
        return fail(err, errlen, "site list version %u is not supported (only %u)", version,
                    UAM_SITES_VERSION);
    uint32_t count = (uint32_t)le(d + 8, 4);
    uint8_t flags = d[12], n_mod = d[13];
    uint64_t mod_off = le(d + 16, 4), mod_size = le(d + 20, 4);
    if (flags & ~UAM_SITES_FLAG_BASES)
        return fail(err, errlen, "unknown header flags 0x%x", flags);
    if (!zero(d + 14, 2))
        return fail(err, errlen, "reserved header bytes are not 0");
    if ((uint64_t)count * SITES_REC > len - SITES_HDR)
        return fail(err, errlen, "%u records do not fit the file (%zu bytes)", count, len);
    if (!n_mod)
        return fail(err, errlen, "no modules");
    if (!mod_size || mod_size > UAM_SITES_NAMES_MAX)
        return fail(err, errlen, "bad module name table size %llu", (unsigned long long)mod_size);
    if (mod_off > len || mod_size > len - mod_off)
        return fail(err, errlen, "the module name table runs past the end of the file");
    if (d[mod_off + mod_size - 1] != 0)
        return fail(err, errlen, "the module name table does not end in NUL");
    uint64_t bo = UAM_SITES_BASES_OFFSET(mod_off, mod_size);
    if ((flags & UAM_SITES_FLAG_BASES) && (bo > len || (uint64_t)n_mod * 8 > len - bo))
        return fail(err, errlen, "the module base table runs past the end of the file");

    out->count = count;
    out->n_modules = n_mod;
    out->rec = calloc(count ? count : 1, sizeof(*out->rec));
    out->module = calloc(n_mod, sizeof(*out->module));
    if (!out->rec || !out->module) {
        cf_sites_free(out);
        return fail(err, errlen, "out of memory");
    }
    const char *names = (const char *)d + mod_off;
    size_t pos = 0;
    for (unsigned i = 0; i < n_mod; i++) {
        if (pos >= mod_size) {
            cf_sites_free(out);
            return fail(err, errlen, "the name table holds fewer than %u names", n_mod);
        }
        size_t n = strnlen(names + pos, mod_size - pos);
        if (!n) {
            cf_sites_free(out);
            return fail(err, errlen, "module %u has an empty name", i);
        }
        if (!(out->module[i] = strndup(names + pos, n))) {
            cf_sites_free(out);
            return fail(err, errlen, "out of memory");
        }
        pos += n + 1;
    }
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *r = d + SITES_HDR + (size_t)i * SITES_REC;
        cf_site *x = &out->rec[i];
        x->offset = le(r, 8);
        x->module = r[8];
        x->len = r[9];
        x->flags = r[10];
        memcpy(x->orig, r + 12, 4);
        const char *bad = x->module >= n_mod                 ? "names a module the list does not have"
                        : x->len > UAM_SITES_ORIG_MAX         ? "checks more than 4 bytes"
                        : x->flags & ~UAM_SITE_ALT_STATE      ? "has unknown flags"
                        : r[11] || !zero(x->orig + x->len, UAM_SITES_ORIG_MAX - x->len)
                                                              ? "has non-zero reserved bytes"
                                                              : NULL;
        if (bad) {
            cf_sites_free(out);
            return fail(err, errlen, "record %u %s", i, bad);
        }
    }
    if (flags & UAM_SITES_FLAG_BASES) {
        out->bases = malloc(sizeof(uint64_t) * n_mod);
        if (!out->bases) {
            cf_sites_free(out);
            return fail(err, errlen, "out of memory");
        }
        for (unsigned i = 0; i < n_mod; i++)
            out->bases[i] = le(d + bo + 8 * i, 8);
        out->has_bases = true;
    }
    return true;
}

bool cf_sites_load(const char *path, cf_sites *out, char *err, size_t errlen)
{
    uint8_t *d;
    size_t len;
    memset(out, 0, sizeof(*out));
    if (!read_all(path, &d, &len, err, errlen))
        return false;
    bool ok = cf_sites_parse(d, len, out, err, errlen);
    free(d);
    return ok;
}

/* ---- boundary file -------------------------------------------------------- */

void cf_markers_free(cf_markers *b)
{
    free(b->rec);
    memset(b, 0, sizeof(*b));
}

bool cf_bounds_parse(const uint8_t *d, size_t len, cf_markers *out, char *err, size_t errlen)
{
    memset(out, 0, sizeof(*out));
    if (len < 4 || memcmp(d, UAM_BOUNDS_MAGIC, 4))
        return fail(err, errlen, "not a boundary file (no %s magic): make one with "
                    "fuzz/tools/gen_boundaries.py", UAM_BOUNDS_MAGIC);
    if (len < BOUNDS_HDR)
        return fail(err, errlen, "too short for a boundary file header (%zu bytes)", len);
    uint32_t version = (uint32_t)le(d + 4, 4);
    if (version != UAM_BOUNDS_VERSION)
        return fail(err, errlen, "boundary file version %u is not supported (only %u)", version,
                    UAM_BOUNDS_VERSION);
    uint32_t count = (uint32_t)le(d + 8, 4);
    if (!zero(d + 12, 4))
        return fail(err, errlen, "reserved header bytes are not 0");
    if ((uint64_t)count * BOUNDS_REC > len - BOUNDS_HDR)
        return fail(err, errlen, "%u markers do not fit the file", count);
    out->rec = calloc(count ? count : 1, sizeof(*out->rec));
    if (!out->rec)
        return fail(err, errlen, "out of memory");
    out->count = count;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *r = d + BOUNDS_HDR + (size_t)i * BOUNDS_REC;
        out->rec[i] = (cf_marker){ .offset = le(r, 8), .kind = r[8], .depth = r[9], .flags = r[10] };
        const char *bad = r[8] > UAM_BOUNDS_END              ? "has an unknown kind"
                        : r[9] > UAM_BOUNDS_DEPTH_ZERO       ? "has an unknown depth filter"
                        : r[10] & ~UAM_SITE_ALT_STATE        ? "has unknown flags"
                        : !zero(r + 11, 5)                   ? "has non-zero reserved bytes"
                                                             : NULL;
        if (bad) {
            cf_markers_free(out);
            return fail(err, errlen, "marker %u %s", i, bad);
        }
    }
    return true;
}

bool cf_bounds_load(const char *path, cf_markers *out, char *err, size_t errlen)
{
    uint8_t *d;
    size_t len;
    memset(out, 0, sizeof(*out));
    if (!read_all(path, &d, &len, err, errlen))
        return false;
    bool ok = cf_bounds_parse(d, len, out, err, errlen);
    free(d);
    return ok;
}

/* ---- into ember --------------------------------------------------------- */

bool cf_resolve_bases(const cf_sites *p, const cf_module_base *cli, size_t n_cli,
                      bool have_image_base, uint64_t image_base, uint64_t *bases,
                      char *err, size_t errlen)
{
    bool *known = calloc(p->n_modules, sizeof(*known));
    if (!known)
        return fail(err, errlen, "out of memory");
    for (size_t k = 0; k < n_cli; k++) {
        unsigned i = 0;
        while (i < p->n_modules && strcmp(p->module[i], cli[k].name))
            i++;
        if (i == p->n_modules) {
            free(known);
            return fail(err, errlen, "--cov-module %s: the site list has no such module", cli[k].name);
        }
        bases[i] = cli[k].base;
        known[i] = true;
    }
    bool ok = true;
    for (unsigned i = 0; i < p->n_modules && ok; i++) {
        if (known[i])
            continue;
        if (p->has_bases)
            bases[i] = p->bases[i];
        else if (i == 0 && have_image_base)
            bases[i] = image_base;
        else
            ok = fail(err, errlen, "no base for module %u (%s): give --cov-module %s=BASE",
                      i, p->module[i], p->module[i]);
    }
    free(known);
    return ok;
}

bool cf_build_sites(const cf_sites *p, const uint64_t *bases, const cf_scheme *s,
                    unsigned addr_width, uint64_t state, uam_cov_site **out,
                    char *err, size_t errlen)
{
    const uint64_t amask = uam_mask(addr_width);
    uam_cov_site *t = malloc(sizeof(*t) * (p->count ? p->count : 1));
    if (!t)
        return fail(err, errlen, "out of memory");
    for (uint32_t i = 0; i < p->count; i++) {
        const cf_site *r = &p->rec[i];
        t[i].pc = ((bases[r->module] + r->offset) & amask) | (r->flags & UAM_SITE_ALT_STATE ? state : 0);
        cf_site_ab(s, i, r->offset & amask, r->module, &t[i].a, &t[i].b);
    }
    *out = t;
    return true;
}

/* (pc, record index) pairs, sorted by pc. */
static int pair_cmp(const void *x, const void *y)
{
    const uint64_t *a = x, *b = y;
    return a[0] < b[0] ? -1 : a[0] > b[0];
}

bool cf_namer_init(cf_namer *n, const cf_sites *p, const uam_cov_site *sites)
{
    memset(n, 0, sizeof(*n));
    n->p = p;
    uint64_t *pairs = malloc(sizeof(uint64_t) * 2 * (p->count ? p->count : 1));
    n->pc = malloc(sizeof(uint64_t) * (p->count ? p->count : 1));
    n->idx = malloc(sizeof(uint32_t) * (p->count ? p->count : 1));
    if (!pairs || !n->pc || !n->idx) {
        free(pairs);
        cf_namer_fini(n);
        return false;
    }
    for (uint32_t i = 0; i < p->count; i++) {
        pairs[2 * i] = sites[i].pc;
        pairs[2 * i + 1] = i;
    }
    qsort(pairs, p->count, 2 * sizeof(uint64_t), pair_cmp);
    for (uint32_t i = 0; i < p->count; i++) {
        n->pc[i] = pairs[2 * i];
        n->idx[i] = (uint32_t)pairs[2 * i + 1];
    }
    free(pairs);
    return true;
}

void cf_namer_fini(cf_namer *n)
{
    free(n->pc);
    free(n->idx);
    n->pc = NULL;
    n->idx = NULL;
}

void cf_namer_describe(void *ctx, uint64_t pc, char *buf, size_t len)
{
    const cf_namer *n = ctx;
    size_t lo = 0, hi = n->p->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (n->pc[mid] < pc)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < n->p->count && n->pc[lo] == pc) {
        const cf_site *r = &n->p->rec[n->idx[lo]];
        snprintf(buf, len, "site-list record %u, module %u %s, offset 0x%llx", n->idx[lo],
                 r->module, n->p->module[r->module], (unsigned long long)r->offset);
    }
}

static uint64_t marker_pc(const cf_marker *m, uint64_t base0, uint64_t amask, uint64_t state)
{
    return ((base0 + m->offset) & amask) | (m->flags & UAM_SITE_ALT_STATE ? state : 0);
}

bool cf_bounds_apply(const cf_markers *b, uint64_t base0, unsigned addr_width, uint64_t state,
                     cf_bounds *out, char *err, size_t errlen)
{
    const uint64_t amask = uam_mask(addr_width);
    memset(out, 0, sizeof(*out));
    int begin = -1;
    size_t n_end = 0, n_begin = 0;
    for (uint32_t i = 0; i < b->count; i++) {
        if (b->rec[i].kind == UAM_BOUNDS_END) {
            n_end++;
            continue;
        }
        if (begin < 0)
            begin = (int)i;
        n_begin++;
    }
    if (begin < 0)
        return fail(err, errlen, "the boundary file has no BEGIN marker");
    /* Depth-zero markers: nested BEGIN/END arrivals (recursion) stay inside one
     * iteration. With "any depth" the iteration ends at the first END. */
    const uint8_t depth = b->rec[begin].depth;
    for (uint32_t i = 0; i < b->count; i++)
        if (b->rec[i].depth != depth)
            return fail(err, errlen, "boundary markers %d and %u have different depth filters",
                        begin, i);
    out->begin_pc = marker_pc(&b->rec[begin], base0, amask, state);
    out->nested = n_end && depth == UAM_BOUNDS_DEPTH_ZERO;
    if ((n_end && !(out->end_pcs = malloc(sizeof(uint64_t) * n_end)))
        || (n_begin > 1 && !(out->more_begin_pcs = malloc(sizeof(uint64_t) * (n_begin - 1))))) {
        cf_bounds_free(out);
        return fail(err, errlen, "out of memory");
    }
    for (uint32_t i = 0; i < b->count; i++) {
        uint64_t pc = marker_pc(&b->rec[i], base0, amask, state);
        if (b->rec[i].kind == UAM_BOUNDS_END)
            out->end_pcs[out->n_end_pcs++] = pc;
        else if ((int)i != begin)
            out->more_begin_pcs[out->n_more_begin_pcs++] = pc;
    }
    return true;
}

void cf_bounds_free(cf_bounds *bd)
{
    free(bd->end_pcs);
    free(bd->more_begin_pcs);
    memset(bd, 0, sizeof(*bd));
}

size_t cf_check_sites(uam_machine *m, const cf_sites *p, const uam_cov_site *sites,
                      FILE *log, unsigned max_report)
{
    uam_cpu *cpu = &m->cpu;
    size_t match = 0, bad = 0;
    for (uint32_t i = 0; i < p->count; i++) {
        const cf_site *r = &p->rec[i];
        uint8_t got[UAM_SITES_ORIG_MAX] = { 0 };
        bool ok = true;
        /* a pc may carry state bits (UAM_SITE_ALT_STATE); the bytes start without them */
        const uint64_t at = sites[i].pc & ~uam_isa_pc_state_bits(cpu->isa);
        for (unsigned k = 0; k < r->len && ok; k++) {
            uint64_t va = at + k, pa = va, v = 0;
            if (cpu->isa->translate) {
                uam_xlate xl = { 0 };
                ok = cpu->isa->translate(cpu, va, UAM_XLATE_DEBUG, &xl) == UAM_OK;
                pa = xl.paddr;
            }
            ok = ok && uam_bus_read(&m->bus, pa, 1, UAM_ACC_LE | UAM_ACC_DEBUG, &v) == UAM_OK;
            got[k] = (uint8_t)v;
        }
        if (ok && !memcmp(got, r->orig, r->len)) {
            match++;
            continue;
        }
        if (bad++ < max_report && log) {
            fprintf(log, "cov: site %u (module %s +0x%llx) at 0x%llx: ", i, p->module[r->module],
                    (unsigned long long)r->offset, (unsigned long long)sites[i].pc);
            if (!ok) {
                fprintf(log, "not readable\n");
            } else {
                for (unsigned k = 0; k < r->len; k++)
                    fprintf(log, "%02x", got[k]);
                fprintf(log, " in memory, ");
                for (unsigned k = 0; k < r->len; k++)
                    fprintf(log, "%02x", r->orig[k]);
                fprintf(log, " in the site list\n");
            }
        }
    }
    if (log)
        fprintf(log, "cov: site check: %zu of %u sites match the site list, %zu do not\n", match,
                p->count, bad);
    return bad;
}
