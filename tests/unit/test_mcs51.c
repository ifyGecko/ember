/*
 * test_mcs51.c - The 8051 module's parts the self-test firmware cannot reach on
 * mcs51-ref: interrupt entry (vectors, the return address on the stack, the levels in
 * progress and RETI, extended sources, four levels, EA and the wake rule), the
 * instruction after an IE write, SFR page saving on interrupts, code banking's mapping,
 * the register-bank and data-pointer state translated code depends on, the CPU models
 * (uam_mcs51_configure), and the timer block and serial port devices.
 *
 * Each test runs on a bare machine: code as RAM at 0, external data RAM, and RAM over
 * the whole SFR space (16 pages) so peripheral SFRs read back what was written.
 */
#include <stdio.h>
#include <string.h>

#include "uam/bus.h"
#include "uam/chardev.h"
#include "uam/dev/mcs51_serial.h"
#include "uam/dev/mcs51_timer.h"
#include "uam/isa/mcs51.h"
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

#define EQ64(got, want, what) do {                                         \
        uint64_t g_ = (got), w_ = (want);                                  \
        CHECK(g_ == w_, "%s: got 0x%llx want 0x%llx", what,                \
              (unsigned long long)g_, (unsigned long long)w_);             \
    } while (0)

/* The module's register file (isa/mcs51/mcs51.h). */
#define REG_SP      262u
#define REG_SFRPAGE 268u

enum { NOP = 0x00, RETI = 0x32, SJMP = 0x80 };

static void code(uam_machine *m, uint32_t a, const uint8_t *b, size_t n)
{
    CHECK(uam_bus_load_bytes(&m->bus, a, b, n) == UAM_OK, "load at 0x%x", (unsigned)a);
}

static void sysw(uam_machine *m, uint32_t id, uint32_t v)
{
    CHECK(m->cpu.isa->sysreg_write(&m->cpu, id, v) == UAM_OK, "write SFR 0x%x", (unsigned)id);
}
static void line(uam_machine *m, unsigned n, int level) { uam_cpu_irq_sink(&m->cpu, n, level); }
static bool take(uam_machine *m) { return m->cpu.isa->irq_take(&m->cpu); }
static bool wake(uam_machine *m) { return m->cpu.isa->irq_wakeup(&m->cpu); }
static void step(uam_machine *m, uint64_t n) { uam_exec_run(&m->exec, n); }
static uint8_t sp(uam_machine *m) { return m->cpu.regfile[REG_SP]; }

static unsigned acked[8], n_acked;
static void ack(void *ctx, unsigned source)
{
    (void)ctx;
    if (n_acked < 8)
        acked[n_acked++] = source;
}

/* A machine of model cfg: every vector slot holds RETI, and the code at 0x400 spins. */
static void machine(uam_machine *m, const uam_mcs51_config *cfg)
{
    CHECK(uam_machine_init(m, &uam_isa_mcs51) == UAM_OK, "machine init");
    CHECK(uam_mcs51_configure(&m->cpu, cfg) == UAM_OK, "%s: configure", cfg->name);
    CHECK(uam_bus_map_ram(&m->bus, "code", 0, 0x40000) == UAM_OK, "map code");
    CHECK(uam_bus_map_ram(&m->bus, "xram", UAM_MCS51_XDATA_BASE, 0x10000) == UAM_OK, "map external data");
    CHECK(uam_bus_map_ram(&m->bus, "sfr", UAM_MCS51_SFR_BASE, 0x1000) == UAM_OK, "map the SFR pages");
    for (unsigned n = 0; n < UAM_MCS51_SOURCES; n++) {
        const uint8_t r = RETI;
        code(m, 3 + 8 * n, &r, 1);
    }
    const uint8_t spin[] = { SJMP, 0xFE };
    code(m, 0x400, spin, sizeof(spin));
    n_acked = 0;
    uam_mcs51_set_irq_ack(&m->cpu, ack, NULL);
    uam_machine_reset(m, 0x400);
}

