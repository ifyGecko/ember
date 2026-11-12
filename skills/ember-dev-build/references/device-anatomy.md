# Ember device anatomy (one component = one file)

Patterns, so iterations are spent on *semantics* rather than boilerplate. Every model named
here exists as a complete file under `examples/` (next to this page), and all of them are
compiled and run against ember by `skills/tests/run-tests.sh` (in the ember repository):

| example | shows |
|---|---|
| `soc_uart.c/.h` | the full device contract: registers, chardev TX/RX, a level IRQ, `write_masked` for byte stores, snapshot `save`/`restore` of a FIFO |
| `soc_timer.c/.h` | a counter derived from virtual time, a one-shot `uam_timer` for compare-match, saving an armed timer |
| `soc_intc.c/.h` | an interrupt controller: an input sink function + one output line; saving input levels |
| `soc_reset.c/.h` | a reset/clock block: lock bits, self-clearing bits, non-zero dividers, a chip-reset line |
| `reg_stride.c/.h` | wrapping a byte-register device for a word-spaced SoC bus (the stock 16550 has this built in, below); delegating snapshots to the inner device |
| `nor_flash.c/.h` | flash as a ROMD region: XIP reads from the array, commands to the device; the array is memory, so snapshots cover it |

Copy one into `emulation/`, rename, and change it to the semantics the firmware shows.

**Get register semantics from the firmware's own driver, not from a datasheet guess.**
Decompile the functions that touch the page (the RECON MMIO table lists them, or
`get_xrefs_to` on the address). Write down exactly what each one reads, writes and polls,
then implement that. Put those driver addresses in the device file's header comment.

## The device contract (`uam/device.h`)

```c
typedef struct soc_uart {
    uam_device   dev;          /* FIRST member: the bus hands the ops &dev */
    uam_chardev *cd;           /* host side, given by the board */
    uam_signal   irq;          /* an output line; the board connects it */
    ...                        /* register state */
} soc_uart;

static const uam_device_ops soc_uart_ops = {
    .type = "soc-uart",
    .endian = UAM_ACC_BE,         /* the registers' byte order (include "uam/access.h") */
    .impl_min = 4, .impl_max = 4, /* the access sizes the model implements */
    .read = uart_read,            /* (dev, off, size, *val): off is relative to the mapped base */
    .write = uart_write,
    .write_masked = uart_write_masked, /* byte stores into a wider register (see below) */
    .reset = uart_reset,          /* machine reset: back to power-on values */
    .destroy = uart_destroy,      /* free; detach from chardev/netdev; cancel timers */
    .save = uart_save,            /* snapshots: write the state (uam/state.h) ... */
    .restore = uart_restore,      /* ... and read it back in the same order */
    /* .flags = UAM_DEVICE_STATELESS   instead of save/restore, only if it holds no state */
    /* .peek: optional side-effect-free read, so --dump-mem and gdb can read the region */
};

uam_device *soc_uart_create(const char *name, uam_chardev *cd);   /* calloc + uam_device_init */
uam_signal *soc_uart_irq_line(uam_device *dev);                   /* lines for the board */
```

The header exports only the create function, the line accessors, input sink functions and a
`SIZE` constant. (The example's `soc_uart_feed` is used by the ember-fuzz skill; ignore it
during bring-up.) The board maps it (`uam_bus_map_mmio`), adds it to the machine (so it is
reset and destroyed with it) and wires its lines. Read `examples/soc_uart.c` in full once:
every device follows that shape.

Write a device from what the firmware does to it, not from a guess: `--trace mmio
--trace-mmio-range BASE:BASE+SIZE` lists every access the guest makes to the window (order,
width, value, pc), first against the catch-all, then against your model to compare. Its
summary at the stop (also `-t mmio-summary` alone) is one line per register: reads and
writes, the value reads always (or last) returned, the last write, the first pc - which
registers to model at all, which are plain configuration storage, which are polled
(`diagnostics.md`, section 4). A device
that wants to say which code touched it (a first-access log) appends `uam_origin_format()`
(`uam/common.h`: " pc=0x... icount=N"); it never asks for the processor itself.

`uam/device.h` does not include `uam/access.h`: include it yourself for `UAM_ACC_BE`.

