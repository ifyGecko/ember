/*
 * test_oracles.c - The memory oracles: call tracking and the return guard (uam/frames.h),
 * checked bus watches (uam/bus.h) and the guarded heap (uam/heap.h).
 *
 * Call tracking runs one small program per ISA (RV32, MIPS32 BE/LE, ARMv7, PPC32) on the
 * interpreter, the JIT and the JIT without chaining: main calls f, which returns; then g,
 * which changes its return address to one no call returns to and returns. The guard must
 * stop at g's return on every backend, and the frame stack must hold what it should.
 * The heap tests drive the allocator directly and make guest-like accesses through the bus.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uam/frames.h"
#include "uam/heap.h"
#include "uam/isa/arm.h"
#include "uam/isa/mips32.h"
#include "uam/isa/ppc.h"
#include "uam/isa/riscv.h"
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

#define EQ64(a, b, what) do {                                              \
        uint64_t a_ = (uint64_t)(a), b_ = (uint64_t)(b);                   \
        CHECK(a_ == b_, "%s: got 0x%llx, expected 0x%llx", what,           \
              (unsigned long long)a_, (unsigned long long)b_);             \
    } while (0)

/* ---- the call programs ----------------------------------------------------- */

typedef struct prog {
    const char    *name;
    const uam_isa *isa;
    bool           be;
    uint64_t       base, phys;
    uint32_t       w[16];
    unsigned       n;
    uint64_t       f, g;          /* the functions */
    uint64_t       ret_f, ret_g;  /* where their calls return to */
    uint64_t       bad_pc;        /* g's return instruction */
    uint64_t       bad_target;    /* where it goes: no call's return address */
} prog;

static uint32_t rv_jal(unsigned rd, int32_t off)
{
    uint32_t o = (uint32_t)off;
    return ((o >> 20) & 1) << 31 | ((o >> 1) & 0x3FF) << 21 | ((o >> 11) & 1) << 20
         | ((o >> 12) & 0xFF) << 12 | rd << 7 | 0x6F;
}
#define RV_ADDI(rd, rs, imm) (((uint32_t)(imm) & 0xFFF) << 20 | (rs) << 15 | (rd) << 7 | 0x13)
#define RV_RET               0x00008067u   /* jalr x0, ra, 0 */

static prog rv_prog(void)
{
    prog p = { .name = "rv32", .isa = &uam_isa_rv32, .base = 0x1000, .phys = 0x1000 };
    uint32_t *w = p.w;
    w[0] = rv_jal(1, 16);            /* 0   jal ra, f */
    w[1] = rv_jal(1, 24);            /* 4   jal ra, g */
    w[2] = rv_jal(0, 0);             /* 8   j . */
    w[3] = RV_ADDI(0, 0, 0);         /* 12  nop: the bad target */
    w[4] = RV_ADDI(10, 10, 1);       /* 16  f: addi a0, a0, 1 */
    w[5] = RV_RET;                   /* 20  ret */
    w[6] = RV_ADDI(0, 0, 0);
    w[7] = RV_ADDI(1, 1, 4);         /* 28  g: addi ra, ra, 4 */
    w[8] = RV_RET;                   /* 32  ret, to 12 */
    p.n = 9;
    p.f = p.base + 16; p.g = p.base + 28;
    p.ret_f = p.base + 4; p.ret_g = p.base + 8;
    p.bad_pc = p.base + 32; p.bad_target = p.base + 12;
    return p;
}

static prog mips_prog(bool be)
{
    prog p = { .name = be ? "mips32be" : "mips32le", .isa = be ? &uam_isa_mips32be : &uam_isa_mips32le,
               .be = be, .base = 0x80001000u, .phys = 0x1000 };
    uint32_t *w = p.w;
    const uint32_t b = (uint32_t)p.base;
#define MIPS_JAL(a) (0x0C000000u | (((a) >> 2) & 0x03FFFFFFu))
#define MIPS_J(a)   (0x08000000u | (((a) >> 2) & 0x03FFFFFFu))
    w[0] = MIPS_JAL(b + 0x18);       /* 00  jal f */
    w[1] = 0;                        /* 04  nop (delay slot) */
    w[2] = MIPS_JAL(b + 0x24);       /* 08  jal g */
    w[3] = 0;
    w[4] = MIPS_J(b + 0x10);         /* 10  j . */
    w[5] = 0;                        /* 14  nop: the bad target */
    w[6] = 0x03E00008u;              /* 18  f: jr ra */
    w[7] = 0x24840001u;              /* 1c  addiu a0, a0, 1 (delay slot) */
    w[8] = 0;
    w[9] = 0x27FF0004u;              /* 24  g: addiu ra, ra, 4 */
    w[10] = 0x03E00008u;             /* 28  jr ra, to 14 */
    w[11] = 0;
    p.n = 12;
    p.f = p.base + 0x18; p.g = p.base + 0x24;
    p.ret_f = p.base + 0x08; p.ret_g = p.base + 0x10;
    p.bad_pc = p.base + 0x28; p.bad_target = p.base + 0x14;
    return p;
}

