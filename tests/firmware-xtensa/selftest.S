/*
 * selftest.S - Xtensa self-test for xtensa-ref / xtensaeb-ref, one source for the three
 * models (model.h): the generated computational vectors (vectors.S, from gen_vectors.py)
 * and hand-written tests of everything else: loads and stores, branches, calls, register
 * windows (overflow and underflow through the manual's handlers, ROTW, MOVSP), zero-overhead
 * loops (including a loop end inside code translated before the loop existed), booleans,
 * MAC16, the FPU and its coprocessor-disabled exception, S32C1I, exceptions (syscall,
 * illegal, divide by zero, alignment, region protection, a double exception, BREAK),
 * interrupts (software at level 1 and 3, CCOMPARE, WAITI, RSIL masking, an edge
 * interrupt and the NMI from the board's timers) and special registers.
 *
 * Registers: a14 holds the number of the test running, a15 is the checks' scratch, a1
 * the stack; tests are functions called with CALL0 (every core has it). A failure prints
 * the test number, the value and the value expected, and fails through the finisher
 * with the test number.
 */
#include "model.h"

/* rec: what the handlers saw */
#define R_COUNT   0
#define R_CAUSE   4
#define R_EPC     8
#define R_VADDR   12
#define R_RESUME  16
#define R_INT     20
#define R_SA3     24
#define R_SA4     28
#define R_HCOUNT  32
#define R_HLEVEL  36
#define R_HEPC    40
#define R_HEPS    44
#define R_HA3     48
#define R_HA4     52
#define R_DCOUNT  56
#define R_DEPC    60
#define R_DCAUSE  64
#define R_BCOUNT  68
#define R_BCAUSE  72
#define R_BEPC    76

/* ---- macros ------------------------------------------------------------------------ */

    .macro CHECK reg, exp
    movi a15, \exp
    beq  \reg, a15, .Lc\@
    mov  a2, \reg
    mov  a3, a15
    call0 fail
.Lc\@:
    .endm

    .macro TEST n
    movi a14, \n
    .endm

    .macro ENTER_FN
    addi a1, a1, -16
    s32i a0, a1, 0
    .endm

    .macro LEAVE_FN
    l32i a0, a1, 0
    addi a1, a1, 16
    ret
    .endm

    .macro T_RRR op, a, b, exp
    addi a14, a14, 1
    movi a3, \a
    movi a4, \b
    \op  a5, a3, a4
    CHECK a5, \exp
    .endm
    .macro T_NEG a, exp
    addi a14, a14, 1
    movi a3, \a
    neg  a5, a3
    CHECK a5, \exp
    .endm
    .macro T_ABS a, exp
    addi a14, a14, 1
    movi a3, \a
    abs  a5, a3
    CHECK a5, \exp
    .endm
    .macro T_NSA a, exp
    addi a14, a14, 1
    movi a3, \a
    nsa  a5, a3
    CHECK a5, \exp
    .endm
    .macro T_NSAU a, exp
    addi a14, a14, 1
    movi a3, \a
    nsau a5, a3
    CHECK a5, \exp
    .endm
    .macro T_RRI op, a, imm, exp
    addi a14, a14, 1
    movi a3, \a
    \op  a5, a3, \imm
    CHECK a5, \exp
    .endm
    .macro T_EXTUI a, sa, bits, exp
    addi a14, a14, 1
    movi a3, \a
    extui a5, a3, \sa, \bits
    CHECK a5, \exp
    .endm
    .macro T_SRC setup, n, a, b, exp
    addi a14, a14, 1
    movi a3, \a
    movi a4, \b
    movi a6, \n
    \setup a6
    src  a5, a3, a4
    CHECK a5, \exp
    .endm
    .macro T_SRL setup, n, b, exp
    addi a14, a14, 1
    movi a4, \b
    movi a6, \n
    \setup a6
    srl  a5, a4
    CHECK a5, \exp
    .endm
    .macro T_SRA setup, n, b, exp
    addi a14, a14, 1
    movi a4, \b
    movi a6, \n
    \setup a6
    sra  a5, a4
    CHECK a5, \exp
    .endm
    .macro T_SLL setup, n, a, exp
    addi a14, a14, 1
    movi a3, \a
    movi a6, \n
    \setup a6
    sll  a5, a3
    CHECK a5, \exp
    .endm

    /* Expect an exception of `cause`, resuming at `resume`. */
    .macro ARM_EXC resume
    movi a2, rec
    movi a3, \resume
    s32i a3, a2, R_RESUME
    movi a3, 0
    s32i a3, a2, R_COUNT
    s32i a3, a2, R_DCOUNT
    s32i a3, a2, R_BCOUNT
    s32i a3, a2, R_HCOUNT
    .endm

    .macro CHECK_REC off, exp
    movi a2, rec
    l32i a2, a2, \off
    CHECK a2, \exp
    .endm


/* bit numbers: a big-endian core counts from the most significant bit */
#ifdef SELFTEST_BE
#define BIT(n) (31 - (n))
#else
#define BIT(n) (n)
#endif
#define BIT_THRE BIT(5)

/* ---- vectors ----------------------------------------------------------------------- */

    .macro HIGH_HANDLER lv
