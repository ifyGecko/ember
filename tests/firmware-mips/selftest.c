/*
 * selftest.c - Bare-metal MIPS32 self-test for the UAM mips-ref / mipsel-ref boards.
 *
 * Exercises the emulator end to end: UART output, the integer ISA (ALU, shifts,
 * mul/div with HI/LO, MADD/MSUB, CLZ/CLO, EXT/INS/SEB/SEH/WSBH, ROTR), branch
 * delay slots including branch-likely, unaligned LWL/LWR/SWL/SWR, LL/SC,
 * synchronous exceptions with precise EPC/ExcCode/BadVAddr, the CP0
 * Count/Compare timer interrupt, software interrupts, the WAIT wake path, the
 * TLB (refill/invalid/modified exceptions, TLBP/TLBR, remapping executable
 * pages), exceptions in branch delay slots (Cause.BD, EPC = branch), user-mode
 * address errors, Cause.CE, and CP0 identification registers.
 *
 * Output is endian-independent and deterministic (the same console text on
 * both byte orders). The result is reported through the board's test
 * finisher.
 */
#include <stdint.h>

#define UART0 0xbfd003f8u
#define UART_THR (*(volatile uint8_t *)(UART0 + 0))
#define UART_LSR (*(volatile uint8_t *)(UART0 + 5))

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
#define RR(op, a, b) ({ uint32_t r_; __asm__ volatile(op " %0,%1,%2" : "=r"(r_) : "r"(a), "r"(b)); r_; })

/* ---- CP0 helpers ------------------------------------------------------- */
#define mfc0(reg, sel) ({ uint32_t v_; __asm__ volatile("mfc0 %0, $" #reg ", " #sel : "=r"(v_)); v_; })
#define mtc0(reg, sel, v) __asm__ volatile("mtc0 %0, $" #reg ", " #sel :: "r"((uint32_t)(v)))
#define C0_STATUS 12
#define C0_CAUSE  13
#define ST_IE  (1u << 0)

/* ---- trap bookkeeping -------------------------------------------------- */
typedef struct { uint32_t x[34]; } frame;
static volatile uint32_t trap_n, last_code, last_epc, last_bad, last_cause, last_refill;
static volatile uint32_t first_code, first_epc, first_bad;  /* first trap after trap_n = 0 */
static volatile uint32_t timer_ticks, soft_irqs;
extern volatile uint32_t g_refill;          /* set by the refill vector (start.S) */

/* Delay-slot test: on a fault, fix register $9 (t1) and restart at EPC. */
static volatile uint32_t bd_fix_addr;
/* Slot-fetch test: on a fault, map the delay slot's page and restart at EPC. */
static volatile int slot_test;
static void slot_map(void);
/* User-mode test: return to the kernel on syscall or a user fetch fault. */
static volatile int user_test;
void user_resume(void);
void enter_user(uint32_t pc, uint32_t arg);

uint32_t trap_handler(frame *f);
uint32_t trap_handler(frame *f)
{
    uint32_t cause = mfc0(13, 0), epc = mfc0(14, 0);
    uint32_t code = (cause >> 2) & 0x1F;

    if (code != 0) {
        /* synchronous exception (ExcCode 0 == interrupt) */
        trap_n++;
        last_code = code;
        last_epc = epc;
        last_bad = mfc0(8, 0);
        last_cause = cause;
        last_refill = g_refill;
        g_refill = 0;
        if (trap_n == 1) {
            first_code = code;
            first_epc = epc;
            first_bad = last_bad;
        }
        if (slot_test) {
            slot_test = 0;
            slot_map();
            return epc;
        }
        if (bd_fix_addr) {
            f->x[9] = bd_fix_addr;  /* make the faulting load valid ... */
            bd_fix_addr = 0;
            return epc;             /* ... and re-execute from EPC (the branch) */
        }
        if (user_test && (code == 8 || (code == 4 && epc == last_bad))) {
            mtc0(12, 0, mfc0(12, 0) & ~0x18u);  /* back to kernel mode */
            return (uint32_t)(uintptr_t)user_resume;
        }
        return epc + 4; /* skip the faulting instruction */
    }

    /* interrupt: dispatch on Cause.IP */
    uint32_t ip = (cause >> 8) & 0xFF;
    if (ip & (1u << 7)) {          /* CP0 timer (IP7) */
        timer_ticks++;
        mtc0(11, 0, mfc0(9, 0) + 100000000u); /* push Compare far out to disarm */
    }
    if (ip & (1u << 0)) {          /* software interrupt 0 */
        soft_irqs++;
        uint32_t c = mfc0(13, 0);
        mtc0(13, 0, c & ~(1u << 8)); /* clear IP0 */
    }
    return epc;
}

