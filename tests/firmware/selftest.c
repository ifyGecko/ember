/*
 * selftest.c - Bare-metal RV32IMAC self-test for the UAM `rv32-ref` board.
 *
 * Exercises the emulator end to end: UART output, base ISA, M/A/C extensions,
 * Zicsr, synchronous exceptions (with precise mepc/mcause/mtval), timer,
 * software and external (PLIC + UART) interrupts, vectored mtvec, WFI wake
 * semantics, and self-modifying code with fence.i.
 *
 * Output is deterministic (no timestamps or counters are printed), so the
 * console log can be compared between runs and execution backends.
 * The result is reported through the sifive,test finisher.
 */
#include <stddef.h>
#include <stdint.h>

/* ---- board ------------------------------------------------------------- */

#define UART0       0x10000000u
#define CLINT       0x02000000u
#define PLIC        0x0C000000u
#define UART0_IRQ   10u

#define REG8(a)  (*(volatile uint8_t *)(uintptr_t)(a))
#define REG32(a) (*(volatile uint32_t *)(uintptr_t)(a))

#define CLINT_MSIP        REG32(CLINT + 0x0000)
#define CLINT_MTIMECMP_LO REG32(CLINT + 0x4000)
#define CLINT_MTIMECMP_HI REG32(CLINT + 0x4004)
#define CLINT_MTIME_LO    REG32(CLINT + 0xBFF8)
#define CLINT_MTIME_HI    REG32(CLINT + 0xBFFC)

#define PLIC_PRIORITY(s)  REG32(PLIC + 4u * (s))
#define PLIC_ENABLE0      REG32(PLIC + 0x2000)
#define PLIC_THRESHOLD0   REG32(PLIC + 0x200000)
#define PLIC_CLAIM0       REG32(PLIC + 0x200004)

#define UART_THR REG8(UART0 + 0)
#define UART_IER REG8(UART0 + 1)
#define UART_IIR REG8(UART0 + 2)
#define UART_LSR REG8(UART0 + 5)

/* ---- freestanding support ---------------------------------------------- */

void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);

void *memset(void *d, int c, size_t n)
{
    unsigned char *p = d;
    while (n--)
        *p++ = (unsigned char)c;
    return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *p = d;
    const unsigned char *q = s;
    while (n--)
        *p++ = *q++;
    return d;
}

#define csr_read(csr) ({ uint32_t v_; __asm__ volatile("csrr %0, " #csr : "=r"(v_)); v_; })
#define csr_write(csr, v) __asm__ volatile("csrw " #csr ", %0" :: "r"((uint32_t)(v)))
#define csr_set(csr, v)   __asm__ volatile("csrs " #csr ", %0" :: "r"((uint32_t)(v)))
#define csr_clear(csr, v) __asm__ volatile("csrc " #csr ", %0" :: "r"((uint32_t)(v)))

/* Execute a two-operand R-type instruction on runtime values. */
#define RR(op, a, b) ({ uint32_t r_; __asm__ volatile(op " %0, %1, %2" : "=r"(r_) : "r"(a), "r"(b)); r_; })

#define MIE_MSIE (1u << 3)
#define MIE_MTIE (1u << 7)
#define MIE_MEIE (1u << 11)
#define MSTATUS_MIE (1u << 3)

/* ---- console ----------------------------------------------------------- */

static void putc_(char c)
{
    while (!(UART_LSR & 0x20))
        ;
    UART_THR = (uint8_t)c;
}

static void puts_(const char *s)
{
    while (*s) {
        if (*s == '\n')
            putc_('\r');
        putc_(*s++);
    }
}

static void puthex(uint32_t v)
{
    static const char digits[] = "0123456789abcdef";
    puts_("0x");
    for (int i = 28; i >= 0; i -= 4)
        putc_(digits[(v >> i) & 0xF]);
}

/* ---- test bookkeeping -------------------------------------------------- */

static unsigned n_pass, n_fail;
static const char *current;

static void check(int ok, const char *what, uint32_t got, uint32_t want)
{
    if (ok)
        return;
    n_fail++;
    puts_("  FAIL ");
    puts_(current);
    puts_(": ");
    puts_(what);
    puts_(" got ");
    puthex(got);
    puts_(" want ");
    puthex(want);
    puts_("\n");
}

