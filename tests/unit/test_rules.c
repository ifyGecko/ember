/*
 * test_rules.c - Rules (uam/rules.h): a condition and actions at a guest pc.
 *
 * An RV32 guest, hand-encoded, builds a frame holding a pointer and a value, then waits
 * until the word behind the pointer equals the value - which nothing in the machine ever
 * stores - and after that counts in a loop:
 *
 *   00 lui  sp, 0x8          sp = 0x8000
 *   04 lui  t0, 0x10         the word waited on: 0x10000
 *   08 sw   t0, 0(sp)        frame: its address
 *   0c addi t1, zero, 0x55
 *   10 sw   t1, 4(sp)        frame: the value expected there
 *   14 lw   t2, 0(sp)        WAIT
 *   18 lw   t3, 0(t2)
 *   1c lw   t4, 4(sp)
 *   20 bne  t3, t4, 14
 *   24 addi s2, zero, 1      the wait is over
 *   28 addi a0, a0, 1        COUNT
 *   2c addi t5, t5, 1        MID (inside COUNT's block)
 *   30 j    28
 *
 * Checked on the interpreter and the JIT: a rule supplies the value (through the frame,
 * and at a physical address by a symbol's pc); a rule moves the pc past the wait; a
 * condition picks one arrival to stop at; count=N bounds how often a rule fires, and a
 * snapshot keeps that count (and fits only a machine with the same counted rules); log
 * prints registers, memory and C's operators with C's precedence, also at a pc inside a
 * block; an operand that cannot be read fails its condition or action without stopping
 * the run; cpu=N picks one of two processors; a detached set no longer runs. Access rules
 * (write=, read=, access=) run when the accessing instruction has completed, with the
 * address and the value, on a range an access only overlaps, for one processor of two,
 * and only once armed; delay=N carries a rule's stores out that many ticks after it
 * fired, with the values of then, at the same instruction on both backends, never firing
 * again in between, and a snapshot keeps stores that are on their way; dump prints memory.
 * A MIPS32 big-endian guest checks the byte order and virtual addresses (kseg0, kseg1),
 * also of an access rule's range. Malformed rules are refused with a reason.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uam/isa/mips32.h"
#include "uam/isa/riscv.h"
#include "uam/machine.h"
#include "uam/rules.h"
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

enum { WAIT = 0x14, DONE = 0x24, COUNT = 0x28, MID = 0x2C };
#define WORD_PA 0x10000u

enum { RZ = 0, RSP = 2, RT0 = 5, RT1, RT2, RA0 = 10, RS2 = 18, RT3 = 28, RT4, RT5 };
static uint32_t rv_i(unsigned op, unsigned f3, unsigned rd, unsigned rs1, int32_t imm)
{
    return ((uint32_t)imm & 0xFFF) << 20 | rs1 << 15 | f3 << 12 | rd << 7 | op;
}
static uint32_t ADDI(unsigned rd, unsigned rs1, int32_t v) { return rv_i(0x13, 0, rd, rs1, v); }
static uint32_t LW(unsigned rd, int32_t off, unsigned rs1)  { return rv_i(0x03, 2, rd, rs1, off); }
static uint32_t LUI(unsigned rd, uint32_t imm20)            { return imm20 << 12 | rd << 7 | 0x37; }
static uint32_t SW(unsigned rs2, int32_t off, unsigned rs1)
{
    return ((uint32_t)off >> 5 & 0x7F) << 25 | rs2 << 20 | rs1 << 15 | 2u << 12
         | ((uint32_t)off & 0x1F) << 7 | 0x23;
}
static uint32_t BNE(unsigned rs1, unsigned rs2, uint32_t at, uint32_t to)
{
    uint32_t o = to - at;
    return (o >> 12 & 1) << 31 | (o >> 5 & 0x3F) << 25 | rs2 << 20 | rs1 << 15 | 1u << 12
         | (o >> 1 & 0xF) << 8 | (o >> 11 & 1) << 7 | 0x63;
}
static uint32_t J(uint32_t at, uint32_t to)
{
    uint32_t o = to - at;
    return (o >> 20 & 1) << 31 | (o >> 1 & 0x3FF) << 21 | (o >> 11 & 1) << 20
         | (o >> 12 & 0xFF) << 12 | 0x6F;
}

static void put_words(uam_machine *m, uint32_t pa, const uint32_t *w, size_t n, bool be)
{
    for (size_t i = 0; i < n; i++) {
        const uint32_t v = w[i];
        uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
        if (be) {
            const uint8_t r[4] = { b[3], b[2], b[1], b[0] };
            memcpy(b, r, 4);
        }
        CHECK(uam_bus_load_bytes(&m->bus, pa + 4 * i, b, 4) == UAM_OK, "load word");
    }
}

static const char *mode_name(bool jit) { return jit ? "jit" : "interp"; }

static void build(uam_machine *m, bool jit, unsigned more_cpus)
{
    CHECK(uam_machine_init(m, &uam_isa_rv32) == UAM_OK, "machine init");
    m->board_name = "rules-test";
    CHECK(uam_bus_map_ram(&m->bus, "ram", 0, 1u << 20) == UAM_OK, "map ram");
    if (more_cpus)
        CHECK(uam_machine_add_cpus(m, more_cpus) == UAM_OK, "more processors");
    const uint32_t p[] = {
        LUI(RSP, 0x8), LUI(RT0, 0x10), SW(RT0, 0, RSP), ADDI(RT1, RZ, 0x55), SW(RT1, 4, RSP),
        LW(RT2, 0, RSP), LW(RT3, 0, RT2), LW(RT4, 4, RSP), BNE(RT3, RT4, 0x20, WAIT),
        ADDI(RS2, RZ, 1),
        ADDI(RA0, RA0, 1), ADDI(RT5, RT5, 1), J(0x30, COUNT),
    };
    put_words(m, 0, p, UAM_ARRAY_LEN(p), false);
    uam_machine_reset(m, 0);
    if (jit)
        for (unsigned i = 0; i < uam_machine_ncpus(m); i++)
            CHECK(uam_exec_enable_jit(uam_machine_exec(m, i)), "jit available");
}

static uint64_t reg(uam_machine *m, unsigned cpu, const char *name)
{
    uam_cpu *c = uam_machine_cpu(m, cpu);
    unsigned r = 0;
    uint64_t v = ~UINT64_C(0);
    CHECK(uam_isa_reg_by_name(c->isa, name, &r) && c->isa->gdb->read_reg(c, r, &v),
          "register %s", name);
    return v;
}

static uint64_t word(uam_machine *m, uint64_t pa, unsigned size)
{
    uint64_t v = ~UINT64_C(0);
    CHECK(uam_bus_read(&m->bus, pa, size, UAM_ACC_LE | UAM_ACC_DEBUG, &v) == UAM_OK,
          "read 0x%llx", (unsigned long long)pa);
    return v;
}

static uam_rules *rules_new(uam_machine *m)
{
    uam_rules *rs = uam_rules_create(m, "test");
    CHECK(rs != NULL, "rule set");
    return rs;
}

static int add(uam_rules *rs, const char *text)
{
    char err[256] = "";
    const int i = uam_rules_add(rs, text, err, sizeof(err));
    CHECK(i >= 0, "rule '%s' refused: %s", text, err);
    return i;
}

static uam_rule_info info(const uam_rules *rs, int i)
{
    uam_rule_info ri = { 0 };
    CHECK(uam_rules_get(rs, (size_t)i, &ri), "rule %d", i);
    return ri;
}

static void run(uam_machine *m, uint64_t n, const char *what, bool jit)
{
    const uam_stop_reason why = uam_exec_run(&m->exec, n);
    CHECK(why == UAM_STOP_INSN_LIMIT, "%s (%s): stopped by %s", what, mode_name(jit),
          uam_stop_reason_str(why));
}

static bool symbols(void *ctx, const char *name, uint64_t *value)
{
    (void)ctx;
    if (!strcmp(name, "wait"))
        *value = WAIT;
    else if (!strcmp(name, "the_word"))
        *value = WORD_PA;
    else
        return false;
    return true;
}

/* ---- standing in for what the machine does not model ------------------------ */

