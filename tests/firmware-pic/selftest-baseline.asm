; selftest-baseline.asm - The baseline (12-bit) self-test for pic-ref -o model=baseline.
;
; Starts at the reset vector, the last word (erased: XORLW 0xFF), and wraps to 0. Flag
; vectors (vectors-baseline.inc, pages 1-3: GOTO with the PA page bits), then: the two-level
; stack (a third CALL loses the oldest return address), RETLW, a computed goto through PCL
; (PC<8> cleared), indirect addressing through FSR across the banks, addresses 0x00-0x0F of
; every bank mapping back to bank 0, an FSR at INDF reading 0, FSR<7:5> as the bank of direct
; operands, bit operations and skips, the STATUS write rule and PCL reads. The devices are in
; bank 7, reached through FSR.
        list    p=16f59
        radix   dec
        errorlevel -302, -1302

INDF    equ     0x00
PCL     equ     0x02
STATUS  equ     0x03
FSR     equ     0x04
C       equ     0
DC      equ     1
Z       equ     2

UTHR    equ     0xF0
ULSR    equ     0xF5
TFIN    equ     0xF8
TCODE   equ     0xF9

; common RAM (0x0A-0x0F, every bank), then bank 0
VX      equ     0x0A
VSTAT   equ     0x0B
VECL    equ     0x0C
VECH    equ     0x0D
TNUM    equ     0x0E
T0      equ     0x0F
T1      equ     0x10
T2      equ     0x11
CNT     equ     0x12

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

        org     0
        goto    start

; ---- subroutines: CALL reaches the first 256 words of a page ----
putc:   movwf   T0
pw:     movlw   ULSR
        movwf   FSR
        btfss   INDF,5
        goto    pw
        movlw   UTHR
        movwf   FSR
        movf    T0,w
        movwf   INDF
        clrf    FSR
        retlw   0
puthex: movwf   VX
        swapf   VX,w
        call    nibble
        movf    VX,w
        goto    nibble
nibble: andlw   0x0F
        movwf   T0
        movlw   10
        subwf   T0,w                ; C: the nibble is >= 10
        movlw   '0'
        btfsc   STATUS,C
        movlw   'A' - 10
        addwf   T0,w
        goto    putc
fail:   clrf    STATUS
        clrf    FSR
        movlw   'F'
        call    putc
        movlw   ' '
        call    putc
        movf    TNUM,w
        call    puthex
        movlw   '\n'
        call    putc
        movlw   TCODE
        movwf   FSR
        movf    TNUM,w
        movwf   INDF
fhang:  goto    fhang
vfail:  clrf    STATUS
        movlw   'V'
        call    putc
        movf    VECL,w
        call    puthex
        movlw   ' '
        call    putc
        goto    fail
table:  addwf   PCL,f               ; PC<8> = 0: this table is in the first 256 words
        retlw   'a'
        retlw   'b'
        retlw   'c'
lvl1:   call    lvl2                ; the third push loses the oldest return address
        incf    CNT,f
        movlw   2
        subwf   CNT,w
        btfsc   STATUS,Z
        goto    t2_out              ; the second time here: the stack held two levels
        retlw   1
lvl2:   call    lvl3
        retlw   2
lvl3:   retlw   3

start:  clrf    FSR
        movlw   'b'
        call    putc
        movlw   'a'
        call    putc
        movlw   's'
        call    putc
        movlw   'e'
        call    putc
        movlw   '\n'
        call    putc
        TEST    1
        movlw   0x20                ; PA = 1: page 1
        movwf   STATUS
        goto    vectors_start
vectors_done:

; ---- the stack, RETLW, computed goto ----
        TEST    2
        movlw   2
        call    table
        xorlw   'c'
        EXPECTZ
        clrf    CNT
        call    lvl1
        call    fail                ; not reached: the return to here was lost
t2_out:

; ---- indirect addressing, banks ----
        TEST    3
        movlw   0x30                ; bank 1's 0x10
        movwf   FSR
        movlw   0x5A
        movwf   INDF
        movlw   0x20                ; FSR<7:5> = 1: direct 0x10 is data 0x30
        movwf   FSR
        movf    0x10,w
        clrf    FSR
        xorlw   0x5A
        EXPECTZ
        movlw   0x11                ; bank 0's 0x10 is another register
        movwf   T1
        EXPECT  T1, 0x11
        movlw   0x2E                ; bank 1's 0x0E maps back to 0x0E (TNUM)
        movwf   FSR
        movf    INDF,w
        clrf    FSR
        xorlw   3
        EXPECTZ
        movlw   0xE0                ; FSR at 0xE0: maps back to 0x00, INDF: reads 0
        movwf   FSR
        movf    INDF,w
        clrf    FSR
        EXPECTZ
        movlw   0x31                ; read-modify-write through INDF
        movwf   FSR
        movlw   9
        movwf   INDF
        incf    INDF,f
        movf    INDF,w
        clrf    FSR
        xorlw   10
        EXPECTZ

; ---- bits, skips ----
        TEST    4
        movlw   3
        movwf   T1
        clrf    T2
sk1:    incf    T2,f
        decfsz  T1,f
        goto    sk1
        EXPECT  T2, 3
        movlw   0xFF
        movwf   T1
        incfsz  T1,f
        call    fail
        bsf     T1,6
        btfss   T1,6
        call    fail
        bcf     T1,6
        btfsc   T1,6
        call    fail

; ---- STATUS writes, PCL reads ----
        TEST    5
        clrf    STATUS              ; PA clear, Z set, C and DC clear: 0001 1100
        swapf   STATUS,w
        xorlw   0xC1
        EXPECTZ
pclhere: movf   PCL,w
        xorlw   low(pclhere + 1)
        EXPECTZ

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
        movlw   TFIN
        movwf   FSR
        movlw   0x55
        movwf   INDF
done:   goto    done

#include "vectors-baseline.inc"
        end
