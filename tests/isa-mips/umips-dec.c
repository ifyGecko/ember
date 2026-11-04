/* umips-dec: reads "addr h0 h1" lines (hex) and prints umips_decode's result for
 * each, for umips-check.py. */
#include <stdio.h>
#include "mips.h"

int main(void)
{
    unsigned a, h0, h1;
    while (scanf("%x %x %x", &a, &h0, &h1) == 3) {
        um_insn d;
        umips_decode(a | 1u, (uint16_t)h0, (uint16_t)h1, &d);
        printf("%x %d %u %08x %d %u %u %u %u %d %d %d %d %08x %08x", a, d.kind, d.len, d.w, d.cond,
               d.rs, d.rt, d.rd, d.cc, d.tf, d.link, d.compact, d.short_slot, d.imm, d.target);
        printf(" %u", d.nregs);
        for (unsigned k = 0; k < d.nregs; k++)
            printf(",%u", d.regs[k]);
        printf("\n");
    }
    return 0;
}
