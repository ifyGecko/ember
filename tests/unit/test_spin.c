/*
 * test_spin.c - Skipping the passes of spin loops (src/exec/spin.h): a run that skips
 * must be indistinguishable from one that runs every pass.
 *
 * Two guests, RV32 and MIPS32 BE, hand-encoded, each wait in turn in four loops:
 *   - a spin loop on a RAM word that a device's timer callback changes; on MIPS its
 *     delay slot writes a constant to a register that the code before the loop clears,
 *     so each arrival starts one pass away from a state the loop keeps, and a CP0 timer
 *     interrupt handler logs that register (a pass skipped from the wrong state shows);
 *   - a delay loop that counts a register down (each pass changes it: never skipped);
 *   - a loop polling a device register (reads have effects: never skipped);
 *   - a loop that stores to memory each pass (never skipped).
 * Each guest runs under the interpreter and the JIT, with and without skipping (and the
 * JIT without chaining), first with no host polling (long chains), then with the host
 * polled every few blocks, then over many short runs, and must end in identical state, device counts, polls, interrupts
 * and block counts. The loops' spin states show which ones were candidates.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uam/isa/mips32.h"
#include "uam/isa/riscv.h"
#include "uam/machine.h"

#include "../../src/exec/spin.h"

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

/* ---- the device: a RAM word its timer bumps, and a status register ------ */

#define FLAG_PA   0x10000u
#define PERIOD    1500u      /* ticks between flag changes */

typedef struct flagdev {
    uam_device dev;
    uam_sched *sched;
    uam_bus   *bus;
    unsigned   endian;       /* access attrs of the guest's byte order */
    uam_timer  timer;
    uint32_t   fires, reads;
} flagdev;

static void flag_fire(void *ctx)
{
    flagdev *f = ctx;
    uint64_t v = 0;
    uam_bus_read(f->bus, FLAG_PA, 4, f->endian, &v);
    uam_bus_write(f->bus, FLAG_PA, 4, f->endian, v + 1);
    f->fires++;
    uam_timer_arm(f->sched, &f->timer, uam_sched_now(f->sched) + PERIOD);
}

static uam_status flag_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    (void)off; (void)size;
    flagdev *f = UAM_CONTAINER_OF(d, flagdev, dev);
    f->reads++;
    *val = f->reads % 5 == 0;   /* ready on every fifth read */
    return UAM_OK;
}

static uam_status flag_write(uam_device *d, uint64_t off, unsigned size, uint64_t val)
{
    (void)d; (void)off; (void)size; (void)val;
    return UAM_OK;
}

static void flag_destroy(uam_device *d)
{
    flagdev *f = UAM_CONTAINER_OF(d, flagdev, dev);
    uam_timer_cancel(f->sched, &f->timer);
    free(f);
}

static const uam_device_ops flag_ops_le = {
    .type = "test,flag", .endian = UAM_ACC_LE,
    .read = flag_read, .write = flag_write, .destroy = flag_destroy,
};
static const uam_device_ops flag_ops_be = {
    .type = "test,flag", .endian = UAM_ACC_BE,
    .read = flag_read, .write = flag_write, .destroy = flag_destroy,
};

/* ---- the guests ----------------------------------------------------------- */

/* RV32 */
enum { RZ = 0, RT0 = 5, RT1, RT2, RS1 = 9, RS2 = 18, RS3, RT3 = 28, RT4, RT5, RT6 };
static uint32_t rv_i(unsigned op, unsigned f3, unsigned rd, unsigned rs1, int32_t imm)
{
    return ((uint32_t)imm & 0xFFF) << 20 | rs1 << 15 | f3 << 12 | rd << 7 | op;
}
static uint32_t ADDI(unsigned rd, unsigned rs1, int32_t v) { return rv_i(0x13, 0, rd, rs1, v); }
static uint32_t RLW(unsigned rd, int32_t off, unsigned rs1) { return rv_i(0x03, 2, rd, rs1, off); }
static uint32_t RLUI(unsigned rd, uint32_t imm20)           { return imm20 << 12 | rd << 7 | 0x37; }
static uint32_t RSW(unsigned rs2, int32_t off, unsigned rs1)
{
    return ((uint32_t)off >> 5 & 0x7F) << 25 | rs2 << 20 | rs1 << 15 | 2u << 12
         | ((uint32_t)off & 0x1F) << 7 | 0x23;
}
static uint32_t rv_b(unsigned f3, unsigned rs1, unsigned rs2, uint32_t at, uint32_t to)
{
    uint32_t o = to - at;
    return (o >> 12 & 1) << 31 | (o >> 5 & 0x3F) << 25 | rs2 << 20 | rs1 << 15 | f3 << 12
         | (o >> 1 & 0xF) << 8 | (o >> 11 & 1) << 7 | 0x63;
}
static uint32_t RJ(uint32_t at, uint32_t to)
{
    uint32_t o = to - at;
    return (o >> 20 & 1) << 31 | (o >> 1 & 0x3FF) << 21 | (o >> 11 & 1) << 20
         | (o >> 12 & 0xFF) << 12 | 0x6F;
}

