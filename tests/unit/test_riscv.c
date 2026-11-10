/*
 * test_riscv.c - The RV32IMAC module's parts the self-test firmware cannot reach on
 * rv32-ref: the reset state (and the interrupt line levels it keeps), which CSRs exist
 * and their WARL behaviour (mstatus, mie, mip, mtvec, mepc, misa, PMP read-as-zero, the
 * read-only space, no S-mode, debug or F CSRs), the 64-bit counters written in halves,
 * CSR instructions that write a read-only CSR by their rs1 field alone, trap entry and
 * MRET (mstatus.MIE/MPIE/MPP, the LR reservation dropped), precise mepc inside a block,
 * mtval of illegal compressed instructions, the atomics' access and alignment faults,
 * misaligned plain accesses, fetch faults at the end of memory (an instruction
 * straddling it), interrupts through the board's API (priority MEI > MSI > MTI, mie and
 * mstatus.MIE, direct and vectored mtvec, the lines reported), WFI waiting and waking,
 * the CLINT (msip, mtime, mtimecmp in virtual time, per hart) and the PLIC (priorities,
 * threshold, claim and complete, the level gateway) through their device ops, the
 * rv32-ref wiring with two harts, the debugger view, and a snapshot of the hart and
 * CLINT state.
 *
 * Each test runs on a bare machine: RAM at 0x80000000, mtvec at its start, each of its
 * 16 vector slots a jump to itself, so a trap leaves the hart spinning where it was
 * delivered. The tests that execute code run interpreted and under the JIT.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uam/boards.h"
#include "uam/bus.h"
#include "uam/dev/clint.h"
#include "uam/dev/plic.h"
#include "uam/isa/riscv.h"
#include "uam/machine.h"
#include "uam/sched.h"
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

#define RAM      0x80000000u
#define RAM_SIZE 0x00010000u
#define RAM_END  (RAM + RAM_SIZE)
#define TVEC     RAM             /* mtvec: 16 slots, each a jump to itself */
#define CODE     0x80001000u
#define DATA     0x80008000u
#define HOLE     0x40000000u     /* nothing mapped */
#define CLINT    0x02000000u

enum {
    CSR_FFLAGS = 0x001, CSR_SSTATUS = 0x100, CSR_SATP = 0x180,
    CSR_MSTATUS = 0x300, CSR_MISA = 0x301, CSR_MIE = 0x304, CSR_MTVEC = 0x305,
    CSR_MCOUNTEREN = 0x306, CSR_MCOUNTINHIBIT = 0x320, CSR_MSCRATCH = 0x340, CSR_MEPC = 0x341,
    CSR_MCAUSE = 0x342, CSR_MTVAL = 0x343, CSR_MIP = 0x344, CSR_PMPCFG0 = 0x3A0,
    CSR_PMPADDR0 = 0x3B0, CSR_PMPADDR63 = 0x3EF, CSR_DCSR = 0x7B0, CSR_CUSTOM = 0x7C0,
    CSR_MCYCLE = 0xB00, CSR_MINSTRET = 0xB02, CSR_MCYCLEH = 0xB80, CSR_MINSTRETH = 0xB82,
    CSR_CYCLE = 0xC00, CSR_INSTRET = 0xC02, CSR_CYCLEH = 0xC80, CSR_INSTRETH = 0xC82,
    CSR_MVENDORID = 0xF11, CSR_MARCHID = 0xF12, CSR_MIMPID = 0xF13, CSR_MHARTID = 0xF14,
    CSR_MCONFIGPTR = 0xF15,
};
#define MS_MIE  (1u << 3)
#define MS_MPIE (1u << 7)
#define MS_MPP  (3u << 11)
#define MISA_RV32IMAC 0x40001105u
#define IRQ(n)  (0x80000000u | (n))
#define MSI UAM_RV_IRQ_MSI
#define MTI UAM_RV_IRQ_MTI
#define MEI UAM_RV_IRQ_MEI

enum { ZERO = 0, T0 = 5, A0 = 10, A1 = 11, A2 = 12, A3 = 13 };

/* ---- instruction encodings ---------------------------------------------------------- */

static uint32_t I_(unsigned op, unsigned rd, unsigned f3, unsigned rs1, int imm)
{
    return op | rd << 7 | f3 << 12 | rs1 << 15 | ((uint32_t)imm & 0xFFF) << 20;
}
static uint32_t S_(unsigned f3, unsigned rs1, unsigned rs2, int imm)
{
    uint32_t u = (uint32_t)imm;
    return 0x23 | (u & 0x1F) << 7 | f3 << 12 | rs1 << 15 | rs2 << 20 | ((u >> 5) & 0x7F) << 25;
}
/* An A-extension .W operation: funct5, aq = rl = 0. */
static uint32_t AMO(unsigned f5, unsigned rd, unsigned rs1, unsigned rs2)
{
    return 0x2F | rd << 7 | 2u << 12 | rs1 << 15 | rs2 << 20 | f5 << 27;
}
static uint32_t ADDI(unsigned rd, unsigned rs1, int imm) { return I_(0x13, rd, 0, rs1, imm); }
static uint32_t LW(unsigned rd, unsigned rs1, int imm) { return I_(0x03, rd, 2, rs1, imm); }
static uint32_t SW(unsigned rs2, unsigned rs1, int imm) { return S_(2, rs1, rs2, imm); }
static uint32_t CSRRW(unsigned rd, unsigned csr, unsigned rs1) { return I_(0x73, rd, 1, rs1, (int)csr); }
static uint32_t CSRRS(unsigned rd, unsigned csr, unsigned rs1) { return I_(0x73, rd, 2, rs1, (int)csr); }
static uint32_t CSRRC(unsigned rd, unsigned csr, unsigned rs1) { return I_(0x73, rd, 3, rs1, (int)csr); }
static uint32_t CSRRSI(unsigned rd, unsigned csr, unsigned z) { return I_(0x73, rd, 6, z, (int)csr); }
static uint32_t CSRRCI(unsigned rd, unsigned csr, unsigned z) { return I_(0x73, rd, 7, z, (int)csr); }
static uint32_t LR_W(unsigned rd, unsigned rs1) { return AMO(0x02, rd, rs1, 0); }
static uint32_t SC_W(unsigned rd, unsigned rs2, unsigned rs1) { return AMO(0x03, rd, rs1, rs2); }
static uint32_t AMOSWAP_W(unsigned rd, unsigned rs2, unsigned rs1) { return AMO(0x01, rd, rs1, rs2); }
static uint32_t AMOADD_W(unsigned rd, unsigned rs2, unsigned rs1) { return AMO(0x00, rd, rs1, rs2); }
#define J_SELF   0x0000006Fu     /* jal x0, . */
#define ECALL    0x00000073u
#define MRET     0x30200073u
#define WFI      0x10500073u
#define CUSTOM0  0x0000000Bu     /* an opcode RV32IMAC does not have */
#define C_EBREAK 0x9002u
#define C_J_SELF 0xA001u         /* c.j . */
#define C_RSVD   0x0004u         /* c.addi4spn with nzuimm = 0: reserved */
#define C_FLW    0x6000u         /* c.flw fs0,0(s0): no F */
#define C_LUI_X0 0x6005u         /* c.lui zero,0x1: a HINT */
#define C_LUI_X0_0 0x6001u       /* c.lui zero,0: nzimm = 0, reserved */

/* ---- the machine ------------------------------------------------------------------- */

