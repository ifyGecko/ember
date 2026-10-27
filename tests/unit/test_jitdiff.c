/*
 * test_jitdiff.c - Differential test of the JIT (src/exec/jit_x86.c) against the
 * reference interpreter (src/exec/interp.c), op by op and on random blocks.
 *
 * The interpreter running the unoptimized block is the reference. Each block also
 * runs on the interpreter after uam_optimize, and on the JIT before and after the
 * optimizer, with an empty data-TLB (bridge paths) and a warm one (inline paths).
 * Every run starts from the same state and must end with the same block result, the
 * same register file, RAM, icount and reservation, and the same log of observable
 * events: each device access, sysreg access and state-reading helper call records
 * the address/id, the value and a hash of the register file at that moment, so guest
 * state must also agree at every point where something outside the block can see it.
 *
 * Level A runs every pure op at every valid width with temp and immediate operands
 * over edge and random values. Level C runs fixed shapes the optimizer rewrites
 * (overwritten register writes, flags read back, CSE) over random register files.
 * Level B runs random verified blocks: guards, CONT and
 * BREAK imarks, loads/stores (LE/BE, aligned, exclusive, page-crossing, faulting,
 * MMIO), AMO/CAS, helpers, sysregs, traps, and exits to constant and computed pcs.
 * Each one runs with every instruction deadline, with an exit already requested, and
 * with the device requesting an exit at its first and second access.
 *
 *   test_jitdiff [blocks [seed]]     (default 3000 blocks, seed 1; make jitdiff-soak
 *                                     runs many more)
 *
 * A failure prints the seed and a shrunk block (instructions deleted while the
 * failure persists).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uam/bus.h"
#include "uam/device.h"
#include "uam/exec.h"
#include "uam/ir.h"
#include "uam/isa.h"
#include "uam/sched.h"

#include "../../src/exec/jit.h"

#define REGFILE    256u
#define ADDR_SLOTS 192u          /* regfile bytes 192..255: eight address slots */
#define RAM_BASE   0x10000u
#define RAM_SIZE   0x4000u
#define ROM_BASE   0x30000u
#define ROM_SIZE   0x1000u
#define DEV_BASE   0x40000u
#define DEV_SIZE   0x1000u
#define HOLE       0x50000u
#define ICOUNT0    1000u
#define NONE       UAM_REF_NONE

static int failures, checks;

/* ---- deterministic randomness ------------------------------------------------- */

static uint64_t rng_state;
static uint64_t rnd(void)
{
    uint64_t x = rng_state;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    rng_state = x;
    return x * 0x2545F4914F6CDD1DULL;
}
static unsigned rn(unsigned n) { return (unsigned)(rnd() % n); }
static bool chance(unsigned pct) { return rn(100) < pct; }

static const unsigned widths[] = { 1, 8, 16, 32, 64 };

/* An "interesting" value of width w: an edge case most of the time. */
static uint64_t val(unsigned w)
{
    const uint64_t m = uam_mask(w), smin = UINT64_C(1) << (w - 1);
    const uint64_t edge[] = { 0, 1, 2, m, m - 1, smin, smin - 1, smin + 1, w - 1, w, w + 1,
                              255, 256, 0x10, 0x7F, 0x80, 63, 64, 2 * w };
    if (chance(60))
        return edge[rn(UAM_ARRAY_LEN(edge))] & m;
    return rnd() & m;
}

/* ---- the observable world: hash, event log -------------------------------- */

static uam_cpu  cpu;
static uint8_t  regfile[REGFILE];

static uint64_t regfile_hash(void)
{
    uint64_t h = 1469598103934665603ULL;
    for (unsigned i = 0; i < REGFILE; i++)
        h = (h ^ regfile[i]) * 1099511628211ULL;
    return h;
}

enum { EV_DEV_RD, EV_DEV_WR, EV_SYSRD, EV_SYSWR, EV_HELPER };
typedef struct ev { uint32_t kind, id; uint64_t a, b, h; } ev;
#define MAX_EV 256
static ev       events[MAX_EV];
static unsigned n_ev;

static void log_ev(uint32_t kind, uint32_t id, uint64_t a, uint64_t b)
{
    if (n_ev < MAX_EV)
        events[n_ev] = (ev){ kind, id, a, b, regfile_hash() };
    n_ev++;
}

/* ---- the observer device ---------------------------------------------------- */

static uint64_t dev_counter;   /* part of the saved state */
static unsigned dev_accesses, dev_exit_at;

static void dev_access(void)
{
    if (++dev_accesses == dev_exit_at)
        uam_cpu_request_exit(&cpu);
}
static uam_status dev_read(uam_device *d, uint64_t off, unsigned size, uint64_t *v)
{
    (void)d;
    *v = (dev_counter++ * 0x9E3779B97F4A7C15ULL + off) & uam_mask(8 * size);
    log_ev(EV_DEV_RD, (uint32_t)off, size, *v);
    dev_access();
    return UAM_OK;
}
static uam_status dev_write(uam_device *d, uint64_t off, unsigned size, uint64_t v)
{
    (void)d;
    dev_counter += v;
    log_ev(EV_DEV_WR, (uint32_t)off, size, v);
    dev_access();
    return UAM_OK;
}
static const uam_device_ops dev_ops = {
    .type = "test,observer", .endian = UAM_ACC_LE, .read = dev_read, .write = dev_write,
    .flags = UAM_DEVICE_STATELESS,
};

/* ---- the test ISA: helpers and sysregs ------------------------------------- */

static uint64_t h_pure(void *env, uint64_t a, uint64_t b, uint64_t c, uam_status *st)
{
    (void)env; (void)c; (void)st;
    return a * 3 + b;
}
static uint64_t h_reads(void *env, uint64_t a, uint64_t b, uint64_t c, uam_status *st)
{
    (void)env; (void)b; (void)c; (void)st;
    log_ev(EV_HELPER, 1, a, 0);
    return regfile_hash() ^ a;
}
static uint64_t h_fault(void *env, uint64_t a, uint64_t b, uint64_t c, uam_status *st)
{
    (void)env; (void)b; (void)c;
    log_ev(EV_HELPER, 2, a, 0);
    if ((a & 3) == 3)
        *st = UAM_ERR_FAULT;
    return a + 1;
}
static uint64_t h_writes(void *env, uint64_t a, uint64_t b, uint64_t c, uam_status *st)
{
    (void)env; (void)c; (void)st;
    log_ev(EV_HELPER, 3, a, b);
    regfile[a % ADDR_SLOTS] = (uint8_t)b;
    return 0;
}
static const uam_helper_desc helper_desc[] = {
    { "pure",   2, { 32, 64, 0 }, 64, UAM_HELPER_PURE, h_pure },
    { "reads",  1, { 64, 0, 0 },  32, UAM_HELPER_READS_STATE, h_reads },
    { "fault",  1, { 32, 0, 0 },  16, UAM_HELPER_MAY_FAULT | UAM_HELPER_READS_STATE, h_fault },
    { "writes", 2, { 8, 64, 0 },  0,  UAM_HELPER_WRITES_STATE, h_writes },
};

