/*
 * start-booke.S - Book E boot page, interrupt vectors and trap trampoline for the
 * self-test (selftest.c built with -DPPC_BOOKE).
 *
 * The processor starts at 0xfffffffc in the boot page, the one page its reset TLB
 * entry maps. The boot code maps RAM (TLB1 entry 1: 0, 256 MiB, every permission),
 * the devices (entry 2: 0xf0000000, 16 MiB, supervisor, I G) and a region nothing
 * answers in (entry 3: 0xe0000000, for the machine-check test), all protected, and
 * goes to _start in RAM, which points IVPR and IVOR0-15 at the vectors below: IVOR n
 * at 0x100 * (n + 1). Each vector saves r3 in SPRG0, loads its IVOR number and joins
 * the common path, which saves the GPRs, CR, LR, CTR, XER, the interrupted pc and MSR
 * from the save pair of the interrupt's class (SRR, CSRR for critical input,
 * watchdog and debug, MCSRR for machine check), DEAR and ESR to g_frame, calls
 * trap_handler(frame, ivor) and returns through the same class.
 */
    .section .text.init, "ax"

    .macro VEC n
    .org 0x100 * (\n + 1)
    mtsprg0 r3
    li      r3, \n
    b       exc_common
    .endm

    VEC 0
    VEC 1
    VEC 2
    VEC 3
    VEC 4
    VEC 5
    VEC 6
    VEC 7
    VEC 8
    VEC 9
    VEC 10
    VEC 11
    VEC 12
    VEC 13
    VEC 14
    VEC 15

    .macro RESTORE
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
    .endm

    .org 0x1100
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
    cmpwi   r3, 1
    beq     1f
    cmpwi   r3, 0
    beq     2f
    cmpwi   r3, 12
    beq     2f
    cmpwi   r3, 15
    beq     2f
    mfsrr0  r4
    mfsrr1  r5
    b       3f
1:  mfspr   r4, 570             /* MCSRR0/1 */
    mfspr   r5, 571
    b       3f
2:  mfspr   r4, 58              /* CSRR0/1 */
    mfspr   r5, 59
3:  stw     r4, 144(r1)
    stw     r5, 148(r1)
    mfspr   r0, 61              /* DEAR */
    stw     r0, 152(r1)
    mfspr   r0, 62              /* ESR */
    stw     r0, 156(r1)
    stw     r3, 160(r1)

    mr      r4, r3
    mr      r3, r1
    lis     r1, __exc_stack_top@ha
    addi    r1, r1, __exc_stack_top@l
    li      r0, 0
    stwu    r0, -16(r1)
    bl      trap_handler

    lis     r1, g_frame@ha
    addi    r1, r1, g_frame@l
    lwz     r4, 160(r1)
    lwz     r0, 148(r1)
    cmpwi   r4, 1
    beq     4f
    cmpwi   r4, 0
    beq     5f
    cmpwi   r4, 12
    beq     5f
    cmpwi   r4, 15
    beq     5f
    mtsrr0  r3
    mtsrr1  r0
    RESTORE
    rfi
4:  mtspr   570, r3
    mtspr   571, r0
    RESTORE
    rfmci
5:  mtspr   58, r3
    mtspr   59, r0
    RESTORE
    rfci

    .org 0x3000
    .globl _start
_start:
    li      r3, 0x3000          /* MSR: FP, ME */
    mtmsr   r3
    isync
    li      r3, 0
    mtspr   63, r3              /* IVPR = 0 */
    .irp n, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
    li      r3, 0x100 * (\n + 1)
    mtspr   400 + \n, r3
    .endr
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
    lis     r4, 0xf000
    stw     r3, 0x100(r4)
3:  b       3b

/* enter_user(pc, arg): as start.S */
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

/* The boot page. */
    .section .boot, "ax"
    .macro TLB1 esel, mas1, mas2, mas3
    lis     r3, 0x1000 | \esel
    mtspr   624, r3             /* MAS0: TLB1, ESEL */
    lis     r3, (\mas1) >> 16
    ori     r3, r3, (\mas1) & 0xffff
    mtspr   625, r3
    lis     r3, (\mas2) >> 16
    ori     r3, r3, (\mas2) & 0xffff
    mtspr   626, r3
    lis     r3, (\mas3) >> 16
    ori     r3, r3, (\mas3) & 0xffff
    mtspr   627, r3
    li      r3, 0
    mtspr   944, r3             /* MAS7 */
    tlbwe
    .endm

_boot:
    TLB1 1, 0xc0000900, 0x00000000, 0x0000003f    /* V IPROT 256 MiB: RAM */
    TLB1 2, 0xc0000700, 0xf000000a, 0xf0000005    /* 16 MiB devices: I G, SR SW */
    TLB1 3, 0xc0000700, 0xe000000a, 0xe0000005    /* 16 MiB of nothing */
    isync
    lis     r3, _start@ha
    addi    r3, r3, _start@l
    mtctr   r3
    bctr

    .org 0xffc
    .globl _reset
_reset:
    b       _boot

    .section .bss
    .align 4
    .globl g_saved
g_saved:
    .space 8
    .globl g_frame
g_frame:
    .space 176
