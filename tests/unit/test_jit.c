/*
 * test_jit.c - JIT block chaining (src/exec/jit.h): chained execution must be
 * indistinguishable from returning to the run loop after every block.
 *
 * Each scenario runs three ways - the interpreter, the JIT with chaining off, and
 * the JIT with chaining on - and must end in bit-identical machine state. The guest
 * is a hand-encoded MIPS32 big-endian program built to stress what chaining skips:
 * calls and returns (constant and computed EXITs, one with eight targets, also run with a
 * two-entry jump table so its links collide), data-dependent branches, code it
 * rewrites itself (on a page other blocks link into), interrupts it disables and
 * re-enables (DI/EI) while a CP0 timer fires, and an MMIO device. Between runs the
 * host rewrites code, adds and removes PC hooks, and takes and restores snapshots.
 * A last check stores into the page of the block being run just after that page
 * became a code page.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uam/isa/mips32.h"
#include "uam/machine.h"
#include "uam/snapshot.h"

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

/* ---- a tiny MIPS32 encoder ---------------------------------------------- */

enum { ZERO = 0, V0 = 2, T0 = 8, T1, T2, T3, T4, T5, T6, T7, S0 = 16, S1, T8 = 24, T9, K0, K1, RA = 31 };

static uint32_t i_type(unsigned op, unsigned rs, unsigned rt, uint32_t imm)
{
    return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF);
}
static uint32_t r_type(unsigned rs, unsigned rt, unsigned rd, unsigned sa, unsigned fn)
{
    return (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | fn;
}
static uint32_t ADDIU(unsigned rt, unsigned rs, int16_t v) { return i_type(0x09, rs, rt, (uint16_t)v); }
static uint32_t ANDI(unsigned rt, unsigned rs, uint16_t v)  { return i_type(0x0C, rs, rt, v); }
static uint32_t ORI(unsigned rt, unsigned rs, uint16_t v)   { return i_type(0x0D, rs, rt, v); }
static uint32_t LUI(unsigned rt, uint16_t v)                { return i_type(0x0F, 0, rt, v); }
static uint32_t LW(unsigned rt, int16_t off, unsigned base) { return i_type(0x23, base, rt, (uint16_t)off); }
static uint32_t SW(unsigned rt, int16_t off, unsigned base) { return i_type(0x2B, base, rt, (uint16_t)off); }
static uint32_t ADDU(unsigned rd, unsigned rs, unsigned rt) { return r_type(rs, rt, rd, 0, 0x21); }
static uint32_t OR(unsigned rd, unsigned rs, unsigned rt)   { return r_type(rs, rt, rd, 0, 0x25); }
static uint32_t SLL(unsigned rd, unsigned rt, unsigned sa)  { return r_type(0, rt, rd, sa, 0x00); }
static uint32_t SRL(unsigned rd, unsigned rt, unsigned sa)  { return r_type(0, rt, rd, sa, 0x02); }
static uint32_t JR(unsigned rs)                             { return r_type(rs, 0, 0, 0, 0x08); }
static uint32_t JALR(unsigned rs)                           { return r_type(rs, 0, RA, 0, 0x09); }
static uint32_t BNE(unsigned rs, unsigned rt, uint32_t at, uint32_t to)
{
    return i_type(0x05, rs, rt, (uint16_t)((int32_t)(to - (at + 4)) >> 2));
}
static uint32_t BEQ(unsigned rs, unsigned rt, uint32_t at, uint32_t to)
{
    return i_type(0x04, rs, rt, (uint16_t)((int32_t)(to - (at + 4)) >> 2));
}
static uint32_t JAL(uint32_t to)                            { return (0x03u << 26) | ((to >> 2) & 0x3FFFFFF); }
static uint32_t MFC0(unsigned rt, unsigned rd)              { return 0x40000000u | (rt << 16) | (rd << 11); }
static uint32_t MTC0(unsigned rt, unsigned rd)              { return 0x40800000u | (rt << 16) | (rd << 11); }
#define NOP  0x00000000u
#define ERET 0x42000018u
#define EI   0x41606020u
#define DI   0x41606000u

/* ---- the machine -------------------------------------------------------- */

#define TICK_PHYS 0x1F000000u   /* kseg1 0xBF000000 */
#define FUNC      0x1000u       /* the called function: its own code page */
#define PATCH     (FUNC + 4)    /* the instruction the guest rewrites */
#define STUBS     0x2000u       /* eight 2-instruction functions: one computed call */
#define RET_PC    0x80000028u   /* where FUNC returns to */
#define CP0(r, s) ((r) * 8 + (s))  /* sysreg id of CP0 register r, select s */

typedef struct tickdev {
    uam_device dev;
    uam_sched *sched;
    uam_timer  timer;
    uint32_t   count, reads;
} tickdev;

static void tick_fire(void *ctx)
{
    tickdev *t = ctx;
    t->count++;
    uam_timer_arm(t->sched, &t->timer, uam_sched_now(t->sched) + 777);
}

static uam_status tick_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    (void)off; (void)size;
    tickdev *t = UAM_CONTAINER_OF(d, tickdev, dev);
    t->reads++;
    *val = t->count;
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
    uam_state_put_u32(w, t->reads);
    uam_state_put_timer(w, &t->timer);
}

