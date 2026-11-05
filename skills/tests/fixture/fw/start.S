    .set noreorder
    .set noat
    .section .text.init, "ax"
    .globl _start
_start:
    j       main_entry
    nop
    .org 0x180
exc_vector:                     /* ack timer + intc, count, return */
    lui     $k0, 0xb804
    li      $k1, 1
    sw      $k1, 0xc($k0)
    lui     $k0, 0xb805
    li      $k1, 8
    sw      $k1, 0($k0)
    lui     $k0, %hi(g_ticks)
    lw      $k1, %lo(g_ticks)($k0)
    addiu   $k1, $k1, 1
    sw      $k1, %lo(g_ticks)($k0)
    eret
    nop
main_entry:
    jal     cmain               /* a0, sp come from the board's entry hook */
    nop
1:  b       1b
    nop