static void test_stand_in(bool jit)
{
    const char *md = mode_name(jit);
    uam_machine m;

    /* Nothing supplies the value: the guest waits for ever. */
    build(&m, jit, 0);
    run(&m, 5000, "no rule", jit);
    CHECK(reg(&m, 0, "s2") == 0, "%s: the wait ended by itself", md);
    uam_machine_fini(&m);

    /* Through the guest's own frame: the pointer it holds, the value it expects. */
    build(&m, jit, 0);
    uam_rules *rs = rules_new(&m);
    int r = add(rs, "pc=0x14,set [[sp]]=[sp+4]");
    run(&m, 5000, "frame rule", jit);
    CHECK(reg(&m, 0, "s2") == 1, "%s: the wait did not end", md);
    CHECK(word(&m, WORD_PA, 4) == 0x55, "%s: the word is 0x%llx", md,
          (unsigned long long)word(&m, WORD_PA, 4));
    uam_rule_info ri = info(rs, r);
    CHECK(ri.arrivals == 1 && ri.fired == 1 && ri.failed == 0 && ri.pc == WAIT && ri.limit == 0,
          "%s: frame rule arrivals=%llu fired=%llu failed=%llu", md,
          (unsigned long long)ri.arrivals, (unsigned long long)ri.fired,
          (unsigned long long)ri.failed);
    CHECK(!strcmp(ri.name, "pc=0x14"), "%s: default name '%s'", md, ri.name);
    CHECK(reg(&m, 0, "a0") > 100, "%s: the guest did not go on counting", md);
    uam_machine_fini(&m);
    uam_rules_free(rs);

    /* At a symbol's pc, a physical address, a narrower store. */
    build(&m, jit, 0);
    rs = rules_new(&m);
    uam_rules_set_symbols(rs, symbols, NULL);
    add(rs, "pc=wait,set [p:the_word]:1=0x50+5");
    run(&m, 5000, "symbol rule", jit);
    CHECK(reg(&m, 0, "s2") == 1 && word(&m, WORD_PA, 4) == 0x55, "%s: symbol rule", md);
    uam_machine_fini(&m);
    uam_rules_free(rs);

    /* Moving the pc instead: the wait is skipped and the word stays as it was. */
    build(&m, jit, 0);
    rs = rules_new(&m);
    add(rs, "pc = 0x10 + 4 , set pc = 0x24");
    run(&m, 5000, "pc rule", jit);
    CHECK(reg(&m, 0, "s2") == 1 && word(&m, WORD_PA, 4) == 0, "%s: pc rule", md);
    uam_machine_fini(&m);
    uam_rules_free(rs);
}

/* ---- conditions, stop, count ------------------------------------------------- */

static void test_cond_count(bool jit)
{
    const char *md = mode_name(jit);
    uam_machine m;
    build(&m, jit, 0);
    uam_rules *rs = rules_new(&m);
    add(rs, "pc=0x14,set [[sp]]=[sp+4]");
    const int stop = add(rs, "pc=0x28,name=fifth,if a0==5,if t5<6,stop");
    const int inc = add(rs, "pc=0x28,count=3,set t6=t6+2");
    uam_rule_stop rsn;
    CHECK(!uam_rules_last_stop(rs, &rsn), "%s: a stop before any ran", md);

    uam_stop_reason why = uam_exec_run(&m.exec, 5000);
    CHECK(why == UAM_STOP_HOOK, "%s: stopped by %s", md, uam_stop_reason_str(why));
    CHECK(reg(&m, 0, "a0") == 5 && m.cpu.pc == COUNT, "%s: stopped at a0=%llu pc=0x%llx", md,
          (unsigned long long)reg(&m, 0, "a0"), (unsigned long long)m.cpu.pc);
    CHECK(uam_rules_last_stop(rs, &rsn) && !strcmp(rsn.name, "fifth") && rsn.pc == COUNT
          && rsn.cpu == 0 && rsn.icount == m.cpu.icount, "%s: the stop's record", md);
    CHECK(info(rs, stop).fired == 1 && info(rs, stop).arrivals == 6, "%s: stop rule fired=%llu "
          "arrivals=%llu", md, (unsigned long long)info(rs, stop).fired,
          (unsigned long long)info(rs, stop).arrivals);

    /* The run goes on from there; the stop does not fire again. */
    run(&m, 5000, "after the stop", jit);
    CHECK(info(rs, stop).fired == 1, "%s: the stop fired again", md);
    uam_rule_info ri = info(rs, inc);
    CHECK(ri.fired == 3 && ri.limit == 3 && ri.arrivals > 100, "%s: counted rule fired=%llu "
          "arrivals=%llu", md, (unsigned long long)ri.fired, (unsigned long long)ri.arrivals);
    CHECK(reg(&m, 0, "t6") == 6, "%s: t6=%llu", md, (unsigned long long)reg(&m, 0, "t6"));

    /* Detached, the rules no longer run. */
    uam_rules_detach(rs);
    const uint64_t arrivals = info(rs, stop).arrivals;
    run(&m, 1000, "detached", jit);
    CHECK(info(rs, stop).arrivals == arrivals, "%s: a detached rule ran", md);
    uam_rules_free(rs);
    uam_machine_fini(&m);
}