### Byte order and access width are the bus's job
- `endian` is the register byte order. The bus converts, so `read`/`write` see register
  *values*. A big-endian CPU on a big-endian device gets the value; a byte load at offset 0
  gets its most significant byte. A mapping can override it per board
  (`uam_map_attrs.endian`), for a device that sits on a bus of the other byte order.
- `impl_min`/`impl_max` are the sizes the model implements (0 = any). A narrower read is
  served from the containing `impl_min` register; a wider access is split.
- A narrower **write** goes to `write_masked(dev, off, size, val, mask)` (the containing
  register, `mask` = 0xFF per enabled byte) if the device has one; otherwise the bus does
  read-merge-write, which **reads the register first**. Any register with read side effects
  (an RX FIFO pop, read-to-clear status) needs `write_masked`.
- Ember's `uam_uart16550` defaults to byte registers at consecutive offsets. Many MIPS SoCs
  space them 4 bytes apart ("reg-shift 2"): create it with `uam_uart16550_create_cfg(name, cd,
  &(uam_uart16550_cfg){ .reg_shift = 2, .endian = UAM_ACC_BE })` and map
  `UAM_UART16550_REGS << reg_shift` bytes (`UAM_UART16550_SIZE` holds shifts up to 5). A
  whole-slot access or a byte access to the register's low byte reaches it; the slot's other
  bytes read 0 and drop writes. `reg_stride` is the same layout as an adapter, for a
  byte-register device of your own that has no spacing option.

### Snapshots: every device saves its state
Ember snapshots a whole machine (`--save-snapshot`, `--snapshot-at`; ember's fuzz harness
is built on them), and taking one **fails, naming the device, if any device has neither
`save`/`restore` nor `UAM_DEVICE_STATELESS`**. Write both when you create the device, not later: the same
iteration that adds a register adds it to the stream. `examples/soc_uart.c` and
`examples/soc_timer.c` are the patterns.

- **Save** everything the guest can observe or that decides future behavior: registers,
  FIFO contents (oldest first, so restore can lay them out from index 0), latched status,
  internal counters and state machines, the time base of a derived counter. **Don't save**
  wiring and configuration (sink pointers, the bus, the chardev, sizes, clock rates), host
  handles, or diagnostics. Configuration that must match between the two machines is saved
  as a value (`uam_state_put_u32(w, CAP)`) and checked on restore
  (`uam_state_expect_u32(r, CAP, "fifo depth")`), which fails the restore on a mismatch.
- **Timers**: `uam_state_put_timer` / `uam_state_get_timer` for every `uam_timer` the device
  owns, armed or not. Restore re-arms them in their original order, so ties fire as before.
  An armed timer nobody saves makes the snapshot fail.
- **Output lines**: `uam_state_put_signal` / `uam_state_get_signal`. Restore sets the level
  without calling the sink, because the consumer restores its own view: an interrupt
  controller therefore saves its *input* levels too (`examples/soc_intc.c`).
- Same order on both sides, fixed widths (`put_u32`, `put_u64`, `put_bytes`). Errors are
  sticky, so no error handling is needed; validate counts read back
  (`if (n > CAP) uam_state_fail(r, ...)`) before using them as lengths.
- After restoring a receive buffer, call `uam_chardev_accept_input` / `uam_netdev_accept_input`:
  its free space changed.
- ROM/RAM/ROMD contents and the ROMD mode are memory, restored before `restore` is called.
  A device that writes a backing store directly reports it (`uam_bus_romd_dirty`,
  `uam_bus_mark_dirty`) or an incremental restore keeps the change.
- A device that owns another device the machine never sees (`reg_stride`) delegates `save`
  and `restore` to it.

Check a device with a split run (`snapshots.md`): the run must be identical
when cut by `--save-snapshot` / `--load-snapshot` at any point.

### What a device may use
Only the device-side interfaces: `uam/common.h access.h device.h signal.h sched.h chardev.h
netdev.h blockdev.h state.h bus.h`, the stock device headers `uam/dev/*.h`, and other device
headers of the project (`check-layering.sh` enforces this). Everything else comes from the board as a
parameter:

| need | parameter | use |
|---|---|---|
| time | `uam_sched *` | `uam_sched_now`, `uam_timer_init/arm/cancel` |
| snapshot state | the `save`/`restore` ops | `uam_state_put_*` / `uam_state_get_*` (`uam/state.h`) |
| an interrupt / GPIO / reset output | a `uam_signal` it owns | `uam_signal_set(&s->irq, level)`; the board connects it |
| an input (interrupt controller, GPIO) | a sink function in its header | `void x_input(void *ctx, unsigned n, int level)` |
| console | `uam_chardev *` | `uam_chardev_write`, `uam_chardev_set_frontend`, `uam_chardev_accept_input` |
| network | `uam_netdev *` | `uam_netdev_send`, `uam_netdev_set_frontend`, `uam_netdev_accept_input` |
| guest memory (DMA) | `uam_bus *` | `uam_bus_dma_read/write` (physical addresses) |
| end the run | `uam_finish_fn` + ctx | the board passes `uam_machine_stop_cb, m` |
| a clock rate | a number | e.g. ticks per counter increment |

Log unmodelled offsets with `uam_log(UAM_LOG_WARN, "<type>", ...)`, so a half-modelled
device still tells you what it ignores.

## Register-semantics traps (each one cost a debugging round)

| Pattern in the driver | What the model must do |
|---|---|
| write a bit, then `while (reg & bit);` (reset, busy, "capture", "flush") | **self-clear** that bit on write. Operations complete instantly. |
| poll a status bit before continuing (TX ready, PLL lock, PHY link) | return the "ready" value |
| `reg = 0xffffffff` in an ISR | write-1-to-clear (W1C) |
| `reg = reg & ~bit` in an ISR | write-0-to-clear: `stat = (stat & val) \| still_asserted_levels` |
| ISR handles ONE item per interrupt (one RX byte, one packet) | level-triggered source: keep the line asserted while work remains |
| `assert(status ⊆ enable)` | the status register is the *masked* view (`raw & enable`) |
| a field used as a divisor (clock decode) | a non-zero reset value; 0 means a divide-by-zero trap (MIPS `break 7`) |
| a chip ID / revision compared to constants | the exact value the driver accepts (read the compare) |
| `sb`/`sh` to a 32-bit register with read side effects | `write_masked` |

**Test a hypothesis with a rule before writing the model.** `--mmio-rule ADDR=fixed:VALUE`
(or `echo`, `echo,set=MASK`, `zero`, `fault`) answers one register without touching the
board; rules sit over the catch-all windows and under every real device. If the guest gets
past the poll with the rule, put that semantics into the owning device; if not, the guess was
wrong and no C was written. `-v` logs each rule's first access per register.

**Replacing a catch-all changes behavior.** The `unimp` window reads 0 and ignores writes. A
storage-backed model returns what was written, so a write-busy-then-poll loop that used to
exit immediately can now hang. When you swap a window for a real device, either keep
unmodelled offsets read-as-zero/write-ignore (and log them), or audit the driver's polls.
Keep the window mapped under the device: it still logs offsets outside the device's size.

**Ghidra's decompiler drops MMIO stores inside delay or bit-bang loops** (it sees repeated
stores to "memory" as dead stores). For drivers built from loops (bit-banged SPI/I2C/MDIO,
LED blink), read the **disassembly**.

