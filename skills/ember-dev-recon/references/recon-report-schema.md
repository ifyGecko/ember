# Recon fact sheet schema (`RECON.md`)

The recon pass produces **one `RECON.md`** in the session project directory. It is the contract
`ember-dev-build` consumes (`new-emulation.sh --recon RECON.md`) and the first entry of the
session record. Every quantitative claim carries its evidence; a number without evidence is a
bug. Device labels are hypotheses for the build phase to confirm.

```markdown
# Recon fact sheet: <target name>

## Identity
- Source image: <path> (<size> bytes, sha256 <...>)
- Container: <raw | ELF (e_machine, e_flags) | Intel HEX (ranges) | Espressif image (chip id) | flash dump> (fwinfo.py)
- Architecture / endianness: <mips32|pic32|rv32|arm|armeb (BE8)|x86_64|ppc32|avr|avr32|pic-baseline|pic-mid|pic-enhanced|pic18|mcs51|6502|65c02|z80|xtensa|xtensaeb|...> / <be|le>; CPU core / part: <e.g. MIPS 24Kc, ATmega2560, PIC16F877A, C8051F120, ESP32>
- Ghidra language id: <e.g. MIPS:BE:32:default, avr8:LE:16:atmega256>
- RTOS / kernel: <name version> (banner: "<string>", build date "<...>")
- BSP / SoC / board: <strings that name them>

## Ember support
- ISA descriptor: <uam_isa_mips32be | uam_isa_mips32le | uam_isa_rv32 | uam_isa_arm | uam_isa_armeb | uam_isa_x86_64 |
  uam_isa_ppc32 | uam_isa_avr | uam_isa_avr32 | uam_isa_pic | uam_isa_mcs51 | uam_isa_m6502 | uam_isa_z80 |
  uam_isa_xtensa | uam_isa_xtensaeb>; closest reference board: <e.g. pic32-ref -o model=mz, avr-ref -o model=eplus>
- gaps: <FPU/NEON used by ...? MIPS64/R6? SSE3+/AVX? AltiVec/SPE? TIE instructions (where)? Timer 2? none>
- CPU model the code checks and uses -> the `uam_<isa>_config` the board selects (start from the reference
  model named, change what the code shows):
  - MIPS: PRId <...> compared at <fn>; Config1 cache/TLB fields read at <fn>; TLB entries <n> or fixed mapping;
    MIPS16e / microMIPS code <yes: where>; FPU (FR mode) / DSP rev / MCU ASE used <...>; shadow sets, vectored
    or EIC interrupts <...> -> `uam_mips_config` (PIC32 class: mx/mm/mz)
  - ARM: MIDR/MPIDR/CTR reads, CNTFRQ -> `uam_arm_config`; x86: see *PC firmware* -> `uam_x86_cpuid_info`
  - PowerPC: classic or Book E (evidence: SDR1/BATs vs IVORs/MAS); PVR compares; SPRs (HID0, ...) and DCRs touched;
    Book E TLB1 entries written at boot -> `uam_ppc_config` + SPR/DCR handlers
  - AVR: core family (ELF e_flags / instructions), flash size, SRAM end (SP init), vector size, boot section,
    MUL/JMP/ELPM/SPM use -> `uam_avr_config`
  - AVR32: A or B, MPU/MMU use, CONFIG0/1 reads -> `uam_avr32_config`
  - PIC: family, program words, baseline banks, XINST/STVREN from the configuration words -> `uam_pic_config`
  - 8051: internal RAM in use (SP init), interrupt sources and their enable/priority SFRs, dual DPTR, MOVX @Ri
    page SFR, SFRPAGE, code banking -> `uam_mcs51_config`
  - 6502: NMOS/2A03/65C02 (decimal mode use, CMOS-only or NMOS-undocumented opcodes) -> `uam_m6502_config`
  - Z80: NMOS or CMOS (OUT (C),0 use); interrupt mode, I register -> `uam_z80_config`
  - Xtensa: LX106/LX6/LX7 (chip id, windowed or CALL0, FPU), interrupt numbers/levels used, VECBASE -> `uam_xtensa_config`
- Processors: <1 | n, how secondaries are told apart and parked> (ember runs n with `-o cpus=N` where the
  ISA's reference board takes it)
- Interrupt inputs: <CPU line per device, e.g. Cause.IP2 = intc out, IP7 = CP0 timer; AVR vector n; 8051 source n;
  Z80 /INT in IM 2 with vector 0x..; Xtensa interrupt n (level, type)>
- Verdict: <runnable as-is | runnable with known gaps: ... | needs ember ISA work: ... (stop)>

### Address spaces and CPU model (Harvard, port-I/O and microcontroller targets)
| space | range (data sheet) | ember bus address | Ghidra | contents | evidence |
|---|---|---|---|---|---|
| program memory | 0x0000-0x.... (<n> words / bytes) | 0x000000 + byte address | `code:` word address (AVR, PIC-12/16) | image; vectors at ... | |
| data / SFR / XDATA / I/O ports | 0x..-0x.. | AVR/PIC 0x800000 + a; 8051 XDATA 0x1000000 + a, SFR 0x2000000 + a; Z80 0x10000 + port | `mem:`, `DATA:`, `EXTMEM:`, `SFR:`, `io:` | SRAM, core registers (the hart's), peripherals (the board's) | |
| EEPROM / configuration words / fuses | ... | not mapped (the board's, if the code reads them) | - | values from the HEX file | |
- vectors in use: <n: address -> handler (device)>; reset vector <...>; bootloader section <base, or none>
- configuration words / fuses: <address = value: oscillator, watchdog, XINST, BOOTRST, ...>
- Espressif images: segments <load address, size, IRAM/DRAM/IROM/DROM>, flash-cache mapping the board must
  provide (IROM/DROM window -> flash offset), partitions

## Boot chain and flash layout
| stage | flash offset | file offset | runs at | XIP? | what | evidence |
|-------|--------------|-------------|---------|------|------|----------|
| 0 | 0x000000 | 0x.. | 0xBFC00000 | yes (kseg1 -> kseg0 flash at 0x9FC0....) | reset/bootrom: PLL/DDR init, self-reset trick?, unpacks stage 1 | scanned with mmio_scan --limit |
| 1 | 0x...... (LZMA) | ... | 0x........ | loader | ... |
| 2 | ... | ... | 0x........ | main OS | the loader decompresses it to 0x........ and jumps there |
| cfg / NVRAM / calibration | ... | absent from image? | — | — | ... | code that reads it |

Flash partitions (one row per partition; label from the WRITER):
| flash offset | size | content | written by (fn, strings) | read by | confidence |
|---|---|---|---|---|---|
| 0x...... | 64K | e.g. RTOS NVRAM: boot line +0x0, app NVRAM +0x5000 | fn @0x.., "... Commit flash" | ... | confirmed / inferred / guess |
- partition table in code: @0x........ = {...} (flashmap_scan.py; accessors fn @0x..)
- file -> flash mapping: flash_offset = file_offset - 0x..   (evidence)
- flash size: <..> MB (evidence: highest offsets referenced, mirrors)
- partitions NOT in the image (must be blank/synthesized for emulation): <...>

## Primary target: <stage name>
### Load address
- Confirmed base: 0x________
- basefind.py: <hits> ptr hits / <paired> paired hits; runner-up 0x________ <paired> paired
- Spot check: word @ file 0x...... = 0x........ -> "<string>"
- Independent cross-check: <loader destination / init-code bounds / jal targets>
### Entry and initial state
- Entry: 0x________ (<how determined>)
- Registers the entry sets itself / expects from the loader: <SR, sp, gp, a0 = boot type ...>
- BSS: 0x________ - 0x________; RAM size assumed by the OS: <..> (evidence)

## Memory map
| region | start | end | rwx | notes |
|--------|-------|-----|-----|-------|
| vectors | ... | ... | rwx | installed by the OS? |
| text+data | ... | ... | rwx | image |
| bss | ... | ... | rw- | zeroed by <fn> |
| heap/stacks | ... | ... | rw- | |
| DRAM (physical) | 0x0 | ... | | |
| flash window (physical) | ... | ... | r-x | cached/uncached aliases, mirrors; executable if any stage is XIP |
| on-chip SRAM / scratch | ... | ... | rw- | used by which stage, for what; must survive reset? |

## Candidate MMIO regions (ranked)
| page (virtual) | physical | R / W | offsets touched (off:count) | guessed device | evidence: functions, strings, compares |
|---|---|---|---|---|---|
| 0xB1000000 | 0x11000000 | 120 / 40 | +0x0:6 +0x10:42 +0x80:40 ... | reset/timer/intc block | FUN_..: polls +0x0 in delay loop; +0x80 compared with 0x...|
| ... | | | | | |

### Not MMIO (filtered constants)
- <formed-only pages that are masks, magic numbers, KSEG conversions>

### Polling loops (all stages)
| stage | register (phys) | spins while | ends when | code | confidence |
|---|---|---|---|---|---|
| 0 | 0x........ | (v & 0x8) == 0 | done bit set | load@0x.. branch@0x.. | confirmed (POLL) |
- wait helpers `(reg, mask, value, timeout)`: fn @0x.. with the constant arguments of its callers

### Inputs and straps
| input | register/bit | active value | effect when active | safe idle under RAZ? |
|---|---|---|---|---|

## Interrupts and timers (hypotheses)
- Interrupt table: <address, format>; CPU line -> vector/demux mapping
- Per device: <device> -> <CPU line / status bit / vector>
- System tick source: <CP0 Count/Compare | SoC timer>

## Identity / config registers the code checks
- <register> compared with <values> at <functions> (chip revision, bootstrap straps ...)

## Suggested bring-up order
1. Board: ISA descriptor + CPU model, RAM, image at base (or ELF segments), PC = entry (+ entry registers);
   on a Harvard part also the data space and its SRAM
2. <devices in the order the boot path will need them, with the key semantics to provide>

## Open questions / risks
- <aliases, unknown registers, data partitions to synthesize, subsystems likely to fail>

## Tooling notes (audit trail)
- Commands run, tool versions, Ghidra project name/path, anything that failed and why.
```

