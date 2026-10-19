/*
 * test_cov.c - IR passes (pass.h), UAM_OP_COV, the coverage pass (cov.h) and the
 * exception observer (exec.h).
 *
 *   - COV in the IR: verifier rules, printer, optimizer, interpreter semantics
 *     (saturation at 255, prev chaining, no effect without coverage state);
 *   - passes: a no-op pass changes nothing, a counting pass sees every IMARK, a
 *     failing or invalid pass stops the run with an error;
 *   - coverage on a small program with a known loop, on RV32, MIPS32 BE and MIPS32
 *     LE, under the interpreter, the JIT and the JIT without chaining: the expected
 *     hit counts and edge buckets, byte-identical maps across backends, saturation,
 *     block mode, delay-slot and duplicate-site rejection, and runs with coverage
 *     off (or on) indistinguishable from runs without it;
 *   - the exception observer seeing an illegal instruction on each ISA.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uam/isa/mips32.h"
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

#define EQ64(a, b, what) do {                                              \
        uint64_t a_ = (uint64_t)(a), b_ = (uint64_t)(b);                   \
        CHECK(a_ == b_, "%s: got 0x%llx, expected 0x%llx", what,           \
              (unsigned long long)a_, (unsigned long long)b_);             \
    } while (0)

#define MAP_SIZE 4096u

/* The COV rule (docs/uam-isa.md section 7.5), as a reference model. */
static void model_cov(uint8_t *map, uint32_t mask, uint32_t *prev, uint32_t a, uint32_t b)
{
    uint32_t idx = (a ^ *prev) & mask;
    if (map[idx] != 0xff)
        map[idx]++;
    *prev = b;
}

/* ======================================================================== */
/* COV in the IR                                                             */
/* ======================================================================== */

/* imark pc; [cov a, b]; exit pc+4. */
static uam_block *cov_block(uint64_t pc, bool with_cov, uint32_t a, uint32_t b)
{
    uam_builder bd;
    uam_builder_init(&bd, 32);
    uam_builder_begin(&bd, pc);
    uam_imark(&bd, pc, 4);
    if (with_cov)
        uam_emit(&bd, UAM_OP_COV, 32, 0, 0, UAM_REF_NONE, uam_imm(&bd, a), uam_imm(&bd, b),
                 UAM_REF_NONE, 0);
    uam_exit(&bd, UAM_REF_NONE, uam_imm(&bd, pc + 4), UAM_EXIT_JUMP);
    uam_block *blk = uam_builder_finish(&bd);
    uam_builder_free(&bd);
    return blk;
}

static bool verifies(const uam_block *blk, char *err, size_t n)
{
    uam_verify_ctx vc = { .regfile_size = 256 };
    return uam_verify(blk, &vc, err, n);
}