/* ---- log ---------------------------------------------------------------------- */

static void test_log(bool jit)
{
    const char *md = mode_name(jit);
    uam_machine m;
    build(&m, jit, 0);
    uam_rules *rs = rules_new(&m);
    FILE *out = tmpfile();
    CHECK(out != NULL, "tmpfile");
    uam_rules_set_log(rs, out);
    add(rs, "pc=0x14,set [[sp]]=[sp+4]");
    add(rs, "pc=0x28,count=1,name=ops,log 1+2*3 (1+2)*3 1<<4|1 5&3==3 -1>>60 !0 ~0&0xff 7-2-1 "
            "2<3&&3<=3 0||0 6^3 2>=3 1!=1 3>2 [sp+4]:1+[sp+4]:2");
    /* a pc inside COUNT's block */
    add(rs, "pc=0x2c,count=2,name=mid,log a0 [sp+4]:4 [p:0x10000]:1 pc");
    add(rs, "pc=0x24,name=bare,log");
    add(rs, "pc=0x24,name=unreadable,log [p:0x40000000] s2");
    run(&m, 5000, "log", jit);

    char text[2048] = "";
    rewind(out);
    const size_t n = fread(text, 1, sizeof(text) - 1, out);
    text[n] = '\0';
    fclose(out);
    static const char *const want[] = {
        "rule bare: cpu 0 pc 0x24 icount=9\n",
        "rule unreadable: cpu 0 pc 0x24 [p:0x40000000]=? s2=0x0 icount=9\n",
        "rule ops: cpu 0 pc 0x28 1+2*3=0x7 (1+2)*3=0x9 1<<4|1=0x11 5&3==3=0x1 -1>>60=0xf !0=0x1 "
        "~0&0xff=0xff 7-2-1=0x4 2<3&&3<=3=0x1 0||0=0x0 6^3=0x5 2>=3=0x0 1!=1=0x0 3>2=0x1 "
        "[sp+4]:1+[sp+4]:2=0xaa icount=10\n",
        "rule mid: cpu 0 pc 0x2c a0=0x1 [sp+4]:4=0x55 [p:0x10000]:1=0x55 pc=0x2c icount=11\n",
        "rule mid: cpu 0 pc 0x2c a0=0x2 [sp+4]:4=0x55 [p:0x10000]:1=0x55 pc=0x2c icount=14\n",
    };
    const char *at = text;
    for (size_t i = 0; i < UAM_ARRAY_LEN(want); i++) {
        const size_t len = strlen(want[i]);
        CHECK(!strncmp(at, want[i], len), "%s: log line %zu is not\n  %s  in\n%s", md, i, want[i],
              text);
        if (strncmp(at, want[i], len))
            break;
        at += len;
    }
    CHECK(!*at || n_failed, "%s: more log lines than expected:\n%s", md, at);
    uam_rules_free(rs);
    uam_machine_fini(&m);
}

/* ---- what cannot be carried out ------------------------------------------------ */

static void test_failures(bool jit)
{
    const char *md = mode_name(jit);
    uam_machine m;
    build(&m, jit, 0);
    uam_rules *rs = rules_new(&m);
    add(rs, "pc=0x14,set [[sp]]=[sp+4]");
    const int store = add(rs, "pc=0x28,count=2,set [p:0x40000000]:4=1,set t6=9");
    const int cond = add(rs, "pc=0x28,if [0x50000000]:4==0,stop");
    const int src = add(rs, "pc=0x28,count=1,set t5=[p:0x40000000]:2");
    run(&m, 3000, "failures", jit);
    uam_rule_info ri = info(rs, store);
    CHECK(ri.fired == 2 && ri.failed == 2, "%s: store rule fired=%llu failed=%llu", md,
          (unsigned long long)ri.fired, (unsigned long long)ri.failed);
    CHECK(reg(&m, 0, "t6") == 9, "%s: the action after a failed one did not run", md);
    ri = info(rs, cond);
    CHECK(ri.fired == 0 && ri.failed == ri.arrivals && ri.arrivals > 100,
          "%s: condition rule fired=%llu failed=%llu arrivals=%llu", md,
          (unsigned long long)ri.fired, (unsigned long long)ri.failed,
          (unsigned long long)ri.arrivals);
    ri = info(rs, src);
    CHECK(ri.fired == 1 && ri.failed == 1, "%s: source rule fired=%llu failed=%llu", md,
          (unsigned long long)ri.fired, (unsigned long long)ri.failed);
    uam_rules_free(rs);
    uam_machine_fini(&m);
}

/* ---- counts and snapshots ------------------------------------------------------ */