static uam_status sys_rd(uam_cpu *c, uint32_t id, uint64_t *v)
{
    (void)c;
    log_ev(EV_SYSRD, id, 0, 0);
    if (id > 3)
        return UAM_ERR_INVALID;
    *v = regfile_hash() + id;
    return UAM_OK;
}
static uam_status sys_wr(uam_cpu *c, uint32_t id, uint64_t v)
{
    (void)c;
    log_ev(EV_SYSWR, id, v, 0);
    return id > 3 ? UAM_ERR_INVALID : UAM_OK;
}

static const uam_isa test_isa = {
    .name = "jitdiff", .addr_width = 64, .default_endian = UAM_ACC_LE, .regfile_size = REGFILE,
    .helpers = { helper_desc, UAM_ARRAY_LEN(helper_desc) },
    .sysreg_read = sys_rd, .sysreg_write = sys_wr,
};

/* ---- machine ------------------------------------------------------------------ */

static uam_bus     bus;
static uam_region *ram;
static uam_sched   sched;
static uam_exec    ex;
static uam_device  dev;
static uint64_t    temps[4096];

static void setup(void)
{
    static uint8_t rom[ROM_SIZE];
    for (unsigned i = 0; i < ROM_SIZE; i++)
        rom[i] = (uint8_t)(i * 37 + 11);
    uam_bus_init(&bus);
    uam_device_init(&dev, &dev_ops, "observer");
    if (uam_bus_map_ram_ex(&bus, "ram", RAM_BASE, RAM_SIZE, NULL, &ram) != UAM_OK
        || uam_bus_map_rom(&bus, "rom", ROM_BASE, ROM_SIZE, rom, sizeof(rom)) != UAM_OK
        || uam_bus_map_mmio(&bus, "dev", DEV_BASE, DEV_SIZE, &dev) != UAM_OK) {
        fprintf(stderr, "test_jitdiff: cannot build the bus\n");
        exit(2);
    }
    cpu.regfile = regfile;
    cpu.regfile_size = REGFILE;
    cpu.bus = &bus;
    cpu.isa = &test_isa;
    cpu.sched = &sched;
    uam_sched_init(&sched, &cpu.icount);
    if (uam_exec_init(&ex, &cpu, &sched) != UAM_OK || !uam_exec_enable_jit(&ex)) {
        fprintf(stderr, "test_jitdiff: no JIT on this host\n");
        exit(0);
    }
}

/* Everything a run may change, and what it must agree on. */
typedef struct state {
    uint8_t          regfile[REGFILE];
    uint8_t          ram[RAM_SIZE];
    uint64_t         icount, deadline, dev_counter;
    bool             exit_request;
    uam_reservation  resv;
} state;

typedef struct outcome {
    state            s;
    uam_block_result r;
    ev               ev[MAX_EV];
    unsigned         n_ev;
} outcome;

static state start;      /* the state every run of the current block starts from */
static bool  with_mem;   /* level A blocks touch no memory: skip RAM copies */

static void restore(const state *s)
{
    memcpy(regfile, s->regfile, REGFILE);
    if (with_mem)
        memcpy(ram->host, s->ram, RAM_SIZE);
    cpu.icount = s->icount;
    cpu.icount_deadline = s->deadline;
    cpu.exit_request = s->exit_request;
    cpu.resv = s->resv;
    dev_counter = s->dev_counter;
    dev_accesses = 0;
    n_ev = 0;
}

static void capture(state *s)
{
    memcpy(s->regfile, regfile, REGFILE);
    if (with_mem)
        memcpy(s->ram, ram->host, RAM_SIZE);
    s->icount = cpu.icount;
    s->deadline = cpu.icount_deadline;
    s->exit_request = cpu.exit_request;
    s->resv = cpu.resv;
    s->dev_counter = dev_counter;
}

enum { V_INTERP, V_INTERP_OPT, V_JIT, V_JIT_OPT, V_JIT_OPT_WARM, V__COUNT };
static const char *const vname[] = { "interp", "interp+opt", "jit", "jit+opt", "jit+opt warm" };

static void poison_temps(void)
{
    uint64_t p = rnd();
    for (unsigned i = 0; i < UAM_ARRAY_LEN(temps); i++)
        temps[i] = p + i * 0x9E3779B97F4A7C15ULL;
}

static void run_one(uam_block *blk, bool jit, bool cold, outcome *o)
{
    restore(&start);
    poison_temps();
    if (!jit) {
        o->r = uam_interp_block(&cpu, blk, &test_isa.helpers, temps);
    } else {
        if (cold)
            uam_cpu_dtlb_flush(&cpu);
        uam_jit_chain ch = { .budget = 0, .link = false };
        uam_jit_run(ex.jit, blk, temps, &ch, &o->r);
    }
    capture(&o->s);
    o->n_ev = n_ev;
    memcpy(o->ev, events, sizeof(ev) * (n_ev < MAX_EV ? n_ev : MAX_EV));
}

/* Compare the parts of a block result its kind defines. */
static bool same_result(const uam_block_result *a, const uam_block_result *b)
{
    if (a->kind != b->kind || a->pc != b->pc)
        return false;
    if (a->kind == UAM_BR_TRAP && a->trap_class != b->trap_class)
        return false;
    if (a->kind == UAM_BR_TRAP || a->kind == UAM_BR_FAULT)
        return a->exc.kind == b->exc.kind && a->exc.flags == b->exc.flags
            && a->exc.pc == b->exc.pc && a->exc.parent_pc == b->exc.parent_pc
            && a->exc.cause == b->exc.cause && a->exc.tval == b->exc.tval;
    return true;
}

