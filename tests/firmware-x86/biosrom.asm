; biosrom.asm - a legacy-BIOS-style boot ROM for the x86_64-ref board
; (--image bios=biosrom.bin), the flow of a PC BIOS in miniature on the
; generic legacy devices:
;
;   reset vector (F000:FFF0) -> F000:start, real mode
;   1  the 8259s, then A20 on through the 8042's output port (checked)
;   2  VGA: the mode 03h register set, "BIOS OK" on the text screen
;   3  floppy: reset the 82077, the four reset SENSE INTERRUPT STATUS, SPECIFY,
;      RECALIBRATE, then READ DATA of the boot sector through 8237 channel 2
;      to 0000:7C00 (55AA checked)
;   4  IDE: IDENTIFY DEVICE and READ SECTORS (PIO, REP INSW) from the primary
;      master; sector 1 of the test disk holds 0x01 bytes
;   5  "BIOS OK" to the printer, then into the boot sector, which reports to
;      the test finisher (0x5555)
; A failed step writes (step << 16) | 0x3333 to the finisher at port 0x700.

        bits    16
        org     0                       ; CS = 0xF000: the image is F0000-FFFFF

TEST_PORT equ   0x700

start:  cli
        cld
        xor     ax, ax
        mov     ss, ax
        mov     sp, 0x7000
        mov     ds, ax
        mov     es, ax
        mov     al, 0x01
        out     0x80, al

        ; 1: 8259s (ICW1-4: vectors 08h/70h, cascade on IRQ 2), all masked
        mov     al, 0x11
        out     0x20, al
        out     0xA0, al
        mov     al, 0x08
        out     0x21, al
        mov     al, 0x70
        out     0xA1, al
        mov     al, 0x04
        out     0x21, al
        mov     al, 0x02
        out     0xA1, al
        mov     al, 0x01
        out     0x21, al
        out     0xA1, al
        mov     al, 0xFF
        out     0x21, al
        out     0xA1, al
        ; A20 on through the 8042, then check that 1 MiB does not wrap
        mov     al, 0xD1
        out     0x64, al
        mov     al, 0xDF
        out     0x60, al
        mov     word [0x0500], 0x1234
        mov     ax, 0xFFFF
        mov     fs, ax
        mov     word [fs:0x0510], 0x5678        ; linear 0x100500
        mov     di, 1
        cmp     word [0x0500], 0x1234
        jne     fail

        ; 2: VGA mode 03h
        mov     dx, 0x3C2
        mov     al, 0x67
        out     dx, al
        mov     si, vga_seq
        mov     dx, 0x3C4
        mov     cx, 5
        call    idx_out
        mov     dx, 0x3D4                       ; unprotect CRTC 0-7
        mov     ax, 0x0011
        out     dx, ax
        mov     si, vga_crtc
        mov     cx, 25
        call    idx_out
        mov     si, vga_gc
        mov     dx, 0x3CE
        mov     cx, 9
        call    idx_out
        mov     dx, 0x3DA
        in      al, dx                          ; attribute flip-flop to index
        mov     dx, 0x3C0
        mov     si, vga_attr
        xor     bx, bx
.attr:  mov     al, bl
        out     dx, al
        mov     al, [cs:si + bx]
        out     dx, al
        inc     bx
        cmp     bx, 21
        jb      .attr
        mov     al, 0x20
        out     dx, al
        mov     ax, 0xB800
        mov     es, ax
        xor     di, di
        mov     si, msg
        mov     ah, 0x07
.text:  mov     al, [cs:si]
        test    al, al
        jz      .text_done
        stosw
        inc     si
        jmp     .text
.text_done:
        xor     ax, ax
        mov     es, ax

        ; 3: floppy boot sector through DMA channel 2
        mov     di, 3
        mov     dx, 0x3F2
        xor     al, al
        out     dx, al                          ; controller reset
        mov     al, 0x0C
        out     dx, al                          ; out of reset, DMA and IRQ on
        mov     cx, 4
.sense: push    cx
        mov     al, 0x08
        call    fdc_out
        call    fdc_in
        call    fdc_in
        pop     cx
        loop    .sense
        mov     al, 0x03                        ; SPECIFY: DMA mode
        call    fdc_out
        mov     al, 0xDF
        call    fdc_out
        mov     al, 0x02
        call    fdc_out
        mov     dx, 0x3F2
        mov     al, 0x1C                        ; motor A on
        out     dx, al
        mov     al, 0x07                        ; RECALIBRATE drive 0
        call    fdc_out
        xor     al, al
        call    fdc_out
        mov     al, 0x08
        call    fdc_out
        call    fdc_in
        cmp     al, 0x20                        ; ST0: seek end, drive 0
        jne     fail
        call    fdc_in
        mov     al, 0x06                        ; DMA channel 2: 512 bytes to 0000:7C00
        out     0x0A, al
        out     0x0C, al
        mov     al, 0x46
        out     0x0B, al
        xor     al, al
        out     0x04, al
        mov     al, 0x7C
        out     0x04, al
        xor     al, al
        out     0x81, al
        mov     al, 0xFF
        out     0x05, al
        mov     al, 0x01
        out     0x05, al
        mov     al, 0x02
        out     0x0A, al
        mov     si, fd_read                     ; READ DATA C0 H0 R1, 512-byte sectors
        mov     cx, 9