static void test_ir_rules(void)
{
    char err[256];
    uam_block *blk = cov_block(0x100, true, 7, 3);
    CHECK(verifies(blk, err, sizeof(err)), "cov after an imark verifies: %s", err);

    /* Printer. */
    char buf[512] = "";
    FILE *f = fmemopen(buf, sizeof(buf), "w");
    uam_print_block(f, blk, NULL);
    fclose(f);
    CHECK(strstr(buf, "cov.32 7, 3") != NULL, "printer shows cov: %s", buf);

    /* Optimizer: kept, directly after its IMARK. */
    CHECK(uam_optimize(blk), "optimize");
    CHECK(blk->n_insn == 3 && blk->insn[1].op == UAM_OP_COV, "optimizer keeps cov in place");
    CHECK(verifies(blk, err, sizeof(err)), "optimized block verifies: %s", err);

    /* Width, operands, guard. */
    blk->insn[1].width = 64;
    CHECK(!verifies(blk, err, sizeof(err)) && strstr(err, "width"), "cov width must be 32: %s", err);
    blk->insn[1].width = 32;
    blk->insn[1].guard = blk->insn[1].a;
    CHECK(!verifies(blk, err, sizeof(err)), "cov takes no guard");
    blk->insn[1].guard = UAM_REF_NONE;
    uam_block_free(blk);

    /* Not directly after an IMARK. */
    uam_builder bd;
    uam_builder_init(&bd, 32);
    uam_builder_begin(&bd, 0x200);
    uam_imark(&bd, 0x200, 4);
    uam_ref t = uam_get(&bd, 32, 0);
    uam_put(&bd, 32, 4, t, UAM_REF_NONE);
    uam_emit(&bd, UAM_OP_COV, 32, 0, 0, UAM_REF_NONE, uam_imm(&bd, 1), uam_imm(&bd, 2),
             UAM_REF_NONE, 0);
    uam_exit(&bd, UAM_REF_NONE, uam_imm(&bd, 0x204), UAM_EXIT_JUMP);
    blk = uam_builder_finish(&bd);
    CHECK(!verifies(blk, err, sizeof(err)) && strstr(err, "directly follow"),
          "cov away from its imark rejected: %s", err);
    uam_block_free(blk);

    /* After a continuation IMARK (a delay slot). */
    uam_builder_begin(&bd, 0x300);
    uam_imark(&bd, 0x300, 4);
    uam_imark_cont(&bd, 0x304, 4);
    uam_emit(&bd, UAM_OP_COV, 32, 0, 0, UAM_REF_NONE, uam_imm(&bd, 1), uam_imm(&bd, 2),
             UAM_REF_NONE, 0);
    uam_exit(&bd, UAM_REF_NONE, uam_imm(&bd, 0x308), UAM_EXIT_JUMP);
    blk = uam_builder_finish(&bd);
    CHECK(!verifies(blk, err, sizeof(err)) && strstr(err, "non-continuation"),
          "cov after a continuation imark rejected: %s", err);
    uam_block_free(blk);

    /* Temp operands. */
    uam_builder_begin(&bd, 0x400);
    uam_imark(&bd, 0x400, 4);
    blk = NULL;
    uam_ref g = uam_builder_new_temp(&bd, 32);
    uam_emit(&bd, UAM_OP_COV, 32, 0, 0, UAM_REF_NONE, g, uam_imm(&bd, 2), UAM_REF_NONE, 0);
    uam_exit(&bd, UAM_REF_NONE, uam_imm(&bd, 0x404), UAM_EXIT_JUMP);
    blk = uam_builder_finish(&bd);
    CHECK(!verifies(blk, err, sizeof(err)) && strstr(err, "immediates"),
          "cov with a temp operand rejected: %s", err);
    uam_block_free(blk);
    uam_builder_free(&bd);
}

static void test_ir_semantics(void)
{
    uam_machine m;
    CHECK(uam_machine_init(&m, &uam_isa_rv32) == UAM_OK, "machine init");
    uint8_t map[MAP_SIZE] = { 0 };
    uam_cov_state cs = { .map = map, .mask = MAP_SIZE - 1 };
    uint64_t temps[8];

    uam_block *b1 = cov_block(0x100, true, 0x10, 0x5);
    uam_block *b2 = cov_block(0x104, true, 0x20, 0x9);

    /* No coverage state: nothing happens, icount counts the instruction. */
    m.cpu.cov = NULL;
    m.cpu.icount_deadline = UINT64_MAX;
    uint64_t ic = m.cpu.icount;
    uam_block_result r = uam_interp_block(&m.cpu, b1, NULL, temps);
    CHECK(r.kind == UAM_BR_NEXT && r.pc == 0x104, "block result");
    EQ64(m.cpu.icount, ic + 1, "cov does not count as an instruction");

    /* Chaining through prev, then saturation. */
    m.cpu.cov = &cs;
    uam_interp_block(&m.cpu, b1, NULL, temps);
    EQ64(map[0x10], 1, "first hit at a ^ 0");
    EQ64(cs.prev, 5, "prev = b");
    uam_interp_block(&m.cpu, b2, NULL, temps);
    EQ64(map[0x20 ^ 5], 1, "second hit at a ^ prev");
    EQ64(cs.prev, 9, "prev = b again");
    for (int i = 0; i < 300; i++) {
        uam_cov_reset_prev(&cs);
        uam_interp_block(&m.cpu, b1, NULL, temps);
    }
    EQ64(map[0x10], 255, "counter saturates at 255");
    unsigned nz = 0;
    for (unsigned i = 0; i < MAP_SIZE; i++)
        nz += map[i] != 0;
    EQ64(nz, 2, "nothing else touched");

    /* The mask applies to the index. */
    memset(map, 0, sizeof(map));
    uam_cov_reset_prev(&cs);
    uam_block *b3 = cov_block(0x108, true, 0xABCDE123u, 0);
    uam_interp_block(&m.cpu, b3, NULL, temps);
    EQ64(map[0x123], 1, "index masked to the map");

    m.cpu.cov = NULL;
    uam_block_free(b1);
    uam_block_free(b2);
    uam_block_free(b3);
    uam_machine_fini(&m);
}

