/*
 * test_z80.c - The Z80 module's parts the self-test firmware cannot reach on z80-ref:
 * interrupt entry in the three modes (the acknowledge hook's bytes, I, the return address,
 * IFF1/IFF2), NMI and RETN, RETI reported to the board, HALT, the instruction after EI,
 * R at interrupts, the flags of the repeating block instructions while they repeat,
 * MEMPTR after the instructions that set it, the port address on the bus and the board's
 * decoding, the CPU models, the debugger view, and the CTC device.
 *
 * Each test runs on a bare machine: RAM over the 64 KiB of memory and over the whole I/O
 * space, so ports read back what was written.
 */
#include <stdio.h>
#include <string.h>

#include "uam/bus.h"
#include "uam/dev/z80_ctc.h"
#include "uam/isa/z80.h"
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

/* The module's register file (isa/z80/z80.h). */
enum { R_BC = 0, R_DE = 2, R_HL = 4, R_F = 6, R_A = 7, R_IX = 8, R_SP = 12, R_I = 22, R_RCNT = 23,
       R_R7 = 24, R_WZ = 26 };
/* Its system registers. */
enum { SYS_IFF = 0, SYS_IM = 1 };

static void code(uam_machine *m, uint32_t a, const uint8_t *b, size_t n)
{
    CHECK(uam_bus_load_bytes(&m->bus, a, b, n) == UAM_OK, "load at 0x%x", (unsigned)a);
}
static uint8_t mem(uam_machine *m, uint64_t a)
{
    uint64_t v = 0;
    uam_bus_read(&m->bus, a, 1, UAM_ACC_LE, &v);
    return (uint8_t)v;
}
static void poke(uam_machine *m, uint64_t a, uint8_t v) { uam_bus_write(&m->bus, a, 1, UAM_ACC_LE, v); }
static uint16_t r16(uam_machine *m, uint32_t off) { return (uint16_t)uam_cpu_reg_read(&m->cpu, off, 16); }
static void w16(uam_machine *m, uint32_t off, uint16_t v) { uam_cpu_reg_write(&m->cpu, off, 16, v); }
static void sysw(uam_machine *m, uint32_t id, uint32_t v)
{
    CHECK(m->cpu.isa->sysreg_write(&m->cpu, id, v) == UAM_OK, "write sysreg %u", (unsigned)id);
}
static uint64_t sysr(uam_machine *m, uint32_t id)
{
    uint64_t v = 0;
    CHECK(m->cpu.isa->sysreg_read(&m->cpu, id, &v) == UAM_OK, "read sysreg %u", (unsigned)id);
    return v;
}
static void line(uam_machine *m, unsigned n, int level) { uam_cpu_irq_sink(&m->cpu, n, level); }
static bool take(uam_machine *m) { return m->cpu.isa->irq_take(&m->cpu); }
static bool wake(uam_machine *m) { return m->cpu.isa->irq_wakeup(&m->cpu); }
/* Run n instructions from where the hart is. */
static void step(uam_machine *m, uint64_t n) { uam_exec_run(&m->exec, n); }
static uint8_t rr(uam_machine *m) { return (uint8_t)((m->cpu.regfile[R_R7] & 0x80) | (m->cpu.regfile[R_RCNT] & 0x7F)); }

static uint8_t bus_bytes[3];
static unsigned n_inta, n_reti;
static uint8_t inta(void *ctx, unsigned n)
{
    (void)ctx;
    n_inta++;
    return n < 3 ? bus_bytes[n] : 0xFF;
}
static void reti(void *ctx) { (void)ctx; n_reti++; }

