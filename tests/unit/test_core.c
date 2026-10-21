/*
 * test_core.c - Unit tests for the bus (priorities, device byte order and
 * access widths, ROMD, aliases, watchpoints), the scheduler under resets,
 * device-requested machine resets, executor PC hooks, and the executor's
 * virtual-time and livelock bounds, and the ABI handshake.
 *
 * The machine-level tests run tiny hand-encoded MIPS32 big-endian programs.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "uam/bus.h"
#include "uam/dev/unimp.h"
#include "uam/device.h"
#include "uam/isa/mips32.h"
#include "uam/machine.h"
#include "uam/signal.h"

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

static uint64_t rd(uam_bus *bus, uint64_t a, unsigned size, unsigned attrs)
{
    uint64_t v = 0xDEAD;
    uam_status st = uam_bus_read(bus, a, size, attrs, &v);
    CHECK(st == UAM_OK, "read 0x%llx/%u failed: %s", (unsigned long long)a, size, uam_status_str(st));
    return v;
}

static void wr(uam_bus *bus, uint64_t a, unsigned size, unsigned attrs, uint64_t v)
{
    uam_status st = uam_bus_write(bus, a, size, attrs, v);
    CHECK(st == UAM_OK, "write 0x%llx/%u failed: %s", (unsigned long long)a, size, uam_status_str(st));
}

/* ------------------------------------------------------------------------ */
/* A register-file test device: 4 x 32-bit registers, configurable ops.      */
/* ------------------------------------------------------------------------ */

typedef struct regdev {
    uam_device dev;
    uint32_t   reg[4];
    unsigned   reads, writes, masked_writes;
    uint64_t   last_mask;
} regdev;

static regdev *to_rd(uam_device *d) { return UAM_CONTAINER_OF(d, regdev, dev); }

static uam_status regdev_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    regdev *r = to_rd(d);
    r->reads++;
    if (size != 4 || (off & 3) || off >= 16)
        return UAM_ERR_FAULT;   /* only aligned 32-bit accesses: the bus must adapt */
    *val = r->reg[off / 4];
    return UAM_OK;
}

static uam_status regdev_write(uam_device *d, uint64_t off, unsigned size, uint64_t val)
{
    regdev *r = to_rd(d);
    r->writes++;
    if (size != 4 || (off & 3) || off >= 16)
        return UAM_ERR_FAULT;
    r->reg[off / 4] = (uint32_t)val;
    return UAM_OK;
}

static uam_status regdev_write_masked(uam_device *d, uint64_t off, unsigned size, uint64_t val,
                                      uint64_t mask)
{
    regdev *r = to_rd(d);
    r->masked_writes++;
    r->last_mask = mask;
    if (size != 4 || (off & 3) || off >= 16)
        return UAM_ERR_FAULT;
    r->reg[off / 4] = (uint32_t)((r->reg[off / 4] & ~mask) | (val & mask));
    return UAM_OK;
}

static void regdev_destroy(uam_device *d) { free(to_rd(d)); }

static const uam_device_ops regdev_be_ops = {
    .type = "test,regdev-be", .endian = UAM_ACC_BE, .impl_min = 4, .impl_max = 4,
    .read = regdev_read, .write = regdev_write, .destroy = regdev_destroy,
};
static const uam_device_ops regdev_be_masked_ops = {
    .type = "test,regdev-be-masked", .endian = UAM_ACC_BE, .impl_min = 4, .impl_max = 4,
    .read = regdev_read, .write = regdev_write, .write_masked = regdev_write_masked,
    .destroy = regdev_destroy,
};
static const uam_device_ops regdev_le_ops = {
    .type = "test,regdev-le", .impl_min = 4, .impl_max = 4,
    .read = regdev_read, .write = regdev_write, .destroy = regdev_destroy,
};

static regdev *regdev_new(const uam_device_ops *ops)
{
    regdev *r = calloc(1, sizeof(*r));
    uam_device_init(&r->dev, ops, ops->type);
    return r;
}

/* ------------------------------------------------------------------------ */
/* #5 device byte order                                                      */
/* ------------------------------------------------------------------------ */

