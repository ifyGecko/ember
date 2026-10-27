/*
 * test_core.c - Unit tests for the bus (priorities, device byte order and
 * access widths, ROMD, aliases, watchpoints), the scheduler under resets,
 * device-requested machine resets, executor PC hooks, and the executor's
 * virtual-time and livelock bounds, and the ABI handshake.
 *
 * The CFI flash's write gate and write-back file are tested here too, and
 * the PCI hierarchy (bridges, windows, INTx swizzling, MSI and MSI-X), and the
 * NVMe and AHCI storage controllers driven as a driver would.
 *
 * The machine-level tests run tiny hand-encoded MIPS32 big-endian programs.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "uam/bus.h"
#include "uam/dev/cfi_flash.h"
#include "uam/dev/ahci.h"
#include "uam/dev/i8237.h"
#include "uam/dev/ide.h"
#include "uam/dev/vga.h"
#include "uam/dev/nvme.h"
#include "uam/dev/pc_misc.h"
#include "uam/dev/pci.h"
#include "uam/dev/unimp.h"
#include "uam/device.h"
#include "uam/isa.h"
#include "uam/isa/mips32.h"
#include "uam/isa/x86.h"
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

    /* More offsets than the table holds: every access still counts, and the untracked
     * ones (probed a bounded number of slots) stay RAZ/WI. */
    uam_device *big = uam_unimp_create("big", 0x30000);
    CHECK(uam_bus_map_mmio_ex(&bus, "big", 0x30000, 0x2000, big,
                              &(uam_map_attrs){ .prio = UAM_PRIO_BACKGROUND }) == UAM_OK, "map big unimp");
    uam_log_level saved = uam_log_threshold;
    uam_log_threshold = UAM_LOG_ERROR;   /* one warning per new offset */
    for (uint64_t a = 0x30000; a < 0x32000; a += 4)
        EQ64(rd(&bus, a, 4, L), 0, "window reads as zero");
    for (int i = 0; i < 1000; i++)
        wr(&bus, 0x31ffc, 4, L, (uint64_t)i);
    uam_log_threshold = saved;
    EQ64(uam_unimp_accesses(big), 2048 + 1000, "a full table still counts accesses");
    CHECK(uam_unimp_offsets(big) > 0 && uam_unimp_offsets(big) <= 1024, "at most 1024 offsets tracked");

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
    uam_log_threshold = UAM_LOG_ERROR;   /* its hot list */
    big->ops->destroy(big);
    uam_log_threshold = saved;
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

/* Regions smaller than a page (I/O ports, registers) resolve through a cache of their
 * own: it must follow map changes and never hide a watch or a higher-priority region. */