static void put32(uam_machine *m, uint32_t a, uint32_t w)
{
    const uint8_t b[4] = { (uint8_t)w, (uint8_t)(w >> 8), (uint8_t)(w >> 16), (uint8_t)(w >> 24) };
    CHECK(uam_bus_load_bytes(&m->bus, a, b, 4) == UAM_OK, "load at 0x%x", (unsigned)a);
}
static void put16(uam_machine *m, uint32_t a, uint16_t h)
{
    const uint8_t b[2] = { (uint8_t)h, (uint8_t)(h >> 8) };
    CHECK(uam_bus_load_bytes(&m->bus, a, b, 2) == UAM_OK, "load at 0x%x", (unsigned)a);
}
/* The words at a, then a jump to itself; returns the address of that jump. */
static uint32_t prog(uam_machine *m, uint32_t a, const uint32_t *w, size_t n)
{
    for (size_t i = 0; i < n; i++, a += 4)
        put32(m, a, w[i]);
    put32(m, a, J_SELF);
    return a;
}
static uint64_t mem(uam_machine *m, uint64_t a, unsigned size)
{
    uint64_t v = 0;
    uam_bus_read(&m->bus, a, size, UAM_ACC_LE, &v);
    return v;
}
static void poke(uam_machine *m, uint64_t a, unsigned size, uint64_t v)
{
    CHECK(uam_bus_write(&m->bus, a, size, UAM_ACC_LE, v) == UAM_OK, "write 0x%llx", (unsigned long long)a);
}
static uint32_t x(uam_cpu *c, unsigned r) { return (uint32_t)uam_cpu_reg_read(c, 4 * r, 32); }
static void setx(uam_cpu *c, unsigned r, uint32_t v) { uam_cpu_reg_write(c, 4 * r, 32, v); }
static void csrw(uam_cpu *c, uint32_t csr, uint32_t v)
{
    CHECK(c->isa->sysreg_write(c, csr, v) == UAM_OK, "write CSR 0x%x", (unsigned)csr);
}
static uint32_t csrr(uam_cpu *c, uint32_t csr)
{
    uint64_t v = 0;
    CHECK(c->isa->sysreg_read(c, csr, &v) == UAM_OK, "read CSR 0x%x", (unsigned)csr);
    return (uint32_t)v;
}
static void line(uam_machine *m, unsigned n, int level) { uam_cpu_irq_sink(&m->cpu, n, level); }
static bool take(uam_machine *m) { return m->cpu.isa->irq_take(&m->cpu); }
static bool wake(uam_machine *m) { return m->cpu.isa->irq_wakeup(&m->cpu); }
static uint64_t lines_taken(uam_machine *m) { return m->cpu.isa->irq_lines(&m->cpu); }
static void run(uam_machine *m, uint32_t pc, uint64_t n)
{
    m->cpu.pc = pc;
    uam_exec_run(&m->exec, n);
}
static void tick(uam_machine *m, uint64_t n)
{
    m->sched.idle += n;
    uam_sched_run_due(&m->sched);
}

/* A machine reset at CODE, mtvec = TVEC (direct), every slot of the table spinning. */
static void machine(uam_machine *m, bool jit)
{
    CHECK(uam_machine_init(m, &uam_isa_rv32) == UAM_OK, "machine init");
    m->board_name = "test-riscv";
    CHECK(uam_bus_map_ram(&m->bus, "ram", RAM, RAM_SIZE) == UAM_OK, "map RAM");
    if (jit)
        CHECK(uam_exec_enable_jit(&m->exec), "JIT");
    uam_machine_reset(m, CODE);
    for (unsigned k = 0; k < 16; k++)
        put32(m, TVEC + 4 * k, J_SELF);
    csrw(&m->cpu, CSR_MTVEC, TVEC);
}

/* The hart took a trap of `cause` (mepc, mtval) and sits at `at`. */
static void expect_trap(uam_machine *m, uint32_t at, uint32_t cause, uint32_t epc, uint32_t tval,
                        const char *what)
{
    uam_cpu *c = &m->cpu;
    CHECK(c->pc == at, "%s: at 0x%x (pc 0x%llx)", what, (unsigned)at, (unsigned long long)c->pc);
    CHECK(csrr(c, CSR_MCAUSE) == cause, "%s: mcause 0x%x want 0x%x", what, csrr(c, CSR_MCAUSE), (unsigned)cause);
    CHECK(csrr(c, CSR_MEPC) == epc, "%s: mepc 0x%x want 0x%x", what, csrr(c, CSR_MEPC), (unsigned)epc);
    CHECK(csrr(c, CSR_MTVAL) == tval, "%s: mtval 0x%x want 0x%x", what, csrr(c, CSR_MTVAL), (unsigned)tval);
}

/* ---- reset and CSRs ---------------------------------------------------------------- */

static void test_reset(void)
{
    uam_machine m;
    machine(&m, false);
    uam_cpu *c = &m.cpu;
    csrw(c, CSR_MSTATUS, MS_MIE | MS_MPIE);
    csrw(c, CSR_MIE, 0x888);
    csrw(c, CSR_MSCRATCH, 1);
    csrw(c, CSR_MEPC, 2);
    csrw(c, CSR_MCAUSE, 3);
    csrw(c, CSR_MTVAL, 4);
    csrw(c, CSR_MCOUNTEREN, 5);
    setx(c, A0, 0x1234);
    c->waiting = true;
    c->resv.valid = true;
    line(&m, MTI, 1);
    uam_machine_reset(&m, CODE + 0x100);
    EQ64(c->pc, CODE + 0x100, "reset: at the entry");
    EQ64(c->priv, 3, "reset: machine mode");
    EQ64(csrr(c, CSR_MSTATUS), MS_MPP, "reset: mstatus MPP = M, interrupts off");
    EQ64(csrr(c, CSR_MIE), 0, "reset: mie");
    EQ64(csrr(c, CSR_MTVEC), 0, "reset: mtvec");
    EQ64(csrr(c, CSR_MSCRATCH) | csrr(c, CSR_MEPC) | csrr(c, CSR_MCAUSE) | csrr(c, CSR_MTVAL), 0,
         "reset: mscratch, mepc, mcause, mtval");
    EQ64(csrr(c, CSR_MCOUNTEREN), 0, "reset: mcounteren");
    EQ64(x(c, A0), 0, "reset: registers cleared");
    CHECK(!c->waiting && !c->resv.valid, "reset: not waiting, no reservation");
    EQ64(csrr(c, CSR_MIP), 1u << MTI, "reset: a line held high through it stays pending");
    EQ64(csrr(c, CSR_MISA), MISA_RV32IMAC, "misa: RV32 with I, M, A, C and nothing else");
    uam_machine_fini(&m);
}

