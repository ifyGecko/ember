/*
 * start.S - ARMv7-A vectors, reset entry and the trap trampoline for the self-test
 * and the fuzz target (arm-ref). Always ARM code, whatever the C was compiled to.
 *
 * Every exception saves r0-r12 and its LR on its mode's stack, below them the SPSR,
 * and calls trap_handler(kind, frame) with kind = the vector number (1 undef, 2 svc,
 * 3 prefetch abort, 4 data abort, 6 irq, 7 fiq) and frame = { spsr, r0..r12, lr }.
 * The handler returns the address to resume at; it may edit the frame (the SPSR
 * included), which is restored by an exception return (LDM with ^ and the pc).
 * enter_user/user_resume and call_guarded/guard_resume give tests a way back
 * from User mode and from calls that fault.
 */
    .syntax unified
    .arm

    .section .text.init, "ax"
    .align 5
    .globl vectors
vectors:
    b       _start
    b       vec_undef
    b       vec_svc
    b       vec_pabt
    b       vec_dabt
    b       .
    b       vec_irq
    b       vec_fiq

vec_undef: push {r0-r12, lr}; mov r0, #1; b trap_common
vec_svc:   push {r0-r12, lr}; mov r0, #2; b trap_common
vec_pabt:  push {r0-r12, lr}; mov r0, #3; b trap_common
vec_dabt:  push {r0-r12, lr}; mov r0, #4; b trap_common
vec_irq:   push {r0-r12, lr}; mov r0, #6; b trap_common
vec_fiq:   push {r0-r12, lr}; mov r0, #7; b trap_common

trap_common:
    mrs     r1, spsr
    push    {r1}
    mov     r1, sp
    mov     r4, sp                  /* keep the frame; realign the stack for C */
    bic     sp, sp, #7
    blx     trap_handler
    mov     sp, r4
    str     r0, [sp, #56]           /* resume address into the lr slot */
    pop     {r1}
    msr     spsr_cxsf, r1
    ldm     sp!, {r0-r12, pc}^

    .globl _start
    .type _start, %function
_start:
    cpsid   aif
    cps     #0x1b                   /* und */
    ldr     sp, =__stack_und
    cps     #0x17                   /* abt */
    ldr     sp, =__stack_abt
    cps     #0x12                   /* irq */
    ldr     sp, =__stack_irq
    cps     #0x11                   /* fiq */
    ldr     sp, =__stack_fiq
    cps     #0x1f                   /* sys: the User-mode stack */
    ldr     sp, =__stack_usr
    cps     #0x13                   /* svc */
    ldr     sp, =__stack_top
    ldr     r0, =vectors
    mcr     p15, 0, r0, c12, c0, 0  /* VBAR */
    isb

    ldr     r0, =__bss_start
    ldr     r1, =__bss_end
    mov     r2, #0
1:  cmp     r0, r1
    strlo   r2, [r0], #4
    blo     1b

    blx     main
    ldr     r1, =0x1c001000         /* main's return value to the test finisher */
    str     r0, [r1]
2:  b       2b

/*
 * enter_user(pc, arg): drop to User mode at pc (bit 0 = Thumb) with r0 = arg. The
 * trap handler ends the excursion by resuming at user_resume in SVC mode, which
 * restores the caller's sp and lr and returns from enter_user.
 */
    .text
    .globl enter_user
    .type enter_user, %function
enter_user:
    ldr     r2, =g_saved
    str     sp, [r2]
    str     lr, [r2, #4]
    mov     lr, r0
    mov     r0, r1
    mov     r3, #0x10               /* User mode, interrupts enabled */
    tst     lr, #1
    orrne   r3, r3, #0x20           /* T */
    msr     spsr_cxsf, r3
    movs    pc, lr

    .globl user_resume
    .type user_resume, %function
user_resume:
    ldr     r2, =g_saved
    ldr     sp, [r2]
    ldr     lr, [r2, #4]
    bx      lr

/*
 * call_guarded(fn): call fn (bit 0 = Thumb) and return its result, or -1 if the
 * trap handler gives up on an exception inside it and resumes at guard_resume.
 */
    .globl call_guarded
    .type call_guarded, %function
call_guarded:
    ldr     r2, =g_guard
    str     sp, [r2]
    str     lr, [r2, #4]
    mov     r1, #1
    str     r1, [r2, #8]
    blx     r0
    ldr     r2, =g_guard
    mov     r1, #0
    str     r1, [r2, #8]
    ldr     lr, [r2, #4]
    bx      lr

    .globl guard_resume
    .type guard_resume, %function
guard_resume:
    ldr     r2, =g_guard
    mov     r1, #0
    str     r1, [r2, #8]
    ldr     sp, [r2]
    ldr     lr, [r2, #4]
    mvn     r0, #0
    bx      lr

    .pool

    .section .bss
    .align 3
    .globl g_saved
g_saved:
    .space 8
    .globl g_guard
g_guard:
    .space 12               /* sp, lr, active */