static void test_entry(void)
{
    uam_machine m;
    machine(&m, &uam_mcs51_8051_ref);
    EQ64(sp(&m), 0x07, "SP after reset");
    m.cpu.pc = 0x1234;
    line(&m, 1, 1);
    CHECK(!take(&m), "nothing taken while IE is 0");
    CHECK(!wake(&m), "nor does a disabled source wake");
    sysw(&m, UAM_MCS51_SFR_IE, 0x02);
    CHECK(!take(&m), "nothing taken without EA");
    CHECK(wake(&m), "an enabled source wakes the core without EA");
    sysw(&m, UAM_MCS51_SFR_IE, 0x82);
    CHECK(take(&m), "Timer 0 taken");
    EQ64(m.cpu.pc, 0x000B, "source 1's vector");
    EQ64(sp(&m), 0x09, "two bytes pushed");
    EQ64(uam_mcs51_iram_read(&m.cpu, 0x08), 0x34, "the pc's low byte first");
    EQ64(uam_mcs51_iram_read(&m.cpu, 0x09), 0x12, "then its high byte");
    EQ64(n_acked, 1, "acknowledged once");
    EQ64(acked[0], 1, "source 1 acknowledged");
    CHECK(!take(&m), "a source at the level in progress waits");
    line(&m, 1, 0);
    step(&m, 1);                                          /* RETI */
    EQ64(m.cpu.pc, 0x1234, "RETI returns");
    EQ64(sp(&m), 0x07, "and pops two bytes");
    line(&m, 3, 1);
    line(&m, 0, 1);
    sysw(&m, UAM_MCS51_SFR_IE, 0x8F);
    CHECK(take(&m), "taken after RETI");
    EQ64(m.cpu.pc, 0x0003, "the lowest source first (INT0 over Timer 1)");
    /* a high-priority source preempts; the low one cannot preempt it */
    sysw(&m, UAM_MCS51_SFR_IP, 0x08);
    CHECK(take(&m), "PT1 preempts INT0's handler");
    EQ64(m.cpu.pc, 0x001B, "Timer 1's vector");
    line(&m, 3, 0);
    line(&m, 2, 1);
    CHECK(!take(&m), "INT1 (low) waits while a high level runs");
    uam_machine_fini(&m);
}

static void test_levels(void)
{
    /* 8052: IPH makes four levels */
    uam_machine m;
    machine(&m, &uam_mcs51_8052_ref);
    m.cpu.pc = 0x400;
    sysw(&m, UAM_MCS51_SFR_IE, 0x8F);
    sysw(&m, UAM_MCS51_SFR_IP, 0x02);                    /* Timer 0: level 1 */
    sysw(&m, 0xB7, 0x08);                                /* Timer 1: level 2 (IPH) */
    sysw(&m, 0xB7, 0x08);
    line(&m, 1, 1);
    CHECK(take(&m), "Timer 0 at level 1");
    line(&m, 1, 0);
    line(&m, 0, 1);
    CHECK(!take(&m), "INT0 at level 0 waits");
    line(&m, 3, 1);
    CHECK(take(&m), "level 2 preempts level 1");
    EQ64(m.cpu.pc, 0x001B, "Timer 1's vector");
    sysw(&m, 0xB7, 0x0A);                                /* Timer 0's source: level 3 */
    line(&m, 3, 0);
    line(&m, 1, 1);
    CHECK(take(&m), "level 3 preempts level 2");
    uam_machine_fini(&m);

    /* CIP-51: sources past 6 have their enables and priorities in EIE1/EIP1 */
    machine(&m, &uam_mcs51_cip51_ref);
    m.cpu.pc = 0x400;
    sysw(&m, UAM_MCS51_SFR_IE, 0x80);
    line(&m, 7, 1);
    line(&m, 9, 1);
    CHECK(!take(&m), "source 7 is not enabled");
    sysw(&m, 0xE6, 0x05);                                /* EIE1: sources 7 and 9 */
    sysw(&m, 0xF6, 0x04);                                /* EIP1: source 9 high */
    CHECK(take(&m), "an extended source");
    EQ64(m.cpu.pc, 0x0003 + 8 * 9, "the high-priority one first: source 9's vector");
    line(&m, 9, 0);
    line(&m, 22, 1);
    sysw(&m, 0xE7, 0x80);                                /* EIE2 bit 7: source 22 */
    CHECK(!take(&m), "a low source cannot preempt");
    uam_machine_fini(&m);
}