static void test_csrs(void)
{
    static const struct { uint16_t csr; bool rd, wr; const char *what; } exist[] = {
        { CSR_MVENDORID, true, false, "mvendorid" }, { CSR_MARCHID, true, false, "marchid" },
        { CSR_MIMPID, true, false, "mimpid" },       { CSR_MHARTID, true, false, "mhartid" },
        { CSR_MCONFIGPTR, true, false, "mconfigptr" },
        { CSR_CYCLE, true, false, "cycle" },         { CSR_INSTRETH, true, false, "instreth" },
        { CSR_MISA, true, true, "misa" },            { CSR_MCOUNTINHIBIT, true, true, "mcountinhibit" },
        { CSR_PMPCFG0, true, true, "pmpcfg0" },      { CSR_PMPADDR63, true, true, "pmpaddr63" },
        { CSR_PMPADDR63 + 1, false, false, "past pmpaddr63" },
        { CSR_SSTATUS, false, false, "sstatus (no S-mode)" }, { CSR_SATP, false, false, "satp" },
        { CSR_DCSR, false, false, "dcsr (no debug mode)" },   { CSR_FFLAGS, false, false, "fflags (no F)" },
        { CSR_CUSTOM, false, false, "a custom CSR" },
    };
    uam_machine m;
    machine(&m, false);
    uam_cpu *c = &m.cpu;
    for (size_t i = 0; i < UAM_ARRAY_LEN(exist); i++) {
        uint64_t v = 0;
        CHECK((c->isa->sysreg_read(c, exist[i].csr, &v) == UAM_OK) == exist[i].rd, "%s: read %s",
              exist[i].what, exist[i].rd ? "exists" : "refused");
        CHECK((c->isa->sysreg_write(c, exist[i].csr, 0) == UAM_OK) == exist[i].wr, "%s: write %s",
              exist[i].what, exist[i].wr ? "exists" : "refused");
    }
    EQ64(csrr(c, CSR_MVENDORID) | csrr(c, CSR_MARCHID) | csrr(c, CSR_MIMPID) | csrr(c, CSR_MCONFIGPTR), 0,
         "the identification CSRs read 0");
    /* WARL */
    csrw(c, CSR_MSTATUS, 0xFFFFFFFFu);
    EQ64(csrr(c, CSR_MSTATUS), MS_MIE | MS_MPIE | MS_MPP, "mstatus: only MIE and MPIE writable, MPP = M");
    csrw(c, CSR_MSTATUS, 0);
    EQ64(csrr(c, CSR_MSTATUS), MS_MPP, "mstatus: MPP stays M");
    csrw(c, CSR_MIE, 0xFFFFFFFFu);
    EQ64(csrr(c, CSR_MIE), 0x888, "mie: MSIE, MTIE, MEIE only");
    csrw(c, CSR_MIP, 0xFFFFFFFFu);
    EQ64(csrr(c, CSR_MIP), 0, "mip: not writable");
    line(&m, MSI, 1);
    line(&m, 5, 1);
    line(&m, 40, 1);
    EQ64(csrr(c, CSR_MIP), 1u << MSI, "mip: the lines of MSI, MTI, MEI only");
    line(&m, MSI, 0);
    EQ64(csrr(c, CSR_MIP), 0, "mip: a line falls");
    csrw(c, CSR_MISA, 0);
    EQ64(csrr(c, CSR_MISA), MISA_RV32IMAC, "misa: fixed");
    static const uint32_t tv[][2] = { { 0x80001001u, 0x80001001u }, { 0x80001002u, 0x80001000u },
                                      { 0x80001003u, 0x80001000u }, { 0x80001004u, 0x80001004u } };
    for (size_t i = 0; i < UAM_ARRAY_LEN(tv); i++) {
        csrw(c, CSR_MTVEC, tv[i][0]);
        CHECK(csrr(c, CSR_MTVEC) == tv[i][1], "mtvec 0x%x reads 0x%x want 0x%x (reserved modes direct)",
              (unsigned)tv[i][0], csrr(c, CSR_MTVEC), (unsigned)tv[i][1]);
    }
    csrw(c, CSR_MEPC, 0xFFFFFFFFu);
    EQ64(csrr(c, CSR_MEPC), 0xFFFFFFFEu, "mepc: bit 0 is zero (IALIGN 16)");
    csrw(c, CSR_MSCRATCH, 0xDEADBEEFu);
    csrw(c, CSR_MCAUSE, 0xC0FFEE11u);
    csrw(c, CSR_MTVAL, 0x13579BDFu);
    csrw(c, CSR_MCOUNTEREN, 5);
    EQ64(csrr(c, CSR_MSCRATCH), 0xDEADBEEFu, "mscratch");
    EQ64(csrr(c, CSR_MCAUSE), 0xC0FFEE11u, "mcause");
    EQ64(csrr(c, CSR_MTVAL), 0x13579BDFu, "mtval");
    EQ64(csrr(c, CSR_MCOUNTEREN), 5, "mcounteren");
    csrw(c, CSR_PMPCFG0, 0x1F);
    csrw(c, CSR_PMPADDR0, 0x20000000u);
    EQ64(csrr(c, CSR_PMPCFG0) | csrr(c, CSR_PMPADDR0), 0, "PMP: read as zero, writes ignored");
    /* the 64-bit counters in halves, counting on, carrying into the high half */
    prog(&m, CODE, NULL, 0);
    csrw(c, CSR_MCYCLEH, 5);
    csrw(c, CSR_MCYCLE, 0xFFFFFFFEu);
    EQ64(csrr(c, CSR_MCYCLE), 0xFFFFFFFEu, "mcycle written");
    EQ64(csrr(c, CSR_MCYCLEH), 5, "mcycleh kept by a write of mcycle");
    EQ64(csrr(c, CSR_CYCLE), 0xFFFFFFFEu, "cycle shadows mcycle");
    EQ64(csrr(c, CSR_CYCLEH), 5, "cycleh shadows mcycleh");
    run(&m, CODE, 3);
    EQ64(csrr(c, CSR_MCYCLE), 1, "mcycle counted 3");
    EQ64(csrr(c, CSR_MCYCLEH), 6, "the carry into mcycleh");
    csrw(c, CSR_MINSTRETH, 0);
    csrw(c, CSR_MINSTRET, 100);
    run(&m, CODE, 10);
    EQ64(csrr(c, CSR_MINSTRET), 110, "minstret counts instructions");
    EQ64(csrr(c, CSR_INSTRET), 110, "instret shadows it");
    EQ64(csrr(c, CSR_MINSTRETH), 0, "minstreth");
    /* two counters, each stopped by its own mcountinhibit bit (CY: bit 0, IR: bit 2) */
    csrw(c, CSR_MCYCLEH, 0);
    csrw(c, CSR_MCYCLE, 0x1000);
    EQ64(csrr(c, CSR_MINSTRET), 110, "writing mcycle leaves minstret");
    csrw(c, CSR_MCOUNTINHIBIT, 0xFFFFFFFFu);
    EQ64(csrr(c, CSR_MCOUNTINHIBIT), 5, "mcountinhibit: CY and IR (TM and the hpm bits read 0)");
    csrw(c, CSR_MCOUNTINHIBIT, 1);
    run(&m, CODE, 10);
    EQ64(csrr(c, CSR_MCYCLE), 0x1000, "CY stops mcycle");
    EQ64(csrr(c, CSR_CYCLE), 0x1000, "and cycle");
    EQ64(csrr(c, CSR_MINSTRET), 120, "not minstret");
    csrw(c, CSR_MCYCLE, 0x2000);
    EQ64(csrr(c, CSR_MCYCLE), 0x2000, "a stopped counter is writable");
    csrw(c, CSR_MCOUNTINHIBIT, 4);
    run(&m, CODE, 10);
    EQ64(csrr(c, CSR_MCYCLE), 0x200A, "mcycle counts on from the value written");
    EQ64(csrr(c, CSR_MINSTRET), 120, "IR stops minstret");
    EQ64(csrr(c, CSR_INSTRET), 120, "and instret");
    csrw(c, CSR_MCOUNTINHIBIT, 0);
    run(&m, CODE, 5);
    EQ64(csrr(c, CSR_MINSTRET), 125, "minstret counts again");
    EQ64(csrr(c, CSR_MCYCLE), 0x200F, "mcycle too");
    uam_machine_fini(&m);
}

