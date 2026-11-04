; selftest-mid.asm - The mid-range (14-bit) self-test for pic-ref -o model=mid.
;
; Flag vectors (vectors-mid.inc, pages 1-3), then: an 8-deep call chain, a computed goto
; through PCL and PCLATH, a call into another page, indirect addressing with IRP, INDF read
; through an FSR at INDF, the RP bank bits and the common RAM every bank shares, bit
; operations and skips, the STATUS write rule, PCL reads, OPTION and TRIS, and the timer
; interrupt (vector 4, GIE/PEIE, SLEEP waking) and INTCON's own T0IE/T0IF pair.
        list    p=16f877
        radix   dec
        errorlevel -302, -306, -1302

INDF    equ     0x00
PCL     equ     0x02
STATUS  equ     0x03
FSR     equ     0x04
PCLATH  equ     0x0A
INTCON  equ     0x0B
C       equ     0
DC      equ     1
Z       equ     2
RP0     equ     5
RP1     equ     6
IRP     equ     7

; pic-ref devices: the UART and finisher in bank 2, the timer in bank 3
UTHR    equ     0x10C
ULSR    equ     0x111
TFIN    equ     0x114
TCODE   equ     0x115
TCTRL   equ     0x18C
TSTAT   equ     0x190
TLOAD   equ     0x194

; common RAM (0x70-0x7F, every bank)
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
WSAVE   equ     0x7A
SSAVE   equ     0x7B
DEPTH   equ     0x7C

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
        org     4
isr:    movwf   WSAVE
        swapf   STATUS,w
        movwf   SSAVE
        btfss   INTCON,2            ; T0IF: INTCON's own pair
        goto    isr_timer
        bcf     INTCON,2
        incf    PCNT,f
        goto    isr_out
isr_timer:
        bsf     STATUS,RP0          ; bank 3: clear the timer's flag
        bsf     STATUS,RP1
        movlw   1
        movwf   TSTAT
        incf    ICNT,f
isr_out:
        swapf   SSAVE,w             ; STATUS (and the bank) back
        movwf   STATUS
        swapf   WSAVE,f
        swapf   WSAVE,w
        retfie

start:  movlw   'm'
        call    putc
        movlw   'i'
        call    putc
        movlw   'd'
        call    putc
        movlw   '\n'
        call    putc
        TEST    1
        movlw   0x08                ; page 1
        movwf   PCLATH
        goto    vectors_start
vectors_done:

; ---- calls ----
        TEST    2
        clrf    DEPTH
        call    c1
        EXPECT  DEPTH, 8
        movlw   2                   ; computed goto
        call    table
        xorlw   'c'
        EXPECTZ
        movlw   high(far_sub)       ; a call into page 3
        movwf   PCLATH
        call    far_sub
        clrf    PCLATH
        xorlw   0x42
        EXPECTZ
        goto    t3

c1:     incf    DEPTH,f
        call    c2
        return
c2:     incf    DEPTH,f
        call    c3
        return
c3:     incf    DEPTH,f
        call    c4
        return
c4:     incf    DEPTH,f
        call    c5
        return
c5:     incf    DEPTH,f
        call    c6
        return
c6:     incf    DEPTH,f
        call    c7
        return
c7:     incf    DEPTH,f
        call    c8
        return
c8:     incf    DEPTH,f             ; eight return addresses on the stack
        return

table:  movwf   T0
        movlw   high(tab0)
        movwf   PCLATH
        movf    T0,w
        addwf   PCL,f
tab0:   retlw   'a'
        retlw   'b'
        retlw   'c'