static prog arm_prog(void)
{
    prog p = { .name = "armv7", .isa = &uam_isa_arm, .base = 0x1000, .phys = 0x1000 };
    uint32_t *w = p.w;
    w[0] = 0xEB000002u;              /* 00  bl f */
    w[1] = 0xEB000003u;              /* 04  bl g */
    w[2] = 0xEAFFFFFEu;              /* 08  b . */
    w[3] = 0xE1A00000u;              /* 0c  nop: the bad target */
    w[4] = 0xE2800001u;              /* 10  f: add r0, r0, #1 */
    w[5] = 0xE12FFF1Eu;              /* 14  bx lr */
    w[6] = 0xE28EE004u;              /* 18  g: add lr, lr, #4 */
    w[7] = 0xE12FFF1Eu;              /* 1c  bx lr, to 0c */
    p.n = 8;
    p.f = p.base + 0x10; p.g = p.base + 0x18;
    p.ret_f = p.base + 4; p.ret_g = p.base + 8;
    p.bad_pc = p.base + 0x1C; p.bad_target = p.base + 0x0C;
    return p;
}

static prog ppc_prog(void)
{
    prog p = { .name = "ppc32", .isa = &uam_isa_ppc32, .be = true, .base = 0x1000, .phys = 0x1000 };
    uint32_t *w = p.w;
    w[0] = 0x48000011u;              /* 00  bl f (+16) */
    w[1] = 0x48000015u;              /* 04  bl g (+20) */
    w[2] = 0x48000000u;              /* 08  b . */
    w[3] = 0x60000000u;              /* 0c  nop: the bad target */
    w[4] = 0x38630001u;              /* 10  f: addi r3, r3, 1 */
    w[5] = 0x4E800020u;              /* 14  blr */
    w[6] = 0x7D2802A6u;              /* 18  g: mflr r9 */
    w[7] = 0x39290004u;              /* 1c  addi r9, r9, 4 */
    w[8] = 0x7D2803A6u;              /* 20  mtlr r9 */
    w[9] = 0x4E800020u;              /* 24  blr, to 0c */
    p.n = 10;
    p.f = p.base + 0x10; p.g = p.base + 0x18;
    p.ret_f = p.base + 4; p.ret_g = p.base + 8;
    p.bad_pc = p.base + 0x24; p.bad_target = p.base + 0x0C;
    return p;
}

typedef enum { BK_INTERP, BK_JIT, BK_JIT_NOCHAIN } backend;
static const char *const bk_name[] = { "interp", "jit", "jit-nochain" };

static void machine_for(uam_machine *m, const prog *p, backend bk)
{
    CHECK(uam_machine_init(m, p->isa) == UAM_OK, "machine init");
    CHECK(uam_bus_map_ram(&m->bus, "ram", 0, 1 << 20) == UAM_OK, "map ram");
    for (unsigned i = 0; i < p->n; i++) {
        uint8_t b[4];
        for (int k = 0; k < 4; k++)
            b[k] = (uint8_t)(p->be ? p->w[i] >> (24 - 8 * k) : p->w[i] >> (8 * k));
        uam_bus_load_bytes(&m->bus, p->phys + 4 * i, b, 4);
    }
    uam_machine_reset(m, p->base);
    if (bk != BK_INTERP)
        CHECK(uam_exec_enable_jit(&m->exec), "jit");
    m->exec.chain = bk == BK_JIT;
}

static void test_return_guard(const prog *p, backend bk)
{
    uam_machine m = { 0 };
    char tag[64];
    snprintf(tag, sizeof(tag), "%s/%s", p->name, bk_name[bk]);
    machine_for(&m, p, bk);
    CHECK(uam_machine_guard_returns(&m, true, UAM_GUARD_STOP) == UAM_OK, "%s: guard on", tag);
    const uam_stop_reason why = uam_exec_run(&m.exec, 1000);
    CHECK(why == UAM_STOP_GUARD, "%s: stopped with %s, expected the guard", tag, uam_stop_reason_str(why));
    const uam_guard_hit *h = uam_machine_guard_last_hit(&m);
    CHECK(h && h->kind == UAM_GUARD_RETURN, "%s: no return hit", tag);
    if (h) {
        EQ64(h->pc, p->bad_pc, tag);
        EQ64(h->addr, p->bad_target, tag);
        char line[320];
        uam_guard_hit_format(h, line, sizeof(line));
        CHECK(strstr(line, "which no call returns to") != NULL, "%s: report '%s'", tag, line);
    }
    EQ64(m.cpu.pc, p->bad_target, tag);   /* the return happened; the run stopped behind it */
    const uam_frame_state *fs = uam_machine_frames(&m, 0);
    CHECK(fs != NULL, "%s: no frame state", tag);
    if (fs) {
        EQ64(fs->calls, 2, tag);
        EQ64(fs->returns, 2, tag);
        EQ64(fs->bad_returns, 1, tag);
    }
    /* f's frame went with its return; g's is still there: its return matched nothing. */
    uam_frame bt[4];
    const size_t n = uam_machine_backtrace(&m, 0, bt, 4);
    EQ64(n, 1, tag);
    if (n == 1) {
        EQ64(bt[0].ret, p->ret_g, tag);
        EQ64(bt[0].callee, p->g, tag);
    }
    uam_machine_fini(&m);
}