/* ---- tests ------------------------------------------------------------- */

static void t_alu(void)
{
    EQ(V7 + V3, 10, "addu");
    EQ(V3 - V7, (uint32_t)-4, "subu");
    EQ(V7 & V3, 3, "and");
    EQ(V7 | 8, 15, "or");
    EQ(V7 ^ V3, 4, "xor");
    EQ(~(V7 | V3), 0xFFFFFFF8u, "nor");
    EQ(RR("sllv", V1, V7) , 128, "sllv");
    EQ(RR("srlv", VMIN, V3), 0x10000000, "srlv");
    EQ((uint32_t)RR("srav", VMIN, V3), 0xF0000000u, "srav");
    EQ(V1 << 31, VMIN, "sll const");
    EQ((uint32_t)((int32_t)VMIN >> 4), 0xF8000000u, "sra const");
    EQ(RR("slt", VM1, V0), 1, "slt");
    EQ(RR("sltu", VM1, V0), 0, "sltu");
    uint32_t lui;
    __asm__ volatile("lui %0, 0xABCD" : "=r"(lui));
    EQ(lui, 0xABCD0000u, "lui");
    uint32_t rot;
    __asm__ volatile("rotr %0, %1, 3" : "=r"(rot) : "r"(V1));     /* immediate ROTR */
    EQ(rot, 0x20000000u, "rotr");
    __asm__ volatile("rotrv %0, %1, %2" : "=r"(rot) : "r"(V1), "r"(V3)); /* variable ROTRV */
    EQ(rot, 0x20000000u, "rotrv");
}

static void t_branch(void)
{
    int taken = 0;
    for (volatile int i = 0; i < 10; i++)
        taken += (i & 1);
    EQ(taken, 5, "loop");
    /* delay-slot side effect: the addiu in the delay slot always runs */
    uint32_t r;
    __asm__ volatile(
        ".set noreorder\n"
        "li   %0, 0\n"
        "beq  $zero, $zero, 1f\n"
        "addiu %0, %0, 7\n"   /* delay slot: always executed */
        "addiu %0, %0, 100\n" /* skipped (branch target is past it) */
        "1:\n"
        ".set reorder\n"
        : "=r"(r) :: "memory");
    EQ(r, 7, "delay slot executes");
    EQ((int32_t)VM1 < (int32_t)V0, 1, "bltz");
    EQ(V0 < VM1, 1, "bne path");
}

static void t_muldiv(void)
{
    uint32_t hi, lo;
    __asm__ volatile("mult %2,%3\n mfhi %0\n mflo %1" : "=r"(hi), "=r"(lo) : "r"(VM1), "r"(VM1));
    EQ(hi, 0, "mult hi (-1*-1)");
    EQ(lo, 1, "mult lo (-1*-1)");
    __asm__ volatile("multu %2,%3\n mfhi %0\n mflo %1" : "=r"(hi), "=r"(lo) : "r"(VM1), "r"(VM1));
    EQ(hi, 0xFFFFFFFEu, "multu hi");
    EQ(lo, 1, "multu lo");
    __asm__ volatile("mult %2,%3\n mfhi %0\n mflo %1" : "=r"(hi), "=r"(lo) : "r"(VMIN), "r"(V3));
    EQ(hi, 0xFFFFFFFEu, "mult neg hi");
    EQ(lo, VMIN, "mult neg lo");
    EQ(RR("mul", V7, V3), 21, "mul");
    EQ(RR("div", (uint32_t)-7, 2u), (uint32_t)-3, "div q");
    EQ(RR("divu", V7, V3), 2, "divu q");
    /* madd: {hi,lo} += a*b */
    __asm__ volatile("mult %2,%3\n madd %4,%5\n mfhi %0\n mflo %1"
                     : "=r"(hi), "=r"(lo) : "r"(V7), "r"(V3), "r"(V7), "r"(V3));
    EQ(lo, 42, "madd lo");
    EQ(hi, 0, "madd hi");
}

