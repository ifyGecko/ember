/*
 * test_smp.c - Observers on a multiprocessor machine (uam_machine_add_cpus).
 *
 * Two RV32 harts run different loops (they branch on mhartid). The machine-level
 * observers (machine.h) must see both: PC hooks report the hart that arrived, the
 * exception observer the hart that faulted, and coverage counts each hart in its own
 * history - so a site map does not depend on where the slices end, and comes out the
 * same on every backend. The fuzz harness reports the hart that ended an iteration.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uam/fuzz.h"
#include "uam/isa/riscv.h"
#include "uam/machine.h"

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

#define BASE      0x1000u
#define HART0_PC  0x100Cu   /* hart 0's loop */
#define HART0_J   0x101Cu   /* its jump back, every fourth round */
#define HART1_PC  0x1020u   /* hart 1's loop */
#define FAULT_PC  0x1034u   /* hart 1's load from an unmapped address, after 2000 rounds */
#define MAP_SIZE  4096u

static const uint32_t prog[] = {
    0xF14022F3u, /* csrr t0, mhartid */
    0x000104B7u, /* lui  s1, 0x10            shared data at 0x10000 */
    0x00029C63u, /* bnez t0, 0x1020 */
    0x00140413u, /* addi s0, s0, 1           hart 0 (0x100c) */
    0x0084A023u, /* sw   s0, 0(s1) */
    0x00347313u, /* andi t1, s0, 3 */
    0xFE031AE3u, /* bnez t1, 0x100c */
    0xFF1FF06Fu, /* j    0x100c              (0x101c) */
    0x00140413u, /* addi s0, s0, 1           hart 1 (0x1020) */
    0x0084A223u, /* sw   s0, 4(s1) */
    0x7D000393u, /* addi t2, zero, 2000 */
    0xFE741AE3u, /* bne  s0, t2, 0x1020 */
    0x00F00E37u, /* lui  t3, 0xf00 */
    0x000E2E03u, /* lw   t3, 0(t3)           unmapped: load fault (0x1034) */
    0xFE9FF06Fu, /* j    0x1020 */
};

typedef enum { INTERP, JIT, JIT_NOCHAIN } backend;
static const char *const bk_name[] = { "interp", "jit", "jit-nochain" };

static void build(uam_machine *m, backend bk, uint32_t quantum)
{
    CHECK(uam_machine_init(m, &uam_isa_rv32) == UAM_OK, "machine init");
    m->board_name = "smp-test";
    CHECK(uam_bus_map_ram(&m->bus, "ram", 0, 1u << 20) == UAM_OK, "map ram");
    CHECK(uam_machine_add_cpus(m, 1) == UAM_OK, "second hart");
    const uam_region *ram = uam_bus_region(&m->bus, "ram");
    for (size_t i = 0; i < UAM_ARRAY_LEN(prog); i++)
        memcpy(ram->host + BASE + 4 * i, &prog[i], 4);
    uam_machine_set_quantum(m, quantum);
    uam_machine_reset(m, BASE);
    if (bk != INTERP)
        CHECK(uam_exec_enable_jit(&m->exec), "jit available");
    m->exec.chain = bk != JIT_NOCHAIN;
}

/* ---- PC hooks and the exception observer ------------------------------- */

typedef struct hits {
    uint64_t per_hart[2];
    unsigned stop_hart, stop_after;   /* stop at that hart's stop_after-th arrival */
} hits;

static uam_hook_action count_hook(void *ctx, uam_cpu *cpu, uint64_t pc)
{
    (void)pc;
    hits *h = ctx;
    if (cpu->hartid < 2)
        h->per_hart[cpu->hartid]++;
    return h->stop_after && cpu->hartid == h->stop_hart && h->per_hart[cpu->hartid] == h->stop_after
         ? UAM_HOOK_STOP : UAM_HOOK_CONTINUE;
}

typedef struct exc_seen {
    unsigned n, hart;
    uam_exception e;
} exc_seen;

static uam_hook_action exc_fn(void *ctx, uam_cpu *cpu, const uam_exception *e)
{
    exc_seen *s = ctx;
    if (s->n++ == 0) {
        s->hart = cpu->hartid;
        s->e = *e;
    }
    return UAM_HOOK_STOP;
}

