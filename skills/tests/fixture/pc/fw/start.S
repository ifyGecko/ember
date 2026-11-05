/*
 * start.S - reset vector and mode switch of the PC fixture firmware: the processor leaves
 * reset in real mode at 0xFFFFFFF0 (CS base 0xFFFF0000); load a flat GDT, enter 32-bit
 * protected mode and call fw_main with a stack in RAM. The code runs in place from flash.
 * Also holds the SMI handler fw_main copies to SMBASE + 0x8000 (16-bit SMM code).
 */
    .section .start16, "ax"
    .code16
    .globl start16
start16:
    cli
    cld
    movl    %eax, %ebp              /* BIST result */
    movw    $gdt_desc_off, %bx      /* offset in the 64 KiB segment (CS base 0xFFFF0000) */
    lgdtl   %cs:(%bx)
    movl    %cr0, %eax
    orl     $1, %eax
    movl    %eax, %cr0
    ljmpl   $0x08, $start32

    .code32
start32:
    movw    $0x10, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    movw    %ax, %fs
    movw    %ax, %gs
    movl    $0x80000, %esp
    pushl   %ebp
    call    fw_main
1:  hlt
    jmp     1b

    .balign 8
gdt:
    .quad   0
    .quad   0x00cf9b000000ffff      /* 0x08: code, flat 4 GiB */
    .quad   0x00cf93000000ffff      /* 0x10: data, flat 4 GiB */
gdt_end:
    .globl gdt_desc
gdt_desc:
    .word   gdt_end - gdt - 1
    .long   gdt

/*
 * SMI handler, entered at SMBASE + 0x8000 in SMM's real-mode-like state (segment bases 0
 * except CS). Records that it ran and what TSEG holds as seen from SMM, acknowledges the
 * APM SMI in the ICH9 ACPI block (PMBASE 0x600) and returns.
 */
    .section .smi, "ax"
    .code16
    .globl smi_handler, smi_handler_end
smi_handler:
    movl    $0x534d4931, %eax       /* "SMI1" */
    movl    %eax, 0x5000
    movl    $0x03f00000, %esi       /* TSEG (top 1 MiB of 64 MiB): RAM in SMM */
    addr32 movl (%esi), %eax
    movl    %eax, 0x5004
    movw    $0x634, %dx             /* SMI_STS: clear APM_STS */
    movl    $0x20, %eax
    outl    %eax, %dx
    movw    $0x630, %dx             /* SMI_EN: GBL_SMI_EN | EOS | APMC_EN */
    movl    $0x23, %eax
    outl    %eax, %dx
    rsm
smi_handler_end:

    .section .reset, "ax"
    .code16
    .globl reset_vector
reset_vector:                       /* 0xFFFFFFF0 */
    jmp     start16
    .fill   16 - (. - reset_vector), 1, 0xff
