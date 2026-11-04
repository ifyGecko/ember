; Z80 self-test for the z80-ref board (boards/z80_ref.c), sdasz80 syntax.
;
; Three tables of vectors (gen_vectors.py: ALU, INC/DEC, DAA, rotates, CPL/SCF/CCF/NEG,
; every CB operation, ADD/ADC/SBC HL, the ED block instructions), each run by a harness
; that patches the instruction into its code; then checks of what tables do not reach:
; loads and the index registers (IXH/IXL, (IX+d) beside H and L, DDCB copies, prefix
; chains), the stack and control flow, I/O, R, I and IFF2, Q (SCF/CCF), MEMPTR (BIT
; n,(HL)), RLD/RRD and the repeating block instructions, and interrupts (IM 1 and IM 2
; from the CTC, the instruction after EI, HALT). A failed check prints the address of
; its call and fails the run; a failed vector prints the vector and what it got.
;
; Memory: code and tables from 0, buffers at 0xF100-0xF4FF, the stack below 0x10000.
; model-*.s sets OUTC0, the byte OUT (C),0 writes on that model.

UART    = 0x00
SCR     = 0x07          ; the 16550's scratch register: an I/O byte that reads back
CTC0    = 0x10
TEST    = 0xF0
BUF     = 0xF100
DST     = 0xF200
IXBUF   = 0xF300
IM2TAB  = 0xF400

	.area	_HEADER (ABS)
	.org	0
	jp	start

	.org	0x08            ; RST 08h..30h: record which one ran
	ld	a,#0x08
	jr	rst_rec
	.org	0x10
	ld	a,#0x10
	jr	rst_rec
	.org	0x18
	ld	a,#0x18
	jr	rst_rec
	.org	0x20
	ld	a,#0x20
	jr	rst_rec
	.org	0x28
	ld	a,#0x28
	jr	rst_rec
	.org	0x30
	ld	a,#0x30
	jr	rst_rec
	.org	0x38            ; IM 1 (and RST 38h)
	jp	isr1
rst_rec:
	ld	(rst_seen),a
	ret

	.org	0x66
	retn

	.org	0x100
start:
	ld	sp,#0x0000
	ld	de,#s_hello
	call	puts
	call	t_basic
	call	t_vec_op
	call	t_vec_16
	call	t_vec_blk
	call	t_index
	call	t_flow
	call	t_io
	call	t_r
	call	t_q
	call	t_memptr
	call	t_misc
	call	t_irq
	ld	de,#s_pass
	call	puts
	ld	a,#0x55
	out	(TEST),a
	halt

; ---- reporting ---------------------------------------------------------------------

putc:	out	(UART),a
	ret
puts:	ld	a,(de)
	or	a
	ret	z
	out	(UART),a
	inc	de
	jr	puts
puthex:	push	af
	rrca
	rrca
	rrca
	rrca
	call	1$
	pop	af
1$:	and	#0x0F
	add	a,#0x30
	cp	#0x3A
	jr	c,2$
	add	a,#7
2$:	out	(UART),a
	ret
nl:	ld	a,#10
	out	(UART),a
	ret

; A check failed: report the CALL (call nz,fail) and stop.
fail:	pop	hl
	dec	hl
	dec	hl
	dec	hl
	ld	de,#s_fail
	call	puts
	ld	a,h
	call	puthex
	ld	a,l
	call	puthex
	call	nl
	ld	a,#1
	out	(TEST+1),a
	halt

; A vector failed: print its B bytes at IX and the C bytes got at HL, stop with code A.
vfail:	push	af
	ld	de,#s_vfail
	call	puts
1$:	ld	a,0(ix)
	call	puthex
	ld	a,#0x20
	call	putc
	inc	ix
	djnz	1$
	ld	de,#s_got
	call	puts
2$:	ld	a,(hl)
	call	puthex
	ld	a,#0x20
	call	putc
	inc	hl
	dec	c
	jr	nz,2$
	call	nl
	pop	af
	out	(TEST+1),a
	halt

; ---- basic sanity: what the harnesses rely on ----------------------------------------

