/*
 * selftest.c - Bare-metal self-test for the UAM pic32-ref board, built once per
 * model and ISA (Makefile): mx in MIPS16e with MIPS32 handlers, mm in microMIPS,
 * mz in MIPS32 and again in microMIPS (bootisa=micromips), both with the FPU and
 * the DSP ASE.
 *
 * Exercises: the compressed ISA end to end (ALU, multiply/divide, loads and
 * stores of every width, calls, jump tables, recursion), calls between ISA modes
 * (JALX, JR to an odd address), precise exceptions with the ISA mode in EPC bit
 * 0 and in delay slots (Cause.BD), the Config registers each model reports, the
 * external interrupt controller (priorities, subpriorities, Status.IPL masking,
 * multi- and single-vector modes, computed and variable vector offsets), shadow
 * register sets (SRSCtl, RDPGPR), the core timer and software interrupts as
 * controller sources, fixed mapping translation (mx, mm) and the TLB (mz).
 */
#include <stdint.h>

#if defined(__mips16)
#define NATIVE __attribute__((nomips16, noinline))
#elif defined(__mips_micromips)
#define NATIVE __attribute__((noinline))
#else
#define NATIVE __attribute__((noinline))
#endif

#if defined(MODEL_MM) || defined(MODEL_MZ)
#define VARIABLE_OFFSET 1
#endif

#define UART0     0xbf820000u
#define UART_THR (*(volatile uint8_t *)(UART0 + 0))
#define UART_LSR (*(volatile uint8_t *)(UART0 + 5))
#define FINISHER 0xbf820100u
#define TIMER    0xbf820200u
#define TIMER_CTRL   (*(volatile uint32_t *)(TIMER + 0x0))
#define TIMER_STATUS (*(volatile uint32_t *)(TIMER + 0x4))
#define TIMER_LOAD   (*(volatile uint32_t *)(TIMER + 0x8))

#ifdef VARIABLE_OFFSET
#define INTC      0xbf810000u
#define R_INTCON  0x000u
#define R_PRISS   0x010u
#define R_INTSTAT 0x020u
#define R_IFS     0x040u
#define R_IEC     0x0C0u
#define R_IPC     0x140u
#define R_OFF     0x540u
#else
#define INTC      0xbf881000u
#define R_INTCON  0x000u
#define R_INTSTAT 0x010u
#define R_IFS     0x030u
#define R_IEC     0x060u
#define R_IPC     0x090u
#endif
#define IREG(o)  (*(volatile uint32_t *)(INTC + (o)))
#define CLR 0x4u
#define SET 0x8u
#define MVEC (1u << 12)

enum { SRC_CORE_TIMER = 0, SRC_SW0 = 1, SRC_SW1 = 2, SRC_UART = 3, SRC_TIMER = 4,
       SRC_A = 10, SRC_B = 11, SRC_C = 12 };

static void putc_(char c) { while (!(UART_LSR & 0x20)) {} UART_THR = (uint8_t)c; }
static void puts_(const char *s) { for (; *s; s++) { if (*s == '\n') putc_('\r'); putc_(*s); } }
static void puthex(uint32_t v)
{
    static const char d[] = "0123456789abcdef";
    puts_("0x");
    for (int i = 28; i >= 0; i -= 4)
        putc_(d[(v >> i) & 0xF]);
}

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
#define EQ(got, want, what) do { uint32_t g_ = (got), w_ = (want); check(g_ == w_, what, g_, w_); } while (0)

static void run(const char *name, void (*fn)(void))
{
    unsigned before = n_fail;
    current = name;
    fn();
    puts_(n_fail == before ? "[ ok ] " : "[FAIL] ");
    puts_(name);
    puts_("\n");
    if (n_fail == before)
        n_pass++;
}

/* Operands the compiler cannot fold. */
static volatile uint32_t V0 = 0, V1 = 1, VM1 = 0xFFFFFFFFu, VMIN = 0x80000000u, V7 = 7, V3 = 3;
static volatile uint32_t VBIG = 0x12345678u, VDIV = 1000u;

