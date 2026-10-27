/*
 * x86-start.S - Entry for the benchmark on x86_64-ref: a flat image loaded at
 * physical 0 and entered in real mode at 0 (--raw-base 0 --entry 0). Switches to
 * protected mode and then to long mode with the low 1 GiB identity-mapped in 2 MiB
 * pages, calls main, and writes its return value to the test finisher (port 0x700).
 */
    .section .text.init, "ax"
    .code16
    .globl _start
_start:
    cli
    xorw    %ax, %ax
    movw    %ax, %ds
    lgdtl   gdt_desc
    movl    %cr0, %eax
    orl     $1, %eax                    /* PE */
    movl    %eax, %cr0
    ljmpl   $0x08, $pm32

    .code32
pm32:
    movw    $0x10, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    movl    $pml4, %edi                 /* clear the three tables */
    movl    $3 * 1024, %ecx
    xorl    %eax, %eax
    rep stosl
    movl    $pdpt + 3, pml4
    movl    $pd + 3, pdpt
    movl    $pd, %edi                   /* 512 x 2 MiB: present, writable, large */
    movl    $0x83, %eax
1:  movl    %eax, (%edi)
    addl    $0x200000, %eax
    addl    $8, %edi
    cmpl    $pd + 4096, %edi
    jne     1b
    movl    %cr4, %eax
    orl     $0x20, %eax                 /* PAE */
    movl    %eax, %cr4
    movl    $pml4, %eax
    movl    %eax, %cr3
    movl    $0xC0000080, %ecx           /* EFER.LME */
    rdmsr
    orl     $0x100, %eax
    wrmsr
    movl    %cr0, %eax
    orl     $0x80000000, %eax           /* PG */
    movl    %eax, %cr0
    ljmpl   $0x18, $lm64

    .code64
lm64:
    movw    $0x20, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    movq    $__stack_top, %rsp
    movq    $__bss_start, %rdi          /* zero .bss */
    movq    $__bss_end, %rcx
    subq    %rdi, %rcx
    xorl    %eax, %eax
    rep stosb
    call    main
    movw    $0x700, %dx                 /* main's return value to the test finisher */
    outl    %eax, %dx
2:  hlt
    jmp     2b

    .p2align 3
gdt:
    .quad   0
    .quad   0x00CF9A000000FFFF          /* 0x08 code32 */
    .quad   0x00CF92000000FFFF          /* 0x10 data */
    .quad   0x00AF9A000000FFFF          /* 0x18 code64 */
    .quad   0x00CF92000000FFFF          /* 0x20 data */
gdt_desc:
    .word   gdt_desc - gdt - 1
    .long   gdt
