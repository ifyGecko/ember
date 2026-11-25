# Board skeleton

The board file is the only place where an ISA, memory and devices meet. It creates the hart,
maps RAM, places the firmware, makes the hart start at the entry point, instantiates each
peripheral, maps it at its MMIO address and connects its lines. **It wires things; it does
not implement devices.** One board file per target: `emulation/<board>_board.c`.

## Where the board file sits: the project and its meson files

The board is one file of a project whose layout is fixed (`organization.md` has all of it):

```
<project>/
  meson.build                 the runner build/<board>-run; ember through dependency('uam')
  subprojects/ember.wrap      ember's url and the pinned revision
  emulation/meson.build       board_sources = files('main.c', '<board>_board.c', ...)
  emulation/main.c            uam_board_register(&<board>_board); return uam_run_main(argc, argv);
  emulation/<board>_board.c   this page
  emulation/<soc>_<block>.c   one device each (device-anatomy.md)
  firmware/                   the images the board runs
  tests/                      meson test: registered, boot
```

`new-emulation.sh` writes the three meson files and they stay as written, except that
every source added to `emulation/` gets a line in `emulation/meson.build`. The project's
`meson.build` in full (generated from `templates/meson.build.in`):

```meson
project('<board>', 'c', version: '0.1', meson_version: '>= 1.1',
  default_options: ['c_std=c11', 'warning_level=2', 'werror=true', 'buildtype=debugoptimized',
                    'wrap_mode=forcefallback'])    # always the pinned ember of subprojects/
board = '<board>'
add_project_arguments('-D_DEFAULT_SOURCE',
  meson.get_compiler('c').get_supported_arguments('-Wshadow', '-Wstrict-prototypes', '-Wmissing-prototypes'),
  language: 'c')
uam_dep = dependency('uam')                        # ember's headers, libuam.a, libm
subdir('emulation')                                # sets board_sources
runner = executable(board + '-run', board_sources,
  include_directories: include_directories('emulation'), dependencies: uam_dep)
subdir('tests')
```

and `subprojects/ember.wrap`:

```ini
[wrap-git]
url = <ember's repository>
revision = <the pinned commit, a full hash>

[provide]
uam = uam_dep
```

The board is compiled with warnings as errors, like ember. Build with
`scripts/build-ember.sh` the first time and after the pin or `patches/` change,
`scripts/rebuild.sh` after edits; both end in `meson compile -C build`.

## The generated board

`new-emulation.sh` generates a board that already builds and runs
(`templates/board.c.in`). This page explains each part and what to change. The tested
`skills/tests/fixture/testbrd_board.c` (in the ember repository, next to this skill) shows
the same board with a UART, a word-spaced 16550, a timer, an interrupt controller, a reset
block and a finisher wired in.

## Per ISA: what the generated board starts from

The defaults come from ember's reference board of each ISA (its file is in the second table
below), which has only what every board of that architecture has; `--model` picks its CPU model. Addresses are
bus (physical) addresses; RECON's numbers replace them.