static void test_snapshot(bool jit)
{
    const char *md = mode_name(jit);
    uam_machine m;
    build(&m, jit, 0);
    uam_rules *rs = rules_new(&m);
    add(rs, "pc=0x14,set [[sp]]=[sp+4]");
    const int inc = add(rs, "pc=0x28,count=3,set t6=t6+1");
    while (info(rs, inc).fired < 2 && m.cpu.icount < 1000)
        uam_exec_run(&m.exec, 1);
    CHECK(info(rs, inc).fired == 2 && reg(&m, 0, "t6") == 2, "%s: two firings", md);

    uam_snapshot *s = NULL;
    CHECK(uam_snapshot_take(&m, &s) == UAM_OK && s, "%s: take", md);
    run(&m, 500, "past the count", jit);
    CHECK(info(rs, inc).fired == 3 && reg(&m, 0, "t6") == 3, "%s: three firings", md);
    CHECK(uam_snapshot_restore(&m, s) == UAM_OK, "%s: restore", md);
    CHECK(info(rs, inc).fired == 2 && reg(&m, 0, "t6") == 2, "%s: the count did not come back "
          "(fired=%llu)", md, (unsigned long long)info(rs, inc).fired);
    run(&m, 500, "past the count again", jit);
    CHECK(info(rs, inc).fired == 3 && reg(&m, 0, "t6") == 3, "%s: the third firing again", md);
    uam_machine_fini(&m);
    uam_rules_free(rs);

    /* Another machine with the same counted rules takes the snapshot ... */
    build(&m, jit, 0);
    rs = rules_new(&m);
    add(rs, "pc=0x14,set [[sp]]=[sp+4]");
    const int inc2 = add(rs, "pc=0x28,count=3,set t6=t6+1");
    CHECK(uam_snapshot_restore(&m, s) == UAM_OK, "%s: restore elsewhere", md);
    CHECK(info(rs, inc2).fired == 2, "%s: the count elsewhere", md);
    run(&m, 500, "elsewhere", jit);
    CHECK(reg(&m, 0, "t6") == 3, "%s: t6 elsewhere", md);
    uam_machine_fini(&m);
    uam_rules_free(rs);

    /* ... one whose counted rule differs, or that has none, does not. (Each refusal
     * logs an error.) */
    build(&m, jit, 0);
    rs = rules_new(&m);
    add(rs, "pc=0x28,count=4,set t6=t6+1");
    CHECK(uam_snapshot_restore(&m, s) != UAM_OK, "%s: restored over another counted rule", md);
    uam_machine_fini(&m);
    uam_rules_free(rs);

    build(&m, jit, 0);
    rs = rules_new(&m);
    add(rs, "pc=0x14,set [[sp]]=[sp+4]");
    CHECK(uam_snapshot_restore(&m, s) != UAM_OK, "%s: restored without the counted rule", md);
    uam_machine_fini(&m);
    uam_rules_free(rs);
    uam_snapshot_free(s);
}

/* ---- two processors -------------------------------------------------------------- */

static void test_smp(bool jit)
{
    const char *md = mode_name(jit);
    uam_machine m;
    build(&m, jit, 1);
    uam_rules *rs = rules_new(&m);
    FILE *out = tmpfile();
    CHECK(out != NULL, "tmpfile");
    uam_rules_set_log(rs, out);
    add(rs, "pc=0x14,set [[sp]]=[sp+4]");
    const int one = add(rs, "pc=0x28,cpu=1,count=1,set t6=7,log a0");
    const int both = add(rs, "pc=0x28,if a0==0,set s2=s2+4");
    run(&m, 20000, "two processors", jit);
    CHECK(reg(&m, 0, "t6") == 0 && reg(&m, 1, "t6") == 7, "%s: t6 is %llu and %llu", md,
          (unsigned long long)reg(&m, 0, "t6"), (unsigned long long)reg(&m, 1, "t6"));
    CHECK(info(rs, one).fired == 1, "%s: cpu=1 rule fired %llu times", md,
          (unsigned long long)info(rs, one).fired);
    CHECK(reg(&m, 0, "s2") == 5 && reg(&m, 1, "s2") == 5 && info(rs, both).fired == 2,
          "%s: a rule for both processors: s2 %llu and %llu", md,
          (unsigned long long)reg(&m, 0, "s2"), (unsigned long long)reg(&m, 1, "s2"));
    char text[256] = "";
    rewind(out);
    const size_t n = fread(text, 1, sizeof(text) - 1, out);
    text[n] = '\0';
    fclose(out);
    CHECK(!strncmp(text, "rule pc=0x28: cpu 1 pc 0x28 a0=0x0 icount=", 42), "%s: the log names "
          "the processor: %s", md, text);

    char err[256] = "";
    CHECK(uam_rules_add(rs, "pc=0x28,cpu=2,stop", err, sizeof(err)) < 0 && strstr(err, "0..1"),
          "%s: cpu=2 of two accepted (%s)", md, err);
    uam_machine_fini(&m);
    uam_rules_free(rs);
}

/* ---- access rules ------------------------------------------------------------------ */

static size_t slurp(FILE *out, char *text, size_t cap)
{
    rewind(out);
    const size_t n = fread(text, 1, cap - 1, out);
    text[n] = '\0';
    fclose(out);
    return n;
}

