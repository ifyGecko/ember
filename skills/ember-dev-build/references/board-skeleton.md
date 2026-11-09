# Board skeleton

The board file is the only place where an ISA, memory and devices meet. It creates the hart,
maps RAM, places the firmware, makes the hart start at the entry point, instantiates each
peripheral, maps it at its MMIO address and connects its lines. **It wires things; it does
not implement devices.** One board file per target: `emulation/<board>_board.c`.

`new-emulation.sh` generates a board that already builds and runs
(`templates/board.c.in`). This page explains each part and what to change. The tested
`skills/tests/fixture/testbrd_board.c` (in the ember repository, next to this skill) shows
the same board with a UART, a word-spaced 16550, a timer, an interrupt controller, a reset
block and a finisher wired in.

## Per ISA: what the generated board starts from

The defaults come from ember's reference board of each ISA (`boards/<isa>_ref.c`), which has
only what every board of that architecture has; `--model` picks its CPU model. Addresses are
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
   `m->reset_pc = BOARD_RESET_VEC`. `uam_machine_init`, `uam_machine_add_device` and
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
| the unmapped fallback | - | `--unknown-mmio zero` or `echo` (`uam_unknown_mmio`), else a bus fault |

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
    return uam_exec_add_pc_hook(&m->exec, 0x80123450 /* rfCalibrate */, skip_rf_cal, NULL);
}
```

(`reg()` looks a name up in `cpu->isa->regs`.) Hook a function's first instruction, never a
branch delay slot. Adding a hook discards translated code once; it costs nothing afterwards.
The board calls the install function before `uam_machine_reset`.

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

## Board options, named images, network ports
- `-o key=value` → `uam_board_config_option(cfg, "key")` (NULL if absent). Use options for
  variants (console on which UART, straps, chip revision) instead of rebuilding.
- `--image name=path` → `uam_board_config_image(cfg, "name")`.
- `--nic port=SPEC` → `uam_board_config_netdev(cfg, "port")`; NULL means the cable is
  unplugged: create the MAC anyway, report link down.
- `--console SPEC` → `cfg->console`, handed to exactly one UART model (a chardev has one
  frontend). The runner holds its input until the run starts, so fixed input
  (`string:`, `in=FILE`) survives the board's power-on reset and a `--load-snapshot`;
  `,after=PATTERN` and `script:FILE` release it only as the guest prints its prompts.
- `--mmio-rule` needs nothing from the board; a board that always wants some rules can map
  them itself with `uam_board_add_mmio_rules` (`uam/boards.h`), but prefer a device.

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
grep -n "_configure\|_ref;\|_BASE" include/uam/isa/<isa>.h       # CPU models, address spaces
sed -n '/^static uam_status .*build(/,/^}/p' boards/<isa>_ref.c   # how ember wires that ISA
grep -n "uam_exec_add_pc_hook\|uam_hook_action\|UAM_STOP_" include/uam/exec.h
sed -n '/static void usage/,/^}/p' src/runner/runner.c            # the runner's options
sed -n '/^ \* Exit status/,/\*\//p' src/runner/runner.c             # exit codes
sed -n '/Dependency rules/,/^## 4/p' docs/architecture.md          # layering (check-layering.sh mirrors it)
```

Then run `skills/tests/run-tests.sh <ember checkout>` (in the ember repository that holds
these skills): it scaffolds, builds and runs a board per ISA and the example devices against
that ember and reports what broke.