#define EXPECT_EQ(got, want, what) do { uint32_t g_ = (got), w_ = (want); check(g_ == w_, what, g_, w_); } while (0)

static void run_test(const char *name, void (*fn)(void))
{
    unsigned before = n_fail;
    current = name;
    fn();
    if (n_fail == before) {
        n_pass++;
        puts_("[ ok ] ");
    } else {
        puts_("[FAIL] ");
    }
    puts_(name);
    puts_("\n");
}

/* Inputs the compiler cannot constant-fold. */
static volatile uint32_t V0 = 0, V1 = 1, VM1 = 0xFFFFFFFFu, VMIN = 0x80000000u, V7 = 7, V3 = 3;

/* ---- trap handling ----------------------------------------------------- */

typedef struct trap_frame { uint32_t x[32]; } trap_frame;

static volatile uint32_t trap_count, last_cause, last_epc, last_tval;
static volatile uint32_t timer_ticks, soft_irqs, ext_irqs, ext_src, ext_iir;
static volatile int resume_at_ra; /* fetch-fault test: return to caller */

static uint32_t insn_len(uint32_t pc)
{
    return ((*(volatile uint16_t *)(uintptr_t)pc) & 3) == 3 ? 4 : 2;
}

uint32_t trap_handler(trap_frame *f);
uint32_t trap_handler(trap_frame *f)
{
    uint32_t cause = csr_read(mcause), epc = csr_read(mepc), tval = csr_read(mtval);

    if (cause & 0x80000000u) {
        switch (cause & 0xFF) {
        case 7: /* machine timer: disarm */
            timer_ticks++;
            CLINT_MTIMECMP_HI = 0xFFFFFFFFu;
            CLINT_MTIMECMP_LO = 0xFFFFFFFFu;
            break;
        case 3: /* machine software */
            soft_irqs++;
            CLINT_MSIP = 0;
            break;
        case 11: { /* machine external: PLIC claim/complete */
            uint32_t src = PLIC_CLAIM0;
            ext_src = src;
            if (src == UART0_IRQ) {
                ext_iir = UART_IIR; /* acknowledges THRE */
                UART_IER = 0;
            }
            ext_irqs++;
            PLIC_CLAIM0 = src;
            break;
        }
        default:
            break;
        }
        return epc;
    }

    trap_count++;
    last_cause = cause;
    last_epc = epc;
    last_tval = tval;
    if (resume_at_ra) {
        resume_at_ra = 0;
        return f->x[1];
    }
    return epc + insn_len(epc);
}

/* ---- tests ------------------------------------------------------------- */

static void test_alu(void)
{
    EXPECT_EQ(V7 + V3, 10, "add");
    EXPECT_EQ(V3 - V7, (uint32_t)-4, "sub");
    EXPECT_EQ(V7 & V3, 3, "and");
    EXPECT_EQ(V7 | 8, 15, "or");
    EXPECT_EQ(V7 ^ V3, 4, "xor");
    EXPECT_EQ(V1 << 31, 0x80000000u, "sll");
    EXPECT_EQ(RR("sll", V1, 33u), 2, "sll masks amount");
    EXPECT_EQ(VMIN >> 31, 1, "srl");
    EXPECT_EQ((uint32_t)((int32_t)VMIN >> 31), 0xFFFFFFFFu, "sra");
    EXPECT_EQ(RR("sra", VMIN, 36u), 0xF8000000u, "sra masks amount");
    EXPECT_EQ(RR("slt", VM1, V0), 1, "slt");
    EXPECT_EQ(RR("sltu", VM1, V0), 0, "sltu");
    uint32_t lui;
    __asm__ volatile("lui %0, 0xABCDE" : "=r"(lui));
    EXPECT_EQ(lui, 0xABCDE000u, "lui");
    uint32_t pc1, pc2;
    __asm__ volatile("auipc %0, 0\n auipc %1, 1" : "=r"(pc1), "=r"(pc2));
    EXPECT_EQ(pc2 - pc1, 0x1004, "auipc");
}