static bool same_outcome(const outcome *a, const outcome *b, bool verbose)
{
    bool ok = true;
#define DIFF(cond, ...) do { if (cond) { ok = false; if (verbose) { fprintf(stderr, "    "); \
        fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } } while (0)
    DIFF(!same_result(&a->r, &b->r),
         "result: kind %d pc 0x%llx exc %d/%llx/%llx cause %llx tval %llx  vs  kind %d pc 0x%llx exc %d/%llx/%llx cause %llx tval %llx",
         a->r.kind, (unsigned long long)a->r.pc, a->r.exc.kind,
         (unsigned long long)a->r.exc.pc, (unsigned long long)a->r.exc.parent_pc,
         (unsigned long long)a->r.exc.cause, (unsigned long long)a->r.exc.tval,
         b->r.kind, (unsigned long long)b->r.pc, b->r.exc.kind,
         (unsigned long long)b->r.exc.pc, (unsigned long long)b->r.exc.parent_pc,
         (unsigned long long)b->r.exc.cause, (unsigned long long)b->r.exc.tval);
    for (unsigned i = 0; i < REGFILE; i++)
        DIFF(a->s.regfile[i] != b->s.regfile[i], "regfile[%u]: 0x%02x vs 0x%02x", i,
             a->s.regfile[i], b->s.regfile[i]);
    if (with_mem)
        for (unsigned i = 0; i < RAM_SIZE; i++)
            DIFF(a->s.ram[i] != b->s.ram[i], "ram[0x%x]: 0x%02x vs 0x%02x", RAM_BASE + i,
                 a->s.ram[i], b->s.ram[i]);
    DIFF(a->s.icount != b->s.icount, "icount %llu vs %llu", (unsigned long long)a->s.icount,
         (unsigned long long)b->s.icount);
    DIFF(a->s.dev_counter != b->s.dev_counter, "device state differs");
    DIFF(a->s.exit_request != b->s.exit_request, "exit_request %d vs %d", a->s.exit_request,
         b->s.exit_request);
    DIFF(a->s.resv.valid != b->s.resv.valid
         || (a->s.resv.valid && (a->s.resv.addr != b->s.resv.addr || a->s.resv.size != b->s.resv.size)),
         "reservation differs");
    DIFF(a->n_ev != b->n_ev, "%u events vs %u", a->n_ev, b->n_ev);
    for (unsigned i = 0; i < a->n_ev && i < b->n_ev && i < MAX_EV; i++)
        DIFF(memcmp(&a->ev[i], &b->ev[i], sizeof(ev)) != 0,
             "event %u: kind %u id %u a 0x%llx b 0x%llx regs %016llx  vs  kind %u id %u a 0x%llx b 0x%llx regs %016llx",
             i, a->ev[i].kind, a->ev[i].id, (unsigned long long)a->ev[i].a,
             (unsigned long long)a->ev[i].b, (unsigned long long)a->ev[i].h, b->ev[i].kind,
             b->ev[i].id, (unsigned long long)b->ev[i].a, (unsigned long long)b->ev[i].b,
             (unsigned long long)b->ev[i].h);
#undef DIFF
    return ok;
}

static uam_block *clone_block(const uam_block *s)
{
    uam_block *d = malloc(sizeof(*d));
    *d = *s;
    d->backend = NULL;
    d->backend_free = NULL;
    d->insn = malloc(sizeof(uam_insn) * (s->n_insn ? s->n_insn : 1));
    d->temp_width = malloc(s->n_temp ? s->n_temp : 1);
    d->imm = malloc(sizeof(uint64_t) * (s->n_imm ? s->n_imm : 1));
    memcpy(d->insn, s->insn, sizeof(uam_insn) * s->n_insn);
    memcpy(d->temp_width, s->temp_width, s->n_temp);
    memcpy(d->imm, s->imm, sizeof(uint64_t) * s->n_imm);
    return d;
}

static bool verify_block(const uam_block *blk, char *err, size_t n)
{
    uam_verify_ctx vc = { .regfile_size = REGFILE, .helpers = &test_isa.helpers };
    return uam_verify(blk, &vc, err, n);
}

/* Run `raw` (a verified block) every way under the current start state. Returns the
 * first variant that disagrees with the reference, or 0. */
static int diff_block(const uam_block *raw, bool verbose)
{
    static outcome ref, got;
    uam_block *b0 = clone_block(raw), *b1 = clone_block(raw);
    int bad = 0;
    char err[256];
    if (!uam_optimize(b1) || !verify_block(b1, err, sizeof(err))) {
        fprintf(stderr, "test_jitdiff: optimizer produced an invalid block: %s\n", err);
        uam_print_block(stderr, b1, NULL);
        exit(1);
    }
    uam_jit_compile(ex.jit, b0);
    uam_jit_compile(ex.jit, b1);
    run_one(b0, false, false, &ref);
    for (int v = V_INTERP_OPT; v < V__COUNT && !bad; v++) {
        uam_block *blk = v == V_JIT ? b0 : b1;
        if (v >= V_JIT && blk->backend == UAM_JIT_INTERP)
            continue;   /* too large to compile: nothing to compare */
        if (v == V_JIT_OPT_WARM)
            run_one(blk, true, true, &got);   /* fills the data-TLB */
        run_one(blk, v >= V_JIT, v != V_JIT_OPT_WARM, &got);
        if (!same_outcome(&ref, &got, false)) {
            bad = v;
            if (verbose) {
                fprintf(stderr, "  %s disagrees with the interpreter:\n", vname[v]);
                same_outcome(&ref, &got, true);
            }
        }
    }
    uam_block_free(b0);
    uam_block_free(b1);
    return bad;
}

/* Greedily delete instructions while the block still verifies and still fails. */
static uam_block *shrink(uam_block *blk)
{
    char err[256];
    bool progress = true;
    while (progress) {
        progress = false;
        for (uint32_t i = blk->n_insn - 1; i >= 1 && i < blk->n_insn; i--) {
            uam_block *c = clone_block(blk);
            if (c->insn[i].op == UAM_OP_IMARK)
                c->n_guest--;
            memmove(&c->insn[i], &c->insn[i + 1], sizeof(uam_insn) * (c->n_insn - i - 1));
            c->n_insn--;
            if (verify_block(c, err, sizeof(err)) && diff_block(c, false)) {
                uam_block_free(blk);
                blk = c;
                progress = true;
            } else {
                uam_block_free(c);
            }
        }
    }
    return blk;
}

static void report(uam_block *raw, const char *what)
{
    failures++;
    fprintf(stderr, "FAIL %s\n", what);
    raw = shrink(raw);
    fprintf(stderr, "  shrunk block:\n");
    uam_print_block(stderr, raw, NULL);
    fprintf(stderr, "  start: icount %llu deadline %llu exit_request %d device exit at %u\n",
            (unsigned long long)start.icount, (unsigned long long)start.deadline,
            start.exit_request, dev_exit_at);
    diff_block(raw, true);
    uam_block_free(raw);
}

/* ---- level A: every pure op ---------------------------------------------------- */

