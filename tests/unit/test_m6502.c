/*
 * test_m6502.c - The 6502 module's parts the self-test firmware cannot reach on
 * 6502-ref, or reaches only in part: binary ADC and SBC over every input and decimal
 * ADC and SBC over every BCD input (interpreted and JIT), the interrupt poll's latency
 * after CLI, SEI, PLP and RTI, the NMI edge, the wired-OR IRQ lines, the set-overflow
 * input, WAI, STP and JAM, reset through the vector, BRK on NMOS and CMOS, the NMOS
 * read-modify-write double store, the JMP (abs) page wrap, the state translated code
 * depends on (D), and the CPU models (uam_m6502_configure).
 *
 * Each test runs on a bare machine: RAM everywhere but a probe device at 0xD000, the
 * vectors pointing at 0x1000 (reset), 0x2000 (NMI) and 0x3000 (IRQ/BRK).
 */
#include <stdio.h>
#include <string.h>

#include "uam/bus.h"
#include "uam/device.h"
#include "uam/isa/m6502.h"
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

/* The module's register file and system registers (isa/m6502/m6502.h). */
enum { R_A, R_X, R_Y, R_S, R_C, R_Z, R_V, R_N, R_PX };
enum { SYS_P, SYS_P_NOW };

#define PROBE 0xD000u
enum { NMI_H = 0x2000, IRQ_H = 0x3000 };

/* The probe: records the writes it gets (offset 0 reads back the last one). */
typedef struct probe {
    uam_device dev;
    uint8_t    last;
    unsigned   n_writes;
    uint8_t    writes[8];
} probe;

static uam_status probe_read(uam_device *d, uint64_t off, unsigned size, uint64_t *v)
{
    (void)off; (void)size;
    *v = ((probe *)d)->last;
    return UAM_OK;
}

static uam_status probe_write(uam_device *d, uint64_t off, unsigned size, uint64_t v)
{
    (void)off; (void)size;
    probe *p = (probe *)d;
    if (p->n_writes < 8)
        p->writes[p->n_writes] = (uint8_t)v;
    p->n_writes++;
    p->last = (uint8_t)v;
    return UAM_OK;
}

static void probe_destroy(uam_device *d) { (void)d; }

static const uam_device_ops probe_ops = {
    .type = "probe", .read = probe_read, .write = probe_write, .destroy = probe_destroy,
    .flags = UAM_DEVICE_STATELESS,
};

static probe g_probe;

static void wr(uam_machine *m, uint32_t a, const uint8_t *b, size_t n)
{
    CHECK(uam_bus_load_bytes(&m->bus, a, b, n) == UAM_OK, "load at 0x%x", (unsigned)a);
}
static void wr8(uam_machine *m, uint32_t a, uint8_t v) { uam_bus_load_bytes(&m->bus, a, &v, 1); }
static uint8_t rd8(uam_machine *m, uint32_t a)
{
    uint64_t v = 0;
    CHECK(uam_bus_read(&m->bus, a, 1, UAM_ACC_LE, &v) == UAM_OK, "read 0x%x", (unsigned)a);
    return (uint8_t)v;
}

static uint8_t *reg(uam_machine *m) { return m->cpu.regfile; }
static void line(uam_machine *m, unsigned n, int level) { uam_cpu_irq_sink(&m->cpu, n, level); }
static bool take(uam_machine *m) { return m->cpu.isa->irq_take(&m->cpu); }
static bool wake(uam_machine *m) { return m->cpu.isa->irq_wakeup(&m->cpu); }
static void step(uam_machine *m, uint64_t n) { uam_exec_run(&m->exec, n); }
static void sysw(uam_machine *m, uint32_t id, uint8_t v)
{
    CHECK(m->cpu.isa->sysreg_write(&m->cpu, id, v) == UAM_OK, "sysreg %u", (unsigned)id);
}
static uint8_t p_of(uam_machine *m)
{
    uint64_t v = 0;
    CHECK(m->cpu.isa->sysreg_read(&m->cpu, SYS_P, &v) == UAM_OK, "read P");
    return (uint8_t)v;
}