/* CSR instructions: which ones write (rs1 / zimm field, not value), and the mtval of a refused one. */
static void test_csr_insns(bool jit)
{
    static const struct { bool ok; const char *what; } cases[] = {
        { true, "csrr mhartid" },
        { true, "csrrsi mhartid, 0: no write" },
        { true, "csrrc cycle, x0: no write" },
        { false, "csrrs mhartid, a1 with a1 = 0: a write" },
        { false, "csrrci mvendorid, 1" },
        { false, "csrw marchid" },
        { false, "csrr of a custom CSR" },
        { false, "csrr satp" },
        { true, "csrrw misa: WARL" },
    };
    const uint32_t insn[] = {
        CSRRS(A0, CSR_MHARTID, ZERO), CSRRSI(A0, CSR_MHARTID, 0), CSRRC(A0, CSR_CYCLE, ZERO),
        CSRRS(A0, CSR_MHARTID, A1), CSRRCI(A0, CSR_MVENDORID, 1), CSRRW(ZERO, CSR_MARCHID, A1),
        CSRRS(A0, CSR_CUSTOM, ZERO), CSRRS(A0, CSR_SATP, ZERO), CSRRW(A0, CSR_MISA, A1),
    };
    uam_machine m;
    machine(&m, jit);
    uam_cpu *c = &m.cpu;
    for (size_t i = 0; i < UAM_ARRAY_LEN(cases); i++) {
        const uint32_t at = CODE + 16 * (uint32_t)i;
        prog(&m, at, &insn[i], 1);
        setx(c, A0, 0x5555);
        setx(c, A1, 0);
        csrw(c, CSR_MCAUSE, 0x3F);
        run(&m, at, 8);
        if (cases[i].ok) {
            CHECK(c->pc == at + 4 && csrr(c, CSR_MCAUSE) == 0x3F, "%s: runs (pc 0x%llx)", cases[i].what,
                  (unsigned long long)c->pc);
        } else {
            expect_trap(&m, TVEC, 2, at, insn[i], cases[i].what);
            EQ64(x(c, A0), 0x5555, "a refused CSR instruction writes no register");
        }
    }
    uam_machine_fini(&m);
}

/* ---- traps ------------------------------------------------------------------------- */

static void test_traps(bool jit)
{
    uam_machine m;
    machine(&m, jit);
    uam_cpu *c = &m.cpu;
    /* entry: MIE into MPIE, MIE clear, MPP = M; an exception is no interrupt line */
    put32(&m, CODE, ECALL);
    csrw(c, CSR_MSTATUS, MS_MIE);
    csrw(c, CSR_MTVAL, 0x1234);
    run(&m, CODE, 4);
    expect_trap(&m, TVEC, 11, CODE, 0, "ecall");
    EQ64(csrr(c, CSR_MSTATUS), MS_MPIE | MS_MPP, "trap entry: MPIE = MIE, MIE clear, MPP = M");
    EQ64(lines_taken(&m), 0, "an exception reports no interrupt line");
    /* MRET: MIE = MPIE, MPIE set, to mepc */
    put32(&m, CODE + 0x100, MRET);
    put32(&m, CODE + 0x200, J_SELF);
    csrw(c, CSR_MEPC, CODE + 0x200);
    csrw(c, CSR_MSTATUS, MS_MPIE);
    run(&m, CODE + 0x100, 4);
    EQ64(c->pc, CODE + 0x200, "mret returns to mepc");
    EQ64(csrr(c, CSR_MSTATUS), MS_MIE | MS_MPIE | MS_MPP, "mret: MIE from MPIE, MPIE set");
    csrw(c, CSR_MSTATUS, 0);
    run(&m, CODE + 0x100, 4);
    EQ64(csrr(c, CSR_MSTATUS), MS_MPIE | MS_MPP, "mret with MPIE clear: MIE clear, MPIE set");
    /* mepc inside a block: the instruction before it committed */
    const uint32_t ill[] = { ADDI(A0, A0, 1), CUSTOM0 };
    prog(&m, CODE + 0x300, ill, 2);
    setx(c, A0, 41);
    run(&m, CODE + 0x300, 4);
    expect_trap(&m, TVEC, 2, CODE + 0x304, CUSTOM0, "illegal after an addi");
    EQ64(x(c, A0), 42, "the addi before it ran");
    /* c.ebreak: mtval the pc; illegal compressed: mtval the 16 bits alone */
    put16(&m, CODE + 0x400, C_EBREAK);
    run(&m, CODE + 0x400, 4);
    expect_trap(&m, TVEC, 3, CODE + 0x400, CODE + 0x400, "c.ebreak");
    put16(&m, CODE + 0x410, C_RSVD);
    put16(&m, CODE + 0x412, C_J_SELF);
    run(&m, CODE + 0x410, 4);
    expect_trap(&m, TVEC, 2, CODE + 0x410, C_RSVD, "reserved c.addi4spn");
    put16(&m, CODE + 0x420, C_FLW);
    put16(&m, CODE + 0x422, C_J_SELF);
    run(&m, CODE + 0x420, 4);
    expect_trap(&m, TVEC, 2, CODE + 0x420, C_FLW, "c.flw without F");
    /* c.lui x0, nzimm: a HINT, it does nothing; c.lui with nzimm = 0 is reserved */
    put16(&m, CODE + 0x430, C_LUI_X0);
    put16(&m, CODE + 0x432, C_J_SELF);
    csrw(c, CSR_MCAUSE, 0x55);
    run(&m, CODE + 0x430, 4);
    EQ64(c->pc, CODE + 0x432, "c.lui x0 is a HINT: it runs");
    EQ64(csrr(c, CSR_MCAUSE), 0x55, "and traps nothing");
    put16(&m, CODE + 0x440, C_LUI_X0_0);
    put16(&m, CODE + 0x442, C_J_SELF);
    run(&m, CODE + 0x440, 4);
    expect_trap(&m, TVEC, 2, CODE + 0x440, C_LUI_X0_0, "c.lui with nzimm = 0");
    /* a trap drops the LR reservation: lr; ecall (handler: mepc += 4; mret); sc fails */
    const uint32_t handler[] = { CSRRS(T0, CSR_MEPC, ZERO), ADDI(T0, T0, 4), CSRRW(ZERO, CSR_MEPC, T0), MRET };
    const uint32_t lrsc[] = { LR_W(A0, A1), SC_W(A2, A3, A1) };
    const uint32_t lrtsc[] = { LR_W(A0, A1), ECALL, SC_W(A2, A3, A1) };
    const uint32_t end1 = prog(&m, CODE + 0x500, lrsc, 2), end2 = prog(&m, CODE + 0x540, lrtsc, 3);
    for (size_t i = 0; i < UAM_ARRAY_LEN(handler); i++)
        put32(&m, TVEC + 0x40 + 4 * (uint32_t)i, handler[i]);
    csrw(c, CSR_MTVEC, TVEC + 0x40);
    setx(c, A1, DATA);
    setx(c, A3, 0x77);
    poke(&m, DATA, 4, 0x11);
    run(&m, CODE + 0x500, 6);
    CHECK(c->pc == end1 && x(c, A2) == 0 && mem(&m, DATA, 4) == 0x77, "lr; sc succeeds");
    poke(&m, DATA, 4, 0x11);
    run(&m, CODE + 0x540, 12);
    CHECK(c->pc == end2, "lr; ecall; sc ran to the end (pc 0x%llx)", (unsigned long long)c->pc);
    EQ64(x(c, A2), 1, "sc fails after a trap");
    EQ64(mem(&m, DATA, 4), 0x11, "and stores nothing");
    uam_machine_fini(&m);
}