static void test_branches(void)
{
    int taken = 0;
    for (volatile int i = 0; i < 10; i++)
        taken += (i & 1) ? 1 : 0;
    EXPECT_EQ(taken, 5, "loop with branches");
    EXPECT_EQ((int32_t)VM1 < (int32_t)V0, 1, "blt signed");
    EXPECT_EQ(VM1 < V0, 0, "bltu unsigned");
    EXPECT_EQ((int32_t)V0 >= (int32_t)VM1, 1, "bge");
    EXPECT_EQ(V0 >= VM1, 0, "bgeu");
}

static void test_muldiv(void)
{
    EXPECT_EQ(RR("mul", V7, V3), 21, "mul");
    EXPECT_EQ(RR("mul", VM1, VM1), 1, "mul -1*-1");
    EXPECT_EQ(RR("mulh", VM1, VM1), 0, "mulh -1*-1");
    EXPECT_EQ(RR("mulh", VMIN, 2u), 0xFFFFFFFFu, "mulh MIN*2");
    EXPECT_EQ(RR("mulhu", VM1, VM1), 0xFFFFFFFEu, "mulhu");
    EXPECT_EQ(RR("mulhsu", VM1, VM1), 0xFFFFFFFFu, "mulhsu -1*(2^32-1)");
    EXPECT_EQ(RR("mulhsu", 2u, VM1), 1, "mulhsu 2*(2^32-1)");
    EXPECT_EQ(RR("div", (uint32_t)-7, 2u), (uint32_t)-3, "div");
    EXPECT_EQ(RR("rem", (uint32_t)-7, 2u), (uint32_t)-1, "rem");
    EXPECT_EQ(RR("divu", V7, V3), 2, "divu");
    EXPECT_EQ(RR("remu", V7, V3), 1, "remu");
    EXPECT_EQ(RR("div", V7, V0), 0xFFFFFFFFu, "div by zero");
    EXPECT_EQ(RR("divu", V7, V0), 0xFFFFFFFFu, "divu by zero");
    EXPECT_EQ(RR("rem", V7, V0), 7, "rem by zero");
    EXPECT_EQ(RR("remu", V7, V0), 7, "remu by zero");
    EXPECT_EQ(RR("div", VMIN, VM1), 0x80000000u, "div overflow");
    EXPECT_EQ(RR("rem", VMIN, VM1), 0, "rem overflow");
}

static void test_loadstore(void)
{
    static volatile uint8_t buf[16] __attribute__((aligned(4)));
    for (int i = 0; i < 16; i++)
        buf[i] = (uint8_t)(0xF0 + i);
    volatile int8_t *sb = (volatile int8_t *)buf;
    volatile int16_t *sh = (volatile int16_t *)buf;
    volatile uint16_t *uh = (volatile uint16_t *)buf;
    volatile uint32_t *w = (volatile uint32_t *)buf;
    EXPECT_EQ((uint32_t)(int32_t)sb[0], 0xFFFFFFF0u, "lb sign-extends");
    EXPECT_EQ((uint32_t)buf[1], 0xF1, "lbu zero-extends");
    EXPECT_EQ((uint32_t)(int32_t)sh[1], 0xFFFFF3F2u, "lh sign-extends");
    EXPECT_EQ((uint32_t)uh[1], 0xF3F2, "lhu zero-extends");
    EXPECT_EQ(w[1], 0xF7F6F5F4u, "lw little-endian");
    buf[8] = 0x11;
    uh[5] = 0x2233;
    EXPECT_EQ(w[2], 0x2233F911u, "sb/sh partial stores");
}

extern uint32_t rvc_workout(void);

static void test_compressed(void)
{
    EXPECT_EQ(rvc_workout(), 0x10D5, "rvc workout checksum");
}