static void test_subpage_cache(void)
{
    uam_bus bus;
    uam_bus_init(&bus);
    const unsigned L = UAM_ACC_LE;
    uam_device *win = uam_unimp_create("win", 0x5000);
    regdev *a = regdev_new(&regdev_le_ops), *b = regdev_new(&regdev_le_ops);
    CHECK(uam_bus_map_mmio_ex(&bus, "win", 0x5000, 0x1000, win,
                              &(uam_map_attrs){ .prio = UAM_PRIO_BACKGROUND }) == UAM_OK, "map window");
    CHECK(uam_bus_map_mmio(&bus, "a", 0x5004, 4, &a->dev) == UAM_OK, "map a");
    a->reg[0] = 0x11223344u;
    for (int i = 0; i < 3; i++) {   /* the same 8-byte granule: a register and a gap */
        EQ64(rd(&bus, 0x5004, 4, L), 0x11223344u, "small register");
        EQ64(rd(&bus, 0x5000, 4, L), 0, "gap beside it");
    }
    CHECK(uam_bus_map_mmio_ex(&bus, "b", 0x5004, 2, &b->dev, &(uam_map_attrs){ .prio = 1 })
          == UAM_OK, "map b over a");
    b->reg[0] = 0xBEEF;
    EQ64(rd(&bus, 0x5004, 2, L), 0xBEEF, "a higher-priority region mapped later wins");
    EQ64(rd(&bus, 0x5006, 2, L), 0x1122, "the rest of a still answers");
    uam_bus_region_enable(&bus, (uam_region *)uam_bus_find(&bus, 0x5004), false);
    EQ64(rd(&bus, 0x5004, 2, L), 0x3344, "a again once b is disabled");
    watch_log w = { 0 };
    int id = uam_bus_add_watch(&bus, 0x5004, 4, UAM_WATCH_READ, watch_cb, &w);
    rd(&bus, 0x5004, 4, L);
    EQ64(w.n, 1, "a watch added after caching fires");
    CHECK(uam_bus_remove_watch(&bus, id), "remove watch");
    uam_bus_region_move(&bus, (uam_region *)uam_bus_find(&bus, 0x5004), 0x5100);
    EQ64(rd(&bus, 0x5004, 4, L), 0, "the old place is the window again");
    EQ64(rd(&bus, 0x5100, 4, L), 0x11223344u, "the register at its new place");
    uam_bus_fini(&bus);
    free(a);
    free(b);
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

/* Code is tracked in 64-byte lines: data sharing a page with code keeps the
 * translation; a write into the code's line replaces it. */
static void test_code_lines(void)
{
    uam_machine m;
    mips_machine(&m);
    put32(&m, 0x1000, 0x24020001u);              /* addiu v0, zero, 1 */
    put32(&m, 0x1004, 0x1000FFFFu);              /* b . */
    put32(&m, 0x1008, 0x00000000u);
    uam_machine_reset(&m, 0x80001000u);
    uam_exec_run(&m.exec, 10);
    EQ64(gpr(&m, 2), 1, "first run");
    uint64_t translated = m.exec.stats.blocks_translated;

    uam_bus_write(&m.bus, 0x1800, 4, UAM_ACC_BE, 0x12345678);   /* data, same page */
    uam_machine_reset(&m, 0x80001000u);
    uam_exec_run(&m.exec, 10);
    EQ64(m.exec.stats.blocks_translated, translated, "data write kept the translation");

    uam_bus_write(&m.bus, 0x1000, 4, UAM_ACC_BE, 0x24020002u);  /* the code itself */
    uam_machine_reset(&m, 0x80001000u);
    uam_exec_run(&m.exec, 10);
    EQ64(gpr(&m, 2), 2, "code write: the new instruction runs");
    uam_machine_fini(&m);
}

/* CFI flash: program, erase, the write gate, and write-through to a file. */
static bool gate_open;
static bool flash_gate(void *ctx, uint64_t off) { (void)ctx; (void)off; return gate_open; }

static void test_cfi_flash(void)
{
    uam_bus bus;
    uam_bus_init(&bus);
    uint8_t image[0x2000];
    memset(image, 0xFF, sizeof(image));
    image[0x10] = 0xF7;
    char path[] = "/tmp/uam-test-flash-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0 && write(fd, image, sizeof(image)) == (ssize_t)sizeof(image), "temp file");
    close(fd);

    const uint64_t base = 0x10000000;
    uam_device *f = uam_cfi_flash_map(&bus, "flash", base, sizeof(image), 0x1000, image, sizeof(image));
    CHECK(f != NULL, "map flash");
    CHECK(uam_cfi_flash_set_backing_file(f, path) == UAM_OK, "backing file");
    EQ64(rd(&bus, base + 0x10, 1, UAM_ACC_LE), 0xF7, "array read");

    uam_bus_write(&bus, base + 0x10, 1, UAM_ACC_LE, 0x40);   /* program */
    uam_bus_write(&bus, base + 0x10, 1, UAM_ACC_LE, 0x5A);
    EQ64(rd(&bus, base + 0x10, 1, UAM_ACC_LE), 0x80, "status after program: ready");
    uam_bus_write(&bus, base, 1, UAM_ACC_LE, 0xFF);
    EQ64(rd(&bus, base + 0x10, 1, UAM_ACC_LE), 0x52, "programming only clears bits");

    gate_open = false;
    uam_cfi_flash_set_write_gate(f, flash_gate, NULL);
    uam_bus_write(&bus, base + 0x11, 1, UAM_ACC_LE, 0x40);
    uam_bus_write(&bus, base + 0x11, 1, UAM_ACC_LE, 0x00);
    EQ64(rd(&bus, base, 1, UAM_ACC_LE), 0x92, "gated program: ready, program error, protected");
    uam_bus_write(&bus, base, 1, UAM_ACC_LE, 0x50);              /* clear status */
    uam_bus_write(&bus, base, 1, UAM_ACC_LE, 0xFF);
    EQ64(rd(&bus, base + 0x11, 1, UAM_ACC_LE), 0xFF, "gated program left the byte");

    gate_open = true;
    uam_bus_write(&bus, base + 0x1000, 1, UAM_ACC_LE, 0x40);
    uam_bus_write(&bus, base + 0x1000, 1, UAM_ACC_LE, 0x00);
    uam_bus_write(&bus, base, 1, UAM_ACC_LE, 0x20);              /* erase block 0 */
    uam_bus_write(&bus, base, 1, UAM_ACC_LE, 0xD0);
    uam_bus_write(&bus, base, 1, UAM_ACC_LE, 0xFF);
    EQ64(rd(&bus, base + 0x10, 1, UAM_ACC_LE), 0xFF, "erased block");
    EQ64(rd(&bus, base + 0x1000, 1, UAM_ACC_LE), 0x00, "the next block keeps its program");

    uint8_t back[0x2000] = { 0 };
    fd = open(path, O_RDONLY);
    CHECK(fd >= 0 && read(fd, back, sizeof(back)) == (ssize_t)sizeof(back), "read back");
    close(fd);
    EQ64(back[0x10], 0xFF, "file: erase written through");
    EQ64(back[0x1000], 0x00, "file: program written through");
    EQ64(back[0x11], 0xFF, "file: refused program not written");
    unlink(path);
    f->ops->destroy(f);
    uam_bus_fini(&bus);
}

static unsigned intx_slot, intx_pin;
static int intx_level = -1;
static void pci_router(void *ctx, unsigned slot, unsigned pin, int level)
{
    (void)ctx;
    intx_slot = slot; intx_pin = pin; intx_level = level;
}
static uint64_t msi_addr;
static uint32_t msi_data;
static int msi_count;
static void pci_msi(void *ctx, uint64_t addr, uint32_t data)
{
    (void)ctx;
    msi_addr = addr; msi_data = data; msi_count++;
}

static void test_pci(void)
{
    uam_bus bus;
    uam_bus_init(&bus);
    uam_pci_bus *pb = uam_pci_bus_create("pci", &bus, 0x10000000000ull);
    uam_device *bar_dev = uam_unimp_create("ep-regs", 0);
    uam_pci_func br, ep;
    uam_pci_bridge_init(&br, "port", 0x1234, 0x0001, 0x06040000, NULL, NULL);
    uam_pci_add_pcie_cap(&br, UAM_PCIE_ROOT_PORT, 1);
    uam_pci_func_init(&ep, "ep", 0x1234, 0x0002, 0x01060100, NULL, NULL);
    uam_pci_func_bar(&ep, 0, 0x1000, UAM_PCI_BAR_MEM, bar_dev);
    ep.cfg[UAM_PCI_INT_PIN] = 1;
    unsigned msi = uam_pci_msi_init(&ep, 0, true, true);
    CHECK(uam_pci_register(pb, &br, 1, 0) == UAM_OK, "register bridge");
    CHECK(uam_pci_register_at(pb, &br, &ep, 2, 0) == UAM_OK, "register endpoint");
    uam_pci_set_intx_router(pb, pci_router, NULL);
    uam_pci_set_msi_sink(pb, pci_msi, NULL);
    unsigned ep_devfn = 2 << 3;

    EQ64(uam_pci_cfg_read(pb, 0, 1 << 3, 0, 4), 0x00011234, "bridge on bus 0");
    EQ64(uam_pci_cfg_read(pb, 0, 1 << 3, UAM_PCI_HEADER, 1), 1, "type-1 header");
    EQ64(uam_pci_cfg_read(pb, 1, ep_devfn, 0, 4), 0xFFFFFFFF, "no bus 1 before numbering");
    uam_pci_cfg_write(pb, 0, 1 << 3, UAM_PCI_PRIMARY_BUS, 4, 0x00010100);
    EQ64(uam_pci_cfg_read(pb, 1, ep_devfn, 0, 4), 0x00021234, "endpoint on bus 1");
    unsigned cap = uam_pci_cfg_read(pb, 0, 1 << 3, UAM_PCI_CAP_PTR, 1);
    EQ64(uam_pci_cfg_read(pb, 0, 1 << 3, cap, 1), UAM_PCI_CAP_PCIE, "PCIe capability listed");
    EQ64(uam_pci_cfg_read(pb, 1, ep_devfn, UAM_PCI_CAP_PTR, 1), msi, "MSI capability listed");

    /* the BAR decodes only inside the bridge's memory window */
    uam_pci_cfg_write(pb, 1, ep_devfn, UAM_PCI_BAR0, 4, 0xC0001000);
    uam_pci_cfg_write(pb, 1, ep_devfn, UAM_PCI_COMMAND, 2, UAM_PCI_CMD_MEM | UAM_PCI_CMD_MASTER);
    uam_region *r = uam_bus_region(&bus, "ep.bar0");
    CHECK(!r || r->disabled, "BAR closed off by the bridge");
    uam_pci_cfg_write(pb, 0, 1 << 3, UAM_PCI_MEM_BASE, 4, 0xC000C000);
    uam_pci_cfg_write(pb, 0, 1 << 3, UAM_PCI_COMMAND, 2, UAM_PCI_CMD_MEM | UAM_PCI_CMD_MASTER);
    r = uam_bus_region(&bus, "ep.bar0");
    CHECK(r && !r->disabled && r->base == 0xC0001000, "BAR decodes inside the window");
    uam_pci_cfg_write(pb, 0, 1 << 3, UAM_PCI_MEM_BASE, 4, 0xD000D000);
    CHECK(r->disabled, "moving the window away closes the BAR");

    /* INTA of device 2 behind the bridge in slot 1: pin (0 + 2) mod 4 = INTC */
    uam_pci_set_intx(&ep, 1);
    CHECK(intx_level == 1 && intx_slot == 1 && intx_pin == 2, "INTx swizzled (%u,%u,%d)",
          intx_slot, intx_pin, intx_level);
    uam_pci_cfg_write(pb, 1, ep_devfn, UAM_PCI_COMMAND, 2,
                      UAM_PCI_CMD_MEM | UAM_PCI_CMD_MASTER | UAM_PCI_CMD_INTX_OFF);
    EQ64(intx_level, 0, "INTx disable drops the line");
    EQ64(uam_pci_cfg_read(pb, 1, ep_devfn, UAM_PCI_STATUS, 2) & UAM_PCI_STATUS_INTX,
         UAM_PCI_STATUS_INTX, "interrupt status still set");
    uam_pci_set_intx(&ep, 0);

    /* MSI: masked messages wait in the pending bits */
    uam_pci_cfg_write(pb, 1, ep_devfn, msi + 4, 4, 0xFEE01000);
    uam_pci_cfg_write(pb, 1, ep_devfn, msi + 8, 4, 0);
    uam_pci_cfg_write(pb, 1, ep_devfn, msi + 0x0C, 2, 0x0041);
    uam_pci_cfg_write(pb, 1, ep_devfn, msi + 0x10, 4, 1);         /* mask vector 0 */
    uam_pci_cfg_write(pb, 1, ep_devfn, msi + 2, 2, 1);            /* enable */
    uam_pci_msi_notify(&ep, 0);
    EQ64(msi_count, 0, "masked MSI held");
    EQ64(uam_pci_cfg_read(pb, 1, ep_devfn, msi + 0x14, 4), 1, "pending bit");
    uam_pci_cfg_write(pb, 1, ep_devfn, msi + 0x10, 4, 0);
    CHECK(msi_count == 1 && msi_addr == 0xFEE01000 && msi_data == 0x41, "unmask delivers");
    uam_pci_cfg_write(pb, 0, 1 << 3, UAM_PCI_COMMAND, 2, 0);
    uam_pci_msi_notify(&ep, 0);
    EQ64(msi_count, 1, "no MSI without bus mastering on the bridge");
    uint8_t b;
    CHECK(uam_pci_dma_read(&ep, 0, &b, 1) != UAM_OK, "no DMA either");

    /* MSI-X on a second endpoint */
    uam_device *x_dev = uam_unimp_create("x-regs", 0);
    uam_pci_func x;
    uam_pci_func_init(&x, "x", 0x1234, 0x0003, 0x01080200, NULL, NULL);
    uam_pci_func_bar(&x, 0, 0x4000, UAM_PCI_BAR_MEM, x_dev);
    unsigned mx = uam_pci_msix_init(&x, 4, 0, 0x2000, 0, 0x3000);
    CHECK(mx != 0 && uam_pci_register(pb, &x, 3, 0) == UAM_OK, "MSI-X function");
    unsigned xd = 3 << 3;
    uam_pci_cfg_write(pb, 0, xd, UAM_PCI_COMMAND, 2, UAM_PCI_CMD_MEM | UAM_PCI_CMD_MASTER);
    uam_pci_cfg_write(pb, 0, xd, mx + 2, 2, 0x8000);
    uint64_t v = 0xFEE00000;
    CHECK(uam_pci_msix_access(&x, 0, 0x2010, 4, true, &v), "table write");
    v = 0x55;
    uam_pci_msix_access(&x, 0, 0x2018, 4, true, &v);
    uam_pci_msi_notify(&x, 1);
    EQ64(msi_count, 1, "MSI-X entries start masked");
    uam_pci_msix_access(&x, 0, 0x3000, 8, false, &v);
    EQ64(v, 2, "MSI-X pending bit");
    v = 0;
    uam_pci_msix_access(&x, 0, 0x201C, 4, true, &v);
    CHECK(msi_count == 2 && msi_data == 0x55, "unmasking the entry delivers");
    CHECK(!uam_pci_msix_access(&x, 0, 0x100, 4, false, &v), "other offsets go to the model");

    uam_pci_bus_device(pb)->ops->destroy(uam_pci_bus_device(pb));
    bar_dev->ops->destroy(bar_dev);
    x_dev->ops->destroy(x_dev);
    uam_bus_fini(&bus);
}

/* Configuration mechanism #1: only a dword access to CONFIG_ADDRESS is one; others go
 * to the pass-through (here: a byte register at offset 1, like a PC's 0xCF9). */
static unsigned through_writes;
static uint64_t through_off, through_val;
static uam_status through_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    (void)d; (void)size;
    *val = 0xA0 + off;
    return UAM_OK;
}
static uam_status through_write(uam_device *d, uint64_t off, unsigned size, uint64_t val)
{
    (void)d; (void)size;
    through_writes++;
    through_off = off;
    through_val = val;
    return UAM_OK;
}
static unsigned cf9_calls;
static uint8_t  cf9_val;
static void cf9_hook(void *ctx, uint8_t val) { (void)ctx; cf9_calls++; cf9_val = val; }