static void tick_restore(uam_device *d, uam_state_reader *r)
{
    tickdev *t = UAM_CONTAINER_OF(d, tickdev, dev);
    t->count = uam_state_get_u32(r);
    t->reads = uam_state_get_u32(r);
    uam_state_get_timer(r, &t->timer);
}

static const uam_device_ops tick_ops = {
    .type = "test,tick", .endian = UAM_ACC_BE,
    .read = tick_read, .write = tick_write, .destroy = tick_destroy,
    .save = tick_save, .restore = tick_restore,
};

static void put32(uam_machine *m, uint64_t pa, uint32_t w)
{
    uint8_t b[4] = { (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w };
    CHECK(uam_bus_load_bytes(&m->bus, pa, b, 4) == UAM_OK, "load word");
}

static void put_prog(uam_machine *m, uint32_t at, const uint32_t *w, size_t n)
{
    for (size_t i = 0; i < n; i++)
        put32(m, at + 4 * i, w[i]);
}

typedef enum mode { INTERP, JIT_NOCHAIN, JIT_CHAIN, JIT_TAB1 } mode;
static const char *const mode_name[] = { "interp", "jit (no chain)", "jit", "jit (2-entry jump table)" };

/*
 * main (0x80000000):
 *       lui  t1, 0x8010          data buffer
 *       lui  t7, 0xBF00          tick device
 *       lui  s0, 0x8000
 *       ori  s0, s0, PATCH       the patched instruction
 *       lui  t8, 0x258C          "addiu t4, t4, 0"
 *       lui  s1, 0x8000
 *       ori  s1, s1, STUBS
 *   loop:
 *       di
 *       jal  FUNC                computed return: jr ra
 *       nop
 *       ei                       the timer may have fired meanwhile
 *       andi t2, t0, 7           computed call to one of eight stubs
 *       sll  t2, t2, 3
 *       addu t2, t2, s1
 *       jalr t2
 *       nop
 *       andi t2, t0, 3
 *       beq  t2, zero, skip      data-dependent branch
 *       nop
 *       lw   t6, 0(t7)           device read
 *       addu t5, t5, t6
 *   skip:
 *       andi t2, t0, 0xFF
 *       bne  t2, zero, loop
 *       nop
 *       srl  t9, t0, 8           every 256 iterations: rewrite FUNC+4 to
 *       andi t9, t9, 0x7F        "addiu t4, t4, (t0 >> 8) & 0x7F"
 *       or   t9, t9, t8
 *       sw   t9, 0(s0)
 *       sw   t9, STUBS+12-PATCH(s0)   and the delay slots of stub 1 (the first target
 *       sw   t9, STUBS+28-PATCH(s0)   after the rewrite: linked in its EXIT's slot) and
 *                                     stub 3 (linked in the jump table)
 *       beq  zero, zero, loop
 *       nop
 * FUNC:
 *       addiu t0, t0, 1
 *       addiu t4, t4, 0          (rewritten by the guest and by the host)
 *       sll  t3, t0, 2
 *       andi t3, t3, 0xFFC
 *       addu t3, t3, t1
 *       sw   t0, 0(t3)
 *       jr   ra
 *       nop
 * STUBS + 8k (k = 0..7):
 *       jr   ra
 *       addiu v0, v0, k + 1
 * CP0 timer handler (0x180): count in k0, re-arm Compare 300 cycles ahead.
 */
static tickdev *build(uam_machine *m, mode md)
{
    CHECK(uam_machine_init(m, &uam_isa_mips32be) == UAM_OK, "machine init");
    m->board_name = "jit-test";
    CHECK(uam_bus_map_ram(&m->bus, "ram", 0, 4u << 20) == UAM_OK, "map ram");
    tickdev *t = calloc(1, sizeof(*t));
    uam_device_init(&t->dev, &tick_ops, "tick");
    t->sched = &m->sched;
    uam_timer_init(&t->timer, tick_fire, t);
    uam_machine_add_device(m, &t->dev);
    CHECK(uam_bus_map_mmio(&m->bus, "tick", TICK_PHYS, 0x100, &t->dev) == UAM_OK, "map tick");

    const uint32_t B = 0x80000000u, LOOP = B + 0x1C, SKIP = B + 0x54;
    const uint32_t main_prog[] = {
        LUI(T1, 0x8010), LUI(T7, 0xBF00), LUI(S0, 0x8000), ORI(S0, S0, PATCH), LUI(T8, 0x258C),
        LUI(S1, 0x8000), ORI(S1, S1, STUBS),
        /* loop: 0x1C */
        DI, JAL(B + FUNC), NOP, EI, ANDI(T2, T0, 7), SLL(T2, T2, 3), ADDU(T2, T2, S1),
        JALR(T2), NOP, ANDI(T2, T0, 3),
        BEQ(T2, ZERO, B + 0x44, SKIP), NOP, LW(T6, 0, T7), ADDU(T5, T5, T6),
        /* skip: 0x54 */
        ANDI(T2, T0, 0xFF), BNE(T2, ZERO, B + 0x58, LOOP), NOP,
        SRL(T9, T0, 8), ANDI(T9, T9, 0x7F), OR(T9, T9, T8), SW(T9, 0, S0),
        SW(T9, STUBS + 12 - PATCH, S0), SW(T9, STUBS + 28 - PATCH, S0),
        BEQ(ZERO, ZERO, B + 0x78, LOOP), NOP,
    };
    put_prog(m, 0, main_prog, UAM_ARRAY_LEN(main_prog));
    const uint32_t func[] = {
        ADDIU(T0, T0, 1), ADDIU(T4, T4, 0), SLL(T3, T0, 2), ANDI(T3, T3, 0xFFC),
        ADDU(T3, T3, T1), SW(T0, 0, T3), JR(RA), NOP,
    };
    put_prog(m, FUNC, func, UAM_ARRAY_LEN(func));
    for (uint32_t k = 0; k < 8; k++) {
        const uint32_t stub[] = { JR(RA), ADDIU(V0, V0, (int16_t)(k + 1)) };
        put_prog(m, STUBS + 8 * k, stub, 2);
    }
    const uint32_t handler[] = {
        ADDIU(K0, K0, 1), MFC0(K1, 9), ADDIU(K1, K1, 300), MTC0(K1, 11), ERET,
    };
    put_prog(m, 0x180, handler, UAM_ARRAY_LEN(handler));

    uam_machine_reset(m, 0x80000000u);
    m->cpu.isa->sysreg_write(&m->cpu, CP0(12, 0), (1u << 15) | 1u);   /* IM7 | IE */
    m->cpu.isa->sysreg_write(&m->cpu, CP0(11, 0), 500);              /* Compare */
    uam_timer_arm(&m->sched, &t->timer, 777);
    if (md != INTERP) {
        if (md == JIT_TAB1)
            setenv("UAM_JIT_JTAB_BITS", "1", 1);
        CHECK(uam_exec_enable_jit(&m->exec), "jit available");
        unsetenv("UAM_JIT_JTAB_BITS");
        m->exec.chain = md >= JIT_CHAIN;
    }
    return t;
}

static uint32_t gpr(uam_machine *m, unsigned r) { return (uint32_t)uam_cpu_reg_read(&m->cpu, r * 4, 32); }

/* Everything observable: memory, registers, CP0, time, the device. */
static uint64_t state_hash(uam_machine *m, const tickdev *t)
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
    static const uint32_t cp0[] = { CP0(9, 0), CP0(11, 0), CP0(12, 0), CP0(13, 0), CP0(14, 0) };
    for (size_t i = 0; i < UAM_ARRAY_LEN(cp0); i++) {
        uint64_t v = 0;
        m->cpu.isa->sysreg_read(&m->cpu, cp0[i], &v);
        MIX(v);
    }
    MIX(t->count);
    MIX(t->reads);
#undef MIX
    return h;
}