/* A machine of model cfg, the code at 0x1000 spinning (JR $). */
static void machine(uam_machine *m, const uam_z80_config *cfg)
{
    CHECK(uam_machine_init(m, &uam_isa_z80) == UAM_OK, "machine init");
    CHECK(uam_z80_configure(&m->cpu, cfg) == UAM_OK, "%s: configure", cfg->name);
    CHECK(uam_bus_map_ram(&m->bus, "ram", 0, 0x10000) == UAM_OK, "map memory");
    CHECK(uam_bus_map_ram(&m->bus, "io", UAM_Z80_IO_BASE, 0x10000) == UAM_OK, "map the I/O space");
    const uint8_t spin[] = { 0x18, 0xFE };
    code(m, 0x1000, spin, sizeof(spin));
    n_inta = n_reti = 0;
    memset(bus_bytes, 0xFF, sizeof(bus_bytes));
    uam_z80_set_inta(&m->cpu, inta, NULL);
    uam_z80_set_reti(&m->cpu, reti, NULL);
    uam_machine_reset(m, 0x1000);
    w16(m, R_SP, 0x8000);
}

static void test_reset_and_models(void)
{
    uam_machine m;
    machine(&m, &uam_z80_nmos_ref);
    EQ64(m.cpu.pc, 0x1000, "entry");
    EQ64(r16(&m, R_F), 0xFFFF, "AF all ones after reset");
    EQ64(sysr(&m, SYS_IFF), 0, "interrupts disabled after reset");
    EQ64(sysr(&m, SYS_IM), 0, "IM 0 after reset");
    CHECK(uam_z80_config_of(&m.cpu) && strcmp(uam_z80_config_of(&m.cpu)->name, "z80-nmos-ref") == 0, "model");
    CHECK(uam_z80_configure(NULL, &uam_z80_cmos_ref) == UAM_ERR_INVALID, "configure needs a hart");
    CHECK(uam_z80_configure(&m.cpu, NULL) == UAM_ERR_INVALID, "and a model");
    /* OUT (C),0: the model's byte */
    const uint8_t outc0[] = { 0xED, 0x71 };
    code(&m, 0x2000, outc0, sizeof(outc0));
    w16(&m, R_BC, 0x0042);
    m.cpu.pc = 0x2000;
    poke(&m, UAM_Z80_IO_BASE + 0x42, 0x5A);
    step(&m, 1);
    EQ64(mem(&m, UAM_Z80_IO_BASE + 0x42), 0x00, "NMOS: OUT (C),0 writes 0");
    uam_machine_fini(&m);
    machine(&m, &uam_z80_cmos_ref);
    code(&m, 0x2000, outc0, sizeof(outc0));
    w16(&m, R_BC, 0x0042);
    m.cpu.pc = 0x2000;
    step(&m, 1);
    EQ64(mem(&m, UAM_Z80_IO_BASE + 0x42), 0xFF, "CMOS: OUT (C),0 writes 0xFF");
    uam_machine_fini(&m);
}