static void test_faults(bool jit)
{
    static const struct { uint32_t addr, cause; const char *what; } amo[] = {
        { HOLE, 7, "amoswap.w on nothing: store/AMO access fault" },
        { HOLE + 8, 7, "amoadd.w on nothing" },
        { HOLE + 4, 5, "lr.w on nothing: load access fault" },
        { DATA + 2, 6, "sc.w misaligned: store/AMO address misaligned" },
        { DATA + 1, 6, "amoswap.w misaligned" },
    };
    const uint32_t insn[] = { AMOSWAP_W(A0, A2, A1), AMOADD_W(A0, A2, A1), LR_W(A0, A1), SC_W(A0, A2, A1),
                              AMOSWAP_W(A0, A2, A1) };
    uam_machine m;
    machine(&m, jit);
    uam_cpu *c = &m.cpu;
    for (size_t i = 0; i < UAM_ARRAY_LEN(amo); i++) {
        const uint32_t at = CODE + 16 * (uint32_t)i;
        prog(&m, at, &insn[i], 1);
        setx(c, A0, 0xA0);
        setx(c, A1, amo[i].addr);
        run(&m, at, 4);
        expect_trap(&m, TVEC, amo[i].cause, at, amo[i].addr, amo[i].what);
        EQ64(x(c, A0), 0xA0, "a faulting atomic writes no register");
    }
    /* plain accesses may be misaligned: done, little-endian */
    const uint32_t mis[] = { LW(A0, A1, 1), SW(A2, A1, 0x11) };
    const uint32_t end = prog(&m, CODE + 0x100, mis, 2);
    for (unsigned i = 0; i < 8; i++)
        poke(&m, DATA + i, 1, 0x10 + i);
    setx(c, A1, DATA);
    setx(c, A2, 0xAABBCCDDu);
    run(&m, CODE + 0x100, 4);
    EQ64(c->pc, end, "misaligned lw and sw raise nothing");
    EQ64(x(c, A0), 0x14131211u, "misaligned lw");
    EQ64(mem(&m, DATA + 0x11, 1) | mem(&m, DATA + 0x12, 1) << 8 | mem(&m, DATA + 0x13, 2) << 16, 0xAABBCCDDu,
         "misaligned sw");
    /* a 32-bit instruction straddling the end of memory: mepc its start, mtval the missing half */
    put16(&m, RAM_END - 2, (uint16_t)ADDI(A0, A0, 1));
    run(&m, RAM_END - 2, 4);
    expect_trap(&m, TVEC, 1, RAM_END - 2, RAM_END, "fetch across the end of memory");
    /* running off the end: the instruction before it committed */
    put32(&m, RAM_END - 4, ADDI(A0, ZERO, 7));
    run(&m, RAM_END - 4, 4);
    expect_trap(&m, TVEC, 1, RAM_END, RAM_END, "fetch past the end of memory");
    EQ64(x(c, A0), 7, "the last instruction in memory ran");
    uam_machine_fini(&m);
}

/* ---- interrupts -------------------------------------------------------------------- */

static void test_irq(void)
{
    uam_machine m;
    machine(&m, false);
    uam_cpu *c = &m.cpu;
    c->pc = CODE + 0x20;
    line(&m, MTI, 1);
    CHECK(!take(&m) && !wake(&m), "MTIP with mie clear: neither taken nor waking");
    csrw(c, CSR_MIE, 1u << MTI);
    CHECK(wake(&m), "enabled in mie: it wakes WFI");
    CHECK(!take(&m), "but with mstatus.MIE clear it is not taken");
    csrw(c, CSR_MSTATUS, MS_MIE);
    csrw(c, CSR_MTVAL, 0x99);
    CHECK(take(&m), "taken with mstatus.MIE");
    expect_trap(&m, TVEC, IRQ(MTI), CODE + 0x20, 0, "timer interrupt, direct mtvec");
    EQ64(lines_taken(&m), 1u << MTI, "the timer line reported");
    EQ64(csrr(c, CSR_MSTATUS), MS_MPIE | MS_MPP, "interrupt entry: MPIE = MIE, MIE clear");
    CHECK(!take(&m), "not again with MIE clear");
    CHECK(!c->resv.valid, "no reservation after an interrupt");

    /* priority MEI > MSI > MTI, each masked by its mie bit */
    line(&m, MSI, 1);
    line(&m, MEI, 1);
    csrw(c, CSR_MIE, 0x888);
    static const unsigned order[] = { MEI, MSI, MTI };
    for (size_t i = 0; i < UAM_ARRAY_LEN(order); i++) {
        csrw(c, CSR_MSTATUS, MS_MIE);
        CHECK(take(&m), "pending: taken");
        CHECK(csrr(c, CSR_MCAUSE) == IRQ(order[i]), "priority %zu: mcause 0x%x want 0x%x", i,
              csrr(c, CSR_MCAUSE), (unsigned)IRQ(order[i]));
        EQ64(lines_taken(&m), 1u << order[i], "the line reported");
        line(&m, order[i], 0);
    }
    line(&m, MSI, 1);
    line(&m, MEI, 1);
    line(&m, MTI, 1);
    csrw(c, CSR_MIE, 1u << MSI | 1u << MTI);
    csrw(c, CSR_MSTATUS, MS_MIE);
    CHECK(take(&m) && csrr(c, CSR_MCAUSE) == IRQ(MSI), "MEIE clear: MSI first");
    csrw(c, CSR_MIE, 1u << MTI);
    csrw(c, CSR_MSTATUS, MS_MIE);
    CHECK(take(&m) && csrr(c, CSR_MCAUSE) == IRQ(MTI), "MTI alone enabled");
    line(&m, MTI, 0);
    CHECK(!take(&m) && !wake(&m), "a line that fell is gone");

    /* vectored: base + 4 * cause; a reserved mode is direct */
    csrw(c, CSR_MIE, 0x888);
    csrw(c, CSR_MTVEC, TVEC | 1);
    line(&m, MSI, 0);
    static const unsigned vec[] = { MEI, MSI, MTI };
    for (size_t i = 0; i < UAM_ARRAY_LEN(vec); i++) {
        line(&m, vec[i], 1);
        csrw(c, CSR_MSTATUS, MS_MIE);
        c->pc = CODE;
        CHECK(take(&m) && c->pc == TVEC + 4 * vec[i], "vectored: line %u at base + %u (pc 0x%llx)", vec[i],
              4 * vec[i], (unsigned long long)c->pc);
        EQ64(csrr(c, CSR_MEPC), CODE, "vectored: mepc");
        line(&m, vec[i], 0);
    }
    csrw(c, CSR_MTVEC, TVEC | 3);
    line(&m, MEI, 1);
    csrw(c, CSR_MSTATUS, MS_MIE);
    CHECK(take(&m) && c->pc == TVEC, "mtvec mode 3 is direct");
    uam_machine_fini(&m);
}