static bool pure_op(uam_opcode op)
{
    const uam_opinfo *oi = &uam_opinfo_table[op];
    return op != UAM_OP_GET && op != UAM_OP_IMARK && oi->result != UAM_RK_NONE
        && !(oi->props & (UAM_P_EFFECT | UAM_P_GUARD | UAM_P_FAULT));
}

/* A temp holding the w-bit input stored at regfile[off]. */
static uam_ref input(uam_builder *b, unsigned w, uint32_t off)
{
    if (w == 1)
        return uam_unop(b, UAM_OP_TRUNC, 1, uam_get(b, 8, off));
    return uam_get(b, w, off);
}

/* Block: inputs from regfile[0/8/16], op, result zero-extended into regfile[32].
 * `imm_mask` bit k: operand k is an immediate. Returns NULL if it does not verify. */
static uam_block *op_block(uam_builder *b, uam_opcode op, unsigned w, unsigned src_w,
                           unsigned imm_mask, const uint64_t v[3])
{
    const uam_opinfo *oi = &uam_opinfo_table[op];
    const bool conv = op == UAM_OP_ZEXT || op == UAM_OP_SEXT || op == UAM_OP_TRUNC;
    uam_builder_begin(b, 0x1000);
    uam_imark(b, 0x1000, 4);
    uam_ref src[3] = { NONE, NONE, NONE };
    for (unsigned k = 0; k < oi->n_src; k++) {
        unsigned ow = (k == 0 && conv) ? src_w : (k == 0 && op == UAM_OP_SELECT) ? 1 : w;
        src[k] = (imm_mask >> k) & 1 ? uam_imm(b, v[k] & uam_mask(ow)) : input(b, ow, 8 * k);
    }
    unsigned rw = oi->result == UAM_RK_BOOL ? 1 : w;
    uam_ref r = uam_emit(b, op, w, 0, 0, NONE, src[0], src[1], src[2], rw);
    if (rw < 64)
        r = uam_unop(b, UAM_OP_ZEXT, 64, r);
    uam_put(b, 64, 32, r, NONE);
    uam_exit(b, NONE, uam_imm(b, 0x1004), UAM_EXIT_JUMP);
    uam_block *blk = uam_builder_finish(b);
    char err[256];
    if (!verify_block(blk, err, sizeof(err))) {
        uam_block_free(blk);
        return NULL;
    }
    return blk;
}

static void level_a(void)
{
    uam_builder b;
    uam_builder_init(&b, 32);
    with_mem = false;
    dev_exit_at = 0;
    unsigned n_blocks = 0;
    for (int op = 0; op < UAM_OP__COUNT; op++) {
        if (!pure_op((uam_opcode)op))
            continue;
        const uam_opinfo *oi = &uam_opinfo_table[op];
        const bool conv = op == UAM_OP_ZEXT || op == UAM_OP_SEXT || op == UAM_OP_TRUNC;
        for (unsigned wi = 0; wi < UAM_ARRAY_LEN(widths); wi++) {
            for (unsigned si = 0; si < (conv ? UAM_ARRAY_LEN(widths) : 1); si++) {
                const unsigned w = widths[wi], sw = conv ? widths[si] : w;
                for (unsigned form = 0; form < (1u << oi->n_src); form++) {
                    if (conv && (form & 1))
                        continue;   /* a conversion's source must be a temp */
                    for (unsigned it = 0; it < 160; it++) {
                        uint64_t v[3];
                        for (unsigned k = 0; k < 3; k++) {
                            unsigned ow = (k == 0 && conv) ? sw : (k == 0 && op == UAM_OP_SELECT) ? 1 : w;
                            v[k] = val(ow);
                        }
                        uam_block *blk = op_block(&b, (uam_opcode)op, w, sw, form, v);
                        if (!blk)
                            goto next_form;   /* this width / form is not valid IR */
                        memset(start.regfile, 0, REGFILE);
                        for (unsigned k = 0; k < 3; k++)
                            for (unsigned i = 0; i < 8; i++)
                                start.regfile[8 * k + i] = (uint8_t)(v[k] >> (8 * i));
                        start.icount = ICOUNT0;
                        start.deadline = UINT64_MAX;
                        start.exit_request = false;
                        checks++;
                        n_blocks++;
                        if (diff_block(blk, false)) {
                            char what[128];
                            snprintf(what, sizeof(what), "level A: %s.%u (src %u, imm mask %u)",
                                     oi->mnemonic, w, sw, form);
                            report(blk, what);
                            goto next_form;
                        }
                        uam_block_free(blk);
                    }
                next_form:;
                }
            }
        }
    }
    uam_builder_free(&b);
    printf("test_jitdiff: level A: %u op blocks\n", n_blocks);
}

/* ---- level C: shapes the optimizer rewrites ------------------------------------- */

/* Each pattern is built from registers 0..72 of a random register file and must
 * behave the same however the optimizer rewrites it. */
