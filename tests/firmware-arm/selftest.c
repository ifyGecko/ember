/*
 * selftest.c - Bare-metal ARMv7-A self-test for the UAM arm-ref and armeb-ref boards,
 * built as ARM code and as Thumb-2 code (start.S is ARM either way), each in both byte
 * orders (big-endian: BE8, little-endian code).
 *
 * Exercises the emulator end to end: UART output, data processing and the flags
 * (carry chains, register-specified shifts, RRX), conditional execution (IT blocks
 * in Thumb), multiplies and divides, saturating and media instructions with Q and
 * GE, loads and stores in every addressing form, unaligned accesses, LDM/STM,
 * exclusives, interworking calls in both directions, TBB and CBZ, synchronous
 * exceptions with precise LR, SPSR, DFSR/DFAR and IFSR, an abort inside an IT block
 * that is retried after the handler fixes it, the generic timer and an SGI through
 * the GIC with WFI, banked registers and SPSR access, the MMU (sections, small
 * pages, domains, permissions, XN, ATS1CPR, remapping executed code with TLBIALL),
 * User mode (permission faults, SVC back to the kernel), CP15 identification, and the
 * byte order: CPSR.E and SCTLR.EE at reset, SETEND, E on exception entry and return.
 *
 * Output is the same for every build apart from the banner. The result is reported
 * through the board's test finisher.
 */
#include <stdint.h>

#ifdef __thumb__
#define IS_THUMB 1
#define ISET "thumb"
#else
#define IS_THUMB 0
#define ISET "arm"
#endif
#ifdef __ARMEB__
#define IS_BE 1
#define BUILD ISET ", big-endian"
#define OTHER_END "le"
#define NATIVE_END "be"
#else
#define IS_BE 0
#define BUILD ISET
#define OTHER_END "be"
#define NATIVE_END "le"
#endif
#define E(le, be) (IS_BE ? (be) : (le))   /* an expectation that depends on the byte order */

#define REG32(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define UART0    0x1c000000u
#define UART_THR (*(volatile uint8_t *)(UART0 + 0))
#define UART_LSR (*(volatile uint8_t *)(UART0 + 5))
#define GICD     0x1c010000u
#define GICC     0x1c020000u
#define BAD_ADDR 0x30000000u   /* nothing is mapped there */

/* The GIC's registers are little-endian: a big-endian CPU swaps, as readl/writel do. */
static uint32_t le32(uint32_t v) { return IS_BE ? __builtin_bswap32(v) : v; }
#define GIC_W(a, v) (REG32(a) = le32(v))
#define GIC_R(a)    le32(REG32(a))

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
static volatile uint32_t V0 = 0, V1 = 1, V2 = 2, V3 = 3, V5 = 5, V7 = 7, VM1 = 0xFFFFFFFFu,
                         VMIN = 0x80000000u, VMAX = 0x7FFFFFFFu;

#define mrc(op1, crn, crm, op2) ({ uint32_t v_; \
    __asm__ volatile("mrc p15, " #op1 ", %0, c" #crn ", c" #crm ", " #op2 : "=r"(v_)); v_; })
#define mcr(op1, crn, crm, op2, v) \
    __asm__ volatile("mcr p15, " #op1 ", %0, c" #crn ", c" #crm ", " #op2 :: "r"((uint32_t)(v)) : "memory")
#define isb() __asm__ volatile("isb" ::: "memory")
#define dsb() __asm__ volatile("dsb" ::: "memory")

/* ---- trap bookkeeping -------------------------------------------------- */

extern uint32_t g_guard[3];
void guard_resume(void);
uint32_t call_guarded(uint32_t fn);
void user_resume(void);
void enter_user(uint32_t pc, uint32_t arg);

static volatile uint32_t trap_n, last_kind, last_lr, last_spsr, last_fsr, last_far, last_fault_pc;
static volatile uint32_t last_handler_cpsr;
static volatile uint32_t timer_ticks, sgi_n, spurious;
static volatile int fix_reg = -1;      /* retry the faulting instruction with this register fixed */
static volatile uint32_t fix_val;
static volatile int user_test;

static unsigned insn_len(uint32_t addr, int thumb)
{
    if (!thumb)
        return 4;
    uint16_t hw = *(volatile uint16_t *)(uintptr_t)addr;   /* code is little-endian (BE8) */
    if (IS_BE)
        hw = __builtin_bswap16(hw);
    return (hw >> 11) >= 0x1D ? 4 : 2;
}

uint32_t trap_handler(uint32_t kind, uint32_t *frame);
uint32_t trap_handler(uint32_t kind, uint32_t *frame)
{
    uint32_t spsr = frame[0], lr = frame[14];
    int t = (spsr >> 5) & 1;

    if (kind == 6) {                                   /* IRQ */
        uint32_t iar = GIC_R(GICC + 0x0C), id = iar & 0x3FF;
        if (id == 30) {
            timer_ticks++;
            mcr(0, 14, 2, 1, 0);                       /* CNTP_CTL = 0 */
        } else if (id == 1) {
            sgi_n++;
        } else {
            spurious++;
        }
        if (id < 1020)
            GIC_W(GICC + 0x10, iar);
        return lr - 4;
    }

    uint32_t fault_pc;
    switch (kind) {
    case 1: fault_pc = lr - (t ? 2 : 4); break;        /* undefined */
    case 2: fault_pc = lr - (t ? 2 : 4); break;        /* SVC */
    case 3: fault_pc = lr - 4; last_fsr = mrc(0, 5, 0, 1); last_far = mrc(0, 6, 0, 2); break;
    default: fault_pc = lr - 8; last_fsr = mrc(0, 5, 0, 0); last_far = mrc(0, 6, 0, 0); break;
    }
    trap_n++;
    __asm__ volatile("mrs %0, cpsr" : "=r"(last_handler_cpsr));
    last_kind = kind;
    last_lr = lr;
    last_spsr = spsr;
    last_fault_pc = fault_pc;

    if (user_test && (spsr & 0x1F) == 0x10) {          /* back to the kernel */
        user_test = 0;
        frame[0] = 0x1D3 | (IS_BE ? 1u << 9 : 0);      /* SVC, A I F masked, our byte order */
        return (uint32_t)user_resume;
    }
    if (g_guard[2]) {                                  /* a guarded call faulted */
        frame[0] = spsr & ~(0x20u | 0x0600FC00u);      /* ARM, outside any IT block */
        return (uint32_t)guard_resume;
    }
    if (fix_reg >= 0) {                                /* fix and retry */
        frame[1 + fix_reg] = fix_val;
        fix_reg = -1;
        return fault_pc;
    }
    if (kind == 2)
        return lr;
    return fault_pc + insn_len(fault_pc, t);           /* skip it */
}

static uint32_t cpsr(void) { uint32_t v; __asm__ volatile("mrs %0, cpsr" : "=r"(v)); return v; }

/* ---- ALU and flags ----------------------------------------------------- */

/* NZCV after an instruction: op is "<insn> %0, %1, %2" with flags set. */
#define FLAGS3(insn, a, b) ({ uint32_t r_, f_; \
    __asm__ volatile(insn " %0, %2, %3\n\tmrs %1, apsr" : "=&r"(r_), "=r"(f_) : "r"(a), "r"(b) : "cc"); \
    f_ >> 28; })

