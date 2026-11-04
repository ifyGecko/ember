/*
 * selftest.c - Bare-metal AVR32 self-test for the UAM avr32-ref board, built for
 * AVR32A (-o model=a) and, with AVR32B defined, AVR32B (-o model=b).
 *
 * Exercises: UART output, the integer ISA through compiled C (64-bit arithmetic,
 * calls, switch tables, loads and stores of every width) and hand-picked
 * instructions (bit fields, byte swaps, division, 64-bit multiply, xchg, stcond,
 * conditional execution), the DSP instructions with saturation and the Q flag,
 * the SIMD instructions (AVR32B), breakpoint and debug mode, scall / rets from the
 * supervisor and from application mode, the privilege violation, the shadowed SP,
 * exceptions with ECR and TLBEAR, COUNT / COMPARE through the interrupt controller
 * (autovectors, levels, priority, sleep), the MPU (AVR32A: regions, subregions,
 * permissions, misses) and the MMU (AVR32B: segments, tlbw / tlbr / tlbs, pages,
 * misses, protection, the Modified exception, invalidation).
 *
 * The console output is deterministic. The result is reported through the board's
 * test finisher.
 */
#include <stdint.h>

#define UART0    0xffff1400u
#define REG8(a)  (*(volatile uint8_t *)(uintptr_t)(a))
#define REG32(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define UART_THR REG8(UART0 + 0)
#define UART_IER REG8(UART0 + 1)
#define UART_LSR REG8(UART0 + 5)
#define INTC     0xffff0800u
#define IPR(g)   REG32(INTC + 4 * (g))
#define IRR(g)   REG32(INTC + 0x100 + 4 * (g))
#define ICR(l)   REG32(INTC + 0x20c - 4 * (l))

/* System registers (addresses). */
#define SR       0
#define EVBA     4
#define ECR      16
#define RSR_DBG  48
#define CONFIG0  256
#define COUNT    264
#define COMPARE  268
#define TLBEHI   272
#define TLBELO   276
#define TLBEAR   284
#define MMUCR    288
#define MPUAR(n) (320 + 4 * (n))
#define MPUPSR(n) (352 + 4 * (n))
#define MPUAPRA  400
#define MPUAPRB  404
#define MPUCR    408

#define mfsr(a) ({ uint32_t v_; __asm__ volatile("mfsr %0, %1" : "=r"(v_) : "i"(a)); v_; })
#define mtsr(a, v) __asm__ volatile("mtsr %0, %1" :: "i"(a), "r"((uint32_t)(v)) : "memory")

static void putc_(char c) { while (!(UART_LSR & 0x20)) {} UART_THR = (uint8_t)c; }
static void puts_(const char *s) { for (; *s; s++) { if (*s == '\n') putc_('\r'); putc_(*s); } }
static void puthex(uint32_t v)
{
    static const char d[] = "0123456789abcdef";
    puts_("0x");
    for (int i = 28; i >= 0; i -= 4)
        putc_(d[(v >> i) & 0xF]);
}

