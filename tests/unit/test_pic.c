/*
 * test_pic.c - The 8-bit PIC module's parts the self-test firmware cannot reach on pic-ref:
 * the CPU models (uam_pic_configure), the stacks' overflow and underflow with STVREN off
 * (the mid-range circular stack, the enhanced wrap, PIC18's STKFUL/STKUNF without a reset)
 * and on (the reset cause and the board's hook), interrupt gating (PEIE, INTCON's own
 * pairs through uam_pic_intcon_set, the wake rule ignoring GIE, PIC18 priorities: GIEL
 * needs GIEH, a high interrupt preempting a low one and RETFIE re-enabling the right
 * level), the baseline's lack of interrupts, the reset vector of each family, TBLWT's
 * hook, the edges of the enhanced FSR map, and the debugger's view of the stack.
 *
 * Each test runs on a bare machine: program memory as RAM at 0, the data space at
 * UAM_PIC_DATA_BASE.
 */
#include <stdio.h>
#include <string.h>

#include "uam/bus.h"
#include "uam/isa/pic.h"
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

/* The module's register file (isa/pic8/pic.h) and gdb register numbers. */
#define REG_W      0u
#define REG_INTCON 10u
#define REG_RCON   11u
#define REG_FSR0   12u
#define REG_TABLAT 20u
#define REG_TBLPTR 24u
#define GDB_STKPTR 9u
#define GDB_STACK0 14u

static void put16(uam_machine *m, uint64_t pa, uint16_t w)
{
    uint8_t b[2] = { (uint8_t)w, (uint8_t)(w >> 8) };
    CHECK(uam_bus_load_bytes(&m->bus, pa, b, 2) == UAM_OK, "load at 0x%llx", (unsigned long long)pa);
}
static uint16_t get16p(uam_machine *m, uint64_t pa)
{
    uint64_t v = 0;
    uam_bus_read(&m->bus, pa, 2, UAM_ACC_LE, &v);
    return (uint16_t)v;
}
static void data_w(uam_machine *m, uint32_t a, uint8_t v)
{
    uam_bus_write(&m->bus, UAM_PIC_DATA_BASE + a, 1, UAM_ACC_LE, v);
}
static uint64_t gdb(uam_machine *m, unsigned n)
{
    uint64_t v = 0;
    CHECK(m->cpu.isa->gdb->read_reg(&m->cpu, n, &v), "gdb register %u", n);
    return v;
}
static void line(uam_machine *m, unsigned n, int level) { uam_cpu_irq_sink(&m->cpu, n, level); }
static bool take(uam_machine *m) { return m->cpu.isa->irq_take(&m->cpu); }
static void step(uam_machine *m, uint64_t n) { uam_exec_run(&m->exec, n); }

static unsigned hook_calls, hook_cause;
static void hook(void *ctx, uam_pic_reset_cause cause)
{
    (void)ctx;
    hook_calls++;
    hook_cause = cause;
}

/* A machine of model cfg (with features changed to `features`), program memory filled
 * with NOP-like words, reset at `entry`. */
static void machine(uam_machine *m, const uam_pic_config *base, unsigned features, uint64_t entry)
{
    uam_pic_config cfg = *base;
    cfg.features = features;
    CHECK(uam_machine_init(m, &uam_isa_pic) == UAM_OK, "machine init");
    CHECK(uam_pic_configure(&m->cpu, &cfg) == UAM_OK, "%s: configure", cfg.name);
    CHECK(uam_bus_map_ram(&m->bus, "flash", 0, 0x20000) == UAM_OK, "map program memory");
    CHECK(uam_bus_map_ram(&m->bus, "data", UAM_PIC_DATA_BASE, 0x1000) == UAM_OK, "map the data space");
    hook_calls = 0;
    hook_cause = 99;
    uam_pic_set_reset_hook(&m->cpu, hook, NULL);
    uam_machine_reset(m, entry);
}

static void test_configure(void)
{
    uam_machine m;
    CHECK(uam_machine_init(&m, &uam_isa_pic) == UAM_OK, "machine init");
    EQ64(uam_pic_config_of(&m.cpu)->core, UAM_PIC18, "the default model is PIC18");
    uam_pic_config c = uam_pic_baseline_ref;
    c.banks = 3;
    EQ64(uam_pic_configure(&m.cpu, &c), UAM_ERR_INVALID, "baseline banks must be 1, 2, 4 or 8");
    c = uam_pic_mid_ref;
    c.features = UAM_PIC_F_XINST;
    EQ64(uam_pic_configure(&m.cpu, &c), UAM_ERR_INVALID, "XINST is PIC18's");
    c = uam_pic_mid_ref;
    c.prog_words = 16384;
    EQ64(uam_pic_configure(&m.cpu, &c), UAM_ERR_INVALID, "a mid-range PC has 13 bits");
    EQ64(uam_pic_configure(&m.cpu, &uam_pic_enhanced_ref), UAM_OK, "enhanced");
    EQ64(uam_pic_config_of(&m.cpu)->prog_words, 32768, "config_of");
    uam_machine_fini(&m);
}