static void test_access(bool jit)
{
    const char *md = mode_name(jit);
    uam_machine m;
    char text[2048];

    /* The guest's store of the value it expects is the request; the rule answers it. */
    build(&m, jit, 0);
    uam_rules *rs = rules_new(&m);
    FILE *out = tmpfile();
    uam_rules_set_log(rs, out);
    int r = add(rs, "write=p:0x8004,len=4,name=req,set [[sp]]=value,log pc addr value [addr]");
    run(&m, 5000, "write rule", jit);
    CHECK(reg(&m, 0, "s2") == 1 && word(&m, WORD_PA, 4) == 0x55, "%s: write rule", md);
    uam_rule_info ri = info(rs, r);
    CHECK(ri.arrivals == 1 && ri.fired == 1 && ri.failed == 0 && ri.addr == 0x8004 && ri.len == 4,
          "%s: write rule arrivals=%llu fired=%llu", md, (unsigned long long)ri.arrivals,
          (unsigned long long)ri.fired);
    slurp(out, text, sizeof(text));
    /* after the storing instruction: pc is the next one, memory holds the value */
    CHECK(!strcmp(text, "rule req: cpu 0 pc 0x10 write 4 at 0x8004 value 0x55 pc=0x14 addr=0x8004 "
                        "value=0x55 [addr]=0x55 icount=5\n"), "%s: write rule's line: %s", md, text);
    CHECK(m.bus.n_watches == 1, "%s: %u watches", md, (unsigned)m.bus.n_watches);
    uam_rules_detach(rs);
    CHECK(m.bus.n_watches == 0, "%s: a detached access rule still watches", md);
    uam_rules_free(rs);
    uam_machine_fini(&m);

    /* A read rule: the value appears after the first load of the word, for the second. */
    build(&m, jit, 0);
    rs = rules_new(&m);
    out = tmpfile();
    uam_rules_set_log(rs, out);
    r = add(rs, "read=p:0x10000,len=4,count=1,set [addr]:4=0x55,log t3");
    run(&m, 5000, "read rule", jit);
    CHECK(reg(&m, 0, "s2") == 1 && info(rs, r).fired == 1 && info(rs, r).arrivals == 2,
          "%s: read rule fired=%llu arrivals=%llu", md, (unsigned long long)info(rs, r).fired,
          (unsigned long long)info(rs, r).arrivals);
    slurp(out, text, sizeof(text));
    CHECK(!strcmp(text, "rule read=p:0x10000: cpu 0 pc 0x18 read 4 at 0x10000 value 0x0 t3=0x0 "
                        "icount=7\n"), "%s: read rule's line: %s", md, text);
    uam_machine_fini(&m);
    uam_rules_free(rs);

    /* A range that accesses only overlap, in both directions, with a condition. */
    build(&m, jit, 0);
    rs = rules_new(&m);
    out = tmpfile();
    uam_rules_set_log(rs, out);
    add(rs, "access = p:0x8002 , len = 4 , count=3, name=any, log");
    add(rs, "access=p:0x8002,len=4,if value==0x55,count=2,name=v,log");
    add(rs, "pc=0x14,set [[sp]]=[sp+4]");
    run(&m, 5000, "access rule", jit);
    slurp(out, text, sizeof(text));
    CHECK(!strcmp(text, "rule any: cpu 0 pc 0x8 write 4 at 0x8000 value 0x10000 icount=3\n"
                        "rule any: cpu 0 pc 0x10 write 4 at 0x8004 value 0x55 icount=5\n"
                        "rule v: cpu 0 pc 0x10 write 4 at 0x8004 value 0x55 icount=5\n"
                        "rule any: cpu 0 pc 0x14 read 4 at 0x8000 value 0x10000 icount=6\n"
                        "rule v: cpu 0 pc 0x1c read 4 at 0x8004 value 0x55 icount=8\n"),
          "%s: access rules' lines:\n%s", md, text);
    uam_machine_fini(&m);
    uam_rules_free(rs);

    /* stop: the run ends after the accessing instruction, which the record names. */
    build(&m, jit, 0);
    rs = rules_new(&m);
    add(rs, "write=p:0x8004,len=4,name=halt,stop");
    uam_stop_reason why = uam_exec_run(&m.exec, 5000);
    uam_rule_stop rsn = { 0 };
    CHECK(why == UAM_STOP_HOOK && m.cpu.pc == WAIT && m.cpu.icount == 5, "%s: access stop: %s at "
          "0x%llx", md, uam_stop_reason_str(why), (unsigned long long)m.cpu.pc);
    CHECK(uam_rules_last_stop(rs, &rsn) && !strcmp(rsn.name, "halt") && rsn.pc == 0x10
          && rsn.icount == 5, "%s: access stop's record", md);
    run(&m, 100, "after the access stop", jit);
    uam_machine_fini(&m);
    uam_rules_free(rs);

    /* Two sets (a board's, a tool's) with rules on the same store both run, and a run that
     * ends with that instruction has run them before it returns. */
    build(&m, jit, 0);
    rs = rules_new(&m);
    uam_rules *other = uam_rules_create(&m, "other");
    char oerr[256] = "";
    r = add(rs, "write=p:0x8004,len=4,set t6=value");
    const int o = uam_rules_add(other, "access=p:0x8006,set [[sp]]=value", oerr, sizeof(oerr));
    CHECK(o >= 0, "second set: %s", oerr);
    run(&m, 5, "to the store", jit);
    uam_rule_info oi = { 0 };
    CHECK(info(rs, r).fired == 1 && uam_rules_get(other, (size_t)o, &oi) && oi.fired == 1
          && reg(&m, 0, "t6") == 0x55 && word(&m, WORD_PA, 4) == 0x55,
          "%s: two sets on one store fired %llu and %llu times", md,
          (unsigned long long)info(rs, r).fired, (unsigned long long)oi.fired);
    uam_rules_detach(other);
    uam_rules_free(other);
    uam_machine_fini(&m);
    uam_rules_free(rs);

    /* Armed later: nothing is watched until then. */
    build(&m, jit, 0);
    rs = rules_new(&m);
    uam_rules_arm_later(rs);
    r = add(rs, "write=p:0x8004,len=4,set [[sp]]=value");
    CHECK(m.bus.n_watches == 0, "%s: armed at once despite arm_later", md);
    run(&m, 5000, "unarmed", jit);
    CHECK(reg(&m, 0, "s2") == 0 && info(rs, r).arrivals == 0, "%s: an unarmed rule ran", md);
    char err[256] = "";
    CHECK(uam_rules_arm(rs, err, sizeof(err)) == UAM_OK && m.bus.n_watches == 1, "%s: arm: %s", md, err);
    CHECK(uam_rules_arm(rs, err, sizeof(err)) == UAM_OK && m.bus.n_watches == 1, "%s: arm again", md);
    uam_machine_fini(&m);
    uam_rules_free(rs);

    /* Two processors run the same code: cpu=1 takes only the second one's store. */
    build(&m, jit, 1);
    rs = rules_new(&m);
    add(rs, "pc=0x14,set [[sp]]=[sp+4]");
    r = add(rs, "write=p:0x8004,len=4,cpu=1,set t6=7");
    const int all = add(rs, "write=p:0x8004,len=4,set s2=s2+2");
    run(&m, 20000, "access rule, two processors", jit);
    CHECK(reg(&m, 0, "t6") == 0 && reg(&m, 1, "t6") == 7 && info(rs, r).arrivals == 1,
          "%s: cpu=1 access rule: t6 %llu and %llu", md, (unsigned long long)reg(&m, 0, "t6"),
          (unsigned long long)reg(&m, 1, "t6"));
    CHECK(reg(&m, 0, "s2") == 1 && reg(&m, 1, "s2") == 1 && info(rs, all).fired == 2,
          "%s: an access rule for both processors fired %llu times", md,
          (unsigned long long)info(rs, all).fired);
    uam_machine_fini(&m);
    uam_rules_free(rs);
}