static void test_im1_im2(void)
{
    uam_machine m;
    machine(&m, &uam_z80_nmos_ref);
    m.cpu.pc = 0x1234;
    line(&m, UAM_Z80_LINE_INT, 1);
    CHECK(!take(&m), "nothing taken with IFF1 clear");
    CHECK(!wake(&m), "nor does /INT wake a hart with IFF1 clear");
    sysw(&m, SYS_IFF, 3);
    CHECK(!take(&m), "not directly after EI");
    CHECK(m.cpu.exit_request, "but it asks to look again after one instruction");
    CHECK(wake(&m), "/INT wakes with IFF1 set");
    sysw(&m, SYS_IM, 1);
    const uint8_t r0 = rr(&m);
    CHECK(take(&m), "IM 1 taken");
    EQ64(m.cpu.pc, 0x0038, "RST 38h");
    EQ64(r16(&m, R_SP), 0x7FFE, "two bytes pushed");
    EQ64(mem(&m, 0x7FFE), 0x34, "the pc's low byte at SP");
    EQ64(mem(&m, 0x7FFF), 0x12, "its high byte above");
    EQ64(sysr(&m, SYS_IFF), 0, "IFF1 and IFF2 cleared");
    EQ64(n_inta, 1, "the acknowledge cycle runs in IM 1 too");
    EQ64(rr(&m), (uint8_t)((r0 + 1) & 0x7F), "R counts the acknowledge's fetch");
    CHECK(!take(&m), "not again with IFF1 clear");

    /* IM 2: I and the device's byte name the word holding the handler. */
    sysw(&m, SYS_IM, 2);
    sysw(&m, SYS_IFF, 3);
    take(&m);                                          /* the EI shadow */
    m.cpu.regfile[R_I] = 0x40;
    bus_bytes[0] = 0x16;
    poke(&m, 0x4016, 0x78);
    poke(&m, 0x4017, 0x56);
    m.cpu.pc = 0x2222;
    CHECK(take(&m), "IM 2 taken");
    EQ64(m.cpu.pc, 0x5678, "the handler from (I:vector)");
    EQ64(r16(&m, R_WZ), 0x5678, "MEMPTR is the handler");
    /* RETI returns, restores IFF1 from IFF2 and tells the board */
    const uint8_t reti_[] = { 0xED, 0x4D };
    code(&m, 0x5678, reti_, sizeof(reti_));
    step(&m, 1);
    EQ64(m.cpu.pc, 0x2222, "RETI returns");
    EQ64(n_reti, 1, "RETI reported");
    EQ64(sysr(&m, SYS_IFF), 0, "IFF1 = IFF2 (0)");
    uam_machine_fini(&m);
}

static void test_im0(void)
{
    uam_machine m;
    machine(&m, &uam_z80_nmos_ref);
    sysw(&m, SYS_IFF, 3);
    take(&m);
    line(&m, UAM_Z80_LINE_INT, 1);
    bus_bytes[0] = 0xD7;                              /* RST 10h */
    m.cpu.pc = 0x3000;
    CHECK(take(&m), "IM 0 taken");
    EQ64(m.cpu.pc, 0x0010, "IM 0 executes RST 10h");
    sysw(&m, SYS_IFF, 3);
    take(&m);
    bus_bytes[0] = 0xCD;                              /* CALL 0x4321 */
    bus_bytes[1] = 0x21;
    bus_bytes[2] = 0x43;
    n_inta = 0;
    CHECK(take(&m), "IM 0 CALL taken");
    EQ64(m.cpu.pc, 0x4321, "IM 0 executes CALL nn");
    EQ64(n_inta, 3, "three bytes acknowledged");
    EQ64(mem(&m, r16(&m, R_SP)), 0x10, "the return address pushed");
    uam_machine_fini(&m);
}

static void test_nmi_halt(void)
{
    uam_machine m;
    machine(&m, &uam_z80_nmos_ref);
    sysw(&m, SYS_IFF, 3);
    take(&m);
    m.cpu.pc = 0x1000;
    line(&m, UAM_Z80_LINE_NMI, 1);
    CHECK(take(&m), "NMI taken");
    EQ64(m.cpu.pc, 0x0066, "NMI vector");
    EQ64(sysr(&m, SYS_IFF), 2, "IFF1 cleared, IFF2 kept");
    CHECK(!take(&m), "an edge is one NMI");
    line(&m, UAM_Z80_LINE_NMI, 0);
    line(&m, UAM_Z80_LINE_NMI, 1);
    CHECK(take(&m), "a new edge is another");
    line(&m, UAM_Z80_LINE_NMI, 0);
    const uint8_t retn[] = { 0xED, 0x45 };
    code(&m, 0x0066, retn, sizeof(retn));
    step(&m, 1);
    EQ64(m.cpu.pc, 0x0066, "RETN returns to the first handler");
    EQ64(sysr(&m, SYS_IFF), 3, "RETN: IFF1 = IFF2");
    EQ64(n_reti, 0, "RETN is not RETI");
    /* HALT waits; the interrupt returns after it */
    const uint8_t halt[] = { 0x76, 0x00 };
    code(&m, 0x2000, halt, sizeof(halt));
    m.cpu.pc = 0x2000;
    sysw(&m, SYS_IM, 1);
    step(&m, 1);
    CHECK(m.cpu.waiting, "HALT waits");
    EQ64(m.cpu.pc, 0x2001, "at the instruction after HALT");
    CHECK(!wake(&m), "nothing wakes it yet");
    line(&m, UAM_Z80_LINE_INT, 1);
    CHECK(wake(&m), "/INT wakes it");
    CHECK(take(&m), "and is taken");
    CHECK(!m.cpu.waiting, "no longer waiting");
    EQ64(mem(&m, r16(&m, R_SP)), 0x01, "the return address is past HALT");
    uam_machine_fini(&m);
}

