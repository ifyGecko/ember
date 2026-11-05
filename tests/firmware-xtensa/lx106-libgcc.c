/*
 * lx106-libgcc.c - What the LX106's libgcc leaves out: __umulsidi3 (its 64-bit multiply
 * calls it) and __umodsi3, which the ESP8266's ROM provides on the chip. Built into the
 * LX106 images only (the core has MULL, but no MULUH and no divide).
 */
#include <stdint.h>

uint64_t __umulsidi3(uint32_t a, uint32_t b);
uint64_t __umulsidi3(uint32_t a, uint32_t b)
{
    uint32_t al = a & 0xFFFF, ah = a >> 16, bl = b & 0xFFFF, bh = b >> 16;
    uint64_t lo = (uint64_t)(al * bl), mid1 = (uint64_t)(al * bh), mid2 = (uint64_t)(ah * bl);
    uint64_t hi = (uint64_t)(ah * bh);
    return lo + (mid1 << 16) + (mid2 << 16) + (hi << 32);
}

/* Shift and subtract: a % b (b = 0 gives a). */
uint32_t __umodsi3(uint32_t a, uint32_t b);
uint32_t __umodsi3(uint32_t a, uint32_t b)
{
    uint32_t r = 0;
    if (!b)
        return a;
    for (int i = 31; i >= 0; i--) {
        r = r << 1 | ((a >> i) & 1);
        if (r >= b)
            r -= b;
    }
    return r;
}
