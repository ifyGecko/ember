/*
 * smpstart.S - Vectors and reset entry for the multiprocessor self-test
 * (smptest.c). Every processor arrives at _start, sets EBase (0x80100000) and
 * takes a 4 KiB stack by EBase.CPUNum; processor 0 clears .bss while the others
 * wait for it, then all call smp_main(CPUNum). Interrupts come to the general
 * vector, which saves the registers a C call may clobber on the interrupted
 * stack and calls smp_trap().
 */
    .set noreorder
    .set noat

    .section .text.init, "ax"
    .org 0x180
exc_vector:
    addiu   $sp, $sp, -88
    sw      $1,  0($sp)
    sw      $2,  4($sp)
    sw      $3,  8($sp)
    sw      $4,  12($sp)
    sw      $5,  16($sp)
    sw      $6,  20($sp)
    sw      $7,  24($sp)
    sw      $8,  28($sp)
    sw      $9,  32($sp)
    sw      $10, 36($sp)
    sw      $11, 40($sp)
    sw      $12, 44($sp)
    sw      $13, 48($sp)
    sw      $14, 52($sp)
    sw      $15, 56($sp)
    sw      $24, 60($sp)
    sw      $25, 64($sp)
    sw      $31, 68($sp)
    mfhi    $8
    sw      $8,  72($sp)
    mflo    $8
    sw      $8,  76($sp)
    jal     smp_trap
    addiu   $sp, $sp, -16           /* (delay slot) the o32 argument area */
    addiu   $sp, $sp, 16
    lw      $8,  72($sp)
    mthi    $8
    lw      $8,  76($sp)
    mtlo    $8
    lw      $1,  0($sp)
    lw      $2,  4($sp)
    lw      $3,  8($sp)
    lw      $4,  12($sp)
    lw      $5,  16($sp)
    lw      $6,  20($sp)
    lw      $7,  24($sp)
    lw      $8,  28($sp)
    lw      $9,  32($sp)
    lw      $10, 36($sp)
    lw      $11, 40($sp)
    lw      $12, 44($sp)
    lw      $13, 48($sp)
    lw      $14, 52($sp)
    lw      $15, 56($sp)
    lw      $24, 60($sp)
    lw      $25, 64($sp)
    lw      $31, 68($sp)
    addiu   $sp, $sp, 88
    eret
    nop

    .globl _start
_start:
    mtc0    $zero, $12              /* Status = 0: kernel, BEV off, interrupts off */
    ehb
    la      $t0, 0x80100000
    mtc0    $t0, $15, 1             /* EBase */
    ehb
    mtc0    $zero, $13              /* Cause = 0 */
    ehb
    mfc0    $a0, $15, 1
    andi    $a0, $a0, 0x3ff         /* CPUNum */
    sltiu   $t0, $a0, 4
    beq     $t0, $zero, park        /* stacks for processors 0..3 only */
    nop
    la      $sp, __stack_top
    sll     $t0, $a0, 12
    subu    $sp, $sp, $t0
    bne     $a0, $zero, wait_bss
    nop

    la      $t0, __bss_start
    la      $t1, __bss_end
1:  sltu    $t2, $t0, $t1
    beq     $t2, $zero, 2f
    nop
    sw      $zero, 0($t0)
    b       1b
    addiu   $t0, $t0, 4
2:  sync
    la      $t0, bss_ready
    li      $t1, 1
    b       go
    sw      $t1, 0($t0)

wait_bss:
    la      $t0, bss_ready
3:  lw      $t1, 0($t0)
    beq     $t1, $zero, 3b
    nop
    sync
go:
    mfc0    $a0, $15, 1
    andi    $a0, $a0, 0x3ff
    jal     smp_main
    addiu   $sp, $sp, -16           /* (delay slot) the o32 argument area */
park:
    wait
    b       park
    nop

    .data
    .align 2
bss_ready:
    .word   0
