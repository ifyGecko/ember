; selftest.asm - x86-64 ISA self-test for the x86_64-ref board.
;
; A flat image loaded at physical 0 and entered in real mode at 0
; (uam-run -b x86_64-ref --raw-base 0 --entry 0). It tests real mode, then
; switches to 32-bit protected mode and to 64-bit long mode (identity-mapped
; with 2 MiB pages), checking arithmetic, flags, string instructions,
; interrupts and exceptions through the IVT and the IDT, paging faults,
; SYSCALL and SSE along the way, and SMM: SMIs sent through the local APIC,
; SMBASE relocation, a handler changing the saved state, the SMM range
; registers (SMRR), and an SMI taken from long mode. The result goes to the test finisher at
; port 0x700: 0x5555 on success, (test << 16) | 0x3333 on the first failure.

%define TEST_PORT 0x700
%define COM1      0x3F8

%define IDT_BASE  0x20000
%define PML4      0x10000
%define PDPT      0x11000
%define PD0       0x12000
%define SCRATCH   0x30000
%define STACK_TOP 0x90000
%define LAPIC     0xFEE00000
%define SMM_COUNT    0x5000      ; SMIs handled
%define SMM_NEWBASE  0x5004      ; non-zero: relocate SMBASE there
%define SMM_SET_SMRR 0x5008      ; non-zero: program the SMRR for 0x40000-0x4FFFF
%define SMM_PROBE    0x500C      ; a dword of 0x48000 as read inside SMM
%define SMM_IOMISC   0x5010      ; the save state's IOMisc, as the handler saw it
%define SMM_RESTART  0x5014      ; non-zero: ask for an I/O instruction restart (once)

; CHECK reg, value, test-number: fail unless reg == value (32-bit immediates)
%macro CHECK 3
        cmp     %1, %2
        je      %%ok
        mov     edi, %3
        jmp     FAIL
%%ok:
%endmacro

; FAILIF cc, test-number: fail if the condition holds
%macro FAILIF 2
        j%-1    %%ok
        mov     edi, %2
        jmp     FAIL
%%ok:
%endmacro

; ---------------------------------------------------------------------------
; real mode
; ---------------------------------------------------------------------------
        bits    16
        org     0
%define FAIL fail16

start:
        cli
        xor     ax, ax
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     sp, 0x7000
        cld
        mov     si, msg_real
        call    puts16

        ; 1-3: add, carry, adc
        mov     ax, 0xFFFF
        add     ax, 1
        FAILIF  nc, 2                   ; before CHECK: its CMP changes CF
        CHECK   ax, 0, 1
        mov     ax, 0xFFFF
        mov     dx, 1
        add     ax, 1
        adc     dx, 0
        CHECK   dx, 2, 3

        ; 4-6: mul / div
        mov     ax, 300
        mov     bx, 7
        mul     bx
        CHECK   ax, 2100, 4
        mov     ax, 2101
        xor     dx, dx
        div     bx
        CHECK   ax, 300, 5
        CHECK   dx, 1, 6

        ; 7-9: rotates and shifts
        mov     al, 0x81
        rol     al, 1
        CHECK   al, 0x03, 7
        mov     ax, 0x8000
        sar     ax, 15
        CHECK   ax, 0xFFFF, 8
        mov     ax, 1
        mov     cl, 4
        shl     ax, cl
        CHECK   ax, 16, 9

        ; 10-11: rep stosw / rep movsb / repe cmpsb
        mov     di, 0x3000
        mov     ax, 0xA55A
        mov     cx, 8
        rep stosw
        mov     si, 0x3000
        mov     di, 0x3100
        mov     cx, 16
        rep movsb
        mov     si, 0x3000
        mov     di, 0x3100
        mov     cx, 16
        repe cmpsb
        FAILIF  ne, 10
        CHECK   cx, 0, 11

        ; 12: INT n through the IVT
        mov     word [0x80 * 4], int80_16
        mov     word [0x80 * 4 + 2], 0
        mov     ax, 5
        int     0x80
        CHECK   ax, 6, 12

        ; 13: #DE through the IVT (the handler skips the 2-byte DIV)
        mov     word [0], de_16
        mov     word [2], 0
        xor     si, si
        xor     bx, bx
        mov     ax, 1
        xor     dx, dx
        div     bx
        CHECK   si, 0x1234, 13

        ; 14-15: PUSHF / LAHF see CF
        stc
        pushf
        pop     ax
        test    al, 1
        FAILIF  z, 14
        clc
        lahf
        test    ah, 1
        FAILIF  nz, 15

        ; 16: 32-bit operations in real mode
        mov     eax, 0x12345678
        ror     eax, 8
        CHECK   eax, 0x78123456, 16

        ; 17: mov moffs, xlat
        mov     byte [0x3205], 0x77
        mov     bx, 0x3200
        mov     al, 5
        xlatb
        CHECK   al, 0x77, 17

        ; enter 32-bit protected mode
        lgdt    [gdt_desc]
        mov     eax, cr0
        or      al, 1
        mov     cr0, eax
        jmp     dword 0x08:pm32