t_basic:
	ld	a,#0x5A
	cp	#0x5A
	call	nz,fail
	ld	b,a
	ld	c,b
	ld	d,c
	ld	e,d
	ld	h,e
	ld	l,h
	ld	a,l
	cp	#0x5A
	call	nz,fail
	ld	hl,#0x1234
	push	hl
	pop	de
	ld	a,d
	cp	#0x12
	call	nz,fail
	ld	a,e
	cp	#0x34
	call	nz,fail
	ld	ix,#0x0100
	ld	bc,#0x0023
	add	ix,bc
	push	ix
	pop	hl
	ld	de,#0x0123
	or	a
	sbc	hl,de
	call	nz,fail
	ret

; ---- vec_op: op1 op2 a f b -> a f b ---------------------------------------------------

t_vec_op:
	ld	ix,#vec_op
	ld	de,#vec_op_n
1$:	ld	a,0(ix)
	ld	(op_patch),a
	ld	a,1(ix)
	ld	(op_patch+1),a
	call	op_run
	ld	(got),hl
	ld	a,b
	ld	(got+2),a
	ld	a,h
	cp	5(ix)
	jr	nz,2$
	ld	a,l
	cp	6(ix)
	jr	nz,2$
	ld	a,b
	cp	7(ix)
	jr	nz,2$
	ld	bc,#8
	add	ix,bc
	dec	de
	ld	a,d
	or	e
	jr	nz,1$
	ret
2$:	ld	b,#8
	ld	c,#3
	ld	hl,#got
	ld	a,#2
	jp	vfail
op_run:	ld	b,4(ix)
	ld	h,2(ix)
	ld	l,3(ix)
	push	hl
	pop	af
op_patch:
	.db	0, 0
	push	af
	pop	hl
	ret

; ---- vec_16: op1 op2 hl bc f -> hl f -----------------------------------------------------

t_vec_16:
	ld	ix,#vec_16
	ld	de,#vec_16_n
1$:	ld	a,0(ix)
	ld	(p16),a
	ld	a,1(ix)
	ld	(p16+1),a
	push	de
	call	run16
	ld	(got),hl
	ld	a,e
	ld	(got+2),a
	pop	de
	ld	a,(got)
	cp	7(ix)
	jr	nz,2$
	ld	a,(got+1)
	cp	8(ix)
	jr	nz,2$
	ld	a,(got+2)
	cp	9(ix)
	jr	nz,2$
	ld	bc,#10
	add	ix,bc
	dec	de
	ld	a,d
	or	e
	jr	nz,1$
	ret
2$:	ld	b,#10
	ld	c,#3
	ld	hl,#got
	ld	a,#3
	jp	vfail
run16:	ld	h,#0
	ld	l,6(ix)
	push	hl
	pop	af
	ld	l,2(ix)
	ld	h,3(ix)
	ld	c,4(ix)
	ld	b,5(ix)
p16:	.db	0, 0
	push	af
	pop	de
	ret

; ---- vec_blk: op a f v c b l -> f b c ----------------------------------------------------

t_vec_blk:
	ld	ix,#vec_blk
	ld	de,#vec_blk_n
1$:	ld	a,0(ix)
	ld	(pblk+1),a
	push	de
	call	runblk
	ld	a,l
	ld	(got),a
	ld	(got+1),bc
	pop	de
	ld	a,(got)
	cp	7(ix)
	jr	nz,2$
	ld	a,b
	cp	8(ix)
	jr	nz,2$
	ld	a,c
	cp	9(ix)
	jr	nz,2$
	ld	bc,#10
	add	ix,bc
	dec	de
	ld	a,d
	or	e
	jr	nz,1$
	ret
2$:	ld	b,#10
	ld	c,#3
	ld	hl,#got
	ld	a,#4
	jp	vfail
runblk:	ld	h,#>BUF
	ld	l,6(ix)
	ld	a,3(ix)
	ld	(hl),a
	out	(SCR),a
	ld	de,#DST
	ld	c,4(ix)
	ld	b,5(ix)
	push	hl
	ld	h,1(ix)
	ld	l,2(ix)
	push	hl
	pop	af
	pop	hl
pblk:	.db	0xED, 0
	push	af
	pop	hl
	ret

; ---- the index registers --------------------------------------------------------------