/* EI, then an instruction, then the interrupt: the shadow inside one block. */
static void test_ei_shadow(void)
{
    uam_machine m;
    machine(&m, &uam_z80_nmos_ref);
    const uint8_t prog[] = { 0xFB, 0x04, 0x04, 0x04, 0x18, 0xFE };   /* EI; INC B x3; JR $ */
    code(&m, 0x2000, prog, sizeof(prog));
    code(&m, 0x0038, (const uint8_t[]){ 0x18, 0xFE }, 2);
    sysw(&m, SYS_IM, 1);
    line(&m, UAM_Z80_LINE_INT, 1);
    m.cpu.pc = 0x2000;
    m.cpu.regfile[1] = 0;                           /* B */
    step(&m, 6);
    EQ64(m.cpu.regfile[1], 1, "one instruction ran after EI");
    EQ64(mem(&m, r16(&m, R_SP)), 0x02, "the interrupt returns to the second INC B");
    EQ64(m.exec.stats.interrupts, 1, "one interrupt");
    uam_machine_fini(&m);
}

static void test_r(void)
{
    uam_machine m;
    machine(&m, &uam_z80_nmos_ref);
    /* NOP; DD 00; CB 00; DD CB 00 06; ED 00; DD DD 00: 1 + 2 + 2 + 2 + 2 + 3 fetches */
    const uint8_t prog[] = { 0x00, 0xDD, 0x00, 0xCB, 0x00, 0xDD, 0xCB, 0x00, 0x06, 0xED, 0x00, 0xDD, 0xDD, 0x00 };
    code(&m, 0x2000, prog, sizeof(prog));
    m.cpu.pc = 0x2000;
    w16(&m, R_IX, 0x3000);
    m.cpu.regfile[R_RCNT] = 0x7E;
    m.cpu.regfile[R_R7] = 0x80;
    step(&m, 7);
    EQ64(m.cpu.pc, 0x2000 + sizeof(prog), "all ran");
    EQ64(rr(&m), 0x80 | ((0x7E + 12) & 0x7F), "R: 12 fetches, bit 7 kept");
    uam_machine_fini(&m);
}

/* One iteration of a repeating block instruction at `pc`; returns F. */
static uint8_t one(uam_machine *m, uint16_t pc, uint8_t op)
{
    const uint8_t insn[] = { 0xED, op };
    code(m, pc, insn, sizeof(insn));
    m->cpu.pc = pc;
    step(m, 1);
    return m->cpu.regfile[R_F];
}

static unsigned even(unsigned v) { return __builtin_popcount(v & 0xFF) % 2 == 0; }

