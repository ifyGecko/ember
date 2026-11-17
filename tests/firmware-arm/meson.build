# The bare-metal ARMv7-A self-test (as ARM and as Thumb-2 code), the fuzz target and the
# multiprocessor self-test, little-endian and big-endian (BE8).

arm_gcc = find_program(get_option('arm_cross') + 'gcc', required: false)
have_arm = arm_gcc.found()
fw_arm = {}

if have_arm
  arm_dir = meson.current_source_dir()
  arm_objcopy = find_program(get_option('arm_cross') + 'objcopy')
  arm_objdump = find_program(get_option('arm_cross') + 'objdump')
  arm_flags = ['-march=armv7ve', '-mfloat-abi=soft', '-O2', '-g', '-std=gnu11', '-Wall', '-Wextra',
    '-ffreestanding', '-fno-builtin', '-fno-pic', '-fno-stack-protector', '-fno-unwind-tables',
    '-fno-asynchronous-unwind-tables', '-nostdlib', '-nostartfiles',
    '-static', '-no-pie', '-Wl,-T,' + arm_dir / 'link.ld', '-Wl,--build-id=none',
    '-Wl,--no-warn-rwx-segments']
  arm_be = ['-mbig-endian', '-Wl,--be8']   # big-endian data, little-endian code (BE8)
  # image: [source, start code, mode flags]; the fuzz target is Thumb-2 code, so the fuzz
  # harness runs on Thumb pcs (bit 0 set).
  foreach name, v : {
    'selftest-arm': ['selftest', 'start.S', ['-marm']],
    'selftest-thumb': ['selftest', 'start.S', ['-mthumb']],
    'selftest-arm-be': ['selftest', 'start.S', ['-marm'] + arm_be],
    'selftest-thumb-be': ['selftest', 'start.S', ['-mthumb'] + arm_be],
    'fuzztarget': ['fuzztarget', 'start.S', ['-mthumb']],
    'fuzztarget-be': ['fuzztarget', 'start.S', ['-mthumb'] + arm_be],
    'smptest-arm': ['smptest', 'smpstart.S', ['-marm']],
    'smptest-thumb-be': ['smptest', 'smpstart.S', ['-mthumb'] + arm_be],
  }
    fw_arm += {name: custom_target('arm-' + name,
      output: name + '.elf',
      input: files(v[1], v[0] + '.c'),
      depend_files: files('link.ld', '../fuzz/fuzztarget.h', '../smp/smp_test.h'),
      command: [arm_gcc, v[2], arm_flags, '@INPUT@', '-o', '@OUTPUT@'])}
  endforeach
  foreach name : ['selftest-arm', 'selftest-thumb']
    firmware_all += custom_target('arm-' + name + '-dis',
      output: name + '.dis', input: fw_arm[name], capture: true,
      command: [arm_objdump, '-d', '@INPUT@'])
  endforeach
  # The same images without their ELF wrapper, for the raw-image path (--raw-base, --entry).
  foreach name : ['fuzztarget', 'fuzztarget-be']
    fw_arm += {name + '.bin': custom_target('arm-' + name + '-bin',
      output: name + '.bin', input: fw_arm[name],
      command: [arm_objcopy, '-O', 'binary', '@INPUT@', '@OUTPUT@'])}
  endforeach
  foreach name, t : fw_arm
    firmware_all += t
  endforeach
endif
