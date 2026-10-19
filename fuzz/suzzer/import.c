/*
 * import.c - LCV3 patch lists and LCVB1 boundary files into ember's site tables and
 * harness boundaries; the site-byte check (suzzer.h).
 *
 * Every field is decoded explicitly as little-endian, so the readers work on any host.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "lcv_formats.h"
#include "suzzer.h"

#define LCV3_HDR sizeof(struct patchlist_header_v3)
#define LCV3_REC sizeof(struct patchlist_record)
#define LCVB_HDR sizeof(struct lcvb_header)
#define LCVB_REC sizeof(struct lcvb_record)

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

/* ---- LCV3 --------------------------------------------------------------- */

void sz_patchlist_free(sz_patchlist *p)
{
    if (p->module)
        for (unsigned i = 0; i < p->n_modules; i++)
            free(p->module[i]);
    free(p->module);
    free(p->rec);
    free(p->bases);
    memset(p, 0, sizeof(*p));
}

bool sz_lcv3_parse(const uint8_t *d, size_t len, sz_patchlist *out, char *err, size_t errlen)
{
    memset(out, 0, sizeof(*out));
    if (len < LCV3_HDR)
        return fail(err, errlen, "too short for an LCV3 header (%zu bytes)", len);
    if (memcmp(d, LIBCOV_MAGIC, 4))
        return fail(err, errlen, "not an LCV3 patch list (magic %.4s)", (const char *)d);
    uint32_t version = (uint32_t)le(d + 4, 4);
    if (version != LIBCOV_VERSION)
        return fail(err, errlen, "LCV3 version %u is not supported (only %u)", version, LIBCOV_VERSION);
    uint32_t count = (uint32_t)le(d + 8, 4);
    uint8_t threading = d[12], flags = d[13], n_mod = d[14];
    uint64_t mod_off = le(d + 16, 4), mod_size = le(d + 20, 4);
    if ((uint64_t)count * LCV3_REC > len - LCV3_HDR)
        return fail(err, errlen, "%u records do not fit the file (%zu bytes)", count, len);
    if (!n_mod)
        return fail(err, errlen, "no modules");
    if (!mod_size || mod_size > LIBCOV_MAX_MODULES_BYTES)
        return fail(err, errlen, "bad module name table size %llu", (unsigned long long)mod_size);
    if (mod_off > len || mod_size > len - mod_off)
        return fail(err, errlen, "the module name table runs past the end of the file");
    if (d[mod_off + mod_size - 1] != 0)
        return fail(err, errlen, "the module name table does not end in NUL");

    out->count = count;
    out->threading = threading;
    out->flags = flags;
    out->n_modules = n_mod;
    out->rec = calloc(count ? count : 1, sizeof(*out->rec));
    out->module = calloc(n_mod, sizeof(*out->module));
    if (!out->rec || !out->module) {
        sz_patchlist_free(out);
        return fail(err, errlen, "out of memory");
    }
    const char *names = (const char *)d + mod_off;
    size_t pos = 0;
    for (unsigned i = 0; i < n_mod; i++) {
        if (pos >= mod_size) {
            sz_patchlist_free(out);
            return fail(err, errlen, "the name table holds fewer than %u names", n_mod);
        }
        size_t n = strnlen(names + pos, mod_size - pos);
        if (!n) {
            sz_patchlist_free(out);
            return fail(err, errlen, "module %u has an empty name", i);
        }
        if (!(out->module[i] = strndup(names + pos, n))) {
            sz_patchlist_free(out);
            return fail(err, errlen, "out of memory");
        }
        pos += n + 1;
    }
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *r = d + LCV3_HDR + (size_t)i * LCV3_REC;
        sz_record *x = &out->rec[i];
        x->offset = le(r, 8);
        x->len = r[8];
        x->module = r[9];
        memcpy(x->orig, r + 10, 4);
        if (x->module >= n_mod) {
            sz_patchlist_free(out);
            return fail(err, errlen, "record %u names module %u of %u", i, x->module, n_mod);
        }
        if (x->len == 0 || x->len > LIBCOV_PATCH_ORIG_MAX) {
            sz_patchlist_free(out);
            return fail(err, errlen, "record %u has length %u", i, x->len);
        }
    }
    /* S2 base table; ignored if it runs past the end (FORMATS.md 1.5). */
    uint64_t bo = LCV_MODULE_BASES_OFFSET(mod_off, mod_size);
    if ((flags & LCV_FLAG_MODULE_BASES) && bo <= len && (uint64_t)n_mod * 8 <= len - bo) {
        out->bases = malloc(sizeof(uint64_t) * n_mod);
        if (!out->bases) {
            sz_patchlist_free(out);
            return fail(err, errlen, "out of memory");
        }
        for (unsigned i = 0; i < n_mod; i++)
            out->bases[i] = le(d + bo + 8 * i, 8);
        out->has_bases = true;
    }
    return true;
}