| `--isa` (`--model`) | descriptor, CPU model call | memory the scaffold maps | start | interrupt inputs (`uam_cpu_irq_sink`) |
|---|---|---|---|---|
| `mips32be`, `mips32le` | `uam_isa_mips32be`/`le`; `uam_mips_configure` (`uam_mips_ref`) | RAM at 0 (kseg0/kseg1 reach it) | 0xBFC00000 | `UAM_MIPS_IRQ_HW0`..`HW5` = Cause.IP2..IP7; with Config3.VEIC an interrupt controller calls `uam_mips_eic_input` |
| `rv32` | `uam_isa_rv32` | RAM at 0x80000000 | 0x80000000 | `UAM_RV_IRQ_MSI` 3, `MTI` 7, `MEI` 11 (mip bits) |
| `arm`, `armeb` | `uam_isa_arm`/`armeb`; `uam_arm_configure` (`uam_arm_ref`) | RAM at 0 | 0 | `UAM_ARM_IRQ`, `UAM_ARM_FIQ`; the generic timer: `uam_arm_set_timer_sink` |
| `x86_64` | the PC template (`pc-platform.md`) | RAM below the hole and above 4 GiB, flash ending at 4 GiB | 0xFFFFFFF0 | `UAM_X86_IRQ_INTR` (from the local APIC), `NMI`, `SMI`, `INIT`, `A20M` |
| `ppc` (classic, booke) | `uam_isa_ppc32`; `uam_ppc_configure` (`uam_ppc_ref`, `uam_ppc_booke_ref` + `uam_ppc_set_reset_sink` for the watchdog) | RAM at 0 (64 MiB), boot ROM 0xFFF00000 (1 MiB) | 0xFFF00100; Book E 0xFFFFFFFC | `UAM_PPC_IRQ_INT(0..7)` (wired-OR external pin), `UAM_PPC_IRQ_MCP`, `SRESET`, `CINT` (Book E critical) |
| `avr` (eplus, e, xm, xt, rc) | `uam_isa_avr`; `uam_avr_configure` (`uam_avr_<m>_ref`) | flash (ROM) at 0; SRAM at `UAM_AVR_DATA_BASE` (0x800000) + its data address; an `io` catch-all over the I/O space; xt/rc: flash also at data 0x4000 | 0 | line n = interrupt vector n; AVRxm/xt levels: `uam_avr_set_irq_level`; flag-clearing acknowledge: `uam_avr_set_irq_ack` |
| `avr32` (a, b) | `uam_isa_avr32`; `uam_avr32_configure` (`uam_avr32a_ref`, `uam_avr32b_ref`) | A: RAM at 0, code memory 0x80000000; B: code memory at 0, RAM 0x10000000; the stock `uam_avr32_intc` at 0xFFFF0800 | A 0x80000000, B 0xA0000000 | `UAM_AVR32_IRQ_INT0`..`INT3` (from the interrupt controller), `NMI`; COUNT/COMPARE is an output, `uam_avr32_compare_line` |
| `pic` (pic18, baseline, mid, enhanced) | `uam_isa_pic`; `uam_pic_configure` (`uam_pic18_ref`, `uam_pic_<m>_ref`) + `uam_pic_set_reset_hook` | flash at 0; data RAM at `UAM_PIC_DATA_BASE` (0x800000) mapped under everything (SFRs sit among the general-purpose bytes); PIC18: an `sfr` catch-all over 0xF60-0xFFF | 0; baseline the last word | `UAM_PIC_IRQ_PERIPH`, `UAM_PIC_IRQ_LOW` (PIC18 IPEN); INTCON's own flags: `uam_pic_intcon_set` |
| `pic32` (mx, mm, mz) | `uam_isa_mips32le`; `uam_mips_configure` (`uam_mips_m16_ref`, `uam_mips_umips_ref`, `uam_mips_mz_ref`) | RAM at 0, program flash 0x1D000000, boot flash 0x1FC00000, an `sfr` catch-all over 0x1F800000-0x1F8FFFFF, the stock `uam_pic32_intc` | 0xBFC00000 | sources of the interrupt controller (`uam_pic32_intc_input`), which drives `uam_mips_eic_input`; the core timer and software interrupts are its sources 0-2 |
| `mcs51` (8052, 8051, cip51) | `uam_isa_mcs51`; `uam_mcs51_configure` (`uam_mcs51_<m>_ref`) | code (ROM) at 0; XDATA RAM at `UAM_MCS51_XDATA_BASE`; an `sfr` catch-all at `UAM_MCS51_SFR_BASE` (+ page * 0x100) | 0 | line n = source n (vector 0x03 + 8n); `uam_mcs51_set_irq_ack` |
| `m6502` (nmos, 2a03, 65c02, r65c02) | `uam_isa_m6502`; `uam_m6502_configure` | RAM 0-0x7FFF, ROM 0x8000-0xFFFF | the reset vector at 0xFFFC | lines 0-29 wired-OR into IRQ, `UAM_M6502_LINE_NMI`, `UAM_M6502_LINE_SO`; devices clear their own lines |
| `z80` (nmos, cmos) | `uam_isa_z80`; `uam_z80_configure` + `uam_z80_set_io_decode` | RAM 0-0xFFFF; an `io` catch-all over the ports at `UAM_Z80_IO_BASE` | 0 | `UAM_Z80_LINE_INT`, `UAM_Z80_LINE_NMI`; IM 0/2 data-bus bytes: `uam_z80_set_inta`; RETI: `uam_z80_set_reti` |
| `xtensa`, `xtensaeb` (lx6, lx106, lx7) | `uam_isa_xtensa`/`xtensaeb`; `uam_xtensa_configure` (`uam_xtensa_<m>`) + `uam_xtensa_set_prid` | RAM at 0x40000000 (4 MiB; VECBASE resets there) | the model's reset vector (0x40000400; LX106 0x40000080) | line n = interrupt n as the model's table types it (level, edge, NMI); CCOMPARE timers are the core's |

