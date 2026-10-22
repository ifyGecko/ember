/*
 * smpstart.S - Vectors and reset entry for the multiprocessor self-test
 * (smptest.c), on arm-ref with -o cpus=4. Every processor arrives at _start and
 * takes its stacks by MPIDR.Aff0 (SVC 4 KiB, IRQ 2 KiB); processor 0 clears
 * .bss while the others wait for it, then all call smp_main(Aff0). An IRQ saves
 * the caller-saved registers on the IRQ stack and calls smp_irq(). Always ARM
 * code, whatever the C was compiled to.
 */
    .syntax unified
    .arm

    .section .text.init, "ax"
    .align 5
vectors:
    b       _start
    b       .                       /* undef */
    b       .                       /* svc */
    b       .                       /* prefetch abort */
    b       .                       /* data abort */
    b       .
    b       vec_irq
    b       .                       /* fiq */

vec_irq:
    push    {r0-r3, r12, lr}
    blx     smp_irq
    pop     {r0-r3, r12, lr}
    subs    pc, lr, #4

    .globl _start
    .type _start, %function
_start:
    cpsid   aif
    mrc     p15, 0, r4, c0, c0, 5   /* MPIDR */
    and     r4, r4, #0xff
    cmp     r4, #4
    bhs     park                    /* stacks for processors 0..3 only */
    cps     #0x12                   /* irq */
    ldr     sp, =__stack_irq
    sub     sp, sp, r4, lsl #11
    cps     #0x13                   /* svc */
    ldr     sp, =__stack_top
    sub     sp, sp, r4, lsl #12
    ldr     r0, =vectors
    mcr     p15, 0, r0, c12, c0, 0  /* VBAR */
    isb
    cmp     r4, #0
    bne     wait_bss

    ldr     r0, =__bss_start
    ldr     r1, =__bss_end
    mov     r2, #0
1:  cmp     r0, r1
    strlo   r2, [r0], #4
    blo     1b
    dmb
    ldr     r0, =bss_ready
    mov     r1, #1
    str     r1, [r0]
    b       go

wait_bss:
    ldr     r0, =bss_ready
2:  ldr     r1, [r0]
    cmp     r1, #0
    beq     2b
    dmb
go:
    mov     r0, r4
    blx     smp_main
park:
    wfi
    b       park

    .data
    .align 2
bss_ready:
    .word   0