## Time: virtual ticks, never host time
- Ember's clock is **ticks = guest instructions issued + ticks skipped while idle**. There is
  no Hz. A device that counts converts with a rate the **board** chooses: define one
  `BOARD_TICKS_PER_US` (ember's `--realtime` default paces 100 M ticks/s, i.e. 100) and derive
  every device's `ticks_per_count` from the counter's clock. The MIPS CP0 Count advances at
  ticks/2, the PowerPC time base and decrementer every `tb_div` ticks, Xtensa CCOUNT, the ARM
  generic timer's count, AVR32 COUNT, the CLINT's `mtime` and the 8051 timers one per tick (one
  per instruction), the Z80 CTC one per 16 or 256 ticks.
- Derive counters from `uam_sched_now()` (`examples/soc_timer.c`: `(now - base) /
  ticks_per_count`); never tick them in a loop.
- `uam_timer_init` once at create; `uam_timer_arm(sched, &t, absolute_tick)` to (re)arm;
  `uam_timer_cancel` in `reset` and `destroy`; save it in `save`. Never re-initialise an
  armed timer.
- **Arm only what the guest asked for, one shot at a time.** An armed timer lets an idle hart
  skip ahead to it. With a one-shot timer, "line not wired" ends as a clean deadlock (exit
  125) right after the match. A self-re-arming timer whose interrupt never reaches the CPU
  keeps the hart skipping from event to event until `--max-stall` events (default 4194304)
  pass with no instruction retired; then the run ends as a livelock (exit 128), or, with
  console/network input attached, sleeps waiting for input. Re-arm when the driver
  re-programs the compare register. A genuinely periodic tick (hardware that reloads by
  itself) may re-arm itself, at its previous deadline plus the period (not `now` plus the
  period, which drifts); the sum cannot overflow, because virtual time stops at 2^62 ticks.
  With its interrupt unwired, such a tick ends in exit 128, never 125.
- Firmware delay loops calibrated against a timer stay consistent, because both are counted
  in the same instructions. A loop that is too slow is a `ticks_per_count` choice, not a bug.

## Interrupt controller (cascade) in its own device
`examples/soc_intc.c`: inputs arrive through a sink function, the output is a `uam_signal`.

```c
void soc_intc_input(void *ctx, unsigned n, int level);       /* in soc_intc.h */
/* board: */
uam_signal_connect(soc_timer_irq_line(timer), soc_intc_input, intc, 3);
uam_signal_connect(soc_intc_output(intc), uam_cpu_irq_sink, &m->cpu, UAM_MIPS_IRQ_HW0);
```

Sinks are called only when a level changes. `--trace irq` shows every change of a
processor's interrupt inputs (and, at the stop, whether one is pending and enabled), so an
interrupt that never arrives is either never raised there (follow the chain back with
`--trace mmio` on the controller) or masked in the CPU (`--trace sysreg`: the status/mask
register writes). `--trace exc` ends with the handlers entered and how often, so a working
chain shows up as a handler count. A line has one sink: an output that must reach
two places needs a splitter. To find the routing (which CPU line, which status bit, which
vector), look for the RTOS's interrupt table. VxWorks/MIPS keeps an `intPrioTable` of
`{Cause IP bit, vector-or-demux function, SR mask, flag}`; Linux uses irq-domain setup in
the SoC IRQ driver. A demux function reads `status & mask`, picks a bit and returns
`base_vector + bit`. Reuse ember's stock controllers where the SoC has the standard one:
`uam_clint` / `uam_plic` (SiFive layouts), `uam_gic` (ARM GICv2), `uam_avr32_intc`,
`uam_pic32_intc` (presents its request to `uam_mips_eic_input`), the PC's 8259/APICs.

## Reset and clock blocks
`examples/soc_reset.c`: PLL lock bits read set, "apply" bits self-clear, divider fields reset
to non-zero, and a chip-reset bit raises a `uam_signal` that the board connects to
`uam_machine_reset_sink`. The reset happens at the next instruction boundary; the device
lowers its line again in its own `reset` op. RAM survives, so firmware "magic in memory, then
reset" tricks work as on hardware.

## Flash
| hardware | ember |
|---|---|
| read-only/XIP window | `uam_bus_map_rom` (+ `uam_bus_map_alias` for mirrors); a guest write faults |
| NOR with a command interface | ROMD region + a flash device (`examples/nor_flash.c`) |
| SPI NOR behind a controller | a controller device that decodes the SPI commands the driver sends against a backing buffer; optionally a ROMD/ROM "memory-mapped read" window over the same buffer |

ROMD essentials (`examples/nor_flash.c`, board side in `skills/tests/fixture/testbrd_board.c`):

```c
/* board */
uam_device *fl = nor_flash_create("flash", &m->bus, 0x1000, 0x000122a8);
uam_region *flash;
uam_machine_add_device(m, fl);
uam_bus_map_romd(&m->bus, "flash", 0x1f000000, size, fl, data, len, NULL, &flash);
uam_bus_map_alias(&m->bus, "flash.mirror", 0x1f000000 + size, size, flash, 0, NULL);
nor_flash_set_region(fl, flash);
/* device */
uam_bus_romd_set_mode(bus, region, false);  /* ID/status mode: reads come to the device */
uint8_t *mem = uam_bus_romd_backing(region); /* program/erase the array ... */
uam_bus_romd_dirty(bus, region, off, len);   /* ... then drop code translated from it (and
                                              * mark the pages dirty for snapshot restore) */
uam_bus_romd_set_mode(bus, region, true);    /* back to array reads and XIP */
```

Small flashes are usually **mirrored** across a larger window: map aliases, never a second
copy. The flash contents come from a named image (`--image flash=build/flash.bin`) built by a
`tools/` script; to persist guest writes, have the device write the backing store back to a
file in `destroy` (opt-in with a board option). A snapshot already holds the array, so a
device's `save` stream carries only its command state.

## DMA engines and network interfaces
Ember's `devices/virtio_net.c` is the worked example of a NIC: descriptor rings and frames
through `uam_bus_dma_read/write`, frames through a `uam_netdev`.

```c
/* create(name, bus, nd, mac): the board passes &m->bus and uam_board_config_netdev(cfg, "lan") */
uam_netdev_set_frontend(nd, x_can_receive, x_receive, x_link_changed, s);
uam_netdev_send(nd, frame, len);          /* TX: a whole Ethernet frame, no preamble/FCS */
uam_netdev_accept_input(nd);              /* the driver returned RX buffers: deliver queued frames */
/* destroy: */ uam_netdev_set_frontend(nd, NULL, NULL, NULL, NULL);
```

- When the guest has no free RX buffer, `can_receive` returns false: ember queues (and the
  host loop stops reading the backend), so nothing is dropped. Call `accept_input` when the
  driver hands buffers back, and after `restore`.
- Snapshots: rings and buffers live in guest RAM (saved with it); the MAC saves its
  registers, ring indices and pending interrupt state. `devices/virtio_net.c` shows it.
- A NULL netdev (port not given with `--nic`) is an unplugged cable: create the MAC anyway
  and report link down. Link state comes from `link_changed`.
- Decode the descriptor layout from the driver's RX/TX reap loops: which bit means "owned by
  hardware", where the length lives, whether it includes the 4-byte FCS (drivers often
  subtract 4). `-t dma,mmio` shows the ring base the driver programs and every transfer your
  model then makes (`dma: r|w LEN 0xADDR region+0xOFF pc=.. icount=N`; `(fault)`: not RAM, a
  wrong address or layout); `--dump-mem` the ring at a `--break` to read it whole.
