# UAM firmware emulator (ember) - top-level build.
#
#   meson setup build            configure (once); options: meson_options.txt, `meson configure build`
#   meson compile -C build       build/uam-run and the libraries
#   meson test -C build          the whole test suite (tests/meson.build lists the suites)
#   meson test -C build --suite unit      one suite on its own
#   meson install -C build       headers, libraries, uam-run and uam.pc under --prefix
#
# Out-of-tree use (README.md, "Building a board on ember"): a board project consumes ember
# as a meson subproject - `dependency('uam')` with subprojects/ember.wrap pinning a
# revision - or, installed, through pkg-config (`uam`).
#
# Libraries, one per module group (each is also contained in libuam.a):
#   libuam-core.a     IR, bus, scheduler, signals/chardev/netdev plumbing, executor, analysis passes,
#                     loader, machine
#   libuam-riscv.a    RISC-V ISA module          libuam-mips.a   MIPS32 ISA module
#   libuam-arm.a      ARM (AArch32) ISA module   libuam-ppc.a    32-bit PowerPC ISA module
#   libuam-x86.a      x86-64 ISA module
#   libuam-avr.a      8-bit AVR ISA module       libuam-avr32.a  AVR32 (A and B) ISA module
#   libuam-pic.a      8-bit PIC ISA module
#   libuam-mcs51.a    8051 (MCS-51) ISA module   libuam-m6502.a  6502 (NMOS, 2A03, 65C02) ISA module
#   libuam-z80.a      Z80 ISA module
#   libuam-xtensa.a   Xtensa (LX106, LX6, LX7; little- and big-endian) ISA module
#   libuam-devices.a  stock device models        libuam-boards.a stock boards (rv32-ref, mips-ref, ...)
#   libuam-fuzz.a     the fuzz harness and its frontends (fuzz/)
#   libuam-host.a     host backends, gdb stub, the stock runner (uam_run_main; uses libuam-fuzz)

project('ember', 'c',
  version: '0.2',
  meson_version: '>= 1.1',
  default_options: [
    'c_std=c11',
    'warning_level=2',          # -Wall -Wextra
    'werror=true',
    'buildtype=debugoptimized', # -O2 -g
    'default_library=static',
    'b_staticpic=false',
  ],
)

cc = meson.get_compiler('c')

# Required flags, always applied (to ember's own sources only, also as a subproject).
add_project_arguments('-D_DEFAULT_SOURCE',
  cc.get_supported_arguments('-Wshadow', '-Wstrict-prototypes', '-Wmissing-prototypes'),
  language: 'c')

uam_inc = include_directories('include')
m_dep = cc.find_library('m', required: false)   # the x87 model uses long-double libm functions

# One list per module, mirroring the dependency rules in docs/architecture.md.
ir_src = files('src/ir/builder.c', 'src/ir/verify.c', 'src/ir/print.c', 'src/ir/opt.c', 'src/ir/pass.c')
core_src = files('src/core/common.c', 'src/core/bus.c', 'src/core/sched.c', 'src/core/chardev.c',
  'src/core/netdev.c', 'src/core/state.c', 'src/core/link.c')
exec_src = files('src/exec/interp.c', 'src/exec/tcache.c', 'src/exec/run.c', 'src/exec/cpu.c',
  'src/exec/jit.c', 'src/exec/jit_x86.c', 'src/exec/jit_a64.c', 'src/exec/coverage.c',
  'src/exec/cmp.c', 'src/exec/frames.c', 'src/exec/spin.c')
analysis_src = files('analysis/cov.c', 'analysis/cmp.c', 'analysis/frames.c')
loader_src = files('src/loader/loader.c')
machine_src = files('src/machine/machine.c', 'src/machine/snapshot.c', 'src/machine/guard.c',
  'src/machine/frames.c', 'src/machine/heap.c', 'src/machine/uninit.c', 'src/machine/rules.c')

