---
name: ember-dev-recon
description: "Firmware reconnaissance and triage for emulation on ember (UAM). Use when the user has an unknown firmware image (router, IoT, microcontroller or bootloader dump; raw, ELF, Intel HEX, Espressif) and needs its architecture and endianness, base address, entry, memory map, RTOS (VxWorks, Linux, eCos, FreeRTOS, ESP-IDF), boot chain, flash layout and MMIO registers. Also PC firmware (UEFI, coreboot, legacy BIOS): flash descriptor, volumes, modules, I/O ports, PCI config, MSRs. Decides early whether ember can run it (RV32IMAC; MIPS32r2 BE/LE incl. MIPS16e, microMIPS, FPU, DSP, PIC32; ARMv7-A LE/BE8; x86-64 PCs; 32-bit PowerPC; AVR; AVR32; 8-bit PIC; 8051; 6502; Z80; Xtensa ESP8266/ESP32/ESP32-S3). Python tools + Ghidra (headless or MCP); emits RECON.md for ember-dev-build. Triggers: 'what arch is this firmware', 'find the base address', 'load firmware into Ghidra', 'map the MMIO', 'what is in this BIOS/UEFI image', 'can ember run this', 'prep this image for ember', firmware triage/recon."
---

# ember-dev-recon: firmware reconnaissance for emulation

Goal: turn a firmware image into a **recon fact sheet** (`RECON.md`) holding everything
`ember-dev-build` needs to write an ember board for it:
- architecture and endianness, and whether ember has an ISA module (and CPU model) for it
- boot chain and flash layout
- the confirmed base address of each stage (with evidence)
- entry point and entry state
- memory map (on a Harvard or port-I/O processor: each address space)
- RTOS/kernel identity
- a ranked MMIO table with register offsets, read/write counts and the functions that
  touch each page
- for a PC: flash layout, CPU and chipset identity, volumes and modules, boot phases, and
  the I/O port, PCI configuration, MSR and CPUID tables (section *PC firmware* below)

Every number in it carries its evidence.

This skill ships in the ember repository, at `skills/ember-dev-recon/` (next to
`skills/ember-dev-build/` and `skills/ember-fuzz/`), so the ember sources it describes are
`../../` from here: `../../include/uam/isa/` for the ISA modules, `../../boards/` for the
reference boards, `../../README.md` for the status of each.

Tools (paths relative to this skill directory; all self-contained):

| tool | needs | does |
|---|---|---|
| `scripts/fwinfo.py` | python3 | **first look**: the container (ELF: e_machine and flags -> arch, ember ISA, board, Ghidra language; Intel HEX: ranges, `--bin` program memory as a raw binary; Espressif image or ESP32 flash dump: chip, entry, segments, partition table, `--elf` as an ELF), and for a raw image instruction fingerprints and the vector-table shapes of the 8-bit parts |
| `scripts/basefind.py` | python3 | base address by string-pointer voting with paired-hit re-ranking; picks endianness; prints spot checks; `--word 2` for 16-bit address spaces |
| `scripts/mmio_scan.py` | python3 | raw MIPS32 register-access table (page, offsets, R/W, max offset, code sites) plus **polling loops** (what the code waits for); no Ghidra needed |
| `scripts/flashmap_scan.py` | python3 | flash **partition-table candidates** (arrays of plain flash offsets) plus flash references grouped by 64 KiB block (MIPS32) |
| `scripts/ghidra/FirmwareRecon.java` | Ghidra | segments, entry candidates, banner strings |
| `scripts/ghidra/FindMmio.java` | Ghidra | MMIO pages with offsets, R/W and accessing functions (any arch via references, per address space; MIPS lui reconstruction; on x86 addresses below 0x80000000 count as RAM, except 0xA0000-0xFFFFF; 256-byte pages on 8/16-bit processors) |
| `scripts/ghidra/SeedEntries.java` | Ghidra | pre-analysis: disassemble from given vectors (raw AVR and 8051 images, whose Ghidra languages name no entry) |
| `scripts/uefi_scan.py` | python3 (`brotli` module only for Brotli sections) | PC firmware structure: flash descriptor regions, BIOS window, reset vector, FIT, microcode CPU signatures, firmware volumes (nested, decompressed) and their files, each PE32/TE image's machine, base, entry and XIP address, variable store, BIOS-ID/vendor strings, option ROMs, coreboot FMAP/CBFS; `--extract DIR` writes the modules and `DIR/modules.tsv` |
| `scripts/ghidra/FindX86Io.java` | Ghidra | x86 only: I/O ports (IN/OUT), PCI config addresses written to 0xCF8 and PCI-library `PCI_LIB_ADDRESS` arguments (decoded b:d.f + register), RDMSR/WRMSR indices and values, CPUID leaves, constant addresses passed to MMIO accessor functions; resolves arguments through edk2-style wrappers, lists what it could not |
| `scripts/recon.sh` | the above, optional binwalk | orchestrates all of it into a draft `RECON.md` |
| `scripts/ghidra-query.sh` + `scripts/ghidra/Decompile.java` | Ghidra | **the Ghidra MCP fallback**: decompile, disassemble, xrefs, function and string lists, program info on an existing (`recon.sh`) project, headless and read-only |

