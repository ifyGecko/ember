# MMIO discovery

MMIO regions are the device registers the firmware touches that are **not** backed by the
image or by RAM. Every MMIO page is a future `unimp` / `unknown-mmio` log line in ember and a device the build
phase may have to model, so recon should hand over not just *which pages* but *which
registers, read or written, by which functions*.

## Why "outside mapped memory" is the signal

At the correct base, with RAM accounted for, there are three kinds of address:
1. **In-image** addresses (code/data you loaded): not MMIO.
2. **RAM** (BSS/heap/stack, including uncached aliases of RAM): not MMIO.
3. **Everything else the code dereferences**: peripheral registers.

A wrong base turns real pointers into garbage and the MMIO table into noise, so confirm the
base first.

## Tools

- `scripts/mmio_scan.py BLOB --base <base> [--endian be]`: raw MIPS32 linear sweep.
  Reconstructs `lui`+load/store and `lui`+`addiu|ori` addresses, filters to kseg1 minus the RAM
  alias (`--ram-size`), and groups by 64 KiB page (`--page`). Per page it prints total, R, W,
  "address formed" counts, offsets touched (`+off:count`) and the first code sites. Seconds,
  no Ghidra.
  It also prints `max_off` per page (the minimum window size) and a **polling-loop** table:
  `POLL` = a single status bit the code spins on; `LOOPCMP` = a multi-bit compare inside a
  loop (often a chip-ID check in a retry loop: read the code). `--limit N` scans only the first
  N bytes, e.g. the XIP bootstrap at the start of a flash dump.
- `flashmap_scan.py BLOB --base <base> [--flash-size N]`: partition-table candidates (arrays
  of increasing, block-aligned plain offsets) and every reference into the flash window,
  folded by part size, grouped by 64 KiB block.
- `FindMmio.java` (headless, run by `recon.sh`): the same table from Ghidra's analysis.
  Resolved references (any arch) plus MIPS lui reconstruction reset at function boundaries.
  Each page also lists the **functions** that access it. Args: `<out> [page] [ram_size]`
  (page default 64 KiB, 256 bytes on 8/16-bit processors). References into a second address
  space are reported per space (`mem:0x0000` on AVR, `DATA:`, `SFR:`, `EXTMEM:`, `io:`):
  there RAM, the core's own registers and the peripherals share the space, so label each
  offset against the part's register map.

The two complement each other. The raw scan catches code Ghidra didn't disassemble; Ghidra
names the functions and handles non-MIPS architectures (and the MIPS16e/microMIPS code of a
PIC32 image, which the raw scan does not decode).

## MIPS specifics

- Segments: `kuseg 0x00000000–0x7FFFFFFF`; `kseg0 0x80000000–0x9FFFFFFF` (cached, physical
  `0x0+`); `kseg1 0xA0000000–0xBFFFFFFF` (**uncached: device I/O lives here**); `kseg2
  0xC0000000+` (mapped).
- Physical = virtual & `0x1FFFFFFF`. Record **physical** bases; ember's bus is physical (the MIPS ISA module translates kseg0/kseg1 before the bus sees an access).
- kuseg values built with `lui` in a kernel image are constants or struct offsets, not
  registers. Both tools drop them.
- Idiom: `lui $t0, 0xB110` then `lw/sw $t1, 0x14($t0)` gives register `0x11100014`.
- **"Formed but never loaded/stored" pages** are usually constants (KSEG conversion masks,
  division magic numbers, hash constants) or device bases stored into driver structs and used
  through a pointer later. Check those with `get_xrefs_to`: a device descriptor table
  `{base, irq, id}` is the usual way a driver for a big block (WLAN, USB, PCIe) is
  parameterized.
- Flash windows: an uncached flash window (`0xBFxxxxxx`) shows up as reads at large offsets.
  A small flash mirrored across a larger window shows the same offsets at several bases (e.g.
  `0xBF0F2000` and `0xBF8F2000` for an 8 MiB part in a 16 MiB window). Note it for the flash model.

## Notes per architecture