static void pattern(uam_builder *b, unsigned p)
{
    const uam_ref N = NONE;
    uam_builder_begin(b, 0x3000);
    uam_imark(b, 0x3000, 4);
    switch (p) {
    case 0: {   /* a used GET between a PUT and the PUT that overwrites it */
        uam_put(b, 32, 0, uam_get(b, 32, 8), N);
        uam_ref t = uam_get(b, 8, 1);
        uam_put(b, 32, 0, uam_get(b, 32, 16), N);
        uam_put(b, 8, 40, t, N);
        break;
    }
    case 1: {   /* a guarded exit between two writes of a flag */
        uam_ref c = uam_unop(b, UAM_OP_TRUNC, 1, uam_get(b, 8, 64));
        uam_put(b, 8, 65, uam_unop(b, UAM_OP_ZEXT, 8, c), N);
        uam_exit(b, uam_unop(b, UAM_OP_TRUNC, 1, uam_get(b, 8, 66)), uam_imm(b, 0x3100), UAM_EXIT_JUMP);
        uam_put(b, 8, 65, uam_imm(b, 7), N);
        break;
    }
    case 2: {   /* the same conversion to two widths */
        uam_ref t = uam_get(b, 8, 1);
        uam_put(b, 16, 40, uam_unop(b, UAM_OP_SEXT, 16, t), N);
        uam_put(b, 32, 48, uam_unop(b, UAM_OP_SEXT, 32, t), N);
        uam_put(b, 32, 56, uam_unop(b, UAM_OP_ZEXT, 32, t), N);
        break;
    }
    case 3: {   /* a flag written, then read back the ways lifters do */
        uam_ref c = uam_binop(b, UAM_OP_ULT, 32, uam_get(b, 32, 0), uam_get(b, 32, 8));
        uam_put(b, 8, 64, uam_unop(b, UAM_OP_ZEXT, 8, c), N);
        uam_imark(b, 0x3004, 4);
        uam_ref f = uam_get(b, 8, 64);
        uam_ref ne = uam_binop(b, UAM_OP_NE, 8, f, uam_imm(b, 0));
        uam_ref eq = uam_binop(b, UAM_OP_EQ, 8, f, uam_imm(b, 0));
        uam_ref e1 = uam_binop(b, UAM_OP_EQ, 8, f, uam_imm(b, 1));
        uam_ref nn = uam_binop(b, UAM_OP_XOR, 1, ne, uam_imm(b, 1));
        uam_ref tr = uam_unop(b, UAM_OP_TRUNC, 1, f);
        uam_put(b, 32, 32, uam_select(b, 32, nn, uam_get(b, 32, 16), uam_get(b, 32, 24)), N);
        uam_put(b, 8, 68, uam_unop(b, UAM_OP_ZEXT, 8, eq), N);
        uam_put(b, 8, 69, uam_unop(b, UAM_OP_ZEXT, 8, e1), N);
        uam_put(b, 8, 70, uam_unop(b, UAM_OP_ZEXT, 8, uam_select(b, 1, tr, uam_imm(b, 0), uam_imm(b, 1))), N);
        uam_put(b, 8, 71, uam_unop(b, UAM_OP_ZEXT, 8, uam_binop(b, UAM_OP_XOR, 1, nn, uam_imm(b, 1))), N);
        break;
    }
    case 4: {   /* guarded and unguarded writes of one register */
        uam_ref g = uam_unop(b, UAM_OP_TRUNC, 1, uam_get(b, 8, 64));
        uam_put(b, 32, 0, uam_get(b, 32, 8), g);
        uam_put(b, 32, 0, uam_get(b, 32, 16), N);
        uam_put(b, 32, 4, uam_get(b, 32, 24), N);
        uam_put(b, 32, 4, uam_get(b, 32, 32), g);
        break;
    }
    case 5: {   /* a state-reading helper between two writes */
        uam_put(b, 32, 0, uam_get(b, 32, 8), N);
        uam_ref h = uam_helper(b, &test_isa.helpers, 1, uam_get(b, 64, 16), N, N, N);
        uam_put(b, 32, 0, h, N);
        break;
    }
    case 6: {   /* overlapping widths */
        uam_put(b, 64, 0, uam_get(b, 64, 8), N);
        uam_put(b, 32, 0, uam_get(b, 32, 16), N);
        uam_put(b, 32, 24, uam_get(b, 32, 32), N);
        uam_put(b, 64, 24, uam_get(b, 64, 40), N);
        break;
    }
    case 8: {   /* compares read once, as a guard or select condition (fused into the
                 * host flags), across a continuation imark */
        static const uam_opcode cmp[] = { UAM_OP_EQ, UAM_OP_NE, UAM_OP_ULT, UAM_OP_ULE,
                                          UAM_OP_SLT, UAM_OP_SLE, UAM_OP_CARRY, UAM_OP_SBORROW };
        uam_ref c = uam_binop(b, cmp[rn(8)], 32, uam_get(b, 32, 0), uam_get(b, 32, 8));
        uam_imark_cont(b, 0x3004, 4);
        uam_put(b, 32, 16, uam_get(b, 32, 24), c);
        uam_ref d = uam_binop(b, cmp[rn(8)], 8, uam_get(b, 8, 1), uam_get(b, 8, 9));
        uam_put(b, 32, 32, uam_select(b, 32, d, uam_get(b, 32, 40), uam_imm(b, 0x1234)), N);
        uam_ref e = uam_binop(b, cmp[rn(8)], 16, uam_get(b, 16, 2), uam_imm(b, 0x8000));
        uam_imark_cont(b, 0x3008, 4);
        uam_exit(b, e, uam_imm(b, 0x3200), UAM_EXIT_JUMP);
        break;
    }
    default: {  /* commutative operands and repeated subexpressions */
        uam_ref x = uam_get(b, 32, 0), y = uam_get(b, 32, 8);
        uam_ref s1 = uam_binop(b, UAM_OP_ADD, 32, x, y), s2 = uam_binop(b, UAM_OP_ADD, 32, y, x);
        uam_ref c1 = uam_binop(b, UAM_OP_CARRY, 32, x, y), c2 = uam_binop(b, UAM_OP_CARRY, 32, y, x);
        uam_ref k1 = uam_binop(b, UAM_OP_AND, 32, uam_imm(b, 0xFF), x);
        uam_ref k2 = uam_binop(b, UAM_OP_AND, 32, x, uam_imm(b, 0xFF));
        uam_put(b, 32, 16, uam_binop(b, UAM_OP_XOR, 32, s1, s2), N);
        uam_put(b, 8, 20, uam_unop(b, UAM_OP_ZEXT, 8, uam_binop(b, UAM_OP_XOR, 1, c1, c2)), N);
        uam_put(b, 32, 24, uam_binop(b, UAM_OP_SUB, 32, k1, k2), N);
        uam_put(b, 32, 28, s2, N);
        break;
    }
    }
    uam_exit(b, NONE, uam_imm(b, 0x3008), UAM_EXIT_JUMP);
}

static void level_c(void)
{
    uam_builder b;
    uam_builder_init(&b, 32);
    with_mem = false;
    dev_exit_at = 0;
    unsigned n = 0;
    for (unsigned p = 0; p < 9; p++) {
        for (unsigned it = 0; it < 400; it++) {
            pattern(&b, p);
            uam_block *blk = uam_builder_finish(&b);
            char err[256];
            if (!verify_block(blk, err, sizeof(err))) {
                fprintf(stderr, "FAIL level C: pattern %u does not verify: %s\n", p, err);
                failures++;
                uam_block_free(blk);
                break;
            }
            for (unsigned i = 0; i < ADDR_SLOTS; i++)
                start.regfile[i] = chance(40) ? (uint8_t)rn(2) : (uint8_t)val(8);
            start.icount = ICOUNT0;
            start.deadline = UINT64_MAX;
            start.exit_request = false;
            checks++;
            n++;
            if (diff_block(blk, false)) {
                char what[64];
                snprintf(what, sizeof(what), "level C: pattern %u", p);
                report(blk, what);
                break;
            }
            uam_block_free(blk);
        }
    }
    uam_builder_free(&b);
    printf("test_jitdiff: level C: %u pattern runs\n", n);
}