/* ---- CP0 (not encodable in MIPS16e: these are MIPS32 functions there) --- */
#define mfc0(reg, sel) ({ uint32_t v_; __asm__ volatile("mfc0 %0, $" #reg ", " #sel : "=r"(v_)); v_; })
#define mtc0(reg, sel, v) __asm__ volatile("mtc0 %0, $" #reg ", " #sel "\n\tehb" :: "r"((uint32_t)(v)))
#define CP0_RD(name, reg, sel) static NATIVE uint32_t rd_##name(void) { return mfc0(reg, sel); }
#define CP0_WR(name, reg, sel) static NATIVE void wr_##name(uint32_t v) { mtc0(reg, sel, v); }
CP0_RD(status, 12, 0) CP0_WR(status, 12, 0)
CP0_RD(cause, 13, 0)  CP0_WR(cause, 13, 0)
CP0_RD(count, 9, 0)   CP0_WR(compare, 11, 0)
CP0_RD(srsctl, 12, 2)
CP0_RD(config, 16, 0) CP0_RD(config1, 16, 1) CP0_RD(config3, 16, 3)

static NATIVE void ei(void) { __asm__ volatile("ei\n\tehb" ::: "memory"); }
static NATIVE void di(void) { __asm__ volatile("di\n\tehb" ::: "memory"); }
static void set_ipl(unsigned ipl) { wr_status((rd_status() & ~(0x3Fu << 10)) | (ipl << 10)); }

/* ---- traps ------------------------------------------------------------- */
typedef struct { uint32_t x[34]; } frame;
extern volatile uint32_t g_refill;
static volatile uint32_t trap_n, last_code, last_epc, last_bad, last_cause, last_refill;
static volatile uint32_t bd_fix_addr;

/* The length of the instruction at EPC, in the ISA EPC bit 0 names. */
static NATIVE uint32_t insn_len(uint32_t epc)
{
    if (!(epc & 1))
        return 4;
    uint16_t hw = *(volatile uint16_t *)(epc & ~1u);
#ifdef MODEL_MX
    unsigned op = hw >> 11;
    return op == 0x1E || op == 0x03 ? 4 : 2;     /* EXTEND, JAL/JALX */
#else
    unsigned op = hw >> 10;
    return (op & 7) == 1 || (op & 7) == 2 || (op & 7) == 3 ? 2 : 4;
#endif
}

NATIVE uint32_t trap_handler(frame *f);
NATIVE uint32_t trap_handler(frame *f)
{
    uint32_t cause = mfc0(13, 0), epc = mfc0(14, 0);
    trap_n++;
    last_code = (cause >> 2) & 0x1F;
    last_epc = epc;
    last_bad = mfc0(8, 0);
    last_cause = cause;
    last_refill = g_refill;
    g_refill = 0;
    if (bd_fix_addr) {
        f->x[4] = bd_fix_addr;     /* make the faulting load valid ... */
        bd_fix_addr = 0;
        return epc;                /* ... and re-execute from EPC (the jump) */
    }
    return epc + insn_len(epc);
}

/* ---- interrupts -------------------------------------------------------- */
static volatile uint32_t irq_n, irq_log[16];
static volatile uint32_t irq_sp_ok;

static NATIVE void ack_source(unsigned src)
{
    if (src == SRC_TIMER)
        TIMER_STATUS = 1;
    if (src == SRC_CORE_TIMER)
        mtc0(11, 0, mfc0(9, 0) + 0x40000000u);   /* push Compare far out */
    if (src == SRC_SW0)
        mtc0(13, 0, mfc0(13, 0) & ~(1u << 8));
    IREG(R_IFS + 0x10u * (src / 32) + CLR) = 1u << (src % 32);
}

