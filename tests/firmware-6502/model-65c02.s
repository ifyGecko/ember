; model-65c02.s - the 65C02 build (WDC W65C02S): the CMOS instructions and modes, the
; undefined opcodes as NOPs, the bit operations, WAI and decimal mode.

        .r65c02

model_cmos:
        .db     1

model_tests:
        lda     #9
        sta     *tag
        jsr     c_insns
        lda     #<s_cmos
        ldx     #>s_cmos
        jsr     ok
        lda     #10
        sta     *tag
        jsr     c_bits
        lda     #<s_bits
        ldx     #>s_bits
        jsr     ok
        lda     #11
        sta     *tag
        jsr     c_wai
        lda     #<s_wai
        ldx     #>s_wai
        jsr     ok
        lda     #12
        sta     *tag
        sed
        clc
        lda     #0x99
        adc     #0x01           ; 0x00, C; the 65C02's Z is the result's
        php
        cld
        cmp     #0
        bne     1$
        pla
        and     #0x03
        cmp     #0x03
        bne     1$
        lda     #<s_dec
        ldx     #>s_dec
        jmp     ok
1$:     jmp     fail

c_fail: jmp     fail

c_insns:
        ; BRA
        bra     1$
        jmp     fail
1$:     ; PHX, PHY, PLX, PLY
        ldx     #0x12
        ldy     #0x34
        phx
        phy
        ldx     #0
        ldy     #0
        plx                     ; 0x34
        ply                     ; 0x12
        cpx     #0x34
        bne     c_fail
        cpy     #0x12
        bne     c_fail
        ; STZ zp, zp,X, abs, abs,X
        lda     #0xff
        sta     *tmp
        sta     *tmp+1
        sta     0x0340
        sta     0x0341
        ldx     #1
        stz     *tmp
        stz     *tmp,x
        stz     0x0340
        stz     0x0340,x
        lda     *tmp
        ora     *tmp+1
        ora     0x0340
        ora     0x0341
        bne     c_fail
        ; (zp): LDA, STA, ADC, CMP
        lda     #0x40
        sta     *tmp
        lda     #0x03
        sta     *tmp+1          ; -> 0x0340
        lda     #0x27
        sta     [*tmp]
        lda     #0
        lda     [*tmp]
        cmp     #0x27
        bne     c_fail
        clc
        adc     [*tmp]
        cmp     [*tmp]
        beq     c_fail
        cmp     #0x4e
        bne     c_fail
        ; BIT zp,X and abs,X set N and V from memory; BIT #imm only Z
        lda     #0xc0
        sta     0x0342
        ldx     #2
        lda     #0
        bit     0x0340,x
        bpl     c_fail
        bvc     c_fail
        bne     c_fail
        clv
        lda     #0x01
        bit     #0x80           ; Z set, N and V kept
        bne     c_fail
        bvs     c_fail
        bmi     c_fail          ; N kept (LDA cleared it), not bit 7 of #imm
        ; JMP (abs,X)
        lda     #<2$
        sta     0x0352
        lda     #>2$
        sta     0x0353
        ldx     #2
        jmp     [0x0350,x]
        jmp     fail
2$:     ; the undefined opcodes are NOPs of one, two and three bytes
        lda     #0x5a
        .db     0x03                    ; 1
        .db     0x0b                    ; 1
        .db     0x02, 0xa9              ; 2: #imm (0xA9 not run)
        .db     0x44, CELL              ; 2: zp
        .db     0x54, CELL              ; 2: zp,X
        .db     0x5c, 0xa9, 0x00        ; 3
        .db     0xdc, 0xa9, 0x00        ; 3
        .db     0xfc, 0xa9, 0x00        ; 3
        cmp     #0x5a
        bne     9$
        ; INC A, DEC A
        lda     #0xff
        inc     a
        bne     9$
        dec     a
        cmp     #0xff
        bne     9$
        rts
9$:     jmp     fail

c_bits:
        ; RMB, SMB (the vectors cover each bit), BBR, BBS
        lda     #0x00
        sta     *CELL
        smb5    *CELL
        bbr5    *CELL, 9$
        bbs5    *CELL, 1$
        jmp     fail
1$:     bbs4    *CELL, 9$
        rmb5    *CELL
        bbs5    *CELL, 9$
        bbr7    *CELL, 2$
9$:     jmp     fail
2$:     lda     *CELL
        bne     9$
        rts

; WAI wakes on an interrupt request even with I set, and runs on without taking it
c_wai:
        sei
        lda     #0
        sta     *isr_cnt
        lda     #40
        sta     T0+8
        lda     #0x05
        sta     T0
        .db     0xcb            ; WAI (sdas6500 is the Rockwell set: no WAI)
        lda     T0+4
        and     #1
        beq     9$              ; the timer expired
        lda     *isr_cnt
        bne     9$              ; and its request was not taken
        lda     #1
        sta     T0+4
        rts
9$:     jmp     fail

s_cmos: .ascii  "65c02 instructions"
        .db     0
s_bits: .ascii  "bit operations"
        .db     0
s_wai:  .ascii  "wai"
        .db     0
s_dec:  .ascii  "decimal mode"
        .db     0
