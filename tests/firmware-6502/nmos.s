; nmos.s - the NMOS undefined opcodes the vectors do not reach (2A03 and NMOS builds):
; the NOPs' lengths, LAX and SAX absolute, and the unstable stores and LAS.

n_fail: jmp     fail

nmos_tests:
        ; the undefined NOPs take their operands
        lda     #0
        .db     0x1a                    ; NOP
        .db     0x80, 0xa9              ; NOP #imm (0xA9 is not run)
        .db     0x04, CELL              ; NOP zp
        .db     0x14, CELL              ; NOP zp,X
        .db     0x0c, 0x00, 0x03        ; NOP abs
        .db     0x1c, 0x00, 0x03        ; NOP abs,X
        .db     0xfa                    ; NOP
        cmp     #0
        bne     n_fail
        ; LAX and SAX absolute
        lda     #0x5e
        sta     0x0330
        lda     #0
        ldx     #0
        .db     0xaf, 0x30, 0x03        ; LAX 0x0330
        cmp     #0x5e
        bne     n_fail
        cpx     #0x5e
        bne     n_fail
        lda     #0xf0
        ldx     #0x3c
        .db     0x8f, 0x31, 0x03        ; SAX 0x0331
        lda     0x0331
        cmp     #0x30
        bne     n_fail
        ; SHX abs,Y: X & (high byte of the base + 1)
        ldx     #0xff
        ldy     #0x10
        .db     0x9e, 0x80, 0x02        ; SHX 0x0280,Y -> 0x0290
        lda     0x0290
        cmp     #0x03
        bne     n_fail
        ; ... and when indexing crosses a page, the value is the high byte too
        lda     #0xee
        sta     0x0408
        sta     0x0508
        ldx     #0x04
        .db     0x9e, 0xf8, 0x04        ; SHX 0x04F8,Y: 4 & 5 = 4, at 0x0408
        lda     0x0408
        cmp     #0x04
        bne     n_fail
        lda     0x0508
        cmp     #0xee
        bne     9$
        ; SHY abs,X
        ldy     #0xff
        ldx     #0x10
        .db     0x9c, 0x81, 0x02        ; SHY 0x0281,X -> 0x0291
        lda     0x0291
        cmp     #0x03
        bne     9$
        ; SHA abs,Y and (zp),Y: A & X & (H + 1)
        lda     #0xff
        ldx     #0xfe
        ldy     #0x10
        .db     0x9f, 0x82, 0x02        ; SHA 0x0282,Y -> 0x0292
        lda     0x0292
        cmp     #0x02
        bne     9$
        lda     #0x83
        sta     *tmp
        lda     #0x02
        sta     *tmp+1
        lda     #0xfd
        ldx     #0xff
        .db     0x93, tmp               ; SHA (tmp),Y -> 0x0293: 0xFD & 3 = 1
        lda     0x0293
        cmp     #0x01
        bne     9$
        jmp     1$
9$:     jmp     fail
1$:     ; TAS abs,Y: S = A & X, stored & (H + 1)
        tsx
        stx     *spsave
        lda     #0xf3
        ldx     #0x3f
        ldy     #0x10
        .db     0x9b, 0x84, 0x02        ; TAS 0x0284,Y -> 0x0294
        tsx
        stx     *tmp+2
        ldx     *spsave
        txs
        lda     *tmp+2
        cmp     #0x33
        bne     9$
        lda     0x0294
        cmp     #0x03
        bne     9$
        ; LAS abs,Y: A = X = S = memory & S
        lda     #0xf0
        sta     0x02a0
        ldx     #0x3c
        txs
        ldy     #0x20
        lda     #0
        .db     0xbb, 0x80, 0x02        ; LAS 0x0280,Y -> 0x02A0
        stx     *tmp+3
        tsx
        stx     *tmp+2
        ldx     *spsave
        txs
        cmp     #0x30
        bne     9$
        lda     *tmp+3
        cmp     #0x30
        bne     9$
        lda     *tmp+2
        cmp     #0x30
        bne     9$
        rts
