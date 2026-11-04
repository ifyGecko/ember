; bank.asm - one code bank of the CIP-51 self-test: mkimage.py assembles it with BANK
; replaced by the block number. At 0x8000: return 0xB0 + BANK; at 0x8100: 0xD0 + BANK.
        .org    0x8000
        mov     a, #(0xb0 + BANK)
        ret
        .org    0x8100
        .db     0xd0 + BANK
