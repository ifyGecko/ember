; smm-amd.asm - SMM with the AMD64 SMRAM save-state map, for the x86_64-test board
; (tests/boards/: x86_64-ref plus the 0xB2 software SMI port) with -o smm-map=amd64
; (uam-test-run -b x86_64-test -o smm-map=amd64 --raw-base 0 --entry 0).
;
; The SMI handler knows only the AMD64 map (SMBASE + 0xFE00..0xFFFF), as an AMD
; firmware's does: it relocates SMBASE through +0xFF00, sets the saved RAX at
; +0xFFF8, records the saved RIP (+0xFF78), the SMM revision (+0xFEFC), the SMM
; I/O trap dword (+0xFEC0), the saved CS attributes (+0xFE12) and EFER (+0xFED0),
; reads the SMM_BASE MSR (C001_0111h) and can ask for an I/O instruction restart
; (+0xFEC8). SMIs come from the local APIC and from the software SMI port 0xB2,
; in 32-bit protected mode and in 64-bit long mode. The result goes to the test
; finisher at port 0x700: 0x5555 on success, (test << 16) | 0x3333 on the first
; failure.

%define TEST_PORT 0x700
%define PML4      0x10000
%define PDPT      0x11000
%define PD0       0x12000
%define STACK_TOP 0x90000
%define LAPIC     0xFEE00000
%define SMM_COUNT    0x5000      ; SMIs handled
%define SMM_NEWBASE  0x5004      ; non-zero: relocate SMBASE there
%define SMM_RESTART  0x5008      ; non-zero: ask for an I/O instruction restart (once)
%define SMM_RIP      0x500C      ; saved RIP (low dword)
%define SMM_REVID    0x5010      ; SMM revision identifier
%define SMM_IOTRAP   0x5014      ; SMM I/O trap dword
%define SMM_CSATTR   0x5018      ; saved CS attributes
%define SMM_EFER     0x501C      ; saved EFER (low dword)
%define SMM_MSRBASE  0x5020      ; SMM_BASE MSR as read in the handler

; CHECK operand, value, test-number: fail unless operand == value
%macro CHECK 3
        cmp     %1, %2
        je      %%ok
        mov     edi, %3
        jmp     fail
%%ok:
%endmacro

        bits    16
        org     0

start:
        cli
        xor     ax, ax
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     sp, 0x7000
        lgdt    [gdt_desc]
        mov     eax, cr0
        or      eax, 1
        mov     cr0, eax
        jmp     dword 0x08:pm32

; ---------------------------------------------------------------------------
; 32-bit protected mode
; ---------------------------------------------------------------------------
        bits    32
pm32:
        mov     ax, 0x10
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     fs, ax
        mov     gs, ax
        mov     esp, STACK_TOP

        ; the handler at the default SMBASE (0x30000) and at 0x40000
        mov     esi, smi_handler
        mov     edi, 0x38000
        mov     ecx, smi_handler_end - smi_handler
        rep movsb
        mov     esi, smi_handler
        mov     edi, 0x48000
        mov     ecx, smi_handler_end - smi_handler
        rep movsb
        mov     dword [SMM_COUNT], 0
        mov     dword [SMM_RESTART], 0
        mov     dword [SMM_NEWBASE], 0x40000
        or      dword [LAPIC + 0xF0], 0x100         ; APIC software enable

        ; 1-6: an SMI at the default SMBASE; the handler sets the saved RAX and
        ; relocates SMBASE
        xor     eax, eax
        mov     ebp, 0x5A5A
        call    send_smi
        CHECK   eax, 0x1234, 1
        CHECK   ebp, 0x5A5A, 2
        CHECK   dword [SMM_COUNT], 1, 3
        CHECK   dword [SMM_REVID], 0x00030064, 4
        CHECK   dword [SMM_MSRBASE], 0x30000, 5
        mov     eax, [SMM_CSATTR]
        and     eax, 0xFFE                          ; ignore the accessed bit
        CHECK   eax, 0xC9A, 6                       ; code32: P, S, type A; G, D

        ; 7-8: the next SMI runs at the relocated SMBASE (the old copy only RSMs)
        mov     word [0x38000], 0xAA0F              ; RSM
        call    send_smi
        CHECK   dword [SMM_COUNT], 2, 7
        CHECK   dword [SMM_MSRBASE], 0x40000, 8

        ; 9-11: an SMI raised by OUT to the SMI port: the I/O trap dword and the
        ; saved RIP (the next instruction)
        mov     dword [SMM_IOTRAP], 0
        mov     dx, 0xB2
        out     dx, al
