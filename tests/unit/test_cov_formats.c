/*
 * test_cov_formats.c - Site lists, boundary files and the coverage region (fuzz/), and
 * the coverage formula.
 *
 *   - golden maps: fixed pseudo-random emit sequences (site ids or pcs, with history
 *     resets, one long run on a single site that saturates its bucket) through every
 *     scheme - site/pc, N = 1, 2, 3, 4, 8, rotation off and on, 4 KiB, 64 KiB and 1 MiB
 *     maps - as UAM_OP_COV on the interpreter, the JIT and the JIT without
 *     chaining: the three must agree byte for byte, and each map's FNV-1a hash must be the
 *     one in the golden file (argv[1], tests/unit/cov_golden.txt), so the formula cannot
 *     change unnoticed. `--write-golden FILE` writes that file anew (only when a formula
 *     change is intended: it invalidates every saved corpus's coverage);
 *   - the formula's parts against values worked out by hand: the mixer, the pc hash, the
 *     rotation amounts and the scheme flags;
 *   - the default map-size rule against a table;
 *   - the site-list and boundary-file readers (every rejection of docs/fuzz-formats.md's
 *     checklists, the base table, the alternate-state flag), base resolution, boundaries,
 *     site naming and the site-byte check;
 *   - the coverage region's ownership.
 */
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

#include "formats.h"
#include "cov_formats.h"
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

/* ---- golden maps ------------------------------------------------------------ */

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

/* xorshift64*: the sequences are part of the golden file's meaning, never change it. */
static uint64_t rng_next(uint64_t *x)
{
    *x ^= *x >> 12;
    *x ^= *x << 25;
    *x ^= *x >> 27;
    return *x * UINT64_C(0x2545F4914F6CDD1D);
}

static uint64_t fnv1a(const uint8_t *d, size_t n)
{
    uint64_t h = UINT64_C(0xcbf29ce484222325);
    for (size_t i = 0; i < n; i++)
        h = (h ^ d[i]) * UINT64_C(0x100000001b3);
    return h;
}

#define GOLDEN_EMITS 3000u
#define GOLDEN_SITES 400u

#define GOLDEN_HOT_FROM 1000u
#define GOLDEN_HOT_LEN 300u

/* The emits of one scheme: site ids below GOLDEN_SITES (site model) or 4-aligned pcs in
 * three modules, the third far above 4 GiB (pc model), loops and back edges included, and
 * a history reset about every hundred emits, as an iteration boundary makes. Emits
 * GOLDEN_HOT_FROM.. are one site GOLDEN_HOT_LEN times with no reset, so its bucket
 * saturates at 255. */
static size_t golden_emits(const cf_scheme *s, emit *e)
{
    uint64_t x = UINT64_C(0x9E3779B97F4A7C15);
    uint32_t prev = 0;
    for (size_t k = 0; k < GOLDEN_EMITS; k++) {
        uint64_t r = rng_next(&x);
        uint32_t id = (r & 3) == 0 ? prev : (uint32_t)((r >> 8) % GOLDEN_SITES);   /* repeats */
        prev = id;
        uint8_t mod = (uint8_t)((r >> 40) % 3);
        uint64_t offset = ((r >> 20) % 0x4000u) * 4 + (mod == 2 ? UINT64_C(0x7f3a5c000000) : 0);
        bool hot = k >= GOLDEN_HOT_FROM && k < GOLDEN_HOT_FROM + GOLDEN_HOT_LEN;
        if (hot) {
            id = 7;
            mod = 2;
            offset = UINT64_C(0x7f3a5c001230);
        }
        cf_site_ab(s, id, offset, mod, &e[k].a, &e[k].b);
        e[k].reset_before = !hot && ((r >> 50) % 100) == 0;
    }
    return GOLDEN_EMITS;
}

typedef struct golden { char name[32]; uint64_t hash; } golden;

static size_t golden_load(const char *path, golden *g, size_t max)
{
    FILE *f = fopen(path, "r");
    CHECK(f != NULL, "golden file %s", path);
    if (!f)
        return 0;
    size_t n = 0;
    char line[128];
    while (n < max && fgets(line, sizeof(line), f)) {
        unsigned long long h;
        if (line[0] != '#' && sscanf(line, "%31s %llx", g[n].name, &h) == 2)
            g[n++].hash = h;
    }
    fclose(f);
    return n;
}

