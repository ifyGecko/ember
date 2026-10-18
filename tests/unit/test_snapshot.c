/*
 * test_snapshot.c - Unit tests for state streams (uam/state.h) and machine
 * snapshots (uam/snapshot.h): exact re-execution after restore (interpreter
 * and JIT), incremental and full restores, files, refusals, extra state,
 * timer tie order, and restoring from a PC hook.
 *
 * The machine runs a hand-encoded MIPS32 big-endian program that writes a
 * different RAM page every iteration, reads a timer-driven test device, and
 * takes CP0 timer interrupts.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "uam/isa/mips32.h"
#include "uam/machine.h"
#include "uam/snapshot.h"
#include "uam/state.h"

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

#define EQ64(got, want, what) do {                                         \
        uint64_t g_ = (got), w_ = (want);                                  \
        CHECK(g_ == w_, "%s: got 0x%llx want 0x%llx", what,                \
              (unsigned long long)g_, (unsigned long long)w_);             \
    } while (0)

static const char *scratch_dir(void)
{
    const char *d = getenv("TMPDIR");
    return d && *d ? d : "/tmp";
}

/* ------------------------------------------------------------------------ */
/* State streams                                                             */
/* ------------------------------------------------------------------------ */

static void test_stream(void)
{
    uam_state_writer *w = uam_state_writer_new(NULL);
    const uint8_t blob[5] = { 1, 2, 3, 4, 5 };
    uam_state_begin(w, "first");
    uam_state_put_u8(w, 0xAB);
    uam_state_put_u16(w, 0xBEEF);
    uam_state_put_u32(w, 0xDEADBEEFu);
    uam_state_put_u64(w, UINT64_C(0x0123456789ABCDEF));
    uam_state_put_bool(w, true);
    uam_state_put_bytes(w, blob, sizeof(blob));
    uam_state_end(w);
    uam_state_begin(w, "second");
    uam_state_put_u32(w, 7);
    uam_state_end(w);
    CHECK(uam_state_writer_ok(w), "writer ok");
    size_t len;
    const uint8_t *data = uam_state_writer_data(w, &len);

    uam_state_reader *r = uam_state_reader_new(data, len, NULL);
    const char *name = uam_state_next(r);
    CHECK(name && !strcmp(name, "first"), "first record");
    EQ64(uam_state_get_u8(r), 0xAB, "u8");
    EQ64(uam_state_get_u16(r), 0xBEEF, "u16");
    EQ64(uam_state_get_u32(r), 0xDEADBEEFu, "u32");
    EQ64(uam_state_get_u64(r), UINT64_C(0x0123456789ABCDEF), "u64");
    CHECK(uam_state_get_bool(r), "bool");
    uint8_t got[5] = { 0 };
    uam_state_get_bytes(r, got, sizeof(got));
    CHECK(!memcmp(got, blob, sizeof(blob)), "bytes");
    uam_state_leave(r, "first");
    name = uam_state_next(r);
    CHECK(name && !strcmp(name, "second"), "second record");
    EQ64(uam_state_get_u32(r), 7, "second value");
    uam_state_leave(r, "second");
    CHECK(uam_state_next(r) == NULL && uam_state_ok(r), "clean end of stream");
    uam_state_reader_free(r);

    /* A record not read to its end is an error (save and restore disagree). */
    r = uam_state_reader_new(data, len, NULL);
    uam_state_next(r);
    uam_state_get_u8(r);
    uam_state_leave(r, "first");
    CHECK(!uam_state_ok(r) && strstr(uam_state_error(r), "unread"), "partial read refused: %s",
          uam_state_error(r) ? uam_state_error(r) : "(none)");
    uam_state_reader_free(r);

    /* Reading past a record, or a byte array of the wrong size, fails and sticks. */
    r = uam_state_reader_new(data, len, NULL);
    uam_state_next(r);
    uam_state_skip(r);
    uam_state_next(r);
    uam_state_get_u64(r);   /* only 4 bytes in "second" */
    CHECK(!uam_state_ok(r), "overrun refused");
    EQ64(uam_state_get_u32(r), 0, "gets return 0 after an error");
    uam_state_reader_free(r);

    r = uam_state_reader_new(data, len, NULL);
    uam_state_next(r);
    for (int i = 0; i < 5; i++)
        uam_state_get_u8(r);   /* skip to the bytes field, misaligned on purpose */
    uint8_t small[4];
    uam_state_get_bytes(r, small, sizeof(small));
    CHECK(!uam_state_ok(r), "wrong-size byte array refused");
    uam_state_reader_free(r);

    /* Truncated stream. */
    r = uam_state_reader_new(data, len - 3, NULL);
    while (uam_state_next(r))
        uam_state_skip(r);
    CHECK(!uam_state_ok(r), "truncated stream refused");
    uam_state_reader_free(r);
    uam_state_writer_free(w);
}