static void test_hooks(backend bk)
{
    const char *b = bk_name[bk];
    uam_machine m;
    build(&m, bk, 300);

    /* A machine hook sees both harts; an executor hook only its own. */
    hits both = { 0 }, own = { 0 };
    CHECK(uam_machine_add_pc_hook(&m, BASE + 4, count_hook, &both) == UAM_OK, "%s: hook", b);
    CHECK(uam_exec_add_pc_hook(&m.exec, BASE + 4, count_hook, &own) == UAM_OK, "%s: hook", b);
    uam_exec_run(&m.exec, 2000);
    CHECK(both.per_hart[0] == 1 && both.per_hart[1] == 1, "%s: machine hook on both harts "
          "(%llu, %llu)", b, (unsigned long long)both.per_hart[0], (unsigned long long)both.per_hart[1]);
    CHECK(own.per_hart[0] == 1 && own.per_hart[1] == 0, "%s: executor hook on hart 0 only", b);

    /* A hook on hart 1 stops the run, with hart 1 at the hooked pc. */
    hits stop = { .stop_hart = 1, .stop_after = 100 };
    uam_machine_add_pc_hook(&m, HART1_PC, count_hook, &stop);
    CHECK(uam_exec_run(&m.exec, 0) == UAM_STOP_HOOK, "%s: hart 1's hook stopped the run", b);
    CHECK(stop.per_hart[1] == 100 && uam_machine_cpu(&m, 1)->pc == HART1_PC,
          "%s: hart 1 stopped at its hook", b);
    CHECK(uam_machine_remove_pc_hook(&m, HART1_PC, count_hook, &stop), "%s: remove", b);
    CHECK(!uam_machine_remove_pc_hook(&m, HART1_PC, count_hook, &stop), "%s: removed once", b);

    /* Hart 1's load fault reaches the exception observer, attributed to hart 1. */
    exc_seen ex = { 0 };
    uam_machine_set_exc_hook(&m, exc_fn, &ex);
    CHECK(uam_exec_run(&m.exec, 0) == UAM_STOP_EXC_HOOK, "%s: exception observer stopped", b);
    CHECK(ex.n == 1 && ex.hart == 1 && ex.e.pc == FAULT_PC && ex.e.kind == UAM_EXC_LOAD_FAULT,
          "%s: hart 1's load fault at 0x%llx", b, (unsigned long long)ex.e.pc);
    uam_machine_set_exc_hook(&m, NULL, NULL);
    CHECK(m.exec.exc_hook == NULL && uam_machine_exec(&m, 1)->exc_hook == NULL, "%s: removed", b);

    uam_exec_stats st;
    uam_machine_get_stats(&m, &st);
    CHECK(st.blocks_executed > m.exec.stats.blocks_executed, "%s: stats add up both harts", b);
    CHECK(uam_machine_icount(&m) == m.cpu.icount + uam_machine_cpu(&m, 1)->icount, "%s: icount", b);
    uam_machine_fini(&m);
}

/* ---- coverage ------------------------------------------------------------ */

/* A site at every instruction of both loops. */
static void sites(uam_cov_site *s, size_t *n)
{
    *n = 0;
    for (uint32_t pc = HART0_PC; pc <= FAULT_PC; pc += 4) {
        uint32_t a = (pc * 2654435761u) >> 20;
        s[(*n)++] = (uam_cov_site){ .pc = pc, .a = a & (MAP_SIZE - 1), .b = (a >> 1) & (MAP_SIZE - 1) };
    }
}

/* The map after `insns` instructions of both harts, with hart 1 short of its fault. */
static void cov_map(backend bk, uint32_t quantum, uint8_t *map, uam_cov_state **st1)
{
    uam_machine m;
    build(&m, bk, quantum);
    memset(map, 0, MAP_SIZE);
    uam_cov_state cs;
    uam_cov_state_init(&cs, map, MAP_SIZE, NULL);
    uam_cov_site s[32];
    size_t n;
    sites(s, &n);
    char err[128];
    CHECK(uam_machine_set_coverage(&m, s, n, &cs, err, sizeof(err)) == UAM_OK, "coverage: %s", err);
    CHECK(uam_machine_cov_state(&m, 0) == &cs, "hart 0 counts in the caller's state");
    uam_cov_state *c1 = uam_machine_cov_state(&m, 1);
    CHECK(c1 && c1 != &cs && c1->map == map, "hart 1 has its own state on the same map");
    /* hart 0 and hart 1 run exactly 6000 instructions each */
    uam_exec_run(&m.exec, 12000);
    CHECK(m.cpu.icount + uam_machine_cpu(&m, 1)->icount == 12000, "%s: ran 12000", bk_name[bk]);
    if (st1)
        *st1 = c1;
    uam_machine_set_coverage(&m, NULL, 0, NULL, NULL, 0);
    CHECK(!uam_machine_cov_state(&m, 0) && !uam_machine_cov_state(&m, 1), "coverage off");
    uam_machine_fini(&m);
}

