/*
 * test_mmio.c - Per-register MMIO rules (uam/dev/mmio_stub.h, uam_board_add_mmio_rules):
 * rule parsing, every policy, set/clear masks, register width and narrow accesses,
 * priorities (over catch-all windows and holes, under real devices, later rules
 * winning), the input stream, snapshots, and a guest reading a rule through the
 * interpreter and the JIT on a big-endian CPU.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uam/boards.h"
#include "uam/dev/unimp.h"
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

#define EQ64(got, want, what) do {                                         \
        uint64_t g_ = (got), w_ = (want);                                  \
        CHECK(g_ == w_, "%s: got 0x%llx want 0x%llx", what,                \
              (unsigned long long)g_, (unsigned long long)w_);             \
    } while (0)

/* ---- parsing ------------------------------------------------------------- */

static void test_parse(void)
{
    uam_mmio_rule r;
    char err[128];
    CHECK(uam_mmio_rule_parse("0x18040000=fixed:0x1", &r, err, sizeof(err)) == UAM_OK, "fixed");
    EQ64(r.addr, 0x18040000, "addr");
    EQ64(r.size, 4, "size defaults to the width");
    EQ64(r.width, 0, "width left at its default");
    EQ64(r.policy, UAM_MMIO_FIXED, "policy");
    EQ64(r.value, 1, "value");

    CHECK(uam_mmio_rule_parse("0x18020000+0x100=echo,set=0x60,clear=0x1", &r, err, sizeof(err))
          == UAM_OK, "echo with masks");
    EQ64(r.size, 0x100, "size");
    EQ64(r.policy, UAM_MMIO_ECHO, "echo");
    EQ64(r.set, 0x60, "set");
    EQ64(r.clear, 1, "clear");

    CHECK(uam_mmio_rule_parse("4096=input,width=1", &r, err, sizeof(err)) == UAM_OK, "input");
    EQ64(r.addr, 4096, "decimal address");
    EQ64(r.size, 1, "size follows width");
    EQ64(r.policy, UAM_MMIO_INPUT, "input policy");
    CHECK(uam_mmio_rule_parse("0x0+8=zero,width=8", &r, err, sizeof(err)) == UAM_OK, "zero");
    CHECK(uam_mmio_rule_parse("0x10+0x10=fault", &r, err, sizeof(err)) == UAM_OK, "fault");

    static const char *const bad[] = {
        "0x1000", "=zero", "0x1000=fixed", "0x1000=fixed:", "0x1000=maybe", "0x1002=zero",
        "0x1000+6=zero", "0x1000+0=zero", "0x1000=zero,width=3", "0x1000=zero,width=16",
        "0x1000=zero,colour=1", "0x1000=zero,set", "zz=zero", "0x1000+q=zero",
        "0xfffffffffffffffc+8=zero", "0x1000=fixed:-1",
    };
    for (size_t i = 0; i < UAM_ARRAY_LEN(bad); i++) {
        err[0] = 0;
        CHECK(uam_mmio_rule_parse(bad[i], &r, err, sizeof(err)) == UAM_ERR_INVALID && err[0],
              "'%s' refused with a reason", bad[i]);
    }

    char d[96];
    uam_mmio_rule f = { .policy = UAM_MMIO_FIXED, .value = 0x80, .set = 1 };
    uam_mmio_rule_describe(&f, d, sizeof(d));
    CHECK(!strcmp(d, "fixed 0x80 set=0x1"), "describe: '%s'", d);
}

/* ---- a machine ------------------------------------------------------------ */

#define DEV_PHYS   0x1F000000u   /* a real device: reads 0xDEC0DE */
#define BLOCK_PHYS 0x1F000000u   /* a catch-all window over the whole block */

static uam_status real_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    (void)d; (void)off; (void)size;
    *val = 0xDEC0DE;
    return UAM_OK;
}

static uam_status real_write(uam_device *d, uint64_t off, unsigned size, uint64_t val)
{
    (void)d; (void)off; (void)size; (void)val;
    return UAM_OK;
}

static void real_destroy(uam_device *d) { free(d); }

static const uam_device_ops real_ops = {
    .type = "test,real", .endian = UAM_ACC_BE, .read = real_read, .write = real_write,
    .destroy = real_destroy, .flags = UAM_DEVICE_STATELESS,
};