/* ---- delay -------------------------------------------------------------------------- */

static void test_delay(bool jit)
{
    const char *md = mode_name(jit);
    static uint64_t done_at[2];
    uam_machine m;

    /* The answer comes 100 ticks after the wait began (at instruction 5). */
    build(&m, jit, 0);
    uam_rules *rs = rules_new(&m);
    int r = add(rs, "pc=0x14,count=1,delay=100,set [[sp]]=[sp+4]");
    run(&m, 100, "before the delay is over", jit);
    uam_rule_info ri = info(rs, r);
    CHECK(reg(&m, 0, "s2") == 0 && word(&m, WORD_PA, 4) == 0 && ri.waiting && ri.fired == 1
          && ri.delay == 100 && ri.arrivals > 10, "%s: early: fired=%llu waiting=%d", md,
          (unsigned long long)ri.fired, ri.waiting);
    while (reg(&m, 0, "s2") == 0 && m.cpu.icount < 200)
        uam_exec_run(&m.exec, 1);
    done_at[jit] = m.cpu.icount;
    CHECK(m.cpu.icount > 105 && m.cpu.icount <= 112 && word(&m, WORD_PA, 4) == 0x55,
          "%s: the wait ended at instruction %llu", md, (unsigned long long)m.cpu.icount);
    CHECK(!jit || done_at[0] == done_at[1], "the delay ends at instruction %llu on the "
          "interpreter and %llu on the JIT", (unsigned long long)done_at[0],
          (unsigned long long)done_at[1]);
    CHECK(!info(rs, r).waiting, "%s: still waiting", md);
    uam_machine_fini(&m);
    uam_rules_free(rs);

    /* The value is the one of when the rule fired; it does not fire again before its
     * stores have happened. */
    build(&m, jit, 0);
    rs = rules_new(&m);
    add(rs, "pc=0x14,set [[sp]]=[sp+4]");
    r = add(rs, "pc=0x28,count=1,delay=50,set t6=a0+100,set [p:0x20000]:4=a0+7");
    const int rep = add(rs, "pc=0x28,delay=40,set s2=s2+1");
    run(&m, 1000, "delayed values", jit);
    CHECK(reg(&m, 0, "t6") == 100 && word(&m, 0x20000, 4) == 7, "%s: delayed values: t6=%llu "
          "word=%llu", md, (unsigned long long)reg(&m, 0, "t6"),
          (unsigned long long)word(&m, 0x20000, 4));
    ri = info(rs, rep);
    CHECK(ri.fired >= 15 && ri.fired <= 30 && reg(&m, 0, "s2") == 1 + ri.fired - ri.waiting,
          "%s: a rule with a delay fired %llu times in 1000 instructions, s2=%llu", md,
          (unsigned long long)ri.fired, (unsigned long long)reg(&m, 0, "s2"));
    uam_machine_fini(&m);
    uam_rules_free(rs);

    /* With an access rule: the request's value, a while after the request. */
    build(&m, jit, 0);
    rs = rules_new(&m);
    r = add(rs, "write=p:0x8004,len=4,delay=60,set [[sp]]=value");
    run(&m, 40, "access rule with a delay, early", jit);
    CHECK(reg(&m, 0, "s2") == 0 && info(rs, r).waiting, "%s: access delay: early", md);
    run(&m, 60, "access rule with a delay, late", jit);
    CHECK(reg(&m, 0, "s2") == 1 && word(&m, WORD_PA, 4) == 0x55, "%s: access delay: late", md);
    uam_machine_fini(&m);
    uam_rules_free(rs);

    /* A snapshot keeps the stores that are on their way. */
    build(&m, jit, 0);
    rs = rules_new(&m);
    r = add(rs, "pc=0x14,delay=100,set [[sp]]=[sp+4]");
    run(&m, 50, "to the snapshot", jit);
    uam_snapshot *s = NULL;
    CHECK(uam_snapshot_take(&m, &s) == UAM_OK && s, "%s: take with a store on its way", md);
    run(&m, 200, "past the delay", jit);
    CHECK(reg(&m, 0, "s2") == 1 && !info(rs, r).waiting, "%s: the delay did not end", md);
    CHECK(uam_snapshot_restore(&m, s) == UAM_OK, "%s: restore", md);
    CHECK(reg(&m, 0, "s2") == 0 && word(&m, WORD_PA, 4) == 0 && info(rs, r).waiting,
          "%s: the store on its way did not come back", md);
    run(&m, 30, "after the restore, early", jit);
    CHECK(reg(&m, 0, "s2") == 0, "%s: the restored store came early", md);
    run(&m, 60, "after the restore, late", jit);
    CHECK(reg(&m, 0, "s2") == 1 && word(&m, WORD_PA, 4) == 0x55, "%s: the restored store did not "
          "happen", md);
    uam_machine_fini(&m);
    uam_rules_free(rs);

    build(&m, jit, 0);
    rs = rules_new(&m);
    r = add(rs, "pc=0x14,delay=100,set [[sp]]=[sp+4]");
    CHECK(uam_snapshot_restore(&m, s) == UAM_OK && info(rs, r).waiting, "%s: restore elsewhere", md);
    run(&m, 90, "elsewhere", jit);
    CHECK(reg(&m, 0, "s2") == 1, "%s: the store did not happen elsewhere", md);
    uam_machine_fini(&m);
    uam_rules_free(rs);

    /* (Refused: an error is logged.) */
    build(&m, jit, 0);
    rs = rules_new(&m);
    add(rs, "pc=0x14,set [[sp]]=[sp+4]");
    CHECK(uam_snapshot_restore(&m, s) != UAM_OK, "%s: restored without the rule with a delay", md);
    uam_machine_fini(&m);
    uam_rules_free(rs);
    uam_snapshot_free(s);
}

/* ---- dump --------------------------------------------------------------------------- */