/* WFI waits until an enabled interrupt is pending; taken, it returns after the WFI. */
static void test_wfi(bool jit)
{
    uam_machine m;
    machine(&m, jit);
    uam_cpu *c = &m.cpu;
    const uint32_t w[] = { WFI };
    prog(&m, CODE, w, 1);
    csrw(c, CSR_MIE, 1u << MTI);
    csrw(c, CSR_MSTATUS, MS_MIE);
    run(&m, CODE, 20);
    CHECK(c->waiting, "WFI waits");
    EQ64(c->pc, CODE + 4, "after the WFI");
    line(&m, MSI, 1);
    CHECK(!wake(&m), "a line not enabled in mie does not wake it");
    uam_exec_run(&m.exec, 20);
    CHECK(c->waiting, "still waiting");
    line(&m, MTI, 1);
    CHECK(wake(&m), "an enabled one does");
    uam_exec_run(&m.exec, 20);
    CHECK(!c->waiting, "no longer waiting");
    expect_trap(&m, TVEC, IRQ(MTI), CODE + 4, 0, "the interrupt that woke WFI");
    uam_machine_fini(&m);
}

/* ---- CLINT and PLIC ---------------------------------------------------------------- */

static uam_device *add_clint(uam_machine *m, unsigned harts)
{
    uam_device *d = uam_clint_create_harts("clint", &m->sched, harts);
    CHECK(d != NULL, "CLINT");
    uam_machine_add_device(m, d);
    CHECK(uam_bus_map_mmio(&m->bus, "clint", CLINT, UAM_CLINT_SIZE, d) == UAM_OK, "map CLINT");
    uam_signal_connect(uam_clint_msip_line(d), uam_cpu_irq_sink, &m->cpu, MSI);
    uam_signal_connect(uam_clint_mtip_line(d), uam_cpu_irq_sink, &m->cpu, MTI);
    return d;
}

static uint64_t dev_rd(uam_device *d, uint64_t off, unsigned size)
{
    uint64_t v = 0xBAD;
    CHECK(d->ops->read(d, off, size, &v) == UAM_OK, "read 0x%llx", (unsigned long long)off);
    return v;
}
static void dev_wr(uam_device *d, uint64_t off, unsigned size, uint64_t v) { d->ops->write(d, off, size, v); }

/* MRET sets MIE again while an interrupt is pending: it is taken at once, also by
 * native code that has linked the MRET's exit to the interrupted loop. The handler
 * raises the software interrupt again on every other entry, so some returns find
 * nothing pending (they link) and the others must not run on into the loop. */
static void mret_run(int mode, uint32_t *count, uint32_t *entries, uint64_t *icount)
{
    uam_machine m;
    machine(&m, mode != 0);
    m.exec.chain = mode == 2;
    uam_cpu *c = &m.cpu;
    add_clint(&m, 1);
    const uint32_t w[] = {
        0x02000437u,   /*          lui  s0, 0x2000     the CLINT's msip */
        0x00100913u,   /*          addi s2, zero, 1 */
        0x00800293u,   /*          addi t0, zero, 8 */
        0x30429073u,   /*          csrw mie, t0        MSIE */
        0x30046073u,   /*          csrsi mstatus, 8    MIE */
        0x00198993u,   /* loop:    addi s3, s3, 1 */
        0x3ff9f293u,   /*          andi t0, s3, 1023 */
        0xfe029ce3u,   /*          bnez t0, loop */
        0x01242023u,   /*          sw   s2, 0(s0)      every 1024 rounds: raise it */
        0xff1ff06fu,   /*          j    loop */
        0x001a0a13u,   /* handler: addi s4, s4, 1 */
        0x00042023u,   /*          sw   zero, 0(s0)    lower it */
        0x001a7313u,   /*          andi t1, s4, 1 */
        0x00030463u,   /*          beqz t1, 1f */
        0x01242023u,   /*          sw   s2, 0(s0)      odd entries: raise it again */
        MRET,          /* 1: */
    };
    prog(&m, CODE, w, sizeof(w) / sizeof(w[0]));
    csrw(c, CSR_MTVEC, CODE + 4 * 10);
    for (unsigned r = 19; r <= 20; r++)
        setx(c, r, 0);
    run(&m, CODE, 40000);
    *count = x(c, 19);
    *entries = x(c, 20);
    *icount = c->icount;
    uam_machine_fini(&m);
}

static void test_mret_pending(void)
{
    uint32_t count[3], entries[3];
    uint64_t icount[3];
    for (int mode = 0; mode < 3; mode++)   /* interpreter, native, native and chained */
        mret_run(mode, &count[mode], &entries[mode], &icount[mode]);
    CHECK(entries[0] >= 20, "the handler ran (%u entries)", entries[0]);
    for (int mode = 1; mode < 3; mode++)
        CHECK(count[mode] == count[0] && entries[mode] == entries[0] && icount[mode] == icount[0],
              "MRET with an interrupt pending, %s: %u rounds, %u entries (interpreter %u, %u)",
              mode == 1 ? "native" : "chained", count[mode], entries[mode], count[0], entries[0]);
}

static void test_clint(void)
{
    uam_machine m;
    CHECK(uam_machine_init(&m, &uam_isa_rv32) == UAM_OK, "init");
    uam_device *d = uam_clint_create_harts("clint", &m.sched, 2);
    uam_machine_add_device(&m, d);
    uam_machine_reset(&m, 0);
    uam_signal *msip0 = uam_clint_hart_msip_line(d, 0), *msip1 = uam_clint_hart_msip_line(d, 1);
    uam_signal *mtip0 = uam_clint_hart_mtip_line(d, 0), *mtip1 = uam_clint_hart_mtip_line(d, 1);
    CHECK(msip1 && mtip1 && !uam_clint_hart_msip_line(d, 2), "two harts' lines");
    CHECK(uam_clint_create_harts("x", &m.sched, 0) == NULL, "a CLINT needs a hart");
    EQ64(dev_rd(d, 0x4000, 4) & dev_rd(d, 0x400C, 4), 0xFFFFFFFFu, "mtimecmp all ones after reset");
    dev_wr(d, 4, 4, 0xFF);
    CHECK(dev_rd(d, 4, 4) == 1 && msip1->level && !msip0->level, "hart 1's msip: bit 0, its line only");
    dev_wr(d, 4, 4, 2);
    CHECK(dev_rd(d, 4, 4) == 0 && !msip1->level, "msip: bit 1 is not msip");
    EQ64(dev_rd(d, 8, 4), 0, "no third hart's msip");
    tick(&m, 100);
    EQ64(dev_rd(d, 0xBFF8, 8), 100, "mtime is virtual time");
    dev_wr(d, 0xBFF8, 4, 1000);
    tick(&m, 5);
    EQ64(dev_rd(d, 0xBFF8, 4) | dev_rd(d, 0xBFFC, 4) << 32, 1005, "mtime written, counting on");
    dev_wr(d, 0x4004, 4, 0);
    dev_wr(d, 0x4000, 4, 1010);
    tick(&m, 4);
    CHECK(!mtip0->level, "mtip low before mtimecmp");
    tick(&m, 1);
    CHECK(mtip0->level && !mtip1->level, "hart 0's mtip at mtimecmp, hart 1's not");
    dev_wr(d, 0xBFF8, 8, 0);
    CHECK(!mtip0->level, "mtime written below mtimecmp: low again");
    tick(&m, 1009);
    CHECK(!mtip0->level, "not yet");
    tick(&m, 1);
    CHECK(mtip0->level, "and at the match again");
    dev_wr(d, 0x4008, 8, 5);
    CHECK(mtip1->level, "hart 1's mtimecmp below mtime: high at once");
    dev_wr(d, 0x400C, 4, 0xFFFFFFFFu);
    CHECK(!mtip1->level, "its high half set: low");
    uam_machine_reset(&m, 0);
    CHECK(!mtip0->level && !mtip1->level && dev_rd(d, 0x4004, 4) == 0xFFFFFFFFu, "reset disarms the timers");
    EQ64(dev_rd(d, 0xBFF8, 8), uam_sched_now(&m.sched), "reset: mtime is virtual time again");
    uam_machine_fini(&m);
}

