# The bare-metal RV32IMAC images: the self-test, the network test, the fuzz targets, the
# multiprocessor self-test and the scripted-console test.
# Any riscv64 GCC works (rv64 toolchains build rv32 freestanding code fine).

rv_prefixes = get_option('riscv_cross') != '' ? [get_option('riscv_cross')] : [
  'riscv64-unknown-elf-', 'riscv64-linux-gnu-', 'riscv32-unknown-elf-']
rv_prefix = rv_prefixes[0]
rv_gcc = find_program(rv_prefix + 'gcc', required: false)
foreach p : rv_prefixes
  if not rv_gcc.found()
    rv_gcc = find_program(p + 'gcc', required: false)
    rv_prefix = p
  endif
endforeach
have_rv = rv_gcc.found()
fw_rv = {}

if have_rv
  rv_objdump = find_program(rv_prefix + 'objdump')
  rv_dir = meson.current_source_dir()
  rv_flags = ['-march=rv32imac_zicsr_zifencei', '-mabi=ilp32', '-mcmodel=medany',
    '-O2', '-g', '-std=gnu11', '-Wall', '-Wextra', '-ffreestanding', '-fno-builtin',
    '-fno-pic', '-fno-stack-protector', '-nostdlib', '-nostartfiles',
    '-static', '-T', rv_dir / 'link.ld', '-Wl,--build-id=none']
  foreach name, start : {
    'selftest': 'start.S', 'nettest': 'start.S', 'fuzztarget': 'start.S',
    'irqtarget': 'start.S', 'heaptarget': 'start.S', 'consoletest': 'start.S',
    'smptest': 'smpstart.S', 'linktest': 'start.S',
  }
    fw_rv += {name: custom_target('rv32-' + name,
      output: name + '.elf',
      input: files(start, name + '.c'),
      depend_files: files('link.ld', '../fuzz/fuzztarget.h', '../smp/smp_test.h'),
      command: [rv_gcc, rv_flags, '@INPUT@', '-o', '@OUTPUT@'])}
  endforeach
  firmware_all += custom_target('rv32-selftest-dis',
    output: 'selftest.dis', input: fw_rv['selftest'], capture: true,
    command: [rv_objdump, '-d', '@INPUT@'])
  foreach name, t : fw_rv
    firmware_all += t
  endforeach
endif
