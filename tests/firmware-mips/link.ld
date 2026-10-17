/* MIPS32 self-test: linked into kseg0 at 0x80100000 (physical 0x00100000),
 * matching where QEMU's mipssim -kernel places an ELF and where the UAM
 * mipssim board maps RAM. The exception vectors sit at fixed offsets from
 * EBase (0x80100000): general exceptions at +0x180. */
OUTPUT_ARCH(mips)
ENTRY(_start)

MEMORY { RAM (rwx) : ORIGIN = 0x80100000, LENGTH = 16M }

SECTIONS
{
    . = 0x80100000;
    .text : {
        KEEP(*(.text.init))     /* _start + vectors, with internal .org */
        *(.text .text.*)
    } > RAM

    .rodata : { *(.rodata .rodata.* .srodata .srodata.*) } > RAM
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
    } > RAM
}
