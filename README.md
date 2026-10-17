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
(a block optimizer feeds both). A gdb stub, host console and network backends (tap, tun,
UDP, pcap; virtio-net on `rv32-ref`), and a library build for out-of-tree boards. C11,
no dependencies beyond libc.

## Build and test

```bash
make                 # build/uam-run and the libraries
make test            # unit tests, RISC-V and MIPS firmware self-tests, external-board example
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
build/uam-run --jit path/to/firmware.elf       # native x86-64 execution (else interpreter)
build/uam-run --no-opt path/to/firmware.elf    # skip the block optimizer
build/uam-run --help
```

`--unknown-mmio zero|echo` keeps firmware running past unmodelled peripherals and
logs the first access to each address. That log is the bring-up to-do list.
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
Only the modules you reference are linked: a MIPS-only board pulls in neither
the RISC-V lifter nor the stock boards.

## Layout

```
include/uam/       public interfaces: ir.h isa.h bus.h device.h signal.h sched.h cpu.h exec.h loader.h
                   machine.h chardev.h netdev.h host.h gdbstub.h runner.h boards.h
include/uam/dev/   one header per device model
include/uam/isa/   one header per ISA module
src/ir/            IR builder, verifier, printer
src/core/          bus, scheduler, logging, chardev/netdev plumbing
src/exec/          interpreter, translation cache, run loop
src/loader/        ELF / raw loaders
src/machine/       machine lifecycle, resets, board registry
src/host/          chardev/netdev backends, host loop, real-time pacing
src/debug/         gdb remote stub
src/runner/        the command-line runner as a library (uam_run_main)
isa/riscv/         RISC-V lifter (rv_lift.c) and privileged architecture (rv_system.c)
isa/mips32/        MIPS32 lifter (mips_lift.c) and privileged architecture (mips_system.c)
devices/           clint, plic, uart16550, sifive_test, unknown_mmio, unimp, virtio_net
boards/            rv32_ref.c (RISC-V), mips_ref.c (MIPS): the only files that pair an ISA with devices
tools/uam-run.c    uam-run: registers the stock boards and calls uam_run_main
examples/          external-board: an out-of-tree board using the library
tests/unit/        IR semantics/verifier, core (bus, resets, hooks), host side
tests/firmware/    bare-metal RV32 self-test and network-test images
tests/net/         net-check.sh: the network test against real tap/tun interfaces
```