NATIVE void irq_handler(uint32_t vec);
NATIVE void irq_handler(uint32_t vec)
{
    uint32_t cause = mfc0(13, 0), srs = mfc0(12, 2);
    unsigned src = vec;
    if (vec == 0 && !(IREG(R_INTCON) & MVEC)) {
#ifdef VARIABLE_OFFSET
        src = IREG(R_INTSTAT) & 0xFF;     /* SIRQ */
#else
        src = IREG(R_INTSTAT) & 0x3F;     /* VEC */
#endif
    }
    uint32_t sp;
    __asm__ volatile("move %0, $sp" : "=r"(sp));
    irq_sp_ok = sp >= 0x80000000u && sp < 0x80008000u;
    irq_log[irq_n++ & 15] = src | (((cause >> 10) & 0x3F) << 8) | ((srs & 0xF) << 16)
                          | (((srs >> 6) & 0xF) << 20);
    ack_source(src);
}

static void src_setup(unsigned src, unsigned pri, unsigned sub)
{
    unsigned sh = 8 * (src % 4);
    volatile uint32_t *ipc = &IREG(R_IPC + 0x10u * (src / 4));
    *ipc = (*ipc & ~(0x1Fu << sh)) | (((pri << 2) | sub) << sh);
#ifdef VARIABLE_OFFSET
    IREG(R_OFF + 4 * src) = 0x200 + 32 * src;
#endif
    IREG(R_IEC + 0x10u * (src / 32) + SET) = 1u << (src % 32);
}

static void src_raise(unsigned src) { IREG(R_IFS + 0x10u * (src / 32) + SET) = 1u << (src % 32); }

static void intc_reset(void)
{
    di();
    for (unsigned k = 0; k < 2; k++) {
        IREG(R_IEC + 0x10u * k) = 0;
        IREG(R_IFS + 0x10u * k) = 0;
    }
    for (unsigned k = 0; k < 4; k++)
        IREG(R_IPC + 0x10u * k) = 0;
    IREG(R_INTCON) = MVEC;
#ifdef VARIABLE_OFFSET
    IREG(R_PRISS) = 0;
#endif
    set_ipl(0);
    irq_n = 0;
}

static int wait_irqs(unsigned n)
{
    for (unsigned spin = 0; spin < 200000; spin++)
        if (irq_n >= n)
            return 1;
    return 0;
}

/* ---- tests ------------------------------------------------------------- */

static void t_config(void)
{
    uint32_t c0 = rd_config(), c1 = rd_config1(), c3 = rd_config3();
    uint32_t hss = (rd_srsctl() >> 26) & 0xF;
    EQ((c3 >> 6) & 1, 1, "Config3.VEIC");
    EQ((c3 >> 5) & 1, 1, "Config3.VInt");
#if defined(MODEL_MX)
    EQ((c0 >> 7) & 7, 3, "Config.MT (FMT)");
    EQ((c1 >> 2) & 1, 1, "Config1.CA (MIPS16e)");
    EQ((c3 >> 14) & 3, 0, "Config3.ISA");
    EQ(c1 & 1, 0, "Config1.FP");
    EQ(hss, 1, "SRSCtl.HSS");
#elif defined(MODEL_MM)
    EQ((c0 >> 7) & 7, 3, "Config.MT (FMT)");
    EQ((c1 >> 2) & 1, 0, "Config1.CA");
    EQ((c3 >> 14) & 3, 1, "Config3.ISA (microMIPS only)");
    EQ((c3 >> 17) & 1, 1, "Config3.MCU");
    EQ(hss, 1, "SRSCtl.HSS");
#else
    EQ((c0 >> 7) & 7, 1, "Config.MT (TLB)");
    EQ((c1 >> 25) & 0x3F, 15, "Config1.MMUSize");
    EQ(c1 & 1, 1, "Config1.FP");
    EQ((c3 >> 10) & 3, 3, "Config3.DSPP DSP2P");
    EQ((c3 >> 14) & 3, BOOT_UMIPS ? 3 : 2, "Config3.ISA");
    EQ(hss, 7, "SRSCtl.HSS");
#endif
}

static uint32_t mul_hi(uint32_t a, uint32_t b) { return (uint32_t)(((uint64_t)a * b) >> 32); }
static int32_t smul_hi(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 32); }

