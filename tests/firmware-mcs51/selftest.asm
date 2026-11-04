; selftest.asm - the 8051 self-test, common part (as31). The Makefile appends the
; model's part (model-*.asm, which defines model_tests) and the generated vectors.
;
; Console: the serial port (SBUF), polled. The finisher is at external data 0xFF00:
; 0x55 there passes, a byte at 0xFF01 fails with that code (the failing test's tag).
; Each test sets R7 to its tag before checking anything; `fail` prints it.

        .equ    FIN, 0xff00
        .equ    STACK, 0x60          ; SP: the stack grows up from 0x61
        .equ    T_FLAGS, 0x30        ; ISR bookkeeping, bit-addressable byte 0x26:
        .equ    ISR_CNT, 0x31        ;   count of interrupts taken
        .equ    ISR_LOG, 0x32        ;   last source and the level seen in it
        .equ    NEST, 0x33

        .org    0x0000
        ljmp    start
        .org    0x0003
        ljmp    isr_ie0
        .org    0x000b
        ljmp    isr_tf0
        .org    0x0013
        ljmp    isr_ie1
        .org    0x001b
        ljmp    isr_tf1
        .org    0x0023
        ljmp    isr_ser

        .org    0x0100
start:  mov     sp, #STACK
        mov     scon, #0x50         ; mode 1, REN
        mov     dptr, #banner
        lcall   puts
        mov     ISR_CNT, #0

        mov     r7, #1
        lcall   t_vectors
        mov     dptr, #s_vectors
        lcall   ok
        mov     r7, #2
        lcall   t_banks
        mov     dptr, #s_banks
        lcall   ok
        mov     r7, #3
        lcall   t_stack
        mov     dptr, #s_stack
        lcall   ok
        mov     r7, #4
        lcall   t_bits
        mov     dptr, #s_bits
        lcall   ok
        mov     r7, #5
        lcall   t_misc
        mov     dptr, #s_misc
        lcall   ok
        mov     r7, #6
        lcall   t_xdata
        mov     dptr, #s_xdata
        lcall   ok
        mov     r7, #7
        lcall   t_timer
        mov     dptr, #s_timer
        lcall   ok
        mov     r7, #8
        lcall   t_irq
        mov     dptr, #s_irq
        lcall   ok
        mov     r7, #9
        lcall   t_prio
        mov     dptr, #s_prio
        lcall   ok
        mov     r7, #10
        lcall   t_idle
        mov     dptr, #s_idle
        lcall   ok
        lcall   model_tests
        mov     dptr, #s_pass
        lcall   puts
        mov     dptr, #FIN
        mov     a, #0x55
        movx    @dptr, a
halt:   sjmp    halt

; ---- console -------------------------------------------------------------------

putc:   mov     sbuf, a
putc_w: jnb     ti, putc_w
        clr     ti
        ret

puts:   clr     a
        movc    a, @a+dptr
        jz      puts_e
        lcall   putc
        inc     dptr
        sjmp    puts
puts_e: ret

puthex: push    acc
        swap    a
        lcall   putnib
        pop     acc
putnib: anl     a, #0x0f
        add     a, #0xf6            ; A - 10; CY if A >= 10
        jnc     putn_d
        add     a, #0x07            ; 'A' comes 7 after '9' + 1
putn_d: add     a, #0x3a            ; + '0' + 10
        ljmp    putc

ok:     lcall   puts
        mov     dptr, #s_ok
        ljmp    puts

; Print "FAIL t=<R7> a=<A>" and end the run with code R7.
fail:   push    acc
        mov     dptr, #s_fail
        lcall   puts
        mov     a, r7
        lcall   puthex
        mov     dptr, #s_a
        lcall   puts
        pop     acc
        lcall   puthex
        mov     a, #10
        lcall   putc
        mov     dptr, #FIN + 1
        mov     a, r7
        movx    @dptr, a
fail_h: sjmp    fail_h

; ---- arithmetic vectors (gen_vectors.py) ---------------------------------------
; The record is copied to 0x40-0x47; results land in 0x50 (PSW), 0x51 (A), 0x52 (B).

t_vectors:
        mov     dptr, #vectab
v_next: clr     a
        movc    a, @a+dptr
        cjne    a, #0xff, v_copy
        ret
v_copy: mov     r0, #0x40
        mov     r6, #8
v_cp:   clr     a
        movc    a, @a+dptr
        mov     @r0, a
        inc     dptr
        inc     r0
        djnz    r6, v_cp
        mov     r2, 0x42
        mov     b, 0x42
        mov     a, 0x40
        cjne    a, #0, v1
        mov     a, 0x41
        mov     psw, 0x43
        add     a, r2
        sjmp    v_chk
v1:     cjne    a, #1, v2
        mov     a, 0x41
        mov     psw, 0x43
        addc    a, r2
        sjmp    v_chk
v2:     cjne    a, #2, v3
        mov     a, 0x41
        mov     psw, 0x43
        subb    a, r2
        sjmp    v_chk
v3:     cjne    a, #3, v4
        mov     a, 0x41
        mov     psw, 0x43
        da      a
        sjmp    v_chk
v4:     cjne    a, #4, v5
        mov     a, 0x41
        mov     psw, 0x43
        mul     ab
        sjmp    v_chk
v5:     cjne    a, #5, v6
        mov     a, 0x41
        mov     psw, 0x43
        div     ab
        sjmp    v_chk
v6:     cjne    a, #6, v7
        mov     a, 0x41
        mov     psw, 0x43
        rlc     a
        sjmp    v_chk
v7:     cjne    a, #7, v8
        mov     a, 0x41
        mov     psw, 0x43
        rrc     a
        sjmp    v_chk
v8:     mov     a, 0x41
        mov     psw, 0x43
        cjne    a, 0x42, v_chk      ; flags only; either way to v_chk
v_chk:  mov     0x50, psw
        mov     0x51, a
        mov     0x52, b
        mov     psw, #0
        mov     a, 0x47
        jnb     acc.0, v_nb_a
        mov     a, 0x51
        cjne    a, 0x44, v_bad
v_nb_a: mov     a, 0x47
        jnb     acc.1, v_nb_b
        mov     a, 0x52
        cjne    a, 0x45, v_bad
v_nb_b: mov     r5, #0xc4
        mov     a, 0x47
        jnb     acc.2, v_np
        mov     r5, #0xc5
v_np:   mov     a, 0x50
        anl     a, r5
        mov     r4, a
        mov     a, 0x46
        anl     a, r5
        xrl     a, r4
        jnz     v_bad
        ljmp    v_next
v_bad:  mov     r0, #0x40           ; print the record and what came out
v_pr:   mov     a, @r0
        lcall   puthex
        mov     a, #32
        lcall   putc
        inc     r0
        cjne    r0, #0x48, v_pr
        mov     a, 0x50
        lcall   puthex
        mov     a, 0x51
        lcall   puthex
        mov     a, 0x52
        ljmp    fail

; ---- register banks ------------------------------------------------------------

t_banks:
        mov     r0, #8              ; fill 0x08-0x1f with i ^ 0xa5 through @R0
tb_f:   mov     a, r0
        xrl     a, #0xa5
        mov     @r0, a
        inc     r0
        cjne    r0, #0x20, tb_f
        mov     0x00, #0x5a         ; bank 0 R0 = 0x5a, R1 too
        mov     0x01, #0xa4
        mov     psw, #0x08          ; bank 1: R0..R7 are 0x08..0x0f
        mov     a, r3
        cjne    a, #0xae, tb_bad
        mov     r5, #0x77
        mov     a, 0x0d
        cjne    a, #0x77, tb_bad
        setb    rs1                 ; bank 3 (RS1 RS0 = 11) by a bit write
        mov     a, r7
        cjne    a, #0xba, tb_bad
        mov     r7, #0x3c           ; R7 of bank 3 is 0x1f
        mov     a, 0x1f
        cjne    a, #0x3c, tb_bad
        clr     rs0                 ; bank 2
        mov     a, r0
        cjne    a, #0xb5, tb_bad
        push    psw                 ; bank 2 saved...
        mov     psw, #0             ; ...bank 0...
        mov     a, r1
        cjne    a, #0xa4, tb_bad0
        pop     psw                 ; ...and back to bank 2
        mov     a, r1
        cjne    a, #0xb4, tb_bad
        mov     psw, #0
        mov     r7, #2
        mov     a, r0
        cjne    a, #0x5a, tb_bad0
        ret
tb_bad: mov     psw, #0
tb_bad0:
        mov     r7, #2
        ljmp    fail

; ---- the stack, indirect addressing, calls ------------------------------------------

t_stack:
        mov     a, sp
        mov     r6, a               ; SP at entry (the return address is above it... below)
        mov     r0, #0x70
        mov     @r0, #0x12
        mov     r1, #0x70
        mov     a, @r1
        cjne    a, #0x12, ts_bad
        inc     @r1
        mov     a, 0x70
        cjne    a, #0x13, ts_bad
        mov     0x71, #0x80
        push    0x71
        mov     a, sp
        dec     a
        xrl     a, r6
        jnz     ts_bad
        mov     r0, sp
        mov     a, @r0
        cjne    a, #0x80, ts_bad
        pop     0x72
        mov     a, 0x72
        cjne    a, #0x80, ts_bad
        lcall   ts_sub              ; the return address: low byte at SP-1, high at SP
ts_ret: cjne    a, #0x5a, ts_bad
        acall   ts_ac
        cjne    a, #0x33, ts_bad
        mov     a, sp
        xrl     a, r6
        jnz     ts_bad
        ret
ts_sub: mov     r0, sp
        mov     a, @r0              ; high byte of ts_ret
        cjne    a, #(ts_ret >> 8), ts_bad2
        dec     r0
        mov     a, @r0
        cjne    a, #(ts_ret & 0xff), ts_bad2
        mov     a, #0x5a
        ret
ts_ac:  mov     a, #0x33
        ret
ts_bad2:
        dec     sp
        dec     sp
ts_bad: ljmp    fail

; ---- bits ---------------------------------------------------------------------------

t_bits:
        mov     0x20, #0
        mov     0x2f, #0
        setb    0x00                ; bit 0 is 0x20.0
        setb    0x7f                ; bit 0x7f is 0x2f.7
        mov     a, 0x20
        cjne    a, #0x01, tbi_bad
        mov     a, 0x2f
        cjne    a, #0x80, tbi_bad
        cpl     0x2f.0
        mov     a, 0x2f
        cjne    a, #0x81, tbi_bad
        clr     0x2f.7
        jb      0x2f.7, tbi_bad
        jnb     0x2f.0, tbi_bad
        jbc     0x2f.0, tbi_j1      ; set: cleared, taken
        sjmp    tbi_bad
tbi_j1: jbc     0x2f.0, tbi_bad     ; now clear: not taken
        mov     a, 0x2f
        jnz     tbi_bad
        mov     a, #0
        setb    acc.6               ; SFR bits: ACC, B
        cjne    a, #0x40, tbi_bad
        mov     b, #0xff
        clr     b.3
        mov     a, b
        cjne    a, #0xf7, tbi_bad
        clr     c
        setb    0xd7                ; CY by bit address
        jnc     tbi_bad
        cpl     c
        jc      tbi_bad
        setb    0x20.1
        orl     c, 0x20.1
        jnc     tbi_bad
        anl     c, /0x20.1
        jc      tbi_bad
        orl     c, /0x20.2
        jnc     tbi_bad
        anl     c, 0x20.2
        jc      tbi_bad
        setb    c
        mov     0x20.5, c
        mov     a, 0x20
        cjne    a, #0x23, tbi_bad
        clr     c
        mov     c, 0x20.5
        jnc     tbi_bad
        mov     p1, #0xff           ; a port: read-modify-write of the latch
        clr     p1.2
        mov     a, p1
        cjne    a, #0xfb, tbi_bad
        mov     a, #0x81            ; P: parity of A in PSW.0
        mov     c, p
        jc      tbi_bad
        mov     a, #0x80
        mov     c, p
        jnc     tbi_bad
        ret
tbi_bad:
        ljmp    fail

; ---- everything else ---------------------------------------------------------------

t_misc:
        mov     a, #0x3c
        mov     0x40, #0xa5
        xch     a, 0x40
        cjne    a, #0xa5, tm_bad
        mov     a, 0x40
        cjne    a, #0x3c, tm_bad
        mov     r0, #0x40
        mov     a, #0x12
        xchd    a, @r0              ; low nibbles swap
        cjne    a, #0x1c, tm_bad
        mov     a, 0x40
        cjne    a, #0x32, tm_bad
        mov     r3, #0x55
        mov     a, #0
        xch     a, r3
        cjne    a, #0x55, tm_bad
        mov     a, #0x96
        swap    a
        cjne    a, #0x69, tm_bad
        rl      a
        cjne    a, #0xd2, tm_bad
        rr      a
        rr      a
        cjne    a, #0xb4, tm_bad
        mov     a, #0xff
        inc     a
        jnz     tm_bad
        dec     a
        cjne    a, #0xff, tm_bad
        mov     0x41, #0xff
        inc     0x41
        mov     a, 0x41
        jnz     tm_bad
        mov     r4, #3
        mov     r5, #0
tm_dj:  inc     r5
        djnz    r4, tm_dj
        cjne    r5, #3, tm_bad
        sjmp    tm_m2
tm_bad: ljmp    fail                ; in the middle: every branch reaches it
tm_m2:
        mov     0x42, #2
tm_dj2: djnz    0x42, tm_dj2
        mov     a, 0x42
        jnz     tm_bad
        mov     r1, #0x43
        mov     @r1, #9
        cjne    @r1, #9, tm_bad
        cjne    @r1, #10, tm_c1     ; 9 < 10: CY
        sjmp    tm_bad
tm_c1:  jnc     tm_bad
        mov     0x44, #7
        mov     0x45, 0x44          ; MOV direct, direct
        mov     a, 0x45
        cjne    a, #7, tm_bad
        mov     0x46, #0x0f
        orl     0x46, #0x30
        anl     0x46, #0x3c
        xrl     0x46, #0x01
        mov     a, 0x46
        cjne    a, #0x3d, tm_bad
        mov     a, #0x40
        orl     0x46, a
        mov     a, 0x46
        cjne    a, #0x7d, tm_bad
        mov     dptr, #0xffff       ; INC DPTR wraps
        inc     dptr
        mov     a, dpl
        orl     a, dph
        jnz     tm_bad
        mov     a, #4               ; MOVC A, @A+PC: PC is the next instruction's
        movc    a, @a+pc            ; address, so 4 is past SJMP's two bytes and two more
        sjmp    tm_pc
        .db     0x11, 0x22, 0x99
tm_pc:  cjne    a, #0x99, tm_bad
        mov     dptr, #tm_jt        ; JMP @A+DPTR
        mov     a, #4
        jmp     @a+dptr
tm_jt:  sjmp    tm_bad
        sjmp    tm_bad
        sjmp    tm_jok
        sjmp    tm_bad
tm_jok: mov     a, #0x80
        jz      tm_bad
        clr     a
        jnz     tm_bad
        nop
        ret

; ---- external data ---------------------------------------------------------------

t_xdata:
        mov     dptr, #0x1234
        mov     a, #0xc3
        movx    @dptr, a
        clr     a
        movx    a, @dptr
        cjne    a, #0xc3, tx_bad
        mov     a, #0x3c
        mov     dptr, #0x12ff
        movx    @dptr, a
        lcall   model_xpage         ; sets the page register to 0x12
        mov     r0, #0x34
        movx    a, @r0              ; external data 0x1234
        cjne    a, #0xc3, tx_bad
        mov     r1, #0xff
        movx    a, @r1
        cjne    a, #0x3c, tx_bad
        mov     a, #0x99
        movx    @r1, a
        mov     dptr, #0x12ff
        movx    a, @dptr
        cjne    a, #0x99, tx_bad
        ret
tx_bad: ljmp    fail

; ---- Timer 0/1 (polled) -------------------------------------------------------------

t_timer:
        mov     tcon, #0
        mov     tmod, #0x21         ; T1 mode 2, T0 mode 1
        mov     th0, #0xff
        mov     tl0, #0xf0
        setb    tr0
tt_w0:  jnb     tf0, tt_w0          ; 16 ticks to overflow
        clr     tr0
        clr     tf0
        mov     a, th0              ; wrapped to 0x00xx
        jnz     tt_bad
        mov     th1, #0xfc          ; mode 2: reloads 0xfc, period 4
        mov     tl1, #0xfc
        setb    tr1
tt_w1:  jnb     tf1, tt_w1
        clr     tf1
tt_w2:  jnb     tf1, tt_w2          ; it overflows again, reloaded
        clr     tr1
        clr     tf1
        mov     a, tl1
        anl     a, #0xfc
        cjne    a, #0xfc, tt_bad
        mov     tmod, #0x03         ; T0 mode 3: TH0 runs on TR1 and sets TF1
        mov     th0, #0xfe
        setb    tr1
tt_w3:  jnb     tf1, tt_w3
        clr     tr1
        clr     tf1
        jb      tf0, tt_bad         ; TL0 is stopped (TR0 clear)
        mov     tmod, #0
        ret
tt_bad: ljmp    fail

; ---- interrupts ---------------------------------------------------------------------

isr_ie0:
isr_ie1:
        reti
isr_tf0:
        push    acc
        clr     tr0
        inc     ISR_CNT
        mov     ISR_LOG, #1
        mov     a, NEST             ; NEST = 1: wait here for the high-priority TF1
        jz      itf0_x
        setb    tr1
itf0_w: mov     a, ISR_LOG
        cjne    a, #3, itf0_w       ; TF1's ISR writes 3
        clr     tr1
itf0_x: pop     acc
        reti
isr_tf1:
        inc     ISR_CNT
        mov     ISR_LOG, #3
        clr     tr1
        reti
isr_ser:
        jnb     ti, iser_x
        clr     ti
        inc     ISR_CNT
        mov     ISR_LOG, #4
iser_x: reti

t_irq:
        mov     tmod, #0x01
        mov     th0, #0xff
        mov     tl0, #0xf8
        mov     ISR_CNT, #0
        mov     NEST, #0
        setb    et0
        setb    ea
        setb    tr0
ti_w:   mov     a, ISR_CNT
        jz      ti_w
        jb      tf0, ti_bad         ; taking the vector cleared TF0
        mov     a, ISR_LOG
        cjne    a, #1, ti_bad
        clr     et0
        mov     ISR_CNT, #0         ; the serial port's TI
        setb    es
        mov     sbuf, #0x2e         ; '.'
ti_w2:  mov     a, ISR_CNT
        jz      ti_w2
        clr     es
        mov     a, ISR_LOG
        cjne    a, #4, ti_bad
        clr     ea
        ret
ti_bad: clr     ea
        ljmp    fail

; Timer 0 at low priority waits in its ISR for Timer 1 at high priority, which
; preempts it.
t_prio:
        mov     tmod, #0x11
        mov     th0, #0xff
        mov     tl0, #0xfc
        mov     th1, #0xff
        mov     tl1, #0xf0
        mov     ISR_CNT, #0
        mov     NEST, #1
        mov     ip, #0x08           ; PT1
        mov     ie, #0x8a           ; EA, ET1, ET0
        setb    tr0
tp_w:   mov     a, ISR_CNT
        cjne    a, #2, tp_w
        mov     ie, #0
        mov     ip, #0
        mov     NEST, #0
        mov     a, ISR_LOG
        cjne    a, #3, tp_bad
        ret
tp_bad: ljmp    fail

; PCON.IDL: the core sleeps until Timer 0's interrupt.
t_idle:
        mov     tmod, #0x01
        mov     th0, #0xfe
        mov     tl0, #0x00
        mov     ISR_CNT, #0
        mov     ie, #0x82
        setb    tr0
        orl     pcon, #0x01
        mov     a, ISR_CNT          ; runs after the ISR
        mov     ie, #0
        cjne    a, #1, tid_bad
        mov     a, pcon
        jb      acc.0, tid_bad      ; IDL reads 0 again
        ret
tid_bad:
        ljmp    fail

; ---- strings -------------------------------------------------------------------------

banner:     .db "8051 self-test", 13, 10, 0
s_vectors:  .db "arith vectors", 0
s_banks:    .db "register banks", 0
s_stack:    .db "stack and calls", 0
s_bits:     .db "bits", 0
s_misc:     .db "misc", 0
s_xdata:    .db "external data", 0
s_timer:    .db "timers 0/1", 0
s_irq:      .db "interrupts", 0
s_prio:     .db "priorities", 0
s_idle:     .db "idle", 0
s_ok:       .db ": ok", 13, 10, 0
s_fail:     .db 13, 10, "FAIL t=", 0
s_a:        .db " a=", 0
s_pass:     .db "PASS", 13, 10, 0
