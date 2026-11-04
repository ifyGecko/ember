/*
 * start.S - PIC32 self-test: exception and interrupt vectors, reset entry. The
 * same source assembles as MIPS32 (mx, mz) or microMIPS (mm, mz with
 * -mmicromips): the processor enters handlers in the ISA of Config3.ISAOnExc.
 *
 * EBase is _ebase (the start of program flash, 0x9d000000). The TLB refill
 * vector (+0x000, mz) notes that it was used and joins the general vector
 * (+0x180), which saves the GPRs to g_frame, calls trap_handler(frame), writes the
 * returned address to EPC, restores and erets. Interrupt vectors are 32 bytes
 * apart from +0x200 (IntCtl.VS = 1; on the variable-offset controller the
 * firmware programs OFFn = 0x200 + 32n): vector n loads n into k1 and joins
 * int_common, which takes the interrupted processor's stack from the previous
 * shadow set (RDPGPR: the same set unless the controller named another), saves
 * the caller-saved registers, calls irq_handler(n) and erets.
 */
    .set noreorder
    .set noat

#define NVEC 16

    .section .text.vectors, "ax"
    .globl _ebase
_ebase:
    .org 0x000
    .globl refill_vector
refill_vector:
    lui     $k0, %hi(g_refill)
    li      $k1, 1
    sw      $k1, %lo(g_refill)($k0)
    j       exc_save
    nop

    .org 0x180
    .globl exc_vector
exc_vector:
    j       exc_save
    nop

    .org 0x200
    .irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
    .org 0x200 + 32 * \n
    li      $k1, \n
    j       int_common
    nop
    .endr

    .org 0x200 + 32 * NVEC
exc_save:
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
    la      $sp, __trap_stack
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

int_common:
    rdpgpr  $sp, $sp            /* the interrupted code's stack */
    addiu   $sp, $sp, -80
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
    jal     irq_handler
    move    $4, $k1             /* a0 = vector (delay slot) */
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
    addiu   $sp, $sp, 80
    eret
    nop

    .text
    .globl _start
    .ent _start
_start:
    la      $sp, __stack_top
    la      $8, _ebase
    mtc0    $8, $15, 1          /* EBase */
    li      $8, 0x20
    mtc0    $8, $12, 1          /* IntCtl.VS = 1: 32-byte vector spacing */
    mfc0    $8, $13
    li      $9, 0x00800000
    or      $8, $8, $9
    mtc0    $8, $13             /* Cause.IV: the interrupt vectors */
    mfc0    $8, $12
    li      $9, ~0x0040FC07     /* clear BEV, IPL, ERL, EXL, IE */
    and     $8, $8, $9
    mtc0    $8, $12
    ehb
#if defined(__mips_hard_float)
    mfc0    $8, $12
#if __mips_fpr == 64
    li      $9, 0x24000000      /* CU1, FR = 1 */
#else
    li      $9, 0x20000000      /* CU1, FR = 0 */
#endif
    or      $8, $8, $9
    mtc0    $8, $12
    ehb
#endif
    la      $8, __bss_start
    la      $9, __bss_end
1:  beq     $8, $9, 2f
    nop
    sw      $0, 0($8)
    addiu   $8, $8, 4
    b       1b
    nop
2:  jal     main
    nop
3:  b       3b
    nop
    .end _start

    .bss
    .align 2
    .globl g_frame
g_frame:
    .space 136
    .globl g_refill
g_refill:
    .space 4