/* ---- level B: random blocks ------------------------------------------------------ */

typedef struct gen {
    uam_builder *b;
    unsigned     aw;
    uam_ref      t[5][48];
    unsigned     nt[5];
    bool         put_seen;   /* the current instruction has written a register */
    uint64_t     pc;
} gen;

static unsigned widx(unsigned w) { return w == 1 ? 0 : w == 8 ? 1 : w == 16 ? 2 : w == 32 ? 3 : 4; }

static void add_temp(gen *g, unsigned w, uam_ref r)
{
    unsigned k = widx(w);
    if (g->nt[k] < 48)
        g->t[k][g->nt[k]++] = r;
    else
        g->t[k][rn(48)] = r;
}

static uam_ref new_value(gen *g, unsigned w);

/* A register offset for a w-bit access: often one of a few hot ones (so writes and
 * reads of the same register meet: forwarding, overwritten PUTs), else anywhere. */
static uint32_t reg_off(unsigned w)
{
    static const uint32_t hot[] = { 0, 8, 16, 24, 64, 65, 66, 67, 68, 72 };
    if (chance(50)) {
        uint32_t o = hot[rn(UAM_ARRAY_LEN(hot))];
        if (o + w / 8 <= ADDR_SLOTS)
            return o;
    }
    return rn(ADDR_SLOTS - w / 8 + 1);
}

/* An operand of width w: an immediate, an existing temp, or a new one. */
static uam_ref operand(gen *g, unsigned w)
{
    unsigned k = widx(w);
    if (chance(25))
        return uam_imm(g->b, val(w));
    if (g->nt[k] && chance(85))
        return g->t[k][rn(g->nt[k])];
    return new_value(g, w);
}

static uam_ref temp_of(gen *g, unsigned w)
{
    unsigned k = widx(w);
    if (g->nt[k] && chance(85))
        return g->t[k][rn(g->nt[k])];
    return new_value(g, w);
}

static uam_ref new_value(gen *g, unsigned w)
{
    uam_ref r;
    if (w == 1) {
        static const uam_opcode cmp[] = { UAM_OP_EQ, UAM_OP_NE, UAM_OP_ULT, UAM_OP_ULE,
                                          UAM_OP_SLT, UAM_OP_SLE, UAM_OP_CARRY, UAM_OP_SBORROW };
        unsigned cw = widths[1 + rn(4)];
        r = uam_binop(g->b, cmp[rn(UAM_ARRAY_LEN(cmp))], cw, temp_of(g, cw), operand(g, cw));
    } else {
        r = uam_get(g->b, w, reg_off(w));
    }
    add_temp(g, w, r);
    return r;
}

static void gen_pure(gen *g)
{
    static const uam_opcode bin8[] = {
        UAM_OP_ADD, UAM_OP_SUB, UAM_OP_MUL, UAM_OP_MULHU, UAM_OP_MULHS, UAM_OP_UDIV, UAM_OP_SDIV,
        UAM_OP_UREM, UAM_OP_SREM, UAM_OP_SHL, UAM_OP_LSHR, UAM_OP_ASHR, UAM_OP_ROTR,
        UAM_OP_ULT, UAM_OP_ULE, UAM_OP_SLT, UAM_OP_SLE, UAM_OP_CARRY, UAM_OP_SCARRY, UAM_OP_SBORROW,
        UAM_OP_AND, UAM_OP_OR, UAM_OP_XOR, UAM_OP_EQ, UAM_OP_NE,
        UAM_OP_ADD, UAM_OP_SUB, UAM_OP_AND, UAM_OP_XOR, UAM_OP_SHL, UAM_OP_LSHR,
    };
    unsigned kind = rn(10);
    if (kind < 6) {                                   /* binary */
        uam_opcode op = bin8[rn(UAM_ARRAY_LEN(bin8))];
        bool logic = op == UAM_OP_AND || op == UAM_OP_OR || op == UAM_OP_XOR
                  || op == UAM_OP_EQ || op == UAM_OP_NE;
        unsigned w = logic && chance(20) ? 1 : widths[1 + rn(4)];
        uam_ref a = operand(g, w);
        uam_ref bb = (op == UAM_OP_SHL || op == UAM_OP_LSHR || op == UAM_OP_ASHR || op == UAM_OP_ROTR)
                     && chance(50) ? uam_imm(g->b, rn(w + 3) & uam_mask(w)) : operand(g, w);
        bool boolres = uam_opinfo_table[op].result == UAM_RK_BOOL;
        add_temp(g, boolres ? 1 : w, uam_binop(g->b, op, w, a, bb));
    } else if (kind < 7) {                            /* unary bit op */
        static const uam_opcode un[] = { UAM_OP_CLZ, UAM_OP_CTZ, UAM_OP_POPCNT, UAM_OP_BSWAP };
        uam_opcode op = un[rn(4)];
        unsigned w = widths[(op == UAM_OP_BSWAP ? 2 : 1) + rn(op == UAM_OP_BSWAP ? 3 : 4)];
        add_temp(g, w, uam_unop(g->b, op, w, operand(g, w)));
    } else if (kind < 9) {                            /* conversion */
        unsigned a = rn(5), c = rn(5);
        if (a == c)
            return;
        unsigned sw = widths[a], w = widths[c];
        uam_opcode op = sw > w ? UAM_OP_TRUNC : chance(50) ? UAM_OP_ZEXT : UAM_OP_SEXT;
        add_temp(g, w, uam_unop(g->b, op, w, temp_of(g, sw)));
    } else {                                          /* select */
        unsigned w = widths[rn(5)];
        add_temp(g, w, uam_select(g->b, w, operand(g, 1), operand(g, w), operand(g, w)));
    }
}

/* A guard: none most of the time, else a 1-bit temp (sometimes a constant). */
static uam_ref guard(gen *g, unsigned pct)
{
    if (!chance(pct))
        return NONE;
    if (chance(10))
        return uam_imm(g->b, rn(2));
    return temp_of(g, 1);
}

/* An address for a `size`-byte access: RAM (aligned, unaligned, crossing a page),
 * ROM, the device, or a hole; constant or computed from an address slot. */
