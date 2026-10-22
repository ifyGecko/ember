/*
 * smpstart.S - Reset entry for the multiprocessor self-test (smptest.c): every
 * hart arrives here. Each takes a 4 KiB stack by mhartid; hart 0 clears .bss
 * while the others wait for it, then all call smp_main(mhartid).
 */
    .section .text.init, "ax"
    .option norvc
    .option norelax
    .globl _start
_start:
    csrr    a0, mhartid
    li      t0, 4
    bgeu    a0, t0, park            /* stacks for harts 0..3 only */
    la      sp, __stack_top
    slli    t0, a0, 12
    sub     sp, sp, t0
    la      t0, trap_entry
    csrw    mtvec, t0
    bnez    a0, wait_bss

    la      t0, __bss_start
    la      t1, __bss_end
1:  bgeu    t0, t1, 2f
    sw      zero, 0(t0)
    addi    t0, t0, 4
    j       1b
2:  fence
    la      t0, bss_ready
    li      t1, 1
    sw      t1, 0(t0)
    j       go

wait_bss:
    la      t0, bss_ready
3:  lw      t1, 0(t0)
    beqz    t1, 3b
    fence
go:
    csrr    a0, mhartid
    call    smp_main
park:
    wfi
    j       park

    .data
    .align 2
bss_ready:
    .word   0

    .text
    .align 2
    .globl trap_entry
trap_entry:
    /* caller-saved registers only: the C handler saves the rest */
    addi    sp, sp, -64
    sw      ra, 0(sp)
    sw      t0, 4(sp)
    sw      t1, 8(sp)
    sw      t2, 12(sp)
    sw      a0, 16(sp)
    sw      a1, 20(sp)
    sw      a2, 24(sp)
    sw      a3, 28(sp)
    sw      a4, 32(sp)
    sw      a5, 36(sp)
    sw      a6, 40(sp)
    sw      a7, 44(sp)
    sw      t3, 48(sp)
    sw      t4, 52(sp)
    sw      t5, 56(sp)
    sw      t6, 60(sp)
    call    smp_trap
    lw      ra, 0(sp)
    lw      t0, 4(sp)
    lw      t1, 8(sp)
    lw      t2, 12(sp)
    lw      a0, 16(sp)
    lw      a1, 20(sp)
    lw      a2, 24(sp)
    lw      a3, 28(sp)
    lw      a4, 32(sp)
    lw      a5, 36(sp)
    lw      a6, 40(sp)
    lw      a7, 44(sp)
    lw      t3, 48(sp)
    lw      t4, 52(sp)
    lw      t5, 56(sp)
    lw      t6, 60(sp)
    addi    sp, sp, 64
    mret
