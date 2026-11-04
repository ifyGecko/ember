/*
 * selftest.c - 8-bit AVR self-test for the avr-ref board, built for each core family
 * (-o model=e|eplus|xm|xt|rc; the Makefile names the part whose memory layout each
 * model has).
 *
 * Exercises: every flag the ALU instructions define, against vectors computed from the
 * manual's equations (gen_vectors.py); 8/16/32-bit arithmetic as the compiler emits
 * it; MUL/MULS/MULSU/FMUL/FMULS/FMULSU; loads and stores through X/Y/Z with displacement
 * and pre-decrement/post-increment; program memory (LPM, LPM Z+, ELPM); indirect and
 * recursive calls; the I/O bit instructions; skips over two-word instructions; the
 * core's registers seen through the data space (SREG, SP, and r0-r31 on AVRe/AVRe+);
 * the timer interrupt with SLEEP, and the vector's hardware acknowledge; XCH, LAS,
 * LAC, LAT and DES (AVRxm). Output goes to the UART; the result to the test finisher.
 */
#include <stdint.h>
#include <string.h>
#include <avr/interrupt.h>
#include <avr/io.h>
#include <avr/pgmspace.h>

#if defined(__AVR_TINY__)
#define VEC_ATTR                              /* program memory is in the data space */
#else
#define VEC_ATTR PROGMEM
#endif
#include "vectors.h"

#define DEV(o) (*(volatile uint8_t *)(DEV_BASE + (o)))
#define UART_THR DEV(0x00)
#define UART_LSR DEV(0x05)
#define FIN_PASS DEV(0x08)
#define FIN_FAIL DEV(0x09)
#define TIMER(o) DEV(0x10 + (o))

#if defined(__AVR_TINY__)
#define VEC_RD(i, f) (VECS[i].f)
#else
#define VEC_RD(i, f) pgm_read_byte(&VECS[i].f)
#endif

static void putc_(char c) { while (!(UART_LSR & 0x20)) {} UART_THR = (uint8_t)c; }
static void puts_(const char *s) { for (; *s; s++) { if (*s == '\n') putc_('\r'); putc_(*s); } }
static void puthex(uint32_t v, uint8_t digits)
{
    static const char d[] = "0123456789abcdef";
    for (int8_t i = (int8_t)(4 * (digits - 1)); i >= 0; i -= 4)
        putc_(d[(v >> i) & 0xF]);
}

static uint8_t n_pass, n_fail;
static const char *current;

static void check(uint8_t ok, const char *what, uint32_t got, uint32_t want)
{
    if (ok)
        return;
    n_fail++;
#if defined(__AVR_TINY__)   /* 4 KiB: the same facts, fewer words */
    puts_(current);
    putc_(' ');
    puts_(what);
    putc_(' ');
    puthex(got, 8);
    putc_('/');
#else
    puts_("  FAIL ");
    puts_(current);
    puts_(": ");
    puts_(what);
    puts_(" got ");
    puthex(got, 8);
    puts_(" want ");
#endif
    puthex(want, 8);
    puts_("\n");
}
#define EQ(got, want, what) do { uint32_t g_ = (got), w_ = (want); check(g_ == w_, what, g_, w_); } while (0)

static void run(const char *name, void (*fn)(void))
{
    uint8_t before = n_fail;
    current = name;
    fn();
    puts_(n_fail == before ? "[ ok ] " : "[FAIL] ");
    puts_(name);
    puts_("\n");
    if (n_fail == before)
        n_pass++;
}

/* Values the compiler cannot fold. */
static volatile uint8_t  V8a = 0x9C, V8b = 0x37;
static volatile uint16_t V16a = 0xBEEF, V16b = 0x1234;
static volatile uint32_t V32a = 0xDEADBEEFu, V32b = 0x01234567u;
static volatile int32_t  S32a = -123456789;

/* ---- the ALU's flags --------------------------------------------------------------- */

