/* Xtensa images for xtensa-ref: RAM from 0x40000000, where VECBASE points at reset. The
 * start code's vector table (.vectors, with its own .org offsets) comes first; _start is
 * at the model's reset vector inside it. */
OUTPUT_ARCH(xtensa)
ENTRY(_start)

MEMORY { RAM (rwx) : ORIGIN = 0x40000000, LENGTH = 4M }

SECTIONS
{
    .text : {
        KEEP(*(.vectors))
        *(.literal .text .literal.* .text.*)
    } > RAM
    .rodata : { *(.rodata .rodata.*) } > RAM
    .data : ALIGN(16) { *(.data .data.*) } > RAM
    .bss (NOLOAD) : ALIGN(16) {
        __bss_start = .;
        *(.bss .bss.* COMMON)
        . = ALIGN(16);
        __bss_end = .;
    } > RAM
    .stack (NOLOAD) : ALIGN(16) {
        . += 0x10000;
        __stack_top = .;
        . += 0x2000;
        __exc_stack_top = .;
    } > RAM
}