static void test_pci_cam(void)
{
    uam_bus bus;
    uam_bus_init(&bus);
    uam_pci_bus *pb = uam_pci_bus_create("pci", &bus, 0);
    uam_pci_func f;
    uam_pci_func_init(&f, "f", 0x1234, 0x0005, 0x01060100, NULL, NULL);
    CHECK(uam_pci_register(pb, &f, 2, 0) == UAM_OK, "register function");
    CHECK(uam_bus_map_mmio(&bus, "cam", 0xCF8, 8, uam_pci_cam(pb)) == UAM_OK, "map CAM");
    const unsigned L = UAM_ACC_LE;

    wr(&bus, 0xCF8, 4, L, 0x80001000);
    EQ64(rd(&bus, 0xCFC, 4, L), 0x00051234, "dword address selects device 2");
    wr(&bus, 0xCF9, 1, L, 0x06);
    EQ64(rd(&bus, 0xCF8, 4, L), 0x80001000, "byte write alone is not an address write");
    EQ64(rd(&bus, 0xCF9, 1, L), 0x10, "byte read of the address without a pass-through");

    static const uam_device_ops through_ops = {
        .type = "test,cam-through", .read = through_read, .write = through_write,
        .flags = UAM_DEVICE_STATELESS,
    };
    uam_device through;
    uam_device_init(&through, &through_ops, "through");
    uam_pci_cam_passthrough(pb, &through);
    wr(&bus, 0xCF9, 1, L, 0x06);
    CHECK(through_writes == 1 && through_off == 1 && through_val == 0x06, "byte write passed through");
    EQ64(rd(&bus, 0xCF9, 1, L), 0xA1, "byte read passed through");
    wr(&bus, 0xCF8, 4, L, 0x80000000);
    EQ64(through_writes, 1, "dword address write not passed through");
    EQ64(rd(&bus, 0xCF8, 4, L), 0x80000000, "dword address read");

    uam_pci_set_cf9_hook(pb, cf9_hook, NULL);
    wr(&bus, 0xCF9, 1, L, 0x0E);
    CHECK(cf9_calls == 1 && cf9_val == 0x0E, "CF9 hook");
    wr(&bus, 0xCFA, 1, L, 0x0E);
    EQ64(cf9_calls, 1, "other bytes are not the hook");
    EQ64(rd(&bus, 0xCF8, 4, L), 0x80000000, "address untouched");

    uam_pci_bus_device(pb)->ops->destroy(uam_pci_bus_device(pb));
    uam_bus_fini(&bus);
}

/* Without an MSI sink, a message is a little-endian dword write to memory. */
static void test_pci_msi_mem(void)
{
    uam_bus bus;
    uam_bus_init(&bus);
    CHECK(uam_bus_map_ram(&bus, "ram", 0, 0x10000) == UAM_OK, "map ram");
    uam_pci_bus *pb = uam_pci_bus_create("pci", &bus, 0);
    uam_pci_func f;
    uam_pci_func_init(&f, "f", 0x1234, 0x0006, 0x01060100, NULL, NULL);
    unsigned msi = uam_pci_msi_init(&f, 0, true, true);
    CHECK(uam_pci_register(pb, &f, 1, 0) == UAM_OK, "register function");
    unsigned d = 1 << 3;
    uam_pci_cfg_write(pb, 0, d, UAM_PCI_COMMAND, 2, UAM_PCI_CMD_MASTER);
    uam_pci_cfg_write(pb, 0, d, msi + 4, 4, 0x8000);
    uam_pci_cfg_write(pb, 0, d, msi + 8, 4, 0);
    uam_pci_cfg_write(pb, 0, d, msi + 0x0C, 2, 0x1234);
    uam_pci_cfg_write(pb, 0, d, msi + 2, 2, 1);
    uam_pci_msi_notify(&f, 0);
    EQ64(rd(&bus, 0x8000, 1, UAM_ACC_LE), 0x34, "message byte 0");
    EQ64(rd(&bus, 0x8001, 1, UAM_ACC_LE), 0x12, "message byte 1");
    uam_pci_bus_device(pb)->ops->destroy(uam_pci_bus_device(pb));
    uam_bus_fini(&bus);
}

/* A 64 KiB disk image whose every byte is its sector number. */
static uam_blockdev *test_disk(char *path, uint32_t ss)
{
    uint8_t img[65536];
    for (size_t k = 0; k < sizeof(img); k++)
        img[k] = (uint8_t)(k / ss);
    int fd = mkstemp(path);
    CHECK(fd >= 0 && write(fd, img, sizeof(img)) == (ssize_t)sizeof(img), "disk image");
    close(fd);
    return uam_blockdev_open_file(path, ss, UAM_BLK_VOLATILE);
}

static void wr32(uam_bus *b, uint64_t a, uint32_t v) { uam_bus_write(b, a, 4, UAM_ACC_LE, v); }
static void wr64(uam_bus *b, uint64_t a, uint64_t v) { uam_bus_write(b, a, 8, UAM_ACC_LE, v); }

static void test_nvme(void)
{
    uam_bus bus;
    uam_bus_init(&bus);
    uam_bus_map_ram(&bus, "ram", 0, 1u << 20);
    uam_pci_bus *pb = uam_pci_bus_create("pci", &bus, 0x10000000000ull);
    uam_device *dev = uam_nvme_create("nvme", 0xFFFE, 2, 0);
    char path[] = "/tmp/uam-test-nvme-XXXXXX";
    uam_blockdev *bd = test_disk(path, 512);
    CHECK(bd && uam_nvme_attach(dev, 1, bd) == UAM_OK, "attach namespace");
    CHECK(uam_pci_register(pb, uam_nvme_func(dev), 3, 0) == UAM_OK, "register");
    const unsigned df = 3 << 3;
    const uint64_t R = 0xF0000000;
    EQ64(uam_pci_cfg_read(pb, 0, df, UAM_PCI_REVISION, 4) >> 8, 0x010802, "class: NVMe");
    uam_pci_cfg_write(pb, 0, df, UAM_PCI_BAR0, 4, (uint32_t)R);
    uam_pci_cfg_write(pb, 0, df, UAM_PCI_BAR0 + 4, 4, 0);
    uam_pci_cfg_write(pb, 0, df, UAM_PCI_COMMAND, 2, UAM_PCI_CMD_MEM | UAM_PCI_CMD_MASTER);
    EQ64(rd(&bus, R + 0x08, 4, UAM_ACC_LE), 0x00010400, "VS 1.4");

    /* admin queues: 4 entries at 0x10000 / 0x11000 */
    wr32(&bus, R + 0x24, 3u << 16 | 3);
    wr64(&bus, R + 0x28, 0x10000);
    wr64(&bus, R + 0x30, 0x11000);
    wr32(&bus, R + 0x14, 1 | 6u << 16 | 4u << 20);
    EQ64(rd(&bus, R + 0x1C, 4, UAM_ACC_LE) & 1, 1, "CSTS.RDY");

    unsigned sq0 = 0, sq1 = 0;
    #define ADMIN(op, cid, prp1, c10, c11) do {                                  \
        uint64_t e_ = 0x10000 + sq0 * 64u;                                      \
        for (unsigned k_ = 0; k_ < 64; k_ += 8) wr64(&bus, e_ + k_, 0);          \
        wr32(&bus, e_, (op) | (uint32_t)(cid) << 16);                            \
        wr64(&bus, e_ + 24, (prp1));                                             \
        wr32(&bus, e_ + 40, (c10));                                              \
        wr32(&bus, e_ + 44, (c11));                                              \
        sq0 = (sq0 + 1) & 3;                                                     \
        wr32(&bus, R + 0x1000, sq0);                                             \
    } while (0)

    ADMIN(0x06, 7, 0x20000, 1, 0);                            /* identify controller */
    uint64_t cqe = 0x11000;
    EQ64(rd(&bus, cqe + 12, 4, UAM_ACC_LE), 7u | 1u << 16, "identify: CID 7, phase 1, success");
    EQ64(rd(&bus, 0x20000 + 24, 4, UAM_ACC_LE), 0x204D4155, "model number \"UAM \"");
    /* I/O queues: CQ 1 (4 entries, 0x12000), SQ 1 (4 entries, 0x13000) */
    ADMIN(0x05, 10, 0x12000, 3u << 16 | 1, 1);
    EQ64(rd(&bus, cqe + 16 + 12, 4, UAM_ACC_LE), 10u | 1u << 16, "create CQ");
    ADMIN(0x01, 11, 0x13000, 3u << 16 | 1, 1u << 16 | 1);
    EQ64(rd(&bus, cqe + 32 + 12, 4, UAM_ACC_LE), 11u | 1u << 16, "create SQ");
    ADMIN(0x01, 12, 0x14000, 3u << 16 | 2, 5u << 16 | 1);
    EQ64(rd(&bus, cqe + 48 + 12, 4, UAM_ACC_LE) >> 17, 0x100, "SQ on a missing CQ refused");

    #define IO(op, cid, prp1, prp2, slba, nlb) do {                              \
        uint64_t e_ = 0x13000 + sq1 * 64u;                                      \
        for (unsigned k_ = 0; k_ < 64; k_ += 8) wr64(&bus, e_ + k_, 0);          \
        wr32(&bus, e_, (op) | (uint32_t)(cid) << 16);                            \
        wr32(&bus, e_ + 4, 1);                                                   \
        wr64(&bus, e_ + 24, (prp1));                                             \
        wr64(&bus, e_ + 32, (prp2));                                             \
        wr64(&bus, e_ + 40, (slba));                                             \
        wr32(&bus, e_ + 48, (nlb) - 1);                                          \
        sq1 = (sq1 + 1) & 3;                                                     \
        wr32(&bus, R + 0x1008, sq1);                                             \
    } while (0)

    /* read 24 sectors from LBA 3 into 0x40000 (PRP1) + a PRP list of two pages */
    wr64(&bus, 0x50000, 0x41000);
    wr64(&bus, 0x50008, 0x42000);
    IO(0x02, 20, 0x40000, 0x50000, 3, 24);
    EQ64(rd(&bus, 0x12000 + 12, 4, UAM_ACC_LE), 20u | 1u << 16, "read: success");
    EQ64(rd(&bus, 0x40000, 1, UAM_ACC_LE), 3, "first sector");
    EQ64(rd(&bus, 0x42000 + 0xFFF, 1, UAM_ACC_LE), 26, "last sector, through the list");
    /* write one sector, read it back elsewhere */
    for (unsigned k = 0; k < 512; k += 4)
        wr32(&bus, 0x60000 + k, 0xA5A5A5A5u);
    IO(0x01, 21, 0x60000, 0, 100, 1);
    IO(0x02, 22, 0x61000, 0, 100, 1);
    EQ64(rd(&bus, 0x12000 + 2 * 16 + 12, 4, UAM_ACC_LE), 22u | 1u << 16, "read back: success");
    EQ64(rd(&bus, 0x61000 + 508, 4, UAM_ACC_LE), 0xA5A5A5A5u, "written data read back");
    IO(0x02, 23, 0x61000, 0, 128, 1);
    EQ64(rd(&bus, 0x12000 + 3 * 16 + 12, 4, UAM_ACC_LE) >> 17, 0x80, "LBA out of range");
    #undef ADMIN
    #undef IO

    wr32(&bus, R + 0x14, 0);                                  /* disable */
    EQ64(rd(&bus, R + 0x1C, 4, UAM_ACC_LE) & 1, 0, "CSTS.RDY cleared");
    uam_pci_bus_device(pb)->ops->destroy(uam_pci_bus_device(pb));
    dev->ops->destroy(dev);
    uam_blockdev_close(bd);
    unlink(path);
    uam_bus_fini(&bus);
}

