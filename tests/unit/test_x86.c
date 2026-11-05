/*
 * test_x86.c - The x86-64 module's parts the self-test firmware cannot reach on
 * x86_64-ref: the reset state (CS F000 with the hidden base FFFF0000 until the first far
 * jump, CR0, EFLAGS, EDX, the descriptor-table registers, DR6/DR7, the MSRs' reset values)
 * and what reset and INIT keep, wait-for-SIPI and the startup IPI, INIT, the interrupt
 * lines through the board's API (INTR and the acknowledge hook, IF, NMI edges and the NMI
 * block until IRET, the lines reported), the interrupt shadow after STI and MOV SS, HLT
 * waiting and what wakes it, exception entry in each mode (the real-mode IVT at IDTR, its
 * frame without an error code; the protected- and long-mode frames, error codes, IDT limit
 * and not-present gates, interrupt and trap gates, the double-fault rules and the triple
 * fault; #PF error codes and CR2; the stack switch from ring 3 and IST, the 16-byte
 * alignment of a 64-bit frame), SMM (entry state, the Intel 64 and AMD64 save-state maps
 * field by field, the SMI and NMI held in SMM, SMBASE relocation, auto-HALT restart,
 * SMIACT#), the MSRs' rules and policy, CPUID, the I/O-port aperture, A20M#, the debugger
 * view, the dump and a snapshot of the ISA state.
 *
 * Each test runs on a bare machine: RAM over the first 4 MiB, at the reset vector and over
 * the I/O-port aperture (ports read back what was written); every vector's handler is a
 * jump to itself, so an exception or interrupt leaves the hart spinning where it was
 * delivered. The tests that execute code run interpreted and under the JIT.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../isa/x86/x86.h"
#include "uam/bus.h"
#include "uam/isa/x86.h"
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

#define RAM_SIZE 0x00400000u
#define ROM      0xFFFF0000u
#define CODE     0x1000u
#define HANDLERS 0x3000u       /* vector v's handler at HANDLERS + 16 v */
#define GDT      0x5000u
#define IDT      0x6000u
#define TSS      0x7000u
#define STACK    0x9000u       /* ring 0 (TSS.RSP0) */
#define USTACK   0xA000u       /* ring 3 */
#define ISTACK   0xB000u       /* TSS.IST1 */
#define PT       0x10000u      /* page tables */
#define SMBASE   0x30000u      /* the reset SMBASE */

/* GDT selectors */
enum { CS32 = 0x08, DS32 = 0x10, CS64 = 0x18, UCS64 = 0x23, UDS = 0x2B, TSSSEL = 0x30 };
#define GDT_LIMIT 0x3Fu

static uint32_t handler(unsigned v) { return HANDLERS + 16 * v; }

/* ---- the machine ----------------------------------------------------------------- */

static uint64_t put_bytes(uam_machine *m, uint64_t a, const uint8_t *b, size_t n)
{
    CHECK(uam_bus_load_bytes(&m->bus, a, b, n) == UAM_OK, "load at 0x%llx", (unsigned long long)a);
    return a + n;
}
/* Put instruction bytes at a; returns the address after them. */
#define PUT(m, a, ...) put_bytes((m), (a), (const uint8_t[]){ __VA_ARGS__ }, \
                                 sizeof((const uint8_t[]){ __VA_ARGS__ }))
#define SPIN 0xEB, 0xFE   /* jmp $ */

static uint64_t rd(uam_machine *m, uint64_t a, unsigned size)
{
    uint64_t v = 0;
    uam_bus_read(&m->bus, a, size, UAM_ACC_LE, &v);
    return v;
}
static void wr(uam_machine *m, uint64_t a, unsigned size, uint64_t v) { uam_bus_write(&m->bus, a, size, UAM_ACC_LE, v); }
static uint64_t gpr(uam_machine *m, unsigned r) { return x86_gpr(&m->cpu, r); }
static void setgpr(uam_machine *m, unsigned r, uint64_t v) { x86_set_gpr(&m->cpu, r, v); }
static x86_state *st(uam_machine *m) { return x86_st(&m->cpu); }
static void sysw(uam_machine *m, uint32_t id, uint64_t v)
{
    CHECK(m->cpu.isa->sysreg_write(&m->cpu, id, v) == UAM_OK, "write sysreg %u", (unsigned)id);
}
static uint64_t sysr(uam_machine *m, uint32_t id)
{
    uint64_t v = 0;
    CHECK(m->cpu.isa->sysreg_read(&m->cpu, id, &v) == UAM_OK, "read sysreg %u", (unsigned)id);
    return v;
}
static uint64_t msr(uam_machine *m, uint32_t n)
{
    uint64_t v = 0;
    CHECK(x86_rdmsr(&m->cpu, n, &v) == UAM_OK, "rdmsr 0x%x", (unsigned)n);
    return v;
}
static void line(uam_machine *m, unsigned n, int level) { uam_cpu_irq_sink(&m->cpu, n, level); }
static bool take(uam_machine *m) { return m->cpu.isa->irq_take(&m->cpu); }
static bool wake(uam_machine *m) { return m->cpu.isa->irq_wakeup(&m->cpu); }
static void run(uam_machine *m, uint64_t pc, uint64_t n)
{
    m->cpu.pc = pc;
    uam_exec_run(&m->exec, n);
}
static uint32_t flags(uam_machine *m) { return x86_get_rflags(&m->cpu); }
static uint64_t sp(uam_machine *m) { return gpr(m, X86_RSP); }

/* The board's side: the interrupt acknowledge, MSRs, SMIACT#. */
typedef struct plat {
    int      vector;          /* what INTA answers */
    unsigned inta, smm_on, smm_off;
    uint64_t wr_val;
} plat;
static int p_inta(void *c) { ((plat *)c)->inta++; return ((plat *)c)->vector; }
static void p_smm(void *c, bool on) { if (on) ((plat *)c)->smm_on++; else ((plat *)c)->smm_off++; }
static bool p_rdmsr(void *c, uint32_t n, uint64_t *v) { (void)c; if (n != 0xC0DE) return false; *v = 0xFEED; return true; }
static bool p_wrmsr(void *c, uint32_t n, uint64_t v)
{
    if (n != 0xC0DE && n != MSR_APIC_BASE) return false;
    ((plat *)c)->wr_val = v;
    return true;
}

/* RAM at 0, at the reset vector and over the ports; real mode at CODE, the IVT pointing
 * at the spinning handlers, SP at STACK. */
static void machine(uam_machine *m, plat *p, bool jit)
{
    CHECK(uam_machine_init(m, &uam_isa_x86_64) == UAM_OK, "machine init");
    CHECK(uam_bus_map_ram(&m->bus, "ram", 0, RAM_SIZE) == UAM_OK, "map RAM");
    CHECK(uam_bus_map_ram(&m->bus, "rom", ROM, 0x10000) == UAM_OK, "map the reset vector");
    CHECK(uam_bus_map_ram(&m->bus, "ports", UAM_X86_PIO_BASE, UAM_X86_PIO_SIZE) == UAM_OK, "map ports");
    if (jit)
        CHECK(uam_exec_enable_jit(&m->exec), "JIT");
    if (p) {
        memset(p, 0, sizeof(*p));
        p->vector = -1;
        uam_x86_set_platform(&m->cpu, &(uam_x86_platform){ .ctx = p, .inta = p_inta, .rdmsr = p_rdmsr,
                                                            .wrmsr = p_wrmsr, .smm_changed = p_smm });
    }
    uam_machine_reset(m, CODE);
    for (unsigned v = 0; v < 256; v++) {
        wr(m, handler(v), 2, 0xFEEB);            /* jmp $ */
        wr(m, 4 * v, 4, handler(v));            /* IVT: 0000:handler */
    }
    setgpr(m, X86_RSP, STACK);
}

static void gate32(uam_machine *m, unsigned v, uint16_t sel, uint32_t off, unsigned type, bool present)
{
    wr(m, IDT + 8 * v, 8, (off & 0xFFFF) | (uint64_t)sel << 16 |
                          (uint64_t)((present ? 0x80 : 0) | type) << 40 | (uint64_t)(off >> 16) << 48);
}
static void gate64(uam_machine *m, unsigned v, uint32_t off, unsigned dpl, unsigned ist)
{
    wr(m, IDT + 16 * v, 8, (off & 0xFFFF) | (uint64_t)CS64 << 16 | (uint64_t)ist << 32 |
                           (uint64_t)(0x8E | dpl << 5) << 40 | (uint64_t)(off >> 16) << 48);
    wr(m, IDT + 16 * v + 8, 8, 0);
}
static void load(uam_machine *m, unsigned seg, uint16_t sel)
{
    CHECK(x86_load_seg_nocheck(&m->cpu, seg, sel) == UAM_OK, "load segment %u with 0x%x", seg, sel);
}
static void gdt(uam_machine *m)
{
    static const uint64_t d[] = { 0, 0x00CF9A000000FFFFull, 0x00CF92000000FFFFull, 0x00AF9A000000FFFFull,
                                  0x00AFFA000000FFFFull, 0x00CFF2000000FFFFull,
                                  0x0000890000000067ull | (uint64_t)TSS << 16, 0 };
    for (unsigned i = 0; i < UAM_ARRAY_LEN(d); i++)
        wr(m, GDT + 8 * i, 8, d[i]);
    sysw(m, X86_SR_GDTR_BASE, GDT);
    sysw(m, X86_SR_GDTR_LIMIT, GDT_LIMIT);
}

/* 32-bit protected mode, flat, every vector an interrupt gate to its handler. */
static void prot32(uam_machine *m)
{
    gdt(m);
    for (unsigned v = 0; v < 256; v++)
        gate32(m, v, CS32, handler(v), 0xE, true);
    sysw(m, X86_SR_IDTR_BASE, IDT);
    sysw(m, X86_SR_IDTR_LIMIT, 256 * 8 - 1);
    sysw(m, X86_SR_CR0, sysr(m, X86_SR_CR0) | CR0_PE);
    load(m, X86_SEG_CS, CS32);
    load(m, X86_SEG_SS, DS32);
    load(m, X86_SEG_DS, DS32);
    load(m, X86_SEG_ES, DS32);
}

