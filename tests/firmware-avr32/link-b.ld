/* AVR32B images for avr32-ref -o model=b: code, read-only and initialised data in the
 * code memory (physical 0) through P2 from the reset address 0xa0000000 (writable on
 * this board, so .data needs no copy); .bss and the stack in RAM (physical 0x10000000)
 * through P1. */
OUTPUT_ARCH(avr32:ap)
ENTRY(_start)

MEMORY {
    CODE (rwx) : ORIGIN = 0xa0000000, LENGTH = 8M
    RAM  (rw)  : ORIGIN = 0x90000000, LENGTH = 32M
}

SECTIONS
{
    .text : {
        KEEP(*(.text.init))
        KEEP(*(.text.evba))
        *(.text .text.*)
    } > CODE
    .rodata : { *(.rodata .rodata.*) } > CODE
    .data   : { *(.data .data.*) } > CODE
    .bss (NOLOAD) : ALIGN(16) {
        __bss_start = .;
        *(.bss .bss.* COMMON)
        . = ALIGN(16);
        __bss_end = .;
    } > RAM
    .stack (NOLOAD) : ALIGN(16) {
        . += 0x4000;
        __stack_top = .;
    } > RAM
}