static void test_ahci(void)
{
    uam_bus bus;
    uam_bus_init(&bus);
    uam_bus_map_ram(&bus, "ram", 0, 1u << 20);
    uam_pci_bus *pb = uam_pci_bus_create("pci", &bus, 0x10000000000ull);
    uam_device *dev = uam_ahci_create("ahci", 2, 0xFFFE, 1, 0);
    char path[] = "/tmp/uam-test-ahci-XXXXXX";
    uam_blockdev *bd = test_disk(path, 512);
    CHECK(bd && uam_ahci_attach(dev, 0, bd, false) == UAM_OK, "attach disk");
    CHECK(uam_pci_register(pb, uam_ahci_func(dev), 2, 0) == UAM_OK, "register");
    const unsigned df = 2 << 3;
    const uint64_t A = 0xF0001000, P = A + 0x100;
    uam_pci_cfg_write(pb, 0, df, UAM_PCI_BAR0 + 20, 4, (uint32_t)A);
    uam_pci_cfg_write(pb, 0, df, UAM_PCI_COMMAND, 2, UAM_PCI_CMD_MEM | UAM_PCI_CMD_MASTER);
    EQ64(rd(&bus, A + 0x0C, 4, UAM_ACC_LE), 3, "PI: two ports");
    EQ64(rd(&bus, P + 0x28, 4, UAM_ACC_LE), 0x123, "port 0: link up");
    EQ64(rd(&bus, P + 0x24, 4, UAM_ACC_LE), 0x101, "port 0: ATA signature");
    EQ64(rd(&bus, P + 0x80 + 0x28, 4, UAM_ACC_LE), 0, "port 1: empty");

    wr32(&bus, P + 0x00, 0x10000);                            /* command list */
    wr32(&bus, P + 0x08, 0x11000);                            /* received FIS */
    wr32(&bus, P + 0x18, (1u << 4) | 1);                      /* FRE, ST */
    EQ64(rd(&bus, 0x11000 + 0x40, 1, UAM_ACC_LE), 0x34, "signature FIS posted");
    /* slot 0: READ DMA EXT, LBA 5, 2 sectors, one PRD to 0x30000 */
    wr32(&bus, 0x10000, 5 | 1u << 16);                        /* CFL 5 dwords, 1 PRD */
    wr32(&bus, 0x10004, 0);
    wr64(&bus, 0x10008, 0x20000);                             /* command table */
    wr32(&bus, 0x20000, 0x27 | 0x80u << 8 | 0x25u << 16);     /* H2D, C, READ DMA EXT */
    wr32(&bus, 0x20004, 5 | 0x40u << 24);                     /* LBA 5, LBA mode */
    wr32(&bus, 0x20008, 0);
    wr32(&bus, 0x2000C, 2);                                   /* count */
    wr64(&bus, 0x20080, 0x30000);
    wr32(&bus, 0x2008C, 1023);                                /* 1024 bytes */
    wr32(&bus, P + 0x38, 1);                                  /* issue */
    EQ64(rd(&bus, P + 0x38, 4, UAM_ACC_LE), 0, "command completed");
    EQ64(rd(&bus, P + 0x20, 4, UAM_ACC_LE) & 0xFF, 0x50, "TFD: ready");
    EQ64(rd(&bus, 0x10004, 4, UAM_ACC_LE), 1024, "PRDBC");
    EQ64(rd(&bus, 0x30000, 1, UAM_ACC_LE), 5, "sector 5");
    EQ64(rd(&bus, 0x30000 + 1023, 1, UAM_ACC_LE), 6, "sector 6");
    EQ64(rd(&bus, P + 0x10, 4, UAM_ACC_LE) & 1, 1, "PxIS.DHRS");
    uam_pci_bus_device(pb)->ops->destroy(uam_pci_bus_device(pb));
    dev->ops->destroy(dev);
    uam_blockdev_close(bd);
    unlink(path);
    uam_bus_fini(&bus);
}

/* ---- PCI expansion ROM ------------------------------------------------------ */

static void test_pci_rom(void)
{
    uam_bus bus;
    uam_bus_init(&bus);
    uam_bus_map_ram(&bus, "ram", 0, 1u << 20);
    uam_pci_bus *pb = uam_pci_bus_create("pci", &bus, 0x10000000000ull);
    static const uint8_t image[] = { 0x55, 0xAA, 0x04, 0xCB };   /* an option ROM header */
    uam_device *rom = uam_pci_rom_image_create("rom", image, sizeof(image));
    uam_pci_func f;
    uam_pci_func_init(&f, "vga", 0xFFFE, 4, 0x03000000, NULL, NULL);
    uam_pci_func_rom(&f, 64 << 10, rom);
    CHECK(uam_pci_register(pb, &f, 3, 0) == UAM_OK, "register");
    const unsigned df = 3 << 3;
    uam_pci_cfg_write(pb, 0, df, 0x30, 4, 0xFFFFFFFFu);
    EQ64(uam_pci_cfg_read(pb, 0, df, 0x30, 4), 0xFFFF0001u, "ROM BAR sizes to 64 KiB");
    uam_pci_cfg_write(pb, 0, df, 0x30, 4, 0xE0000);           /* address, not enabled */
    uam_pci_cfg_write(pb, 0, df, UAM_PCI_COMMAND, 2, UAM_PCI_CMD_MEM);
    uint64_t v = 0;
    CHECK(uam_bus_read(&bus, 0xF0000000, 2, UAM_ACC_LE, &v) != UAM_OK, "nothing at the BAR before");
    uam_pci_cfg_write(pb, 0, df, 0x30, 4, 0xF0000001u);       /* enabled */
    EQ64(rd(&bus, 0xF0000000, 2, UAM_ACC_LE), 0xAA55, "ROM signature through the BAR");
    EQ64(rd(&bus, 0xF0000003, 1, UAM_ACC_LE), 0xCB, "ROM byte 3");
    EQ64(rd(&bus, 0xF0000010, 1, UAM_ACC_LE), 0xFF, "past the image: 0xFF");
    uam_pci_cfg_write(pb, 0, df, 0x30, 4, 0xF0000000u);       /* disabled */
    CHECK(uam_bus_read(&bus, 0xF0000000, 2, UAM_ACC_LE, &v) != UAM_OK, "gone when disabled");
    uam_pci_bus_device(pb)->ops->destroy(uam_pci_bus_device(pb));
    rom->ops->destroy(rom);
    uam_bus_fini(&bus);
}

