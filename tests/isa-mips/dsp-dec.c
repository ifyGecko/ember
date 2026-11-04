/* dsp-dec: reads MIPS32 words (hex) and prints mips_dsp_rev for each, for dsp-check.py. */
#include <stdio.h>
#include "mips.h"

int main(void)
{
    unsigned w;
    while (scanf("%x", &w) == 1)
        printf("%08x %d\n", w, mips_dsp_rev(w));
    return 0;
}