/* MIPS32 */
enum { MZ = 0, T2 = 10, T3, T4, T5, T6, T7, S1 = 17, S2, S3, K0 = 26, K1 };
static uint32_t m_i(unsigned op, unsigned rs, unsigned rt, uint32_t imm)
{
    return op << 26 | rs << 21 | rt << 16 | (imm & 0xFFFF);
}
static uint32_t ADDIU(unsigned rt, unsigned rs, int16_t v) { return m_i(0x09, rs, rt, (uint16_t)v); }
static uint32_t ANDI(unsigned rt, unsigned rs, uint16_t v)  { return m_i(0x0C, rs, rt, v); }
static uint32_t LUI(unsigned rt, uint16_t v)                { return m_i(0x0F, 0, rt, v); }
static uint32_t LW(unsigned rt, int16_t off, unsigned base) { return m_i(0x23, base, rt, (uint16_t)off); }
static uint32_t SW(unsigned rt, int16_t off, unsigned base) { return m_i(0x2B, base, rt, (uint16_t)off); }
static uint32_t ADDU(unsigned rd, unsigned rs, unsigned rt) { return rs << 21 | rt << 16 | rd << 11 | 0x21; }
static uint32_t MBEQ(unsigned rs, unsigned rt, uint32_t at, uint32_t to)
{
    return m_i(0x04, rs, rt, (uint16_t)((int32_t)(to - (at + 4)) >> 2));
}
static uint32_t MBNE(unsigned rs, unsigned rt, uint32_t at, uint32_t to)
{
    return m_i(0x05, rs, rt, (uint16_t)((int32_t)(to - (at + 4)) >> 2));
}
static uint32_t MFC0(unsigned rt, unsigned rd) { return 0x40000000u | rt << 16 | rd << 11; }
static uint32_t MTC0(unsigned rt, unsigned rd) { return 0x40800000u | rt << 16 | rd << 11; }
#define NOP  0x00000000u
#define ERET 0x42000018u
#define CP0(r, s) ((r) * 8 + (s))

typedef enum guest { RV32, MIPS } guest;
static const char *const guest_name[] = { "rv32", "mips" };

/* Where each loop starts, relative to the guest's code base. */
enum { LOOP = 0x0C, COUNT = 0x20 };
static const uint32_t mmio_at[] = { 0x28, 0x2C }, store_at[] = { 0x34, 0x3C };

/*
 * RV32 at 0:                                MIPS at 0x80000000:
 *  00 lui  s1, 0x10        FLAG              00 lui   s1, 0x8001      FLAG
 *  04 lui  s3, 0x20000     device            04 lui   s3, 0xBF00      device (kseg1)
 *  08 addi s2, zero, 0     last flag seen    08 addiu s2, zero, 0
 *  0c lw   t0, 0(s1)       spin              0c lw    t5, 0(s1)       spin
 *  10 lui  t6, 0x12345                       10 beq   t5, s2, 0c
 *  14 beq  t0, s2, 0c                        14 lui   t6, 0x1234      (delay slot)
 *  18 addi s2, t0, 0                         18 addu  s2, t5, zero
 *  1c addi t2, zero, 50                      1c addiu t2, zero, 50
 *  20 addi t2, t2, -1      count down        20 addiu t2, t2, -1      count down
 *  24 bne  t2, zero, 20                      24 bne   t2, zero, 20
 *  28 lw   t3, 0(s3)       poll the device   28 nop
 *  2c beq  t3, zero, 28                      2c lw    t3, 0(s3)       poll the device
 *  30 addi t4, zero, 3                       30 beq   t3, zero, 2c
 *  34 sw   t4, 0x100(s1)   store each pass   34 nop
 *  38 lw   t5, 0(s1)                         38 addiu t4, zero, 3
 *  3c beq  t5, s2, 34                        3c sw    t4, 0x100(s1)   store each pass
 *  40 addi s2, t5, 0                         40 lw    t7, 0(s1)
 *  44 addi t6, zero, 0                       44 beq   t7, s2, 3c
 *  48 j    0c                                48 nop
 *                                            4c addu  s2, t7, zero
 *                                            50 addiu t6, zero, 0
 *                                            54 beq   zero, zero, 0c
 *                                            58 nop
 * MIPS CP0 timer handler (0x180): log t5 and t6 in a ring at FLAG + 0x400 (index at
 * FLAG + 0x200), re-arm Compare 700 cycles ahead.
 */