static void test_mid_stack(void)
{
    /* nine nested calls: the ninth overwrites the first return address (an 8-entry ring) */
    uam_machine m;
    machine(&m, &uam_pic_mid_ref, 0, 0x200);
    for (unsigned i = 0; i < 9; i++)
        put16(&m, 2 * (0x100 + i), (uint16_t)(0x2000 | (0x101 + i)));   /* CALL next */
    put16(&m, 2 * 0x109, 0x2800 | 0x109);                                 /* GOTO $ */
    m.cpu.pc = 2 * 0x100;
    step(&m, 12);
    EQ64(m.cpu.pc, 2 * 0x109, "mid: at the end of the chain");
    EQ64(gdb(&m, GDB_STACK0 + 0), 2 * 0x109, "mid: the ninth return address took slot 0");
    EQ64(gdb(&m, GDB_STACK0 + 1), 2 * 0x102, "mid: the second is in slot 1");
    EQ64(gdb(&m, GDB_STKPTR), 1, "mid: the ring's pointer wrapped");
    EQ64(hook_calls, 0, "mid: no stack reset");
    uam_machine_fini(&m);
}

static void test_enh_stack(void)
{
    uam_machine m;
    /* STVREN off: the 17th push wraps to entry 0 (STKPTR 0x10) */
    machine(&m, &uam_pic_enhanced_ref, 0, 0x200);
    EQ64(gdb(&m, GDB_STKPTR), 0x1F, "enhanced: empty after reset");
    for (unsigned i = 0; i < 17; i++)
        put16(&m, 2 * (0x100 + i), (uint16_t)(0x2000 | (0x101 + i)));
    put16(&m, 2 * 0x111, 0x2800 | 0x111);
    m.cpu.pc = 2 * 0x100;
    step(&m, 20);
    EQ64(gdb(&m, GDB_STKPTR), 0x10, "enhanced: STKPTR 0x10 after 17 pushes");
    EQ64(gdb(&m, GDB_STACK0 + 0), 2 * 0x111, "enhanced: entry 0 overwritten");
    EQ64(hook_calls, 0, "enhanced: no reset without STVREN");
    uam_machine_fini(&m);

    /* STVREN on: a RETURN on the empty stack resets the core */
    machine(&m, &uam_pic_enhanced_ref, UAM_PIC_F_STVREN, 0x200);
    put16(&m, 0x200, 0x0008);                                             /* RETURN */
    m.cpu.pc = 0x200;
    step(&m, 1);
    EQ64(hook_calls, 1, "enhanced: underflow calls the reset hook");
    EQ64(hook_cause, UAM_PIC_RESET_STACK_UNDERFLOW, "enhanced: underflow cause");
    EQ64(m.cpu.pc, 0, "enhanced: at the reset vector");
    EQ64(uam_pic_last_reset(&m.cpu), UAM_PIC_RESET_STACK_UNDERFLOW, "enhanced: last reset");
    uam_machine_fini(&m);
}

static void test_p18_stack(void)
{
    uam_machine m;
    /* STVREN off: underflow returns 0 and sets STKUNF; overflow sets STKFUL and stops */
    machine(&m, &uam_pic18_ref, 0, 0x200);
    put16(&m, 0x200, 0x0012);                                             /* RETURN */
    m.cpu.pc = 0x200;
    step(&m, 1);
    EQ64(m.cpu.pc, 0, "pic18: underflow returns 0");
    EQ64(gdb(&m, GDB_STKPTR), 0x40, "pic18: STKUNF");
    EQ64(hook_calls, 0, "pic18: no reset without STVREN");
    put16(&m, 0x300, 0xDFFF);                                             /* RCALL $ */
    m.cpu.pc = 0x300;
    step(&m, 40);
    EQ64(gdb(&m, GDB_STKPTR), 0xC0 | 31, "pic18: STKFUL with the pointer at 31");
    EQ64(gdb(&m, GDB_STACK0 + 31), 0x302, "pic18: the 31st push is kept");
    uam_machine_fini(&m);

    /* STVREN on: the 31st push resets */
    machine(&m, &uam_pic18_ref, UAM_PIC_F_STVREN, 0x200);
    put16(&m, 0x300, 0xDFFF);
    m.cpu.pc = 0x300;
    step(&m, 31);
    EQ64(hook_cause, UAM_PIC_RESET_STACK_OVERFLOW, "pic18: overflow reset");
    EQ64(gdb(&m, GDB_STKPTR), 0x80, "pic18: STKFUL kept, the pointer at 0");
    EQ64(m.cpu.pc, 0, "pic18: at the reset vector");
    uam_machine_fini(&m);
}