high\lv:
    movi a2, rec
    s32i a3, a2, R_HA3
    s32i a4, a2, R_HA4
    movi a3, \lv
    s32i a3, a2, R_HLEVEL
    rsr  a3, epc\lv
    s32i a3, a2, R_HEPC
    rsr  a3, eps\lv
    s32i a3, a2, R_HEPS
    l32i a3, a2, R_HCOUNT
    addi a3, a3, 1
    s32i a3, a2, R_HCOUNT
    rsr  a3, interrupt
    rsr  a4, intenable
    and  a3, a3, a4
    xor  a4, a4, a3
    wsr  a4, intenable
    movi a3, TIMER1         /* the NMI's source: acknowledge it */
    movi a4, 1
    s32i a4, a3, 4
    l32i a3, a2, R_HA3
    l32i a4, a2, R_HA4
    rsr  a2, excsave\lv
    rfi  \lv
    .endm

    .macro DEBUG_HANDLER lv
debugh:
    movi a2, rec
    s32i a3, a2, R_HA3
    rsr  a3, debugcause
    s32i a3, a2, R_BCAUSE
    rsr  a3, epc\lv
    s32i a3, a2, R_BEPC
    l32i a3, a2, R_BCOUNT
    addi a3, a3, 1
    s32i a3, a2, R_BCOUNT
    l32i a3, a2, R_RESUME
    wsr  a3, epc\lv
    l32i a3, a2, R_HA3
    rsr  a2, excsave\lv
    rfi  \lv
    .endm

    .section .vectors, "ax"
    .literal_position
#if XT_HAVE_WINDOWED
#include "window.inc"
    .org 0x180
    wsr  a2, excsave2
    j    high2
    .org 0x1C0
    wsr  a2, excsave3
    j    high3
    .org 0x200
    wsr  a2, excsave4
    j    high4
    .org 0x240
    wsr  a2, excsave5
    j    high5
    .org 0x280
    wsr  a2, excsave6
    j    debugh
    .org 0x2C0
    wsr  a2, excsave7
    j    high7
    .org 0x300
    wsr  a2, excsave1
    j    exc_common
    .org 0x340
    wsr  a2, excsave1
    j    exc_common
    .org 0x3C0
    wsr  a2, excsave1
    j    double_h
    .org 0x400
#else
    .org 0x10
    wsr  a2, excsave2
    j    debugh
    .org 0x20
    wsr  a2, excsave3
    j    high3
    .org 0x30
    wsr  a2, excsave1
    j    exc_common
    .org 0x50
    wsr  a2, excsave1
    j    exc_common
    .org 0x70
    wsr  a2, excsave1
    j    double_h
    .org 0x80
#endif
    .global _start
_start:
    j    start1

    .text
    .literal_position
    .align 4
#if XT_HAVE_WINDOWED
    HIGH_HANDLER 2
    HIGH_HANDLER 3
    HIGH_HANDLER 4
    HIGH_HANDLER 5
    HIGH_HANDLER 7
    DEBUG_HANDLER 6
#else
    HIGH_HANDLER 3
    DEBUG_HANDLER 2
#endif

/* General exceptions and level-1 interrupts: record, then resume at R_RESUME (an
 * exception) or where the interrupt came (a level-1 interrupt, its source masked). */
exc_common:
    movi a2, rec
    s32i a3, a2, R_SA3
    s32i a4, a2, R_SA4
    rsr  a3, exccause
    s32i a3, a2, R_CAUSE
    rsr  a3, epc1
    s32i a3, a2, R_EPC
    rsr  a3, excvaddr
    s32i a3, a2, R_VADDR
    l32i a4, a2, R_COUNT
    addi a4, a4, 1
    s32i a4, a2, R_COUNT
    rsr  a3, exccause
    movi a4, 4
    bne  a3, a4, 1f
    rsr  a3, interrupt
    rsr  a4, intenable
    and  a3, a3, a4
    s32i a3, a2, R_INT
    xor  a4, a4, a3
    wsr  a4, intenable
    j    2f
1:  l32i a3, a2, R_RESUME
    beqz a3, unexpected
    wsr  a3, epc1
2:  l32i a3, a2, R_SA3
    l32i a4, a2, R_SA4
    rsr  a2, excsave1
    rfe
unexpected:
    rsr  a2, exccause
    movi a3, 0xEEEE
    call0 fail

double_h:
    movi a2, rec
    s32i a3, a2, R_SA3
    rsr  a3, depc
    s32i a3, a2, R_DEPC
    rsr  a3, exccause
    s32i a3, a2, R_DCAUSE
    l32i a3, a2, R_DCOUNT
    addi a3, a3, 1
    s32i a3, a2, R_DCOUNT
    l32i a3, a2, R_RESUME
    beqz a3, unexpected_double
    wsr  a3, depc
    l32i a3, a2, R_SA3
    rsr  a2, excsave1
    rfde
unexpected_double:
    movi a14, 0xDDDD
    rsr  a2, exccause
    rsr  a3, depc
    call0 fail

/* ---- console and verdict ------------------------------------------------------------- */

/* putc(a2); clobbers a3, a4 */
putc:
    movi a3, UART
1:  l8ui a4, a3, 5
    bbci a4, BIT_THRE, 1b
    s8i  a2, a3, 0
    ret

/* puts(a2); clobbers a2-a6 */
puts:
    mov  a5, a0
    mov  a6, a2
1:  l8ui a2, a6, 0
    beqz a2, 2f
    call0 putc
    addi a6, a6, 1
    j    1b
2:  mov  a0, a5
    ret

/* puthex(a2); clobbers a2-a8 */
puthex:
    mov  a7, a0
    mov  a6, a2
    movi a8, 28
1:  ssr  a8
    srl  a2, a6
    extui a2, a2, 0, 4
    movi a3, hexdig
    add  a3, a3, a2
    l8ui a2, a3, 0
    call0 putc
    addi a8, a8, -4
    bgez a8, 1b
    mov  a0, a7
    ret