static void t_alu(void)
{
    EQ(V1 + VM1, 0, "add wrap");
    EQ(V0 - V1, 0xFFFFFFFFu, "sub");
    EQ(VBIG * V7, 0x7f6e5d48u, "mul");
    EQ(mul_hi(VBIG, 0x9ABCDEF0u), 0x0b00ea4eu, "multu hi");
    EQ((uint32_t)smul_hi((int32_t)VMIN, 3), 0xfffffffeu, "mult hi");
    EQ(VBIG / VDIV, 305419u, "divu");
    EQ(VBIG % VDIV, 896u, "remu");
    EQ((uint32_t)((int32_t)VMIN / (int32_t)V7), 0xedb6db6eu, "div");
    EQ(VBIG >> V7, 0x2468acu, "srlv");
    EQ((uint32_t)((int32_t)VMIN >> 31), 0xFFFFFFFFu, "srav");
    EQ(VBIG << 4, 0x23456780u, "sll");
    EQ(VBIG & 0xFF00FF00u, 0x12005600u, "and");
    EQ(VBIG | 0x0F0F0000u, 0x1f3f5678u, "or");
    EQ(VBIG ^ VM1, 0xedcba987u, "xor/not");
    EQ((int32_t)VM1 < (int32_t)V0, 1, "slt");
    EQ(VM1 < V0, 0, "sltu");
    EQ((uint32_t)(int8_t)(VBIG >> 0), 0x78u, "seb");
    EQ((uint32_t)(int16_t)(VBIG | 0x8000u), 0xffffd678u, "seh");
    uint32_t x = VBIG, n = 0;
    while (x) { x &= x - 1; n++; }
    EQ(n, 13, "popcount loop");
}

static volatile uint8_t buf[32] __attribute__((aligned(4)));

static void t_memory(void)
{
    for (unsigned i = 0; i < 32; i++)
        buf[i] = (uint8_t)(i * 7 + 1);
    volatile uint16_t *h = (volatile uint16_t *)buf;
    volatile uint32_t *w = (volatile uint32_t *)buf;
    EQ(h[3], 0x322bu, "lhu");
    EQ((uint32_t)(int16_t)h[9], 0xffff867fu, "lh");
    EQ(w[2], 0x4e474039u, "lw");
    EQ((uint32_t)(int8_t)buf[19], 0xffffff86u, "lb");
    w[1] = 0xCAFEF00Du;
    EQ(buf[4], 0x0D, "sw/lbu");
    h[0] = 0xBEEF;
    EQ(w[0], 0x160fbeefu, "sh");
    buf[31] = 0x5A;
    EQ(w[7], 0x5ad3ccc5u, "sb");
}