Where each `--isa` lives in ember, and the name RECON.md gives the architecture
(ember-dev-recon's `--arch` and its "Architecture" line), which is not always the `--isa`:

| `--isa` (`--model`) | RECON architecture | reference board | ISA header (`include/uam/isa/`) |
|---|---|---|---|
| `mips32be`, `mips32le` | `mips32` + endianness be / le | `boards/mips_ref.c` | `mips32.h` |
| `pic32` (`mx`, `mm`, `mz`) | `pic32` | `boards/pic32_ref.c` | `mips32.h` |
| `rv32` | `rv32` | `boards/rv32_ref.c` | `riscv.h` |
| `arm`, `armeb` | `arm`, `armeb` | `boards/arm_ref.c` | `arm.h` |
| `x86_64` | `x86_64` (also `x86_16`, `x86_32` stages of the same PC) | `boards/x86_64_ref.c` | `x86.h` |
| `ppc` | `ppc32` | `boards/ppc_ref.c` | `ppc.h` |
| `avr`, `avr32` | `avr`, `avr32` | `boards/avr_ref.c`, `avr32_ref.c` | `avr.h`, `avr32.h` |
| `pic` (`baseline`, `mid`, `enhanced`, `pic18`) | `pic-baseline`, `pic-mid`, `pic-enhanced`, `pic18` | `boards/pic_ref.c` | `pic.h` |
| `mcs51` | `mcs51` | `boards/mcs51_ref.c` | `mcs51.h` |
| `m6502` (`nmos`, `2a03`, `65c02`, `r65c02`) | `6502`, `65c02` | `boards/m6502_ref.c` | `m6502.h` |
| `z80` | `z80` | `boards/z80_ref.c` | `z80.h` |
| `xtensa`, `xtensaeb` | `xtensa`, `xtensaeb` | `boards/xtensa_ref.c` | `xtensa.h` |

The microcontroller ISAs (AVR, AVR32, PIC, PIC32, 8051, 6502, Z80) have one processor per
board (`BOARD_MAX_CPUS` 1); the image is usually the whole flash, started at the reset vector.

## The parts of the generated board

```c
#define BOARD_RAM_BASE   UINT64_C(0x0)          /* PHYSICAL */
#define BOARD_RAM_SIZE   UINT64_C(0x2000000)
#define BOARD_IMAGE_BASE UINT64_C(0x80010000)   /* guest-virtual: RECON confirmed base */
#define BOARD_ENTRY      UINT64_C(0x80010000)   /* RECON entry */
#define BOARD_RESET_VEC  UINT64_C(0xbfc00000)   /* start pc when no --image os= is given */

static const struct board_window { ... } board_windows[] = {     /* every RECON MMIO window */
    { "sysctl", UINT64_C(0x18000000), 0x10000 },
    { NULL, 0, 0 },
};
static const struct board_reg { ... } board_entry_regs[] = {      /* RECON entry state */
    { "sp", 0x80400000 },
    { NULL, 0 },
};
```

`build()` then does, in order:

1. `uam_machine_init(m, &uam_isa_mips32be)` (the ISA's descriptor), `m->board_name`,
   `m->reset_pc = BOARD_RESET_VEC`. Then, where the ISA can have several processors
   (`BOARD_MAX_CPUS` over 1), `-o cpus=N`: `uam_machine_add_cpus(m, N - 1)`, before the
   first reset. Processor 0 is `m->cpu`, processor i `uam_machine_cpu(m, i)`; everything
   per processor (the CPU model of step 2, a timer or interrupt line wired to one, a
   per-processor device: `uam_machine_current_cpu` tells a device who is asking) is done
   for each. The processors share the bus and the clock and take turns, `--quantum N`
   instructions each (default 4096), deterministically. How the others start is the
   board's to model, as the hardware does it: held in reset or parked in a loop until the
   boot processor releases them (a register of your reset block, an IPI device; a PC's
   wait for SIPI is in `pc-platform.md`). Hooks for all processors are added after
   `uam_machine_add_cpus`, with `uam_machine_add_pc_hook`. `uam_machine_init`, `uam_machine_add_device` and
   `uam_board_register` are macros that pass the struct layout the board was compiled
   with; if libuam was built from other headers, the run stops at once with `abi:` errors
   and exit 2. Rebuild the library and the board together (`scripts/build-ember.sh`).
2. **CPU model**: `uam_<isa>_configure(cpu, &cfg)` on every processor, before the first
   reset (it takes effect at a reset). The config is copied, so a board-local variation of
   the reference model is fine. MIPS:
   ```c
   uam_mips_config cpu = uam_mips_ref;        /* then change what the firmware checks */
   cpu.name = "74Kc"; cpu.prid = 0x00019750; cpu.tlb_entries = 32;
   if ((st = uam_mips_configure(&m->cpu, &cpu)) != UAM_OK) return st;
   ```
   Fields: `prid`, `config0`..`config3` (Config.BE is forced to the descriptor's byte order,
   Config1.MMUSize follows `tlb_entries`; Config.MT, Config1.CA/FP and Config3.ISA/VInt/VEIC/
   DSPP/DSP2P/MCU switch the TLB or fixed mapping, MIPS16e, the FPU, microMIPS, vectored and
   external-controller interrupts and the ASEs), `status_rw` (writable Status bits),
   `tlb_entries` (1..64; 0 with fixed mapping), `shadow_sets`, `fir` (with Config1.FP). Take the values from the firmware's
   PRId/Config compares in RECON. The other ISAs' fields are in their `uam/isa/*.h` (PowerPC
   PVR, BAT pairs, Book E TLB geometry; AVR flash size and features; Xtensa options and
   interrupt table; ...).
3. Board-private state (`m->board_data`, freed by `m->board_fini`) and the **reset hook**
   (`m->on_reset`, see below). `board_data` holds configuration; anything in it the guest
   can observe or change is registered with `uam_machine_add_state` (see "Snapshots").
4. RAM: `uam_bus_map_ram(&m->bus, "ram", BOARD_RAM_BASE, cfg->ram_size ?: BOARD_RAM_SIZE)`.
   `--ram SIZE` overrides the size. Then the ISA's other memory (program flash, a boot ROM,
   code memory: `map_flash()` maps a ROM erased to 0xFF that the loader fills).