static void t_alu(void)
{
    EQ(FLAGS3("adds", VMAX, V1), 0x9, "adds overflow NZCV");
    EQ(FLAGS3("subs", V0, V1), 0x8, "subs borrow NZCV");
    EQ(FLAGS3("subs", V5, V5), 0x6, "subs equal NZCV");
    EQ(FLAGS3("adds", VM1, V1), 0x6, "adds carry NZCV");
    EQ(FLAGS3("subs", VMIN, V1), 0x3, "subs overflow NZCV");

    /* 64-bit add and subtract through the carry */
    uint32_t lo, hi;
    __asm__ volatile("adds %0, %2, %3\n\tadc %1, %4, %5" : "=&r"(lo), "=r"(hi)
                     : "r"(VM1), "r"(V2), "r"(V3), "r"(V0) : "cc");
    EQ(lo, 1, "adds/adc lo");
    EQ(hi, 4, "adds/adc hi");
    __asm__ volatile("subs %0, %2, %3\n\tsbc %1, %4, %5" : "=&r"(lo), "=r"(hi)
                     : "r"(V1), "r"(V2), "r"(V3), "r"(V0) : "cc");
    EQ(lo, 0xFFFFFFFFu, "subs/sbc lo");
    EQ(hi, 2, "subs/sbc hi");

    uint32_t r;
    __asm__ volatile("rsb %0, %1, #100" : "=r"(r) : "r"(V7));
    EQ(r, 93, "rsb");
    __asm__ volatile("mvn %0, %1" : "=r"(r) : "r"(V5));
    EQ(r, ~5u, "mvn");
    __asm__ volatile("bic %0, %1, #0xff00" : "=r"(r) : "r"(VM1));
    EQ(r, 0xFFFF00FFu, "bic imm");
    __asm__ volatile("orr %0, %1, #0x3fc00" : "=r"(r) : "r"(V0));
    EQ(r, 0x3FC00u, "rotated immediate");
    __asm__ volatile("eor %0, %1, %2, lsl #4" : "=r"(r) : "r"(V3), "r"(V5));
    EQ(r, 0x53, "eor shifted");
    __asm__ volatile("add %0, %1, %2, asr #1" : "=r"(r) : "r"(V3), "r"(VM1));
    EQ(r, 2, "add asr");
    __asm__ volatile("clz %0, %1" : "=r"(r) : "r"(0x00010000u + V0));
    EQ(r, 15, "clz");
    __asm__ volatile("clz %0, %1" : "=r"(r) : "r"(V0));
    EQ(r, 32, "clz 0");
}

static void t_shifts(void)
{
    uint32_t r, f, n32 = 32 + V0, n33 = 33 + V0, n40 = 40 + V0, n4 = 4 + V0;
    /* register-specified amounts are taken from the bottom byte, unmasked */
    __asm__ volatile("lsls %0, %2, %3\n\tmrs %1, apsr" : "=&r"(r), "=r"(f) : "r"(V1 | 0x80u), "r"(n32) : "cc");
    EQ(r, 0, "lsl 32");
    EQ((f >> 29) & 1, 1, "lsl 32 carry = bit 0");
    __asm__ volatile("lsrs %0, %2, %3\n\tmrs %1, apsr" : "=&r"(r), "=r"(f) : "r"(VM1), "r"(n33) : "cc");
    EQ(r, 0, "lsr 33");
    EQ((f >> 29) & 1, 0, "lsr 33 carry");
    __asm__ volatile("asrs %0, %1, %2" : "=r"(r) : "r"(VMIN), "r"(n40) : "cc");
    EQ(r, 0xFFFFFFFFu, "asr 40");
    __asm__ volatile("rors %0, %2, %3\n\tmrs %1, apsr" : "=&r"(r), "=r"(f) : "r"(VMIN | 1u), "r"(n32) : "cc");
    EQ(r, 0x80000001u, "ror 32");
    EQ((f >> 29) & 1, 1, "ror 32 carry");
    __asm__ volatile("rors %0, %1, %2" : "=r"(r) : "r"(0x12345678u + V0), "r"(n4) : "cc");
    EQ(r, 0x81234567u, "ror 4");
    __asm__ volatile("lsls %0, %1, %2" : "=r"(r) : "r"(V3), "r"(0x104u + V0) : "cc");
    EQ(r, 48, "shift amount is the bottom byte");
    /* RRX: carry into bit 31, bit 0 out */
    __asm__ volatile("msr apsr_nzcvq, %2\n\trrxs %0, %3\n\tmrs %1, apsr"
                     : "=&r"(r), "=r"(f) : "r"(0x20000000u + V0), "r"(V3) : "cc");
    EQ(r, 0x80000001u, "rrx");
    EQ((f >> 29) & 1, 1, "rrx carry out");
    __asm__ volatile("lsrs %0, %2, #32\n\tmrs %1, apsr" : "=&r"(r), "=r"(f) : "r"(VMIN) : "cc");
    EQ(r, 0, "lsr #32");
    EQ(f >> 28, 0x6, "lsr #32 flags");
}