static void test_device_endian(void)
{
    uam_bus bus;
    uam_bus_init(&bus);
    regdev *be = regdev_new(&regdev_be_ops);
    CHECK(uam_bus_map_mmio(&bus, "be", 0x1000, 16, &be->dev) == UAM_OK, "map be");
    be->reg[0] = 0x11223344u;

    const unsigned B = UAM_ACC_BE, L = UAM_ACC_LE;
    EQ64(rd(&bus, 0x1000, 4, B), 0x11223344u, "BE lw");
    EQ64(rd(&bus, 0x1000, 1, B), 0x11, "BE lb +0");
    EQ64(rd(&bus, 0x1003, 1, B), 0x44, "BE lb +3");
    EQ64(rd(&bus, 0x1002, 2, B), 0x3344, "BE lh +2");
    EQ64(rd(&bus, 0x1000, 4, L), 0x44332211u, "LE lw on BE device");
    EQ64(rd(&bus, 0x1000, 1, L), 0x11, "LE lb +0 on BE device");
    EQ64(rd(&bus, 0x1002, 2, L), 0x4433, "LE lh +2 on BE device");

    /* Narrow write without byte enables: read-merge-write. */
    wr(&bus, 0x1001, 1, B, 0xAA);
    EQ64(be->reg[0], 0x11AA3344u, "BE sb +1 (rmw)");
    /* 8-byte access split into two 32-bit registers. */
    be->reg[1] = 0x55667788u;
    EQ64(rd(&bus, 0x1000, 8, B), UINT64_C(0x11AA334455667788), "BE ld split");
    wr(&bus, 0x1008, 8, B, UINT64_C(0x0102030405060708));
    EQ64(be->reg[2], 0x01020304u, "BE sd split hi");
    EQ64(be->reg[3], 0x05060708u, "BE sd split lo");

    /* Narrow write with byte enables. */
    regdev *bm = regdev_new(&regdev_be_masked_ops);
    CHECK(uam_bus_map_mmio(&bus, "bem", 0x2000, 16, &bm->dev) == UAM_OK, "map bem");
    bm->reg[0] = 0x11223344u;
    unsigned reads_before = bm->reads;
    wr(&bus, 0x2002, 2, B, 0xBEEF);
    EQ64(bm->reg[0], 0x1122BEEFu, "BE sh +2 (masked)");
    EQ64(bm->last_mask, 0x0000FFFFu, "byte-enable mask");
    EQ64(bm->reads, reads_before, "masked write does not read");

    /* A little-endian device seen from a BE CPU: today's behaviour. */
    regdev *le = regdev_new(&regdev_le_ops);
    CHECK(uam_bus_map_mmio(&bus, "le", 0x3000, 16, &le->dev) == UAM_OK, "map le");
    le->reg[0] = 0x11223344u;
    EQ64(rd(&bus, 0x3000, 4, L), 0x11223344u, "LE lw on LE device");
    EQ64(rd(&bus, 0x3000, 1, L), 0x44, "LE lb +0 on LE device");
    EQ64(rd(&bus, 0x3000, 4, B), 0x44332211u, "BE lw on LE device");

    /* Per-mapping override: the LE device mapped as big-endian. */
    CHECK(uam_bus_map_mmio_ex(&bus, "le-as-be", 0x4000, 16, &le->dev,
                              &(uam_map_attrs){ .endian = UAM_ACC_BE }) == UAM_OK, "map override");
    EQ64(rd(&bus, 0x4000, 4, B), 0x11223344u, "override: BE lw");
    EQ64(rd(&bus, 0x4000, 1, B), 0x11, "override: BE lb +0");

    uam_bus_fini(&bus);
    free(be);
    free(bm);
    free(le);
}

/* ------------------------------------------------------------------------ */
/* #6 overlapping regions and priority                                       */
/* ------------------------------------------------------------------------ */