static void build(uam_machine *m)
{
    CHECK(uam_machine_init(m, &uam_isa_mips32be) == UAM_OK, "machine init");
    m->board_name = "mmio-test";
    CHECK(uam_bus_map_ram(&m->bus, "ram", 0, 1u << 20) == UAM_OK, "map ram");
    uam_device *real = calloc(1, sizeof(*real));
    uam_device_init(real, &real_ops, "real");
    uam_machine_add_device(m, real);
    CHECK(uam_bus_map_mmio(&m->bus, "real", DEV_PHYS, 0x100, real) == UAM_OK, "map real device");
    uam_device *win = uam_unimp_create("block", BLOCK_PHYS);
    uam_machine_add_device(m, win);
    CHECK(uam_bus_map_mmio_ex(&m->bus, "block", BLOCK_PHYS, 0x10000, win,
                              &(uam_map_attrs){ .prio = UAM_PRIO_BACKGROUND }) == UAM_OK,
          "map catch-all window");
}

/* What a big-endian CPU reads (the bus adapts device byte order for it). */
static uint64_t rd(uam_machine *m, uint64_t pa, unsigned size, uam_status *st)
{
    uint64_t v = 0;
    uam_status s = uam_bus_read(&m->bus, pa, size, UAM_ACC_BE, &v);
    if (st)
        *st = s;
    return v;
}

static uam_status wr(uam_machine *m, uint64_t pa, unsigned size, uint64_t v)
{
    return uam_bus_write(&m->bus, pa, size, UAM_ACC_BE, v);
}

static void parse_all(const char *const *specs, size_t n, uam_mmio_rule *out)
{
    char err[128];
    for (size_t i = 0; i < n; i++)
        CHECK(uam_mmio_rule_parse(specs[i], &out[i], err, sizeof(err)) == UAM_OK, "parse '%s': %s",
              specs[i], err);
}

static int exhausted_calls;
static void on_exhausted(void *ctx) { (void)ctx; exhausted_calls++; }