/* Tracking alone reports nothing; a target named as valid is not a hit; LOG goes on. */
static void test_return_modes(const prog *p, backend bk)
{
    char tag[64];
    snprintf(tag, sizeof(tag), "%s/%s", p->name, bk_name[bk]);
    {
        uam_machine m = { 0 };
        machine_for(&m, p, bk);
        CHECK(uam_machine_track_calls(&m, true) == UAM_OK, "%s: tracking on", tag);
        CHECK(uam_exec_run(&m.exec, 200) == UAM_STOP_INSN_LIMIT, "%s: tracking alone stopped the run", tag);
        EQ64(uam_machine_guard_hits(&m), 0, tag);
        CHECK(uam_machine_frames(&m, 0)->bad_returns >= 1, "%s: the bad return was not counted", tag);
        CHECK(uam_machine_track_calls(&m, false) == UAM_OK && !uam_machine_frames(&m, 0), "%s: off", tag);
        CHECK(uam_exec_run(&m.exec, 200) == UAM_STOP_INSN_LIMIT, "%s: runs on without tracking", tag);
        uam_machine_fini(&m);
    }
    {
        uam_machine m = { 0 };
        machine_for(&m, p, bk);
        CHECK(uam_machine_guard_returns(&m, true, UAM_GUARD_STOP) == UAM_OK, "%s: guard on", tag);
        CHECK(uam_machine_add_return_site(&m, p->bad_target) == UAM_OK, "%s: add site", tag);
        CHECK(uam_exec_run(&m.exec, 200) == UAM_STOP_INSN_LIMIT, "%s: a named target stopped the run", tag);
        EQ64(uam_machine_guard_hits(&m), 0, tag);
        uam_machine_fini(&m);
    }
    {
        uam_machine m = { 0 };
        machine_for(&m, p, bk);
        uam_machine_set_guard_log(&m, NULL);
        CHECK(uam_machine_guard_returns(&m, true, UAM_GUARD_LOG) == UAM_OK, "%s: log on", tag);
        CHECK(uam_exec_run(&m.exec, 200) == UAM_STOP_INSN_LIMIT, "%s: LOG stopped the run", tag);
        EQ64(uam_machine_guard_hits(&m), 1, tag);   /* each address once */
        uam_machine_fini(&m);
    }
}

/* The frames and the set of return addresses travel with a snapshot. */
static void test_frames_snapshot(const prog *p, backend bk)
{
    char tag[64];
    snprintf(tag, sizeof(tag), "%s/%s snapshot", p->name, bk_name[bk]);
    uam_machine m = { 0 };
    machine_for(&m, p, bk);
    CHECK(uam_machine_guard_returns(&m, true, UAM_GUARD_STOP) == UAM_OK, "%s: guard on", tag);
    /* Stop inside g: its frame is live. */
    for (int i = 0; i < 50 && m.cpu.pc != p->g; i++)
        uam_exec_run(&m.exec, 1);
    EQ64(m.cpu.pc, p->g, tag);
    uam_frame before[4], after[4];
    const size_t nb = uam_machine_backtrace(&m, 0, before, 4);
    EQ64(nb, 1, tag);
    uam_snapshot *snap = NULL;
    CHECK(uam_snapshot_take(&m, &snap) == UAM_OK, "%s: take", tag);
    CHECK(uam_exec_run(&m.exec, 1000) == UAM_STOP_GUARD, "%s: first run", tag);
    for (int round = 0; round < 3 && snap; round++) {
        CHECK(uam_snapshot_restore(&m, snap) == UAM_OK, "%s: restore", tag);
        const size_t na = uam_machine_backtrace(&m, 0, after, 4);
        CHECK(na == nb && !memcmp(before, after, sizeof(*before) * nb), "%s: frames differ after a restore", tag);
        const uint64_t hits = uam_machine_guard_hits(&m);
        CHECK(uam_exec_run(&m.exec, 1000) == UAM_STOP_GUARD, "%s: run after restore", tag);
        EQ64(uam_machine_guard_hits(&m), hits + 1, tag);
        EQ64(uam_machine_guard_last_hit(&m)->pc, p->bad_pc, tag);
    }
    uam_snapshot_free(snap);

    /* A machine without tracking does not take that snapshot's state, nor the reverse. */
    uam_machine plain = { 0 };
    machine_for(&plain, p, bk);
    uam_snapshot *ps = NULL;
    CHECK(uam_snapshot_take(&plain, &ps) == UAM_OK, "%s: plain take", tag);
    CHECK(ps && uam_snapshot_restore(&m, ps) != UAM_OK, "%s: a snapshot without frames was restored", tag);
    uam_snapshot_free(ps);
    uam_machine_fini(&plain);
    uam_machine_fini(&m);
}

/* ---- the frame state by itself --------------------------------------------- */

static unsigned g_bad;
static uint64_t g_bad_target;

static void on_bad(void *ctx, struct uam_cpu *cpu, uint64_t pc, uint64_t target)
{
    (void)ctx; (void)cpu; (void)pc;
    g_bad++;
    g_bad_target = target;
}

