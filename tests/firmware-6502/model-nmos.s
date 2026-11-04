; model-nmos.s - the NMOS 6502 build: the undefined opcodes and decimal mode.

model_cmos:
        .db     0

model_tests:
        lda     #9
        sta     *tag
        jsr     nmos_tests
        lda     #<s_nmos
        ldx     #>s_nmos
        jsr     ok
        lda     #10
        sta     *tag
        sed
        clc
        lda     #0x09
        adc     #0x01
        cld
        cmp     #0x10
        bne     1$
        lda     #<s_dec
        ldx     #>s_dec
        jmp     ok
1$:     jmp     fail

s_nmos: .ascii  "nmos undefined opcodes"
        .db     0
s_dec:  .ascii  "decimal mode"
        .db     0
