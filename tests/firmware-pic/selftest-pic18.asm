; selftest-pic18.asm - The PIC18 self-test for pic-ref -o model=pic18 (boards/pic_ref.c).
;
; Flag vectors (vectors-pic18.inc, from gen_vectors.py), then: multiply, DAW, the skips
; (including over a two-word instruction), branches and calls, a computed goto through PCL
; and PCLATH, indirect addressing through FSR0-2 in every mode, MOVFF, banked and access-bank
; operands (and a banked write that reaches a core register), table reads, the stack (PUSH,
; POP, TOS, STKPTR, CALL/RETURN FAST), interrupts without and with priorities (the timer
; high, the UART low; RETFIE FAST; INTCON's own TMR0IE/TMR0IF pair), and last the RESET
; instruction (RCON.RI) and a stack overflow reset (STKFUL). Reports through the UART and
; the test finisher (a failing test's number is the exit code).
        list    p=18f4620
        radix   dec
        errorlevel -302, -1302

ACCESS  equ     0
BANKED  equ     1
FAST    equ     1

; core registers
TOSU    equ     0xFFF
TOSH    equ     0xFFE
TOSL    equ     0xFFD
STKPTR  equ     0xFFC
PCLATU  equ     0xFFB
PCLATH  equ     0xFFA
PCL     equ     0xFF9
TBLPTRU equ     0xFF8
TBLPTRH equ     0xFF7
TBLPTRL equ     0xFF6
TABLAT  equ     0xFF5
PRODH   equ     0xFF4
PRODL   equ     0xFF3
INTCON  equ     0xFF2
INDF0   equ     0xFEF
POSTINC0 equ    0xFEE
POSTDEC0 equ    0xFED
PREINC0 equ     0xFEC
PLUSW0  equ     0xFEB
FSR0H   equ     0xFEA
FSR0L   equ     0xFE9
WREG    equ     0xFE8
INDF1   equ     0xFE7
POSTINC1 equ    0xFE6
FSR1H   equ     0xFE2
FSR1L   equ     0xFE1
BSR     equ     0xFE0
INDF2   equ     0xFDF
POSTINC2 equ    0xFDE
FSR2H   equ     0xFDA
FSR2L   equ     0xFD9
STATUS  equ     0xFD8
RCON    equ     0xFD0
Z       equ     2
C       equ     0
N       equ     4
OV      equ     3
DC      equ     1

; pic-ref devices
UTHR    equ     0xF60
UIER    equ     0xF61
ULSR    equ     0xF65
TFIN    equ     0xF68
TCODE   equ     0xF69
TCTRL   equ     0xF70
TSTAT   equ     0xF74
TLOAD   equ     0xF78

; RAM (access bank)
VX      equ     0x00
VSTAT   equ     0x01
VECL    equ     0x02
VECH    equ     0x03
TNUM    equ     0x04
T0      equ     0x05
T1      equ     0x06
T2      equ     0x07
ICNT    equ     0x08        ; high-priority (timer) interrupts
LCNT    equ     0x09        ; low-priority (UART) interrupts
PCNT    equ     0x0A        ; INTCON pair interrupts
INLOW   equ     0x0B        ; set while the low handler runs
PRE     equ     0x0C        ; high interrupts seen inside the low handler
PHASE   equ     0x5F        ; survives resets
BUF     equ     0x40        ; 16 bytes

TEST    macro   n
        movlw   n
        movwf   TNUM,A
        endm
; fail unless Z
EXPECTZ macro
        btfss   STATUS,Z,A
        call    fail
        endm
; fail unless register f == k
EXPECT  macro   f, k
        movf    f,w,A
        xorlw   k
        EXPECTZ
        endm

        org     0
        goto    start
        org     0x08
        goto    isr_high
        org     0x18
        goto    isr_low

start:  movlw   0xA1
        cpfseq  PHASE,A
        bra     start2
        goto    after_reset
start2: movlw   0xA2
        cpfseq  PHASE,A
        bra     start3
        goto    after_ovf
start3: movlw   upper(banner)
        movwf   TBLPTRU,A
        movlw   high(banner)
        movwf   TBLPTRH,A
        movlw   low(banner)
        movwf   TBLPTRL,A
        call    puts
        TEST    1
        goto    vectors_start
vectors_done:

; ---- multiply, DAW, SETF, CLRF ----
        TEST    2
        movlw   0xC5
        movwf   T0,A
        movlw   0x3B
        mulwf   T0,A                ; 0x3B * 0xC5 = 0x2D67
        EXPECT  PRODH, 0x2D
        EXPECT  PRODL, 0x67
        movlw   0xFF
        mullw   0xFF                ; 0xFE01
        EXPECT  PRODH, 0xFE
        EXPECT  PRODL, 0x01
        movlw   0xA5                ; DAW examples of the data sheet
        bcf     STATUS,C,A
        bcf     STATUS,DC,A
        daw
        xorlw   0x05
        EXPECTZ
        btfss   STATUS,C,A
        call    fail
        movlw   0xCE
        bcf     STATUS,C,A
        bcf     STATUS,DC,A
        daw
        xorlw   0x34
        EXPECTZ
        btfss   STATUS,C,A
        call    fail
        movlw   0x19                ; 0x19 + 0x19 = 0x32 binary, DC set: DAW gives 0x38
        movwf   T0,A
        movlw   0x19
        addwf   T0,w,A              ; W = 0x32, DC = 1
        daw
        xorlw   0x38
        EXPECTZ
        setf    T0,A
        EXPECT  T0, 0xFF
        clrf    T0,A
        btfss   STATUS,Z,A
        call    fail

; ---- skips ----
        TEST    3
        movlw   5
        movwf   T0,A
        movlw   5
        clrf    T1,A
        cpfseq  T0,A                ; equal: skip the two-word MOVFF entirely
        movff   T0,T1
        EXPECT  T1, 0
        movlw   4
        cpfsgt  T0,A                ; 5 > 4: skip
        call    fail
        movlw   6
        cpfslt  T0,A                ; 5 < 6: skip
        call    fail
        movlw   5
        cpfsgt  T0,A                ; 5 > 5: no skip
        bra     sk1
        call    fail
sk1:    clrf    T1,A
        tstfsz  T1,A
        call    fail
        movlw   0xFF
        movwf   T1,A
        infsnz  T1,f,A              ; 0x00: no skip
        bra     sk2
        call    fail
sk2:    infsnz  T1,f,A              ; 0x01: skip
        call    fail
        dcfsnz  T1,f,A              ; 0x00: no skip
        bra     sk3
        call    fail
sk3:    movlw   3
        movwf   T1,A
        clrf    T2,A
sk4:    incf    T2,f,A
        decfsz  T1,f,A
        bra     sk4
        EXPECT  T2, 3
        movlw   0xFE
        movwf   T1,A
        incfsz  T1,f,A              ; 0xFF: no skip
        incfsz  T1,f,A              ; 0x00: skip
        call    fail
        movlw   0x80
        movwf   T1,A
        btfss   T1,7,A
        call    fail
        btfsc   T1,6,A
        call    fail
        btg     T1,0,A
        btg     T1,7,A
        EXPECT  T1, 0x01
        bsf     T1,4,A
        bcf     T1,0,A
        EXPECT  T1, 0x10

; ---- branches and calls ----
        TEST    4
        movlw   0
        iorlw   0                   ; Z = 1, N = 0
        bnz     bfail
        bn      bfail
        bz      b1
        bra     bfail
b1:     movlw   0x80
        iorlw   0                   ; N = 1, Z = 0
        bz      bfail
        bnn     bfail
        bn      b2
        bra     bfail
b2:     movlw   0x7F
        addlw   1                   ; OV = 1, C = 0
        bnov    bfail
        bc      bfail
        bov     b3
        bra     bfail
b3:     movlw   0xFF
        addlw   1                   ; C = 1, OV = 0
        bnc     bfail
        bov     bfail
        bc      b4
bfail:  call    fail
b4:     clrf    T0,A
        rcall   sub_inc
        call    sub_inc
        EXPECT  T0, 2
        movlw   2                   ; computed goto: PCL with PCLATH latched by reading PCL
        call    table
        xorlw   'c'
        EXPECTZ
        movlw   0
        call    table
        xorlw   'a'
        EXPECTZ
        bra     b5
sub_inc: incf   T0,f,A
        return
table:  rlncf   WREG,w,A            ; two bytes per entry
        movwf   T1,A
        movf    PCL,w,A             ; latches PCLATH and PCLATU
        movf    T1,w,A
        addlw   low(tab0)
        btfsc   STATUS,C,A
        incf    PCLATH,f,A
        movwf   PCL,A
tab0:   retlw   'a'
        retlw   'b'
        retlw   'c'
b5:

; ---- indirect addressing ----
        TEST    5
        lfsr    0,BUF
        movlw   0x11
        movwf   POSTINC0,A
        movlw   0x22
        movwf   POSTINC0,A
        movlw   0x33
        movwf   POSTINC0,A
        EXPECT  FSR0L, BUF + 3
        movf    POSTDEC0,w,A        ; reads BUF+3 (0), FSR0 = BUF+2
        movf    POSTDEC0,w,A        ; 0x33, FSR0 = BUF+1
        xorlw   0x33
        EXPECTZ
        movf    PREINC0,w,A         ; FSR0 = BUF+2: 0x33
        xorlw   0x33
        EXPECTZ
        movlw   0xFE                ; PLUSW: W is signed
        movf    PLUSW0,w,A          ; BUF+2-2 = BUF: 0x11
        xorlw   0x11
        EXPECTZ
        EXPECT  FSR0L, BUF + 2
        lfsr    1,BUF
        incf    INDF1,f,A           ; read-modify-write through INDF: 0x12
        EXPECT  BUF, 0x12
        lfsr    2,BUF + 8           ; MOVFF through POSTINC: copy three bytes
        lfsr    1,BUF
        movff   POSTINC1,POSTINC2
        movff   POSTINC1,POSTINC2
        movff   POSTINC1,POSTINC2
        EXPECT  BUF + 10, 0x33
        EXPECT  FSR2L, BUF + 11
        lfsr    0,INDF1             ; INDF0 at INDF1: reads 0, a write does nothing
        movlw   0x5A
        movwf   INDF0,A
        movf    INDF0,w,A
        EXPECTZ
        lfsr    0,0xFFF             ; FSR rolls over at 12 bits
        movf    POSTINC0,w,A
        EXPECT  FSR0H, 0x00
        EXPECT  FSR0L, 0x00
        lfsr    0,WREG              ; a core register through an FSR
        movlw   0x42
        incf    INDF0,f,A           ; WREG = 0x43
        xorlw   0x43
        EXPECTZ

; ---- banks ----
        TEST    6
        movlb   2
        movlw   0x77
        movwf   0x10,BANKED         ; data 0x210
        movwf   0x10,ACCESS         ; data 0x010
        lfsr    0,0x210
        EXPECT  INDF0, 0x77
        clrf    0x10,ACCESS
        EXPECT  INDF0, 0x77
        movlb   0x0F                ; BSR 15: banked operands reach the SFRs
        movlw   0x34
        movwf   0xD9,BANKED         ; FSR2L
        EXPECT  FSR2L, 0x34
        movlw   0x10
        movwf   0xE8,BANKED         ; WREG
        xorlw   0x10
        EXPECTZ
        movlb   0

; ---- table reads ----
        TEST    7
        movlw   upper(tabdata)
        movwf   TBLPTRU,A
        movlw   high(tabdata)
        movwf   TBLPTRH,A
        movlw   low(tabdata)
        movwf   TBLPTRL,A
        tblrd*+
        EXPECT  TABLAT, 0x10
        tblrd*+
        EXPECT  TABLAT, 0x20
        tblrd+*
        EXPECT  TABLAT, 0x40
        tblrd*-
        EXPECT  TABLAT, 0x40
        tblrd*
        EXPECT  TABLAT, 0x30
        EXPECT  TBLPTRL, low(tabdata + 2)

; ---- the stack ----
        TEST    8
        EXPECT  STKPTR, 0
        call    stk1
        bra     stk_done
stk1:   EXPECT  STKPTR, 1
        push                        ; TOS = the next instruction's address
        EXPECT  STKPTR, 2
        movlw   upper(stk_target)
        movwf   TOSU,A
        movlw   high(stk_target)
        movwf   TOSH,A
        movlw   low(stk_target)
        movwf   TOSL,A
        return                      ; to stk_target
        call    fail
stk_target:
        EXPECT  STKPTR, 1
        push
        pop
        EXPECT  STKPTR, 1
        movlw   0x99                ; CALL FAST / RETURN FAST keep W, STATUS, BSR
        movlb   3
        bsf     STATUS,C,A
        call    clobber,FAST
        xorlw   0x99
        EXPECTZ
        movf    BSR,w,A
        xorlw   3
        EXPECTZ
        movlb   0
        return
clobber: movlw  0
        movlb   0
        bcf     STATUS,C,A
        return  FAST
stk_done:
        EXPECT  STKPTR, 0

; ---- interrupts without priorities ----
        TEST    9
        clrf    ICNT,A
        clrf    PCNT,A
        movlw   50
        movwf   TLOAD,A
        movlw   0x05                ; EN | IE, one-shot
        movwf   TCTRL,A
        movlw   0xC0                ; GIE | PEIE
        movwf   INTCON,A
        sleep                       ; until the timer's interrupt
        nop
        EXPECT  ICNT, 1
        btfss   INTCON,7,A          ; RETFIE set GIE again
        call    fail
        bsf     INTCON,5,A          ; TMR0IE, then its flag: INTCON's own pair
        bsf     INTCON,2,A
        nop
        EXPECT  PCNT, 1
        bcf     INTCON,5,A
        clrf    INTCON,A

; ---- interrupts with priorities ----
        TEST    10
        bsf     RCON,7,A            ; IPEN
        clrf    ICNT,A
        clrf    LCNT,A
        clrf    PRE,A
        movlw   0xC0                ; GIEH | GIEL
        movwf   INTCON,A
        movlw   0x02                ; the UART's transmitter-empty interrupt: low priority
        movwf   UIER,A
irq_wait:
        movf    LCNT,w,A
        bz      irq_wait
        EXPECT  PRE, 1              ; the timer preempted the low handler
        EXPECT  ICNT, 1
        clrf    INTCON,A
        bcf     RCON,7,A
        goto    reset_tests

isr_high:
        movlw   0xEE                ; clobber what RETFIE FAST restores
        movlb   7
        movlw   0x01
        movwf   TSTAT,A             ; clear the timer's flag
        btfss   INTCON,2,A
        bra     ih_timer
        bcf     INTCON,2,A          ; INTCON's pair
        incf    PCNT,f,A
        retfie  FAST
ih_timer:
        incf    ICNT,f,A
        movf    INLOW,w,A
        btfss   STATUS,Z,A
        incf    PRE,f,A
        retfie  FAST

isr_low:
        movff   WREG,T2
        clrf    UIER,A              ; the UART's interrupt off
        setf    INLOW,A
        movlw   30                  ; the timer interrupts this handler
        movwf   TLOAD,A
        movlw   0x05
        movwf   TCTRL,A
il_wait: movf   ICNT,w,A
        bz      il_wait
        clrf    INLOW,A
        incf    LCNT,f,A
        movff   T2,WREG
        retfie

; ---- resets ----
reset_tests:
        TEST    11
        movlw   0xA1
        movwf   PHASE,A
        reset
        call    fail
after_reset:
        TEST    11
        btfsc   RCON,4,A            ; RI cleared by RESET
        call    fail
        bsf     RCON,4,A
        movlw   0xA2
        movwf   PHASE,A
        TEST    12
recurse:
        rcall   recurse             ; the 31st push resets the device (STVREN)
after_ovf:
        TEST    12
        btfss   STKPTR,7,A          ; STKFUL
        call    fail
        EXPECT  STKPTR, 0x80
        clrf    STKPTR,A
        clrf    PHASE,A
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
        movlw   0x55
        movwf   TFIN,A
done:   bra     done

; ---- reporting ----
putc:   btfss   ULSR,5,A
        bra     putc
        movwf   UTHR,A
        return
puts:   tblrd*+
        movf    TABLAT,w,A
        btfsc   STATUS,Z,A
        return
        rcall   putc
        bra     puts
puthex: movwf   T2,A
        swapf   T2,w,A
        rcall   nibble
        movf    T2,w,A
nibble: andlw   0x0F
        addlw   0xF6                ; >= 10: C
        btfsc   STATUS,C,A
        addlw   7
        addlw   0x3A
        bra     putc
fail:   clrf    INTCON,A
        movlw   'F'
        rcall   putc
        movlw   ' '
        rcall   putc
        movf    TNUM,w,A
        rcall   puthex
        movlw   '\n'
        rcall   putc
        movff   TNUM,TCODE
fhang:  bra     fhang
vfail:  movlw   'V'
        rcall   putc
        movf    VECH,w,A
        rcall   puthex
        movf    VECL,w,A
        rcall   puthex
        movlw   ' '
        rcall   putc
        bra     fail

banner: db      "pic18 selftest\n",0
tabdata: db     0x10,0x20,0x30,0x40

        org     0x4000
#include "vectors-pic18.inc"
        end
