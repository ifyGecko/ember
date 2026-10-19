# UAM: a Universal Abstract Machine firmware emulator

UAM runs raw firmware images by lifting the native instruction set into a small,
ISA-neutral IR (47 opcodes) and executing that IR. The abstract machine only does
computation. Memory, time, interrupts and devices all sit behind explicit interfaces,
with one module per concern.

- [docs/architecture.md](docs/architecture.md): modules, interfaces, dependency rules
- [docs/uam-isa.md](docs/uam-isa.md): the IR specification (normative)

Status: two guest ISAs - RV32IMAC (+Zicsr, Zifencei, M-mode) on the `rv32-ref` board, and
MIPS32r2 big/little-endian (CP0, R4K-style TLB, configurable CPU model) on the `mips-ref` /
`mipsel-ref` boards - run by a reference interpreter or, with `--jit`, a native x86-64 JIT
with block chaining (a block optimizer feeds both). Machine snapshots (boot once, resume in
milliseconds), a gdb stub, host console and network backends (tap, tun, UDP, pcap; virtio-net
on `rv32-ref`), and a library build for out-of-tree boards. C11, no dependencies beyond libc.

## Build and test

```bash
make                 # build/uam-run and the libraries
make test            # unit tests, RISC-V and MIPS firmware self-tests, external-board example,
                     # snapshot split runs (interpreter and JIT)
make net-check       # ARP/ping between an rv32-ref guest and the host kernel over a real tap and tun
make lib BUILD=DIR   # libraries only, into DIR (for out-of-tree projects)
make install PREFIX=DIR
```

The MIPS self-test needs `mips-linux-gnu-gcc` / `mipsel-linux-gnu-gcc`.

The firmware self-test needs a RISC-V GCC (`riscv64-linux-gnu-gcc` or
`riscv64-unknown-elf-gcc` both work).

## Run firmware

```bash
build/uam-run build/firmware/selftest.elf
build/uam-run --stats --trace exc --unknown-mmio zero path/to/firmware.elf
build/uam-run --raw-base 0x80000000 path/to/image.bin
build/uam-run --raw-base 0x80100000 --entry 0x801002bc image.bin  # reset code not at the base
build/uam-run --jit path/to/firmware.elf       # native x86-64 execution (else interpreter)
build/uam-run --jit --no-chain fw.elf          # return to the run loop after every block
build/uam-run --no-opt path/to/firmware.elf    # skip the block optimizer
build/uam-run --cov-blocks --cov-out cov.map fw.elf  # block-edge coverage map (64 KiB by default)
build/uam-run --cov-blocks --fuzz-begin 0x80000318 --fuzz-input uart0 --fuzz-replay crashes/ fw.elf
build/uam-run --help
```

With `--jit`, compiled blocks jump straight into each other (block chaining) instead of
returning to the run loop after every block, which pays off most on firmware with short
blocks: a VxWorks boot on a MIPS board ran 7.5x faster (19.8 s to 2.6 s for 3e9 instructions).
Results are bit-identical with and without it, and to the interpreter. `--stats` reports how
many blocks were entered by chaining.

`--cov-blocks` counts AFL-style edges between translated blocks into a coverage map
(`--cov-map-size`, a power of two), and `--cov-out FILE` writes the raw map when the run
stops. Blocks are not basic blocks, so the edges are approximate; site-accurate coverage
comes from a site table (`uam_exec_set_coverage`, `uam/exec.h`). The interpreter and the JIT
produce byte-identical maps, and the guest runs exactly as without coverage. Overhead on a
synthetic worst case (a MIPS loop of 2-7 instruction blocks, 3e8 instructions): JIT 0.145 s
to 0.190 s (+31%), interpreter 2.93 s to 3.05 s (+4%). Real firmware has longer blocks.

`--fuzz-begin PC` turns the runner into a persistent-mode fuzz harness: it boots to PC,
snapshots there and runs every input from the snapshot, delivered through `--fuzz-input`
(`mem:ADDR`, `mmio`, or a board input such as `uart0`), and classifies each run as ok, crash
(an exception, a `--fuzz-crash-pc`, a reset), hang or timeout. `--fuzz-replay FILE|DIR` runs
saved inputs once each and exits with the most severe verdict of any input (20/21 crash,
else 22 hang, else 23 timeout, else 0). See [`docs/fuzzing.md`](docs/fuzzing.md).

With a [suzzer](docs/suzzer.md) checkout (`make SUZZER_DIR=path`, or `make suzzer` to clone one
into `./suzzer`), suzzer's site lists and boundary files plug in directly: `--cov-sites fw.lcv3` gives bitmaps
with the same meaning as libcov's (site-id or hashed-PC model, N=1..8, rotation, leapfrog's
emit stream), `--cov-boundary fw.lcvb` replaces `--fuzz-begin`/`--fuzz-end`, and
`--cov-strict` checks every site's bytes in memory against the list. `--fuzz-szr HOST:PORT`
lets suzzer's brain drive ember in place of `suzzer-relay`, and `--cov-shm PATH` publishes
the maps in an LCVS region for suzzer's readers.