5. Catch-all windows: one `uam_unimp` per `board_windows[]` row, mapped with
   `&(uam_map_attrs){ .prio = UAM_PRIO_BACKGROUND }`. Real devices mapped later (priority 0)
   win where they overlap; the window keeps logging the gaps.
6. Devices, one `attach(m, dev, base, size)` each (adds to the machine so it is reset and
   destroyed with it, then maps it), and their lines. AVR32 and PIC32 boards start with the
   architecture's stock interrupt controller already wired.
7. The unknown-MMIO fallback for addresses no region covers (`--unknown-mmio zero|echo`;
   `fault`, the runner's default, makes them bus errors). `run.sh` passes `zero`.
   `--unknown-mmio stop` needs nothing from the board: the board sees `fault`, and the
   runner puts its own fallback on the bus that stops the run at the first such access.
8. A first `uam_machine_reset`, so the processor is in its reset state, then the OS:
   `--image os=PATH` is placed with `uam_load_image()` (ELF segments at their load addresses
   `p_paddr`, anything else raw at `BOARD_IMAGE_BASE`), passing `&m->cpu` so addresses are
   translated as the reset processor does (MIPS kseg0/kseg1, AVR32B's P1/P2, the Book E boot
   page) before the bytes reach the bus. The loader writes ROM too.
9. The entry-register hook, then `uam_machine_reset(m, entry)`.

After `build()` returns, the runner (not the board) maps every `--mmio-rule` and, with
`--load-snapshot`, restores the snapshot over the freshly built machine. Who answers an
address, highest priority first:

| layer | priority | from |
|---|---|---|
| real devices, RAM, ROM, ROMD, aliases | 0 (default) | the board |
| per-register rules | `UAM_PRIO_STUB` (-900) + rule index: later rules win | `--mmio-rule`, or `uam_board_add_mmio_rules` in a board |
| catch-all windows | `UAM_PRIO_BACKGROUND` (-1000) | the board's `board_windows[]` (`uam_unimp`) |
| the unmapped fallback | - | `--unknown-mmio zero` or `echo` (`uam_unknown_mmio`), else a bus fault; `stop`: the runner's, which stops the run (exit 131) |

**Do not also pass the image as the runner's positional argument.** The runner would load it
a second time at `--raw-base` (default: the board's reset pc) and restart the hart at that
image's entry (`--entry` overrides it), bypassing the board's placement. Named images
(`--image NAME=PATH`) are the board's business; `run.sh` uses only those. (`--raw-base` /
`--entry` with a positional image are for quick looks on a stock board: `uam-run -b mips-ref
--raw-base 0x80100000 --entry 0x801002bc image.bin`.)

## Address spaces
- **Ember's bus is physical.** RAM, ROM and device bases are physical addresses. The MIPS ISA
  module translates kseg0 (`0x8...`) and kseg1 (`0xA...`) to `virtual & 0x1FFFFFFF` (and the
  TLB for kuseg/kseg2/3); RISC-V M-mode uses physical addresses directly.
- **Never map RAM at `0x80000000` on MIPS**: that is a virtual address. RAM goes at physical
  0 and both kseg0 and kseg1 reach it. The same holds for AVR32B's P1/P2 segments.
- **Separate address spaces share the one bus** at fixed offsets (the ISA header defines
  them): AVR and PIC data space at `UAM_AVR_DATA_BASE` / `UAM_PIC_DATA_BASE` (0x800000; program
  memory at 0, the pc a byte address), 8051 XDATA at `UAM_MCS51_XDATA_BASE` and SFRs at
  `UAM_MCS51_SFR_BASE`, Z80 I/O ports at `UAM_Z80_IO_BASE`, x86 I/O ports at
  `UAM_X86_PIO_BASE`. Devices, windows, `--mmio-rule`s, `--guard p:` and `--map` all use these
  bus addresses (an AVR register at data 0xC6 is 0x8000C6). Registers the core owns (AVR SREG
  and SP, PIC INDF/STATUS/FSR..., 8051 ACC/PSW/SP/DPTR/IE/IP...) never reach the bus.
- Image placement and the entry point are in the guest's *own* (virtual) addresses: the
  loader translates them. An avr-gcc ELF already uses the bus layout (`.data` at 0x800000+).

## Entry state
Read the first instructions at the entry point (RECON records them). A self-contained entry
(sets Status/sp/gp itself) needs nothing. A stage that expects registers from its loader
(`sp`, `a0` = boot type, `a1` = argv, a device-tree pointer) gets them from
`board_entry_regs[]`: a PC hook at the entry writes them on every arrival, so they also hold
after a guest-triggered reset. The names are the ISA's register names (`isa->regs`: MIPS
`zero at v0 v1 a0..a3 t0..t9 s0..s7 k0 k1 gp sp s8/fp ra`, RISC-V ABI names; the others as
`-d` prints them). On the 6502 the hart starts at the address in the reset vector, so the
hook's address (`BOARD_ENTRY`) must be that one.

