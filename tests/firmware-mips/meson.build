# The bare-metal MIPS32 self-test, fuzz target and multiprocessor self-test, one ELF per
# endianness.

mips_be_gcc = find_program(get_option('mips_cross') + 'gcc', required: false)
mips_el_gcc = find_program(get_option('mipsel_cross') + 'gcc', required: false)
have_mips = mips_be_gcc.found() and mips_el_gcc.found()
fw_mips = {}

if have_mips
  mips_dir = meson.current_source_dir()
  mips_flags = ['-march=mips32r2', '-mabi=32',
    '-O2', '-g', '-std=gnu11', '-Wall', '-Wextra', '-ffreestanding', '-fno-builtin', '-fno-pic',
    '-mno-abicalls', '-fno-stack-protector', '-nostdlib', '-nostartfiles', '-G0',
    '-static', '-no-pie', '-Wl,-T,' + mips_dir / 'link.ld', '-Wl,--build-id=none']
  foreach end, tools : {
    'be': [mips_be_gcc, '-EB', get_option('mips_cross')],
    'el': [mips_el_gcc, '-EL', get_option('mipsel_cross')],
  }
    foreach name, start : {'selftest': 'start.S', 'fuzztarget': 'start.S', 'smptest': 'smpstart.S'}
      fw_mips += {name + '-' + end: custom_target('mips-' + name + '-' + end,
        output: name + '-' + end + '.elf',
        input: files(start, name + '.c'),
        depend_files: files('link.ld', '../fuzz/fuzztarget.h', '../smp/smp_test.h'),
        command: [tools[0], tools[1], mips_flags, '@INPUT@', '-o', '@OUTPUT@'])}
    endforeach
    firmware_all += custom_target('mips-selftest-' + end + '-dis',
      output: 'selftest-' + end + '.dis', input: fw_mips['selftest-' + end], capture: true,
      command: [find_program(tools[2] + 'objdump'), '-d', '@INPUT@'])
  endforeach
  # The same image without its ELF wrapper, for the raw-image path (--raw-base, --entry).
  fw_mips += {'fuzztarget-be.bin': custom_target('mips-fuzztarget-be-bin',
    output: 'fuzztarget-be.bin', input: fw_mips['fuzztarget-be'],
    command: [find_program(get_option('mips_cross') + 'objcopy'), '-O', 'binary', '@INPUT@', '@OUTPUT@'])}
  foreach name, t : fw_mips
    firmware_all += t
  endforeach
endif
