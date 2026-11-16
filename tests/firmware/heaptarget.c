/*
 * heaptarget.c - A fuzz target with planted heap and stack bugs for rv32-ref
 * (tests/fuzz/oracle-check.sh).
 *
 * It has an allocator of its own (fw_malloc, fw_free, fw_realloc over fw_heap) with its
 * bookkeeping in front of each block, as firmware does: on flat memory none of the bugs
 * below faults, so without an oracle every input ends "ok". A board that hooks the three
 * functions into a guarded heap (uam/heap.h; tests/fuzz/heap_board.c does) catches the
 * heap ones, and the return guard (--guard-returns) the stack one.
 *
 * fuzz_iteration drains the UART; the first byte picks the case:
 *   'k'  a block allocated, filled, grown, read back and freed: nothing wrong
 *   'o'  a write one byte past a block's end          'r'  a read there
 *   'u'  a write one byte before a block's start
 *   'f'  a read of a freed block                      'g'  a write through a pointer
 *   'd'  a block freed twice                               realloc left behind
 *   'b'  a free of an address inside a block
 *   'w'  a write to heap memory that is in no block
 *   's'  a buffer overrun on the stack that replaces a return address with the address
 *        of recover(), which nothing calls
 *   'j'  a longjmp out of a call: nothing wrong
 *   't'  a second stack, entered for the first time by returning into co_entry (which
 *        nothing calls: the return guard reports it unless told co_entry is a valid
 *        return target), and left by returning into the first: nothing wrong
 */
#include <stdint.h>

#define UART0    0x10000000u
#define REG8(a)  (*(volatile uint8_t *)(uintptr_t)(a))
#define UART_THR REG8(UART0 + 0)
#define UART_LSR REG8(UART0 + 5)
#define BUF_SIZE 64
#define HEAP_SIZE 4096

static uint8_t g_buf[BUF_SIZE];
volatile uint32_t g_sink;

/* ---- the firmware's own allocator ---------------------------------------- */

uint8_t fw_heap[HEAP_SIZE] __attribute__((aligned(16)));
static uint32_t fw_top;

/* A header of 8 bytes (the size, then a "freed" mark) in front of each block. */
void *fw_malloc(uint32_t n) __attribute__((noinline));
void *fw_malloc(uint32_t n)
{
    n = (n + 7u) & ~7u;
    if (fw_top + 8 + n > HEAP_SIZE)
        fw_top = 0;                     /* a toy: start over */
    uint32_t *h = (uint32_t *)(void *)&fw_heap[fw_top];
    h[0] = n;
    h[1] = 0;
    fw_top += 8 + n;
    return h + 2;
}

void fw_free(void *p) __attribute__((noinline));
void fw_free(void *p)
{
    if (p)
        ((uint32_t *)p)[-1] = 1;
}

void *fw_realloc(void *p, uint32_t n) __attribute__((noinline));
void *fw_realloc(void *p, uint32_t n)
{
    uint8_t *q = fw_malloc(n), *o = p;
    uint32_t old = o ? ((uint32_t *)p)[-2] : 0;
    for (uint32_t i = 0; i < old && i < n; i++)
        q[i] = o[i];
    fw_free(p);
    return q;
}

/* ---- setjmp/longjmp and a context switch, by hand ------------------------- */

typedef struct ctx { uint32_t ra, sp, s[12]; } ctx;

int  my_setjmp(ctx *c);
void my_longjmp(ctx *c);
void ctx_switch(ctx *from, ctx *to);

__asm__(
    ".text\n"
    ".macro CTX_SAVE r\n"
    "  sw ra, 0(\\r)\n  sw sp, 4(\\r)\n"
    "  sw s0, 8(\\r)\n  sw s1, 12(\\r)\n  sw s2, 16(\\r)\n  sw s3, 20(\\r)\n"
    "  sw s4, 24(\\r)\n sw s5, 28(\\r)\n  sw s6, 32(\\r)\n  sw s7, 36(\\r)\n"
    "  sw s8, 40(\\r)\n sw s9, 44(\\r)\n  sw s10, 48(\\r)\n sw s11, 52(\\r)\n"
    ".endm\n"
    ".macro CTX_LOAD r\n"
    "  lw ra, 0(\\r)\n  lw sp, 4(\\r)\n"
    "  lw s0, 8(\\r)\n  lw s1, 12(\\r)\n  lw s2, 16(\\r)\n  lw s3, 20(\\r)\n"
    "  lw s4, 24(\\r)\n lw s5, 28(\\r)\n  lw s6, 32(\\r)\n  lw s7, 36(\\r)\n"
    "  lw s8, 40(\\r)\n lw s9, 44(\\r)\n  lw s10, 48(\\r)\n lw s11, 52(\\r)\n"
    ".endm\n"
    ".globl my_setjmp\nmy_setjmp:\n  CTX_SAVE a0\n  li a0, 0\n  ret\n"
    ".globl my_longjmp\nmy_longjmp:\n  CTX_LOAD a0\n  li a0, 1\n  ret\n"
    ".globl ctx_switch\nctx_switch:\n  CTX_SAVE a0\n  CTX_LOAD a1\n  ret\n"
    /* Where the smashed return address leads: a fresh stack, and on with the main loop. */
    ".globl recover\nrecover:\n  la sp, __stack_top\n  j main_loop\n"
);