static void put_words(uam_machine *m, uint32_t pa, const uint32_t *w, size_t n, bool be)
{
    for (size_t i = 0; i < n; i++) {
        uint32_t v = w[i];
        uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
        if (be) {
            uint8_t r[4] = { b[3], b[2], b[1], b[0] };
            memcpy(b, r, 4);
        }
        CHECK(uam_bus_load_bytes(&m->bus, pa + 4 * i, b, 4) == UAM_OK, "load word");
    }
}

typedef enum mode { INTERP_NOSKIP, INTERP, JIT_NOSKIP, JIT, JIT_NOCHAIN } mode;
static const char *const mode_name[] = { "interp (no skip)", "interp", "jit (no skip)", "jit",
                                         "jit (no chain)" };
static bool skips(mode md) { return md != INTERP_NOSKIP && md != JIT_NOSKIP; }

static uint64_t base_of(guest g) { return g == MIPS ? 0x80000000u : 0; }

static flagdev *build(uam_machine *m, guest g, mode md)
{
    CHECK(uam_machine_init(m, g == MIPS ? &uam_isa_mips32be : &uam_isa_rv32) == UAM_OK,
          "machine init");
    m->board_name = "spin-test";
    CHECK(uam_bus_map_ram(&m->bus, "ram", 0, 1u << 20) == UAM_OK, "map ram");
    flagdev *f = calloc(1, sizeof(*f));
    uam_device_init(&f->dev, g == MIPS ? &flag_ops_be : &flag_ops_le, "flag");
    f->sched = &m->sched;
    f->bus = &m->bus;
    f->endian = g == MIPS ? UAM_ACC_BE : UAM_ACC_LE;
    uam_timer_init(&f->timer, flag_fire, f);
    uam_machine_add_device(m, &f->dev);
    const uint32_t dev_pa = g == MIPS ? 0x1F000000u : 0x20000000u;
    CHECK(uam_bus_map_mmio(&m->bus, "flag", dev_pa, 0x100, &f->dev) == UAM_OK, "map flag");

    if (g == RV32) {
        const uint32_t p[] = {
            RLUI(RS1, 0x10), RLUI(RS3, 0x20000), ADDI(RS2, RZ, 0),
            RLW(RT0, 0, RS1), RLUI(RT6, 0x12345), rv_b(0, RT0, RS2, 0x14, LOOP),
            ADDI(RS2, RT0, 0), ADDI(RT2, RZ, 50),
            ADDI(RT2, RT2, -1), rv_b(1, RT2, RZ, 0x24, COUNT),
            RLW(RT3, 0, RS3), rv_b(0, RT3, RZ, 0x2C, 0x28),
            ADDI(RT4, RZ, 3),
            RSW(RT4, 0x100, RS1), RLW(RT5, 0, RS1), rv_b(0, RT5, RS2, 0x3C, 0x34),
            ADDI(RS2, RT5, 0), ADDI(RT6, RZ, 0), RJ(0x48, LOOP),
        };
        put_words(m, 0, p, UAM_ARRAY_LEN(p), false);
        uam_machine_reset(m, 0);
    } else {
        const uint32_t B = 0x80000000u;
        const uint32_t p[] = {
            LUI(S1, 0x8001), LUI(S3, 0xBF00), ADDIU(S2, MZ, 0),
            LW(T5, 0, S1), MBEQ(T5, S2, B + 0x10, B + LOOP), LUI(T6, 0x1234),
            ADDU(S2, T5, MZ), ADDIU(T2, MZ, 50),
            ADDIU(T2, T2, -1), MBNE(T2, MZ, B + 0x24, B + COUNT), NOP,
            LW(T3, 0, S3), MBEQ(T3, MZ, B + 0x30, B + 0x2C), NOP,
            ADDIU(T4, MZ, 3),
            SW(T4, 0x100, S1), LW(T7, 0, S1), MBEQ(T7, S2, B + 0x44, B + 0x3C), NOP,
            ADDU(S2, T7, MZ), ADDIU(T6, MZ, 0), MBEQ(MZ, MZ, B + 0x54, B + LOOP), NOP,
        };
        put_words(m, 0, p, UAM_ARRAY_LEN(p), true);
        const uint32_t h[] = {
            LUI(K1, 0x8001), LW(K0, 0x200, K1), ADDIU(K0, K0, 8), ANDI(K0, K0, 0x3F8),
            SW(K0, 0x200, K1), ADDU(K0, K0, K1), SW(T5, 0x400, K0), SW(T6, 0x404, K0),
            MFC0(K1, 9), ADDIU(K1, K1, 700), MTC0(K1, 11), ERET,
        };
        put_words(m, 0x180, h, UAM_ARRAY_LEN(h), true);
        uam_machine_reset(m, B);
        m->cpu.isa->sysreg_write(&m->cpu, CP0(12, 0), (1u << 15) | 1u);   /* IM7 | IE */
        m->cpu.isa->sysreg_write(&m->cpu, CP0(11, 0), 900);              /* Compare */
    }
    uam_timer_arm(&m->sched, &f->timer, PERIOD);
    if (md >= JIT_NOSKIP) {
        CHECK(uam_exec_enable_jit(&m->exec), "jit available");
        m->exec.chain = md != JIT_NOCHAIN;
    }
    return f;
}

