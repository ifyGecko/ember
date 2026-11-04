/* The benchmark on pic32-ref (tests/bench): the PIC32 self-test's layout (code in program
 * flash at kseg0 0x9d000000, the vectors first: EBase) with 128 KiB of RAM at kseg0
 * 0x80000000; xbench runs it with --ram 0x20000, as mm has only 32 KiB. */
OUTPUT_ARCH(mips)
ENTRY(_start)

MEMORY {
    FLASH (rx)  : ORIGIN = 0x9d000000, LENGTH = 256K
    RAM   (rwx) : ORIGIN = 0x80000000, LENGTH = 128K
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
        . += 0x4000;
        __stack_top = .;
        . += 0x800;
        __trap_stack = .;
    } > RAM
}