/* Timers with equal deadlines fire in their saved order, however their
 * owners happen to restore them. */
typedef struct tie_timer {
    uam_timer t;
    char      tag;
    char     *log;
} tie_timer;

static void tie_fire(void *ctx)
{
    tie_timer *tt = ctx;
    size_t n = strlen(tt->log);
    tt->log[n] = tt->tag;
    tt->log[n + 1] = 0;
}

static void test_timer_ties(void)
{
    uint64_t icount = 0;
    uam_sched s;
    uam_sched_init(&s, &icount);
    char log[16] = "";
    tie_timer tt[4];
    const char tags[4] = { 'A', 'B', 'C', 'D' };
    for (int i = 0; i < 4; i++) {
        tt[i] = (tie_timer){ .tag = tags[i], .log = log };
        uam_timer_init(&tt[i].t, tie_fire, &tt[i]);
    }
    uam_timer_arm(&s, &tt[0].t, 100);   /* A, B, C tie at 100; D comes first */
    uam_timer_arm(&s, &tt[1].t, 100);
    uam_timer_arm(&s, &tt[2].t, 100);
    uam_timer_arm(&s, &tt[3].t, 50);

    uam_state_writer *w = uam_state_writer_new(&s);
    uam_state_begin(w, "timers");
    for (int i = 3; i >= 0; i--)       /* saved in another order than armed */
        uam_state_put_timer(w, &tt[i].t);
    uam_state_end(w);
    size_t len;
    const uint8_t *data = uam_state_writer_data(w, &len);

    uam_sched_cancel_all(&s);
    EQ64(uam_sched_count(&s), 0, "all timers cancelled");
    uam_state_reader *r = uam_state_reader_new(data, len, &s);
    uam_state_next(r);
    for (int i = 3; i >= 0; i--)
        uam_state_get_timer(r, &tt[i].t);
    uam_state_leave(r, "timers");
    EQ64(uam_sched_count(&s), 0, "nothing armed before arm_timers");
    uam_state_arm_timers(r);
    CHECK(uam_sched_check(&s), "list well formed");
    uam_sched_skip_to(&s, 100);
    uam_sched_run_due(&s);
    CHECK(!strcmp(log, "DABC"), "firing order after restore: %s (want DABC)", log);
    uam_state_reader_free(r);
    uam_state_writer_free(w);
}

/* ------------------------------------------------------------------------ */
/* The test machine                                                          */
/* ------------------------------------------------------------------------ */

#define RAM_SIZE  (UINT64_C(4) << 20)
#define TICK_PHYS UINT64_C(0x1F000000)
#define LOOP_PC   UINT64_C(0x80000010)
#define CP0(r, s) ((r) * 8 + (s))

/* A device with a periodic timer: reads return how often it has fired. */
typedef struct tickdev {
    uam_device dev;
    uam_sched *sched;
    uam_timer  timer;
    uint32_t   count;
    bool       save_timer;   /* false: "forgets" its timer (a buggy device) */
} tickdev;

