/* The self-test: as link.ld, but the literal pool (32-bit words) is a section of its own,
 * apart from the code and from the byte strings, so that le2be.py can lay each out the
 * big-endian way. */
OUTPUT_ARCH(xtensa)
ENTRY(_start)

MEMORY { RAM (rwx) : ORIGIN = 0x40000000, LENGTH = 4M }

SECTIONS
{
    .vectors : { KEEP(*(.vectors)) } > RAM
    .lit4 : ALIGN(4) { *(.literal .literal.*) } > RAM
    .text : ALIGN(4) { *(.text .text.*) } > RAM
    .data : ALIGN(16) { *(.data .data.*) } > RAM
    .strings : { *(.strings) } > RAM
    .xt.prop 0 : { *(.xt.prop .xt.prop.*) }
    .xt.lit 0 : { *(.xt.lit .xt.lit.*) }
}