MIPS reset state is a cold reset: Status = BEV | ERL, interrupts off, EBase 0x80000000. An OS
image started directly (without its boot loader) normally sets Status itself; check RECON.

## Multi-stage images and flash
Start with the fastest path: place the main OS in RAM and start at its entry, skipping the
boot loader. Model flash and boot through the real loader later, when the OS needs flash
contents or you need the loader's hardware setup.

| need | ember mechanism |
|---|---|
| a boot ROM / flash window the CPU only reads and executes (XIP) | `uam_bus_map_rom(bus, "flash", base, size, data, len)`; fill it from a named image (`--image flash=`) with `uam_load_raw(&m->bus, NULL, path, phys, &info)` (NULL cpu: physical address) |
| flash that also takes commands (CFI/JEDEC writes, erase) | ROMD: `uam_bus_map_romd(...)` with a flash-controller device; see `device-anatomy.md` |
| the same chip seen at several addresses (kseg0/kseg1 are free; a 8 MiB part mirrored in a 16 MiB window is not) | `uam_bus_map_alias(bus, "flash.mirror", base2, size, uam_bus_region(bus, "flash"), 0, NULL)` |
| on-chip SRAM that must survive a reset (self-reset tricks) | `uam_bus_map_ram`: RAM is never cleared by a reset |
| start at the reset vector with the real loader | set `BOARD_RESET_VEC` / `m->reset_pc` to the reset address (MIPS `0xbfc00000`) and give no `os` image; or place the whole flash image as the `os` image at the flash's address, as the microcontroller scaffolds do |

