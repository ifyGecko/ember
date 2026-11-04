; model-8052.asm - the 8052 model's part of the self-test: the upper 128 bytes of
; internal RAM, two data pointers (AUXR1.DPS at 0xA2) and four priority levels (IPH at
; 0xB7).

        .equ    AUXR1, 0xa2
        .equ    IPH, 0xb7

model_xpage:
        mov     p2, #0x12
        ret

model_tests:
        mov     r7, #0x20           ; @R0 above 0x7f is RAM, not the SFR there
        mov     p1, #0xff
        mov     r0, #0x90
        mov     @r0, #0x5a
        mov     a, p1
        cjne    a, #0xff, m2_bad
        mov     a, @r0
        cjne    a, #0x5a, m2_bad
        mov     a, sp               ; the stack in the upper half
        mov     r6, a
        mov     sp, #0xc0
        lcall   m2_sub
m2_ret: mov     r0, #0xc1
        mov     a, @r0
        cjne    a, #(m2_ret & 0xff), m2_bad
        mov     a, r6
        mov     sp, a
        mov     dptr, #s_upper
        lcall   ok
        sjmp    m2_dp
m2_bad: ljmp    fail
m2_dp:

        mov     r7, #0x21           ; DPTR0 and DPTR1
        mov     AUXR1, #0
        mov     dptr, #0x1111
        inc     AUXR1               ; DPS = 1
        mov     dptr, #0x2222
        mov     a, #0x77
        movx    @dptr, a
        inc     AUXR1               ; DPS = 0 (bit 1 reads 0)
        mov     a, dpl
        cjne    a, #0x11, m2_bad
        mov     a, AUXR1
        jnz     m2_bad
        orl     AUXR1, #1
        mov     a, dph
        cjne    a, #0x22, m2_bad
        clr     a
        movx    a, @dptr
        cjne    a, #0x77, m2_bad
        mov     AUXR1, #0
        mov     dptr, #s_dptr
        lcall   ok

        mov     r7, #0x22           ; Timer 1 at level 2 preempts Timer 0 at level 1
        mov     tmod, #0x11
        mov     th0, #0xff
        mov     tl0, #0xfc
        mov     th1, #0xff
        mov     tl1, #0xf0
        mov     ISR_CNT, #0
        mov     NEST, #1
        mov     ip, #0x02           ; PT0: level 1
        mov     IPH, #0x08          ; PT1H: level 2
        mov     ie, #0x8a
        setb    tr0
m2_w:   mov     a, ISR_CNT
        cjne    a, #2, m2_w
        mov     ie, #0
        mov     ip, #0
        mov     IPH, #0
        mov     NEST, #0
        mov     a, ISR_LOG
        cjne    a, #3, m2_bad2
        mov     dptr, #s_iph
        ljmp    ok
m2_sub: ret
m2_bad2:
        ljmp    fail

s_upper:    .db "upper RAM", 0
s_dptr:     .db "dual DPTR", 0
s_iph:      .db "four levels", 0