static void test_atomics(void)
{
    static volatile uint32_t word __attribute__((aligned(4)));
    uint32_t old, rc, val;

    word = 41;
    __asm__ volatile("lr.w %0, (%2)\n addi %0, %0, 1\n sc.w %1, %0, (%2)"
                     : "=&r"(val), "=&r"(rc) : "r"(&word) : "memory");
    EXPECT_EQ(rc, 0, "sc.w after lr.w succeeds");
    EXPECT_EQ(word, 42, "sc.w stored");
    __asm__ volatile("sc.w %0, %1, (%2)" : "=&r"(rc) : "r"(7u), "r"(&word) : "memory");
    EXPECT_EQ(rc, 1, "sc.w without reservation fails");
    EXPECT_EQ(word, 42, "failed sc.w does not store");

#define AMO(op, v) ({ uint32_t o_; __asm__ volatile(op " %0, %1, (%2)" : "=r"(o_) : "r"(v), "r"(&word) : "memory"); o_; })
    word = 10;
    old = AMO("amoswap.w", 5u);  EXPECT_EQ(old, 10, "amoswap old"); EXPECT_EQ(word, 5, "amoswap new");
    old = AMO("amoadd.w", 3u);   EXPECT_EQ(old, 5, "amoadd old");   EXPECT_EQ(word, 8, "amoadd new");
    old = AMO("amoand.w", 0xCu); EXPECT_EQ(word, 8, "amoand");      (void)old;
    AMO("amoor.w", 1u);          EXPECT_EQ(word, 9, "amoor");
    AMO("amoxor.w", 0xFu);       EXPECT_EQ(word, 6, "amoxor");
    AMO("amomin.w", VM1);        EXPECT_EQ(word, 0xFFFFFFFFu, "amomin signed");
    AMO("amomax.w", 3u);         EXPECT_EQ(word, 3, "amomax signed");
    AMO("amominu.w", VM1);       EXPECT_EQ(word, 3, "amominu");
    AMO("amomaxu.w", VM1);       EXPECT_EQ(word, 0xFFFFFFFFu, "amomaxu");
#undef AMO
}

static void test_csr(void)
{
    csr_write(mscratch, 0x12345678u);
    EXPECT_EQ(csr_read(mscratch), 0x12345678u, "mscratch rw");
    csr_set(mscratch, 0x0F);
    EXPECT_EQ(csr_read(mscratch), 0x1234567Fu, "csrrs");
    csr_clear(mscratch, 0xF0);
    EXPECT_EQ(csr_read(mscratch), 0x1234560Fu, "csrrc");
    uint32_t old;
    __asm__ volatile("csrrwi %0, mscratch, 21" : "=r"(old));
    EXPECT_EQ(old, 0x1234560Fu, "csrrwi returns old");
    EXPECT_EQ(csr_read(mscratch), 21, "csrrwi writes zimm");

    uint32_t misa = csr_read(misa);
    EXPECT_EQ(misa >> 30, 1, "misa.MXL = 32");
    uint32_t imac = (1u << 8) | (1u << 12) | (1u << 0) | (1u << 2);
    EXPECT_EQ(misa & imac, imac, "misa has I, M, A, C");
    EXPECT_EQ(csr_read(mhartid), 0, "mhartid");

    uint32_t a = csr_read(minstret);
    for (volatile int i = 0; i < 10; i++)
        ;
    uint32_t b = csr_read(minstret);
    EXPECT_EQ(b - a > 10, 1, "minstret advances");
}

static void expect_trap(uint32_t cause, uint32_t epc, uint32_t tval, const char *what)
{
    EXPECT_EQ(trap_count, 1, what);
    EXPECT_EQ(last_cause, cause, what);
    EXPECT_EQ(last_epc, epc, what);
    EXPECT_EQ(last_tval, tval, what);
    trap_count = 0;
}