/* 32-bit paging over the first 4 MiB: 0x300000 not present, 0x301000 read-only. */
static void paging32(uam_machine *m)
{
    wr(m, PT, 4, (PT + 0x1000) | 3);
    for (uint32_t i = 0; i < 1024; i++)
        wr(m, PT + 0x1000 + 4 * i, 4, i == 0x300 ? 0 : i == 0x301 ? (i << 12 | 1) : (i << 12 | 3));
    sysw(m, X86_SR_CR3, PT);
    sysw(m, X86_SR_CR0, sysr(m, X86_SR_CR0) | CR0_PG | CR0_WP);
}

/* 64-bit mode: 0-2 MiB a user page, 2-4 MiB supervisor and not executable, nothing at
 * 1 GiB; the TSS's RSP0 = STACK and IST1 = ISTACK. */
static void long64(uam_machine *m)
{
    gdt(m);
    for (unsigned v = 0; v < 256; v++)
        gate64(m, v, handler(v), 0, 0);
    sysw(m, X86_SR_IDTR_BASE, IDT);
    sysw(m, X86_SR_IDTR_LIMIT, 256 * 16 - 1);
    wr(m, PT, 8, (PT + 0x1000) | 7);
    wr(m, PT + 0x1000, 8, (PT + 0x2000) | 7);
    wr(m, PT + 0x2000, 8, 0x000000 | 0x87);
    wr(m, PT + 0x2008, 8, 0x200000 | 0x83 | 1ull << 63);
    wr(m, TSS + 4, 8, STACK);
    wr(m, TSS + 0x24, 8, ISTACK);
    sysw(m, X86_SR_CR4, CR4_PAE);
    sysw(m, X86_SR_CR3, PT);
    CHECK(x86_wrmsr(&m->cpu, MSR_EFER, EFER_LME | EFER_NXE) == UAM_OK, "EFER.LME");
    sysw(m, X86_SR_CR0, sysr(m, X86_SR_CR0) | CR0_PE | CR0_PG);
    CHECK(st(m)->efer & EFER_LMA, "long mode active");
    load(m, X86_SEG_CS, CS64);
    load(m, X86_SEG_SS, DS32);
    load(m, X86_SEG_DS, DS32);
    CHECK(x86_ltr(&m->cpu, TSSSEL) == UAM_OK, "ltr");
    EQ64(x86_priv_code(m->cpu.priv), X86_CODE64, "64-bit code");
}

/* ---- reset, INIT and the startup IPI ---------------------------------------------- */

static void test_reset(bool jit)
{
    uam_machine m;
    machine(&m, NULL, jit);
    uam_x86_cpuid_info id;
    uam_x86_get_cpuid(&m.cpu, &id);
    id.signature = 0x000906EA;
    uam_x86_set_cpuid(&m.cpu, &id);
    uam_x86_set_smm_map(&m.cpu, UAM_X86_SMM_MAP_AMD64);
    setgpr(&m, X86_RAX, 0x1234);
    uam_machine_reset(&m, 0xFFFFFFF0u);
    EQ64(m.cpu.pc, 0xFFFFFFF0u, "the first fetch is at FFFFFFF0");
    EQ64(x86_seg_sel(&m.cpu, X86_SEG_CS), 0xF000, "CS F000");
    EQ64(x86_seg_base(&m.cpu, X86_SEG_CS), 0xFFFF0000u, "CS base FFFF0000");
    EQ64(x86_rip(&m.cpu), 0xFFF0, "IP FFF0");
    EQ64(x86_seg_attr(&m.cpu, X86_SEG_CS), 0x9B, "CS: present code, accessed");
    for (unsigned s = 0; s < X86_NSEG; s++) {
        EQ64(uam_cpu_reg_read(&m.cpu, X86_OFF_SEG_LIMIT(s), 32), 0xFFFF, "segment limit FFFF");
        if (s != X86_SEG_CS)
            CHECK(x86_seg_sel(&m.cpu, s) == 0 && x86_seg_base(&m.cpu, s) == 0 &&
                  x86_seg_attr(&m.cpu, s) == 0x93, "data segment %u: 0, base 0, data RW", s);
    }
    EQ64(sysr(&m, X86_SR_CR0), 0x60000010u, "CR0: CD NW ET");
    CHECK(sysr(&m, X86_SR_CR2) == 0 && sysr(&m, X86_SR_CR3) == 0 && sysr(&m, X86_SR_CR4) == 0, "CR2-CR4 0");
    EQ64(flags(&m), 2, "EFLAGS 2");
    EQ64(gpr(&m, X86_RDX), 0x000906EA, "EDX: the processor signature");
    EQ64(gpr(&m, X86_RAX), 0, "EAX 0");
    CHECK(sysr(&m, X86_SR_GDTR_BASE) == 0 && sysr(&m, X86_SR_GDTR_LIMIT) == 0xFFFF &&
          sysr(&m, X86_SR_IDTR_BASE) == 0 && sysr(&m, X86_SR_IDTR_LIMIT) == 0xFFFF, "GDTR, IDTR 0/FFFF");
    EQ64(sysr(&m, X86_SR_DR6), 0xFFFF0FF0u, "DR6");
    EQ64(sysr(&m, X86_SR_DR7), 0x400, "DR7");
    EQ64(sysr(&m, X86_SR_XCR0), 1, "XCR0: x87");
    EQ64(uam_cpu_reg_read(&m.cpu, X86_OFF_MXCSR, 32), 0x1F80, "MXCSR");
    EQ64(uam_cpu_reg_read(&m.cpu, X86_OFF_FCW, 16), 0x37F, "FCW");
    EQ64(x86_priv_mode(m.cpu.priv), X86_MODE_REAL, "real mode");
    CHECK(x86_priv_code(m.cpu.priv) == X86_CODE16 && x86_priv_cpl(m.cpu.priv) == 0 &&
          !x86_priv_smm(m.cpu.priv), "16-bit code, CPL 0, not SMM");
    EQ64(msr(&m, MSR_APIC_BASE), 0xFEE00900u, "IA32_APIC_BASE: FEE00000, enabled, BSP");
    EQ64(msr(&m, MSR_EFER), 0, "EFER 0");
    EQ64(msr(&m, MSR_PAT), 0x0007040600070406ull, "PAT");
    EQ64(msr(&m, MSR_AMD_SMM_BASE), SMBASE, "SMBASE 30000; the AMD64 map outlives the reset");
    uam_x86_get_cpuid(&m.cpu, &id);
    EQ64(id.signature, 0x000906EA, "the identity outlives the reset");

    /* the hidden base holds until CS is loaded */
    PUT(&m, 0xFFFFFFF0u, 0xE8, 0x00, 0x00,                  /* call $+3 */
                         0xEA, 0x5B, 0xE0, 0x00, 0xF0);     /* jmp far F000:E05B */
    PUT(&m, 0xFE05B, SPIN);
    uam_exec_run(&m.exec, 10);
    EQ64(m.cpu.pc, 0xFE05B, "the far jump lands at FE05B");
    EQ64(x86_seg_base(&m.cpu, X86_SEG_CS), 0xF0000, "CS base F0000 after it");
    EQ64(sp(&m), 0xFFFE, "SP wrapped from 0");
    EQ64(rd(&m, 0xFFFE, 2), 0xFFF3, "CALL pushed IP FFF3");
    uam_machine_fini(&m);
}

static void test_init_sipi(void)
{
    uam_machine m;
    machine(&m, NULL, false);
    /* an application processor leaves reset waiting for a startup IPI */
    m.cpu.hartid = 1;
    uam_machine_reset(&m, 0xFFFFFFF0u);
    CHECK(m.cpu.waiting && st(&m)->wait_sipi, "AP: waiting for SIPI");
    EQ64(msr(&m, MSR_APIC_BASE), 0xFEE00800u, "AP: no BSP flag");
    line(&m, UAM_X86_IRQ_NMI, 1);
    line(&m, UAM_X86_IRQ_INTR, 1);
    CHECK(!wake(&m) && !take(&m), "AP: NMI and INTR are not recognised");
    uam_x86_sipi(&m.cpu, 0x12);
    CHECK(wake(&m), "AP: the SIPI wakes it");
    CHECK(take(&m), "AP: and starts it");
    CHECK(!m.cpu.waiting && !st(&m)->wait_sipi, "AP: running");
    EQ64(m.cpu.pc, 0x12000, "AP: at vector << 12");
    CHECK(x86_seg_sel(&m.cpu, X86_SEG_CS) == 0x1200 && x86_seg_base(&m.cpu, X86_SEG_CS) == 0x12000,
          "AP: CS 1200, base 12000");
    uam_x86_sipi(&m.cpu, 0x34);
    CHECK(!st(&m)->sipi_pending, "a running processor ignores a SIPI");
    line(&m, UAM_X86_IRQ_NMI, 0);
    line(&m, UAM_X86_IRQ_INTR, 0);

    /* INIT on the BSP in protected mode: back to the reset state, waiting for a SIPI;
     * SMBASE, the APIC base and the BSP flag stay */
    m.cpu.hartid = 0;
    uam_machine_reset(&m, CODE);
    prot32(&m);
    st(&m)->smbase = 0x50000;
    line(&m, UAM_X86_IRQ_INIT, 1);
    line(&m, UAM_X86_IRQ_INIT, 0);
    CHECK(wake(&m), "INIT wakes");
    CHECK(!take(&m), "INIT starts nothing");
    CHECK(m.cpu.waiting && st(&m)->wait_sipi, "BSP after INIT: waiting for SIPI");
    EQ64(sysr(&m, X86_SR_CR0), 0x60000010u, "INIT: CR0 as at reset");
    EQ64(x86_priv_mode(m.cpu.priv), X86_MODE_REAL, "INIT: real mode");
    EQ64(st(&m)->smbase, 0x50000, "INIT keeps SMBASE");
    EQ64(msr(&m, MSR_APIC_BASE), 0xFEE00900u, "INIT keeps the BSP flag");
    uam_x86_sipi(&m.cpu, 0x20);
    CHECK(take(&m) && m.cpu.pc == 0x20000, "the SIPI starts the BSP at 20000");
    uam_machine_fini(&m);
}

