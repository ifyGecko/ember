; smp.asm - multiprocessor start-up test for the x86_64-ref board (-o cpus=N).
;
; A flat image entered in real mode at physical 0. The bootstrap processor
; enables its APIC in x2APIC mode and broadcasts INIT and two startup IPIs
; (vector 0x08) to every other processor. Each application processor starts
; at 0x8000 in real mode, sets the bit of its APIC ID in a mask and counts
; itself in. The bootstrap processor waits for N - 1 arrivals (N from
; CPUID) and checks the mask.
;
; Then every processor appends its APIC ID to a shared log K times (an
; atomic index, a delay of varying length between entries), and the
; bootstrap processor prints an FNV-1a hash of the log on COM1. The hash
; depends on exactly how the processors were interleaved, so a run split by
; a snapshot must print the same one. The result goes to the test finisher
; at I/O port 0x700: 0x5555 pass, (code << 16) | 0x3333 fail.

        bits    16
        org     0

MASK    equ     0x500
COUNT   equ     0x504
IDX     equ     0x508                   ; next free log entry
DONE    equ     0x50C                   ; processors that finished their entries
NCPU    equ     0x510
LOG     equ     0xA000                  ; N * K bytes (N <= 4)
K       equ     1000
TEST_PORT equ   0x700
COM1    equ     0x3F8

start:
        cli
        xor     ax, ax
        mov     ds, ax
        mov     ss, ax
        mov     sp, 0x7000
        mov     dword [MASK], 0
        mov     dword [COUNT], 0
        mov     dword [IDX], 0
        mov     dword [DONE], 0

        mov     ecx, 0x1B               ; IA32_APIC_BASE: EN | EXTD (x2APIC)
        rdmsr
        or      eax, 0xC00
        wrmsr
        mov     ecx, 0x80F              ; SVR: enable
        mov     eax, 0x1FF
        xor     edx, edx
        wrmsr
        mov     ecx, 0x830              ; ICR: INIT, assert, all excluding self
        mov     eax, 0x000C4500
        xor     edx, edx
        wrmsr
        mov     eax, 0x000C4608         ; startup IPI, vector 0x08
        wrmsr
        wrmsr                           ; the second one is ignored by started APs

        mov     eax, 1                  ; expected: CPUID.1 EBX[23:16] - 1
        cpuid
        shr     ebx, 16
        and     ebx, 0xFF
        dec     ebx
        jz      fail1                   ; run with -o cpus=2 or more
        mov     esi, 2000000
.wait:  cmp     [COUNT], ebx
        je      .arrived
        dec     esi
        jnz     .wait
        mov     edi, 2                  ; 2: not every AP arrived
        jmp     fail
.arrived:
        mov     ecx, ebx                ; mask = bits 1..N-1
        inc     ecx
        mov     eax, 1
        shl     eax, cl
        sub     eax, 2
        cmp     [MASK], eax
        mov     edi, 3                  ; 3: wrong APIC IDs
        jne     fail
        inc     ebx
        mov     [NCPU], ebx

        xor     ebx, ebx                ; the bootstrap processor is APIC ID 0
        call    work
.wdone: mov     eax, [NCPU]
        cmp     [DONE], eax
        jne     .wdone
        imul    eax, eax, K
        mov     edi, 4                  ; 4: entries lost
        cmp     [IDX], eax
        jne     fail

        mov     ecx, eax                ; FNV-1a over the log
        mov     esi, LOG
        mov     edi, 0x811C9DC5
.hash:  movzx   eax, byte [esi]
        xor     edi, eax
        imul    edi, edi, 0x01000193
        inc     esi
        dec     ecx
        jnz     .hash
        mov     si, msg
.msg:   mov     al, [si]
        test    al, al
        jz      .hex
        call    putc
        inc     si
        jmp     .msg
.hex:   mov     cx, 8
.digit: rol     edi, 4
        mov     eax, edi
        and     al, 0x0F
        add     al, '0'
        cmp     al, '9'
        jbe     .put
        add     al, 'a' - '0' - 10
.put:   call    putc
        loop    .digit
        mov     al, 10
        call    putc

        mov     eax, 0x5555
        mov     dx, TEST_PORT
        out     dx, eax
done:   hlt
        jmp     done
fail1:  mov     edi, 1
fail:   shl     edi, 16
        mov     eax, edi
        or      eax, 0x3333
        mov     dx, TEST_PORT
        out     dx, eax
        jmp     done

putc:   mov     dx, COM1
        out     dx, al
        ret

msg:    db      "smp log hash ", 0

; Append APIC ID bl to the log K times, then count this processor done.
work:   xor     bp, bp
.entry: mov     eax, 1
        lock xadd [IDX], eax
        mov     [LOG + eax], bl
        mov     cx, bp                  ; delay: 1..32 rounds, by entry and processor
        imul    cx, cx, 7
        movzx   dx, bl
        imul    dx, dx, 13
        add     cx, dx
        and     cx, 31
        inc     cx
.delay: loop    .delay
        inc     bp
        cmp     bp, K
        jb      .entry
        lock inc dword [DONE]
        ret

        times   0x8000 - ($ - $$) db 0

; ---- application processors: real mode at 0x8000 (CS = 0x0800) ---------
ap:
        cli
        xor     ax, ax
        mov     ds, ax
        mov     eax, 1
        cpuid
        shr     ebx, 24                 ; initial APIC ID
        lock bts dword [MASK], ebx
        lock inc dword [COUNT]
        jmp     0:ap_work               ; on with CS = 0, like the bootstrap processor
ap_work:
        call    work
.h:     hlt
        jmp     .h