/* Everything observable: memory, registers, time, the device. */
static uint64_t state_hash(uam_machine *m, guest g, const flagdev *f)
{
    uint64_t h = UINT64_C(0xcbf29ce484222325);
#define MIX(v) (h = (h ^ (uint64_t)(v)) * UINT64_C(0x100000001b3))
    const uam_region *ram = uam_bus_region(&m->bus, "ram");
    for (uint64_t i = 0; i < 0x20000; i += 8) {
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
    if (g == MIPS) {
        static const uint32_t cp0[] = { CP0(9, 0), CP0(11, 0), CP0(12, 0), CP0(13, 0), CP0(14, 0) };
        for (size_t i = 0; i < UAM_ARRAY_LEN(cp0); i++) {
            uint64_t v = 0;
            m->cpu.isa->sysreg_read(&m->cpu, cp0[i], &v);
            MIX(v);
        }
    }
    MIX(f->fires);
    MIX(f->reads);
#undef MIX
    return h;
}

typedef struct outcome {
    uint64_t hash[3];
    uint64_t blocks, chained, interrupts, polls, reads;
    int8_t   spin[4];   /* spin states of LOOP, COUNT, the device poll, the store loop */
} outcome;

static void count_poll(void *ctx) { (*(uint64_t *)ctx)++; }

static void run(uam_machine *m, uint64_t n, guest g, mode md)
{
    uam_stop_reason why = uam_exec_run(&m->exec, n);
    CHECK(why == UAM_STOP_INSN_LIMIT, "%s %s: stopped by %s", guest_name[g], mode_name[md],
          uam_stop_reason_str(why));
}

static int8_t spin_state(uam_machine *m, uint64_t pc)
{
    int8_t *s = NULL;
    if (!uam_tcache_find(&m->exec.tc, pc, m->cpu.priv, m->cpu.xlate_gen, &s) || !s)
        return 0;
    return *s;
}

static outcome scenario(guest g, mode md)
{
    outcome o = { 0 };
    if (!skips(md))
        setenv("UAM_SPIN_SKIP", "0", 1);
    uam_machine m;
    flagdev *f = build(&m, g, md);

    /* No host polling: chains run long. */
    run(&m, 400000, g, md);
    o.hash[0] = state_hash(&m, g, f);
    /* The host polled every 7 blocks: a skip must stop short of the next poll. */
    uam_exec_host host = { .poll = count_poll, .ctx = &o.polls, .poll_interval = 7 };
    uam_exec_set_host(&m.exec, &host);
    run(&m, 400000, g, md);
    /* Short budgets: every stop, inside skipped passes too, lands on the same insn. */
    o.hash[1] = state_hash(&m, g, f);
    for (int i = 1; i < 60; i++)
        run(&m, (uint64_t)i * 37, g, md);
    o.hash[2] = state_hash(&m, g, f);

    const uint64_t b = base_of(g);
    o.spin[0] = spin_state(&m, b + LOOP);
    o.spin[1] = spin_state(&m, b + COUNT);
    o.spin[2] = spin_state(&m, b + mmio_at[g]);
    o.spin[3] = spin_state(&m, b + store_at[g]);
    o.blocks = m.exec.stats.blocks_executed;
    o.chained = m.exec.stats.blocks_chained;
    o.interrupts = m.exec.stats.interrupts;
    o.reads = f->reads;
    CHECK(f->fires > 100, "%s %s: the flag changed (%u)", guest_name[g], mode_name[md], f->fires);
    if (g == MIPS)
        CHECK(o.interrupts > 100, "%s %s: interrupts were taken (%llu)", guest_name[g],
              mode_name[md], (unsigned long long)o.interrupts);
    uam_machine_fini(&m);
    unsetenv("UAM_SPIN_SKIP");
    return o;
}

static void test_guest(guest g)
{
    outcome ref = scenario(g, INTERP_NOSKIP);
    uint64_t chained_noskip = 0;
    for (mode md = INTERP; md <= JIT_NOCHAIN; md++) {
        outcome o = scenario(g, md);
        const char *gn = guest_name[g], *mn = mode_name[md];
        for (unsigned i = 0; i < 3; i++)
            CHECK(o.hash[i] == ref.hash[i], "%s %s: checkpoint %u matches the interpreter "
                  "without skipping", gn, mn, i);
        CHECK(o.blocks == ref.blocks, "%s %s: blocks executed %llu, reference %llu", gn, mn,
              (unsigned long long)o.blocks, (unsigned long long)ref.blocks);
        CHECK(o.polls == ref.polls, "%s %s: host polls %llu, reference %llu", gn, mn,
              (unsigned long long)o.polls, (unsigned long long)ref.polls);
        CHECK(o.interrupts == ref.interrupts, "%s %s: interrupts %llu, reference %llu", gn, mn,
              (unsigned long long)o.interrupts, (unsigned long long)ref.interrupts);
        CHECK(o.reads == ref.reads, "%s %s: device reads %llu, reference %llu", gn, mn,
              (unsigned long long)o.reads, (unsigned long long)ref.reads);
        if (skips(md)) {
            CHECK(o.spin[0] >= SPIN_CAND, "%s %s: the spin loop is a candidate (%d)", gn, mn,
                  o.spin[0]);
            CHECK(o.spin[1] == -1, "%s %s: the counting loop is not (%d)", gn, mn, o.spin[1]);
            CHECK(o.spin[2] == -1, "%s %s: the device poll is not (%d)", gn, mn, o.spin[2]);
            CHECK(o.spin[3] == -1, "%s %s: the storing loop is not (%d)", gn, mn, o.spin[3]);
        } else {
            CHECK(o.spin[0] == -1, "%s %s: UAM_SPIN_SKIP=0 skips nothing (%d)", gn, mn, o.spin[0]);
        }
        /* Skipped passes are neither run nor chained: the spin loop takes about half of
         * the blocks, the storing loop most of the rest. */
        if (md == JIT_NOSKIP)
            chained_noskip = o.chained;
        if (md == JIT)
            CHECK(o.chained < chained_noskip / 3 * 2, "%s %s: %llu blocks chained, %llu without "
                  "skipping", gn, mn, (unsigned long long)o.chained,
                  (unsigned long long)chained_noskip);
    }
}

int main(void)
{
    test_guest(RV32);
    test_guest(MIPS);
    printf("test_spin: %d/%d checks passed\n", n_checks - n_failed, n_checks);
    return n_failed ? 1 : 0;
}