/* ---- IDE ----------------------------------------------------------------- */

#define IDE_CMD(ch) (0x200000ull + 0x20 * (ch))
#define IDE_CTL(ch) (0x200010ull + 0x20 * (ch))
static uint8_t ib(uam_bus *b, uint64_t a) { return (uint8_t)rd(b, a, 1, UAM_ACC_LE); }
static void ob(uam_bus *b, uint64_t a, uint8_t v) { uam_bus_write(b, a, 1, UAM_ACC_LE, v); }

/* Read `n` bytes of PIO data from a channel. */
static void ide_in(uam_bus *b, unsigned ch, uint8_t *buf, unsigned n)
{
    for (unsigned i = 0; i < n; i += 2) {
        uint64_t w = rd(b, IDE_CMD(ch), 2, UAM_ACC_LE);
        buf[i] = (uint8_t)w;
        if (i + 1 < n)
            buf[i + 1] = (uint8_t)(w >> 8);
    }
}

static void ide_lba(uam_bus *b, unsigned ch, uint32_t lba, uint8_t count, uint8_t cmd)
{
    ob(b, IDE_CMD(ch) + 6, (uint8_t)(0xE0 | ((lba >> 24) & 0xF)));
    ob(b, IDE_CMD(ch) + 2, count);
    ob(b, IDE_CMD(ch) + 3, (uint8_t)lba);
    ob(b, IDE_CMD(ch) + 4, (uint8_t)(lba >> 8));
    ob(b, IDE_CMD(ch) + 5, (uint8_t)(lba >> 16));
    ob(b, IDE_CMD(ch) + 7, cmd);
}

static int irq_level;
static void irq_sink(void *ctx, unsigned in, int level) { (void)ctx; (void)in; irq_level = level; }

/* An ATAPI packet by PIO on channel 1; returns the bytes the device sent. */
static unsigned ide_packet(uam_bus *b, const uint8_t cdb[12], uint16_t limit, uint8_t *out, unsigned cap)
{
    ob(b, IDE_CMD(1) + 1, 0);                       /* PIO */
    ob(b, IDE_CMD(1) + 4, (uint8_t)limit);
    ob(b, IDE_CMD(1) + 5, (uint8_t)(limit >> 8));
    ob(b, IDE_CMD(1) + 7, 0xA0);
    CHECK((ib(b, IDE_CMD(1) + 7) & 0x08) && ib(b, IDE_CMD(1) + 2) == 0x01, "packet: CDB wanted");
    for (unsigned i = 0; i < 12; i += 2)
        uam_bus_write(b, IDE_CMD(1), 2, UAM_ACC_LE, cdb[i] | cdb[i + 1] << 8);
    unsigned got = 0;
    while (ib(b, IDE_CMD(1) + 7) & 0x08) {
        unsigned n = ib(b, IDE_CMD(1) + 4) | ib(b, IDE_CMD(1) + 5) << 8;
        CHECK(n <= limit && ib(b, IDE_CMD(1) + 2) == 0x02, "packet: a data piece within the limit");
        uint8_t tmp[4096];
        ide_in(b, 1, tmp, n);
        memcpy(out + got, tmp, got + n <= cap ? n : (got < cap ? cap - got : 0));
        got += n;
    }
    CHECK(ib(b, IDE_CMD(1) + 2) == 0x03, "packet: complete");
    return got;
}

static void test_ide(void)
{
    uam_bus bus;
    uam_bus_init(&bus);
    uam_bus_map_ram(&bus, "ram", 0, 1u << 20);
    uam_pci_bus *pb = uam_pci_bus_create("pci", &bus, 0x10000000000ull);
    uam_device *dev = uam_ide_create("ide", 0xFFFE, 3, 0);
    char p0[] = "/tmp/uam-test-ide0-XXXXXX", p1[] = "/tmp/uam-test-ide1-XXXXXX";
    uam_blockdev *disk = test_disk(p0, 512), *disc = test_disk(p1, 2048);
    CHECK(uam_ide_attach(dev, 0, 0, disk, false) == UAM_OK, "attach disk");
    CHECK(uam_ide_attach(dev, 1, 0, disc, true) == UAM_OK, "attach disc");
    CHECK(uam_ide_attach(dev, 0, 0, disk, false) != UAM_OK, "drive taken");
    for (unsigned ch = 0; ch < 2; ch++) {
        uam_bus_map_mmio(&bus, ch ? "ide-cmd1" : "ide-cmd0", IDE_CMD(ch), 8, uam_ide_cmd(dev, ch));
        uam_bus_map_mmio(&bus, ch ? "ide-ctl1" : "ide-ctl0", IDE_CTL(ch), 1, uam_ide_ctl(dev, ch));
    }
    CHECK(uam_pci_register(pb, uam_ide_func(dev), 1, 1) == UAM_OK, "register");
    const unsigned df = 1 << 3 | 1;
    uam_pci_cfg_write(pb, 0, df, UAM_PCI_BAR0 + 16, 4, 0xC001);
    uam_pci_cfg_write(pb, 0, df, UAM_PCI_COMMAND, 2, UAM_PCI_CMD_IO | UAM_PCI_CMD_MASTER);
    const uint64_t BM = 0x10000000000ull + 0xC000;
    uam_signal_connect(uam_ide_irq_line(dev, 0), irq_sink, NULL, 0);

    /* signatures */
    EQ64(ib(&bus, IDE_CMD(0) + 7), 0x50, "ch0: disk ready");
    EQ64(ib(&bus, IDE_CMD(1) + 4) | ib(&bus, IDE_CMD(1) + 5) << 8, 0xEB14, "ch1: ATAPI signature");
    ob(&bus, IDE_CMD(0) + 6, 0xB0);                 /* slave: absent */
    EQ64(ib(&bus, IDE_CMD(0) + 7), 0, "ch0 slave: no device");

    /* IDENTIFY */
    uint8_t b[4096];
    ob(&bus, IDE_CMD(0) + 6, 0xA0);
    ob(&bus, IDE_CMD(0) + 7, 0xEC);
    CHECK(irq_level == 1, "IDENTIFY raises INTRQ");
    EQ64(ib(&bus, IDE_CTL(0)), 0x58, "DRQ (alternate status)");
    CHECK(irq_level == 1, "alternate status leaves INTRQ");
    EQ64(ib(&bus, IDE_CMD(0) + 7), 0x58, "DRQ");
    CHECK(irq_level == 0, "status read acknowledges");
    ide_in(&bus, 0, b, 512);
    EQ64(b[120] | b[121] << 8 | (uint32_t)(b[122] | b[123] << 8) << 16, 128, "IDENTIFY: 128 sectors");
    EQ64(ib(&bus, IDE_CMD(0) + 7), 0x50, "IDENTIFY done");

    /* PIO reads: LBA, then CHS (cylinder 0, head 0, sector 2 = LBA 1) */
    ide_lba(&bus, 0, 2, 2, 0x20);
    ide_in(&bus, 0, b, 512);
    EQ64(b[0], 2, "READ SECTORS: sector 2");
    CHECK(ib(&bus, IDE_CMD(0) + 7) & 0x08, "second sector follows");
    ide_in(&bus, 0, b, 512);
    EQ64(b[511], 3, "sector 3");
    EQ64(ib(&bus, IDE_CMD(0) + 7), 0x50, "read done");
    ob(&bus, IDE_CMD(0) + 6, 0xA0);
    ob(&bus, IDE_CMD(0) + 2, 1);
    ob(&bus, IDE_CMD(0) + 3, 2);
    ob(&bus, IDE_CMD(0) + 4, 0);
    ob(&bus, IDE_CMD(0) + 5, 0);
    ob(&bus, IDE_CMD(0) + 7, 0x20);
    ide_in(&bus, 0, b, 512);
    EQ64(b[0], 1, "CHS 0/0/2 is LBA 1");

    /* PIO write, read back */
    ide_lba(&bus, 0, 10, 1, 0x30);
    EQ64(ib(&bus, IDE_CMD(0) + 7), 0x58, "WRITE SECTORS: DRQ for data");
    for (unsigned i = 0; i < 256; i++)
        uam_bus_write(&bus, IDE_CMD(0), 2, UAM_ACC_LE, 0xABAB);
    EQ64(ib(&bus, IDE_CMD(0) + 7), 0x50, "written");
    ide_lba(&bus, 0, 10, 1, 0x20);
    ide_in(&bus, 0, b, 512);
    EQ64(b[300], 0xAB, "read back");

    /* READ MULTIPLE in blocks of 4 */
    ob(&bus, IDE_CMD(0) + 2, 4);
    ob(&bus, IDE_CMD(0) + 7, 0xC6);
    EQ64(ib(&bus, IDE_CMD(0) + 7), 0x50, "SET MULTIPLE MODE 4");
    ide_lba(&bus, 0, 0, 8, 0xC4);
    CHECK(irq_level == 1, "first block: INTRQ");
    (void)ib(&bus, IDE_CMD(0) + 7);
    ide_in(&bus, 0, b, 2048);
    EQ64(b[2047], 3, "first block: sectors 0-3");
    CHECK(irq_level == 1 && (ib(&bus, IDE_CMD(0) + 7) & 0x08), "second block: INTRQ and DRQ");
    ide_in(&bus, 0, b, 2048);
    EQ64(b[0], 4, "second block: from sector 4");
    EQ64(ib(&bus, IDE_CMD(0) + 7), 0x50, "READ MULTIPLE done");

    /* READ SECTORS EXT: the HOB bytes first */
    ob(&bus, IDE_CMD(0) + 6, 0x40);
    ob(&bus, IDE_CMD(0) + 2, 0);  ob(&bus, IDE_CMD(0) + 2, 1);
    ob(&bus, IDE_CMD(0) + 3, 0);  ob(&bus, IDE_CMD(0) + 3, 20);
    ob(&bus, IDE_CMD(0) + 4, 0);  ob(&bus, IDE_CMD(0) + 4, 0);
    ob(&bus, IDE_CMD(0) + 5, 0);  ob(&bus, IDE_CMD(0) + 5, 0);
    ob(&bus, IDE_CMD(0) + 7, 0x24);
    ide_in(&bus, 0, b, 512);
    EQ64(b[0], 20, "READ SECTORS EXT: sector 20");
    ob(&bus, IDE_CMD(0) + 3, 1);  ob(&bus, IDE_CMD(0) + 3, 20);   /* LBA 2^24 + 20: past the end */
    ob(&bus, IDE_CMD(0) + 7, 0x24);
    EQ64(ib(&bus, IDE_CMD(0) + 7), 0x41, "READ SECTORS EXT: the HOB byte counts (ID not found)");

    /* bus-master READ DMA: LBA 5, two sectors, one PRD */
    wr32(&bus, 0x8000, 0x30000);
    wr32(&bus, 0x8004, 1024 | 0x80000000u);
    wr32(&bus, BM + 4, 0x8000);
    ide_lba(&bus, 0, 5, 2, 0xC8);
    EQ64(rd(&bus, 0x30000, 1, UAM_ACC_LE), 0, "nothing before the bus master starts");
    ob(&bus, BM, 0x09);                             /* start, to memory */
    EQ64(rd(&bus, 0x30000, 1, UAM_ACC_LE), 5, "DMA: sector 5");
    EQ64(rd(&bus, 0x30000 + 1023, 1, UAM_ACC_LE), 6, "DMA: sector 6");
    EQ64(ib(&bus, BM + 2) & 0x07, 0x04, "bus master: interrupt, not active");
    EQ64(ib(&bus, IDE_CMD(0) + 7), 0x50, "DMA done");
    ob(&bus, BM + 2, 0x04);
    EQ64(ib(&bus, BM + 2) & 0x04, 0, "interrupt bit cleared");

    /* ATAPI: INQUIRY in 16-byte pieces; the first other command sees UNIT ATTENTION */
    ob(&bus, IDE_CMD(1) + 6, 0xA0);
    const uint8_t inq[12] = { 0x12, 0, 0, 0, 36 };
    EQ64(ide_packet(&bus, inq, 16, b, sizeof(b)), 36, "INQUIRY: 36 bytes");
    EQ64(b[0], 0x05, "INQUIRY: CD/DVD");
    const uint8_t tur[12] = { 0x00 };
    ide_packet(&bus, tur, 0xFFFE, b, sizeof(b));
    CHECK(ib(&bus, IDE_CMD(1) + 7) & 0x01, "TEST UNIT READY: unit attention");
    const uint8_t rd10[12] = { 0x28, 0, 0, 0, 0, 3, 0, 0, 1 };
    EQ64(ide_packet(&bus, rd10, 0xFFFE, b, sizeof(b)), 2048, "READ(10): one block");
    EQ64(b[0], 3, "READ(10): block 3");
    ob(&bus, IDE_CMD(1) + 7, 0x20);                 /* an ATA read on a packet device */
    CHECK(ib(&bus, IDE_CMD(1) + 7) & 0x01, "ATA command aborts on ATAPI");
    EQ64(ib(&bus, IDE_CMD(1) + 5), 0xEB, "with the signature");

    /* software reset, nIEN */
    ob(&bus, IDE_CTL(0), 0x04);
    ob(&bus, IDE_CTL(0), 0x02);                     /* release SRST, interrupts off */
    EQ64(ib(&bus, IDE_CMD(0) + 7), 0x50, "after SRST: ready");
    EQ64(ib(&bus, IDE_CMD(0) + 3), 1, "after SRST: signature");
    ob(&bus, IDE_CMD(0) + 7, 0xEC);
    CHECK(irq_level == 0, "nIEN masks INTRQ");

    uam_pci_bus_device(pb)->ops->destroy(uam_pci_bus_device(pb));
    dev->ops->destroy(dev);
    uam_blockdev_close(disk);
    uam_blockdev_close(disc);
    unlink(p0);
    unlink(p1);
    uam_bus_fini(&bus);
}