/* A machine of model cfg, reset: the NMI and IRQ handlers count in Y and return. */
static void machine(uam_machine *m, const uam_m6502_config *cfg, bool jit)
{
    CHECK(uam_machine_init(m, &uam_isa_m6502) == UAM_OK, "machine init");
    CHECK(uam_m6502_configure(&m->cpu, cfg) == UAM_OK, "%s: configure", cfg->name);
    CHECK(uam_bus_map_ram(&m->bus, "lo", 0, PROBE) == UAM_OK, "map RAM");
    CHECK(uam_bus_map_ram(&m->bus, "hi", PROBE + 0x100, 0x10000 - PROBE - 0x100) == UAM_OK, "map RAM");
    memset(&g_probe, 0, sizeof(g_probe));
    uam_device_init(&g_probe.dev, &probe_ops, "probe");
    CHECK(uam_bus_map_mmio(&m->bus, "probe", PROBE, 0x100, &g_probe.dev) == UAM_OK, "map the probe");
    const uint8_t vec[] = { NMI_H & 0xFF, NMI_H >> 8, 0x00, 0x10, IRQ_H & 0xFF, IRQ_H >> 8 };
    wr(m, 0xFFFA, vec, sizeof(vec));
    const uint8_t h[] = { 0xC8, 0x40 };   /* INY; RTI */
    wr(m, NMI_H, h, sizeof(h));
    wr(m, IRQ_H, h, sizeof(h));
    if (jit)
        CHECK(uam_exec_enable_jit(&m->exec), "JIT");
    uam_machine_reset(m, 0);
}

static void done(uam_machine *m) { uam_machine_fini(m); }

/* ---- reset ------------------------------------------------------------------------ */

static void test_reset(void)
{
    uam_machine m;
    machine(&m, &uam_m6502_nmos, false);
    EQ64(m.cpu.pc, 0x1000, "reset: the pc from the reset vector");
    EQ64(reg(&m)[R_S], 0xFD, "reset: S");
    EQ64(p_of(&m), 0x34, "reset: I set (bit 5 and B as read)");
    done(&m);

    /* nothing at the vector: the entry given */
    CHECK(uam_machine_init(&m, &uam_isa_m6502) == UAM_OK, "init");
    CHECK(uam_bus_map_ram(&m.bus, "ram", 0, 0x8000) == UAM_OK, "map");
    uam_machine_reset(&m, 0x1234);
    EQ64(m.cpu.pc, 0x1234, "reset with no vector: the entry");
    done(&m);
}

/* ---- arithmetic -------------------------------------------------------------------- */

static unsigned bcd(unsigned v) { return (v / 10) << 4 | v % 10; }

/* ADC $40 / SBC $40 over every A, operand and carry: binary (D clear) against plain
 * arithmetic, and decimal over every pair of BCD bytes against decimal arithmetic. */
