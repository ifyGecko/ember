/*
 * bench.c - One benchmark for every guest ISA (docs/jit-perf.md, "Parity across ISAs").
 *
 * The same kernels, built for each reference board and natively for the host:
 * CRC-32, heapsort, matrix multiply, string search, a switch-dispatch bytecode
 * interpreter, pointer chasing, 64-bit arithmetic and number formatting. They use
 * fixed-width types only (no pointer, long or char signedness reaches a result), so
 * every build prints the same checksums: tools/xbench.sh compares them with the host
 * build's and times each guest.
 *
 * Each kernel runs BENCH_ROUNDS times; with BENCH_ONLY=n only kernel n runs (for
 * timing kernels one by one). The start code (the board's start.S, or x86-start.S)
 * calls main and gives its return value to the test finisher.
 */
#include <stddef.h>
#include <stdint.h>

#ifndef BENCH_ROUNDS
#define BENCH_ROUNDS 400
#endif

/* ---- the board: console output ------------------------------------------------ */

#if defined(BENCH_HOST)
#include <stdio.h>
static void putc_(char c) { putchar(c); }
#elif defined(__x86_64__)
#define COM1 0x3F8
static void putc_(char c)
{
    uint8_t lsr;
    do
        __asm__ volatile("inb %1, %0" : "=a"(lsr) : "Nd"((uint16_t)(COM1 + 5)));
    while (!(lsr & 0x20));
    __asm__ volatile("outb %0, %1" : : "a"((uint8_t)c), "Nd"((uint16_t)COM1));
}
#else
#if defined(__riscv)
#define UART0 0x10000000u
#elif defined(__mips__)
#define UART0 0xbfd003f8u
#elif defined(__arm__)
#define UART0 0x1c000000u
#elif defined(__powerpc__)
#define UART0 0xf0000000u
#else
#error "no board for this ISA"
#endif
#define REG8(a) (*(volatile uint8_t *)(uintptr_t)(a))
static void putc_(char c)
{
    while (!(REG8(UART0 + 5) & 0x20))
        ;
    REG8(UART0) = (uint8_t)c;
}
#endif

static void puts_(const char *s)
{
    while (*s)
        putc_(*s++);
}

static void puthex(uint32_t v)
{
    for (int i = 28; i >= 0; i -= 4)
        putc_("0123456789abcdef"[(v >> i) & 15]);
}

#if !defined(BENCH_HOST)
/* The compiler may call these for copies and clears. */
void *memcpy(void *d, const void *s, size_t n);
void *memcpy(void *d, const void *s, size_t n)
{
    uint8_t *dp = d;
    const uint8_t *sp = s;
    while (n--)
        *dp++ = *sp++;
    return d;
}
void *memset(void *d, int c, size_t n);
void *memset(void *d, int c, size_t n)
{
    uint8_t *dp = d;
    while (n--)
        *dp++ = (uint8_t)c;
    return d;
}

/* An exception is a failed run: report it and stop. */
uint32_t trap_handler(void *frame);
uint32_t trap_handler(void *frame)
{
    (void)frame;
    puts_("bench: unexpected exception\n");
    for (;;)
        ;
}
#endif

/* ---- data ---------------------------------------------------------------------- */

static uint32_t rng;
static uint32_t lcg(void)
{
    rng = rng * 1664525u + 1013904223u;
    return rng;
}

#define BUF_SIZE 4096
static uint8_t buf[BUF_SIZE];

/* ---- kernels --------------------------------------------------------------------- */

