/* PIC32 self-test: code in program flash (kseg0 0x9d000000, the vectors first:
 * EBase), data and stacks in the first 32 KiB of RAM (kseg0 0x80000000), which
 * every pic32-ref model has. */
OUTPUT_ARCH(mips)
ENTRY(_start)

MEMORY {
    FLASH (rx)  : ORIGIN = 0x9d000000, LENGTH = 256K
    RAM   (rwx) : ORIGIN = 0x80000000, LENGTH = 32K
}

SECTIONS
{
    .text : {
        KEEP(*(.text.vectors))
        *(.text .text.*)
    } > FLASH
    .rodata : { *(.rodata .rodata.* .srodata .srodata.*) } > FLASH

    .data   : { *(.data .data.* .sdata .sdata.*) } > RAM
    .bss (NOLOAD) : ALIGN(16) {
        __bss_start = .;
        *(.bss .bss.* .sbss .sbss.* COMMON)
        . = ALIGN(16);
        __bss_end = .;
    } > RAM
    .stack (NOLOAD) : ALIGN(16) {
        . += 0x1800;
        __stack_top = .;
        . += 0x800;
        __trap_stack = .;
    } > RAM
}