static void test_plic(void)
{
    uam_machine m;
    CHECK(uam_machine_init(&m, &uam_isa_rv32) == UAM_OK, "init");
    CHECK(uam_plic_create("x", 1, 1) == NULL && uam_plic_create("x", 8, 0) == NULL, "bad PLIC shapes refused");
    uam_device *p = uam_plic_create("plic", 40, 2);
    uam_machine_add_device(&m, p);
    uam_machine_reset(&m, 0);
    uam_signal *c0 = uam_plic_context_line(p, 0), *c1 = uam_plic_context_line(p, 1);
    CHECK(c0 && c1 && !uam_plic_context_line(p, 2), "two contexts");
    dev_wr(p, 0, 4, 7);
    EQ64(dev_rd(p, 0, 4), 0, "source 0 has no priority");
    dev_wr(p, 4 * 5, 4, 0xFF);
    EQ64(dev_rd(p, 4 * 5, 4), 7, "priority: 3 bits");
    dev_wr(p, 0x2000, 4, 0xFFFFFFFFu);
    EQ64(dev_rd(p, 0x2000, 4), 0xFFFFFFFEu, "source 0 cannot be enabled");
    dev_wr(p, 0x200000, 4, 0xF);
    EQ64(dev_rd(p, 0x200000, 4), 7, "threshold: 3 bits");
    dev_wr(p, 0x200000, 4, 0);
    dev_wr(p, 0x1000, 4, 0xFFFFFFFFu);
    EQ64(dev_rd(p, 0x1000, 4), 0, "pending is read-only");

    dev_wr(p, 4 * 5, 4, 2);
    dev_wr(p, 4 * 33, 4, 2);
    dev_wr(p, 4 * 7, 4, 1);
    dev_wr(p, 4 * 34, 4, 1);
    dev_wr(p, 0x2000, 4, 1u << 5 | 1u << 7 | 1u << 9);
    dev_wr(p, 0x2004, 4, 1u << 1 | 1u << 2);                     /* 33, 34 */
    dev_wr(p, 0x2080, 4, 1u << 7);                               /* context 1: source 7 */
    uam_plic_input(p, 9, 1);
    CHECK(dev_rd(p, 0x1000, 4) == 1u << 9 && !c0->level, "priority 0: pending, never an interrupt");
    uam_plic_input(p, 7, 1);
    CHECK(c0->level && c1->level, "source 7 reaches both contexts");
    dev_wr(p, 0x201000, 4, 1);
    CHECK(!c1->level && c0->level, "context 1's threshold 1 masks priority 1");
    uam_plic_input(p, 33, 1);
    uam_plic_input(p, 5, 1);
    EQ64(dev_rd(p, 0x1004, 4), 1u << 1, "source 33 pending in the second word");
    EQ64(dev_rd(p, 0x200004, 4), 5, "claim: highest priority, the lower id of a tie");
    EQ64(dev_rd(p, 0x200004, 4), 33, "then the other");
    EQ64(dev_rd(p, 0x200004, 4), 7, "then priority 1");
    CHECK(dev_rd(p, 0x200004, 4) == 0 && !c0->level, "then nothing (9 has priority 0)");
    EQ64(dev_rd(p, 0x1000, 4), 1u << 9, "claimed sources are no longer pending");
    /* the level gateway */
    uam_plic_input(p, 5, 1);
    EQ64(dev_rd(p, 0x1000, 4) >> 5 & 1, 0, "in service: not pending again");
    dev_wr(p, 0x200004, 4, 5);
    CHECK((dev_rd(p, 0x1000, 4) >> 5 & 1) && c0->level, "completed while high: pending again");
    EQ64(dev_rd(p, 0x200004, 4), 5, "and claimed again");
    uam_plic_input(p, 5, 0);
    dev_wr(p, 0x200004, 4, 5);
    EQ64(dev_rd(p, 0x1000, 4) >> 5 & 1, 0, "completed while low: done");
    uam_plic_input(p, 34, 1);
    uam_plic_input(p, 34, 0);
    EQ64(dev_rd(p, 0x200004, 4), 34, "a request that fell before its claim stays pending");
    /* a complete naming a source the context does not enable is ignored */
    dev_wr(p, 0x201004, 4, 34);                                  /* context 1 never enabled 34 */
    uam_plic_input(p, 34, 1);
    EQ64(dev_rd(p, 0x1004, 4) >> 2 & 1, 0, "complete from a context without the source: still in service");
    dev_wr(p, 0x2004, 4, 1u << 1);                               /* context 0 disables 34 */
    dev_wr(p, 0x200004, 4, 34);
    EQ64(dev_rd(p, 0x1004, 4) >> 2 & 1, 0, "nor from one that no longer enables it");
    dev_wr(p, 0x2004, 4, 1u << 1 | 1u << 2);
    dev_wr(p, 0x200004, 4, 34);
    EQ64(dev_rd(p, 0x1004, 4) >> 2 & 1, 1, "the context that enables it completes it (high: pending again)");
    uam_plic_input(p, 34, 0);
    EQ64(dev_rd(p, 0x200004, 4), 34, "and it is claimed again");
    dev_wr(p, 0x200004, 4, 40);                                  /* out of range: ignored */
    EQ64(dev_rd(p, 0x202000, 4), 0, "no third context");
    /* reset: priorities and enables cleared, a still-high input stays pending */
    uam_machine_reset(&m, 0);
    CHECK(dev_rd(p, 4 * 7, 4) == 0 && dev_rd(p, 0x2000, 4) == 0 && dev_rd(p, 0x201000, 4) == 0,
          "reset clears priorities, enables, thresholds");
    EQ64(dev_rd(p, 0x1000, 4), 1u << 7 | 1u << 9, "reset: the high inputs (7, 9) pending");
    CHECK(!c0->level && !c1->level, "reset: no context asks");
    uam_machine_fini(&m);
}

/* ---- rv32-ref with two harts --------------------------------------------------------- */

