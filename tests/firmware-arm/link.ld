/* ARMv7-A self-test and fuzz target: linked at 0x00100000 in the RAM of arm-ref.
 * VBAR points at the vectors at the start of .text. Each exception mode gets its
 * own stack. */
OUTPUT_ARCH(arm)
ENTRY(_start)

MEMORY { RAM (rwx) : ORIGIN = 0x00100000, LENGTH = 16M }

SECTIONS
{
    . = 0x00100000;
    .text : {
        KEEP(*(.text.init))
        *(.text .text.*)
    } > RAM

    .rodata : { *(.rodata .rodata.*) } > RAM
    .ARM.exidx : { *(.ARM.exidx*) } > RAM
    .data   : { *(.data .data.*) } > RAM
    .bss (NOLOAD) : ALIGN(16) {
        __bss_start = .;
        *(.bss .bss.* COMMON)
        . = ALIGN(16);
        __bss_end = .;
    } > RAM
    .stack (NOLOAD) : ALIGN(16) {
        . += 0x2000; __stack_und = .;
        . += 0x2000; __stack_abt = .;
        . += 0x2000; __stack_irq = .;
        . += 0x2000; __stack_fiq = .;
        . += 0x4000; __stack_usr = .;
        . += 0x8000; __stack_top = .;
    } > RAM
}
