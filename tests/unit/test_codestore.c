/*
 * test_codestore.c - Stores to a page that holds translated code (src/exec/jit_x86.c,
 * emit_code_probe): the JIT keeps them inline only when they touch no 64-byte line
 * that holds code, and the result must equal the interpreter's.
 *
 * An RV32 guest runs from page 0x3000 and keeps its data on the same page: each pass
 * stores to a line next to its code (inline), and every 64 passes it rewrites a
 * function in another line of that page (code since its first call) and calls it: a
 * compressed c.addi whose immediate changes each time, then c.jr ra. The c.addi is
 * rewritten by one store, alternately a misaligned word that starts in a data line and
 * ends in the function's line, and a byte store into it. A store that missed
 * the code bit of its first or its last byte's line would leave the old c.addi
 * running. Runs under the interpreter and the JIT with
 * and without chaining, across a snapshot restore (which cleans every page), and must
 * end in identical state; the JIT must take the bridge only for the stores into code.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uam/isa/riscv.h"
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

enum { RZ = 0, RA = 1, T0 = 5, T1, T2, S0 = 8, S1, T3 = 28, T4, T5 };

static uint32_t rv_i(unsigned op, unsigned f3, unsigned rd, unsigned rs1, int32_t imm)
{
    return ((uint32_t)imm & 0xFFF) << 20 | rs1 << 15 | f3 << 12 | rd << 7 | op;
}
static uint32_t rv_s(unsigned f3, unsigned rs2, unsigned rs1, int32_t off)
{
    return ((uint32_t)off >> 5 & 0x7F) << 25 | rs2 << 20 | rs1 << 15 | f3 << 12
         | ((uint32_t)off & 0x1F) << 7 | 0x23;
}
static uint32_t ADDI(unsigned rd, unsigned rs, int32_t v) { return rv_i(0x13, 0, rd, rs, v); }
static uint32_t ANDI(unsigned rd, unsigned rs, int32_t v) { return rv_i(0x13, 7, rd, rs, v); }
static uint32_t ORI(unsigned rd, unsigned rs, int32_t v)  { return rv_i(0x13, 6, rd, rs, v); }
static uint32_t SLLI(unsigned rd, unsigned rs, unsigned s) { return rv_i(0x13, 1, rd, rs, (int32_t)s); }
static uint32_t SRLI(unsigned rd, unsigned rs, unsigned s) { return rv_i(0x13, 5, rd, rs, (int32_t)s); }
static uint32_t ADD(unsigned rd, unsigned a, unsigned b)  { return b << 20 | a << 15 | rd << 7 | 0x33; }
static uint32_t LUI(unsigned rd, uint32_t imm20)          { return imm20 << 12 | rd << 7 | 0x37; }
static uint32_t SW(unsigned rs2, int32_t off, unsigned rs1) { return rv_s(2, rs2, rs1, off); }
static uint32_t SB(unsigned rs2, int32_t off, unsigned rs1) { return rv_s(0, rs2, rs1, off); }
static uint32_t BNE(unsigned a, unsigned b, uint32_t at, uint32_t to)
{
    uint32_t o = to - at;
    return (o >> 12 & 1) << 31 | (o >> 5 & 0x3F) << 25 | b << 20 | a << 15 | 1u << 12
         | (o >> 1 & 0xF) << 8 | (o >> 11 & 1) << 7 | 0x63;
}
static uint32_t JAL(unsigned rd, uint32_t at, uint32_t to)
{
    uint32_t o = to - at;
    return (o >> 20 & 1) << 31 | (o >> 1 & 0x3FF) << 21 | (o >> 11 & 1) << 20
         | (o >> 12 & 0xFF) << 12 | rd << 7 | 0x6F;
}

#define CODE  0x3000u   /* the program */
#define DATA  0x3800u   /* lines 32..47 of the same page */
#define GEN   0x3C00u   /* line 48: the generated function */
#define LOOP  (CODE + 0x1C)

/*
 * 3000 lui  s0, 4 ; addi s0, s0, -0x800     s0 = DATA
 * 3008 lui  s1, 4 ; addi s1, s1, -0x400     s1 = GEN
 * 3010 lui  t5, 0x80820 ; addi t5, t5, 0x285 ; sw t5, 0(s1)    GEN: c.addi t0, 1; c.jr ra
 * loop (301c):
 *      addi t1, t1, 1
 *      andi t2, t1, 0xFF ; slli t2, t2, 2 ; add t2, t2, s0
 *      sw   t1, 0(t2)                         a data line of the code page
 *      andi t3, t1, 0x3F ; bne t3, zero, loop
 *      srli t4, t1, 6 ; andi t4, t4, 0xF ; addi t4, t4, 1 ; slli t4, t4, 2
 *      ori  t4, t4, 0x281                     t4 = c.addi t0, k (k = 1..16, new each time)
 *      andi t3, t1, 0x40 ; bne t3, zero, byte
 *      slli t4, t4, 16 ; sw t4, -2(s1)        GEN-2..GEN+1: starts in a data line
 *      jal  zero, call
 * byte: sb  t4, 0(s1)                         c.addi's low byte (immediate, rd, op)
 * call: jal ra, GEN
 *      jal  zero, loop
 * Each generation rewrites c.addi with one store, alternating between the two.
 */