static void test_frame_state(void)
{
    uam_ret_sites *rs = uam_ret_sites_create();
    uam_frame_state *s = calloc(1, sizeof(*s));
    CHECK(rs && s, "alloc");
    s->sites = rs;
    s->pc_mask = ~UINT64_C(1);
    s->guard = true;
    s->bad_return = on_bad;

    for (uint64_t a = 0x1000; a < 0x1000 + 8 * 5000; a += 8)
        CHECK(uam_ret_sites_add(rs, a), "add");
    EQ64(uam_ret_sites_count(rs), 5000, "sites");
    CHECK(uam_ret_sites_add(rs, 0x1000) && uam_ret_sites_count(rs) == 5000, "a second add counts once");
    CHECK(uam_ret_sites_has(rs, 0x1000 + 8 * 4999) && !uam_ret_sites_has(rs, 0x1004), "membership");
    CHECK(!uam_ret_sites_has(rs, UINT64_MAX), "the last address is never a member");

    /* calls and returns in order */
    uam_frame_call(s, 0x1000, 0x5000);
    uam_frame_call(s, 0x1008, 0x6000);
    uam_frame_ret(s, 0x1009, 0x6010);     /* state bit set: masked */
    EQ64(s->n, 1, "pop");
    /* a return to an older frame drops the ones above it */
    uam_frame_call(s, 0x1010, 0x7000);
    uam_frame_call(s, 0x1018, 0x8000);
    uam_frame_ret(s, 0x1000, 0x8010);
    EQ64(s->n, 0, "resync");
    EQ64(s->resyncs, 1, "resync count");
    EQ64(g_bad, 0, "no report yet");
    /* a return to a call's return address with no frame: another task's, fine */
    uam_frame_ret(s, 0x1020, 0x9000);
    EQ64(g_bad, 0, "a known return address");
    /* a return to anything else */
    uam_frame_ret(s, 0x2468AC, 0x9000);
    EQ64(g_bad, 1, "bad return");
    EQ64(g_bad_target, 0x2468AC, "bad target");
    s->guard = false;
    uam_frame_ret(s, 0x2468AC, 0x9000);
    EQ64(g_bad, 1, "no report with the guard off");
    EQ64(s->bad_returns, 2, "but counted");

    /* an interrupt's marker goes at its return, with what the handler left */
    uam_frame_call(s, 0x1000, 0x5000);
    uam_frame_mark(s, 0x5004);
    uam_frame_call(s, 0x1008, 0x6000);
    uam_frame_eret(s);
    EQ64(s->n, 1, "eret pops to the marker");
    uam_frame_eret(s);
    EQ64(s->n, 1, "eret without a marker pops nothing");
    /* a hook that returned for the function it replaced */
    uam_frame_hook_return(s, 0x2000);
    EQ64(s->n, 1, "a hook's jump elsewhere");
    uam_frame_hook_return(s, 0x1001);
    EQ64(s->n, 0, "a hook's return");

    /* deeper than the stack: the innermost half stays */
    for (uint64_t i = 0; i < UAM_FRAME_DEPTH + 10; i++)
        uam_frame_call(s, 0x1000 + 8 * i, 0x5000);
    CHECK(s->n > UAM_FRAME_DEPTH / 2 && s->n <= UAM_FRAME_DEPTH, "depth %u", s->n);
    EQ64(s->f[s->n - 1].ret, 0x1000 + 8 * (UAM_FRAME_DEPTH + 9), "innermost kept");
    uam_frame_reset(s);
    EQ64(s->n, 0, "reset");

    free(s);
    uam_ret_sites_destroy(rs);
}

/* ---- checked watches -------------------------------------------------------- */

typedef struct chk {
    unsigned asked;
    uint64_t lo, hi;   /* refuse accesses that touch [lo, hi) */
} chk;

static bool chk_fn(void *ctx, uint64_t addr, unsigned size, bool is_write, uint64_t val)
{
    chk *c = ctx;
    (void)is_write; (void)val;
    c->asked++;
    return addr < c->hi && addr + size > c->lo;
}

static void test_checked_watch(void)
{
    uam_machine m = { 0 };
    CHECK(uam_machine_init(&m, &uam_isa_rv32) == UAM_OK, "init");
    CHECK(uam_bus_map_ram(&m.bus, "ram", 0, 1 << 20) == UAM_OK, "ram");
    chk c = { .lo = 0x2010, .hi = 0x2020 };
    const int id = uam_bus_add_check(&m.bus, 0x2000, 0x100, UAM_WATCH_READ | UAM_WATCH_WRITE, chk_fn, &c);
    CHECK(id > 0, "add check");
    CHECK(uam_bus_add_check(&m.bus, 0x2000, 0x100, UAM_WATCH_FAULT, chk_fn, &c) < 0, "bad kinds");
    uint64_t v = 0;
    CHECK(uam_bus_write(&m.bus, 0x2000, 4, 0, 0x11223344) == UAM_OK, "allowed write");
    CHECK(uam_bus_read(&m.bus, 0x2000, 4, 0, &v) == UAM_OK && v == 0x11223344, "allowed read");
    EQ64(c.asked, 2, "asked twice");
    CHECK(uam_bus_write(&m.bus, 0x2010, 4, 0, 1) == UAM_ERR_FAULT, "refused write");
    CHECK(uam_bus_read(&m.bus, 0x201C, 4, 0, &v) == UAM_ERR_FAULT, "refused read");
    CHECK(uam_bus_read(&m.bus, 0x2010, 4, UAM_ACC_DEBUG, &v) == UAM_OK && v == 0, "debug reads are not asked");
    EQ64(c.asked, 4, "asked for guest accesses only");
    CHECK(uam_bus_read(&m.bus, 0x3000, 4, 0, &v) == UAM_OK, "outside the watch");
    EQ64(c.asked, 4, "not asked outside");
    CHECK(uam_bus_remove_watch(&m.bus, id), "remove");
    CHECK(uam_bus_write(&m.bus, 0x2010, 4, 0, 1) == UAM_OK, "gone");
    uam_machine_fini(&m);
}

/* ---- the guarded heap ------------------------------------------------------- */

#define ARENA      0x10000u
#define ARENA_SIZE 0x4000u

static uam_heap *heap_for(uam_machine *m, uint64_t quarantine, uam_guard_action action, const char *name)
{
    uam_heap_opts o = UAM_HEAP_OPTS_INIT;
    o.base = ARENA;
    o.size = ARENA_SIZE;
    o.phys = true;
    o.quarantine = quarantine;
    o.action = action;
    o.name = name;
    char err[256];
    uam_heap *h = uam_heap_create(m, &o, err, sizeof(err));
    CHECK(h != NULL, "heap create: %s", err);
    return h;
}

