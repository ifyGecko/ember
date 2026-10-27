/* The benchmark on x86_64-ref: a flat image at physical 0 (x86-start.S first). Page
 * tables and the stack follow .bss; everything stays below 0x90000. */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

SECTIONS
{
    . = 0;
    .text : {
        KEEP(*(.text.init))
        *(.text .text.*)
    }
    .rodata : { *(.rodata .rodata.*) }
    .data   : { *(.data .data.*) }
    .bss (NOLOAD) : ALIGN(16) {
        __bss_start = .;
        *(.bss .bss.* COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }
    .pagetables (NOLOAD) : ALIGN(4096) {
        pml4 = .; . += 4096;
        pdpt = .; . += 4096;
        pd = .;   . += 4096;
    }
    .stack (NOLOAD) : ALIGN(16) {
        . += 0x10000;
        __stack_top = .;
    }
    ASSERT(__stack_top <= 0x90000, "the benchmark must fit below 0x90000")
    /DISCARD/ : { *(.note* .comment .eh_frame*) }
}