## PC firmware (x86-64: UEFI, coreboot, legacy BIOS)

For a PC image this section replaces *Boot chain and flash layout*, *Primary target*, *Memory
map* and the MMIO table; *Identity*, *Ember support*, *Suggested bring-up order*, *Open
questions* and *Tooling notes* stay. Method: `uefi-recon.md`.

```markdown
## PC firmware
### Platform identity
- Firmware: <UEFI/PI (edk2, AMI, Insyde, Phoenix) | coreboot + payload | legacy BIOS>; BIOS ID "<...>"; <debug | release> build (evidence)
- Chipset: <host bridge 00:00.0 vvvv:dddd, LPC/eSPI 00:1f.0 vvvv:dddd, generation> (device-ID compares at fn@..., PE debug paths, option ROMs)
- CPU: vendor "<...>", signature <0x000906EA> (microcode @0x.. rev .. platform 0x..; CPUID compares at fn@...), brand "<...>", address bits <..>
  -> BOARD_CPU_VENDOR / BOARD_CPU_SIGNATURE / BOARD_CPU_BRAND / BOARD_CPU_PHYS_BITS
- Processors: <n>; APs started by <fn> (INIT/SIPI vector 0x.., AP stack)

### Flash layout
| region | flash offset | size | physical | content / writer | evidence |
|---|---|---|---|---|---|
| descriptor | 0x0000000 | 4K | - | IFD (signature @0x10) | uefi_scan |
| ME | 0x....... | ... | - | CSME; host sees HECI 00:16.0 only | |
| BIOS | 0x....... | ... | 0x........-0xFFFFFFFF | volumes below; variable store @.. | |
- image the board loads: <whole flash (SPI model) | BIOS region | OVMF code= + vars=>; flash size <..>;
  part the flash driver probes: <CFI query / JEDEC ID compared at fn@..>
- reset vector: 0xFFFFFFF0 <insn> -> 0x........ (<SEC / VTF / legacy F000:E05B>)
- FIT @0x........: <microcode, startup ACM, Boot Guard manifests | none>; code that checks their outcome: <fn | none>

### Volumes and modules
| volume (phys) | type | name | format/machine | image base | entry | XIP | phase | analysed |
|---|---|---|---|---|---|---|---|---|
| 0xfffcc000 | SEC | SecMain | PE32+ X64 | 0xfffcc094 | 0xfffd4394 | 0xfffcc094 (rebased) | SEC | yes |
| - (LZMA in ...) | PEIM | ... | ... | ... | ... | - | PEI post-mem | |
- compressed volumes: <guid, compression, decompressed to 0x.. by fn@..>
- run addresses of non-XIP modules: <boot log "Loading PEIM at ..." | computed: how>

### Boot phases
| phase | modules | runs from | needs from the platform | evidence |
|---|---|---|---|---|
| SEC | ... | flash (XIP) | CAR range 0x..-0x.. (MTRR base/mask or TempRamBase), microcode, ... | |
| PEI pre-memory | ... | flash / CAR | chipset BARs, SMBus, SPD | |
| memory init | <FSP-M / MRC in ... / none> | | polls: ... | |
| PEI post-memory, DXE, SMM, BDS | ... | RAM | ... | |

### I/O ports (all analysed modules)
| port(s) | R / W | sizes | values written | device | functions | ember side | confidence |
|---|---|---|---|---|---|---|---|
| 0x0080 | 0 / .. | 1 | 0x.. 0x.. | POST codes | ... | template | confirmed |
- ember side: template device | chipset device to write | `--mmio-rule` | catch-all suffices

### PCI configuration
| b:d.f | vendor:device | function | registers (reg: R/W, values written) | functions | confidence |
|---|---|---|---|---|---|
| 00:1f.0 | vvvv:dddd (compared with {...} at fn@..) | LPC | 0x40 PMBASE <- 0x...; 0xF0 RCBA <- 0x... | ... | |
- BARs and decode bases programmed: <name: b:d.f reg -> value -> window/port block>
- ECAM: PCIEXBAR <- 0x........ (configuration after that: MMIO at base + bus<<20|dev<<15|fn<<12|reg)

### MSRs and CPUID
| MSR | rd / wr | values written | what the code does with it | ember core / board | functions |
|---|---|---|---|---|---|
- CPUID: <leaf: what is compared, constants, path selected>; leaves ember leaves at 0 that the code uses: <...>

### MMIO windows
| window | base (set by, value) | size | offsets touched | device | functions | board side |
|---|---|---|---|---|---|---|
| MCHBAR | 0xFED10000 (00:00.0 reg 0x48) | .. | ... | memory controller | ... | board_windows[] |

### Variable store and other data
- store: <format, flash offset, physical, size, live variables; variables PEI reads>; FTW blocks: <...>
- CMOS offsets read (port 0x70/0x72 index values): <...>; straps / board-ID inputs: <register, active value>

### SMM
- modules: <...>; SMRAM: <A-seg / TSEG: register <- value>; SMRR: <0x1F2/0x1F3 values>; SMBASE relocation by <fn> (APs started before it: <yes/no>)
- AMD: SMM_ADDR/SMM_MASK <values or 'programmed by the PSP'>, HWCR.SMMLOCK set by <fn>; Intel: SMM_Code_Chk_En (0x4E0) <enabled by fn / not used>
- SMI sources: <APM 0xB2, SMI_EN @PMBASE+0x30 <- ...>; locks: <SMI_LOCK, D_LCK, ...>

### Memory init
- path: <FSP-M (FSPH @..) | MRC in <module> | none: size from <fw_cfg / CMOS>>
- SPD: SMBus <b:d.f, base reg -> I/O base>; slaves <0x50..>; bytes read <...>; DRAM type expected <...>
- memory-controller polls: <register, bit, what ends the wait>
- results written: TOLUD <..>, TOUUD <..>, TSEG <..> -> BOARD_LOW_RAM_MAX; boot mode decided by <fn, source>

### Console and progress
- console: <COM1 0x3F8 (Super I/O @0x2E LDN .. enabled by fn@..) | debug port 0x402 | PCH UART @.. | none>
- POST codes: <value -> function, in order>
```