static uam_ref address(gen *g, unsigned size)
{
    uint64_t a;
    switch (rn(10)) {
    case 0: case 1: case 2: a = RAM_BASE + (uint64_t)rn(RAM_SIZE / size) * size; break;
    case 3: a = RAM_BASE + rn(RAM_SIZE - 8); break;
    case 4: a = RAM_BASE + 0x1000u * (1 + rn(4)) - 1 - rn(7); break;  /* the last one leaves RAM */
    case 5: a = ROM_BASE + rn(ROM_SIZE - 8); break;
    case 6: case 7: a = DEV_BASE + (uint64_t)rn(64) * size; break;
    case 8: a = HOLE + rn(0x100); break;
    default:
        a = g->aw == 64 && chance(50) ? 0x123400000000ULL + rn(64) : RAM_BASE + rn(RAM_SIZE - 8);
        break;
    }
    if (chance(50))
        return uam_imm(g->b, a & uam_mask(g->aw));
    /* computed: a slot holding a base address, plus an offset */
    unsigned slot = rn(8);
    uam_ref base = uam_get(g->b, g->aw, ADDR_SLOTS + 8 * slot);
    if (chance(50))
        return base;
    return uam_binop(g->b, UAM_OP_ADD, g->aw, base, uam_imm(g->b, rn(16)));
}

static unsigned acc_flags(void)
{
    unsigned f = chance(50) ? UAM_ACC_LE : UAM_ACC_BE;
    if (chance(20))
        f |= UAM_ACC_ALIGNED;
    return f;
}

/* One op that may fault (only before the instruction's first PUT). */
static void gen_faulting(gen *g)
{
    unsigned w = widths[1 + rn(4)], size = w / 8;
    switch (rn(12)) {
    case 0: case 1: case 2: case 3:
        add_temp(g, w, uam_ld(g->b, w, acc_flags(), address(g, size), guard(g, 20)));
        break;
    case 4: case 5: case 6:
        uam_st(g->b, w, acc_flags(), address(g, size), operand(g, w), guard(g, 20));
        break;
    case 7:   /* exclusive pair pieces */
        if (chance(50))
            add_temp(g, w, uam_ld(g->b, w, acc_flags() | UAM_ACC_X, address(g, size), NONE));
        else
            add_temp(g, 1, uam_stx(g->b, w, acc_flags(), address(g, size), operand(g, w), NONE));
        break;
    case 8:
        if (chance(50))
            add_temp(g, w, uam_amo(g->b, w, acc_flags(), (uam_amo_op)rn(UAM_AMO__COUNT),
                                   address(g, size), operand(g, w), guard(g, 20)));
        else
            add_temp(g, w, uam_cas(g->b, w, acc_flags(), address(g, size), operand(g, w),
                                   operand(g, w), guard(g, 20)));
        break;
    case 9:
        add_temp(g, w, uam_sysrd(g->b, w, chance(85) ? rn(4) : 7, guard(g, 20)));
        break;
    case 10:
        uam_syswr(g->b, w, chance(85) ? rn(4) : 7, operand(g, w), guard(g, 20));
        break;
    default: {
        uam_ref r = uam_helper(g->b, &test_isa.helpers, 2, operand(g, 32), NONE, NONE, guard(g, 20));
        add_temp(g, 16, r);
        if (chance(15))
            uam_trap(g->b, temp_of(g, 1), (uam_trap_class)rn(3), operand(g, g->aw),
                     operand(g, g->aw));
        break;
    }
    }
}

static uam_ref exit_target(gen *g)
{
    if (chance(70))
        return uam_imm(g->b, (g->pc + 4 * rn(8)) & uam_mask(g->aw));
    return uam_get(g->b, g->aw, ADDR_SLOTS + 8 * rn(8));
}

/* One op that cannot fault. */
static void gen_safe(gen *g)
{
    unsigned r = rn(20);
    if (r < 9) {
        gen_pure(g);
    } else if (r < 11) {
        new_value(g, widths[1 + rn(4)]);
    } else if (r < 14) {
        unsigned w = widths[1 + rn(4)];
        uam_put(g->b, w, reg_off(w), operand(g, w), guard(g, 25));
        g->put_seen = true;
    } else if (r < 15) {   /* a flag byte written as a zero-extended boolean */
        uam_put(g->b, 8, 64 + rn(4), uam_unop(g->b, UAM_OP_ZEXT, 8, temp_of(g, 1)), guard(g, 10));
        g->put_seen = true;
    } else if (r < 16) {   /* a flag byte read back as a boolean, the ways lifters do */
        uam_ref f = uam_get(g->b, 8, 64 + rn(4));
        static const uam_opcode cmp[] = { UAM_OP_NE, UAM_OP_EQ };
        uam_ref v = rn(3) == 0 ? uam_unop(g->b, UAM_OP_TRUNC, 1, f)
                  : uam_binop(g->b, cmp[rn(2)], 8, f, uam_imm(g->b, rn(2)));
        if (chance(30))
            v = uam_binop(g->b, UAM_OP_XOR, 1, v, uam_imm(g->b, 1));
        add_temp(g, 1, v);
    } else if (r < 17) {
        uam_exit(g->b, temp_of(g, 1), exit_target(g), UAM_EXIT_JUMP);
    } else if (r < 18) {
        add_temp(g, 64, uam_helper(g->b, &test_isa.helpers, 0, operand(g, 32), operand(g, 64),
                                   NONE, NONE));
    } else if (r < 19) {
        add_temp(g, 32, uam_helper(g->b, &test_isa.helpers, 1, operand(g, 64), NONE, NONE,
                                   guard(g, 20)));
    } else {
        uam_helper(g->b, &test_isa.helpers, 3, operand(g, 8), operand(g, 64), NONE, guard(g, 20));
    }
}

static uam_block *gen_block(uam_builder *b, unsigned aw)
{
    gen g = { .b = b, .aw = aw, .pc = 0x2000 };
    uam_builder_begin(b, g.pc);
    unsigned n_guest = 1 + rn(6);
    for (unsigned k = 0; k < n_guest; k++) {
        if (k > 0 && chance(15))
            uam_imark_cont(b, g.pc, 4);
        else
            uam_imark(b, g.pc, 4);
        if (k > 0 && chance(3))
            b->insn[b->n_insn - 1].flags |= UAM_IMARK_BREAK;
        g.put_seen = false;
        unsigned n1 = rn(4), n2 = 1 + rn(7);
        for (unsigned i = 0; i < n1; i++)
            gen_faulting(&g);
        for (unsigned i = 0; i < n2; i++)
            gen_safe(&g);
        g.pc += 4;
    }
    unsigned t = rn(10);
    if (t < 8)
        uam_exit(b, NONE, exit_target(&g), UAM_EXIT_JUMP);
    else if (t < 9 && !g.put_seen)   /* a trap may fault: not after a register write */
        uam_trap(b, NONE, (uam_trap_class)rn(3), operand(&g, aw), operand(&g, aw));
    else
        uam_wait(b, uam_imm(b, g.pc & uam_mask(aw)));
    return uam_builder_finish(b);
}