A guest **write** to a plain ROM region is a bus fault (MIPS DBE). If the firmware sends flash
commands, the region must be ROMD.

## Interrupt lines
The board connects every device output with `uam_signal_connect(line, sink, ctx, input)`:

| target | sink | input numbers |
|---|---|---|
| a CPU | `uam_cpu_irq_sink`, ctx `&m->cpu` (or `uam_machine_cpu(m, i)`) | the ISA's inputs (table at the top; MIPS IP7 is shared with the CP0 timer, which needs no wiring without an external controller) |
| an interrupt controller | its input function (`soc_intc_input`, `uam_plic_input`, `uam_avr32_intc_input`, `uam_pic32_intc_input`) | its source numbers |
| a chip reset | `uam_machine_reset_sink`, ctx `m` | `UAM_RESET_SYSTEM` or `UAM_RESET_CPU` |

Take the routing from RECON (the RTOS interrupt table: which Cause.IP / mip bit, which
controller bit). A line can have only one sink; an output that fans out needs a tiny splitter
device.

## The reset hook
A device that resets the chip raises a line wired to `uam_machine_reset_sink`. Between two
instructions the machine then calls the board's `on_reset` hook, resets every device (their
`reset` op) and restarts the hart at `m->reset_pc`. **RAM is preserved**, as on hardware.

Because the board skips the real boot loader, the generated hook does the loader's job:
it re-places the `os` image (fresh `.data`) and keeps `reset_pc` at its entry. A board that
boots through the real loader instead sets `m->reset_pc` to the reset vector and re-places
nothing.

## Replacing a firmware function (HLE), in `<board>_hle.c`
A last resort (see `iteration-loop.md`, step 4), kept out of the board and the devices. A PC
hook runs when the hart arrives at an address, with precise state; it may change registers,
memory and `cpu->pc`. Tested version: `skills/tests/fixture/testbrd_hle.c`.

```c
static void hle_return(uam_cpu *cpu, uint64_t v)       /* "return v" from the hooked function */
{
    const uam_regdesc *ret = reg(cpu, "v0"), *ra = reg(cpu, "ra");   /* RISC-V: "a0", "ra" */
    uam_cpu_reg_write(cpu, ret->offset, ret->width, v);
    cpu->pc = uam_cpu_reg_read(cpu, ra->offset, ra->width);
}

static uam_hook_action skip_rf_cal(void *ctx, uam_cpu *cpu, uint64_t pc)
{
    (void)ctx; (void)pc;
    hle_return(cpu, 0);                                   /* "calibration ok" */
    return UAM_HOOK_CONTINUE;
}

uam_status board_hle_install(uam_machine *m, const uam_board_config *cfg)
{
    if (!uam_board_config_option(cfg, "hle"))             /* opt-in while it is evaluated */
        return UAM_OK;
    return uam_machine_add_pc_hook(m, 0x80123450 /* rfCalibrate */, skip_rf_cal, NULL);   /* every processor */
}
```

(`reg()` looks a name up in `cpu->isa->regs`, the ISA module's own register table: MIPS has
`v0`, `a0`, `ra` there. `--log-pc` and gdb use other names, `r2`, `r4`, `r31`.) Hook a function's first instruction, never a
branch delay slot (that is an error: hook the branch). Adding a hook discards translated code once; it costs nothing afterwards.
The board calls the install function before `uam_machine_reset`.

## A guarded heap behind the firmware's allocator, in `<board>_heap.c`
For fuzzing (ember-fuzz, "Memory oracles"): on flat memory a heap overflow or a use after
free faults nowhere. `uam/heap.h` is an allocator ember runs **in place of** the firmware's
own, over the memory the firmware's heap used (the arena): redzones around every block, a
quarantine for freed ones, every access to the arena checked to the byte, double and bad
frees reported. Ember does not know which functions of your firmware allocate or how they
take arguments; the board does, and says so with hooks (the HLE mechanism above). Tested
version: `tests/boards/heap_hooks.c` in ember (RISC-V). `<board>_heap.c` is a board-level
file like `<board>_hle.c` (`check-layering.sh` lets both include any ember header): add it
to `emulation/meson.build`. The snippet uses `reg()` and `hle_return()` from the HLE section
above: put it in `<board>_hle.c` beside them, or declare the two in `<board>_hle.h`
(without `static`) and include that.