int80_16:
        inc     ax
        iret

de_16:
        push    bp
        mov     bp, sp
        add     word [bp + 2], 2
        pop     bp
        mov     si, 0x1234
        iret

puts16:
        mov     dx, COM1
.next:  lodsb
        test    al, al
        jz      .done
        out     dx, al
        jmp     .next
.done:  ret

fail16:
        mov     eax, edi
        shl     eax, 16
        or      eax, 0x3333
        mov     dx, TEST_PORT
        out     dx, eax
        hlt
        jmp     fail16

; ---------------------------------------------------------------------------
; 32-bit protected mode
; ---------------------------------------------------------------------------
        bits    32
%undef FAIL
%define FAIL fail32

pm32:
        mov     ax, 0x10
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     fs, ax
        mov     gs, ax
        mov     esp, STACK_TOP
        mov     esi, msg_prot
        call    puts32

        ; 20: imul with an immediate
        mov     eax, -3
        imul    eax, eax, 7
        CHECK   eax, -21, 20
        ; 21-22: idiv
        mov     eax, -22
        cdq
        mov     ecx, 7
        idiv    ecx
        CHECK   eax, -3, 21
        CHECK   edx, -1, 22
        ; 23-24: bsf / bsr
        mov     eax, 0xF0
        bsf     ecx, eax
        CHECK   ecx, 4, 23
        bsr     ecx, eax
        CHECK   ecx, 7, 24
        ; 25-26: bts / bt
        xor     eax, eax
        bts     eax, 5
        CHECK   eax, 32, 25
        bt      eax, 5
        FAILIF  nc, 26
        ; 27-28: cmov / setcc
        xor     ecx, ecx
        mov     eax, 1
        cmp     eax, 1
        cmove   ecx, eax
        sete    dl
        CHECK   ecx, 1, 27
        CHECK   dl, 1, 28
        ; 29-30: lock cmpxchg
        mov     dword [SCRATCH], 5
        mov     eax, 5
        mov     ecx, 9
        lock cmpxchg [SCRATCH], ecx
        FAILIF  ne, 29
        CHECK   dword [SCRATCH], 9, 30
        ; 31-33: xadd, bswap, movsx
        mov     eax, 10
        mov     dword [SCRATCH], 32
        xadd    [SCRATCH], eax
        CHECK   eax, 32, 31
        mov     eax, 0x11223344
        bswap   eax
        CHECK   eax, 0x44332211, 32
        mov     byte [SCRATCH], 0x80
        movsx   eax, byte [SCRATCH]
        CHECK   eax, 0xFFFFFF80, 33
        ; 34: shld
        mov     eax, 0x80000001
        mov     edx, 0xF0000000
        shld    eax, edx, 4
        CHECK   eax, 0x1F, 34
        ; 35: call / ret with enter / leave
        mov     eax, 3
        call    sub_triple
        CHECK   eax, 9, 35

        ; IDT: #DE, #UD, #GP, int3
        mov     edi, IDT_BASE
        xor     eax, eax
        mov     ecx, 256 * 2
        rep stosd
        mov     eax, 3
        mov     ebx, int3_32
        call    set_gate32
        mov     eax, 6
        mov     ebx, ud_32
        call    set_gate32
        mov     eax, 13
        mov     ebx, gp_32
        call    set_gate32
        lidt    [idt32_desc]

        ; 36: int3
        xor     ebp, ebp
        int3
        CHECK   ebp, 1, 36
        ; 37: #UD from ud2 (the handler skips it)
        xor     ebp, ebp
        ud2
        CHECK   ebp, 2, 37
        ; 38-39: #GP with an error code from a selector past the GDT limit
        xor     ebp, ebp
        mov     ax, 0x1000
        mov     ds, ax
        CHECK   ebp, 3, 38
        CHECK   esi, 0x1000, 39
        mov     ax, 0x10
        mov     ds, ax

        ; SMM: the handler at the default SMBASE (0x30000) and at 0x40000
        mov     esi, smi_handler
        mov     edi, 0x38000
        mov     ecx, smi_handler_end - smi_handler
        rep movsb
        mov     esi, smi_handler
        mov     edi, 0x48000
        mov     ecx, smi_handler_end - smi_handler
        rep movsb
        mov     dword [SMM_COUNT], 0
        mov     dword [SMM_IOMISC], 0
        mov     dword [SMM_RESTART], 0
        mov     dword [SMM_NEWBASE], 0x40000
        mov     dword [SMM_SET_SMRR], 0
        or      dword [LAPIC + 0xF0], 0x100        ; APIC software enable
        ; 70-72: an SMI; the handler sets the saved RAX and relocates SMBASE
        xor     eax, eax
        mov     ebp, 0x5A5A
        call    send_smi32
        CHECK   eax, 0x1234, 70
        CHECK   ebp, 0x5A5A, 71
        CHECK   dword [SMM_COUNT], 1, 72
        ; 73-74: the next SMI runs at the new SMBASE (the old copy only RSMs)
        mov     word [0x38000], 0xAA0F              ; RSM
        mov     dword [SMM_SET_SMRR], 1
        xor     eax, eax
        call    send_smi32
        CHECK   dword [SMM_COUNT], 2, 73
        CHECK   eax, 0x1234, 74
        ; 75-76: SMRR: all ones outside SMM, the handler's bytes inside
        CHECK   dword [0x48000], 0xFFFFFFFF, 75
        mov     eax, [SMM_PROBE]
        cmp     eax, [smi_handler]
        FAILIF  ne, 76

        ; 67-69, 77-79, 96-97: SMIs raised by I/O instructions (the software
        ; SMI port 0xB2) and I/O instruction restart
        CHECK   dword [SMM_IOMISC], 0, 67           ; the APIC's SMIs were not I/O
        mov     ebx, [SMM_COUNT]
        mov     dword [SMM_RESTART], 1
        mov     al, 0x5A
        mov     dx, 0xB2
        out     dx, al                              ; SMI; restarted: a second one
        mov     ecx, [SMM_COUNT]
        sub     ecx, ebx
        CHECK   ecx, 2, 68
        CHECK   dword [SMM_IOMISC], 0x00B20003, 69  ; port B2, OUT DX, byte, SMI
        out     0xB2, al
        CHECK   dword [SMM_IOMISC], 0x00B20083, 77  ; OUT imm8
        mov     ebx, [SMM_COUNT]
        mov     dword [SMM_RESTART], 1
        mov     esi, fp_vec
        mov     ecx, 3
        cld
        rep outsb                                   ; 3 SMIs, the first restarted: 4
        mov     edx, [SMM_COUNT]
        sub     edx, ebx
        CHECK   edx, 4, 78
        CHECK   ecx, 0, 79
        CHECK   esi, fp_vec + 3, 96
        CHECK   dword [SMM_IOMISC], 0x00B20063, 97  ; REP OUTS

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
        or      eax, 0x220              ; PAE, OSFXSR
        mov     cr4, eax
        mov     eax, PML4
        mov     cr3, eax
        mov     ecx, 0xC0000080
        rdmsr
        or      eax, 0x901              ; LME, NXE, SCE
        wrmsr
        mov     eax, cr0
        or      eax, 0x80000000
        mov     cr0, eax
        jmp     0x18:lm64