t_index:
	ld	ix,#IXBUF
	ld	iy,#IXBUF+0x80
	ld	a,#0x12
	ld	5(ix),a
	ld	b,5(ix)
	ld	a,b
	cp	#0x12
	call	nz,fail
	ld	-1(iy),#0x34
	ld	c,-1(iy)
	ld	a,c
	cp	#0x34
	call	nz,fail
	ld	a,(IXBUF+0x7F)
	cp	#0x34
	call	nz,fail
	; ALU and INC on (IX+d)
	ld	a,#0x10
	add	a,5(ix)                 ; 0x22
	cp	#0x22
	call	nz,fail
	inc	-1(iy)                  ; 0x35
	ld	a,-1(iy)
	cp	#0x35
	call	nz,fail
	; IXH/IXL (undocumented): ld ixh,n / ld a,ixh / add a,ixl / inc iyl
	.db	0xDD, 0x26, 0x9A        ; ld ixh,#0x9A
	.db	0xDD, 0x7C              ; ld a,ixh
	cp	#0x9A
	call	nz,fail
	.db	0xDD, 0x2E, 0x05        ; ld ixl,#5
	ld	a,#0x10
	.db	0xDD, 0x85              ; add a,ixl
	cp	#0x15
	call	nz,fail
	push	ix
	pop	hl
	ld	de,#0x9A05
	or	a
	sbc	hl,de
	call	nz,fail
	ld	iy,#0x10FF
	.db	0xFD, 0x2C              ; inc iyl -> 0x00, Z
	call	nz,fail
	.db	0xFD, 0x7C              ; ld a,iyh
	cp	#0x10
	call	nz,fail
	; Beside (IX+d), H and L are H and L.
	ld	ix,#IXBUF
	ld	5(ix),#0x77
	ld	hl,#0x0000
	.db	0xDD, 0x66, 0x05        ; ld h,(ix+5)
	ld	a,h
	cp	#0x77
	call	nz,fail
	push	ix
	pop	de
	ld	a,d
	cp	#>IXBUF
	call	nz,fail
	; LD IXH,IXL; LD B,IXL
	.db	0xDD, 0x65              ; ld ixh,ixl
	.db	0xDD, 0x44              ; ld b,ixh
	ld	a,b
	cp	#<IXBUF
	call	nz,fail
	; DDCB with a register copy: rlc (ix+2),b ; set 0,(ix+3),a ; bit with (IX+d)
	ld	ix,#IXBUF
	ld	2(ix),#0x81
	.db	0xDD, 0xCB, 0x02, 0x00  ; rlc (ix+2),b
	ld	a,b
	cp	#0x03
	call	nz,fail
	ld	a,2(ix)
	cp	#0x03
	call	nz,fail
	ld	3(ix),#0x40
	.db	0xDD, 0xCB, 0x03, 0xC7  ; set 0,(ix+3),a
	cp	#0x41
	call	nz,fail
	ld	a,3(ix)
	cp	#0x41
	call	nz,fail
	.db	0xDD, 0xCB, 0x03, 0x30  ; sll (ix+3),b -> 0x83
	ld	a,b
	cp	#0x83
	call	nz,fail
	; Prefix chains: the last prefix counts; a prefix before ED does nothing.
	ld	ix,#0x1111
	.db	0xDD, 0xFD, 0x21, 0x34, 0x12   ; ld iy,#0x1234
	push	iy
	pop	hl
	ld	de,#0x1234
	or	a
	sbc	hl,de
	call	nz,fail
	push	ix
	pop	hl
	ld	de,#0x1111
	or	a
	sbc	hl,de
	call	nz,fail
	ld	a,#1
	.db	0xDD, 0xED, 0x44        ; neg
	cp	#0xFF
	call	nz,fail
	ld	de,#0x1234
	ld	hl,#0x5678
	.db	0xDD, 0xEB              ; ex de,hl: no index form
	ld	a,d
	cp	#0x56
	call	nz,fail
	ret

; ---- stack and control flow ------------------------------------------------------------

t_flow:
	ld	a,#0
	ld	(rst_seen),a
	rst	0x28
	ld	a,(rst_seen)
	cp	#0x28
	call	nz,fail
	rst	0x10
	ld	a,(rst_seen)
	cp	#0x10
	call	nz,fail
	; CALL cc / RET cc
	xor	a                       ; Z, P/V (even), NC, P
	call	nz,fail
	call	c,fail
	call	po,fail
	call	m,fail
	ld	b,#0
	call	z,1$
	call	nc,1$
	call	pe,1$
	call	p,1$
	ld	a,b
	cp	#4
	call	nz,fail
	ld	a,#0x80
	or	a                       ; NZ, M, PO (one bit)
	call	z,fail
	call	p,fail
	call	pe,fail
	call	2$
	jr	3$
