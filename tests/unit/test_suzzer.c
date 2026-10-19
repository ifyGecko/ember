/*
 * test_suzzer.c - ember's suzzer format support (fuzz/suzzer/).
 *
 *   - conformance: every vector in suzzer's test/vectors (the directory is the first
 *     argument; `make test` passes $(SUZZER_DIR)/test/vectors) replayed through UAM_OP_COV on the interpreter, the JIT and the JIT
 *     without chaining reproduces expected.bin byte for byte. Each emit becomes one
 *     guest instruction whose site carries the COV operands the importer computes;
 *     history resets become PC hooks;
 *   - the map-size rule against map_size.jsonl;
 *   - the LCV3 and LCVB1 readers (every rejection of FORMATS.md's checklist, the S2
 *     base table, unknown flag bits), base resolution, boundaries, site naming and the
 *     site-byte check.
 */
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "lcv_formats.h"
#include "suzzer.h"
#include "uam/isa/riscv.h"

static int n_checks, n_failed;

#define CHECK(cond, ...) do {                                              \
        n_checks++;                                                        \
        if (!(cond)) {                                                     \
            n_failed++;                                                    \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);           \
            fprintf(stderr, __VA_ARGS__);                                  \
            fputc('\n', stderr);                                           \
        }                                                                  \
    } while (0)

#define EQ64(a, b, what) do {                                              \
        uint64_t a_ = (uint64_t)(a), b_ = (uint64_t)(b);                   \
        CHECK(a_ == b_, "%s: got 0x%llx, expected 0x%llx", what,           \
              (unsigned long long)a_, (unsigned long long)b_);             \
    } while (0)

static const char *vec_dir;   /* suzzer's test/vectors (argv[1]) */

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(n > 0 ? (size_t)n : 1);
    if (d && fread(d, 1, (size_t)n, f) != (size_t)n) {
        free(d);
        d = NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return d;
}

/* ---- conformance vectors ------------------------------------------------- */

typedef struct emit {
    uint32_t a, b;
    bool     reset_before;
} emit;

static uam_hook_action stop_hook(void *ctx, uam_cpu *cpu, uint64_t pc)
{
    (void)ctx; (void)cpu; (void)pc;
    return UAM_HOOK_STOP;
}

static uam_hook_action reset_hook(void *ctx, uam_cpu *cpu, uint64_t pc)
{
    (void)ctx; (void)pc;
    uam_cov_reset_prev(cpu->cov);
    return UAM_HOOK_CONTINUE;
}

#define CODE_BASE 0x1000u

/* Run the emits as straight-line guest code; returns the map (caller frees). */
static uint8_t *run_emits(const emit *e, size_t n, uint32_t map_size, const uam_cov_history *h,
                          int backend)
{
    uam_machine m;
    CHECK(uam_machine_init(&m, &uam_isa_rv32) == UAM_OK, "machine");
    CHECK(uam_bus_map_ram(&m.bus, "ram", 0, 1 << 20) == UAM_OK, "ram");
    uam_cov_site *sites = malloc(sizeof(*sites) * (n ? n : 1));
    for (size_t k = 0; k <= n; k++) {
        uint32_t w = k < n ? 0x00000013u /* addi x0, x0, 0 */ : 0x0000006Fu /* jal x0, 0 */;
        uint8_t b[4] = { (uint8_t)w, (uint8_t)(w >> 8), (uint8_t)(w >> 16), (uint8_t)(w >> 24) };
        uam_bus_load_bytes(&m.bus, CODE_BASE + 4 * k, b, 4);
        if (k < n) {
            sites[k] = (uam_cov_site){ .pc = CODE_BASE + 4 * k, .a = e[k].a, .b = e[k].b };
            if (e[k].reset_before)
                uam_exec_add_pc_hook(&m.exec, CODE_BASE + 4 * k, reset_hook, NULL);
        }
    }
    uam_exec_add_pc_hook(&m.exec, CODE_BASE + 4 * n, stop_hook, NULL);
    uam_machine_reset(&m, CODE_BASE);
    if (backend)
        CHECK(uam_exec_enable_jit(&m.exec), "jit");
    m.exec.chain = backend == 1;
    uint8_t *map = calloc(1, map_size);
    uam_cov_state cs;
    uam_cov_state_init(&cs, map, map_size, h);
    char err[128];
    CHECK(uam_exec_set_coverage(&m.exec, sites, n, &cs, err, sizeof(err)) == UAM_OK, "sites: %s", err);
    uam_stop_reason why = uam_exec_run(&m.exec, 10 * n + 100);
    CHECK(why == UAM_STOP_HOOK && m.cpu.pc == CODE_BASE + 4 * n, "ran to the end (%s)",
          uam_stop_reason_str(why));
    free(sites);
    uam_machine_fini(&m);
    return map;
}

