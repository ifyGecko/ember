# UAM: a Universal Abstract Machine firmware emulator

UAM runs raw firmware images by lifting the native instruction set into a small,
ISA-neutral IR (47 opcodes, plus COV for instrumentation) and executing that IR. The abstract machine only does
computation. Memory, time, interrupts and devices all sit behind explicit interfaces,
with one module per concern.

- [docs/architecture.md](docs/architecture.md): modules, interfaces, dependency rules
- [docs/uam-isa.md](docs/uam-isa.md): the IR specification (normative)

Status: twelve guest ISAs - RV32IMAC (+Zicsr, Zifencei, M-mode) on the `rv32-ref` board,
MIPS32r2 big/little-endian (CP0, an R4K-style TLB or fixed mapping, GPR shadow register sets,
vectored and external-controller interrupts, configurable CPU model) on the `mips-ref` /
`mipsel-ref` boards and, as a MIPS32 microcontroller, the PIC32 memory map with a vectored
interrupt controller on the `pic32-ref` board (`-o model=mx|mm|mz`), and ARMv7-A (ARM and Thumb-2 with interworking, banked modes, CP15 with
the short-descriptor MMU, generic timer; little-endian and BE8) on the `arm-ref` / `armeb-ref`
boards with a GIC, x86-64 (real, protected, virtual-8086 and long mode, SMM, x87/MMX/
SSE2) on the `x86_64-ref` board (a generic PC that starts boot flash or a BIOS ROM at the reset
vector), and 32-bit
PowerPC (the user instruction set with the FPU, and either privileged architecture as the CPU
model selects: the classic one of the 6xx/7xx/74xx - BATs, segments and the hashed page table -
or Book E - IVORs, critical and machine-check classes, TCR/TSR timers and watchdog, TLB arrays
with the MAS interface; implementation SPRs, DCRs and TLB interfaces left to the board) on the
`ppc-ref` board, and 8-bit AVR (every core family of the AVR Instruction Set Manual: AVRe,
AVRe+, AVRxm (XMEGA, with its PMIC, DES and XCH/LAS/LAC/LAT), AVRxt (with its CPUINT) and
the reduced core AVRrc, each with the memory layout of common devices of the family) on the
`avr-ref` board, and AVR32 (both microarchitectures of the AVR32 Architecture Manual: AVR32A, the
UC3 class, with events on the system stack and an MPU, and AVR32B, the AP7 class, with
RSR/RAR per context, INT3 shadow registers, the P0-P4 segments, an ITLB and a DTLB and the
SIMD instructions; the DSP instructions on both) on the `avr32-ref` board with an AVR32-style
interrupt controller, and 8-bit PIC (the baseline, mid-range, enhanced mid-range and PIC18
instruction sets as CPU models of one module, with PIC18's extended instruction set as an
option; the core's own registers, hardware stack and interrupt gating, the peripherals left to
the board) on the `pic-ref` board, and the 8051 (the MCS-51 instruction set, with the
core's differences as CPU models: the 8051 and 8052, and a CIP-51-style core with extended
interrupts, SFR paging and code banking) on the `mcs51-ref` board with the 8051's own timers,
serial port and ports, the 6502 (the NMOS 6502 with its undefined opcodes, the 2A03 without
decimal mode and the WDC 65C02 as CPU models of one module) on the `6502-ref` board, and the
Z80 (documented and undocumented: the index registers' halves, the flags' bits 3 and 5,
MEMPTR, R, interrupt modes 0-2 and NMI; NMOS and CMOS models) on the `z80-ref` board with a
16550 and a Z80 CTC on its I/O ports, and Xtensa (a configurable core: the options of the
LX106, LX6 and LX7 cores as the ESP8266, ESP32 and ESP32-S3 have them, as CPU models of one
module - register windows, zero-overhead loops, booleans, MAC16, the single-precision FPU,
the XEA2 exceptions with up to seven interrupt levels, the NMI the highest, CCOUNT/CCOMPARE, Region
Protection - little- and big-endian) on the `xtensa-ref` and `xtensaeb-ref` boards - run by a
reference interpreter or, with `--jit`, a native JIT for x86-64 and AArch64 hosts
with block chaining (a block optimizer feeds both). Every reference board but `avr-ref`,
`avr32-ref`, `pic-ref`, `pic32-ref`, `mcs51-ref`, `6502-ref` and `z80-ref` (the microcontroller
boards have one processor) takes `-o cpus=N`:
the processors run interleaved deterministically, so snapshots, coverage, fuzzing and the gdb
stub (one thread per processor) work on multiprocessor machines too. Machine snapshots (boot
once, resume in milliseconds), a gdb stub, guarded and watched guest memory (`--guard`,
`--watch`), a persistent-mode fuzz harness, host console and network backends (tap, tun, UDP,
pcap; a virtio-net device model), and a library build for out-of-tree boards. C11, no
dependencies beyond libc.

## Hosts

