# Architecture cheat sheet

Quick reference for recon steps 1–3 and for picking the Ghidra language id. `scripts/fwinfo.py`
reads the containers below and counts the fingerprints (hits per KiB, and how many of each
architecture's pattern kinds appear: the right one shows all of them, far ahead).

## Fingerprints (check both byte orders)

| arch | very common words / bytes | notes |
|---|---|---|
| MIPS32 | `03E00008` (`jr ra`), `27BDxxxx` with negative imm (`addiu sp,sp,-N`), `3C1Cxxxx` (`lui gp,...`), `8FBFxxxx`/`AFBFxxxx` (`lw`/`sw ra`) | embedded MIPS is mostly big-endian; PIC32 is little-endian, with MIPS16e or microMIPS functions at odd addresses |
| ARM (A32) | `E12FFF1E` (`bx lr`), `E92D4xxx` (`push {...,lr}`), `E8BD8xxx` (`pop {...,pc}`); vectors at 0 as `E59FF0xx` (`ldr pc,[pc,#x]`) or `EAxxxxxx` (`b`) | usually little-endian; instruction words little-endian while `basefind.py` picks big-endian pointers = BE8 (`armeb`) |
| Thumb | halfwords `B5xx` (`push {...,lr}`), `4770` (`bx lr`), `BDxx` (`pop {...,pc}`) | Cortex-M images are Thumb only, with a vector table at the base: SP (`0x2000xxxx`), then odd handler addresses |
| PowerPC | `4E800020` (`blr`), `7C0802A6` (`mflr r0`), `9421xxxx` (`stwu r1,-N(r1)`) | big-endian; Book E code writes IVORs (`mtspr` 400-415) and MAS registers (624-627), classic code BATs (528-543) and SDR1 (25) |
| RISC-V RV32 | `00008067` (`ret`), `ff010113`-style `addi sp,sp,-N` (low 7 bits `0x13`, rd=rs1=2); compressed halfwords `8082` (`c.ret`), `1141` (`c.addi sp,-16`) | always little-endian; mixed 16/32-bit with the C extension |
| x86 | bytes `55 48 89 E5` / `55 89 E5` (`push rbp/ebp; mov`), `C3` (`ret`), `48 83 EC xx` (`sub rsp,N`); `E9`/`EA` in the last 16 bytes (reset vector); `_FVH` at +0x28 of a volume, `MZ`/`VZ` (PE/TE) modules | little-endian, variable length: count byte patterns, not aligned words; a PC image goes to `uefi_scan.py` |
| Xtensa LE | bytes `0d f0` (`ret.n`), `1d f0` (`retw.n`), `36 41 00` (`entry a1,32`), `12 c1 f0` (`addi a1,a1,-16`) | 24- and 16-bit instructions at any byte offset; windowed ABI (`entry`/`retw`: ESP32, ESP32-S3) or CALL0 (`addi a1`/`ret`: ESP8266); `l32r` literal pools of 32-bit words just below the code |
| Xtensa BE | bytes `d0 0f` (`ret.n`), `d1 0f` (`retw.n`), `6c 10 04` (`entry a1,32`), `21 1c f0` (`addi a1,a1,-16`) | the same instructions with their fields in the big-endian order |
| AVR (8-bit) | halfwords (LE) `9508` (`ret`), `9518` (`reti`), `920F`/`900F` masked `FE0F` (`push`/`pop`); vector table at 0: `940C kkkk` (`jmp`) every 4 bytes, or `Cxxx` (`rjmp`) every 2 | always little-endian 16-bit words |
| AVR32 | halfwords (BE) `D401` masked `FF0F` (`pushm ...,lr`), `D802` masked `FF0F` (`popm ...,pc`) | big-endian, 16- and 32-bit instructions; code typically at 0x80000000 (UC3 flash) or 0xA0000000 (AP7) |
| PIC baseline / mid-range | little-endian 16-bit words that all fit 12 bits (baseline) or 14 bits (mid-range, enhanced); `MOVLB k` (`0021`-`003F`) only on the enhanced core; `0008` (`return`), `0009` (`retfie`), `34xx` (`retlw`) | two bytes per instruction word, as the PIC tools' HEX files lay it out |
| PIC18 | `0012` (`return`), `0010`/`0011` (`retfie`), two-word `ECkk Fkkk` (`call`) and `EFkk Fkkk` (`goto`) | little-endian, byte-addressed program memory |
| 8051 | `02 hh ll` (`ljmp`) at 0 (reset), and `02`/`32` (`ljmp`/`reti`) or an AJMP (low 5 bits `00001`) at 0x03, 0x0B, 0x13, 0x1B, 0x23; `12 hh ll` (`lcall`), `22` (`ret`), `90 hh ll` (`mov dptr,#`) | 16-bit immediates are big-endian (high byte first) |
| 6502 | vectors in the last 6 bytes of a ROM that ends at 0xFFFF: NMI, RESET, IRQ/BRK (little-endian); reset code starts `78` (`sei`) / `D8` (`cld`) / `A2 FF 9A` (`ldx #$ff; txs`); `20 ll hh` (`jsr`), `60` (`rts`), `40` (`rti`) | 65C02-only opcodes (`80` `bra`, `64`/`9C` `stz`, `DA`/`5A` `phx`/`phy`) select `-o model=65c02` |
| Z80 | at 0: `F3` (`di`), `C3 ll hh` (`jp`), `31 ll hh` (`ld sp,nn`); RST handlers at 0x08-0x38, NMI at 0x66; `CD ll hh` (`call`), `C9` (`ret`), `ED 4D` (`reti`), `ED 56`/`ED 5E` (`im 1`/`im 2`), `DB nn`/`D3 nn` (`in a,(n)`/`out (n),a`) | little-endian 16-bit operands |

Count them with `fwinfo.py` (or a short Python loop / `xxd -p | grep -c`). The right arch and
byte order give counts orders of magnitude above the others; 8-bit opcodes collide with
everything, so for those the vector table and the word widths decide. `basefind.py` also
settles endianness for 32-bit code: the correct order has many paired pointer hits; the wrong
one has almost none.

## Containers and ELF machines

| e_machine | arch (`recon.sh --arch`) | what the ELF tells |
|---|---|---|
| 8 / 10 (`EM_MIPS`, `EM_MIPS_RS3_LE`) | `mips32` | e_flags bits 31:28 the revision (7 = MIPS32r2, 9 = r6: not ember), `0x04000000` MIPS16e, `0x02000000` microMIPS; an odd entry starts in the compressed ISA |
| 40 (`EM_ARM`) | `arm` / `armeb` | big-endian ELF with e_flags `0x00800000` = BE8 (`armeb`), without it BE32 (not ember); `0x400` hard-float (VFP: not ember); an odd entry is Thumb; a `.isr_vector` section or Cortex-M vector table: ARMv7-M (not ember) |
| 243 (`EM_RISCV`) | `rv32` | ELFCLASS64 = RV64 (not ember); e_flags `0x1` C extension, `0x6` float ABI (any = F/D: not ember) |
| 20 (`EM_PPC`) | `ppc32` | big-endian only for ember; 21 (`EM_PPC64`): not ember |
| 3 / 62 (`EM_386`, `EM_X86_64`) | `x86_32` / `x86_64` | a PC's firmware usually comes as a flash image, not an ELF |
| 83 (`EM_AVR`) | `avr` | e_flags & 0x7F = the GNU `-mmcu` architecture: 2-5, 25, 31, 35, 51 AVRe (`model=e`; avr2/25/3/31/35 lack MUL), 6 AVRe+ (`eplus`), 100 avrtiny = AVRrc (`rc`), 102-107 avrxmega2-7 = AVRxm (XMEGA, `xm`) or AVRxt (tinyAVR 0/1/2, megaAVR 0, AVR Dx: `xt`, mostly avrxmega3/4); data-space sections sit at 0x800000 + address |
| 185 (`EM_AVR32`; old 0x18AD) | `avr32` | big-endian; the entry says A (0x80000000) or B (0xA0000000) |
| 94 (`EM_XTENSA`) | `xtensa` / `xtensaeb` | the core is not in the header: sections name the SDK (`.irom0.text`: ESP8266 NONOS/RTOS SDK; `.flash.text`, `.iram0.text`, `.dram0.data`: ESP-IDF) |
| 165 (`EM_8051`), 220 (`EM_Z80`) | `mcs51`, `z80` | rare: 8051 and Z80 builds usually arrive as Intel HEX or raw binaries |
| none for PIC or 6502 | `pic-*`, `pic18`, `6502` | MPLAB/gputils and SDCC deliver Intel HEX (gputils also a `.cod` symbol file and a `.lst` listing) |

| container | magic / shape | what to do |
|---|---|---|
| Intel HEX | text lines `:LLAAAATT...CC` | `fwinfo.py --bin prog.bin` (program memory from its lowest address; the far ranges are PIC configuration words at 0x300000 (PIC18) or word 0x2007/0x8007 (mid-range/enhanced: bytes 0x400E/0x1000E), EEPROM at 0xF00000 (PIC18) or word 0x2100, AVR EEPROM/fuses/lock at 0x810000/0x820000/0x830000) |
| Espressif app/bootloader image | byte 0 `E9`, byte 1 the segment count, u32 entry at +4; ESP8266: segment headers from +8; ESP32 family: a 24-byte header with the chip id (u16 at +12: 0 ESP32, 2 ESP32-S2, 9 ESP32-S3; 5 ESP32-C3, 0xC C2, 0xD C6, 0x10 H2 are RISC-V) | `fwinfo.py --elf app.elf`: each `{load address, length}` segment becomes a PT_LOAD |
| ESP8266 v2 image | byte 0 `EA`: an IROM segment, then an `E9` image | as above |
| ESP32-family flash dump | partition table at 0x8000 (32-byte entries starting `AA 50`: type, subtype, offset, size, label), second-stage bootloader at 0x1000 (ESP32, ESP32-S2) or 0x0 (ESP32-S3 and the RISC-V chips), apps at their partitions | `fwinfo.py` lists partitions and images; `--elf` takes the last app |
| ESP8266 NONOS flash dump | `E9` image at 0 (IRAM/DRAM), the irom0 text raw at flash 0x10000 or 0x40000, executed at 0x40200000 + flash offset | import the irom0 part at its mapped address |

## Ghidra language IDs (`-processor`)

| arch / endian | language id |
|---|---|
| MIPS32 BE | `MIPS:BE:32:default` (MIPS16e included: the `ISA_MODE` context register selects it; `MIPS:BE:32:16e` is the same language starting in MIPS16e mode, for an image whose entry is MIPS16e code) |
| MIPS32 LE (PIC32MX/MZ) | `MIPS:LE:32:default` |
| MIPS32 microMIPS | `MIPS:BE:32:micro` / `MIPS:LE:32:micro` (PIC32MM) |
| ARM32 LE | `ARM:LE:32:v7` (or `v5`/`v6`, `Cortex` for Thumb-only) |
| ARM32 BE8 (data BE, code LE) | `ARM:LEBE:32:v7LEInstruction` |
| ARM32 BE32 (code BE too) | `ARM:BE:32:v7` |
| AArch64 | `AARCH64:LE:64:v8A` |
| PPC32 classic | `PowerPC:BE:32:default` |
| PPC32 Book E | `PowerPC:BE:32:e500` (e500), `PowerPC:BE:32:4xx` (440 class) |
| RV32IMAC | `RISCV:LE:32:default` (current Ghidra, checked on 12.x; older releases had per-extension variants such as `RV32IMC`) |
| x86 real mode (reset code, legacy BIOS) | `x86:LE:16:Real Mode` |
| x86 32-bit (IA32 PEIMs) / 64-bit (X64 modules) | `x86:LE:32:default` / `x86:LE:64:default` (PE modules: the loader picks from the header) |
| AVR (8-bit) | `avr8:LE:16:default` (<= 128 KiB), `avr8:LE:16:extended` (avr51), `avr8:LE:16:atmega256` (AVRe+, 22-bit PC), `avr8:LE:24:xmega` (AVRxm/AVRxt); code addresses count 16-bit words; data in the `mem` space; no language decodes AVRrc's 16-bit LDS/STS |
| AVR32 | `avr32:BE:32:default` (Ghidra's one AVR32 language, named AVR32-A, with the DSP and SIMD instructions; use it for B code too and check B-only system instructions in the listing) |
| PIC baseline | `PIC-16:LE:16:PIC-16C5x` or `PIC-12:LE:16:PIC-12C5xx` (word-addressed `CODE`) |
| PIC mid-range | `PIC-16:LE:16:PIC-16` |
| PIC enhanced mid-range | `PIC-16:LE:16:PIC-16F` |
| PIC18 | `PIC-18:LE:24:PIC-18` (byte-addressed `CODE`, data in `DATA`) |
| 8051 | `8051:BE:16:default`; banked CIP-51 `8051:BE:24:cip-51` (spaces `CODE`, `INTMEM`, `SFR`, `EXTMEM`, `BITS`) |
| 6502 / 65C02 | `6502:LE:16:default` / `65C02:LE:16:default` |
| Z80 | `z80:LE:16:default` (memory `ram`, ports `io`) |
| Xtensa LE / BE | `Xtensa:LE:32:default` / `Xtensa:BE:32:default` (one generic configuration: TIE instructions show as bad data) |

## Ember support (see the gate in SKILL.md)

| arch | ember | board-side choices to record |
|---|---|---|
| MIPS32r2 BE/LE | yes: `uam_isa_mips32be` / `uam_isa_mips32le` | CPU model (`uam_mips_config`: PRId, Config0-3 - Config.MT TLB or fixed mapping, Config1.CA MIPS16e, Config1.FP + `fir` FPU, Config3.ISA microMIPS, DSPP/DSP2P, MCU, VInt/VEIC -, TLB entries, shadow sets, writable Status bits); which Cause.IP line each device uses, or with an EIC the controller (`uam_mips_eic_input`) and the CPU's own timer/software lines it takes |
| PIC32 (MIPS32 LE microcontroller) | yes: `uam_isa_mips32le` (`pic32-ref -o model=mx\|mm\|mz` for MX: MIPS16e; MM: microMIPS only; MZ: microMIPS + FPU + DSP r2 + TLB) | which class; the interrupt controller layout (computed-offset MX, variable-offset MM/MZ) and sources used; SFRs at physical 0x1F800000-0x1F8FFFFF; RAM size, program flash (0x1D000000) and boot flash (0x1FC00000) sizes; configuration words in boot flash |
| RV32IMAC M-mode | yes: `uam_isa_rv32` | CLINT/PLIC layout, mip lines (MSI 3, MTI 7, MEI 11) |
| ARMv7-A, ARM + Thumb-2, LE / BE8 | yes: `uam_isa_arm` / `uam_isa_armeb` | CPU model (`uam_arm_config`: MIDR, CTR, MPIDR, ID registers, CNTFRQ); IRQ/FIQ lines (`UAM_ARM_IRQ`/`UAM_ARM_FIQ`); generic timer outputs to GIC PPIs 30/27 (`uam_arm_set_timer_sink`); Thumb entry = bit 0 set |
| x86-64 (PC) | yes: `uam_isa_x86_64` | CPUID identity (`uam_x86_cpuid_info`: vendor, signature, brand); platform MSRs; I/O ports at `UAM_X86_PIO_BASE + port`; chipset devices (`uefi-recon.md`) |
| PowerPC 32-bit | yes: `uam_isa_ppc32` (`ppc-ref`, `-o model=booke`) | CPU model (`uam_ppc_config`: classic or Book E, PVR, FPU, isel, dcbz block size, time-base divider; classic: 4 or 8 BAT pairs, reset prefix (MSR[IP]); Book E: TLB0/TLB1 geometry and page sizes, PIDs, MAS TSIZE encoding, MCSRR/DSRR, FIT/watchdog bits, the boot TLB entry); implementation SPRs (`uam_ppc_set_spr_ops`) and DCRs (`uam_ppc_set_dcr_ops`) the code touches; the interrupt controller (OpenPIC, the SoC's) on the eight wired-OR external inputs |
| AVR (8-bit) | yes: `uam_isa_avr` (`avr-ref -o model=e\|eplus\|xm\|xt\|rc`) | CPU model (`uam_avr_config`: core family, optional instructions MUL/JMP/ELPM/SPM/BREAK, flash size, SP at reset = end of SRAM, vector size 2 or 4, boot section base); SRAM range and the I/O registers used (data space at bus 0x800000); vectors used and, on AVRxm/AVRxt, each vector's level (`uam_avr_set_irq_level`); peripherals that clear their flag when the vector is taken (`uam_avr_set_irq_ack`); SPM writes (`uam_avr_set_spm`); EEPROM and fuses read |
| AVR32 | yes: `uam_isa_avr32` (`avr32-ref -o model=a\|b`) | A or B, MPU regions or TLB entries, INT3 shadow, SIMD (`uam_avr32_config`); the interrupt controller's groups and autovectors (`uam_avr32_set_autovector`, `uam/dev/avr32_intc.h`); where COUNT/COMPARE goes (`uam_avr32_compare_line`); peripherals (UC3: 0xFFFF0000+; AP7: P4) |
| PIC (8-bit) | yes: `uam_isa_pic` (`pic-ref -o model=baseline\|mid\|enhanced\|pic18`, `-o xinst=1`) | CPU model (`uam_pic_config`: family, program words, baseline banks, XINST, STVREN); the part's SFRs used (data addresses as in the data sheet, data space at bus 0x800000); which peripheral flags drive `UAM_PIC_IRQ_PERIPH`/`UAM_PIC_IRQ_LOW` and INTCON's own sources (`uam_pic_intcon_set`); TBLWT flash writes (`uam_pic_set_tblwt`); the configuration words |
| 8051 | yes: `uam_isa_mcs51` (`mcs51-ref -o model=8051\|8052\|cip51`) | CPU model (`uam_mcs51_config`: internal RAM 128/256, interrupt sources, EIE/EIP/IPH addresses, DPS/second DPTR, the MOVX @Ri page SFR, SFRPAGE, code-banking window); SFRs used (bus 0x2000000 + address, + page * 0x100); XDATA devices and RAM (bus 0x1000000 + address); the bank-select SFR (`uam_mcs51_set_code_bank`); sources whose flags clear on vector entry (`uam_mcs51_set_irq_ack`) |
| 6502 | yes: `uam_isa_m6502` (`6502-ref -o model=nmos\|2a03\|65c02`) | CPU model (`uam_m6502_config`: NMOS or CMOS, decimal mode, Rockwell/WDC bit operations, WAI/STP, the NMOS XAA/LXA constant); the memory map (RAM, ROM, I/O pages, bank switching registers); which devices drive IRQ (lines 0-29, wired-OR), NMI (30) and SO (31) |
| Z80 | yes: `uam_isa_z80` (`z80-ref -o model=nmos\|cmos`) | CPU model (`uam_z80_config`: NMOS or CMOS); the port map (ports at bus 0x10000 + port) and whether the hardware decodes 8 or 16 port bits (`uam_z80_set_io_decode`); interrupt mode (IM 0/1/2), the I register and the IM 2 vector table; who answers the acknowledge (`uam_z80_set_inta`: a CTC/PIO/SIO vector) and watches RETI (`uam_z80_set_reti`); memory banking |
| Xtensa | yes: `uam_isa_xtensa` / `uam_isa_xtensaeb` (`xtensa-ref`/`xtensaeb-ref -o model=lx106\|lx6\|lx7`) | CPU model (`uam_xtensa_config`: options, interrupt table - type and level per interrupt -, levels, EXCMLEVEL, CCOMPARE count and interrupts, vectors and VECBASE, region rights, PRID); which interrupt each device raises; the ERI registers (`uam_xtensa_set_eri`); per-processor PRID (`uam_xtensa_set_prid`); the flash-cache mapping of IROM/DROM (the board maps flash there); TIE instructions in the code (stop or work around) |
| AArch64, ARMv7-M/v8-M (Cortex-M), BE32 ARM, VFP/NEON-dependent code, MIPS64 / release 6, RV64, RISC-V S-mode or F/D, PowerPC64 / AltiVec / SPE / 40x / 8xx / VLE, PIC24/dsPIC, PIC17, 65C816, Z180, 80251/80390 modes, MSP430, 68k, SuperH, x86 AVX/SSE3+/VMX | no | stop and report: needs new ember ISA work |

## Address spaces of the Harvard and port-I/O processors (ember bus)

| processor | program memory | data / I/O | Ghidra |
|---|---|---|---|
| AVR | bus 0 + byte address (pc = 2 x word address) | data space at 0x800000 + data address (AVRe/AVRe+: r0-r31 at 0x00-0x1F, I/O 0x20-0x5F (`IN`/`OUT` A = data 0x20 + A), extended I/O 0x60-0xFF, SRAM above; AVRxm/AVRxt/AVRrc: I/O from data 0, registers not in the data space; AVRxt/AVRrc also see flash in the data space) | `code` (words), `mem` (data) |
| PIC baseline / mid / enhanced | bus 0, two bytes per word (pc = 2 x word address) | data space at 0x800000 + data address (baseline bank * 32 + f; mid-range bank * 128 + f; enhanced also the linear view 0x2000-0x29AF and program memory from FSR 0x8000) | `CODE` (words), `DATA` |
| PIC18 | bus 0 + byte address | data space at 0x800000 + bank * 256 + f (access bank's SFR half 0xF60-0xFFF) | `CODE` (bytes), `DATA` |
| 8051 | bus 0 (banked: block n * W + offset) | XDATA at 0x1000000, SFRs at 0x2000000 (+ page * 0x100); internal RAM in the hart | `CODE`, `EXTMEM`, `SFR`, `INTMEM`, `BITS` |
| Z80 | bus 0 (64 KiB) | I/O ports at 0x10000 + 16-bit port (A or B on the high byte) | `ram`, `io` |
| 6502 | one 64 KiB space at bus 0 | memory-mapped | one space |
| x86 | physical memory | I/O ports at `UAM_X86_PIO_BASE` (2^40) + port | `ram` only: IN/OUT are no references, `FindX86Io.java` lists the ports |

## Base address conventions (priors, never evidence)

- **MIPS**: reset vector `0xBFC00000` (kseg1, the bootrom). On flash-booting SoCs this is
  flash offset 0, and the bootstrap usually runs **in place** (XIP), often switching to the
  cached kseg0 alias (`0x9FC0xxxx`) for its C code. Scan it at that base too. OS kernels run in kseg0
  (`0x80000000+`). A loader-handed stage starts at its own entry, often the base itself
  (typically a page-aligned kseg0 address such as `0x80010000`). Physical = virtual &
  `0x1FFFFFFF`. Flash is usually seen at `0x9Fxxxxxx`/`0xBFxxxxxx` (physical `0x1Fxxxxxx`).
  PIC32: boot flash at `0xBFC00000` (physical `0x1FC00000`), program flash at `0x9D000000`
  (physical `0x1D000000`), RAM at `0x80000000`/`0xA0000000` (physical 0).
- **ARM Cortex-M** (not ember): vector table at the base. Word 0 = initial SP (in RAM), word 1 =
  reset handler (odd = Thumb). Flash often at `0x00000000` or `0x08000000`.
- **ARM A/R**: vectors at `0x00000000` or `0xFFFF0000`. Linux zImage/Image loaded at
  RAM + offset.
- **RISC-V**: reset vector is SoC-defined (`0x00001000` boot ROM, `0x20000000` SPI-XIP, `0x80000000`
  RAM are common). `mtvec` set early names the trap handler; `la sp, _stack_top` names RAM.
- **x86 PC**: reset at `0xFFFFFFF0` (real mode, CS base `0xFFFF0000`); the boot flash (its BIOS
  region) ends at 4 GiB, so the base is `4 GiB - BIOS size`; UEFI modules carry their own
  image base. No base search: `uefi_scan.py` (`uefi-recon.md`). A legacy BIOS's last 128 KiB
  also appear below 1 MiB (`0xE0000-0xFFFFF`).
- **PowerPC**: classic resets at `0xFFF00100` (MSR[IP] = 1; vectors at `0xFFF00000 + n`) or
  `0x00000100`, so boot ROMs sit at `0xFFF00000`; Book E resets at `0xFFFFFFFC` inside a boot
  page (4 KiB at `0xFFFFF000` on e500) and moves the vectors with IVPR. Kernels: `0x00000000`.
- **AVR, PIC, 8051**: program memory at 0, reset vector at 0 (baseline PIC: the last program
  word, then the pc wraps to 0). Interrupt vectors: AVR vector n at n x 2 or n x 4 bytes;
  mid-range PIC word 4; PIC18 0x08 (high) and 0x18 (low); 8051 0x03 + 8n. A bootloader
  section (AVR BOOTRST, XMEGA/AVRxt boot section) moves the reset and vectors to its base.
- **6502**: vectors at 0xFFFA-0xFFFF, so a ROM that holds them ends at 0xFFFF (base = 0x10000 -
  size); cartridge and banked ROMs: the window the bank appears in.
- **Z80**: reset at 0; RST vectors 0x00-0x38, NMI 0x66; an IM 2 table anywhere (I register).
- **AVR32**: AVR32A (UC3) resets at `0x80000000` (internal flash), AVR32B (AP7) at
  `0xA0000000` (P2, uncached, physical 0); EVBA set early names the exception base.
- **Xtensa / Espressif**: segment addresses come from the image header. The ROM reset vectors
  are `0x40000080` (ESP8266) and `0x40000400` (ESP32, ESP32-S3), VECBASE resets to
  `0x40000000`; apps run from IRAM (`0x40100000` ESP8266, `0x40080000` ESP32, `0x40370000`
  ESP32-S3) and flash through the cache (`0x40200000` ESP8266, `0x400D0000`/`0x3F400000`
  ESP32, `0x42000000`/`0x3C000000` ESP32-S3).

A monolithic RTOS image frequently loads nowhere near the reset vector. Only string-pointer
evidence plus an independent cross-check (loader destination, init code bounds) confirms a
base.

## Entry state to record

| arch | check at the entry |
|---|---|
| MIPS | writes to CP0 Status/Cause/Config, `sp` and `gp` setup, BSS clear bounds, `a0` usage (boot type); EBase/IntCtl/SRSCtl setup on vectored or EIC cores; the ISA bit of the entry (MIPS16e/microMIPS) |
| ARM | mode switches (`msr cpsr`), stack setup per mode, VBAR/vector copy, cache/MMU enable |
| RISC-V | `csrw mtvec`, `mstatus`/`mie` setup, `la sp`/`la gp` (`__global_pointer$`), `mhartid` checks that park secondary harts (ember: hart 0 alone, or `-o cpus=N`) |
| x86 | cache-as-RAM setup (MTRR MSRs, CR0.CD/NW), microcode load (WRMSR 0x79), `lgdt` + CR0.PE, then CR4.PAE + EFER.LME + CR3 for long mode; the BSP check (IA32_APIC_BASE bit 8) that sends APs elsewhere |
| PPC | MSR setup, `r1` (stack) and `r2`/`r13` (small-data anchors) setup; Book E: IVPR/IVORs, the TLB1 entries it writes before leaving the boot page; classic: BATs, HID0 |
| AVR | SP init (often from RAMEND: SPH/SPL at I/O 0x3E/0x3D), `eor r1,r1` + `out SREG,r1`, the `.data` copy loop (LPM from flash, with ELPM/RAMPZ above 64 KiB) and `.bss` clear, which mark the SRAM range; EIND, watchdog and clock-prescaler writes |
| AVR32 | SP setup, EVBA, SR mode bits, the MPU/TLB setup |
| PIC | the `goto` at the reset vector, bank and page selection (STATUS RP/PA, BSR, PCLATH), the interrupt-context save; FSR use; baseline: the OSCCAL move at the reset vector |
| 8051 | SP (`75 81 nn`: `mov sp,#nn`, so the internal RAM size in use), PSW register bank, IE/IP, the XDATA init copy loops, SFRPAGE and bank-select writes |
| 6502 | `sei`, `cld`, `ldx #$ff` / `txs` (stack), RAM clear loops, which I/O addresses are written first |
| Z80 | `di`, `ld sp,nn`, `im 1`/`im 2` and `ld a,n` / `ld i,a`, the first OUTs (port map), bank-switch writes |
| Xtensa | VECBASE writes, PS (INTLEVEL, EXCM, WOE), `a1` stack setup, WINDOWBASE/WINDOWSTART, CPENABLE (FPU), PRID checks that park or start the second ESP32 core |

If the entry doesn't set `sp` (or another register it uses), the board's entry-register hook
(`board_entry_regs`) must provide it. Take the value from the loader that normally jumps there.

## RTOS identification hints

| RTOS | tells |
|---|---|
| VxWorks | `VxWorks5.x`/`VxWorks 6.x` banner, `Wind River Systems`, `usrInit`/`usrRoot`/`sysHwInit`, `tShell`/`tNetTask`, BSP path strings (`.../config/<bsp>/sysLib.c`), `intPrioTable`-style interrupt tables |
| Linux | `Linux version x.y.z`, `init=`, `/sbin/init`, device-tree blobs (`d00dfeed`) |
| U-Boot | `U-Boot 20xx.xx`, `bootcmd`, `bootargs`, environment blocks |
| eCos / RedBoot | `eCos`, `RedBoot>`, `cyg_` symbols |
| ThreadX / FreeRTOS / Nucleus | `ThreadX`, `tx_thread_`, `FreeRTOS`, `xTaskCreate`, `Nucleus PLUS` |
| Zephyr / NuttX | `*** Booting Zephyr OS`, `k_thread`; `NuttShell`, `nsh>` |
| ESP-IDF / ESP8266 SDK | `ESP-IDF v...`, `app_main`, `esp_image`, `cpu_start`, a partition table; NONOS SDK `SDK ver:`, `user_init`, `ets_` ROM calls (`ets_printf`), `.irom0.text` |
| bare-metal microcontroller runtimes | avr-libc (`__vectors`, `__do_copy_data`, `__do_clear_bss`), XC8/MPLAB (`__initialization`, `start_initialization`), SDCC (`_sdcc_gsinit_startup`, `__sdcc_`), Keil C51 (`?C_STARTUP`, `?C_C51STARTUP`), Arduino (`setup`/`loop` called from `main`, `HardwareSerial`) |
| UEFI / coreboot / legacy BIOS | `_FVH` volumes, `$IBIOSI$` BIOS ID, `American Megatrends`/`InsydeH2O`/`EDK II`; `__FMAP__`, `LARCHIVE` (CBFS); `$PIR`, `$PnP`, `_32_`; `uefi_scan.py` reports them all |

VxWorks 5.x images are position-dependent and single-address-space: one base covers the
whole kernel. The BSP and board strings name the SoC, which gives you the public register
map family to compare against. On a microcontroller the part number names the register map:
look for it in strings, the ELF's `-mmcu`, the HEX configuration words, and the SFR addresses
the code touches.