static void t_cond(void)
{
    uint32_t r;
#ifdef __thumb__
    __asm__ volatile("cmp %1, #5\n\tite eq\n\tmoveq %0, #1\n\tmovne %0, #2" : "=r"(r) : "r"(V5) : "cc");
    EQ(r, 1, "ite eq taken");
    __asm__ volatile("cmp %1, #5\n\tite eq\n\tmoveq %0, #1\n\tmovne %0, #2" : "=r"(r) : "r"(V7) : "cc");
    EQ(r, 2, "ite eq not taken");
    /* a full IT block: flags are not set by 16-bit instructions inside it */
    __asm__ volatile("movs %0, #0\n\tcmp %1, #3\n\titttt hi\n\taddhi %0, #1\n\taddhi %0, #2\n\t"
                     "addhi %0, #4\n\taddhi %0, #8" : "=&r"(r) : "r"(V5) : "cc");
    EQ(r, 15, "itttt");
    __asm__ volatile("movs %0, #0\n\tcmp %1, #3\n\titete lt\n\taddlt %0, #1\n\taddge %0, #2\n\t"
                     "addlt %0, #4\n\taddge %0, #8" : "=&r"(r) : "r"(V5) : "cc");
    EQ(r, 10, "itete");
#else
    __asm__ volatile("cmp %1, #5\n\tmoveq %0, #1\n\tmovne %0, #2" : "=r"(r) : "r"(V5) : "cc");
    EQ(r, 1, "moveq");
    __asm__ volatile("cmp %1, #5\n\tmoveq %0, #1\n\tmovne %0, #2" : "=r"(r) : "r"(V7) : "cc");
    EQ(r, 2, "movne");
    __asm__ volatile("mov %0, #0\n\tcmp %1, #3\n\taddhi %0, %0, #1\n\taddls %0, %0, #2\n\t"
                     "addgt %0, %0, #4\n\taddle %0, %0, #8\n\taddvs %0, %0, #16" : "=&r"(r) : "r"(V5) : "cc");
    EQ(r, 5, "conditional chain");
    uint32_t mem = 0;
    __asm__ volatile("cmp %1, #0\n\tstreq %2, [%3]\n\tstrne %4, [%3]" :: "r"(mem), "r"(V0), "r"(V7),
                     "r"(&mem), "r"(V3) : "cc", "memory");
    EQ(mem, 7, "streq");
#endif
}

/* ---- multiply, divide, saturate, media ---------------------------------- */

static void t_muldiv(void)
{
    uint32_t lo, hi, r;
    __asm__ volatile("umull %0, %1, %2, %3" : "=&r"(lo), "=&r"(hi) : "r"(VM1), "r"(VM1));
    EQ(lo, 1, "umull lo");
    EQ(hi, 0xFFFFFFFEu, "umull hi");
    __asm__ volatile("smull %0, %1, %2, %3" : "=&r"(lo), "=&r"(hi) : "r"(VM1), "r"(V7));
    EQ(lo, (uint32_t)-7, "smull lo");
    EQ(hi, 0xFFFFFFFFu, "smull hi");
    lo = 0xFFFFFFFFu + V0;
    hi = 1 + V0;
    __asm__ volatile("umlal %0, %1, %2, %3" : "+r"(lo), "+r"(hi) : "r"(V2), "r"(V1));
    EQ(lo, 1, "umlal lo");
    EQ(hi, 2, "umlal hi");
    lo = 1 + V0;
    hi = 2 + V0;
    __asm__ volatile("umaal %0, %1, %2, %3" : "+r"(lo), "+r"(hi) : "r"(VM1), "r"(VM1));
    EQ(lo, 4, "umaal lo");
    EQ(hi, 0xFFFFFFFEu, "umaal hi");
    __asm__ volatile("mla %0, %1, %2, %3" : "=r"(r) : "r"(V3), "r"(V5), "r"(V7));
    EQ(r, 22, "mla");
    __asm__ volatile("mls %0, %1, %2, %3" : "=r"(r) : "r"(V3), "r"(V5), "r"(V7));
    EQ(r, (uint32_t)-8, "mls");
    __asm__ volatile("sdiv %0, %1, %2" : "=r"(r) : "r"(V7), "r"((uint32_t)-2 + V0));
    EQ(r, (uint32_t)-3, "sdiv");
    __asm__ volatile("sdiv %0, %1, %2" : "=r"(r) : "r"(V7), "r"(V0));
    EQ(r, 0, "sdiv by 0");
    __asm__ volatile("sdiv %0, %1, %2" : "=r"(r) : "r"(VMIN), "r"(VM1));
    EQ(r, 0x80000000u, "sdiv MIN/-1");
    __asm__ volatile("udiv %0, %1, %2" : "=r"(r) : "r"(VM1), "r"(V2));
    EQ(r, 0x7FFFFFFFu, "udiv");
    __asm__ volatile("udiv %0, %1, %2" : "=r"(r) : "r"(V7), "r"(V0));
    EQ(r, 0, "udiv by 0");

    __asm__ volatile("smulbb %0, %1, %2" : "=r"(r) : "r"(0x0002FFFFu + V0), "r"(0x00050003u + V0));
    EQ(r, (uint32_t)-3, "smulbb");
    __asm__ volatile("smultb %0, %1, %2" : "=r"(r) : "r"(0x0002FFFFu + V0), "r"(0x00050003u + V0));
    EQ(r, 6, "smultb");
    __asm__ volatile("smulwb %0, %1, %2" : "=r"(r) : "r"(0x40000000u + V0), "r"(V2));
    EQ(r, 0x8000, "smulwb");
    __asm__ volatile("smmul %0, %1, %2" : "=r"(r) : "r"(0x40000000u + V0), "r"(0x40000000u + V0));
    EQ(r, 0x10000000u, "smmul");
    __asm__ volatile("smmulr %0, %1, %2" : "=r"(r) : "r"(0x00010000u + V0), "r"(0x00008000u + V0));
    EQ(r, 1, "smmulr rounds");
    __asm__ volatile("smuad %0, %1, %2" : "=r"(r) : "r"(0x00030002u + V0), "r"(0x00050004u + V0));
    EQ(r, 23, "smuad");
    __asm__ volatile("smusd %0, %1, %2" : "=r"(r) : "r"(0x00030002u + V0), "r"(0x00050004u + V0));
    EQ(r, (uint32_t)-7, "smusd");
}

static uint32_t q_flag(void) { return (cpsr() >> 27) & 1; }
static void clear_q(void) { __asm__ volatile("msr apsr_nzcvq, %0" :: "r"(V0) : "cc"); }

static void t_saturate(void)
{
    uint32_t r;
    clear_q();
    __asm__ volatile("qadd %0, %1, %2" : "=r"(r) : "r"(VMAX), "r"(V1));
    EQ(r, 0x7FFFFFFFu, "qadd saturates");
    EQ(q_flag(), 1, "qadd sets Q");
    clear_q();
    __asm__ volatile("qsub %0, %1, %2" : "=r"(r) : "r"(V5), "r"(V7));
    EQ(r, (uint32_t)-2, "qsub");
    EQ(q_flag(), 0, "qsub leaves Q");
    __asm__ volatile("qdadd %0, %1, %2" : "=r"(r) : "r"(V1), "r"(0x40000000u + V0));
    EQ(r, 0x7FFFFFFFu, "qdadd");
    EQ(q_flag(), 1, "qdadd sets Q");
    clear_q();
    __asm__ volatile("ssat %0, #8, %1" : "=r"(r) : "r"(300 + V0));
    EQ(r, 127, "ssat");
    __asm__ volatile("ssat %0, #8, %1, lsl #1" : "=r"(r) : "r"((uint32_t)-100 + V0));
    EQ(r, (uint32_t)-128, "ssat lsl");
    __asm__ volatile("usat %0, #8, %1" : "=r"(r) : "r"((uint32_t)-5 + V0));
    EQ(r, 0, "usat negative");
    EQ(q_flag(), 1, "sat sets Q");
    clear_q();
    __asm__ volatile("usat %0, #8, %1" : "=r"(r) : "r"(200 + V0));
    EQ(r, 200, "usat in range");
    EQ(q_flag(), 0, "usat in range leaves Q");
    __asm__ volatile("smlabb %0, %1, %2, %3" : "=r"(r) : "r"(V1), "r"(V1), "r"(VMAX));
    EQ(r, 0x80000000u, "smlabb wraps");
    EQ(q_flag(), 1, "smlabb sets Q");
    clear_q();
}