static void test_arith(const uam_m6502_config *cfg, bool jit)
{
    uam_machine m;
    machine(&m, cfg, jit);
    const uint8_t code[] = { 0x65, 0x40, 0xE5, 0x40 };   /* ADC $40 at 0x1000, SBC $40 at 0x1002 */
    wr(&m, 0x1000, code, sizeof(code));
    unsigned bad = 0;
    for (unsigned sub = 0; sub < 2; sub++)
        for (unsigned a = 0; a < 256; a++)
            for (unsigned b = 0; b < 256; b++)
                for (unsigned c = 0; c < 2; c++) {
                    reg(&m)[R_A] = (uint8_t)a;
                    reg(&m)[R_C] = (uint8_t)c;
                    wr8(&m, 0x40, (uint8_t)b);
                    m.cpu.pc = 0x1000 + 2 * sub;
                    step(&m, 1);
                    const unsigned bb = sub ? b ^ 0xFF : b, s = a + bb + c, r = s & 0xFF;
                    const bool v = (a ^ r) & (bb ^ r) & 0x80;
                    if (reg(&m)[R_A] != r || reg(&m)[R_C] != (s >> 8) || reg(&m)[R_Z] != (r == 0)
                        || reg(&m)[R_N] != (r >> 7) || reg(&m)[R_V] != v)
                        bad++;
                }
    CHECK(bad == 0, "%s%s: binary ADC/SBC: %u wrong", cfg->name, jit ? " jit" : "", bad);

    if (cfg->decimal) {
        sysw(&m, SYS_P_NOW, UAM_M6502_P_D | UAM_M6502_P_I);
        bad = 0;
        for (unsigned sub = 0; sub < 2; sub++)
            for (unsigned x = 0; x < 100; x++)
                for (unsigned y = 0; y < 100; y++)
                    for (unsigned c = 0; c < 2; c++) {
                        reg(&m)[R_A] = (uint8_t)bcd(x);
                        reg(&m)[R_C] = (uint8_t)c;
                        wr8(&m, 0x40, (uint8_t)bcd(y));
                        m.cpu.pc = 0x1000 + 2 * sub;
                        step(&m, 1);
                        const int d = sub ? (int)x - (int)y - (int)(1 - c) : (int)(x + y + c);
                        const unsigned r = bcd((unsigned)((d + 100) % 100));
                        const unsigned cy = sub ? d >= 0 : d >= 100;
                        bool ok = reg(&m)[R_A] == r && reg(&m)[R_C] == cy;
                        if (cfg->cmos)   /* the 65C02's N and Z are the result's */
                            ok = ok && reg(&m)[R_Z] == (r == 0) && reg(&m)[R_N] == (r >> 7);
                        if (!ok)
                            bad++;
                    }
        CHECK(bad == 0, "%s%s: decimal ADC/SBC: %u wrong", cfg->name, jit ? " jit" : "", bad);
        EQ64(m.cpu.priv, 1, "D set: blocks are translated for decimal arithmetic");
    }
    done(&m);
}

/* ---- interrupts ------------------------------------------------------------------- */

/* After CLI one more instruction runs before a pending IRQ; then the handler runs. */
static void test_cli_latency(bool jit)
{
    uam_machine m;
    machine(&m, &uam_m6502_nmos, jit);
    const uint8_t code[] = { 0x58, 0xE8, 0xE8, 0xE8, 0x4C, 0x04, 0x10 };   /* CLI INX INX INX; JMP * */
    wr(&m, 0x1000, code, sizeof(code));
    const uint8_t h[] = { 0x86, 0x50, 0x40 };   /* STX $50; RTI */
    wr(&m, IRQ_H, h, sizeof(h));
    line(&m, 0, 1);
    step(&m, 1);
    EQ64(m.cpu.pc, 0x1001, "CLI ran; the request waits");
    step(&m, 3);
    EQ64(rd8(&m, 0x50), 1, "one instruction after CLI ran before the handler");
    line(&m, 0, 0);
    done(&m);

    /* PLP clearing I: the same */
    machine(&m, &uam_m6502_nmos, jit);
    const uint8_t plp[] = { 0x28, 0xE8, 0xE8, 0xE8, 0x4C, 0x04, 0x10 };   /* PLP INX ... */
    wr(&m, 0x1000, plp, sizeof(plp));
    wr(&m, IRQ_H, h, sizeof(h));
    wr8(&m, 0x01FE, 0x00);      /* the P PLP pulls: I clear */
    line(&m, 0, 1);
    step(&m, 4);
    EQ64(rd8(&m, 0x50), 1, "one instruction after PLP cleared I");
    done(&m);
}