1. **Find the functions** in Ghidra: the allocate, release and (if any) resize, zeroing
   and aligned variants the firmware's code calls (`malloc`/`free`, `pvPortMalloc`,
   `memPartAlloc`, a pool's `get`/`put`), their arguments, and the arena: the memory range
   the firmware's allocator was given (a linker symbol, an array, the range its init is
   called with). Hook the innermost functions every caller goes through.
2. **Create the heap and hook them**, before `uam_machine_reset`:

```c
static uam_heap *g_heap;

static uam_hook_action on_malloc(void *ctx, uam_cpu *cpu, uint64_t pc)   /* void *malloc(size) */
{
    (void)ctx; (void)pc;
    const uam_regdesc *a0 = reg(cpu, "a0"), *ra = reg(cpu, "ra");   /* this ISA's convention */
    uint64_t caller = uam_cpu_reg_read(cpu, ra->offset, ra->width);
    hle_return(cpu, uam_heap_alloc(g_heap, uam_cpu_reg_read(cpu, a0->offset, a0->width), 0, caller));
    return UAM_HOOK_CONTINUE;
}

static uam_hook_action on_free(void *ctx, uam_cpu *cpu, uint64_t pc)     /* void free(ptr) */
{
    (void)ctx; (void)pc;
    const uam_regdesc *a0 = reg(cpu, "a0"), *ra = reg(cpu, "ra");
    uam_heap_free(g_heap, uam_cpu_reg_read(cpu, a0->offset, a0->width),
                  uam_cpu_reg_read(cpu, ra->offset, ra->width));    /* a bad free is reported */
    hle_return(cpu, 0);
    return UAM_HOOK_CONTINUE;
}

uam_status board_heap_install(uam_machine *m, const uam_board_config *cfg)
{
    if (!uam_board_config_option(cfg, "guard-heap"))      /* opt-in: -o guard-heap=1 */
        return UAM_OK;
    uam_heap_opts o = UAM_HEAP_OPTS_INIT;
    char err[256];
    o.base = 0x80200000;   /* the firmware's heap, guest-virtual (o.phys = true: physical) */
    o.size = 0x00100000;
    if (!(g_heap = uam_heap_create(m, &o, err, sizeof(err)))) {
        uam_log(UAM_LOG_ERROR, "board", "guarded heap: %s", err);
        return UAM_ERR_INVALID;
    }
    uam_status st = uam_machine_add_pc_hook(m, 0x80012340 /* malloc */, on_malloc, NULL);
    return st != UAM_OK ? st : uam_machine_add_pc_hook(m, 0x800123a0 /* free */, on_free, NULL);
}
```

   `uam_heap_realloc(h, ptr, size, site)` and `uam_heap_calloc(h, n, size, site)` are there
   for those variants, `uam_heap_alloc`'s third argument for an aligned one, and
   `uam_heap_block` for a "size of this block" function. The last argument of each is what
   reports call the allocation or free site; pass the caller's return address. Free the heap
   in the board's `board_fini` (`uam_heap_destroy`).
3. **Check the boot still passes** with the option on. Checking starts with the first
   allocation, so start-up code clearing the arena is fine. A hit during the boot after that
   is firmware code touching its old heap structures: the allocator's init, a heap walker,
   a statistics or integrity routine. Hook or stub those as well (they have nothing to walk
   now), or disarm the heap around them (`uam_heap_arm`). A function that returns the heap's
   free space gets a hook that answers from `uam_heap_get_stats`.
4. The heap is machine state: create it before any snapshot is taken or loaded, and a
   snapshot taken with it is loaded with it (and the same name and arena).

What this does not test: the firmware's own allocator (its bugs, its fragmentation, its
reuse order). Keep the option off for bring-up and on for fuzzing.

## Snapshots
Snapshots save memory, the hart, virtual time, every device's `save` stream and registered
extra state; `snapshots.md` has the whole picture. What the board itself must do:

- Every device it adds takes part (`save`/`restore` or `UAM_DEVICE_STATELESS`). The stock
  devices already do. A device the machine never sees (wrapped inside another) is saved by
  its owner.
- Guest-visible board state goes in with `uam_machine_add_state(m, "board", save, restore, b)`
  (same `uam_state_put_*` stream as a device). The generated board has none: `os_path` is
  configuration.