/* ---- scenarios ---------------------------------------------------------- */

typedef struct outcome {
    uint64_t hash[8];
    unsigned n_hash;
    uint64_t hook_hits, blocks, chained, interrupts, polls;
} outcome;

static uam_hook_action count_hook(void *ctx, uam_cpu *cpu, uint64_t pc)
{
    (void)cpu; (void)pc;
    (*(uint64_t *)ctx)++;
    return UAM_HOOK_CONTINUE;
}

static void count_poll(void *ctx)
{
    (*(uint64_t *)ctx)++;
}

static void note(outcome *o, uam_machine *m, const tickdev *t)
{
    if (o->n_hash < UAM_ARRAY_LEN(o->hash))
        o->hash[o->n_hash++] = state_hash(m, t);
}

static void run(uam_machine *m, uint64_t n, const char *what, mode md)
{
    uam_stop_reason why = uam_exec_run(&m->exec, n);
    CHECK(why == UAM_STOP_INSN_LIMIT, "%s: %s: stopped by %s", mode_name[md], what,
          uam_stop_reason_str(why));
}

static outcome scenario(mode md)
{
    outcome o = { 0 };
    uam_machine m;
    tickdev *t = build(&m, md);

    /* 1. Plain run, no host bridge (long chains): links form, the guest rewrites
     * FUNC+4 now and then. */
    run(&m, 200000, "run", md);
    note(&o, &m, t);

    /* From here on the host is polled every 16 blocks, chained or not. */
    uam_exec_host host = { .poll = count_poll, .ctx = &o.polls, .poll_interval = 16 };
    uam_exec_set_host(&m.exec, &host);

    /* 2. The host rewrites code that linked blocks jump into. */
    put32(&m, PATCH, ADDIU(T4, T4, 1000));
    run(&m, 50000, "after host patch", md);
    note(&o, &m, t);

    /* 3. A hook on a chain target (FUNC entry) and on the return target. */
    uint64_t hits = 0;
    CHECK(uam_exec_add_pc_hook(&m.exec, 0x80000000u + FUNC, count_hook, &hits) == UAM_OK, "hook");
    CHECK(uam_exec_add_pc_hook(&m.exec, RET_PC, count_hook, &hits) == UAM_OK, "hook 2");
    run(&m, 50000, "with hooks", md);
    CHECK(hits > 1000, "%s: hooks fired on chain targets (%llu)", mode_name[md],
          (unsigned long long)hits);
    uam_exec_remove_pc_hook(&m.exec, 0x80000000u + FUNC, count_hook, &hits);
    uam_exec_remove_pc_hook(&m.exec, RET_PC, count_hook, &hits);
    o.hook_hits = hits;
    run(&m, 50000, "hooks removed", md);
    note(&o, &m, t);

    /* 4. Snapshot, run (the guest rewrites code), restore (code comes back), run. */
    uam_snapshot *s = NULL;
    CHECK(uam_snapshot_take(&m, &s) == UAM_OK, "take");
    uint64_t h0 = state_hash(&m, t);
    run(&m, 100000, "after snapshot", md);
    uint64_t h1 = state_hash(&m, t);
    for (int i = 0; i < 3; i++) {
        CHECK(uam_snapshot_restore(&m, s) == UAM_OK, "restore");
        CHECK(state_hash(&m, t) == h0, "%s: restore %d matches the snapshot", mode_name[md], i);
        run(&m, 100000, "re-run", md);
        CHECK(state_hash(&m, t) == h1, "%s: re-run %d is identical", mode_name[md], i);
    }
    note(&o, &m, t);
    uam_snapshot_free(s);

    /* 5. Short budgets: every stop lands on the same instruction. */
    for (int i = 1; i < 40; i++)
        run(&m, (uint64_t)i * 7, "short run", md);
    note(&o, &m, t);

    o.blocks = m.exec.stats.blocks_executed;
    o.chained = m.exec.stats.blocks_chained;
    o.interrupts = m.exec.stats.interrupts;
    CHECK(gpr(&m, K0) > 100, "%s: timer interrupts were taken (%u)", mode_name[md], gpr(&m, K0));
    uam_machine_fini(&m);
    return o;
}