static uint8_t do_op(uint8_t op, uint8_t a, uint8_t b, uint8_t sin, uint8_t *sout)
{
    uint8_t s;
#define BIN(insn) __asm__ volatile("out __SREG__, %3\n\t" insn " %0, %2\n\tin %1, __SREG__" \
                                   : "+d"(a), "=d"(s) : "d"(b), "d"(sin))
#define UN(insn)  __asm__ volatile("out __SREG__, %2\n\t" insn " %0\n\tin %1, __SREG__" \
                                   : "+d"(a), "=d"(s) : "d"(sin))
    switch (op) {
    case OP_ADD: BIN("add"); break;
    case OP_ADC: BIN("adc"); break;
    case OP_SUB: BIN("sub"); break;
    case OP_SBC: BIN("sbc"); break;
    case OP_AND: BIN("and"); break;
    case OP_OR:  BIN("or"); break;
    case OP_EOR: BIN("eor"); break;
    case OP_CP:  BIN("cp"); break;
    case OP_CPC: BIN("cpc"); break;
    case OP_COM: UN("com"); break;
    case OP_NEG: UN("neg"); break;
    case OP_INC: UN("inc"); break;
    case OP_DEC: UN("dec"); break;
    case OP_LSR: UN("lsr"); break;
    case OP_ROR: UN("ror"); break;
    default:     UN("asr"); break;
    }
#undef BIN
#undef UN
    *sout = s & 0x3F;
    return a;
}

static void t_flags(void)
{
    for (uint16_t i = 0; i < sizeof(VECS) / sizeof(VECS[0]); i++) {
        uint8_t op = VEC_RD(i, op), sout;
        uint8_t r = do_op(op, VEC_RD(i, a), VEC_RD(i, b), VEC_RD(i, sin), &sout);
        if (r != VEC_RD(i, res) || sout != VEC_RD(i, sout)) {
            EQ((uint32_t)i << 16 | (uint32_t)op << 8 | VEC_RD(i, a), 0, "vector (index:op:a)");
            EQ((uint32_t)r << 8 | sout, (uint32_t)VEC_RD(i, res) << 8 | VEC_RD(i, sout), "result:sreg");
            return;
        }
    }
}

/* ---- arithmetic as compiled ------------------------------------------------------ */

static void t_arith(void)
{
    EQ((uint8_t)(V8a + V8b), (uint8_t)(0x9C + 0x37), "add8");
    EQ((uint8_t)(V8a - V8b), (uint8_t)(0x9C - 0x37), "sub8");
    EQ((uint16_t)(V16a + V16b), (uint16_t)(0xBEEF + 0x1234), "add16");
    EQ((uint16_t)(V16a - V16b), (uint16_t)(0xBEEF - 0x1234), "sub16");
#if !defined(__AVR_TINY__)   /* 32-bit code and the libgcc routines do not fit 4 KiB */
    EQ(V32a + V32b, 0xDEADBEEFu + 0x01234567u, "add32");
    EQ(V32a - V32b, 0xDEADBEEFu - 0x01234567u, "sub32");
    EQ(V32a ^ V32b, 0xDEADBEEFu ^ 0x01234567u, "xor32");
    EQ(V32a >> 7, 0xDEADBEEFu >> 7, "shr32");
    EQ(V32b << 9, 0x01234567u << 9, "shl32");
    EQ((uint32_t)(S32a >> 5), (uint32_t)(-123456789 >> 5), "sar32");
    EQ(V16a / V8b, 0xBEEF / 0x37, "udiv16");
    EQ(V16a % V8b, 0xBEEF % 0x37, "umod16");
    EQ(V32a / V32b, 0xDEADBEEFu / 0x01234567u, "udiv32");
    EQ((uint32_t)(S32a / 1000), (uint32_t)(-123456789 / 1000), "sdiv32");
    EQ(V32a * V32b, 0xDEADBEEFu * 0x01234567u, "mul32");
    EQ((uint32_t)(V16a * V16b), (uint32_t)((uint32_t)0xBEEF * 0x1234) & 0xFFFF, "mul16");
#endif
    EQ(V8a > V8b, 1, "ucmp");
    EQ((int8_t)V8a > (int8_t)V8b, 0, "scmp");
}