/* fail(a2 got, a3 expected): never returns */
fail:
    mov  a9, a2
    mov  a10, a3
    movi a2, msg_fail
    call0 puts
    mov  a2, a14
    call0 puthex
    movi a2, msg_got
    call0 puts
    mov  a2, a9
    call0 puthex
    movi a2, msg_exp
    call0 puts
    mov  a2, a10
    call0 puthex
    movi a2, '\n'
    call0 putc
    slli a2, a14, 16
    movi a3, 0x3333
    or   a2, a2, a3
    movi a3, FINISHER
    s32i a2, a3, 0
1:  j    1b

/* ---- start ------------------------------------------------------------------------- */

start1:
    movi a0, 0
#if XT_HAVE_WINDOWED
    movi a2, 1
    wsr  a2, windowstart
    wsr  a0, windowbase
    rsync
#endif
    movi a1, stack_top
    movi a2, PS_RUN
    wsr  a2, ps
    rsync
    movi a14, 0
    call0 t_loadstore
    call0 t_branch
    call0 t_calls
    call0 run_vectors
#if XT_HAVE_WINDOWED
    call0 t_windows
#endif
#if XT_HAVE_LOOPS
    call0 t_loops
#endif
#if XT_HAVE_BOOLEANS
    call0 t_booleans
#endif
#if XT_HAVE_MAC16
    call0 t_mac16
#endif
#if XT_HAVE_FP
    call0 t_fp
#endif
#if XT_HAVE_S32C1I
    call0 t_atomic
#endif
    call0 t_exceptions
    call0 t_interrupts
    call0 t_sregs
    movi a2, msg_ok
    call0 puts
    movi a2, FINISHER
    movi a3, 0x5555
    s32i a3, a2, 0
1:  j    1b

/* ---- loads and stores ---------------------------------------------------------------- */

t_loadstore:
    ENTER_FN
    TEST 1
    movi a2, buf
    movi a3, 0x80FF7F01
    s32i a3, a2, 0
    l32i a4, a2, 0
    CHECK a4, 0x80FF7F01
    TEST 2
    l8ui a4, a2, 0
#ifdef SELFTEST_BE
    CHECK a4, 0x80
#else
    CHECK a4, 0x01
#endif
    TEST 3
    l16si a4, a2, 2
#ifdef SELFTEST_BE
    CHECK a4, 0x7F01
#else
    CHECK a4, 0xFFFF80FF
#endif
    TEST 4
    l16ui a4, a2, 2
#ifdef SELFTEST_BE
    CHECK a4, 0x7F01
#else
    CHECK a4, 0x80FF
#endif
    TEST 5
    movi a3, 0xAB
    s8i  a3, a2, 7
    movi a3, 0x1234
    s16i a3, a2, 4
    l32i a4, a2, 4
#ifdef SELFTEST_BE
    CHECK a4, 0x123400AB
#else
    CHECK a4, 0xAB001234
#endif
    TEST 6
    movi a3, 0x13579BDF
    s32i.n a3, a2, 12
    l32i.n a4, a2, 12
    CHECK a4, 0x13579BDF
    TEST 7
    l32i a4, a2, 1020 - 1020        /* offset 0 again */
    CHECK a4, 0x80FF7F01
    TEST 8                          /* misaligned word load */
#if XT_HAVE_UNALIGNED_EXC
    ARM_EXC 9f                      /* (clobbers a2, a3) */
    movi a5, 0
    movi a6, buf + 2
    l32i a5, a6, 0                  /* LoadStoreAlignment, a5 unchanged */
9:  CHECK_REC R_COUNT, 1
    CHECK_REC R_CAUSE, 9
    CHECK_REC R_VADDR, buf + 2
    CHECK a5, 0
    ARM_EXC 0
#else
    addi a6, a2, 1
    l32i a5, a6, 0
#ifdef SELFTEST_BE
    CHECK a5, 0xFF7F0112            /* memory: 80 FF 7F 01 | 12 34 AB 00 */
#else
    CHECK a5, 0x3480FF7F            /* memory: 01 7F FF 80 | 34 12 00 AB */
#endif
#endif
    TEST 9                          /* L32R through the literal pool */
    l32r a4, lit_word
    CHECK a4, 0xCAFEF00D
    LEAVE_FN

    .section .literal
    .align 4
lit_word:
    .word 0xCAFEF00D
    .text

/* ---- branches ---------------------------------------------------------------------- */

    .macro BR_T op, a, b, taken
    addi a14, a14, 1
    movi a3, \a
    movi a4, \b
    movi a5, 0
    \op  a3, a4, .Lt\@
    movi a5, 1
.Lt\@:
    CHECK a5, \taken ^ 1
    .endm
    .macro BRI_T op, a, imm, taken
    addi a14, a14, 1
    movi a3, \a
    movi a5, 0
    \op  a3, \imm, .Lt\@
    movi a5, 1
.Lt\@:
    CHECK a5, \taken ^ 1
    .endm
    .macro BRZ_T op, a, taken
    addi a14, a14, 1
    movi a3, \a
    movi a5, 0
    \op  a3, .Lt\@
    movi a5, 1
.Lt\@:
    CHECK a5, \taken ^ 1
    .endm