static void tick_fire(void *ctx)
{
    tickdev *t = ctx;
    t->count++;
    uam_timer_arm(t->sched, &t->timer, uam_sched_now(t->sched) + 333);
}

static uam_status tick_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    (void)off; (void)size;
    *val = UAM_CONTAINER_OF(d, tickdev, dev)->count;
    return UAM_OK;
}

static uam_status tick_write(uam_device *d, uint64_t off, unsigned size, uint64_t val)
{
    (void)d; (void)off; (void)size; (void)val;
    return UAM_OK;
}

static void tick_destroy(uam_device *d)
{
    tickdev *t = UAM_CONTAINER_OF(d, tickdev, dev);
    uam_timer_cancel(t->sched, &t->timer);
    free(t);
}

static void tick_save(uam_device *d, uam_state_writer *w)
{
    tickdev *t = UAM_CONTAINER_OF(d, tickdev, dev);
    uam_state_put_u32(w, t->count);
    if (t->save_timer)
        uam_state_put_timer(w, &t->timer);
}

static void tick_restore(uam_device *d, uam_state_reader *r)
{
    tickdev *t = UAM_CONTAINER_OF(d, tickdev, dev);
    t->count = uam_state_get_u32(r);
    if (t->save_timer)
        uam_state_get_timer(r, &t->timer);
}

static const uam_device_ops tick_ops = {
    .type = "test,tick", .endian = UAM_ACC_BE,
    .read = tick_read, .write = tick_write, .destroy = tick_destroy,
    .save = tick_save, .restore = tick_restore,
};

/* The same device type without snapshot support. */
static const uam_device_ops tick_ops_nosnap = {
    .type = "test,tick-nosnap", .endian = UAM_ACC_BE,
    .read = tick_read, .write = tick_write, .destroy = tick_destroy,
};

static const uam_device_ops stateless_ops = {
    .type = "test,stateless", .endian = UAM_ACC_BE,
    .read = tick_read, .write = tick_write, .destroy = tick_destroy,
    .flags = UAM_DEVICE_STATELESS,
};

