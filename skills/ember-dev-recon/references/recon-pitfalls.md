# Recon pitfalls: checks to run before handing RECON.md over

Each item is a class of error that a static recon pass makes easily and that only surfaces
later, as a confusing blocker in the build phase. Run through this list before step 7.

## 1. Scan every code stage, including the one that runs from flash
The decompressed main OS is the obvious target, but the **bootstrap that executes from the
reset vector** (often uncompressed and XIP: run in place from flash) does the hardware
bring-up the later stages assume is done: PLL/DPLL programming, DRAM init, on-chip SRAM use,
chip-reset tricks.
- Disassemble it at its run address (MIPS: `--base 0xBFC00000` on the first flash bytes up to
  the next stage; use `--limit`). Run `mmio_scan.py` and `flashmap_scan.py` on it.
- Record, per stage, whether it runs **XIP** (jumps to flash addresses such as `0x9Fxxxxxx` /
  `0xBFxxxxxx`, or runs C code there) or from RAM. An XIP stage needs the flash mapped as ROM or ROMD
  in the board (fetchable), not as a device-only MMIO window.
- Look for **self-reset tricks**: a magic value written to some memory, followed by a
  chip-reset bit and a spin loop. The memory must survive the reset, and the reset bit must
  actually reset the chip (a reset line to `uam_machine_reset_sink`).

## 2. Not every non-RAM page is a peripheral
A kseg1 page that is only stored to and loaded from, with magic values or counters and no
polled bits, is likely **on-chip SRAM / scratch memory**: a boot marker, a debug counter, a
cache-as-RAM stack. List it in the memory map as memory, with a size estimate and whether it
must survive a reset. Leaving it unmapped makes the first access a bus error.

## 3. Requirements need a code path, not an access count
"Register X is touched 96 times, so it must count / must return Y" is a hypothesis. State a
requirement as fact only when code shows the dependency:
- a **polling loop** on the value (`mmio_scan.py` POLL lines; also wait helpers taking
  `(reg, mask, value, timeout)` that a linear scan can't see),
- a **divisor** or array index derived from it (zero, so a trap),
- a **compare** that selects a code path (chip ID, revision, straps).

Static counts include dead code, init-once paths and write-only registers. A register can be
"hot" in the scan and never be read at runtime.

## 4. Size device windows from the union of all stages
A block's window must cover the highest offset any stage touches (`max_off` per page in
`mmio_scan.py`), rounded up to the block's natural size. On SoCs, **foreign registers live
inside other IP blocks**: clock DPLLs inside a radio's analog block, PHY controls inside a
switch. A window sized from the main OS's offsets alone can miss the bootstrap's.

## 5. Label flash partitions from their writers and from layout tables
- A region read at one fixed address is not proven to be "the config". Name a partition from
  the function that **writes** it, and that function's strings (`"Commit flash"`, `"log"`,
  `"upgrade"`, `"bootline"`).
- Look for **partition tables stored as plain offsets** (`flashmap_scan.py`): arrays such as
  `{0x40000, 0x1c0000, 0x1d0000, 0x1e0000, 0x1f0000}` reached as `flash_base + table[i]`. They
  never appear as absolute addresses, so an absolute-reference scan alone misses them.
  `get_xrefs_to` on the table finds the per-partition accessors and their size limits.
- Distinguish the kinds: boot environment / RTOS NVRAM (boot line), application config,
  wireless/NVRAM key=value text, event/exception logs, upgrade state, calibration (ART/EEPROM),
  MAC address. They are often separate sectors.
- One sector can hold several things at fixed sub-offsets (`+0x0` boot line, `+0x5000` app
  NVRAM). Record sub-offsets.