static void test_coverage(void)
{
    static uint8_t ref[MAP_SIZE], map[MAP_SIZE];
    cov_map(INTERP, 4096, ref, NULL);
    unsigned hit = 0;
    for (unsigned i = 0; i < MAP_SIZE; i++)
        hit += ref[i] != 0;
    CHECK(hit >= 8, "both loops covered (%u buckets)", hit);
    for (int bk = INTERP; bk <= JIT_NOCHAIN; bk++) {
        static const uint32_t quanta[] = { 4096, 1000, 37, 1 };
        for (size_t q = 0; q < UAM_ARRAY_LEN(quanta); q++) {
            cov_map((backend)bk, quanta[q], map, NULL);
            /* each hart runs 6000 instructions whatever the slicing: its own history
             * makes its edges independent of the other's */
            CHECK(!memcmp(map, ref, MAP_SIZE), "%s, quantum %u: same map", bk_name[bk],
                  (unsigned)quanta[q]);
        }
    }
}

/* ---- the fuzz harness ------------------------------------------------------ */

static uam_status no_input(void *ctx, uam_machine *m, const uint8_t *data, size_t len)
{
    (void)ctx; (void)m; (void)data; (void)len;
    return UAM_OK;
}

static void test_fuzz(backend bk)
{
    const char *b = bk_name[bk];
    uam_machine m;
    build(&m, bk, 500);
    static const uint64_t never[] = { 0xFFF0 };
    static const uint64_t crash[] = { FAULT_PC };
    uam_fuzz_config cfg = { .begin_pc = HART0_PC, .end_pcs = never, .n_end_pcs = 1,
                            .crash_pcs = crash, .n_crash_pcs = 1,
                            .crash_exc = UAM_FUZZ_EXC_DEFAULT, .map_size = MAP_SIZE };
    char err[128];
    uam_fuzz *f = uam_fuzz_create(&m, &cfg, err, sizeof(err));
    CHECK(f, "%s: fuzz create: %s", b, err);
    if (!f) {
        uam_machine_fini(&m);
        return;
    }
    uam_cov_site s[32];
    size_t n;
    sites(s, &n);
    CHECK(uam_fuzz_set_sites(f, s, n, "test", err, sizeof(err)) == UAM_OK, "%s: sites", b);
    CHECK(uam_fuzz_add_route(f, "none", no_input, NULL) == UAM_OK &&
          uam_fuzz_select_route(f, "none", err, sizeof(err)) == UAM_OK, "%s: route", b);
    uam_fuzz_result r;
    CHECK(uam_fuzz_boot(f, &r) == UAM_OK && r.pc == HART0_PC, "%s: booted at hart 0's begin", b);
    uint8_t first[MAP_SIZE];
    for (int i = 0; i < 3; i++) {
        CHECK(uam_fuzz_run_one(f, (const uint8_t *)"", 0, &r) == UAM_OK, "%s: run %d", b, i);
        /* hart 1 reaches the crash pc: its pc is reported, the insns are both harts' */
        CHECK(r.verdict == UAM_FUZZ_CRASH_ORACLE && r.detail == FAULT_PC && r.pc == FAULT_PC,
              "%s: run %d: hart 1's crash pc (%s, pc 0x%llx)", b, i,
              uam_fuzz_verdict_str(r.verdict), (unsigned long long)r.pc);
        CHECK(r.insns > 2 * 2000 * 4, "%s: insns count both harts (%llu)", b,
              (unsigned long long)r.insns);
        size_t sz;
        const uint8_t *mp = uam_fuzz_map(f, &sz);
        if (i == 0)
            memcpy(first, mp, MAP_SIZE);
        else
            CHECK(!memcmp(first, mp, MAP_SIZE), "%s: run %d: same map every iteration", b, i);
    }
    uam_fuzz_destroy(f);
    CHECK(m.exec.n_hooks == 0 && uam_machine_exec(&m, 1)->n_hooks == 0, "%s: hooks removed", b);
    uam_machine_fini(&m);
}

int main(void)
{
    for (int bk = INTERP; bk <= JIT; bk++) {
        test_hooks((backend)bk);
        test_fuzz((backend)bk);
    }
    test_coverage();
    printf("test_smp: %d/%d checks passed\n", n_checks - n_failed, n_checks);
    return n_failed ? 1 : 0;
}