Which tool decodes which arch: `fwinfo.py` reads any container and fingerprints MIPS, PowerPC,
ARM/Thumb, RV32, Xtensa (both byte orders), AVR, PIC18 and x86 code, plus the AVR, PIC, 8051,
Z80, 6502 and Cortex-M vector/word shapes. `basefind.py` is arch-neutral (pointer voting;
`--word 8` for 64-bit, `--word 2` for 6502/Z80/8051 address spaces). `mmio_scan.py` and
`flashmap_scan.py` decode the MIPS32 encoding only (not the MIPS16e or microMIPS functions of a
PIC32 image). For every other arch the MMIO table comes from `FindMmio.java` (reference-based)
and the Ghidra MCP tools. For x86, `FindX86Io.java` adds what memory references cannot show,
and `uefi_scan.py` replaces the base search for PC images.

Environment (optional): `GHIDRA_HOME` (auto-detected from PATH, `~/ghidra*`, `/opt/ghidra*`),
`GHIDRA_PROJECTS` (default `~/GhidraProjects`).

## Before you start: tools must work
Check the Ghidra MCP server first: `get_program_info` must answer and name the program under
study (name, base). The MCP tools cannot open or switch programs. If the server is down,
unreachable, or holds another program, **say so once, then use headless Ghidra**:
`scripts/ghidra-query.sh` runs the same queries (decompile, disassembly, xrefs, functions,
strings, program info) on the project `recon.sh` made, read-only (table in step 4). It needs
a Ghidra install and that project; if `recon.sh` has not run yet, run it first (it creates
the project). **Stop and ask the user** only when neither works: no MCP and no Ghidra install,
or a headless import that fails. Don't fall back to weaker methods (raw byte reading,
guessing from strings) for questions Ghidra answers.

## Gate: can ember run this target?
Ember lifts guest code into its own IR and ships exactly these ISA modules (check
`../../include/uam/isa/` and the Status paragraph of `../../README.md` in case more were
added; a build pinned to an older ember commit has only what that commit has). The CPU model
column is what the board selects; `references/arch-cheatsheet.md` has what to record for each.