send_smi32:
        mov     dword [LAPIC + 0x310], 0
        mov     dword [LAPIC + 0x300], 0x00040200   ; to self, delivery mode SMI
        nop                                         ; taken at an instruction boundary
        ret

sub_triple:
        enter   8, 0
        mov     [ebp - 4], eax
        add     eax, [ebp - 4]
        add     eax, [ebp - 4]
        leave
        ret

; eax = vector, ebx = handler
set_gate32:
        lea     edi, [IDT_BASE + eax * 8]
        mov     [edi], bx
        mov     word [edi + 2], 0x08
        mov     word [edi + 4], 0x8E00
        shr     ebx, 16
        mov     [edi + 6], bx
        ret

int3_32:
        inc     ebp
        iretd
ud_32:
        add     dword [esp], 2
        mov     ebp, 2
        iretd
gp_32:
        pop     esi                     ; error code: the selector
        add     dword [esp], 2          ; skip MOV DS, AX
        mov     ebp, 3
        iretd

puts32:
        mov     dx, COM1
.next:  lodsb
        test    al, al
        jz      .done
        out     dx, al
        jmp     .next
.done:  ret

fail32:
        mov     eax, edi
        shl     eax, 16
        or      eax, 0x3333
        mov     dx, TEST_PORT
        out     dx, eax
        hlt
        jmp     fail32