static void test_dump(bool jit)
{
    const char *md = mode_name(jit);
    uam_machine m;
    char text[2048];
    build(&m, jit, 0);
    uam_rules *rs = rules_new(&m);
    FILE *out = tmpfile();
    uam_rules_set_log(rs, out);
    add(rs, "pc=0x14,set [[sp]]=[sp+4]");
    const int d = add(rs, "pc=0x24,name=d,dump sp 4+4,dump p:[sp]+0x10-2 2*10,dump p:0x40000000 4,"
                          "dump p:0xffffe 4,dump sp 0x10001");
    add(rs, "write=p:0x8004,len=4,name=w,dump addr-4 8");
    run(&m, 1000, "dump", jit);
    slurp(out, text, sizeof(text));
    CHECK(!strcmp(text,
        "rule w: cpu 0 pc 0x10 write 4 at 0x8004 value 0x55 dump 0x8000+0x8 icount=5\n"
        "  0000000000008000  00 00 01 00 55 00 00 00                          |....U...|\n"
        "rule d: cpu 0 pc 0x24 dump 0x8000+0x8 icount=9\n"
        "  0000000000008000  00 00 01 00 55 00 00 00                          |....U...|\n"
        "rule d: cpu 0 pc 0x24 dump p:0x1000e+0x14 icount=9\n"
        "  000000000001000e  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  |................|\n"
        "  000000000001001e  00 00 00 00                                      |....|\n"
        "rule d: cpu 0 pc 0x24 dump p:0x40000000+0x4 icount=9\n"
        "  (physical 0x40000000 cannot be read)\n"
        "rule d: cpu 0 pc 0x24 dump p:0xffffe+0x4 icount=9\n"
        "  00000000000ffffe  00 00                                            |..|\n"
        "  (physical 0x100000 cannot be read)\n"), "%s: dumps:\n%s", md, text);
    CHECK(info(rs, d).failed == 3, "%s: %llu dumps failed, not 3", md,
          (unsigned long long)info(rs, d).failed);
    uam_machine_fini(&m);
    uam_rules_free(rs);
}

/* ---- byte order and virtual addresses (MIPS32 big-endian) ------------------------ */

static void test_mips_be(bool jit)
{
    const char *md = mode_name(jit);
    uam_machine m;
    CHECK(uam_machine_init(&m, &uam_isa_mips32be) == UAM_OK, "machine init");
    m.board_name = "rules-test-be";
    CHECK(uam_bus_map_ram(&m.bus, "ram", 0, 1u << 20) == UAM_OK, "map ram");
    const uint32_t p[] = { 0x3C098001u /* lui r9, 0x8001 */, 0xAD290020u /* sw r9, 0x20(r9) */,
                           0x08000002u /* j 0x80000008 */, 0 /* nop */ };
    put_words(&m, 0, p, UAM_ARRAY_LEN(p), true);
    uam_machine_reset(&m, 0x80000000u);
    m.cpu.isa->sysreg_write(&m.cpu, 12 * 8 + 0, 0);   /* Status.ERL off: kuseg goes through the TLB */
    if (jit)
        CHECK(uam_exec_enable_jit(&m.exec), "jit available");
    uam_rules *rs = rules_new(&m);
    FILE *out = tmpfile();
    uam_rules_set_log(rs, out);
    /* access rules on virtual ranges: kseg0, and kseg1's alias of the same bytes */
    add(rs, "write=0x80010020,len=4,name=k0,log value [addr] [p:0x10020]:2");
    add(rs, "access=0xa0010022,name=k1,log");
    /* kseg0, then kseg1 and physical addresses of the same RAM */
    const int r = add(rs, "pc=0x80000000,count=1,set [0x80010000]=0x11223344,"
                          "set [0xa0010004]:2=[0x80010000]:2,set [p:0x10006]:1=[p:0x10003]:1,"
                          "set [p:0x10008]:8=[0x80010000]<<32|0xaabbccdd,set r8=[0x80010002]:2,"
                          "set [0x00010010]:4=1");
    run(&m, 1000, "mips", jit);
    static const uint8_t want[16] = { 0x11, 0x22, 0x33, 0x44, 0x11, 0x22, 0x44, 0x00,
                                      0x11, 0x22, 0x33, 0x44, 0xAA, 0xBB, 0xCC, 0xDD };
    for (unsigned i = 0; i < sizeof(want); i++)
        CHECK(word(&m, 0x10000 + i, 1) == want[i], "%s: byte %u is 0x%llx", md, i,
              (unsigned long long)word(&m, 0x10000 + i, 1));
    CHECK(reg(&m, 0, "r8") == 0x3344, "%s: t0 is 0x%llx", md, (unsigned long long)reg(&m, 0, "r8"));
    /* kuseg has no TLB entry: that store fails, the others stand */
    CHECK(info(rs, r).failed == 1 && word(&m, 0x10010, 4) == 0, "%s: an unmapped virtual store: "
          "failed=%llu", md, (unsigned long long)info(rs, r).failed);
    char text[512];
    slurp(out, text, sizeof(text));
    CHECK(!strcmp(text, "rule k0: cpu 0 pc 0x80000004 write 4 at 0x80010020 value 0x80010000 "
                        "value=0x80010000 [addr]=0x80010000 [p:0x10020]:2=0x8001 icount=2\n"
                        "rule k1: cpu 0 pc 0x80000004 write 4 at 0xa0010020 value 0x80010000 "
                        "icount=2\n"), "%s: access rules on virtual ranges:\n%s", md, text);
    char err[256] = "";
    CHECK(uam_rules_add(rs, "write=0x00010020,len=4,log", err, sizeof(err)) < 0
          && strstr(err, "does not map"), "%s: an access rule on an unmapped range: %s", md, err);
    uam_machine_fini(&m);
    uam_rules_free(rs);
}

/* ---- malformed rules, rule files -------------------------------------------------- */