#if !defined(__AVR_TINY__)
static void t_mul(void)
{
    uint16_t r;
    uint8_t a = V8a, b = V8b;   /* 0x9C, 0x37 */
#define MUL(insn) __asm__ volatile(insn " %1, %2\n\tmovw %0, r0\n\tclr r1" : "=r"(r) : "a"(a), "a"(b) : "r0", "r1")
    MUL("mul");    EQ(r, (uint16_t)(0x9C * 0x37), "mul");
    MUL("muls");   EQ(r, (uint16_t)((int8_t)0x9C * (int8_t)0x37), "muls");
    MUL("mulsu");  EQ(r, (uint16_t)((int8_t)0x9C * 0x37), "mulsu");
    MUL("fmul");   EQ(r, (uint16_t)((0x9C * 0x37) << 1), "fmul");
    MUL("fmuls");  EQ(r, (uint16_t)((uint16_t)((int8_t)0x9C * (int8_t)0x37) << 1), "fmuls");
    MUL("fmulsu"); EQ(r, (uint16_t)((uint16_t)((int8_t)0x9C * 0x37) << 1), "fmulsu");
    a = 0x80; b = 0x80;
    MUL("fmuls");  EQ(r, 0x8000, "fmuls -1*-1");
    uint8_t s;
    a = 0x80; b = 0x02;   /* 0x100: C = bit 15 (0), Z = 0 */
    __asm__ volatile("mul %1, %2\n\tin %0, __SREG__\n\tclr r1" : "=r"(s) : "a"(a), "a"(b) : "r0", "r1");
    EQ(s & 3, 0, "mul flags");
    a = 0; b = 0x55;
    __asm__ volatile("mul %1, %2\n\tin %0, __SREG__\n\tclr r1" : "=r"(s) : "a"(a), "a"(b) : "r0", "r1");
    EQ(s & 3, 2, "mul Z");
    a = 0xFF; b = 0xFF;   /* 0xFE01: C = 1 */
    __asm__ volatile("mul %1, %2\n\tin %0, __SREG__\n\tclr r1" : "=r"(s) : "a"(a), "a"(b) : "r0", "r1");
    EQ(s & 3, 1, "mul C");
#undef MUL
}
#endif

/* ---- memory ------------------------------------------------------------------------ */

typedef struct { uint8_t a; uint16_t b; uint32_t c; char s[5]; } rec;
static rec recs[3];
static uint8_t buf[24];

static void t_mem(void)
{
    for (uint8_t i = 0; i < sizeof(buf); i++)
        buf[i] = (uint8_t)(i * 7 + 3);
    uint16_t sum = 0;
    for (uint8_t *p = buf; p < buf + sizeof(buf); p++)
        sum += *p;
    EQ(sum, 24 * 3 + 7 * (23 * 24 / 2), "post-increment sum");
    uint8_t *q = buf + sizeof(buf);
    uint8_t x = 0;
    while (q > buf)
        x ^= *--q;
    uint8_t want = 0;
    for (uint8_t i = 0; i < sizeof(buf); i++)
        want ^= (uint8_t)(i * 7 + 3);
    EQ(x, want, "pre-decrement xor");
    rec r = { 1, 0x2233, 0x44556677u, "abcd" };
    recs[2] = r;
    recs[1] = recs[2];
    EQ(recs[1].c, 0x44556677u, "struct copy");
    EQ(recs[1].s[3], 'd', "struct copy tail");
#if !defined(__AVR_TINY__)
    memmove(buf + 1, buf, 10);
    EQ(buf[10], (uint8_t)(9 * 7 + 3), "memmove");
#endif
}