after_out:
        CHECK   dword [SMM_COUNT], 3, 9
        CHECK   dword [SMM_IOTRAP], 0x00B20012, 10  ; port B2, byte, valid, OUT
        CHECK   dword [SMM_RIP], after_out, 11

        ; 12: I/O instruction restart: the OUT runs again and raises a second SMI
        mov     dword [SMM_RESTART], 1
        out     dx, al
        CHECK   dword [SMM_COUNT], 5, 12

        ; identity-map 4 GiB with 2 MiB pages and enter long mode
        mov     edi, PML4
        xor     eax, eax
        mov     ecx, 6 * 1024
        rep stosd
        mov     dword [PML4], PDPT | 3
        mov     eax, PD0 | 3
        mov     edi, PDPT
        mov     ecx, 4
.pdpt:  mov     [edi], eax
        add     eax, 0x1000
        add     edi, 8
        loop    .pdpt
        mov     edi, PD0
        mov     eax, 0x83
        mov     ecx, 2048
.pd:    mov     [edi], eax
        add     eax, 0x200000
        add     edi, 8
        loop    .pd
        mov     eax, cr4
        or      eax, 0x20                           ; PAE
        mov     cr4, eax
        mov     eax, PML4
        mov     cr3, eax
        mov     ecx, 0xC0000080
        rdmsr
        or      eax, 0x100                          ; LME
        wrmsr
        mov     eax, cr0
        or      eax, 0x80000000
        mov     cr0, eax
        jmp     0x18:lm64

send_smi:
        mov     dword [LAPIC + 0x310], 0
        mov     dword [LAPIC + 0x300], 0x00040200   ; to self, delivery mode SMI
        nop                                         ; taken at an instruction boundary
        ret

fail:
        mov     eax, edi
        shl     eax, 16
        or      eax, 0x3333
        mov     dx, TEST_PORT
        out     dx, eax
        hlt

; ---------------------------------------------------------------------------
; 64-bit long mode
; ---------------------------------------------------------------------------
        bits    64
lm64:
        ; 20-24: an SMI from long mode returns to long mode with its registers
        mov     r8, 0x1122334455667788
        mov     rbp, 0x0123456789ABCDEF
        xor     eax, eax
        mov     rdi, LAPIC
        mov     dword [rdi + 0x310], 0
        mov     dword [rdi + 0x300], 0x00040200
        nop
        CHECK   eax, 0x1234, 20
        mov     r9, 0x1122334455667788
        CHECK   r8, r9, 21
        mov     r9, 0x0123456789ABCDEF
        CHECK   rbp, r9, 22
        mov     eax, [SMM_CSATTR]
        and     eax, 0xFFE
        CHECK   eax, 0xA9A, 23                      ; code64: P, S, type A; G, L
        mov     eax, [SMM_EFER]
        and     eax, 0x500
        CHECK   eax, 0x500, 24                      ; LME, LMA
        CHECK   dword [SMM_COUNT], 6, 25

        mov     eax, 0x5555
        mov     dx, TEST_PORT
        out     dx, eax
        hlt

; ---------------------------------------------------------------------------
; the SMI handler, copied to SMBASE + 0x8000: real-address mode, 4 GiB limits
; ---------------------------------------------------------------------------
        bits    16
smi_handler:
        inc     dword [SMM_COUNT]
        xor     ebx, ebx
        mov     bx, cs
        shl     ebx, 4                              ; SMBASE
        mov     eax, [ebx + 0xFF78]                 ; saved RIP
        mov     [SMM_RIP], eax
        mov     eax, [ebx + 0xFEFC]                 ; SMM revision
        mov     [SMM_REVID], eax
        mov     eax, [ebx + 0xFEC0]                 ; SMM I/O trap
        mov     [SMM_IOTRAP], eax
        movzx   eax, word [ebx + 0xFE12]            ; saved CS attributes
        mov     [SMM_CSATTR], eax
        mov     eax, [ebx + 0xFED0]                 ; saved EFER
        mov     [SMM_EFER], eax
        mov     ecx, 0xC0010111                     ; SMM_BASE
        rdmsr
        mov     [SMM_MSRBASE], eax
        mov     dword [ebx + 0xFFF8], 0x1234        ; saved RAX
        mov     dword [ebx + 0xFFFC], 0
        mov     eax, [SMM_NEWBASE]
        test    eax, eax
        jz      .norelocate
        mov     [ebx + 0xFF00], eax                 ; saved SMBASE: relocate
        mov     dword [SMM_NEWBASE], 0
.norelocate:
        cmp     dword [SMM_RESTART], 0
        je      .norestart
        mov     dword [SMM_RESTART], 0
        mov     byte [ebx + 0xFEC8], 0xFF           ; I/O instruction restart
.norestart:
        rsm
smi_handler_end:

; ---------------------------------------------------------------------------
; data
; ---------------------------------------------------------------------------
        align   8
gdt:    dq      0
        dq      0x00CF9A000000FFFF      ; 0x08 code32
        dq      0x00CF92000000FFFF      ; 0x10 data
        dq      0x00AF9A000000FFFF      ; 0x18 code64
gdt_end:
gdt_desc:
        dw      gdt_end - gdt - 1
        dd      gdt