/* ---- interrupt lines, NMI, the interrupt shadow, HLT ------------------------------- */

static void test_intr(void)
{
    uam_machine m;
    plat p;
    machine(&m, &p, false);
    PUT(&m, CODE, SPIN);
    p.vector = 0x21;
    line(&m, UAM_X86_IRQ_INTR, 1);
    line(&m, UAM_X86_IRQ_A20M, 1);
    EQ64(m.cpu.isa->irq_lines(&m.cpu), 1u << UAM_X86_IRQ_INTR | 1u << UAM_X86_IRQ_A20M, "the lines reported");
    line(&m, UAM_X86_IRQ_A20M, 0);
    CHECK(!wake(&m) && !take(&m), "IF clear: INTR neither wakes nor is taken");
    EQ64(p.inta, 0, "no acknowledge with IF clear");
    x86_set_rflags(&m.cpu, RFLAGS_IF | 2);
    p.vector = -1;
    CHECK(wake(&m), "IF set: INTR wakes");
    CHECK(!take(&m) && m.cpu.pc == CODE, "withdrawn at the acknowledge: nothing taken");
    p.vector = 0x21;
    CHECK(take(&m), "INTR taken");
    EQ64(p.inta, 2, "one acknowledge per attempt");
    EQ64(m.cpu.pc, handler(0x21), "through IVT entry 0x21");
    CHECK(!(flags(&m) & RFLAGS_IF), "IF cleared on entry");
    EQ64(rd(&m, sp(&m), 2), CODE, "the return IP");
    EQ64(rd(&m, sp(&m) + 4, 2), RFLAGS_IF | 2, "FLAGS pushed with IF");
    line(&m, UAM_X86_IRQ_INTR, 0);

    /* the IVT is where IDTR says */
    sysw(&m, X86_SR_IDTR_BASE, 0x2000);
    wr(&m, 0x2000 + 4 * 0x21, 4, handler(0x77));
    x86_set_rflags(&m.cpu, RFLAGS_IF | 2);
    line(&m, UAM_X86_IRQ_INTR, 1);
    CHECK(take(&m) && m.cpu.pc == handler(0x77), "the IVT at IDTR.base");
    uam_machine_fini(&m);
}

static void test_nmi(bool jit)
{
    uam_machine m;
    machine(&m, NULL, jit);
    PUT(&m, CODE, SPIN);
    PUT(&m, handler(2), 0x46, 0xCF, SPIN);   /* inc si; iret */
    line(&m, UAM_X86_IRQ_NMI, 1);
    CHECK(wake(&m), "an NMI wakes with IF clear");
    CHECK(take(&m) && m.cpu.pc == handler(2), "and is taken, vector 2");
    line(&m, UAM_X86_IRQ_NMI, 1);
    CHECK(!st(&m)->nmi_pending, "a line held high is one edge");
    line(&m, UAM_X86_IRQ_NMI, 0);
    line(&m, UAM_X86_IRQ_NMI, 1);
    CHECK(!wake(&m) && !take(&m), "a second NMI waits for the handler's IRET");
    uam_exec_run(&m.exec, 2);
    EQ64(m.cpu.pc, CODE, "IRET back");
    EQ64(gpr(&m, X86_RSI), 1, "the handler ran once");
    CHECK(take(&m) && m.cpu.pc == handler(2), "after IRET the held NMI is taken");
    uam_machine_fini(&m);
}

static void test_shadow(bool jit)
{
    uam_machine m;
    plat p;
    machine(&m, &p, jit);
    /* STI: an interrupt pending all along is taken after the next instruction */
    PUT(&m, CODE, 0xFB, 0x40, 0x43, SPIN);   /* sti; inc ax; inc bx */
    p.vector = 0x20;
    line(&m, UAM_X86_IRQ_INTR, 1);
    run(&m, CODE, 10);
    EQ64(m.cpu.pc, handler(0x20), "the interrupt after STI");
    EQ64(gpr(&m, X86_RAX), 1, "STI's shadow: the next instruction ran");
    EQ64(gpr(&m, X86_RBX), 0, "but no further");
    EQ64(rd(&m, sp(&m), 2), CODE + 2, "the return IP after the shadowed instruction");
    line(&m, UAM_X86_IRQ_INTR, 0);

    /* MOV SS retires with the next instruction */
    PUT(&m, CODE, 0x8E, 0xD0, 0x40, 0x43, SPIN);   /* mov ss, ax; inc ax; inc bx */
    setgpr(&m, X86_RAX, 0);
    setgpr(&m, X86_RBX, 0);
    run(&m, CODE, 1);
    EQ64(m.cpu.pc, CODE + 3, "MOV SS: stopped after the shadowed instruction");
    EQ64(gpr(&m, X86_RAX), 1, "MOV SS: the next instruction ran with it");
    uam_machine_fini(&m);
}

/* The shadow when MOV SS / POP SS end a block (protected mode re-keys the stack width),
 * and a fault in the shadowed instruction: reported at that instruction. */
static void test_shadow_cross(bool jit)
{
    uam_machine m;
    plat p;
    /* protected mode: INTR and NMI wait for the instruction after MOV SS / POP SS */
    for (int k = 0; k < 3; k++) {
        machine(&m, &p, jit);
        prot32(&m);
        x86_set_rflags(&m.cpu, RFLAGS_IF | 2);
        if (k < 2)
            PUT(&m, CODE, 0x8E, 0xD0, 0x40, 0x43, SPIN);         /* mov ss, ax; inc eax; inc ebx */
        else
            PUT(&m, CODE, 0x17, 0x90, 0x40, 0x43, SPIN);         /* pop ss; nop; inc eax; inc ebx */
        wr(&m, STACK, 4, DS32);
        setgpr(&m, X86_RAX, DS32);
        setgpr(&m, X86_RBX, 0);
        const char *what = k == 0 ? "INTR after MOV SS" : k == 1 ? "NMI after MOV SS" : "INTR after POP SS";
        run(&m, CODE, 1);
        CHECK(m.cpu.pc == CODE + (k < 2 ? 2 : 1), "%s: the SS load ran (pc 0x%llx)", what,
              (unsigned long long)m.cpu.pc);
        CHECK(m.cpu.priv & X86_PRIV_SHADOW, "%s: the shadow carried to the next block", what);
        p.vector = 0x20;
        line(&m, k == 1 ? UAM_X86_IRQ_NMI : UAM_X86_IRQ_INTR, 1);
        CHECK(!take(&m), "%s: not taken before the next instruction", what);
        run(&m, m.cpu.pc, 10);
        const unsigned vec = k == 1 ? 2 : 0x20;
        CHECK(m.cpu.pc == handler(vec), "%s: then taken (pc 0x%llx)", what, (unsigned long long)m.cpu.pc);
        const uint32_t after = k < 2 ? CODE + 3 : CODE + 2;
        CHECK(rd(&m, sp(&m), 4) == after, "%s: EIP 0x%x, after the shadowed instruction", what,
              (unsigned)rd(&m, sp(&m), 4));
        EQ64(gpr(&m, X86_RBX), 0, "no further");
        CHECK(!(m.cpu.priv & X86_PRIV_SHADOW), "%s: the shadow is over", what);
        uam_machine_fini(&m);
    }
    /* a fault in the shadowed instruction: its own address, the one before it done */
    static const struct { bool prot; uint8_t b[3]; unsigned len; uint32_t ip; int32_t dsp; const char *what; } f[] = {
        { false, { 0xFB, 0x0F, 0x0B }, 3, CODE + 1, -6, "real mode, sti; ud2" },
        { false, { 0x17, 0x0F, 0x0B }, 3, CODE + 1, 2 - 6, "real mode, pop ss; ud2 (popped once)" },
        { true,  { 0xFB, 0x0F, 0x0B }, 3, CODE + 1, -12, "protected mode, sti; ud2" },
        { true,  { 0x8E, 0xD0, 0x0F }, 3, CODE + 2, -12, "protected mode, mov ss, ax; ud2" },
    };
    for (size_t i = 0; i < UAM_ARRAY_LEN(f); i++) {
        machine(&m, NULL, jit);
        if (f[i].prot)
            prot32(&m);
        put_bytes(&m, CODE, f[i].b, f[i].len);
        if (f[i].len == 3 && f[i].b[0] == 0x8E)
            PUT(&m, CODE + 3, 0x0B);
        wr(&m, STACK, 4, f[i].prot ? DS32 : 0);
        setgpr(&m, X86_RAX, DS32);
        run(&m, CODE, 10);
        CHECK(m.cpu.pc == handler(6), "%s: #UD (pc 0x%llx)", f[i].what, (unsigned long long)m.cpu.pc);
        EQ64(sp(&m), (uint64_t)(STACK + f[i].dsp), f[i].what);
        CHECK(rd(&m, sp(&m), f[i].prot ? 4 : 2) == f[i].ip, "%s: return IP 0x%x want 0x%x", f[i].what,
              (unsigned)rd(&m, sp(&m), f[i].prot ? 4 : 2), (unsigned)f[i].ip);
        CHECK(!(m.cpu.priv & X86_PRIV_SHADOW), "%s: no shadow left", f[i].what);
        uam_machine_fini(&m);
    }
}