/* ---- VGA -------------------------------------------------------------------- */

#define VGA_IO  0x300000ull                         /* port p at VGA_IO + p - 0x3B0 */
#define VGA_MEM 0x320000ull                         /* the 0xA0000 window */
static void vout(uam_bus *b, unsigned port, uint8_t v) { ob(b, VGA_IO + port - 0x3B0, v); }
static uint8_t vin(uam_bus *b, unsigned port) { return ib(b, VGA_IO + port - 0x3B0); }

/* A mode set from the IBM VGA register tables, as a VGA BIOS does it. */
static void vga_mode(uam_bus *b, uint8_t misc, const uint8_t seq[5], const uint8_t crtc[25],
                     const uint8_t gc[9], const uint8_t attr[21])
{
    vout(b, 0x3C2, misc);
    for (unsigned i = 0; i < 5; i++) { vout(b, 0x3C4, (uint8_t)i); vout(b, 0x3C5, seq[i]); }
    vout(b, 0x3D4, 0x11); vout(b, 0x3D5, 0);        /* unprotect */
    for (unsigned i = 0; i < 25; i++) { vout(b, 0x3D4, (uint8_t)i); vout(b, 0x3D5, crtc[i]); }
    for (unsigned i = 0; i < 9; i++) { vout(b, 0x3CE, (uint8_t)i); vout(b, 0x3CF, gc[i]); }
    (void)vin(b, 0x3DA);                            /* the attribute flip-flop to index */
    for (unsigned i = 0; i < 21; i++) { vout(b, 0x3C0, (uint8_t)i); vout(b, 0x3C0, attr[i]); }
    vout(b, 0x3C0, 0x20);                           /* palette address source: display on */
}

static void dac_set(uam_bus *b, uint8_t i, uint8_t r, uint8_t g, uint8_t bl)
{
    vout(b, 0x3C8, i); vout(b, 0x3C9, r); vout(b, 0x3C9, g); vout(b, 0x3C9, bl);
}

static const uint8_t pal16[16] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07,
                                   0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F };

