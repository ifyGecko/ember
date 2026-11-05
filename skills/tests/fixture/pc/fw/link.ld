/* link.ld - the PC fixture firmware: the top 64 KiB of the boot flash, run in place. */
OUTPUT_FORMAT("elf32-i386")
OUTPUT_ARCH(i386)
ENTRY(reset_vector)
SECTIONS
{
    . = 0xFFFF0000;
    .text : { KEEP(*(.start16)) *(.text .text.*) KEEP(*(.smi)) }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.* .bss .bss.* COMMON) }
    . = 0xFFFFFFF0;
    .reset : { KEEP(*(.reset)) }
    /DISCARD/ : { *(.comment) *(.note .note.*) *(.eh_frame*) }
    gdt_desc_off = gdt_desc & 0xFFFF;
}
ASSERT(SIZEOF(.data) == 0, "the firmware runs from flash: no writable globals")
