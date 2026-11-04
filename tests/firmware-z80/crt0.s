; Start code for SDCC's z80 port on z80-ref (the fuzz target and tests/bench): the stack
; below 0xFF00 (the fuzz check runs with --ram 0xFF00, leaving the planted bad store nothing
; to reach), initialized data copied, BSS cleared, main called; main's return value
; (sdcccall(1): in DE) is handed to the test finisher (0x5555: pass, else fail).
	.module	crt0
	.optsdcc -mz80 sdcccall(1)
	.globl	_main
	.globl	l__DATA, s__DATA, l__INITIALIZER, s__INITIALIZED, s__INITIALIZER

STACK	= 0xFF00
TEST	= 0xF0                  ; the test finisher's port

	.area	_HEADER (ABS)
	.org	0
	di
	ld	sp,#STACK
	call	gsinit
	call	_main
	ld	a,e
	cp	#0x55
	jr	nz,1$
	ld	a,d
	cp	#0x55
	jr	nz,1$
	out	(TEST),a                ; 0x55 at offset 0: pass
	halt
1$:	ld	a,#1
	out	(TEST+1),a              ; a code at offset 1: fail
	halt

	.org	0x38                    ; IM 1 (unused)
	ei
	reti
	.org	0x66                    ; NMI (unused)
	retn

	.area	_HOME
	.area	_CODE
	.area	_INITIALIZER
	.area	_GSINIT
	.area	_GSFINAL
	.area	_DATA
	.area	_INITIALIZED
	.area	_BSEG
	.area	_BSS
	.area	_HEAP

	.area	_GSINIT
gsinit::
	ld	bc,#l__DATA
	ld	a,b
	or	a,c
	jr	z,2$
	ld	hl,#s__DATA
	ld	(hl),#0x00
	dec	bc
	ld	a,b
	or	a,c
	jr	z,2$
	ld	e,l
	ld	d,h
	inc	de
	ldir
2$:	ld	bc,#l__INITIALIZER
	ld	a,b
	or	a,c
	jr	z,3$
	ld	de,#s__INITIALIZED
	ld	hl,#s__INITIALIZER
	ldir
3$:
	.area	_GSFINAL
	ret