/* ======================================================================== */
/* A small guest program on three ISAs                                       */
/* ======================================================================== */

/*
 * s0:   x = 0                 site 0
 *       n = N
 * loop: x = x + 1             site 1
 * br:   if (x != n) goto loop site 2        (MIPS: + a delay-slot nop)
 * s3:   y = 7                 site 3
 * done: spin                  a PC hook stops the run here
 */
typedef struct prog {
    const char   *name;
    const uam_isa *isa;
    uint64_t      base;       /* virtual address of the first instruction */
    uint64_t      phys;       /* where it is loaded */
    bool          be;
    uint32_t      w[8];
    unsigned      n;
    uint64_t      site[4];
    uint64_t      delay;      /* a delay-slot pc (MIPS), else 0 */
    uint64_t      done;
    uint64_t      illegal;    /* an illegal instruction word for this ISA */
    uint32_t      xreg;       /* register offset of x */
} prog;

/* RV32 encodings */
static uint32_t RV_ADDI(unsigned rd, unsigned rs1, int32_t imm)
{
    return ((uint32_t)imm & 0xFFF) << 20 | rs1 << 15 | rd << 7 | 0x13;
}
static uint32_t RV_BNE(unsigned rs1, unsigned rs2, int32_t off)
{
    uint32_t o = (uint32_t)off;
    return ((o >> 12) & 1) << 31 | ((o >> 5) & 0x3F) << 25 | rs2 << 20 | rs1 << 15 | 1u << 12
         | ((o >> 1) & 0xF) << 8 | ((o >> 11) & 1) << 7 | 0x63;
}
#define RV_JAL_SELF 0x0000006Fu   /* jal x0, 0 */

/* MIPS32 encodings */
static uint32_t MIPS_ADDIU(unsigned rt, unsigned rs, int16_t v)
{
    return 0x09u << 26 | rs << 21 | rt << 16 | (uint16_t)v;
}
static uint32_t MIPS_BNE(unsigned rs, unsigned rt, int32_t words)
{
    return 0x05u << 26 | rs << 21 | rt << 16 | ((uint32_t)words & 0xFFFF);
}
#define MIPS_B_SELF 0x1000FFFFu   /* beq zero, zero, . */

static prog rv_prog(int n)
{
    enum { T0 = 5, T1 = 6, T2 = 7 };
    prog p = { .name = "rv32", .isa = &uam_isa_rv32, .base = 0x1000, .phys = 0x1000, .be = false,
               .xreg = T0 * 4, .illegal = 0x00000000u };
    p.w[0] = RV_ADDI(T0, 0, 0);
    p.w[1] = RV_ADDI(T1, 0, n);
    p.w[2] = RV_ADDI(T0, T0, 1);
    p.w[3] = RV_BNE(T0, T1, -4);
    p.w[4] = RV_ADDI(T2, 0, 7);
    p.w[5] = RV_JAL_SELF;
    p.n = 6;
    p.site[0] = p.base;
    p.site[1] = p.base + 8;
    p.site[2] = p.base + 12;
    p.site[3] = p.base + 16;
    p.done = p.base + 20;
    return p;
}

static prog mips_prog(int n, bool be)
{
    enum { T0 = 8, T1 = 9, T2 = 10 };
    prog p = { .name = be ? "mips32be" : "mips32le", .isa = be ? &uam_isa_mips32be : &uam_isa_mips32le,
               .base = 0x80001000u, .phys = 0x1000, .be = be, .xreg = T0 * 4,
               .illegal = 0x70000010u /* SPECIAL2 funct 0x10: reserved */ };
    p.w[0] = MIPS_ADDIU(T0, 0, 0);
    p.w[1] = MIPS_ADDIU(T1, 0, (int16_t)n);
    p.w[2] = MIPS_ADDIU(T0, T0, 1);
    p.w[3] = MIPS_BNE(T0, T1, -2);   /* to base+8 */
    p.w[4] = 0;                      /* delay slot: nop */
    p.w[5] = MIPS_ADDIU(T2, 0, 7);
    p.w[6] = MIPS_B_SELF;
    p.w[7] = 0;
    p.n = 8;
    p.site[0] = p.base;
    p.site[1] = p.base + 8;
    p.site[2] = p.base + 12;
    p.site[3] = p.base + 20;
    p.delay = p.base + 16;
    p.done = p.base + 24;
    return p;
}