1$:	push	af                      ; keep the flags the next CALL cc tests
	inc	b
	pop	af
	ret
2$:	ret	z                       ; not taken
	ret	m                       ; taken
	jp	fail
3$:	; DJNZ, JR cc, JP cc, JP (HL), JP (IX)
	ld	b,#5
	ld	c,#0
4$:	inc	c
	djnz	4$
	ld	a,c
	cp	#5
	call	nz,fail
	scf
	jr	nc,5$
	jr	c,6$
5$:	call	fail
6$:	or	a
	jp	c,fail
	ld	hl,#7$
	jp	(hl)
	call	fail
7$:	ld	ix,#8$
	jp	(ix)
	call	fail
8$:	; EX (SP),HL / EX (SP),IX, EXX, EX AF,AF'
	ld	hl,#0xAAAA
	push	hl
	ld	hl,#0x1234
	ex	(sp),hl
	ld	de,#0xAAAA
	or	a
	sbc	hl,de
	call	nz,fail
	ld	ix,#0x5555
	ex	(sp),ix
	push	ix
	pop	hl
	ld	de,#0x1234
	or	a
	sbc	hl,de
	call	nz,fail
	pop	hl
	ld	de,#0x5555
	or	a
	sbc	hl,de
	call	nz,fail
	ld	bc,#0x1111
	ld	de,#0x2222
	ld	hl,#0x3333
	exx
	ld	bc,#0x4444
	ld	de,#0x5555
	ld	hl,#0x6666
	exx
	ld	a,b
	cp	#0x11
	call	nz,fail
	ld	a,h
	cp	#0x33
	call	nz,fail
	exx
	ld	a,d
	cp	#0x55
	call	nz,fail
	exx
	ld	a,#0x12
	.db	0x08                    ; ex af,af'
	ld	a,#0x34
	.db	0x08
	cp	#0x12
	call	nz,fail
	; PUSH/POP AF keeps every flag bit
	ld	hl,#0x00FF
	push	hl
	pop	af
	push	af
	pop	de
	ld	a,e
	cp	#0xFF
	call	nz,fail
	ret

; ---- I/O ------------------------------------------------------------------------------------

t_io:
	ld	a,#0x96
	out	(SCR),a
	xor	a
	in	a,(SCR)
	cp	#0x96
	call	nz,fail
	ld	bc,#0x1200 + SCR
	ld	a,#0x80
	out	(c),a
	in	d,(c)                   ; S, P/V odd -> PO, NZ
	call	z,fail
	call	p,fail
	call	pe,fail
	ld	a,d
	cp	#0x80
	call	nz,fail
	ld	a,#0x00
	out	(c),a
	.db	0xED, 0x70              ; in f,(c): Z, PE
	call	nz,fail
	call	po,fail
	ld	a,#0x5A
	out	(c),a
	.db	0xED, 0x71              ; out (c),0
	in	a,(c)
	cp	#OUTC0
	call	nz,fail
	; OUT (n),A puts A on the high byte; the board decodes the low one.
	ld	a,#0x3C
	out	(SCR),a
	in	a,(SCR)
	cp	#0x3C
	call	nz,fail
	ret

; ---- R and I --------------------------------------------------------------------------------

t_r:	ld	a,#0x80
	ld	r,a
	ld	a,r                     ; R: two fetches since LD R,A
	cp	#0x82
	call	nz,fail
	nop                             ; with CP and CALL: four fetches
	nop
	ld	a,r
	cp	#0x88
	call	nz,fail
	ld	a,#0x7F
	ld	r,a
	ld	a,r                     ; bits 0-6 wrap, bit 7 stays
	cp	#0x01
	call	nz,fail
	ld	a,#0
	ld	r,a
	.db	0xDD, 0x00              ; a prefixed NOP: two fetches
	ld	a,r
	cp	#4
	call	nz,fail
	ld	a,#0
	ld	r,a
	.db	0xDD, 0xDD, 0x00        ; a lone prefix, then a prefixed NOP: three
	ld	a,r
	cp	#5
	call	nz,fail
	ld	a,#0
	ld	r,a
	.db	0xDD, 0xCB, 0x00, 0x46  ; bit 0,(ix+0): two
	ld	a,r
	cp	#4
	call	nz,fail
	; LD A,I: P/V is IFF2.
	di
	ld	a,#0
	ld	i,a
	ld	a,i
	call	nz,fail
	call	pe,fail
	ei
	ld	a,#0x21
	ld	i,a
	ld	a,i
	call	po,fail
	call	z,fail
	di
	cp	#0x21
	call	nz,fail
	ret

