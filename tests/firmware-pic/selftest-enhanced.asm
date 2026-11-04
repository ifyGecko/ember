; selftest-enhanced.asm - The enhanced mid-range self-test for pic-ref -o model=enhanced.
;
; Flag vectors (vectors-enhanced.inc, from page 1), then: a 16-deep call chain with STKPTR
; and TOS, BRA, BRW and CALLW, MOVLP and a call into another page, the FSRs over the
; traditional data space, the linear view (0x2000) and program memory (0x8000), MOVIW and
; MOVWI in every mode and indexed, ADDFSR, an FSR at INDF reading 0, BSR banking and the
; common RAM, OPTION and TRIS, the STATUS write rule and PCL reads, the timer interrupt
; with the automatic context save (RETFIE restores W, STATUS, BSR, PCLATH, FSR0/1),
; INTCON's own TMR0IE/TMR0IF pair, and last the RESET instruction and a stack overflow
; reset (STVREN), told apart by a RAM byte that survives them.
        list    p=16f1939
        radix   dec
        errorlevel -302, -1302

INDF0   equ     0x00
INDF1   equ     0x01
PCL     equ     0x02
STATUS  equ     0x03
FSR0L   equ     0x04
FSR0H   equ     0x05
FSR1L   equ     0x06
FSR1H   equ     0x07
BSR     equ     0x08
WREG    equ     0x09
PCLATH  equ     0x0A
INTCON  equ     0x0B
STKPTR  equ     0xFED
TOSL    equ     0xFEE
TOSH    equ     0xFEF
FSR0    equ     0x04
FSR1    equ     0x06
C       equ     0
DC      equ     1
Z       equ     2

UTHR    equ     0x10C
ULSR    equ     0x111
TFIN    equ     0x114
TCODE   equ     0x115
TCTRL   equ     0x18C
TSTAT   equ     0x190
TLOAD   equ     0x194

VX      equ     0x70
VSTAT   equ     0x71
VECL    equ     0x72
VECH    equ     0x73
TNUM    equ     0x74
T0      equ     0x75
T1      equ     0x76
T2      equ     0x77
ICNT    equ     0x78
PCNT    equ     0x79
DEPTH   equ     0x7A
SP0     equ     0x7B
PHASE   equ     0x7F

TEST    macro   n
        movlw   n
        movwf   TNUM
        endm
EXPECTZ macro
        btfss   STATUS,Z
        call    fail
        endm
EXPECT  macro   f, k
        movf    f,w
        xorlw   k
        EXPECTZ
        endm
; STKPTR is in bank 31
EXPECTSP macro  k
        movlb   31
        movf    STKPTR,w
        movlb   0
        xorlw   k
        EXPECTZ
        endm

        org     0
        goto    start
        org     4
isr:    movlw   0xEE                ; clobber what the context save restores
        movwf   FSR0L
        movwf   FSR1H
        movlb   3
        btfss   INTCON,2            ; TMR0IF: INTCON's own pair
        goto    isr_timer
        bcf     INTCON,2
        incf    PCNT,f
        movlp   0x7F                ; (after the GOTO, which PCLATH steers)
        retfie
isr_timer:
        movlw   1
        movwf   TSTAT               ; bank 3: clear the timer's flag
        incf    ICNT,f
        bsf     STATUS,C
        movlp   0x7F
        retfie

start:  movlw   0xA1
        xorwf   PHASE,w
        btfsc   STATUS,Z
        goto    after_reset
        movlw   0xA2
        xorwf   PHASE,w
        btfsc   STATUS,Z
        goto    after_ovf
        movlw   'e'
        call    putc
        movlw   'n'
        call    putc
        movlw   'h'
        call    putc
        movlw   '\n'
        call    putc
        TEST    1
        movlp   0x08
        goto    vectors_start
vectors_done:

; ---- the stack ----
        TEST    2
        EXPECTSP 0x1F        ; empty
        clrf    DEPTH
        call    deep
        EXPECT  DEPTH, 16
        EXPECTSP 0x1F
        call    tos_check