/* Labels give the expected mepc of each faulting instruction. */
static void test_exceptions(void)
{
    uint32_t pc;
    trap_count = 0;

    __asm__ volatile("la %0, 1f\n 1: ecall" : "=r"(pc) :: "memory");
    expect_trap(11, pc, 0, "ecall from M");

    __asm__ volatile("la %0, 1f\n 1: .4byte 0x00100073" : "=r"(pc) :: "memory"); /* ebreak */
    expect_trap(3, pc, pc, "ebreak");

    __asm__ volatile("la %0, 1f\n 1: .4byte 0xc0001073" : "=r"(pc) :: "memory"); /* unimp: csrw cycle */
    expect_trap(2, pc, 0xc0001073u, "illegal (write read-only CSR)");

    __asm__ volatile("la %0, 1f\n 1: .2byte 0x0000" : "=r"(pc) :: "memory"); /* c.unimp */
    expect_trap(2, pc, 0, "illegal compressed");

    __asm__ volatile("la %0, 1f\n 1: .4byte 0x0000000b" : "=r"(pc) :: "memory"); /* custom-0 opcode */
    expect_trap(2, pc, 0x0b, "illegal opcode");

    uint32_t bad = 0xF0000000u, sink;
    __asm__ volatile("la %0, 1f\n 1: lw %1, 0(%2)" : "=&r"(pc), "=r"(sink) : "r"(bad) : "memory");
    expect_trap(5, pc, bad, "load access fault");
    (void)sink;

    __asm__ volatile("la %0, 1f\n 1: sw zero, 4(%1)" : "=&r"(pc) : "r"(bad) : "memory");
    expect_trap(7, pc, bad + 4, "store access fault");

    static volatile uint32_t w[2] __attribute__((aligned(8)));
    uint32_t mis = (uint32_t)(uintptr_t)&w[0] + 1;
    __asm__ volatile("la %0, 1f\n 1: amoadd.w %1, %2, (%3)" : "=&r"(pc), "=r"(sink) : "r"(1u), "r"(mis) : "memory");
    expect_trap(6, pc, mis, "misaligned amo");
    __asm__ volatile("la %0, 1f\n 1: lr.w %1, (%2)" : "=&r"(pc), "=r"(sink) : "r"(mis) : "memory");
    expect_trap(4, pc, mis, "misaligned lr");

    resume_at_ra = 1;
    ((void (*)(void))(uintptr_t)bad)();
    expect_trap(1, bad, bad, "instruction access fault");
}

static uint64_t mtime(void)
{
    uint32_t hi, lo;
    do {
        hi = CLINT_MTIME_HI;
        lo = CLINT_MTIME_LO;
    } while (hi != CLINT_MTIME_HI);
    return ((uint64_t)hi << 32) | lo;
}

static void arm_timer(uint64_t delta)
{
    uint64_t when = mtime() + delta;
    CLINT_MTIMECMP_HI = 0xFFFFFFFFu; /* avoid a spurious match while updating */
    CLINT_MTIMECMP_LO = (uint32_t)when;
    CLINT_MTIMECMP_HI = (uint32_t)(when >> 32);
}

static void test_timer_irq(void)
{
    timer_ticks = 0;
    csr_set(mie, MIE_MTIE);
    csr_set(mstatus, MSTATUS_MIE);
    for (unsigned k = 1; k <= 3; k++) {
        arm_timer(2000);
        while (timer_ticks < k)
            __asm__ volatile("wfi");
    }
    csr_clear(mstatus, MSTATUS_MIE);
    csr_clear(mie, MIE_MTIE);
    EXPECT_EQ(timer_ticks, 3, "three timer interrupts");
    EXPECT_EQ(csr_read(mcause), 0x80000007u, "mcause = machine timer");
}

static void test_soft_irq(void)
{
    soft_irqs = 0;
    csr_set(mie, MIE_MSIE);
    csr_set(mstatus, MSTATUS_MIE);
    CLINT_MSIP = 1;
    for (volatile int i = 0; i < 100 && !soft_irqs; i++)
        ;
    csr_clear(mstatus, MSTATUS_MIE);
    csr_clear(mie, MIE_MSIE);
    EXPECT_EQ(soft_irqs, 1, "software interrupt taken once");
    EXPECT_EQ(CLINT_MSIP, 0, "handler cleared msip");
}

static void test_ext_irq(void)
{
    ext_irqs = 0;
    ext_src = 0;
    PLIC_PRIORITY(UART0_IRQ) = 1;
    PLIC_ENABLE0 = 1u << UART0_IRQ;
    PLIC_THRESHOLD0 = 0;
    csr_set(mie, MIE_MEIE);
    csr_set(mstatus, MSTATUS_MIE);
    UART_IER = 0x02; /* THR-empty interrupt: fires immediately */
    for (volatile int i = 0; i < 100 && !ext_irqs; i++)
        ;
    csr_clear(mstatus, MSTATUS_MIE);
    csr_clear(mie, MIE_MEIE);
    PLIC_ENABLE0 = 0;
    EXPECT_EQ(ext_irqs, 1, "external interrupt taken once");
    EXPECT_EQ(ext_src, UART0_IRQ, "PLIC claim returned the UART source");
    EXPECT_EQ(ext_iir & 0x0F, 0x02, "UART IIR reported THR empty");
    EXPECT_EQ(PLIC_CLAIM0, 0, "nothing left to claim");
}