static void test_priority(void)
{
    uam_bus bus;
    uam_bus_init(&bus);
    const unsigned L = UAM_ACC_LE;
    CHECK(uam_bus_map_ram(&bus, "ram", 0x10000, 0x2000) == UAM_OK, "map ram");
    regdev *reg = regdev_new(&regdev_le_ops);
    CHECK(uam_bus_map_mmio_ex(&bus, "reg", 0x10800, 4, &reg->dev, &(uam_map_attrs){ .prio = 1 })
          == UAM_OK, "map reg over ram");
    CHECK(uam_bus_map_mmio(&bus, "clash", 0x10900, 4, &reg->dev) == UAM_ERR_INVALID,
          "equal-priority overlap rejected");

    wr(&bus, 0x107FC, 4, L, 0xAABBCCDDu);
    reg->reg[0] = 0x44332211u;
    EQ64(rd(&bus, 0x107FC, 4, L), 0xAABBCCDDu, "ram below the register");
    EQ64(rd(&bus, 0x10800, 4, L), 0x44332211u, "register wins over ram");
    /* Straddles RAM and the register: each byte goes to its owner. */
    EQ64(rd(&bus, 0x107FE, 4, L), 0x2211AABBu, "straddling read");
    wr(&bus, 0x10804, 4, L, 0x01020304u);
    EQ64(rd(&bus, 0x10804, 4, L), 0x01020304u, "ram just above the register");
    for (int i = 0; i < 3; i++) /* repeated: exercises the TLB (page must not be cached) */
        EQ64(rd(&bus, 0x10800, 4, L), 0x44332211u, "register still wins");
    EQ64(rd(&bus, 0x11000, 4, L), 0, "other ram page is cacheable");

    /* A catch-all window under a device sees only the gaps. */
    uam_device *un = uam_unimp_create("blk", 0x20000);
    regdev *dev = regdev_new(&regdev_le_ops);
    CHECK(uam_bus_map_mmio_ex(&bus, "blk", 0x20000, 0x1000, un,
                              &(uam_map_attrs){ .prio = UAM_PRIO_BACKGROUND }) == UAM_OK, "map unimp");
    CHECK(uam_bus_map_mmio(&bus, "dev", 0x20100, 16, &dev->dev) == UAM_OK, "map dev over unimp");
    dev->reg[1] = 7;
    EQ64(rd(&bus, 0x20104, 4, L), 7, "device inside the window");
    EQ64(uam_unimp_accesses(un), 0, "catch-all not consulted for the device");
    EQ64(rd(&bus, 0x20200, 4, L), 0, "gap reads as zero");
    wr(&bus, 0x20204, 4, L, 9);
    EQ64(uam_unimp_accesses(un), 2, "catch-all sees the gaps");
    EQ64(uam_unimp_offsets(un), 2, "offsets relative to the window");

    FILE *f = tmpfile();
    uam_bus_dump_map(&bus, f);
    rewind(f);
    char buf[2048] = { 0 };
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    (void)n;
    fclose(f);
    CHECK(strstr(buf, "prio -1000") && strstr(buf, "prio     1"), "dump shows priorities:\n%s", buf);

    uam_bus_fini(&bus);
    un->ops->destroy(un);
    free(reg);
    free(dev);
}

/* ------------------------------------------------------------------------ */
/* #7 ROMD and aliases                                                       */
/* ------------------------------------------------------------------------ */

static void test_romd_alias(void)
{
    uam_bus bus;
    uam_bus_init(&bus);
    const unsigned B = UAM_ACC_BE;
    uint8_t img[8] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04 };
    regdev *ctl = regdev_new(&regdev_be_ops);
    uam_region *flash = NULL;
    CHECK(uam_bus_map_romd(&bus, "flash", 0x1F000000, 0x200000, &ctl->dev, img, sizeof(img),
                           NULL, &flash) == UAM_OK && flash, "map romd");
    CHECK(uam_bus_map_alias(&bus, "flash-mirror", 0x1F200000, 0x200000, flash, 0, NULL) == UAM_OK,
          "map alias");
    CHECK(uam_bus_region(&bus, "flash") == flash, "region by name");

    ctl->reg[0] = 0x0BADF00Du;
    EQ64(rd(&bus, 0x1F000000, 4, B), 0xDEADBEEFu, "rom mode reads backing");
    EQ64(rd(&bus, 0x1F200000, 4, B), 0xDEADBEEFu, "alias reads backing");
    uint64_t v;
    CHECK(uam_bus_read(&bus, 0x1F000000, 4, B | UAM_ACC_FETCH, &v) == UAM_OK, "fetch in rom mode");

    wr(&bus, 0x1F000000, 4, B, 0x12345678u);
    EQ64(ctl->reg[0], 0x12345678u, "writes go to the device in rom mode");
    EQ64(rd(&bus, 0x1F000000, 4, B), 0xDEADBEEFu, "backing unchanged by the write");

    uam_bus_romd_set_mode(&bus, flash, false);
    EQ64(rd(&bus, 0x1F000000, 4, B), 0x12345678u, "device mode reads the device");
    EQ64(rd(&bus, 0x1F200000, 4, B), 0x12345678u, "alias follows the mode");
    CHECK(uam_bus_read(&bus, 0x1F000000, 4, B | UAM_ACC_FETCH, &v) == UAM_ERR_FAULT,
          "no fetch in device mode");
    uam_bus_romd_set_mode(&bus, flash, true);

    /* The device programs the flash: backing store + dirty. */
    uam_bus_mark_code(&bus, 0x1F000000);
    uint32_t gen = uam_bus_page_gen(&bus, 0x1F000000);
    uint8_t *backing = uam_bus_romd_backing(flash);
    backing[0] = 0xCA;
    uam_bus_romd_dirty(&bus, flash, 0, 1);
    CHECK(uam_bus_page_gen(&bus, 0x1F000000) != gen, "dirty bumps the page generation");
    EQ64(rd(&bus, 0x1F200000, 1, B), 0xCA, "alias sees the new byte");
    CHECK(uam_bus_page_gen(&bus, 0x1F200000) == uam_bus_page_gen(&bus, 0x1F000000),
          "alias shares page generations");

    uam_bus_fini(&bus);
    free(ctl);
}