static void test_repeat_flags(void)
{
    uam_machine m;
    machine(&m, &uam_z80_nmos_ref);
    /* LDIR with BC = 3 at 0x2800: bits 5 and 3 from 0x28, P/V set, back to itself */
    w16(&m, R_HL, 0x4000);
    w16(&m, R_DE, 0x5000);
    w16(&m, R_BC, 3);
    m.cpu.regfile[R_F] = 0x00;
    uint8_t f = one(&m, 0x2800, 0xB0);
    EQ64(m.cpu.pc, 0x2800, "LDIR repeats");
    EQ64(f & 0x2C, 0x2C, "LDIR repeating: Y and X from PC 0x28.., P/V");
    EQ64(r16(&m, R_WZ), 0x2801, "MEMPTR = pc + 1 while repeating");
    f = one(&m, 0x0000 | 0x0800, 0xB0);
    EQ64(f & 0x28, 0x08, "from PC 0x08..");
    /* CPIR repeating */
    w16(&m, R_HL, 0x4000);
    w16(&m, R_BC, 5);
    poke(&m, 0x4000, 0x10);
    m.cpu.regfile[R_A] = 0x20;
    f = one(&m, 0x2000, 0xB1);
    EQ64(m.cpu.pc, 0x2000, "CPIR repeats on no match");
    EQ64(f & 0x28, 0x20, "CPIR repeating: Y and X from PC 0x20..");
    /* INIR / OTIR repeating: the extra P/V and H terms */
    struct { uint8_t op, b, c, v, l; } cases[] = {
        { 0xB2, 0x05, 0xF7, 0x83, 0 },   /* carry, byte bit 7: B down */
        { 0xB2, 0x11, 0xF0, 0x42, 0 },   /* carry, bit 7 clear: B up */
        { 0xB2, 0x07, 0x01, 0x10, 0 },   /* no carry */
        { 0xB3, 0x21, 0x10, 0xF0, 0x40 },
        { 0xB3, 0x10, 0x10, 0x90, 0x80 },
        { 0xBA, 0x02, 0x02, 0xFF, 0 },
        { 0xBB, 0x40, 0x99, 0x01, 0x02 },
    };
    for (size_t i = 0; i < UAM_ARRAY_LEN(cases); i++) {
        const uint8_t op = cases[i].op, v = cases[i].v;
        const bool dec = op & 8, in = !(op & 1);
        w16(&m, R_BC, (uint16_t)(cases[i].b << 8 | cases[i].c));
        w16(&m, R_HL, (uint16_t)(0x4000 | cases[i].l));
        if (in)
            poke(&m, UAM_Z80_IO_BASE + (uint16_t)(cases[i].b << 8 | cases[i].c), v);
        else
            poke(&m, 0x4000 | cases[i].l, v);
        f = one(&m, 0x3800, op);
        const uint8_t b2 = (uint8_t)(cases[i].b - 1);
        const unsigned k = in ? v + (uint8_t)(cases[i].c + (dec ? -1 : 1)) : v + (uint8_t)(cases[i].l + (dec ? -1 : 1));
        const bool kc = k > 255, n = v & 0x80;
        unsigned pv = even((k & 7) ^ b2), h = kc;
        if (kc) {
            pv ^= !even((uint8_t)(n ? b2 - 1 : b2 + 1) & 7);
            h = n ? (b2 & 0xF) == 0 : (b2 & 0xF) == 0xF;
        } else {
            pv ^= !even(b2 & 7);
        }
        const unsigned want = (b2 & 0x80) | (b2 ? 0 : 0x40) | (0x38 & 0x28) | h << 4 | pv << 2 | (n ? 2 : 0) | kc;
        EQ64(m.cpu.pc, 0x3800, "block I/O repeats");
        CHECK(f == want, "case %zu (op %02x): F %02x want %02x", i, op, f, want);
        if (in)
            EQ64(mem(&m, 0x4000), v, "INIR stored the byte");
        else
            EQ64(mem(&m, UAM_Z80_IO_BASE + (uint16_t)(b2 << 8 | cases[i].c)), v, "OTIR sent it to port B-1:C");
    }
    uam_machine_fini(&m);
}