## 6. Inputs: record polarity and the safe idle value
For every input the code reads (GPIO buttons, straps, presence detects), record which value
the code treats as **active**. Ember's default for unmodelled registers (read-as-zero) must be the idle
state. Otherwise a "pressed" reset button triggers a factory reset or a recovery mode. On a PC
board the catch-alls read 0 but an address nothing claims master-aborts (reads all ones):
record which of the two each presence check or strap read expects. On a microcontroller,
port pins read through PINx/PORTx/Px registers: record the pull-up state the code sets and
the level it treats as pressed.

## 7. Displayed values may be software state
Before claiming that a value shown by the OS (interface counters, link speed, uptime) comes
from a hardware register, find the code that produces it. Many drivers keep counters in
software, and some never update them. A missing number can be firmware behaviour, not a
missing device.

## 8. ID/revision compares: list every constant
Collect every constant the revision/chip-ID register is compared with, across **all
stages**, and which code path each selects (`LOOPCMP` lines in `mmio_scan.py` are often
these). A wrong ID silently routes the firmware down another SoC's paths: wrong UART driver,
wrong delays, a skipped LED or PHY setup. Boot can still "work".

## 9. PC firmware: addresses, modules and identity
- A module's Ghidra addresses are its image base. They are run addresses only for an XIP
  module stored at its base (`uefi_scan.py`: `(rebased)`); for the rest, say where it runs.
- Analyse SEC and the PEI modules, not only DXE: chipset BARs, memory init and SPD reads all
  happen before DXE exists.
- A CPU signature needs a microcode update for it and the CPUID compares that accept it; a
  chipset needs its device-ID compares. One source alone is *inferred*.
- Ports and MMIO behind a BAR (PMBASE, SMBus, GPIO, MCHBAR) show up as unresolved sites:
  resolve them through the value the PCI table writes to that BAR.

## 10. Microcontrollers: spaces, word addresses, configuration
- **Word or byte addresses.** Ghidra's AVR8 and PIC-12/PIC-16 code addresses count 16-bit
  words; ember's pc, `--break`, `--fuzz-begin` and coverage sites are byte addresses (twice
  as large). PIC18 and the 8051 are byte-addressed everywhere. A wrong factor puts every
  breakpoint in the wrong function.
- **Which space.** A data address means nothing without its space: AVR data 0x00E0 is bus
  0x8000E0, 8051 SFR 0x99 is bus 0x2000099 and XDATA 0x0099 is bus 0x1000099, Z80 port 0x10 is
  bus 0x10010. Write the space in every MMIO row.
- **Core registers are not devices.** STATUS/SREG/SP, PIC's FSR/PCLATH/BSR/INTCON, the 8051's
  ACC/B/PSW/SP/DPTR/IE/IP and the model's extended interrupt registers belong to ember's hart;
  only the rest is the board's. They top FindMmio's counts and must not become device models.
- **Unanswered accesses halt.** On the AVR, PIC, 8051, 6502 and Z80 cores an access nothing
  answers stops the core with a message (no bus-error exception to take), so every register
  the boot path reads needs a device, RAM or a catch-all; list them all, including the ones
  only read once.
- **Configuration words, fuses, EEPROM** come with the HEX file, not with the code: record
  their values (oscillator, watchdog enable, XINST, boot reset vector, code protection). A
  watchdog left enabled by a fuse is a reset the firmware expects to be cleared regularly.
- **Espressif images** run partly through the flash cache: IROM/DROM segments execute and
  read at cache addresses that the bootloader's MMU setup maps onto flash offsets, and app code
  calls ROM functions at fixed addresses. Record the mapping and every ROM function called; the
  board needs both. Code that uses TIE instructions (ESP32-S3 vector/AI libraries) traps in
  ember: note where.

## 11. Confidence on every claim
Tag claims as **confirmed** (code path + cross-check), **inferred** (static evidence, one
source) or **guess** (naming convention, public map of a sibling chip). The build phase
trusts confirmed claims and verifies the rest. A fact sheet that states inferred claims as
confirmed costs a full debugging iteration each time one is wrong.