extern char trap_vector_table[];
extern char trap_entry[];

static void test_vectored(void)
{
    csr_write(mtvec, (uint32_t)(uintptr_t)trap_vector_table | 1);
    EXPECT_EQ(csr_read(mtvec) & 3, 1, "mtvec vectored mode");

    /* Interrupts go to base + 4*cause. */
    soft_irqs = 0;
    csr_write(mscratch, 0xFF);
    csr_set(mie, MIE_MSIE);
    csr_set(mstatus, MSTATUS_MIE);
    CLINT_MSIP = 1;
    for (volatile int i = 0; i < 100 && !soft_irqs; i++)
        ;
    csr_clear(mstatus, MSTATUS_MIE);
    csr_clear(mie, MIE_MSIE);
    EXPECT_EQ(soft_irqs, 1, "vectored software interrupt");
    EXPECT_EQ(csr_read(mscratch), 3, "used slot 3");

    /* Exceptions always go to base. */
    trap_count = 0;
    __asm__ volatile("ecall" ::: "memory");
    EXPECT_EQ(csr_read(mscratch), 0, "exception used slot 0");
    EXPECT_EQ(trap_count, 1, "vectored ecall handled");
    trap_count = 0;

    csr_write(mtvec, (uint32_t)(uintptr_t)trap_entry);
}

static void test_wfi_wakeup(void)
{
    /* With mstatus.MIE clear, a pending enabled interrupt wakes WFI but is
     * not taken. */
    timer_ticks = 0;
    csr_clear(mstatus, MSTATUS_MIE);
    csr_set(mie, MIE_MTIE);
    arm_timer(1000);
    __asm__ volatile("wfi");
    uint32_t mip = csr_read(mip);
    csr_clear(mie, MIE_MTIE);
    CLINT_MTIMECMP_HI = 0xFFFFFFFFu;
    CLINT_MTIMECMP_LO = 0xFFFFFFFFu;
    EXPECT_EQ((mip >> 7) & 1, 1, "MTIP pending after wake");
    EXPECT_EQ(timer_ticks, 0, "interrupt not taken with MIE clear");
}

/* Self-modifying code: write `li a0, N; ret`, fence.i, call. */
static uint32_t smc_buf[4] __attribute__((aligned(16)));

static uint32_t call_generated(uint32_t n)
{
    smc_buf[0] = 0x00000513u | (n << 20); /* addi a0, zero, n */
    smc_buf[1] = 0x00008067u;             /* jalr zero, 0(ra) */
    __asm__ volatile("fence.i" ::: "memory");
    return ((uint32_t (*)(void))(uintptr_t)smc_buf)();
}

static void test_smc(void)
{
    EXPECT_EQ(call_generated(1), 1, "generated code (first)");
    EXPECT_EQ(call_generated(2), 2, "generated code rewritten");
    EXPECT_EQ(call_generated(3), 3, "generated code rewritten again");
}

/* ---- main -------------------------------------------------------------- */

int main(void);
int main(void)
{
    puts_("UAM self-test (rv32imac, rv32-ref)\n");
    run_test("alu", test_alu);
    run_test("branches", test_branches);
    run_test("muldiv", test_muldiv);
    run_test("loadstore", test_loadstore);
    run_test("compressed", test_compressed);
    run_test("atomics", test_atomics);
    run_test("csr", test_csr);
    run_test("exceptions", test_exceptions);
    run_test("timer-irq", test_timer_irq);
    run_test("soft-irq", test_soft_irq);
    run_test("ext-irq", test_ext_irq);
    run_test("vectored", test_vectored);
    run_test("wfi-wakeup", test_wfi_wakeup);
    run_test("smc", test_smc);

    puts_("result: ");
    puthex(n_pass);
    puts_(" passed, ");
    puthex(n_fail);
    puts_(" failed\n");
    puts_(n_fail ? "SELFTEST FAIL\n" : "SELFTEST PASS\n");

    /* sifive,test finisher word: 0x5555 = pass, (code << 16) | 0x3333 = fail */
    return n_fail ? (int)((n_fail << 16) | 0x3333) : 0x5555;
}