static void put32(uam_machine *m, uint64_t pa, uint32_t w)
{
    uint8_t b[4] = { (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w };
    CHECK(uam_bus_load_bytes(&m->bus, pa, b, 4) == UAM_OK, "load word");
}

static uint32_t gpr(uam_machine *m, unsigned r) { return (uint32_t)uam_cpu_reg_read(&m->cpu, r * 4, 32); }

static tickdev *add_tick(uam_machine *m, const uam_device_ops *ops, const char *name, uint64_t base)
{
    tickdev *t = calloc(1, sizeof(*t));
    uam_device_init(&t->dev, ops, name);
    t->sched = &m->sched;
    t->save_timer = true;
    uam_timer_init(&t->timer, tick_fire, t);
    uam_machine_add_device(m, &t->dev);
    CHECK(uam_bus_map_mmio(&m->bus, name, base, 0x100, &t->dev) == UAM_OK, "map %s", name);
    return t;
}

/*
 *   0x00 lui  t1, 0x8010         buffer at 0x80100000
 *   0x04 lui  t2, 0x000F
 *   0x08 ori  t2, t2, 0xF000     page mask 0x000FF000 (256 pages)
 *   0x0c lui  t7, 0xBF00         tick device (kseg1)
 * loop:
 *   0x10 addiu t0, t0, 1
 *   0x14 sll  t3, t0, 12
 *   0x18 and  t3, t3, t2
 *   0x1c addu t3, t3, t1
 *   0x20 sw   t0, 0(t3)          one page per iteration
 *   0x24 lw   t6, 0(t7)          tick count
 *   0x28 addu t5, t5, t6
 *   0x2c b    loop
 *   0x30 nop
 * CP0 timer handler (0x180): count in k0, re-arm 500 Count cycles ahead.
 */
static void build(uam_machine *m, uint64_t ram_size, bool jit)
{
    CHECK(uam_machine_init(m, &uam_isa_mips32be) == UAM_OK, "machine init");
    m->board_name = "snap-test";
    CHECK(uam_bus_map_ram(&m->bus, "ram", 0, ram_size) == UAM_OK, "map ram");
    tickdev *t = add_tick(m, &tick_ops, "tick", TICK_PHYS);
    static const uint32_t prog[] = {
        0x3C098010u, 0x3C0A000Fu, 0x354AF000u, 0x3C0FBF00u,
        0x25080001u, 0x00085B00u, 0x016A5824u, 0x01695821u,
        0xAD680000u, 0x8DEE0000u, 0x01AE6821u, 0x1000FFF8u, 0x00000000u,
    };
    for (size_t i = 0; i < UAM_ARRAY_LEN(prog); i++)
        put32(m, 4 * i, prog[i]);
    static const uint32_t handler[] = {
        0x275A0001u, 0x401B4800u, 0x277B01F4u, 0x409B5800u, 0x42000018u,
    };
    for (size_t i = 0; i < UAM_ARRAY_LEN(handler); i++)
        put32(m, 0x180 + 4 * i, handler[i]);
    uam_machine_reset(m, 0x80000000u);
    m->cpu.isa->sysreg_write(&m->cpu, CP0(12, 0), (1u << 15) | 1u);   /* IM7 | IE */
    m->cpu.isa->sysreg_write(&m->cpu, CP0(11, 0), 700);              /* Compare */
    uam_timer_arm(&m->sched, &t->timer, 333);
    if (jit)
        uam_exec_enable_jit(&m->exec);
}

/* Everything observable: memory, registers, CP0, time, the device. */
static uint64_t state_hash(uam_machine *m)
{
    uint64_t h = UINT64_C(0xcbf29ce484222325);
#define MIX(v) (h = (h ^ (uint64_t)(v)) * UINT64_C(0x100000001b3))
    const uam_region *ram = uam_bus_region(&m->bus, "ram");
    for (uint64_t i = 0; i < ram->size; i += 8) {
        uint64_t w;
        memcpy(&w, ram->host + i, 8);
        MIX(w);
    }
    for (size_t i = 0; i < m->cpu.regfile_size; i++)
        MIX(m->cpu.regfile[i]);
    MIX(m->cpu.pc);
    MIX(m->cpu.icount);
    MIX(uam_sched_now(&m->sched));
    MIX(uam_sched_next(&m->sched));
    MIX(uam_sched_count(&m->sched));
    static const uint32_t cp0[] = { CP0(9, 0), CP0(11, 0), CP0(12, 0), CP0(13, 0), CP0(14, 0) };
    for (size_t i = 0; i < UAM_ARRAY_LEN(cp0); i++) {
        uint64_t v = 0;
        m->cpu.isa->sysreg_read(&m->cpu, cp0[i], &v);
        MIX(v);
    }
    for (uam_device *d = m->devices; d; d = d->next)
        if (d->ops == &tick_ops)
            MIX(UAM_CONTAINER_OF(d, tickdev, dev)->count);
#undef MIX
    return h;
}

/* ------------------------------------------------------------------------ */
/* Machine snapshots                                                         */
/* ------------------------------------------------------------------------ */

/* Restore + re-run gives bit-identical results, repeatedly; the incremental
 * path (restoring the baseline) undoes JIT fast-path stores; translated code
 * survives. */
static void test_restore_loop(bool jit)
{
    const char *mode = jit ? "jit" : "interp";
    uam_machine m;
    build(&m, RAM_SIZE, jit);
    uam_exec_run(&m.exec, 20000);
    uam_snapshot *s = NULL;
    CHECK(uam_snapshot_take(&m, &s) == UAM_OK && s, "%s: take", mode);
    if (!s) {
        uam_machine_fini(&m);
        return;
    }
    const uint64_t h0 = state_hash(&m);
    uam_exec_run(&m.exec, 30000);
    const uint64_t h1 = state_hash(&m);
    CHECK(h1 != h0, "%s: the run changed the state", mode);
    CHECK(gpr(&m, 26) > 0, "%s: timer interrupts were taken", mode);

    uint64_t translated = 0;
    for (int i = 0; i < 5; i++) {
        CHECK(uam_snapshot_restore(&m, s) == UAM_OK, "%s: restore %d", mode, i);
        CHECK(state_hash(&m) == h0, "%s: state after restore %d matches the snapshot", mode, i);
        CHECK(uam_sched_check(&m.sched), "%s: timer list well formed", mode);
        uam_exec_run(&m.exec, 30000);
        CHECK(state_hash(&m) == h1, "%s: re-run %d is bit-identical", mode, i);
        if (i == 1)
            translated = m.exec.stats.blocks_translated;
    }
    EQ64(m.exec.stats.blocks_translated, translated, "translated code survives restores");

    /* A stop requested after the snapshot does not survive the restore. */
    uam_exec_stop(&m.exec, 3);
    CHECK(uam_exec_run(&m.exec, 1000) == UAM_STOP_REQUEST, "%s: stop request pending", mode);
    uam_snapshot_restore(&m, s);
    uam_exec_run(&m.exec, 30000);
    CHECK(state_hash(&m) == h1, "%s: runs again after a restore that follows a stop", mode);
    if (jit)
        CHECK(m.exec.stats.blocks_native > 0, "jit: blocks ran natively");

    uam_snapshot_info info;
    uam_snapshot_get_info(s, &info);
    EQ64(info.mem_bytes, RAM_SIZE, "info: memory covered");
    CHECK(info.mem_pages > 0 && info.mem_pages <= 257, "info: only non-zero pages stored (%llu)",
          (unsigned long long)info.mem_pages);
    CHECK(!strcmp(info.board, "snap-test") && !strcmp(info.isa, "mips32be"), "info: names");
    uam_snapshot_free(s);
    uam_machine_fini(&m);
}

/* Restoring a snapshot that is not the baseline (full compare), and files. */
static void test_full_and_file(void)
{
    uam_machine m;
    build(&m, RAM_SIZE, true);
    uam_exec_run(&m.exec, 20000);
    uam_snapshot *s, *s2;
    CHECK(uam_snapshot_take(&m, &s) == UAM_OK, "take s");
    const uint64_t h0 = state_hash(&m);
    uam_exec_run(&m.exec, 30000);
    const uint64_t h1 = state_hash(&m);
    uam_exec_run(&m.exec, 30000);
    CHECK(uam_snapshot_take(&m, &s2) == UAM_OK, "take s2 (new baseline)");
    const uint64_t h2 = state_hash(&m);
    uam_exec_run(&m.exec, 10000);

    CHECK(uam_snapshot_restore(&m, s) == UAM_OK, "restore an older snapshot");
    CHECK(state_hash(&m) == h0, "full restore matches");
    uam_exec_run(&m.exec, 30000);
    CHECK(state_hash(&m) == h1, "re-run after full restore is identical");
    CHECK(uam_snapshot_restore(&m, s2) == UAM_OK, "restore the newer one");
    CHECK(state_hash(&m) == h2, "and back again");

    char path[512];
    snprintf(path, sizeof(path), "%s/uam-test-%d.snap", scratch_dir(), (int)getpid());
    CHECK(uam_snapshot_save(s, path) == UAM_OK, "save to %s", path);
    uam_machine_fini(&m);

    /* A fresh machine (interpreter this time) resumes from the file. */
    build(&m, RAM_SIZE, false);
    CHECK(uam_machine_load_snapshot(&m, path) == UAM_OK, "load + restore from file");
    CHECK(state_hash(&m) == h0, "state from file matches");
    uam_exec_run(&m.exec, 30000);
    CHECK(state_hash(&m) == h1, "re-run from file is identical (across backends)");
    uam_machine_fini(&m);

    /* A corrupted file is refused. */
    FILE *f = fopen(path, "r+b");
    CHECK(f != NULL, "reopen");
    if (f) {
        fseek(f, 200, SEEK_SET);
        int c = fgetc(f);
        fseek(f, 200, SEEK_SET);
        fputc(c ^ 0x40, f);
        fclose(f);
    }
    uam_snapshot *bad = NULL;
    fprintf(stderr, "(expected snapshot errors follow)\n");
    CHECK(uam_snapshot_load(path, &bad) != UAM_OK && !bad, "corrupt file refused");
    CHECK(uam_snapshot_load("/nonexistent/uam.snap", &bad) != UAM_OK, "missing file refused");
    remove(path);
    uam_snapshot_free(s);
    uam_snapshot_free(s2);
}

static void test_refusals(void)
{
    uam_machine m;
    uam_snapshot *s = NULL;

    /* A device with no save/restore. */
    build(&m, RAM_SIZE, false);
    add_tick(&m, &tick_ops_nosnap, "legacy", TICK_PHYS + 0x1000);
    CHECK(uam_snapshot_take(&m, &s) == UAM_ERR_INVALID && !s, "device without save refused");
    uam_machine_fini(&m);

    /* Stateless devices are fine. */
    build(&m, RAM_SIZE, false);
    add_tick(&m, &stateless_ops, "plain", TICK_PHYS + 0x1000);
    CHECK(uam_snapshot_take(&m, &s) == UAM_OK, "stateless device accepted");
    uam_snapshot_free(s);
    s = NULL;
    uam_machine_fini(&m);

    /* An armed timer nobody saves. */
    build(&m, RAM_SIZE, false);
    UAM_CONTAINER_OF(m.devices, tickdev, dev)->save_timer = false;
    uam_exec_run(&m.exec, 1000);
    CHECK(uam_snapshot_take(&m, &s) == UAM_ERR_INVALID && !s, "unsaved armed timer refused");
    uam_machine_fini(&m);

    /* Restore into an incompatible machine changes nothing. */
    build(&m, RAM_SIZE, false);
    uam_exec_run(&m.exec, 5000);
    CHECK(uam_snapshot_take(&m, &s) == UAM_OK, "take");
    uam_machine_fini(&m);
    uam_machine other;
    build(&other, RAM_SIZE * 2, false);
    uam_exec_run(&other.exec, 100);
    uint64_t before = state_hash(&other);
    CHECK(uam_snapshot_restore(&other, s) == UAM_ERR_INVALID, "other RAM size refused");
    CHECK(state_hash(&other) == before, "refused restore left the machine alone");
    uam_machine_fini(&other);

    build(&other, RAM_SIZE, false);
    add_tick(&other, &tick_ops, "extra", TICK_PHYS + 0x1000);
    CHECK(uam_snapshot_restore(&other, s) == UAM_ERR_INVALID, "extra device refused");
    uam_machine_fini(&other);

    build(&other, RAM_SIZE, false);
    other.board_name = "something-else";
    CHECK(uam_snapshot_restore(&other, s) == UAM_ERR_INVALID, "other board refused");
    uam_machine_fini(&other);
    uam_snapshot_free(s);
}

/* State registered with uam_machine_add_state travels with the snapshot. */
typedef struct analysis {
    uint64_t hits;
} analysis;

static void analysis_save(void *ctx, uam_state_writer *w)
{
    uam_state_put_u64(w, ((analysis *)ctx)->hits);
}

static void analysis_restore(void *ctx, uam_state_reader *r)
{
    ((analysis *)ctx)->hits = uam_state_get_u64(r);
}

static uam_hook_action analysis_hook(void *ctx, uam_cpu *cpu, uint64_t pc)
{
    (void)cpu; (void)pc;
    ((analysis *)ctx)->hits++;
    return UAM_HOOK_CONTINUE;
}

static void test_extra_state(void)
{
    uam_machine m;
    analysis a = { 0 };
    build(&m, RAM_SIZE, false);
    CHECK(uam_machine_add_state(&m, "analysis", analysis_save, analysis_restore, &a) == UAM_OK,
          "add state");
    CHECK(uam_machine_add_state(&m, "analysis", analysis_save, analysis_restore, &a)
              == UAM_ERR_INVALID, "duplicate name refused");
    uam_exec_add_pc_hook(&m.exec, LOOP_PC, analysis_hook, &a);
    uam_exec_run(&m.exec, 10000);
    uam_snapshot *s;
    CHECK(uam_snapshot_take(&m, &s) == UAM_OK, "take");
    uint64_t at_snap = a.hits;
    uam_exec_run(&m.exec, 10000);
    CHECK(a.hits > at_snap, "hits grew");
    uam_snapshot_restore(&m, s);
    EQ64(a.hits, at_snap, "extra state restored");

    /* A machine without that state cannot take the snapshot. */
    uam_machine other;
    build(&other, RAM_SIZE, false);
    CHECK(uam_snapshot_restore(&other, s) == UAM_ERR_INVALID, "missing extra state refused");
    uam_machine_fini(&other);
    uam_snapshot_free(s);
    uam_machine_fini(&m);
}

/* A PC hook takes a snapshot and later restores it, without leaving the run. */
typedef struct hook_ctx {
    uam_machine  *m;
    uam_snapshot *s;
    unsigned      hits, restores;
} hook_ctx;

static uam_hook_action snap_hook(void *ctx, uam_cpu *cpu, uint64_t pc)
{
    (void)cpu; (void)pc;
    hook_ctx *h = ctx;
    h->hits++;
    if (h->hits == 100)
        uam_snapshot_take(h->m, &h->s);
    else if (h->hits == 200 || h->hits == 300 || h->hits == 400) {
        uam_snapshot_restore(h->m, h->s);
        h->restores++;
    } else if (h->hits == 450)
        return UAM_HOOK_STOP;
    return UAM_HOOK_CONTINUE;
}

static uam_hook_action stop_at_hook(void *ctx, uam_cpu *cpu, uint64_t pc)
{
    (void)cpu; (void)pc;
    unsigned *left = ctx;
    return --*left == 0 ? UAM_HOOK_STOP : UAM_HOOK_CONTINUE;
}

static void test_hook_restore(bool jit)
{
    uam_machine m;
    build(&m, RAM_SIZE, jit);
    hook_ctx h = { .m = &m };
    uam_exec_add_pc_hook(&m.exec, LOOP_PC, snap_hook, &h);
    uam_stop_reason why = uam_exec_run(&m.exec, 10000000);
    EQ64(why, UAM_STOP_HOOK, "run ends at the stopping hit");
    EQ64(h.restores, 3, "three restores from the hook");
    /* Arrival k at the loop sees t0 = k - 1; restored to arrival 100, then 50 more. */
    EQ64(gpr(&m, 8), 149, "guest resumed exactly at the snapshot each time");

    /* The same as a straight run to arrival 150. */
    uam_machine ref;
    build(&ref, RAM_SIZE, jit);
    unsigned limit = 150;
    uam_exec_add_pc_hook(&ref.exec, LOOP_PC, stop_at_hook, &limit);
    EQ64(uam_exec_run(&ref.exec, 10000000), UAM_STOP_HOOK, "reference run stops");
    EQ64(gpr(&ref, 8), 149, "reference run");
    EQ64(ref.cpu.icount, m.cpu.icount, "same instruction count as the reference");
    EQ64(state_hash(&ref), state_hash(&m), "same state as the reference");
    uam_snapshot_free(h.s);
    uam_machine_fini(&ref);
    uam_machine_fini(&m);
}

int main(void)
{
    test_stream();
    test_timer_ties();
    test_restore_loop(false);
    test_restore_loop(true);
    test_full_and_file();
    test_refusals();
    test_extra_state();
    test_hook_restore(false);
    test_hook_restore(true);
    printf("test_snapshot: %d/%d checks passed\n", n_checks - n_failed, n_checks);
    return n_failed ? 1 : 0;
}
