# The Xtensa self-test, once per xtensa-ref model (boards/xtensa_ref.c) with that core's
# GNU toolchain: selftest-lx106.elf (xtensa-lx106-elf, Debian's gcc-xtensa-lx106),
# selftest-lx6.elf and selftest-lx7.elf (the ESP32's xtensa-esp32-elf and the ESP32-S3's
# xtensa-esp32s3-elf, from Espressif's xtensa-esp-elf release); the big-endian builds
# (-be.bin, raw images at 0x40000000) are the little-endian ones laid out the big-endian
# way by ../isa-xtensa/le2be.py; and the fuzz target (fuzztarget-MODEL.elf). A model whose
# compiler is missing is left out.

xtensa_cross = {
  'lx106': get_option('xtensa_lx106_cross'),
  'lx6': get_option('xtensa_lx6_cross'),
  'lx7': get_option('xtensa_lx7_cross'),
}
xtensa_models = []      # the models with a toolchain
xtensa_objdump = {}     # model -> its objdump
fw_xtensa = {}

xtensa_dir = meson.current_source_dir()
xtensa_ld = ['-nostdlib', '-nostartfiles', '-Wl,--build-id=none', '-Wl,--no-warn-rwx-segments']
xtensa_cflags = ['-O2', '-g', '-std=gnu11', '-Wall', '-Wextra', '-ffreestanding', '-fno-builtin',
  '-fno-pic', '-fno-stack-protector', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables']
xtensa_vectors = custom_target('xtensa-vectors',
  output: 'vectors.S', input: 'gen_vectors.py',
  command: [python, '-I', '-B', '@INPUT@', '@OUTPUT@'])

foreach m, prefix : xtensa_cross
  xt_gcc = find_program(prefix + 'gcc', required: false)
  if xt_gcc.found()
    xtensa_models += m
    xt_objdump = find_program(prefix + 'objdump')
    xtensa_objdump += {m: xt_objdump}
    selftest_cmd = [xt_gcc, '-DXT_' + m.to_upper(), '-I' + xtensa_dir, '-I' + meson.current_build_dir(),
      xtensa_ld, '-T', xtensa_dir / 'selftest.ld']
    selftest_deps = files('model.h', 'window.inc', 'selftest.ld')
    fw_xtensa += {'selftest-' + m: custom_target('xtensa-selftest-' + m,
      output: 'selftest-' + m + '.elf', input: 'selftest.S',
      depends: xtensa_vectors, depend_files: selftest_deps,
      command: [selftest_cmd, '@INPUT@', '-o', '@OUTPUT@'])}
    be_elf = custom_target('xtensa-selftest-' + m + '-be-elf',
      output: 'selftest-' + m + '-be.elf', input: 'selftest.S',
      depends: xtensa_vectors, depend_files: selftest_deps,
      command: [selftest_cmd, '-DSELFTEST_BE', '@INPUT@', '-o', '@OUTPUT@'])
    fw_xtensa += {'selftest-' + m + '-be.bin': custom_target('xtensa-selftest-' + m + '-be',
      output: 'selftest-' + m + '-be.bin', input: be_elf,
      depend_files: files('../isa-xtensa/le2be.py'),
      command: [python, '-I', '-B', files('../isa-xtensa/le2be.py'), xt_objdump, '@INPUT@', '@OUTPUT@'])}
    # The C firmware; the LX106's libgcc lacks two routines the ESP8266's ROM provides.
    fw_xtensa += {'fuzztarget-' + m: custom_target('xtensa-fuzztarget-' + m,
      output: 'fuzztarget-' + m + '.elf',
      input: m == 'lx106' ? files('start.S', 'fuzztarget.c', 'lx106-libgcc.c') : files('start.S', 'fuzztarget.c'),
      depend_files: files('window.inc', 'link.ld', '../fuzz/fuzztarget.h'),
      command: [xt_gcc, xtensa_cflags, m == 'lx106' ? ['-mtext-section-literals'] : [], xtensa_ld,
        '-T', xtensa_dir / 'link.ld', '@INPUT@', '-lgcc', '-o', '@OUTPUT@'])}
  endif
endforeach
have_xtensa = xtensa_models.length() > 0
foreach name, t : fw_xtensa
  firmware_all += t
endforeach