t_branch:
    ENTER_FN
    TEST 100
    BR_T beq, 5, 5, 1
    BR_T beq, 5, 6, 0
    BR_T bne, 5, 6, 1
    BR_T bne, 6, 6, 0
    BR_T blt, -1, 0, 1
    BR_T blt, 0, -1, 0
    BR_T bge, 0, -1, 1
    BR_T bge, -2, -1, 0
    BR_T bge, 7, 7, 1
    BR_T bltu, 0, 0xFFFFFFFF, 1
    BR_T bltu, 0xFFFFFFFF, 0, 0
    BR_T bgeu, 0xFFFFFFFF, 0, 1
    BR_T bgeu, 3, 4, 0
    BR_T bany, 0x10, 0x30, 1
    BR_T bany, 0x10, 0x20, 0
    BR_T bnone, 0x10, 0x20, 1
    BR_T bnone, 0x10, 0x30, 0
    BR_T ball, 0x3F, 0x30, 1
    BR_T ball, 0x1F, 0x30, 0
    BR_T bnall, 0x1F, 0x30, 1
    BR_T bnall, 0x3F, 0x30, 0
    BRI_T beqi, -1, -1, 1
    BRI_T beqi, 256, 256, 1
    BRI_T beqi, 255, 256, 0
    BRI_T bnei, 12, 10, 1
    BRI_T bnei, 10, 10, 0
    BRI_T blti, -5, 1, 1
    BRI_T blti, 32, 32, 0
    BRI_T bgei, 128, 128, 1
    BRI_T bgei, -2, -1, 0
    BRI_T bltui, 32767, 32768, 1
    BRI_T bltui, 0x80000000, 65536, 0
    BRI_T bgeui, 65536, 65536, 1
    BRI_T bgeui, 1, 2, 0
    BRZ_T beqz, 0, 1
    BRZ_T beqz, 1, 0
    BRZ_T bnez, 1, 1
    BRZ_T bnez, 0, 0
    BRZ_T bltz, 0x80000000, 1
    BRZ_T bltz, 0, 0
    BRZ_T bgez, 0, 1
    BRZ_T bgez, -1, 0
    BRZ_T beqz.n, 0, 1
    BRZ_T beqz.n, 3, 0
    BRZ_T bnez.n, 3, 1
    BRZ_T bnez.n, 0, 0
    BRI_T bbsi, 1, BIT(0), 1
    BRI_T bbsi, 2, BIT(0), 0
    BRI_T bbci, 0x7FFFFFFF, BIT(31), 1
    BRI_T bbsi, 0x80000000, BIT(31), 1
    BRI_T bbci, 0x00010000, BIT(16), 0
    BR_T bbs, 0x00000100, BIT(8), 1
    BR_T bbs, 0x00000100, BIT(9), 0
    BR_T bbc, 0x00000100, BIT(9), 1
    BR_T bbc, 0x80000000, BIT(31) + 32, 0     /* the bit number is AR[t] mod 32 */
    LEAVE_FN

/* ---- calls ------------------------------------------------------------------------- */

t_calls:
    ENTER_FN
    TEST 150
    movi a2, 0
    call0 f_inc
    CHECK a2, 1
    TEST 151
    movi a3, f_inc
    callx0 a3
    CHECK a2, 2
    TEST 152
    movi a3, 1f
    jx   a3
    movi a2, 99
1:  CHECK a2, 2
    TEST 153
    movi a2, 7
    movi.n a3, -32
    add.n a2, a2, a3
    CHECK a2, -25
    addi.n a2, a2, -1
    CHECK a2, -26
    movi.n a3, 95
    mov.n a4, a3
    CHECK a4, 95
    LEAVE_FN

f_inc:
    addi a2, a2, 1
    ret.n

/* ---- register windows ------------------------------------------------------------------ */
#if XT_HAVE_WINDOWED
/* sum(n) = n + sum(n - 1), through CALL8 so that the windows overflow and underflow */
    .align 4
w_sum:
    entry a1, 32
    beqz a2, 1f
    addi a10, a2, -1
    call8 w_sum
    add  a2, a2, a10
1:  retw

    .align 4
w_sum4:
    entry a1, 32
    beqz a2, 1f
    addi a6, a2, -1
    call4 w_sum4
    add  a2, a2, a6
1:  retw

    .align 4
w_sum12:
    entry a1, 48
    beqz a2, 1f
    addi a14, a2, -1
    call12 w_sum12
    add  a2, a2, a14
1:  retw

    .align 4
w_movsp:
    entry a1, 32
    mov  a3, a2
    movsp a2, a3
    retw

/* a windowed function with all registers live: a4..a15 survive a deep call */
    .align 4
w_keep:
    entry a1, 32
    movi a4, 4
    movi a5, 5
    movi a6, 6
    movi a7, 7
    movi a10, 20                    /* CALL8: a8.. are the callee's (a8 its return) */
    call8 w_sum
    add  a2, a4, a5
    add  a2, a2, a6
    add  a2, a2, a7
    add  a2, a2, a10                /* + sum(20) = 210 */
    retw

t_windows:
    /* This call0 frame becomes the root of the windowed calls: a spill of it as an 8- or
     * 12-register frame saves a4.. below the stack pointer the word at a1 - 12 gives
     * (normally its caller's spill wrote it): point it at a scratch area. */
    addi a1, a1, -80
    s32i a0, a1, 0
    addi a2, a1, 64
    addi a3, a1, -12
    s32i a2, a3, 0
    s32i a14, a1, 4
    TEST 200
    movi a6, 5
    call4 w_sum4
    movi a14, 200
    CHECK a6, 15
    TEST 201
    movi a10, 40                    /* deep enough to spill the whole register file */
    call8 w_sum
    movi a14, 201
    CHECK a10, 820
    TEST 202
    movi a14, 30
    call12 w_sum12
    mov  a2, a14
    movi a14, 202
    CHECK a2, 465
    TEST 203
    movi a6, 0
    call4 w_keep
    movi a14, 203
    CHECK a6, 232
    TEST 204                        /* ROTW: a4 of the next window is today's a8 */
    rsr  a2, ps
    movi a3, ~PS_WOE
    and  a3, a2, a3
    wsr  a3, ps                     /* no window checks while rotating by hand */
    rsync
    movi a8, 0x1234
    movi a9, 0
    rotw 1
    mov  a5, a4                     /* a5 here is a9 below */
    rotw -1
    wsr  a2, ps
    rsync
    CHECK a9, 0x1234
    TEST 205                        /* MOVSP at the root: no caller frame resident, Alloca */
    ARM_EXC 1f
    movi a2, 1
    movi a3, 2
    movsp a3, a2