static void test_board(void)
{
    const uam_board_option opt[] = { { "cpus", "2" } };
    const uam_board_config cfg = { .ram_size = 1u << 20, .options = opt, .n_options = 1 };
    uam_machine m;
    CHECK(uam_board_rv32_ref.build(&m, &cfg) == UAM_OK, "build rv32-ref, cpus=2");
    uam_cpu *h0 = &m.cpu, *h1 = uam_machine_cpu(&m, 1);
    CHECK(h1 && !uam_machine_cpu(&m, 2), "two harts");
    if (!h1)
        return;
    CHECK(h0->pc == RAM && h1->pc == RAM, "both at the reset address");
    CHECK(csrr(h0, CSR_MHARTID) == 0 && csrr(h1, CSR_MHARTID) == 1, "mhartid 0 and 1");
    csrw(h1, CSR_MSCRATCH, 0x11);
    EQ64(csrr(h0, CSR_MSCRATCH), 0, "each hart its own CSRs");
    poke(&m, CLINT + 4, 4, 1);
    CHECK(csrr(h1, CSR_MIP) == 1u << MSI && csrr(h0, CSR_MIP) == 0, "CLINT msip 1: hart 1's MSIP");
    poke(&m, CLINT + 4, 4, 0);
    poke(&m, CLINT + 0x400C, 4, 0);
    poke(&m, CLINT + 0x4008, 4, 0);
    CHECK(csrr(h1, CSR_MIP) == 1u << MTI && csrr(h0, CSR_MIP) == 0, "CLINT mtimecmp 1: hart 1's MTIP");
    poke(&m, CLINT + 0x400C, 4, 0xFFFFFFFFu);
    /* the UART (source 10) through the PLIC: context 2h is hart h's, 2h + 1 nobody's */
    const uint32_t plic = 0x0C000000u;
    poke(&m, plic + 4 * 10, 4, 1);
    poke(&m, 0x10000001u, 1, 0x02);                              /* UART IER: THR empty */
    CHECK(mem(&m, plic + 0x1000, 4) >> 10 & 1, "the UART pends source 10");
    poke(&m, plic + 0x2080, 4, 1u << 10);
    CHECK(csrr(h0, CSR_MIP) == 0 && csrr(h1, CSR_MIP) == 0, "context 1 reaches no hart");
    poke(&m, plic + 0x2100, 4, 1u << 10);
    CHECK(csrr(h1, CSR_MIP) == 1u << MEI && csrr(h0, CSR_MIP) == 0, "context 2: hart 1's MEIP");
    poke(&m, plic + 0x2000, 4, 1u << 10);
    EQ64(csrr(h0, CSR_MIP), 1u << MEI, "context 0: hart 0's MEIP");
    uam_machine_fini(&m);
}

/* ---- the debugger view and the dump -------------------------------------------------- */

static void test_gdb(void)
{
    uam_machine m;
    machine(&m, false);
    uam_cpu *c = &m.cpu;
    const uam_gdb_arch *g = c->isa->gdb;
    CHECK(g && g->n_regs == 33 && g->reg_bytes == 4, "33 registers of 4 bytes");
    CHECK(strstr(g->target_xml, "<architecture>riscv:rv32</architecture>")
          && strstr(g->target_xml, "org.gnu.gdb.riscv.cpu")
          && strstr(g->target_xml, "name=\"pc\" bitsize=\"32\" regnum=\"32\""), "target description");
    uint64_t v = 0;
    setx(c, A0, 0xBEEF);
    CHECK(g->read_reg(c, 10, &v) && v == 0xBEEF, "gdb a0");
    CHECK(g->read_reg(c, 32, &v) && v == CODE, "gdb pc");
    CHECK(g->write_reg(c, 0, 0x1234) && g->read_reg(c, 0, &v) && v == 0, "gdb: x0 stays 0");
    CHECK(g->write_reg(c, 11, 0xCAFE) && x(c, A1) == 0xCAFE, "gdb write a1");
    CHECK(g->write_reg(c, 32, 0x180002000ull) && c->pc == 0x80002000u, "gdb pc: 32 bits");
    CHECK(!g->read_reg(c, 33, &v) && !g->write_reg(c, 33, 0), "no register 33");
    csrw(c, CSR_MCAUSE, IRQ(MEI));
    csrw(c, CSR_MTVAL, 0x7777);
    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    c->isa->dump_state(c, f);
    fclose(f);
    CHECK(buf && strstr(buf, "pc=80002000") && strstr(buf, "a0  =0000beef") && strstr(buf, "mcause=8000000b")
          && strstr(buf, "mtval=00007777") && strstr(buf, "mtvec=80000000"), "dump: %s", buf ? buf : "");
    free(buf);
    uam_machine_fini(&m);
}

/* ---- snapshots ----------------------------------------------------------------------- */

static void test_snapshot(void)
{
    static const uint32_t st[][3] = {   /* CSR, written, read back */
        { CSR_MSTATUS, MS_MPIE, MS_MPIE | MS_MPP }, { CSR_MIE, 0x888, 0x888 }, { CSR_MTVEC, TVEC | 1, TVEC | 1 },
        { CSR_MSCRATCH, 0x5C, 0x5C }, { CSR_MEPC, 0x80001234u, 0x80001234u }, { CSR_MCAUSE, 2, 2 },
        { CSR_MTVAL, 0x7777, 0x7777 }, { CSR_MCOUNTEREN, 3, 3 }, { CSR_MCYCLEH, 0x12, 0x12 },
        { CSR_MCYCLE, 0x3456789Au, 0x3456789Au }, { CSR_MCOUNTINHIBIT, 4, 4 },
        { CSR_MINSTRET, 0x777, 0x777 },
    };
    uam_machine m;
    machine(&m, false);
    uam_cpu *c = &m.cpu;
    add_clint(&m, 1);
    for (size_t i = 0; i < UAM_ARRAY_LEN(st); i++)
        csrw(c, st[i][0], st[i][1]);
    setx(c, A0, 0xA0A0);
    line(&m, MEI, 1);
    poke(&m, CLINT, 4, 1);                                       /* MSI from the CLINT */
    poke(&m, CLINT + 0x4004, 4, 0);
    poke(&m, CLINT + 0x4000, 4, 300);
    uam_snapshot *s = NULL;
    CHECK(uam_snapshot_take(&m, &s) == UAM_OK && s, "take");
    if (!s)
        return;
    uam_machine_reset(&m, 0);                                    /* the hart's and the CLINT's state gone */
    line(&m, MEI, 0);
    CHECK(uam_snapshot_restore(&m, s) == UAM_OK, "restore");
    for (size_t i = 0; i < UAM_ARRAY_LEN(st); i++)
        CHECK(csrr(c, st[i][0]) == st[i][2], "restored CSR 0x%x: 0x%x want 0x%x", (unsigned)st[i][0],
              csrr(c, st[i][0]), (unsigned)st[i][2]);
    EQ64(x(c, A0), 0xA0A0, "a0");
    EQ64(c->pc, CODE, "pc");
    EQ64(csrr(c, CSR_MIP), 1u << MEI | 1u << MSI, "mip: the line levels");
    tick(&m, 299 - uam_sched_now(&m.sched));
    EQ64(csrr(c, CSR_MIP) >> MTI & 1, 0, "the CLINT's timer not yet");
    tick(&m, 1);
    EQ64(csrr(c, CSR_MIP) >> MTI & 1, 1, "the CLINT's timer fires at the restored mtimecmp");
    csrw(c, CSR_MSTATUS, MS_MIE);
    CHECK(take(&m) && c->pc == TVEC + 4 * MEI, "MEI taken through the restored vectored mtvec");
    uam_snapshot_free(s);
    uam_machine_fini(&m);
}

int main(void)
{
    test_reset();
    test_csrs();
    test_irq();
    test_clint();
    test_plic();
    test_board();
    test_gdb();
    test_snapshot();
    for (int jit = 0; jit < 2; jit++) {
        test_csr_insns(jit);
        test_traps(jit);
        test_faults(jit);
        test_wfi(jit);
    }
    test_mret_pending();
    printf("test_riscv: %d/%d checks passed\n", n_checks - n_failed, n_checks);
    return n_failed ? 1 : 0;
}