; ---- Q: SCF and CCF -------------------------------------------------------------------------

t_q:	xor	a                       ; sets the flags: Q = F
	scf                             ; bits 3/5 = A = 0
	push	af
	pop	hl
	ld	a,l
	cp	#0x45
	call	nz,fail
	ld	hl,#0x0028              ; POP AF does not set the flags: Q = 0
	push	hl
	pop	af
	scf                             ; bits 3/5 = (F | A) = 0x28
	push	af
	pop	hl
	ld	a,l
	cp	#0x29
	call	nz,fail
	ld	hl,#0x0029
	push	hl
	pop	af
	scf                             ; Q = 0: 0x29
	scf                             ; Q = F: bits 3/5 = A = 0
	push	af
	pop	hl
	ld	a,l
	cp	#0x01
	call	nz,fail
	ld	hl,#0x2800
	push	hl
	pop	af
	ccf                             ; Q = 0: (F | A) = 0x28; C 0 -> 1, H 0
	push	af
	pop	hl
	ld	a,l
	cp	#0x29
	call	nz,fail
	ret

; ---- MEMPTR, through BIT n,(HL) ---------------------------------------------------------------

t_memptr:
	ld	hl,#BUF
	ld	(hl),#0
	ld	a,(0x28FF)              ; MEMPTR = 0x2900
	bit	0,(hl)
	push	af
	pop	de
	ld	a,e
	and	#0x28
	cp	#0x28
	call	nz,fail
	ld	a,(0x0000)              ; MEMPTR = 0x0001
	bit	0,(hl)
	push	af
	pop	de
	ld	a,e
	and	#0x28
	call	nz,fail
	ld	de,#0x0800              ; ADD HL,DE: MEMPTR = HL + 1
	ld	hl,#0x1FFF
	add	hl,de
	ld	hl,#BUF
	bit	0,(hl)                  ; 0x2000 -> 0x20
	push	af
	pop	de
	ld	a,e
	and	#0x28
	cp	#0x20
	call	nz,fail
	ld	ix,#0x0800              ; BIT n,(IX+d): from IX + d = 0x0805 -> 0x08
	.db	0xDD, 0xCB, 0x05, 0x46
	push	af
	pop	de
	ld	a,e
	and	#0x28
	cp	#0x08
	call	nz,fail
	ld	a,#0x28                 ; OUT (n),A: MEMPTR = A:(n + 1)
	out	(SCR),a
	ld	hl,#BUF
	bit	0,(hl)
	push	af
	pop	de
	ld	a,e
	and	#0x28
	cp	#0x28
	call	nz,fail
	jp	9$                      ; JP nn: MEMPTR = nn
9$:	ld	hl,#BUF
	bit	0,(hl)
	push	af
	pop	de
	ld	a,e
	and	#0x28
	ld	b,a
	ld	a,#>9$
	and	#0x28
	cp	b
	call	nz,fail
	ret

; ---- RLD/RRD, block instructions that repeat, the rest --------------------------------------

t_misc:
	ld	hl,#BUF
	ld	(hl),#0x34
	ld	a,#0x12
	rld                             ; (HL) = 0x42, A = 0x13
	cp	#0x13
	call	nz,fail
	ld	a,(hl)
	cp	#0x42
	call	nz,fail
	ld	a,#0x56
	rrd                             ; (HL) = 0x64, A = 0x52
	cp	#0x52
	call	nz,fail
	ld	a,(hl)
	cp	#0x64
	call	nz,fail
	; LDIR / LDDR copy, CPIR finds
	ld	hl,#s_hello
	ld	de,#DST
	ld	bc,#8
	ldir
	ld	a,b
	or	c
	call	nz,fail
	ld	hl,#DST
	ld	de,#s_hello
	ld	b,#8