1:  CHECK_REC R_CAUSE, 5
    CHECK a3, 2
    ARM_EXC 0
    TEST 206                        /* and in a function whose caller's frame is resident */
    movi a6, 0x77
    call4 w_movsp
    movi a14, 206
    CHECK a6, 0x77
    l32i a0, a1, 0
    addi a1, a1, 80
    ret
#endif

/* ---- zero-overhead loops ------------------------------------------------------------- */
#if XT_HAVE_LOOPS
/* Straight-line code that later turns out to hold a loop end. */
    .align 4
pre_loop:
    addi a5, a5, 1
pre_mid:
    addi a6, a6, 1
    ret

t_loops:
    ENTER_FN
    TEST 300
    movi a2, 5
    movi a3, 0
    loop a2, 1f
    addi a3, a3, 3
1:  CHECK a3, 15
    TEST 301
    movi a2, 0
    movi a3, 0
    loopnez a2, 1f
    addi a3, a3, 1
1:  CHECK a3, 0
    TEST 302
    movi a2, -3
    movi a3, 0
    loopgtz a2, 1f
    addi a3, a3, 1
1:  CHECK a3, 0
    TEST 303
    movi a2, 1
    movi a3, 0
    loopgtz a2, 1f
    addi a3, a3, 1
    addi a3, a3, 1
1:  CHECK a3, 2
    TEST 304                        /* a taken branch to the loop end leaves the loop */
    movi a2, 10
    movi a3, 0
    loop a2, 1f
    addi a3, a3, 1
    beqi a3, 4, 1f
    nop
1:  CHECK a3, 4
    TEST 305                        /* LCOUNT counts down */
    movi a2, 3
    loop a2, 1f
    nop
1:  rsr  a4, lcount
    CHECK a4, 0
    TEST 306                        /* a loop end inside code translated without one */
    movi a5, 0
    movi a6, 0
    call0 pre_loop                  /* translated now: no loop */
    CHECK a5, 1
    movi a2, pre_loop
    wsr  a2, lbeg
    movi a2, pre_mid
    wsr  a2, lend
    movi a2, 3
    wsr  a2, lcount
    isync
    movi a5, 0
    movi a6, 0
    call0 pre_loop                  /* addi a5 four times, then on */
    CHECK a5, 4
    CHECK a6, 1
    TEST 307                        /* and code translated with a loop end, after it moved */
    movi a2, lit_word
    wsr  a2, lend
    movi a2, 5
    wsr  a2, lcount
    isync
    movi a5, 0
    movi a6, 0
    call0 pre_loop
    CHECK a5, 1
    CHECK a6, 1
    movi a2, 0
    wsr  a2, lcount
    TEST 308                        /* no loop-back with PS.EXCM set */
    rsr  a7, ps
    movi a2, PS_EXCM | 15
    wsr  a2, ps
    rsync
    movi a2, 4
    movi a3, 0
    loop a2, 1f
    addi a3, a3, 1
1:  wsr  a7, ps
    rsync
    CHECK a3, 1
    TEST 309                        /* a loop over 64 instructions: several blocks */
    movi a2, 3
    movi a3, 0
    loop a2, 2f
    .rept 70
    addi a3, a3, 1
    .endr
2:  CHECK a3, 210
    LEAVE_FN
#endif

/* ---- booleans ---------------------------------------------------------------------- */
#if XT_HAVE_BOOLEANS
t_booleans:
    ENTER_FN
    TEST 400
    movi a2, 0x00F5                 /* b0 b2 b4-b7 */
    wsr  a2, br
    andb b8, b0, b2
    orb  b9, b1, b3
    xorb b10, b0, b4
    andbc b11, b0, b1
    orbc b12, b1, b2
    any4 b13, b4
    all4 b14, b4
    all8 b15, b0
    rsr  a3, br
    CHECK a3, 0x69F5                /* b8 b11 b13 b14 */
    TEST 401
    movi a2, 0
    bt   b0, 1f
    movi a2, 1
1:  CHECK a2, 0
    bf   b0, 2f
    movi a2, 2
2:  CHECK a2, 2
    TEST 402
    movi a3, 7
    movi a4, 9
    movt a4, a3, b0
    CHECK a4, 7
    movi a4, 9
    movf a4, a3, b0
    CHECK a4, 9
    TEST 403
    movi a2, 0xFF00
    wsr  a2, br
    any8 b0, b8
    all8 b1, b8
    any4 b2, b0
    rsr  a3, br
    CHECK a3, 0xFF07
    LEAVE_FN
#endif

