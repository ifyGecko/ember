/*
 * start.S - MIPS32 exception vectors, reset entry, and the trap trampoline for
 * the self-test. EBase is set to 0x80100000, so the TLB refill vector is
 * 0x80100000 and the general exception vector 0x80100180. The refill vector
 * notes that it was used (g_refill = 1) and joins the general vector, which
 * saves all GPRs (and HI/LO) to g_frame, calls trap_handler(frame), writes the
 * returned address to EPC, restores (the handler may edit the frame), and
 * erets. The ELF entry point is _start, after the vectors.
 */
    .set noreorder
    .set noat

    .section .text.init, "ax"
    .org 0x000
    .globl refill_vector
refill_vector:
    lui     $k0, %hi(g_refill)
    li      $k1, 1
    j       exc_vector
    sw      $k1, %lo(g_refill)($k0)

    .org 0x180
    .globl exc_vector
exc_vector:
    la      $k0, g_frame
    sw      $1,  4($k0)
    sw      $2,  8($k0)
    sw      $3,  12($k0)
    sw      $4,  16($k0)
    sw      $5,  20($k0)
    sw      $6,  24($k0)
    sw      $7,  28($k0)
    sw      $8,  32($k0)
    sw      $9,  36($k0)
    sw      $10, 40($k0)
    sw      $11, 44($k0)
    sw      $12, 48($k0)
    sw      $13, 52($k0)
    sw      $14, 56($k0)
    sw      $15, 60($k0)
    sw      $16, 64($k0)
    sw      $17, 68($k0)
    sw      $18, 72($k0)
    sw      $19, 76($k0)
    sw      $20, 80($k0)
    sw      $21, 84($k0)
    sw      $22, 88($k0)
    sw      $23, 92($k0)
    sw      $24, 96($k0)
    sw      $25, 100($k0)
    sw      $28, 112($k0)
    sw      $29, 116($k0)
    sw      $30, 120($k0)
    sw      $31, 124($k0)
    mfhi    $8
    sw      $8,  128($k0)
    mflo    $8
    sw      $8,  132($k0)

    la      $sp, __stack_top
    move    $4, $k0             /* a0 = frame */
    jal     trap_handler
    nop
    mtc0    $2, $14             /* EPC = trap_handler() return */
    ehb

    la      $k0, g_frame
    lw      $8,  128($k0)
    mthi    $8
    lw      $8,  132($k0)
    mtlo    $8
    lw      $1,  4($k0)
    lw      $2,  8($k0)
    lw      $3,  12($k0)
    lw      $4,  16($k0)
    lw      $5,  20($k0)
    lw      $6,  24($k0)
    lw      $7,  28($k0)
    lw      $8,  32($k0)
    lw      $9,  36($k0)
    lw      $10, 40($k0)
    lw      $11, 44($k0)
    lw      $12, 48($k0)
    lw      $13, 52($k0)
    lw      $14, 56($k0)
    lw      $15, 60($k0)
    lw      $16, 64($k0)
    lw      $17, 68($k0)
    lw      $18, 72($k0)
    lw      $19, 76($k0)
    lw      $20, 80($k0)
    lw      $21, 84($k0)
    lw      $22, 88($k0)
    lw      $23, 92($k0)
    lw      $24, 96($k0)
    lw      $25, 100($k0)
    lw      $28, 112($k0)
    lw      $29, 116($k0)
    lw      $30, 120($k0)
    lw      $31, 124($k0)
    eret
    nop

    .globl _start
_start:
    la      $sp, __stack_top
    mtc0    $zero, $12          /* Status = 0: kernel, BEV off, EXL/ERL off, ints off */
    ehb
    la      $t0, 0x80100000
    mtc0    $t0, $15, 1         /* EBase */
    ehb
    mtc0    $zero, $13          /* Cause = 0 (IV=0) */
    ehb

    /* zero .bss */
    la      $t0, __bss_start
    la      $t1, __bss_end
1:  sltu    $t2, $t0, $t1
    beq     $t2, $zero, 2f
    nop
    sw      $zero, 0($t0)
    b       1b
    addiu   $t0, $t0, 4
2:
    jal     main
    nop
    /* main returns the finisher word; write it to the test device, then spin */
    li      $t0, 0xbfd00010
    sw      $2, 0($t0)
3:  b       3b
    nop

/*
 * enter_user(pc, arg): drop to user mode at `pc` with a0 = arg. The trap
 * handler ends the excursion by returning user_resume, which restores the
 * caller's sp/ra and returns from enter_user.
 */
    .text
    .globl enter_user
enter_user:
    la      $t0, g_saved
    sw      $sp, 0($t0)
    sw      $ra, 4($t0)
    mtc0    $a0, $14            /* EPC = user pc */
    mfc0    $t1, $12
    ori     $t1, $t1, 0x12      /* EXL | KSU=user (IE stays clear) */
    mtc0    $t1, $12
    ehb
    move    $a0, $a1
    eret
    nop

    .globl user_resume
user_resume:
    la      $t0, g_saved
    lw      $sp, 0($t0)
    lw      $ra, 4($t0)
    jr      $ra
    nop

/* User-mode code, alone on its own 4 KiB page so it can be TLB-mapped:
 * a load from a0 (a kseg0 address: AdEL in user mode), then a syscall. */
    .section .text.user, "ax"
    .align 12
    .globl user_stub
user_stub:
    lw      $t0, 0($a0)
    nop
    syscall
    nop
    .align 12

    .section .bss
    .align 4
    .globl g_saved
g_saved:
    .space 8
    .globl g_frame
g_frame:
    .space 136
    .globl g_refill
g_refill:
    .space 4
