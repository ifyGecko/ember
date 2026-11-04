; model-cip51.asm - the CIP-51 model's part of the self-test: SFR paging (SFRPAGE at
; 0x84; mcs51-ref puts the ports P4-P7 on page 0x0F only), EMI0CN (0xAA) paging MOVX
; @Ri, and code banking (PSBANK at 0xB1: IFBANK in bits 1:0, COBANK in 5:4). The
; banked blocks come from bank.asm, placed by mkimage.py: block n's code at 0x8000
; returns 0xB0 + n in A, and its byte at 0x8100 is 0xD0 + n.

        .equ    SFRPAGE, 0x84
        .equ    P4, 0xc8
        .equ    EMI0CN, 0xaa
        .equ    PSBANK, 0xb1

model_xpage:
        mov     EMI0CN, #0x12
        ret

model_tests:
        mov     r7, #0x30           ; SFR pages
        mov     SFRPAGE, #0x0f
        mov     P4, #0x5a
        mov     p1, #0xa5           ; the ports are on every page
        mov     a, #0x3c            ; core SFRs too
        mov     b, a
        mov     SFRPAGE, #0
        mov     a, b
        cjne    a, #0x3c, mc_bad
        mov     a, p1
        cjne    a, #0xa5, mc_bad
        mov     SFRPAGE, #0x0f
        mov     a, P4
        cjne    a, #0x5a, mc_bad
        mov     a, SFRPAGE
        cjne    a, #0x0f, mc_bad
        mov     SFRPAGE, #0
        mov     dptr, #s_pages
        lcall   ok

        mov     r7, #0x31           ; code banks
        mov     a, PSBANK
        cjne    a, #0x11, mc_bad    ; after reset: block 1, the window's own addresses
        mov     PSBANK, #0x22
        lcall   0x8000
        cjne    a, #0xb2, mc_bad
        mov     dptr, #0x8100
        clr     a
        movc    a, @a+dptr
        cjne    a, #0xd2, mc_bad
        mov     PSBANK, #0x31       ; fetch from block 1, MOVC from block 3
        lcall   0x8000
        cjne    a, #0xb1, mc_bad
        clr     a
        movc    a, @a+dptr
        cjne    a, #0xd3, mc_bad
        mov     PSBANK, #0x03       ; block 0 aliases the common area
        clr     a
        movc    a, @a+dptr          ; 0x8100 in block 0 is code address 0x0100
        cjne    a, #0x75, mc_bad    ; start: mov sp, #STACK is 75 81 60
        mov     PSBANK, #0x11
        mov     dptr, #s_banks2
        ljmp    ok
mc_bad: mov     SFRPAGE, #0
        mov     PSBANK, #0x11
        ljmp    fail

s_pages:    .db "SFR pages", 0
s_banks2:   .db "code banks", 0