1$:	ld	a,(de)
	cp	(hl)
	call	nz,fail
	inc	hl
	inc	de
	djnz	1$
	ld	hl,#DST+7
	ld	de,#DST+0x17
	ld	bc,#8
	lddr
	ld	a,(DST+0x10)
	ld	b,a
	ld	a,(DST)
	cp	b
	call	nz,fail
	ld	hl,#DST
	ld	bc,#8
	ld	a,(DST+5)
	cpir
	call	nz,fail                 ; found
	ld	a,c
	cp	#2                      ; after 6 of 8
	call	nz,fail
	; OTIR / INIR through the scratch register
	ld	hl,#DST
	ld	bc,#0x0400 + SCR
	otir
	ld	a,b
	or	a
	call	nz,fail
	in	a,(SCR)
	ld	b,a
	ld	a,(DST+3)
	cp	b
	call	nz,fail
	ld	hl,#BUF
	ld	bc,#0x0300 + SCR
	inir                            ; the same byte three times
	in	a,(SCR)
	ld	b,a
	ld	a,(BUF+2)
	cp	b
	call	nz,fail
	; LD SP,HL; INC/DEC rp; ED loads
	ld	(sp_save),sp
	ld	hl,#0xE000
	ld	sp,hl
	push	hl
	pop	de
	ld	sp,(sp_save)
	ld	a,d
	cp	#0xE0
	call	nz,fail
	ld	bc,#0xFFFF
	inc	bc
	ld	a,b
	or	c
	call	nz,fail
	ld	de,#0xBEEF
	ld	(BUF),de
	ld	hl,(BUF)
	ld	a,h
	cp	#0xBE
	call	nz,fail
	; undefined ED opcodes do nothing
	ld	a,#0x77
	.db	0xED, 0x00, 0xED, 0xFF, 0xED, 0x77
	cp	#0x77
	call	nz,fail
	ret

; ---- interrupts -------------------------------------------------------------------------------

isr1:	push	af
	ld	a,(icount)
	inc	a
	ld	(icount),a
	ld	a,(seen_b)              ; B at the first interrupt after seen_b = 0xFF
	inc	a
	jr	nz,1$
	ld	a,b
	ld	(seen_b),a
1$:	pop	af
	ei
	reti

isr2:	push	af
	ld	a,(icount2)
	inc	a
	ld	(icount2),a
	pop	af
	ei
	reti

t_irq:
	di
	xor	a
	ld	(icount),a
	ld	(icount2),a
	im	1
	ld	a,#0x85                 ; ch 0: interrupt, timer /16, time constant follows
	out	(CTC0),a
	ld	a,#4
	out	(CTC0),a
	ei
1$:	halt
	ld	a,(icount)
	cp	#3
	jr	c,1$
	di
	ld	a,#0x03                 ; reset ch 0
	out	(CTC0),a
	; The instruction after EI runs before the interrupt.
	ld	a,#0x85
	out	(CTC0),a
	ld	a,#1
	out	(CTC0),a
	ld	b,#40
2$:	djnz	2$                      ; the CTC asks meanwhile
	ld	a,#0xFF
	ld	(seen_b),a
	ld	b,#0
	ei
	inc	b
	inc	b
	inc	b
	di
	ld	a,(seen_b)
	cp	#1
	call	nz,fail
	ld	a,#0x03
	out	(CTC0),a
	; IM 2: the CTC's vector through the table at I.
	ld	hl,#isr2
	ld	(IM2TAB+0x10),hl
	ld	a,#>IM2TAB
	ld	i,a
	im	2
	ld	a,#0x10                 ; vector base 0x10
	out	(CTC0),a
	ld	a,#0xA5                 ; interrupt, timer /256, time constant follows
	out	(CTC0),a
	ld	a,#1
	out	(CTC0),a
	ei
3$:	halt
	ld	a,(icount2)
	cp	#2
	jr	c,3$
	di
	ld	a,#0x03
	out	(CTC0),a
	im	1
	ret

s_hello: .ascii	"z80 self-test"
	.db	10, 0
s_pass:	.ascii	"z80 self-test: pass"
	.db	10, 0
s_fail:	.asciz	"FAIL at "
s_vfail: .asciz	"FAIL vector "
s_got:	.asciz	" got "

got:	.ds	4
rst_seen: .ds	1
icount:	.ds	1
icount2: .ds	1
seen_b:	.ds	1
sp_save: .ds	2
