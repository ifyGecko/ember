# The cross-ISA benchmark (bench.c) for every reference board, with each board's own start
# code, and natively for the host (the reference checksums). A meson project of its own,
# because it is built several times over with different work: tools/xbench.sh configures
# and builds it into the directories it names.
#
#   meson setup DIR tests/bench [-Drounds=N] [-Donly=K] && meson compile -C DIR
#
#   -Drounds=N   rounds per kernel (default: bench.c's BENCH_ROUNDS)
#   -Donly=K     run kernel K only
# A guest whose toolchain is not found is left out (meson_options.txt names them).

project('ember-bench', 'c',
  meson_version: '>= 1.1',
  default_options: ['buildtype=plain', 'warning_level=0'])

fs = import('fs')
here = meson.current_source_dir()
fw = here / '../firmware'
sh = find_program('sh')
python = find_program('python3')

defs = []
if get_option('rounds') > 0
  defs += '-DBENCH_ROUNDS=@0@'.format(get_option('rounds'))
endif
if get_option('only') >= 0
  defs += '-DBENCH_ONLY=@0@'.format(get_option('only'))
endif

executable('bench-host', 'bench.c',
  c_args: ['-O2', '-std=gnu11', '-Wall', '-Wextra', '-DBENCH_HOST'] + defs)

common = ['-O2', '-g', '-std=gnu11', '-Wall', '-Wextra', '-ffreestanding', '-fno-builtin', '-fno-pic',
  '-fno-stack-protector', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables',
  '-fno-tree-loop-distribute-patterns', '-nostdlib', '-nostartfiles'] + defs
ldcommon = ['-static', '-no-pie', '-Wl,--build-id=none', '-Wl,--no-warn-rwx-segments']
bench = files('bench.c')

# RISC-V, MIPS, ARM, PowerPC: the firmware self-tests' start code and link scripts.
rv_prefixes = get_option('riscv_cross') != '' ? [get_option('riscv_cross')] : [
  'riscv64-unknown-elf-', 'riscv64-linux-gnu-', 'riscv32-unknown-elf-', 'riscv64-elf-']
rv_gcc = find_program(rv_prefixes[0] + 'gcc', required: false)
foreach p : rv_prefixes
  if not rv_gcc.found()
    rv_gcc = find_program(p + 'gcc', required: false)
  endif
endforeach
mips_flags = ['-march=mips32r2', '-mabi=32', '-mno-abicalls', '-G0']
arm_flags = ['-march=armv7ve', '-mfloat-abi=soft']
ppc_flags = ['-m32', '-mbig-endian', '-mcpu=powerpc', '-mno-altivec', '-mno-vsx', '-msdata=none', '-G0',
  '-Wa,-mregnames']
mips_gcc = find_program(get_option('mips_cross') + 'gcc', required: false)
mipsel_gcc = find_program(get_option('mipsel_cross') + 'gcc', required: false)
arm_prefixes = get_option('arm_cross') != '' ? [get_option('arm_cross')] : [
  'arm-linux-gnueabi-', 'arm-none-eabi-']
arm_gcc = find_program(arm_prefixes[0] + 'gcc', required: false)
foreach p : arm_prefixes
  if not arm_gcc.found()
    arm_gcc = find_program(p + 'gcc', required: false)
  endif
