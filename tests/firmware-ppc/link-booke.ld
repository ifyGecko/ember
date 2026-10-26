/* 32-bit PowerPC Book E self-test: the program at physical 0 in the RAM of ppc-ref,
 * as for the classic build, plus the boot page at 0xfffff000 (in the board's boot
 * ROM), which the processor can fetch at reset through the TLB entry it starts
 * with. The entry point is the reset vector 0xfffffffc. */
OUTPUT_ARCH(powerpc:common)
ENTRY(_reset)

MEMORY {
    RAM  (rwx) : ORIGIN = 0x00000000, LENGTH = 16M
    BOOT (rx)  : ORIGIN = 0xfffff000, LENGTH = 4K
}

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
    .boot : { KEEP(*(.boot)) } > BOOT
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame) *(.got2) }
}