static void test_vga(void)
{
    uam_bus bus;
    uam_bus_init(&bus);
    uam_device *dev = uam_vga_create("vga", 0xFFFE, 5, 0);
    uam_bus_map_mmio(&bus, "vga-io", VGA_IO, UAM_VGA_IO_SIZE, uam_vga_io(dev));
    uam_bus_map_mmio(&bus, "vga-mem", VGA_MEM, UAM_VGA_MEM_SIZE, uam_vga_mem(dev));
    static uint32_t rgb[640 * 480];
    unsigned w = 0, h = 0, cols = 0, rows = 0;

    /* mode 03h: 80x25 text */
    static const uint8_t s3[5] = { 0x03, 0x00, 0x03, 0x00, 0x02 };
    static const uint8_t c3[25] = { 0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F, 0x00, 0x4F, 0x0D,
                                    0x0E, 0x00, 0x00, 0x00, 0x00, 0x9C, 0x8E, 0x8F, 0x28, 0x1F,
                                    0x96, 0xB9, 0xA3, 0xFF };
    static const uint8_t g3[9] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0E, 0x00, 0xFF };
    uint8_t a3[21];
    memcpy(a3, pal16, 16);
    a3[16] = 0x0C; a3[17] = 0x00; a3[18] = 0x0F; a3[19] = 0x08; a3[20] = 0x00;
    vga_mode(&bus, 0x67, s3, c3, g3, a3);
    CHECK(uam_vga_text_mode(dev, &cols, &rows) && cols == 80 && rows == 25, "mode 3: 80x25 text (%ux%u)",
          cols, rows);
    ob(&bus, VGA_MEM + 0x18000, 'H');
    ob(&bus, VGA_MEM + 0x18001, 0x1F);              /* white on blue */
    ob(&bus, VGA_MEM + 0x18002, 'i');
    EQ64(ib(&bus, VGA_MEM + 0x18000), 'H', "text reads back (odd/even)");
    uint16_t cells[80 * 25];
    EQ64(uam_vga_text(dev, cells, 80 * 25), 2000, "2000 cells");
    EQ64(cells[0], 'H' | 0x1F00, "cell 0: 'H' on blue");
    EQ64(cells[1] & 0xFF, 'i', "cell 1");
    /* a glyph for 'H' in plane 2: the font loads the way a BIOS does it */
    vout(&bus, 0x3C4, 2); vout(&bus, 0x3C5, 0x04);  /* plane 2 */
    vout(&bus, 0x3C4, 4); vout(&bus, 0x3C5, 0x06);  /* sequential addressing */
    vout(&bus, 0x3CE, 5); vout(&bus, 0x3CF, 0x00);
    vout(&bus, 0x3CE, 6); vout(&bus, 0x3CF, 0x04);  /* A0000, 64 KiB */
    ob(&bus, VGA_MEM + 'H' * 32, 0xFF);             /* scan line 0: all foreground */
    vout(&bus, 0x3C4, 2); vout(&bus, 0x3C5, 0x03);
    vout(&bus, 0x3C4, 4); vout(&bus, 0x3C5, 0x02);
    vout(&bus, 0x3CE, 5); vout(&bus, 0x3CF, 0x10);
    vout(&bus, 0x3CE, 6); vout(&bus, 0x3CF, 0x0E);
    dac_set(&bus, 0x3F, 63, 63, 63);
    dac_set(&bus, 0x01, 0, 0, 42);
    CHECK(uam_vga_render(dev, rgb, 640, 480, &w, &h) && w == 640 && h == 400, "text renders 640x400");
    EQ64(rgb[0], 0xFFFFFF, "glyph pixel: white");
    EQ64(rgb[640], 0x0000AA, "below it: the blue background");
    vout(&bus, 0x3C7, 0x01);                        /* the DAC reads back */
    EQ64(vin(&bus, 0x3C9) | vin(&bus, 0x3C9) << 8 | vin(&bus, 0x3C9) << 16, 0x2A0000, "DAC read back");
    vout(&bus, 0x3D4, 0x11); vout(&bus, 0x3D5, 0x80);   /* protect CRTC 0-7 */
    vout(&bus, 0x3D4, 0x01); vout(&bus, 0x3D5, 0x27);
    EQ64(vin(&bus, 0x3D5), 0x4F, "CRTC write protection");
    bool retrace = false, display = false;
    for (unsigned i = 0; i < 16; i++) {
        uint8_t st = vin(&bus, 0x3DA);
        retrace |= (st & 0x08) != 0;
        display |= !(st & 0x08);
    }
    CHECK(retrace && display, "input status 1: vertical retrace comes and goes");

    /* mode 12h: 640x480, 16 colours, planar */
    static const uint8_t s12[5] = { 0x03, 0x01, 0x0F, 0x00, 0x06 };
    static const uint8_t c12[25] = { 0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0x0B, 0x3E, 0x00, 0x40, 0x00,
                                     0x00, 0x00, 0x00, 0x00, 0x00, 0xEA, 0x8C, 0xDF, 0x28, 0x00,
                                     0xE7, 0x04, 0xE3, 0xFF };
    static const uint8_t g12[9] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x0F, 0xFF };
    uint8_t a12[21];
    memcpy(a12, pal16, 16);
    a12[16] = 0x01; a12[17] = 0x00; a12[18] = 0x0F; a12[19] = 0x00; a12[20] = 0x00;
    vga_mode(&bus, 0xE3, s12, c12, g12, a12);
    CHECK(!uam_vga_text_mode(dev, NULL, NULL), "mode 12h: graphics");
    ob(&bus, VGA_MEM, 0);                           /* clear what mode 3 left (write mode 0) */
    ob(&bus, VGA_MEM + 1, 0);
    vout(&bus, 0x3CE, 5); vout(&bus, 0x3CF, 0x02);  /* write mode 2 */
    vout(&bus, 0x3CE, 8); vout(&bus, 0x3CF, 0x80);  /* bit mask: pixel 0 */
    (void)ib(&bus, VGA_MEM);                        /* load the latches */
    ob(&bus, VGA_MEM, 5);
    vout(&bus, 0x3CE, 5); vout(&bus, 0x3CF, 0x08);  /* read mode 1 */
    vout(&bus, 0x3CE, 2); vout(&bus, 0x3CF, 0x05);  /* colour compare 5 */
    EQ64(ib(&bus, VGA_MEM), 0x80, "read mode 1: pixel 0 is colour 5");
    vout(&bus, 0x3CE, 5); vout(&bus, 0x3CF, 0x00);  /* write mode 0, set/reset to colour 3 */
    vout(&bus, 0x3CE, 0); vout(&bus, 0x3CF, 0x03);
    vout(&bus, 0x3CE, 1); vout(&bus, 0x3CF, 0x0F);
    vout(&bus, 0x3CE, 8); vout(&bus, 0x3CF, 0xFF);
    (void)ib(&bus, VGA_MEM + 1);
    ob(&bus, VGA_MEM + 1, 0x00);
    dac_set(&bus, 5, 63, 0, 0);
    dac_set(&bus, 3, 0, 42, 42);
    CHECK(uam_vga_render(dev, rgb, 640, 480, &w, &h) && w == 640 && h == 480, "mode 12h renders 640x480");
    EQ64(rgb[0], 0xFF0000, "pixel 0: colour 5");
    EQ64(rgb[1], 0x000000, "pixel 1: untouched by the bit mask");
    EQ64(rgb[8], 0x00AAAA, "pixels 8-15: set/reset colour 3");

    /* mode 13h: 320x200, 256 colours, chain-4 */
    static const uint8_t s13[5] = { 0x03, 0x01, 0x0F, 0x00, 0x0E };
    static const uint8_t c13[25] = { 0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F, 0x00, 0x41, 0x00,
                                     0x00, 0x00, 0x00, 0x00, 0x00, 0x9C, 0x8E, 0x8F, 0x28, 0x40,
                                     0x96, 0xB9, 0xA3, 0xFF };
    static const uint8_t g13[9] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F, 0xFF };
    uint8_t a13[21];
    for (unsigned i = 0; i < 16; i++) a13[i] = (uint8_t)i;
    a13[16] = 0x41; a13[17] = 0x00; a13[18] = 0x0F; a13[19] = 0x00; a13[20] = 0x00;
    vga_mode(&bus, 0x63, s13, c13, g13, a13);
    ob(&bus, VGA_MEM + 320 * 2 + 5, 0x77);
    EQ64(ib(&bus, VGA_MEM + 320 * 2 + 5), 0x77, "chain-4 reads back");
    dac_set(&bus, 0x77, 0, 63, 0);
    CHECK(uam_vga_render(dev, rgb, 640, 480, &w, &h) && w == 320 && h == 200, "mode 13h renders 320x200");
    EQ64(rgb[2 * 320 + 5], 0x00FF00, "pixel (5, 2): colour 0x77");
    dev->ops->destroy(dev);
    uam_bus_fini(&bus);
}

/* ---- 8237 DMA ---------------------------------------------------------------- */