A run always ends. `-n N` bounds guest instructions and `--max-ticks N` bounds virtual
time, which also advances while the hart idles in WFI/WAIT (both exit 124). When a hart
waits for an interrupt that the device timers can never deliver (an interrupt line left
unwired, say), `--max-stall N` timer events with no instruction retired (default 4194304)
end the time skipping: with a console or network input attached, the run sleeps until
input arrives (typing still wakes the guest, SIGINT/SIGTERM still stop it); with none,
nothing can ever wake the hart and the run stops as a livelock (exit 128). Idle time costs next to
nothing (the default 4194304 events take about 0.1 s). Virtual time never passes 2^62 ticks.
All of these are counted in virtual time, so runs stay deterministic.
The other exit statuses: the firmware's own status when it ends the run, 2 usage or setup error
(including a library/header ABI mismatch), 125 deadlock (waiting with nothing that could wake it),
126 internal error or fetch-fault loop, 127 breakpoint or debug trap, 130 SIGINT/SIGTERM.

```bash
build/uam-run -n 50000000 --max-ticks 1000000000 fw.elf   # bound both instructions and idle time
```

`--unknown-mmio zero|echo` keeps firmware running past unmodelled peripherals and
logs the first access to each address. That log is the bring-up to-do list.

`--mmio-rule` answers individual unmodelled registers, for when firmware needs more than
zero, for example a status register that must read "ready". The syntax is
`ADDR[+SIZE]=POLICY[,width=N][,set=MASK][,clear=MASK]`, and the policies are `zero`, `echo`,
`fixed:VALUE`, `fault` (bus error), and `input`, which reads the next bytes of
`--mmio-input FILE` (for fuzzing). Rules sit over a board's catch-all windows and unmapped
holes and under its real devices; later rules win where they overlap. Values are what the
CPU reads, on either byte order. Registers are 4 bytes unless `width=` says otherwise. `-v`
logs each rule's first access to each register and an access summary. A snapshot taken with
rules is restored with the same rules.

```bash
build/uam-run --mmio-rule 0x18040000=fixed:0x1 fw.elf                 # a status register reads 1
build/uam-run --mmio-rule 0x18020000+0x100=echo,set=0x60 fw.elf       # read back writes, bits 5-6 set
build/uam-run --mmio-rule 0x18060010=input,width=1 --mmio-input case.bin fw.elf
```
`--trace ir` prints every translated block in the IR's text form; `--trace blocks`
can be narrowed with `--trace-range LO:HI` and `--trace-after N`.

```bash
build/uam-run --log-pc 0x8023f00c fw.elf      # report every arrival at a pc
build/uam-run --break 0x8023f00c -d fw.elf    # stop there and dump the hart
build/uam-run --gdb 1234 fw.elf               # then: gdb-multiarch fw.elf -ex 'target remote :1234'
build/uam-run --console pty fw.elf            # console on a pseudo-terminal (also tcp:PORT, unix:PATH,
                                              # stdio-raw, file:OUT[,in=FILE], string:TEXT, null)
build/uam-run --realtime fw.elf               # pace virtual time to the host clock
build/uam-run --map -b mips-ref               # print the bus map
```

Fixed console input (`string:TEXT`, `file:OUT,in=FILE`) reaches the guest when the run starts,
after the board's power-on reset and any `--load-snapshot`, never while the board is being built.
Firmware that drains its UART while booting discards input typed ahead like this, as real
hardware does. To script a shell, save a snapshot at the prompt and resume from it with the
input (`--load-snapshot shell.snap --console 'string:help\n'`).

## Snapshots

Boot once, then start every later run from the booted state:

```bash
build/uam-run --break 0x8023f00c --save-snapshot booted.snap fw.elf   # stop where "booted" is, save
build/uam-run --snapshot-at 0x8023f00c --save-snapshot booted.snap fw.elf  # or save there and run on
build/uam-run -n 100000000 --save-snapshot booted.snap fw.elf         # or after N instructions
build/uam-run --load-snapshot booted.snap -n 5000000                  # resume (same board and options)
```

A run split by `--save-snapshot` / `--load-snapshot` is bit-identical to the uninterrupted run,
whichever backend (interpreter or `--jit`) runs each half. Without `--snapshot-at`, the snapshot
is saved when the run stops, Ctrl-C included.

In-process, `uam/snapshot.h` restores a machine in microseconds. Writes are tracked per 4 KiB
page, so restoring the last snapshot copies back only what the guest changed, and translated or
JIT-compiled code is kept for unchanged pages:

```c
#include "uam/snapshot.h"

uam_snapshot *booted;
uam_exec_run(&m.exec, boot_insns);       /* the slow part, once */
uam_snapshot_take(&m, &booted);          /* or uam_snapshot_load("booted.snap", &booted) */
for (int i = 0; i < n_tests; i++) {
    uam_snapshot_restore(&m, booted);
    /* ... feed input, uam_exec_run(&m.exec, budget), check the outcome ... */
}
uam_snapshot_save(booted, "booted.snap");
uam_snapshot_free(booted);
```