/* ------------------------------------------------------------------------ */
/* Watchpoints                                                               */
/* ------------------------------------------------------------------------ */

typedef struct watch_log { unsigned n; uint64_t addr, val; bool write; } watch_log;

static void watch_cb(void *ctx, uint64_t addr, unsigned size, bool is_write, uint64_t val)
{
    (void)size;
    watch_log *w = ctx;
    w->n++;
    w->addr = addr;
    w->val = val;
    w->write = is_write;
}

static void test_watch(void)
{
    uam_bus bus;
    uam_bus_init(&bus);
    const unsigned L = UAM_ACC_LE;
    CHECK(uam_bus_map_ram(&bus, "ram", 0, 0x10000) == UAM_OK, "map ram");
    watch_log w = { 0 };
    rd(&bus, 0x1000, 4, L);  /* populate the TLB before the watch exists */
    int id = uam_bus_add_watch(&bus, 0x1004, 4, UAM_WATCH_WRITE, watch_cb, &w);
    CHECK(id > 0, "add watch");
    wr(&bus, 0x1000, 4, L, 1);
    EQ64(w.n, 0, "write beside the watch");
    wr(&bus, 0x1006, 2, L, 0xBEEF);
    EQ64(w.n, 1, "write into the watch");
    EQ64(w.addr, 0x1006, "watch address");
    EQ64(w.val, 0xBEEF, "watch value");
    rd(&bus, 0x1004, 4, L);
    EQ64(w.n, 1, "reads not watched");
    uint64_t v;
    uam_bus_read(&bus, 0x1004, 4, L | UAM_ACC_DEBUG, &v);
    CHECK(uam_bus_remove_watch(&bus, id), "remove watch");
    wr(&bus, 0x1004, 4, L, 2);
    EQ64(w.n, 1, "removed watch is silent");
    uam_bus_fini(&bus);
}

/* ------------------------------------------------------------------------ */
/* Machine-level helpers (MIPS32 BE)                                         */
/* ------------------------------------------------------------------------ */

#define CP0(r, s) ((r) * 8 + (s))

static void put32(uam_machine *m, uint64_t pa, uint32_t w)
{
    uint8_t b[4] = { (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w };
    CHECK(uam_bus_load_bytes(&m->bus, pa, b, 4) == UAM_OK, "load word at 0x%llx", (unsigned long long)pa);
}

static uint32_t gpr(uam_machine *m, unsigned r) { return (uint32_t)uam_cpu_reg_read(&m->cpu, r * 4, 32); }

static void sysw(uam_machine *m, uint32_t id, uint32_t v) { m->cpu.isa->sysreg_write(&m->cpu, id, v); }
static uint32_t sysr(uam_machine *m, uint32_t id)
{
    uint64_t v = 0;
    m->cpu.isa->sysreg_read(&m->cpu, id, &v);
    return (uint32_t)v;
}

static void mips_machine(uam_machine *m)
{
    CHECK(uam_machine_init(m, &uam_isa_mips32be) == UAM_OK, "machine init");
    CHECK(uam_bus_map_ram(&m->bus, "ram", 0, 32 << 20) == UAM_OK, "map ram");
}

/* ------------------------------------------------------------------------ */
/* #3 CP0 timer across resets                                                */
/* ------------------------------------------------------------------------ */

static void test_timer_reset(void)
{
    uam_machine m;
    mips_machine(&m);
    put32(&m, 0x000, 0x1000FFFFu);   /* b . */
    put32(&m, 0x004, 0x00000000u);   /* nop */
    put32(&m, 0x180, 0x275A0001u);   /* addiu k0, k0, 1 */
    put32(&m, 0x184, 0x40805800u);   /* mtc0 zero, Compare (clears the interrupt) */
    put32(&m, 0x188, 0x42000018u);   /* eret */
    uam_machine_reset(&m, 0x80000000u);

    /* Arm Compare, then reset twice while it is armed. */
    sysw(&m, CP0(11, 0), sysr(&m, CP0(9, 0)) + 1000);
    EQ64(uam_sched_count(&m.sched), 1, "compare armed");
    uam_machine_reset(&m, 0x80000000u);
    uam_machine_reset(&m, 0x80000000u);
    CHECK(uam_sched_check(&m.sched), "timer list well formed after resets");
    EQ64(uam_sched_count(&m.sched), 0, "reset cancels the timer");

    sysw(&m, CP0(12, 0), (1u << 15) | 1u);          /* IM7 | IE, BEV/ERL clear */
    sysw(&m, CP0(11, 0), sysr(&m, CP0(9, 0)) + 1000);
    EQ64(uam_sched_count(&m.sched), 1, "one timer after re-arm");
    uam_exec_run(&m.exec, 5000);
    CHECK(uam_sched_check(&m.sched), "timer list well formed after run");
    EQ64(gpr(&m, 26), 1, "exactly one timer interrupt");
    uam_exec_run(&m.exec, 5000);
    EQ64(gpr(&m, 26), 1, "no spurious second interrupt");
    uam_machine_fini(&m);
}

/* ------------------------------------------------------------------------ */
/* #4 machine reset requested by a device; PC hooks; watchpoints in a run    */
/* ------------------------------------------------------------------------ */

typedef struct rstdev {
    uam_device   dev;
    uam_signal   rst;
    uam_machine *m;
    unsigned     resets;
} rstdev;

static uam_status rst_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    (void)d; (void)off; (void)size;
    *val = 0;
    return UAM_OK;
}