static void test_equivalence(void)
{
    outcome ref = scenario(INTERP);
    for (mode md = JIT_NOCHAIN; md <= JIT_TAB1; md++) {
        outcome o = scenario(md);
        CHECK(o.n_hash == ref.n_hash, "%s: same number of checkpoints", mode_name[md]);
        for (unsigned i = 0; i < ref.n_hash && i < o.n_hash; i++)
            CHECK(o.hash[i] == ref.hash[i], "%s: checkpoint %u matches the interpreter",
                  mode_name[md], i);
        CHECK(o.hook_hits == ref.hook_hits, "%s: hook hits %llu, interpreter %llu", mode_name[md],
              (unsigned long long)o.hook_hits, (unsigned long long)ref.hook_hits);
        CHECK(o.blocks == ref.blocks, "%s: blocks executed %llu, interpreter %llu", mode_name[md],
              (unsigned long long)o.blocks, (unsigned long long)ref.blocks);
        CHECK(o.interrupts == ref.interrupts, "%s: interrupts %llu, interpreter %llu",
              mode_name[md], (unsigned long long)o.interrupts, (unsigned long long)ref.interrupts);
        CHECK(o.polls == ref.polls, "%s: host polls %llu, interpreter %llu", mode_name[md],
              (unsigned long long)o.polls, (unsigned long long)ref.polls);
        if (md == JIT_CHAIN)   /* 54% when only one target of a computed EXIT chains */
            CHECK(o.chained > o.blocks / 5 * 3, "chained: %llu of %llu blocks entered by chaining",
                  (unsigned long long)o.chained, (unsigned long long)o.blocks);
        else if (md == JIT_TAB1)
            CHECK(o.chained > o.blocks / 2, "chained: %llu of %llu blocks entered by chaining",
                  (unsigned long long)o.chained, (unsigned long long)o.blocks);
        else
            CHECK(o.chained == 0, "no chaining when it is off");
    }
}