static void test_hlt(bool jit)
{
    uam_machine m;
    plat p;
    machine(&m, &p, jit);
    PUT(&m, CODE, 0xF4, 0x40, SPIN);   /* hlt; inc ax */
    run(&m, CODE, 10);
    CHECK(m.cpu.waiting, "HLT waits");
    EQ64(m.cpu.pc, CODE + 1, "after the HLT");
    p.vector = 0x22;
    line(&m, UAM_X86_IRQ_INTR, 1);
    CHECK(!wake(&m), "INTR with IF clear does not wake it");
    line(&m, UAM_X86_IRQ_NMI, 1);
    CHECK(wake(&m), "an NMI does");
    uam_exec_run(&m.exec, 1);
    CHECK(!m.cpu.waiting, "no longer waiting");
    EQ64(m.cpu.pc, handler(2), "the NMI handler");
    EQ64(rd(&m, sp(&m), 2), CODE + 1, "returning after the HLT");
    EQ64(gpr(&m, X86_RAX), 0, "nothing after the HLT ran");

    setgpr(&m, X86_RSP, STACK);
    x86_set_rflags(&m.cpu, RFLAGS_IF | 2);
    st(&m)->nmi_blocked = false;
    line(&m, UAM_X86_IRQ_INTR, 0);
    run(&m, CODE, 10);
    CHECK(m.cpu.waiting && m.cpu.pc == CODE + 1, "HLT with IF set");
    line(&m, UAM_X86_IRQ_INTR, 1);
    CHECK(wake(&m), "INTR wakes it");
    uam_exec_run(&m.exec, 1);
    EQ64(m.cpu.pc, handler(0x22), "IF set: INTR ends the HLT");
    EQ64(rd(&m, sp(&m), 2), CODE + 1, "returning after the HLT");
    uam_machine_fini(&m);
}

/* ---- exceptions ------------------------------------------------------------------ */

static void test_real_exceptions(bool jit)
{
    uam_machine m;
    machine(&m, NULL, jit);
    PUT(&m, CODE, 0xF6, 0xF3);   /* div bl */
    x86_set_rflags(&m.cpu, RFLAGS_IF | 2);
    run(&m, CODE, 10);
    EQ64(m.cpu.pc, handler(0), "#DE through IVT entry 0");
    EQ64(sp(&m), STACK - 6, "IP, CS, FLAGS: no error code in real mode");
    EQ64(rd(&m, sp(&m), 2), CODE, "the faulting instruction's IP");
    EQ64(rd(&m, sp(&m) + 2, 2), 0, "CS");
    EQ64(rd(&m, sp(&m) + 4, 2), RFLAGS_IF | 2, "FLAGS");
    CHECK(!(flags(&m) & RFLAGS_IF), "IF cleared");

    /* #GP from RDMSR of a missing MSR under the #GP policy: still no error code */
    uam_x86_set_msr_policy(&m.cpu, UAM_X86_MSR_GP, false);
    PUT(&m, CODE, 0x0F, 0x32);
    setgpr(&m, X86_RCX, 0x1234);
    setgpr(&m, X86_RSP, STACK);
    run(&m, CODE, 10);
    EQ64(m.cpu.pc, handler(13), "#GP: missing MSR");
    EQ64(sp(&m), STACK - 6, "#GP in real mode pushes no error code");
    EQ64(rd(&m, sp(&m), 2), CODE, "at the RDMSR");

    /* RSM outside SMM */
    PUT(&m, CODE, 0x0F, 0xAA);
    run(&m, CODE, 10);
    EQ64(m.cpu.pc, handler(6), "RSM outside SMM is #UD");

    /* triple fault: nothing in the IVT is reachable */
    sysw(&m, X86_SR_IDTR_LIMIT, 0);
    PUT(&m, CODE, 0x0F, 0x0B);   /* ud2 */
    run(&m, CODE, 10);
    CHECK(st(&m)->shutdown && m.cpu.waiting, "triple fault: shut down");
    line(&m, UAM_X86_IRQ_NMI, 1);
    CHECK(!wake(&m), "an NMI does not wake a shut-down hart");
    line(&m, UAM_X86_IRQ_INIT, 1);
    CHECK(wake(&m), "INIT does");
    uam_machine_fini(&m);
}

static void test_prot_exceptions(bool jit)
{
    uam_machine m;
    machine(&m, NULL, jit);
    prot32(&m);
    x86_set_rflags(&m.cpu, RFLAGS_IF | 2);
    PUT(&m, CODE, 0x0F, 0x0B);   /* ud2 */
    run(&m, CODE, 10);
    EQ64(m.cpu.pc, handler(6), "#UD");
    EQ64(sp(&m), STACK - 12, "EIP, CS, EFLAGS");
    EQ64(rd(&m, sp(&m), 4), CODE, "EIP: the UD2");
    EQ64(rd(&m, sp(&m) + 4, 4), CS32, "CS");
    EQ64(rd(&m, sp(&m) + 8, 4), RFLAGS_IF | 2, "EFLAGS");
    CHECK(!(flags(&m) & RFLAGS_IF), "an interrupt gate clears IF");
    gate32(&m, 6, CS32, handler(6), 0xF, true);
    x86_set_rflags(&m.cpu, RFLAGS_IF | 2);
    setgpr(&m, X86_RSP, STACK);
    run(&m, CODE, 10);
    CHECK(m.cpu.pc == handler(6) && (flags(&m) & RFLAGS_IF), "a trap gate keeps IF");

    struct { const char *what; uint8_t b0, b1; unsigned vec; uint32_t err; } cases[] = {
        { "INT 40h beyond the IDT limit: #GP(IDT 40h)", 0xCD, 0x40, 13, 0x40 * 8 + 2 },
        { "INT 30h through a not-present gate: #NP(IDT 30h)", 0xCD, 0x30, 11, 0x30 * 8 + 2 },
        { "a gate's CS beyond the GDT: #GP(sel)", 0xCD, 0x31, 13, 0x40 },
        { "#UD through a not-present gate: #NP, not #DF", 0x0F, 0x0B, 11, 6 * 8 + 2 },
    };
    for (size_t i = 0; i < UAM_ARRAY_LEN(cases); i++) {
        for (unsigned v = 0; v < 256; v++)
            gate32(&m, v, CS32, handler(v), 0xE, true);
        sysw(&m, X86_SR_IDTR_LIMIT, i == 0 ? 0x40 * 8 - 1 : 256 * 8 - 1);
        gate32(&m, 0x30, CS32, handler(0x30), 0xE, false);
        gate32(&m, 6, CS32, handler(6), 0xE, false);
        gate32(&m, 0x31, 0x40, handler(0x31), 0xE, true);
        PUT(&m, CODE, cases[i].b0, cases[i].b1);
        setgpr(&m, X86_RSP, STACK);
        run(&m, CODE, 10);
        CHECK(m.cpu.pc == handler(cases[i].vec), "%s: vector (pc 0x%llx)", cases[i].what,
              (unsigned long long)m.cpu.pc);
        CHECK(sp(&m) == STACK - 16 && rd(&m, sp(&m), 4) == cases[i].err, "%s: error code 0x%x",
              cases[i].what, (unsigned)rd(&m, sp(&m), 4));
        EQ64(rd(&m, sp(&m) + 4, 4), CODE, cases[i].what);
    }
    sysw(&m, X86_SR_IDTR_LIMIT, 256 * 8 - 1);
    gate32(&m, 6, CS32, handler(6), 0xE, true);

    /* the double-fault rules */
    gate32(&m, 11, CS32, handler(11), 0xE, false);
    PUT(&m, CODE, 0xCD, 0x30);
    setgpr(&m, X86_RSP, STACK);
    run(&m, CODE, 10);
    EQ64(m.cpu.pc, handler(8), "#NP while delivering #NP: #DF");
    CHECK(sp(&m) == STACK - 16 && rd(&m, sp(&m), 4) == 0, "#DF error code 0");
    EQ64(rd(&m, sp(&m) + 4, 4), CODE, "#DF at the INT");
    gate32(&m, 11, CS32, handler(11), 0xE, true);

    paging32(&m);
    gate32(&m, 14, 0x40, handler(14), 0xE, true);
    PUT(&m, CODE, 0x89, 0x05, 0x23, 0x01, 0x30, 0x00);   /* mov [0x300123], eax */
    setgpr(&m, X86_RSP, STACK);
    run(&m, CODE, 10);
    EQ64(m.cpu.pc, handler(8), "#GP while delivering #PF: #DF");
    gate32(&m, 14, CS32, handler(14), 0xE, true);

    /* #PF: error code and CR2 */
    struct { const char *what; uint8_t op; uint32_t addr, err; bool wp; } pf[] = {
        { "store to a not-present page", 0x89, 0x300123, PFEC_W, true },
        { "load from a not-present page", 0x8B, 0x300FFC, 0, true },
        { "store to a read-only page, CR0.WP", 0x89, 0x301040, PFEC_P | PFEC_W, true },
        { "load from a read-only page", 0x8B, 0x301040, ~0u, true },
        { "store to a read-only page, no CR0.WP", 0x89, 0x301044, ~0u, false },
    };
    for (size_t i = 0; i < UAM_ARRAY_LEN(pf); i++) {
        uint32_t a = pf[i].addr;
        uint64_t cr0 = sysr(&m, X86_SR_CR0);
        sysw(&m, X86_SR_CR0, pf[i].wp ? cr0 | CR0_WP : cr0 & ~(uint64_t)CR0_WP);
        PUT(&m, PUT(&m, CODE, pf[i].op, 0x05, a & 0xFF, a >> 8 & 0xFF, a >> 16 & 0xFF, a >> 24), SPIN);
        sysw(&m, X86_SR_CR2, 0);
        setgpr(&m, X86_RSP, STACK);
        run(&m, CODE, 10);
        if (pf[i].err == ~0u) {
            CHECK(m.cpu.pc == CODE + 6, "%s: no fault (pc 0x%llx)", pf[i].what, (unsigned long long)m.cpu.pc);
            continue;
        }
        CHECK(m.cpu.pc == handler(14), "%s: #PF (pc 0x%llx)", pf[i].what, (unsigned long long)m.cpu.pc);
        CHECK(rd(&m, sp(&m), 4) == pf[i].err, "%s: error code 0x%x", pf[i].what, (unsigned)rd(&m, sp(&m), 4));
        EQ64(rd(&m, sp(&m) + 4, 4), CODE, pf[i].what);
        EQ64(sysr(&m, X86_SR_CR2), a, "CR2: the faulting linear address");
    }

    /* triple fault in protected mode */
    gate32(&m, 0x30, CS32, handler(0x30), 0xE, false);
    gate32(&m, 11, CS32, handler(11), 0xE, false);
    gate32(&m, 8, CS32, handler(8), 0xE, false);
    PUT(&m, CODE, 0xCD, 0x30);
    run(&m, CODE, 10);
    CHECK(st(&m)->shutdown, "#NP, #NP, then #DF undeliverable: shut down");
    uam_machine_fini(&m);
}