static void put_prog(uint8_t *at)
{
    const uint32_t prog[] = {
        LUI(S0, 4), ADDI(S0, S0, -0x800), LUI(S1, 4), ADDI(S1, S1, -0x400),
        /* 0x10 */ LUI(T5, 0x80820), ADDI(T5, T5, 0x285), SW(T5, 0, S1),
        /* 0x1c */ ADDI(T1, T1, 1), ANDI(T2, T1, 0xFF), SLLI(T2, T2, 2), ADD(T2, T2, S0),
        /* 0x2c */ SW(T1, 0, T2), ANDI(T3, T1, 0x3F), BNE(T3, RZ, CODE + 0x34, LOOP),
        /* 0x38 */ SRLI(T4, T1, 6), ANDI(T4, T4, 0xF), ADDI(T4, T4, 1), SLLI(T4, T4, 2),
        /* 0x48 */ ORI(T4, T4, 0x281), ANDI(T3, T1, 0x40), BNE(T3, RZ, CODE + 0x50, CODE + 0x60),
        /* 0x54 */ SLLI(T4, T4, 16), SW(T4, -2, S1), JAL(RZ, CODE + 0x5C, CODE + 0x64),
        /* 0x60 */ SB(T4, 0, S1),
        /* 0x64 */ JAL(RA, CODE + 0x64, GEN), JAL(RZ, CODE + 0x68, LOOP),
    };
    memcpy(at, prog, sizeof(prog));   /* little-endian host */
}

typedef enum mode { INTERP, JIT_NOCHAIN, JIT } mode;
static const char *const mode_name[] = { "interp", "jit (no chain)", "jit" };

static uint64_t state_hash(uam_machine *m)
{
    uint64_t h = UINT64_C(0xcbf29ce484222325);
#define MIX(v) (h = (h ^ (uint64_t)(v)) * UINT64_C(0x100000001b3))
    const uam_region *ram = uam_bus_region(&m->bus, "ram");
    for (uint64_t i = 0; i < 0x8000; i += 8) {
        uint64_t w;
        memcpy(&w, ram->host + i, 8);
        MIX(w);
    }
    for (size_t i = 0; i < m->cpu.regfile_size; i++)
        MIX(m->cpu.regfile[i]);
    MIX(m->cpu.pc);
    MIX(m->cpu.icount);
#undef MIX
    return h;
}

typedef struct outcome {
    uint64_t hash[3], blocks, store_calls;
    uint32_t t0, t1;
} outcome;

static void run(uam_machine *m, uint64_t n, mode md)
{
    uam_stop_reason why = uam_exec_run(&m->exec, n);
    CHECK(why == UAM_STOP_INSN_LIMIT, "%s: stopped by %s", mode_name[md],
          uam_stop_reason_str(why));
}

static outcome scenario(mode md)
{
    outcome o = { 0 };
    uam_machine m;
    CHECK(uam_machine_init(&m, &uam_isa_rv32) == UAM_OK, "machine init");
    m.board_name = "codestore-test";
    CHECK(uam_bus_map_ram(&m.bus, "ram", 0, 1u << 20) == UAM_OK, "map ram");
    const uam_region *ram = uam_bus_region(&m.bus, "ram");
    put_prog(ram->host + CODE);
    uam_machine_reset(&m, CODE);
    if (md != INTERP) {
        CHECK(uam_exec_enable_jit(&m.exec), "jit available");
        m.exec.chain = md == JIT;
    }

    run(&m, 300000, md);
    o.hash[0] = state_hash(&m);
    /* A snapshot cleans every page: stores must go through the bus again first. */
    uam_snapshot *s = NULL;
    CHECK(uam_snapshot_take(&m, &s) == UAM_OK, "take");
    run(&m, 100000, md);
    o.hash[1] = state_hash(&m);
    CHECK(uam_snapshot_restore(&m, s) == UAM_OK, "restore");
    run(&m, 100000, md);
    CHECK(state_hash(&m) == o.hash[1], "%s: the run after restore repeats", mode_name[md]);
    for (int i = 1; i < 40; i++)
        run(&m, (uint64_t)i * 13, md);
    o.hash[2] = state_hash(&m);
    uam_snapshot_free(s);

    o.blocks = m.exec.stats.blocks_executed;
    o.store_calls = m.exec.stats.jit_store_calls;
    o.t0 = (uint32_t)uam_cpu_reg_read(&m.cpu, T0 * 4, 32);
    o.t1 = (uint32_t)uam_cpu_reg_read(&m.cpu, T1 * 4, 32);
    uam_machine_fini(&m);
    return o;
}

int main(void)
{
    outcome ref = scenario(INTERP);
    CHECK(ref.t0 > 1000, "the generated functions ran (t0 = %u)", ref.t0);
    for (mode md = JIT_NOCHAIN; md <= JIT; md++) {
        outcome o = scenario(md);
        for (unsigned i = 0; i < 3; i++)
            CHECK(o.hash[i] == ref.hash[i], "%s: checkpoint %u matches the interpreter",
                  mode_name[md], i);
        CHECK(o.blocks == ref.blocks, "%s: blocks executed %llu, interpreter %llu", mode_name[md],
              (unsigned long long)o.blocks, (unsigned long long)ref.blocks);
        /* One data store per pass; a store into code every 64 passes, a few first
         * stores to a page, and the first stores after the snapshot cleaned the pages. */
        CHECK(o.store_calls < ref.t1 / 8, "%s: %llu bridge stores for %u passes", mode_name[md],
              (unsigned long long)o.store_calls, ref.t1);
    }
    printf("test_codestore: %d/%d checks passed\n", n_checks - n_failed, n_checks);
    return n_failed ? 1 : 0;
}