static void test_golden(const char *path, bool write)
{
    static const unsigned ngrams[] = { 1, 2, 3, 4, 8 };
    static const uint32_t sizes[] = { 4096, 65536, 1048576 };
    golden want[96];
    size_t n_want = write ? 0 : golden_load(path, want, 96);
    FILE *out = write ? fopen(path, "w") : NULL;
    if (write) {
        CHECK(out != NULL, "write %s", path);
        if (!out)
            return;
        fprintf(out, "# tests/unit/test_cov_formats.c --write-golden: FNV-1a 64 of each scheme's map\n");
    }
    emit *e = calloc(GOLDEN_EMITS, sizeof(*e));
    unsigned checked = 0;
    for (int model = 0; model < 2; model++)
        for (size_t ni = 0; ni < UAM_ARRAY_LEN(ngrams); ni++)
            for (int rot = 0; rot < 2; rot++)
                for (size_t si = 0; si < UAM_ARRAY_LEN(sizes); si++) {
                    cf_scheme s = { .model = model ? CF_MODEL_PC : CF_MODEL_SITE, .ngram = ngrams[ni],
                                    .rotate = rot != 0, .map_size = sizes[si] };
                    char name[32];
                    snprintf(name, sizeof(name), "%s/%u", cf_scheme_name(&s), s.map_size);
                    uam_cov_history h;
                    cf_cov_history(&s, &h);
                    size_t n = golden_emits(&s, e);
                    static const char *const bk[3] = { "interp", "jit", "jit-nochain" };
                    uint8_t *ref = run_emits(e, n, s.map_size, &h, 0);
                    for (int b = 1; b < 3; b++) {
                        uint8_t *map = run_emits(e, n, s.map_size, &h, b);
                        CHECK(!memcmp(map, ref, s.map_size), "%s: %s differs from the interpreter",
                              name, bk[b]);
                        free(map);
                    }
                    uint64_t hash = fnv1a(ref, s.map_size);
                    size_t nz = 0, full = 0;
                    for (uint32_t i = 0; i < s.map_size; i++) {
                        nz += ref[i] != 0;
                        full += ref[i] == 255;
                    }
                    CHECK(nz > 50, "%s: only %zu buckets hit", name, nz);
                    CHECK(full >= 1, "%s: no bucket saturated", name);
                    free(ref);
                    if (write) {
                        fprintf(out, "%s %016llx\n", name, (unsigned long long)hash);
                        continue;
                    }
                    size_t k = 0;
                    while (k < n_want && strcmp(want[k].name, name))
                        k++;
                    CHECK(k < n_want, "%s: not in the golden file", name);
                    if (k < n_want)
                        CHECK(want[k].hash == hash, "%s: map hash %016llx, golden %016llx: the "
                              "coverage formula changed", name, (unsigned long long)hash,
                              (unsigned long long)want[k].hash);
                    checked++;
                }
    free(e);
    if (write)
        fclose(out);
    else
        CHECK(checked == n_want, "%u schemes checked, %zu in the golden file", checked, n_want);
}