static uam_status rst_write(uam_device *d, uint64_t off, unsigned size, uint64_t val)
{
    (void)off; (void)size;
    rstdev *r = UAM_CONTAINER_OF(d, rstdev, dev);
    if (val < 3)
        uam_signal_set(&r->rst, 1);              /* pull the chip reset line */
    else
        uam_machine_stop_cb(r->m, (int64_t)val);
    return UAM_OK;
}

static void rst_reset(uam_device *d)
{
    rstdev *r = UAM_CONTAINER_OF(d, rstdev, dev);
    r->resets++;
    uam_signal_set(&r->rst, 0);
}

static void rst_destroy(uam_device *d) { free(UAM_CONTAINER_OF(d, rstdev, dev)); }

static const uam_device_ops rst_ops = {
    .type = "test,reset", .endian = UAM_ACC_BE,
    .read = rst_read, .write = rst_write, .reset = rst_reset, .destroy = rst_destroy,
};

static uam_hook_action count_hook(void *ctx, uam_cpu *cpu, uint64_t pc)
{
    (void)cpu; (void)pc;
    (*(unsigned *)ctx)++;
    return UAM_HOOK_CONTINUE;
}

static uam_hook_action stop_hook(void *ctx, uam_cpu *cpu, uint64_t pc)
{
    (void)cpu; (void)pc;
    (*(unsigned *)ctx)++;
    return UAM_HOOK_STOP;
}

static void test_device_reset(void)
{
    uam_machine m;
    mips_machine(&m);
    rstdev *r = calloc(1, sizeof(*r));
    uam_device_init(&r->dev, &rst_ops, "rst");
    uam_signal_init(&r->rst);
    r->m = &m;
    uam_machine_add_device(&m, &r->dev);
    CHECK(uam_bus_map_mmio(&m.bus, "rst", 0x1F000000, 0x10, &r->dev) == UAM_OK, "map rst");
    uam_signal_connect(&r->rst, uam_machine_reset_sink, &m, UAM_RESET_SYSTEM);

    static const uint32_t prog[] = {
        0x3C088000u, /* 00 lui   t0, 0x8000 */
        0x8D091000u, /* 04 lw    t1, 0x1000(t0)   counter (RAM, survives resets) */
        0x25290001u, /* 08 addiu t1, t1, 1 */
        0xAD091000u, /* 0c sw    t1, 0x1000(t0) */
        0x3C0ABF00u, /* 10 lui   t2, 0xbf00        (hooked) */
        0xAD490000u, /* 14 sw    t1, 0(t2)        -> device: reset while < 3 */
        0x1000FFFFu, /* 18 b . */
        0x00000000u, /* 1c nop */
    };
    for (unsigned i = 0; i < UAM_ARRAY_LEN(prog); i++)
        put32(&m, 4 * i, prog[i]);
    uam_machine_reset(&m, 0x80000000u);
    unsigned resets_at_start = r->resets;

    unsigned hook_hits = 0;
    CHECK(uam_exec_add_pc_hook(&m.exec, 0x80000010u, count_hook, &hook_hits) == UAM_OK, "add hook");
    watch_log w = { 0 };
    CHECK(uam_bus_add_watch(&m.bus, 0x1000, 4, UAM_WATCH_WRITE, watch_cb, &w) > 0, "add watch");

    uam_stop_reason why = uam_exec_run(&m.exec, 100000);
    EQ64(why, UAM_STOP_REQUEST, "stopped by the device, in one run");
    EQ64(m.exec.stop_status, 3, "third pass stops");
    EQ64(m.exec.stats.resets, 2, "two system resets");
    EQ64(r->resets - resets_at_start, 2, "device reset by each system reset");
    uint64_t counter = 0;
    uam_bus_read(&m.bus, 0x1000, 4, UAM_ACC_BE, &counter);
    EQ64(counter, 3, "RAM preserved across resets");
    EQ64(hook_hits, 3, "hook ran once per pass (mid-block pc)");
    EQ64(w.n, 3, "watch saw every counter store");
    EQ64(w.val, 3, "watch value");

    /* A stopping hook: run returns at the hooked pc, then resumes past it. */
    uam_exec_remove_pc_hook(&m.exec, 0x80000010u, count_hook, &hook_hits);
    unsigned stops = 0;
    uam_exec_add_pc_hook(&m.exec, 0x80000014u, stop_hook, &stops);
    m.exec.stop_request = false;
    uam_machine_reset(&m, 0x80000000u);
    why = uam_exec_run(&m.exec, 1000);
    EQ64(why, UAM_STOP_HOOK, "stopping hook");
    EQ64(m.cpu.pc, 0x80000014u, "stopped before the hooked instruction");
    why = uam_exec_run(&m.exec, 1000);
    EQ64(why, UAM_STOP_REQUEST, "resumes past the hook (counter 4 stops the device)");
    EQ64(stops, 1, "hook not re-run on resume");

    uam_machine_fini(&m);
}

