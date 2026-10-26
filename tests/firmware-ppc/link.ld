/* 32-bit PowerPC self-test: linked at physical 0, inside the RAM of the UAM ppc-ref
 * board. The exception vectors sit at their architectural offsets (MSR[IP] = 0: the
 * vector table at 0); the entry point _start follows them at 0x3000. */
OUTPUT_ARCH(powerpc:common)
ENTRY(_start)

MEMORY { RAM (rwx) : ORIGIN = 0x00000000, LENGTH = 16M }

SECTIONS
{
    . = 0x00000000;
    .text : {
        KEEP(*(.text.init))     /* vectors and _start, with internal .org */
        *(.text .text.*)
    } > RAM

    .rodata : { *(.rodata .rodata.* .sdata2 .sdata2.*) } > RAM
    .data   : { *(.data .data.* .sdata .sdata.*) } > RAM
    .bss (NOLOAD) : ALIGN(16) {
        __bss_start = .;
        *(.bss .bss.* .sbss .sbss.* COMMON)
        . = ALIGN(16);
        __bss_end = .;
    } > RAM
    .stack (NOLOAD) : ALIGN(16) {
        . += 0x8000;
        __stack_top = .;
        . += 0x4000;
        __exc_stack_top = .;
    } > RAM
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame) *(.got2) }
}