/* CRC-32 (IEEE): the table built bit by bit, then table-driven over 4 KiB, twice. */
static uint32_t crc_table[256];
static uint32_t k_crc(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
    for (uint32_t i = 0; i < BUF_SIZE; i++)
        buf[i] = (uint8_t)(lcg() >> 24);
    uint32_t c = 0xFFFFFFFFu;
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t i = 0; i < BUF_SIZE; i++)
            c = crc_table[(c ^ buf[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

/* Heapsort of 1024 words. */
#define SORT_N 1024
static uint32_t sort_a[SORT_N];
static void sift(uint32_t *a, uint32_t root, uint32_t n)
{
    for (;;) {
        uint32_t child = 2 * root + 1;
        if (child >= n)
            return;
        if (child + 1 < n && a[child + 1] > a[child])
            child++;
        if (a[root] >= a[child])
            return;
        uint32_t t = a[root]; a[root] = a[child]; a[child] = t;
        root = child;
    }
}
static uint32_t k_sort(void)
{
    for (uint32_t i = 0; i < SORT_N; i++)
        sort_a[i] = lcg();
    for (uint32_t i = SORT_N / 2; i-- > 0;)
        sift(sort_a, i, SORT_N);
    for (uint32_t n = SORT_N - 1; n > 0; n--) {
        uint32_t t = sort_a[0]; sort_a[0] = sort_a[n]; sort_a[n] = t;
        sift(sort_a, 0, n);
    }
    uint32_t h = 0;
    for (uint32_t i = 0; i < SORT_N; i += 7)
        h = h * 31 + sort_a[i];
    return h;
}

/* 24x24 signed matrix multiply. */
#define MAT_N 24
static int32_t mat_a[MAT_N][MAT_N], mat_b[MAT_N][MAT_N], mat_c[MAT_N][MAT_N];
static uint32_t k_matmul(void)
{
    for (int i = 0; i < MAT_N; i++)
        for (int j = 0; j < MAT_N; j++) {
            mat_a[i][j] = (int32_t)(lcg() >> 20) - 2048;
            mat_b[i][j] = (int32_t)(lcg() >> 20) - 2048;
        }
    for (int i = 0; i < MAT_N; i++)
        for (int j = 0; j < MAT_N; j++) {
            int32_t s = 0;
            for (int k = 0; k < MAT_N; k++)
                s += mat_a[i][k] * mat_b[k][j];
            mat_c[i][j] = s;
        }
    uint32_t h = 0;
    for (int i = 0; i < MAT_N; i++)
        h = h * 33 + (uint32_t)mat_c[i][(i * 7) % MAT_N];
    return h;
}

/* Text of four letters; count the matches of a few patterns by naive search, and
 * the length of each run of one letter. */
static uint32_t k_string(void)
{
    for (uint32_t i = 0; i < BUF_SIZE - 1; i++)
        buf[i] = (uint8_t)('a' + (lcg() >> 30));
    buf[BUF_SIZE - 1] = 0;
    static const uint8_t pat[4][5] = { "abca", "dd", "cab", "bbad" };
    uint32_t h = 0;
    for (int p = 0; p < 4; p++) {
        uint32_t n = 0;
        for (const uint8_t *s = buf; *s; s++) {
            const uint8_t *a = s, *b = pat[p];
            while (*b && *a == *b)
                a++, b++;
            if (!*b)
                n++;
        }
        h = h * 131 + n;
    }
    uint32_t runs = 0, longest = 0;
    for (const uint8_t *s = buf; *s;) {
        const uint8_t *e = s;
        while (*e == *s)
            e++;
        uint32_t len = (uint32_t)(e - s);
        if (len > longest)
            longest = len;
        runs++;
        s = e;
    }
    return h ^ runs << 8 ^ longest;
}

/* A stack-machine bytecode run by a switch: the total of the Collatz path lengths
 * of 1..40. */
enum { OP_PUSH, OP_DUP, OP_DROP, OP_SWAP, OP_OVER, OP_ADD, OP_SUB, OP_MUL3, OP_HALF,
       OP_ODD, OP_EQ1, OP_JZ, OP_JMP, OP_LT, OP_LOADN, OP_STOREN, OP_LOADT, OP_STORET,
       OP_HALT };
static const uint8_t prog[] = {
    OP_PUSH, 1, OP_STOREN, OP_PUSH, 0, OP_STORET,   /*  0: n = 1, t = 0 */
    OP_LOADN, OP_PUSH, 41, OP_LT, OP_JZ, 43,        /*  6: while n < 41 */
    OP_LOADN,                                       /* 12:   x = n */
    OP_DUP, OP_EQ1, OP_JZ, 35,                      /* 13:   while x != 1 */
    OP_LOADT, OP_PUSH, 1, OP_ADD, OP_STORET,        /* 17:     t += 1 */
    OP_DUP, OP_ODD, OP_JZ, 32,                      /* 22:     if x odd */
    OP_MUL3, OP_PUSH, 1, OP_ADD, OP_JMP, 13,        /* 26:       x = 3x + 1 */
    OP_HALF, OP_JMP, 13,                            /* 32:     else x = x / 2 */
    OP_DROP, OP_LOADN, OP_PUSH, 1, OP_ADD, OP_STOREN, OP_JMP, 6,   /* 35: n += 1 */
    OP_LOADT, OP_HALT,                              /* 43: the result: t */
};
static uint32_t k_interp(void)
{
    uint32_t stack[32], n = 0, t = 0;
    unsigned sp = 0, pc = 0;
    uint32_t h = 0;
    for (int run = 0; run < 1; run++) {
        sp = 0;
        pc = 0;
        for (;;) {
            uint8_t op = prog[pc++];
            uint32_t a, b;
            switch (op) {
            case OP_PUSH:   stack[sp++] = prog[pc++]; break;
            case OP_DUP:    a = stack[sp - 1]; stack[sp++] = a; break;
            case OP_DROP:   sp--; break;
            case OP_SWAP:   a = stack[sp - 1]; stack[sp - 1] = stack[sp - 2]; stack[sp - 2] = a; break;
            case OP_OVER:   a = stack[sp - 2]; stack[sp++] = a; break;
            case OP_ADD:    b = stack[--sp]; stack[sp - 1] += b; break;
            case OP_SUB:    b = stack[--sp]; stack[sp - 1] -= b; break;
            case OP_MUL3:   stack[sp - 1] *= 3; break;
            case OP_HALF:   stack[sp - 1] >>= 1; break;
            case OP_ODD:    stack[sp - 1] &= 1; break;
            case OP_EQ1:    stack[sp - 1] = stack[sp - 1] != 1; break;
            case OP_JZ:     a = stack[--sp]; if (!a) pc = prog[pc]; else pc++; break;
            case OP_JMP:    pc = prog[pc]; break;
            case OP_LT:     b = stack[--sp]; stack[sp - 1] = stack[sp - 1] < b; break;
            case OP_LOADN:  stack[sp++] = n; break;
            case OP_STOREN: n = stack[--sp]; break;
            case OP_LOADT:  stack[sp++] = t; break;
            case OP_STORET: t = stack[--sp]; break;
            default:        goto halt;
            }
        }
halt:
        h = h * 7 + stack[sp - 1] + sp;
    }
    return h;
}

/* Walk a linked list whose order is a random permutation of its nodes. */
#define LIST_N 1024
static struct node {
    struct node *next;
    uint32_t     val;
} nodes[LIST_N];
static uint16_t perm[LIST_N];
static uint32_t k_list(void)
{
    for (uint32_t i = 0; i < LIST_N; i++)
        perm[i] = (uint16_t)i;
    for (uint32_t i = LIST_N - 1; i > 0; i--) {
        uint32_t j = lcg() % (i + 1);
        uint16_t t = perm[i]; perm[i] = perm[j]; perm[j] = t;
    }
    for (uint32_t i = 0; i < LIST_N; i++) {
        nodes[perm[i]].next = i + 1 < LIST_N ? &nodes[perm[i + 1]] : NULL;
        nodes[perm[i]].val = lcg() >> 8;
    }
    uint32_t h = 0;
    for (int pass = 0; pass < 16; pass++)
        for (const struct node *p = &nodes[perm[0]]; p; p = p->next)
            h = (h ^ p->val) * 0x01000193u;
    return h;
}

/* 64-bit arithmetic: xorshift64*, multiply-accumulate, rotates, compares. */
static uint32_t k_u64(void)
{
    uint64_t x = 0x9E3779B97F4A7C15ull ^ lcg(), acc = 0, mx = 0;
    for (int i = 0; i < 2000; i++) {
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        uint64_t r = x * 0x2545F4914F6CDD1Dull;
        acc += (r >> 7) + (acc << 3 | acc >> 61);
        if (r > mx)
            mx = r;
        acc -= (r & 0xFFFF) * 3;
    }
    return (uint32_t)(acc ^ acc >> 32) ^ (uint32_t)(mx >> 29);
}

/* Format numbers as decimal text and parse them back. */
static uint32_t k_fmt(void)
{
    uint32_t h = 0;
    for (int i = 0; i < 900; i++) {
        uint32_t v = lcg() >> (lcg() & 15);
        char text[12];
        int n = 0;
        do {
            text[n++] = (char)('0' + v % 10);
            v /= 10;
        } while (v);
        uint32_t back = 0;
        while (n--)
            back = back * 10 + (uint32_t)(text[n] - '0');
        h = h * 31 + back;
    }
    return h;
}

/* ---- driver ------------------------------------------------------------------------ */

static const struct kernel {
    const char *name;
    uint32_t  (*fn)(void);
} kernels[] = {
    { "crc",    k_crc },
    { "sort",   k_sort },
    { "matmul", k_matmul },
    { "string", k_string },
    { "interp", k_interp },
    { "list",   k_list },
    { "u64",    k_u64 },
    { "fmt",    k_fmt },
};

int main(void)
{
    uint32_t total = 0;
    for (unsigned k = 0; k < sizeof(kernels) / sizeof(kernels[0]); k++) {
#ifdef BENCH_ONLY
        if (k != BENCH_ONLY)
            continue;
#endif
        rng = 0x1234567u + k;
        uint32_t sum = 0;
        for (int r = 0; r < BENCH_ROUNDS; r++)
            sum = sum * 31 + kernels[k].fn();
        puts_("bench: ");
        puts_(kernels[k].name);
        putc_(' ');
        puthex(sum);
        putc_('\n');
        total = (total << 5 | total >> 27) ^ sum;
    }
    puts_("bench: total ");
    puthex(total);
    putc_('\n');
    return 0x5555;
}