typedef enum { BK_INTERP, BK_JIT, BK_JIT_NOCHAIN } backend;
static const char *const bk_name[] = { "interp", "jit", "jit-nochain" };

typedef enum { COV_NONE, COV_OFF, COV_SITES, COV_BLOCKS } cov_mode;

typedef struct run_cfg {
    cov_mode            mode;
    const uam_cov_site *sites;
    size_t              n_sites;
    const uam_pass     *pass;       /* an extra pass, or NULL */
    uam_exc_hook_fn     exc_hook;
    void               *exc_ctx;
    bool                illegal_at_s3;  /* replace site 3's instruction by an illegal one */
} run_cfg;

typedef struct run_out {
    uam_stop_reason why;
    uint64_t        pc, icount;
    uint8_t         regs[256];
    uint8_t         map[MAP_SIZE];
    uam_exec_stats  stats;
} run_out;

static uam_hook_action stop_hook(void *ctx, uam_cpu *cpu, uint64_t pc)
{
    (void)ctx; (void)cpu; (void)pc;
    return UAM_HOOK_STOP;
}

static void put_word(uam_machine *m, const prog *p, uint64_t pa, uint32_t w)
{
    uint8_t b[4];
    for (int i = 0; i < 4; i++)
        b[i] = (uint8_t)(p->be ? w >> (24 - 8 * i) : w >> (8 * i));
    CHECK(uam_bus_load_bytes(&m->bus, pa, b, 4) == UAM_OK, "load word");
}

static void run_prog(const prog *p, backend bk, const run_cfg *cfg, run_out *out)
{
    memset(out, 0, sizeof(*out));
    uam_machine m;
    CHECK(uam_machine_init(&m, p->isa) == UAM_OK, "%s: machine init", p->name);
    CHECK(uam_bus_map_ram(&m.bus, "ram", 0, 1 << 20) == UAM_OK, "map ram");
    for (unsigned i = 0; i < p->n; i++)
        put_word(&m, p, p->phys + 4 * i, p->w[i]);
    if (cfg->illegal_at_s3)
        put_word(&m, p, p->phys + (p->site[3] - p->base), (uint32_t)p->illegal);
    uam_machine_reset(&m, p->base);
    if (bk != BK_INTERP)
        CHECK(uam_exec_enable_jit(&m.exec), "jit available");
    m.exec.chain = bk == BK_JIT;
    CHECK(uam_exec_add_pc_hook(&m.exec, p->done, stop_hook, NULL) == UAM_OK, "hook");

    uam_cov_state cs = { .map = out->map, .mask = MAP_SIZE - 1 };
    char err[128];
    switch (cfg->mode) {
    case COV_NONE:
        break;
    case COV_OFF:   /* on, then off again */
        CHECK(uam_exec_set_block_coverage(&m.exec, &cs) == UAM_OK, "blocks on");
        CHECK(uam_exec_set_coverage(&m.exec, NULL, 0, NULL, err, sizeof(err)) == UAM_OK, "off");
        CHECK(m.cpu.cov == NULL && m.exec.n_passes == 0, "coverage off detaches the state");
        break;
    case COV_SITES:
        CHECK(uam_exec_set_coverage(&m.exec, cfg->sites, cfg->n_sites, &cs, err, sizeof(err)) == UAM_OK,
              "set coverage: %s", err);
        break;
    case COV_BLOCKS:
        CHECK(uam_exec_set_block_coverage(&m.exec, &cs) == UAM_OK, "block coverage");
        break;
    }
    if (cfg->pass)
        CHECK(uam_exec_add_pass(&m.exec, cfg->pass) == UAM_OK, "add pass");
    if (cfg->exc_hook)
        uam_exec_set_exc_hook(&m.exec, cfg->exc_hook, cfg->exc_ctx);

    out->why = uam_exec_run(&m.exec, 200000);
    out->pc = m.cpu.pc;
    out->icount = m.cpu.icount;
    memcpy(out->regs, m.cpu.regfile, m.cpu.regfile_size < 256 ? m.cpu.regfile_size : 256);
    out->stats = m.exec.stats;
    uam_machine_fini(&m);
}