/* Block tracing reports every block, so nothing is chained while it is on. */
static void test_trace_disables(void)
{
    uam_machine m;
    build(&m, JIT_CHAIN);
    FILE *devnull = fopen("/dev/null", "w");
    CHECK(devnull != NULL, "open /dev/null");
    m.exec.trace = UAM_TRACE_BLOCKS;
    m.exec.trace_out = devnull;
    uam_exec_run(&m.exec, 20000);
    CHECK(m.exec.stats.blocks_chained == 0, "no chaining while tracing blocks");
    m.exec.trace = 0;
    uam_exec_run(&m.exec, 20000);
    CHECK(m.exec.stats.blocks_chained > 0, "chaining resumes when tracing stops");
    uam_machine_fini(&m);
    fclose(devnull);
}

/* A stop requested from inside a chain (a device read) still ends the run at the
 * same instruction. */
static uam_hook_action stop_hook(void *ctx, uam_cpu *cpu, uint64_t pc)
{
    (void)cpu; (void)pc;
    uam_exec_stop(ctx, 7);
    return UAM_HOOK_CONTINUE;
}

static void test_stop_in_chain(void)
{
    uint64_t at[3];
    for (mode md = INTERP; md <= JIT_CHAIN; md++) {
        uam_machine m;
        build(&m, md);
        uam_exec_run(&m.exec, 123457);
        /* a hook that requests a stop: the run ends right there */
        uam_exec_add_pc_hook(&m.exec, 0x80000030u, stop_hook, &m.exec);
        CHECK(uam_exec_run(&m.exec, 0) == UAM_STOP_REQUEST, "%s: stop requested",
              mode_name[md]);
        at[md] = m.cpu.icount;
        uam_machine_fini(&m);
    }
    CHECK(at[1] == at[0] && at[2] == at[0], "stop at the same instruction (%llu %llu %llu)",
          (unsigned long long)at[0], (unsigned long long)at[1], (unsigned long long)at[2]);
}