/* ---- MAC16 ------------------------------------------------------------------------- */
#if XT_HAVE_MAC16
t_mac16:
    ENTER_FN
    TEST 450
    movi a2, 0x7FFF8000             /* high 32767, low -32768 */
    movi a3, 0x0003FFFE             /* high 3, low -2 */
    mul.aa.ll a2, a3                /* -32768 * -2 = 65536 */
    rsr  a4, acclo
    CHECK a4, 65536
    rsr  a4, acchi
    CHECK a4, 0
    mula.aa.hh a2, a3               /* + 32767 * 3 */
    rsr  a4, acclo
    CHECK a4, 65536 + 98301
    muls.aa.hl a2, a3               /* - 32767 * -2 */
    rsr  a4, acclo
    CHECK a4, 65536 + 98301 + 65534
    TEST 451                        /* the 40-bit accumulator carries into ACCHI */
    movi a4, 0xFFFFFFFF
    wsr  a4, acclo
    movi a4, 0x7F
    wsr  a4, acchi
    movi a2, 1
    movi a3, 1
    mula.aa.ll a2, a3
    rsr  a4, acclo
    CHECK a4, 0
    rsr  a4, acchi
    CHECK a4, 0xFFFFFF80             /* ACC[39:32] = 0x80, read sign-extended */
    TEST 452
    movi a2, 0xFFFF
    movi a3, 0xFFFF
    umul.aa.ll a2, a3
    rsr  a4, acclo
    CHECK a4, 0xFFFE0001
    TEST 453                        /* loads with update; the multiply uses the old m0 */
    movi a5, buf
    movi a2, 0x00050004
    s32i a2, a5, 4
    movi a2, 0x00070006
    s32i a2, a5, 8
    movi a2, 3
    wsr  a2, m0
    movi a2, 10
    wsr  a2, m2
    mul.dd.ll m0, m2                /* 30 */
    mula.dd.ll.ldinc m0, a5, m0, m2  /* + 3 * 10, m0 = 0x00050004, a5 += 4 */
    rsr  a4, acclo
    CHECK a4, 60
    rsr  a4, m0
    CHECK a4, 0x00050004
    movi a2, buf + 4
    CHECK a5, buf + 4
    ldinc m1, a5
    rsr  a4, m1
    CHECK a4, 0x00070006
    lddec m3, a5
    rsr  a4, m3
    CHECK a4, 0x00050004
    LEAVE_FN
#endif

/* ---- floating point ---------------------------------------------------------------- */
#if XT_HAVE_FP
t_fp:
    ENTER_FN
    TEST 500                        /* the coprocessor disabled: Coprocessor0Disabled */
    movi a2, 0
    wsr  a2, cpenable
    rsync
    ARM_EXC 1f
    wfr  f0, a2
1:  CHECK_REC R_COUNT, 1
    CHECK_REC R_CAUSE, 32
    ARM_EXC 0
    movi a2, 1
    wsr  a2, cpenable
    rsync
    TEST 501
    movi a2, 0x3FC00000             /* 1.5 */
    movi a3, 0x40200000             /* 2.5 */
    wfr  f1, a2
    wfr  f2, a3
    add.s f3, f1, f2
    rfr  a4, f3
    CHECK a4, 0x40800000            /* 4.0 */
    mul.s f4, f1, f2
    rfr  a4, f4
    CHECK a4, 0x40700000            /* 3.75 */
    madd.s f4, f1, f1               /* 3.75 + 2.25 */
    rfr  a4, f4
    CHECK a4, 0x40C00000
    sub.s f5, f1, f2
    neg.s f6, f5
    abs.s f7, f5
    rfr  a4, f6
    CHECK a4, 0x3F800000
    rfr  a4, f7
    CHECK a4, 0x3F800000
    TEST 502
    movi a2, -7
    float.s f8, a2, 1               /* -3.5 */
    rfr  a4, f8
    CHECK a4, 0xC0600000
    trunc.s a5, f8, 2               /* -14 */
    CHECK a5, -14
    round.s a5, f8, 0               /* -3.5 -> -4 (ties to even) */
    CHECK a5, -4
    floor.s a5, f8, 0
    CHECK a5, -4
    ceil.s a5, f8, 0
    CHECK a5, -3
    TEST 503
    olt.s b1, f1, f2
    ole.s b2, f2, f1
    oeq.s b3, f1, f1
    rsr  a4, br
    extui a4, a4, 1, 3
    CHECK a4, 0x5
    TEST 504
    movi a5, buf
    ssi  f3, a5, 8
    lsi  f9, a5, 8
    rfr  a4, f9
    CHECK a4, 0x40800000
    movi a6, 4
    ssx  f1, a5, a6
    lsx  f10, a5, a6
    rfr  a4, f10
    CHECK a4, 0x3FC00000
    lsip f11, a5, 4                 /* post-increment: loads buf, then a5 = buf + 4 */
    rfr  a4, f11
    movi a2, buf
    l32i a3, a2, 0
    sub  a4, a4, a3
    CHECK a4, 0
    movi a2, buf + 4
    CHECK a5, buf + 4
    TEST 505
    movi a2, 0x40490FDB             /* pi */
    movi a3, 0x402DF854             /* e */
    wfr  f1, a2
    wfr  f2, a3
    div0.s f3, f2                   /* the step instructions libgcc's __divsf3 runs */
    nexp01.s f4, f2
    const.s f0, 1
    maddn.s f0, f4, f3
    mov.s f5, f3
    nexp01.s f6, f1
    maddn.s f5, f0, f3
    const.s f7, 1
    const.s f2, 0
    neg.s f8, f6
    maddn.s f7, f4, f5
    maddn.s f2, f8, f3
    wfr  f9, a3                     /* b */
    wfr  f10, a2                    /* a */
    mkdadj.s f9, f10
    maddn.s f5, f7, f5
    maddn.s f8, f4, f2
    const.s f3, 1
    maddn.s f3, f4, f5
    maddn.s f2, f8, f5
    neg.s f7, f6
    maddn.s f5, f3, f5
    maddn.s f7, f4, f2
    addexpm.s f2, f9
    addexp.s f5, f9
    divn.s f2, f7, f5
    rfr  a4, f2
    CHECK a4, 0x3F93EEE0            /* pi / e, correctly rounded */
    TEST 506
    rur  a2, fcr
    movi a3, 0
    wur  a3, fcr
    rur  a4, fsr
    LEAVE_FN