bool sz_lcv3_load(const char *path, sz_patchlist *out, char *err, size_t errlen)
{
    uint8_t *d;
    size_t len;
    memset(out, 0, sizeof(*out));
    if (!read_all(path, &d, &len, err, errlen))
        return false;
    bool ok = sz_lcv3_parse(d, len, out, err, errlen);
    free(d);
    return ok;
}

/* ---- LCVB1 -------------------------------------------------------------- */

void sz_boundaries_free(sz_boundaries *b)
{
    free(b->rec);
    memset(b, 0, sizeof(*b));
}

bool sz_lcvb_parse(const uint8_t *d, size_t len, sz_boundaries *out, char *err, size_t errlen)
{
    memset(out, 0, sizeof(*out));
    if (len < LCVB_HDR)
        return fail(err, errlen, "too short for an LCVB1 header (%zu bytes)", len);
    if (memcmp(d, LIBCOV_BOUNDARY_MAGIC, 4))
        return fail(err, errlen, "not an LCVB1 boundary file (magic %.4s)", (const char *)d);
    uint32_t version = (uint32_t)le(d + 4, 4);
    if (version != LIBCOV_BOUNDARY_VERSION)
        return fail(err, errlen, "LCVB version %u is not supported (only %u)", version,
                    LIBCOV_BOUNDARY_VERSION);
    uint32_t count = (uint32_t)le(d + 8, 4);
    if ((uint64_t)count * LCVB_REC > len - LCVB_HDR)
        return fail(err, errlen, "%u records do not fit the file", count);
    out->rec = calloc(count ? count : 1, sizeof(*out->rec));
    if (!out->rec)
        return fail(err, errlen, "out of memory");
    out->count = count;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *r = d + LCVB_HDR + (size_t)i * LCVB_REC;
        out->rec[i] = (sz_marker){ .offset = le(r, 8), .kind = r[8], .depth_filter = r[9] };
        if (r[8] != LIBCOV_BOUNDARY_KIND_BEGIN && r[8] != LIBCOV_BOUNDARY_KIND_END) {
            sz_boundaries_free(out);
            return fail(err, errlen, "marker %u has unknown kind %u", i, r[8]);
        }
        if (r[9] > LIBCOV_BOUNDARY_DEPTH_ZERO) {
            sz_boundaries_free(out);
            return fail(err, errlen, "marker %u has unknown depth filter %u", i, r[9]);
        }
    }
    return true;
}

bool sz_lcvb_load(const char *path, sz_boundaries *out, char *err, size_t errlen)
{
    uint8_t *d;
    size_t len;
    memset(out, 0, sizeof(*out));
    if (!read_all(path, &d, &len, err, errlen))
        return false;
    bool ok = sz_lcvb_parse(d, len, out, err, errlen);
    free(d);
    return ok;
}

/* ---- into ember --------------------------------------------------------- */