/* ------------------------------------------------------------------------ */
/* #7 code executed from ROMD is invalidated by romd_dirty                   */
/* ------------------------------------------------------------------------ */

static void test_romd_exec(void)
{
    uam_machine m;
    mips_machine(&m);
    regdev *ctl = regdev_new(&regdev_be_ops);
    uam_machine_add_device(&m, &ctl->dev);
    uam_region *flash = NULL;
    CHECK(uam_bus_map_romd(&m.bus, "flash", 0x1FC00000, 0x10000, &ctl->dev, NULL, 0, NULL, &flash)
          == UAM_OK, "map romd");
    uint8_t prog[] = { 0x24, 0x02, 0x00, 0x05,  /* addiu v0, zero, 5 */
                       0x10, 0x00, 0xFF, 0xFF,  /* b . */
                       0x00, 0x00, 0x00, 0x00 };
    memcpy(uam_bus_romd_backing(flash), prog, sizeof(prog));
    uam_machine_reset(&m, 0xBFC00000u);
    uam_exec_run(&m.exec, 10);
    EQ64(gpr(&m, 2), 5, "executes in place from ROMD");

    uam_bus_romd_backing(flash)[3] = 7;          /* "program" the flash */
    uam_bus_romd_dirty(&m.bus, flash, 0, 4);
    uam_machine_reset(&m, 0xBFC00000u);
    uam_exec_run(&m.exec, 10);
    EQ64(gpr(&m, 2), 7, "new code after romd_dirty");
    uam_machine_fini(&m);
}

/* Run-time decode changes: code translated from a region that stops decoding
 * (disabled, or moved away) must not run, and must run again when it returns. */
static void test_region_remap(void)
{
    uam_machine m;
    mips_machine(&m);
    put32(&m, 0x1000, 0x24020001u);              /* RAM:     addiu v0, zero, 1 */
    put32(&m, 0x1004, 0x1000FFFFu);              /*          b . */
    put32(&m, 0x1008, 0x00000000u);
    uam_region *ov = NULL;
    CHECK(uam_bus_map_ram_ex(&m.bus, "overlay", 0x1000, 0x1000, &(uam_map_attrs){ .prio = 1 }, &ov)
          == UAM_OK, "map overlay");
    uint32_t prog[] = { 0x24020002u, 0x1000FFFFu, 0 };   /* addiu v0, zero, 2; b . */
    for (unsigned i = 0; i < 3; i++)
        uam_bus_write(&m.bus, 0x1000 + i * 4, 4, UAM_ACC_BE, prog[i]);

    uam_machine_reset(&m, 0x80001000u);
    uam_exec_run(&m.exec, 10);
    EQ64(gpr(&m, 2), 2, "overlay decodes over RAM");

    uam_bus_region_enable(&m.bus, ov, false);
    uam_machine_reset(&m, 0x80001000u);
    uam_exec_run(&m.exec, 10);
    EQ64(gpr(&m, 2), 1, "disabled overlay: RAM code, not the cached overlay block");

    uam_bus_region_enable(&m.bus, ov, true);
    uam_machine_reset(&m, 0x80001000u);
    uam_exec_run(&m.exec, 10);
    EQ64(gpr(&m, 2), 2, "re-enabled overlay");

    uam_bus_region_move(&m.bus, ov, 0x3000);
    uam_machine_reset(&m, 0x80001000u);
    uam_exec_run(&m.exec, 10);
    EQ64(gpr(&m, 2), 1, "moved overlay leaves RAM visible");
    uam_machine_reset(&m, 0x80003000u);
    uam_exec_run(&m.exec, 10);
    EQ64(gpr(&m, 2), 2, "moved overlay decodes at its new base");
    uam_machine_fini(&m);
}