static bool same_state(const run_out *a, const run_out *b)
{
    return a->why == b->why && a->pc == b->pc && a->icount == b->icount
        && !memcmp(a->regs, b->regs, sizeof(a->regs));
}

static unsigned map_nonzero(const uint8_t *map)
{
    unsigned n = 0;
    for (unsigned i = 0; i < MAP_SIZE; i++)
        n += map[i] != 0;
    return n;
}

/* The site sequence the program executes for loop count n: 0, (1, 2) x n, 3. */
static void model_run(const uam_cov_site *s, int n, uint8_t *map)
{
    uint32_t prev = 0;
    memset(map, 0, MAP_SIZE);
    model_cov(map, MAP_SIZE - 1, &prev, s[0].a, s[0].b);
    for (int i = 0; i < n; i++) {
        model_cov(map, MAP_SIZE - 1, &prev, s[1].a, s[1].b);
        model_cov(map, MAP_SIZE - 1, &prev, s[2].a, s[2].b);
    }
    model_cov(map, MAP_SIZE - 1, &prev, s[3].a, s[3].b);
}

static uint32_t spread(uint32_t id)
{
    uint64_t v = id;
    v ^= v >> 33;
    v *= UINT64_C(0xff51afd7ed558ccd);
    v ^= v >> 33;
    return (uint32_t)v & (MAP_SIZE - 1);
}

static void test_coverage_on(const prog *p3, const prog *p300)
{
    /* Block counts: B = 0, one bucket per site. */
    uam_cov_site counts[4], edges[4];
    for (unsigned i = 0; i < 4; i++) {
        counts[i] = (uam_cov_site){ .pc = p3->site[i], .a = 100 + i, .b = 0 };
        edges[i]  = (uam_cov_site){ .pc = p3->site[i], .a = i, .b = spread(i) };
    }
    run_out base, out[3], ref;
    run_cfg none = { .mode = COV_NONE };
    run_prog(p3, BK_INTERP, &none, &base);
    CHECK(base.why == UAM_STOP_HOOK && base.pc == p3->done, "%s: runs to done (%s at 0x%llx)",
          p3->name, uam_stop_reason_str(base.why), (unsigned long long)base.pc);
    EQ64(uam_cpu_reg_read(&(uam_cpu){ .regfile = base.regs }, p3->xreg, 32), 3, "loop ran 3 times");

    uint8_t want[MAP_SIZE];
    for (int bk = 0; bk < 3; bk++) {
        run_cfg c = { .mode = COV_SITES, .sites = counts, .n_sites = 4 };
        run_prog(p3, (backend)bk, &c, &out[bk]);
        CHECK(same_state(&out[bk], &base), "%s/%s: coverage changes nothing in the guest",
              p3->name, bk_name[bk]);
        EQ64(out[bk].map[100], 1, "site 0 once");
        EQ64(out[bk].map[101], 3, "site 1 three times");
        EQ64(out[bk].map[102], 3, "site 2 three times");
        EQ64(out[bk].map[103], 1, "site 3 once");
        EQ64(map_nonzero(out[bk].map), 4, "only the sites");
        if (bk)
            CHECK(!memcmp(out[bk].map, out[0].map, MAP_SIZE), "%s/%s: map identical to the interpreter",
                  p3->name, bk_name[bk]);
    }

    /* Edges: the whole map equals the reference model. */
    model_run(edges, 3, want);
    for (int bk = 0; bk < 3; bk++) {
        run_cfg c = { .mode = COV_SITES, .sites = edges, .n_sites = 4 };
        run_prog(p3, (backend)bk, &c, &out[bk]);
        CHECK(!memcmp(out[bk].map, want, MAP_SIZE), "%s/%s: edge buckets match the model",
              p3->name, bk_name[bk]);
    }

    /* Saturation through real execution (300 iterations). */
    for (unsigned i = 0; i < 4; i++)
        counts[i].pc = p300->site[i];
    for (int bk = 0; bk < 3; bk++) {
        run_cfg c = { .mode = COV_SITES, .sites = counts, .n_sites = 4 };
        run_prog(p300, (backend)bk, &c, &out[bk]);
        EQ64(out[bk].map[101], 255, "site 1 saturates");
        EQ64(out[bk].map[103], 1, "site 3 once");
        if (bk)
            CHECK(!memcmp(out[bk].map, out[0].map, MAP_SIZE), "%s/%s: saturated map identical",
                  p300->name, bk_name[bk]);
    }
    /* The loop block links to itself, so COV runs in chained native code too. */
    CHECK(out[BK_JIT].stats.blocks_chained > 0, "%s: the jit run chained (%llu hops)", p300->name,
          (unsigned long long)out[BK_JIT].stats.blocks_chained);

    /* Block mode: backends agree byte for byte; the guest is unaffected. */
    for (int bk = 0; bk < 3; bk++) {
        run_cfg c = { .mode = COV_BLOCKS };
        run_prog(p3, (backend)bk, &c, &out[bk]);
        CHECK(same_state(&out[bk], &base), "%s/%s: block coverage changes nothing", p3->name, bk_name[bk]);
        CHECK(map_nonzero(out[bk].map) > 0, "block mode records something");
        if (bk)
            CHECK(!memcmp(out[bk].map, out[0].map, MAP_SIZE), "%s/%s: block map identical",
                  p3->name, bk_name[bk]);
    }

    /* Coverage turned on and off again: exactly a run without it, every backend. */
    for (int bk = 0; bk < 3; bk++) {
        run_cfg c = { .mode = COV_OFF };
        run_prog(p3, (backend)bk, &c, &ref);
        CHECK(same_state(&ref, &base), "%s/%s: coverage off = no coverage", p3->name, bk_name[bk]);
        EQ64(map_nonzero(ref.map), 0, "nothing counted while off");
        EQ64(ref.stats.blocks_translated, base.stats.blocks_translated, "same blocks");
    }
}

