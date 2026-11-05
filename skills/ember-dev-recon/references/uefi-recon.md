# PC firmware recon: UEFI/PI, coreboot, legacy BIOS

How to recon a PC firmware image for an ember x86-64 board (the `ember-dev-build` skill next
to this one, `../ember-dev-build/references/pc-platform.md`). A PC differs from an embedded target in where the facts are:
there is no base to search (the BIOS region ends at 4 GiB, the processor starts at
0xFFFFFFF0, UEFI modules carry their own image base), and the device map is the chipset, found
through I/O ports, PCI configuration and MSRs as much as through MMIO. Tool output quoted here
is from `uefi_scan.py` and `FindX86Io.java`; run them through `recon.sh` (SKILL.md, *PC
firmware*). Fact sheet shape: `recon-report-schema.md`, *PC firmware*.

## 1. What the build side consumes

| RECON fact | lands in (ember-dev-build: `templates/board-pc.c.in`, `references/pc-platform.md`) |
|---|---|
| flash image and BIOS window | `FLASH=bios.bin ./run.sh` (`--image flash=`, ends at 4 GiB, at most 16 MiB), or `CODE=` + `VARS=` for an OVMF split; a full SPI image when the firmware reads the descriptor back through the SPI controller |
| CPU vendor, signature, brand, address width | `BOARD_CPU_VENDOR` / `BOARD_CPU_SIGNATURE` / `BOARD_CPU_BRAND` / `BOARD_CPU_PHYS_BITS` (`uam_x86_set_cpuid`) |
| chipset PCI functions (b:d.f, vendor/device IDs compared, config registers written) | one chipset device per function in `emulation/` (ember-dev-build's `references/examples/pc/`: `q35_mch`, `ich9_lpc`, `i801_smbus`, `pch_spi`, `superio`) |
| fixed MMIO windows (MCHBAR, DMIBAR, RCBA, SPIBAR, PMC, TPM, ...) | `board_windows[]` catch-alls, then devices; windows set by a BAR belong to the device that owns the BAR |
| I/O ports | template devices, a chipset device, or a probe `--mmio-rule 0x10000000000+port=...` (`UAM_X86_PIO_BASE` + port); the "io" catch-all logs the rest and reads 0 |
| MSRs | `board_rdmsr` / `board_wrmsr` (platform MSRs only; see 7.4) |
| memory size, TOLUD, SPD, CMOS | `BOARD_LOW_RAM_MAX`, SPD contents of the SMBus model, `--image cmos=` |
| SMRAM, SMI sources | host bridge (SMRAM windows + the SMM view), LPC/PM device; `-o apm-smi=1` meanwhile |
| console, POST codes | COM1 0x3F8, `-o debugcon=0x402`, port 0x80: the first milestones |
| processors | `-o cpus=N`; how APs are started (INIT/SIPI vector, AP stack) |

## 2. Flash layout: descriptor, regions, BIOS window

- **Intel flash descriptor**: signature `0x0FF0A55A` at 0x10 (0x0 on old ICH parts).
  `uefi_scan.py` prints each region (descriptor, BIOS, ME, GbE, PDR, EC, ...) with its flash
  range, then `BIOS region flash A-B -> physical X-Y`: only the BIOS region is decoded below
  4 GiB, ending at 0xFFFFFFFF. Volume offsets are file offsets, printed with the decoded
  physical address next to them.
- The **ME region** is the CSME's; host firmware sees the ME through HECI (PCI 00:16.0), whose
  status registers PEI reads. That is a PCI function to answer, not flash to emulate.
- **No descriptor**: the whole file is taken as the BIOS region ending at 4 GiB (`OVMF_CODE.fd`).
  An OVMF split has the variables below the code: scan `OVMF_VARS.fd` with
  `--top <4 GiB - code size>` (or scan the concatenation `VARS + CODE`).
- **coreboot**: `uefi_scan.py` lists the FMAP areas and the CBFS files (offset, type, size,
  name); it does not extract them (coreboot's `cbfstool` does). Uncompressed FSP components
  in CBFS are firmware volumes and show up as such.
- Record the flash size, which regions the board must hold, and every region the firmware
  writes (variable store, FTW, vendor NVRAM): those need a writable flash model. Also the part
  its flash driver expects: the CFI query or JEDEC ID values it compares.

## 3. Reset vector, FIT, microcode: the CPU identity

- **Reset vector** (`== reset vector 0xFFFFFFF0: ...`): the decoded jump and its target. A
  `jmp near` lands in the last 64 KiB (SEC / the Volume Top File); `jmp far F000:E05B` is a
  legacy BIOS; `code before the jump (...)` is OVMF's protected-mode check ahead of its real-mode
  jump; `erased` means the image does not end at 4 GiB as decoded (pass `--top`, or scan the
  whole flash).
- **FIT** (pointer at 4 GiB - 0x40): its entries by type (microcode, startup ACM, Boot Guard key
  and boot policy manifests, ...). ember runs no ACM and does no Boot Guard verification: the
  processor starts at the reset vector. Record the entries, and any code that checks their
  outcome.
- **Microcode updates**, found by a whole-image scan: `rev`, date, `CPUID` signature(s) with
  family/model/stepping, `platform` (the processor-flags mask). Each signature is a processor
  this image supports; `BOARD_CPU_SIGNATURE` should be one of them.
- **CPUID compares**: `FindX86Io` lists the CPUID leaves each module asks for and the functions
  asking. Read those functions for the vendor string and family/model constants they compare;
  they select silicon code paths. ember answers leaves 0, 1, 0xB, 0x80000000-0x80000004 and
  0x80000008 (SSE2-level features, no AVX); every other leaf reads 0. A firmware that needs
  leaf 4/6/7 contents, or SSE3+ features, is a finding for *Ember support*.
- ember implements PLATFORM_ID (0x17) and BIOS_SIGN_ID (0x8B) itself and reads both as 0: a
  microcode loader sees platform 0 and revision 0 after its WRMSR 0x79, and a board hook does
  not change that. Record any check on the loaded revision (an assert, a halt, a skipped path).

## 4. Firmware volumes and modules

`uefi_scan.py` prints each volume with its name GUID, file offset and physical address (`-`:
inside a compressed section, so in RAM at run time), size, file system (FFS2/FFS3, `NVRAM
(variables)`) and header checksum, then its files: type (`SEC`, `PEI_CORE`, `PEIM`,
`PEIM+DRIVER`, `DXE_CORE`, `DRIVER`, `SMM`, `SMM+DXE`, `SMM_CORE`, `MM_STANDALONE`,
`APPLICATION`, `FV_IMAGE`, ...), GUID, name (UI section, else the PE debug path, else a known
GUID), compression (`[LZMA]`, Tiano, Brotli) and per image: format (PE32/PE32+/TE), machine
(IA32/X64), image base, entry and XIP. From OVMF_CODE_4M.fd:

```
 FV 48DB5E17-...  offset 0x0 phys 0xffc84000  0x348000 bytes  FFS2
   FV_IMAGE  9E21FD93-... FvMainCompact (compressed DXE volume)  [LZMA]
    FV 6938079B-...  offset - phys -  0xe0000 bytes  FFS2 (in 9E21FD93-...)
      PEI_CORE  52C05B14-... PeiCore  PE32+ X64 base 0x820140 entry 0x826742
      PEIM      222C386D-... PlatformPei  PE32+ X64 base 0x82b9c0 entry 0x8346c0
    FV 7CB8BDC9-...  offset - phys -  0xe80000 bytes  FFS2 (in 9E21FD93-...)
      DXE_CORE  D6A2CB7F-... DxeCore  PE32+ X64 base 0x0 entry 0x13cbc
 FV 763BED0D-...  offset 0x348000 phys 0xfffcc000  0x34000 bytes  FFS2
   SEC       DF1CCEF6-... SecMain  PE32+ X64 base 0xfffcc094 entry 0xfffd4394 XIP 0xfffcc094 (rebased)
```

Reading the addresses:
- `XIP 0x... (rebased)`: stored uncompressed in a flash volume and linked at its flash address.
  It executes in place; Ghidra addresses (PE loader, image base) are run addresses.
- `XIP 0x... (NOT at its base)`: in flash but linked elsewhere. Something loads or relocates it
  before it runs; find what, and the run address.
- no XIP (compressed volume): loaded to RAM at run time. A nonzero base (OVMF's PEIMs at
  0x82xxxx) is the address the build placed it for, where the decompressed volume lands; base
  0x0 (OVMF's DXE drivers) means its loader relocates it. Run addresses then come from the boot
  log: edk2 debug builds print `Loading PEIM at 0x... EntryPoint=0x...` and `Loading driver at
  0x... EntryPoint=0x...`. Rebase the Ghidra program to match before citing addresses.
- `modules.tsv` carries the same rows (name, guid, type, format, machine, image_base, entry,
  xip, file) with absolute file paths; `uefi_scan.py --json` adds each image's PE debug path,
  whose directories name the silicon and platform packages.

Boot phases to record, with the module that implements each: SEC (reset vector, cache-as-RAM,
microcode, finds the PEI core), PEI before memory (chipset BARs, SMBus, memory init), PEI after
memory (DXE IPL, S3 resume), DXE (from the compressed volume), SMM, BDS / applications. Note
the machine per phase: Intel platforms often run PEI as IA32 and DXE as X64; OVMF runs both
as X64.

## 5. Which modules to analyse first

`--modules pei` (the default) imports SEC, the PEI core and every PEIM: the code that runs
first and programs the chipset. Within them, read in this order:
1. **SEC**: the reset path, cache-as-RAM (MTRR MSRs 0x200-0x20F and 0x2FF, CR0.CD/NW), the
   microcode load, the jump to the PEI core and what it hands over (stack, PEI core entry).
   Record the CAR range (the MTRR base/mask written, or FSP-T's TempRamBase): no caches are
   modelled, so the board must map RAM there.
2. **The platform and chipset PEIMs**: the ones with the most rows in their PCI tables, and
   names such as `PlatformPei`, `*InitPei`, `Pch*`, `Sa*`, `Cpu*`, `Silicon*`, `FspmWrapper*`,
   `MemoryInit`. They hold the BAR programming, device-ID compares and memory init.
3. **Status code / debug PEIMs**: which port or UART they write (the console).
4. DXE, SMM and other drivers later, by name, when the build reaches them
   (`--modules DxeCore,<driver>,...`).

Dispatch order follows the volume order and the dependency expressions, so a PEIM's position
in the volume is a first approximation of when it runs.

## 6. Variable store

`== variable store (<format>, <size> bytes): N live variables`, then name, vendor GUID and size
of each live variable (`VSS` or `authenticated` edk2 stores; AMI NVAR: names only, best
effort). The store sits in an `NVRAM (variables)` volume in the BIOS region; record its flash
offset, physical address and size. In edk2 the fault-tolerant-write working and spare blocks
follow the store in the same flash range; the board's flash must accept writes over all of it
(`-o persist=1` keeps them). A fresh store (OVMF_VARS: 0 live variables) is normal: the
firmware creates its defaults on first boot. List the variables PEI reads (`Setup`,
memory-configuration and boot-mode variables): a missing one selects a default path, which
may differ from the board's normal boot.

## 7. Hardware access: reading FindX86Io output

One `OUT/ghidra/<module>.x86io.txt` per module. Operands resolve by constant propagation in the
function, or at the call sites of small accessors (edk2 `IoRead8(Port)`, `AsmReadMsr64`,
`AsmCpuid`, `PciRead32`), following wrappers up to three deep; those rows name the caller and
`via <accessor>`. The header gives the count of rows resolved that way and of sites still
unresolved.

### 7.1 I/O ports
`port R W sizes values-written functions`. Classify each port:

| ports | device | ember side |
|---|---|---|
| 0x20/0x21, 0xA0/0xA1, 0x4D0/0x4D1 | 8259 pair, ELCR | template |
| 0x40-0x43, 0x61 | 8254, port 61h | template |
| 0x60/0x64 | 8042 | template |
| 0x70-0x73 | RTC/CMOS (values written to 0x70/0x72 are CMOS offsets) | template; `--image cmos=` |
| 0x80 | POST codes (values = checkpoints) | template |
| 0x92 | system control A (A20, fast reset) | template |
| 0x3F8-0x3FF | COM1 | template (the console) |
| 0xCF8/0xCFC-0xCFF, 0xCF9 | PCI configuration, reset control | template; 0xCF9: chipset (LPC reset control, given to `uam_pci_cam_passthrough`) |
| 0x402 | edk2 debug port (OVMF) | `-o debugcon=0x402` |
| 0xB2/0xB3 | APM control/status: software SMI | chipset (LPC/PM); `-o apm-smi=1` meanwhile |
| 0x2E/0x2F, 0x4E/0x4F | Super I/O index/data (enter key, LDN in 0x07, base in 0x60/0x61, activate in 0x30) | chipset (`superio`) |
| 0x510/0x511 (0x514) | fw_cfg, the firmware configuration interface of the virtual platform OVMF is built for | a fw_cfg device |
| anything else | a BAR-relative block (PMBASE, SMBus, GPIO) or a vendor port | chipset; unresolved sites (7.6) |

### 7.2 PCI configuration
Two tables. `PCI configuration addresses written to 0xCF8`: each constant CF8 address,
decoded `bb:dd.f reg 0xRRR (0x8.......)` (address bits 27:24 become register bits 11:8), with
the functions writing it. `PCI configuration through the PCI library`: constant
`PCI_LIB_ADDRESS(bus, dev, fn, reg)` arguments of the functions that drive 0xCF8, decoded the
same way, with R/W counts, sizes and the values written. Together they are the chipset map:
- which functions exist (b:d.f), and which registers each one's code programs: BARs and
  decode bases (PMBASE, RCBA, MCHBAR, PCIEXBAR, SMBus base, ...), with the values written,
  which give the fixed windows and port blocks the rest of the code uses;
- **identity**: reads of register 0x000/0x002 (vendor/device ID) and 0x008 (revision) on
  00:00.0 (host bridge) and 00:1f.0 (LPC/eSPI). Open the reading functions and collect every
  constant compared and the path each selects (`recon-pitfalls.md` 8). With the BIOS ID,
  module names, PE debug paths and option-ROM IDs (`== PCI option ROMs`: vendor:device,
  class) this names the chipset generation.
- After PCIEXBAR is set, configuration may go through ECAM: MMIO at
  `base + (bus << 20 | dev << 15 | fn << 12 | reg)`. Those accesses are not PCI rows; look
  for them among the MMIO rows (FindMmio, 7.5) or the unresolved sites, and decode by hand.

### 7.3 CPUID
`leaf xN functions`: the leaves asked and by whom. See 3 for what to read and what ember
answers.

### 7.4 MSRs
`index rd N wr N values-written functions`. Split the table:
- **ember implements**: TSC 0x10, PLATFORM_ID 0x17, APIC_BASE 0x1B, FEATURE_CONTROL 0x3A,
  TSC_ADJUST 0x3B, BIOS_SIGN_ID 0x8B, SMBASE 0x9E (in SMM), MTRRCAP 0xFE, SMRR 0x1F2/0x1F3
  (in SMM), SYSENTER 0x174-0x176, MCG 0x179/0x17A, MISC_ENABLE 0x1A0, the MTRRs (0x200-0x21F,
  0x250, 0x258/0x259, 0x268-0x26F, 0x2FF; storage), PAT 0x277, EFER and the syscall/segment
  MSRs 0xC0000080-0xC0000103, and AMD's SMM_BASE 0xC0010111 (with the AMD64 SMM save-state
  map). x2APIC 0x800-0x8FF and TSC_DEADLINE 0x6E0 go to the local APIC.
- **everything else** (platform info, power and thermal, uncore, vendor MSRs) goes to the
  board's `board_rdmsr`/`board_wrmsr`, then to the MSR policy: writes remembered and read
  back, unknown reads 0 (`-o msr=gp`: #GP). Mark each such MSR with what the code does with
  the value it reads (compares, masks, polls): those need an answer from the board.

### 7.5 MMIO
`MMIO addresses passed to accessor functions`: constant addresses handed to small leaf
functions that do one load or store through their argument (`MmioRead32`-style), with R/W and
values. The rule catches any such helper, so RAM constants (CAR, HOBs, PCDs) appear too; the
device rows are the fixed chipset windows (0xFED00000-0xFEDFFFFF, 0xFEC00000 I/O APIC,
0xFEE00000 local APIC, 0xFED40000 TPM) and anything inside a BAR the PCI table programs.
`FindMmio.java` (`<module>.mmio.txt`) adds direct references: on x86 it counts addresses
below 0x80000000 as RAM, except 0xA0000-0xFFFFF.

### 7.6 Unresolved
Up to 40 `function (kind via accessor)` lines: the port, MSR or address is computed, read
from a table, or a BAR value. Read them. The base is usually a value the PCI table shows
being written to a BAR (PMBASE + 0x30 = SMI_EN on ICH9; SMBus base + 0..7).

## 8. Memory init and SPD

- **Who initializes DRAM**: FSP-M (an FSP wrapper PEIM calls into the FSP binary; FSP
  components carry an `FSPH` info header and their own volumes), an MRC inside a silicon
  PEIM, coreboot's romstage, or nobody (OVMF: the virtual platform's RAM exists; `PlatformPei`
  reads the size from fw_cfg or CMOS 0x34/0x35, 0x5B-0x5D).
- **SPD over SMBus**: the SMBus controller (Intel: 00:1f.3, I/O base from its BAR at 0x20) is
  driven as HST_STS +0, HST_CNT +2, HST_CMD +3 (the SPD byte), XMIT_SLVA +4 (`(0x50+slot) << 1
  | 1` for a read), HST_D0 +5. From the values written, record the slot addresses probed, the
  SPD bytes read and what decides the DRAM type (byte 2) and size. An empty slot answers
  DEV_ERR (`i801_smbus.c`).
- **Memory controller**: the MCHBAR registers the MRC programs, and every poll in it (training
  or init done bits), with what ends each wait.
- **Results**: what the firmware writes as TOLUD/TOUUD/TSEG base (host-bridge registers) and
  the memory it reports; the board's `BOARD_LOW_RAM_MAX` and RAM size follow them.
- **Boot mode**: where S3 resume vs normal boot is decided (PM1 control SLP_TYP, CMOS, a
  variable), so the board can present a cold boot.

## 9. SMM

SMM modules (`SMM`, `SMM+DXE`, `SMM_CORE`, `MM_STANDALONE`), and from the code: SMRAM (A/B
segment and TSEG: the host-bridge registers and values written), the SMRR MSRs (0x1F2/0x1F3),
SMBASE relocation (who sends the SMIs, the new bases), the SMI sources (APM 0xB2, SMI_EN /
SMI_STS in the PM block, GBL_SMI_EN, EOS) and the lock bits set (SMI_LOCK, D_LCK). The build
phase models these in the host bridge and the LPC/PM device (`q35_mch.c`, `ich9_lpc.c`).

## 10. Console and progress markers

- POST codes: the values written to 0x80 and the functions writing them; a list of
  checkpoints the board run can be measured against.
- Serial: whether the firmware writes COM1 directly (0x3F8: LCR, divisor, LSR polls at
  0x3FD), after enabling it in the Super I/O (which LDN, which base), or a vendor UART (a PCI
  or MMIO UART in the PCH).
- Debug port 0x402 (OVMF), and whether this is a debug build at all (DEBUG strings). A
  release build with no console leaves POST codes as the only marker.

## 11. Legacy BIOS

Reset vector `jmp far F000:E05B`, tables `$PIR` (IRQ routing), `_32_`, `$PnP`, `_SM_`/`_SM3_`,
option ROMs (`55AA` + `PCIR`): `uefi_scan.py` reports all of them. There are no modules: import
the image by hand (`recon.sh --arch x86_16 --base ...` for the real-mode part, `x86_32` for
protected-mode code). Record the shadowing (PAM registers in the host bridge) and the
decompression of its modules into RAM.