/* The parts of the formula, against values worked out outside this code. */
static void test_formula_parts(void)
{
    EQ64(cf_mix(0), 0, "mix(0)");
    EQ64(cf_mix(1), UINT64_C(0xff51afd792fd5b26), "mix(1)");
    EQ64(cf_mix(2), UINT64_C(0xfea35fafa5fab64d), "mix(2)");
    EQ64(cf_mix(0x12345678), UINT64_C(0x05e9303903fe4604), "mix(0x12345678)");
    EQ64(cf_mix(UINT64_C(0x7f3a5c001230)), UINT64_C(0xb5a321af4241bf5e), "mix(wide)");
    EQ64(cf_spread(1, 0xfff), 0xb26, "spread(1) in 4 KiB");
    EQ64(cf_spread(2, 0xffff), 0xb64d, "spread(2) in 64 KiB");
    EQ64(cf_hash_pc(UINT64_C(0x7f3a5c001230), 0, 0xfffff), 0x1bf5e, "pc hash, module 0");
    EQ64(cf_hash_pc(UINT64_C(0x7f3a5c001230), 2, 0xfffff), 0x5fd82, "pc hash, module 2");

    static const struct { unsigned ngram; uint32_t size; uint8_t rot[7]; } r[] = {
        { 2, 4096, { 1 } },
        { 3, 4096, { 1, 6 } },
        { 8, 4096, { 1, 2, 3, 4, 5, 6, 7 } },
        { 4, 65536, { 1, 6, 11 } },
        { 8, 65536, { 1, 3, 5, 7, 9, 11, 13 } },
        { 3, 1048576, { 1, 10 } },
    };
    for (size_t i = 0; i < UAM_ARRAY_LEN(r); i++) {
        cf_scheme s = { .model = CF_MODEL_SITE, .ngram = r[i].ngram, .rotate = true,
                        .map_size = r[i].size };
        uam_cov_history h;
        cf_cov_history(&s, &h);
        EQ64(h.depth, r[i].ngram - 1, "history depth");
        for (unsigned j = 0; j < 7; j++)
            CHECK(h.rot[j] == r[i].rot[j], "N=%u, %u-byte map: rotation %u is %u, expected %u",
                  r[i].ngram, r[i].size, j, h.rot[j], r[i].rot[j]);
        s.rotate = false;
        cf_cov_history(&s, &h);
        for (unsigned j = 0; j < 7; j++)
            CHECK(h.rot[j] == 0, "no rotation asked, rotation %u is %u", j, h.rot[j]);
    }
    cf_scheme n1 = { .model = CF_MODEL_PC, .ngram = 1, .rotate = true, .map_size = 4096 };
    uam_cov_history h1;
    cf_cov_history(&n1, &h1);
    CHECK(h1.depth == 1 && h1.rot[0] == 0, "N=1 folds no history");

    for (int model = 0; model < 2; model++)
        for (unsigned n = 1; n <= CF_NGRAM_MAX; n++)
            for (int rot = 0; rot < 2; rot++) {
                cf_scheme s = { .model = model ? CF_MODEL_PC : CF_MODEL_SITE, .ngram = n,
                                .rotate = rot != 0, .map_size = 4096 };
                EQ64(cf_scheme_flags(&s), 1u | (model ? 2u : 0) | (rot ? 4u : 0) | (n << 8),
                     cf_scheme_name(&s));
            }
}

static void test_map_size(void)
{
    static const struct { uint64_t sites; unsigned ngram; uint32_t size; } t[] = {
        { 0, 1, 4096 }, { 1, 1, 4096 }, { 2048, 1, 4096 }, { 2049, 1, 8192 }, { 100000, 1, 262144 },
        { 0, 2, 4096 }, { 256, 2, 4096 }, { 257, 2, 8192 }, { 1000, 2, 16384 },
        { 1000000, 2, 16777216 }, { 1048576, 2, 16777216 }, { 20000000, 2, 33554432 },
        { 1000, 3, 32768 }, { 1000, 8, 1048576 }, { 100000, 8, 16777216 },
    };
    for (size_t i = 0; i < UAM_ARRAY_LEN(t); i++)
        EQ64(cf_map_size_for(t[i].sites, t[i].ngram), t[i].size, "map size rule");
}

/* ---- site list ------------------------------------------------------------ */

typedef struct buf { uint8_t d[1024]; size_t n; } buf;

static void put(buf *b, uint64_t v, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        b->d[b->n++] = (uint8_t)(v >> (8 * i));
}

#define SL_HDR 24u
#define SL_REC 16u

/* Two modules ("fw", "lib"), three records (the second in the alternate state, checking 2
 * bytes), optionally the base table. */