- Build deterministically: the same options must give the same devices, in the same order,
  with the same names. Restore matches devices by name and order and refuses a different
  board, memory map or device list. It does not notice a different image or an option that
  builds the same devices: such a restore succeeds and then runs inconsistently (a guest
  reset re-places the other image). Load a snapshot only with the board build, `-o`
  options, images, `--ram`, rules and ports it was taken with.
- The reset hook may re-place images: the loader writes through the bus, so the pages are
  tracked. A hook that writes a backing store directly calls `uam_bus_mark_dirty`.

## Board options, named images, network ports, link ports
- `-o key=value` → `uam_board_config_option(cfg, "key")` (NULL if absent). Use options for
  variants (console on which UART, straps, chip revision) instead of rebuilding.
- `--image name=path` → `uam_board_config_image(cfg, "name")`.
- `--nic port=SPEC` → `uam_board_config_netdev(cfg, "port")`; NULL means the cable is
  unplugged: create the MAC anyway, report link down.
- `--link port=HUB[,bus=BUS][,latency=TIME]` → `uam_board_config_link(cfg, "port")`, a
  `uam_link_port *` for a device that talks to another emulation (`links.md`); NULL means
  the option was not given: create the device anyway, with nothing on the wire. A port
  nobody takes is reported by the runner (`nothing uses link port ...`).
- `--console SPEC` → `cfg->console`, handed to exactly one UART model (a chardev has one
  frontend). The runner holds its input until the run starts, so fixed input
  (`string:`, `in=FILE`) survives the board's power-on reset and a `--load-snapshot`;
  `,after=PATTERN` and `script:FILE` release it only as the guest prints its prompts.
- `--mmio-rule` needs nothing from the board; a board that always wants some rules can map
  them itself with `uam_board_add_mmio_rules` (`uam/boards.h`), but prefer a device.
- **Monitor commands of the board's own** (a button pressed, a strap changed, a device's
  hidden state shown): offer them from `build()` with `uam_monitor_offer_command(m, NAME,
  USAGE, HELP, FLAGS, fn, ctx)` (`uam/monitor.h`); every monitor created over the machine
  then has them. Flags: `UAM_MONITOR_STOPPED_ONLY` (refused while it runs),
  `UAM_MONITOR_CHANGES` (it changes the machine: `--monitor-log` records it). The command
  does what the board says; ember names no device. ember's
  `examples/external-board/emulation/ext_board.c` (`ext-id`) is the pattern.

## API checklist for your pinned commit
Ember is young and its API moves. Before writing code against a new pin, run these in the
checkout and copy whatever the tree uses:

```bash
git -C <ember> log --oneline -5                                   # what you pinned
grep -n "UAM_ABI_VERSION" include/uam/common.h                    # bumped on every struct/signature change
ls include/uam/ include/uam/dev/ include/uam/isa/                 # interfaces, stock devices, ISAs
grep -n "build\|uam_board_config_\|add_state" include/uam/machine.h  # board API
grep -n "uam_bus_map_\|uam_map_attrs\|UAM_PRIO_\|dirty" include/uam/bus.h      # mapping, priorities
grep -n "impl_min\|write_masked\|endian\|save\|STATELESS" include/uam/device.h  # device ops contract
grep -n "uam_state_put_\|uam_state_get_" include/uam/state.h       # snapshot streams
grep -n "uam_board_add_" include/uam/boards.h                     # board helpers (MMIO rules)
grep -n "uam_monitor_offer_command\|UAM_MONITOR_" include/uam/monitor.h  # board monitor commands (if present)
grep -n "_configure\|_ref;\|_BASE" include/uam/isa/mips32.h      # CPU models, address spaces (your ISA's header:
                                                                  # the second table of "Per ISA")
sed -n '/^static uam_status .*build/,/^}/p' boards/mips_ref.c     # how ember wires that ISA (its reference board)
grep -n "uam_exec_add_pc_hook\|uam_hook_action\|UAM_STOP_" include/uam/exec.h
sed -n '/static void usage/,/^}/p' src/runner/runner.c            # the runner's options
sed -n '/^ \* Exit status/,/\*\//p' src/runner/runner.c             # exit codes (also in uam-run --help)
sed -n '/Dependency rules/,/^## 4/p' docs/architecture.md          # layering (check-layering.sh mirrors it)
```

Then run `skills/tests/run-tests.sh <ember checkout>` (in the ember repository that holds
these skills): it scaffolds, builds and runs a board per ISA and the example devices against
that ember and reports what broke.