/* After a write to IE the next instruction runs before the interrupt. */
static void test_ie_shadow(void)
{
    uam_machine m;
    machine(&m, &uam_mcs51_8051_ref);
    const uint8_t prog[] = { 0x75, 0xA8, 0x82,          /* 0x200 MOV IE, #0x82 */
                             NOP,                       /* 0x203 */
                             NOP,                       /* 0x204 */
                             SJMP, 0xFE };              /* 0x205 */
    code(&m, 0x200, prog, sizeof(prog));
    m.cpu.pc = 0x200;
    line(&m, 1, 1);
    step(&m, 3);                                          /* MOV, NOP, then the entry */
    EQ64(uam_mcs51_iram_read(&m.cpu, 0x08), 0x04, "the return address is the second NOP");
    uam_machine_fini(&m);

    /* the IE write as a full block's last instruction still takes the next one along */
    machine(&m, &uam_mcs51_8051_ref);
    uint8_t late[31 + 6];
    memset(late, NOP, 31);
    memcpy(late + 31, (const uint8_t[]){ 0x75, 0xA8, 0x82, NOP, SJMP, 0xFE }, 6);
    code(&m, 0x200, late, sizeof(late));
    m.cpu.pc = 0x200;
    line(&m, 1, 1);
    EQ64(uam_exec_run(&m.exec, 34), UAM_STOP_INSN_LIMIT, "an IE write in the last slot is not an error");
    EQ64(uam_mcs51_iram_read(&m.cpu, 0x08), 0x23, "the return address is after the NOP behind it");
    uam_machine_fini(&m);
}

static void test_sfr_pages(void)
{
    uam_machine m;
    machine(&m, &uam_mcs51_cip51_ref);
    m.cpu.pc = 0x400;
    m.cpu.regfile[REG_SFRPAGE] = 2;
    uam_mcs51_set_sfr_autopage(&m.cpu, true);
    CHECK(uam_mcs51_set_irq_sfrpage(&m.cpu, 3, 0x0F) == UAM_OK, "page of source 3");
    CHECK(uam_mcs51_set_irq_sfrpage(&m.cpu, 32, 0) == UAM_ERR_INVALID, "no source 32");
    sysw(&m, UAM_MCS51_SFR_IE, 0x88);
    line(&m, 3, 1);
    CHECK(take(&m), "Timer 1 taken");
    EQ64(m.cpu.regfile[REG_SFRPAGE], 0x0F, "its page selected");
    line(&m, 3, 0);
    step(&m, 1);                                          /* RETI */
    EQ64(m.cpu.regfile[REG_SFRPAGE], 2, "RETI restores the page");
    /* a peripheral SFR access goes to the page selected */
    const uint8_t prog[] = { 0x75, 0x84, 0x05,          /* MOV SFRPAGE, #5 */
                             0x75, 0x99, 0x5A,          /* MOV 0x99, #0x5A */
                             SJMP, 0xFE };
    code(&m, 0x300, prog, sizeof(prog));
    m.cpu.pc = 0x300;
    sysw(&m, UAM_MCS51_SFR_IE, 0x00);
    step(&m, 2);
    uint64_t v = 0;
    uam_bus_read(&m.bus, UAM_MCS51_SFR_BASE + 5 * 0x100 + 0x99, 1, UAM_ACC_LE, &v);
    EQ64(v, 0x5A, "page 5's SFR 0x99");
    uam_machine_fini(&m);
}

static void test_banks(void)
{
    uam_machine m;
    machine(&m, &uam_mcs51_cip51_ref);
    uam_xlate x;
    const uam_isa *isa = m.cpu.isa;
    isa->translate(&m.cpu, 0x8123, UAM_XLATE_FETCH, &x);
    EQ64(x.paddr, 0x8123, "block 1 after reset: the window's own addresses");
    CHECK(!x.fixed, "a banked mapping can change");
    isa->translate(&m.cpu, 0x7FFF, UAM_XLATE_FETCH, &x);
    CHECK(x.paddr == 0x7FFF && x.fixed, "the common area is fixed");
    uam_mcs51_set_code_bank(&m.cpu, 3, 0);
    isa->translate(&m.cpu, 0x8123, UAM_XLATE_FETCH, &x);
    EQ64(x.paddr, 0x18123, "fetch from block 3");
    isa->translate(&m.cpu, 0x8123, UAM_XLATE_LOAD, &x);
    EQ64(x.paddr, 0x0123, "MOVC from block 0 (the common area)");
    isa->translate(&m.cpu, 0x8123, UAM_XLATE_DEBUG, &x);
    EQ64(x.paddr, 0x8123, "a debugger sees bus addresses");
    isa->translate(&m.cpu, UAM_MCS51_XDATA_BASE + 0x8123, UAM_XLATE_LOAD, &x);
    EQ64(x.paddr, UAM_MCS51_XDATA_BASE + 0x8123, "external data is not banked");
    uam_machine_reset(&m, 0);
    isa->translate(&m.cpu, 0x8000, UAM_XLATE_FETCH, &x);
    EQ64(x.paddr, 0x8000, "reset selects block 1 again");
    uam_machine_fini(&m);
}