static void test_irq_poll(void)
{
    uam_machine m;
    machine(&m, &uam_m6502_nmos, false);
    /* SEI with a request pending: the next poll still sees I clear, and the handler is
     * entered with I set in the pushed P */
    sysw(&m, SYS_P_NOW, 0x00);
    line(&m, 0, 1);
    sysw(&m, SYS_P, UAM_M6502_P_I);
    m.cpu.pc = 0x1234;
    CHECK(take(&m), "the IRQ is still taken right after SEI");
    EQ64(m.cpu.pc, IRQ_H, "the IRQ vector");
    EQ64(rd8(&m, 0x01FD), 0x12, "pc high byte pushed first");
    EQ64(rd8(&m, 0x01FC), 0x34, "then its low byte");
    EQ64(rd8(&m, 0x01FB), 0x24, "then P: I set, bit 5 set, B clear");
    EQ64(reg(&m)[R_S], 0xFA, "three bytes pushed");
    CHECK(!take(&m), "masked once in the handler");
    line(&m, 0, 0);
    done(&m);

    /* RTI's I takes effect at once: with the line held the handler is re-entered
     * before the interrupted code runs an instruction */
    machine(&m, &uam_m6502_nmos, false);
    const uint8_t code[] = { 0xE8, 0xE8, 0xE8, 0x4C, 0x00, 0x10 };   /* INX INX INX; JMP 0x1000 */
    wr(&m, 0x1000, code, sizeof(code));
    sysw(&m, SYS_P_NOW, 0x00);
    line(&m, 0, 1);
    step(&m, 20);
    EQ64(reg(&m)[R_X], 0, "the interrupted code made no progress");
    EQ64(reg(&m)[R_Y], 10, "the handler ran ten times");
    /* wired-OR: the request stays while any line is high */
    line(&m, 3, 1);
    line(&m, 0, 0);
    m.cpu.pc = 0x1000;
    CHECK(take(&m), "line 3 still requests");
    line(&m, 3, 0);
    sysw(&m, SYS_P_NOW, 0x00);
    CHECK(!take(&m), "no line, no request");
    done(&m);
}

static void test_nmi_so(void)
{
    uam_machine m;
    machine(&m, &uam_m6502_nmos, false);
    m.cpu.pc = 0x1000;
    CHECK(!take(&m), "nothing pending");
    line(&m, UAM_M6502_LINE_NMI, 1);
    CHECK(take(&m), "NMI taken with I set");
    EQ64(m.cpu.pc, NMI_H, "the NMI vector");
    CHECK(!take(&m), "one NMI per edge");
    line(&m, UAM_M6502_LINE_NMI, 1);
    CHECK(!take(&m), "a level held is no new edge");
    line(&m, UAM_M6502_LINE_NMI, 0);
    line(&m, UAM_M6502_LINE_NMI, 1);
    CHECK(take(&m), "the next rising edge");
    EQ64(m.cpu.isa->irq_lines(&m.cpu), UINT64_C(1) << UAM_M6502_LINE_NMI, "irq_lines names the NMI");
    /* set overflow */
    reg(&m)[R_V] = 0;
    line(&m, UAM_M6502_LINE_SO, 1);
    EQ64(reg(&m)[R_V], 1, "SO sets V");
    reg(&m)[R_V] = 0;
    line(&m, UAM_M6502_LINE_SO, 0);
    EQ64(reg(&m)[R_V], 0, "the falling edge does nothing");
    done(&m);
}