/* A guest-like access: its status, and whether it was a hit of `kind`. */
static bool hits(uam_machine *m, uint64_t addr, unsigned size, bool write, uam_guard_kind kind)
{
    const uint64_t before = uam_machine_guard_hits(m);
    uint64_t v = 0;
    const uam_status st = write ? uam_bus_write(&m->bus, addr, size, 0, 0xA5) : uam_bus_read(&m->bus, addr, size, 0, &v);
    if (uam_machine_guard_hits(m) == before)
        return false;
    const uam_guard_hit *h = uam_machine_guard_last_hit(m);
    return st == UAM_ERR_FAULT && h->kind == kind && h->addr == addr && h->is_write == write;
}

static bool passes(uam_machine *m, uint64_t addr, unsigned size, bool write)
{
    const uint64_t before = uam_machine_guard_hits(m);
    uint64_t v = 0;
    const uam_status st = write ? uam_bus_write(&m->bus, addr, size, 0, 0xA5) : uam_bus_read(&m->bus, addr, size, 0, &v);
    return st == UAM_OK && uam_machine_guard_hits(m) == before;
}

static void heap_machine(uam_machine *m)
{
    CHECK(uam_machine_init(m, &uam_isa_rv32) == UAM_OK, "init");
    CHECK(uam_bus_map_ram(&m->bus, "ram", 0, 1 << 20) == UAM_OK, "ram");
}

static void test_heap_blocks(void)
{
    uam_machine m = { 0 };
    heap_machine(&m);
    uam_heap *h = heap_for(&m, 0, UAM_GUARD_STOP, NULL);
    if (!h)
        return;
    /* Before the first allocation nothing is checked: start-up code may clear the arena. */
    CHECK(passes(&m, ARENA + 0x100, 4, true), "the arena is open before the first allocation");

    const uint64_t a = uam_heap_alloc(h, 24, 0, 0x111);
    const uint64_t b = uam_heap_alloc(h, 100, 0, 0x222);
    const uint64_t c = uam_heap_alloc(h, 1, 64, 0x333);
    const uint64_t z = uam_heap_alloc(h, 0, 0, 0x444);
    CHECK(a >= ARENA + 32 && a % 8 == 0, "first block at 0x%llx", (unsigned long long)a);
    CHECK(b >= a + 24 + 32 && b % 8 == 0, "a redzone between blocks");
    CHECK(c >= b + 100 + 32 && c % 64 == 0, "an aligned block");
    CHECK(z && z != c && z >= c + 1 + 32, "an empty block has an address of its own");
    CHECK(uam_heap_alloc(h, 16, 24, 0) == 0, "an alignment that is no power of two");

    /* inside a live block */
    CHECK(passes(&m, a, 4, true) && passes(&m, a + 20, 4, false) && passes(&m, a + 23, 1, true), "inside a");
    CHECK(passes(&m, b + 96, 4, true), "inside b");
    /* around it */
    CHECK(hits(&m, a + 24, 1, true, UAM_GUARD_HEAP_REDZONE), "one past the end");
    const uam_guard_hit *g = uam_machine_guard_last_hit(&m);
    CHECK(g->start == a && g->len == 24 && g->site[0] == 0x111 && g->size == 1, "the block in the report");
    CHECK(hits(&m, a + 22, 4, false, UAM_GUARD_HEAP_REDZONE), "an access that runs out of the block");
    CHECK(hits(&m, a - 1, 1, true, UAM_GUARD_HEAP_REDZONE), "one before the start");
    CHECK(uam_machine_guard_last_hit(&m)->start == a, "the underflow names the block after it");
    CHECK(hits(&m, a - 2, 4, true, UAM_GUARD_HEAP_REDZONE), "an access that runs into the block");
    CHECK(hits(&m, z, 1, false, UAM_GUARD_HEAP_REDZONE), "an empty block's only byte");
    CHECK(hits(&m, ARENA + ARENA_SIZE - 8, 4, true, UAM_GUARD_HEAP_WILD), "far from any block");
    CHECK(hits(&m, ARENA, 1, true, UAM_GUARD_HEAP_WILD) || hits(&m, ARENA, 1, true, UAM_GUARD_HEAP_REDZONE),
          "the arena's first byte");
    /* outside the arena is not the heap's */
    CHECK(passes(&m, ARENA - 4, 4, true) && passes(&m, ARENA + ARENA_SIZE, 4, true), "outside the arena");
    /* a refused write changed nothing */
    uint64_t v = 0;
    uam_bus_read(&m.bus, a + 24, 1, UAM_ACC_DEBUG, &v);
    EQ64(v, 0, "the refused write did not happen");

    uam_heap_info info;
    CHECK(uam_heap_block(h, b + 50, &info) && info.addr == b && info.size == 100 && info.live
          && info.alloc_site == 0x222, "block lookup");
    CHECK(!uam_heap_block(h, b + 100, &info), "no block past the end");

    /* disarmed */
    uam_heap_arm(h, false);
    CHECK(passes(&m, a + 24, 1, true), "disarmed");
    uam_heap_arm(h, true);
    CHECK(hits(&m, a + 24, 1, true, UAM_GUARD_HEAP_REDZONE), "armed again");

    uam_heap_stats st;
    uam_heap_get_stats(h, &st);
    EQ64(st.live_blocks, 4, "live blocks");
    EQ64(st.live_bytes, 125, "live bytes");
    CHECK(st.hits >= 8 && st.allocs == 5 && st.failed == 1, "counters");
    uam_heap_destroy(h);
    CHECK(passes(&m, ARENA + 0x800, 4, true), "the arena is plain memory again");
    uam_machine_fini(&m);
}