void recover(void);
void main_loop(void) __attribute__((noreturn));

static ctx g_jmp, g_main_ctx, g_co_ctx;
static uint32_t g_co_stack[256];

static volatile unsigned g_depth = 3;

static __attribute__((noinline)) uint32_t deep(unsigned n)
{
    if (n)
        return deep(n - 1) + n;
    my_longjmp(&g_jmp);
    return 0;
}

void co_entry(void) __attribute__((noinline));
void co_entry(void)
{
    g_sink += 7;
    ctx_switch(&g_co_ctx, &g_main_ctx);
    for (;;)
        g_sink++;
}

/* ---- the planted stack bug ------------------------------------------------ */

static __attribute__((noinline)) void touch(volatile uint32_t *p)
{
    g_sink += p[0];
}

static volatile unsigned g_words = 12;   /* how far smash() writes: the compiler cannot tell */

static __attribute__((noinline)) void smash(void)
{
    volatile uint32_t buf[4];
    const unsigned words = g_words;
    for (unsigned i = 0; i < words; i++)
        buf[i] = (uint32_t)(uintptr_t)recover;   /* planted bug: 12 words into 4 */
    touch(buf);
}

/* ---- one input ------------------------------------------------------------ */

static __attribute__((noinline)) void parse(const uint8_t *in, unsigned n)
{
    if (!n)
        return;
    volatile uint8_t *p = fw_malloc(24);
    volatile uint8_t *q;
    switch (in[0]) {
    case 'k':
        for (unsigned i = 0; i < 24; i++)
            p[i] = (uint8_t)i;
        q = fw_realloc((void *)p, 48);
        for (unsigned i = 24; i < 48; i++)
            q[i] = q[i - 24];
        g_sink += q[47];
        fw_free((void *)q);
        return;
    case 'o': p[24] = 1; break;
    case 'r': g_sink += p[24]; break;
    case 'u': p[-1] = 1; break;
    case 'f': fw_free((void *)p); g_sink += p[0]; return;
    case 'g': q = fw_realloc((void *)p, 64); p[0] = 1; fw_free((void *)q); return;
    case 'd': fw_free((void *)p); fw_free((void *)p); return;
    case 'b': fw_free((void *)(p + 4)); break;
    case 'w': fw_heap[HEAP_SIZE - 8] = 1; break;
    case 's': smash(); break;
    case 'j':
        if (!my_setjmp(&g_jmp))
            g_sink += deep(g_depth);
        break;
    case 't':
        g_co_ctx.ra = (uint32_t)(uintptr_t)co_entry;
        g_co_ctx.sp = (uint32_t)(uintptr_t)&g_co_stack[256];
        ctx_switch(&g_main_ctx, &g_co_ctx);
        break;
    default:
        p[0] = in[0];
        break;
    }
    fw_free((void *)p);
}

void fuzz_panic(void) __attribute__((noinline, noreturn));
void fuzz_panic(void)
{
    for (;;)
        g_sink++;
}

uint32_t trap_handler(void *frame);
uint32_t trap_handler(void *frame)
{
    (void)frame;
    fuzz_panic();
}

void fuzz_iteration(void) __attribute__((noinline));
void fuzz_iteration(void)
{
    unsigned n = 0;
    while ((UART_LSR & 0x01) && n < BUF_SIZE)
        g_buf[n++] = UART_THR;
    parse(g_buf, n);
}

void main_loop(void)
{
    for (;;)
        fuzz_iteration();
}

int main(void);
int main(void)
{
    main_loop();
}