#endif

/* ---- atomics ----------------------------------------------------------------------- */
#if XT_HAVE_S32C1I
t_atomic:
    ENTER_FN
    TEST 550
    movi a5, buf
    movi a2, 100
    s32i a2, a5, 0
    movi a2, 100
    wsr  a2, scompare1
    movi a3, 200
    s32c1i a3, a5, 0                /* matches: stores 200, a3 = 100 */
    CHECK a3, 100
    l32i a4, a5, 0
    CHECK a4, 200
    movi a3, 300
    s32c1i a3, a5, 0                /* 200 != 100: no store, a3 = 200 */
    CHECK a3, 200
    l32i a4, a5, 0
    CHECK a4, 200
    TEST 551
    movi a2, 0x5A5A
    s32ri a2, a5, 4
    l32ai a3, a5, 4
    CHECK a3, 0x5A5A
    LEAVE_FN
#endif

/* ---- exceptions -------------------------------------------------------------------- */

t_exceptions:
    ENTER_FN
    TEST 600                        /* SYSCALL: EPC1 is the SYSCALL itself */
    ARM_EXC 1f
2:  syscall
1:  CHECK_REC R_COUNT, 1
    CHECK_REC R_CAUSE, 1
    CHECK_REC R_EPC, 2b
    TEST 601                        /* ILL */
    ARM_EXC 1f
    ill
1:  CHECK_REC R_CAUSE, 0
    TEST 602                        /* an undefined opcode: op0 = 0, op1 = 0xC is reserved */
    ARM_EXC 1f
    .byte 0x00, 0x00, 0x0C
1:  CHECK_REC R_CAUSE, 0
#if XT_HAVE_DIV32
    TEST 603                        /* divide by zero: the destination keeps its value */
    ARM_EXC 1f
    movi a3, 10
    movi a4, 0
    movi a5, 77
    quou a5, a3, a4
1:  CHECK_REC R_CAUSE, 6
    CHECK a5, 77
#endif
    TEST 604                        /* region protection: no access to region 0 */
    movi a2, 15
    movi a3, 0
    wdtlb a2, a3
    dsync
    ARM_EXC 1f
    movi a3, 0x100
    l32i a4, a3, 0
1:  CHECK_REC R_CAUSE, 28
    CHECK_REC R_VADDR, 0x100
    ARM_EXC 1f
    movi a3, 0x104
    s32i a4, a3, 0
1:  CHECK_REC R_CAUSE, 29
    CHECK_REC R_VADDR, 0x104
    TEST 605
    movi a3, 0
    rdtlb1 a4, a3
    CHECK a4, 15
    movi a2, 2
    wdtlb a2, a3
    dsync
    TEST 606                        /* and no instruction fetch from it */
    movi a2, 15
    movi a3, 0
    witlb a2, a3
    isync
    ARM_EXC 1f
    movi a3, 0x200
    callx0 a3
1:  CHECK_REC R_CAUSE, 20
    CHECK_REC R_EPC, 0x200
    movi a2, 2
    movi a3, 0
    witlb a2, a3
    isync
    TEST 607                        /* a double exception: an exception with PS.EXCM set */
    rsr  a7, ps
    movi a2, PS_EXCM | 15
    wsr  a2, ps
    rsync
    ARM_EXC 1f
2:  syscall
1:  wsr  a7, ps
    rsync
    CHECK_REC R_DCOUNT, 1
    CHECK_REC R_DCAUSE, 1
    CHECK_REC R_DEPC, 2b
    CHECK_REC R_COUNT, 0
    TEST 608                        /* BREAK: the debug vector */
    ARM_EXC 1f
2:  break 1, 2
1:  CHECK_REC R_BCOUNT, 1
    CHECK_REC R_BCAUSE, 8
    CHECK_REC R_BEPC, 2b
    ARM_EXC 1f
2:  break.n 3
1:  CHECK_REC R_BCAUSE, 16
    CHECK_REC R_BEPC, 2b
    ARM_EXC 0
    LEAVE_FN

/* ---- interrupts -------------------------------------------------------------------- */

t_interrupts:
    ENTER_FN
    TEST 700                        /* software interrupt 7, level 1 */
    ARM_EXC 0
    movi a2, 1 << 7
    wsr  a2, intenable
    wsr  a2, intset
    esync
    nop
    nop
    CHECK_REC R_COUNT, 1
    CHECK_REC R_CAUSE, 4
    CHECK_REC R_INT, 1 << 7
    movi a2, 1 << 7
    wsr  a2, intclear
    TEST 701                        /* masked by PS.INTLEVEL until RSIL lowers it */
    ARM_EXC 0
    rsil a8, 1
    movi a2, 1 << 7
    wsr  a2, intset
    wsr  a2, intenable
    esync
    nop
    CHECK_REC R_COUNT, 0
    rsil a2, 0
    nop
    nop
    CHECK_REC R_COUNT, 1
    movi a2, 1 << 7
    wsr  a2, intclear
    TEST 702                        /* CCOMPARE0: interrupt 6, level 1, while WAITI waits */
    ARM_EXC 0
    rsil a2, 1
    rsr  a3, ccount
    addi a3, a3, 100
    wsr  a3, ccompare0
    movi a2, 1 << 6
    wsr  a2, intenable
    waiti 0
    CHECK_REC R_COUNT, 1
    CHECK_REC R_INT, 1 << 6
    rsr  a2, ccount
    sub  a2, a2, a3
    bgez a2, 1f
    movi a2, 0xBAD
    CHECK a2, 0