/* ------------------------------------------------------------------------ */
/* #8 WAIT whose wakeup never comes while a timer keeps re-arming itself     */
/* ------------------------------------------------------------------------ */

/* A free-running counter's compare match: re-arms itself on every fire, but
 * its interrupt line is not connected to the hart. */
typedef struct rearm_timer {
    uam_timer  t;
    uam_sched *s;
    uam_exec  *x;
    uint64_t   period;     /* 0: re-arm at now (a timer storm) */
    uint64_t   fires;
    uint64_t   stop_at;    /* call uam_exec_stop (a signal stand-in) at this fire; 0 = never */
} rearm_timer;

static void rearm_fire(void *ctx)
{
    rearm_timer *r = ctx;
    if (++r->fires == r->stop_at)
        uam_exec_stop(r->x, 130);
    uam_timer_arm(r->s, &r->t, uam_sched_now(r->s) + r->period);
}

/* A hart parked in WAIT with interrupts enabled but no line ever raised. */
static void wait_machine(uam_machine *m, rearm_timer *r, uint64_t period)
{
    mips_machine(m);
    put32(m, 0x000, 0x42000020u);    /* wait */
    put32(m, 0x004, 0x1000FFFFu);    /* b . */
    put32(m, 0x008, 0x00000000u);    /* nop */
    uam_machine_reset(m, 0x80000000u);
    sysw(m, CP0(12, 0), (1u << 10) | 1u);   /* IM2 | IE: enabled, never pending */
    *r = (rearm_timer){ .s = &m->sched, .x = &m->exec, .period = period };
    uam_timer_init(&r->t, rearm_fire, r);
    uam_timer_arm(&m->sched, &r->t, uam_sched_now(&m->sched) + period);
}

