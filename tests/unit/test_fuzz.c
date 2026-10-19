/*
 * test_fuzz.c - The fuzz harness (uam/fuzz.h) and the replay frontend.
 *
 * A small target with planted bugs, hand-assembled for RV32, MIPS32 BE and MIPS32 LE:
 * after a short boot loop it reaches BEGIN, reads the input from guest memory (the
 * "mem:" route) and dispatches on its first byte:
 *   'B' a load from an unmapped address (bus fault)   'H' a tight loop (timeout)
 *   'O' a jump to a crash pc (oracle)                 'I' an illegal instruction
 *   'W' WFI/WAIT with no interrupt source (hang)      else: count, then END
 * END jumps back to BEGIN (begin-only mode ends the iteration there; paired mode
 * ends it at END). Every scenario runs on the interpreter, the JIT and the JIT
 * without chaining, and must give the same verdicts, details and maps.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "uam/fuzz.h"
#include "uam/isa/mips32.h"
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

#define MAP_SIZE 4096u

/* ---- the target ---------------------------------------------------------- */

typedef struct target {
    const char    *name;
    const uam_isa *isa;
    bool           be;
    uint64_t       base, phys;      /* first instruction: virtual, physical */
    uint32_t       w[64];
    unsigned       n;
    uint64_t       begin, end, fault_pc, oracle, illegal;
    const char    *route;           /* mem:BUF,len=BUF-4,max=64 */
    uint32_t       s0;              /* register offset of the iteration counter */
} target;

/* RV32 */
static uint32_t rv_i(unsigned f3, unsigned op, unsigned rd, unsigned rs1, int32_t imm)
{
    return ((uint32_t)imm & 0xFFF) << 20 | rs1 << 15 | f3 << 12 | rd << 7 | op;
}
static uint32_t rv_b(unsigned f3, unsigned rs1, unsigned rs2, int32_t off)
{
    uint32_t o = (uint32_t)off;
    return ((o >> 12) & 1) << 31 | ((o >> 5) & 0x3F) << 25 | rs2 << 20 | rs1 << 15 | f3 << 12
         | ((o >> 1) & 0xF) << 8 | ((o >> 11) & 1) << 7 | 0x63;
}
static uint32_t rv_jal(unsigned rd, int32_t off)
{
    uint32_t o = (uint32_t)off;
    return ((o >> 20) & 1) << 31 | ((o >> 1) & 0x3FF) << 21 | ((o >> 11) & 1) << 20
         | ((o >> 12) & 0xFF) << 12 | rd << 7 | 0x6F;
}
#define RV_ADDI(rd, rs, imm) rv_i(0, 0x13, rd, rs, imm)
#define RV_LW(rd, off, rs)   rv_i(2, 0x03, rd, rs, off)
#define RV_LBU(rd, off, rs)  rv_i(4, 0x03, rd, rs, off)
#define RV_LUI(rd, imm20)    ((uint32_t)(imm20) << 12 | (rd) << 7 | 0x37)

static target rv_target(void)
{
    enum { T0 = 5, T1 = 6, S0 = 8, S1 = 9, T3 = 28, T4 = 29, T5 = 30, T6 = 31 };
    target t = { .name = "rv32", .isa = &uam_isa_rv32, .be = false, .base = 0x1000, .phys = 0x1000,
                 .route = "mem:0x8000,len=0x7ffc,max=64", .s0 = S0 * 4 };
    uint32_t *w = t.w;
    unsigned i = 0;
    w[i++] = RV_ADDI(S0, 0, 0);
    w[i++] = RV_ADDI(S1, 0, 5);
    w[i++] = RV_ADDI(S1, S1, -1);                 /* boot loop */
    w[i++] = rv_b(1, S1, 0, -4);                  /* bne s1, x0 */
    unsigned b = i;
    t.begin = t.base + 4 * b;
    /* Branch targets, fixed below. */
    enum { L_CRASH = 15, L_HANG = 18, L_ORACLE = 19, L_ILL = 20, L_WAIT = 21 };
    w[i++] = RV_LUI(T3, 0x8);                     /* b+0  t3 = 0x8000 */
    w[i++] = RV_LW(T4, -4, T3);                   /* b+1  len */
    w[i++] = RV_LBU(T5, 0, T3);                   /* b+2  c */
    static const char keys[5] = { 'B', 'H', 'O', 'I', 'W' };
    static const unsigned dest[5] = { L_CRASH, L_HANG, L_ORACLE, L_ILL, L_WAIT };
    for (unsigned k = 0; k < 5; k++) {            /* b+3 .. b+12 */
        w[i++] = RV_ADDI(T6, 0, keys[k]);
        w[i] = rv_b(0, T5, T6, (int32_t)(4 * ((int)(b + dest[k]) - (int)i)));
        i++;
    }
    w[i++] = RV_ADDI(S0, S0, 1);                  /* b+13 */
    t.end = t.base + 4 * i;
    w[i] = rv_jal(0, (int32_t)(4 * ((int)b - (int)i)));  /* b+14 END: back to BEGIN */
    i++;
    w[i++] = RV_LUI(T0, 0x40000);                 /* b+15 CRASH */
    t.fault_pc = t.base + 4 * i;
    w[i++] = RV_LW(T1, 0, T0);                    /* b+16 bus fault */
    w[i++] = rv_jal(0, 0);                        /* b+17 */
    w[i++] = rv_jal(0, 0);                        /* b+18 HANG: spin */
    t.oracle = t.base + 4 * i;
    w[i++] = rv_jal(0, 0);                        /* b+19 ORACLE */
    t.illegal = t.base + 4 * i;
    w[i++] = 0;                                   /* b+20 ILL */
    w[i++] = 0x10500073u;                         /* b+21 WAIT: wfi */
    w[i++] = rv_jal(0, -4);
    t.n = i;
    return t;
}

/* MIPS32 */
static uint32_t mi(unsigned op, unsigned rs, unsigned rt, uint32_t imm)
{
    return op << 26 | rs << 21 | rt << 16 | (imm & 0xFFFF);
}
#define BR(i, to) ((uint32_t)((int)(to) - (int)(i) - 1))   /* branch offset in words */