static void test_long_exceptions(bool jit)
{
    uam_machine m;
    machine(&m, NULL, jit);
    long64(&m);
    /* a 64-bit frame: aligned to 16, SS:RSP always */
    PUT(&m, CODE, 0x0F, 0x0B);
    setgpr(&m, X86_RSP, STACK - 8);
    run(&m, CODE, 10);
    EQ64(m.cpu.pc, handler(6), "#UD in 64-bit mode");
    EQ64(sp(&m), STACK - 16 - 40, "the frame aligned to 16");
    EQ64(rd(&m, sp(&m), 8), CODE, "RIP");
    EQ64(rd(&m, sp(&m) + 8, 8), CS64, "CS");
    EQ64(rd(&m, sp(&m) + 24, 8), STACK - 8, "RSP");
    EQ64(rd(&m, sp(&m) + 32, 8), DS32, "SS");

    /* #PF: not present at 1 GiB; an instruction fetch from a no-execute page */
    PUT(&m, CODE, 0x8B, 0x04, 0x25, 0x56, 0x04, 0x00, 0x40);   /* mov eax, [0x40000456] */
    setgpr(&m, X86_RSP, STACK);
    run(&m, CODE, 10);
    CHECK(m.cpu.pc == handler(14) && rd(&m, sp(&m), 8) == 0, "#PF not present, a supervisor read: 0");
    EQ64(sysr(&m, X86_SR_CR2), 0x40000456, "CR2");
    EQ64(rd(&m, sp(&m) + 8, 8), CODE, "at the load");
    PUT(&m, CODE, 0xFF, 0xE0);                                 /* jmp rax */
    setgpr(&m, X86_RAX, 0x200000);
    setgpr(&m, X86_RSP, STACK);
    run(&m, CODE, 10);
    CHECK(m.cpu.pc == handler(14) && rd(&m, sp(&m), 8) == (PFEC_P | PFEC_I), "#PF fetch from NX: P|I (0x%x)",
          (unsigned)rd(&m, sp(&m), 8));
    EQ64(sysr(&m, X86_SR_CR2), 0x200000, "CR2 the fetch address");
    EQ64(rd(&m, sp(&m) + 8, 8), 0x200000, "RIP the fetch address");

    /* IST: the NMI on its own stack */
    gate64(&m, 2, handler(2), 0, 1);
    PUT(&m, CODE, SPIN);
    m.cpu.pc = CODE;
    setgpr(&m, X86_RSP, STACK);
    line(&m, UAM_X86_IRQ_NMI, 1);
    CHECK(take(&m) && m.cpu.pc == handler(2), "NMI");
    EQ64(sp(&m), ISTACK - 40, "on the IST1 stack");
    EQ64(rd(&m, sp(&m) + 24, 8), STACK, "the old RSP");
    uam_machine_fini(&m);

    /* from ring 3: the stack switch to RSP0, the gate DPL, user/supervisor pages, HLT */
    struct { const char *what; uint8_t b[2]; unsigned dpl, vec; uint32_t err; } u[] = {
        { "INT 21h, DPL 0 gate", { 0xCD, 0x21 }, 0, 13, 0x21 * 8 + 2 },
        { "INT 21h, DPL 3 gate", { 0xCD, 0x21 }, 3, 0x21, ~0u },
        { "read of a supervisor page", { 0x8B, 0x03 }, 0, 14, PFEC_P | PFEC_U },
        { "write of a supervisor page", { 0x89, 0x03 }, 0, 14, PFEC_P | PFEC_W | PFEC_U },
        { "HLT", { 0xF4, 0x90 }, 0, 13, 0 },
    };
    for (size_t i = 0; i < UAM_ARRAY_LEN(u); i++) {
        machine(&m, NULL, jit);
        long64(&m);
        gate64(&m, 0x21, handler(0x21), u[i].dpl, 0);
        PUT(&m, CODE, u[i].b[0], u[i].b[1]);
        load(&m, X86_SEG_CS, UCS64);
        load(&m, X86_SEG_SS, UDS);
        EQ64(x86_priv_cpl(m.cpu.priv), 3, "ring 3");
        setgpr(&m, X86_RSP, USTACK);
        setgpr(&m, X86_RBX, 0x200100);
        run(&m, CODE, 10);
        CHECK(m.cpu.pc == handler(u[i].vec), "ring 3 %s: vector %u (pc 0x%llx)", u[i].what, u[i].vec,
              (unsigned long long)m.cpu.pc);
        const unsigned e = u[i].err != ~0u;
        EQ64(sp(&m), STACK - 40 - 8 * e, u[i].what);
        if (e)
            CHECK(rd(&m, sp(&m), 8) == u[i].err, "ring 3 %s: error code 0x%x", u[i].what,
                  (unsigned)rd(&m, sp(&m), 8));
        EQ64(rd(&m, sp(&m) + 8 * e, 8), e ? CODE : CODE + 2, "RIP");
        EQ64(rd(&m, sp(&m) + 8 * e + 8, 8), UCS64, "the user CS");
        EQ64(rd(&m, sp(&m) + 8 * e + 24, 8), USTACK, "the user RSP");
        EQ64(rd(&m, sp(&m) + 8 * e + 32, 8), UDS, "the user SS");
        CHECK(x86_seg_sel(&m.cpu, X86_SEG_CS) == CS64 && x86_seg_sel(&m.cpu, X86_SEG_SS) == 0, "ring 0, SS null");
        if (u[i].vec == 14)
            EQ64(sysr(&m, X86_SR_CR2), 0x200100, "CR2");
        uam_machine_fini(&m);
    }
}

/* ---- SMM ------------------------------------------------------------------------- */