static void t_media(void)
{
    uint32_t r;
    __asm__ volatile("rev %0, %1" : "=r"(r) : "r"(0x11223344u + V0));
    EQ(r, 0x44332211u, "rev");
    __asm__ volatile("rev16 %0, %1" : "=r"(r) : "r"(0x11223344u + V0));
    EQ(r, 0x22114433u, "rev16");
    __asm__ volatile("revsh %0, %1" : "=r"(r) : "r"(0x00001280u + V0));
    EQ(r, 0xFFFF8012u, "revsh");
    __asm__ volatile("rbit %0, %1" : "=r"(r) : "r"(0x00000013u + V0));
    EQ(r, 0xC8000000u, "rbit");
    __asm__ volatile("ubfx %0, %1, #8, #12" : "=r"(r) : "r"(0xABCD1234u + V0));
    EQ(r, 0xD12, "ubfx");
    __asm__ volatile("sbfx %0, %1, #12, #4" : "=r"(r) : "r"(0x00008000u + V0));
    EQ(r, 0xFFFFFFF8u, "sbfx");
    r = VM1;
    __asm__ volatile("bfi %0, %1, #4, #4" : "+r"(r) : "r"(V5));
    EQ(r, 0xFFFFFF5Fu, "bfi");
    __asm__ volatile("bfc %0, #8, #8" : "+r"(r));
    EQ(r, 0xFFFF005Fu, "bfc");
    __asm__ volatile("uxtab %0, %1, %2, ror #8" : "=r"(r) : "r"(0x100u + V0), "r"(0x1234u + V0));
    EQ(r, 0x112, "uxtab ror 8");
    __asm__ volatile("sxth %0, %1" : "=r"(r) : "r"(0x18000u + V0));
    EQ(r, 0xFFFF8000u, "sxth");
    __asm__ volatile("sxtb %0, %1, ror #16" : "=r"(r) : "r"(0x00800000u + V0));
    EQ(r, 0xFFFFFF80u, "sxtb ror 16");
    __asm__ volatile("uxtb16 %0, %1" : "=r"(r) : "r"(0x11223344u + V0));
    EQ(r, 0x00220044u, "uxtb16");

    uint32_t s;
    __asm__ volatile("uadd8 %0, %2, %3\n\tsel %1, %4, %5" : "=&r"(r), "=r"(s)
                     : "r"(0x80FF0102u + V0), "r"(0x80010101u + V0), "r"(0x11111111u + V0),
                       "r"(0x22222222u + V0));
    EQ(r, 0x00000203u, "uadd8");
    EQ(s, 0x11112222u, "sel after uadd8 (GE)");
    __asm__ volatile("ssub16 %0, %2, %3\n\tsel %1, %4, %5" : "=&r"(r), "=r"(s)
                     : "r"(0x00050001u + V0), "r"(0x00030002u + V0), "r"(0xAAAAAAAAu + V0),
                       "r"(0x55555555u + V0));
    EQ(r, 0x0002FFFFu, "ssub16");
    EQ(s, 0xAAAA5555u, "sel after ssub16 (GE)");
    __asm__ volatile("uqadd8 %0, %1, %2" : "=r"(r) : "r"(0xF0F0F0F0u + V0), "r"(0x20100100u + V0));
    EQ(r, 0xFFFFF1F0u, "uqadd8");
    __asm__ volatile("shadd16 %0, %1, %2" : "=r"(r) : "r"(0x7FFF0003u + V0), "r"(0x7FFF0001u + V0));
    EQ(r, 0x7FFF0002u, "shadd16");
    __asm__ volatile("pkhbt %0, %1, %2, lsl #16" : "=r"(r) : "r"(0x1234ABCDu + V0), "r"(0x5678EF01u + V0));
    EQ(r, 0xEF01ABCDu, "pkhbt");
    __asm__ volatile("pkhtb %0, %1, %2, asr #16" : "=r"(r) : "r"(0x1234ABCDu + V0), "r"(0x5678EF01u + V0));
    EQ(r, 0x12345678u, "pkhtb");
    __asm__ volatile("usad8 %0, %1, %2" : "=r"(r) : "r"(0x01020304u + V0), "r"(0x04030201u + V0));
    EQ(r, 8, "usad8");
    __asm__ volatile("movw %0, #0x1234\n\tmovt %0, #0xabcd" : "=r"(r));
    EQ(r, 0xABCD1234u, "movw/movt");
}

/* ---- loads and stores --------------------------------------------------- */

static uint8_t buf[64] __attribute__((aligned(8)));

