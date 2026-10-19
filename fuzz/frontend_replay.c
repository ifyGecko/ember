/*
 * frontend_replay.c - Run saved inputs once each, without a fuzzer (uam/fuzz.h).
 *
 * For crash triage, regression tests and CI: a crash file saved by any fuzzer
 * reproduces the same verdict and detail, since the harness is deterministic.
 */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "uam/fuzz.h"

struct uam_fuzz_replay {
    uam_fuzz *f;
    FILE     *out;
    char    **paths;
    size_t    n_paths, next;
    uint8_t  *data;
    int       exit_status;
    int       rank;          /* of the verdict behind exit_status (verdict_rank) */
    uint8_t  *last_map;
    size_t    map_size;
};

static int path_cmp(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static bool add_path(uam_fuzz_replay *r, const char *p)
{
    char **np = realloc(r->paths, sizeof(*np) * (r->n_paths + 1));
    if (!np)
        return false;
    r->paths = np;
    if (!(r->paths[r->n_paths] = strdup(p)))
        return false;
    r->n_paths++;
    return true;
}

/* X<suffix> for one of the suffixes, with X among the (sorted) paths too. */
static bool is_sidecar(const uam_fuzz_replay *r, const char *p, const char *const *sidecars,
                       size_t n_sidecars)
{
    size_t len = strlen(p);
    for (size_t i = 0; i < n_sidecars; i++) {
        size_t sl = strlen(sidecars[i]);
        if (!sl || len <= sl || strcmp(p + len - sl, sidecars[i]))
            continue;
        char *base = strndup(p, len - sl);
        bool found = base && bsearch(&base, r->paths, r->n_paths, sizeof(*r->paths), path_cmp);
        free(base);
        if (found)
            return true;
    }
    return false;
}

/* Drops the sidecars from r->paths (sorted), keeping the order. */
static void drop_sidecars(uam_fuzz_replay *r, const char *dir, const char *const *sidecars,
                          size_t n_sidecars)
{
    bool *drop = n_sidecars && r->n_paths ? calloc(r->n_paths, sizeof(*drop)) : NULL;
    if (!drop)
        return;
    size_t n = 0;
    for (size_t i = 0; i < r->n_paths; i++)
        n += drop[i] = is_sidecar(r, r->paths[i], sidecars, n_sidecars);
    size_t k = 0;
    for (size_t i = 0; i < r->n_paths; i++) {
        if (drop[i])
            free(r->paths[i]);
        else
            r->paths[k++] = r->paths[i];
    }
    r->n_paths = k;
    free(drop);
    if (n)
        uam_log(UAM_LOG_INFO, "replay", "%s: skipped %zu sidecar file%s (notes beside an input)",
                dir, n, n == 1 ? "" : "s");
}

uam_fuzz_replay *uam_fuzz_replay_create(const char *path, uam_fuzz *f, FILE *out)
{
    return uam_fuzz_replay_create_ex(path, f, out, NULL, 0);
}

uam_fuzz_replay *uam_fuzz_replay_create_ex(const char *path, uam_fuzz *f, FILE *out,
                                           const char *const *sidecars, size_t n_sidecars)
{
    uam_fuzz_replay *r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    r->f = f;
    r->out = out;
    struct stat sb;
    if (stat(path, &sb) != 0) {
        uam_log(UAM_LOG_ERROR, "replay", "cannot read %s: %s", path, strerror(errno));
        uam_fuzz_replay_destroy(r);
        return NULL;
    }
    if (S_ISDIR(sb.st_mode)) {
        DIR *d = opendir(path);
        if (!d) {
            uam_log(UAM_LOG_ERROR, "replay", "cannot open %s: %s", path, strerror(errno));
            uam_fuzz_replay_destroy(r);
            return NULL;
        }
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.')
                continue;
            size_t n = strlen(path) + strlen(e->d_name) + 2;
            char *full = malloc(n);
            if (!full)
                break;
            snprintf(full, n, "%s/%s", path, e->d_name);
            struct stat fs;
            bool ok = stat(full, &fs) != 0 || !S_ISREG(fs.st_mode) || add_path(r, full);
            free(full);
            if (!ok)
                break;
        }
        closedir(d);
        qsort(r->paths, r->n_paths, sizeof(*r->paths), path_cmp);
        drop_sidecars(r, path, sidecars, n_sidecars);
    } else if (!add_path(r, path)) {
        uam_fuzz_replay_destroy(r);
        return NULL;
    }
    return r;
}

void uam_fuzz_replay_destroy(uam_fuzz_replay *r)
{
    if (!r)
        return;
    for (size_t i = 0; i < r->n_paths; i++)
        free(r->paths[i]);
    free(r->paths);
    free(r->data);
    free(r->last_map);
    free(r);
}

int uam_fuzz_replay_exit_status(const uam_fuzz_replay *r) { return r->exit_status; }
size_t uam_fuzz_replay_count(const uam_fuzz_replay *r) { return r->n_paths; }

const uint8_t *uam_fuzz_replay_last_map(const uam_fuzz_replay *r, size_t *size)
{
    *size = r->map_size;
    return r->last_map;
}