static void test_irq_mid(void)
{
    uam_machine m;
    machine(&m, &uam_pic_mid_ref, 0, 0x200);
    m.cpu.pc = 0x400;
    line(&m, UAM_PIC_IRQ_PERIPH, 1);
    m.cpu.regfile[REG_INTCON] = 0x80;                                     /* GIE, no PEIE */
    CHECK(!take(&m), "mid: a peripheral needs PEIE");
    CHECK(!m.cpu.isa->irq_wakeup(&m.cpu), "mid: nor does it wake SLEEP without PEIE");
    m.cpu.regfile[REG_INTCON] = 0x40;                                     /* PEIE, no GIE */
    CHECK(!take(&m), "mid: nothing without GIE");
    CHECK(m.cpu.isa->irq_wakeup(&m.cpu), "mid: but it wakes SLEEP");
    m.cpu.regfile[REG_INTCON] = 0xC0;
    CHECK(take(&m), "mid: taken");
    EQ64(m.cpu.pc, 8, "mid: word 4");
    EQ64(m.cpu.regfile[REG_INTCON], 0x40, "mid: GIE cleared");
    EQ64(gdb(&m, GDB_STACK0), 0x400, "mid: the pc pushed");
    line(&m, UAM_PIC_IRQ_PERIPH, 0);
    m.cpu.regfile[REG_INTCON] = 0xA0;                                     /* GIE, T0IE */
    CHECK(!take(&m), "mid: T0IE without its flag");
    uam_pic_intcon_set(&m.cpu, 0x04);                                     /* T0IF */
    CHECK(take(&m), "mid: INTCON's own pair needs no PEIE");
    uam_machine_fini(&m);

    machine(&m, &uam_pic_baseline_ref, 0, 0x200);
    EQ64(m.cpu.pc, 0x200, "baseline: the board's entry");
    line(&m, UAM_PIC_IRQ_PERIPH, 1);
    m.cpu.regfile[REG_INTCON] = 0xFF;
    CHECK(!take(&m), "baseline: no interrupts");
    uam_machine_fini(&m);
}

static void test_irq_p18(void)
{
    uam_machine m;
    machine(&m, &uam_pic18_ref, 0, 0x200);
    put16(&m, 0x08, 0x0010);                                              /* RETFIE */
    put16(&m, 0x18, 0x0010);
    put16(&m, 0x400, 0xD7FF);                                             /* BRA $ */
    m.cpu.pc = 0x400;
    m.cpu.regfile[REG_RCON] |= 0x80;                                      /* IPEN */
    m.cpu.regfile[REG_INTCON] = 0x80;                                     /* GIEH only */
    line(&m, UAM_PIC_IRQ_LOW, 1);
    CHECK(!take(&m), "pic18: a low interrupt needs GIEL");
    m.cpu.regfile[REG_INTCON] = 0x40;                                     /* GIEL only */
    CHECK(!take(&m), "pic18: and GIEH");
    m.cpu.regfile[REG_INTCON] = 0xC0;
    CHECK(take(&m), "pic18: low taken");
    EQ64(m.cpu.pc, 0x18, "pic18: the low vector");
    EQ64(m.cpu.regfile[REG_INTCON], 0x80, "pic18: GIEL cleared");
    line(&m, UAM_PIC_IRQ_LOW, 0);
    line(&m, UAM_PIC_IRQ_PERIPH, 1);
    CHECK(take(&m), "pic18: a high interrupt preempts the low handler");
    EQ64(m.cpu.pc, 0x08, "pic18: the high vector");
    EQ64(m.cpu.regfile[REG_INTCON], 0x00, "pic18: GIEH cleared");
    line(&m, UAM_PIC_IRQ_PERIPH, 0);
    step(&m, 1);                                                          /* RETFIE */
    EQ64(m.cpu.pc, 0x18, "pic18: back in the low handler");
    EQ64(m.cpu.regfile[REG_INTCON], 0x80, "pic18: RETFIE set GIEH (not GIEL)");
    step(&m, 1);
    EQ64(m.cpu.pc, 0x400, "pic18: back in the program");
    EQ64(m.cpu.regfile[REG_INTCON], 0xC0, "pic18: RETFIE set GIEL");
    /* without IPEN both lines are peripheral interrupts gated by PEIE */
    m.cpu.regfile[REG_RCON] &= 0x7F;
    m.cpu.regfile[REG_INTCON] = 0x80;
    line(&m, UAM_PIC_IRQ_LOW, 1);
    CHECK(!take(&m), "pic18: compatibility mode: PEIE");
    m.cpu.regfile[REG_INTCON] = 0xC0;
    CHECK(take(&m), "pic18: compatibility mode: taken");
    EQ64(m.cpu.pc, 0x08, "pic18: compatibility mode: one vector");
    uam_machine_fini(&m);
}