static target mips_target(bool be)
{
    enum { T0 = 8, T1 = 9, T3 = 11, T4 = 12, T5 = 13, T6 = 14, S0 = 16, S1 = 17 };
    target t = { .name = be ? "mips32be" : "mips32le", .isa = be ? &uam_isa_mips32be : &uam_isa_mips32le,
                 .be = be, .base = 0x80001000u, .phys = 0x1000,
                 .route = "mem:0x80008000,len=0x80007ffc,max=64", .s0 = S0 * 4 };
    uint32_t *w = t.w;
    unsigned i = 0;
    w[i++] = mi(0x09, 0, S0, 0);                  /* addiu s0, zero, 0 */
    w[i++] = mi(0x09, 0, S1, 5);
    w[i++] = mi(0x09, S1, S1, 0xFFFF);            /* boot loop: addiu s1, s1, -1 */
    w[i] = mi(0x05, S1, 0, BR(i, 2)); i++;        /* bne s1, zero */
    w[i++] = 0;
    unsigned b = i;
    t.begin = t.base + 4 * b;
    enum { L_CRASH = 22, L_HANG = 26, L_ORACLE = 28, L_ILL = 30, L_WAIT = 31 };
    w[i++] = mi(0x0F, 0, T3, 0x8000);             /* b+0 lui t3, 0x8000 */
    w[i++] = mi(0x0D, T3, T3, 0x8000);            /* b+1 ori: t3 = 0x80008000 */
    w[i++] = mi(0x23, T3, T4, 0xFFFC);            /* b+2 lw t4, -4(t3) */
    w[i++] = mi(0x24, T3, T5, 0);                 /* b+3 lbu t5, 0(t3) */
    static const char keys[5] = { 'B', 'H', 'O', 'I', 'W' };
    static const unsigned dest[5] = { L_CRASH, L_HANG, L_ORACLE, L_ILL, L_WAIT };
    for (unsigned k = 0; k < 5; k++) {            /* b+4 .. b+18 */
        w[i++] = mi(0x09, 0, T6, (uint32_t)keys[k]);
        w[i] = mi(0x04, T5, T6, BR(i, b + dest[k])); i++;
        w[i++] = 0;
    }
    w[i++] = mi(0x09, S0, S0, 1);                 /* b+19 */
    t.end = t.base + 4 * i;
    w[i] = mi(0x04, 0, 0, BR(i, b)); i++;         /* b+20 END: b BEGIN */
    w[i++] = 0;                                   /* b+21 */
    w[i++] = mi(0x0F, 0, T0, 0x8040);             /* b+22 CRASH: t0 = 0x80400000 */
    t.fault_pc = t.base + 4 * i;
    w[i++] = mi(0x23, T0, T1, 0);                 /* b+23 bus fault */
    w[i++] = 0x1000FFFFu; w[i++] = 0;             /* b+24 */
    w[i++] = 0x1000FFFFu; w[i++] = 0;             /* b+26 HANG */
    t.oracle = t.base + 4 * i;
    w[i++] = 0x1000FFFFu; w[i++] = 0;             /* b+28 ORACLE */
    t.illegal = t.base + 4 * i;
    w[i++] = 0x70000010u;                         /* b+30 ILL */
    w[i++] = 0x42000020u;                         /* b+31 WAIT */
    w[i++] = 0x1000FFFEu; w[i++] = 0;
    t.n = i;
    return t;
}

typedef enum { BK_INTERP, BK_JIT, BK_JIT_NOCHAIN } backend;
static const char *const bk_name[] = { "interp", "jit", "jit-nochain" };

static void machine_for(uam_machine *m, const target *t, backend bk)
{
    CHECK(uam_machine_init(m, t->isa) == UAM_OK, "machine init");
    CHECK(uam_bus_map_ram(&m->bus, "ram", 0, 1 << 20) == UAM_OK, "map ram");
    for (unsigned i = 0; i < t->n; i++) {
        uint8_t b[4];
        for (int k = 0; k < 4; k++)
            b[k] = (uint8_t)(t->be ? t->w[i] >> (24 - 8 * k) : t->w[i] >> (8 * k));
        uam_bus_load_bytes(&m->bus, t->phys + 4 * i, b, 4);
    }
    uam_machine_reset(m, t->base);
    if (bk != BK_INTERP)
        CHECK(uam_exec_enable_jit(&m->exec), "jit");
    m->exec.chain = bk == BK_JIT;
}

static uam_fuzz *harness_for(uam_machine *m, const target *t, const uint64_t *ends, size_t n_ends,
                             unsigned exc)
{
    uam_fuzz_config cfg = { .begin_pc = t->begin, .end_pcs = ends, .n_end_pcs = n_ends,
                            .iter_insns = 20000, .boot_insns = 100000, .crash_exc = exc,
                            .crash_pcs = &t->oracle, .n_crash_pcs = 1, .map_size = MAP_SIZE };
    char err[256];
    uam_fuzz *f = uam_fuzz_create(m, &cfg, err, sizeof(err));
    CHECK(f != NULL, "create: %s", err);
    if (!f)
        return NULL;
    CHECK(uam_fuzz_set_block_coverage(f) == UAM_OK, "coverage");
    CHECK(uam_fuzz_select_route(f, t->route, err, sizeof(err)) == UAM_OK, "route: %s", err);
    return f;
}

static unsigned nonzero(const uint8_t *map)
{
    unsigned n = 0;
    for (unsigned i = 0; i < MAP_SIZE; i++)
        n += map[i] != 0;
    return n;
}

/* ---- verdicts, determinism, backends ------------------------------------ */

#define N_INPUTS 8
static const char *const inputs[N_INPUTS] = { "AA", "B", "H", "O", "I", "W", "AA", "" };

typedef struct outcome {
    uam_fuzz_result r[N_INPUTS];
    uint8_t         map[N_INPUTS][MAP_SIZE];
    uint8_t         init[MAP_SIZE];
} outcome;

static void run_inputs(const target *t, backend bk, outcome *o)
{
    uam_machine m;
    machine_for(&m, t, bk);
    uam_fuzz *f = harness_for(&m, t, NULL, 0, UAM_FUZZ_EXC_DEFAULT | UAM_FUZZ_EXC_TRAP);
    uam_fuzz_result br;
    CHECK(uam_fuzz_boot(f, &br) == UAM_OK, "%s/%s: boot", t->name, bk_name[bk]);
    EQ64(br.pc, t->begin, "boot stops at begin");
    size_t sz;
    memcpy(o->init, uam_fuzz_map(f, &sz), MAP_SIZE);
    EQ64(sz, MAP_SIZE, "map size");
    CHECK(nonzero(o->init) > 0, "%s/%s: the INIT map holds the boot's coverage", t->name, bk_name[bk]);
    uint32_t s0_boot = (uint32_t)uam_cpu_reg_read(&m.cpu, t->s0, 32);

    for (int i = 0; i < N_INPUTS; i++) {
        CHECK(uam_fuzz_run_one(f, (const uint8_t *)inputs[i], strlen(inputs[i]), &o->r[i]) == UAM_OK,
              "run '%s'", inputs[i]);
        memcpy(o->map[i], uam_fuzz_map(f, NULL), MAP_SIZE);
        if (o->r[i].verdict == UAM_FUZZ_OK)
            EQ64(uam_cpu_reg_read(&m.cpu, t->s0, 32), s0_boot + 1,
                 "every iteration starts from the snapshot");
    }
    const uam_fuzz_stats *st = uam_fuzz_get_stats(f);
    EQ64(st->iterations, N_INPUTS, "iterations counted");
    uam_fuzz_destroy(f);
    CHECK(m.exec.exc_hook == NULL && m.cpu.cov == NULL, "destroy detaches the harness");
    uam_machine_fini(&m);
}