static void test_wait_livelock(void)
{
    uam_machine m;
    rearm_timer r;

    /* Livelock: with no host bridge nothing but the timers could wake the
     * hart. Stall rounds are counted per idle skip, without host time. */
    wait_machine(&m, &r, 100);
    m.exec.max_stall = 1000;
    uam_stop_reason why = uam_exec_run(&m.exec, 1000000);
    EQ64(why, UAM_STOP_LIVELOCK, "WAIT + unwired re-arming timer is a livelock");
    EQ64(m.cpu.icount, 1, "only the WAIT retired");
    EQ64(r.fires, 1000, "timer fired after each idle skip");
    EQ64(m.exec.stats.idle_ticks, 1000 * 100 - 1, "idle time skipped (WAIT took one tick)");
    CHECK(m.cpu.waiting, "still waiting");
    CHECK(uam_sched_check(&m.sched), "timer list well formed");
    /* Deterministic: the same machine stops at the same point again. */
    uam_machine_fini(&m);
    wait_machine(&m, &r, 100);
    m.exec.max_stall = 1000;
    uam_exec_run(&m.exec, 1000000);
    EQ64(r.fires, 1000, "same stop point on a second run");
    uam_machine_fini(&m);

    /* Default bound: the run ends without any limit set by the caller. */
    wait_machine(&m, &r, 7);
    why = uam_exec_run(&m.exec, 0);
    EQ64(why, UAM_STOP_LIVELOCK, "default max_stall ends an unbounded run");
    EQ64(r.fires, UAM_EXEC_DEFAULT_MAX_STALL, "after the default stall budget");
    uam_machine_fini(&m);

    /* A virtual-time budget stops exactly at max_time. */
    wait_machine(&m, &r, 100);
    m.exec.max_stall = 0;
    m.exec.max_time = 12345;
    why = uam_exec_run(&m.exec, 0);
    EQ64(why, UAM_STOP_TIME_LIMIT, "virtual-time budget");
    EQ64(uam_sched_now(&m.sched), 12345, "stopped at max_time");
    EQ64(r.fires, 123, "timer fires up to the limit");
    uam_machine_fini(&m);

    /* With no bound at all, time saturates at the horizon instead of wrapping. */
    wait_machine(&m, &r, UINT64_C(1) << 60);
    m.exec.max_stall = 0;
    why = uam_exec_run(&m.exec, 0);
    EQ64(why, UAM_STOP_TIME_LIMIT, "time horizon");
    EQ64(uam_sched_now(&m.sched), UAM_SCHED_TIME_MAX, "stopped at UAM_SCHED_TIME_MAX");
    EQ64(r.fires, 3, "fires below the horizon");
    EQ64(uam_sched_next(&m.sched), UAM_SCHED_TIME_MAX, "re-armed deadline did not wrap");
    uam_sched_skip_to(&m.sched, UINT64_MAX);
    EQ64(uam_sched_now(&m.sched), UAM_SCHED_TIME_MAX, "skip_to saturates");
    uam_machine_fini(&m);

    /* A stop request (what SIGINT/SIGTERM send) from a timer while idling. */
    wait_machine(&m, &r, 1);
    m.exec.max_stall = 0;
    r.stop_at = 5000;
    why = uam_exec_run(&m.exec, 0);
    EQ64(why, UAM_STOP_REQUEST, "stop request while idling");
    EQ64(r.fires, 5000, "stopped at the requesting fire");
    uam_machine_fini(&m);

    /* A timer storm (re-arms at now): run_due returns after its firing budget,
     * so a stop request is seen, and the stall bound still ends the run. */
    wait_machine(&m, &r, 0);
    m.exec.max_stall = 0;
    r.stop_at = 10000;
    why = uam_exec_run(&m.exec, 0);
    EQ64(why, UAM_STOP_REQUEST, "stop request during a timer storm");
    CHECK(r.fires >= 10000 && r.fires < 10000 + UAM_SCHED_MAX_FIRE,
          "stopped within one firing budget (%llu fires)", (unsigned long long)r.fires);
    uam_machine_fini(&m);

    wait_machine(&m, &r, 0);
    m.exec.max_stall = 3;
    why = uam_exec_run(&m.exec, 0);
    EQ64(why, UAM_STOP_LIVELOCK, "timer storm is a livelock");
    EQ64(r.fires, 3 * UAM_SCHED_MAX_FIRE, "three firing budgets");
    uam_machine_fini(&m);
}

/* ------------------------------------------------------------------------ */
/* #9 ABI handshake: a caller built from other headers is refused            */
/* ------------------------------------------------------------------------ */

/* Run uam_machine_init with `abi` in a child; true if the child was stopped with
 * the ABI error status (a plain exit, no signal, no core dump). */
static bool init_refused(const uam_abi_layout *abi)
{
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        uam_machine m;
        uam_machine_init_abi(&m, &uam_isa_mips32be, abi);
        _exit(0);
    }
    int ws = 0;
    CHECK(pid > 0 && waitpid(pid, &ws, 0) == pid, "fork/wait");
    return WIFEXITED(ws) && WEXITSTATUS(ws) == UAM_ABI_EXIT_STATUS;
}

static void test_abi(void)
{
    EQ64(uam_abi_version(), UAM_ABI_VERSION, "library ABI version");
    CHECK(uam_abi_check(UAM_ABI_LAYOUT, "test"), "matching layout accepted");

    /* A field added to uam_exec: the case that moved uam_machine.on_reset. */
    uam_abi_layout grown = *UAM_ABI_LAYOUT;
    grown.exec += 16;
    grown.machine += 16;
    fprintf(stderr, "(expected ABI errors follow)\n");
    CHECK(!uam_abi_check(&grown, "test"), "grown uam_exec refused");
    uam_abi_layout old = *UAM_ABI_LAYOUT;
    old.version--;
    CHECK(!uam_abi_check(&old, "test"), "other ABI version refused");

    CHECK(!init_refused(UAM_ABI_LAYOUT), "uam_machine_init runs with matching headers");
    CHECK(init_refused(&grown), "uam_machine_init exits with status %d on a layout mismatch",
          UAM_ABI_EXIT_STATUS);
}

int main(void)
{
    test_device_endian();
    test_priority();
    test_romd_alias();
    test_watch();
    test_timer_reset();
    test_device_reset();
    test_romd_exec();
    test_region_remap();
    test_wait_livelock();
    test_abi();
    printf("test_core: %d/%d checks passed\n", n_checks - n_failed, n_checks);
    return n_failed ? 1 : 0;
}