endforeach
ppc_gcc = find_program(get_option('ppc_cross') + 'gcc', required: false)
# output: [compiler, flags, start code, link script]
guests = {
  'bench-rv32.elf': [rv_gcc, ['-march=rv32imac_zicsr_zifencei', '-mabi=ilp32', '-mcmodel=medany'] + common
    + ['-static', '-Wl,--build-id=none', '-Wl,--no-warn-rwx-segments'], fw / 'start.S', fw / 'link.ld'],
  'bench-mips.elf': [mips_gcc, mips_flags + ['-EB'] + common + ldcommon, fw + '-mips/start.S', fw + '-mips/link.ld'],
  'bench-mipsel.elf': [mipsel_gcc, mips_flags + ['-EL'] + common + ldcommon, fw + '-mips/start.S', fw + '-mips/link.ld'],
  'bench-arm.elf': [arm_gcc, ['-marm'] + arm_flags + common + ldcommon, fw + '-arm/start.S', fw + '-arm/link.ld'],
  'bench-thumb.elf': [arm_gcc, ['-mthumb'] + arm_flags + common + ldcommon, fw + '-arm/start.S', fw + '-arm/link.ld'],
  'bench-thumb-be.elf': [arm_gcc, ['-mthumb', '-mbig-endian', '-Wl,--be8'] + arm_flags + common + ldcommon,
    fw + '-arm/start.S', fw + '-arm/link.ld'],
  'bench-ppc.elf': [ppc_gcc, ppc_flags + common + ldcommon, fw + '-ppc/start.S', fw + '-ppc/link.ld'],
  # PIC32 (pic32-ref, with the PIC32 self-test's start code and this directory's map):
  # microMIPS on mm, MIPS32 with the FPU enabled on mz (MIPS16e on mx: below).
  'bench-pic32mm.elf': [mipsel_gcc, ['-march=m14k', '-mmicromips', '-msoft-float', '-DBENCH_PIC32', '-mabi=32',
    '-mno-abicalls', '-G0', '-EL'] + common + ldcommon + ['-fno-optimize-sibling-calls'],
    fw + '-pic32/start.S', here / 'pic32.ld'],
  'bench-pic32mz.elf': [mipsel_gcc, ['-march=mips32r2', '-mhard-float', '-mfp64', '-DBENCH_PIC32', '-mabi=32',
    '-mno-abicalls', '-G0', '-EL'] + common + ldcommon + ['-fno-optimize-sibling-calls'],
    fw + '-pic32/start.S', here / 'pic32.ld'],
}
foreach out, v : guests
  if v[0].found()
    custom_target(out,
      output: out, input: bench, depend_files: files(v[2], v[3]), build_by_default: true,
      command: [v[0], v[1], '-Wl,-T,' + v[3], v[2], '@INPUT@', '-o', '@OUTPUT@'])
  endif
endforeach

# PIC32 mx: the start code in MIPS32, the benchmark in MIPS16e.
if mipsel_gcc.found()
  pic32 = ['-DBENCH_PIC32', '-mabi=32', '-mno-abicalls', '-G0', '-EL'] + common + ldcommon + [
    '-Wl,-T,' + here / 'pic32.ld', '-fno-optimize-sibling-calls']
  mx_start = custom_target('pic32mx-start.o',
    output: 'pic32mx-start.o', input: fw + '-pic32/start.S',
    command: [mipsel_gcc, '-march=m4k', '-msoft-float', pic32, '-c', '@INPUT@', '-o', '@OUTPUT@'])
  custom_target('bench-pic32mx.elf',
    output: 'bench-pic32mx.elf', input: [mx_start, bench], depend_files: files('pic32.ld'),
    build_by_default: true,
    command: [mipsel_gcc, '-march=m4k', '-msoft-float', '-mips16', pic32, '@INPUT@', '-o', '@OUTPUT@'])
endif

# x86-64 (x86_64-ref): a flat image entered in real mode at 0.
x86_prefixes = get_option('x86_cross') != '' ? [get_option('x86_cross')] : [
  'x86_64-linux-gnu-', 'x86_64-elf-']
x86_prefix = x86_prefixes[0]
x86_gcc = find_program(x86_prefix + 'gcc', required: false)
foreach p : x86_prefixes
  if not x86_gcc.found()
    x86_gcc = find_program(p + 'gcc', required: false)
    x86_prefix = p
  endif