static unsigned tblwt_addr, tblwt_byte, tblwt_calls;
static void tblwt(void *ctx, uint32_t addr, uint8_t byte)
{
    (void)ctx;
    tblwt_calls++;
    tblwt_addr = addr;
    tblwt_byte = byte;
}

static void test_misc(void)
{
    uam_machine m;
    /* TBLWT*+ hands the board TBLPTR and TABLAT */
    machine(&m, &uam_pic18_ref, 0, 0x200);
    uam_pic_set_tblwt(&m.cpu, tblwt, NULL);
    put16(&m, 0x200, 0x000D);                                             /* TBLWT*+ */
    uam_cpu_reg_write(&m.cpu, REG_TBLPTR, 32, 0x301234);
    m.cpu.regfile[REG_TABLAT] = 0xA5;
    step(&m, 1);
    EQ64(tblwt_calls, 1, "tblwt: called");
    EQ64(tblwt_addr, 0x301234, "tblwt: the address");
    EQ64(tblwt_byte, 0xA5, "tblwt: the byte");
    EQ64(uam_cpu_reg_read(&m.cpu, REG_TBLPTR, 32), 0x301235, "tblwt: post-increment");
    /* RESET: the cause, RCON.RI */
    put16(&m, 0x202, 0x00FF);
    step(&m, 1);
    EQ64(hook_cause, UAM_PIC_RESET_INSN, "reset: the hook's cause");
    EQ64(m.cpu.pc, 0, "reset: the vector");
    EQ64(m.cpu.regfile[REG_RCON] & 0x10, 0, "reset: RI cleared");
    uam_machine_fini(&m);

    /* the baseline reset vector is the last word */
    uam_pic_config b = uam_pic_baseline_ref;
    CHECK(uam_machine_init(&m, &uam_isa_pic) == UAM_OK && uam_pic_configure(&m.cpu, &b) == UAM_OK,
          "baseline machine");
    CHECK(uam_bus_map_ram(&m.bus, "flash", 0, 0x1000) == UAM_OK, "map");
    put16(&m, 0xFFE, 0x0004);                                             /* CLRWDT */
    put16(&m, 0x000, 0x0A00);                                             /* GOTO 0 */
    uam_machine_reset(&m, 0xFFE);
    step(&m, 2);
    EQ64(m.cpu.pc, 0, "baseline: the PC wraps from the last word to 0");
    uam_machine_fini(&m);

    /* the enhanced FSR map's edges */
    machine(&m, &uam_pic_enhanced_ref, 0, 0x200);
    data_w(&m, 30 * 128 + 0x6F, 0x5C);                                    /* bank 30's last GPR */
    put16(&m, 0x200, 0x0012);                                             /* MOVIW FSR0++ */
    put16(&m, 0x202, 0x0012);
    put16(&m, 0x204, 0x0018);                                             /* MOVWI ++FSR0 */
    uam_cpu_reg_write(&m.cpu, REG_FSR0, 16, 0x29AF);
    step(&m, 1);
    EQ64(m.cpu.regfile[REG_W], 0x5C, "enhanced: linear 0x29AF is bank 30's 0x6F");
    m.cpu.regfile[REG_W] = 0x77;
    step(&m, 1);
    EQ64(m.cpu.regfile[REG_W], 0, "enhanced: linear 0x29B0 reads 0");
    uam_cpu_reg_write(&m.cpu, REG_FSR0, 16, 0x8000 + 0x7F);
    m.cpu.regfile[REG_W] = 0x99;
    uint16_t before = get16p(&m, 2 * 0x80);
    step(&m, 1);
    EQ64(get16p(&m, 2 * 0x80), before, "enhanced: a write to program memory does nothing");
    uam_machine_fini(&m);
}

int main(void)
{
    test_configure();
    test_mid_stack();
    test_enh_stack();
    test_p18_stack();
    test_irq_mid();
    test_irq_p18();
    test_misc();
    printf("test_pic: %d/%d checks passed\n", n_checks - n_failed, n_checks);
    return n_failed ? 1 : 0;
}