static void test_site_errors(const prog *p)
{
    uam_machine m;
    CHECK(uam_machine_init(&m, p->isa) == UAM_OK, "machine init");
    uint8_t map[MAP_SIZE];
    uam_cov_state cs = { .map = map, .mask = MAP_SIZE - 1 };
    char err[128] = "";
    uam_cov_site dup[2] = { { .pc = 0x40, .a = 1 }, { .pc = 0x40, .a = 2 } };
    CHECK(uam_exec_set_coverage(&m.exec, dup, 2, &cs, err, sizeof(err)) == UAM_ERR_INVALID
          && strstr(err, "0x40"), "duplicate sites rejected: %s", err);
    CHECK(m.cpu.cov == NULL, "a rejected table leaves coverage off");
    uam_cov_state bad = { .map = map, .mask = 1000 };
    CHECK(uam_exec_set_coverage(&m.exec, dup, 1, &bad, err, sizeof(err)) == UAM_ERR_INVALID,
          "map size must be a power of two");

    /* The history shape is checked too. */
    uam_cov_history deep = { .depth = UAM_COV_HIST_MAX + 1 };
    uam_cov_state_init(&bad, map, MAP_SIZE, &deep);
    CHECK(uam_exec_set_coverage(&m.exec, dup, 1, &bad, err, sizeof(err)) == UAM_ERR_INVALID
          && strstr(err, "deeper"), "history deeper than UAM_COV_HIST_MAX rejected: %s", err);
    unsigned bits = 0;
    while ((1u << bits) < MAP_SIZE)
        bits++;
    uam_cov_history wide = { .depth = 2, .rot = { 1, (uint8_t)bits } };
    uam_cov_state_init(&bad, map, MAP_SIZE, &wide);
    CHECK(uam_exec_set_coverage(&m.exec, dup, 1, &bad, err, sizeof(err)) == UAM_ERR_INVALID
          && strstr(err, "below"), "rotation by the full map width rejected: %s", err);
    uam_cov_history ok = { .depth = 2, .rot = { 1, (uint8_t)(bits - 1) } };
    uam_cov_state nobits = { .map = map, .mask = MAP_SIZE - 1, .depth = 2, .rot = { 1, 2 } };
    CHECK(uam_exec_set_coverage(&m.exec, dup, 1, &nobits, err, sizeof(err)) == UAM_ERR_INVALID
          && strstr(err, "bits"), "rotation without bits (no uam_cov_state_init) rejected: %s", err);
    uam_cov_state good;
    uam_cov_state_init(&good, map, MAP_SIZE, &ok);
    CHECK(uam_exec_set_coverage(&m.exec, dup, 1, &good, err, sizeof(err)) == UAM_OK,
          "a valid history shape is accepted: %s", err);

    /* Reset forgets the whole history, not just the newest value. */
    for (unsigned j = 0; j < UAM_COV_HIST_MAX; j++)
        good.hist[j] = 0x100 + j;
    uam_cov_reset_prev(&good);
    unsigned left = 0;
    for (unsigned j = 0; j < UAM_COV_HIST_MAX; j++)
        left += good.hist[j] != 0;
    CHECK(left == 0 && good.prev == 0, "uam_cov_reset_prev clears all %u history values",
          UAM_COV_HIST_MAX);

    /* uam_cov_count, the reference: depth 2, no rotation is N=3 XOR folding. */
    uint8_t m2[16] = { 0 };
    uam_cov_history d2 = { .depth = 2 };
    uam_cov_state r;
    uam_cov_state_init(&r, m2, 16, &d2);
    uam_cov_count(&r, 1, 3);       /* idx 1 ^ 0 ^ 0           */
    uam_cov_count(&r, 2, 5);       /* idx 2 ^ 3 ^ 0 = 1       */
    uam_cov_count(&r, 4, 9);       /* idx 4 ^ 5 ^ 3 = 2       */
    CHECK(m2[1] == 2 && m2[2] == 1 && r.hist[0] == 9 && r.hist[1] == 5,
          "depth-2 history folds the last two B values");
    uam_exec_set_coverage(&m.exec, NULL, 0, NULL, NULL, 0);
    uam_machine_fini(&m);

    if (!p->delay)
        return;
    /* A delay-slot site cannot be instrumented: the run stops with an error. */
    uam_cov_site ds = { .pc = p->delay, .a = 1 };
    run_cfg c = { .mode = COV_SITES, .sites = &ds, .n_sites = 1 };
    run_out out;
    fprintf(stderr, "(expected delay-slot error follows)\n");
    run_prog(p, BK_INTERP, &c, &out);
    CHECK(out.why == UAM_STOP_ERROR, "%s: delay-slot site stops the run (%s)", p->name,
          uam_stop_reason_str(out.why));
}