static void test_verdicts(const target *t)
{
    static outcome o[3];
    for (int bk = 0; bk < 3; bk++) {
        run_inputs(t, (backend)bk, &o[bk]);
        const uam_fuzz_result *r = o[bk].r;
        EQ64(r[0].verdict, UAM_FUZZ_OK, "'AA' is fine");
        EQ64(r[0].pc, t->begin, "begin-only: ends back at begin");
        EQ64(r[1].verdict, UAM_FUZZ_CRASH_FAULT, "'B' faults");
        EQ64(r[1].detail, t->fault_pc, "at the load");
        CHECK(r[1].exc_valid && r[1].exc.kind == UAM_EXC_LOAD_FAULT, "a load bus fault");
        EQ64(r[2].verdict, UAM_FUZZ_TIMEOUT, "'H' times out");
        EQ64(r[3].verdict, UAM_FUZZ_CRASH_ORACLE, "'O' reaches the crash pc");
        EQ64(r[3].detail, t->oracle, "the crash pc");
        EQ64(r[4].verdict, UAM_FUZZ_CRASH_FAULT, "'I' is an illegal instruction");
        EQ64(r[4].detail, t->illegal, "at the illegal instruction");
        CHECK(r[4].exc_valid && r[4].exc.kind == UAM_EXC_TRAP, "reported as a trap");
        EQ64(r[5].verdict, UAM_FUZZ_HANG, "'W' hangs");
        EQ64(r[5].detail, UAM_STOP_DEADLOCK, "deadlock");
        EQ64(r[6].verdict, UAM_FUZZ_OK, "a crash does not end the campaign");
        CHECK(!memcmp(o[bk].map[0], o[bk].map[6], MAP_SIZE) && r[0].insns == r[6].insns,
              "%s/%s: the same input gives the same map and result", t->name, bk_name[bk]);
        EQ64(r[7].verdict, UAM_FUZZ_OK, "an empty input");
        CHECK(memcmp(o[bk].map[0], o[bk].map[1], MAP_SIZE), "different paths, different maps");
        if (bk) {
            bool same = !memcmp(o[bk].init, o[0].init, MAP_SIZE);
            for (int i = 0; i < N_INPUTS; i++)
                same = same && !memcmp(o[bk].map[i], o[0].map[i], MAP_SIZE)
                    && o[bk].r[i].verdict == o[0].r[i].verdict && o[bk].r[i].detail == o[0].r[i].detail
                    && o[bk].r[i].insns == o[0].r[i].insns && o[bk].r[i].pc == o[0].r[i].pc;
            CHECK(same, "%s/%s: identical to the interpreter", t->name, bk_name[bk]);
        }
    }
}

static void test_modes(const target *t)
{
    /* Paired: the iteration ends at END, before the jump back. */
    uam_machine m;
    machine_for(&m, t, BK_INTERP);
    uam_fuzz *f = harness_for(&m, t, &t->end, 1, UAM_FUZZ_EXC_DEFAULT);
    uam_fuzz_result r, bo;
    CHECK(uam_fuzz_boot(f, &r) == UAM_OK, "boot");
    uam_fuzz_run_one(f, (const uint8_t *)"AA", 2, &r);
    EQ64(r.verdict, UAM_FUZZ_OK, "paired: ok");
    EQ64(r.pc, t->end, "paired: ends at end");
    /* Without the trap class, an illegal instruction is delivered to the guest. */
    uam_fuzz_run_one(f, (const uint8_t *)"I", 1, &r);
    CHECK(!(r.verdict == UAM_FUZZ_CRASH_FAULT && r.exc.kind == UAM_EXC_TRAP),
          "%s: trap class off: not a trap crash (%s)", t->name, uam_fuzz_verdict_str(r.verdict));
    uam_fuzz_destroy(f);
    uam_machine_fini(&m);

    /* Several end pcs: whichever is reached first ends the iteration. With the bus
     * fault's pc as a second end, 'B' ends cleanly there instead of crashing. */
    const uint64_t ends[3] = { t->end, t->fault_pc, t->end /* a duplicate: one hook */ };
    machine_for(&m, t, BK_INTERP);
    f = harness_for(&m, t, ends, 3, UAM_FUZZ_EXC_DEFAULT);
    CHECK(uam_fuzz_boot(f, &r) == UAM_OK, "boot");
    uam_fuzz_run_one(f, (const uint8_t *)"AA", 2, &r);
    CHECK(r.verdict == UAM_FUZZ_OK && r.pc == t->end, "%s: two ends: 'AA' ends at END", t->name);
    uam_fuzz_run_one(f, (const uint8_t *)"B", 1, &r);
    CHECK(r.verdict == UAM_FUZZ_OK && r.pc == t->fault_pc, "%s: two ends: 'B' ends at the second (%s)",
          t->name, uam_fuzz_verdict_str(r.verdict));
    uam_fuzz_destroy(f);
    uam_machine_fini(&m);

    machine_for(&m, t, BK_INTERP);
    f = harness_for(&m, t, NULL, 0, UAM_FUZZ_EXC_DEFAULT);
    uam_fuzz_boot(f, &bo);
    uam_fuzz_run_one(f, (const uint8_t *)"AA", 2, &bo);
    CHECK(bo.insns > 0 && bo.verdict == UAM_FUZZ_OK, "begin-only: ok");
    uam_fuzz_destroy(f);
    uam_machine_fini(&m);

    /* Boot that never reaches begin. */
    machine_for(&m, t, BK_INTERP);
    uam_fuzz_config cfg = { .begin_pc = 0x12340, .boot_insns = 1000 };
    f = uam_fuzz_create(&m, &cfg, NULL, 0);
    fprintf(stderr, "(expected boot error follows)\n");
    CHECK(uam_fuzz_boot(f, &r) != UAM_OK && r.stop == UAM_STOP_INSN_LIMIT, "boot failure reported");
    uam_fuzz_destroy(f);
    uam_machine_fini(&m);
}

/* ---- routes ------------------------------------------------------------- */

typedef struct route_log {
    unsigned calls;
    size_t   len;
    bool     reset, abort;
    uam_status status;   /* returned after logging the call */
    uam_fuzz *f;
} route_log;

static uam_status log_route(void *ctx, uam_machine *m, const uint8_t *data, size_t len)
{
    route_log *l = ctx;
    l->calls++;
    l->len = len;
    if (l->reset)
        uam_exec_request_reset(&m->exec, UAM_RESET_SYSTEM);
    if (l->abort)
        uam_fuzz_abort(l->f);
    (void)data;
    return l->status;
}