static void t_loadstore(void)
{
    for (unsigned i = 0; i < sizeof(buf); i++)
        buf[i] = (uint8_t)(0x80 + i);
    uint32_t r, p = (uint32_t)buf;
    __asm__ volatile("ldrsb %0, [%1]" : "=r"(r) : "r"(p));
    EQ(r, 0xFFFFFF80u, "ldrsb");
    __asm__ volatile("ldrsh %0, [%1, #2]" : "=r"(r) : "r"(p));
    EQ(r, E(0xFFFF8382u, 0xFFFF8283u), "ldrsh");
    __asm__ volatile("ldrh %0, [%1, #2]" : "=r"(r) : "r"(p));
    EQ(r, E(0x8382, 0x8283), "ldrh");
    uint32_t q = p;
    __asm__ volatile("ldr %0, [%1], #4" : "=r"(r), "+r"(q));
    EQ(r, E(0x83828180u, 0x80818283u), "ldr post-index");
    EQ(q, p + 4, "post-index writeback");
    __asm__ volatile("ldr %0, [%1, #4]!" : "=r"(r), "+r"(q));
    EQ(r, E(0x8B8A8988u, 0x88898A8Bu), "ldr pre-index");
    EQ(q, p + 8, "pre-index writeback");
    uint32_t idx = 3 + V0;
    __asm__ volatile("ldrb %0, [%1, %2, lsl #2]" : "=r"(r) : "r"(p), "r"(idx));
    EQ(r, 0x8C, "ldrb register lsl");
    uint32_t a, b;
    __asm__ volatile("ldrd r4, r5, [%2, #8]\n\tmov %0, r4\n\tmov %1, r5" : "=r"(a), "=r"(b) : "r"(p) : "r4", "r5");
    EQ(a, E(0x8B8A8988u, 0x88898A8Bu), "ldrd lo");
    EQ(b, E(0x8F8E8D8Cu, 0x8C8D8E8Fu), "ldrd hi");
    __asm__ volatile("mov r4, %0\n\tmov r5, %1\n\tstrd r4, r5, [%2, #16]" :: "r"(V5), "r"(V7), "r"(p) : "r4", "r5", "memory");
    EQ(*(volatile uint32_t *)(p + 16), 5, "strd lo");
    EQ(*(volatile uint32_t *)(p + 20), 7, "strd hi");
    __asm__ volatile("strh %0, [%1, #24]" :: "r"(0x12345678u + V0), "r"(p) : "memory");
    EQ(*(volatile uint32_t *)(p + 24), E(0x9B9A5678u, 0x56789A9Bu), "strh");

    /* unaligned word and halfword accesses (SCTLR.A = 0) */
    __asm__ volatile("ldr %0, [%1, #1]" : "=r"(r) : "r"(p));
    EQ(r, E(0x84838281u, 0x81828384u), "unaligned ldr");
    __asm__ volatile("ldrh %0, [%1, #3]" : "=r"(r) : "r"(p));
    EQ(r, E(0x8483, 0x8384), "unaligned ldrh");
    __asm__ volatile("str %0, [%1, #33]" :: "r"(0xDEADBEEFu + V0), "r"(p) : "memory");
    EQ(*(volatile uint32_t *)(p + 32), E(0xADBEEFA0u, 0xA0DEADBEu), "unaligned str (low)");
    EQ(*(volatile uint8_t *)(p + 36), E(0xDE, 0xEF), "unaligned str (high)");

    /* block transfers */
    static uint32_t words[8];
    uint32_t base = (uint32_t)&words[4], x, y;
    __asm__ volatile("mov r4, %1\n\tmov r5, %2\n\tstmdb %0!, {r4, r5}" : "+r"(base) : "r"(V3), "r"(V5)
                     : "r4", "r5", "memory");
    EQ(base, (uint32_t)&words[2], "stmdb writeback");
    EQ(words[2], 3, "stmdb first");
    EQ(words[3], 5, "stmdb second");
    __asm__ volatile("ldmia %0!, {r4, r5}\n\tmov %1, r4\n\tmov %2, r5" : "+r"(base), "=r"(x), "=r"(y)
                     :: "r4", "r5", "memory");
    EQ(base, (uint32_t)&words[4], "ldmia writeback");
    EQ(x + 10 * y, 53, "ldmia values");
#ifndef __thumb__
    __asm__ volatile("ldmib %1, {%0}" : "=r"(x) : "r"(&words[1]) : "memory");
    EQ(x, 3, "ldmib");
    __asm__ volatile("ldmda %1, {%0}" : "=r"(x) : "r"(&words[3]) : "memory");
    EQ(x, 5, "ldmda");
    uint32_t old, sw = 11;
    __asm__ volatile("swp %0, %1, [%2]" : "=&r"(old) : "r"(V7), "r"(&sw) : "memory");
    EQ(old, 11, "swp old");
    EQ(sw, 7, "swp new");
#endif
}

static void t_exclusive(void)
{
    static volatile uint32_t w;
    static volatile uint64_t d __attribute__((aligned(8)));
    uint32_t v, st;
    w = 41;
    __asm__ volatile("ldrex %0, [%2]\n\tadd %0, %0, #1\n\tstrex %1, %0, [%2]"
                     : "=&r"(v), "=&r"(st) : "r"(&w) : "memory");
    EQ(st, 0, "strex succeeds");
    EQ(w, 42, "strex stored");
    __asm__ volatile("ldrex %0, [%2]\n\tclrex\n\tstrex %1, %3, [%2]"
                     : "=&r"(v), "=&r"(st) : "r"(&w), "r"(V7) : "memory");
    EQ(st, 1, "strex after clrex fails");
    EQ(w, 42, "failed strex stores nothing");
    __asm__ volatile("strex %0, %1, [%2]" : "=&r"(st) : "r"(V7), "r"(&w) : "memory");
    EQ(st, 1, "strex without ldrex fails");
    d = 0x1111222233334444ull;
    uint32_t lo, hi;
    __asm__ volatile("mov r6, %4\n\tmov r7, %5\n\tldrexd r4, r5, [%3]\n\tstrexd %2, r6, r7, [%3]\n\t"
                     "mov %0, r4\n\tmov %1, r5"
                     : "=&r"(lo), "=&r"(hi), "=&r"(st) : "r"(&d), "r"(V5), "r"(V7)
                     : "r4", "r5", "r6", "r7", "memory");
    EQ(lo, E(0x33334444u, 0x11112222u), "ldrexd: Rt is the word at the address");
    EQ(hi, E(0x11112222u, 0x33334444u), "ldrexd: Rt2 the next");
    EQ(st, 0, "strexd succeeds");
    EQ((uint32_t)d, E(5, 7), "strexd: Rt to the word at the address");
    EQ((uint32_t)(d >> 32), E(7, 5), "strexd: Rt2 to the next");
    static volatile uint8_t bb = 9;
    __asm__ volatile("ldrexb %0, [%2]\n\tstrexb %1, %3, [%2]" : "=&r"(v), "=&r"(st) : "r"(&bb), "r"(V3) : "memory");
    EQ(v, 9, "ldrexb");
    EQ(bb, 3, "strexb");
}

/* ---- interworking, table branches --------------------------------------- */

#ifdef __thumb__
__attribute__((target("arm"), noinline)) static uint32_t other_isa(uint32_t x)
#else
__attribute__((target("thumb"), noinline)) static uint32_t other_isa(uint32_t x)
#endif
{
    __asm__ volatile("" ::: "memory");
    return x * 3 + 1;
}

static uint32_t (*volatile other_ptr)(uint32_t) = other_isa;

