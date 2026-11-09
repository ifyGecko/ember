# PC platforms: x86-64 boards, UEFI, coreboot and legacy BIOS firmware

A PC board is an x86-64 board like any other ember board - same organization, same
iteration loop, same snapshot rules - but its firmware expects a whole platform: a chipset
it programs before memory exists, interrupt controllers in three generations, timers it
calibrates against each other, SPI flash it writes variables to, SMM, several processors.
This page is how that platform maps onto ember, what the generated PC board already has,
what you add per board, and how a UEFI (or coreboot, or legacy BIOS) boot fails and gets
unblocked phase by phase. Recon for such images is ember-dev-recon's
`references/uefi-recon.md`.

Tested pieces this page refers to:

| piece | where | what it proves |
|---|---|---|
| PC board template | `templates/board-pc.c.in` (`new-emulation.sh --isa x86_64`) | builds warning-free; runs ember's x86 self-test (real, protected, long mode, SMM, SMRR) to PASS |
| chipset examples | `references/examples/pc/` | each one exercised by `skills/tests/fixture/pc/fw` from the reset vector, interpreter and JIT, snapshot-split |
| a Q35-class board from them | `skills/tests/fixture/pc/pcbrd_board.c` | **boots OVMF to the UEFI shell** (`skills/tests/run-tests.sh`, step 4) |

## 1. How a PC maps onto ember