static void test_policies(void)
{
    uam_machine m;
    build(&m);
    static const char *const specs[] = {
        "0x1F001000=fixed:0x12345678",              /* 0: in the catch-all window */
        "0x1F002000+0x10=echo,set=0x1",             /* 1 */
        "0x1F002008=zero",                          /* 2: overrides rule 1 there */
        "0x1F000000+0x10=fixed:0x55",               /* 3: under the real device */
        "0x1F003000+0x100=fault",                   /* 4 */
        "0x10000000=fixed:0xA5A5,width=2",          /* 5: an unmapped hole */
        "0x1F004000+4=input,width=1",               /* 6 */
        "0x1F004010=input",                         /* 7 */
        "0x1F005000=fixed:0xFF00,clear=0xF000,set=0x3", /* 8 */
    };
    uam_mmio_rule rules[UAM_ARRAY_LEN(specs)];
    parse_all(specs, UAM_ARRAY_LEN(specs), rules);
    uam_mmio_input *in = uam_mmio_input_create();
    static const uint8_t bytes[] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77 };
    uam_mmio_input_set(in, bytes, sizeof(bytes));
    uam_mmio_input_on_exhausted(in, on_exhausted, NULL);
    CHECK(uam_board_add_mmio_rules(&m, rules, UAM_ARRAY_LEN(rules), in) == UAM_OK, "add rules");

    uam_status st;
    /* FIXED, as the CPU sees it, whole and in parts (big-endian byte lanes) */
    EQ64(rd(&m, 0x1F001000, 4, &st), 0x12345678, "fixed word");
    CHECK(st == UAM_OK, "fixed read ok");
    EQ64(rd(&m, 0x1F001000, 1, NULL), 0x12, "fixed: byte 0 is the most significant (BE)");
    EQ64(rd(&m, 0x1F001003, 1, NULL), 0x78, "fixed: byte 3");
    EQ64(rd(&m, 0x1F001002, 2, NULL), 0x5678, "fixed: low halfword");
    CHECK(wr(&m, 0x1F001000, 4, 0) == UAM_OK, "write to fixed is dropped");
    EQ64(rd(&m, 0x1F001000, 4, NULL), 0x12345678, "fixed unchanged");
    EQ64(rd(&m, 0x1F001004, 4, NULL), 0, "next to the rule: the catch-all window (RAZ)");

    /* ECHO with set, per register, narrow writes merging */
    EQ64(rd(&m, 0x1F002000, 4, NULL), 1, "echo before any write: set bit only");
    wr(&m, 0x1F002000, 4, 0xF0);
    EQ64(rd(&m, 0x1F002000, 4, NULL), 0xF1, "echo + set");
    wr(&m, 0x1F002004, 4, 0xAABBCC00);
    wr(&m, 0x1F002005, 1, 0x99);
    EQ64(rd(&m, 0x1F002004, 4, NULL), 0xAA99CC01, "narrow write merges into its register");
    EQ64(rd(&m, 0x1F002000, 4, NULL), 0xF1, "registers are separate");
    wr(&m, 0x1F002008, 4, 0x1234);
    EQ64(rd(&m, 0x1F002008, 4, NULL), 0, "later rule wins where rules overlap");
    wr(&m, 0x1F00200C, 4, 0x1234);
    EQ64(rd(&m, 0x1F00200C, 4, NULL), 0x1235, "the earlier rule still answers elsewhere");

    /* priorities against real devices */
    EQ64(rd(&m, 0x1F000000, 4, NULL), 0xDEC0DE, "a real device wins over a rule");

    /* FAULT */
    rd(&m, 0x1F003010, 4, &st);
    CHECK(st != UAM_OK, "fault rule: read is a bus error");
    CHECK(wr(&m, 0x1F003010, 4, 1) != UAM_OK, "fault rule: write is a bus error");

    /* an unmapped hole, 2-byte registers */
    EQ64(rd(&m, 0x10000000, 2, NULL), 0xA5A5, "rule fills an unmapped hole");
    rd(&m, 0x10000004, 4, &st);
    CHECK(st != UAM_OK, "beyond the rule the hole still faults");

    /* set and clear */
    EQ64(rd(&m, 0x1F005000, 4, NULL), 0x0F03, "(value & ~clear) | set");

    /* INPUT: width 1 takes one byte per register read, width 4 takes four (LE order) */
    EQ64(rd(&m, 0x1F004000, 1, NULL), 0x11, "input byte 0");
    EQ64(rd(&m, 0x1F004001, 1, NULL), 0x22, "input byte 1");
    EQ64(rd(&m, 0x1F004010, 4, NULL), 0x66554433, "input word: first byte least significant");
    EQ64(uam_mmio_input_pos(in), 6, "six bytes consumed");
    CHECK(exhausted_calls == 0 && !uam_mmio_input_exhausted(in), "not exhausted yet");
    EQ64(rd(&m, 0x1F004010, 4, NULL), 0x77, "last byte, then zeros");
    CHECK(exhausted_calls == 1 && uam_mmio_input_exhausted(in), "exhaustion reported once");
    EQ64(rd(&m, 0x1F004000, 1, NULL), 0, "exhausted: 0");
    CHECK(exhausted_calls == 1, "and only once");
    uam_mmio_input_set(in, bytes, 2);
    EQ64(rd(&m, 0x1F004002, 1, NULL), 0x11, "a new input starts over");

    /* counters */
    for (uam_device *d = m.devices; d; d = d->next)
        if (d->ops->type && !strcmp(d->ops->type, "uam,mmio-stub")
            && uam_mmio_stub_rule(d)->addr == 0x1F001000) {
            EQ64(uam_mmio_stub_reads(d), 5, "reads counted");
            EQ64(uam_mmio_stub_writes(d), 1, "writes counted");
        }

    uam_machine_fini(&m);
    uam_mmio_input_free(in);
}