1:  wsr  a3, ccompare0              /* clears the request */
    TEST 703                        /* timer 0: interrupt 10, edge-triggered, level 1 */
    ARM_EXC 0
    movi a2, TIMER0
    movi a3, 50
    s32i a3, a2, 8                  /* LOAD */
    movi a3, 1 << 10
    wsr  a3, intenable
    movi a3, 5                      /* EN | IE, one-shot */
    s32i a3, a2, 0
    movi a4, 2000
1:  addi a4, a4, -1
    bnez a4, 1b
    CHECK_REC R_COUNT, 1
    CHECK_REC R_INT, 1 << 10
    movi a2, TIMER0
    movi a3, 1
    s32i a3, a2, 4                  /* acknowledge the timer */
    movi a3, 1 << 10
    wsr  a3, intclear
    TEST 704                        /* timer 1: the NMI, whatever PS.INTLEVEL */
    ARM_EXC 0
    rsil a5, 15
    movi a2, TIMER1
    movi a3, 50
    s32i a3, a2, 8
    movi a3, 5
    s32i a3, a2, 0
    movi a4, 2000
1:  addi a4, a4, -1
    bnez a4, 1b
    wsr  a5, ps
    rsync
    CHECK_REC R_HCOUNT, 1
    CHECK_REC R_HLEVEL, XT_NMI_LEVEL
#if XT_HAVE_HIGHINT
    TEST 705                        /* software interrupt 29, level 3: EPC3/EPS3, RFI 3 */
    ARM_EXC 0
    movi a2, 1 << 29
    wsr  a2, intenable
    wsr  a2, intset
    esync
    nop
    nop
    CHECK_REC R_HCOUNT, 1
    CHECK_REC R_HLEVEL, 3
    CHECK_REC R_COUNT, 0
    movi a2, 1 << 29
    wsr  a2, intclear
    TEST 706                        /* level 3 interrupts PS.INTLEVEL 2, not 3 */
    ARM_EXC 0
    rsil a5, 3
    movi a2, 1 << 29
    wsr  a2, intset
    wsr  a2, intenable
    esync
    nop
    CHECK_REC R_HCOUNT, 0
    rsil a3, 2
    nop
    nop
    CHECK_REC R_HCOUNT, 1
    wsr  a5, ps
    rsync
    movi a2, 1 << 29
    wsr  a2, intclear
#endif
    movi a2, 0
    wsr  a2, intenable
    LEAVE_FN

/* ---- special registers ------------------------------------------------------------- */

t_sregs:
    ENTER_FN
    TEST 800
    rsr  a2, prid
    CHECK a2, XT_PRID
    TEST 801                        /* CCOUNT moves */
    rsr  a2, ccount
    nop
    rsr  a3, ccount
    sub  a3, a3, a2
    beqz a3, 1f
    movi a3, 1
1:  CHECK a3, 1
    TEST 802                        /* XSR swaps */
    movi a2, 0x11
    wsr  a2, excsave1
    movi a3, 0x22
    xsr  a3, excsave1
    CHECK a3, 0x11
    rsr  a4, excsave1
    CHECK a4, 0x22
    TEST 803
    movi a2, 0x3F
    wsr  a2, sar
    rsr  a3, sar
    CHECK a3, 0x3F
    ssai 17
    rsr  a3, sar
    CHECK a3, 17
#if XT_HAVE_THREADPTR
    TEST 804
    movi a2, 0xDEADBEEF
    wur  a2, threadptr
    rur  a3, threadptr
    CHECK a3, 0xDEADBEEF
    TEST 805
    movi a2, 0x1357
    wsr  a2, misc0
    rsr  a3, misc0
    CHECK a3, 0x1357
    rsr  a3, configid0
    beqz a3, 1f
    movi a3, 1
1:  CHECK a3, 1
#endif
#if XT_HAVE_LITBASE
    TEST 806                        /* L32R from LITBASE: the word just below lit_page */
    movi a2, lit_page + 1
    wsr  a2, litbase
    isync
    .byte 0x21, 0xFF, 0xFF          /* l32r a2, -4 */
    movi a3, 0
    wsr  a3, litbase
    isync
    CHECK a2, 0x600DCAFE
#endif
    TEST 807                        /* RER: no ERI device reads 0 */
    movi a2, 0x100000
    movi a3, 1
    .byte 0x30, 0x62, 0x40          /* rer a3, a2 (the LX106 assembler takes no operands) */
    CHECK a3, 0
    LEAVE_FN

/* ---- data -------------------------------------------------------------------------- */

    .data
    .align 16
rec:    .space 128
buf:    .space 64
#if XT_HAVE_LITBASE
    .balign 4096
    .space 4092
    .word 0x600DCAFE
lit_page:                           /* a 4 KiB boundary: LITBASE's base */
    .word 0
#endif
    .align 16
    .space 4096
stack_top:

    .section .strings, "a"
hexdig:   .ascii "0123456789abcdef"
msg_fail: .asciz "xtensa selftest: FAIL test "
msg_got:  .asciz " got "
msg_exp:  .asciz " expected "
msg_ok:   .asciz "xtensa selftest: all tests passed\n"

#include "vectors.S"