| PC concept | ember |
|---|---|
| the processor | `uam_isa_x86_64` (`uam/isa/x86.h`): resets in real mode at 0xFFFFFFF0 (CS 0xF000, base 0xFFFF0000; EAX 0, EDX = the CPUID signature, SMBASE 0x30000) and follows the firmware into protected and long mode; x87/MMX/SSE/SSE2, no AVX/VMX. An entry below 0xFFFF0000 starts flat real mode at that address (raw stages, tests) |
| I/O ports (IN/OUT) | ordinary bus accesses at `UAM_X86_PIO_BASE + port` (2^40): port devices are MMIO devices, catch-alls, `--mmio-rule`s, snapshots and the bus map all apply. `uam_x86_pio(port)` |
| physical memory | the bus is physical; paging is the ISA's. RAM below the MMIO hole (TOLUD) and above 4 GiB; the boot flash ends at 4 GiB |
| unclaimed addresses | a PC master-aborts: reads all ones, writes dropped. Firmware depends on it (an absent TPM at 0xFED40000, an absent PCI function's MMIO, SMRR-protected memory read outside SMM). The PC `run.sh` therefore defaults to `UNKNOWN_MMIO=fault`, which makes the board install `emulation/master_abort_log.c`: master abort that **logs the first access to each address** (`master-abort: first read ... (nothing decodes it: all ones)`) and the hottest 64 KiB pages at exit; ember's own `uam_master_abort_create` is silent, which hides a forgotten window (the firmware reads 0xFF.. and takes the "absent" path). `-o abort-log=0` silences it; `UNKNOWN_MMIO=zero` reads unclaimed addresses as 0 instead |
| interrupts | `UAM_X86_IRQ_INTR` from the local APIC (`uam/dev/apic.h`), which passes the 8259 through LINT0 (ExtINT); I/O APIC messages and MSI writes travel on an APIC bus; NMI, SMI, INIT, A20M are their own inputs. ISA IRQs go to both the 8259 and the I/O APIC (IRQ 0 on pin 2); PCI INTx through the chipset's PIRQ router |
| PCI | `uam/dev/pci.h`: a bus with CAM (0xCF8/0xCFC) and ECAM views, functions with 4 KiB config spaces and write masks, BARs that map themselves where the firmware programs them, bridges, MSI/MSI-X, INTx routing, bus-master DMA, expansion ROMs, a pass-through for narrow CONFIG_ADDRESS accesses (the board's 0xCF9 reset control) |
| timers | 8254 (with port 61h), MC146818 RTC/CMOS, HPET, local APIC timer (and TSC-deadline), ACPI PM timer (`uam/dev/acpi_pm.h`), the TSC (counts virtual ticks). A board picks `tick_hz` (the template: 1e9, one instruction = 1 ns); devices convert with `uam/dev/clock.h` |
| boot flash | CFI (Intel command set, `uam/dev/cfi_flash.h`: what OVMF's flash driver programs; `uam_cfi_flash_set_write_gate` applies a chipset's rules, such as writes only in SMM) or, on real boards, SPI behind the PCH's controller (`examples/pc/pch_spi.c`); both are ROMD regions, so the array executes in place and snapshots cover it |
| chipset decode (PAM, SMRAM, TSEG, PCIEXBAR, PMBASE, RCBA, Super I/O bases) | regions a chipset device owns and moves/switches at run time when firmware programs it: `uam_bus_region_move`, `uam_bus_region_enable`, `uam_map_attrs.disabled`; ember revalidates translated code after every change. SMRAM is the exception for SMM entry and exit: never switch regions then - SMRAM lives in the SMM view (row SMM) |
| SMM | SMI#, the Intel 64 and AMD64 SMRAM save states (`uam_x86_set_smm_map`), RSM (a saved state no processor could be in shuts the hart down: `x86: RSM with an invalid saved state (...)`), SMBASE relocation, I/O instruction restart. SMRAM is decoded per access by the accessing processor's SMM state: a processor in SMM presents its accesses to an SMRAM window (`uam_x86_set_smram_window`, set by the host bridge for every processor from its SMRAM registers; `UAM_X86_SMRAM_DATA_CLOSED` for D_CLS) at `UAM_X86_SMM_VIEW` (2^46) + address, where the board maps SMRAM (an alias of low RAM); other processors, DMA and the debugger see the normal decode, so nothing switches on SMM entry/exit (`smm_changed` remains for chipset registers that track SMIACT#). To read SMRAM as SMM sees it: `--dump-mem p:0x400000000000+ADDR+LEN`. SMRR-protected ranges and, with the AMD64 map, the TSeg of SMM_ADDR/SMM_MASK read through `UAM_X86_SMRR_SINK` (2^47, master abort) outside SMM. With the AMD64 map the core owns HWCR, SMM_ADDR and SMM_MASK (SMMLOCK makes them and SMM_BASE read-only until RESET): an AMD board drops its own copies and, where the security processor programs the TSeg before the cores run, calls `uam_x86_preset_amd_smm`. The ASeg is an SMM-view window while SMM_MASK.AValid is set (AClose closes it for data): an AMD board maps the DRAM behind it at `UAM_X86_SMM_VIEW + 0xA0000`. Intel's SMM code-access check is offered with `UAM_X86_FEAT_SMM_CODE_CHK`; `uam_x86_set_smm_confine` (`-o smm-confine=1` on the PC template and `x86_64-ref`) makes any SMM fetch outside SMRAM a machine check on either vendor. An AP waiting for its startup IPI holds an SMI until the SIPI (then takes it before its first instruction): SMM rendezvous code must not wait for it, or the board offers Intel's MSR_SMM_BLOCKED (`UAM_X86_FEAT_SMM_BLOCKED` + `uam_x86_set_package`). SMI and NMI end the shutdown state |
| CR4 | MOV to CR4 raises #GP(0) on a bit whose feature CPUID does not report (the board's `cpuid` hook included); ember reports DE, MCE, PSE, PAE, PGE, TSC, FXSR, SSE. RSM shuts down on such a bit. CR4.DE makes DR4/DR5 #UD |
| CPUID / MSRs | `uam_x86_set_cpuid` (vendor, signature, brand, topology), and the platform hook `cpuid` to change any leaf's answer; the platform hooks `rdmsr`/`wrmsr` for the MSRs a board answers; everything else falls to the MSR policy (remember writes, read them back; or #GP). MTRRs are storage (no caches are modelled) |
| several processors | `-o cpus=N`: processor 0 is the BSP, the others wait for INIT/SIPI from their local APIC (`uam_x86_sipi`); in wait-for-SIPI a processor holds an SMI until its startup IPI and takes it before its first instruction, as hardware does (start the APs before broadcasting SMIs, e.g. for SMBASE relocation, or offer `UAM_X86_FEAT_SMM_BLOCKED` so the SMM rendezvous skips them); an INIT IPI parks any processor, the BSP too, in wait-for-SIPI. Every processor sees its own APIC at the register page (`uam_lapic_window_create`). The processors take turns, `--quantum N` instructions each (default 4096): deterministic, but a smaller quantum shortens a rendezvous spin |
| storage, display, input | AHCI, NVMe, IDE (`uam/dev/ahci.h`, `nvme.h`, `ide.h`; a board you wire one into opens its image with `uam_board_open_disk`, e.g. `-o disk=PATH`; the template wires none), floppy (`fdc.h`), ISA DMA (`i8237.h`), VGA (`vga.h`: text and planar modes, option ROM), PS/2 (`i8042.h`), parallel port (`lpt.h`). No USB (xHCI/EHCI), no PC NIC, no GOP framebuffer: the console is COM1 |

## 2. The generated PC board

`new-emulation.sh <board> --isa x86_64` writes `emulation/<board>_board.c` from
`templates/board-pc.c.in`. Out of the box it has: the processors (`-o cpus=N`), RAM below
`BOARD_LOW_RAM_MAX` and above 4 GiB, the boot flash (`--image flash=` or `code=` + `vars=`,
CFI, `-o persist=1` writes back), a local APIC per processor on an APIC bus, the I/O APIC,
the 8259 pair + ELCR, 8254 + port 61h, RTC/CMOS (memory-size bytes, or `--image cmos=`),
HPET, 8042 (reset and A20 lines), COM1 (the console, also the board input `com1` for
ember-fuzz), POST codes at 0x80, port 92h, the A20 gate, `-o debugcon=0x402`, the PCI bus
at 0xCF8 with MSI to the APIC bus, an "io" catch-all over the whole port space,
`board_windows[]` catch-alls for the chipset's fixed MMIO, `-o msr-trace=1` / `-o msr=gp`,
`-o smm-map=intel64|amd64` (the default follows `BOARD_CPU_VENDOR`), `-o smm-code-chk=1`,
`-o smm-blocked=1`, `-o smm-confine=1`,
`-o apm-smi=1` (a plain software-SMI port at 0xB2 until the chipset's SMI logic exists),
`--image os=` for a raw stage entered in real mode, and a logged master abort for
unclaimed addresses (`master_abort_log.c`, generated next to the board). What it does not have is the chipset: that is where a board's work is.

Fill these from RECON first:
- `BOARD_CPU_VENDOR/SIGNATURE/BRAND` - a signature the firmware has microcode for and its
  silicon code supports (RECON: microcode signatures, CPUID compares). Firmware picks code
  paths by family/model; a signature it does not know ends in a dead loop or an ASSERT long
  before any device is touched. `BOARD_CPU_PHYS_BITS` as the platform's CPUs report it
  (page-table and MTRR mask widths follow it). `AuthenticAMD` or `HygonGenuine` also selects
  the AMD64 SMRAM save state and with it AMD's SMM_ADDR/SMM_MASK/HWCR.SMMLOCK in the core.
- RAM: `BOARD_LOW_RAM_MAX` is the TOLUD the firmware programs (or the size it computes);
  `--ram` decides how much lands above 4 GiB. Cache-as-RAM is **not** free here: no caches
  are modelled, so the range SEC turns into its stack (the MTRR base/mask it programs, or
  FSP-T's TempRamBase; RECON) needs RAM mapped there, or the first `push` faults.
- CMOS: the template writes the PC/AT memory-size bytes (0x15-0x18, 0x30-0x31). OVMF, a
  virtual-machine firmware, reads extended bytes when it has no fw_cfg port: 64 KiB units
  above 16 MiB at 0x34-0x35 and above 4 GiB at 0x5B-0x5D (`skills/tests/fixture/pc/pcbrd_board.c`
  `cmos_init`). Anything else the firmware keeps in CMOS (setup options, a checksum) goes
  in a `tools/`-generated `--image cmos=`.
- The flash: a whole SPI image (descriptor + ME + BIOS) needs the SPI controller
  (`examples/pc/pch_spi.c`), which decodes only the BIOS region below 4 GiB; the CFI
  template path takes the BIOS region alone (`--image flash=`), or OVMF's `code=` +
  `vars=`. Give the CFI part the IDs the firmware's flash driver probes for
  (`uam_cfi_flash_set_ids`; the fixture board gives OVMF an Intel part, 0x89/0x18).
- `board_windows[]`: one catch-all per fixed chipset MMIO window in RECON (MCHBAR,
  DMIBAR, RCBA, SPIBAR, PMC, the sideband port window, TPM), so their first accesses are
  named in the log instead of master-aborting silently.
- `board_entry_regs[]` only matters for `--image os=` stages; a reset-vector boot starts
  with the architectural reset state.

## 3. The chipset layer

One device per PCI function (or per fixed block), each in its own `emulation/` file with
its own save/restore, wired by the board. Add them in the order the firmware needs them -
the next blocker decides - but the order is nearly always the same: host bridge, LPC/PMC
(ACPI timer, SMI, reset), SMBus + SPD (memory init), SPI (variables), Super I/O (console),
then whatever DXE drivers probe. The tested examples, and how `pcbrd_board.c` wires each:

| example | function | models | the board wires |
|---|---|---|---|
| `q35_mch.c` | 00:00.0 host bridge | PCIEXBAR -> ECAM window, PAM shadowing of 0xE0000-0xFFFFF, SMRAM/ESMRAMC (A-segment and TSEG are RAM only to a processor in SMM: low RAM mirrored in the SMM view + two SMRAM windows per processor; D_OPEN, D_CLS, D_LCK), TOLUD/TSEGMB storage | `q35_mch_create(&(q35_mch_config){ sys, pci, low_ram, low_ram_top, boot_flash, smm_view = UAM_X86_SMM_VIEW, smram_window, smram_ctx })`; the board's `smram_window` callback calls `uam_x86_set_smram_window` on every processor (a device file may not include `uam/isa/x86.h`); no `smm_changed` wiring |
| `ich9_lpc.c` | 00:1f.0 LPC | PMBASE + ACPI_EN -> ACPI PM block (PM1, PM timer, GPE0, SMI_EN/SMI_STS, TCO), APM 0xB2/0xB3 -> SMI#, PIRQA-H routing, RCBA, S5 power-off, 0xCF9 reset | `ich9_apm` at port 0xB2; `uam_pci_cam_passthrough(pci, ich9_reset_control(lpc))`; `uam_pci_set_intx_router(pci, ich9_pci_intx)`; `ich9_set_irq_targets`; `ich9_smi_line` -> `smi_all`; `ich9_sci_line` -> ISA IRQ 9; `ich9_reset_line` -> `uam_machine_reset_sink`; `ich9_set_poweroff` |
| `i801_smbus.c` | 00:1f.3 SMBus | host controller (quick, byte, byte-data, word-data commands; block and I2C-read answer FAILED), SPD EEPROMs at 0x50-0x57 (DEV_ERR for an empty slot), DDR4 page select | `i801_smbus_add_spd(smb, 0x50, spd)` from `--image spd=`; `uam_pci_register(pci, i801_smbus_func(smb), 31, 3)` |
| `pch_spi.c` | 00:1f.5 SPI | descriptor regions, the BIOS region as a ROMD window ending at 4 GiB, hardware-sequencing read/write/erase, BIOS_CONTROL.WPD, protected ranges | `pch_spi_create(..., window_top, vid, did, jedec)`; `pch_spi_window` as the boot flash; `uam_pci_register(pci, pch_spi_func(spi), 31, 5)` |
| `superio.c` | LPC 0x2E/0x4E | config mode (enter/exit keys), logical device select, activate, I/O base, IRQ | the UART's region mapped `.disabled`; `superio_add_ldn(sio, ldn, region, default_base, ...)` hands it over |

A real platform's parts differ in IDs and offsets (a 100-series PCH's PMC, eSPI, P2SB,
sideband ports), not in kind: copy the closest example, change the register layout to the
datasheet, and keep the wiring. `references/device-anatomy.md` has the device contract;
`references/snapshots.md` what each device owes a snapshot.

## 4. The first runs

```bash
CODE=OVMF_CODE.fd VARS=build/vars.fd timeout -k 5 300 ./run.sh --jit -n 3000000000 \
    > build/console.txt 2> build/emu.log < /dev/null      # or FLASH=bios.bin
python3 scripts/log_summary.py build/emu.log
```

Copy the variable store into `build/` before the first run: with `-o persist=1` the
firmware writes back to it, and a run must not change its own inputs between iterations. Use
`--jit` once the board is stable: a PC firmware boot is billions of instructions.

**Expect no console output.** A release UEFI build prints nothing until DXE starts its
serial console; a debug build prints from SEC, but often to a debug port (edk2: 0x402,
`-o debugcon=0x402`), not COM1. Until then, progress is read from:
- POST codes (`-v -v`; `log_summary.py` lists them in order, the last one is where the
  firmware got to; decode them with the vendor's table or the firmware's own constants);
- `log_summary.py`'s first distinct events: I/O ports are named (`port 0x510 (fw_cfg ...)`,
  `port 0xcf9 (reset control)`, ...), and the first unclaimed port or window in time order is the
  candidate blocker;
- MSRs: `-o msr-trace=1` logs every MSR no device answers; `-o msr=gp` makes them fault,
  to find the one a loop or a bad decision comes from;
- the `master-abort:` lines (`log_summary.py` groups them by 64 KiB page): every address
  nothing decodes, read as all ones. A missing ROM, flash or chipset window shows up here
  long before the failure it causes; check each page against RECON before assuming the
  firmware meant to probe an absent device.

What a bare template does with OVMF (`OVMF_CODE_4M.fd`, no chipset, observed):

| run | log | meaning | fix |
|---|---|---|---|
| 1 | `port 0x510/0x511 (fw_cfg)` first, then `STORE_XLATE #PF` at an address above RAM, then `x86: triple fault` (exit 128) | no fw_cfg, so OVMF sizes memory from CMOS 0x34-0x35, which reads 0 | the extended CMOS memory bytes (section 2) |
| 2 | exit 124, no console, ~10^8 4-byte reads of `port 0x0` | a delay loop on the ACPI PM timer at an address of 0: OVMF's timer library finds PMBASE through the host bridge's device ID at 00:00.0, and an empty slot reads all ones | the host bridge (`q35_mch.c`), then the LPC (`ich9_lpc.c`) whose PMBASE holds the timer |

Both shapes recur on real firmware: **a hot 4-byte port read is a timer the firmware
polls** (PM timer, HPET counter, 8254 latch, port 61h refresh toggle) at an address that
only decodes once a chipset register is right; **a fault far outside RAM is a size or
base the firmware computed from a register or a byte that read 0 or all ones**. Trace
the computed value back with Ghidra before modelling anything.

Triage specific to x86 (the general table is in `references/iteration-loop.md`):

| symptom | usual cause | next step |
|---|---|---|
| `x86: triple fault` (then exit 125/128) | an exception with no usable IDT yet (SEC, early PEI), or a #PF while delivering one | the **first** `exc:` line before it; its pc in Ghidra (RECON module + XIP address) |
| a #GP at a `mov cr4, ...` early in boot | the firmware sets a CR4 bit for a feature the board's CPUID does not report (VME, OSXSAVE, SMEP, ...) | report the feature in the board's `cpuid` hook if the core implements it; otherwise the firmware checks CPUID wrongly |
| SMM rendezvous times out / an AP never arrives in SMM | the AP is still waiting for its startup IPI: it holds the SMI, as hardware does | start the APs before the first broadcast SMI, or offer `UAM_X86_FEAT_SMM_BLOCKED` so the handler skips them |
| `FETCH_FAULT (delivered as #GP: nothing decodes the address)` | a jump into a module that was never copied, or a flash window that moved (PAM, SPI BIOS window, a BAR) | which region should hold that address; the chipset register that maps it |
| `#UD` in SEC/PEI | an instruction ember lacks (AVX, VMX: see the gate) or code executed as data after a wrong jump | disassemble at pc; if real, an ember gap, stop and say so |
| reset loop (`resets=N` grows) | firmware asked for a reset: 0xCF9, port 92h, the 8042, a TCO timeout, or a "warm reset after memory init" its own state machine expects to survive | which writer; CMOS / chipset scratch / a sticky register usually records why |
| exit 124, hot unclaimed port or window | polling a device that isn't there | model it, or `--mmio-rule ADDR=fixed:V` first (a port: `0x10000000000 + port`) |
| exit 124, nothing hot, pc in a small range | spinning on RAM or a register that is now plain storage (MTRR-style), or an AP rendezvous | `--log-pc` / gdb on the loop; with `-o cpus=N`, check every processor reached its loop |
| exit 125 with interrupts expected | the PIT/HPET/APIC timer's line is not routed where the firmware unmasked it (8259 vs I/O APIC, PIRQ routing) | `--trace exc` to see deliveries; the routing registers the firmware wrote |
| `SetVariable` returns `EFI_UNSUPPORTED`, variables never persist, SMM drivers silent; `log_summary.py`: "software SMI lost" | nothing decodes port 0xB2: without `-o apm-smi=1` or the chipset's APM/SMI device the write lands in the "io" catch-all and SMM never runs | `ich9_lpc.c`-style APM_CNT + SMI_EN, or `-o apm-smi=1` as a stand-in |
| SMI raised, nothing happens / the run hangs after `0xB2` | SMI gated off (SMI_EN, GBL_SMI_EN, EOS), or SMRAM missing from the SMM view (the handler fetches all ones at SMBASE + 0x8000: no SMRAM window, or nothing mapped at `UAM_X86_SMM_VIEW` + address) | the LPC's SMI registers; the host bridge's `uam_x86_set_smram_window` calls and its SMM-view mapping; `--dump-mem p:0x400000000000+SMBASE+0x8000+16` |

## 5. Boot phases and their blockers

The PC milestone ladder (SKILL.md) with what each phase needs from the board. Journal the
phase reached in every `iter-XX.md`.

| phase | what runs | typical blockers | devices |
|---|---|---|---|
| reset -> SEC | the reset vector at 0xFFFFFFF0, real -> protected (-> long) mode, microcode load (FIT or a loop over the microcode region), cache-as-RAM | CPUID signature compares; MSRs the CAR setup reads back (`-o msr-trace=1`); no RAM under the CAR range; flash not decoded where the code expects (descriptor, BIOS window) | template, `pch_spi.c`, `BOARD_CPU_*` |
| PEI, before memory | PEI core, platform PEIMs: chipset early init, PMBASE/RCBA/PCIEXBAR, GPIO, the ACPI timer for delays, straps and boot mode (S3? recovery?) | host bridge / LPC registers; timer polls; boot-mode bits read as all ones (a "recovery" or "S3 resume" path taken by mistake) | `q35_mch.c`, `ich9_lpc.c` |
| memory init | MRC / FSP-M / coreboot raminit: SPD over SMBus, training registers in MCHBAR, then a memory test | SPD absent or not matching the DIMMs the firmware supports; training loops on MCHBAR status bits; a reset requested after training | `i801_smbus.c` + a real SPD dump; an MCHBAR device answering status/done bits (a stub is usually enough: ember RAM needs no training) |
| PEI, after memory | the switch from CAR to RAM, DXE IPL, page tables for long mode, S3 data | memory size from TOLUD / CMOS; the HOBs it builds (a wrong size faults far above RAM) | host bridge TOLUD/TOUUD, CMOS |
| DXE | DXE core and drivers: PCI enumeration (BARs placed and enabled), variable services on the flash, timer interrupt (8254 or HPET via the 8259/I/O APIC), MP services (INIT/SIPI to the APs) | the variable store's flash protocol (CFI IDs, SPI cycles, WPD); a timer interrupt that never arrives (exit 125/128); AP startup (`-o cpus=N`). **Early check, before chasing variable or boot-option bugs:** a write to 0xB2 enters SMM (with SMM variables, `SetVariable` otherwise returns `EFI_UNSUPPORTED` and nothing else says why) | `pch_spi.c` or CFI, interrupt routing, local APICs |
| SMM | SMM IPL, SMBASE relocation on every processor, SMRAM open/close/lock, SMM drivers (variables in SMM), SMIs from 0xB2 | SMI gating, SMRAM decode by SMIACT# (the SMM view and SMRAM windows), SMRR ranges (read through `UAM_X86_SMRR_SINK` outside SMM) | `ich9_lpc.c` APM/SMI, `q35_mch.c` SMRAM/TSEG |
| BDS -> shell / OS loader | console on COM1 (Super I/O decode), boot options from variables, a disk (`-o disk=PATH`) | COM1 not decoded until the Super I/O is programmed; no USB or GOP: the console must be serial; boot options that never stick (variables written through SMM: check software SMIs reach SMM, section 4) | `superio.c`, AHCI/NVMe/IDE |

Coreboot follows the same ladder under other names (bootblock = SEC, romstage = PEI and
raminit, postcar/ramstage = DXE, payload = BDS): its console is usually COM1 from the
bootblock, and `cbmem` timestamps tell where it got to. A legacy BIOS (POST from
F000:FFF0) shadows itself through PAM, sizes memory, runs option ROMs (`uam/dev/vga.h`
and PCI expansion ROMs) and ends in INT 19h; ember's `x86_64-ref` board boots one with
`--image bios=` and is the reference for its generic devices.

Once the firmware reaches its shell or OS loader, the last two milestones are the same as
for any board: no guest exceptions over a long run with interpreter and `--jit` agreeing,
then snapshot-exact (`references/snapshots.md`; save a snapshot at the shell prompt and
iterate from it).