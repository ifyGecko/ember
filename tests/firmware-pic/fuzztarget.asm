; fuzztarget.asm - A fuzz target with planted bugs for pic-ref (PIC18), the assembly
; counterpart of tests/fuzz/fuzztarget.h: every byte the UART holds is one input; "FUZ!"
; stores to nothing (data 0xA00: a PIC has no exceptions, the harness sees a store fault),
; "FUZ?" calls fuzz_panic, an input starting with 'L' never ends.
        list    p=18f4620
        radix   dec
        errorlevel -302, -1302

BANKED  equ     1
TABLAT  equ     0xFF5
TBLPTRU equ     0xFF8
TBLPTRH equ     0xFF7
TBLPTRL equ     0xFF6
POSTINC0 equ    0xFEE
INDF0   equ     0xFEF
FSR0L   equ     0xFE9
STATUS  equ     0xFD8
Z       equ     2

URBR    equ     0xF60
UTHR    equ     0xF60
ULSR    equ     0xF65

N       equ     0x00        ; bytes in the input
SUM     equ     0x01
SINK    equ     0x02
BUF     equ     0x100       ; 256 bytes

        org     0
        goto    main

main:   movlw   upper(ready)
        movwf   TBLPTRU,A
        movlw   high(ready)
        movwf   TBLPTRH,A
        movlw   low(ready)
        movwf   TBLPTRL,A
m_puts: tblrd*+
        movf    TABLAT,w,A
        bz      m_loop
m_wait: btfss   ULSR,5,A
        bra     m_wait
        movwf   UTHR,A
        bra     m_puts
m_loop: call    fuzz_iteration
        bra     m_loop

; one iteration: read the input, parse it
fuzz_iteration:
        lfsr    0,BUF
        clrf    N,A
fi_read: btfss  ULSR,0,A            ; data ready
        bra     parse
        movf    URBR,w,A
        movwf   POSTINC0,A
        incfsz  N,f,A               ; at most 256 bytes
        bra     fi_read
        decf    N,f,A
parse:  clrf    SUM,A               ; sum = sum * 31 + p[i]
        lfsr    0,BUF
        movf    N,w,A
        bz      p_check
        movwf   SINK,A
p_sum:  movf    SUM,w,A
        mullw   31
        movf    POSTINC0,w,A
        addwf   PRODL,w,A
        movwf   SUM,A
        decfsz  SINK,f,A
        bra     p_sum
p_check: lfsr   0,BUF
        movlw   'L'                 ; planted bug: an endless loop
        cpfseq  INDF0,A
        bra     p_fuz
        movf    N,w,A
        bz      p_fuz
p_spin: incf    SINK,f,A
        bra     p_spin
p_fuz:  movlw   4
        cpfslt  N,A                 ; n >= 4
        bra     p_f1
        return
p_f1:   movlw   'F'
        cpfseq  POSTINC0,A
        return
        movlw   'U'
        cpfseq  POSTINC0,A
        return
        movlw   'Z'
        cpfseq  POSTINC0,A
        return
        movlw   '!'
        cpfseq  INDF0,A
        bra     p_q
        movf    SUM,w,A             ; planted bug: a store to nothing
        movlb   0x0A
        movwf   0x00,BANKED
        movlb   0
        return
p_q:    movlw   '?'
        cpfseq  INDF0,A
        return
        call    fuzz_panic          ; planted bug: an assertion
        return

; the firmware's fatal-error routine
fuzz_panic:
        incf    SINK,f,A
        bra     fuzz_panic

PRODL   equ     0xFF3
ready:  db      "fuzztarget: ready\n",0
        end