static void test_routes(const target *t)
{
    uam_machine m;
    machine_for(&m, t, BK_INTERP);
    route_log mine = { 0 }, board = { 0 };
    CHECK(uam_machine_add_input(&m, "board0", log_route, &board) == UAM_OK, "machine input");
    CHECK(uam_machine_add_input(&m, "board0", log_route, &board) == UAM_ERR_INVALID, "unique names");
    uam_fuzz *f = harness_for(&m, t, NULL, 0, UAM_FUZZ_EXC_DEFAULT);
    CHECK(uam_fuzz_add_route(f, "mine", log_route, &mine) == UAM_OK, "route");
    char err[256];
    CHECK(uam_fuzz_select_route(f, "nope", err, sizeof(err)) == UAM_ERR_INVALID
          && strstr(err, "mine") && strstr(err, "board0"), "unknown route lists the known: %s", err);
    CHECK(uam_fuzz_select_route(f, "mem:zz", err, sizeof(err)) == UAM_ERR_INVALID, "bad mem spec");
    CHECK(uam_fuzz_select_route(f, "mem:0x8000,len=0x7ffc,max=8,bogus=1", err, sizeof(err))
          == UAM_ERR_INVALID, "unknown mem option");
    uam_fuzz_result r;
    uam_fuzz_boot(f, &r);

    CHECK(uam_fuzz_select_route(f, "mine", err, sizeof(err)) == UAM_OK, "select mine");
    uam_fuzz_run_one(f, (const uint8_t *)"xyz", 3, &r);
    CHECK(mine.calls == 1 && mine.len == 3, "own route delivers");
    CHECK(uam_fuzz_select_route(f, "board0", err, sizeof(err)) == UAM_OK, "select board0");
    uam_fuzz_run_one(f, (const uint8_t *)"xy", 2, &r);
    CHECK(board.calls == 1 && board.len == 2, "machine input delivers");

    /* A reset during an iteration is a crash. */
    board.reset = true;
    uam_fuzz_run_one(f, (const uint8_t *)"x", 1, &r);
    CHECK(r.verdict == UAM_FUZZ_CRASH_ORACLE && r.reset, "reset is reported (%s)",
          uam_fuzz_verdict_str(r.verdict));
    board.reset = false;
    uam_fuzz_run_one(f, (const uint8_t *)"x", 1, &r);
    EQ64(r.verdict, UAM_FUZZ_OK, "and the next input runs normally");

    /* mem: truncates to max. */
    CHECK(uam_fuzz_select_route(f, t->route, err, sizeof(err)) == UAM_OK, "select mem");
    uint8_t big[200];
    memset(big, 'A', sizeof(big));
    uam_fuzz_run_one(f, big, sizeof(big), &r);
    uint64_t len = 0;
    /* physical 0x7ffc on both targets, in the guest byte order */
    uam_bus_read(&m.bus, 0x7ffc, 4, (t->be ? UAM_ACC_BE : UAM_ACC_LE) | UAM_ACC_DEBUG, &len);
    EQ64(len, 64, "the length word is the truncated length");

    /* mem:...,reject refuses an input over max: nothing runs, the campaign goes on. */
    char spec[128];
    snprintf(spec, sizeof(spec), "%s,reject", t->route);
    CHECK(uam_fuzz_select_route(f, spec, err, sizeof(err)) == UAM_OK, "select mem reject: %s", err);
    const uam_fuzz_stats *st = uam_fuzz_get_stats(f);
    uint64_t rejected = st->by_verdict[UAM_FUZZ_REJECTED];
    CHECK(uam_fuzz_run_one(f, big, sizeof(big), &r) == UAM_OK, "a rejection is no harness error");
    CHECK(r.verdict == UAM_FUZZ_REJECTED && r.insns == 0, "over max: rejected, nothing ran (%s, %llu)",
          uam_fuzz_verdict_str(r.verdict), (unsigned long long)r.insns);
    EQ64(st->by_verdict[UAM_FUZZ_REJECTED], rejected + 1, "counted");
    CHECK(uam_fuzz_run_one(f, big, 64, &r) == UAM_OK && r.verdict == UAM_FUZZ_OK, "at max: runs");
    EQ64(uam_fuzz_exit_status(UAM_FUZZ_REJECTED), UAM_FUZZ_EXIT_REJECTED, "exit code");

    /* A route of its own says the same with UAM_ERR_REJECTED; other errors stay errors. */
    mine.status = UAM_ERR_REJECTED;
    CHECK(uam_fuzz_select_route(f, "mine", err, sizeof(err)) == UAM_OK, "select mine");
    CHECK(uam_fuzz_run_one(f, (const uint8_t *)"x", 1, &r) == UAM_OK && r.verdict == UAM_FUZZ_REJECTED,
          "a route's rejection");
    mine.status = UAM_ERR_INVALID;
    fprintf(stderr, "(expected delivery error follows)\n");
    CHECK(uam_fuzz_run_one(f, (const uint8_t *)"x", 1, &r) == UAM_ERR_INVALID,
          "any other delivery error stops the run");
    mine.status = UAM_OK;
    CHECK(uam_fuzz_run_one(f, (const uint8_t *)"x", 1, &r) == UAM_OK && r.verdict == UAM_FUZZ_OK,
          "and an accepted input runs");
    uam_fuzz_destroy(f);
    uam_machine_fini(&m);
}

/* ---- begin_skip: snapshot at a later arrival ----------------------------- */

static uint32_t reg32(const uam_machine *m, uint32_t off)
{
    uint32_t v;
    memcpy(&v, m->cpu.regfile + off, 4);
    return v;
}

static uam_fuzz *skip_harness(uam_machine *m, const target *t, uint32_t skip, uint64_t boot_insns)
{
    uam_fuzz_config cfg = { .begin_pc = t->begin, .iter_insns = 20000, .boot_insns = boot_insns,
                            .begin_skip = skip, .crash_pcs = &t->oracle, .n_crash_pcs = 1,
                            .map_size = MAP_SIZE, .crash_exc = UAM_FUZZ_EXC_DEFAULT };
    char err[256];
    uam_fuzz *f = uam_fuzz_create(m, &cfg, err, sizeof(err));
    CHECK(f != NULL, "create: %s", err);
    if (f) {
        CHECK(uam_fuzz_set_block_coverage(f) == UAM_OK, "coverage");
        CHECK(uam_fuzz_select_route(f, t->route, err, sizeof(err)) == UAM_OK, "route: %s", err);
    }
    return f;
}

/* ---- several begins ------------------------------------------------------ */

static uam_fuzz *begins_harness(uam_machine *m, const target *t, const uint64_t *more, size_t n_more,
                                const uint64_t *ends, size_t n_ends, bool nested, uint32_t skip)
{
    uam_fuzz_config cfg = { .begin_pc = t->begin, .more_begin_pcs = more, .n_more_begin_pcs = n_more,
                            .end_pcs = ends, .n_end_pcs = n_ends, .nested = nested,
                            .iter_insns = 20000, .boot_insns = 100000, .begin_skip = skip,
                            .crash_pcs = &t->oracle, .n_crash_pcs = 1, .map_size = MAP_SIZE,
                            .crash_exc = UAM_FUZZ_EXC_DEFAULT };
    char err[256];
    uam_fuzz *f = uam_fuzz_create(m, &cfg, err, sizeof(err));
    CHECK(f != NULL, "create: %s", err);
    if (f) {
        CHECK(uam_fuzz_set_block_coverage(f) == UAM_OK, "coverage");
        CHECK(uam_fuzz_select_route(f, t->route, err, sizeof(err)) == UAM_OK, "route: %s", err);
    }
    return f;
}

/* "count" is the instruction right before END (s0++): a second begin there ends a
 * begin-only iteration before the count; the boot loop is a begin the boot reaches
 * before BEGIN. */
static void test_begins(const target *t)
{
    const uint64_t count = t->end - 4, boot_loop = t->begin - (t->isa == &uam_isa_rv32 ? 8 : 12);
    for (int bk = BK_INTERP; bk <= BK_JIT; bk++) {
        uam_machine m;
        uam_fuzz_result r;
        /* The boot stops at the first begin it reaches, wherever that is. */
        machine_for(&m, t, (backend)bk);
        uam_fuzz *f = begins_harness(&m, t, &boot_loop, 1, NULL, 0, false, 0);
        CHECK(uam_fuzz_boot(f, &r) == UAM_OK && m.cpu.pc == boot_loop, "%s: snapshot at the first "
              "begin reached (pc 0x%llx)", t->name, (unsigned long long)m.cpu.pc);
        uam_fuzz_destroy(f);
        uam_machine_fini(&m);

        /* Begin-only: reaching another begin ends the iteration (before the count). */
        machine_for(&m, t, (backend)bk);
        f = begins_harness(&m, t, &count, 1, NULL, 0, false, 0);
        CHECK(uam_fuzz_boot(f, &r) == UAM_OK && m.cpu.pc == t->begin, "%s: snapshot at BEGIN", t->name);
        CHECK(uam_fuzz_run_one(f, (const uint8_t *)"AA", 2, &r) == UAM_OK && r.verdict == UAM_FUZZ_OK
              && r.pc == count, "%s: the second begin ends the iteration (pc 0x%llx)", t->name,
              (unsigned long long)r.pc);
        EQ64(reg32(&m, t->s0), 0, "stopped before the count");
        uam_fuzz_destroy(f);
        uam_machine_fini(&m);

        /* begin_skip counts arrivals at every begin: the boot loop's 5 passes, then BEGIN. */
        machine_for(&m, t, (backend)bk);
        f = begins_harness(&m, t, &boot_loop, 1, NULL, 0, false, 5);
        CHECK(uam_fuzz_boot(f, &r) == UAM_OK && m.cpu.pc == t->begin,
              "%s: five arrivals in the boot loop, the sixth at BEGIN (pc 0x%llx)", t->name,
              (unsigned long long)m.cpu.pc);
        uam_fuzz_destroy(f);
        uam_machine_fini(&m);

        /* Paired and nested: another begin opens a level, so END no longer closes the
         * iteration and it runs out of budget; not nested, END ends it. */
        for (int nested = 0; nested <= 1; nested++) {
            machine_for(&m, t, (backend)bk);
            f = begins_harness(&m, t, &count, 1, &t->end, 1, nested, 0);
            uam_fuzz_boot(f, &r);
            CHECK(uam_fuzz_run_one(f, (const uint8_t *)"AA", 2, &r) == UAM_OK, "run");
            EQ64(r.verdict, nested ? UAM_FUZZ_TIMEOUT : UAM_FUZZ_OK,
                 nested ? "nested: a begin inside the iteration opens a level"
                        : "not nested: END ends the iteration");
            uam_fuzz_destroy(f);
            uam_machine_fini(&m);
        }
    }
    /* An end may not be any begin. */
    uam_machine m;
    machine_for(&m, t, BK_INTERP);
    uam_fuzz_config cfg = { .begin_pc = t->begin, .more_begin_pcs = &count, .n_more_begin_pcs = 1,
                            .end_pcs = &count, .n_end_pcs = 1 };
    char err[128] = "";
    CHECK(uam_fuzz_create(&m, &cfg, err, sizeof(err)) == NULL && strstr(err, "begin"),
          "an end equal to another begin is refused (%s)", err);
    uam_machine_fini(&m);
}

