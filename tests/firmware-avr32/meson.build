# The bare-metal AVR32 self-test, once for AVR32A (UC3 class, -mpart=uc3a0512) and once for
# AVR32B (AP7 class, -mpart=ap7000), and the fuzz target for AVR32A. The toolchain is
# avr32-gcc 4.x of the AVR32 GNU toolchain (-Davr32_cross=PREFIX), which few hosts have.

avr32_gcc = find_program(get_option('avr32_cross') + 'gcc', required: false)
have_avr32 = avr32_gcc.found()
fw_avr32 = {}

if have_avr32
  avr32_dir = meson.current_source_dir()
  avr32_objdump = find_program(get_option('avr32_cross') + 'objdump')
  avr32_env = {'LC_ALL': 'C'}   # the 4.x toolchain asserts in non-C locales
  avr32_flags = ['-O2', '-g', '-std=gnu99', '-Wall', '-Wextra', '-ffreestanding', '-fno-builtin',
    '-mno-pic', '-fno-stack-protector', '-nostdlib', '-nostartfiles', '-static', '-Wl,--build-id=none']
  avr32_a = ['-mpart=uc3a0512', '-Wl,-T,' + avr32_dir / 'link-a.ld']
  avr32_b = ['-mpart=ap7000', '-DAVR32B', '-Wl,-T,' + avr32_dir / 'link-b.ld']
  # image: [source, part flags, libraries]
  foreach name, v : {
    'selftest-a': ['selftest.c', avr32_a, ['-lgcc']],
    'selftest-b': ['selftest.c', avr32_b, ['-lgcc']],
    'fuzztarget-a': ['fuzztarget.c', avr32_a, []],
  }
    fw_avr32 += {name: custom_target('avr32-' + name,
      output: name + '.elf',
      input: files('start.S', v[0]),
      depend_files: files('link-a.ld', 'link-b.ld', '../fuzz/fuzztarget.h'),
      env: avr32_env,
      command: [avr32_gcc, v[1], avr32_flags, '@INPUT@', v[2], '-o', '@OUTPUT@'])}
  endforeach
  foreach name : ['selftest-a', 'selftest-b']
    firmware_all += custom_target('avr32-' + name + '-dis',
      output: name + '.dis', input: fw_avr32[name], capture: true, env: avr32_env,
      command: [avr32_objdump, '-d', '@INPUT@'])
  endforeach
  foreach name, t : fw_avr32
    firmware_all += t
  endforeach
endif
