/*
 * xt-dec.c - Runs ember's Xtensa decoder on instruction words, for xt-check.py.
 *
 *     xt-dec MODEL [be] < words
 *
 * Each input line is the instruction's bytes in hex, in memory order. Each output line
 * is the length the decoder takes and the mnemonic it decodes, or "-" if the model has
 * no such instruction.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xtensa.h"
#include "xtensa_opc.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: xt-dec lx106|lx6|lx7 [be] < hex bytes\n");
        return 2;
    }
    const uam_xtensa_config *cfg = !strcmp(argv[1], "lx106") ? &uam_xtensa_lx106
                                   : !strcmp(argv[1], "lx7") ? &uam_xtensa_lx7 : &uam_xtensa_lx6;
    bool be = argc > 2 && !strcmp(argv[2], "be");
    char line[64];
    while (fgets(line, sizeof(line), stdin)) {
        uint8_t b[4] = { 0 };
        unsigned n = 0;
        for (char *p = line; n < 4 && p[0] && p[1] && p[0] != '\n'; p += 2) {
            char h[3] = { p[0], p[1], 0 };
            b[n++] = (uint8_t)strtoul(h, NULL, 16);
        }
        unsigned op0 = be ? b[0] >> 4 : b[0] & 15;
        unsigned len = xt_insn_len(cfg, op0);
        uint32_t w = 0;
        for (unsigned i = 0; i < len && i < 3; i++)
            w = be ? w << 8 | b[i] : w | (uint32_t)b[i] << (8 * i);
        if (be && len <= 3)
            w = xt_be_to_le(w, len);
        const xt_opc *op = len <= 3 ? xt_decode(cfg, w, len) : NULL;
        printf("%u %s\n", len, op ? xt_opname(op->id) : "-");
    }
    return 0;
}