static void test_smm_intel(bool jit)
{
    uam_machine m;
    plat p;
    machine(&m, &p, jit);
    prot32(&m);
    CHECK(x86_ltr(&m.cpu, TSSSEL) == UAM_OK, "ltr");
    PUT(&m, CODE, SPIN);
    PUT(&m, SMBASE + 0x8000, 0x0F, 0xAA);              /* rsm */
    for (unsigned r = 0; r < 16; r++)
        setgpr(&m, r, 0x1111111111111111ull * (r + 1));
    setgpr(&m, X86_RSP, STACK);
    x86_set_rflags(&m.cpu, RFLAGS_IF | RFLAGS_CF | 2);
    sysw(&m, X86_SR_CR3, 0x12345000);
    sysw(&m, X86_SR_CR4, CR4_OSFXSR);
    sysw(&m, X86_SR_DR7, 0x401);
    const uint16_t cs_attr = x86_seg_attr(&m.cpu, X86_SEG_CS);
    const uint64_t cr0 = sysr(&m, X86_SR_CR0);
    m.cpu.pc = CODE;
    line(&m, UAM_X86_IRQ_SMI, 1);
    CHECK(wake(&m) && take(&m), "SMI taken");
    EQ64(p.smm_on, 1, "SMIACT# on");
    EQ64(m.cpu.pc, SMBASE + 0x8000, "at SMBASE + 8000");
    CHECK(x86_priv_smm(m.cpu.priv) && x86_priv_mode(m.cpu.priv) == X86_MODE_REAL, "SMM, real-address mode");
    CHECK(x86_seg_sel(&m.cpu, X86_SEG_CS) == SMBASE >> 4 && x86_seg_base(&m.cpu, X86_SEG_CS) == SMBASE,
          "CS = SMBASE >> 4, base SMBASE");
    EQ64(uam_cpu_reg_read(&m.cpu, X86_OFF_SEG_LIMIT(X86_SEG_DS), 32), 0xFFFFFFFFu, "4 GiB limits");
    EQ64(flags(&m), 2, "EFLAGS 2");
    CHECK(!(sysr(&m, X86_SR_CR0) & (CR0_PE | CR0_PG)) && sysr(&m, X86_SR_CR4) == 0, "CR0.PE/PG and CR4 clear");
    EQ64(sysr(&m, X86_SR_DR7), 0x400, "DR7");
    EQ64(msr(&m, MSR_SMBASE), SMBASE, "IA32_SMBASE in SMM");
    /* the save state */
    const uint64_t ss = SMBASE + 0x8000;
    EQ64(rd(&m, ss + 0x7EF8, 4), SMBASE, "save: SMBASE");
    EQ64(rd(&m, ss + 0x7EFC, 4), 0x30064, "save: revision");
    EQ64(rd(&m, ss + 0x7FD8, 8), CODE, "save: RIP");
    EQ64(rd(&m, ss + 0x7F5C, 8), 0x1111111111111111ull, "save: RAX");
    EQ64(rd(&m, ss + 0x7F94, 8), 0x8888888888888888ull, "save: RDI");
    EQ64(rd(&m, ss + 0x7F7C, 8), STACK, "save: RSP");
    EQ64(rd(&m, ss + 0x7F1C, 8), 0x1111111111111111ull * 16, "save: R15");
    EQ64(rd(&m, ss + 0x7F54, 8), 0x9999999999999999ull, "save: R8");
    EQ64(rd(&m, ss + 0x7FE8, 8), RFLAGS_IF | RFLAGS_CF | 2, "save: RFLAGS");
    EQ64(rd(&m, ss + 0x7FF8, 8), cr0, "save: CR0");
    EQ64(rd(&m, ss + 0x7FF0, 8), 0x12345000, "save: CR3");
    EQ64(rd(&m, ss + 0x7E40, 4), CR4_OSFXSR, "save: CR4");
    EQ64(rd(&m, ss + 0x7FC8, 8), 0x401, "save: DR7");
    EQ64(rd(&m, ss + 0x7FAC, 4), CS32, "save: CS selector");
    EQ64(rd(&m, ss + 0x7FB0, 4), DS32, "save: SS selector");
    EQ64(rd(&m, ss + 0x7FC4, 4), TSSSEL, "save: TR selector");
    EQ64(rd(&m, ss + 0x7E8C, 4), GDT, "save: GDT base");
    EQ64(rd(&m, ss + 0x7E94, 4), IDT, "save: IDT base");
    EQ64(rd(&m, ss + 0x7F02, 2), 0, "save: no auto-HALT");
    EQ64(rd(&m, ss + 0x7FA4, 4), 0, "save: IOMisc, no I/O");

    /* held in SMM: an NMI and a second SMI; INIT */
    line(&m, UAM_X86_IRQ_NMI, 1);
    line(&m, UAM_X86_IRQ_SMI, 0);
    line(&m, UAM_X86_IRQ_SMI, 1);
    line(&m, UAM_X86_IRQ_INIT, 1);
    CHECK(!take(&m), "NMI, SMI and INIT are held in SMM");
    EQ64(m.cpu.pc, SMBASE + 0x8000, "still in the handler");
    line(&m, UAM_X86_IRQ_INIT, 0);
    st(&m)->init_pending = false;

    /* the handler changes RAX and relocates SMBASE; RSM */
    wr(&m, ss + 0x7F5C, 8, 0xABCD);
    wr(&m, ss + 0x7EF8, 4, 0x40000);
    PUT(&m, 0x48000, 0x0F, 0xAA);
    uam_exec_run(&m.exec, 1);
    EQ64(p.smm_off, 1, "SMIACT# off");
    CHECK(!x86_priv_smm(m.cpu.priv) && x86_priv_mode(m.cpu.priv) == X86_MODE_PROT, "RSM: protected mode again");
    EQ64(m.cpu.pc, CODE, "RSM: back at the RIP");
    EQ64(gpr(&m, X86_RAX), 0xABCD, "RSM: RAX from the save state");
    EQ64(gpr(&m, X86_R15), 0x1111111111111111ull * 16, "RSM: R15");
    CHECK(sysr(&m, X86_SR_CR0) == cr0 && sysr(&m, X86_SR_CR3) == 0x12345000 && sysr(&m, X86_SR_CR4) == CR4_OSFXSR,
          "RSM: CR0, CR3, CR4");
    EQ64(x86_seg_attr(&m.cpu, X86_SEG_CS), cs_attr, "RSM: the hidden CS attributes");
    EQ64(flags(&m), RFLAGS_IF | RFLAGS_CF | 2, "RSM: RFLAGS");
    CHECK(take(&m) && m.cpu.pc == 0x48000, "the held SMI, at the relocated SMBASE + 8000");
    uam_exec_run(&m.exec, 1);
    EQ64(m.cpu.pc, CODE, "RSM again");
    CHECK(take(&m) && m.cpu.pc == handler(2), "then the held NMI");

    /* auto-HALT restart */
    PUT(&m, CODE, 0xF4, SPIN);
    setgpr(&m, X86_RSP, STACK);
    st(&m)->nmi_blocked = false;
    run(&m, CODE, 5);
    CHECK(m.cpu.waiting, "HLT");
    line(&m, UAM_X86_IRQ_SMI, 0);
    line(&m, UAM_X86_IRQ_SMI, 1);
    CHECK(wake(&m) && take(&m) && m.cpu.pc == 0x48000, "the SMI ends the HLT");
    EQ64(rd(&m, 0x40000 + 0xFF02, 2), 1, "save: auto-HALT");
    EQ64(rd(&m, 0x40000 + 0xFFD8, 8), CODE + 1, "save: RIP after the HLT");
    uam_exec_run(&m.exec, 1);
    EQ64(m.cpu.pc, CODE, "auto-HALT left set: back to the HLT");
    uam_exec_run(&m.exec, 5);
    CHECK(m.cpu.waiting && m.cpu.pc == CODE + 1, "halted again");
    line(&m, UAM_X86_IRQ_SMI, 0);
    line(&m, UAM_X86_IRQ_SMI, 1);
    CHECK(take(&m), "SMI");
    wr(&m, 0x40000 + 0xFF02, 2, 0);
    uam_exec_run(&m.exec, 1);
    CHECK(m.cpu.pc == CODE + 1 && !m.cpu.waiting, "auto-HALT cleared: on after the HLT");
    uam_machine_fini(&m);
}

static void test_smm_amd(bool jit)
{
    uam_machine m;
    machine(&m, NULL, jit);
    uam_x86_set_smm_map(&m.cpu, UAM_X86_SMM_MAP_AMD64);
    prot32(&m);
    PUT(&m, CODE, SPIN);
    PUT(&m, SMBASE + 0x8000, 0x0F, 0xAA);
    for (unsigned r = 0; r < 16; r++)
        setgpr(&m, r, 0x0101010101010101ull * (r + 1));
    setgpr(&m, X86_RSP, STACK);
    x86_set_rflags(&m.cpu, RFLAGS_ZF | 2);
    sysw(&m, X86_SR_CR3, 0x7000);
    sysw(&m, X86_SR_CR4, CR4_PSE);
    st(&m)->nmi_blocked = true;
    const uint64_t cr0 = sysr(&m, X86_SR_CR0);
    const uint16_t cs_attr = x86_seg_attr(&m.cpu, X86_SEG_CS);
    m.cpu.pc = CODE;
    line(&m, UAM_X86_IRQ_SMI, 1);
    CHECK(take(&m) && m.cpu.pc == SMBASE + 0x8000, "AMD64: SMI");
    const uint64_t b = SMBASE;
    EQ64(rd(&m, b + 0xFF00, 4), SMBASE, "AMD save: SMBASE");
    EQ64(rd(&m, b + 0xFEFC, 4), 0x30064, "AMD save: revision");
    EQ64(rd(&m, b + 0xFF78, 8), CODE, "AMD save: RIP");
    EQ64(rd(&m, b + 0xFF70, 8), RFLAGS_ZF | 2, "AMD save: RFLAGS");
    EQ64(rd(&m, b + 0xFFF8, 8), 0x0101010101010101ull, "AMD save: RAX");
    EQ64(rd(&m, b + 0xFFF0, 8), 0x0202020202020202ull, "AMD save: RCX");
    EQ64(rd(&m, b + 0xFFD8, 8), STACK, "AMD save: RSP");
    EQ64(rd(&m, b + 0xFF80, 8), 0x1010101010101010ull, "AMD save: R15");
    EQ64(rd(&m, b + 0xFF58, 8), cr0, "AMD save: CR0");
    EQ64(rd(&m, b + 0xFF50, 8), 0x7000, "AMD save: CR3");
    EQ64(rd(&m, b + 0xFF48, 8), CR4_PSE, "AMD save: CR4");
    EQ64(rd(&m, b + 0xFED0, 8), 0, "AMD save: EFER");
    EQ64(rd(&m, b + 0xFE10, 2), CS32, "AMD save: CS selector");
    EQ64(rd(&m, b + 0xFE12, 2), (cs_attr & 0xFFu) | (cs_attr >> 4 & 0xF00u), "AMD save: CS attributes, AMD's packing");
    EQ64(rd(&m, b + 0xFE14, 4), 0xFFFFFFFFu, "AMD save: CS limit");
    EQ64(rd(&m, b + 0xFE68, 8), GDT, "AMD save: GDTR base");
    EQ64(rd(&m, b + 0xFE64, 4), GDT_LIMIT, "AMD save: GDTR limit");
    EQ64(rd(&m, b + 0xFE88, 8), IDT, "AMD save: IDTR base");
    EQ64(rd(&m, b + 0xFECA, 1), 1, "AMD save: NMIs were blocked");
    EQ64(rd(&m, b + 0xFEC9, 1), 0, "AMD save: no auto-HALT");
    EQ64(msr(&m, MSR_AMD_SMM_BASE), SMBASE, "SMM_BASE MSR");
    wr(&m, b + 0xFFF0, 8, 0x5150);
    wr(&m, b + 0xFF00, 4, 0x60000);
    wr(&m, b + 0xFECA, 1, 0);
    uam_exec_run(&m.exec, 1);
    EQ64(m.cpu.pc, CODE, "AMD RSM: back");
    CHECK(x86_priv_mode(m.cpu.priv) == X86_MODE_PROT && x86_seg_attr(&m.cpu, X86_SEG_CS) == cs_attr,
          "AMD RSM: protected mode, CS attributes unpacked (0x%x)", x86_seg_attr(&m.cpu, X86_SEG_CS));
    EQ64(gpr(&m, X86_RCX), 0x5150, "AMD RSM: RCX from the save state");
    EQ64(sysr(&m, X86_SR_CR3), 0x7000, "AMD RSM: CR3");
    CHECK(!st(&m)->nmi_blocked, "AMD RSM: the NMI mask from the save state");
    EQ64(msr(&m, MSR_AMD_SMM_BASE), 0x60000, "the relocated SMBASE");
    uam_machine_fini(&m);
}

/* ---- MSRs and CPUID --------------------------------------------------------------- */

static bool gp(uam_machine *m, uam_status s)
{
    bool r = s == UAM_ERR_FAULT && st(m)->pend_valid && st(m)->pend_vec == X86_EXC_GP;
    st(m)->pend_valid = false;
    return r;
}