/*
 * Self-modifying code on a page that has just become a code page again. A store to a
 * code page takes the slow path, which drops the page's translations and lets the
 * page into the JIT's data-TLB for writes. The next block on that page makes it a code
 * page once more; a store in that block must still take the slow path, so that the
 * block ends and the instruction it rewrote runs as rewritten, as on the interpreter.
 */
static uam_hook_action halt_hook(void *ctx, uam_cpu *cpu, uint64_t pc)
{
    (void)cpu; (void)pc;
    uam_exec_stop(ctx, 0);
    return UAM_HOOK_CONTINUE;
}

static void test_store_to_new_code_page(void)
{
    const uint32_t p = 0x80002000u;
    const uint32_t patch = ADDIU(T1, ZERO, 3);
    const uint32_t prog[] = {
        (0x02u << 26) | (((p + 8) >> 2) & 0x3FFFFFFu), 0,  /* p+0  j p+8: p is a code page */
        LUI(T0, 0x8000), ORI(T0, T0, 0x2000),        /* p+8  t0 = p */
        SW(T0, 0x800, T0),                           /* p+16 data store into this code page */
        LUI(T2, (uint16_t)(patch >> 16)),            /* p+20 a new block on the same page */
        ORI(T2, T2, (uint16_t)patch),
        SW(T2, 40, T0),                              /* p+28 rewrite p+40 */
        ADDIU(T1, ZERO, 1),
        ADDIU(T1, ZERO, 1),
        ADDIU(T1, ZERO, 2),                          /* p+40 becomes t1 = 3 */
        BEQ(ZERO, ZERO, p + 44, p + 44), 0,          /* p+44 */
    };
    for (mode md = INTERP; md <= JIT_CHAIN; md++) {
        uam_machine m;
        CHECK(uam_machine_init(&m, &uam_isa_mips32be) == UAM_OK, "machine init");
        CHECK(uam_bus_map_ram(&m.bus, "ram", 0, 1 << 20) == UAM_OK, "map ram");
        put_prog(&m, p & 0x1FFFFFFFu, prog, UAM_ARRAY_LEN(prog));
        uam_machine_reset(&m, p);
        if (md != INTERP)
            CHECK(uam_exec_enable_jit(&m.exec), "jit");
        m.exec.chain = md == JIT_CHAIN;
        uam_exec_add_pc_hook(&m.exec, p + 44, halt_hook, &m.exec);
        uam_exec_run(&m.exec, 1000);
        CHECK(gpr(&m, T1) == 3, "%s: the rewritten instruction ran (t1 = %u)", mode_name[md],
              gpr(&m, T1));
        uam_machine_fini(&m);
    }
}

int main(void)
{
    test_equivalence();
    test_trace_disables();
    test_stop_in_chain();
    test_store_to_new_code_page();
    printf("test_jit: %d/%d checks passed\n", n_checks - n_failed, n_checks);
    return n_failed ? 1 : 0;
}
