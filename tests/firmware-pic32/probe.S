/*
 * probe.S - Delay-slot probe, assembled in the self-test's main ISA (MIPS32,
 * MIPS16e or microMIPS): bd_probe(p) returns *p, loaded in the delay slot of
 * the JR that returns. A fault there reports EPC = the JR, Cause.BD = 1.
 */
    .set noreorder
    .set noat
    .text
    .globl bd_probe
    .ent bd_probe
bd_probe:
    move    $3, $31
    jr      $3
    lw      $2, 0($4)
    .end bd_probe