/* The target's boot finds an empty input (count, END, back to BEGIN), so each skipped
 * arrival is one pass through the handler, counted in s0. */
static void test_begin_skip(const target *t)
{
    uint8_t init0[MAP_SIZE];
    for (uint32_t skip = 0; skip <= 3; skip++) {
        for (int bk = BK_INTERP; bk <= BK_JIT; bk++) {
            uam_machine m;
            machine_for(&m, t, (backend)bk);
            uam_fuzz *f = skip_harness(&m, t, skip, 100000);
            uam_fuzz_result r;
            CHECK(uam_fuzz_boot(f, &r) == UAM_OK, "%s: boot with begin_skip %u", t->name, skip);
            EQ64(reg32(&m, t->s0), skip, "the handler ran once per skipped arrival");
            EQ64(m.cpu.pc, t->begin, "snapshot at begin");
            size_t n;
            const uint8_t *map = uam_fuzz_map(f, &n);
            if (skip == 0)
                memcpy(init0, map, MAP_SIZE);
            else
                CHECK(nonzero(map) > nonzero(init0), "the skipped passes are in INIT");
            CHECK(uam_fuzz_run_one(f, (const uint8_t *)"AA", 2, &r) == UAM_OK && r.verdict == UAM_FUZZ_OK,
                  "inputs run from the later snapshot");
            EQ64(reg32(&m, t->s0), skip + 1, "from the state at that arrival");
            uam_fuzz_destroy(f);
            uam_machine_fini(&m);
        }
    }
    /* Not reached often enough within the boot budget: the boot fails. */
    uam_machine m;
    machine_for(&m, t, BK_INTERP);
    uam_fuzz *f = skip_harness(&m, t, 1000000, 5000);
    uam_fuzz_result r;
    fprintf(stderr, "(expected boot error follows)\n");
    CHECK(uam_fuzz_boot(f, &r) != UAM_OK, "too few arrivals fail the boot");
    uam_fuzz_destroy(f);
    uam_machine_fini(&m);

    /* Starting at the begin pc is one arrival, counted once (its hook does not fire again
     * when the hart starts). */
    machine_for(&m, t, BK_INTERP);
    f = skip_harness(&m, t, 0, 100000);
    CHECK(uam_fuzz_boot(f, &r) == UAM_OK, "first boot");
    uam_fuzz_destroy(f);
    f = skip_harness(&m, t, 1, 100000);
    CHECK(uam_fuzz_boot(f, &r) == UAM_OK, "boot from begin with begin_skip 1");
    EQ64(reg32(&m, t->s0), 1, "one more pass, not none");
    uam_fuzz_destroy(f);
    uam_machine_fini(&m);
}

/* ---- the frontend loop and replay ------------------------------------- */

typedef struct array_fe {
    const char *const *in;
    unsigned n, next, hellos, byes;
    uam_fuzz_info info;
    uint64_t ids[16];
    int kinds[16];
    uam_fuzz_verdict v[16];
    unsigned reports;
    route_log *abort_on;
} array_fe;

static uam_status fe_hello(void *ctx, const uam_fuzz_info *info)
{
    array_fe *a = ctx;
    a->hellos++;
    a->info = *info;
    return UAM_OK;
}
static int fe_next(void *ctx, uint8_t **data, size_t *len, uint64_t *id)
{
    array_fe *a = ctx;
    if (a->next >= a->n)
        return 0;
    *data = (uint8_t *)(uintptr_t)a->in[a->next];
    *len = strlen(a->in[a->next]);
    *id = 100 + a->next++;
    return 1;
}
static uam_status fe_report(void *ctx, uint64_t id, uam_fuzz_verdict v, uint64_t detail,
                            const uint8_t *map, size_t map_size, int kind)
{
    (void)detail; (void)map; (void)map_size;
    array_fe *a = ctx;
    if (a->reports < 16) {
        a->ids[a->reports] = id;
        a->kinds[a->reports] = kind;
        a->v[a->reports] = v;
    }
    a->reports++;
    return UAM_OK;
}
static void fe_bye(void *ctx) { ((array_fe *)ctx)->byes++; }

static const uam_fuzz_frontend array_frontend = { fe_hello, fe_next, fe_report, fe_bye };

static void test_frontend(const target *t)
{
    uam_machine m;
    machine_for(&m, t, BK_JIT);
    uam_fuzz *f = harness_for(&m, t, NULL, 0, UAM_FUZZ_EXC_DEFAULT);
    static const char *const in[3] = { "AA", "B", "AA" };
    array_fe a = { .in = in, .n = 3 };
    CHECK(uam_fuzz_run(f, &array_frontend, &a) == UAM_OK, "run");
    EQ64(a.hellos, 1, "hello once");
    EQ64(a.info.map_size, MAP_SIZE, "hello map size");
    CHECK(!strcmp(a.info.scheme, "blocks") && !strcmp(a.info.arch, t->isa->name), "hello scheme/arch");
    EQ64(a.reports, 4, "INIT + one per input");
    CHECK(a.ids[0] == 0 && a.kinds[0] == UAM_FUZZ_KIND_INIT, "INIT first, id 0");
    CHECK(a.ids[1] == 100 && a.ids[2] == 101 && a.ids[3] == 102 && a.kinds[3] == UAM_FUZZ_KIND_ITER,
          "ids echoed in order");
    CHECK(a.v[1] == UAM_FUZZ_OK && a.v[2] == UAM_FUZZ_CRASH_FAULT && a.v[3] == UAM_FUZZ_OK, "verdicts");
    EQ64(a.byes, 1, "bye once");
    uam_fuzz_destroy(f);
    uam_machine_fini(&m);

    /* Abort: a route that aborts stops the loop after that input. */
    machine_for(&m, t, BK_INTERP);
    f = harness_for(&m, t, NULL, 0, UAM_FUZZ_EXC_DEFAULT);
    route_log ab = { .abort = true, .f = f };
    uam_fuzz_add_route(f, "abort", log_route, &ab);
    uam_fuzz_select_route(f, "abort", NULL, 0);
    a = (array_fe){ .in = in, .n = 3 };
    CHECK(uam_fuzz_run(f, &array_frontend, &a) == UAM_OK && uam_fuzz_aborted(f), "aborted run");
    EQ64(a.reports, 1, "only INIT reported");
    EQ64(a.byes, 1, "bye still called");
    uam_fuzz_destroy(f);
    uam_machine_fini(&m);
}