/* -nostdlib: what the compiler may call */
void *memcpy(void *d, const void *s, unsigned long n);
void *memset(void *d, int c, unsigned long n);
void *memcpy(void *d, const void *s, unsigned long n)
{
    uint8_t *dp = d;
    const uint8_t *sp = s;
    while (n--)
        *dp++ = *sp++;
    return d;
}
void *memset(void *d, int c, unsigned long n)
{
    uint8_t *dp = d;
    while (n--)
        *dp++ = (uint8_t)c;
    return d;
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

/* ---- the trampolines in start.S ----------------------------------------- */
/* stm --sp, r0-r12, lr puts the highest register at the lowest address */
typedef struct frame { uint32_t lr, r_hi[13], sr, pc; } frame;
#define FR(f, n) ((f)->r_hi[12 - (n)])
extern volatile uint32_t g_bkpt;
extern char _evba[], int0_stub[], int2_stub[], user_stub[], user_resume[], probe_ret[];
uint32_t probe_ld(uint32_t addr);
uint32_t probe_ldh(uint32_t addr);
uint32_t probe_st(uint32_t addr, uint32_t v);
uint32_t probe_call(uint32_t fn);
uint32_t enter_user(uint32_t pc, uint32_t arg, uint32_t usp);

#define FAULTED 0xfa017edu
static volatile uint32_t trap_n, last_ecr, last_pc, last_tlbear, user_traps;

void trap_handler(frame *f);
void trap_handler(frame *f)
{
    trap_n++;
    last_ecr = mfsr(ECR);
    last_pc = f->pc;
    last_tlbear = mfsr(TLBEAR);
    if (((f->sr >> 22) & 7) == 0) {
        /* from application mode: end the excursion in the supervisor */
        user_traps++;
        FR(f, 12) = FR(f, 11);
        f->pc = (uint32_t)user_resume;
        f->sr = (f->sr & ~(7u << 22)) | (1u << 22);
        return;
    }
    FR(f, 12) = FAULTED;   /* a probe: return to its caller */
    f->pc = (uint32_t)probe_ret;
}

static volatile uint32_t ticks, order[4], n_order, uart_icr;

void irq_handler(unsigned level);
void irq_handler(unsigned level)
{
    if (n_order < 4)
        order[n_order++] = level;
    if (level == 0) {
        ticks++;
        mtsr(COMPARE, ticks < 3 ? 300 : 0);   /* writing COMPARE clears the request */
    } else if (level == 2) {
        uart_icr = ICR(2);
        UART_IER = 0;
    }
}

/* Operands the compiler cannot fold. */
static volatile uint32_t V0 = 0, V1 = 1, VM1 = 0xFFFFFFFFu, V7 = 7, V3 = 3;
static volatile int32_t VS7 = -7;
static volatile uint64_t VD = 0x0123456789abcdefull;

/* ---- tests ---------------------------------------------------------------- */

static uint32_t fib(uint32_t n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

static uint32_t sw(uint32_t x)
{
    switch (x) {
    case 0: return 11;
    case 1: return 22;
    case 2: return 33;
    case 3: return 44;
    case 4: return 55;
    case 5: return 66;
    case 7: return 88;
    default: return 99;
    }
}

static void t_alu(void)
{
    EQ(V7 + V3, 10, "add");
    EQ(V3 - V7, 0xFFFFFFFCu, "sub");
    EQ(V7 * V3, 21, "mul");
    EQ(V7 / V3, 2, "udiv");
    EQ(V7 % V3, 1, "urem");
    EQ((uint32_t)(VS7 / 2), (uint32_t)-3, "sdiv");
    EQ((uint32_t)(VS7 % 2), (uint32_t)-1, "srem");
    EQ((uint32_t)(VS7 >> 1), (uint32_t)-4, "asr");
    EQ(VM1 >> 28, 0xF, "lsr");
    EQ(V7 << 29, 0xE0000000u, "lsl");
    EQ((uint32_t)(int8_t)(V7 + 0xF8), 0xFFFFFFFFu, "sext8");
    EQ((uint32_t)(int16_t)(V7 + 0x8000), 0xFFFF8007u, "sext16");
    EQ(VM1 < V1, 0, "unsigned compare");
    EQ((int32_t)VM1 < (int32_t)V1, 1, "signed compare");
    EQ(fib(V7 + 13), 6765, "fib(20)");
    uint32_t s = 0;
    for (uint32_t i = 0; i < 9; i++)
        s += sw(i);
    EQ(s, 11 + 22 + 33 + 44 + 55 + 66 + 99 + 88 + 99, "switch");
}

static void t_wide(void)
{
    uint64_t a = VD, b = 0x1111111111111111ull * V3;
    EQ((uint32_t)((a + b) >> 32), 0x3456789au, "add64 hi");
    EQ((uint32_t)(a + b), 0xbcdf0122u, "add64 lo");
    EQ((uint32_t)((a - b) >> 32), 0xcdf01234u, "sub64 hi");
    uint64_t p = (uint64_t)VM1 * VM1;
    EQ((uint32_t)(p >> 32), 0xFFFFFFFEu, "mulu.d hi");
    EQ((uint32_t)p, 1, "mulu.d lo");
    int64_t q = (int64_t)(int32_t)VM1 * 5;
    EQ((uint32_t)(q >> 32), 0xFFFFFFFFu, "muls.d hi");
    EQ((uint32_t)q, 0xFFFFFFFBu, "muls.d lo");
    EQ((uint32_t)(a / 1000), (uint32_t)(0x0123456789abcdefull / 1000), "udiv64");
    EQ((uint32_t)(a >> 36), 0x0123456u, "lsr64");
    EQ((uint32_t)((a << 20) >> 32), 0x56789abcu, "lsl64");
    static volatile uint64_t mem64[2];
    mem64[1] = a;
    EQ((uint32_t)(mem64[1] >> 32), 0x01234567u, "st.d/ld.d hi");
    EQ((uint32_t)mem64[1], 0x89abcdefu, "st.d/ld.d lo");
}

static void t_mem(void)
{
    static volatile uint8_t buf[16];
    for (unsigned i = 0; i < 16; i++)
        buf[i] = (uint8_t)(0xF0 + i);
    EQ(*(volatile uint32_t *)&buf[4], 0xF4F5F6F7u, "big-endian word");
    EQ(*(volatile uint16_t *)&buf[2], 0xF2F3u, "halfword");
    EQ((uint32_t)*(volatile int16_t *)&buf[2], 0xFFFFF2F3u, "signed halfword");
    EQ((uint32_t)*(volatile int8_t *)&buf[1], 0xFFFFFFF1u, "signed byte");
    *(volatile uint16_t *)&buf[8] = 0x1234;
    EQ(buf[8] * 256 + buf[9], 0x1234, "store halfword");
    struct { uint32_t a, b, c, d, e; } x = { 1, 2, 3, 4, 5 }, y;
    y = *(volatile __typeof__(x) *)&x;
    EQ(y.a + y.b + y.c + y.d + y.e, 15, "struct copy");
}

static void t_bits(void)
{
    uint32_t r, a = 0x12345678u + V0;
    __asm__ volatile("bfextu %0, %1, 4, 8" : "=r"(r) : "r"(a));
    EQ(r, 0x67, "bfextu");
    __asm__ volatile("bfexts %0, %1, 28, 4" : "=r"(r) : "r"(VM1 - 0x0FFFFFFF));
    EQ(r, 0xFFFFFFFFu, "bfexts");
    r = VM1;
    __asm__ volatile("bfins %0, %1, 4, 8" : "+r"(r) : "r"(V0));
    EQ(r, 0xFFFFF00Fu, "bfins");
    r = V1;
    __asm__ volatile("brev %0" : "+r"(r));
    EQ(r, 0x80000000u, "brev");
    __asm__ volatile("clz %0, %1" : "=r"(r) : "r"(V1 << 16));
    EQ(r, 15, "clz");
    __asm__ volatile("clz %0, %1" : "=r"(r) : "r"(V0));
    EQ(r, 32, "clz 0");
    r = 0x11223344u + V0;
    __asm__ volatile("swap.b %0" : "+r"(r));
    EQ(r, 0x44332211u, "swap.b");
    r = 0x11223344u + V0;
    __asm__ volatile("swap.bh %0" : "+r"(r));
    EQ(r, 0x22114433u, "swap.bh");
    r = 0x11223344u + V0;
    __asm__ volatile("swap.h %0" : "+r"(r));
    EQ(r, 0x33441122u, "swap.h");
    r = 0x8001u + V0;
    __asm__ volatile("casts.h %0" : "+r"(r));
    EQ(r, 0xFFFF8001u, "casts.h");
    r = 0x80u + V0;
    __asm__ volatile("castu.b %0" : "+r"(r) :: "cc");
    EQ(r, 0x80, "castu.b");
    r = V0;
    __asm__ volatile("sbr %0, 31" : "+r"(r) :: "cc");
    EQ(r, 0x80000000u, "sbr");
    __asm__ volatile("cbr %0, 31" : "+r"(r) :: "cc");
    EQ(r, 0, "cbr");
    r = 0xFFFFF0F0u + V0;
    __asm__ volatile("andh %0, 0x1234, COH" : "+r"(r));
    EQ(r, 0x12340000u, "andh coh");
    r = 0xF0F0u + V0;
    __asm__ volatile("orh %0, 0xabcd" : "+r"(r));
    EQ(r, 0xabcdf0f0u, "orh");
    uint32_t x = 5, y = 9;
    __asm__ volatile("max %0, %1, %2" : "=r"(r) : "r"(x + V0), "r"(y - 10));
    EQ(r, 5, "max (signed)");
    __asm__ volatile("min %0, %1, %2" : "=r"(r) : "r"(x + V0), "r"(y - 10));
    EQ(r, 0xFFFFFFFFu, "min (signed)");
}

static void t_muldiv(void)
{
    register uint32_t q __asm__("r0"), rem __asm__("r1");
    register uint32_t a __asm__("r2") = (uint32_t)VS7, b __asm__("r3") = 2 + V0;
    __asm__ volatile("divs r0, r2, r3" : "=r"(q), "=r"(rem) : "r"(a), "r"(b));
    EQ(q, (uint32_t)-3, "divs quotient");
    EQ(rem, (uint32_t)-1, "divs remainder");
    a = 7 + V0;
    __asm__ volatile("divu r0, r2, r3" : "=r"(q), "=r"(rem) : "r"(a), "r"(b));
    EQ(q, 3, "divu quotient");
    EQ(rem, 1, "divu remainder");
    a = VM1;
    __asm__ volatile("mulu.d r0, r2, r3" : "=r"(q), "=r"(rem) : "r"(a), "r"(b));
    EQ(q, 0xFFFFFFFEu, "mulu.d low");
    EQ(rem, 1, "mulu.d high");
    uint32_t acc = 10 + V0;
    __asm__ volatile("mac %0, %1, %2" : "+r"(acc) : "r"(V7), "r"(V3));
    EQ(acc, 31, "mac");
}

static void t_atomic(void)
{
    static volatile uint32_t word = 5;
    uint32_t old, nv = 9 + V0, sr;
    __asm__ volatile("xchg %0, %1, %2" : "=&r"(old) : "r"(&word), "r"(nv) : "memory");
    EQ(old, 5, "xchg old");
    EQ(word, 9, "xchg new");
    __asm__ volatile("ssrf 5\n\tstcond %1[0], %2\n\tmfsr %0, 0" : "=r"(sr) : "r"(&word), "r"(V7) : "memory", "cc");
    EQ(sr & 2, 2, "stcond with L: Z");
    EQ(word, 7, "stcond with L: stored");
    __asm__ volatile("csrf 5\n\tstcond %1[0], %2\n\tmfsr %0, 0" : "=r"(sr) : "r"(&word), "r"(V3) : "memory", "cc");
    EQ(sr & 2, 0, "stcond without L: Z");
    EQ(word, 7, "stcond without L: not stored");
}

static void t_cond(void)
{
    uint32_t r = 1 + V0;
    __asm__ volatile("cp.w %1, 3\n\tmoveq %0, 42\n\tmovne %0, 43" : "+r"(r) : "r"(V3) : "cc");
    EQ(r, 42, "moveq");
#ifndef AVR32B   /* revision 2 instructions: the AVR32B (ap7000) assembler refuses them */
    __asm__ volatile("cp.w %1, 3\n\taddeq %0, %0, %1\n\tsubne %0, 100" : "+r"(r) : "r"(V7) : "cc");
    EQ(r, 0xFFFFFFFFu - 57, "conditional add/sub");
#endif
    r = V0;
    __asm__ volatile("cp.w %1, 0\n\tsrlt %0" : "+r"(r) : "r"(VM1) : "cc");
    EQ(r, 1, "srlt");
}

#define DSP(op, a, b) ({ uint32_t r_; __asm__ volatile(op " %0, %1, %2" : "=r"(r_) : "r"(a), "r"(b) : "cc"); r_; })
static uint32_t q_flag(void) { return (mfsr(SR) >> 4) & 1; }

static void t_dsp(void)
{
    __asm__ volatile("csrf 4");
    EQ(DSP("satadd.w", 0x7FFFFFF0u + V0, 0x100u), 0x7FFFFFFFu, "satadd.w");
    EQ(q_flag(), 1, "satadd.w Q");
    __asm__ volatile("csrf 4");
    EQ(DSP("satsub.w", 0x80000005u + V0, 0x10u), 0x80000000u, "satsub.w");
    EQ(DSP("satadd.h", 0x7FF0u + V0, 0x0100u), 0x7FFFu, "satadd.h");
    EQ(DSP("satsub.h", 0x8005u + V0, 0x0010u), 0xFFFF8000u, "satsub.h");
    __asm__ volatile("csrf 4");
    EQ(DSP("satadd.w", 3 + V0, 4u), 7, "satadd.w no saturation");
    EQ(q_flag(), 0, "Q stays clear");
    uint32_t r = 0x1000 + V0;
    __asm__ volatile("sats %0 >> 4, 8" : "+r"(r) :: "cc");
    EQ(r, 0x7F, "sats");
    r = 300 + V0;
    __asm__ volatile("satu %0 >> 0, 8" : "+r"(r) :: "cc");
    EQ(r, 0xFF, "satu");
    uint32_t h = 0x4000 + V0, m = 0x8000 + V0;
    __asm__ volatile("mulsathh.h %0, %1:b, %1:b" : "=r"(r) : "r"(h) : "cc");
    EQ(r, 0x2000, "mulsathh.h");
    __asm__ volatile("csrf 4\n\tmulsathh.h %0, %1:b, %1:b" : "=r"(r) : "r"(m) : "cc");
    EQ(r, 0x7FFF, "mulsathh.h -1 * -1");
    EQ(q_flag(), 1, "mulsathh.h Q");
    __asm__ volatile("mulsathh.w %0, %1:b, %1:b" : "=r"(r) : "r"(h) : "cc");
    EQ(r, 0x20000000u, "mulsathh.w");
    __asm__ volatile("mulsathh.w %0, %1:b, %1:b" : "=r"(r) : "r"(m) : "cc");
    EQ(r, 0x7FFFFFFFu, "mulsathh.w -1 * -1");
    __asm__ volatile("mulhh.w %0, %1:t, %2:b" : "=r"(r) : "r"(0xFFFE0000u + V0), "r"(3 + V0));
    EQ(r, 0xFFFFFFFAu, "mulhh.w");
    r = 0x10 + V0;
    __asm__ volatile("macsathh.w %0, %1:b, %1:b" : "+r"(r) : "r"(h) : "cc");
    EQ(r, 0x20000010u, "macsathh.w");
    __asm__ volatile("mulwh.d r0, %1, %2:t\n\tmov %0, r1" : "=r"(r) : "r"(0x10000 + V0), "r"(0x00030000u + V0) : "r0", "r1");
    EQ(r, 3, "mulwh.d high");
}

#ifdef AVR32B
static void t_simd(void)
{
    EQ(DSP("padd.b", 0x01020304u + V0, 0x10203040u), 0x11223344u, "padd.b");
    EQ(DSP("padds.ub", 0xF0F0F0F0u + V0, 0x20202020u), 0xFFFFFFFFu, "padds.ub");
    EQ(DSP("padds.sb", 0x7F80017Fu + V0, 0x01FF0101u), 0x7F80027Fu, "padds.sb");
    EQ(DSP("psub.h", 0x00050003u + V0, 0x00010004u), 0x0004FFFFu, "psub.h");
    EQ(DSP("pavg.ub", 0x10203040u + V0, 0x21212121u), 0x19212931u, "pavg.ub");
    EQ(DSP("psad", 0x10203040u + V0, 0x21212121u), 0x40, "psad");
    EQ(DSP("pmax.sh", 0x0005FFF0u + V0, 0xFFFF0003u), 0x00050003u, "pmax.sh");
    EQ(DSP("pmin.ub", 0x10FF2080u + V0, 0x20013040u), 0x10012040u, "pmin.ub");
    uint32_t r;
    __asm__ volatile("pabs.sb %0, %1" : "=r"(r) : "r"(0xFE05F17Fu + V0));
    EQ(r, 0x02050F7Fu, "pabs.sb");
    __asm__ volatile("pasr.h %0, %1, 1" : "=r"(r) : "r"(0x80000004u + V0));
    EQ(r, 0xC0000002u, "pasr.h");
    __asm__ volatile("plsl.b %0, %1, 4" : "=r"(r) : "r"(0x12345678u + V0));
    EQ(r, 0x20406080u, "plsl.b");
}
#endif

static void t_breakpoint(void)
{
    uint32_t before = g_bkpt;
    __asm__ volatile("breakpoint\n\tbreakpoint" ::: "memory");
    EQ(g_bkpt - before, 2, "breakpoint entries");
    EQ((mfsr(RSR_DBG) >> 22) & 7, 1, "RSR_DBG mode (supervisor)");
}

static void t_scall(void)
{
    register uint32_t r __asm__("r12") = 20 + V0;
    __asm__ volatile("scall" : "+r"(r) : : "memory", "cc");
    EQ(r, 41, "scall from the supervisor");
}

static uint32_t user_stack[64];

static void t_user(void)
{
    uint32_t pc = (uint32_t)user_stub, usp = (uint32_t)&user_stack[64];
#ifdef AVR32B
    pc &= 0x1FFFFFFFu;    /* U0: untranslated while paging is off */
    usp &= 0x1FFFFFFFu;
#endif
    uint32_t sp_before;
    __asm__ volatile("mov %0, sp" : "=r"(sp_before));
    trap_n = 0;
    user_traps = 0;
    uint32_t r = enter_user(pc, 33, usp);
    EQ(r, 67, "scall result from application mode");
    EQ(user_traps, 1, "privilege violation taken");
    EQ(last_ecr, 0x28 >> 2, "ECR = privilege violation");
    EQ(last_pc, pc + 4, "faulting pc");
    uint32_t sp_after;
    __asm__ volatile("mov %0, sp" : "=r"(sp_after));
    EQ(sp_after, sp_before, "supervisor SP kept");
    EQ((mfsr(SR) >> 22) & 7, 1, "back in supervisor mode");
}

static void t_exceptions(void)
{
    static volatile uint32_t w[2] = { 0x11112222u, 0x33334444u };
    trap_n = 0;
    EQ(probe_ld((uint32_t)&w[0] + 2), FAULTED, "unaligned ld.w faults");
    EQ(last_ecr, 0x34 >> 2, "ECR = data address read");
    EQ(last_tlbear, (uint32_t)&w[0] + 2, "TLBEAR = address");
    EQ(probe_ldh((uint32_t)&w[0] + 2), 0x2222, "aligned ld.sh");
    EQ(probe_st((uint32_t)&w[1] + 1, 0), FAULTED, "unaligned st.w faults");
    EQ(last_ecr, 0x38 >> 2, "ECR = data address write");
    EQ(w[1], 0x33334444u, "nothing stored");
    EQ(trap_n, 2, "exception count");
    EQ(mfsr(EVBA), (uint32_t)_evba, "EVBA");
}

static void t_config(void)
{
    uint32_t c0 = mfsr(CONFIG0);
#ifdef AVR32B
    EQ((c0 >> 13) & 7, 1, "CONFIG0.AT = AVR32B");
    EQ((c0 >> 7) & 7, 1, "CONFIG0.MMUT = ITLB + DTLB");
#else
    EQ((c0 >> 13) & 7, 0, "CONFIG0.AT = AVR32A");
    EQ((c0 >> 7) & 7, 3, "CONFIG0.MMUT = MPU");
#endif
    EQ(c0 & 3, 3, "CONFIG0.D and R");
    uint32_t a = mfsr(COUNT), b = mfsr(COUNT);
    EQ(b - a > 0 && b - a < 100, 1, "COUNT runs");
}

static void t_timer(void)
{
    IPR(0) = (0u << 30) | (uint32_t)(int0_stub - _evba);
    ticks = 0;
    n_order = 0;
    mtsr(COUNT, 0);
    mtsr(COMPARE, 300);
    __asm__ volatile("csrf 16" ::: "memory");
    for (unsigned spins = 0; ticks < 3 && spins < 100; spins++)
        __asm__ volatile("sleep 0" ::: "memory");
    __asm__ volatile("ssrf 16" ::: "memory");
    EQ(ticks, 3, "COMPARE interrupts");
    EQ(mfsr(ECR), (uint32_t)(int0_stub - _evba) >> 2, "ECR = autovector");
    EQ(IRR(0), 0, "request cleared");
}

static void t_priority(void)
{
    IPR(0) = (0u << 30) | (uint32_t)(int0_stub - _evba);
    IPR(1) = (2u << 30) | (uint32_t)(int2_stub - _evba);
    ticks = 2;   /* the next tick disables the timer */
    n_order = 0;
    mtsr(COUNT, 0);
    mtsr(COMPARE, 20);
    unsigned spins = 0;
    while (!(IRR(0) & 1) && spins < 1000)
        spins++;
    EQ(IRR(0), 1, "COMPARE request pending");
    UART_IER = 0x02;   /* THR empty */
    EQ(IRR(1), 1, "UART request pending");
    EQ(ICR(2), 1, "ICR2 = group 1");
    EQ(ICR(0), 0, "ICR0 = group 0");
    EQ(n_order, 0, "masked by GM");
    __asm__ volatile("csrf 16\n\tnop\n\tnop\n\tssrf 16" ::: "memory");
    EQ(n_order, 2, "both taken");
    EQ(order[0], 2, "INT2 first");
    EQ(order[1], 0, "then INT0");
    EQ(uart_icr, 1, "ICR2 in the handler");
}

#ifdef AVR32B
#define P1(pa) ((pa) | 0x80000000u)
static uint32_t ram_phys(void *p) { return (uint32_t)p & 0x1FFFFFFFu; }
static void tlb_write(unsigned idx, uint32_t vpn, uint32_t lo)
{
    mtsr(MMUCR, (mfsr(MMUCR) & ~(63u << 14)) | (idx << 14));
    mtsr(TLBEHI, vpn | (1u << 9));
    mtsr(TLBELO, lo);
    __asm__ volatile("tlbw" ::: "memory");
}

static void t_mmu(void)
{
    static uint32_t page[3][1024] __attribute__((aligned(4096)));
    uint32_t pa = ram_phys(page);
    EQ(mfsr(MMUCR) & 0x10, 0x10, "segmentation on at reset");
    /* G, AP = priv RW (010), 4 KB, dirty */
    tlb_write(0, 0x00010000u, pa | (1u << 8) | (2u << 4) | (1u << 2) | 2u);
    tlb_write(1, 0x00020000u, (pa + 0x1000) | (1u << 8) | (2u << 4) | (1u << 2));        /* clean */
    tlb_write(2, 0x00030000u, (pa + 0x2000) | (1u << 8) | (0u << 4) | (1u << 2) | 2u);   /* read-only */
    mtsr(MMUCR, mfsr(MMUCR) | 1);   /* E: paging */
    REG32(P1(pa) + 8) = 0xCAFE0001u;
    EQ(probe_ld(0x00010008u), 0xCAFE0001u, "paged load");
    EQ(probe_st(0x0001000Cu, 0xBEEF0002u), 0, "paged store");
    EQ(REG32(P1(pa) + 12), 0xBEEF0002u, "store reached the frame");
    trap_n = 0;
    EQ(probe_ld(0x00400000u), FAULTED, "unmapped load faults");
    EQ(last_ecr, 0x60 >> 2, "ECR = DTLB miss read");
    EQ(last_tlbear, 0x00400000u, "TLBEAR");
    EQ(mfsr(TLBEHI) & 0xFFFFFC00u, 0x00400000u, "TLBEHI.VPN");
    EQ(probe_st(0x00400004u, 1), FAULTED, "unmapped store faults");
    EQ(last_ecr, 0x70 >> 2, "ECR = DTLB miss write");
    EQ(probe_st(0x00020000u, 1), FAULTED, "store to a clean page");
    EQ(last_ecr, 0x44 >> 2, "ECR = DTLB modified");
    EQ(probe_ld(0x00020000u), 0, "load from a clean page");
    EQ(probe_st(0x00030000u, 1), FAULTED, "store to a read-only page");
    EQ(last_ecr, 0x40 >> 2, "ECR = DTLB protection write");
    EQ(trap_n, 4, "exception count");
    /* tlbs and tlbr */
    mtsr(TLBEHI, 0x00020000u);
    __asm__ volatile("tlbs" ::: "memory");
    EQ(mfsr(MMUCR) & 8, 0, "tlbs found");
    EQ((mfsr(MMUCR) >> 14) & 63, 1, "tlbs DRP");
    mtsr(TLBEHI, 0x00500000u);
    __asm__ volatile("tlbs" ::: "memory");
    EQ(mfsr(MMUCR) & 8, 8, "tlbs not found");
    mtsr(MMUCR, (mfsr(MMUCR) & ~(63u << 14)) | (2u << 14));
    __asm__ volatile("tlbr" ::: "memory");
    EQ(mfsr(TLBELO), (pa + 0x2000) | (1u << 8) | (1u << 2) | 2u, "tlbr TLBELO");
    EQ(mfsr(TLBEHI) & 0xFFFFFE00u, 0x00030200u, "tlbr TLBEHI");
    /* application mode cannot reach P1-P4 */
    /* invalidate */
    mtsr(MMUCR, mfsr(MMUCR) | 4);
    EQ(probe_ld(0x00010008u), FAULTED, "invalidated");
    mtsr(MMUCR, mfsr(MMUCR) & ~1u);
    EQ(probe_ld(P1(pa) + 8), 0xCAFE0001u, "P1 still untranslated");
}
#else
static uint32_t ram_code[4] __attribute__((aligned(4)));

static void t_mpu(void)
{
    /* 0: code memory 8 MB RWX, 1: RAM 8 MB RW, 2: peripherals 64 KB RW, 3: RAM 8 MB at
     * 8 MB, read-only but for its first subregion (MPUAPRB: RW) */
    mtsr(MPUAR(0), 0x80000000u | (22u << 1) | 1);
    mtsr(MPUAR(1), 0x00000000u | (22u << 1) | 1);
    mtsr(MPUAR(2), 0xFFFF0000u | (15u << 1) | 1);
    mtsr(MPUAR(3), 0x00800000u | (22u << 1) | 1);
    mtsr(MPUPSR(3), 1);
    mtsr(MPUAPRA, 0x0223u);
    mtsr(MPUAPRB, 0x2000u);
    mtsr(MPUCR, 1);
    trap_n = 0;
    EQ(probe_ld(0x00800000u), 0, "read-only region: load");
    EQ(probe_st(0x00800000u, 5), 0, "subregion 0 (APRB): store");
    EQ(probe_ld(0x00800000u), 5, "subregion 0 stored");
    EQ(probe_st(0x00880000u, 5), FAULTED, "subregion 1 (APRA): store faults");
    EQ(last_ecr, 0x40 >> 2, "ECR = DTLB protection write");
    EQ(last_tlbear, 0x00880000u, "TLBEAR");
    EQ(probe_ld(0x01000000u), FAULTED, "no region: load faults");
    EQ(last_ecr, 0x60 >> 2, "ECR = DTLB miss read");
    ram_code[0] = 0x5efc5efcu;   /* retal r12 */
    EQ(probe_call((uint32_t)ram_code), FAULTED, "fetch from a non-executable region");
    EQ(last_ecr, 0x18 >> 2, "ECR = ITLB protection");
    EQ(last_pc, (uint32_t)ram_code, "faulting pc");
    EQ(trap_n, 3, "exception count");
    mtsr(MPUCR, 0);
    EQ(probe_st(0x00880000u, 0), 0, "MPU off");
    EQ(probe_ld(0x00880000u), 0, "MPU off: stored");
}
#endif

int main(void);
int main(void)
{
    puts_("avr32 selftest ("
#ifdef AVR32B
          "AVR32B"
#else
          "AVR32A"
#endif
          ")\n");
    run("alu", t_alu);
    run("wide", t_wide);
    run("mem", t_mem);
    run("bits", t_bits);
    run("muldiv", t_muldiv);
    run("atomic", t_atomic);
    run("cond", t_cond);
    run("dsp", t_dsp);
#ifdef AVR32B
    run("simd", t_simd);
#endif
    run("breakpoint", t_breakpoint);
    run("scall", t_scall);
    run("user", t_user);
    run("exceptions", t_exceptions);
    run("config", t_config);
    run("timer", t_timer);
    run("priority", t_priority);
#ifdef AVR32B
    run("mmu", t_mmu);
#else
    run("mpu", t_mpu);
#endif
    puts_(n_fail ? "selftest: FAIL\n" : "selftest: PASS\n");
    return n_fail ? (int)((n_fail << 16) | 0x3333) : 0x5555;
}