; ---------------------------------------------------------------------------
; 64-bit long mode
; ---------------------------------------------------------------------------
        bits    64
%undef FAIL
%define FAIL fail64

; CHECK64 reg, imm64, test-number
%macro CHECK64 3
        mov     r15, %2
        cmp     %1, r15
        je      %%ok
        mov     edi, %3
        jmp     FAIL
%%ok:
%endmacro

lm64:
        mov     ax, 0x20
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     rsp, STACK_TOP
        lea     rsi, [rel msg_long]
        call    puts64

        ; 40: REX registers and 64-bit immediates
        mov     r8, 0x123456789ABCDEF0
        mov     r9, r8
        shr     r9, 32
        CHECK64 r9, 0x12345678, 40
        ; 41: a 32-bit write zero-extends
        mov     rax, -1
        mov     eax, 1
        CHECK64 rax, 1, 41
        ; 42-43: 64-bit mul
        mov     rax, -1
        mov     rbx, 2
        mul     rbx
        CHECK64 rdx, 1, 42
        CHECK64 rax, 0xFFFFFFFFFFFFFFFE, 43
        ; 44-45: 128-bit div
        mov     rdx, 1
        xor     eax, eax
        mov     rbx, 2
        div     rbx
        CHECK64 rax, 0x8000000000000000, 44
        CHECK64 rdx, 0, 45
        ; 46-47: idiv
        mov     rax, -7
        cqo
        mov     rcx, 2
        idiv    rcx
        CHECK64 rax, -3, 46
        CHECK64 rdx, -1, 47
        ; 48-49: cmpxchg16b
        mov     rdi, SCRATCH
        mov     qword [rdi], 11
        mov     qword [rdi + 8], 22
        mov     rax, 11
        mov     rdx, 22
        mov     rbx, 33
        mov     rcx, 44
        lock cmpxchg16b [rdi]
        FAILIF  ne, 48
        CHECK64 qword [rdi + 8], 44, 49
        ; 50: rep movsq / rep stosq
        mov     rdi, SCRATCH + 0x100
        mov     rax, 0x0102030405060708
        mov     rcx, 4
        rep stosq
        mov     rsi, SCRATCH + 0x100
        mov     rdi, SCRATCH + 0x200
        mov     rcx, 4
        rep movsq
        CHECK64 qword [SCRATCH + 0x218], 0x0102030405060708, 50
        ; 51-53: SSE2
        movdqu  xmm0, [rel vec_a]
        movdqu  xmm1, [rel vec_b]
        paddd   xmm0, xmm1
        movq    rax, xmm0
        CHECK64 rax, 0x0000000700000005, 51
        pxor    xmm2, xmm2
        pcmpeqb xmm2, xmm2
        pmovmskb eax, xmm2
        CHECK   eax, 0xFFFF, 52
        pshufd  xmm3, xmm0, 0x1B
        movd    eax, xmm3
        CHECK   eax, 11, 53
        ; 54-55: bit scans, popcnt
        mov     rax, 0x8000000000000010
        bsr     rcx, rax
        CHECK   ecx, 63, 54
        popcnt  rcx, rax
        CHECK   ecx, 2, 55
        ; 56: RIP-relative store and load
        mov     dword [rel rip_slot], 0xCAFE
        mov     eax, [rel rip_slot]
        CHECK   eax, 0xCAFE, 56

        ; IDT for long mode: #PF, int 0x40
        mov     rdi, IDT_BASE
        xor     eax, eax
        mov     ecx, 256 * 4
        rep stosd
        mov     eax, 14
        lea     rbx, [rel pf_64]
        call    set_gate64
        mov     eax, 0x40
        lea     rbx, [rel int40_64]
        call    set_gate64
        lidt    [rel idt64_desc]

        ; 57: INT n through the 64-bit IDT
        xor     ebp, ebp
        int     0x40
        CHECK   ebp, 0x40, 57
        ; 58-59: #PF on an unmapped address (above 4 GiB); the handler skips the load
        xor     ebp, ebp
        mov     rbx, 0x0000000140000000
        mov     rax, [rbx]
        CHECK   ebp, 14, 58
        CHECK64 r12, 0x0000000140000000, 59

        ; 60: SYSCALL to a 64-bit handler at CPL 0
        mov     ecx, 0xC0000081         ; STAR: SYSCALL CS = 0x18
        xor     eax, eax
        mov     edx, 0x00000018
        wrmsr
        mov     ecx, 0xC0000082         ; LSTAR
        lea     rax, [rel syscall_64]
        mov     rdx, rax
        shr     rdx, 32
        wrmsr
        xor     ebp, ebp
        syscall
        CHECK   ebp, 0x5C, 60

        ; 62-64: an SMI from long mode returns to long mode
        mov     r8, 0x1122334455667788
        xor     eax, eax
        mov     rdi, LAPIC
        mov     dword [rdi + 0x310], 0
        mov     dword [rdi + 0x300], 0x00040200
        nop
        CHECK   eax, 0x1234, 62
        CHECK64 r8, 0x1122334455667788, 63
        CHECK   dword [SMM_COUNT], 10, 64

        ; 65-66: HPET: 14.31818 MHz period, and the counter runs once enabled
        mov     rdi, 0xFED00000
        mov     eax, [rdi + 4]
        CHECK   eax, 69841279, 65
        mov     dword [rdi + 0x10], 1               ; ENABLE_CNF
        mov     rax, [rdi + 0xF0]
        mov     ecx, 1000