static const uint8_t table[16] PROGMEM = { 1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144, 233, 121, 98, 219, 61 };
#if !defined(__AVR_TINY__) && defined(FAR_TABLE)
static const uint8_t far_table[4] __attribute__((section(".fartable"), used)) = { 0xC0, 0xFF, 0xEE, 0x42 };
#endif

static void t_progmem(void)
{
    uint16_t s = 0;
#if defined(__AVR_TINY__)
    for (uint8_t i = 0; i < 16; i++)
        s += table[i];
#else
    for (uint8_t i = 0; i < 16; i++)
        s += pgm_read_byte(&table[i]);
    EQ(pgm_read_word(&table[2]), 3 | 5 << 8, "lpm word");
    /* LPM Rd, Z+ */
    uint8_t a, b;
    const uint8_t *z = table + 4;
    __asm__ volatile("lpm %0, Z+\n\tlpm %1, Z+" : "=r"(a), "=r"(b), "+z"(z));
    EQ(a << 8 | b, 8 << 8 | 13, "lpm Z+");
    EQ((uint16_t)(z - table), 6, "lpm Z+ pointer");
#endif
    EQ(s, 1107, "table sum");
#if !defined(__AVR_TINY__) && defined(FAR_TABLE)
    uint32_t fa = pgm_get_far_address(far_table);
    EQ(fa >> 16 != 0, 1, "far table is above 64 KiB");
    EQ(pgm_read_byte_far(fa + 3) << 8 | pgm_read_byte_far(fa), 0x42C0, "elpm");
#endif
}

/* ---- calls ------------------------------------------------------------------------- */

