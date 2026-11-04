; model-2a03.s - the 2A03 build: the NMOS undefined opcodes, and D only a flag.

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
        php
        cld
        cmp     #0x0a           ; binary
        bne     1$
        pla
        and     #0x08           ; D set all the same
        beq     1$
        lda     #<s_dec
        ldx     #>s_dec
        jmp     ok
1$:     jmp     fail

s_nmos: .ascii  "nmos undefined opcodes"
        .db     0
s_dec:  .ascii  "no decimal mode"
        .db     0