/* ======================================================================== */
/* Passes                                                                    */
/* ======================================================================== */

static uam_status noop_run(void *ctx, uam_block *blk) { (void)ctx; (void)blk; return UAM_OK; }

static uam_status count_run(void *ctx, uam_block *blk)
{
    uint64_t *n = ctx;
    for (uint32_t i = 0; i < blk->n_insn; i++)
        *n += blk->insn[i].op == UAM_OP_IMARK;
    return UAM_OK;
}

static uam_status fail_run(void *ctx, uam_block *blk) { (void)ctx; (void)blk; return UAM_ERR_INVALID; }

/* Breaks the IR: moves the block's terminator to the front. */
static uam_status break_run(void *ctx, uam_block *blk)
{
    (void)ctx;
    uam_insn t = blk->insn[blk->n_insn - 1];
    uint32_t at = 0;
    return uam_block_insert(blk, &at, &t, 1);
}

static void test_passes(const prog *p)
{
    run_out base, out;
    run_cfg none = { .mode = COV_NONE };
    run_prog(p, BK_INTERP, &none, &base);

    for (int bk = 0; bk < 3; bk++) {
        uam_pass noop = { .name = "noop", .run = noop_run };
        run_cfg c = { .mode = COV_NONE, .pass = &noop };
        run_prog(p, (backend)bk, &c, &out);
        CHECK(same_state(&out, &base), "%s/%s: a no-op pass changes nothing", p->name, bk_name[bk]);

        uint64_t n_imark = 0;
        uam_pass count = { .name = "count", .run = count_run, .ctx = &n_imark };
        c.pass = &count;
        run_prog(p, (backend)bk, &c, &out);
        CHECK(same_state(&out, &base), "%s/%s: a counting pass changes nothing", p->name, bk_name[bk]);
        CHECK(n_imark > 0 && n_imark >= p->n - 1, "%s/%s: the pass saw %llu imarks", p->name,
              bk_name[bk], (unsigned long long)n_imark);
    }
    uam_pass fail = { .name = "failing", .run = fail_run };
    run_cfg c = { .mode = COV_NONE, .pass = &fail };
    fprintf(stderr, "(expected pass errors follow)\n");
    run_prog(p, BK_INTERP, &c, &out);
    EQ64(out.why, UAM_STOP_ERROR, "a failing pass stops the run");
    uam_pass brk = { .name = "breaking", .run = break_run };
    c.pass = &brk;
    run_prog(p, BK_INTERP, &c, &out);
    EQ64(out.why, UAM_STOP_ERROR, "a pass producing invalid IR stops the run");

    /* Registration. */
    uam_machine m;
    CHECK(uam_machine_init(&m, p->isa) == UAM_OK, "machine init");
    uam_pass a = { .name = "a", .run = noop_run }, b = { .name = "b", .run = noop_run };
    CHECK(uam_exec_add_pass(&m.exec, &a) == UAM_OK, "add a");
    CHECK(uam_exec_add_pass(&m.exec, &a) == UAM_ERR_INVALID, "adding twice is rejected");
    CHECK(uam_exec_add_pass(&m.exec, &b) == UAM_OK, "add b");
    CHECK(m.exec.passes[0] == &a && m.exec.passes[1] == &b, "registration order");
    CHECK(uam_exec_remove_pass(&m.exec, &a), "remove a");
    CHECK(!uam_exec_remove_pass(&m.exec, &a), "remove a again");
    CHECK(m.exec.n_passes == 1 && m.exec.passes[0] == &b, "b remains");
    uam_machine_fini(&m);
}