static void write_file(const char *dir, const char *name, const char *data)
{
    char p[512];
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    FILE *fp = fopen(p, "wb");
    CHECK(fp != NULL, "create %s", p);
    if (fp) {
        fwrite(data, 1, strlen(data), fp);
        fclose(fp);
    }
}

static void test_replay(const target *t)
{
    char dir[] = "/tmp/uam-replay-XXXXXX";
    CHECK(mkdtemp(dir) != NULL, "mkdtemp");
    write_file(dir, "1-ok", "AA");
    write_file(dir, "2-oracle", "O");
    write_file(dir, "3-crash", "B");

    char buf[2048] = "";
    for (int pass = 0; pass < 2; pass++) {
        uam_machine m;
        machine_for(&m, t, pass ? BK_JIT : BK_INTERP);
        uam_fuzz *f = harness_for(&m, t, NULL, 0, UAM_FUZZ_EXC_DEFAULT);
        FILE *out = fmemopen(buf, sizeof(buf), "w");
        uam_fuzz_replay *r = uam_fuzz_replay_create(dir, f, out);
        CHECK(r && uam_fuzz_replay_count(r) == 3, "three files");
        CHECK(uam_fuzz_run(f, &uam_fuzz_replay_frontend, r) == UAM_OK, "replay");
        fclose(out);
        EQ64(uam_fuzz_replay_exit_status(r), UAM_FUZZ_EXIT_CRASH_ORACLE,
             "exit status: the first crash (an ok input after it does not clear it)");
        char want[64];
        snprintf(want, sizeof(want), "3-crash: crash-fault pc=0x%llx", (unsigned long long)t->fault_pc);
        CHECK(strstr(buf, "1-ok: ok") && strstr(buf, "2-oracle: crash-oracle") && strstr(buf, want),
              "%s: replay output:\n%s", t->name, buf);
        size_t sz;
        CHECK(uam_fuzz_replay_last_map(r, &sz) && sz == MAP_SIZE, "last map kept");
        uam_fuzz_replay_destroy(r);

        uam_fuzz_destroy(f);
        uam_machine_fini(&m);

        /* One file. */
        char p[512];
        snprintf(p, sizeof(p), "%s/2-oracle", dir);
        machine_for(&m, t, BK_INTERP);
        f = harness_for(&m, t, NULL, 0, UAM_FUZZ_EXC_DEFAULT);
        out = fmemopen(buf, sizeof(buf), "w");
        r = uam_fuzz_replay_create(p, f, out);
        CHECK(uam_fuzz_run(f, &uam_fuzz_replay_frontend, r) == UAM_OK, "replay one");
        fclose(out);
        EQ64(uam_fuzz_replay_exit_status(r), UAM_FUZZ_EXIT_CRASH_ORACLE, "single file status");
        uam_fuzz_replay_destroy(r);
        uam_fuzz_destroy(f);
        uam_machine_fini(&m);
    }
    fprintf(stderr, "(expected replay error follows)\n");
    CHECK(uam_fuzz_replay_create("/nonexistent/x", NULL, stdout) == NULL, "missing path");
    char p[512];
    const char *names[3] = { "1-ok", "2-oracle", "3-crash" };
    for (int i = 0; i < 3; i++) {
        snprintf(p, sizeof(p), "%s/%s", dir, names[i]);
        unlink(p);
    }
    rmdir(dir);
}

/* A fuzzer's notes beside its inputs (X.info next to crash X) are skipped when their
 * suffix is named, and only when X is there too. */
static void test_replay_sidecars(void)
{
    char dir[] = "/tmp/uam-replay-XXXXXX";
    CHECK(mkdtemp(dir) != NULL, "mkdtemp");
    const char *names[] = { "crash_1", "crash_1.info", "lone.info", "x.notes", "x" };
    for (size_t i = 0; i < UAM_ARRAY_LEN(names); i++)
        write_file(dir, names[i], "A");
    uam_fuzz_replay *r = uam_fuzz_replay_create(dir, NULL, stdout);
    CHECK(r && uam_fuzz_replay_count(r) == 5, "no suffixes: every file runs");
    uam_fuzz_replay_destroy(r);
    static const char *const one[] = { ".info" };
    r = uam_fuzz_replay_create_ex(dir, NULL, stdout, one, 1);
    CHECK(r && uam_fuzz_replay_count(r) == 4, "crash_1.info skipped, lone.info (no lone) kept");
    uam_fuzz_replay_destroy(r);
    static const char *const two[] = { ".info", ".notes" };
    r = uam_fuzz_replay_create_ex(dir, NULL, stdout, two, 2);
    CHECK(r && uam_fuzz_replay_count(r) == 3, "both kinds of sidecar skipped");
    uam_fuzz_replay_destroy(r);
    char p[512];
    snprintf(p, sizeof(p), "%s/crash_1.info", dir);
    r = uam_fuzz_replay_create_ex(p, NULL, stdout, one, 1);
    CHECK(r && uam_fuzz_replay_count(r) == 1, "a FILE path always runs");
    uam_fuzz_replay_destroy(r);
    for (size_t i = 0; i < UAM_ARRAY_LEN(names); i++) {
        snprintf(p, sizeof(p), "%s/%s", dir, names[i]);
        unlink(p);
    }
    rmdir(dir);
}

static void test_misc(void)
{
    unsigned c;
    CHECK(uam_fuzz_parse_exc_classes("fetch,trap", &c) && c == (UAM_FUZZ_EXC_FETCH | UAM_FUZZ_EXC_TRAP),
          "parse classes");
    CHECK(uam_fuzz_parse_exc_classes("all", &c) && c == UAM_FUZZ_EXC_ALL, "all");
    CHECK(uam_fuzz_parse_exc_classes("none", &c) && c == 0, "none");
    CHECK(!uam_fuzz_parse_exc_classes("fetch,bogus", &c), "unknown class");
    EQ64(uam_fuzz_exit_status(UAM_FUZZ_HANG), UAM_FUZZ_EXIT_HANG, "exit codes");
    uam_machine m;
    CHECK(uam_machine_init(&m, &uam_isa_rv32) == UAM_OK, "init");
    uam_fuzz_config cfg = { .begin_pc = 0x100, .map_size = 1000 };
    CHECK(uam_fuzz_create(&m, &cfg, NULL, 0) == NULL, "map size must be a power of two");
    const uint64_t ends[2] = { 0x200, 0x100 };
    cfg = (uam_fuzz_config){ .begin_pc = 0x100, .end_pcs = ends, .n_end_pcs = 2 };
    CHECK(uam_fuzz_create(&m, &cfg, NULL, 0) == NULL, "no end may equal the begin");
    uam_machine_fini(&m);
}

/* ---- crashing interrupts ------------------------------------------------- */

typedef struct irq_route {
    unsigned line;
    unsigned count;   /* interrupts the plain observer saw */
} irq_route;