.spin:  loop    .spin
        mov     rbx, [rdi + 0xF0]
        cmp     rbx, rax
        FAILIF  be, 66

        ; 80-86: SSE/SSE2 floating point
        mov     eax, 3
        cvtsi2sd xmm0, eax                  ; 3.0
        mov     eax, 4
        cvtsi2sd xmm1, eax                  ; 4.0
        mulsd   xmm0, xmm0
        mulsd   xmm1, xmm1
        addsd   xmm0, xmm1                  ; 25.0
        sqrtsd  xmm0, xmm0                  ; 5.0
        cvttsd2si eax, xmm0
        CHECK   eax, 5, 80
        mov     eax, 7
        cvtsi2ss xmm2, eax
        mov     eax, 2
        cvtsi2ss xmm3, eax
        divss   xmm2, xmm3                  ; 3.5
        cvtss2si eax, xmm2                  ; round to nearest even: 4
        CHECK   eax, 4, 81
        cvttss2si eax, xmm2                 ; truncate: 3
        CHECK   eax, 3, 82
        comiss  xmm3, xmm2                  ; 2.0 < 3.5
        FAILIF  ae, 83
        cvtss2sd xmm4, xmm2
        ucomisd xmm4, xmm4
        FAILIF  ne, 84
        movdqu  xmm5, [rel fp_vec]          ; 1.5 2.5 -1.0 8.0
        movdqu  xmm6, [rel fp_vec + 16]     ; 0.5 0.5 0.5 0.5
        addps   xmm5, xmm6                  ; 2.0 3.0 -0.5 8.5
        cmpltps xmm6, xmm5                  ; 0.5 < x: T T F T
        movmskps eax, xmm6
        CHECK   eax, 0xB, 85
        cvtps2dq xmm7, xmm5                 ; 2 3 0 8 (nearest even)
        movd    eax, xmm7
        pshufd  xmm7, xmm7, 0xFF
        movd    ecx, xmm7
        add     eax, ecx
        CHECK   eax, 10, 86

        ; 90-95: x2APIC mode and the TSC-deadline timer
        cli
        mov     eax, 1
        cpuid
        bt      ecx, 21                     ; x2APIC
        FAILIF  nc, 90
        bt      ecx, 24                     ; TSC-deadline
        FAILIF  nc, 90
        mov     ecx, 0x1B                   ; IA32_APIC_BASE: EN | EXTD
        rdmsr
        or      eax, 0xC00
        wrmsr
        mov     ecx, 0x802                  ; x2APIC ID
        rdmsr
        CHECK   eax, 0, 91
        mov     ecx, 0x80F                  ; SVR: software enable
        mov     eax, 0x1FF
        xor     edx, edx
        wrmsr
        mov     ecx, 0x83F                  ; SELF IPI, vector 0x41
        mov     eax, 0x41
        wrmsr
        mov     ecx, 0x822                  ; IRR[95:64]
        rdmsr
        CHECK   eax, 2, 92
        mov     ecx, 0x832                  ; LVT timer: vector 0x40, TSC-deadline
        mov     eax, 0x40 | (2 << 17)
        xor     edx, edx
        wrmsr
        rdtsc
        shl     rdx, 32
        or      rax, rdx
        add     rax, 3000
        mov     rdx, rax
        shr     rdx, 32
        mov     ecx, 0x6E0                  ; IA32_TSC_DEADLINE
        wrmsr
        mov     ecx, 0x822
        rdmsr
        CHECK   eax, 2, 93                  ; not yet
        mov     ecx, 4000
