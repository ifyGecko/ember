/* AVR32A images for avr32-ref -o model=a: code, read-only and initialised data in the
 * code memory at the reset address 0x80000000 (writable on this board, so .data needs
 * no copy); .bss and the stack in RAM. */
OUTPUT_ARCH(avr32:uc)
ENTRY(_start)

MEMORY {
    CODE (rwx) : ORIGIN = 0x80000000, LENGTH = 8M
    RAM  (rw)  : ORIGIN = 0x00001000, LENGTH = 16M - 4K
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