static void random_start(void)
{
    for (unsigned i = 0; i < ADDR_SLOTS; i++)
        start.regfile[i] = chance(30) ? (uint8_t)rn(2) : (uint8_t)rnd();
    const uint64_t slot[8] = { RAM_BASE + 0x100, RAM_BASE + 0xFFD, ROM_BASE + 0x10, DEV_BASE,
                               HOLE, RAM_BASE + 0x2FFE, RAM_BASE + RAM_SIZE - 1 - rn(7),
                               RAM_BASE + rn(RAM_SIZE - 16) };
    for (unsigned s = 0; s < 8; s++)
        for (unsigned i = 0; i < 8; i++)
            start.regfile[ADDR_SLOTS + 8 * s + i] = (uint8_t)(slot[s] >> (8 * i));
    for (unsigned i = 0; i < RAM_SIZE; i++)
        start.ram[i] = (uint8_t)rnd();
    start.icount = ICOUNT0;
    start.dev_counter = rnd();
    start.resv = (uam_reservation){ 0 };
    if (chance(30)) {
        start.resv.valid = true;
        start.resv.addr = RAM_BASE + 0x100;
        start.resv.size = 4;
    }
}

static void level_b(unsigned n, uint64_t seed)
{
    uam_builder b32, b64;
    uam_builder_init(&b32, 32);
    uam_builder_init(&b64, 64);
    with_mem = true;
    unsigned made = 0, runs = 0;
    for (unsigned i = 0; i < n && failures < 5; i++) {
        rng_state = seed * 0x9E3779B97F4A7C15ULL + i + 1;
        unsigned aw = chance(50) ? 32 : 64;
        uam_block *raw = gen_block(aw == 32 ? &b32 : &b64, aw);
        char err[256];
        if (!raw || !verify_block(raw, err, sizeof(err))) {
            fprintf(stderr, "test_jitdiff: generator made an invalid block (block %u): %s\n", i, err);
            if (raw)
                uam_print_block(stderr, raw, NULL);
            failures++;
            uam_block_free(raw);
            continue;
        }
        made++;
        random_start();
        /* stop conditions: none, every deadline, an exit already requested, the device
         * requesting one at its first or second access */
        bool bad = false;
        for (unsigned s = 0; s < raw->n_guest + 5 && !bad; s++) {
            start.deadline = UINT64_MAX;
            start.exit_request = false;
            dev_exit_at = 0;
            if (s >= 1 && s <= raw->n_guest + 1)
                start.deadline = ICOUNT0 + s - 1;
            else if (s == raw->n_guest + 2)
                start.exit_request = true;
            else if (s >= raw->n_guest + 3)
                dev_exit_at = s - raw->n_guest - 2;
            checks++;
            runs++;
            if (diff_block(raw, false)) {
                char what[96];
                snprintf(what, sizeof(what), "level B: block %u of seed %llu (aw %u, stop case %u)",
                         i, (unsigned long long)seed, aw, s);
                report(raw, what);
                bad = true;
            }
        }
        if (!bad)
            uam_block_free(raw);
    }
    uam_builder_free(&b32);
    uam_builder_free(&b64);
    printf("test_jitdiff: level B: %u random blocks, %u start states, seed %llu\n", made, runs,
           (unsigned long long)seed);
}

/* ---- privilege: data-TLB entries belong to one privilege level ----------------- */

/* test_isa with a translation that depends on cpu->priv: at level 1 the first RAM
 * page is read-only. */
static uam_status xlate_priv(uam_cpu *c, uint64_t va, uam_xlate_kind kind, uam_xlate *out)
{
    if (c->priv == 1 && kind == UAM_XLATE_STORE && (va >> UAM_PAGE_SHIFT) == (RAM_BASE >> UAM_PAGE_SHIFT)) {
        out->fault = 1;
        return UAM_ERR_XLATE;
    }
    out->paddr = va;
    out->fixed = true;
    return UAM_OK;
}

/* A store that succeeds at level 0 (filling the data-TLB) must fault at level 1, in
 * the JIT as in the interpreter. Both address forms: the constant-address probe and
 * the computed one. */
static void level_priv(void)
{
    uam_isa isa = test_isa;
    isa.translate = xlate_priv;
    cpu.isa = &isa;
    with_mem = true;
    dev_exit_at = 0;
    uam_builder b;
    uam_builder_init(&b, 32);
    for (int computed = 0; computed < 2; computed++) {
        uam_imark(&b, 0x3000, 4);
        uam_ref addr = computed ? uam_get(&b, 32, ADDR_SLOTS) : uam_imm(&b, RAM_BASE + 0x100);
        uam_st(&b, 32, UAM_ACC_LE, addr, uam_get(&b, 32, 0), NONE);
        uam_exit(&b, NONE, uam_imm(&b, 0x3004), UAM_EXIT_JUMP);
        uam_block *blk = uam_builder_finish(&b);
        uam_jit_compile(ex.jit, blk);
        memset(&start, 0, sizeof(start));
        for (unsigned i = 0; i < 4; i++)
            start.regfile[ADDR_SLOTS + i] = (uint8_t)((RAM_BASE + 0x100) >> (8 * i));
        start.icount = ICOUNT0;
        start.deadline = UINT64_MAX;
        static outcome o;
        for (unsigned priv = 0; priv < 2; priv++) {
            cpu.priv = priv;
            for (int jit = 0; jit < 2; jit++) {
                run_one(blk, jit, false, &o);   /* warm: the level-0 run filled the TLB */
                bool fault = o.r.kind == UAM_BR_FAULT && o.r.exc.kind == UAM_EXC_STORE_XLATE;
                checks++;
                if (fault != (priv == 1)) {
                    fprintf(stderr, "FAIL privilege: %s store (%s address) at level %u: %s\n",
                            jit ? "JIT" : "interpreter", computed ? "computed" : "constant", priv,
                            fault ? "faulted" : "did not fault");
                    failures++;
                }
            }
        }
        uam_block_free(blk);
    }
    uam_builder_free(&b);
    cpu.priv = 0;
    cpu.isa = &test_isa;
    uam_cpu_dtlb_flush(&cpu);
    printf("test_jitdiff: privilege: data-TLB per level checked\n");
}

int main(int argc, char **argv)
{
    unsigned n = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0) : 3000;
    uint64_t seed = argc > 2 ? strtoull(argv[2], NULL, 0) : 1;
    setup();
    rng_state = seed;
    level_a();
    level_c();
    level_priv();
    level_b(n, seed);
    uam_exec_fini(&ex);
    printf("test_jitdiff: %d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