endforeach
if x86_gcc.found()
  x86_elf = custom_target('bench-x86.elf',
    output: 'bench-x86.elf', input: files('x86-start.S', 'bench.c'), depend_files: files('x86.ld'),
    command: [x86_gcc, '-m64', '-mno-red-zone', '-mgeneral-regs-only', common, ldcommon,
      '-Wl,-T,' + here / 'x86.ld', '@INPUT@', '-o', '@OUTPUT@'])
  custom_target('bench-x86.bin',
    output: 'bench-x86.bin', input: x86_elf, build_by_default: true,
    command: [find_program(x86_prefix + 'objcopy'), '-O', 'binary', '@INPUT@', '@OUTPUT@'])
endif

# AVR (avr-ref, model eplus): the data does not fit a part's SRAM, so the board runs with
# --ram 0xFE00 (SRAM up to the end of the 64 KiB data space) and the stack starts there.
avr_gcc = find_program(get_option('avr_cross') + 'gcc', required: false)
if avr_gcc.found()
  custom_target('bench-avr.elf',
    output: 'bench-avr.elf', input: bench, build_by_default: true,
    command: [avr_gcc, '-mmcu=atmega2560', '-O2', '-g', '-std=gnu11', '-Wall', '-Wextra', '-ffreestanding',
      '-fno-builtin', '-fno-tree-loop-distribute-patterns', defs, '-Wl,--defsym=__DATA_REGION_LENGTH__=0xFE00',
      '-Wl,--defsym=__stack=0xFFFF', '@INPUT@', '-o', '@OUTPUT@'])
endif

# Xtensa (xtensa-ref, models lx106, lx6, lx7) with the start code and link script of the
# C firmware; the LX106's libgcc lacks two routines the ESP8266's ROM provides.
xt_fw = fw + '-xtensa'
foreach m, prefix : {'lx106': get_option('xtensa_lx106_cross'), 'lx6': get_option('xtensa_lx6_cross'),
                     'lx7': get_option('xtensa_lx7_cross')}
  xt_gcc = find_program(prefix + 'gcc', required: false)
  if xt_gcc.found()
    custom_target('bench-' + m + '.elf',
      output: 'bench-' + m + '.elf',
      input: m == 'lx106' ? files(xt_fw / 'start.S', 'bench.c', xt_fw / 'lx106-libgcc.c') : files(xt_fw / 'start.S', 'bench.c'),
      depend_files: files(xt_fw / 'link.ld'), build_by_default: true,
      command: [xt_gcc, m == 'lx106' ? ['-mtext-section-literals'] : [], common, ldcommon, '-T', xt_fw / 'link.ld',
        '@INPUT@', '-lgcc', '-o', '@OUTPUT@'])
  endif
endforeach

# AVR32 (avr32-ref, models a and b), only when an AVR32 toolchain is found: its gcc 4.4
# lacks -fno-tree-loop-distribute-patterns, and wants the C locale.
avr32_gcc = find_program(get_option('avr32_cross') + 'gcc', required: false)
if avr32_gcc.found()
  avr32_flags = ['-O2', '-g', '-std=gnu99', '-Wall', '-Wextra', '-ffreestanding', '-fno-builtin', '-mno-pic',
    '-fno-stack-protector', '-nostdlib', '-nostartfiles', '-static'] + defs
  foreach out, v : {'bench-avr32a.elf': ['uc3a0512', 'link-a.ld'], 'bench-avr32b.elf': ['ap7000', 'link-b.ld']}
    custom_target(out,
      output: out, input: files(fw + '-avr32/start.S', 'bench.c'), depend_files: files(fw + '-avr32' / v[1]),
      build_by_default: true, env: {'LC_ALL': 'C'},
      command: [avr32_gcc, '-mpart=' + v[0], avr32_flags, '-Wl,-T,' + fw + '-avr32' / v[1], '@INPUT@', '-lgcc',
        '-o', '@OUTPUT@'])
  endforeach
endif