- **RISC-V (RV32, M-mode)**: no address translation, so virtual = physical. Registers are
  built with `lui rX, hi20` plus a 12-bit *signed* offset on the load/store (`lw a0, -0x7fc(a5)`
  means `hi20<<12 - 0x7fc`: watch the borrow). `FindMmio.java` resolves these through Ghidra's
  references; `mmio_scan.py` does not decode RISC-V. The CLINT (`mtime`/`mtimecmp`/`msip`) and
  PLIC are often standard SiFive layouts: ember ships both (`uam/dev/clint.h`, `uam/dev/plic.h`).
- **ARM** (ARMv7-A: `uam_isa_arm` / `uam_isa_armeb`): peripherals sit at fixed physical
  addresses (virtual = physical until the MMU is on; then read the translation tables the code
  builds); look for `MOVW/MOVT` pairs or literal-pool `LDR rX, =0x4xxxxxxx`. `mmio_scan.py`
  does not decode ARM: `FindMmio.java` and the Ghidra MCP tools. Ember ships a GICv2
  (`uam/dev/gic.h`); the generic timer is inside the CPU (`uam_arm_set_timer_sink`).
- **x86** (PC): most devices are I/O ports, PCI functions and MSRs, not MMIO pages:
  `FindX86Io.java` and `uefi-recon.md`. Fixed MMIO sits mostly in 0xFEC00000-0xFEDFFFFF
  (plus the local APIC at 0xFEE00000) and in the windows the firmware programs into BARs.
- **PowerPC** (`uam_isa_ppc32`): registers often hang off a CCSRBAR/IMMR base. Find where
  that base is programmed, then treat accesses as offsets from it. Registers can also be SPRs
  (`mfspr`/`mtspr` of implementation SPRs such as HID0) and, on 4xx/Book E parts, DCRs
  (`mfdcr`/`mtdcr`): list those numbers too, they become the board's SPR and DCR handlers, not
  bus devices.
- **PIC32** (MIPS32 LE): SFRs at physical 0x1F800000-0x1F8FFFFF (kseg1 `0xBF8xxxxx`); most
  have SET/CLR/INV aliases at +4/+8/+0xC (writes to those modify bits of the register at
  +0), so offsets come in groups of four. The interrupt controller layout differs by class
  (`pic32-ref`: MX computed-offset at 0x1F881000, MM/MZ variable-offset at 0x1F810000).
- **AVR**: I/O registers are data addresses 0x20-0x5F (`IN`/`OUT`/`SBI`/`CBI` take I/O
  address A = data 0x20 + A) and extended I/O 0x60-0xFF (`LDS`/`STS`) on AVRe/AVRe+; on
  AVRxm/AVRxt (and AVRrc) the I/O space starts at data 0 with peripheral blocks above it
  (XMEGA and AVRxt: 0x0000-0x0FFF). SREG, SPL/SPH, RAMPx, EIND and CCP are the hart's.
  Ghidra shows them in the `mem` space; ember's bus has them at 0x800000 + data address.
- **PIC**: SFRs are the data addresses of the part's data sheet in each bank (mid-range:
  bank * 128 + f; PIC18: the access bank's upper half 0xF60-0xFFF, the rest by BSR). The
  core's registers (INDF, PCL, STATUS, FSR, PCLATH, INTCON, BSR, WREG, TBLPTR, ...) dominate
  the counts and are the hart's; peripherals are the rest. A banked address needs the bank the
  code selected (STATUS RP bits, BSR): Ghidra resolves it only where the bank is known.
- **8051**: direct addresses 0x80-0xFF are SFRs (`SFR:` in Ghidra; ember bus 0x2000000 +
  address, + page * 0x100 with SFR paging), bit addresses 0x80+ are SFR bits; XDATA devices
  are `MOVX` targets (`EXTMEM:`, bus 0x1000000 + address). Internal RAM (direct 0x00-0x7F,
  indirect up to 0xFF) is the hart's. ACC, B, PSW, SP, DPL/DPH, IE, IP, PCON and the model's
  extended interrupt, DPS and SFRPAGE registers are the core's; P0-P3, TCON/TMOD/TLx/THx and
  SCON/SBUF are the generic 8051 devices (`uam/dev/mcs51_*.h`); everything else is the part's.