static void test_msr(void)
{
    uam_machine m;
    plat p;
    machine(&m, &p, false);
    uam_cpu *c = &m.cpu;
    uint64_t v = 0;
    /* EFER */
    CHECK(gp(&m, x86_wrmsr(c, MSR_EFER, 1u << 2)), "EFER: a reserved bit is #GP");
    CHECK(x86_wrmsr(c, MSR_EFER, EFER_LMA | EFER_SCE) == UAM_OK && msr(&m, MSR_EFER) == EFER_SCE,
          "EFER.LMA is read-only");
    /* IA32_APIC_BASE */
    CHECK(x86_wrmsr(c, MSR_APIC_BASE, 0xFED00800u) == UAM_OK, "move the APIC");
    EQ64(msr(&m, MSR_APIC_BASE), 0xFED00900u, "the BSP flag stays");
    EQ64(p.wr_val, 0xFED00900u, "the platform sees the APIC move");
    CHECK(gp(&m, x86_wrmsr(c, MSR_APIC_BASE, 0xFEE00C00u)), "EXTD without x2APIC: #GP");
    CHECK(gp(&m, x86_rdmsr(c, 0x802, &v)), "x2APIC MSR outside x2APIC mode: #GP");
    uam_x86_cpuid_info id;
    uam_x86_get_cpuid(c, &id);
    id.features = UAM_X86_FEAT_X2APIC;
    uam_x86_set_cpuid(c, &id);
    CHECK(gp(&m, x86_wrmsr(c, MSR_APIC_BASE, 0xFEE00400u)), "EXTD without EN: #GP");
    CHECK(x86_wrmsr(c, MSR_APIC_BASE, 0xFEE00C00u) == UAM_OK, "x2APIC mode");
    CHECK(gp(&m, x86_wrmsr(c, MSR_APIC_BASE, 0xFEE00800u)), "x2APIC to xAPIC directly: #GP");
    CHECK(x86_wrmsr(c, MSR_APIC_BASE, 0xFEE00000u) == UAM_OK, "x2APIC to disabled");
    /* TSC */
    CHECK(x86_wrmsr(c, MSR_TSC, 1000) == UAM_OK, "write TSC");
    m.sched.idle += 25;
    EQ64(msr(&m, MSR_TSC), 1025, "the TSC counts virtual time, idle included");
    EQ64(uam_x86_read_tsc(c), 1025, "uam_x86_read_tsc");
    EQ64(sysr(&m, X86_SR_TSC), 1025, "RDTSC");
    EQ64(msr(&m, MSR_TSC_ADJUST), 1000 - uam_sched_now(&m.sched) + 25, "IA32_TSC_ADJUST: the offset");
    /* lock, read-only, SMM-only */
    CHECK(x86_wrmsr(c, MSR_FEATURE_CONTROL, 5) == UAM_OK, "FEATURE_CONTROL, locked");
    CHECK(gp(&m, x86_wrmsr(c, MSR_FEATURE_CONTROL, 0)), "FEATURE_CONTROL locked: #GP");
    EQ64(msr(&m, MSR_MTRRCAP), 0xD08, "MTRRCAP");
    CHECK(gp(&m, x86_wrmsr(c, MSR_MTRRCAP, 0)), "MTRRCAP is read-only");
    CHECK(gp(&m, x86_rdmsr(c, MSR_SMRR_PHYSBASE, &v)), "SMRR outside SMM: #GP");
    CHECK(gp(&m, x86_wrmsr(c, MSR_SMRR_PHYSMASK, 0x800)), "SMRR write outside SMM: #GP");
    st(&m)->in_smm = true;
    CHECK(x86_wrmsr(c, MSR_SMRR_PHYSBASE, 0xFFFFFFFFFFFFFFFFull) == UAM_OK, "SMRR in SMM");
    EQ64(msr(&m, MSR_SMRR_PHYSBASE), 0x000FFFFFFFFFF0FFull, "SMRR_PHYSBASE: the defined bits");
    st(&m)->in_smm = false;
    /* segment-base aliases */
    CHECK(x86_wrmsr(c, MSR_GS_BASE, 0xFFFF800000001000ull) == UAM_OK, "GS_BASE");
    EQ64(x86_seg_base(c, X86_SEG_GS), 0xFFFF800000001000ull, "GS_BASE is GS's base");
    /* the platform, then the policy */
    EQ64(msr(&m, 0xC0DE), 0xFEED, "an MSR the platform answers");
    CHECK(x86_wrmsr(c, 0xC0DE, 7) == UAM_OK && p.wr_val == 7, "and takes");
    CHECK(x86_wrmsr(c, 0x1234, 0x55) == UAM_OK && msr(&m, 0x1234) == 0x55, "store policy: read back");
    EQ64(msr(&m, 0x1235), 0, "store policy: unwritten reads 0");
    CHECK(x86_wrmsr(c, MSR_EFER, EFER_SCE) == UAM_OK && msr(&m, MSR_EFER) == EFER_SCE, "EFER is the core's");
    uam_x86_set_msr_policy(c, UAM_X86_MSR_GP, false);
    CHECK(gp(&m, x86_rdmsr(c, 0x1235, &v)), "#GP policy: a missing MSR");
    CHECK(gp(&m, x86_wrmsr(c, 0x1236, 1)), "#GP policy: a missing MSR written");
    EQ64(msr(&m, 0x1234), 0x55, "#GP policy: one stored before still reads");
    CHECK(x86_wrmsr(c, MSR_MTRR_PHYSBASE0 + 3, 0x66) == UAM_OK && msr(&m, MSR_MTRR_PHYSBASE0 + 3) == 0x66,
          "#GP policy: MTRRs are storage");
    CHECK(gp(&m, x86_rdmsr(c, MSR_AMD_SMM_BASE, &v)), "the AMD SMM_BASE MSR is missing with the Intel map");
    CHECK(gp(&m, x86_rdmsr(c, MSR_SMBASE, &v)), "IA32_SMBASE is missing outside SMM");
    uam_machine_fini(&m);
}

static void test_cpuid(void)
{
    uam_machine m;
    machine(&m, NULL, false);
    uint32_t r[4];
    char s[13] = { 0 };
    x86_cpuid(&m.cpu, 0, 0, r);
    memcpy(s, &r[1], 4); memcpy(s + 4, &r[3], 4); memcpy(s + 8, &r[2], 4);
    CHECK(strcmp(s, "UAMx86RefCPU") == 0, "default vendor: %s", s);
    EQ64(r[0], 0xB, "max leaf");
    x86_cpuid(&m.cpu, 1, 0, r);
    EQ64(r[0], 0x600, "default signature: family 6");
    CHECK(!(r[3] & 1u << 28) && !(r[2] & 1u << 21), "one processor: no HTT, no x2APIC");
    CHECK((r[3] & (1u << 25 | 1u << 26)) == (1u << 25 | 1u << 26) && (r[2] & 1u << 30), "SSE SSE2 RDRAND");
    x86_cpuid(&m.cpu, 0x80000008, 0, r);
    EQ64(r[0], 39 | 48u << 8, "39 physical, 48 linear address bits");
    x86_cpuid(&m.cpu, 0x80000001, 0, r);
    EQ64(r[3] & (1u << 29 | 1u << 20), 1u << 29 | 1u << 20, "LM and NX");

    uam_x86_cpuid_info id = { .signature = 0x00A20F10, .logical_cpus = 6, .phys_addr_bits = 48,
                              .initial_apic_id = 5, .features = UAM_X86_FEAT_X2APIC | UAM_X86_FEAT_TSC_DEADLINE };
    memcpy(id.vendor, "AuthenticAMDxxxx", 12);
    strcpy(id.brand, "A brand string of more than sixteen characters");
    uam_x86_set_cpuid(&m.cpu, &id);
    x86_cpuid(&m.cpu, 0, 0, r);
    memcpy(s, &r[1], 4); memcpy(s + 4, &r[3], 4); memcpy(s + 8, &r[2], 4);
    CHECK(strcmp(s, "AuthenticAMD") == 0, "the board's vendor: %s", s);
    x86_cpuid(&m.cpu, 1, 0, r);
    EQ64(r[0], 0x00A20F10, "the board's signature");
    EQ64(r[1] >> 16, 5u << 8 | 6, "EBX: initial APIC id 5, 6 logical processors");
    CHECK((r[3] & 1u << 28) && (r[2] & 1u << 21) && (r[2] & 1u << 24), "HTT, x2APIC, TSC-deadline");
    x86_cpuid(&m.cpu, 0xB, 1, r);
    CHECK(r[0] == 3 && r[1] == 6 && r[3] == 5, "leaf B level 1: shift 3, 6 processors, x2APIC id 5");
    x86_cpuid(&m.cpu, 0x80000003, 0, r);
    CHECK(memcmp(r, "f more than sixt", 16) == 0, "brand, the second 16 characters");
    x86_cpuid(&m.cpu, 0x80000008, 0, r);
    EQ64(r[0] & 0xFF, 48, "the board's physical address width");
    uam_machine_fini(&m);
}

/* ---- ports and A20M# ---------------------------------------------------------------- */

static void test_ports_a20(bool jit)
{
    uam_machine m;
    machine(&m, NULL, jit);
    wr(&m, uam_x86_pio(0x3F8), 1, 0xA5);
    PUT(&m, CODE, 0xB0, 0x5A, 0xE6, 0x80,              /* mov al, 5Ah; out 80h, al */
                  0xBA, 0xF8, 0x03, 0xEC, SPIN);        /* mov dx, 3F8h; in al, dx */
    run(&m, CODE, 10);
    EQ64(rd(&m, uam_x86_pio(0x80), 1), 0x5A, "OUT 80h: the aperture + 80h");
    EQ64(gpr(&m, X86_RAX) & 0xFF, 0xA5, "IN from 3F8h");

    /* A20M# */
    wr(&m, 0x000010, 1, 0x11);
    wr(&m, 0x100010, 1, 0x22);
    PUT(&m, CODE, 0xB8, 0xFF, 0xFF, 0x8E, 0xD8, 0xA0, 0x20, 0x00, SPIN);   /* mov ds, FFFFh; mov al, [20h] */
    run(&m, CODE, 10);
    EQ64(gpr(&m, X86_RAX) & 0xFF, 0x22, "FFFF:0020 is 100010h");
    line(&m, UAM_X86_IRQ_A20M, 1);
    run(&m, CODE, 10);
    EQ64(gpr(&m, X86_RAX) & 0xFF, 0x11, "with A20M#, 000010h");
    st(&m)->in_smm = true;
    x86_update_priv(&m.cpu);
    CHECK(!(m.cpu.priv & X86_PRIV_A20M), "A20M# is ignored in SMM");
    st(&m)->in_smm = false;
    line(&m, UAM_X86_IRQ_A20M, 0);
    x86_update_priv(&m.cpu);
    uam_machine_fini(&m);

    /* in 64-bit mode with paging the ports are still physical */
    machine(&m, NULL, jit);
    long64(&m);
    PUT(&m, CODE, 0xB0, 0x77, 0xE6, 0x81, SPIN);
    run(&m, CODE, 10);
    EQ64(rd(&m, uam_x86_pio(0x81), 1), 0x77, "64-bit OUT 81h, paging on");
    uam_machine_fini(&m);
}