tos_ret: goto   t3

deep:   incf    DEPTH,f
        movlw   16
        xorwf   DEPTH,w
        btfss   STATUS,Z
        call    deep                ; sixteen return addresses
        return

tos_check:
        EXPECTSP 0
        movlb   31                  ; TOS is this call's return address
        movf    TOSL,w
        movlb   0
        xorlw   low(tos_ret)
        EXPECTZ
        return

; ---- BRA, BRW, CALLW, pages ----
t3:     TEST    3
        bra     t3a
        call    fail
t3a:    movlw   2
        call    brw_table
        xorlw   'c'
        EXPECTZ
        movlp   high(callw_target) & 0x7F
        movlw   low(callw_target)
        callw
        movlp   0
        xorlw   0x77
        EXPECTZ
        movlp   high(far_sub) & 0x7F
        call    far_sub
        movlp   0
        xorlw   0x42
        EXPECTZ
        goto    t4
brw_table:
        brw
        retlw   'a'
        retlw   'b'
        retlw   'c'
callw_target:
        retlw   0x77

; ---- the FSRs ----
t4:     TEST    4
        movlw   0x20                ; traditional: 0x120 (bank 2)
        movwf   FSR0L
        movlw   0x01
        movwf   FSR0H
        movlw   0x5A
        movwf   INDF0
        movlb   2
        movf    0x20,w
        movlb   0
        xorlw   0x5A
        EXPECTZ
        movlw   low(0x2050)         ; linear 0x2050: bank 1's first byte, 0xA0
        movwf   FSR1L
        movlw   high(0x2050)
        movwf   FSR1H
        movlw   0x3C
        movwf   INDF1
        movlb   1
        movf    0x20,w
        movlb   0
        xorlw   0x3C
        EXPECTZ
        movlw   low(0x209F)         ; linear 0x209F: bank 1's last, 0xEF
        movwf   FSR1L
        movlw   0x99
        movwf   INDF1
        movlb   1
        movf    0x6F,w
        movlb   0
        xorlw   0x99
        EXPECTZ
        movlw   low(0x8000 + progdata)   ; program memory: the low byte of each word
        movwf   FSR0L
        movlw   high(0x8000 + progdata)
        movwf   FSR0H
        moviw   FSR0++
        xorlw   0x11
        EXPECTZ
        moviw   FSR0++
        xorlw   0x22
        EXPECTZ
        moviw   1[FSR0]
        xorlw   0x44
        EXPECTZ
        movlw   0x30                ; MOVWI/MOVIW modes at 0x30
        movwf   FSR1L
        clrf    FSR1H
        movlw   1
        movwi   FSR1++              ; 0x30 = 1
        movlw   2
        movwi   FSR1++              ; 0x31 = 2
        movlw   3
        movwi   ++FSR1              ; 0x33 = 3
        EXPECT  FSR1L, 0x33
        moviw   --FSR1              ; 0x32: 0
        btfss   STATUS,Z
        call    fail
        moviw   FSR1--              ; 0x32, FSR1 = 0x31
        moviw   FSR1--              ; 0x31 = 2, FSR1 = 0x30
        xorlw   2
        EXPECTZ
        moviw   3[FSR1]             ; 0x33
        xorlw   3
        EXPECTZ
        movlw   0x55
        movwi   -1[FSR1]            ; 0x2F
        EXPECT  0x2F, 0x55
        addfsr  FSR1,-16            ; 0x20
        EXPECT  FSR1L, 0x20
        clrf    FSR1L               ; FSR1 at INDF0 (data 0x000): reads 0
        clrf    FSR1H
        moviw   0[FSR1]
        btfss   STATUS,Z
        call    fail
        movlw   0x23                ; read-modify-write through INDF
        movwf   FSR0L
        clrf    FSR0H
        movlw   7
        movwf   0x23
        incf    INDF0,f
        EXPECT  0x23, 8