## Rules

- **Verify that the target is code.** Name the executable stage, and say how you checked it
  (base corroborated, disassembles cleanly, OS strings). Never assume from size or position.
- **A base needs paired hits, a spot check and an independent cross-check.** A base with 0
  paired hits is unconfirmed and must not be written as "confirmed".
- **MMIO rows carry register offsets and owning functions.** The build phase models devices
  from exactly this.
- **Physical addresses.** Record the physical base of each device and note aliases
  (kseg0/kseg1, mirrored flash) so the board maps each device once (mirrors become bus aliases).
  On a Harvard or port-I/O processor give each address in its space as the data sheet writes
  it and as ember's bus sees it (AVR data 0xE0 = bus 0x8000E0; Z80 port 0x10 = bus 0x10010),
  and code addresses as byte addresses (Ghidra's AVR/PIC `code:` addresses are word addresses).
- **Ember verdict.** The *Ember support* section is filled in before anything else is handed over.
- **Say what's missing.** List partitions or data the firmware reads that the image doesn't
  contain (NVRAM, calibration, MAC addresses). The build phase will need them blank or
  synthesized.
- **Confidence tags.** Mark claims *confirmed* (code path + cross-check), *inferred* (static,
  one source) or *guess*. A requirement ("must count", "must return X") is *confirmed* only
  with a poll, divisor or compare that depends on it. Access counts alone give *inferred*.
- **All stages.** MMIO, polls, window sizes and ID compares are merged across every code
  stage, including an XIP bootstrap. A device window covers the highest offset any stage uses.
- **PC rows.** Each module row says whether its addresses are run addresses (XIP, rebased) or
  link addresses; each PCI row names b:d.f and, for identity registers, every constant compared;
  each MSR row says whether ember's core answers it or the board must. Ports, PCI, MSR and
  MMIO tables are merged across all analysed modules.
- **Corrections.** When the build phase disproves a claim, fix it here, tagged
  `*[build: iter-XX]*` with the evidence.