.dl:    loop    .dl
        mov     ecx, 0x822
        rdmsr
        CHECK   eax, 3, 94                  ; vector 0x40 pending now
        mov     ecx, 0x6E0                  ; the deadline disarmed itself
        rdmsr
        or      eax, edx
        CHECK   eax, 0, 95

        ; 61: CPUID vendor
        xor     eax, eax
        cpuid
        CHECK   ebx, 0x784D4155, 61     ; "UAMx"

        lea     rsi, [rel msg_pass]
        call    puts64
        mov     eax, 0x5555
        mov     dx, TEST_PORT
        out     dx, eax
        hlt

; eax = vector, rbx = handler
set_gate64:
        shl     eax, 4
        lea     rdi, [IDT_BASE + rax]
        mov     [rdi], bx
        mov     word [rdi + 2], 0x18
        mov     word [rdi + 4], 0x8E00
        shr     rbx, 16
        mov     [rdi + 6], bx
        shr     rbx, 16
        mov     [rdi + 8], ebx
        mov     dword [rdi + 12], 0
        ret

int40_64:
        mov     ebp, 0x40
        iretq
pf_64:
        mov     r12, cr2
        add     rsp, 8                  ; error code
        add     qword [rsp], 3          ; skip MOV RAX, [RBX]
        mov     ebp, 14
        iretq
