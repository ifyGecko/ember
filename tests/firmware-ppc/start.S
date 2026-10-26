/*
 * start.S - 32-bit PowerPC exception vectors, reset entry and the trap trampoline
 * for the self-test and the fuzz target. With MSR[IP] = 0 the vectors are at their
 * offsets from 0; each saves r3 in SPRG0, loads its vector offset and joins the
 * common path, which saves every GPR and CR, LR, CTR, XER, SRR0, SRR1, DAR, DSISR
 * to g_frame, calls trap_handler(frame, vector) on its own stack, takes the
 * returned address as SRR0 (and SRR1 from the frame, which the handler may edit),
 * restores the registers and returns with rfi. The handler runs with translation
 * off, so g_frame must be reachable at its physical address. The ELF entry point
 * is _start.
 */
    .section .text.init, "ax"

    .macro VEC off
    .org \off
    mtsprg0 r3
    li      r3, \off
    b       exc_common
    .endm

    VEC 0x100   /* system reset */
    VEC 0x200   /* machine check */
    VEC 0x300   /* DSI */
    VEC 0x400   /* ISI */
    VEC 0x500   /* external */
    VEC 0x600   /* alignment */
    VEC 0x700   /* program */
    VEC 0x800   /* floating point unavailable */
    VEC 0x900   /* decrementer */
    VEC 0xc00   /* system call */
    VEC 0xd00   /* trace */

    .org 0x1000
exc_common:
    mtsprg1 r1
    lis     r1, g_frame@ha
    addi    r1, r1, g_frame@l
    stw     r0, 0(r1)
    mfsprg1 r0
    stw     r0, 4(r1)
    stw     r2, 8(r1)
    mfsprg0 r0
    stw     r0, 12(r1)
    stmw    r4, 16(r1)
    mfcr    r0
    stw     r0, 128(r1)
    mflr    r0
    stw     r0, 132(r1)
    mfctr   r0
    stw     r0, 136(r1)
    mfxer   r0
    stw     r0, 140(r1)
    mfsrr0  r0
    stw     r0, 144(r1)
    mfsrr1  r0
    stw     r0, 148(r1)
    mfdar   r0
    stw     r0, 152(r1)
    mfdsisr r0
    stw     r0, 156(r1)
    stw     r3, 160(r1)

    mr      r4, r3              /* vector */
    mr      r3, r1              /* frame */
    lis     r1, __exc_stack_top@ha
    addi    r1, r1, __exc_stack_top@l
    li      r0, 0
    stwu    r0, -16(r1)
    bl      trap_handler

    lis     r1, g_frame@ha
    addi    r1, r1, g_frame@l
    mtsrr0  r3
    lwz     r0, 148(r1)
    mtsrr1  r0
    lwz     r0, 128(r1)
    mtcr    r0
    lwz     r0, 132(r1)
    mtlr    r0
    lwz     r0, 136(r1)
    mtctr   r0
    lwz     r0, 140(r1)
    mtxer   r0
    lwz     r0, 0(r1)
    lwz     r2, 8(r1)
    lwz     r3, 12(r1)
    lmw     r4, 16(r1)
    lwz     r1, 4(r1)
    rfi

    .org 0x3000
    .globl _start
_start:
    li      r3, 0x3000          /* MSR: FP, ME; IP = 0 (vectors at 0), no translation */
    mtmsr   r3
    isync
    lis     r1, __stack_top@ha
    addi    r1, r1, __stack_top@l
    li      r0, 0
    stwu    r0, -16(r1)

    lis     r3, __bss_start@ha  /* zero .bss */
    addi    r3, r3, __bss_start@l
    lis     r4, __bss_end@ha
    addi    r4, r4, __bss_end@l
1:  cmplw   r3, r4
    bge     2f
    stw     r0, 0(r3)
    addi    r3, r3, 4
    b       1b
2:
    bl      main
    /* main returns the finisher word; write it to the test device, then spin */
    lis     r4, 0xf000
    stw     r3, 0x100(r4)
3:  b       3b

/*
 * enter_user(pc, arg): continue at `pc` in problem state with r3 = arg (MSR as now,
 * plus PR). The trap handler ends the excursion by returning user_resume, which
 * restores the caller's stack pointer and link register and returns from enter_user.
 */
    .text
    .globl enter_user
enter_user:
    lis     r5, g_saved@ha
    addi    r5, r5, g_saved@l
    stw     r1, 0(r5)
    mflr    r0
    stw     r0, 4(r5)
    mtsrr0  r3
    mfmsr   r6
    ori     r6, r6, 0x4000      /* PR */
    mtsrr1  r6
    mr      r3, r4
    rfi

    .globl user_resume
user_resume:
    lis     r5, g_saved@ha
    addi    r5, r5, g_saved@l
    lwz     r1, 0(r5)
    lwz     r0, 4(r5)
    mtlr    r0
    blr

    .section .bss
    .align 4
    .globl g_saved
g_saved:
    .space 8
    .globl g_frame
g_frame:
    .space 176