.rd:    mov     al, [cs:si]
        call    fdc_out
        inc     si
        loop    .rd
        call    fdc_in                          ; ST0
        test    al, 0xC0
        jnz     fail
        mov     cx, 6
.res:   call    fdc_in
        loop    .res
        cmp     word [0x7DFE], 0xAA55
        jne     fail

        ; 4: IDE primary master
        mov     di, 4
        mov     dx, 0x1F6
        mov     al, 0xA0
        out     dx, al
        mov     dx, 0x1F7
        mov     al, 0xEC                        ; IDENTIFY DEVICE
        out     dx, al
        call    ide_drq
        mov     di, 0x8000
        mov     cx, 256
        mov     dx, 0x1F0
        rep     insw
        mov     di, 4
        test    word [0x8000], 0x8000           ; an ATA device
        jnz     fail
        cmp     word [0x8000 + 60 * 2], 128     ; the test disk's sectors
        jne     fail
        mov     dx, 0x1F6
        mov     al, 0xE0                        ; LBA, master
        out     dx, al
        mov     dx, 0x1F2
        mov     al, 1
        out     dx, al
        inc     dx                              ; LBA 1
        out     dx, al
        inc     dx
        xor     al, al
        out     dx, al
        inc     dx
        out     dx, al
        mov     dx, 0x1F7
        mov     al, 0x20                        ; READ SECTORS
        out     dx, al
        call    ide_drq
        mov     di, 0x8200
        mov     cx, 256
        mov     dx, 0x1F0
        rep     insw
        mov     di, 4
        cmp     word [0x8200], 0x0101
        jne     fail

        ; 5: the printer, then boot
        mov     si, msg
.lpt:   mov     al, [cs:si]
        test    al, al
        jz      .boot
        mov     dx, 0x378
        out     dx, al
        mov     dx, 0x37A
        mov     al, 0x0D                        ; strobe
        out     dx, al
        mov     al, 0x0C
        out     dx, al
        inc     si
        jmp     .lpt
.boot:  mov     dx, 0x378
        mov     al, 10
        out     dx, al
        mov     dx, 0x37A
        mov     al, 0x0D
        out     dx, al
        mov     al, 0x0C
        out     dx, al
        jmp     0x0000:0x7C00

fail:   mov     eax, edi
        and     eax, 0xFF
        shl     eax, 16
        or      eax, 0x3333
        mov     dx, TEST_PORT
        out     dx, eax
.h:     hlt
        jmp     .h

; cs:si -> (index, value) pairs written as words to index port dx, cx of them
idx_out:
        xor     bx, bx
.l:     mov     al, bl
        mov     ah, [cs:si + bx]
        out     dx, ax
        inc     bx
        loop    .l
        ret

fdc_out:                                        ; al -> FIFO when the FDC asks for it
        push    ax
        mov     dx, 0x3F4
.w:     in      al, dx
        and     al, 0xC0
        cmp     al, 0x80
        jne     .w
        pop     ax
        mov     dx, 0x3F5
        out     dx, al
        ret

fdc_in:                                         ; a result byte -> al
        mov     dx, 0x3F4
.w:     in      al, dx
        and     al, 0xC0
        cmp     al, 0xC0
        jne     .w
        mov     dx, 0x3F5
        in      al, dx
        ret

ide_drq:                                        ; wait for DRQ (and not BSY)
        mov     dx, 0x1F7
.w:     in      al, dx
        test    al, 0x80
        jnz     .w
        test    al, 0x08
        jz      .w
        ret

msg:      db    "BIOS OK", 0
fd_read:  db    0xE6, 0x00, 0, 0, 1, 2, 18, 0x1B, 0xFF
vga_seq:  db    0x03, 0x00, 0x03, 0x00, 0x02
vga_crtc: db    0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F, 0x00, 0x4F, 0x0D, 0x0E, 0x00
          db    0x00, 0x00, 0x00, 0x9C, 0x8E, 0x8F, 0x28, 0x1F, 0x96, 0xB9, 0xA3, 0xFF
vga_gc:   db    0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0E, 0x00, 0xFF
vga_attr: db    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07, 0x38, 0x39, 0x3A, 0x3B
          db    0x3C, 0x3D, 0x3E, 0x3F, 0x0C, 0x00, 0x0F, 0x08, 0x00

        times   0xFFF0 - ($ - $$) db 0xFF
reset:  jmp     0xF000:start                    ; the reset vector
        times   0x10000 - ($ - $$) db 0xFF
