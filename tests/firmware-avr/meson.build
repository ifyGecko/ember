# The 8-bit AVR self-test, once per avr-ref model (boards/avr_ref.c): the part named gives
# the compiler the model's memory layout; the devices are the board's own.
#
#   model  part          device base  flag vectors  program memory above 64 KiB
#   e      atmega328p    0x00E0       medium        -
#   eplus  atmega2560    0x00E0       full          .fartable at 0x30000
#   xm     atxmega128a1u 0x0FE0       full          .fartable at 0x18000
#   xt     atmega4809    0x0FE0       medium        -
#   rc     attiny40      0x1000       small         -

avr_gcc = find_program(get_option('avr_cross') + 'gcc', required: false)
have_avr = avr_gcc.found()
fw_avr = {}
avr_models = ['e', 'eplus', 'xm', 'xt', 'rc']

if have_avr
  avr_objdump = find_program(get_option('avr_cross') + 'objdump')
  avr_flags = ['-Os', '-g', '-std=gnu11', '-Wall', '-Wextra', '-mrelax', '-ffunction-sections',
    '-Wl,--gc-sections']
  # model: [part, device base, flag vectors, extra flags]
  foreach m, v : {
    'e': ['atmega328p', '0x00E0', 'medium', []],
    'eplus': ['atmega2560', '0x00E0', 'full', ['-DFAR_TABLE', '-Wl,--section-start=.fartable=0x30000']],
    'xm': ['atxmega128a1u', '0x0FE0', 'full',
      ['-DFAR_TABLE', '-DMODEL_XM', '-Wl,--section-start=.fartable=0x18000']],
    'xt': ['atmega4809', '0x0FE0', 'medium', []],
    'rc': ['attiny40', '0x1000', 'small', []],
  }
    # The model's vectors.h goes into a directory of its own, on the include path.
    vectors = custom_target('avr-vectors-' + m,
      output: 'vectors-' + m,
      input: 'gen_vectors.py',
      command: [sh, '-ec', 'd=$1; shift; mkdir -p "$d"; "$@"; touch "$d"', 'sh', '@OUTPUT@',
        python, '@INPUT@', '@OUTPUT@/vectors.h', v[2]])
    fw_avr += {'selftest-' + m: custom_target('avr-selftest-' + m,
      output: 'selftest-' + m + '.elf',
      input: 'selftest.c',
      depends: vectors,
      command: [avr_gcc, '-mmcu=' + v[0], avr_flags, '-I' + meson.current_build_dir() / 'vectors-' + m,
        '-DDEV_BASE=' + v[1], '-DMODEL_NAME="' + m + '"', v[3], '@INPUT@', '-o', '@OUTPUT@'])}
    firmware_all += custom_target('avr-selftest-' + m + '-dis',
      output: 'selftest-' + m + '.dis', input: fw_avr['selftest-' + m], capture: true,
      command: [avr_objdump, '-d', '@INPUT@'])
  endforeach
  fw_avr += {'fuzztarget': custom_target('avr-fuzztarget',
    output: 'fuzztarget.elf',
    input: 'fuzztarget.c',
    depend_files: files('../fuzz/fuzztarget.h'),
    command: [avr_gcc, '-mmcu=atmega2560', avr_flags, '@INPUT@', '-o', '@OUTPUT@'])}
  foreach name, t : fw_avr
    firmware_all += t
  endforeach
endif