/* MEMPTR after the instructions that set it. */
static void test_memptr(void)
{
    static const struct { uint8_t insn[4]; unsigned len; uint16_t want; const char *what; } cases[] = {
        { { 0x3A, 0xFF, 0x12 }, 3, 0x1300, "LD A,(nn): nn + 1" },
        { { 0x32, 0xFF, 0x12 }, 3, 0x9900, "LD (nn),A: A:(nn + 1)" },
        { { 0x0A }, 1, 0x4568, "LD A,(BC): BC + 1" },
        { { 0x02 }, 1, 0x9968, "LD (BC),A: A:(C + 1)" },
        { { 0x2A, 0x00, 0x30 }, 3, 0x3001, "LD HL,(nn): nn + 1" },
        { { 0xED, 0x53, 0x00, 0x30 }, 4, 0x3001, "LD (nn),DE: nn + 1" },
        { { 0x09 }, 1, 0x7001, "ADD HL,BC: HL + 1" },
        { { 0xED, 0x42 }, 2, 0x7001, "SBC HL,BC: HL + 1" },
        { { 0xDB, 0x05 }, 2, 0x9906, "IN A,(n): A:n + 1" },
        { { 0xD3, 0xFF }, 2, 0x9900, "OUT (n),A: A:(n + 1)" },
        { { 0xED, 0x78 }, 2, 0x4568, "IN A,(C): BC + 1" },
        { { 0xED, 0x6F }, 2, 0x7001, "RLD: HL + 1" },
        { { 0xC3, 0x00, 0x30 }, 3, 0x3000, "JP nn" },
        { { 0xCA, 0x00, 0x31 }, 3, 0x3100, "JP Z,nn not taken" },
        { { 0xDD, 0x7E, 0xFE }, 3, 0x2FFE, "LD A,(IX-2): IX - 2" },
    };
    for (size_t i = 0; i < UAM_ARRAY_LEN(cases); i++) {
        uam_machine m;
        machine(&m, &uam_z80_nmos_ref);
        code(&m, 0x2000, cases[i].insn, cases[i].len);
        m.cpu.pc = 0x2000;
        m.cpu.regfile[R_A] = 0x99;
        m.cpu.regfile[R_F] = 0x00;                     /* NZ */
        w16(&m, R_BC, 0x4567);
        w16(&m, R_HL, 0x7000);
        w16(&m, R_IX, 0x3000);
        step(&m, 1);
        CHECK(r16(&m, R_WZ) == cases[i].want, "%s: MEMPTR %04x want %04x", cases[i].what,
              r16(&m, R_WZ), cases[i].want);
        uam_machine_fini(&m);
    }
}

/* The port address on the bus, and the board's decoding. */
static void test_ports(void)
{
    uam_machine m;
    machine(&m, &uam_z80_nmos_ref);
    const uint8_t prog[] = { 0xD3, 0x34, 0xED, 0x41 };   /* OUT (0x34),A; OUT (C),B */
    code(&m, 0x2000, prog, sizeof(prog));
    m.cpu.pc = 0x2000;
    m.cpu.regfile[R_A] = 0x12;
    w16(&m, R_BC, 0xAB07);
    step(&m, 2);
    EQ64(mem(&m, UAM_Z80_IO_BASE + 0x1234), 0x12, "OUT (n),A: A on the high byte");
    EQ64(mem(&m, UAM_Z80_IO_BASE + 0xAB07), 0xAB, "OUT (C),B: B on the high byte");
    uam_machine_fini(&m);
    machine(&m, &uam_z80_nmos_ref);
    uam_z80_set_io_decode(&m.cpu, 0x00FF);
    code(&m, 0x2000, prog, sizeof(prog));
    m.cpu.pc = 0x2000;
    m.cpu.regfile[R_A] = 0x12;
    w16(&m, R_BC, 0xAB07);
    step(&m, 2);
    EQ64(mem(&m, UAM_Z80_IO_BASE + 0x34), 0x12, "decoded to the low byte");
    EQ64(mem(&m, UAM_Z80_IO_BASE + 0x07), 0xAB, "OUT (C) decoded too");
    uam_machine_fini(&m);
}