; ---- indirect addressing, banks ----
t3:     TEST    3
        clrf    PCLATH
        bsf     STATUS,IRP          ; FSR 0x20 with IRP: data 0x120 (bank 2)
        movlw   0x20
        movwf   FSR
        movlw   0x5A
        movwf   INDF
        bcf     STATUS,IRP
        bsf     STATUS,RP1          ; read it directly from bank 2
        movf    0x20,w
        bcf     STATUS,RP1
        xorlw   0x5A
        EXPECTZ
        movlw   0x20                ; and not in bank 0
        movwf   0x20
        EXPECT  0x20, 0x20
        clrf    FSR                 ; INDF through an FSR at INDF reads 0
        movf    INDF,w
        EXPECTZ
        movlw   0x22                ; INDF read-modify-write
        movwf   FSR
        movlw   7
        movwf   0x22
        incf    INDF,f
        EXPECT  0x22, 8
        movlw   0x99                ; the common RAM: 0x70 is 0xF0, 0x170, 0x1F0
        movwf   T2
        bsf     STATUS,RP0
        movf    T2,w                ; 0xF7
        bcf     STATUS,RP0
        xorlw   0x99
        EXPECTZ
        bsf     STATUS,IRP          ; and through FSR: 0x1F7
        movlw   0xF7
        movwf   FSR
        movf    INDF,w
        bcf     STATUS,IRP
        xorlw   0x99
        EXPECTZ

; ---- bits, skips ----
        TEST    4
        movlw   3
        movwf   T0
        clrf    T1
sk1:    incf    T1,f
        decfsz  T0,f
        goto    sk1
        EXPECT  T1, 3
        movlw   0xFF
        movwf   T0
        incfsz  T0,f                ; 0: skip
        call    fail
        bsf     T0,3
        btfss   T0,3
        call    fail
        bcf     T0,3
        btfsc   T0,3
        call    fail

; ---- STATUS writes, PCL reads, OPTION and TRIS ----
        TEST    5
        bsf     STATUS,RP0
        clrf    STATUS              ; "000u u100": the bank bits clear, Z set, C and DC clear
        swapf   STATUS,w
        xorlw   0xC1                ; TO = PD = 1 since reset
        EXPECTZ
        movlw   0x07
        movwf   STATUS
        swapf   STATUS,w
        xorlw   0xF1
        EXPECTZ
pclhere: movf   PCL,w
        xorlw   low(pclhere + 1)
        EXPECTZ
        movlw   0x3C
        option                      ; OPTION_REG at 0x81
        movlw   0x0F
        tris    6                   ; TRISB at 0x86
        bsf     STATUS,RP0
        movf    0x01,w
        bcf     STATUS,RP0
        xorlw   0x3C
        EXPECTZ
        bsf     STATUS,RP0
        movf    0x06,w
        bcf     STATUS,RP0
        xorlw   0x0F
        EXPECTZ

; ---- interrupts ----
        TEST    6
        clrf    ICNT
        clrf    PCNT
        bsf     STATUS,RP0
        bsf     STATUS,RP1
        movlw   50
        movwf   TLOAD
        movlw   0x05                ; EN | IE
        movwf   TCTRL
        bcf     STATUS,RP0
        bcf     STATUS,RP1
        movlw   0xC0                ; GIE | PEIE
        movwf   INTCON
        sleep
        nop
        EXPECT  ICNT, 1
        btfss   INTCON,7
        call    fail
        bsf     INTCON,5            ; T0IE, then its flag
        bsf     INTCON,2
        nop
        EXPECT  PCNT, 1
        clrf    INTCON

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
        bsf     STATUS,RP1
        movlw   0x55
        movwf   TFIN
done:   goto    done

; ---- reporting ----
putc:   movwf   T2
        bsf     STATUS,RP1          ; bank 2
pw:     btfss   ULSR,5
        goto    pw
        movf    T2,w
        movwf   UTHR
        bcf     STATUS,RP1
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
        clrf    STATUS
        clrf    PCLATH
        movlw   'F'
        call    putc
        movlw   ' '
        call    putc
        movf    TNUM,w
        call    puthex
        movlw   '\n'
        call    putc
        movf    TNUM,w
        bsf     STATUS,RP1
        movwf   TCODE
fhang:  goto    fhang
vfail:  clrf    STATUS
        clrf    PCLATH
        movlw   'V'
        call    putc
        movf    VECH,w
        call    puthex
        movf    VECL,w
        call    puthex
        movlw   ' '
        call    putc
        goto    fail

#include "vectors-mid.inc"

        org     0x1FF0
far_sub: retlw  0x42
        end
