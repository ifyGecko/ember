; selftest.s - the 6502 self-test, common part (sdas6500). The Makefile appends the
; model's part (model-*.s: model_tests, model_cmos) and the generated vectors.
;
; Console: the 16550 at 0x7F00, polled. The finisher is at 0x7F10: 0x55 there passes, a
; byte at 0x7F11 fails with that code (the failing test's tag). Each test sets `tag`
; before checking anything; `fail` prints it.
;
; Only the documented NMOS instructions are used here, so the common part runs on every
; model; the model parts test what differs.

        .module selftest
        .r6500
        .area   CODE (ABS)

        UART    = 0x7f00
        LSR     = 0x7f05
        FIN     = 0x7f10
        T0      = 0x7f20        ; gptimer: +0 CTRL (EN 1, PERIODIC 2, IE 4), +4 STATUS,
        T1      = 0x7f30        ;          +8 LOAD (32 bits, little-endian)
        CODE    = 0x0300        ; RAM: the instruction a vector runs
        CELL    = 0x40          ; the vectors' memory byte

        tag     = 0x02
        vp      = 0x10          ; vector pointer
        sptr    = 0x12          ; string pointer
        cnt     = 0x14          ; vectors left
        va      = 0x16
        ra      = 0x17
        rx      = 0x18
        rp      = 0x19
        tmp     = 0x1a          ; 0x1a-0x1f scratch
        isr_cnt = 0x20          ; IRQs taken
        nmi_cnt = 0x21          ; NMIs taken
        isr_p   = 0x22          ; the P an interrupt or BRK pushed
        isr_x   = 0x23          ; X when the IRQ was taken
        brk_cnt = 0x24
        brk_ret = 0x25          ; BRK's return address (2)
        cur_p   = 0x27          ; P inside the handler
        spsave  = 0x28

        .org    0x8000
start:  sei
        cld
        ldx     #0xff
        txs
        lda     #0
        ldx     #0x40
1$:     sta     *0,x            ; clear the zero page below CELL
        dex
        bpl     1$
        lda     #<banner
        ldx     #>banner
        jsr     puts

        lda     #1
        sta     *tag
        jsr     t_vectors
        lda     #<s_vectors
        ldx     #>s_vectors
        jsr     ok
        lda     #2
        sta     *tag
        jsr     t_addr
        lda     #<s_addr
        ldx     #>s_addr
        jsr     ok
        lda     #3
        sta     *tag
        jsr     t_stack
        lda     #<s_stack
        ldx     #>s_stack
        jsr     ok
        lda     #4
        sta     *tag
        jsr     t_branch
        lda     #<s_branch
        ldx     #>s_branch
        jsr     ok
        lda     #5
        sta     *tag
        jsr     t_brk
        lda     #<s_brk
        ldx     #>s_brk
        jsr     ok
        lda     #6
        sta     *tag
        jsr     t_irq
        lda     #<s_irq
        ldx     #>s_irq
        jsr     ok
        lda     #7
        sta     *tag
        jsr     t_nmi
        lda     #<s_nmi
        ldx     #>s_nmi
        jsr     ok
        lda     #8
        sta     *tag
        jsr     t_smc
        lda     #<s_smc
        ldx     #>s_smc
        jsr     ok
        jsr     model_tests
        lda     #<s_pass
        ldx     #>s_pass
        jsr     puts
        lda     #0x55
        sta     FIN
        jmp     .

; ---- console ---------------------------------------------------------------------

putc:   pha
1$:     lda     LSR
        and     #0x20
        beq     1$
        pla
        sta     UART
        rts

; puts: the zero-terminated string at A (low), X (high)
puts:   sta     *sptr
        stx     *sptr+1
        ldy     #0
1$:     lda     [*sptr],y
        beq     2$
        jsr     putc
        iny
        bne     1$
2$:     rts

phex:   pha
        lsr     a
        lsr     a
        lsr     a
        lsr     a
        jsr     1$
        pla
        and     #0x0f
1$:     cmp     #10
        bcc     2$
        adc     #6              ; C set: + 7
2$:     adc     #0x30
        jmp     putc

; ok: "ok <name>\n"
ok:     pha
        txa
        pha
        lda     #<s_ok
        ldx     #>s_ok
        jsr     puts
        pla
        tax
        pla
        jsr     puts
        lda     #10
        jmp     putc

fail:   cld
        sei
        lda     #<s_fail
        ldx     #>s_fail
        jsr     puts
        lda     *tag
        jsr     phex
        lda     #10
        jsr     putc
        lda     *tag
        sta     FIN+1
        jmp     .

; ---- 1: instruction vectors (gen_vectors.py) --------------------------------------

t_vectors:
        lda     #<vectors
        sta     *vp
        lda     #>vectors
        sta     *vp+1
        ldy     #0
        lda     [*vp],y
        sta     *cnt
        iny
        lda     [*vp],y
        sta     *cnt+1
        lda     #2
        jsr     advance
        lda     #0x60           ; RTS after the instruction and its operand
        sta     CODE+2
v_next: lda     *cnt
        ora     *cnt+1
        bne     1$
        rts
1$:     ldy     #0
        lda     [*vp],y
        sta     CODE
        iny
        lda     [*vp],y
        sta     CODE+1
        iny
        lda     [*vp],y
        sta     *va
        iny
        lda     [*vp],y
        tax
        iny
        lda     [*vp],y
        sta     *CELL
        iny
        lda     [*vp],y
        pha
        lda     *va
        plp
        jsr     CODE
        php
        cld
        sta     *ra
        stx     *rx
        pla
        sta     *rp
        ldy     #6
        lda     [*vp],y
        cmp     *ra
        bne     v_bad
        iny
        lda     [*vp],y
        cmp     *rx
        bne     v_bad
        iny
        lda     [*vp],y
        cmp     *CELL
        bne     v_bad
        iny
        lda     [*vp],y
        cmp     *rp
        bne     v_bad
        lda     #10
        jsr     advance
        lda     *cnt
        bne     2$
        dec     *cnt+1
2$:     dec     *cnt
        jmp     v_next
; print the vector and what came out: op opnd a x cell p / a x cell p
v_bad:  ldy     #0
1$:     lda     [*vp],y
        jsr     phex
        lda     #0x20
        jsr     putc
        iny
        cpy     #6
        bne     1$
        lda     #0x2f
        jsr     putc
        lda     *ra
        jsr     phex
        lda     *rx
        jsr     phex
        lda     *CELL
        jsr     phex
        lda     *rp
        jsr     phex
        lda     #10
        jsr     putc
        jmp     fail

; vp += A
advance:
        clc
        adc     *vp
        sta     *vp
        bcc     1$
        inc     *vp+1
1$:     rts

; ---- 2: addressing modes ------------------------------------------------------------

t_addr:
        ; zp,X wraps within the zero page: 0xFE + 0x20 = 0x1E
        lda     #0x5a
        ldx     #0x20
        sta     *0xfe,x
        lda     #0
        lda     *tmp+4
        cmp     #0x5a
        bne     1$
        ; zp,Y (LDX) likewise
        ldy     #0x21
        ldx     *0xfe,y         ; 0x1F
        stx     *tmp
        lda     #0x77
        sta     *tmp+5
        ldy     #0x21
        ldx     *0xfe,y
        cpx     #0x77
        bne     1$
        ; abs,X crosses into the next page
        lda     #0xc3
        ldx     #0x20
        sta     0x02f0,x
        lda     0x0310
        cmp     #0xc3
        bne     1$
        ; abs,Y wraps at 64 KiB: 0xFFF5 + 0x30 = 0x0025
        lda     #0x3c
        sta     *0x25
        ldy     #0x30
        lda     0xfff5,y
        cmp     #0x3c
        bne     1$
        ; (zp,X) takes its pointer's high byte from 0x00 after 0xFF
        lda     #0x20
        sta     *0xff
        lda     #0x03
        sta     *0x00
        lda     #0x99
        sta     0x0320
        ldx     #0
        lda     #0
        lda     [*0xff,x]
        cmp     #0x99
        bne     1$
        ldx     #0x10           ; (0xEF + 0x10) & 0xFF = 0xFF
        lda     #0
        lda     [*0xef,x]
        cmp     #0x99
        bne     1$
        ; (zp),Y: the pointer at 0x1A is 0x02FF, + 1 crosses the page
        lda     #0xff
        sta     *tmp
        lda     #0x02
        sta     *tmp+1
        ldy     #0x21
        lda     #0xa7
        sta     [*tmp],y        ; 0x0320
        lda     0x0320
        cmp     #0xa7
        bne     1$
        jmp     t_jmpind
1$:     jmp     fail

; JMP (0x02FF): the NMOS takes the high byte from 0x0200, the 65C02 from 0x0300
t_jmpind:
        lda     #<jt_target
        sta     0x02ff
        lda     #>jt_nmos
        sta     0x0200
        lda     #>jt_cmos
        sta     0x0300
        jmp     [0x02ff]
t_jmpdone:
        cmp     model_cmos
        bne     1$
        rts
1$:     jmp     fail

; ---- 3: the stack -------------------------------------------------------------------

t_stack:
        ; JSR pushes the address of its last byte, high byte first
        jsr     1$
2$:     jmp     3$
1$:     tsx
        lda     0x0101,x
        cmp     #<(2$-1)
        bne     9$
        lda     0x0102,x
        cmp     #>(2$-1)
        bne     9$
        rts
3$:     ; PHP pushes B and bit 5; PLP ignores them
        lda     #0
        pha
        plp
        php
        pla
        cmp     #0x30
        bne     9$
        lda     #0xcf           ; every flag but B and bit 5
        pha
        plp
        php
        cld
        sei
        pla
        cmp     #0xff
        bne     9$
        ; RTI pulls P and the pc (no + 1)
        lda     #>4$
        pha
        lda     #<4$
        pha
        lda     #0x83           ; N, C (and I clear)
        pha
        rti
9$:     jmp     fail
4$:     php
        sei
        pla
        cmp     #0xb3           ; N, bit 5, B, C
        bne     9$
        ; S wraps within page 1
        tsx
        stx     *spsave
        ldx     #0
        txs
        lda     #0x6e
        pha                     ; at 0x0100; S = 0xFF
        tsx
        cpx     #0xff
        bne     5$
        lda     0x0100
        cmp     #0x6e
        bne     5$
        lda     #0
        pla                     ; S back to 0
        cmp     #0x6e
        bne     5$
        tsx
        bne     5$
        ldx     *spsave
        txs
        rts
5$:     ldx     *spsave
        txs
        jmp     fail

; ---- 4: branches --------------------------------------------------------------------

t_branch:
        lda     #0
        clc
        bcs     9$
        bcc     1$
        jmp     fail
1$:     sec
        bcc     9$
        bcs     2$
9$:     jmp     fail
2$:     lda     #0              ; Z
        bne     9$
        beq     3$
        jmp     fail
3$:     lda     #0x80           ; N
        bpl     9$
        bmi     4$
        jmp     fail
4$:     clv
        bvs     9$
        lda     #0x40
        adc     #0x40           ; V
        bvc     9$
        ; a backward branch that counts
        ldx     #5
        ldy     #0
5$:     iny
        dex
        bne     5$
        cpy     #5
        bne     9$
        rts

; ---- 5: BRK -------------------------------------------------------------------------

t_brk:
        lda     #0
        sta     *brk_cnt
        sed
        brk
        .db     0xee            ; the signature byte BRK skips
1$:     php
        cld
        pla
        and     #0x08
        beq     9$              ; RTI restored D
        lda     *brk_cnt
        cmp     #1
        bne     9$
        lda     *isr_p
        and     #0x38           ; B and bit 5 pushed, and D
        cmp     #0x38
        bne     9$
        lda     *brk_ret
        cmp     #<1$
        bne     9$
        lda     *brk_ret+1
        cmp     #>1$
        bne     9$
        ; the handler ran with I set, and D cleared on CMOS
        lda     *cur_p
        and     #0x04
        beq     9$
        lda     *cur_p
        and     #0x08
        lsr     a
        lsr     a
        lsr     a
        eor     #1              ; 1 if D was cleared
        cmp     model_cmos
        bne     9$
        rts
9$:     jmp     fail

; ---- 6: IRQ -------------------------------------------------------------------------

t_irq:
        lda     #0
        sta     *isr_cnt
        lda     #50
        sta     T0+8
        lda     #0
        sta     T0+9
        lda     #0x05           ; EN | IE, one-shot
        sta     T0
        cli
        ldy     #0
1$:     lda     *isr_cnt
        bne     2$
        iny
        bne     1$
        jmp     9$
2$:     sei
        lda     *isr_p
        and     #0x34           ; B clear, bit 5 set, I clear
        cmp     #0x20
        bne     9$
        ; with I set the request waits; after CLI one more instruction runs first
        lda     #0
        sta     *isr_cnt
        lda     #1
        sta     T0+8
        lda     #0x05
        sta     T0
3$:     lda     T0+4
        and     #1
        beq     3$              ; expired: the line is high
        lda     *isr_cnt
        bne     9$
        ldx     #0
        cli
        inx
        inx
        inx
        sei
        lda     *isr_cnt
        cmp     #1
        bne     9$
        lda     *isr_x
        cmp     #1
        bne     9$
        rts
9$:     sei
        jmp     fail

; ---- 7: NMI -------------------------------------------------------------------------

t_nmi:
        lda     #0
        sta     *nmi_cnt
        sei                     ; I does not mask NMI
        lda     #30
        sta     T1+8
        lda     #0x05
        sta     T1
        ldy     #0
1$:     lda     *nmi_cnt
        bne     2$
        iny
        bne     1$
        jmp     fail
2$:     cmp     #1
        bne     9$
        ; a second edge (the handler cleared the flag: the line fell)
        lda     #0x05
        sta     T1
        ldy     #0
3$:     lda     *nmi_cnt
        cmp     #2
        beq     4$
        iny
        bne     3$
9$:     jmp     fail
4$:     rts

; ---- 8: self-modifying code ---------------------------------------------------------

t_smc:
        lda     #0xa9           ; LDA #imm
        sta     CODE
        lda     #0x11
        sta     CODE+1
        lda     #0x60
        sta     CODE+2
        jsr     CODE
        cmp     #0x11
        bne     9$
        lda     #0x22           ; rewrite the operand of code that has run
        sta     CODE+1
        jsr     CODE
        cmp     #0x22
        bne     9$
        rts
9$:     jmp     fail

; ---- interrupt handlers ---------------------------------------------------------------

irq:    pha
        txa
        pha
        php
        pla
        sta     *cur_p
        tsx
        lda     0x0103,x        ; the pushed P (above X and A)
        sta     *isr_p
        and     #0x10
        bne     1$
        lda     0x0101,x        ; X, pushed last
        sta     *isr_x
        lda     #1              ; clear the timer's flag: the line falls
        sta     T0+4
        inc     *isr_cnt
        pla
        tax
        pla
        rti
1$:     lda     0x0104,x
        sta     *brk_ret
        lda     0x0105,x
        sta     *brk_ret+1
        inc     *brk_cnt
        pla
        tax
        pla
        rti

nmi:    pha
        lda     #1
        sta     T1+4
        inc     *nmi_cnt
        pla
        rti

banner: .ascii  "6502 self-test\n"
        .db     0
s_ok:   .ascii  "ok "
        .db     0
s_fail: .ascii  "FAIL "
        .db     0
s_pass: .ascii  "PASS\n"
        .db     0
s_vectors: .ascii "vectors"
        .db     0
s_addr: .ascii  "addressing"
        .db     0
s_stack: .ascii "stack"
        .db     0
s_branch: .ascii "branches"
        .db     0
s_brk:  .ascii  "brk"
        .db     0
s_irq:  .ascii  "irq"
        .db     0
s_nmi:  .ascii  "nmi"
        .db     0
s_smc:  .ascii  "self-modifying code"
        .db     0

; The two targets of t_jmpind, at the same offset in two pages: A says which ran.
        .org    0x9055
jt_nmos = 0x9055
jt_target = 0x55
        lda     #0
        jmp     t_jmpdone
        .org    0xa055
jt_cmos = 0xa055
        lda     #1
        jmp     t_jmpdone

        .org    0xfffa
        .dw     nmi, start, irq

        .org    0xa100