static void test_parse(void)
{
    uam_machine m;
    build(&m, false, 0);
    uam_rules *rs = rules_new(&m);
    static const struct { const char *text, *why; } bad[] = {
        { "", "starts with pc=" },
        { "stop", "starts with pc=" },
        { "pc=0x28", "does nothing" },
        { "pc=0x28,if a0==1", "does nothing" },
        { "pc=,stop", "value is expected" },
        { "pc=a0,stop", "constant" },
        { "pc=[0x10],stop", "constant" },
        { "pc=nosuch,stop", "neither a register" },
        { "pc=0x28,,stop", "empty item" },
        { "pc=0x28,stop,", "empty item" },
        { "pc=0x28,halt", "unknown item" },
        { "pc=0x28,stop now", "unknown item" },
        { "pc=0x28,cpu=1,stop", "0..0" },
        { "pc=0x28,cpu=x,stop", "cpu=" },
        { "pc=0x28,count=0,stop", "at least 1" },
        { "pc=0x28,count=3x,stop", "at least 1" },
        { "pc=0x28,name=,stop", "name=NAME" },
        { "pc=0x28,if,stop", "unknown item" },
        { "pc=0x28,if a0==,stop", "value is expected" },
        { "pc=0x28,if (a0,stop", "expected ')'" },
        { "pc=0x28,if a0 a1,stop", "unexpected" },
        { "pc=0x28,if 0x12q,stop", "bad number" },
        { "pc=0x28,if [a0,stop", "expected ']'" },
        { "pc=0x28,if [a0]:3==1,stop", "1, 2, 4 or 8" },
        { "pc=0x28,if [a0]:16==1,stop", "1, 2, 4 or 8" },
        { "pc=0x28,set a0", "DST=EXPR" },
        { "pc=0x28,set a0==1", "DST=EXPR" },
        { "pc=0x28,set 5=1", "before '='" },
        { "pc=0x28,set (a0)=1", "before '='" },
        { "pc=0x28,set a0+1=1", "DST=EXPR" },
        { "pc=0x28,set [a0]+1=1", "DST=EXPR" },
        { "pc=0x28,set wait=1", "symbol" },
        { "pc=0x28,set a0=", "value is expected" },
        { "pc=0x28,set a0=1 2", "unexpected" },
        { "pc=0x28,log a0 nosuch", "neither a register" },
        { "pc=0x28,if value==1,stop", "neither a register" },
        { "pc=0x28,len=4,stop", "goes with" },
        { "pc=0x28,delay=10,stop", "has none" },
        { "pc=0x28,delay=10,log", "has none" },
        { "pc=0x28,delay=10,set a0=1,stop", "cannot stop" },
        { "pc=0x28,delay=x,set a0=1", "delay=N" },
        { "pc=0x28,dump a0", "dump wants" },
        { "pc=0x28,dump a0 4 4", "dump wants" },
        { "pc=0x28,dump a0 nosuch", "neither a register" },
        { "write=a0,stop", "constant" },
        { "write=p:,stop", "value is expected" },
        { "write=p:0x8000,len=0,stop", "len=N" },
        { "write=p:0x8000,len=a0,stop", "constant" },
        { "write=p:0x8000,len=0x10000001,stop", "len=N" },
        { "write=p:0xffffffffffffffff,len=4,stop", "address space" },
        { "writes=p:0x8000,stop", "starts with pc=" },
        { "pc=0x28,if ((((((((((((((((((((((((((((((((((1)))))))))))))))))))))))))))))))))),stop",
          "too deep" },
        { "pc=0x28,if 1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1,stop",
          "too long" },
    };
    uam_rules_set_symbols(rs, symbols, NULL);
    for (size_t i = 0; i < UAM_ARRAY_LEN(bad); i++) {
        char err[256] = "";
        CHECK(uam_rules_add(rs, bad[i].text, err, sizeof(err)) < 0, "'%s' accepted", bad[i].text);
        CHECK(strstr(err, bad[i].why), "'%s': the reason is '%s', not about '%s'", bad[i].text,
              err, bad[i].why);
    }
    CHECK(uam_rules_n(rs) == 0, "a refused rule was kept");
    uam_rule_info ri;
    CHECK(!uam_rules_get(rs, 0, &ri), "a rule in an empty set");

    /* A rule file: comments, blank lines, and the line of the first bad rule. */
    char path[] = "/tmp/uam-test-rules-XXXXXX";
    const int fd = mkstemp(path);
    FILE *f = fd >= 0 ? fdopen(fd, "w") : NULL;
    CHECK(f != NULL, "temporary rule file");
    if (f) {
        fputs("# stand in for the coprocessor\n\n  pc=wait,set [[sp]]=[sp+4]\n"
              "pc=0x28,count=2,name=twice,set t6=t6+1\r\n", f);
        fclose(f);
        char err[512] = "";
        CHECK(uam_rules_add_file(rs, path, err, sizeof(err)) == UAM_OK, "rule file refused: %s", err);
        CHECK(uam_rules_n(rs) == 2 && !strcmp(info(rs, 1).name, "twice"), "rules from the file");
        run(&m, 2000, "rule file", false);
        CHECK(reg(&m, 0, "s2") == 1 && reg(&m, 0, "t6") == 2, "the file's rules did not run");

        f = fopen(path, "w");
        fputs("pc=0x24,name=fine,log\n# next\npc=0x28,frob\npc=0x24,name=late,log\n", f);
        fclose(f);
        CHECK(uam_rules_add_file(rs, path, err, sizeof(err)) != UAM_OK && strstr(err, ":3: ")
              && strstr(err, "unknown item"), "a bad line: %s", err);
        CHECK(uam_rules_n(rs) == 3, "rules after a bad line were added");
        remove(path);
        CHECK(uam_rules_add_file(rs, path, err, sizeof(err)) != UAM_OK && strstr(err, "cannot read"),
              "a missing file: %s", err);
    }

    /* Two sets on one machine need two names when both count. */
    uam_rules *other = uam_rules_create(&m, "test");
    char err[256] = "";
    CHECK(other && uam_rules_add(other, "pc=0x28,count=1,stop", err, sizeof(err)) < 0
          && strstr(err, "state"), "a second counting set of the same name: %s", err);
    uam_rules_free(other);
    uam_machine_fini(&m);
    uam_rules_free(rs);
}

int main(void)
{
    for (int jit = 0; jit < 2; jit++) {
        test_stand_in(jit);
        test_cond_count(jit);
        test_log(jit);
        test_failures(jit);
        test_snapshot(jit);
        test_smp(jit);
        test_access(jit);
        test_delay(jit);
        test_dump(jit);
        test_mips_be(jit);
    }
    test_parse();
    printf("test_rules: %d checks, %d failed\n", n_checks, n_failed);
    return n_failed ? 1 : 0;
}