static void test_i8237(void)
{
    uam_bus bus;
    uam_bus_init(&bus);
    uam_bus_map_ram(&bus, "ram", 0, 1u << 20);
    uam_device *dma = uam_i8237_create("dma", &bus);
    const uint64_t D1 = 0x200000, D2 = 0x200100, PG = 0x200200;
    uam_bus_map_mmio(&bus, "dma1", D1, 16, uam_i8237_dma1(dma));
    uam_bus_map_mmio(&bus, "dma2", D2, 32, uam_i8237_dma2(dma));
    uam_bus_map_mmio(&bus, "page", PG, 15, uam_i8237_page(dma));

    /* channel 2: write 4 bytes to 0x51234 */
    uint8_t buf[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    ob(&bus, D1 + 0xC, 0);                          /* flip-flop */
    ob(&bus, D1 + 4, 0x34); ob(&bus, D1 + 4, 0x12);
    ob(&bus, D1 + 5, 0x03); ob(&bus, D1 + 5, 0x00);
    ob(&bus, PG + 0, 0x05);                         /* port 0x81: channel 2's page */
    ob(&bus, D1 + 0xB, 0x46);                       /* single, write to memory, channel 2 */
    EQ64(uam_i8237_transfer(dma, 2, buf, 8, NULL), 0, "masked: nothing moves");
    ob(&bus, D1 + 0xA, 0x02);                       /* unmask channel 2 */
    bool tc = false;
    EQ64(uam_i8237_transfer(dma, 2, buf, 8, &tc), 4, "four bytes to terminal count");
    CHECK(tc, "terminal count");
    EQ64(rd(&bus, 0x51234, 4, UAM_ACC_LE), 0x04030201, "in memory at page:address");
    EQ64(ib(&bus, D1 + 8) & 0x04, 0x04, "status: channel 2 TC");
    EQ64(ib(&bus, D1 + 8) & 0x04, 0, "TC bits clear on read");
    ob(&bus, D1 + 0xC, 0);
    EQ64(ib(&bus, D1 + 4) | ib(&bus, D1 + 4) << 8, 0x1238, "current address advanced");
    EQ64(ib(&bus, D1 + 5) | ib(&bus, D1 + 5) << 8, 0xFFFF, "count wrapped");
    EQ64(uam_i8237_transfer(dma, 2, buf, 8, NULL), 0, "masked again after TC");

    /* channel 5 (words): read 4 bytes from 0x20200 */
    wr32(&bus, 0x20200, 0xDDCCBBAA);
    ob(&bus, D2 + (0xC << 1), 0);
    ob(&bus, D2 + (2 << 1), 0x00); ob(&bus, D2 + (2 << 1), 0x01);   /* word address 0x100 */
    ob(&bus, D2 + (3 << 1), 0x01); ob(&bus, D2 + (3 << 1), 0x00);   /* 2 words */
    ob(&bus, PG + 0xA, 0x02);                       /* port 0x8B: channel 5's page */
    ob(&bus, D2 + (0xB << 1), 0x49);                /* single, read from memory, channel 5 */
    ob(&bus, D2 + (0xA << 1), 0x01);
    uint8_t got[4] = { 0 };
    EQ64(uam_i8237_transfer(dma, 5, got, 4, NULL), 4, "two words");
    EQ64(got[0] | got[1] << 8 | got[2] << 16 | (uint32_t)got[3] << 24, 0xDDCCBBAA,
         "word channel: page bits 17-23, address << 1");
    dma->ops->destroy(dma);
    uam_bus_fini(&bus);
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

/* ---- debugger reads of device regions (uam_device_ops.peek) ------------- */

typedef struct peekdev {
    uam_device dev;
    unsigned   reads, peeks;
} peekdev;

static uam_status pd_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    (void)size;
    UAM_CONTAINER_OF(d, peekdev, dev)->reads++;   /* a side effect */
    *val = 0xA0 + off;
    return UAM_OK;
}

static uam_status pd_peek(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    (void)size;
    UAM_CONTAINER_OF(d, peekdev, dev)->peeks++;
    *val = 0xA0 + off;
    return UAM_OK;
}

static uam_status pd_write(uam_device *d, uint64_t off, unsigned size, uint64_t val)
{
    (void)d; (void)off; (void)size; (void)val;
    return UAM_OK;
}

static void pd_destroy(uam_device *d) { (void)d; }

static const uam_device_ops pd_plain_ops = {
    .type = "test,plain", .impl_min = 1, .impl_max = 1, .read = pd_read, .write = pd_write,
    .destroy = pd_destroy, .flags = UAM_DEVICE_STATELESS,
};
static const uam_device_ops pd_peek_ops = {
    .type = "test,peek", .impl_min = 1, .impl_max = 1, .read = pd_read, .write = pd_write,
    .destroy = pd_destroy, .flags = UAM_DEVICE_STATELESS, .peek = pd_peek,
};

/* A debug read of a device region goes to its peek handler, never its read
 * handler; without one it faults. Fetches from devices still fault. */
static void test_device_peek(void)
{
    uam_bus bus;
    uam_bus_init(&bus);
    peekdev plain = { 0 }, pk = { 0 };
    uam_device_init(&plain.dev, &pd_plain_ops, "plain");
    uam_device_init(&pk.dev, &pd_peek_ops, "peek");
    CHECK(uam_bus_map_mmio(&bus, "plain", 0x1000, 0x100, &plain.dev) == UAM_OK, "map plain");
    CHECK(uam_bus_map_mmio(&bus, "peek", 0x2000, 0x100, &pk.dev) == UAM_OK, "map peek");
    uint64_t v = 0;
    CHECK(uam_bus_read(&bus, 0x1004, 1, UAM_ACC_DEBUG, &v) == UAM_ERR_FAULT, "no peek: debug read faults");
    EQ64(plain.reads, 0, "no peek: the read handler was not called");
    CHECK(uam_bus_read(&bus, 0x2004, 4, UAM_ACC_LE | UAM_ACC_DEBUG, &v) == UAM_OK, "peek: debug read");
    EQ64(v, 0xA7A6A5A4, "peek: bytes as a read would give them");
    EQ64(pk.peeks, 4, "peek: served byte by byte (impl_max 1)");
    EQ64(pk.reads, 0, "peek: the read handler was not called");
    CHECK(uam_bus_read(&bus, 0x2004, 4, UAM_ACC_LE | UAM_ACC_FETCH, &v) == UAM_ERR_FAULT,
          "peek: fetching from a device still faults");
    EQ64(rd(&bus, 0x2004, 1, 0), 0xA4, "peek: a normal read uses the read handler");
    EQ64(pk.reads, 1, "peek: normal read counted");
    CHECK(uam_bus_write(&bus, 0x2004, 1, UAM_ACC_DEBUG, 0) == UAM_ERR_FAULT, "debug writes to devices fault");
    uam_bus_fini(&bus);
}

/* The master abort reads all ones at any width and drops writes; it names up
 * to 4 windows. */
static void test_master_abort(void)
{
    uam_device *ma = uam_master_abort_create("ma");
    uam_bus bus;
    uam_bus_init(&bus);
    uam_bus_set_unmapped(&bus, ma);
    uam_log_level saved = uam_log_threshold;
    uam_log_threshold = UAM_LOG_ERROR;   /* its warnings are expected here */
    EQ64(rd(&bus, 0x12345678, 1, 0), 0xFF, "master abort: byte");
    EQ64(rd(&bus, 0x12345678, 8, 0), ~0ull, "master abort: quadword");
    CHECK(uam_bus_write(&bus, 0x12345678, 4, 0, 0) == UAM_OK, "master abort: write dropped");
    uint64_t v;
    CHECK(uam_bus_read(&bus, 0x12345678, 4, UAM_ACC_DEBUG, &v) == UAM_ERR_FAULT,
          "master abort: a debugger sees unmapped memory as unmapped");
    for (int i = 0; i < 4; i++)
        CHECK(uam_master_abort_name_window(ma, (uint64_t)i << 32, 0x1000, "w", i & 1), "window %d", i);
    CHECK(!uam_master_abort_name_window(ma, 5ull << 32, 0x1000, "w", false), "no fifth window");
    uam_bus_fini(&bus);
    ma->ops->destroy(ma);
    uam_log_threshold = saved;
}

/* ---- x86 CPUID platform hook ------------------------------------------- */

typedef struct cpuid_seen {
    uint32_t leaf, sub, module_ebx;
    unsigned calls;
} cpuid_seen;

static void cpuid_hook(void *ctx, uint32_t leaf, uint32_t sub, uint32_t r[4])
{
    cpuid_seen *c = ctx;
    c->calls++;
    c->leaf = leaf;
    c->sub = sub;
    c->module_ebx = r[1];
    if (leaf == 0x80000001)
        r[1] = 0x12345678;   /* a leaf register the module leaves 0 */
}

static uam_hook_action cpuid_halt(void *ctx, uam_cpu *cpu, uint64_t pc)
{
    (void)ctx; (void)cpu; (void)pc;
    return UAM_HOOK_STOP;
}

static uint64_t x86_reg(uam_machine *m, unsigned gdb_regno)
{
    uint64_t v = 0;
    m->cpu.isa->gdb->read_reg(&m->cpu, gdb_regno, &v);
    return v;
}

/* The board's cpuid hook sees the module's answer for the leaf and subleaf the
 * instruction asked for, and what it changes is what the guest reads. */
static void test_x86_cpuid_hook(void)
{
    static const uint8_t prog[] = {
        0x66, 0xB8, 0x01, 0x00, 0x00, 0x80,   /* mov eax, 0x80000001 */
        0x66, 0xB9, 0x07, 0x00, 0x00, 0x00,   /* mov ecx, 7 */
        0x0F, 0xA2,                           /* cpuid */
        0xEB, 0xFE,                           /* 14: jmp $ */
    };
    for (int hooked = 0; hooked <= 1; hooked++) {
        uam_machine m;
        cpuid_seen seen = { 0 };
        CHECK(uam_machine_init(&m, &uam_isa_x86_64) == UAM_OK, "x86 machine init");
        CHECK(uam_bus_map_ram(&m.bus, "ram", 0, 1 << 20) == UAM_OK, "map ram");
        CHECK(uam_bus_load_bytes(&m.bus, 0, prog, sizeof(prog)) == UAM_OK, "load code");
        uam_machine_reset(&m, 0);
        if (hooked)
            uam_x86_set_platform(&m.cpu, &(uam_x86_platform){ .ctx = &seen, .cpuid = cpuid_hook });
        uam_exec_add_pc_hook(&m.exec, 14, cpuid_halt, NULL);
        EQ64(uam_exec_run(&m.exec, 100), UAM_STOP_HOOK, "cpuid program ran to its end");
        /* gdb amd64 numbering: rax 0, rbx 1, rcx 2, rdx 3 */
        EQ64(x86_reg(&m, 1), hooked ? 0x12345678 : 0, hooked ? "hooked EBX of leaf 0x80000001" : "module EBX of leaf 0x80000001");
        EQ64(x86_reg(&m, 3) & (1u << 29), 1u << 29, "module's long-mode bit kept");
        if (hooked) {
            EQ64(seen.calls, 1, "hook called once");
            EQ64(seen.leaf, 0x80000001, "hook saw the leaf");
            EQ64(seen.sub, 7, "hook saw the subleaf");
            EQ64(seen.module_ebx, 0, "hook saw the module's answer");
        }
        uam_machine_fini(&m);
    }
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
    test_subpage_cache();
    test_timer_reset();
    test_device_reset();
    test_romd_exec();
    test_region_remap();
    test_cfi_flash();
    test_pci();
    test_pci_cam();
    test_pci_msi_mem();
    test_nvme();
    test_ahci();
    test_pci_rom();
    test_ide();
    test_i8237();
    test_vga();
    test_code_lines();
    test_wait_livelock();
    test_device_peek();
    test_master_abort();
    test_x86_cpuid_hook();
    test_abi();
    printf("test_core: %d/%d checks passed\n", n_checks - n_failed, n_checks);
    return n_failed ? 1 : 0;
}