static void test_heap_free(void)
{
    uam_machine m = { 0 };
    heap_machine(&m);
    uam_heap *h = heap_for(&m, 256, UAM_GUARD_STOP, "pool");
    if (!h)
        return;
    const uint64_t a = uam_heap_alloc(h, 64, 0, 0x111);
    const uint64_t b = uam_heap_alloc(h, 64, 0, 0x222);
    CHECK(uam_heap_free(h, 0, 0), "free(0)");
    CHECK(uam_heap_free(h, a, 0x999), "free");
    CHECK(hits(&m, a, 4, false, UAM_GUARD_HEAP_FREED), "use after free");
    const uam_guard_hit *g = uam_machine_guard_last_hit(&m);
    CHECK(g->start == a && g->site[0] == 0x111 && g->site[1] == 0x999 && !strcmp(g->name, "pool"),
          "the freed block and both sites in the report");
    CHECK(hits(&m, a + 60, 4, true, UAM_GUARD_HEAP_FREED), "its last bytes");

    /* double and bad frees: reported, nothing freed */
    uint64_t n0 = uam_machine_guard_hits(&m);
    CHECK(!uam_heap_free(h, a, 0x998), "double free refused");
    g = uam_machine_guard_last_hit(&m);
    CHECK(uam_machine_guard_hits(&m) == n0 + 1 && g->kind == UAM_GUARD_HEAP_DOUBLE_FREE && g->start == a
          && g->site[1] == 0x999, "double free reported");
    CHECK(!uam_heap_free(h, b + 8, 0), "free inside a block refused");
    g = uam_machine_guard_last_hit(&m);
    CHECK(g->kind == UAM_GUARD_HEAP_BAD_FREE && g->start == b && g->addr == b + 8, "bad free reported");
    CHECK(!uam_heap_free(h, ARENA + ARENA_SIZE - 64, 0), "free of nothing refused");
    g = uam_machine_guard_last_hit(&m);
    CHECK(g->kind == UAM_GUARD_HEAP_BAD_FREE && g->len == 0, "wild free reported");
    CHECK(passes(&m, b, 4, true), "b is still live");

    /* The quarantine holds 256 bytes: freed blocks stay poisoned, and their memory is
     * not handed out, until newer frees push them out, oldest first. */
    uint64_t p[6];
    for (int i = 0; i < 6; i++)
        p[i] = uam_heap_alloc(h, 64, 0, 0x300 + (unsigned)i);
    for (int i = 0; i < 3; i++)
        CHECK(uam_heap_free(h, p[i], 0x400 + (unsigned)i), "free %d", i);
    /* a (64) + p0..p2 (192) = 256: all four still held */
    uam_heap_info info;
    CHECK(uam_heap_block(h, a, &info) && !info.live, "a is still quarantined");
    CHECK(uam_heap_free(h, p[3], 0x403), "free 3");
    CHECK(!uam_heap_block(h, a, &info), "the oldest left the quarantine");
    CHECK(uam_heap_block(h, p[0], &info) && !info.live, "the next oldest has not");
    CHECK(hits(&m, a, 1, false, UAM_GUARD_HEAP_WILD) || hits(&m, a, 1, false, UAM_GUARD_HEAP_REDZONE),
          "memory out of quarantine is still poisoned");
    uam_heap_stats st;
    uam_heap_get_stats(h, &st);
    EQ64(st.quarantined_blocks, 4, "quarantined blocks");
    EQ64(st.quarantined_bytes, 256, "quarantined bytes");
    uam_heap_destroy(h);

    /* No quarantine: memory is free at once. */
    uam_heap *q0 = heap_for(&m, UINT64_MAX, UAM_GUARD_STOP, NULL);
    if (q0) {
        const uint64_t x = uam_heap_alloc(q0, 32, 0, 0);
        CHECK(uam_heap_free(q0, x, 0) && !uam_heap_block(q0, x, &info), "freed at once");
        CHECK(!uam_heap_free(q0, x, 0), "a second free is then a bad free");
        CHECK(uam_machine_guard_last_hit(&m)->kind == UAM_GUARD_HEAP_BAD_FREE, "reported as one");
        uam_heap_destroy(q0);
    }
    uam_machine_fini(&m);
}