/* WAI wakes on a request I masks and runs on; STP and JAM stop until reset. */
static void test_wait_stop(void)
{
    uam_machine m;
    machine(&m, &uam_m6502_w65c02, false);
    const uint8_t code[] = { 0xCB, 0xE8, 0xDB, 0xE8 };   /* WAI INX STP INX */
    wr(&m, 0x1000, code, sizeof(code));
    step(&m, 1);
    CHECK(m.cpu.waiting, "WAI waits");
    CHECK(!wake(&m), "no request: no wake");
    line(&m, 1, 1);
    CHECK(wake(&m), "a masked request wakes WAI");
    line(&m, 1, 0);
    m.cpu.waiting = false;
    step(&m, 2);
    EQ64(reg(&m)[R_X], 1, "the instruction after WAI ran");
    EQ64(reg(&m)[R_Y], 0, "and the handler did not");
    CHECK(m.cpu.waiting, "STP stops");
    line(&m, UAM_M6502_LINE_NMI, 1);
    CHECK(!wake(&m), "an NMI does not restart STP");
    CHECK(!take(&m), "nor is it taken");
    uam_machine_reset(&m, 0);
    EQ64(m.cpu.pc, 0x1000, "reset restarts it");
    CHECK(!m.cpu.waiting, "running again");
    done(&m);

    /* Rockwell: no WAI or STP, 1-byte NOPs instead */
    machine(&m, &uam_m6502_r65c02, false);
    wr(&m, 0x1000, code, sizeof(code));
    step(&m, 4);
    EQ64(reg(&m)[R_X], 2, "R65C02: 0xCB and 0xDB are NOPs");
    done(&m);

    /* an NMOS JAM */
    machine(&m, &uam_m6502_nmos, false);
    const uint8_t jam[] = { 0xE8, 0x02, 0xE8 };
    wr(&m, 0x1000, jam, sizeof(jam));
    step(&m, 3);
    EQ64(reg(&m)[R_X], 1, "JAM stops the core");
    EQ64(m.cpu.pc, 0x1001, "at the JAM");
    line(&m, UAM_M6502_LINE_NMI, 1);
    CHECK(!wake(&m), "for good");
    done(&m);
}

static void test_brk(const uam_m6502_config *cfg)
{
    uam_machine m;
    machine(&m, cfg, false);
    const uint8_t code[] = { 0xF8, 0x00, 0xEE, 0xEA };   /* SED; BRK #0xEE; NOP */
    wr(&m, 0x1000, code, sizeof(code));
    const uint8_t h[] = { 0x08, 0x40 };   /* PHP; RTI won't run */
    wr(&m, IRQ_H, h, sizeof(h));
    step(&m, 3);
    EQ64(m.cpu.pc, IRQ_H + 1, "BRK enters through the IRQ vector");
    EQ64(rd8(&m, 0x01FD), 0x10, "return address high byte");
    EQ64(rd8(&m, 0x01FC), 0x03, "BRK + 2");
    EQ64(rd8(&m, 0x01FB) & 0x3C, 0x3C, "pushed P: B, bit 5, D and I (from reset) set");
    const uint8_t in_handler = rd8(&m, 0x01FA);
    EQ64(in_handler & UAM_M6502_P_I, UAM_M6502_P_I, "I set in the handler");
    EQ64(in_handler & UAM_M6502_P_D, cfg->cmos ? 0 : UAM_M6502_P_D, "D cleared on CMOS only");
    EQ64(m.cpu.priv, cfg->cmos ? 0 : 1, "and the blocks follow it");
    done(&m);
}

/* ---- memory ------------------------------------------------------------------------ */

/* An NMOS read-modify-write stores the byte it read, then the result; a 65C02 stores once. */
static void test_rmw(void)
{
    static const struct { uint8_t op; bool nmos_only; uint8_t result; } ops[] = {
        { 0xEE, false, 0x81 },   /* INC abs */
        { 0x0E, false, 0x00 },   /* ASL abs */
        { 0x6E, false, 0x40 },   /* ROR abs (C clear) */
        { 0x0F, true,  0x00 },   /* SLO abs */
        { 0xCF, true,  0x7F },   /* DCP abs */
    };
    for (unsigned cmos = 0; cmos < 2; cmos++)
        for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
            if (cmos && ops[i].nmos_only)
                continue;
            uam_machine m;
            machine(&m, cmos ? &uam_m6502_w65c02 : &uam_m6502_nmos, false);
            const uint8_t code[] = { ops[i].op, PROBE & 0xFF, PROBE >> 8 };
            wr(&m, 0x1000, code, sizeof(code));
            g_probe.last = 0x80;
            step(&m, 1);
            if (cmos) {
                EQ64(g_probe.n_writes, 1, "65C02: one store");
            } else {
                EQ64(g_probe.n_writes, 2, "NMOS: two stores");
                EQ64(g_probe.writes[0], 0x80, "the unmodified byte first");
            }
            EQ64(g_probe.last, ops[i].result, "then the result");
            done(&m);
        }
}