static void t_bitops(void)
{
    uint32_t r;
    __asm__ volatile("clz %0, %1" : "=r"(r) : "r"(V1)); EQ(r, 31, "clz(1)");
    __asm__ volatile("clz %0, %1" : "=r"(r) : "r"(V0)); EQ(r, 32, "clz(0)");
    __asm__ volatile("clo %0, %1" : "=r"(r) : "r"(VM1)); EQ(r, 32, "clo(-1)");
    __asm__ volatile("ext %0, %1, 4, 8" : "=r"(r) : "r"(0x12345678u)); EQ(r, 0x67, "ext");
    r = 0x0000FFFFu;
    __asm__ volatile("ins %0, %1, 8, 8" : "+r"(r) : "r"(0xAAu)); EQ(r, 0x0000AAFFu, "ins");
    __asm__ volatile("seb %0, %1" : "=r"(r) : "r"(0x80u)); EQ(r, 0xFFFFFF80u, "seb");
    __asm__ volatile("seh %0, %1" : "=r"(r) : "r"(0x8000u)); EQ(r, 0xFFFF8000u, "seh");
    __asm__ volatile("wsbh %0, %1" : "=r"(r) : "r"(0x11223344u)); EQ(r, 0x22114433u, "wsbh");
}

/* Reference little/big-endian word assembly from bytes, chosen at runtime. */
static int be;
static uint32_t assemble(const uint8_t *p)
{
    return be ? ((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | p[2] << 8 | p[3])
              : ((uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | p[1] << 8 | p[0]);
}

#ifdef __MIPSEB__
#define ULW(r, p) __asm__ volatile("lwl %0, 0(%1)\n lwr %0, 3(%1)" : "=&r"(r) : "r"(p) : "memory")
#define USW(v, p) __asm__ volatile("swl %0, 0(%1)\n swr %0, 3(%1)" :: "r"(v), "r"(p) : "memory")
#else
#define ULW(r, p) __asm__ volatile("lwr %0, 0(%1)\n lwl %0, 3(%1)" : "=&r"(r) : "r"(p) : "memory")
#define USW(v, p) __asm__ volatile("swr %0, 0(%1)\n swl %0, 3(%1)" :: "r"(v), "r"(p) : "memory")
#endif

static void t_unaligned(void)
{
    static volatile uint8_t buf[16];
    for (int i = 0; i < 16; i++)
        buf[i] = (uint8_t)(0x10 + i);
    for (int off = 0; off < 4; off++) {
        uint32_t r;
        ULW(r, &buf[1 + off]);
        EQ(r, assemble((const uint8_t *)&buf[1 + off]), "lwl/lwr unaligned load");
    }
    for (int off = 0; off < 4; off++) {
        volatile uint8_t dst[16];
        for (int i = 0; i < 16; i++)
            dst[i] = 0;
        uint32_t w = 0xDEADBEEFu;
        USW(w, &dst[1 + off]);
        EQ(assemble((const uint8_t *)&dst[1 + off]), w, "swl/swr unaligned store");
    }
}

static void t_llsc(void)
{
    static volatile uint32_t word;
    uint32_t rc, val;
    word = 41;
    __asm__ volatile("ll %0, 0(%2)\n addiu %0, %0, 1\n move %1, %0\n sc %1, 0(%2)"
                     : "=&r"(val), "=&r"(rc) : "r"(&word) : "memory");
    EQ(rc, 1, "sc success");
    EQ(word, 42, "sc stored");
}

static void t_loadstore(void)
{
    static volatile uint8_t buf[8] __attribute__((aligned(4)));
    *(volatile uint32_t *)buf = 0;
    *(volatile uint32_t *)(buf + 4) = 0;
    buf[0] = 0x80;
    buf[1] = 0x7F;
    EQ((uint32_t)(int32_t)*(volatile int8_t *)&buf[0], 0xFFFFFF80u, "lb sign");
    EQ((uint32_t)*(volatile uint8_t *)&buf[0], 0x80, "lbu");
    *(volatile int16_t *)(buf + 2) = (int16_t)0x8001;
    EQ((uint32_t)(int32_t)*(volatile int16_t *)(buf + 2), 0xFFFF8001u, "lh sign");
    EQ((uint32_t)*(volatile uint16_t *)(buf + 2), 0x8001, "lhu");
}

/* Each inline op faults; the handler records cause and resumes after it. */
static void expect(uint32_t code, const char *what)
{
    EQ(trap_n, 1, what);
    EQ(last_code, code, what);
    trap_n = 0;
}

static void t_exceptions(void)
{
    uint32_t pc, sink = 0;
    trap_n = 0;

    __asm__ volatile("la %0, 1f\n 1: syscall" : "=r"(pc) :: "memory");
    EQ(last_epc, pc, "syscall epc");
    expect(8, "syscall");

    __asm__ volatile("break" ::: "memory");
    expect(9, "break");

    /* signed overflow: 0x7fffffff + 1 */
    __asm__ volatile("add %0, %1, %2" : "=r"(sink) : "r"(0x7FFFFFFFu), "r"(1u)); (void)sink;
    expect(12, "add overflow");

    __asm__ volatile("teq %0, %1" :: "r"(V7), "r"(V7)); /* equal -> trap */
    expect(13, "teq trap");

    /* unaligned load -> address error (AdEL), BadVAddr = the address */
    static volatile uint8_t ab[8] __attribute__((aligned(4)));
    uint32_t addr = (uint32_t)(uintptr_t)&ab[1];
    __asm__ volatile("lw %0, 0(%1)" : "=r"(sink) : "r"(addr) : "memory");
    EQ(last_bad, addr, "AdEL badvaddr");
    expect(4, "unaligned load");

    __asm__ volatile("sw %0, 0(%1)" :: "r"(V7), "r"(addr) : "memory");
    expect(5, "unaligned store");
}

static void t_timer_irq(void)
{
    timer_ticks = 0;
    for (unsigned k = 1; k <= 3; k++) {
        mtc0(11, 0, mfc0(9, 0) + 5000);     /* Compare = Count + 5000 */
        mtc0(12, 0, (1u << 15) | ST_IE); /* IM7 + IE */
        __asm__ volatile("wait");
        while (timer_ticks < k) {}
    }
    mtc0(12, 0, 0);
    EQ(timer_ticks, 3, "three timer interrupts");
}

static void t_soft_irq(void)
{
    soft_irqs = 0;
    mtc0(12, 0, (1u << 8) | ST_IE);  /* IM0 + IE */
    uint32_t c = mfc0(13, 0);
    mtc0(13, 0, c | (1u << 8));       /* raise software interrupt 0 */
    for (volatile int i = 0; i < 1000 && !soft_irqs; i++) {}
    mtc0(12, 0, 0);
    EQ(soft_irqs, 1, "software interrupt");
}


/* ---- TLB --------------------------------------------------------------- */

#define EHB() __asm__ volatile("ehb" ::: "memory")
#define LO_V 0x2u
#define LO_D 0x4u
#define LO_G 0x1u
#define LO_C (2u << 3)  /* uncached */
#define PFN(pa) (((uint32_t)(pa) >> 12) << 6)

static void tlb_set(unsigned idx, uint32_t hi, uint32_t lo0, uint32_t lo1)
{
    mtc0(0, 0, idx);
    mtc0(10, 0, hi);
    mtc0(2, 0, lo0);
    mtc0(3, 0, lo1);
    mtc0(5, 0, 0);
    EHB();
    __asm__ volatile("tlbwi" ::: "memory");
    EHB();
}

/* Park every entry on a distinct kseg0 VPN2, which is never translated. */
static void tlb_clear(void)
{
    for (unsigned i = 0; i < 16; i++)
        tlb_set(i, 0x80000000u + i * 0x2000u, 0, 0);
    mtc0(10, 0, 0);
}

static uint32_t lw_at(uint32_t addr)
{
    uint32_t v;
    __asm__ volatile("lw %0, 0(%1)" : "=r"(v) : "r"(addr) : "memory");
    return v;
}

static void sw_at(uint32_t addr, uint32_t v)
{
    __asm__ volatile("sw %0, 0(%1)" :: "r"(v), "r"(addr) : "memory");
}

#define PAGE_PA 0x00200000u              /* physical page used through the TLB */
#define PAGE_K1 (0xA0000000u | PAGE_PA)  /* the same page via kseg1 */

static void t_tlb(void)
{
    tlb_clear();
    trap_n = 0;
    g_refill = 0;

    /* Empty TLB: a kuseg load misses -> TLBL through the refill vector. */
    (void)lw_at(0x3008);
    EQ(last_refill, 1, "refill vector");
    EQ(last_bad, 0x3008, "refill badvaddr");
    EQ(mfc0(4, 0) & 0x007FFFF0u, (0x3008u >> 9) & 0x007FFFF0u, "context badvpn2");
    EQ(mfc0(10, 0) & 0xFFFFE000u, 0x2000, "entryhi vpn2");
    expect(2, "tlb refill (load)");

    sw_at(0x5000, 1);
    EQ(last_refill, 1, "store refill vector");
    expect(3, "tlb refill (store)");

    /* Map 0x3000 (odd page of VPN2 0x2000) to PAGE_PA; the even page is invalid. */
    sw_at(PAGE_K1 + 8, 0x12345678u);
    tlb_set(5, 0x2000, LO_G, PFN(PAGE_PA) | LO_C | LO_D | LO_V | LO_G);
    EQ(lw_at(0x3008), 0x12345678u, "mapped load");
    sw_at(0x3010, 0xCAFEF00Du);
    EQ(lw_at(PAGE_K1 + 0x10), 0xCAFEF00Du, "mapped store");
    EQ(trap_n, 0, "no faults while mapped");

    /* Matching entry with V=0: TLB invalid, general vector. */
    (void)lw_at(0x2008);
    EQ(last_refill, 0, "invalid uses general vector");
    EQ(last_bad, 0x2008, "invalid badvaddr");
    expect(2, "tlb invalid");

    /* Clean (D=0) page: store raises TLB Modified, load still works. */
    tlb_set(5, 0x2000, LO_G, PFN(PAGE_PA) | LO_C | LO_V | LO_G);
    EQ(lw_at(0x3008), 0x12345678u, "clean page load");
    sw_at(0x3008, 0);
    EQ(last_bad, 0x3008, "mod badvaddr");
    expect(1, "tlb modified");

    /* TLBP / TLBR */
    mtc0(10, 0, 0x2000);
    EHB();
    __asm__ volatile("tlbp" ::: "memory");
    EHB();
    EQ(mfc0(0, 0), 5, "tlbp hit");
    mtc0(10, 0, 0x4000);
    EHB();
    __asm__ volatile("tlbp" ::: "memory");
    EHB();
    EQ(mfc0(0, 0) >> 31, 1, "tlbp miss");
    mtc0(0, 0, 5);
    EHB();
    __asm__ volatile("tlbr" ::: "memory");
    EHB();
    EQ(mfc0(10, 0), 0x2000, "tlbr entryhi");
    EQ(mfc0(3, 0), PFN(PAGE_PA) | LO_C | LO_V | LO_G, "tlbr entrylo1");
    EQ(mfc0(2, 0), LO_G, "tlbr entrylo0 (g)");

    tlb_clear();
}

/* Remapping an executable page must run the new code (no stale translation). */
static void t_tlb_remap(void)
{
    const uint32_t pa_a = 0x00210000u, pa_b = 0x00211000u;
    /* jr ra ; addiu v0, zero, N */
    sw_at(0xA0000000u | pa_a, 0x03E00008u);
    sw_at((0xA0000000u | pa_a) + 4, 0x24020000u | 1);
    sw_at(0xA0000000u | pa_b, 0x03E00008u);
    sw_at((0xA0000000u | pa_b) + 4, 0x24020000u | 2);
    uint32_t (*fn)(void) = (uint32_t (*)(void))0x00600000u;

    tlb_clear();
    tlb_set(3, 0x00600000u, PFN(pa_a) | LO_C | LO_V | LO_G, 0);
    EQ(fn(), 1, "code via first mapping");
    tlb_set(3, 0x00600000u, PFN(pa_b) | LO_C | LO_V | LO_G, 0);
    EQ(fn(), 2, "code via new mapping");
    tlb_clear();
}

/* A TLB miss in a branch delay slot: EPC = branch, Cause.BD = 1, and after
 * the handler fixes the address the branch is re-executed correctly. */
static volatile uint32_t bd_word = 0x5A5A5A5Au;

static void t_delay_slot(void)
{
    uint32_t r, v, bpc;
    tlb_clear();
    trap_n = 0;

    bd_fix_addr = (uint32_t)(uintptr_t)&bd_word;
    __asm__ volatile(
        ".set noreorder\n"
        "li    %0, 0\n"
        "li    $9, 0x5000\n"       /* unmapped kuseg */
        "la    %2, 2f\n"
        "2: beq $zero, $zero, 1f\n"
        "lw    $8, 0($9)\n"        /* delay slot: TLB refill */
        "addiu %0, %0, 100\n"     /* skipped: branch taken */
        "1: move %1, $8\n"
        ".set reorder\n"
        : "=&r"(r), "=&r"(v), "=&r"(bpc) :: "$8", "$9", "memory");
    EQ(r, 0, "taken branch after delay-slot fault");
    EQ(v, 0x5A5A5A5Au, "delay-slot load after fix");
    EQ(last_cause >> 31, 1, "cause.bd (taken)");
    EQ(last_epc, bpc, "epc = branch (taken)");
    expect(2, "delay-slot refill (taken)");

    bd_fix_addr = (uint32_t)(uintptr_t)&bd_word;
    __asm__ volatile(
        ".set noreorder\n"
        "li    %0, 0\n"
        "li    $9, 0x5000\n"
        "la    %2, 2f\n"
        "li    $10, 0\n"           /* compare a register: a branch evaluated at run time */
        "2: bne $10, $zero, 1f\n"
        "lw    $8, 0($9)\n"        /* delay slot: TLB refill */
        "addiu %0, %0, 100\n"     /* executed: branch not taken */
        "1: move %1, $8\n"
        ".set reorder\n"
        : "=&r"(r), "=&r"(v), "=&r"(bpc) :: "$8", "$9", "$10", "memory");
    EQ(r, 100, "not-taken branch after delay-slot fault");
    EQ(v, 0x5A5A5A5Au, "delay-slot load after fix (not taken)");
    EQ(last_cause >> 31, 1, "cause.bd (not taken)");
    EQ(last_epc, bpc, "epc = branch (not taken)");
    expect(2, "delay-slot refill (not taken)");

    /* A fault outside a delay slot clears BD. */
    (void)lw_at(0x5000);
    EQ(last_cause >> 31, 0, "cause.bd clear");
    expect(2, "plain refill");
    tlb_clear();
}

/* A branch on the last word of a page whose delay slot lies on an invalid
 * page: the slot's fetch fault is reported with BD = 1 and EPC = branch. */
#define SLOT_PA_A 0x00220000u   /* holds "jr ra" in its last word */
#define SLOT_PA_B 0x00221000u   /* holds "addiu v0, zero, 7" at offset 0 */

static void slot_map(void)
{
    tlb_set(2, 0x00700000u, PFN(SLOT_PA_A) | LO_C | LO_V | LO_G, PFN(SLOT_PA_B) | LO_C | LO_V | LO_G);
}

static void t_slot_fetch(void)
{
    sw_at(0xA0000000u | (SLOT_PA_A + 0xFFC), 0x03E00008u);   /* jr ra */
    sw_at(0xA0000000u | SLOT_PA_B, 0x24020007u);             /* addiu v0, zero, 7 */
    uint32_t (*fn)(void) = (uint32_t (*)(void))0x00700FFCu;

    tlb_clear();
    tlb_set(2, 0x00700000u, PFN(SLOT_PA_A) | LO_C | LO_V | LO_G, LO_G); /* odd page invalid */
    trap_n = 0;
    slot_test = 1;
    EQ(fn(), 7, "branch completes after slot fault");
    EQ(last_cause >> 31, 1, "slot fetch: cause.bd");
    EQ(last_epc, 0x00700FFCu, "slot fetch: epc = branch");
    EQ(last_bad, 0x00701000u, "slot fetch: badvaddr = slot");
    expect(2, "slot fetch TLBL");
    tlb_clear();
}

/* User mode: kseg0 is off limits for data (AdEL) and for instruction fetch. */
extern char user_stub[];

static void t_user(void)
{
    uint32_t stub_pa = (uint32_t)(uintptr_t)user_stub & 0x1FFFFFFFu;
    uint32_t kaddr = (uint32_t)(uintptr_t)&bd_word;
    tlb_clear();
    tlb_set(1, 0x00400000u, PFN(stub_pa) | LO_C | LO_V | LO_G, 0);

    trap_n = 0;
    user_test = 1;
    enter_user(0x00400000u, kaddr);   /* lw from kseg0 -> AdEL, then syscall */
    user_test = 0;
    EQ(trap_n, 2, "user: two exceptions");
    EQ(first_code, 4, "user: kseg0 load is AdEL");
    EQ(first_bad, kaddr, "user AdEL badvaddr");
    EQ(first_epc, 0x00400000u, "user AdEL epc");
    EQ(last_code, 8, "user: ends with syscall");
    trap_n = 0;

    user_test = 1;
    enter_user(0x00400000u, 0x00400000u); /* user load from its own page: fine */
    user_test = 0;
    EQ(trap_n, 1, "user: mapped load is allowed");
    trap_n = 0;

    user_test = 1;
    enter_user((uint32_t)(uintptr_t)user_stub, 0); /* fetch from kseg0 in user mode */
    user_test = 0;
    EQ(last_bad, (uint32_t)(uintptr_t)user_stub, "user fetch badvaddr");
    EQ(last_epc, (uint32_t)(uintptr_t)user_stub, "user fetch epc");
    expect(4, "user fetch AdEL");
    tlb_clear();
}

/* ---- CP0 identity and coprocessor-unusable --------------------------------- */

static void t_cp0(void)
{
    uint32_t cfg = mfc0(16, 0), cfg1 = mfc0(16, 1);
    EQ((cfg >> 10) & 7, 1, "config.ar = release 2");
    EQ((cfg >> 7) & 7, 1, "config.mt = tlb");
    EQ((cfg1 >> 25) & 0x3F, 15, "config1.mmusize");
    EQ(mfc0(15, 0) & 0x00FFFF00u, 0x00019300u, "prid 24K");

    mtc0(12, 0, 0x10000000u);           /* CU0 */
    EHB();
    EQ(mfc0(12, 0), 0x10000000u, "status cu0 writable");
    mtc0(12, 0, 0);
    EHB();

    trap_n = 0;
    uint32_t sink;
    __asm__ volatile("cfc1 %0, $31" : "=r"(sink) :: "memory");
    (void)sink;
    EQ((last_cause >> 28) & 3, 1, "cause.ce = 1");
    expect(11, "coprocessor unusable");
}

int main(void);
int main(void)
{
    union { uint32_t w; uint8_t b[4]; } u = { .w = 1 };
    be = (u.b[0] == 0);

    puts_("UAM self-test (mips32, mips-ref)\n");
    run("alu", t_alu);
    run("branch", t_branch);
    run("muldiv", t_muldiv);
    run("bitops", t_bitops);
    run("unaligned", t_unaligned);
    run("llsc", t_llsc);
    run("loadstore", t_loadstore);
    run("exceptions", t_exceptions);
    run("timer-irq", t_timer_irq);
    run("soft-irq", t_soft_irq);
    run("tlb", t_tlb);
    run("tlb-remap", t_tlb_remap);
    run("delay-slot", t_delay_slot);
    run("slot-fetch", t_slot_fetch);
    run("user-mode", t_user);
    run("cp0", t_cp0);

    puts_("result: ");
    puthex(n_pass);
    puts_(" passed, ");
    puthex(n_fail);
    puts_(" failed\n");
    puts_(n_fail ? "SELFTEST FAIL\n" : "SELFTEST PASS\n");
    return n_fail ? (int)((n_fail << 16) | 0x3333) : 0x5555;
}
