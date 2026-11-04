/* mips16-dec: reads "addr ext has_ext i i2" lines (hex) and prints mips16_decode's
 * result for each, for mips16-check.py. */
#include <stdio.h>
#include "mips.h"

int main(void)
{
    unsigned a, e, h, i, i2;
    while (scanf("%x %x %x %x %x", &a, &e, &h, &i, &i2) == 5) {
        m16_insn d;
        mips16_decode(a | 1u, (uint16_t)e, h != 0, (uint16_t)i, (uint16_t)i2, &d);
        printf("%x %d %u %08x %u %u %08x %08x %d %d %u %u %u %u %d %d %d\n", a, d.kind, d.len, d.w,
               d.rx, d.rd, d.imm, d.target, d.link, d.compact, d.args, d.astatic, d.xsregs,
               d.framesize, d.ra, d.s0, d.s1);
    }
    return 0;
}