static void t_interwork(void)
{
    EQ(other_isa(V5), 16, "direct call to the other instruction set");
    EQ(other_ptr(V7), 22, "pointer call to the other instruction set");
    EQ(((uint32_t)other_ptr & 1), !IS_THUMB, "function pointer state bit");
#ifdef __thumb__
    uint32_t r;
    __asm__ volatile("tbb [pc, %1]\n"
                     "1: .byte (2f - 1b) / 2, (3f - 1b) / 2\n"
                     "2: movs %0, #10\n\tb 4f\n"
                     "3: movs %0, #20\n"
                     "4:" : "=r"(r) : "r"(V1) : "cc");
    EQ(r, 20, "tbb");
    __asm__ volatile("tbh [pc, %1, lsl #1]\n"
                     "1: .hword (2f - 1b) / 2, (3f - 1b) / 2\n"
                     "2: movs %0, #10\n\tb 4f\n"
                     "3: movs %0, #20\n"
                     "4:" : "=r"(r) : "r"(V0) : "cc");
    EQ(r, 10, "tbh");
    __asm__ volatile("cbz %1, 1f\n\tmovs %0, #1\n\tb 2f\n1: movs %0, #2\n2:" : "=r"(r) : "r"(V0) : "cc");
    EQ(r, 2, "cbz taken");
    __asm__ volatile("cbnz %1, 1f\n\tmovs %0, #1\n\tb 2f\n1: movs %0, #2\n2:" : "=r"(r) : "r"(V0) : "cc");
    EQ(r, 1, "cbnz not taken");
#endif
}

/* ---- exceptions --------------------------------------------------------- */

static void t_exceptions(void)
{
    uint32_t at;
    trap_n = 0;
#ifdef __thumb__
    __asm__ volatile("adr %0, 1f\n1: .inst.n 0xde00" : "=r"(at) :: "memory");  /* udf #0 */
#else
    __asm__ volatile("adr %0, 1f\n1: .inst 0xe7f000f0" : "=r"(at) :: "memory");  /* udf #0 */
#endif
    EQ(trap_n, 1, "undefined trapped");
    EQ(last_kind, 1, "undefined vector");
    EQ(last_fault_pc, at, "undefined: LR");
    EQ(last_spsr & 0x3F, 0x13 | (IS_THUMB << 5), "undefined: SPSR mode and T");

    __asm__ volatile("adr %0, 1f\n1: svc #0x12" : "=r"(at) :: "memory");
    EQ(last_kind, 2, "svc vector");
    EQ(last_fault_pc, at, "svc: LR");

    __asm__ volatile("adr %0, 1f\n1: bkpt #3" : "=r"(at) :: "memory");
    EQ(last_kind, 3, "bkpt: prefetch abort");
    EQ(last_fault_pc, at, "bkpt: LR");
    EQ(last_fsr & 0x40F, 2, "bkpt: IFSR debug event");

    uint32_t v;
    __asm__ volatile("adr %0, 1f\n1: ldr %1, [%2]" : "=&r"(at), "=&r"(v) : "r"(BAD_ADDR) : "memory");
    EQ(last_kind, 4, "load bus error: data abort");
    EQ(last_fault_pc, at, "data abort: LR");
    EQ(last_fsr & 0xC0F, 0x008, "load: DFSR external abort, read");
    EQ(last_far, BAD_ADDR, "load: DFAR");
    __asm__ volatile("str %0, [%1, #4]" :: "r"(V1), "r"(BAD_ADDR) : "memory");
    EQ(last_fsr & 0xC0F, 0x808, "store: DFSR WnR");
    EQ(last_far, BAD_ADDR + 4, "store: DFAR");

    uint32_t a, b;
    __asm__ volatile("ldrd r4, r5, [%2]\n\tmov %0, r4\n\tmov %1, r5" : "=r"(a), "=r"(b)
                     : "r"((uint32_t)buf + 2) : "r4", "r5", "memory");
    EQ(last_fsr & 0x40F, 1, "ldrd misaligned: alignment fault");
    __asm__ volatile("ldrex %0, [%1]" : "=&r"(a) : "r"((uint32_t)buf + 1) : "memory");
    EQ(last_fsr & 0x40F, 1, "ldrex misaligned: alignment fault");
    EQ(trap_n, 7, "trap count");

    /* A faulting load is retried after the handler repairs its base register. */
    static volatile uint32_t good = 0x600DF00Du;
    fix_reg = 2;
    fix_val = (uint32_t)&good;
    __asm__ volatile("mov r2, %1\n\tldr %0, [r2]" : "=r"(v) : "r"(BAD_ADDR) : "r2", "memory");
    EQ(v, 0x600DF00Du, "retried load");

#ifdef __thumb__
    /* An abort inside an IT block, retried: the IT state comes back through SPSR. */
    uint32_t r3 = 0;
    fix_reg = 2;
    fix_val = (uint32_t)&good;
    __asm__ volatile("mov r2, %2\n\tcmp %3, #0\n\tittee eq\n\tldreq %0, [r2]\n\taddeq %1, %1, #1\n\t"
                     "addne %1, %1, #100\n\taddne %1, %1, #200"
                     : "=&r"(v), "+r"(r3) : "r"(BAD_ADDR), "r"(V0) : "r2", "cc", "memory");
    EQ(v, 0x600DF00Du, "IT block: retried load");
    EQ(r3, 1, "IT block: rest of the block after the retry");
    const uint32_t it = 0x07;                         /* ITTEE EQ at its first instruction */
    EQ(last_spsr & 0x0600FC00u, ((it & 3) << 25) | ((it >> 2) << 10), "IT block: SPSR.IT at the abort");
#endif
}

/* ---- interrupts --------------------------------------------------------- */

static void gic_init(void)
{
    GIC_W(GICD + 0x000, 1);                           /* distributor on */
    GIC_W(GICD + 0x100, (1u << 30) | (1u << 1));      /* PPI 30 (timer), SGI 1 */
    GIC_W(GICC + 0x004, 0xF0);                        /* priority mask */
    GIC_W(GICC + 0x000, 1);                           /* CPU interface on */
}

static void t_timer_irq(void)
{
    gic_init();
    EQ(mrc(0, 14, 0, 0), 62500000u, "CNTFRQ");
    uint32_t lo1, hi1, lo2, hi2;
    __asm__ volatile("mrrc p15, 0, %0, %1, c14" : "=r"(lo1), "=r"(hi1));
    __asm__ volatile("mrrc p15, 0, %0, %1, c14" : "=r"(lo2), "=r"(hi2));
    check(lo2 > lo1 || hi2 > hi1, "CNTPCT advances", lo2, lo1);

    timer_ticks = 0;
    mcr(0, 14, 2, 0, 2000);                           /* CNTP_TVAL */
    mcr(0, 14, 2, 1, 1);                              /* CNTP_CTL: enable */
    EQ(mrc(0, 14, 2, 1) & 4, 0, "ISTATUS clear before the deadline");
    __asm__ volatile("cpsie i");
    for (unsigned k = 0; k < 10 && !timer_ticks; k++)
        __asm__ volatile("wfi");
    __asm__ volatile("cpsid i");
    EQ(timer_ticks, 1, "timer interrupt through the GIC");
    EQ(GIC_R(GICC + 0x18), 1023, "nothing pending after EOI");

    /* masked: the interrupt is held until CPSR.I clears */
    mcr(0, 14, 2, 0, 50);
    mcr(0, 14, 2, 1, 1);
    for (volatile unsigned k = 0; k < 200; k++) {}
    EQ(mrc(0, 14, 2, 1) & 4, 4, "ISTATUS set after the deadline");
    EQ(timer_ticks, 1, "no interrupt while masked");
    EQ(mrc(0, 12, 1, 0) & 0x80, 0x80, "ISR.I pending");
    __asm__ volatile("cpsie i\n\tnop\n\tnop\n\tcpsid i");
    EQ(timer_ticks, 2, "taken once unmasked");
    EQ(spurious, 0, "no spurious interrupts");
}