modules = {
  'core': ir_src + core_src + exec_src + analysis_src + loader_src + machine_src,
  'riscv': files('isa/riscv/rv_lift.c', 'isa/riscv/rv_system.c'),
  'mips': files('isa/mips32/mips_lift.c', 'isa/mips32/mips_system.c', 'isa/mips32/mips16.c',
    'isa/mips32/mips_umips.c', 'isa/mips32/mips_fpu.c', 'isa/mips32/mips_dsp.c'),
  'arm': files('isa/arm/arm_lift.c', 'isa/arm/arm_thumb.c', 'isa/arm/arm_system.c'),
  'x86': files('isa/x86/x86_decode.c', 'isa/x86/x86_f80.c', 'isa/x86/x86_lift.c',
    'isa/x86/x86_lift_ops.c', 'isa/x86/x86_lift0f.c', 'isa/x86/x86_string.c', 'isa/x86/x86_sse.c',
    'isa/x86/x86_sse_ops.c', 'isa/x86/x86_sse_fp.c', 'isa/x86/x86_x87.c', 'isa/x86/x86_helpers.c',
    'isa/x86/x86_mmu.c', 'isa/x86/x86_seg.c', 'isa/x86/x86_msr.c', 'isa/x86/x86_smm.c',
    'isa/x86/x86_gdb.c', 'isa/x86/x86_system.c', 'isa/x86/x86_task.c'),
  'ppc': files('isa/ppc32/ppc_lift.c', 'isa/ppc32/ppc_fpu.c', 'isa/ppc32/ppc_mmu.c',
    'isa/ppc32/ppc_booke.c', 'isa/ppc32/ppc_system.c'),
  'avr': files('isa/avr8/avr_lift.c', 'isa/avr8/avr_system.c', 'isa/avr8/avr_des.c'),
  'avr32': files('isa/avr32/avr32_lift.c', 'isa/avr32/avr32_dec.c', 'isa/avr32/avr32_dsp.c',
    'isa/avr32/avr32_system.c'),
  'pic': files('isa/pic8/pic_lift.c', 'isa/pic8/pic_system.c'),
  'mcs51': files('isa/mcs51/mcs51_lift.c', 'isa/mcs51/mcs51_system.c'),
  'm6502': files('isa/m6502/m6502_lift.c', 'isa/m6502/m6502_system.c'),
  'z80': files('isa/z80/z80_lift.c', 'isa/z80/z80_system.c'),
  'xtensa': files('isa/xtensa/xtensa_decode.c', 'isa/xtensa/xtensa_lift.c',
    'isa/xtensa/xtensa_system.c', 'isa/xtensa/xtensa_fpu.c'),
  'devices': files('devices/clint.c', 'devices/plic.c', 'devices/uart16550.c',
    'devices/test_finisher.c', 'devices/unknown_mmio.c', 'devices/unimp.c', 'devices/virtio_net.c',
    'devices/mmio_stub.c', 'devices/gic.c', 'devices/i8259.c', 'devices/i8254.c',
    'devices/mc146818.c', 'devices/lapic.c', 'devices/ioapic.c', 'devices/pci.c',
    'devices/ata_core.c', 'devices/ahci.c', 'devices/ide.c', 'devices/nvme.c',
    'devices/cfi_flash.c', 'devices/pc_misc.c', 'devices/acpi_pm.c', 'devices/ipi.c',
    'devices/i8237.c', 'devices/vga.c', 'devices/fdc.c', 'devices/lpt.c', 'devices/i8042.c',
    'devices/hpet.c', 'devices/gptimer.c', 'devices/avr32_intc.c', 'devices/mcs51_timer.c',
    'devices/mcs51_serial.c', 'devices/mcs51_port.c', 'devices/pic32_intc.c', 'devices/z80_ctc.c'),
  'boards': files('boards/rv32_ref.c', 'boards/mips_ref.c', 'boards/arm_ref.c',
    'boards/x86_64_ref.c', 'boards/ppc_ref.c', 'boards/avr_ref.c', 'boards/avr32_ref.c',
    'boards/pic_ref.c', 'boards/mcs51_ref.c', 'boards/pic32_ref.c', 'boards/m6502_ref.c',
    'boards/z80_ref.c', 'boards/xtensa_ref.c', 'boards/stock.c', 'boards/mmio_rules.c'),
  'fuzz': files('fuzz/harness.c', 'fuzz/frontend_replay.c', 'fuzz/stock_exts.c',
    'fuzz/cov_scheme.c', 'fuzz/site_list.c', 'fuzz/cov_cli.c', 'fuzz/frontend_net.c',
    'fuzz/cov_region.c', 'fuzz/mutate.c', 'fuzz/campaign.c', 'fuzz/i2s.c'),
  'host': files('src/host/chardev_backends.c', 'src/host/netdev_backends.c',
    'src/host/blockdev_file.c', 'src/host/hostloop.c', 'src/debug/gdbstub.c',
    'src/runner/runner.c', 'src/runner/trace.c'),
}

# Every source is compiled once: each module library is built from its sources, and
# libuam.a is made of the same objects.
uam_objects = []
foreach name, src : modules
  module_lib = static_library('uam-' + name, src,
    include_directories: uam_inc,
    install: true)
  uam_objects += module_lib.extract_all_objects(recursive: false)
endforeach

libuam = static_library('uam',
  objects: uam_objects,
  include_directories: uam_inc,
  install: true)

# What an out-of-tree project links: `dependency('uam')` resolves to this when ember is
# its subproject (subprojects/ember.wrap: `[provide] uam = uam_dep`).
uam_dep = declare_dependency(
  link_with: libuam,
  include_directories: uam_inc,
  dependencies: m_dep,
  version: meson.project_version())
meson.override_dependency('uam', uam_dep)

uam_run = executable('uam-run', 'tools/uam-run.c',
  dependencies: uam_dep,
  install: true)

install_subdir('include/uam', install_dir: get_option('includedir'))

# uam.pc for an installed ember (and build/meson-uninstalled/uam-uninstalled.pc for a
# build tree).
import('pkgconfig').generate(libuam,
  name: 'uam',
  filebase: 'uam',
  description: 'UAM firmware emulator library',
  libraries: m_dep)

# The tests, their firmware and their check targets: only in ember's own build.
if get_option('tests') and not meson.is_subproject()
  subdir('tests')
endif
