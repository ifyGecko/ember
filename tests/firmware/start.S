/*
 * start.S - Reset entry, trap entry, and the vectored-mode trap table for the
 * self-test firmware.
 *
 * Trap frame: x1..x31 saved at sp[4*i] (slot 0 unused). trap_handler(frame)
 * returns the pc to resume at, which is written to mepc before mret.
 */
    .section .text.init, "ax"
    .option norvc
    .globl _start
_start:
    la      sp, __stack_top
    la      t0, trap_entry
    csrw    mtvec, t0

    /* Zero .bss */
    la      t0, __bss_start
    la      t1, __bss_end
1:  bgeu    t0, t1, 2f
    sw      zero, 0(t0)
    addi    t0, t0, 4
    j       1b
2:
    call    main
    /* main returns the finisher word; write it and spin. */
    li      t0, 0x100000
    sw      a0, 0(t0)
3:  wfi
    j       3b

    .align 2
    .globl trap_entry
trap_entry:
    addi    sp, sp, -128
    sw      x1,  4(sp)
    sw      x3,  12(sp)
    sw      x4,  16(sp)
    sw      x5,  20(sp)
    sw      x6,  24(sp)
    sw      x7,  28(sp)
    sw      x8,  32(sp)
    sw      x9,  36(sp)
    sw      x10, 40(sp)
    sw      x11, 44(sp)
    sw      x12, 48(sp)
    sw      x13, 52(sp)
    sw      x14, 56(sp)
    sw      x15, 60(sp)
    sw      x16, 64(sp)
    sw      x17, 68(sp)
    sw      x18, 72(sp)
    sw      x19, 76(sp)
    sw      x20, 80(sp)
    sw      x21, 84(sp)
    sw      x22, 88(sp)
    sw      x23, 92(sp)
    sw      x24, 96(sp)
    sw      x25, 100(sp)
    sw      x26, 104(sp)
    sw      x27, 108(sp)
    sw      x28, 112(sp)
    sw      x29, 116(sp)
    sw      x30, 120(sp)
    sw      x31, 124(sp)
    addi    t0, sp, 128
    sw      t0,  8(sp)          /* original sp */

    mv      a0, sp
    call    trap_handler
    csrw    mepc, a0

    lw      x1,  4(sp)
    lw      x3,  12(sp)
    lw      x4,  16(sp)
    lw      x5,  20(sp)
    lw      x6,  24(sp)
    lw      x7,  28(sp)
    lw      x8,  32(sp)
    lw      x9,  36(sp)
    lw      x10, 40(sp)
    lw      x11, 44(sp)
    lw      x12, 48(sp)
    lw      x13, 52(sp)
    lw      x14, 56(sp)
    lw      x15, 60(sp)
    lw      x16, 64(sp)
    lw      x17, 68(sp)
    lw      x18, 72(sp)
    lw      x19, 76(sp)
    lw      x20, 80(sp)
    lw      x21, 84(sp)
    lw      x22, 88(sp)
    lw      x23, 92(sp)
    lw      x24, 96(sp)
    lw      x25, 100(sp)
    lw      x26, 104(sp)
    lw      x27, 108(sp)
    lw      x28, 112(sp)
    lw      x29, 116(sp)
    lw      x30, 120(sp)
    lw      x31, 124(sp)
    addi    sp, sp, 128
    mret

/*
 * Vectored mode table (mtvec.MODE = 1): slot N records N in mscratch and
 * joins the common trap entry, so the test can see which slot was used.
 */
    .align 6
    .globl trap_vector_table
trap_vector_table:
    j       vslot0
    j       vslot_bad
    j       vslot_bad
    j       vslot3
    j       vslot_bad
    j       vslot_bad
    j       vslot_bad
    j       vslot7
    j       vslot_bad
    j       vslot_bad
    j       vslot_bad
    j       vslot11
vslot0:     csrwi mscratch, 0
            j trap_entry
vslot3:     csrwi mscratch, 3
            j trap_entry
vslot7:     csrwi mscratch, 7
            j trap_entry
vslot11:    csrwi mscratch, 11
            j trap_entry
vslot_bad:  csrwi mscratch, 31
            j trap_entry

/*
 * Compressed-instruction workout: exercises the RV32C forms explicitly
 * (the compiler uses many of them anyway). Returns a checksum in a0.
 */
    .option rvc
    .align 2
    .globl rvc_workout
rvc_workout:
    c.addi16sp sp, -32
    c.li    a0, 5           /* a0 = 5 */
    c.addi  a0, 7           /* 12 */
    c.slli  a0, 4           /* 192 */
    c.mv    a1, a0          /* a1 = 192 */
    c.srli  a1, 2           /* 48 */
    c.li    a2, -16
    c.srai  a2, 2           /* -4 */
    c.andi  a1, 0x1c        /* 48 & 28 = 16 */
    c.add   a0, a1          /* 208 */
    c.sub   a0, a2          /* 212 */
    c.xor   a1, a2          /* 16 ^ -4 = -20 */
    c.or    a1, a0          /* -20 | 212 = -12 */
    c.and   a0, a1          /* 212 & -12 = 212 */
    c.lui   a3, 1           /* 0x1000 */
    c.add   a0, a3          /* 0x10D4 */
    c.swsp  a0, 12(sp)
    c.lwsp  a4, 12(sp)
    c.addi4spn a5, sp, 16
    c.sw    a4, 0(a5)
    c.lw    a0, 0(a5)       /* 0x10D4 */
    c.beqz  a3, 1f          /* not taken */
    c.addi  a0, 1           /* 0x10D5 */
1:  c.bnez  a3, 2f          /* taken */
    c.addi  a0, 8           /* skipped */
2:  c.addi16sp sp, 32
    c.jr    ra

    .option norvc