/* ======================================================================== */
/* Exception observer                                                        */
/* ======================================================================== */

typedef struct exc_seen {
    unsigned        n;
    uam_exception   e;
    uint64_t        cpu_pc;
    uam_hook_action act;
} exc_seen;

static uam_hook_action exc_obs(void *ctx, uam_cpu *cpu, const uam_exception *e)
{
    exc_seen *s = ctx;
    if (!s->n++) {
        s->e = *e;
        s->cpu_pc = cpu->pc;
    }
    return s->act;
}

static void test_exc_hook(const prog *p)
{
    for (int bk = 0; bk < 3; bk++) {
        exc_seen s = { .act = UAM_HOOK_STOP };
        run_cfg c = { .mode = COV_NONE, .exc_hook = exc_obs, .exc_ctx = &s, .illegal_at_s3 = true };
        run_out out;
        run_prog(p, (backend)bk, &c, &out);
        EQ64(out.why, UAM_STOP_EXC_HOOK, "the observer stops the run");
        EQ64(s.n, 1, "seen once");
        EQ64(s.e.kind, UAM_EXC_TRAP, "an illegal instruction is a TRAP exception");
        EQ64(s.e.pc, p->site[3], "at the illegal instruction");
        EQ64(s.cpu_pc, p->site[3], "cpu->pc is precise");
        EQ64(out.pc, p->site[3], "the run stops there");
        EQ64(out.stats.exceptions, 0, "not delivered");

        /* CONTINUE: delivered as usual (the handler at the vector is not ours, so the
         * run goes on until the instruction budget or a fault loop; only the first
         * report matters here). */
        s = (exc_seen){ .act = UAM_HOOK_CONTINUE };
        run_prog(p, (backend)bk, &c, &out);
        CHECK(s.n >= 1 && s.e.pc == p->site[3], "%s/%s: continue: the exception is reported",
              p->name, bk_name[bk]);
        CHECK(out.stats.exceptions >= 1, "and delivered");
    }
}

int main(void)
{
    test_ir_rules();
    test_ir_semantics();

    const prog progs3[3] = { rv_prog(3), mips_prog(3, true), mips_prog(3, false) };
    const prog progs300[3] = { rv_prog(300), mips_prog(300, true), mips_prog(300, false) };
    for (int i = 0; i < 3; i++) {
        test_coverage_on(&progs3[i], &progs300[i]);
        test_site_errors(&progs3[i]);
        test_passes(&progs3[i]);
        test_exc_hook(&progs3[i]);
    }

    printf("test_cov: %d/%d checks passed\n", n_checks - n_failed, n_checks);
    return n_failed ? 1 : 0;
}