bool sz_resolve_bases(const sz_patchlist *p, const sz_module_base *cli, size_t n_cli,
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
            return fail(err, errlen, "--cov-module %s: the patch list has no such module", cli[k].name);
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

bool sz_build_sites(const sz_patchlist *p, const uint64_t *bases, const sz_scheme *s,
                    unsigned addr_width, uam_cov_site **out, char *err, size_t errlen)
{
    const uint64_t amask = uam_mask(addr_width);
    uam_cov_site *t = malloc(sizeof(*t) * (p->count ? p->count : 1));
    if (!t)
        return fail(err, errlen, "out of memory");
    for (uint32_t i = 0; i < p->count; i++) {
        const sz_record *r = &p->rec[i];
        t[i].pc = (bases[r->module] + r->offset) & amask;
        sz_site_ab(s, i, r->offset & amask, r->module, &t[i].a, &t[i].b);
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

bool sz_namer_init(sz_namer *n, const sz_patchlist *p, const uam_cov_site *sites)
{
    memset(n, 0, sizeof(*n));
    n->p = p;
    uint64_t *pairs = malloc(sizeof(uint64_t) * 2 * (p->count ? p->count : 1));
    n->pc = malloc(sizeof(uint64_t) * (p->count ? p->count : 1));
    n->idx = malloc(sizeof(uint32_t) * (p->count ? p->count : 1));
    if (!pairs || !n->pc || !n->idx) {
        free(pairs);
        sz_namer_fini(n);
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

void sz_namer_fini(sz_namer *n)
{
    free(n->pc);
    free(n->idx);
    n->pc = NULL;
    n->idx = NULL;
}

void sz_namer_describe(void *ctx, uint64_t pc, char *buf, size_t len)
{
    const sz_namer *n = ctx;
    size_t lo = 0, hi = n->p->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (n->pc[mid] < pc)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < n->p->count && n->pc[lo] == pc) {
        const sz_record *r = &n->p->rec[n->idx[lo]];
        snprintf(buf, len, "patch-list record %u, module %u %s, offset 0x%llx", n->idx[lo],
                 r->module, n->p->module[r->module], (unsigned long long)r->offset);
    }
}

bool sz_boundaries_apply(const sz_boundaries *b, uint64_t base0, unsigned addr_width,
                         sz_bounds *out, char *err, size_t errlen)
{
    const uint64_t amask = uam_mask(addr_width);
    memset(out, 0, sizeof(*out));
    int begin = -1;
    size_t n_end = 0, n_begin = 0;
    for (uint32_t i = 0; i < b->count; i++) {
        if (b->rec[i].kind == LIBCOV_BOUNDARY_KIND_END) {
            n_end++;
            continue;
        }
        if (begin < 0)
            begin = (int)i;
        n_begin++;
    }
    if (begin < 0)
        return fail(err, errlen, "the boundary file has no BEGIN marker");
    /* Depth-zero-only markers: nested BEGIN/END arrivals (recursion) stay inside one
     * iteration. With "any depth" libcov publishes each nested segment, which a
     * snapshot-per-input harness does not; it ends the iteration at the first END. */
    const uint8_t depth = b->rec[begin].depth_filter;
    for (uint32_t i = 0; i < b->count; i++)
        if (b->rec[i].depth_filter != depth)
            return fail(err, errlen, "boundary markers %d and %u have different depth filters",
                        begin, i);
    out->begin_pc = (base0 + b->rec[begin].offset) & amask;
    out->nested = n_end && depth == LIBCOV_BOUNDARY_DEPTH_ZERO;
    if ((n_end && !(out->end_pcs = malloc(sizeof(uint64_t) * n_end)))
        || (n_begin > 1 && !(out->more_begin_pcs = malloc(sizeof(uint64_t) * (n_begin - 1))))) {
        sz_bounds_free(out);
        return fail(err, errlen, "out of memory");
    }
    for (uint32_t i = 0; i < b->count; i++) {
        uint64_t pc = (base0 + b->rec[i].offset) & amask;
        if (b->rec[i].kind == LIBCOV_BOUNDARY_KIND_END)
            out->end_pcs[out->n_end_pcs++] = pc;
        else if ((int)i != begin)
            out->more_begin_pcs[out->n_more_begin_pcs++] = pc;
    }
    return true;
}

void sz_bounds_free(sz_bounds *bd)
{
    free(bd->end_pcs);
    free(bd->more_begin_pcs);
    memset(bd, 0, sizeof(*bd));
}

size_t sz_check_sites(uam_machine *m, const sz_patchlist *p, const uam_cov_site *sites,
                      FILE *log, unsigned max_report)
{
    uam_cpu *cpu = &m->cpu;
    size_t match = 0, bad = 0;
    for (uint32_t i = 0; i < p->count; i++) {
        const sz_record *r = &p->rec[i];
        uint8_t got[LIBCOV_PATCH_ORIG_MAX] = { 0 };
        bool ok = true;
        for (unsigned k = 0; k < r->len && ok; k++) {
            uint64_t va = sites[i].pc + k, pa = va, v = 0;
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
                fprintf(log, " in the patch list\n");
            }
        }
    }
    if (log)
        fprintf(log, "cov: site check: %zu of %u sites match the patch list, %zu do not\n", match,
                p->count, bad);
    return bad;
}