Every device must take part. Implement `save` and `restore` in its `uam_device_ops` with the
state stream in `uam/state.h`, or set `.flags = UAM_DEVICE_STATELESS` if it holds no
guest-visible state. Save registers, FIFOs, latched conditions and counters, and timers
(`uam_state_put_timer`) and output lines (`uam_state_put_signal`). Don't save wiring,
configuration or host handles. Board-private state goes in with `uam_machine_add_state`.
Taking a snapshot fails, naming the culprit, if a device has neither, or if an armed timer is
not saved by its owner. Code that writes a RAM/ROMD backing store directly, rather than through
the bus or `uam_bus_romd_dirty`, must report it with `uam_bus_mark_dirty`.

## Networking

A board names its network ports; `--nic PORT=SPEC` (repeatable) plugs one into a host
backend, and a port left out is an unplugged cable. Backends:

| SPEC | what the guest is attached to |
|---|---|
| `tap:[IFNAME]` | a Linux tap interface: a real Ethernet segment (bridge it, or give the tap an address) |
| `tun:[IFNAME]` | a Linux tun interface (IP only) behind an adapter that answers ARP / IPv6 ND as one host-side station |
| `tap-fd:FD`, `tun-fd:FD` | an interface already attached by a privileged launcher |
| `udp:LPORT:RHOST:RPORT` | a UDP socket pair: a "cable" or radio "air" to another emulator |
| `null` | a link with nothing on the other end |

Append `,pcap=FILE` to record the port's traffic. On the `rv32-ref` board, ports
`net0`..`net7` put a virtio-net device in the matching virtio-mmio slot
(`-o net0.mac=02:75:61:6d:01:00`).

```bash
ip tuntap add dev tap0 mode tap user $USER       # once, as root: then no privilege is needed
ip addr add 10.0.2.2/24 dev tap0 && ip link set tap0 up
build/uam-run --nic net0=tap:tap0,pcap=net0.pcap fw.elf
```

A host interface implies `--realtime`, because a guest network stack times out in virtual
time; `--virtual-time` keeps the run deterministic anyway. Boards built on the library get
ports with `uam_board_config_netdev(cfg, "wan")`; devices reach guest memory with
`uam_bus_dma_read` / `uam_bus_dma_write`.

## Using UAM from another project

Register a board and reuse the stock runner; see `examples/external-board/`:

```c
int main(int argc, char **argv)
{
    uam_board_register(&my_board);
    return uam_run_main(argc, argv);
}
```

Build with `make -C path/to/uam BUILD=$PWD/build/uam lib` and link
`build/uam/libuam.a` (or `pkg-config --cflags --libs uam` after `make install`).
Compile against the headers of the same tree the library was built from. A board
built from different headers is stopped at `uam_board_register` / `uam_machine_init`
with an `abi:` error that names each struct whose size differs and exit status 2, instead
of corrupting memory. Rebuild the library and the board together after updating UAM.
Only the modules you reference are linked: a MIPS-only board pulls in neither
the RISC-V lifter nor the stock boards.

## Layout

```
include/uam/       public interfaces: common.h access.h ir.h isa.h bus.h device.h signal.h sched.h
                   cpu.h exec.h loader.h machine.h state.h snapshot.h chardev.h netdev.h host.h
                   gdbstub.h runner.h boards.h
include/uam/dev/   one header per device model
include/uam/isa/   one header per ISA module
src/ir/            IR builder, verifier, printer
src/core/          bus, scheduler, logging, chardev/netdev plumbing, state streams
src/exec/          interpreter, translation cache, run loop, x86-64 JIT (jit_x86.c)
src/loader/        ELF / raw loaders
src/machine/       machine lifecycle, resets, board registry, snapshots
src/host/          chardev/netdev backends, host loop, real-time pacing
src/debug/         gdb remote stub
src/runner/        the command-line runner as a library (uam_run_main)
isa/riscv/         RISC-V lifter (rv_lift.c) and privileged architecture (rv_system.c)
isa/mips32/        MIPS32 lifter (mips_lift.c) and privileged architecture (mips_system.c)
devices/           clint, plic, uart16550, sifive_test, unknown_mmio, unimp, mmio_stub, virtio_net
boards/            rv32_ref.c (RISC-V), mips_ref.c (MIPS): the only files that pair an ISA with
                   devices; mmio_rules.c: per-register rules for any board
tools/uam-run.c    uam-run: registers the stock boards and calls uam_run_main
examples/          external-board: an out-of-tree board using the library
tests/unit/        IR semantics/verifier, core (bus, resets, hooks, ABI), host side, snapshots,
                   JIT chaining, MMIO rules
tests/firmware/    bare-metal RV32 self-test and network-test images
tests/firmware-mips/ bare-metal MIPS32 self-test (big and little endian)
tests/snapshot/    snapshot-check.sh: split runs must equal the uninterrupted run
tests/net/         net-check.sh: the network test against real tap/tun interfaces
```