/* Echo values and the input position come back with a snapshot. */
static void test_snapshot(void)
{
    uam_machine m;
    build(&m);
    static const char *const specs[] = { "0x1F002000+0x10=echo", "0x1F004000=input,width=1" };
    uam_mmio_rule rules[2];
    parse_all(specs, 2, rules);
    uam_mmio_input *in = uam_mmio_input_create();
    static const uint8_t bytes[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    uam_mmio_input_set(in, bytes, sizeof(bytes));
    CHECK(uam_board_add_mmio_rules(&m, rules, 2, in) == UAM_OK, "add rules");
    uam_machine_reset(&m, 0x80000000u);

    wr(&m, 0x1F002004, 4, 0x1111);
    rd(&m, 0x1F004000, 1, NULL);
    uam_snapshot *s = NULL;
    CHECK(uam_snapshot_take(&m, &s) == UAM_OK, "take (stubs and the input stream save state)");
    wr(&m, 0x1F002004, 4, 0x2222);
    wr(&m, 0x1F002008, 4, 0x3333);
    uint64_t a = rd(&m, 0x1F004000, 1, NULL), b = rd(&m, 0x1F004000, 1, NULL);
    CHECK(uam_snapshot_restore(&m, s) == UAM_OK, "restore");
    EQ64(rd(&m, 0x1F002004, 4, NULL), 0x1111, "echo value restored");
    EQ64(rd(&m, 0x1F002008, 4, NULL), 0, "register written after the snapshot forgotten");
    EQ64(uam_mmio_input_pos(in), 1, "input position restored");
    EQ64(rd(&m, 0x1F004000, 1, NULL), a, "same input again");
    EQ64(rd(&m, 0x1F004000, 1, NULL), b, "and the next");

    /* fuzz loop shape: restore, feed a new input, run */
    static const uint8_t other[] = { 0x7F };
    CHECK(uam_snapshot_restore(&m, s) == UAM_OK, "restore again");
    uam_mmio_input_set(in, other, 1);
    EQ64(rd(&m, 0x1F004000, 1, NULL), 0x7F, "input set after a restore is read from the start");

    /* a machine without the same rules refuses the snapshot */
    uam_machine other_m;
    build(&other_m);
    CHECK(uam_snapshot_restore(&other_m, s) == UAM_ERR_INVALID, "rules are part of the machine");
    uam_machine_fini(&other_m);

    uam_snapshot_free(s);
    uam_machine_fini(&m);
    uam_mmio_input_free(in);
}

static void test_refusals(void)
{
    uam_machine m;
    build(&m);
    uam_mmio_rule r = { .addr = 0x1F006000, .size = 4, .policy = UAM_MMIO_INPUT };
    fprintf(stderr, "(expected rule errors follow)\n");
    CHECK(uam_board_add_mmio_rules(&m, &r, 1, NULL) == UAM_ERR_INVALID, "input rule needs a stream");
    uam_mmio_rule odd = { .addr = 0x1F006001, .size = 4, .policy = UAM_MMIO_ZERO };
    CHECK(uam_board_add_mmio_rules(&m, &odd, 1, NULL) == UAM_ERR_INVALID, "misaligned rule refused");
    CHECK(uam_board_add_mmio_rules(&m, &odd, UAM_MAX_MMIO_RULES + 1, NULL) == UAM_ERR_INVALID,
          "too many rules refused");
    uam_machine_fini(&m);
}

/*
 * The CPU's view: a guest loads the FIXED register (kseg1) and stores it to RAM, on
 * the interpreter and on the JIT (whose MMIO accesses go through its slow path).
 *   lui t0, 0xBF00 ; lw t1, 0x1000(t0) ; lbu t2, 0x1003(t0) ; lui t3, 0x8001
 *   sw t1, 0(t3) ; sw t2, 4(t3) ; b . ; nop
 */
static void test_guest(void)
{
    for (int jit = 0; jit < 2; jit++) {
        uam_machine m;
        build(&m);
        uam_mmio_rule r;
        CHECK(uam_mmio_rule_parse("0x1F001000=fixed:0x12345678", &r, NULL, 0) == UAM_OK, "parse");
        CHECK(uam_board_add_mmio_rules(&m, &r, 1, NULL) == UAM_OK, "add rule");
        static const uint32_t prog[] = {
            0x3C08BF00u, 0x8D091000u, 0x910A1003u, 0x3C0B8001u,
            0xAD690000u, 0xAD6A0004u, 0x1000FFFFu, 0x00000000u,
        };
        for (size_t i = 0; i < UAM_ARRAY_LEN(prog); i++) {
            uint8_t b[4] = { (uint8_t)(prog[i] >> 24), (uint8_t)(prog[i] >> 16),
                             (uint8_t)(prog[i] >> 8), (uint8_t)prog[i] };
            uam_bus_load_bytes(&m.bus, 4 * i, b, 4);
        }
        uam_machine_reset(&m, 0x80000000u);
        if (jit)
            CHECK(uam_exec_enable_jit(&m.exec), "jit");
        uam_exec_run(&m.exec, 100);
        EQ64(rd(&m, 0x10000, 4, NULL), 0x12345678, jit ? "jit: lw of the rule" : "interp: lw of the rule");
        EQ64(rd(&m, 0x10004, 4, NULL), 0x78, jit ? "jit: lbu of byte 3" : "interp: lbu of byte 3");
        uam_machine_fini(&m);
    }
}

int main(void)
{
    test_parse();
    test_policies();
    test_snapshot();
    test_refusals();
    test_guest();
    printf("test_mmio: %d/%d checks passed\n", n_checks - n_failed, n_checks);
    return n_failed ? 1 : 0;
}