/* Delivering the input asserts an interrupt line, so it is taken as the iteration starts. */
static uam_status raise_irq(void *ctx, uam_machine *m, const uint8_t *data, size_t len)
{
    (void)data; (void)len;
    uam_cpu_irq_sink(&m->cpu, ((irq_route *)ctx)->line, 1);
    return UAM_OK;
}

static uam_hook_action count_irq(void *ctx, uam_cpu *cpu, const uam_interrupt *irq)
{
    (void)cpu; (void)irq;
    ((irq_route *)ctx)->count++;
    return UAM_HOOK_CONTINUE;
}

/* Interrupts on, a handler for them (its first word is a spin, so it is visible as the
 * handler pc). Returns the wired line and the handler address. */
static void enable_irqs(uam_machine *m, const target *t, unsigned *line, uint64_t *handler)
{
    if (t->isa == &uam_isa_rv32) {
        *line = UAM_RV_IRQ_MEI;
        *handler = 0x1800;
        m->cpu.isa->sysreg_write(&m->cpu, 0x305, *handler);              /* mtvec */
        m->cpu.isa->sysreg_write(&m->cpu, 0x304, 1u << UAM_RV_IRQ_MEI);  /* mie.MEIE */
        m->cpu.isa->sysreg_write(&m->cpu, 0x300, 1u << 3);               /* mstatus.MIE */
        const uint8_t spin[4] = { 0x6f, 0, 0, 0 };                       /* jal x0, 0 */
        uam_bus_load_bytes(&m->bus, 0x1800, spin, 4);
    } else {
        *line = UAM_MIPS_IRQ_HW0;
        *handler = 0x80000180u;                                          /* BEV=0, EBase 0x80000000 */
        m->cpu.isa->sysreg_write(&m->cpu, 12 * 8, 1u | (1u << (8 + UAM_MIPS_IRQ_HW0))); /* Status: IE, IM2 */
        const uint8_t spin[8] = { 0x10, 0x00, 0xff, 0xff, 0, 0, 0, 0 };   /* b . ; nop (big-endian) */
        uint8_t w[8];
        for (int i = 0; i < 8; i++)
            w[i] = t->be ? spin[i] : spin[(i & ~3) + 3 - (i & 3)];
        uam_bus_load_bytes(&m->bus, 0x180, w, 8);
    }
}

static void test_irq(const target *t)
{
    for (int bk = 0; bk < 3; bk++) {
        uam_machine m;
        machine_for(&m, t, (backend)bk);
        unsigned line;
        uint64_t handler;
        enable_irqs(&m, t, &line, &handler);
        irq_route ir;
        /* A crashing line. */
        uam_fuzz_config cfg = { .begin_pc = t->begin, .iter_insns = 2000, .boot_insns = 100000,
                                .crash_irq_lines = UINT64_C(1) << line };
        uam_fuzz *f = uam_fuzz_create(&m, &cfg, NULL, 0);
        ir = (irq_route){ .line = line };
        uam_fuzz_add_route(f, "irq", raise_irq, &ir);
        uam_fuzz_select_route(f, "irq", NULL, 0);
        uam_fuzz_result r;
        CHECK(uam_fuzz_boot(f, &r) == UAM_OK, "irq: boot");
        uam_fuzz_run_one(f, NULL, 0, &r);
        CHECK(r.verdict == UAM_FUZZ_CRASH_ORACLE && r.irq_valid && r.detail == (UINT64_C(1) << line),
              "%s/%s: an interrupt from a crash line is a crash (%s, detail 0x%llx)", t->name,
              bk_name[bk], uam_fuzz_verdict_str(r.verdict), (unsigned long long)r.detail);
        CHECK(r.irq.from_pc == t->begin && r.irq.handler_pc == handler && (r.irq.lines >> line & 1),
              "%s/%s: interrupted at begin, handler 0x%llx, lines 0x%llx", t->name, bk_name[bk],
              (unsigned long long)r.irq.handler_pc, (unsigned long long)r.irq.lines);
        uam_fuzz_destroy(f);
        CHECK(m.exec.irq_hook == NULL, "destroy removes the interrupt observer");
        uam_machine_fini(&m);   /* it stopped in the handler: boot a fresh one */

        /* Another line is not: the interrupt is delivered and the handler spins. */
        machine_for(&m, t, (backend)bk);
        enable_irqs(&m, t, &line, &handler);
        cfg.crash_irq_lines = UINT64_C(1) << (line + 1);
        f = uam_fuzz_create(&m, &cfg, NULL, 0);
        uam_fuzz_add_route(f, "irq", raise_irq, &ir);
        uam_fuzz_select_route(f, "irq", NULL, 0);
        uam_fuzz_boot(f, &r);
        uam_fuzz_run_one(f, NULL, 0, &r);
        CHECK(r.verdict == UAM_FUZZ_TIMEOUT && !r.irq_valid && r.pc == handler,
              "%s/%s: another line's interrupt is delivered (%s at 0x%llx)", t->name, bk_name[bk],
              uam_fuzz_verdict_str(r.verdict), (unsigned long long)r.pc);
        uam_fuzz_destroy(f);
        uam_machine_fini(&m);

        /* The executor's observer on its own, continuing. */
        machine_for(&m, t, (backend)bk);
        enable_irqs(&m, t, &line, &handler);
        ir.count = 0;
        uam_exec_set_irq_hook(&m.exec, count_irq, &ir);
        uam_cpu_irq_sink(&m.cpu, line, 1);
        uam_exec_run(&m.exec, 100);
        CHECK(ir.count >= 1 && m.exec.stats.interrupts >= 1, "%s/%s: the observer sees interrupts",
              t->name, bk_name[bk]);
        uam_exec_set_irq_hook(&m.exec, NULL, NULL);
        uam_machine_fini(&m);
    }
}

/* ---- nested boundaries (recursion) ------------------------------------- */

static uam_status no_input(void *ctx, uam_machine *m, const uint8_t *data, size_t len)
{
    (void)ctx; (void)m; (void)data; (void)len;
    return UAM_OK;
}

/*
 * RV32: BEGIN is arrived at three times before END (as a function recursing three
 * deep would), then END three times:
 *   0x1000  addi t0, x0, 3
 *   0x1004  BEGIN: addi s0, s0, 1
 *   0x1008  blt  s0, t0, BEGIN
 *   0x100c  END:   addi s1, s1, 1
 *   0x1010  blt  s1, t0, END
 *   0x1014  jal  x0, 0
 */
static void run_nested(bool nested, const uint64_t *ends, size_t n_ends, uam_fuzz_result *r,
                       uint32_t *s1)
{
    enum { T0 = 5, S0 = 8, S1 = 9 };
    static const uint32_t code[6] = {
        0x00300293u,                    /* addi t0, x0, 3 */
        0x00140413u,                    /* addi s0, s0, 1 */
        0xfe544ee3u,                    /* blt  s0, t0, -4 */
        0x00148493u,                    /* addi s1, s1, 1 */
        0xfe54cee3u,                    /* blt  s1, t0, -4 */
        0x0000006fu,                    /* jal  x0, 0 */
    };
    uam_machine m;
    CHECK(uam_machine_init(&m, &uam_isa_rv32) == UAM_OK, "machine");
    uam_bus_map_ram(&m.bus, "ram", 0, 1 << 16);
    for (unsigned i = 0; i < 6; i++) {
        uint8_t b[4] = { (uint8_t)code[i], (uint8_t)(code[i] >> 8), (uint8_t)(code[i] >> 16),
                         (uint8_t)(code[i] >> 24) };
        uam_bus_load_bytes(&m.bus, 0x1000 + 4 * i, b, 4);
    }
    uam_machine_reset(&m, 0x1000);
    uam_fuzz_config cfg = { .begin_pc = 0x1004, .end_pcs = ends, .n_end_pcs = n_ends,
                            .nested = nested, .iter_insns = 1000 };
    uam_fuzz *f = uam_fuzz_create(&m, &cfg, NULL, 0);
    uam_fuzz_add_route(f, "none", no_input, NULL);
    uam_fuzz_select_route(f, "none", NULL, 0);
    CHECK(uam_fuzz_boot(f, r) == UAM_OK, "nested: boot");
    CHECK(uam_fuzz_run_one(f, NULL, 0, r) == UAM_OK, "nested: run");
    *s1 = (uint32_t)uam_cpu_reg_read(&m.cpu, S1 * 4, 32);
    (void)S0;
    uam_fuzz_destroy(f);
    uam_machine_fini(&m);
}

