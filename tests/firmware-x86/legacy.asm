; legacy.asm - protected-mode legacy features on the x86_64-ref board: hardware
; task switches, call gates, the I/O permission bitmap and virtual-8086 mode.
;
; A flat image entered in real mode at physical 0. It enters 32-bit protected
; mode (no paging) and runs, in task A:
;   1  CALL to TSS B, which sees NT and its back link and IRETs back
;   2  CALL through a task gate to B again (B resumes after its IRET)
;   3  #DE through a task gate to task D, which fixes A's saved ECX in A's TSS;
;      the DIV is retried when D IRETs back and succeeds
;   4  JMP to the 16-bit TSS C, whose 16-bit code JMPs back to TSS A
;   5  ring 3: a CALL through a call gate to ring 0 with two parameters copied
;      (RETF 8 back), OUT to an allowed and a denied port (I/O bitmap)
;   6  virtual-8086 mode with IOPL 0: CLI, OUT to a denied port, INT n and
;      PUSHF fault (#GP), an allowed OUT works; INT3 leaves for ring 0, which
;      sees VM set and null data segments
;   7  virtual-8086 mode with IOPL 3: CLI/STI, PUSHF/POPF and INT n work, INT n
;      reaches a ring 0 handler and comes back, IRET works within the mode
;   8  (first) the A20 gate: with the 8042's output port and port 0x92 both
;      leaving A20 off, 1 MiB + x wraps to x; port 0x92 turns it back on
;   9  MMX: MOVD/MOVQ, PADDB, PSHUFW, PACKUSWB, PCMPEQB, PMOVMSKB, PSRLQ, EMMS
; The result goes to the test finisher at I/O port 0x700: 0x5555 pass,
; (n << 16) | 0x3333 fail at check n (0x6n: part n of check 6).

        bits    16
        org     0

VARS    equ     0x5000                  ; variables (v86 reaches them with DS = 0x0100)
GP_N    equ     VARS + 0x00             ; #GP count
GP_VM   equ     VARS + 0x04             ; #GPs taken with VM set in the frame
GP_FDS  equ     VARS + 0x08             ; DS from a v86 #GP frame
MARK_B  equ     VARS + 0x10
MARK_C  equ     VARS + 0x14
MARK_D  equ     VARS + 0x18
G_P0    equ     VARS + 0x20             ; call gate: parameters and CPL seen
G_P1    equ     VARS + 0x24
G_CPL   equ     VARS + 0x28
BP_VM   equ     VARS + 0x30             ; INT3 from v86: VM in the frame
BP_DS   equ     VARS + 0x34             ; DS at handler entry (null)
PHASE   equ     VARS + 0x38
V_INT   equ     VARS + 0x3C             ; INT 0x31 handler: VM in its frame
V_DONE  equ     VARS + 0x40             ; v86 IOPL 3 checkpoints
IDT     equ     0x7000
TSS_A   equ     0x6000
TSS_B   equ     0x6100
TSS_D   equ     0x6200
TSS_C   equ     0x6300
STK0    equ     0x9000                  ; ring 0 of task A
STK0T   equ     0x8800                  ; TSS A's ESP0 (ring 3 / v86 -> ring 0)
STK3    equ     0x8000                  ; ring 3
STKB    equ     0xA000
STKD    equ     0xA800
STKC    equ     0xB000
TEST_PORT equ   0x700

start:
        cli
        xor     ax, ax
        mov     ds, ax
        lgdt    [gdtr]
        mov     eax, cr0
        or      al, 1
        mov     cr0, eax
        jmp     dword 0x08:pm

        align   8
gdt:    dq      0
        ; base, limit, access, flags
%macro DESC 4
        dw      (%2) & 0xFFFF, (%1) & 0xFFFF
        db      ((%1) >> 16) & 0xFF, %3, (((%2) >> 16) & 0x0F) | ((%4) << 4), ((%1) >> 24) & 0xFF
%endmacro
%macro GATE 4                           ; selector, offset, access, parameter count
        dw      (%2) & 0xFFFF, %1
        db      %4, %3
        dw      (%2) >> 16
%endmacro
        DESC    0, 0xFFFFF, 0x9A, 0xC   ; 0x08 code32 DPL 0
        DESC    0, 0xFFFFF, 0x92, 0xC   ; 0x10 data32 DPL 0
        DESC    0, 0xFFFFF, 0xFA, 0xC   ; 0x18 code32 DPL 3
        DESC    0, 0xFFFFF, 0xF2, 0xC   ; 0x20 data32 DPL 3
        DESC    TSS_A, 0x88, 0x89, 0    ; 0x28 TSS A, I/O bitmap for ports 0-255
        DESC    TSS_B, 0x67, 0x89, 0    ; 0x30 TSS B
        GATE    0x08, gate_fn - $$, 0xEC, 2   ; 0x38 32-bit call gate, DPL 3, 2 parameters
        DESC    TSS_C, 0x2B, 0x81, 0    ; 0x40 16-bit TSS C
        DESC    0, 0xFFFF, 0x9A, 0      ; 0x48 code16 DPL 0
        GATE    0x30, 0, 0x85, 0        ; 0x50 task gate to TSS B
        DESC    TSS_D, 0x67, 0x89, 0    ; 0x58 TSS D
        DESC    0, 0xFFFF, 0x92, 0      ; 0x60 data16 DPL 0
gdt_end:
gdtr:   dw      gdt_end - gdt - 1
        dd      gdt

; ---- 32-bit protected mode, task A ----------------------------------------
        bits    32
pm:     mov     ax, 0x10
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     fs, ax
        mov     gs, ax
        mov     esp, STK0
        xor     eax, eax                ; clear the variables and the TSSes
        mov     edi, VARS
        mov     ecx, 0x100
        rep     stosd
        mov     edi, TSS_A
        mov     ecx, 0x400 / 4
        rep     stosd

        ; TSS A: ring 0 stack, I/O bitmap at 0x68 (port 0x81 denied, 0xFF ends it)
        mov     dword [TSS_A + 0x04], STK0T
        mov     dword [TSS_A + 0x08], 0x10
        mov     word [TSS_A + 0x66], 0x68
        mov     byte [TSS_A + 0x68 + 0x10], 0x02
        mov     byte [TSS_A + 0x88], 0xFF
        mov     ax, 0x28
        ltr     ax
        ; TSS B and D: flat ring 0 tasks
        mov     ebx, TSS_B
        mov     eax, task_b
        mov     edx, STKB
        call    init_tss32
        mov     ebx, TSS_D
        mov     eax, task_d
        mov     edx, STKD
        call    init_tss32
        ; TSS C (16-bit): code16 at task_c, data16 everywhere else
        mov     word [TSS_C + 0x0E], task_c
        mov     word [TSS_C + 0x10], 0x0002
        mov     word [TSS_C + 0x1A], STKC
        mov     word [TSS_C + 0x22], 0x60
        mov     word [TSS_C + 0x24], 0x48
        mov     word [TSS_C + 0x26], 0x60
        mov     word [TSS_C + 0x28], 0x60
        ; the IDT
        mov     eax, task_gate_d        ; #DE: task gate to D
        mov     ecx, 0
        call    set_raw_gate
        mov     eax, bp_fn
        mov     ecx, 3
        mov     edx, 0xEE00             ; 32-bit interrupt gate, DPL 3
        call    set_gate
        mov     eax, gp_fn
        mov     ecx, 13
        mov     edx, 0x8E00             ; DPL 0
        call    set_gate
        mov     eax, vint_fn
        mov     ecx, 0x31
        mov     edx, 0xEE00
        call    set_gate
        mov     eax, ring0_fn
        mov     ecx, 0x80
        mov     edx, 0xEE00
        call    set_gate
        lidt    [idtr]

        ; 8: the A20 gate (port 0x92 bit 1 is clear after reset)
        mov     edi, 8
        mov     al, 0xD1                ; 8042: write the output port, A20 off
        out     0x64, al
        mov     al, 0xDD
        out     0x60, al
        mov     dword [0x105080], 0xA20A20      ; wraps to 0x5080
        cmp     dword [VARS + 0x80], 0xA20A20
        jne     fail
        in      al, 0x92                ; fast A20 on
        or      al, 2
        out     0x92, al
        mov     dword [0x105080], 0x12345678
        cmp     dword [VARS + 0x80], 0xA20A20
        jne     fail
        cmp     dword [0x105080], 0x12345678
        jne     fail

        ; 1: CALL to TSS B and back
        mov     ebx, 0x12345678
        call    0x30:0
        mov     edi, 1
        cmp     dword [MARK_B], 0xB
        jne     fail
        cmp     ebx, 0x12345678         ; A's registers came back from its TSS
        jne     fail
        str     ax
        cmp     ax, 0x28
        jne     fail
        cmp     byte [gdt + 0x30 + 5], 0x89     ; B no longer busy, A busy
        jne     fail
        cmp     byte [gdt + 0x28 + 5], 0x8B
        jne     fail
        ; 2: through the task gate
        mov     dword [MARK_B], 0
        call    0x50:0
        mov     edi, 2
        cmp     dword [MARK_B], 0xB
        jne     fail
        ; 3: #DE through a task gate; D sets A's saved ECX to 5
        mov     eax, 100
        xor     edx, edx
        xor     ecx, ecx
        div     ecx
        mov     edi, 3
        cmp     eax, 20
        jne     fail
        cmp     dword [MARK_D], 0xD
        jne     fail
        ; 4: JMP to the 16-bit TSS C, which JMPs back here
        mov     esi, 0xCAFE
        jmp     0x40:0
after_c:
        mov     edi, 4
        cmp     dword [MARK_C], 0xC
        jne     fail
        cmp     esi, 0xCAFE
        jne     fail
        cmp     byte [gdt + 0x40 + 5], 0x81     ; C left by JMP: not busy
        jne     fail
        clts

        ; 9: MMX
        mov     edi, 9
        mov     eax, 0x01020304
        movd    mm0, eax
        mov     eax, 0x10203040
        movd    mm1, eax
        paddb   mm0, mm1
        movd    eax, mm0
        cmp     eax, 0x11223344
        jne     fail
        movq    mm2, [mmx_words]        ; words 1, 2, 3, 4
        pshufw  mm3, mm2, 0x1B          ; 4, 3, 2, 1
        movq    [mmx_out], mm3
        cmp     dword [mmx_out], 0x00030004
        jne     fail
        cmp     dword [mmx_out + 4], 0x00010002
        jne     fail
        packuswb mm3, mm2               ; bytes 4 3 2 1 1 2 3 4
        movq    [mmx_out], mm3
        cmp     dword [mmx_out], 0x01020304
        jne     fail
        cmp     dword [mmx_out + 4], 0x04030201
        jne     fail
        pcmpeqb mm4, mm4
        pmovmskb eax, mm4
        cmp     eax, 0xFF
        jne     fail
        psrlq   mm4, 60
        movd    eax, mm4
        cmp     eax, 0xF
        jne     fail
        emms

        ; 5: ring 3 through RETF; it comes back with INT 0x80
        mov     dword [r0_next], after_r3
        push    dword 0x23
        push    dword STK3
        push    dword 0x1B
        push    dword ring3
        retf
after_r3:
        mov     ax, 0x10
        mov     ds, ax
        mov     es, ax
        mov     edi, 5
        cmp     dword [G_P0], 0x22222222
        jne     fail
        cmp     dword [G_P1], 0x11111111
        jne     fail
        cmp     dword [G_CPL], 0
        jne     fail
        cmp     dword [r3_esp], STK3
        jne     fail
        cmp     dword [GP_N], 1         ; the denied OUT
        jne     fail

        ; 6: virtual-8086 mode, IOPL 0
        mov     dword [PHASE], 1
        mov     eax, 0x00020002         ; VM, IOPL 0
        call    enter_v86
        ; (bp_fn continues at v86_back)
v86_back:
        mov     esp, STK0
        mov     edi, 0x60               ; 0x6n: the n-th check
        cmp     dword [GP_N], 5         ; + CLI, OUT 0x81, INT 0x31, PUSHF
        jne     fail
        inc     edi
        cmp     dword [GP_VM], 4
        jne     fail
        inc     edi
        cmp     dword [GP_FDS], 0x0100
        jne     fail
        inc     edi
        cmp     word [0x1010], 0x1234   ; DS:0x10 in the v86 task
        jne     fail
        inc     edi
        cmp     dword [BP_VM], 1
        jne     fail
        inc     edi
        cmp     dword [BP_DS], 0
        jne     fail
        ; 7: virtual-8086 mode, IOPL 3
        mov     dword [PHASE], 2
        mov     eax, 0x00023002         ; VM, IOPL 3
        call    enter_v86
v86_back2:
        mov     esp, STK0
        mov     edi, 7
        cmp     dword [GP_N], 5         ; nothing faulted
        jne     fail
        cmp     dword [V_INT], 1
        jne     fail
        cmp     dword [V_DONE], 0x3333
        jne     fail

        mov     eax, 0x5555
        mov     dx, TEST_PORT
        out     dx, eax
done:   hlt
        jmp     done
fail:   shl     edi, 16
        mov     eax, edi
        or      eax, 0x3333
        mov     dx, TEST_PORT
        out     dx, eax
        jmp     done

; ebx = TSS, eax = EIP, edx = ESP: a flat ring 0 task
init_tss32:
        mov     [ebx + 0x20], eax
        mov     dword [ebx + 0x24], 0x2
        mov     [ebx + 0x38], edx
        mov     dword [ebx + 0x48], 0x10        ; ES
        mov     dword [ebx + 0x4C], 0x08        ; CS
        mov     dword [ebx + 0x50], 0x10        ; SS
        mov     dword [ebx + 0x54], 0x10        ; DS
        mov     dword [ebx + 0x58], 0x10        ; FS
        mov     dword [ebx + 0x5C], 0x10        ; GS
        ret

; eax = handler, ecx = vector, edx = access word
set_gate:
        mov     [IDT + ecx * 8], ax
        mov     word [IDT + ecx * 8 + 2], 0x08
        mov     [IDT + ecx * 8 + 4], dx
        shr     eax, 16
        mov     [IDT + ecx * 8 + 6], ax
        ret
; eax = address of an 8-byte gate, ecx = vector
set_raw_gate:
        mov     edx, [eax]
        mov     [IDT + ecx * 8], edx
        mov     edx, [eax + 4]
        mov     [IDT + ecx * 8 + 4], edx
        ret
task_gate_d:
        dw      0, 0x58
        db      0, 0x85
        dw      0

idtr:   dw      0x7FF
        dd      IDT

; eax = EFLAGS: IRET into virtual-8086 mode at v86_code (DS = 0x0100)
enter_v86:
        push    dword 0                 ; GS
        push    dword 0                 ; FS
        push    dword 0x0100            ; DS
        push    dword 0                 ; ES
        push    dword 0x0C00            ; SS
        push    dword 0x0400            ; ESP
        push    eax                     ; EFLAGS
        push    dword (v86_code - $$) >> 4
        push    dword 0
        iretd

; ---- tasks ------------------------------------------------------------------
task_b: pushfd
        pop     eax
        test    eax, 0x4000             ; NT
        jz      .bad
        str     ax
        cmp     ax, 0x30
        jne     .bad
        cmp     word [TSS_B], 0x28      ; back link
        jne     .bad
        mov     ebx, 0xBBBB             ; A's EBX is restored from A's TSS
        mov     dword [MARK_B], 0xB
.bad:   iretd
        jmp     task_b

task_d: mov     dword [TSS_A + 0x2C], 5 ; A's ECX: the DIV succeeds when retried
        mov     dword [MARK_D], 0xD
        iretd
        jmp     task_d

; ---- ring 3 -------------------------------------------------------------------
ring3:  mov     ax, 0x23                ; RETF to an outer level left DS/ES null
        mov     ds, ax
        mov     es, ax
        push    dword 0x11111111
        push    dword 0x22222222
        call    0x3B:0                  ; the call gate (RPL 3)
        mov     [r3_esp], esp
        mov     al, 0x5A
        out     0x80, al                ; allowed by the bitmap
        out     0x81, al                ; denied: #GP, skipped
        int     0x80                    ; back to ring 0

gate_fn:                                ; ring 0 through the gate: EIP, CS, P0, P1, ESP, SS
        mov     eax, [esp + 8]
        mov     [G_P0], eax
        mov     eax, [esp + 12]
        mov     [G_P1], eax
        mov     ax, cs
        and     eax, 3
        mov     [G_CPL], eax
        retf    8

ring0_fn:                               ; INT 0x80: continue in ring 0
        mov     ax, 0x10
        mov     ds, ax
        jmp     [r0_next]

; #GP: count it, note a v86 frame, skip the faulting instruction
gp_fn:  add     esp, 4                  ; the error code
        push    ds
        push    eax
        push    ebx
        mov     ax, 0x10
        mov     ds, ax
        inc     dword [GP_N]
        mov     eax, [esp + 12]         ; EIP
        mov     ebx, [esp + 16]         ; CS
        test    dword [esp + 20], 0x20000
        jz      .flat
        inc     dword [GP_VM]
        push    eax
        mov     eax, [esp + 4 + 36]     ; DS of the v86 frame (EIP CS EFLAGS ESP SS ES DS)
        mov     [GP_FDS], eax
        pop     eax
        shl     ebx, 4
        add     ebx, eax
        jmp     .len
.flat:  mov     ebx, eax
.len:   movzx   ebx, byte [ebx]         ; the opcode: one or two bytes long
        cmp     bl, 0xE6                ; OUT imm8
        je      .two
        cmp     bl, 0xCD                ; INT n
        je      .two
        inc     dword [esp + 12]
        jmp     .out
.two:   add     dword [esp + 12], 2
.out:   pop     ebx
        pop     eax
        pop     ds
        iretd

; INT3: leaves virtual-8086 mode for good
bp_fn:  mov     ax, ds
        movzx   eax, ax
        mov     bx, 0x10
        mov     ds, bx
        mov     [BP_DS], eax
        mov     eax, [esp + 8]          ; EFLAGS
        shr     eax, 17
        and     eax, 1
        mov     [BP_VM], eax
        cmp     dword [PHASE], 1
        je      v86_back
        jmp     v86_back2

; INT 0x31 from v86 at IOPL 3: note it and return
vint_fn:
        push    ds
        push    eax
        mov     ax, 0x10
        mov     ds, ax
        mov     eax, [esp + 16]         ; EFLAGS
        shr     eax, 17
        and     eax, 1
        mov     [V_INT], eax
        pop     eax
        pop     ds
        iretd

        align   8
mmx_words: dw   1, 2, 3, 4
mmx_out: dq     0
r0_next: dd     0
r3_esp:  dd     0

; ---- the 16-bit task C ----------------------------------------------------------
        bits    16
task_c: mov     dword [MARK_C], 0xC
        jmp     0x28:0                  ; back to task A, after its JMP
        jmp     task_c

; ---- virtual-8086 code: CS = its address >> 4, DS = 0x0100 ----------------------
        align   16
v86_code:
        mov     word [0x0010], 0x1234
        cmp     dword [PHASE - 0x1000], 2
        je      .iopl3
        cli                             ; #GP (IOPL 0)
        mov     al, 0x5A
        out     0x80, al                ; allowed by the bitmap
        out     0x81, al                ; #GP
        int     0x31                    ; #GP (IOPL 0)
        pushf                           ; #GP (IOPL 0)
        int3                            ; to ring 0
.iopl3: cli
        sti
        pushf
        popf
        int     0x31                    ; to the ring 0 handler and back
        mov     word [V_DONE - 0x1000], 0x1111
        push    word 0x0002             ; IRET within v86: FLAGS, CS, IP
        push    cs
        push    word .after - v86_code
        iret
        jmp     .bad
.after: or      word [V_DONE - 0x1000], 0x2222
.bad:   int3