; ---- banks, OPTION/TRIS, STATUS, PCL ----
        TEST    5
        movlw   0x66                ; the common RAM
        movwf   T2
        movlb   20
        movf    T2,w
        movlb   0
        xorlw   0x66
        EXPECTZ
        movlw   0x3C
        option                      ; OPTION_REG at 0x95
        movlw   0x0F
        tris    5                   ; TRISA at 0x8C
        movlb   1
        movf    0x15,w
        movwf   T0
        movf    0x0C,w
        movwf   T1
        movlb   0
        EXPECT  T0, 0x3C
        EXPECT  T1, 0x0F
        clrf    STATUS              ; Z set, C and DC clear; TO and PD read-only
        swapf   STATUS,w
        xorlw   0xC1
        EXPECTZ
pclhere: movf   PCL,w
        xorlw   low(pclhere + 1)
        EXPECTZ

; ---- interrupts ----
        TEST    6
        clrf    ICNT
        clrf    PCNT
        movlb   3
        movlw   50
        movwf   TLOAD
        movlw   0x05
        movwf   TCTRL
        movlb   5                   ; context the handler clobbers
        movlw   0x12
        movwf   FSR0L
        movlw   0x34
        movwf   FSR1H
        movlp   0x03
        bcf     STATUS,C
        movlw   0xC0
        movwf   INTCON
        movlw   0xAB
        sleep
        nop
        movwf   T0                  ; W restored
        movf    BSR,w
        movwf   T1
        movf    PCLATH,w
        movwf   T2
        movlp   0
        movlb   0
        btfsc   STATUS,C            ; STATUS restored (the handler set C)
        call    fail
        EXPECT  ICNT, 1
        EXPECT  T0, 0xAB
        EXPECT  T1, 5
        EXPECT  T2, 3
        EXPECT  FSR0L, 0x12
        EXPECT  FSR1H, 0x34
        bsf     INTCON,5            ; TMR0IE, then its flag
        bsf     INTCON,2
        nop
        EXPECT  PCNT, 1
        clrf    INTCON

; ---- resets ----
        TEST    7
        movlw   0xA1
        movwf   PHASE
        reset
        call    fail
after_reset:
        TEST    7
        EXPECTSP 0x1F
        movlw   0xA2
        movwf   PHASE
        TEST    8
recurse:
        call    recurse             ; the 17th push resets the device
after_ovf:
        TEST    8
        clrf    PHASE
        EXPECTSP 0x1F
        movlw   'P'
        call    putc
        movlw   'A'
        call    putc
        movlw   'S'
        call    putc
        movlw   'S'
        call    putc
        movlw   '\n'
        call    putc
        movlb   2
        movlw   0x55
        movwf   TFIN
done:   goto    done

; ---- reporting ----
putc:   movwf   T2
        movlb   2
pw:     btfss   ULSR,5
        goto    pw
        movf    T2,w
        movwf   UTHR
        movlb   0
        return
puthex: movwf   T1
        swapf   T1,w
        call    nibble
        movf    T1,w
nibble: andlw   0x0F
        addlw   0xF6
        btfsc   STATUS,C
        addlw   7
        addlw   0x3A
        goto    putc
fail:   clrf    INTCON
        movlp   0
        movlb   0
        movlw   'F'
        call    putc
        movlw   ' '
        call    putc
        movf    TNUM,w
        call    puthex
        movlw   '\n'
        call    putc
        movf    TNUM,w
        movlb   2
        movwf   TCODE
fhang:  goto    fhang
vfail:  movlp   0
        movlb   0
        movlw   'V'
        call    putc
        movf    VECH,w
        call    puthex
        movf    VECL,w
        call    puthex
        movlw   ' '
        call    putc
        goto    fail

progdata:
        retlw   0x11
        retlw   0x22
        retlw   0x33
        retlw   0x44

#include "vectors-enhanced.inc"

        org     0x7FF0
far_sub: retlw  0x42
        end