static void test_heap_realloc(void)
{
    uam_machine m = { 0 };
    heap_machine(&m);
    uam_heap *h = heap_for(&m, 0, UAM_GUARD_STOP, NULL);
    if (!h)
        return;
    const uint64_t a = uam_heap_realloc(h, 0, 16, 0x111);
    CHECK(a != 0, "realloc(0, n) allocates");
    for (unsigned i = 0; i < 16; i++)
        uam_bus_write(&m.bus, a + i, 1, 0, 0x40 + i);
    const uint64_t b = uam_heap_realloc(h, a, 40, 0x222);
    CHECK(b && b != a, "the block moved");
    uint64_t v = 0;
    bool same = true;
    for (unsigned i = 0; i < 16; i++)
        same &= uam_bus_read(&m.bus, b + i, 1, 0, &v) == UAM_OK && v == 0x40 + i;
    CHECK(same, "the contents moved with it");
    CHECK(passes(&m, b + 36, 4, true), "the new size");
    CHECK(hits(&m, a, 1, true, UAM_GUARD_HEAP_FREED), "the old block is freed");
    const uint64_t c = uam_heap_realloc(h, b, 8, 0x333);
    CHECK(c && hits(&m, c + 8, 1, false, UAM_GUARD_HEAP_REDZONE), "shrunk");
    CHECK(uam_heap_realloc(h, c, 0, 0x444) == 0 && hits(&m, c, 1, false, UAM_GUARD_HEAP_FREED), "realloc(p, 0) frees");
    const uint64_t n0 = uam_machine_guard_hits(&m);
    CHECK(uam_heap_realloc(h, c, 8, 0) == 0 && uam_machine_guard_hits(&m) == n0 + 1, "realloc of a freed block");

    /* calloc zeroes, whatever was there */
    uam_heap_arm(h, false);
    for (uint64_t x = ARENA; x < ARENA + ARENA_SIZE; x += 8)
        uam_bus_write(&m.bus, x, 8, 0, UINT64_MAX);
    uam_heap_arm(h, true);
    const uint64_t z = uam_heap_calloc(h, 10, 12, 0);
    bool zero = z != 0;
    for (unsigned i = 0; z && i < 120; i += 4)
        zero &= uam_bus_read(&m.bus, z + i, 4, 0, &v) == UAM_OK && v == 0;
    CHECK(zero, "calloc zeroes");
    CHECK(uam_heap_calloc(h, UINT64_MAX / 2, 4, 0) == 0, "calloc overflow");
    uam_heap_destroy(h);

    /* fill bytes */
    uam_heap_opts o = UAM_HEAP_OPTS_INIT;
    o.base = ARENA; o.size = ARENA_SIZE; o.phys = true; o.fill_alloc = 0xCD; o.fill_free = 0xDD;
    char err[128];
    h = uam_heap_create(&m, &o, err, sizeof(err));
    CHECK(h != NULL, "create: %s", err);
    if (h) {
        const uint64_t f = uam_heap_alloc(h, 8, 0, 0);
        CHECK(uam_bus_read(&m.bus, f, 4, 0, &v) == UAM_OK && v == 0xCDCDCDCD, "fill on alloc");
        uam_heap_free(h, f, 0);
        CHECK(uam_bus_read(&m.bus, f, 4, UAM_ACC_DEBUG, &v) == UAM_OK && v == 0xDDDDDDDD, "fill on free");
        uam_heap_destroy(h);
    }
    uam_machine_fini(&m);
}

static void test_heap_exhaustion(void)
{
    uam_machine m = { 0 };
    heap_machine(&m);
    uam_heap *h = heap_for(&m, ARENA_SIZE, UAM_GUARD_STOP, NULL);   /* quarantine everything */
    if (!h)
        return;
    /* Fill the arena, free it all, and allocate again: quarantined blocks are let go
     * rather than failing, oldest first, and no two live blocks ever overlap. */
    uint64_t p[256];
    unsigned n = 0;
    while (n < 256 && (p[n] = uam_heap_alloc(h, 100, 0, n)) != 0)
        n++;
    CHECK(n > 50 && n < 256, "%u blocks of 100 bytes fit", n);
    CHECK(uam_heap_alloc(h, 100, 0, 0) == 0, "full");
    for (unsigned i = 0; i < n; i++)
        CHECK(p[i] >= ARENA + 32 && p[i] + 100 + 32 <= ARENA + ARENA_SIZE
              && (!i || p[i] >= p[i - 1] + 100 + 32), "block %u is placed with its redzones", i);
    for (unsigned i = 0; i < n; i++)
        uam_heap_free(h, p[i], 0);
    unsigned again = 0;
    uint64_t q[256];
    while (again < 256 && (q[again] = uam_heap_alloc(h, 100, 0, 0)) != 0)
        again++;
    EQ64(again, n, "the same number fit after everything was freed");
    bool apart = true;
    for (unsigned i = 0; i < again; i++)
        for (unsigned k = i + 1; k < again; k++)
            apart &= q[i] + 100 + 32 <= q[k] || q[k] + 100 + 32 <= q[i];
    CHECK(apart, "blocks handed out again keep their redzones");
    CHECK(passes(&m, q[0], 4, true) && hits(&m, q[0] + 100, 1, true, UAM_GUARD_HEAP_REDZONE), "and are checked");
    /* a larger block needs a larger gap */
    uam_heap_free(h, q[3], 0);
    uam_heap_free(h, q[4], 0);
    CHECK(uam_heap_alloc(h, 200, 0, 0) != 0, "two neighbours' memory serves one larger block");
    uam_heap_stats st;
    uam_heap_get_stats(h, &st);
    CHECK(st.failed == 3 && st.peak_bytes == (uint64_t)n * 100, "counters: %llu failed", (unsigned long long)st.failed);
    uam_heap_destroy(h);
    uam_machine_fini(&m);
}