Ember builds and runs on any POSIX host with a C11 compiler (GCC or Clang) that has
`__int128`; the interpreter needs nothing more. The JIT has a code generator for x86-64
(`src/exec/jit_x86.c`) and one for little-endian AArch64 (`src/exec/jit_a64.c`), both on
the host-independent core in `src/exec/jit.c`; on any other host `--jit` falls back to the
interpreter. Guest behaviour does not depend on the host: the x87 model computes in
software (`isa/x86/x86_f80.c`), and `make test` passes the same checks on both JIT hosts.

- **Linux on x86-64 and on AArch64**: everything, tested.
- **macOS** (Apple silicon and Intel): written for and not yet tested there. The code arena
  is a `MAP_JIT` mapping that a thread switches between writable and executable
  (`pthread_jit_write_protect_np`), and the AArch64 code generator leaves `x18`, the
  platform register, alone. A binary signed with the hardened runtime needs the
  `com.apple.security.cs.allow-jit` entitlement. The tap and tun network backends are
  Linux-only; x87 transcendentals compute in the host's `long double`, a `double` on Apple
  silicon (everything else in the x87 model is exact there too).

To build for another host, point the build at its compiler, e.g.
`make CC=aarch64-linux-gnu-gcc AR=aarch64-linux-gnu-ar BUILD=build-a64`, and run
`make test` with the same variables on that host. The firmware images are
host-independent.

## Build and test

```bash
make                 # build/uam-run and the libraries
make test            # unit tests, RISC-V, MIPS, ARM, x86, PowerPC, AVR, AVR32, PIC, PIC32, 8051, 6502, Z80 and Xtensa firmware self-tests, external-board example,
                     # snapshot split runs (interpreter and JIT), coverage and fuzzing checks, the gdb stub,
                     # the runner's debugging options, the cross-ISA benchmark's checksums
                     # (and suzzer interop, with a suzzer checkout)
make runner-test     # one group on its own (also unit-test, fuzz-test, gdb-test, bench-test, ...)
make jitdiff-soak    # the JIT-against-interpreter differential test, at length
make xbench          # time the cross-ISA benchmark (docs/jit-perf.md)
make net-check       # ARP/ping between an rv32-test guest and the host kernel over a real tap and tun
make lib BUILD=DIR   # libraries only, into DIR (for out-of-tree projects)
make install PREFIX=DIR
```

The MIPS self-test needs `mips-linux-gnu-gcc` / `mipsel-linux-gnu-gcc`; so does the PIC32
self-test (`pic32-ref`), which is built as MIPS32 little-endian.

The ARM self-test (built as ARM and as Thumb-2 code, little-endian and BE8) needs
`arm-linux-gnueabi-gcc`.

The firmware self-test needs a RISC-V GCC (`riscv64-linux-gnu-gcc` or
`riscv64-unknown-elf-gcc` both work).

The PowerPC self-test needs a GCC for PowerPC: by default `powerpc64le-linux-gnu-gcc`, which
builds the 32-bit big-endian code with `-m32 -mbig-endian`; a 32-bit one works too
(`make CROSS=powerpc-linux-gnu- ppc-test`).

The x86 self-tests are assembled with `nasm`; the BIOS boot and gdb stub checks need `python3`.

The AVR self-tests (one per core family) and the AVR builds of the fuzz target and the
benchmark need `avr-gcc` and avr-libc (Debian: `gcc-avr`, `binutils-avr`, `avr-libc`), and
`python3` for their flag vectors.

The AVR32 self-tests, fuzz target and benchmark builds need `avr32-gcc`, from the AVR32 GNU
toolchain (3.4.3, gcc 4.4; no distribution packages it). `make AVR32_CROSS=PREFIX test` points
the build at it (default `avr32-` in PATH); without it the AVR32 firmware checks are skipped
with a note. Its tools want `LC_ALL=C`, and its static binaries call the legacy vsyscall page,
which current kernels no longer provide: they crash in `time`/`gettimeofday` unless the kernel
emulates vsyscalls (`vsyscall=emulate`) or the two calls are patched into system calls.