static void test_gdb(void)
{
    uam_machine m;
    machine(&m, &uam_z80_nmos_ref);
    const uam_gdb_arch *g = m.cpu.isa->gdb;
    w16(&m, R_HL, 0xBEEF);
    m.cpu.regfile[R_I] = 0x12;
    m.cpu.regfile[R_RCNT] = 0x34;
    m.cpu.regfile[R_R7] = 0x80;
    uint64_t v = 0;
    CHECK(g->read_reg(&m.cpu, 3, &v) && v == 0xBEEF, "gdb hl");
    CHECK(g->read_reg(&m.cpu, 5, &v) && v == 0x1000, "gdb pc");
    CHECK(g->read_reg(&m.cpu, 12, &v) && v == 0x12B4, "gdb ir: I and R with bit 7");
    CHECK(g->write_reg(&m.cpu, 0, 0x1234), "gdb write af");
    EQ64(m.cpu.regfile[R_A], 0x12, "A from AF");
    CHECK(!g->read_reg(&m.cpu, 13, &v), "13 registers");
    uam_machine_fini(&m);
}

/* The CTC: timer counting and its line, the vector, acknowledge, RETI and priority. */
static void test_ctc(void)
{
    uam_machine m;
    CHECK(uam_machine_init(&m, &uam_isa_z80) == UAM_OK, "init");
    uam_device *t = uam_z80_ctc_create("ctc", &m.sched);
    uam_machine_add_device(&m, t);
    uam_machine_reset(&m, 0);
    const uam_device_ops *o = t->ops;
    uint64_t v;
    o->write(t, 0, 1, 0x48);                          /* vector 0x48 */
    o->write(t, 1, 1, 0x85);                          /* ch 1: IE, timer /16, TC follows */
    o->write(t, 1, 1, 2);                             /* 32 ticks */
    o->write(t, 2, 1, 0xC5);                          /* ch 2: IE, counter */
    o->write(t, 2, 1, 3);
    o->read(t, 1, 1, &v);
    EQ64(v, 2, "the timer reads its count");
    CHECK(!uam_z80_ctc_pending(t), "nothing pending yet");
    m.sched.idle = 31;
    uam_sched_run_due(&m.sched);
    CHECK(!uam_z80_ctc_pending(t), "not before 32 ticks");
    o->read(t, 1, 1, &v);
    EQ64(v, 1, "one count left");
    m.sched.idle = 32;
    uam_sched_run_due(&m.sched);
    CHECK(uam_z80_ctc_pending(t), "pending at 32 ticks");
    CHECK(uam_z80_ctc_irq(t)->level, "its line high");
    for (int i = 0; i < 3; i++)
        uam_z80_ctc_trigger(t, 2);
    EQ64(uam_z80_ctc_ack(t), 0x4A, "channel 1 (vector | 1 << 1) before channel 2");
    CHECK(!uam_z80_ctc_irq(t)->level, "channel 2 waits while 1 is in service");
    uam_z80_ctc_reti(t);
    CHECK(uam_z80_ctc_irq(t)->level, "RETI ends it: channel 2 asks");
    EQ64(uam_z80_ctc_ack(t), 0x4C, "channel 2's vector");
    m.sched.idle = 64;
    uam_sched_run_due(&m.sched);
    CHECK(uam_z80_ctc_irq(t)->level, "a higher channel interrupts one in service");
    EQ64(uam_z80_ctc_ack(t), 0x4A, "channel 1 again");
    o->write(t, 1, 1, 0x03);                          /* reset ch 1 */
    m.sched.idle = 200;
    uam_sched_run_due(&m.sched);
    uam_z80_ctc_reti(t);
    uam_z80_ctc_reti(t);
    CHECK(!uam_z80_ctc_pending(t), "a reset channel stops");
    uam_machine_fini(&m);
}

int main(void)
{
    test_reset_and_models();
    test_im1_im2();
    test_im0();
    test_nmi_halt();
    test_ei_shadow();
    test_r();
    test_repeat_flags();
    test_memptr();
    test_ports();
    test_gdb();
    test_ctc();
    printf("test_z80: %d/%d checks passed\n", n_checks - n_failed, n_checks);
    return n_failed ? 1 : 0;
}