static buf sites_file(bool bases)
{
    buf b = { .n = 0 };
    memcpy(b.d, UAM_SITES_MAGIC, 4);
    b.n = 4;
    put(&b, UAM_SITES_VERSION, 4);
    put(&b, 3, 4);                       /* count */
    b.d[b.n++] = bases ? UAM_SITES_FLAG_BASES : 0;
    b.d[b.n++] = 2;                      /* modules */
    put(&b, 0, 2);                       /* reserved */
    put(&b, SL_HDR + 3 * SL_REC, 4);     /* modules_offset */
    put(&b, 7, 4);                       /* "fw\0lib\0" */
    static const struct { uint64_t off; uint8_t mod, len, flags; uint32_t orig; } r[3] = {
        { 0x10, 0, 4, 0, 0x00000013 }, { 0x20, 0, 2, UAM_SITE_ALT_STATE, 0x00000593 },
        { 0x30, 1, 4, 0, 0x00008067 } };
    for (int i = 0; i < 3; i++) {
        put(&b, r[i].off, 8);
        b.d[b.n++] = r[i].mod;
        b.d[b.n++] = r[i].len;
        b.d[b.n++] = r[i].flags;
        b.d[b.n++] = 0;
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

static bool parses(const buf *b, cf_sites *p, char *err)
{
    return cf_sites_parse(b->d, b->n, p, err, 256);
}

static void test_sites(void)
{
    char err[256];
    cf_sites p;
    buf b = sites_file(false);
    CHECK(parses(&b, &p, err), "valid list: %s", err);
    CHECK(p.count == 3 && p.n_modules == 2 && !strcmp(p.module[0], "fw") && !strcmp(p.module[1], "lib")
          && p.rec[2].offset == 0x30 && p.rec[2].module == 1 && p.rec[1].orig[0] == 0x93
          && p.rec[1].len == 2 && p.rec[1].flags == UAM_SITE_ALT_STATE && !p.has_bases,
          "fields decoded");
    cf_sites_free(&p);

    b = sites_file(true);
    CHECK(parses(&b, &p, err) && p.has_bases && p.bases[0] == 0x1000 && p.bases[1] == 0x8000,
          "base table: %s", err);
    cf_sites_free(&p);

    /* The reader checklist. */
    const size_t names = SL_HDR + 3 * SL_REC;
    struct { const char *what; size_t at; uint8_t val; size_t trunc; bool bases; } bad[] = {
        { "magic", 0, 'X', 0, false },
        { "version", 4, 2, 0, false },
        { "count too large", 8, 200, 0, false },
        { "unknown header flag", 12, 0x80, 0, false },
        { "no modules", 13, 0, 0, false },
        { "reserved header bytes", 14, 1, 0, false },
        { "empty name table", 20, 0, 0, false },
        { "name table past the end", 16, 250, 0, false },
        { "name table not NUL-terminated", names + 6, 'x', 0, false },
        { "module index", SL_HDR + 8, 5, 0, false },
        { "length over 4", SL_HDR + 9, 5, 0, false },
        { "unknown record flag", SL_HDR + 10, 2, 0, false },
        { "reserved record byte", SL_HDR + 11, 1, 0, false },
        { "orig bytes past len", SL_HDR + SL_REC + 12 + 3, 1, 0, false },
        { "short file", 0, 'U', 10, false },
        { "base table past the end", 0, 'U', 0, true },
    };
    for (size_t i = 0; i < UAM_ARRAY_LEN(bad); i++) {
        b = sites_file(bad[i].bases);
        b.d[bad[i].at] = bad[i].val;
        if (bad[i].trunc)
            b.n = bad[i].trunc;
        if (bad[i].bases)
            b.n -= 4;
        CHECK(!parses(&b, &p, err), "rejected: %s", bad[i].what);
    }
    b = sites_file(false);
    b.d[4] = 4;
    CHECK(!parses(&b, &p, err) && strstr(err, "version 4"), "the error names the version: %s", err);
    b = sites_file(false);
    memcpy(b.d, UAM_BOUNDS_MAGIC, 4);
    CHECK(!parses(&b, &p, err) && strstr(err, "gen_sites.py"), "a boundary file: %s", err);
    /* An empty name: "\0w\0lib\0" holds an empty first name. */
    b = sites_file(false);
    memcpy(b.d + names, "\0w\0lib\0", 7);
    CHECK(!parses(&b, &p, err) && strstr(err, "empty"), "empty module name: %s", err);

    /* Bases: CLI, then the base table, then the image base for module 0. */
    b = sites_file(false);
    parses(&b, &p, err);
    uint64_t bases[2];
    CHECK(!cf_resolve_bases(&p, NULL, 0, true, 0x2000, bases, err, sizeof(err)) && strstr(err, "lib"),
          "module 1 has no base: %s", err);
    cf_module_base cli = { "lib", 0x9000 };
    CHECK(cf_resolve_bases(&p, &cli, 1, true, 0x2000, bases, err, sizeof(err))
          && bases[0] == 0x2000 && bases[1] == 0x9000, "image base + --cov-module");
    cf_module_base nope = { "nope", 1 };
    CHECK(!cf_resolve_bases(&p, &nope, 1, true, 0, bases, err, sizeof(err)), "unknown --cov-module");

    cf_scheme s = { .model = CF_MODEL_SITE, .ngram = 2, .map_size = 4096 };
    uam_cov_site *sites;
    CHECK(cf_build_sites(&p, bases, &s, 32, 1, &sites, err, sizeof(err)), "build sites");
    EQ64(sites[0].pc, 0x2010, "site 0 address");
    EQ64(sites[1].pc, 0x2021, "site 1 address: the alternate state's bit set");
    EQ64(sites[2].pc, 0x9030, "site 2 address (module 1)");
    CHECK(sites[1].a == 1 && sites[1].b == cf_spread(1, 4095), "site-id operands");
    cf_namer nm;
    CHECK(cf_namer_init(&nm, &p, sites), "namer");
    char desc[128] = "";
    cf_namer_describe(&nm, 0x9030, desc, sizeof(desc));
    CHECK(strstr(desc, "record 2") && strstr(desc, "lib"), "namer: %s", desc);
    cf_namer_fini(&nm);
    free(sites);

    /* The site-byte check: write two of the three sites' bytes into memory (the second
     * one checks 2 bytes, at its address without the state bit). */
    CHECK(cf_build_sites(&p, bases, &s, 32, 0, &sites, err, sizeof(err)), "build sites, no state");
    EQ64(sites[1].pc, 0x2020, "no state bits on an ISA without them");
    uam_machine m;
    CHECK(uam_machine_init(&m, &uam_isa_rv32) == UAM_OK, "machine");
    uam_bus_map_ram(&m.bus, "ram", 0, 1 << 16);
    uam_bus_load_bytes(&m.bus, 0x2010, p.rec[0].orig, 4);
    uam_bus_load_bytes(&m.bus, 0x2020, p.rec[1].orig, 2);
    char out[1024] = "";
    FILE *f = fmemopen(out, sizeof(out), "w");
    size_t bad_sites = cf_check_sites(&m, &p, sites, f, 4);
    fclose(f);
    EQ64(bad_sites, 1, "one mismatching site");
    CHECK(strstr(out, "2 of 3 sites match") && strstr(out, "site 2"), "check report:\n%s", out);
    uam_machine_fini(&m);
    free(sites);

    s.model = CF_MODEL_PC;
    s.ngram = 2;
    CHECK(cf_build_sites(&p, bases, &s, 32, 0, &sites, err, sizeof(err)), "pc model sites");
    CHECK(sites[2].a == cf_hash_pc(0x30, 1, 4095) && sites[2].b == sites[2].a >> 1,
          "hashed-pc operands: A = hash(offset, module), B = A >> 1");
    free(sites);
    cf_sites_free(&p);
}

/* ---- boundary file -------------------------------------------------------- */

static buf bounds_file(unsigned n, const uint8_t *kinds, uint8_t depth)
{
    buf b = { .n = 0 };
    memcpy(b.d, UAM_BOUNDS_MAGIC, 4);
    b.n = 4;
    put(&b, UAM_BOUNDS_VERSION, 4);
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

static void test_bounds(void)
{
    char err[256];
    cf_markers bd;
    cf_bounds bo;
    const uint8_t be[2] = { 0, 1 }, bb[2] = { 0, 0 }, b1[1] = { 0 }, beee[4] = { 1, 0, 1, 1 };

    buf b = bounds_file(2, be, 1);
    CHECK(cf_bounds_parse(b.d, b.n, &bd, err, sizeof(err)), "paired: %s", err);
    CHECK(cf_bounds_apply(&bd, 0x80000000u, 32, 0, &bo, err, sizeof(err)), "apply: %s", err);
    CHECK(bo.n_end_pcs == 1 && bo.nested && bo.begin_pc == 0x80000100u && bo.end_pcs[0] == 0x80000110u,
          "paired, depth-zero only: nested");
    cf_bounds_free(&bo);
    cf_markers_free(&bd);

    b = bounds_file(2, be, 0);
    cf_bounds_parse(b.d, b.n, &bd, err, sizeof(err));
    CHECK(cf_bounds_apply(&bd, 0, 32, 0, &bo, err, sizeof(err)) && bo.n_end_pcs == 1 && !bo.nested,
          "paired, any depth");
    cf_bounds_free(&bo);
    cf_markers_free(&bd);

    /* Several END markers, the BEGIN among them: every END is kept. */
    b = bounds_file(4, beee, 1);
    cf_bounds_parse(b.d, b.n, &bd, err, sizeof(err));
    CHECK(cf_bounds_apply(&bd, 0x1000, 32, 0, &bo, err, sizeof(err)) && bo.begin_pc == 0x1110
          && bo.n_end_pcs == 3 && bo.end_pcs[0] == 0x1100 && bo.end_pcs[1] == 0x1120
          && bo.end_pcs[2] == 0x1130 && bo.nested, "three END markers: %s", err);
    cf_bounds_free(&bo);
    cf_markers_free(&bd);

    /* Mixed depth filters have no single meaning. */
    b = bounds_file(4, beee, 1);
    b.d[16 + 2 * 16 + 9] = 0;   /* marker 2: any depth */
    cf_bounds_parse(b.d, b.n, &bd, err, sizeof(err));
    CHECK(!cf_bounds_apply(&bd, 0, 32, 0, &bo, err, sizeof(err)) && strstr(err, "depth filters"),
          "mixed depth filters refused: %s", err);
    cf_markers_free(&bd);

    b = bounds_file(1, b1, 0);
    cf_bounds_parse(b.d, b.n, &bd, err, sizeof(err));
    CHECK(cf_bounds_apply(&bd, 0x1000, 32, 0, &bo, err, sizeof(err)) && bo.n_end_pcs == 0
          && !bo.nested && bo.begin_pc == 0x1100, "begin-only");
    cf_bounds_free(&bo);
    cf_markers_free(&bd);

    /* The alternate state: the flag sets the ISA's state bits in the marker's pc. */
    b = bounds_file(2, be, 0);
    b.d[16 + 16 + 10] = UAM_SITE_ALT_STATE;   /* the END */
    cf_bounds_parse(b.d, b.n, &bd, err, sizeof(err));
    CHECK(cf_bounds_apply(&bd, 0x1000, 32, 1, &bo, err, sizeof(err)) && bo.begin_pc == 0x1100
          && bo.end_pcs[0] == 0x1111, "alternate-state END marker: %s", err);
    cf_bounds_free(&bo);
    cf_markers_free(&bd);

    /* Several BEGIN markers: the first is begin_pc, the others more begins. */
    b = bounds_file(2, bb, 0);
    cf_bounds_parse(b.d, b.n, &bd, err, sizeof(err));
    CHECK(cf_bounds_apply(&bd, 0x1000, 32, 0, &bo, err, sizeof(err)) && bo.begin_pc == 0x1100
          && bo.n_more_begin_pcs == 1 && bo.more_begin_pcs[0] == 0x1110 && bo.n_end_pcs == 0
          && !bo.nested, "two BEGINs: %s", err);
    cf_bounds_free(&bo);
    cf_markers_free(&bd);
    const uint8_t bbe[3] = { 0, 1, 0 };
    b = bounds_file(3, bbe, 1);
    cf_bounds_parse(b.d, b.n, &bd, err, sizeof(err));
    CHECK(cf_bounds_apply(&bd, 0x1000, 32, 0, &bo, err, sizeof(err)) && bo.begin_pc == 0x1100
          && bo.n_more_begin_pcs == 1 && bo.more_begin_pcs[0] == 0x1120 && bo.n_end_pcs == 1
          && bo.end_pcs[0] == 0x1110 && bo.nested, "two BEGINs and an END, nested: %s", err);
    cf_bounds_free(&bo);
    cf_markers_free(&bd);

    /* The reader checklist. */
    struct { const char *what; size_t at; uint8_t val; } bad[] = {
        { "magic", 0, 'X' }, { "version", 4, 2 }, { "reserved header", 12, 1 },
        { "unknown kind", 16 + 8, 7 }, { "unknown depth", 16 + 9, 2 },
        { "unknown flag", 16 + 10, 2 }, { "reserved record bytes", 16 + 15, 1 },
    };
    for (size_t i = 0; i < UAM_ARRAY_LEN(bad); i++) {
        b = bounds_file(1, b1, 0);
        b.d[bad[i].at] = bad[i].val;
        CHECK(!cf_bounds_parse(b.d, b.n, &bd, err, sizeof(err)), "rejected: %s", bad[i].what);
    }
    b = bounds_file(1, b1, 0);
    b.d[4] = 2;
    CHECK(!cf_bounds_parse(b.d, b.n, &bd, err, sizeof(err)) && strstr(err, "version 2"), "version: %s", err);
    b = bounds_file(1, b1, 0);
    b.n -= 1;
    CHECK(!cf_bounds_parse(b.d, b.n, &bd, err, sizeof(err)), "truncated");
}

/* ---- coverage region ownership -------------------------------------------- */

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
    fseek(fp, (long)offsetof(struct uam_cov_region_header, owner_pid), SEEK_SET);
    fwrite(&pid, 4, 1, fp);
    fseek(fp, (long)offsetof(struct uam_cov_region_header, owner_start), SEEK_SET);
    fwrite(&start, 4, 1, fp);
    fclose(fp);
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void test_region_owner(void)
{
    char path[] = "/tmp/uam-covregion-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0, "mkstemp");
    if (fd < 0)
        return;
    close(fd);
    char err[256] = "";
    cf_region *l = cf_region_create(path, 4096, 2, 0, err, sizeof(err));
    CHECK(l != NULL, "create: %s", err);
    cf_region_close(l);
    CHECK((l = cf_region_create(path, 4096, 2, 0, err, sizeof(err))) != NULL,
          "our own earlier region is ours: %s", err);
    cf_region_close(l);

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
    l = cf_region_create(path, 4096, 2, 0, err, sizeof(err));
    CHECK(!l && strstr(err, "in use"), "a live owner's region is refused (%s)", err);
    cf_region_close(l);
    /* The same pid with another start time is a reused pid: the owner is gone. */
    set_owner(path, (uint32_t)kid, start + 1);
    l = cf_region_create(path, 4096, 2, 0, err, sizeof(err));
    CHECK(l != NULL, "a reused pid does not own the region: %s", err);
    cf_region_close(l);
    kill(kid, SIGKILL);
    waitpid(kid, NULL, 0);

    /* The decision is made under an exclusive flock. */
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
    l = cf_region_create(path, 4096, 2, 0, err, sizeof(err));
    double waited = now_s() - t0;
    CHECK(l != NULL, "create after the lock is released: %s", err);
    CHECK(waited >= 0.2, "create waited for the lock (%.3f s)", waited);
    cf_region_close(l);
    waitpid(kid, NULL, 0);
    close(pfd[0]);
    close(pfd[1]);
    unlink(path);
}

int main(int argc, char **argv)
{
    const bool write = argc == 3 && !strcmp(argv[1], "--write-golden");
    if (argc != 2 && !write) {
        fprintf(stderr, "usage: %s GOLDEN | --write-golden GOLDEN\n", argv[0]);
        return 2;
    }
    test_golden(argv[argc - 1], write);
    if (write) {
        printf("test_cov_formats: wrote %s\n", argv[2]);
        return n_failed ? 1 : 0;
    }
    test_formula_parts();
    test_map_size();
    test_sites();
    test_bounds();
    test_region_owner();
    printf("test_cov_formats: %d/%d checks passed\n", n_checks - n_failed, n_checks);
    return n_failed ? 1 : 0;
}