static void t_sgi(void)
{
    sgi_n = 0;
    GIC_W(GICD + 0xF00, (2u << 24) | 1);              /* SGI 1 to this CPU */
    EQ(sgi_n, 0, "SGI held while masked");
    __asm__ volatile("cpsie i\n\tnop\n\tnop\n\tcpsid i");
    EQ(sgi_n, 1, "SGI taken");
}

/* ---- modes -------------------------------------------------------------- */

static void t_modes(void)
{
    EQ(cpsr() & 0x1F, 0x13, "running in SVC");
    uint32_t sp_svc, sp_irq, r8v;
    __asm__ volatile("mov %0, sp\n\tcps #0x12\n\tmov %1, sp\n\tcps #0x13" : "=r"(sp_svc), "=r"(sp_irq));
    check(sp_svc != sp_irq, "banked sp", sp_irq, sp_svc);
    uint32_t sp_now;
    __asm__ volatile("mov %0, sp" : "=r"(sp_now));
    EQ(sp_now, sp_svc, "SVC sp kept across a mode switch");
    __asm__ volatile("mov r8, #1\n\tcps #0x11\n\tmov r8, #2\n\tcps #0x12\n\tmov %0, r8\n\tcps #0x13"
                     : "=r"(r8v) :: "r8");
    EQ(r8v, 1, "FIQ banks r8");
    uint32_t spsr_v;
    __asm__ volatile("msr spsr_fsxc, %1\n\tmrs %0, spsr" : "=r"(spsr_v) : "r"(0xA00001D0u + V0));
    EQ(spsr_v, 0xA00001D0u, "SPSR write/read");
#ifndef __thumb__
    uint32_t usp;
    volatile uint32_t usp2 = 0;
    __asm__ volatile("cps #0x1f\n\tmov %0, sp\n\tcps #0x13" : "=r"(usp));
    __asm__ volatile("stmia %0, {sp}^" :: "r"(&usp2) : "memory");
    EQ(usp2, usp, "stm ^ stores the User sp");
#endif
}

/* ---- MMU and User mode -------------------------------------------------- */

static uint32_t l1[4096] __attribute__((aligned(16384)));
static uint32_t l2[256] __attribute__((aligned(1024)));

#define SECT(pa, ap, dom) (((pa) & 0xFFF00000u) | (((ap) & 3) << 10) | (((ap) >> 2) << 15) | ((dom) << 5) | 2u)
#define PAGE(pa, ap)      (((pa) & 0xFFFFF000u) | (((ap) & 3) << 4) | (((ap) >> 2) << 9) | 2u)

static void tlb_flush(void) { dsb(); mcr(0, 8, 7, 0, 0); mcr(0, 7, 5, 0, 0); dsb(); isb(); }

static uint32_t ld(uint32_t a) { return *(volatile uint32_t *)(uintptr_t)a; }
static void st(uint32_t a, uint32_t v) { *(volatile uint32_t *)(uintptr_t)a = v; }
/* An ARM instruction written as data: code is little-endian in either byte order (BE8). */
static void st_insn(uint32_t a, uint32_t insn) { st(a, IS_BE ? __builtin_bswap32(insn) : insn); }

static void expect_dabt(uint32_t fs, uint32_t far, const char *what)
{
    EQ(last_kind, 4, what);
    EQ(last_fsr & 0x40F, fs, what);
    EQ(last_far, far, what);
}

static void t_mmu(void)
{
    for (unsigned i = 0; i < 4096; i++)
        l1[i] = SECT(i << 20, 3, 0);                  /* identity, full access */
    l1[0x800] = SECT(0x00800000u, 3, 0);              /* VA 0x80000000 -> PA 0x00800000 */
    l1[0x801] = (uint32_t)l2 | 1u;                    /* VA 0x80100000: pages */
    l1[0x802] = SECT(0x00800000u, 3, 1);              /* domain 1: no access */
    l1[0x803] = 0;                                    /* translation fault */
    l1[0x804] = SECT(0x00A00000u, 3, 0);              /* remapped code */
    for (unsigned i = 0; i < 256; i++)
        l2[i] = 0;
    l2[0] = PAGE(0x00900000u, 2);                     /* priv RW, user RO */
    l2[1] = PAGE(0x00901000u, 5);                     /* priv RO */
    l2[3] = PAGE(0x00903000u, 3) | 1u;                /* XN */
    l2[4] = PAGE(0x00904000u, 1);                     /* priv only */

    st(0x00800010u, 0xA11A5);
    st(0x00900000u, 0x1234);
    st(0x00901000u, 0x5678);
    /* code: mov r0, #42 / bx lr at PA 0x00A00000; mov r0, #43 / bx lr at 0x00B00000 */
    st_insn(0x00A00000u, 0xE3A0002Au); st_insn(0x00A00004u, 0xE12FFF1Eu);
    st_insn(0x00B00000u, 0xE3A0002Bu); st_insn(0x00B00004u, 0xE12FFF1Eu);
    st_insn(0x00903000u, 0xE3A0002Cu); st_insn(0x00903004u, 0xE12FFF1Eu);

    mcr(0, 2, 0, 0, (uint32_t)l1);                    /* TTBR0 */
    mcr(0, 2, 0, 2, 0);                               /* TTBCR: N = 0 */
    mcr(0, 3, 0, 0, 0x55555551u);                     /* DACR: domain 1 none, others client */
    tlb_flush();
    mcr(0, 1, 0, 0, mrc(0, 1, 0, 0) | 1u);            /* SCTLR.M */
    isb();
    EQ(mrc(0, 1, 0, 0) & 1, 1, "MMU on");

    EQ(ld(0x80000010u), 0xA11A5, "section alias");
    EQ(ld(0x80100000u), 0x1234, "small page");
    mcr(0, 7, 8, 0, 0x80100010u);                     /* ATS1CPR */
    isb();
    EQ(mrc(0, 7, 4, 0), 0x00900000u, "ATS1CPR -> PAR");

    trap_n = 0;
    EQ(ld(0x80101000u), 0x5678, "read-only page reads");
    st(0x80101004u, 1);
    expect_dabt(0xF, 0x80101004u, "write to a read-only page: permission fault");
    EQ(last_fsr & 0x800, 0x800, "permission fault WnR");
    (void)ld(0x80102000u);
    expect_dabt(0x7, 0x80102000u, "unmapped page: translation fault");
    (void)ld(0x80200000u);
    expect_dabt(0x9, 0x80200000u, "no-access domain: domain fault");
    EQ((last_fsr >> 4) & 0xF, 1, "domain fault: DFSR.Domain");
    (void)ld(0x80300008u);
    expect_dabt(0x5, 0x80300008u, "unmapped section: translation fault");
    EQ(trap_n, 4, "MMU fault count");

    EQ(call_guarded(0x80400000u), 42, "code through a section");
    l1[0x804] = SECT(0x00B00000u, 3, 0);
    tlb_flush();
    EQ(call_guarded(0x80400000u), 43, "remapped code after TLBIALL");
    trap_n = 0;
    EQ(call_guarded(0x80103000u), 0xFFFFFFFFu, "XN page does not execute");
    EQ(last_kind, 3, "XN: prefetch abort");
    EQ(last_fsr & 0x40F, 0xF, "XN: IFSR permission fault");
    EQ(last_far, 0x80103000u, "XN: IFAR");
}