| ember ISA descriptor (reference board) | covers | not covered |
|---|---|---|
| `uam_isa_mips32be` / `uam_isa_mips32le` (`mips-ref`, `mipsel-ref`; `pic32-ref -o model=mx\|mm\|mz`) | MIPS32r2 user + CP0, an R4K-style JTLB (1..64 entries) or fixed mapping, Count/Compare, and as the CPU model's Config registers say (`uam_mips_config`): MIPS16e or microMIPS (pc bit 0), the release 2 FPU (single/double, FR 0/1), the DSP ASE rev 1/2, the MCU ASE, GPR shadow sets, vectored and external-controller (EIC) interrupts. The default model `uam_mips_ref` has no FPU: COP1 then raises Coprocessor Unusable (soft-float or lazy-FPU code survives) | MIPS64, release 6, MT ASE, COP2 (Coprocessor Unusable), paired-single FP, caches (CACHE is a no-op) |
| `uam_isa_rv32` (`rv32-ref`) | RV32IMAC + Zicsr + Zifencei, M-mode (PMP CSRs read as zero) | S/U-mode, MMU, F/D, 64-bit |
| `uam_isa_arm` / `uam_isa_armeb` (`arm-ref`, `armeb-ref`) | ARMv7-A: ARM (A32) and Thumb/Thumb-2 with interworking and IT blocks, processor modes and banked registers, vectors at 0/0xFFFF0000/VBAR, CP15 with the short-descriptor MMU, generic timer. `armeb` is BE8 (data big-endian, code little-endian) | VFP/NEON, Jazelle, security and virtualization extensions (all take Undefined Instruction), LPAE, BE32 (big-endian code), ARMv7-M (Cortex-M), AArch64 |
| `uam_isa_x86_64` (`x86_64-ref`) | x86-64 from the reset vector (0xFFFFFFF0, real mode) through protected, virtual-8086 and long mode; SMM (Intel 64 and AMD64 save states, SMBASE relocation, I/O restart, SMRR, AMD TSeg/ASeg/SMMLOCK, SMRAM decoded per access through the SMM view; optional SMM code-fetch check); x87, MMX, SSE/SSE2; CPUID identity set by the board; I/O ports as bus accesses at `UAM_X86_PIO_BASE + port` | SSE3 and later (the 0F38/0F3A maps raise #UD), AVX (VEX not decoded), VMX/SVM |
| `uam_isa_ppc32` (`ppc-ref`, `-o model=booke`) | 32-bit PowerPC, big-endian: the user ISA with the FPU and either privileged architecture per CPU model (`uam_ppc_config`): classic OEA (6xx/7xx/74xx, e300/e600: BATs, segments, hashed page table, decrementer) or Book E (e500/e200/440 class: IVPR/IVORs, critical/machine-check/debug classes, TCR/TSR timers and watchdog, TLB arrays through MAS); the board supplies implementation SPRs, DCRs and a non-MAS TLB interface | 64-bit, little-endian, AltiVec, SPE, the 40x and 8xx privileged architectures (PPC405, MPC8xx), VLE, Book E little-endian pages, debug address compares |
| `uam_isa_avr` (`avr-ref -o model=e\|eplus\|xm\|xt\|rc`) | 8-bit AVR, every core family: AVRe, AVRe+ (22-bit PC, EIND/RAMPZ), AVRxm (XMEGA: PMIC, DES, XCH/LAS/LAC/LAT), AVRxt (CPUINT), AVRrc (reduced core); optional MUL/JMP/ELPM/SPM/BREAK; program memory at bus 0 (pc = byte address), data space at 0x800000; SREG, SP, RAMPx, EIND, CCP and the PMIC/CPUINT belong to the hart | every peripheral (the board's), the watchdog, fuses, configuration change protection, instruction timing (one tick per instruction) |
| `uam_isa_avr32` (`avr32-ref -o model=a\|b`) | AVR32A (UC3 class: events on the system stack, MPU) and AVR32B (AP7 class: RSR/RAR per context, INT3 shadow registers, P0-P4 segments, ITLB/DTLB, SIMD); DSP instructions on both; COUNT/COMPARE as an output line; autovectored interrupts | caches (`cache` does nothing), the Java and secure states, coprocessors, on-chip debug registers |
| `uam_isa_pic` (`pic-ref -o model=baseline\|mid\|enhanced\|pic18`, `-o xinst=1`) | 8-bit PIC: baseline (12-bit), mid-range and enhanced mid-range (14-bit), PIC18 (16-bit, XINST as an option); the core's registers, hardware stack, banks and pages, GIE/PEIE and INTCON's three sources; program memory at bus 0 (two bytes per 12/14-bit word), data space at 0x800000 | peripherals (the board's), the watchdog (CLRWDT/SLEEP set TO/PD only), PIC18 INTCON2 priorities, the vectored interrupt controller and 14-bit data addresses of the newest PIC18 parts, the enhanced baseline (PIC12F529 class), debug-executive opcodes; PIC17 and the 16-bit PIC24/dsPIC not at all |
| `uam_isa_mcs51` (`mcs51-ref -o model=8051\|8052\|cip51`) | the MCS-51 instruction set; 128/256-byte internal RAM in the hart; code at bus 0, XDATA at 0x1000000, SFRs at 0x2000000 (+ page * 0x100); up to 32 interrupt sources (IE/IP, extended enables/priorities, IPH), two DPTRs, the MOVX @Ri page SFR, SFR paging, code banking; generic Timer 0/1, serial port and port devices | Timer 2, power-down (idles like IDL), instruction timing; the 80251 and 80390 extended modes |
| `uam_isa_m6502` (`6502-ref -o model=nmos\|2a03\|65c02`) | NMOS 6502 (6510, 2A03) with its undefined opcodes, the 2A03 without decimal mode, WDC W65C02S (`65c02`) and Rockwell R65C02 (`uam_m6502_r65c02` in a board, `new-emulation.sh --model r65c02`; `6502-ref` has no such model); one 64 KiB space, vectors at 0xFFFA-0xFFFF | 65C816/65802, instruction timing, the dummy reads of indexed modes, an NMI hijacking BRK, the branch that delays an interrupt |
| `uam_isa_z80` (`z80-ref -o model=nmos\|cmos`) | the whole Z80, documented and undocumented; memory at bus 0, I/O ports at 0x10000 + port (16-bit port address, the board masks its decoding); IM 0/1/2 through the board's acknowledge hook, NMI, RETI reported to the board | Z180/HD64180 (MMU, on-chip I/O), 8080/8085 models, instruction timing, R while HALT waits, IM 0 with other than RST/CALL on the bus |
| `uam_isa_xtensa` / `uam_isa_xtensaeb` (`xtensa-ref`, `xtensaeb-ref -o model=lx106\|lx6\|lx7`) | Xtensa as the ESP8266 (LX106: CALL0, no windows), ESP32 (LX6) and ESP32-S3 (LX7) configure it: windows, loops, booleans, MUL/DIV, MAC16, the single-precision FPU, S32C1I, XEA2 exceptions with up to 7 levels and NMI, CCOUNT/CCOMPARE, Region Protection; little- and big-endian | designer TIE instructions (IllegalInstruction: the ESP32-S3's AI/vector extension, the ESP32's double-precision helpers), MMU-option rings (one ring), cache and OCD instructions, IBREAK/DBREAK/ICOUNT debug exceptions, timing; no Espressif peripheral |

Processors: `rv32-ref`, `mips-ref`/`mipsel-ref`, `arm-ref`/`armeb-ref`, `ppc-ref`,
`x86_64-ref` and `xtensa-ref`/`xtensaeb-ref` take `-o cpus=N`; firmware tells the processors
apart by `mhartid`, EBase.CPUNum, MPIDR.Aff0, PIR, PRID or the x86 initial APIC ID (x86 APs
wait for INIT/SIPI). The microcontroller boards (`avr-ref`, `avr32-ref`, `pic-ref`,
`pic32-ref`, `mcs51-ref`, `6502-ref`, `z80-ref`) have one processor.

Decide this as soon as the architecture is known (step 3), before the expensive passes. If
the target needs an ISA, mode or extension ember lacks (AArch64, a Cortex-M or BE32 ARM image,
MIPS64 or release 6, RV64, RISC-V S-mode or F/D, PowerPC64, AltiVec/SPE, a PPC405/MPC8xx or
VLE core, PIC24/dsPIC, 65C816, Z180-specific code, an FPU or NEON the OS actually uses on ARM,
x86 code that needs AVX/SSE3+ or VMX, MSP430, 68k, SuperH), **stop and tell the user**:
bringing it up means extending ember itself (an ISA module), which is a different project
from writing a board. Record any partial gap (e.g. "FPU present but only used by one
benchmark task", "ESP32-S3 TIE vector instructions in the DSP library") in RECON.md under
*Ember support*, so the build phase knows which traps to expect.

## The pass

A PC image (x86 with firmware volumes or an Intel flash descriptor) has no base to search:
take the *PC firmware* flow below for steps 1-5, then continue at step 6.

### 1. Triage the container: what is in the file?
Start with `scripts/fwinfo.py <image>`: it names the container and, where the format says so,
the architecture.
- **ELF**: e_machine and e_flags give the arch and variant (MIPS revision and MIPS16e/microMIPS,
  ARM BE8 and float ABI, RISC-V C and float ABI, the AVR core family); PT_LOAD segments give
  the load addresses, so there is no base to search. ember loads ELF (PT_LOAD by physical
  address) as well as raw binaries.
- **Intel HEX** (`.hex`, `.ihx`; the usual form for AVR, PIC, 8051 and 6502/Z80 SDCC builds):
  ember does not load HEX. `fwinfo.py --bin OUT` writes the program memory as a raw binary
  and lists the far ranges, which are not code: PIC configuration words (PIC18 at 0x300000,
  with XINST in CONFIG4L), PIC EEPROM, AVR EEPROM/fuses at 0x810000/0x820000. Record the
  configuration words: they select oscillators, the watchdog and XINST.
- **Espressif** (magic 0xE9; ESP8266 v2 0xEA; an ESP32-family flash dump with its partition
  table at 0x8000): chip, entry, and the segments with the memory each loads into (IRAM, DRAM,
  flash-mapped IROM/DROM). `fwinfo.py --elf OUT` writes the image as an ELF that both Ghidra
  and `uam-run` load at the segment addresses.
- **raw**: `binwalk <image>` and look at the head of the file (`xxd | head`). Record every
  member with offset and size: headers, bootloader, compressed streams (LZMA/gzip/xz),
  filesystems, config and calibration partitions. Then extract the compressed members (e.g.
  `binwalk -e`).
- **Verify each member, don't assume.** A large decompressed blob might be the main OS, a
  filesystem or a data archive. Check: does it disassemble cleanly from offset 0? Does
  `basefind.py` find a corroborated base? What strings does it hold (task names, shell
  commands, drivers)? The main OS is usually the largest member that is *code*.
- **Map file offsets to flash offsets.** Upgrade images often prepend a vendor header, so
  `flash_offset = file_offset - header_len`. Confirm it with a member whose flash location the
  loader code uses (it reads `flash_base + X`).
- Identify partitions the image does *not* contain (config/NVRAM, radio calibration/ART, MAC
  address, an ESP `nvs`/`phy_init` partition, a microcontroller's EEPROM). The emulated flash
  will need them blank or synthesized.
- **Name partitions from their writers, not from one read.** Run `flashmap_scan.py` on each
  code stage (MIPS). A partition table stored as plain offsets (reached as
  `flash_base + table[i]`) is the authoritative layout: `get_xrefs_to` it and read the
  accessors. A region read once at a fixed address is a lead, not a label. Separate boot env /
  RTOS NVRAM, app config, logs, upgrade state and calibration, and record sub-offsets within a
  sector.

### 2. Base address and endianness for each code stage
"Each code stage" **includes the bootstrap that runs from the reset vector**. It is often
uncompressed and executes in place from flash, so its base is the reset address (MIPS
`0xBFC00000` = flash offset 0) and needs no base search. It must still be scanned (step 5).

The architecture fixes some bases: AVR, PIC and 8051 program memory starts at 0 with the
reset vector there (a baseline PIC resets at its last program word); a Z80 resets at 0; a
6502 takes its reset vector from 0xFFFC, so a ROM that holds the vectors ends at 0xFFFF
(`fwinfo.py` checks the vectors; a banked ROM: the bank's window). AVR32A resets at
0x80000000, AVR32B at 0xA0000000, PIC32 at 0xBFC00000 (boot flash; program flash at
0x9D000000), classic PowerPC at 0xFFF00100 (MSR[IP]) or 0x100, Book E at 0xFFFFFFFC. Those
are priors for the reset stage only; anything a loader copies elsewhere needs the search:
```bash
python3 scripts/basefind.py <stage.bin>            # --endian auto, align 0x1000
python3 scripts/basefind.py <rom.bin> --word 2 --align 0x100   # 6502 / Z80 / 8051 bank
```
Accept a base only with **paired hits** clearly above the runner-up (instruction-word noise
scores ~0 paired hits) **and** spot checks that resolve to real strings. If nothing wins, try
`--align 0x100`, `--word 8` (64-bit), or check that the stage really is decompressed code.
Harvard parts reach their strings through LPM/RETLW/MOVC, not stored pointers, so expect no
votes there. Then corroborate independently:
- the loader that decompresses or copies the stage names the destination (`memcpy`/
  `unlzma(..., dst, ...)` then a jump);
- the stage's own init code sets `gp`/`sp`/BSS bounds inside `[base, base+len)`;
- `jal`/absolute branch targets (`call`/`jmp`/`lcall`/`jsr` on the 8-bit parts) land inside
  the image.

### 3. Architecture details and entry state
Confirm the CPU family and variant (`references/arch-cheatsheet.md`: fingerprints, ELF
machines, Ghidra languages, vector tables), then apply the ember gate above. Record the CPU
model the board must select, from what the code checks and uses:
- MIPS: PRId compares, Config1/2/3 reads (cache geometry, TLB size) and the features in use
  (MIPS16e/microMIPS code, FPU, DSP, shadow sets, EIC): a `uam_mips_config`.
- ARM: the MIDR/MPIDR/CTR reads and the CNTFRQ value (`uam_arm_config`), and whether the entry
  is Thumb (bit 0 set; BE8 images: `--arch armeb`).
- PowerPC: classic or Book E (mtspr to IVORn/MAS registers vs SDR1/BATs), PVR compares, the
  implementation SPRs (HID0, L2CR, ...) and DCRs the code touches: `uam_ppc_config` plus SPR/DCR
  handlers.
- AVR, PIC, 8051, 6502, Z80: the core family or part (ELF flags, HEX configuration words,
  instructions only some cores have), memory sizes, interrupt vectors used; Xtensa: the core
  (windowed ABI or CALL0, FPU use, PRID reads, TIE instructions), VECBASE; AVR32: A or B.
Disassemble the first instructions at the reset address. Record whether the entry is
self-contained (sets status register, `sp`, `gp`) or expects registers from its loader (`sp`,
boot-type argument), since the board must provide those (its entry-register hook).
For each stage also record whether it runs **XIP** (jumps into the flash window or runs C
code there). If so, the board must map that flash as ROM or ROMD (fetchable), not as a
device-only MMIO region. Also look for **self-reset tricks**: a magic written to some memory,
then a chip-reset bit. That memory must survive a reset. Find BSS bounds and the RAM size the
OS assumes (e.g. a `sysPhysMemTop`/`mem=`-style constant).

### 4. Ghidra import and analysis at the confirmed base
`scripts/recon.sh <stage> [--arch ARCH] [--base 0x...]` runs fwinfo, binwalk, basefind, the
raw MMIO scan (MIPS32) and a headless Ghidra import with `FirmwareRecon.java` and
`FindMmio.java` (plus `FindX86Io.java` on x86), and writes a draft `./RECON.md` (working files
in `./.recon/`). ARCH is `mips32`, `pic32`, `rv32`, `arm`, `armeb`, `x86_16`, `x86_32`,
`x86_64`, `ppc32`, `avr`, `avr32`, `pic-baseline`, `pic-mid`, `pic-enhanced`, `pic18`,
`mcs51`, `6502`, `65c02`, `z80`, `xtensa` or `xtensaeb`; without `--arch` it takes what
`fwinfo.py` reads from an ELF or Espressif header, or a clear fingerprint/vector-table lead of
a raw image, and stops if there is none. An ELF (or a converted Espressif image) is imported
with its own segments; an Intel HEX file goes in as its program memory at its lowest address;
an AVR, PIC, 8051 or Z80 image defaults to base 0 and a 6502 ROM to its vectors. The Ghidra
language follows ARCH (`references/arch-cheatsheet.md`); `--processor` overrides it (Book E:
`PowerPC:BE:32:e500`; banked CIP-51 code: `8051:BE:24:cip-51`; microMIPS-only PIC32MM:
`MIPS:LE:32:micro`). `--reuse` re-runs the scripts on an existing project; `--no-ghidra`
stops after the Python tools.

When the program is open in the Ghidra GUI with the MCP server running, interrogate it live:
`get_program_info` (check it is the right program and base), `list_segments`, `list_strings`,
`decompile_function_by_address`, `disassemble_function`, `get_xrefs_to`. Rename functions you
identify (`rename_function_by_address`) so the build phase inherits the names.

Without the MCP server, the same questions go to `scripts/ghidra-query.sh` (headless, about
5 s per call plus the decompile time; give several targets per call):

| MCP tool | headless |
|---|---|
| `get_program_info`, `list_segments` | `ghidra-query.sh --blob stage.bin info` |
| `decompile_function`, `decompile_function_by_address` | `... decompile 0x80012340 sysUartInit` |
| `disassemble_function` | `... disasm 0x80012340` |
| `get_xrefs_to` | `... xrefs 0x80012340` (a function name: its callers; `mem:0xe5`: an AVR data address) |
| `list_functions`, `search_functions_by_name` | `... functions [substring]` |
| `list_strings` | `... strings [substring]` |
| `rename_*`, `set_*_comment` | none (read-only): put the names in RECON.md, with addresses |

`--blob FILE` finds the project `recon.sh` made for that file (`--project NAME` or a `.gpr`
path for any other; a HEX or Espressif input's project is named after the original file). An
address no function contains gets a temporary function, so code the analysis missed still
decompiles. In a UEFI project every module is searched and the one holding the address
answers (`--program NAME` picks one). A project open in the Ghidra GUI is locked; the query
then runs on a temporary copy.

**Addresses on Harvard processors.** Ghidra's AVR8 and PIC-12/PIC-16 code spaces count 16-bit
words (`code:0x10b` is byte 0x216), while ember's pc, `--break` and coverage sites use the bus
byte address (twice the word address; PIC18 and the 8051 are byte-addressed in both). Data
addresses are separate spaces in Ghidra (`mem:`, `DATA:`, `SFR:`, `EXTMEM:`, `io:`) and sit
at their bus bases in ember (`references/arch-cheatsheet.md`). Write both forms in RECON.md.

### 5. MMIO: devices, registers and owners
Combine `mmio_scan.py` (raw, fast; MIPS32) and `FindMmio.java` (analysis-aware). For each
page, record the offsets touched, R/W counts and accessing functions, then **label it from the
code**: read one or two of the listed functions and look for the device's strings, chip-ID
compares, baud-rate math, polling loops. Note the hypotheses the build phase must verify.
Details, per architecture (SFRs, I/O ports, ESP peripheral windows), in
`references/mmio-discovery.md`.

Also look for the **interrupt routing table** (which CPU line or vector each device uses), the
**chip revision / bootstrap** registers the code compares against constants, and device
descriptor tables (`{base, irq, vendor/device id}`).

Do this for **every stage** and merge the results:
- **Polling loops** (`mmio_scan.py` POLL lines, plus wait helpers that take
  `(reg, mask, value, timeout)`) are the first bring-up blockers. List each one with what ends
  the wait.
- **Window sizes** come from the highest offset any stage touches (`max_off`). Clock or PHY
  registers can sit inside another block's window.
- **Inputs** (buttons, straps): record the active polarity, so ember's read-as-zero default is
  known to be safe.
- Pages that are only stored to and loaded from (magics, counters) may be **on-chip SRAM**,
  not a peripheral.

### 6. RTOS / kernel identity
Version banners, build strings, BSP/board names, task names, shell commands. These tell the
build phase which subsystems will be exercised (console shell, network stack, WLAN, web
server). Microcontroller images often carry no banner: name the toolchain and runtime instead
(avr-libc, XC8/MPLAB, SDCC, Keil C51, the ESP8266 NONOS/RTOS SDK, ESP-IDF, Arduino), from
startup-code shapes, library strings and section names. For a PC: the `uefi_scan.py` identity
line (BIOS ID, vendor hints), module names and PE debug paths, whether it is a debug build
(DEBUG strings such as `Loading PEIM`).

### 7. Write `RECON.md` in the session project directory
Run the checklist in `references/recon-pitfalls.md` first. Tag each claim **confirmed /
inferred / guess**. "Must count" or "must return X" is only *confirmed* when a code path
(poll, divisor, compare) shows the dependency; access counts alone are *inferred*.

Rewrite the draft into the shape in `references/recon-report-schema.md`, including the
*Ember support* section (for a microcontroller its *Address spaces and CPU model* part, and
for a PC the *PC firmware* section). `RECON.md` is the handoff to `ember-dev-build` and the
first entry of the session record, followed by the build phase's `iter-XX.md` journal.

## PC firmware (x86-64: UEFI, coreboot, legacy BIOS)
The BIOS region is decoded to end at 4 GiB, the processor starts at 0xFFFFFFF0, and every
UEFI module carries its own image base, so steps 1-5 become:
```bash
scripts/recon.sh bios.bin --arch x86_64 [--modules pei|all|NAME,GUID,...] [--no-ghidra]
```
- The UEFI flow is automatic for `--arch x86_*` (and without `--arch`) when the image holds a
  firmware volume (`_FVH`) or an Intel flash descriptor; `--uefi` forces it. It runs
  `uefi_scan.py --extract OUT/modules` (`OUT` = `--out`, default `./.recon`) into
  `OUT/uefi-scan.txt`: descriptor regions, BIOS window, reset vector, FIT, microcode
  signatures, volumes and files, XIP addresses, variable store, identity strings. Every
  PE32/TE image is extracted (TE also as a loadable `.efi`) and listed in
  `OUT/modules/modules.tsv`: name, guid, type, format, machine, image base, entry, XIP, file.
- Ghidra then imports the selected modules into one project (PE loader: each at its own
  image base, IA32 or X64 from its header) and runs `FirmwareRecon`, `FindMmio` and
  `FindX86Io` on each, writing `OUT/ghidra/<module>.recon.txt`, `.mmio.txt` and
  `.x86io.txt`; the x86io and mmio tables are appended to the draft per module.
  `--modules pei` (default) selects SEC, PEI core and PEIMs (types `PEIM+DRIVER` too), `all`
  every module, or a comma list of names/GUIDs from `modules.tsv` (a DXE or SMM driver once
  the build reaches it). `--no-ghidra` stops after `uefi_scan.py`; `--reuse` re-runs the
  scripts on the existing project.
- A legacy BIOS (no volumes, no descriptor) takes the embedded flow: `--arch x86_16` (real
  mode) or `x86_32`, with `--base`; `uefi_scan.py <image>` still reports its reset vector,
  legacy tables (`$PIR`, `_32_`, `$PnP`, `_SM_`) and option ROMs. coreboot images: it lists
  FMAP areas and CBFS files but does not extract CBFS stages.

What to establish from that output (CPU identity from microcode and CPUID compares, chipset
from the PCI tables, which modules to read first, memory init and SPD, console, SMM, variable
store) and how: `references/uefi-recon.md`. Fact sheet shape: the *PC firmware* section of
`references/recon-report-schema.md`.

Nothing in this skill modifies the firmware (conversions such as HEX -> binary or Espressif ->
ELF are written next to it, under `--out`).

`RECON.md` is a living document. When the build phase proves a claim wrong, it folds the
correction back in, tagged `*[build: iter-XX]*` with its evidence, rather than leaving the fix
only in a journal.