static void test_priv(void)
{
    uam_machine m;
    machine(&m, &uam_mcs51_8052_ref);
    EQ64(m.cpu.priv, 0, "bank 0, DPTR0");
    sysw(&m, UAM_MCS51_SFR_PSW, 0x18);
    EQ64(m.cpu.priv, 3, "PSW.RS = 3");
    sysw(&m, 0xA2, 0x01);
    EQ64(m.cpu.priv, 3 | 4, "DPS = 1");
    /* the same code runs on the bank it finds */
    const uint8_t prog[] = { 0x7A, 0x11,                /* MOV R2, #0x11 */
                             0x75, 0xD0, 0x08,          /* MOV PSW, #0x08 */
                             0x7A, 0x22,                /* MOV R2, #0x22 */
                             SJMP, 0xFE };
    code(&m, 0x300, prog, sizeof(prog));
    sysw(&m, UAM_MCS51_SFR_PSW, 0x00);
    m.cpu.pc = 0x300;
    step(&m, 3);
    EQ64(uam_mcs51_iram_read(&m.cpu, 0x02), 0x11, "bank 0's R2");
    EQ64(uam_mcs51_iram_read(&m.cpu, 0x0A), 0x22, "bank 1's R2, same block");
    uam_machine_fini(&m);
}

static void test_configure(void)
{
    uam_machine m;
    machine(&m, &uam_mcs51_8051_ref);
    uam_mcs51_config c = uam_mcs51_8052_ref;
    c.iram_size = 64;
    CHECK(uam_mcs51_configure(&m.cpu, &c) == UAM_ERR_INVALID, "64 bytes of RAM");
    c = uam_mcs51_8052_ref;
    c.n_sources = 33;
    CHECK(uam_mcs51_configure(&m.cpu, &c) == UAM_ERR_INVALID, "33 sources");
    c = uam_mcs51_8052_ref;
    c.dps = 0;
    c.dpl1 = 0x84;
    CHECK(uam_mcs51_configure(&m.cpu, &c) == UAM_ERR_INVALID, "DPTR1 without DPS");
    c = uam_mcs51_cip51_ref;
    c.bank_from = 0x9000;
    CHECK(uam_mcs51_configure(&m.cpu, &c) == UAM_ERR_INVALID, "a window not dividing its start");
    c.bank_from = 0xC000;
    CHECK(uam_mcs51_configure(&m.cpu, &c) == UAM_OK, "a 16 KiB window");
    c.iph = 0x40;
    CHECK(uam_mcs51_configure(&m.cpu, &c) == UAM_ERR_INVALID, "an SFR below 0x80");
    EQ64(uam_mcs51_config_of(&m.cpu)->bank_from, 0xC000, "the last valid model stays");
    CHECK(uam_mcs51_config_of(NULL) == NULL, "no cpu");
    uam_machine_fini(&m);
}

/* ---- devices -------------------------------------------------------------------- */

static unsigned lv[5];
static void sink(void *ctx, unsigned input, int level)
{
    (void)ctx;
    if (input < 5)
        lv[input] = (unsigned)level;
}

static uint8_t rd(uam_device *d, unsigned off)
{
    uint64_t v = 0;
    d->ops->read(d, off, 1, &v);
    return (uint8_t)v;
}
static void wr(uam_device *d, unsigned off, uint8_t v) { d->ops->write(d, off, 1, v); }

enum { TCON = 0, TMOD, TL0, TL1, TH0, TH1 };

