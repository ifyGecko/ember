/*
 * smpstart.S - Vectors and reset entry for the multiprocessor self-test
 * (smptest.c). Every processor arrives at _start, turns the exception prefix off
 * (vectors at 0) and takes a 4 KiB stack by its number in PIR; processor 0 clears
 * .bss while the others wait for it, then all call smp_main(PIR). The external
 * interrupt and decrementer vectors save the registers a C call may clobber on
 * the interrupted stack and call smp_trap(vector).
 */
    .section .text.init, "ax"

    .macro VEC off
    .org \off
    stwu    r1, -96(r1)
    stw     r3, 12(r1)
    li      r3, \off
    b       irq_common
    .endm

    VEC 0x500   /* external */
    VEC 0x900   /* decrementer */

    .org 0x1000
irq_common:
    stw     r0, 8(r1)
    stw     r4, 16(r1)
    stw     r5, 20(r1)
    stw     r6, 24(r1)
    stw     r7, 28(r1)
    stw     r8, 32(r1)
    stw     r9, 36(r1)
    stw     r10, 40(r1)
    stw     r11, 44(r1)
    stw     r12, 48(r1)
    mfcr    r0
    stw     r0, 52(r1)
    mflr    r0
    stw     r0, 56(r1)
    mfctr   r0
    stw     r0, 60(r1)
    mfxer   r0
    stw     r0, 64(r1)
    mfsrr0  r0
    stw     r0, 68(r1)
    mfsrr1  r0
    stw     r0, 72(r1)
    bl      smp_trap
    lwz     r0, 72(r1)
    mtsrr1  r0
    lwz     r0, 68(r1)
    mtsrr0  r0
    lwz     r0, 64(r1)
    mtxer   r0
    lwz     r0, 60(r1)
    mtctr   r0
    lwz     r0, 56(r1)
    mtlr    r0
    lwz     r0, 52(r1)
    mtcr    r0
    lwz     r0, 8(r1)
    lwz     r3, 12(r1)
    lwz     r4, 16(r1)
    lwz     r5, 20(r1)
    lwz     r6, 24(r1)
    lwz     r7, 28(r1)
    lwz     r8, 32(r1)
    lwz     r9, 36(r1)
    lwz     r10, 40(r1)
    lwz     r11, 44(r1)
    lwz     r12, 48(r1)
    addi    r1, r1, 96
    rfi

    .org 0x3000
    .globl _start
_start:
    li      r3, 0x3000          /* MSR: FP, ME; vectors at 0 */
    mtmsr   r3
    isync
    mfspr   r3, 1023            /* PIR */
    cmplwi  r3, 4
    bge     park                /* stacks for processors 0..3 only */
    lis     r1, __stack_top@ha
    addi    r1, r1, __stack_top@l
    slwi    r4, r3, 12
    subf    r1, r4, r1
    li      r0, 0
    stwu    r0, -16(r1)
    cmpwi   r3, 0
    bne     wait_bss

    lis     r4, __bss_start@ha
    addi    r4, r4, __bss_start@l
    lis     r5, __bss_end@ha
    addi    r5, r5, __bss_end@l
1:  cmplw   r4, r5
    bge     2f
    stw     r0, 0(r4)
    addi    r4, r4, 4
    b       1b
2:  sync
    lis     r4, bss_ready@ha
    li      r5, 1
    stw     r5, bss_ready@l(r4)
    b       go

wait_bss:
    lis     r4, bss_ready@ha
3:  lwz     r5, bss_ready@l(r4)
    cmpwi   r5, 0
    beq     3b
    isync
go:
    mfspr   r3, 1023
    bl      smp_main
park:
    lis     r3, 0x0004          /* MSR[POW]: sleep (interrupts off: for good) */
    ori     r3, r3, 0x3000
    mtmsr   r3
    b       park

    .data
    .align 2
bss_ready:
    .long   0