/* ---- the debugger view, the dump, snapshots ----------------------------------------- */

static void test_gdb(void)
{
    uam_machine m;
    machine(&m, NULL, false);
    const uam_gdb_arch *g = m.cpu.isa->gdb;
    CHECK(g && g->n_regs == 40 && g->reg_bytes == 8, "40 registers of 8 bytes");
    CHECK(strstr(g->target_xml, "<architecture>i386:x86-64</architecture>"), "target description");
    uint64_t v = 0;
    setgpr(&m, X86_RBX, 0xB0B0);
    setgpr(&m, X86_RSP, 0x5555);
    setgpr(&m, X86_R13, 0x1313);
    CHECK(g->read_reg(&m.cpu, 1, &v) && v == 0xB0B0, "gdb 1 is rbx");
    CHECK(g->read_reg(&m.cpu, 7, &v) && v == 0x5555, "gdb 7 is rsp");
    CHECK(g->read_reg(&m.cpu, 13, &v) && v == 0x1313, "gdb 13 is r13");
    x86_seg_set_real(&m.cpu, X86_SEG_CS, 0x1000);
    m.cpu.pc = 0x10123;
    CHECK(g->read_reg(&m.cpu, 16, &v) && v == 0x10123, "gdb rip is the linear pc");
    CHECK(g->read_reg(&m.cpu, 18, &v) && v == 0x1000, "gdb 18 is cs");
    CHECK(g->read_reg(&m.cpu, 17, &v) && v == 2, "eflags");
    CHECK(g->read_reg(&m.cpu, 32, &v) && v == 0x37F, "fctrl");
    CHECK(!g->read_reg(&m.cpu, 40, &v), "no register 40");
    CHECK(g->write_reg(&m.cpu, 3, 0xD0D0) && gpr(&m, X86_RDX) == 0xD0D0, "gdb write rdx");
    CHECK(g->write_reg(&m.cpu, 17, 0x246) && flags(&m) == 0x246, "gdb write eflags");
    CHECK(!g->write_reg(&m.cpu, 18, 0x2000), "selectors are not written");
    /* st(i) is relative to TOP */
    uam_cpu_reg_write(&m.cpu, X86_OFF_FSW, 16, 3u << 11);
    static const uint8_t one[10] = { 0, 0, 0, 0, 0, 0, 0, 0x80, 0xFF, 0x3F };   /* 1.0 */
    uint8_t buf[10];
    CHECK(g->write_wide(&m.cpu, 24 + 1, one), "write st1");
    CHECK(st(&m)->fpr[4].m == 1ull << 63 && st(&m)->fpr[4].se == 0x3FFF, "st1 is physical register 4");
    CHECK(g->read_wide(&m.cpu, 24 + 1, buf) && memcmp(buf, one, 10) == 0, "read st1");
    CHECK(!g->read_wide(&m.cpu, 32, buf), "fctrl is not wide");

    char *out = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&out, &len);
    m.cpu.isa->dump_state(&m.cpu, f);
    st(&m)->in_smm = true;
    m.cpu.isa->dump_state(&m.cpu, f);
    st(&m)->in_smm = false;
    fclose(f);
    CHECK(out && strstr(out, "rip=0000000000000123") && strstr(out, "cs=1000 base=0000000000010000"),
          "dump: rip and CS");
    CHECK(out && strstr(out, "cr0=60000010"), "dump: CR0");
    CHECK(out && strstr(out, " SMM\n"), "dump: SMM");
    free(out);
    uam_machine_fini(&m);
}

static void test_snapshot(void)
{
    uam_machine m;
    machine(&m, NULL, false);
    prot32(&m);
    uam_cpu *c = &m.cpu;
    sysw(&m, X86_SR_CR3, 0x55000);
    sysw(&m, X86_SR_CR4, CR4_PSE | CR4_OSFXSR);
    sysw(&m, X86_SR_CR2, 0xDEAD0000);
    sysw(&m, X86_SR_CR8, 9);
    sysw(&m, X86_SR_DR0, 0x4444);
    CHECK(x86_wrmsr(c, MSR_EFER, EFER_SCE | EFER_NXE) == UAM_OK, "EFER");
    CHECK(x86_wrmsr(c, MSR_LSTAR, 0xFFFF800012345678ull) == UAM_OK, "LSTAR");
    CHECK(x86_wrmsr(c, MSR_TSC_AUX, 3) == UAM_OK, "TSC_AUX");
    CHECK(x86_wrmsr(c, MSR_TSC, 77777) == UAM_OK, "TSC");
    CHECK(x86_wrmsr(c, 0x4321, 0x99) == UAM_OK, "a stored MSR");
    CHECK(x86_ltr(c, TSSSEL) == UAM_OK, "ltr");
    st(&m)->smbase = 0x70000;
    st(&m)->fpr[5] = (x86_f80){ 0xC000000000000000ull, 0x4000 };
    st(&m)->fpu_ip = 0x1234;                                     /* FNSTENV's FIP, FDP, FOP */
    st(&m)->fpu_dp = 0x5678;
    st(&m)->fpu_op = 0x5D9;
    line(&m, UAM_X86_IRQ_A20M, 1);
    line(&m, UAM_X86_IRQ_NMI, 1);
    const unsigned priv = m.cpu.priv;
    const uint64_t tsc = msr(&m, MSR_TSC);

    uam_snapshot *s = NULL;
    CHECK(uam_snapshot_take(&m, &s) == UAM_OK && s, "take");

    uam_machine_reset(&m, CODE);
    CHECK(x86_wrmsr(c, 0x4321, 0) == UAM_OK, "clobber the stored MSR");
    CHECK(x86_wrmsr(c, 0x4322, 1) == UAM_OK, "and store another");
    line(&m, UAM_X86_IRQ_A20M, 0);
    line(&m, UAM_X86_IRQ_NMI, 0);
    st(&m)->nmi_pending = false;
    m.sched.idle += 1000;

    CHECK(uam_snapshot_restore(&m, s) == UAM_OK, "restore");
    EQ64(m.cpu.priv, priv, "the mode recomputed: protected, A20M#");
    EQ64(sysr(&m, X86_SR_CR3), 0x55000, "CR3");
    EQ64(sysr(&m, X86_SR_CR4), CR4_PSE | CR4_OSFXSR, "CR4");
    EQ64(sysr(&m, X86_SR_CR2), 0xDEAD0000, "CR2");
    EQ64(sysr(&m, X86_SR_CR8), 9, "CR8");
    EQ64(sysr(&m, X86_SR_DR0), 0x4444, "DR0");
    CHECK(sysr(&m, X86_SR_GDTR_BASE) == GDT && sysr(&m, X86_SR_GDTR_LIMIT) == GDT_LIMIT, "GDTR");
    CHECK(sysr(&m, X86_SR_IDTR_BASE) == IDT && sysr(&m, X86_SR_IDTR_LIMIT) == 256 * 8 - 1, "IDTR");
    CHECK(sysr(&m, X86_SR_TR) == TSSSEL && st(&m)->tr.base == TSS && st(&m)->tr.limit == 0x67, "TR");
    EQ64(msr(&m, MSR_EFER), EFER_SCE | EFER_NXE, "EFER");
    EQ64(msr(&m, MSR_LSTAR), 0xFFFF800012345678ull, "LSTAR");
    EQ64(msr(&m, MSR_TSC_AUX), 3, "TSC_AUX");
    EQ64(msr(&m, MSR_TSC), tsc, "the TSC: the snapshot's time and offset");
    EQ64(msr(&m, 0x4321), 0x99, "the stored MSR");
    EQ64(msr(&m, 0x4322), 0, "the one stored after is gone");
    EQ64(st(&m)->smbase, 0x70000, "SMBASE");
    CHECK(st(&m)->fpr[5].m == 0xC000000000000000ull && st(&m)->fpr[5].se == 0x4000, "x87 register 5");
    EQ64(st(&m)->fpu_ip, 0x1234, "the x87 last instruction pointer");
    EQ64(st(&m)->fpu_dp, 0x5678, "the x87 last operand pointer");
    EQ64(st(&m)->fpu_op, 0x5D9, "the x87 last opcode");
    EQ64(m.cpu.isa->irq_lines(c), 1u << UAM_X86_IRQ_A20M | 1u << UAM_X86_IRQ_NMI, "the line levels");
    CHECK(take(&m) && m.cpu.pc == handler(2), "the pending NMI, through the restored IDT");
    uam_snapshot_free(s);
    uam_machine_fini(&m);
}

int main(void)
{
    test_init_sipi();
    test_intr();
    test_msr();
    test_cpuid();
    test_gdb();
    test_snapshot();
    for (int jit = 0; jit < 2; jit++) {
        test_reset(jit);
        test_nmi(jit);
        test_shadow(jit);
        test_shadow_cross(jit);
        test_hlt(jit);
        test_real_exceptions(jit);
        test_prot_exceptions(jit);
        test_long_exceptions(jit);
        test_smm_intel(jit);
        test_smm_amd(jit);
        test_ports_a20(jit);
    }
    printf("test_x86: %d/%d checks passed\n", n_checks - n_failed, n_checks);
    return n_failed ? 1 : 0;
}