syscall_64:
        mov     ebp, 0x5C
        push    r11
        popfq
        jmp     rcx

puts64:
        mov     dx, COM1
.next:  lodsb
        test    al, al
        jz      .done
        out     dx, al
        jmp     .next
.done:  ret

fail64:
        mov     eax, edi
        shl     eax, 16
        or      eax, 0x3333
        mov     dx, TEST_PORT
        out     dx, eax
        hlt
        jmp     fail64

; ---------------------------------------------------------------------------
; SMI handler: copied to SMBASE + 0x8000, runs in SMM's real-address mode
; with CS.base = SMBASE and flat data segments
; ---------------------------------------------------------------------------
        bits    16
smi_handler:
        inc     dword [SMM_COUNT]
        xor     ebx, ebx
        mov     bx, cs
        shl     ebx, 4                              ; SMBASE
        mov     dword [ebx + 0xFF5C], 0x1234        ; saved RAX
        mov     dword [ebx + 0xFF60], 0
        mov     eax, [SMM_NEWBASE]
        test    eax, eax
        jz      .norelocate
        mov     [ebx + 0xFEF8], eax                 ; saved SMBASE: relocate
        mov     dword [SMM_NEWBASE], 0
.norelocate:
        cmp     dword [SMM_SET_SMRR], 0
        je      .nosmrr
        mov     dword [SMM_SET_SMRR], 0
        mov     ecx, 0x1F2                          ; IA32_SMRR_PHYSBASE: 0x40000, WB
        mov     eax, 0x00040006
        xor     edx, edx
        wrmsr
        mov     ecx, 0x1F3                          ; IA32_SMRR_PHYSMASK: 64 KiB, valid
        mov     eax, 0xFFFF0800
        mov     edx, 0x7F
        wrmsr
.nosmrr:
        mov     eax, [dword 0x48000]
        mov     [SMM_PROBE], eax
        mov     eax, [ebx + 0xFFA4]                 ; IOMisc
        mov     [SMM_IOMISC], eax
        cmp     dword [SMM_RESTART], 0
        je      .norestart
        mov     dword [SMM_RESTART], 0
        mov     byte [ebx + 0xFF00], 0xFF           ; I/O instruction restart
.norestart:
        rsm
smi_handler_end:

; ---------------------------------------------------------------------------
; data
; ---------------------------------------------------------------------------
        align   16
fp_vec: dd 1.5, 2.5, -1.0, 8.0
        dd 0.5, 0.5, 0.5, 0.5
vec_a:  dd      2, 3, 4, 5
vec_b:  dd      3, 4, 5, 6
rip_slot: dd    0

        align   8
gdt:    dq      0
        dq      0x00CF9A000000FFFF      ; 0x08 code32
        dq      0x00CF92000000FFFF      ; 0x10 data
        dq      0x00AF9A000000FFFF      ; 0x18 code64
        dq      0x00CF92000000FFFF      ; 0x20 data
gdt_end:
gdt_desc:
        dw      gdt_end - gdt - 1
        dd      gdt
idt32_desc:
        dw      256 * 8 - 1
        dd      IDT_BASE
idt64_desc:
        dw      256 * 16 - 1
        dq      IDT_BASE

msg_real: db    "x86 selftest: real", 0
msg_prot: db    " prot", 0
msg_long: db    " long", 0
msg_pass: db    " PASS", 13, 10, 0