The PIC self-tests (one per core family, and PIC18's extended instruction set) and the PIC
fuzz target are assembled with `gpasm` from gputils (Debian: `gputils`) and need `python3`;
without gpasm they are skipped with a note (`GPASM=path` names another). There is no PIC build
of the benchmark: its data (about 20 KiB) does not fit an 8-bit PIC's data space.
The 8051 self-tests (one per core model) are assembled with `as31` (Debian: `as31`); the
8051 fuzz target and benchmark build need SDCC (Debian: `sdcc`). Without either, its checks
are skipped with a note (`AS31=`, `SDCC=` point the build at them).
The 6502 self-tests (one per core model) are assembled with SDCC's `sdas6500` and linked with
its `sdld`; the 6502 fuzz target and benchmark build need `sdcc` (all in Debian's `sdcc`).
Without them those checks are skipped with a note (`SDAS6500=`, `SDLD=`, `SDCC=`).
The Z80 self-tests (one per core model) are assembled with SDCC's `sdasz80`, and the Z80 fuzz
target and benchmark build with SDCC; without them they are skipped (`SDASZ80=` names it).
The Xtensa firmware (self-test, fuzz target, benchmark) is built with each core's GNU
toolchain: the LX106's `xtensa-lx106-elf-` (Debian's `gcc-xtensa-lx106`), the LX6's and LX7's
`xtensa-esp32-elf-` and `xtensa-esp32s3-elf-` (Espressif's `xtensa-esp-elf` release). A model
whose toolchain is missing is skipped with a note (`XTENSA_LX106_CROSS=`, `XTENSA_LX6_CROSS=`,
`XTENSA_LX7_CROSS=` give the prefixes). No big-endian Xtensa toolchain is needed: the
big-endian self-test is the little-endian build laid out the big-endian way
(tests/isa-xtensa/le2be.py).

## Run firmware

```bash
build/uam-run build/firmware/selftest.elf
build/uam-run --stats --trace exc --unknown-mmio zero path/to/firmware.elf
build/uam-run --raw-base 0x80000000 path/to/image.bin
build/uam-run -b arm-ref build/firmware-arm/selftest-thumb.elf   # ARMv7, Thumb-2 code
build/uam-run -b armeb-ref build/firmware-arm/selftest-arm-be.elf # big-endian (BE8)
build/uam-run -b ppc-ref build/firmware-ppc/selftest.elf        # 32-bit PowerPC
build/uam-run -b ppc-ref -o model=booke build/firmware-ppc/selftest-booke.elf  # Book E
build/uam-run -b ppc-ref --raw-base 0xfff00000 --entry 0xfff00100 boot.bin   # a PowerPC boot ROM
build/uam-run -b x86_64-ref --image flash=fw.rom -o cpus=4       # a PC: boot flash ending at 4 GiB
build/uam-run -b x86_64-ref --image bios=bios.rom                # a legacy BIOS ROM (also below 1 MiB)
build/uam-run -b x86_64-ref --raw-base 0 --entry 0 flat.bin      # a flat image, entered in real mode
build/uam-run --raw-base 0x80100000 --entry 0x801002bc image.bin  # reset code not at the base
build/uam-run -b avr-ref build/firmware-avr/selftest-eplus.elf   # 8-bit AVR (ATmega2560 layout)
build/uam-run -b avr-ref -o model=xm build/firmware-avr/selftest-xm.elf  # XMEGA
build/uam-run -b avr32-ref build/firmware-avr32/selftest-a.elf   # AVR32A (UC3 class)
build/uam-run -b avr32-ref -o model=b build/firmware-avr32/selftest-b.elf  # AVR32B (AP7 class)
build/uam-run -b pic-ref build/firmware-pic/selftest-pic18.elf  # PIC18
build/uam-run -b pic-ref -o model=enhanced build/firmware-pic/selftest-enhanced.elf  # PIC16F1xxx
build/uam-run -b mcs51-ref build/firmware-mcs51/selftest-8052.bin  # an 8052 (raw image at 0)
build/uam-run -b mcs51-ref -o model=cip51 build/firmware-mcs51/selftest-cip51.bin  # CIP-51 style
build/uam-run -b 6502-ref build/firmware-6502/selftest-nmos.bin  # NMOS 6502 (raw ROM image at 0x8000)
build/uam-run -b 6502-ref -o model=65c02 build/firmware-6502/selftest-65c02.bin  # WDC 65C02
build/uam-run -b z80-ref build/firmware-z80/selftest-nmos.bin  # a Z80 (raw image at 0)
build/uam-run -b xtensa-ref -o model=lx106 build/firmware-xtensa/selftest-lx106.elf  # ESP8266's core
build/uam-run -b xtensa-ref build/firmware-xtensa/selftest-lx6.elf  # the ESP32's core (lx6, the default)
build/uam-run -b pic32-ref -o model=mz build/firmware-pic32/selftest-mz.elf  # PIC32 (MIPS32, TLB, EIC)
build/uam-run --jit path/to/firmware.elf       # native execution on x86-64 and AArch64 hosts
build/uam-run --jit --no-chain fw.elf          # return to the run loop after every block
build/uam-run --no-opt path/to/firmware.elf    # skip the block optimizer
build/uam-run --cov-blocks --cov-out cov.map fw.elf  # block-edge coverage map (64 KiB by default)
build/uam-run --cov-blocks --fuzz-begin 0x80000318 --fuzz-input uart0 --fuzz-replay crashes/ fw.elf
build/uam-run --help
```

Boards take options (`-o KEY=VALUE`) and named images (`--image NAME=PATH`); `--board list`
names the boards. Every reference board takes `--ram SIZE` and, but `avr-ref`, `avr32-ref`, `pic-ref`,
`pic32-ref`, `mcs51-ref`, `6502-ref` and `z80-ref`,
`-o cpus=N`
(`--quantum N`: instructions each processor runs before the next, default 4096); `ppc-ref` takes
`-o model=classic|booke`. `avr-ref` takes `-o model=e|eplus|xm|xt|rc` (default `eplus`), each
with the memory layout of a common part of that core family, so firmware built for that part
runs (`e` ATmega328P, `eplus` ATmega2560, `xm` ATxmega128A1U, `xt` ATmega4809, `rc`
ATtiny40; boards/avr_ref.c has the map); its devices (a 16550, a timer, the test finisher) are
generic, not those parts' peripherals, and `--ram` sets the size of its SRAM. `avr32-ref` takes
`-o model=a|b` (default `a`: RAM at 0 and code memory at the reset address 0x80000000; `b`: code
memory at physical 0, reset through P2 at 0xa0000000, RAM at physical 0x10000000), with its
interrupt controller, a 16550 and the test finisher at 0xffff0800, 0xffff1400 and 0xffff1800.
`pic-ref` takes `-o model=baseline|mid|enhanced|pic18` (default `pic18`; the program memory and
data space of a PIC16F59, PIC16F877, PIC16F19197 and PIC18F4620) and `-o xinst=1` (PIC18's
extended instruction set); its data space is RAM wherever the core has no register, with a
16550, a timer and the test finisher (boards/pic_ref.c has the map). gpasm writes Intel hex:
tests/firmware-pic/hex2elf.py turns a hex file and its listing into an ELF image with the
listing's labels as symbols.
`mcs51-ref` takes `-o model=8051|8052|cip51` (default `8052`): 64 KiB of program memory (128
KiB banked from 0x8000 on `cip51`, with PSBANK at SFR 0xB1 and SFR paging), external data RAM
below 0xFF00 (`--ram` sets its size) with the test finisher at 0xFF00, and in the SFR space the
8051's Timer 0/1 (TCON at 0x88), serial port (the console, SCON at 0x98) and ports P0-P3; an
image built with SDCC runs as a raw binary (`makebin -p`).
`z80-ref` takes `-o model=nmos|cmos` (default `nmos`; they differ in what OUT (C),0 writes):
64 KiB of RAM (`--ram` sets less), and on I/O ports (the low byte decoded) a 16550 (the
console, ports 0x00-0x07), a Z80 CTC (0x10-0x13), whose vectors serve interrupt mode 2, and
the test finisher (0xF0); images are raw binaries loaded at 0.
`xtensa-ref` (little-endian) and `xtensaeb-ref` (big-endian) take `-o model=lx106|lx6|lx7`
(default `lx6`; the cores of the ESP8266, ESP32 and ESP32-S3, with their options, interrupt
tables and vectors) and `-o cpus=N` (1-4; PRID 0xCDCD, 0xABAB, ...): RAM at 0x40000000, where
VECBASE and the reset vectors are (`--ram`, default 4 MiB), a 16550 at 0x60000000 (interrupt
5), timers at 0x60001000 (interrupt 10, edge) and 0x60002000 (interrupt 14, the NMI) and the
test finisher at 0x60003000; designer-defined TIE (the ESP32-S3's AI instructions, the ESP32's
double-precision helpers) is not modelled.
`pic32-ref` is a MIPS32 microcontroller with the PIC32 memory map (boot flash at 0x1fc00000,
program flash at 0x1d000000, RAM at physical 0) and a PIC32-style vectored interrupt controller;
`-o model=mx|mm|mz` picks the core (mx: MIPS16e-capable, fixed mapping; mm: microMIPS-only; mz:
MIPS32 + microMIPS, a TLB, FPU and DSP), and on mz `-o bootisa=micromips` resets into microMIPS.
MIPS32, MIPS16e and microMIPS firmware runs (mx and mz switch between their two ISAs through
JALX and jumps to odd addresses; mm is microMIPS only), and mz has the FPU (FR = 0 and 1, in
either ISA) and the DSP ASE revision 2 (again in either ISA).
`6502-ref` takes `-o model=nmos|2a03|65c02` (default `nmos`): RAM from 0 to 0x7EFF (`--ram` sets
its size), an I/O page at 0x7F00 with a 16550 (the console, IRQ line 0), the test finisher at
0x7F10 and two general-purpose timers at 0x7F20 (IRQ line 1) and 0x7F30 (the NMI), and ROM from
0x8000 with the vectors at its top; a raw image is loaded at 0x8000 and the core starts at its
reset vector.
`x86_64-ref` takes the images `flash` (a CFI boot flash of up to 16 MiB ending at 4 GiB; `-o persist=1` writes programs and erases back to the file) and `bios`
(a legacy BIOS ROM of up to 1 MiB, its last 128 KiB also just below 1 MiB), `-o apic-hz=HZ`
(the local APIC timer clock, default 1 GHz), `-o smm-map=intel64|amd64` (the SMRAM
save-state layout the firmware's SMM handler expects; `amd64` also brings AMD's SMM_ADDR,
SMM_MASK and HWCR.SMMLOCK: the TSeg closed outside SMM, locked until RESET),
`-o smm-code-chk=1` (offer Intel's SMM code-access check, which the firmware enables),
`-o smm-blocked=1` (offer Intel's MSR_SMM_BLOCKED/MSR_SMM_DELAYED: the processors that hold
an SMI while they wait for a startup IPI) and
`-o smm-confine=1` (an SMM instruction fetch outside SMRAM is a machine check, on any
vendor: a crash oracle for SMM call-outs).

With `--jit`, compiled blocks jump straight into each other (block chaining) instead of
returning to the run loop after every block, which pays off most on firmware with short
blocks: an RTOS boot on a MIPS32 board ran 7.5x faster (19.8 s to 2.6 s for 3e9 instructions).
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
(an exception, a `--fuzz-crash-pc`, a reset, a touched redzone or guard), hang or timeout, or
rejected when the route refuses an input. The `mem:` route can put read and/or write redzones
in front of its buffer and past the input in it (`before=N`, `after`). `--fuzz-replay
FILE|DIR` runs saved inputs once each and exits with the most severe verdict of any input
(20/21/25 crash, else 22 hang, else 23 timeout, else 24 rejected, else 0). See [`docs/fuzzing.md`](docs/fuzzing.md).

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
126 internal error or fetch-fault loop, 127 breakpoint or debug trap, 129 a `--guard` stopped the
run, 130 SIGINT/SIGTERM. `--fuzz-replay` has its own (20-25, above).

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
can be narrowed with `--trace-range LO:HI` and `--trace-after N`. A block trace lists every
block also with `--jit`: blocks are not chained while it is on. `--trace-after N` is an
absolute instruction count, like the `icount` the runner prints, and a snapshot keeps that
count; `--trace-after +N` counts from where the run starts. `-n` and `--max-ticks` always
count from where the run starts (after `--load-snapshot`, from the snapshot).

```bash
build/uam-run --log-pc 0x8023f00c fw.elf      # report every arrival at a pc
build/uam-run --break 0x8023f00c -d fw.elf    # stop there and dump the hart
build/uam-run --break 0x8023f00c --dump-mem 0x80100000+256 fw.elf   # and hex-dump guest memory
build/uam-run --break 0x8023f00c --dump-mem p:0x100000+64K=heap.bin fw.elf  # physical, raw to a file
build/uam-run --guard 0x80100000-16+16:w fw.elf   # stop before any write within 16 bytes of an address
build/uam-run --watch p:0x3000+4 fw.elf          # log every access to 4 physical bytes
build/uam-run --gdb 1234 fw.elf               # then: gdb-multiarch fw.elf -ex 'target remote :1234'
build/uam-run --console pty fw.elf            # console on a pseudo-terminal (also tcp:PORT, unix:PATH,
                                              # stdio-raw, file:OUT[,in=FILE], string:TEXT,
                                              # script:FILE, null)
build/uam-run --console 'string:ls\n,after=# ' fw.elf  # type "ls" once the shell's prompt is printed
build/uam-run --realtime fw.elf               # pace virtual time to the host clock
build/uam-run --map -b mips-ref               # print the bus map
```

`--dump-mem` and gdb read memory without side effects. A device region (a ROM or flash window
served by a read handler, a frame buffer) is readable only if its device implements
`uam_device_ops.peek`, a side-effect-free read (ember's VGA memory and PCI option ROMs do);
otherwise the dump names the region and its device and writes no file. On PC boards an access
no device claims reads all ones (master abort), as on hardware; the first one at each memory
page or I/O port is logged as a warning, and the most frequent are listed at the end of the run.

`--guard [p:]ADDR[-BEFORE][+AFTER][:r|w|rw][,stop|,fault|,log][,name=NAME]` guards the bytes
`[ADDR-BEFORE, ADDR+AFTER)` (a bare ADDR is one byte; ADDR is virtual unless `p:`). A data access
that touches them in the given directions (default both) is refused and stops the run at that
instruction (`stop`, the default: exit 129, SIGSEGV in gdb), becomes a bus fault for the guest
(`fault`), or is allowed and logged (`log`; `--watch SPEC` is `--guard SPEC,log`). Instruction
fetches, gdb and `--dump-mem` reads and DMA are not guarded. `uam/guard.h` is the same in a
program; the fuzz harness builds its redzones on it ([`docs/fuzzing.md`](docs/fuzzing.md#guards---guard---watch)).

Fixed console input (`string:TEXT`, `file:OUT,in=FILE`) reaches the guest when the run starts,
after the board's power-on reset and any `--load-snapshot`, never while the board is being built.
Firmware that drains its UART while booting, or an OS driver that clears the receive FIFO,
discards input typed ahead like this, as real hardware does. To type at a prompt instead, add
`,after=PATTERN` as the last option: each line of the input (through its `\n`) waits until the
guest has printed PATTERN (`\n`, `\r`, `\t`, `\\`, `\xHH` escapes; the rest of the spec, commas
included). Nothing is queued before the prompt, so nothing during boot can eat it, and the run stays
deterministic: the guest's output is. A session with different prompts is a script,
`--console script:FILE` (or `file:OUT,script=FILE`), one step per line:

```
# log in, then run a command
expect login:\x20
send root\n
expect #\x20
send /wmi_example\n
```

`expect TEXT` holds the following steps until the guest prints TEXT (only output after the step is
reached counts); `send TEXT` queues TEXT. The text is everything after the keyword's single space,
with the escapes above. Input still waiting at the end of the run is reported as a warning.
Patterns match output of this run: after `--load-snapshot` at a prompt that is not printed again,
use plain input (`--load-snapshot shell.snap --console 'string:help\n'`).

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

Append `,pcap=FILE` to record the port's traffic. The reference boards have no network
ports (not every board of their architecture has one). The test board `rv32-test`
(`tests/boards/`, run by `build/uam-test-run`) shows how a board adds them: ports
`net0`..`net7` put a virtio-net device in the matching virtio-mmio slot
(`-o net0.mac=02:75:61:6d:01:00`).

```bash
ip tuntap add dev tap0 mode tap user $USER       # once, as root: then no privilege is needed
ip addr add 10.0.2.2/24 dev tap0 && ip link set tap0 up
build/uam-test-run -b rv32-test --nic net0=tap:tap0,pcap=net0.pcap fw.elf
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

## Agent skills

`skills/` holds agent skills that take a firmware image from an unknown blob to a running
ember board and then fuzz it: `ember-dev-recon` (triage: architecture, load address, memory
map, MMIO, whether ember can run it), `ember-dev-build` (an out-of-tree board brought up one
device at a time, against a local ember clone pinned to the latest commit, with any change
to ember kept as `git am`-ready patch files) and `ember-fuzz` (the snapshot
fuzz harness and suzzer). They are plain `SKILL.md` folders; `skills/install.sh` links them
into Claude Code and Qwen Code (`--claude`, `--qwen`, or `--agents` for `~/.agents/skills`).
`skills/tests/run-tests.sh ~/ember` checks their tooling end to end. See `skills/README.md`.

## Layout

```
include/uam/       public interfaces: common.h access.h ir.h isa.h bus.h device.h signal.h sched.h
                   cpu.h exec.h loader.h machine.h state.h snapshot.h chardev.h netdev.h
                   blockdev.h host.h gdbstub.h runner.h boards.h pass.h cov.h guard.h fuzz.h
include/uam/dev/   one header per device model
include/uam/isa/   one header per ISA module (riscv.h mips32.h arm.h x86.h ppc.h avr.h avr32.h
                   pic.h mcs51.h m6502.h z80.h xtensa.h)
src/ir/            IR builder, verifier, printer
src/core/          bus, scheduler, logging, chardev/netdev plumbing, state streams
src/exec/          interpreter, translation cache, run loop, the JIT (jit.c: analyses, arena,
                   linking; code generators jit_x86.c and jit_a64.c), coverage, spin-loop
                   skipping
src/loader/        ELF / raw loaders
src/machine/       machine lifecycle, resets, board registry, snapshots, guards
src/host/          chardev/netdev/block-device backends, host loop, real-time pacing
src/debug/         gdb remote stub
src/runner/        the command-line runner as a library (uam_run_main)
analysis/          IR passes (cov.c: the edge-coverage pass)
fuzz/              the persistent-mode fuzz harness, the replay frontend, the runner's fuzz
                   options; fuzz/suzzer/: suzzer's formats (built only with a suzzer checkout)
isa/riscv/         RISC-V lifter (rv_lift.c) and privileged architecture (rv_system.c)
isa/mips32/        MIPS32 lifter (mips_lift.c) and privileged architecture (mips_system.c);
                   mips16.c (MIPS16e), mips_umips.c (microMIPS), mips_fpu.c (the FPU),
                   mips_dsp.c (the DSP ASE; umips_dsp.h, its microMIPS encodings)
isa/arm/           ARM lifters (arm_lift.c: A32 and the shared emitters, arm_thumb.c: T16/T32)
                   and privileged architecture (arm_system.c: modes, exceptions, CP15, MMU, timer)
isa/ppc32/         32-bit PowerPC lifter (ppc_lift.c), floating-point unit (ppc_fpu.c), MMU
                   (ppc_mmu.c: BATs, segments, hashed page table), operating environment
                   (ppc_system.c: MSR, SPRs, exceptions, decrementer, time base) and the Book E
                   model (ppc_booke.c: interrupts, timers, TLB arrays, MAS registers)
isa/x86/           x86-64 lifter (real, protected, virtual-8086 and long mode; x87 on a
                   software 80-bit format (x86_f80.c), MMX, SSE/SSE2) and system architecture (segmentation, call gates, hardware
                   task switching, the I/O permission bitmap, paging, SMM with the Intel 64
                   or AMD64 save-state map, A20M#, CPUID/MSRs)
isa/avr8/          8-bit AVR lifter (avr_lift.c: every core family, the model's instructions),
                   system side (avr_system.c: the CPU models, the data space on the bus,
                   interrupt entry, the AVRxm PMIC and AVRxt CPUINT, the core's I/O registers)
                   and the AVRxm DES instruction (avr_des.c)
isa/avr32/         AVR32 decoder (avr32_dec.c, avr32_op.h), lifter (avr32_lift.c: the whole
                   instruction set), DSP and SIMD (avr32_dsp.c), and system side
                   (avr32_system.c: SR and the modes, events and autovectored interrupts for
                   AVR32A and AVR32B, system registers, COUNT/COMPARE, the AVR32A MPU, the
                   AVR32B segments and TLBs)
isa/pic8/          8-bit PIC lifter (pic_lift.c: the baseline, mid-range, enhanced mid-range and
                   PIC18 instruction sets) and system side (pic_system.c: the CPU models, the
                   data space on the bus, the core's registers, the hardware stack, interrupt
                   entry with PIC18's priorities and the enhanced core's context save)
isa/mcs51/         8051 lifter (mcs51_lift.c: the MCS-51 instruction set, register banks and
                   the data pointer as translation state) and system side (mcs51_system.c:
                   the CPU models, interrupt levels and extended sources, SFR page saving,
                   code banking, the core SFRs)
isa/m6502/         6502 lifter (m6502_lift.c: the NMOS and 65C02 opcode maps, decimal mode as
                   translation state) and system side (m6502_system.c: the CPU models, the
                   interrupt poll's latency, NMI and IRQ entry, decimal arithmetic)
isa/z80/           Z80 lifter (z80_lift.c: the documented and undocumented instruction set,
                   prefixes, flags with bits 3 and 5, MEMPTR, Q and R) and system side
                   (z80_system.c: the CPU models, IFF1/IFF2 and the interrupt modes, NMI)
isa/xtensa/        Xtensa: the decoder (xtensa_decode.c over xtensa_opc.h, generated from
                   tests/isa-xtensa/opcodes.py; the big-endian field order), the lifter
                   (xtensa_lift.c: every option's instructions, the window-overflow check,
                   zero-overhead loops), the system side (xtensa_system.c: the LX106, LX6
                   and LX7 models, XEA2 exceptions and interrupts, register windows,
                   CCOUNT/CCOMPARE, Region Protection) and the FPU (xtensa_fpu.c: single
                   precision in software, the divide and square-root steps)
devices/           clint, plic, gic, uart16550, test_finisher, unknown_mmio, unimp, mmio_stub,
                   virtio_net; PC architecture: i8259, i8254, mc146818, lapic, ioapic, hpet,
                   i8042, acpi_pm, pc_misc (port 92, A20 gate, POST codes), i8237 DMA, fdc
                   (82077), ide (PIO + bus master), lpt, vga; pci (bridges, MSI/MSI-X,
                   expansion ROMs), ahci, nvme, cfi_flash; ipi; gptimer (a general-purpose
                   timer for CPUs without one of their own); avr32_intc (AVR32-style interrupt
                   controller: groups with a level and an autovector each); pic32_intc
                   (PIC32-style vectored interrupt controller on the MIPS EIC interface); the 8051's own
                   peripherals: mcs51_timer (Timer 0/1 and TCON), mcs51_serial (SCON/SBUF),
                   mcs51_port (ports and plain SFR registers); z80_ctc (the Z80 CTC: four
                   counter/timers with mode 2 vectors and daisy-chain priority)
boards/            rv32_ref.c (RISC-V), mips_ref.c (MIPS), arm_ref.c (ARM),
                   x86_64_ref.c (x86-64), ppc_ref.c (PowerPC), avr_ref.c (8-bit AVR),
                   avr32_ref.c (AVR32), pic_ref.c (8-bit PIC), pic32_ref.c (PIC32),
                   mcs51_ref.c (8051),
                   m6502_ref.c (6502), z80_ref.c (Z80), xtensa_ref.c (Xtensa):
                   reference boards of generic
                   parts, the only files that pair an ISA with devices; mmio_rules.c:
                   per-register rules for any board
tools/uam-run.c    uam-run: registers the stock boards and calls uam_run_main
tools/             also fetch-suzzer.sh (make suzzer), xbench.sh / jitbench.sh (timing),
                   jitprof.py (native-code profiles; docs/jit-perf.md)
examples/          external-board: an out-of-tree board using the library
skills/            agent skills (recon, board bring-up, fuzzing), their installer and tests
tests/unit/        IR semantics/verifier, core (bus, resets, hooks, guards, ABI), host side,
                   snapshots, JIT chaining and the JIT-vs-interpreter differential test, spin
                   loops, stores next to code, MMIO rules, coverage, the fuzz harness,
                   multiprocessor observers, suzzer's formats, the x87's software 80-bit
                   format (against the host's x87 on x86-64), the Xtensa FPU against exact
                   arithmetic (test_xtensa_fpu); and one test_<arch>.c per ISA for what its
                   self-test cannot reach (such as the CPU models and reset state, exception
                   and interrupt entry through the board's API, timers, the debugger view): test_riscv (with
                   the CLINT and PLIC), test_mips (CP0, TLB, vectored and EIC interrupts,
                   MIPS16e/microMIPS modes), test_arm (banked modes, aborts, CP15, the MMU's
                   formats, the generic timer, the GIC), test_x86 (reset and SIPI, IVT/IDT
                   delivery and double faults, SMM on both save maps, MSRs, CPUID), test_ppc,
                   test_avr (interrupt controllers, the general-purpose timer), test_avr32
                   (the event model of A and B, MPU and MMU, the interrupt controller),
                   test_pic (stacks, interrupt priorities), test_mcs51 (interrupts, SFR
                   pages, code banks, its timers and serial port), test_m6502 (arithmetic
                   over every input, the interrupt poll), test_z80 (interrupt modes, block
                   instruction flags, MEMPTR, the CTC) and test_xtensa (special registers per
                   model, interrupt levels, CCOMPARE, window exceptions, BREAK, the ERI)
tests/boards/      x86_64-test and rv32-test: reference boards plus components only the tests
                   use (storage, VGA, printer, virtio-net), run by build/uam-test-run
tests/firmware/    bare-metal RV32 self-test, network-test, multiprocessor, fuzz-target and
                   scripted-console images
tests/firmware-mips/ bare-metal MIPS32 self-test (big and little endian)
tests/firmware-arm/ bare-metal ARMv7 self-test (ARM and Thumb-2, LE and BE8) and fuzz target
tests/firmware-ppc/ bare-metal 32-bit PowerPC self-test (classic and Book E builds), multiprocessor
                   test and fuzz target
tests/firmware-avr/ 8-bit AVR self-test, one build per core family (flag vectors from the
                   manual's equations: gen_vectors.py), and fuzz target
tests/firmware-avr32/ AVR32 self-test (AVR32A and AVR32B builds) and fuzz target
tests/firmware-pic/ 8-bit PIC self-tests in gpasm assembly, one per core family and one for
                   PIC18's extended instructions (flag vectors from the data sheets'
                   definitions: gen_vectors.py), fuzz target, hex2elf.py
tests/firmware-mcs51/ 8051 self-test (as31), one build per core model (arithmetic vectors from
                   the instruction set's definitions: gen_vectors.py; CIP-51 code banks:
                   mkimage.py), and fuzz target (SDCC)
tests/firmware-6502/ 6502 self-test (sdas6500), one build per core model (instruction vectors
                   from the instruction definitions: gen_vectors.py), and fuzz target (SDCC)
tests/firmware-z80/ Z80 self-test (sdasz80), one build per core model (vectors from the
                   instruction set's definitions: gen_vectors.py), fuzz target and start code
                   for SDCC (crt0.s)
tests/firmware-xtensa/ Xtensa self-test, one build per model (computational vectors from
                   gen_vectors.py; windows, loops, exceptions, interrupts, ...), and with
                   start.S the C programs: fuzz target, benchmark
tests/isa-xtensa/  the opcode table (opcodes.py) and its generator, the decoder check against
                   the GNU disassemblers (xt-check.py), the big-endian layout (le2be.py)
tests/firmware-pic32/ self-test for the pic32-ref board (MIPS32 and microMIPS on mz, MIPS16e
                   on mx, microMIPS on mm)
tests/isa-mips/    the MIPS16e, microMIPS and DSP decoders checked against binutils
tests/firmware-x86/ x86-64 self-test, multiprocessor, legacy (tasks, V86, MMX), AMD SMM and BIOS
                   ROM tests (nasm)
tests/smp/         the multiprocessor self-test shared by RISC-V, MIPS, ARM and PowerPC
tests/snapshot/    snapshot-check.sh: split runs must equal the uninterrupted run
tests/cov/ tests/fuzz/  block-coverage maps across backends; the fuzz harness on planted bugs
tests/suzzer/ tests/szr/  suzzer's formats and the SZR1 executor (with a suzzer checkout)
tests/gdb/         gdb-check.py: the gdb stub over the remote protocol
tests/runner/      runner-check.sh: --log-pc, --dump-mem, --trace-after, --guard/--watch
tests/bench/       one C benchmark for every guest ISA but the 8-bit PIC (make bench-test,
                   make xbench)
tests/net/         net-check.sh: the network test against real tap/tun interfaces
```