static void test_nested(void)
{
    const uint64_t end = 0x100c, ends_dup[2] = { 0x100c, 0x100c };
    uam_fuzz_result r;
    uint32_t s1;
    /* Nested: three BEGIN arrivals open three levels; the third END closes the last. */
    run_nested(true, &end, 1, &r, &s1);
    CHECK(r.verdict == UAM_FUZZ_OK && r.pc == 0x100c && s1 == 2,
          "nested: ends at the END that balances every BEGIN (pc 0x%llx, %u ENDs passed)",
          (unsigned long long)r.pc, s1);
    /* A duplicated end pc still closes one level per arrival. */
    run_nested(true, ends_dup, 2, &r, &s1);
    CHECK(r.verdict == UAM_FUZZ_OK && s1 == 2, "nested: a duplicate end pc closes one level");
    /* Not nested: the first END ends the iteration. */
    run_nested(false, &end, 1, &r, &s1);
    CHECK(r.verdict == UAM_FUZZ_OK && r.pc == 0x100c && s1 == 0, "not nested: the first END ends it");
}

/*
 * Coverage sites at the boundaries are counted by the hooks, in the map they close
 * (suzzer's FORMATS.md 5.7): the begin's hit goes to INIT (and, in begin-only mode,
 * to the iteration that ends back at begin), an end's to the iteration it ends, and an
 * iteration's first count is the site after the begin with an empty history. B values
 * are non-zero so that a begin counted inside the iteration would move the next bucket.
 */
static void test_markers(const target *t)
{
    const uam_cov_site sites[4] = {
        { t->base, 4, 0x40 },        /* boot */
        { t->begin, 1, 0x50 },       /* BEGIN */
        { t->begin + 4, 2, 0x60 },   /* the first site of an iteration */
        { t->end, 3, 0x70 },         /* END (paired), or an ordinary site (begin-only) */
    };
    for (int bk = 0; bk < 3; bk++) {
        for (int paired = 0; paired < 2; paired++) {
            for (int skip = 0; skip < 2; skip++) {
                uam_machine m;
                machine_for(&m, t, (backend)bk);
                uam_fuzz_config cfg = { .begin_pc = t->begin, .end_pcs = &t->end,
                                        .n_end_pcs = paired ? 1 : 0, .iter_insns = 20000,
                                        .boot_insns = 100000, .map_size = MAP_SIZE,
                                        .skip_repeats = skip };
                char err[256];
                uam_fuzz *f = uam_fuzz_create(&m, &cfg, err, sizeof(err));
                CHECK(f != NULL, "create: %s", err);
                CHECK(uam_fuzz_set_sites(f, sites, 4, "test", err, sizeof(err)) == UAM_OK,
                      "sites: %s", err);
                CHECK(uam_fuzz_select_route(f, t->route, err, sizeof(err)) == UAM_OK, "route");
                uam_fuzz_result r;
                CHECK(uam_fuzz_boot(f, &r) == UAM_OK, "boot");
                const uint8_t *map = uam_fuzz_map(f, NULL);
                const char *what = paired ? "paired" : "begin-only";
                EQ64(map[4], 1, "INIT: the boot site");
                EQ64(map[1 ^ 0x40], 1, "INIT: the begin's hit, after the boot site");
                EQ64(nonzero(map), 2, "INIT: nothing else");
                for (int it = 0; it < 2; it++) {
                    CHECK(uam_fuzz_run_one(f, (const uint8_t *)"AA", 2, &r) == UAM_OK, "run");
                    EQ64(r.verdict, UAM_FUZZ_OK, "ok");
                    CHECK(map[2] == 1, "%s/%s/%s: the iteration's first count has an empty "
                          "history (map[2] = %u)", t->name, bk_name[bk], what, map[2]);
                    EQ64(map[3 ^ 0x60], 1, "the end site follows it");
                    if (paired) {
                        EQ64(nonzero(map), 2, "paired: the begin is not counted in the iteration");
                    } else {
                        EQ64(map[1 ^ 0x70], 1, "begin-only: the begin's hit closes the iteration");
                        EQ64(nonzero(map), 3, "begin-only: nothing else");
                    }
                }
                uam_fuzz_destroy(f);
                uam_machine_fini(&m);
            }
        }
    }
}

/* An internal error (here a site the pass cannot instrument, a MIPS delay slot) is the
 * harness's error, not a verdict: it would recur on every input. */
static void test_internal_error(const target *t)
{
    const uam_cov_site site = { t->begin + 24, 1, 0 };   /* b+6: the first beq's delay slot */
    uam_machine m;
    machine_for(&m, t, BK_INTERP);
    uam_fuzz_config cfg = { .begin_pc = t->begin, .end_pcs = &t->end, .n_end_pcs = 1,
                            .iter_insns = 20000, .boot_insns = 100000, .map_size = MAP_SIZE };
    char err[256];
    uam_fuzz *f = uam_fuzz_create(&m, &cfg, err, sizeof(err));
    CHECK(f && uam_fuzz_set_sites(f, &site, 1, "test", err, sizeof(err)) == UAM_OK, "sites: %s", err);
    CHECK(uam_fuzz_select_route(f, t->route, err, sizeof(err)) == UAM_OK, "route");
    uam_fuzz_result r;
    CHECK(uam_fuzz_boot(f, &r) == UAM_OK, "boot");
    fprintf(stderr, "(expected coverage errors follow)\n");
    CHECK(uam_fuzz_run_one(f, (const uint8_t *)"AA", 2, &r) != UAM_OK,
          "%s: an internal error fails the iteration instead of being a crash", t->name);
    EQ64(r.stop, UAM_STOP_ERROR, "the stop reason says so");
    EQ64(uam_fuzz_get_stats(f)->iterations, 0, "and it is not counted as a result");
    uam_fuzz_destroy(f);
    uam_machine_fini(&m);
}

int main(void)
{
    test_misc();
    test_nested();
    const target ts[3] = { rv_target(), mips_target(true), mips_target(false) };
    for (int i = 0; i < 3; i++) {
        test_verdicts(&ts[i]);
        test_modes(&ts[i]);
        test_routes(&ts[i]);
        test_frontend(&ts[i]);
        test_replay(&ts[i]);
        test_irq(&ts[i]);
        test_markers(&ts[i]);
    }
    test_internal_error(&ts[1]);
    test_replay_sidecars();
    for (size_t i = 0; i < UAM_ARRAY_LEN(ts); i++) {
        test_begin_skip(&ts[i]);
        test_begins(&ts[i]);
    }
    printf("test_fuzz: %d/%d checks passed\n", n_checks - n_failed, n_checks);
    return n_failed ? 1 : 0;
}
