/* Linker script for the self-test firmware: everything in DRAM at 0x80000000
 * (the reset/entry address of both UAM's and QEMU's `virt` board). */
OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY
{
    DRAM (rwx) : ORIGIN = 0x80000000, LENGTH = 128M
}

SECTIONS
{
    .text : {
        KEEP(*(.text.init))
        *(.text .text.*)
    } > DRAM

    .rodata : { *(.rodata .rodata.* .srodata .srodata.*) } > DRAM

    .data : { *(.data .data.* .sdata .sdata.*) } > DRAM

    .bss (NOLOAD) : ALIGN(16) {
        __bss_start = .;
        *(.bss .bss.* .sbss .sbss.* COMMON)
        . = ALIGN(16);
        __bss_end = .;
    } > DRAM

    .stack (NOLOAD) : ALIGN(16) {
        . += 0x4000;
        __stack_top = .;
    } > DRAM
}