static void test_timer(void)
{
    uam_machine m;
    machine(&m, &uam_mcs51_8051_ref);
    uam_device *t = uam_mcs51_timer_create("timer", &m.sched);
    CHECK(t != NULL, "create");
    uam_machine_add_device(&m, t);
    t->ops->reset(t);
    memset(lv, 0, sizeof(lv));
    for (unsigned k = 0; k < 4; k++)
        uam_signal_connect(uam_mcs51_timer_irq(t, k), sink, NULL, k);
    /* mode 0: 13 bits, TL0's low five */
    wr(t, TMOD, 0x00);
    wr(t, TH0, 0xFF);
    wr(t, TL0, 0x1C);
    wr(t, TCON, 0x10);                                   /* TR0 */
    step(&m, 3);
    EQ64(rd(t, TL0) & 0x1F, 0x1F, "three ticks");
    EQ64(lv[1], 0, "no overflow yet");
    step(&m, 1);
    CHECK((rd(t, TCON) & 0x20) != 0, "TF0 at 0x2000 (a read brings the count up to date)");
    EQ64(lv[1], 1, "TF0 raises its line");
    EQ64(rd(t, TH0), 0x00, "TH0 wrapped");
    uam_mcs51_timer_ack(t, 1);
    EQ64(lv[1], 0, "taking the vector clears TF0");
    /* mode 2: reload */
    wr(t, TCON, 0x00);
    wr(t, TMOD, 0x20);
    wr(t, TH1, 0xF0);
    wr(t, TL1, 0xFE);
    wr(t, TCON, 0x40);                                   /* TR1 */
    step(&m, 2 + 16 * 3 + 5);
    EQ64(lv[3], 1, "the scheduled overflow raised TF1's line");
    EQ64(rd(t, TL1), 0xF5, "reloaded from TH1 each 16 ticks");
    CHECK((rd(t, TCON) & 0x80) != 0, "TF1");
    /* GATE: Timer 1 counts only while INT1 is high */
    wr(t, TCON, 0x40);
    wr(t, TMOD, 0xA0);                                   /* GATE, mode 2 */
    wr(t, TL1, 0x00);
    uam_mcs51_timer_int_pin(t, 1, 0);
    step(&m, 10);
    EQ64(rd(t, TL1), 0x00, "stopped with INT1 low");
    uam_mcs51_timer_int_pin(t, 1, 1);
    step(&m, 10);
    EQ64(rd(t, TL1), 10, "counting with INT1 high");
    /* INT0: edge-triggered sets IE0, which the acknowledge clears; level follows the pin */
    wr(t, TCON, 0x01);                                   /* IT0 */
    uam_mcs51_timer_int_pin(t, 0, 0);
    EQ64(lv[0], 1, "a falling edge sets IE0");
    uam_mcs51_timer_int_pin(t, 0, 1);
    EQ64(lv[0], 1, "and it stays set");
    uam_mcs51_timer_ack(t, 0);
    EQ64(lv[0], 0, "the acknowledge clears an edge");
    wr(t, TCON, 0x00);                                   /* level-triggered */
    uam_mcs51_timer_int_pin(t, 0, 0);
    EQ64(lv[0], 1, "IE0 follows a low pin");
    uam_mcs51_timer_ack(t, 0);
    EQ64(lv[0], 1, "the acknowledge leaves a level alone");
    uam_mcs51_timer_int_pin(t, 0, 1);
    EQ64(lv[0], 0, "and the pin going high clears it");
    uam_machine_fini(&m);
}

typedef struct sink_cd {
    uam_chardev cd;
    char        out[64];
    size_t      n;
} sink_cd;

static size_t cd_write(uam_chardev *cd, const void *buf, size_t len)
{
    sink_cd *s = (sink_cd *)cd;
    for (size_t i = 0; i < len && s->n < sizeof(s->out) - 1; i++)
        s->out[s->n++] = ((const char *)buf)[i];
    return len;
}
static int cd_fd(uam_chardev *cd) { (void)cd; return -1; }
static void cd_nop(uam_chardev *cd) { (void)cd; }
static const uam_chardev_ops sink_ops = { "test", cd_write, cd_fd, cd_nop, cd_nop };

static void test_serial(void)
{
    sink_cd cd = { .n = 0 };
    uam_chardev_init(&cd.cd, &sink_ops);
    uam_device *s = uam_mcs51_serial_create("serial0", &cd.cd);
    CHECK(s != NULL, "create");
    memset(lv, 0, sizeof(lv));
    uam_signal_connect(uam_mcs51_serial_irq(s), sink, NULL, 4);
    wr(s, 1, 'h');
    EQ64(cd.n, 1, "a byte out");
    EQ64(rd(s, 0) & 0x02, 0x02, "TI");
    EQ64(lv[4], 1, "TI raises the line");
    wr(s, 0, 0x00);
    EQ64(lv[4], 0, "clearing TI lowers it");
    uam_chardev_input(&cd.cd, "ab", 2);
    EQ64(rd(s, 0) & 0x01, 0, "nothing received with REN clear");
    wr(s, 0, 0x50);                                      /* mode 1, REN */
    EQ64(rd(s, 0) & 0x05, 0x05, "RI and RB8");
    EQ64(rd(s, 1), 'a', "the first byte");
    EQ64(rd(s, 1), 'a', "reading SBUF does not take the next");
    wr(s, 0, 0x50);                                      /* clear RI */
    EQ64(rd(s, 1), 'b', "the second byte once RI is cleared");
    EQ64(uam_mcs51_serial_feed(s, "xyz", 3), 3, "feed");
    s->ops->destroy(s);
}

int main(void)
{
    test_entry();
    test_levels();
    test_ie_shadow();
    test_sfr_pages();
    test_banks();
    test_priv();
    test_configure();
    test_timer();
    test_serial();
    printf("test_mcs51: %d/%d checks passed\n", n_checks - n_failed, n_checks);
    return n_failed ? 1 : 0;
}