/* "0x400000" strings in the scheme.json "module_bases" array. */
static unsigned parse_bases(const char *json, uint64_t *bases, unsigned max)
{
    const char *p = strstr(json, "\"module_bases\"");
    unsigned n = 0;
    if (!p)
        return 0;
    const char *end = strchr(p, ']');
    while (n < max && (p = strstr(p, "\"0x")) && p < end) {
        bases[n++] = strtoull(p + 1, NULL, 16);
        p += 3;
    }
    return n;
}

static void check_vector(const char *name)
{
    char model[8];
    unsigned ngram, rot, map_size;
    if (sscanf(name, "%7[a-z]_n%u_r%u_%u", model, &ngram, &rot, &map_size) != 4)
        return;
    char path[512];
    size_t seq_len, exp_len, json_len;
    snprintf(path, sizeof(path), "%s/%s/sequence.bin", vec_dir, name);
    uint8_t *seq = slurp(path, &seq_len);
    snprintf(path, sizeof(path), "%s/%s/expected.bin", vec_dir, name);
    uint8_t *expected = slurp(path, &exp_len);
    snprintf(path, sizeof(path), "%s/%s/scheme.json", vec_dir, name);
    uint8_t *json = slurp(path, &json_len);
    CHECK(seq && expected && json && exp_len == map_size, "%s: vector files", name);
    if (!seq || !expected || !json) {
        free(seq); free(expected); free(json);
        return;
    }
    char *js = realloc(json, json_len + 1);
    js[json_len] = '\0';

    sz_scheme s = { .model = strcmp(model, "pc") ? SZ_MODEL_SITE : SZ_MODEL_PC, .ngram = ngram,
                    .rotate = rot != 0, .map_size = map_size };
    uam_cov_history h;
    sz_cov_history(&s, &h);
    if (rot && ngram >= 2) {
        /* scheme.json records libcov's own rotation amounts: ours must be the same. */
        char want_rot[64] = "\"rot_amounts\": [";
        for (unsigned j = 0; j < h.depth; j++)
            snprintf(want_rot + strlen(want_rot), sizeof(want_rot) - strlen(want_rot), "%s%u",
                     j ? ", " : "", h.rot[j]);
        strncat(want_rot, "]", sizeof(want_rot) - strlen(want_rot) - 1);
        CHECK(strstr(js, want_rot) != NULL, "%s: rotation amounts %s", name, want_rot);
    }
    uint64_t bases[8] = { 0 };
    unsigned n_bases = s.model == SZ_MODEL_PC ? parse_bases(js, bases, 8) : 0;
    CHECK(s.model == SZ_MODEL_SITE || n_bases > 0, "%s: module bases", name);
    char want_flags[16];
    snprintf(want_flags, sizeof(want_flags), "\"0x%x\"", sz_lcvs_scheme_flags(&s));
    CHECK(strstr(js, want_flags) != NULL, "%s: LCVS scheme flags %s", name, want_flags);

    const size_t rec = s.model == SZ_MODEL_SITE ? 4 : 9;
    emit *e = calloc(seq_len / rec + 1, sizeof(*e));
    size_t n = 0;
    bool reset = false;
    for (size_t off = 0; off + rec <= seq_len; off += rec) {
        const uint8_t *r = seq + off;
        bool all_ones = true;
        for (size_t k = 0; k < rec; k++)
            all_ones &= r[k] == 0xff;
        if (all_ones) {
            reset = true;
            continue;
        }
        uint32_t id = 0;
        uint64_t offset = 0;
        uint8_t mod = 0;
        if (s.model == SZ_MODEL_SITE) {
            id = (uint32_t)r[0] | (uint32_t)r[1] << 8 | (uint32_t)r[2] << 16 | (uint32_t)r[3] << 24;
        } else {
            uint64_t pc = 0;
            for (int k = 0; k < 8; k++)
                pc |= (uint64_t)r[k] << (8 * k);
            mod = r[8];
            offset = pc - (mod < n_bases ? bases[mod] : 0);   /* the target is 64-bit */
        }
        sz_site_ab(&s, id, offset, mod, &e[n].a, &e[n].b);
        e[n].reset_before = reset;
        reset = false;
        n++;
    }
    static const char *const bk[3] = { "interp", "jit", "jit-nochain" };
    for (int b = 0; b < 3; b++) {
        uint8_t *map = run_emits(e, n, map_size, &h, b);
        size_t diff = 0;
        for (size_t i = 0; i < map_size; i++)
            diff += map[i] != expected[i];
        CHECK(diff == 0, "%s/%s: %zu buckets differ from expected.bin (%zu emits)", name, bk[b], diff, n);
        free(map);
    }
    printf("test_suzzer: vector %s: %zu emits reproduced\n", name, n);
    free(e);
    free(seq);
    free(expected);
    free(js);
}