static const char *exc_name(uam_exc_kind k)
{
    switch (k) {
    case UAM_EXC_TRAP:             return "trap";
    case UAM_EXC_FETCH_FAULT:      return "fetch-fault";
    case UAM_EXC_LOAD_FAULT:       return "load-fault";
    case UAM_EXC_STORE_FAULT:      return "store-fault";
    case UAM_EXC_LOAD_MISALIGNED:  return "load-misaligned";
    case UAM_EXC_STORE_MISALIGNED: return "store-misaligned";
    case UAM_EXC_SYSREG:           return "sysreg";
    case UAM_EXC_HELPER:           return "helper";
    case UAM_EXC_FETCH_XLATE:      return "fetch-xlate";
    case UAM_EXC_LOAD_XLATE:       return "load-xlate";
    case UAM_EXC_STORE_XLATE:      return "store-xlate";
    }
    return "?";
}

/* The exit status is the most severe verdict of any input: a crash (the first one
 * seen), else a hang, else a timeout, else ok. So replaying a directory fails when any
 * input in it does, whatever else (or whatever order) the directory holds. */
static int verdict_rank(uam_fuzz_verdict v)
{
    switch (v) {
    case UAM_FUZZ_CRASH_FAULT:
    case UAM_FUZZ_CRASH_ORACLE: return 3;
    case UAM_FUZZ_HANG:         return 2;
    case UAM_FUZZ_TIMEOUT:      return 1;
    case UAM_FUZZ_OK:           return 0;
    }
    return 0;
}

static int replay_next(void *ctx, uint8_t **data, size_t *len, uint64_t *id)
{
    uam_fuzz_replay *r = ctx;
    if (r->next >= r->n_paths)
        return 0;
    const char *p = r->paths[r->next];
    FILE *fp = fopen(p, "rb");
    if (!fp) {
        uam_log(UAM_LOG_ERROR, "replay", "cannot open %s: %s", p, strerror(errno));
        return -1;
    }
    free(r->data);
    r->data = NULL;
    size_t cap = 0, n = 0;
    for (;;) {
        if (n == cap) {
            size_t ncap = cap ? cap * 2 : 4096;
            uint8_t *nd = realloc(r->data, ncap);
            if (!nd) {
                fclose(fp);
                return -1;
            }
            r->data = nd;
            cap = ncap;
        }
        size_t got = fread(r->data + n, 1, cap - n, fp);
        n += got;
        if (got == 0)
            break;
    }
    bool err = ferror(fp);
    fclose(fp);
    if (err) {
        uam_log(UAM_LOG_ERROR, "replay", "reading %s failed", p);
        return -1;
    }
    *data = r->data;
    *len = n;
    *id = ++r->next;   /* 1-based: id 0 is the INIT report */
    return 1;
}

static uam_status replay_report(void *ctx, uint64_t id, uam_fuzz_verdict v, uint64_t detail,
                                const uint8_t *map, size_t map_size, int kind)
{
    uam_fuzz_replay *r = ctx;
    if (!r->last_map || r->map_size != map_size) {
        free(r->last_map);
        r->map_size = 0;
        if (!(r->last_map = malloc(map_size)))
            return UAM_ERR_NOMEM;
        r->map_size = map_size;
    }
    memcpy(r->last_map, map, map_size);
    if (kind != UAM_FUZZ_KIND_ITER)
        return UAM_OK;

    const uam_fuzz_result *res = uam_fuzz_last_result(r->f);
    size_t hit = 0;
    for (size_t i = 0; i < map_size; i++)
        hit += map[i] != 0;
    fprintf(r->out, "replay: %s: %s", r->paths[id - 1], uam_fuzz_verdict_str(v));
    switch (v) {
    case UAM_FUZZ_CRASH_FAULT:
        fprintf(r->out, " pc=0x%llx", (unsigned long long)detail);
        if (res->exc_valid)
            fprintf(r->out, " (%s cause 0x%llx tval 0x%llx)", exc_name(res->exc.kind),
                    (unsigned long long)res->exc.cause, (unsigned long long)res->exc.tval);
        else
            fprintf(r->out, " (%s)", uam_stop_reason_str(res->stop));
        break;
    case UAM_FUZZ_CRASH_ORACLE:
        if (res->reset)
            fprintf(r->out, " (reset)");
        else if (res->irq_valid)
            fprintf(r->out, " interrupt lines=0x%llx (at pc 0x%llx, handler 0x%llx)",
                    (unsigned long long)detail, (unsigned long long)res->irq.from_pc,
                    (unsigned long long)res->irq.handler_pc);
        else if (res->stop == UAM_STOP_REQUEST || res->stop == UAM_STOP_HALT)
            fprintf(r->out, " status=%lld", (long long)detail);
        else
            fprintf(r->out, " pc=0x%llx", (unsigned long long)detail);
        break;
    case UAM_FUZZ_HANG:
        fprintf(r->out, " (%s)", uam_stop_reason_str((uam_stop_reason)detail));
        break;
    case UAM_FUZZ_OK:
    case UAM_FUZZ_TIMEOUT:
        break;
    }
    fprintf(r->out, " insns=%llu buckets=%zu\n", (unsigned long long)res->insns, hit);
    if (verdict_rank(v) > r->rank) {
        r->rank = verdict_rank(v);
        r->exit_status = uam_fuzz_exit_status(v);
    }
    return UAM_OK;
}

const uam_fuzz_frontend uam_fuzz_replay_frontend = {
    .next_input = replay_next,
    .report     = replay_report,
};