# The 8-bit guests SDCC builds, only when it is found.
sdcc = find_program(get_option('sdcc'), required: false)
makebin = find_program('makebin', required: false)
if sdcc.found() and makebin.found()
  sdcc_defs = ' '.join(defs)
  # 8051 (mcs51-ref, model 8052): SDCC's large reentrant model (data in external RAM, locals
  # on the internal-RAM stack, whose 128 directly addressed bytes the non-reentrant model's
  # spill slots overflow). SDCC ships no 8051 build of its 64-bit multiply, so it is
  # compiled here from SDCC's own library source.
  sdcc_libsrc = get_option('sdcc_libsrc')
  if sdcc_libsrc == ''
    sdcc_libsrc = fs.parent(fs.parent(sdcc.full_path())) / 'share/sdcc/lib/src'
  endif
  custom_target('bench-mcs51.bin',
    output: 'bench-mcs51.bin', input: bench, build_by_default: true,
    command: [sh, '-ec', '''
      sdcc=$1 makebin=$2 src=$3 work=$4 out=$5 libsrc=$6; shift 6
      flags="-mmcs51 --model-large --stack-auto --std-c11 --iram-size 256 --xram-size 0xff00 --code-size 0x10000 $*"
      mkdir -p "$work"
      "$sdcc" $flags -c "$libsrc/_mullonglong.c" -o "$work/mullonglong.rel"
      "$sdcc" $flags -c "$src" -o "$work/bench.rel"
      "$sdcc" $flags "$work/bench.rel" "$work/mullonglong.rel" -o "$work/bench.ihx"
      "$makebin" -p "$work/bench.ihx" "$out"
      ''', 'sh', sdcc, makebin, '@INPUT@', '@PRIVATE_DIR@', '@OUTPUT@', sdcc_libsrc, defs])

  # 6502 (6502-ref, models nmos and 65c02): code and constants from the ROM at 0x8000
  # (GSINIT is the first code area SDCC's start code links), data in RAM from 0x0200, the
  # image the ROM's 32 KiB.
  foreach out, port : {'bench-6502.bin': '-mmos6502', 'bench-65c02.bin': '-mmos65c02'}
    custom_target(out,
      output: out, input: bench, depend_files: files(fw + '-6502/ihx2bin.py'), build_by_default: true,
      command: [sh, '-ec', '''
        sdcc=$1 port=$2 src=$3 work=$4 out=$5 py=$6 ihx2bin=$7; shift 7
        mkdir -p "$work"
        "$sdcc" "$port" --std-c11 --code-loc 0x8000 -Wl-bGSINIT=0x8000 "$@" "$src" -o "$work/bench.ihx"
        "$py" "$ihx2bin" "$work/bench.ihx" "$out"
        ''', 'sh', sdcc, port, '@INPUT@', '@PRIVATE_DIR@', '@OUTPUT@', python.full_path(),
        fw + '-6502/ihx2bin.py', defs])
  endforeach

  # Z80 (z80-ref): SDCC's z80 port with the Z80 self-test's start code (crt0.s), which
  # hands main's value to the test finisher.
  sdasz80 = find_program(get_option('sdasz80'), required: false)
  if sdasz80.found()
    custom_target('bench-z80.bin',
      output: 'bench-z80.bin', input: [bench, files(fw + '-z80/crt0.s')], build_by_default: true,
      command: [sh, '-ec', '''
        sdas=$1 sdcc=$2 makebin=$3 src=$4 crt0=$5 work=$6 out=$7; shift 7
        flags="-mz80 --std-c11 --no-std-crt0 --code-loc 0x0100 --data-loc 0x3000 $*"
        mkdir -p "$work"
        "$sdas" -o "$work/crt0.rel" "$crt0"
        "$sdcc" $flags -c "$src" -o "$work/bench.rel"
        "$sdcc" $flags "$work/crt0.rel" "$work/bench.rel" -o "$work/bench.ihx"
        "$makebin" -p "$work/bench.ihx" "$out"
        ''', 'sh', sdasz80, sdcc, makebin, '@INPUT0@', '@INPUT1@', '@PRIVATE_DIR@', '@OUTPUT@', defs])
  endif
endif