static void test_heap_actions(void)
{
    uam_machine m = { 0 };
    heap_machine(&m);
    uam_machine_set_guard_log(&m, NULL);
    /* LOG: reported, and the access happens. */
    uam_heap *h = heap_for(&m, 0, UAM_GUARD_LOG, "logged");
    if (h) {
        const uint64_t a = uam_heap_alloc(h, 8, 0, 0);
        const uint64_t n0 = uam_machine_guard_hits(&m);
        uint64_t v = 0;
        CHECK(uam_bus_write(&m.bus, a + 8, 1, 0, 0x5A) == UAM_OK, "LOG lets the write through");
        CHECK(uam_machine_guard_hits(&m) == n0 + 1 && uam_machine_guard_last_hit(&m)->action == UAM_GUARD_LOG,
              "and reports it");
        uam_bus_read(&m.bus, a + 8, 1, UAM_ACC_DEBUG, &v);
        EQ64(v, 0x5A, "the write happened");
        uam_heap_destroy(h);
    }
    /* Two heaps side by side; a name twice is refused. */
    uam_heap_opts o = UAM_HEAP_OPTS_INIT;
    o.base = ARENA; o.size = 0x1000; o.phys = true; o.name = "one";
    char err[128];
    uam_heap *h1 = uam_heap_create(&m, &o, err, sizeof(err));
    CHECK(h1 != NULL, "one: %s", err);
    CHECK(uam_heap_create(&m, &o, err, sizeof(err)) == NULL && strstr(err, "already"), "a name twice: %s", err);
    o.base = ARENA + 0x2000; o.name = "two"; o.action = UAM_GUARD_FAULT;
    uam_heap *h2 = uam_heap_create(&m, &o, err, sizeof(err));
    CHECK(h2 != NULL, "two: %s", err);
    if (h1 && h2) {
        const uint64_t a = uam_heap_alloc(h1, 16, 0, 0), b = uam_heap_alloc(h2, 16, 0, 0);
        CHECK(a < ARENA + 0x1000 && b >= ARENA + 0x2000, "each in its arena");
        CHECK(hits(&m, a + 16, 1, true, UAM_GUARD_HEAP_REDZONE) && !strcmp(uam_machine_guard_last_hit(&m)->name, "one"),
              "heap one's hit");
        CHECK(hits(&m, b + 16, 1, true, UAM_GUARD_HEAP_REDZONE) && !strcmp(uam_machine_guard_last_hit(&m)->name, "two")
              && uam_machine_guard_last_hit(&m)->action == UAM_GUARD_FAULT, "heap two's hit");
        CHECK(passes(&m, ARENA + 0x1800, 4, true), "between the arenas");
    }
    uam_heap_destroy(h1);
    uam_heap_destroy(h2);
    /* Bad arenas. */
    o = (uam_heap_opts)UAM_HEAP_OPTS_INIT;
    CHECK(uam_heap_create(&m, &o, err, sizeof(err)) == NULL, "an empty arena");
    o.base = UINT64_MAX - 16; o.size = 64; o.phys = true;
    CHECK(uam_heap_create(&m, &o, err, sizeof(err)) == NULL, "a wrapping arena");
    o.base = ARENA; o.size = 40;
    CHECK(uam_heap_create(&m, &o, err, sizeof(err)) == NULL, "an arena smaller than its redzones");
    o.size = 0x1000; o.align = 24;
    CHECK(uam_heap_create(&m, &o, err, sizeof(err)) == NULL, "a bad alignment");
    uam_machine_fini(&m);
}

/* The heap's blocks follow a snapshot: what a fuzz harness's restore relies on. */
static void test_heap_snapshot(void)
{
    uam_machine m = { 0 };
    heap_machine(&m);
    uam_machine_reset(&m, 0x1000);
    uam_heap *h = heap_for(&m, 4096, UAM_GUARD_STOP, NULL);
    if (!h)
        return;
    const uint64_t a = uam_heap_alloc(h, 24, 0, 0x111);
    const uint64_t b = uam_heap_alloc(h, 24, 0, 0x222);
    uam_heap_free(h, b, 0x333);
    uam_snapshot *snap = NULL;
    CHECK(uam_snapshot_take(&m, &snap) == UAM_OK, "take");
    for (int round = 0; round < 3 && snap; round++) {
        /* an input's worth of changes */
        const uint64_t c = uam_heap_alloc(h, 500, 0, 0x444);
        uam_heap_free(h, a, 0x555);
        CHECK(c && hits(&m, a, 1, true, UAM_GUARD_HEAP_FREED), "round %d: changed", round);
        CHECK(uam_snapshot_restore(&m, snap) == UAM_OK, "restore");
        uam_heap_info info;
        CHECK(uam_heap_block(h, a, &info) && info.live && info.alloc_site == 0x111, "a is live again");
        CHECK(uam_heap_block(h, b, &info) && !info.live && info.free_site == 0x333, "b is freed again");
        CHECK(!uam_heap_block(h, c, &info), "c is gone");
        CHECK(passes(&m, a, 4, true) && hits(&m, b, 1, false, UAM_GUARD_HEAP_FREED)
              && hits(&m, c + 100, 1, true, UAM_GUARD_HEAP_WILD), "and checked as such");
        CHECK(uam_heap_alloc(h, 500, 0, 0x444) == c, "the same address is handed out again");
        CHECK(uam_snapshot_restore(&m, snap) == UAM_OK, "restore again");
        uam_heap_stats st;
        uam_heap_get_stats(h, &st);
        CHECK(st.live_blocks == 1 && st.live_bytes == 24 && st.quarantined_bytes == 24, "counts");
    }
    uam_snapshot_free(snap);
    uam_heap_destroy(h);
    /* Without the heap the machine saves no heap state: snapshots fit again. */
    CHECK(uam_snapshot_take(&m, &snap) == UAM_OK && uam_snapshot_restore(&m, snap) == UAM_OK, "plain again");
    uam_snapshot_free(snap);
    uam_machine_fini(&m);
}

int main(void)
{
    const prog progs[] = { rv_prog(), mips_prog(true), mips_prog(false), arm_prog(), ppc_prog() };
    for (size_t i = 0; i < sizeof(progs) / sizeof(progs[0]); i++) {
        for (int bk = BK_INTERP; bk <= BK_JIT_NOCHAIN; bk++) {
            test_return_guard(&progs[i], (backend)bk);
            test_return_modes(&progs[i], (backend)bk);
            test_frames_snapshot(&progs[i], (backend)bk);
        }
    }
    test_frame_state();
    test_checked_watch();
    test_heap_blocks();
    test_heap_free();
    test_heap_realloc();
    test_heap_exhaustion();
    test_heap_actions();
    test_heap_snapshot();
    printf("test_oracles: %d checks, %d failed\n", n_checks, n_failed);
    return n_failed ? 1 : 0;
}
