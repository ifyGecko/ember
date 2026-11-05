/*
 * start.S - Start code and vectors for C programs on xtensa-ref (the fuzz target, the
 * benchmark). One source for both ABIs: the windowed ABI (LX6, LX7) gets the window
 * overflow and underflow handlers of the ISA manual (4.7.1.6); the CALL0 ABI (LX106) has
 * no windows. _start sets up the stack and PS, clears .bss, enables the FPU if there is
 * one, calls main and hands its return value to the test finisher (0x5555 passes). Any
 * exception or interrupt calls trap_handler(frame), frame pointing at EPC1, EXCCAUSE and
 * EXCVADDR; if it returns, its value goes to the finisher.
 */
#define FINISHER 0x60003000
#define PS_UM   0x20
#define PS_WOE  0x40000

    .section .vectors, "ax"
    .literal_position
#if defined(__XTENSA_WINDOWED_ABI__)
/* VECBASE + 0x000 ... 0x3C0: the LX6/LX7 layout */
#include "window.inc"
    .org 0x180
    j    _trap          /* level 2 */
    .org 0x1C0
    j    _trap          /* level 3 */
    .org 0x200
    j    _trap          /* level 4 */
    .org 0x240
    j    _trap          /* level 5 */
    .org 0x280
    j    _trap          /* level 6: debug */
    .org 0x2C0
    j    _trap          /* level 7: NMI */
    .org 0x300
    j    _trap          /* kernel */
    .org 0x340
    j    _trap          /* user */
    .org 0x3C0
    j    _trap          /* double */
    .org 0x400          /* the reset vector */
#else
/* VECBASE + 0x10 ... 0x70: the LX106 layout */
    .org 0x10
    j    _trap          /* level 2: debug */
    .org 0x20
    j    _trap          /* level 3: NMI */
    .org 0x30
    j    _trap          /* kernel */
    .org 0x50
    j    _trap          /* user */
    .org 0x70
    j    _trap          /* double */
    .org 0x80           /* the reset vector */
#endif
    .global _start
_start:
    j    _start1

    .text
    .literal_position
    .align 4
_start1:
    movi a0, 0
#if defined(__XTENSA_WINDOWED_ABI__)
    movi a2, 1
    wsr  a2, windowstart
    wsr  a0, windowbase
    rsync
#endif
    movi a1, __stack_top
    movi a2, __bss_start
    movi a3, __bss_end
1:  bgeu a2, a3, 2f
    s32i a0, a2, 0
    addi a2, a2, 4
    j    1b
2:
#if defined(__XTENSA_WINDOWED_ABI__)
    movi a2, PS_WOE | PS_UM
#else
    movi a2, PS_UM
#endif
    wsr  a2, ps
    rsync
#if __XCHAL_HAVE_FP
    movi a2, 1
    wsr  a2, cpenable
    rsync
#endif
#if defined(__XTENSA_WINDOWED_ABI__)
    call4 main
    mov  a2, a6
#else
    call0 main
#endif
_finish:
    movi a3, FINISHER
    s32i a2, a3, 0
3:  j    3b

/* An exception or interrupt: trap_handler(&{EPC1, EXCCAUSE, EXCVADDR}) on its own stack,
 * with window overflow detection back on so that C code can run. */
    .align 4
_trap:
    movi a1, __exc_stack_top
    addi a1, a1, -32
    rsr  a2, epc1
    s32i a2, a1, 16
    rsr  a2, exccause
    s32i a2, a1, 20
    rsr  a2, excvaddr
    s32i a2, a1, 24
#if defined(__XTENSA_WINDOWED_ABI__)
    movi a2, 0
    wsr  a2, windowbase
    movi a2, 1
    wsr  a2, windowstart
    rsync
    movi a1, __exc_stack_top
    addi a1, a1, -32
    movi a2, PS_WOE | PS_UM | 15
    wsr  a2, ps
    rsync
    addi a6, a1, 16
    call4 trap_handler
    mov  a2, a6
#else
    movi a2, PS_UM | 15
    wsr  a2, ps
    rsync
    addi a2, a1, 16
    call0 trap_handler
#endif
    j    _finish