- **6502**: one space: I/O is memory-mapped pages (VIA/ACIA/PIA chips at fixed page
  addresses). RAM below the I/O and ROM above show up as "pages" too: zero page and page 1 are
  RAM by definition, and a page only stored to and loaded from is RAM.
- **Z80**: devices are I/O ports, reached by `IN A,(n)`/`OUT (n),A` (`DB nn`/`D3 nn`, A on the
  high address byte) and `IN r,(C)`/`OUT (C),r` and the block I/O instructions (B on the high
  byte): Ghidra shows them in the `io` space. Record whether the code relies on 16-bit port
  addresses (`ld bc,nnnn` then `in a,(c)`) or the hardware decodes the low byte only
  (`uam_z80_set_io_decode`). Memory-mapped devices and bank-switch registers exist too.
- **Xtensa / Espressif**: addresses come from `l32r` literals (32-bit words in a literal pool
  below the function) and `movi`; `memw` before a load/store is the compiler's barrier for
  `volatile`, so it marks device accesses. Peripheral windows: ESP8266 0x60000000-0x60001FFF
  (UART0 at 0x60000000, RTC memory from 0x60001000) and DPORT at 0x3FF00000; ESP32
  0x3FF00000-0x3FF7FFFF (DPORT at 0x3FF00000, UART0 at 0x3FF40000, RTC at 0x3FF48000);
  ESP32-S3 from 0x60000000 (UART0 at 0x60000000). Many driver accesses go
  through ROM functions (addresses 0x4000xxxx called from the app: the board must provide
  that ROM or hook the calls).
- **AVR32**: UC3 (AVR32A) peripherals sit at the top of the map (0xFFFE0000 and above, on the
  peripheral bus bridges), AP7
  (AVR32B) peripherals through P4 (0xE0000000-0xFFFFFFFF, untranslated) and the P2 uncached
  segment; the interrupt controller supplies the autovectors (`uam_avr32_set_autovector`).

## Polls and wait helpers

A polling loop is the strongest statement the code makes about a register: it will not
proceed until the bit changes. Linear-scan polls are inline `lw; [andi|and]; beq/bne back`.
Larger drivers (WLAN HALs, PHY libraries) route every wait through one helper such as
`wait(dev, reg, mask, value, timeout_us)`. Find it once (a short loop around a register read
with a delay call and a timeout message), then list the constant `(reg, mask, value)` of its
callers. Those are the bits the device model must eventually set or clear.

## Pages that are not peripherals

- **On-chip SRAM / scratch**: plain store-then-load of magics or counters, no polls, often
  touched only by the bootstrap. Model as RAM that survives a reset.
- **Flash windows**: large offsets, mirrored; see above.
- **Foreign registers inside a block**: SoCs park unrelated controls in a big block's window
  (for example a CPU clock DPLL inside the radio's analog section). Keep the page in that
  block's window, and size the window to cover it.

## Labeling a page

Rank by load/store count. Hot pages are usually the reset/clock/timer block, GPIO (LEDs,
buttons), the console UART, and the flash controller. A high count is a reason to read the
code, not a requirement: confirm "read in delay loops" from an actual loop before claiming
the register must count. Many hot registers are written at init and never read again. Then for each page:
1. Read one or two of the listed functions (`decompile_function_by_address`). For bit-bang or
   delay loops, read the **disassembly**; the decompiler drops repeated MMIO stores.
2. Note the semantics the code implies: bits polled (busy/ready), values compared (chip ID,
   revision), write-1-to-clear patterns in ISRs, fields used as divisors.
3. Compare with the public register map of the SoC family named by the BSP strings, but keep
   the firmware's behavior as the authority.
4. Once any ember board runs the code (even a bare one with catch-all windows), confirm
   statically found registers at run time: the catch-all first-access lines name the
   instruction (`pc=... icount=N`), `uam-run --trace mmio --trace-mmio-range LO:HI` gives the
   real access order and values, including the reads a POLL row's loop repeats. Static
   polls this table misses (computed addresses) show up there.

The **interrupt routing** is part of the MMIO picture. Find the RTOS's interrupt table (VxWorks
`intPrioTable`-style `{cause bit, vector or demux, mask}` entries; Linux irq-domain setup) and
the demux function that reads an interrupt status register. Record which CPU line and which
status bit each device uses.
