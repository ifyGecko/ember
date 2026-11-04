; selftest-xinst.asm - PIC18's extended instruction set, for pic-ref -o model=pic18 -o xinst=1.
;
; Indexed literal offset addressing (an access-bank operand below 0x60 is [FSR2 + f] in
; byte and bit instructions, not in MOVFF), ADDFSR/SUBFSR, ADDULNK/SUBULNK, PUSHL, MOVSF,
; MOVSS (an INDF source reads 0) and CALLW. The test's own variables are banked (BSR 0),
; since the access bank's lower half is FSR2-relative here.
        list    p=18f4620
        radix   dec
        errorlevel -302, -1302

ACCESS  equ     0
BANKED  equ     1
PCLATU  equ     0xFFB
PCLATH  equ     0xFFA
FSR0H   equ     0xFEA
FSR0L   equ     0xFE9
WREG    equ     0xFE8
INDF1   equ     0xFE7
FSR2H   equ     0xFDA
FSR2L   equ     0xFD9
STATUS  equ     0xFD8
Z       equ     2
C       equ     0

UTHR    equ     0xF60
ULSR    equ     0xF65
TFIN    equ     0xF68
TCODE   equ     0xF69

TNUM    equ     0x20        ; banked, BSR 0
T0      equ     0x21
T2      equ     0x22

TEST    macro   n
        movlw   n
        movwf   TNUM,BANKED
        endm
EXPECTZ macro
        btfss   STATUS,Z,ACCESS
        call    fail
        endm
; banked register f (BSR 0 or as set) == k
EXPECT  macro   f, k
        movf    f,w,BANKED
        xorlw   k
        EXPECTZ
        endm

        org     0
        goto    start
start:  movlb   0
        TEST    1                   ; indexed literal offset: [FSR2 + f]
        lfsr    2,0x100
        movlw   0x5A
        movwf   [5]                 ; data 0x105
        incf    [5],f
        bsf     [5],7
        movlb   1
        EXPECT  0x05, 0xDB
        movlb   0
        movf    [5],w
        xorlw   0xDB
        EXPECTZ
        movlw   0x11                ; MOVFF is not indexed: 0x005 itself
        movwf   T0,BANKED
        movff   T0,0x005
        EXPECT  0x05, 0x11

        TEST    2                   ; ADDFSR, SUBFSR
        addfsr  2,0x10              ; FSR2 = 0x110
        movf    FSR2L,w,ACCESS
        xorlw   0x10
        EXPECTZ
        subfsr  2,0x20              ; 0x0F0
        movf    FSR2L,w,ACCESS
        xorlw   0xF0
        EXPECTZ
        lfsr    0,0x0FF
        addfsr  0,1                 ; 0x100
        movf    FSR0H,w,ACCESS
        xorlw   1
        EXPECTZ

        TEST    3                   ; PUSHL
        lfsr    2,0x150
        pushl   0x77                ; [0x150] = 0x77, FSR2 = 0x14F
        pushl   0x66                ; [0x14F] = 0x66
        movlb   1
        EXPECT  0x50, 0x77
        EXPECT  0x4F, 0x66
        movlb   0
        movf    FSR2L,w,ACCESS
        xorlw   0x4E
        EXPECTZ

        TEST    4                   ; MOVSF, MOVSS
        lfsr    2,0x140
        movsf   [0x0F],T2           ; [0x14F] = 0x66 -> T2
        EXPECT  T2, 0x66
        movss   [0x10],[0x12]       ; [0x150] -> [0x152]
        movlb   1
        EXPECT  0x52, 0x77
        movlb   0
        lfsr    2,INDF1             ; an INDF source reads 0
        movlw   0x99
        movwf   T2,BANKED
        movsf   [0],T2
        EXPECT  T2, 0

        TEST    5                   ; CALLW, ADDULNK, SUBULNK
        movlw   upper(target)
        movwf   PCLATU,ACCESS
        movlw   high(target)
        movwf   PCLATH,ACCESS
        movlw   low(target)
        callw
        xorlw   0x42
        EXPECTZ
        lfsr    2,0x100
        call    ulnk                ; returns through ADDULNK
        movf    FSR2L,w,ACCESS
        xorlw   0x08
        EXPECTZ
        call    slnk
        movf    FSR2L,w,ACCESS
        xorlw   0x05
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
        movlw   0x55
        movwf   TFIN,ACCESS
done:   bra     done

target: retlw   0x42
ulnk:   addulnk 8
        call    fail
slnk:   subulnk 3
        call    fail

putc:   btfss   ULSR,5,ACCESS
        bra     putc
        movwf   UTHR,ACCESS
        return
fail:   movlw   'F'
        rcall   putc
        movf    TNUM,w,BANKED
        addlw   '0'
        rcall   putc
        movlw   '\n'
        rcall   putc
        movff   TNUM,TCODE
fhang:  bra     fhang
        end