static uint16_t fib(uint8_t n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
static uint8_t f_add(uint8_t a, uint8_t b) { return a + b; }
static uint8_t f_sub(uint8_t a, uint8_t b) { return a - b; }
static uint8_t (*volatile fns[2])(uint8_t, uint8_t) = { f_add, f_sub };

static void t_calls(void)
{
    EQ(fib(V8b & 15), 13, "recursion");    /* V8b & 15 = 7 */
    EQ(fns[0](V8a, V8b), (uint8_t)(0x9C + 0x37), "indirect call");
#if !defined(__AVR_TINY__)
    EQ(fns[1](V8a, V8b), (uint8_t)(0x9C - 0x37), "indirect call 2");
#endif
}

/* ---- I/O bit instructions and skips -------------------------------------------- */

#define GPIO 0x1E   /* a general-purpose I/O register */

static void t_io(void)
{
    uint8_t v, a = 0, b = 0;
    __asm__ volatile("out %1, __zero_reg__\n\tsbi %1, 3\n\tsbi %1, 0\n\tcbi %1, 3\n\tin %0, %1"
                     : "=d"(v) : "I"(GPIO));
    EQ(v, 0x01, "sbi/cbi");
    __asm__ volatile("sbis %2, 0\n\tinc %0\n\tsbic %2, 0\n\tinc %1"
                     : "+d"(a), "+d"(b) : "I"(GPIO));
    EQ(a << 8 | b, 0x0001, "sbis/sbic");
#if !defined(__AVR_TINY__)
    /* skipping a two-word instruction (LDS) */
    uint8_t c = 0x11;
    __asm__ volatile("sbis %1, 0\n\tlds %0, V8a\n\tnop" : "+r"(c) : "I"(GPIO));
    EQ(c, 0x11, "skip over lds");
    c = 0x11;
    __asm__ volatile("cpse %0, %0\n\tjmp 0\n\tldi %0, 0x22" : "+d"(c));
    EQ(c, 0x22, "cpse over jmp");
#endif
    uint8_t d = 0x80;
    __asm__ volatile("sbrc %0, 7\n\tldi %0, 0x33\n\tsbrs %0, 2\n\tldi %0, 0x44" : "+d"(d));
    EQ(d, 0x44, "sbrc/sbrs");
    uint8_t t = 0x10, u = 0;
    __asm__ volatile("bst %1, 4\n\tbld %0, 6\n\tswap %0" : "+d"(u) : "d"(t));
    EQ(u, 0x04, "bst/bld/swap");
}

/* ---- the core's registers in the data space ---------------------------------------- */

#if defined(__AVR_ARCH__) && (__AVR_ARCH__ < 100)
#define IO_OFFSET 0x20    /* AVRe/AVRe+: I/O space at data 0x20 */
#else
#define IO_OFFSET 0x00
#endif

static void t_core(void)
{
    volatile uint8_t *sreg = (volatile uint8_t *)(0x3F + IO_OFFSET);
    uint8_t s = SREG;
    *sreg = 0x01;                                 /* C, through a pointer */
    uint8_t c;
    __asm__ volatile("ldi %0, 0\n\tadc %0, __zero_reg__" : "=d"(c));
    EQ(c, 1, "SREG through the data space");
    SREG = s;
    volatile uint8_t *spl = (volatile uint8_t *)(0x3D + IO_OFFSET);
    uint8_t here;
    uint16_t sp = spl[0] | spl[1] << 8;
    EQ((uint16_t)&here > sp, 1, "SP through the data space");
#if IO_OFFSET
    /* r0-r31 are data addresses 0-31 */
    uint8_t v;
    __asm__ volatile("ldi r20, 0x5A\n\tldi r30, 20\n\tldi r31, 0\n\tld %0, Z\n\tldi r21, 0xA5\n\tldi r30, 21\n\tst Z, r20\n\tmov %0, r21"
                     : "=r"(v) :: "r20", "r21", "r30", "r31");
    EQ(v, 0x5A, "registers in the data space");
#endif
}

/* ---- interrupts ---------------------------------------------------------------- */

static volatile uint8_t ticks, flag_at_isr;

ISR(__vector_2)
{
    flag_at_isr = TIMER(4);   /* the flag the vector acknowledged */
    ticks++;
}

static void t_timer(void)
{
#if defined(MODEL_XM)
    *(volatile uint8_t *)0xA2 = 0x01;             /* PMIC.CTRL: LOLVLEN */
#endif
    ticks = 0;
    flag_at_isr = 0xFF;
    TIMER(8) = 0xF4;                              /* LOAD = 500 ticks */
    TIMER(9) = 0x01;
    TIMER(0) = 0x07;                              /* EN | PERIODIC | IE */
    sei();
    for (uint16_t spin = 0; ticks < 3 && spin < 10000; spin++)
        __asm__ volatile("sleep");
    cli();
    TIMER(0) = 0;
    EQ(ticks >= 3, 1, "timer interrupts taken");
    EQ(flag_at_isr & 1, 0, "the vector acknowledged the timer");
#if defined(MODEL_XM)
    EQ(*(volatile uint8_t *)0xA0, 0, "PMIC.STATUS after RETI");
#endif
}

/* ---- AVRxm ------------------------------------------------------------------------- */

#if defined(MODEL_XM)
static volatile uint8_t cell = 0x0F;

static void t_xmega(void)
{
    uint8_t r = 0xF0;
    __asm__ volatile("xch Z, %0" : "+r"(r) : "z"(&cell));
    EQ((uint16_t)(r << 8 | cell), 0x0FF0, "xch");
    r = 0x0C;
    __asm__ volatile("las Z, %0" : "+r"(r) : "z"(&cell));
    EQ((uint16_t)(r << 8 | cell), 0xF0FC, "las");
    r = 0xF0;
    __asm__ volatile("lac Z, %0" : "+r"(r) : "z"(&cell));
    EQ((uint16_t)(r << 8 | cell), 0xFC0C, "lac");
    r = 0xFF;
    __asm__ volatile("lat Z, %0" : "+r"(r) : "z"(&cell));
    EQ((uint16_t)(r << 8 | cell), 0x0CF3, "lat");
}

/* One DES block: r0-r7 data, r8-r15 key, 16 rounds; H selects decryption. */
static void des(uint8_t data[8], const uint8_t key[8], uint8_t decrypt)
{
    __asm__ volatile(
        "push r2\n\tpush r3\n\tpush r4\n\tpush r5\n\tpush r6\n\tpush r7\n\tpush r8\n\tpush r9\n\t"
        "push r10\n\tpush r11\n\tpush r12\n\tpush r13\n\tpush r14\n\tpush r15\n\t"
        "ld r0, X+\n\tld r1, X+\n\tld r2, X+\n\tld r3, X+\n\tld r4, X+\n\tld r5, X+\n\tld r6, X+\n\tld r7, X+\n\t"
        "ld r8, Z+\n\tld r9, Z+\n\tld r10, Z+\n\tld r11, Z+\n\tld r12, Z+\n\tld r13, Z+\n\tld r14, Z+\n\tld r15, Z+\n\t"
        "clh\n\tsbrc %2, 0\n\tseh\n\t"
        "des 0\n\tdes 1\n\tdes 2\n\tdes 3\n\tdes 4\n\tdes 5\n\tdes 6\n\tdes 7\n\t"
        "des 8\n\tdes 9\n\tdes 10\n\tdes 11\n\tdes 12\n\tdes 13\n\tdes 14\n\tdes 15\n\t"
        "st -X, r7\n\tst -X, r6\n\tst -X, r5\n\tst -X, r4\n\tst -X, r3\n\tst -X, r2\n\tst -X, r1\n\tst -X, r0\n\t"
        "clr r1\n\t"
        "pop r15\n\tpop r14\n\tpop r13\n\tpop r12\n\tpop r11\n\tpop r10\n\tpop r9\n\tpop r8\n\t"
        "pop r7\n\tpop r6\n\tpop r5\n\tpop r4\n\tpop r3\n\tpop r2"
        : "+x"(data), "+z"(key) : "d"(decrypt) : "r0", "memory");
}

static void t_des(void)
{
    /* FIPS 46 example: key 133457799BBCDFF1, plaintext 0123456789ABCDEF ->
     * 85E813540F0AB405; r7 holds the most significant byte */
    static const uint8_t key[8] = { 0xF1, 0xDF, 0xBC, 0x9B, 0x79, 0x57, 0x34, 0x13 };
    uint8_t d[8] = { 0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01 };
    static const uint8_t ct[8] = { 0x05, 0xB4, 0x0A, 0x0F, 0x54, 0x13, 0xE8, 0x85 };
    des(d, key, 0);
    EQ(memcmp(d, ct, 8) == 0, 1, "des encrypt");
    EQ((uint32_t)d[7] << 24 | (uint32_t)d[6] << 16 | (uint32_t)d[5] << 8 | d[4], 0x85E81354u, "des high");
    des(d, key, 1);
    EQ((uint32_t)d[3] << 24 | (uint32_t)d[2] << 16 | (uint32_t)d[1] << 8 | d[0], 0x89ABCDEFu, "des decrypt");
}
#endif

int main(void)
{
    puts_("UAM self-test (avr, " MODEL_NAME ")\n");
    run("flags", t_flags);
    run("arith", t_arith);
#if !defined(__AVR_TINY__)
    run("mul", t_mul);
#endif
    run("memory", t_mem);
    run("progmem", t_progmem);
    run("calls", t_calls);
    run("io-bits", t_io);
    run("core-regs", t_core);
    run("timer-irq", t_timer);
#if defined(MODEL_XM)
    run("xmega-rmw", t_xmega);
    run("des", t_des);
#endif
    puts_("result: ");
    puthex(n_pass, 2);
    puts_(" passed, ");
    puthex(n_fail, 2);
    puts_(" failed\n");
    if (n_fail) {
        FIN_FAIL = n_fail;
    } else {
        puts_("SELFTEST PASS\n");
        FIN_PASS = 0x55;
    }
    for (;;)
        __asm__ volatile("sleep");
}