static void test_jmp_ind(void)
{
    for (unsigned cmos = 0; cmos < 2; cmos++) {
        uam_machine m;
        machine(&m, cmos ? &uam_m6502_w65c02 : &uam_m6502_nmos, false);
        const uint8_t code[] = { 0x6C, 0xFF, 0x10 };   /* JMP ($10FF) */
        wr(&m, 0x0F00, code, sizeof(code));
        wr8(&m, 0x10FF, 0x34);
        wr8(&m, 0x1000, 0x12);
        wr8(&m, 0x1100, 0x56);
        m.cpu.pc = 0x0F00;
        step(&m, 1);
        EQ64(m.cpu.pc, cmos ? 0x5634 : 0x1234, cmos ? "65C02: the pointer crosses the page"
                                                    : "NMOS: the pointer wraps within its page");
        done(&m);
    }
}

/* ---- models ---------------------------------------------------------------------------- */

static void test_models(void)
{
    uam_machine m;
    machine(&m, &uam_m6502_nmos, false);
    uam_m6502_config bad = uam_m6502_nmos;
    bad.bitops = true;
    EQ64(uam_m6502_configure(&m.cpu, &bad), UAM_ERR_INVALID, "CMOS options on NMOS");
    EQ64(uam_m6502_configure(&m.cpu, NULL), UAM_ERR_INVALID, "no model");
    CHECK(uam_m6502_config_of(&m.cpu) == uam_m6502_config_of(&m.cpu)
          && strcmp(uam_m6502_config_of(&m.cpu)->name, "m6502-nmos") == 0, "the model in effect");
    /* SED and CLD change what blocks are translated for, on a model with decimal mode */
    const uint8_t code[] = { 0xF8, 0xEA, 0xD8, 0xEA };
    wr(&m, 0x1000, code, sizeof(code));
    step(&m, 2);
    EQ64(m.cpu.priv, 1, "SED: decimal blocks");
    step(&m, 2);
    EQ64(m.cpu.priv, 0, "CLD: binary blocks");
    done(&m);

    machine(&m, &uam_m6502_2a03, false);
    wr(&m, 0x1000, code, sizeof(code));
    step(&m, 2);
    EQ64(m.cpu.priv, 0, "2A03: D is only a flag");
    EQ64(p_of(&m) & UAM_M6502_P_D, UAM_M6502_P_D, "but it is set");
    done(&m);

    /* a CMOS model without the bit operations: 0x07 is a 1-byte NOP */
    uam_m6502_config plain = uam_m6502_w65c02;
    plain.bitops = false;
    machine(&m, &plain, false);
    const uint8_t rmb[] = { 0x07, 0xE8 };
    wr(&m, 0x1000, rmb, sizeof(rmb));
    step(&m, 2);
    EQ64(reg(&m)[R_X], 1, "no RMB0: a 1-byte NOP");
    done(&m);
}

int main(void)
{
    uam_log_threshold = UAM_LOG_ERROR;   /* the JAM warning is expected */
    test_reset();
    for (unsigned j = 0; j < 2; j++) {
        test_arith(&uam_m6502_nmos, j);
        test_arith(&uam_m6502_w65c02, j);
        test_cli_latency(j);
    }
    test_arith(&uam_m6502_2a03, false);
    test_irq_poll();
    test_nmi_so();
    test_wait_stop();
    test_brk(&uam_m6502_nmos);
    test_brk(&uam_m6502_w65c02);
    test_rmw();
    test_jmp_ind();
    test_models();
    printf("test_m6502: %d/%d checks passed\n", n_checks - n_failed, n_checks);
    return n_failed ? 1 : 0;
}