- Drivers often store **virtual** (kseg0/kseg1) pointers in descriptors; DMA takes physical
  addresses: mask with `& 0x1FFFFFFF` on MIPS. Bound every ring walk.
- Several ports: the board names them (`"wan"`, `"lan1"`), one `uam_board_config_netdev`
  lookup each. A MAC behind an on-chip switch has no backend of its own: connect it to the
  switch model through a frame-port interface (below).

**Testing with real clients, rootless:** `--nic lan=tap:tap0` needs a tap device; create it in
a user + network namespace (`unshare -Urn`, then `ip tuntap add dev tap0 mode tap`), exactly
as ember's `tests/net/net-check.sh` does; run `ping`/`busybox udhcpc`/`wget` there as the
client. `,pcap=build/lan.pcap` records a port. `udp:LPORT:RHOST:RPORT` cables two emulators
together. A host interface makes the runner pace virtual time to the host clock
(`--virtual-time` keeps it deterministic).

## Buses without MMIO (MDIO, I2C-like side channels)
Ember has no generic side bus. Define a tiny C interface in its own header (a device
header, so both sides may include it):

```c
/* mdio_bus.h */
typedef struct mdio_slave {
    uint16_t (*read)(struct mdio_slave *s, unsigned phy, unsigned reg);
    void     (*write)(struct mdio_slave *s, unsigned phy, unsigned reg, uint16_t v);
} mdio_slave;
/* the switch embeds a mdio_slave and exports  mdio_slave *sw_mdio(uam_device *);
 * the MAC's create function takes  mdio_slave *  - the board passes one to the other */
```

That keeps the MAC and the switch in separate files with no knowledge of each other's
internals. The same pattern carries frames between a MAC and a switch's CPU port
(`can_receive` / `receive` / `link_up` hooks). There is no ordering trap: the board creates
both devices first, then hands each the other's interface pointer.

## Device granularity (restating the rule)
- each peripheral block (UART, timer/reset block, interrupt controller, GPIO, flash
  controller, Ethernet MAC, switch, radio, FPGA) → its own `.c` / `.h`
- bus glue (an interface header, maybe a small `.c`) → its own file
- function replacements (PC hooks) → `<board>_hle.c`, never inside a device
- the board → `<board>_board.c` (wires only)

The Makefile compiles every `emulation/*.c`; there is nothing else to register.