static uint32_t fib(uint32_t n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
static uint32_t op_add(uint32_t a, uint32_t b) { return a + b; }
static uint32_t op_sub(uint32_t a, uint32_t b) { return a - b; }
static uint32_t op_mul(uint32_t a, uint32_t b) { return a * b; }
static uint32_t (*volatile ops[3])(uint32_t, uint32_t) = { op_add, op_sub, op_mul };

static uint32_t sw(uint32_t k)
{
    switch (k) {
    case 0: return 11;
    case 1: return 22;
    case 2: return 33;
    case 3: return 44;
    case 4: return 55;
    case 5: return 66;
    case 6: return 77;
    default: return 0;
    }
}

static void t_calls(void)
{
    EQ(fib(V7 + 13), 6765, "recursion");
    EQ(ops[0](7, 5) + ops[1](7, 5) + ops[2](7, 5), 49, "indirect calls");
    uint32_t s = 0;
    for (uint32_t k = V0; k < 8; k++)
        s = s * 3 + sw(k);
    EQ(s, 53988u, "jump table");
}

/* Calls between ISA modes: a MIPS32 function called from the compressed ISA
 * (JALX) and calling back through a pointer (JALR to an odd address). */
#if defined(MODEL_MX) || (defined(MODEL_MZ) && BOOT_UMIPS)
#ifdef MODEL_MX
#define OTHER_ISA __attribute__((nomips16, noinline))
#else
#define OTHER_ISA __attribute__((nomicromips, noinline))
#endif
static uint32_t back(uint32_t x) { return x * 3 + 1; }
static OTHER_ISA uint32_t native_call(uint32_t (*fn)(uint32_t), uint32_t x)
{
    return fn(x) + 100;
}
static OTHER_ISA uint32_t native_isa_bit(void)
{
    uint32_t ra;
    __asm__ volatile("bal 1f\n\tnop\n1:\tmove %0, $31" : "=r"(ra) :: "$31");
    return ra & 1;
}
static void t_modes(void)
{
    /* Direct calls (JALX); the pointer call (JALR to an odd address) back. */
    EQ(native_call(back, 13), 140, "jalx + jalr to compressed");
    uint32_t (*volatile isa_bit)(void) = native_isa_bit;   /* JALR to MIPS32 */
    EQ(isa_bit(), 0, "MIPS32 link bit");
    uint32_t (*volatile pf)(uint32_t) = back;
    EQ((uint32_t)(uintptr_t)pf & 1, 1, "compressed function address bit 0");
}
#endif

uint32_t bd_probe(volatile uint32_t *p);
static volatile uint32_t probe_word = 0x600DF00Du;

static NATIVE void do_break(void) { __asm__ volatile("break 7"); }
static NATIVE void do_syscall(void) { __asm__ volatile("syscall"); }

static void t_exceptions(void)
{
    trap_n = 0;
    do_break();
    EQ(trap_n, 1, "break traps");
    EQ(last_code, 9, "break ExcCode");
    do_syscall();
    EQ(last_code, 8, "syscall ExcCode");
#if defined(MODEL_MM) || (defined(MODEL_MZ) && BOOT_UMIPS)
    EQ(last_epc & 1, 1, "EPC bit 0 (microMIPS)");
#else
    EQ(last_epc & 1, 0, "EPC bit 0 (MIPS32)");
#endif
    /* A load in the delay slot of the probe's JR faults: EPC is the JR. */
    EQ(bd_probe(&probe_word), 0x600DF00Du, "probe");
    trap_n = 0;
    bd_fix_addr = (uint32_t)(uintptr_t)&probe_word;
    uint32_t v = bd_probe((volatile uint32_t *)0xbf900000u);   /* nothing mapped */
    EQ(v, 0x600DF00Du, "probe restarted");
    EQ(trap_n, 1, "one trap");
    EQ(last_code, 7, "DBE");
    EQ(last_cause >> 31, 1, "Cause.BD");
    uint32_t probe = (uint32_t)(uintptr_t)bd_probe;
#if defined(MODEL_MX) || defined(MODEL_MM) || (defined(MODEL_MZ) && BOOT_UMIPS)
    EQ(last_epc, probe + 2, "EPC = the JR (compressed)");
#else
    EQ(last_epc, probe + 4, "EPC = the JR");
#endif
}

static void t_eic(void)
{
    intc_reset();
    src_setup(SRC_TIMER, 3, 0);
    TIMER_LOAD = 300;
    TIMER_CTRL = 1 | 4;          /* one-shot, interrupt enabled */
    ei();
    EQ(wait_irqs(1), 1, "timer interrupt taken");
    di();
    EQ(irq_log[0] & 0xFF, SRC_TIMER, "vector");
    EQ((irq_log[0] >> 8) & 0x3F, 3, "Cause.RIPL");
    EQ(irq_sp_ok, 1, "handler stack");
    EQ((rd_cause() >> 2) & 0x1F, 0, "Cause.ExcCode = Int");
}

static void t_priority(void)
{
    intc_reset();
    src_setup(SRC_A, 2, 0);
    src_setup(SRC_B, 5, 0);
    src_setup(SRC_C, 5, 2);      /* same level, higher subpriority: first */
    src_raise(SRC_A);
    src_raise(SRC_B);
    src_raise(SRC_C);
    ei();
    EQ(wait_irqs(3), 1, "three interrupts");
    di();
    EQ(irq_log[0] & 0xFF, SRC_C, "first: level 5 subpriority 2");
    EQ(irq_log[1] & 0xFF, SRC_B, "second: level 5");
    EQ(irq_log[2] & 0xFF, SRC_A, "third: level 2");
}

static void t_ipl(void)
{
    intc_reset();
    src_setup(SRC_A, 3, 0);
    set_ipl(4);
    src_raise(SRC_A);
    ei();
    for (volatile int i = 0; i < 2000; i++) {}
    EQ(irq_n, 0, "held off by Status.IPL");
    set_ipl(2);
    EQ(wait_irqs(1), 1, "taken once IPL drops");
    di();
    set_ipl(0);
}

static void t_shadow(void)
{
    intc_reset();
#ifdef VARIABLE_OFFSET
    IREG(R_PRISS) = 2u << 24;    /* level 6 -> shadow set 2 */
    unsigned lvl = 6, want = 2;
#else
    unsigned lvl = 7, want = 1;  /* the board gives level 7 shadow set 1 */
#endif
    src_setup(SRC_B, lvl, 0);
    src_raise(SRC_B);
    ei();
    EQ(wait_irqs(1), 1, "taken");
    di();
    EQ((irq_log[0] >> 16) & 0xF, want, "SRSCtl.CSS in the handler");
    EQ((irq_log[0] >> 20) & 0xF, 0, "SRSCtl.PSS in the handler");
    EQ(irq_sp_ok, 1, "RDPGPR gave the stack");
    EQ(rd_srsctl() & 0xF, 0, "back in set 0 (ERET: CSS = PSS)");
}

static void t_core_sources(void)
{
    intc_reset();
    src_setup(SRC_CORE_TIMER, 2, 0);
    src_setup(SRC_SW0, 1, 0);
    wr_compare(rd_count() + 500);
    ei();
    EQ(wait_irqs(1), 1, "core timer");
    EQ(irq_log[0] & 0xFF, SRC_CORE_TIMER, "core timer source");
    wr_cause(rd_cause() | (1u << 8));     /* Cause.IP0 */
    EQ(wait_irqs(2), 1, "software interrupt");
    di();
    EQ(irq_log[1] & 0xFF, SRC_SW0, "software interrupt source");
}

static void t_single_vector(void)
{
    intc_reset();
    IREG(R_INTCON) = 0;          /* single vector: every request at vector 0 */
    src_setup(SRC_A, 4, 0);
    src_raise(SRC_A);
    ei();
    EQ(wait_irqs(1), 1, "taken");
    di();
    EQ(irq_log[0] & 0xFF, SRC_A, "INTSTAT names the source");
    EQ((irq_log[0] >> 8) & 0x3F, 4, "RIPL");
    IREG(R_INTCON) = MVEC;
}

static void t_mapping(void)
{
    trap_n = 0;
    /* kuseg with ERL = 0 (away from the zeroed TLB entries' page 0) */
    uint32_t v = *(volatile uint32_t *)0x00101000u;
    (void)v;
    EQ(trap_n, 1, "kuseg access traps");
#if defined(MODEL_MZ)
    EQ(last_code, 2, "TLB refill (TLBL)");
    EQ(last_refill, 1, "refill vector");
#else
    EQ(last_code, 7, "FMT: physical 0x40101000 is a bus error");
#endif
    EQ(last_bad == 0x00101000u || last_code == 7, 1, "BadVAddr");
}

int main(void);
int main(void)
{
    puts_("pic32-ref self-test (" MODEL_NAME ")\n");
    run("config", t_config);
    run("alu", t_alu);
    run("memory", t_memory);
    run("calls", t_calls);
#if defined(MODEL_MX) || (defined(MODEL_MZ) && BOOT_UMIPS)
    run("isa-modes", t_modes);
#endif
    run("exceptions", t_exceptions);
    run("eic", t_eic);
    run("priority", t_priority);
    run("ipl", t_ipl);
    run("shadow-sets", t_shadow);
    run("core-sources", t_core_sources);
    run("single-vector", t_single_vector);
    run("mapping", t_mapping);
    puts_(n_fail ? "SELFTEST FAIL\n" : "SELFTEST PASS\n");
    volatile uint8_t *fin = (volatile uint8_t *)FINISHER;
    if (n_fail)
        fin[1] = (uint8_t)n_fail;
    else
        fin[0] = 0x55;
    for (;;) {}
}