static int name_cmp(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void test_vectors(void)
{
    DIR *d = opendir(vec_dir);
    CHECK(d != NULL, "suzzer's vectors at %s", vec_dir);
    if (!d)
        return;
    char *names[128];
    unsigned n = 0;
    struct dirent *de;
    while ((de = readdir(d)) && n < 128)
        if (de->d_name[0] != '.' && strchr(de->d_name, '_') && !strchr(de->d_name, '.'))
            names[n++] = strdup(de->d_name);
    closedir(d);
    qsort(names, n, sizeof(*names), name_cmp);
    CHECK(n >= 60, "%u vectors found (expected all 60: site/pc, N 1,2,3,4,8, rotation "
          "off/on, three map sizes)", n);
    for (unsigned i = 0; i < n; i++) {
        check_vector(names[i]);
        free(names[i]);
    }
}

static void test_map_size(void)
{
    char path[512];
    size_t len;
    snprintf(path, sizeof(path), "%s/map_size.jsonl", vec_dir);
    uint8_t *d = slurp(path, &len);
    CHECK(d != NULL, "map_size.jsonl");
    if (!d)
        return;
    char *s = realloc(d, len + 1);
    s[len] = '\0';
    unsigned checked = 0;
    for (char *line = strtok(s, "\n"); line; line = strtok(NULL, "\n")) {
        unsigned ngram;
        if (sscanf(line, "{\"n\": %u", &ngram) != 1 || ngram > 2)
            continue;
        char *p = strstr(line, "\"sizes\"");
        while (p && (p = strstr(p, "[")) != NULL) {
            unsigned long long sites, size;
            if (sscanf(p, "[%llu, %llu]", &sites, &size) == 2) {
                EQ64(sz_map_size_for(sites, ngram), size, "map size rule");
                checked++;
            }
            p++;
        }
    }
    CHECK(checked >= 20, "map size cases checked: %u", checked);
    free(s);
}

/* ---- LCV3 ----------------------------------------------------------------- */

typedef struct buf { uint8_t d[1024]; size_t n; } buf;

static void put(buf *b, uint64_t v, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        b->d[b->n++] = (uint8_t)(v >> (8 * i));
}

/* Two modules ("fw", "lib"), three records, optionally the S2 base table. */
static buf lcv3(bool bases, uint8_t flags_extra)
{
    buf b = { .n = 0 };
    memcpy(b.d, "LCV3", 4);
    b.n = 4;
    put(&b, 3, 4);          /* version */
    put(&b, 3, 4);          /* count */
    b.d[b.n++] = 0;         /* threading */
    b.d[b.n++] = (uint8_t)((bases ? LCV_FLAG_MODULE_BASES : 0) | flags_extra);
    b.d[b.n++] = 2;         /* modules */
    b.d[b.n++] = 0;
    put(&b, 24 + 3 * 14, 4); /* modules_offset */
    put(&b, 7, 4);           /* "fw\0lib\0" */
    static const struct { uint64_t off; uint8_t len, mod; uint32_t orig; } r[3] = {
        { 0x10, 4, 0, 0x00000013 }, { 0x20, 4, 0, 0x00a00593 }, { 0x30, 4, 1, 0x00008067 } };
    for (int i = 0; i < 3; i++) {
        put(&b, r[i].off, 8);
        b.d[b.n++] = r[i].len;
        b.d[b.n++] = r[i].mod;
        put(&b, r[i].orig, 4);
    }
    memcpy(b.d + b.n, "fw\0lib\0", 7);
    b.n += 7;
    if (bases) {
        while (b.n & 7)
            b.d[b.n++] = 0;
        put(&b, 0x1000, 8);
        put(&b, 0x8000, 8);
    }
    return b;
}

static bool parses(const buf *b, sz_patchlist *p, char *err)
{
    return sz_lcv3_parse(b->d, b->n, p, err, 256);
}

static void test_lcv3(void)
{
    char err[256];
    sz_patchlist p;
    buf b = lcv3(false, 0);
    CHECK(parses(&b, &p, err), "valid list: %s", err);
    CHECK(p.count == 3 && p.n_modules == 2 && !strcmp(p.module[0], "fw") && !strcmp(p.module[1], "lib")
          && p.rec[2].offset == 0x30 && p.rec[2].module == 1 && p.rec[1].orig[0] == 0x93 && !p.has_bases,
          "fields decoded");
    sz_patchlist_free(&p);

    b = lcv3(true, 0x80);   /* with bases, and an unknown flag bit (ignored) */
    CHECK(parses(&b, &p, err) && p.has_bases && p.bases[0] == 0x1000 && p.bases[1] == 0x8000,
          "S2 base table: %s", err);
    sz_patchlist_free(&p);
    b = lcv3(true, 0);
    b.n -= 4;               /* the table runs past the end: ignored */
    CHECK(parses(&b, &p, err) && !p.has_bases, "truncated base table ignored");
    sz_patchlist_free(&p);

    /* The reader checklist. */
    struct { const char *what; size_t at; uint8_t val; size_t trunc; } bad[] = {
        { "magic", 0, 'X', 0 },
        { "version", 4, 2, 0 },
        { "count too large", 8, 200, 0 },
        { "no modules", 14, 0, 0 },
        { "empty name table", 20, 0, 0 },
        { "name table past the end", 16, 250, 0 },
        { "name table not NUL-terminated", 24 + 42 + 6, 'x', 0 },
        { "module index", 24 + 9, 5, 0 },
        { "short file", 0, 'L', 10 },
    };
    for (size_t i = 0; i < UAM_ARRAY_LEN(bad); i++) {
        b = lcv3(false, 0);
        b.d[bad[i].at] = bad[i].val;
        if (bad[i].trunc)
            b.n = bad[i].trunc;
        CHECK(!parses(&b, &p, err), "rejected: %s", bad[i].what);
    }
    b = lcv3(false, 0);
    b.d[4] = 4;
    CHECK(!parses(&b, &p, err) && strstr(err, "version 4"), "the error names the version: %s", err);
    /* An empty name: "fw\0\0lib" would be two names, the second empty. */
    b = lcv3(false, 0);
    memcpy(b.d + 24 + 42, "\0w\0lib\0", 7);
    CHECK(!parses(&b, &p, err) && strstr(err, "empty"), "empty module name: %s", err);

    /* Bases: CLI, then S2, then the image base for module 0. */
    b = lcv3(false, 0);
    parses(&b, &p, err);
    uint64_t bases[2];
    CHECK(!sz_resolve_bases(&p, NULL, 0, true, 0x2000, bases, err, sizeof(err)) && strstr(err, "lib"),
          "module 1 has no base: %s", err);
    sz_module_base cli = { "lib", 0x9000 };
    CHECK(sz_resolve_bases(&p, &cli, 1, true, 0x2000, bases, err, sizeof(err))
          && bases[0] == 0x2000 && bases[1] == 0x9000, "image base + --cov-module");
    sz_module_base nope = { "nope", 1 };
    CHECK(!sz_resolve_bases(&p, &nope, 1, true, 0, bases, err, sizeof(err)), "unknown --cov-module");

    sz_scheme s = { .model = SZ_MODEL_SITE, .ngram = 2, .map_size = 4096 };
    uam_cov_site *sites;
    CHECK(sz_build_sites(&p, bases, &s, 32, &sites, err, sizeof(err)), "build sites");
    EQ64(sites[0].pc, 0x2010, "site 0 address");
    EQ64(sites[2].pc, 0x9030, "site 2 address (module 1)");
    CHECK(sites[1].a == 1 && sites[1].b == sz_spread(1, 4095), "site-id operands");
    sz_namer nm;
    CHECK(sz_namer_init(&nm, &p, sites), "namer");
    char desc[128] = "";
    sz_namer_describe(&nm, 0x9030, desc, sizeof(desc));
    CHECK(strstr(desc, "record 2") && strstr(desc, "lib"), "namer: %s", desc);
    sz_namer_fini(&nm);

    /* The site-byte check: write two of the three sites' bytes into memory. */
    uam_machine m;
    CHECK(uam_machine_init(&m, &uam_isa_rv32) == UAM_OK, "machine");
    uam_bus_map_ram(&m.bus, "ram", 0, 1 << 16);
    uam_bus_load_bytes(&m.bus, 0x2010, p.rec[0].orig, 4);
    uam_bus_load_bytes(&m.bus, 0x2020, p.rec[1].orig, 4);
    char out[1024] = "";
    FILE *f = fmemopen(out, sizeof(out), "w");
    size_t bad_sites = sz_check_sites(&m, &p, sites, f, 4);
    fclose(f);
    EQ64(bad_sites, 1, "one mismatching site");
    CHECK(strstr(out, "2 of 3 sites match") && strstr(out, "site 2"), "check report:\n%s", out);
    uam_machine_fini(&m);
    free(sites);

    s.model = SZ_MODEL_PC;
    s.ngram = 2;
    CHECK(sz_build_sites(&p, bases, &s, 32, &sites, err, sizeof(err)), "pc model sites");
    CHECK(sites[2].a == sz_hash_pc(0x30, 1, 4095) && sites[2].b == sites[2].a >> 1,
          "hashed-PC operands: A = hash(offset, module), B = A >> 1");
    free(sites);
    sz_patchlist_free(&p);
}

/* ---- LCVB1 ---------------------------------------------------------------- */

static buf lcvb(unsigned n, const uint8_t *kinds, uint8_t depth)
{
    buf b = { .n = 0 };
    memcpy(b.d, "LCVB", 4);
    b.n = 4;
    put(&b, 1, 4);
    put(&b, n, 4);
    put(&b, 0, 4);
    for (unsigned i = 0; i < n; i++) {
        put(&b, 0x100 + 0x10 * i, 8);
        b.d[b.n++] = kinds[i];
        b.d[b.n++] = depth;
        put(&b, 0, 6);
    }
    return b;
}

static void test_lcvb(void)
{
    char err[256];
    sz_boundaries bd;
    sz_bounds bo;
    const uint8_t be[2] = { 0, 1 }, bb[2] = { 0, 0 }, b1[1] = { 0 }, beee[4] = { 1, 0, 1, 1 };

    buf b = lcvb(2, be, 1);
    CHECK(sz_lcvb_parse(b.d, b.n, &bd, err, sizeof(err)), "paired: %s", err);
    CHECK(sz_boundaries_apply(&bd, 0x80000000u, 32, &bo, err, sizeof(err)), "apply: %s", err);
    CHECK(bo.n_end_pcs == 1 && bo.nested && bo.begin_pc == 0x80000100u && bo.end_pcs[0] == 0x80000110u,
          "paired, depth-zero only: nested");
    sz_bounds_free(&bo);
    sz_boundaries_free(&bd);

    b = lcvb(2, be, 0);
    sz_lcvb_parse(b.d, b.n, &bd, err, sizeof(err));
    CHECK(sz_boundaries_apply(&bd, 0, 32, &bo, err, sizeof(err)) && bo.n_end_pcs == 1 && !bo.nested,
          "paired, any depth");
    sz_bounds_free(&bo);
    sz_boundaries_free(&bd);

    /* Several END markers, the BEGIN among them: every END is kept. */
    b = lcvb(4, beee, 1);
    sz_lcvb_parse(b.d, b.n, &bd, err, sizeof(err));
    CHECK(sz_boundaries_apply(&bd, 0x1000, 32, &bo, err, sizeof(err)) && bo.begin_pc == 0x1110
          && bo.n_end_pcs == 3 && bo.end_pcs[0] == 0x1100 && bo.end_pcs[1] == 0x1120
          && bo.end_pcs[2] == 0x1130 && bo.nested, "three END markers: %s", err);
    sz_bounds_free(&bo);
    sz_boundaries_free(&bd);

    /* Mixed depth filters have no single meaning. */
    b = lcvb(4, beee, 1);
    b.d[16 + 2 * 16 + 9] = 0;   /* record 2: any depth */
    sz_lcvb_parse(b.d, b.n, &bd, err, sizeof(err));
    CHECK(!sz_boundaries_apply(&bd, 0, 32, &bo, err, sizeof(err)) && strstr(err, "depth filters"),
          "mixed depth filters refused: %s", err);
    sz_boundaries_free(&bd);

    b = lcvb(1, b1, 0);
    sz_lcvb_parse(b.d, b.n, &bd, err, sizeof(err));
    CHECK(sz_boundaries_apply(&bd, 0x1000, 32, &bo, err, sizeof(err)) && bo.n_end_pcs == 0
          && !bo.nested && bo.begin_pc == 0x1100, "begin-only");
    sz_bounds_free(&bo);
    sz_boundaries_free(&bd);

    b = lcvb(2, bb, 0);
    sz_lcvb_parse(b.d, b.n, &bd, err, sizeof(err));
    CHECK(!sz_boundaries_apply(&bd, 0, 32, &bo, err, sizeof(err)) && strstr(err, "several BEGIN"),
          "two BEGINs refused: %s", err);
    sz_boundaries_free(&bd);

    b = lcvb(1, b1, 0);
    b.d[16 + 8] = 7;
    CHECK(!sz_lcvb_parse(b.d, b.n, &bd, err, sizeof(err)), "unknown kind");
    b = lcvb(1, b1, 0);
    b.d[4] = 2;
    CHECK(!sz_lcvb_parse(b.d, b.n, &bd, err, sizeof(err)) && strstr(err, "version 2"), "version: %s", err);
    b = lcvb(1, b1, 0);
    b.n -= 1;
    CHECK(!sz_lcvb_parse(b.d, b.n, &bd, err, sizeof(err)), "truncated");
}

/* ---- LCVS region ownership (as libcov decides it) ---------------------- */

/* Field 22 of /proc/PID/stat, low 32 bits; 0 if unreadable. */
static uint32_t proc_start(pid_t pid)
{
    char path[64], line[1024];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    FILE *fp = fopen(path, "r");
    if (!fp)
        return 0;
    size_t n = fread(line, 1, sizeof(line) - 1, fp);
    fclose(fp);
    line[n] = '\0';
    char *p = strrchr(line, ')');   /* the command name may hold spaces */
    if (p)
        p++;                         /* the space before field 3 */
    for (int field = 3; field < 22 && p; field++)
        p = strchr(p + 1, ' ');
    return p ? (uint32_t)strtoull(p + 1, NULL, 10) : 0;
}

static void set_owner(const char *path, uint32_t pid, uint32_t start)
{
    FILE *fp = fopen(path, "r+b");
    CHECK(fp != NULL, "open %s", path);
    if (!fp)
        return;
    fseek(fp, (long)offsetof(struct libcov_shm_header, owner_pid), SEEK_SET);
    fwrite(&pid, 4, 1, fp);
    fseek(fp, (long)offsetof(struct libcov_shm_header, owner_start), SEEK_SET);
    fwrite(&start, 4, 1, fp);
    fclose(fp);
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void test_lcvs_owner(void)
{
    char path[] = "/tmp/uam-lcvs-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0, "mkstemp");
    if (fd < 0)
        return;
    close(fd);
    char err[256] = "";
    sz_lcvs *l = sz_lcvs_create(path, 4096, 2, 0, err, sizeof(err));
    CHECK(l != NULL, "create: %s", err);
    sz_lcvs_close(l);
    CHECK((l = sz_lcvs_create(path, 4096, 2, 0, err, sizeof(err))) != NULL,
          "our own earlier region is ours: %s", err);
    sz_lcvs_close(l);

    /* A live owner: a child process, recorded with its start time. */
    pid_t kid = fork();
    if (kid == 0) {
        pause();
        _exit(0);
    }
    uint32_t start = 0;
    for (int i = 0; i < 1000 && !(start = proc_start(kid)); i++)
        usleep(1000);
    set_owner(path, (uint32_t)kid, start);
    err[0] = '\0';
    l = sz_lcvs_create(path, 4096, 2, 0, err, sizeof(err));
    CHECK(!l && strstr(err, "in use"), "a live owner's region is refused (%s)", err);
    sz_lcvs_close(l);
    /* The same pid with another start time is a reused pid: the owner is gone. */
    set_owner(path, (uint32_t)kid, start + 1);
    l = sz_lcvs_create(path, 4096, 2, 0, err, sizeof(err));
    CHECK(l != NULL, "a reused pid does not own the region: %s", err);
    sz_lcvs_close(l);
    kill(kid, SIGKILL);
    waitpid(kid, NULL, 0);

    /* The decision is made under the exclusive flock libcov takes for it. */
    int pfd[2];
    CHECK(pipe(pfd) == 0, "pipe");
    kid = fork();
    if (kid == 0) {
        int lfd = open(path, O_RDWR);
        if (lfd >= 0 && flock(lfd, LOCK_EX) == 0 && write(pfd[1], "L", 1) == 1)
            usleep(300000);
        _exit(0);   /* releases the lock */
    }
    char c = 0;
    CHECK(read(pfd[0], &c, 1) == 1 && c == 'L', "the child holds the lock");
    double t0 = now_s();
    l = sz_lcvs_create(path, 4096, 2, 0, err, sizeof(err));
    double waited = now_s() - t0;
    CHECK(l != NULL, "create after the lock is released: %s", err);
    CHECK(waited >= 0.2, "create waited for the lock (%.3f s)", waited);
    sz_lcvs_close(l);
    waitpid(kid, NULL, 0);
    close(pfd[0]);
    close(pfd[1]);
    unlink(path);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s SUZZER_DIR/test/vectors\n", argv[0]);
        return 2;
    }
    vec_dir = argv[1];
    test_map_size();
    test_lcv3();
    test_lcvb();
    test_vectors();
    test_lcvs_owner();
    printf("test_suzzer: %d/%d checks passed\n", n_checks - n_failed, n_checks);
    return n_failed ? 1 : 0;
}