static volatile uint32_t user_saw;

static void __attribute__((noinline)) user_fn(uint32_t a)
{
    user_saw = ld(a);       /* allowed */
    st(a, 0);               /* faults: user read-only */
    for (;;) {}
}

static void __attribute__((noinline)) user_fn2(uint32_t a)
{
    user_saw = ld(a);       /* faults: privileged only */
    for (;;) {}
}

static void __attribute__((noinline)) user_svc(uint32_t a)
{
    user_saw = a + cpsr();
    __asm__ volatile("svc #1");
    for (;;) {}
}

static void t_user(void)
{
    st(0x80104000u, 0x99);
    user_saw = 0;
    user_test = 1;
    enter_user((uint32_t)user_fn, 0x80100000u);
    EQ(user_saw, 0x1234, "user read of a user read-only page");
    expect_dabt(0xF, 0x80100000u, "user write: permission fault");
    EQ(last_spsr & 0x1F, 0x10, "faulted in User mode");

    user_saw = 0;
    user_test = 1;
    enter_user((uint32_t)user_fn2, 0x80104000u);
    EQ(user_saw, 0, "privileged page not readable");
    expect_dabt(0xF, 0x80104000u, "user read of a privileged page");

    user_test = 1;
    enter_user((uint32_t)user_svc, 100);
    EQ(last_kind, 2, "svc from User mode");
    EQ((user_saw - 100) & 0x1F, 0x10, "user mode cpsr");
    EQ(cpsr() & 0x1F, 0x13, "back in SVC");
    EQ(ld(0x80104000u), 0x99, "privileged read after return");
}

/* ---- byte order --------------------------------------------------------- */

static void t_endian(void)
{
    EQ((cpsr() >> 9) & 1, IS_BE, "CPSR.E at reset");
    EQ((mrc(0, 1, 0, 0) >> 25) & 1, IS_BE, "SCTLR.EE at reset");
    static volatile uint32_t w = 0x11223344u;
    static volatile uint16_t h = 0x5566u;
    uint32_t r, rh, e;
    /* the other byte order for a moment: the same bytes load swapped */
    __asm__ volatile("setend " OTHER_END "\n\tldr %0, [%3]\n\tldrh %1, [%4]\n\tmrs %2, cpsr\n\t"
                     "setend " NATIVE_END : "=&r"(r), "=&r"(rh), "=&r"(e)
                     : "r"(&w), "r"(&h) : "memory");
    EQ(r, 0x44332211u, "setend: word");
    EQ(rh, 0x6655u, "setend: halfword");
    EQ((e >> 9) & 1, !IS_BE, "setend: CPSR.E");
    EQ(w, 0x11223344u, "back to the native order");

    /* an exception taken in the other byte order: entered with E = SCTLR.EE, and the
     * order comes back from SPSR */
    trap_n = 0;
    __asm__ volatile("setend " OTHER_END "\n\tsvc #7\n\tmrs %0, cpsr\n\tsetend " NATIVE_END
                     : "=r"(e) :: "memory");
    EQ(trap_n, 1, "svc taken");
    EQ((last_handler_cpsr >> 9) & 1, IS_BE, "handler runs with E = SCTLR.EE");
    EQ((last_spsr >> 9) & 1, !IS_BE, "SPSR.E holds the interrupted order");
    EQ((e >> 9) & 1, !IS_BE, "exception return restores E");
    __asm__ volatile("str %1, [%2]\n\tldr %0, [%2]" : "=&r"(r) : "r"(0xCAFEF00Du + V0), "r"(&w)
                     : "memory");
    EQ(r, 0xCAFEF00Du, "native order after the round trip");
}

/* ---- CP15 --------------------------------------------------------------- */

static void t_cp15(void)
{
    EQ(mrc(0, 0, 0, 0), 0x000F0000u, "MIDR");
    EQ((mrc(0, 0, 2, 0) >> 24) & 0xF, 2, "ID_ISAR0.Divide");
    mcr(0, 13, 0, 2, 0xC0FFEE);
    EQ(mrc(0, 13, 0, 2), 0xC0FFEE, "TPIDRURW");
    extern uint32_t vectors[];
    EQ(mrc(0, 12, 0, 0), (uint32_t)vectors, "VBAR");
    EQ(mrc(0, 1, 0, 2) & 0x00F00000u, 0, "CPACR: no cp10/cp11");
}

int main(void);
int main(void)
{
    puts_("UAM self-test (armv7, arm-ref, " BUILD ")\n");
    run("alu", t_alu);
    run("shifts", t_shifts);
    run("cond", t_cond);
    run("muldiv", t_muldiv);
    run("saturate", t_saturate);
    run("media", t_media);
    run("loadstore", t_loadstore);
    run("exclusive", t_exclusive);
    run("interwork", t_interwork);
    run("exceptions", t_exceptions);
    run("timer-irq", t_timer_irq);
    run("sgi", t_sgi);
    run("modes", t_modes);
    run("mmu", t_mmu);
    run("user-mode", t_user);
    run("cp15", t_cp15);
    run("endian", t_endian);

    puts_("result: ");
    puthex(n_pass);
    puts_(" passed, ");
    puthex(n_fail);
    puts_(" failed\n");
    puts_(n_fail ? "SELFTEST FAIL\n" : "SELFTEST PASS\n");
    return n_fail ? (int)((n_fail << 16) | 0x3333) : 0x5555;
}
