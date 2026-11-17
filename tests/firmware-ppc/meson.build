# The bare-metal 32-bit PowerPC self-test (classic and Book E), the fuzz target and the
# multiprocessor self-test. Any GCC for PowerPC works: a 32-bit one
# (-Dppc_cross=powerpc-linux-gnu-) or, by default, the 64-bit little-endian one told to
# make 32-bit big-endian code.

ppc_gcc = find_program(get_option('ppc_cross') + 'gcc', required: false)
have_ppc = ppc_gcc.found()
fw_ppc = {}

if have_ppc
  ppc_dir = meson.current_source_dir()
  ppc_objdump = find_program(get_option('ppc_cross') + 'objdump')
  ppc_flags = ['-m32', '-mbig-endian', '-mcpu=powerpc', '-mno-altivec', '-mno-vsx', '-msdata=none', '-G0',
    '-O2', '-g', '-std=gnu11', '-Wall', '-Wextra', '-ffreestanding', '-fno-builtin', '-fno-pic',
    '-fno-stack-protector', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables',
    '-nostdlib', '-nostartfiles', '-Wa,-mregnames',
    '-static', '-no-pie', '-Wl,--build-id=none', '-Wl,--no-warn-rwx-segments']
  # image: [source, start code, link script, extra flags]; selftest-booke is the same
  # self-test for the Book E model (ppc-ref -o model=booke).
  foreach name, v : {
    'selftest': ['selftest', 'start.S', 'link.ld', []],
    'selftest-booke': ['selftest', 'start-booke.S', 'link-booke.ld', ['-DPPC_BOOKE', '-Wa,-many']],
    'fuzztarget': ['fuzztarget', 'start.S', 'link.ld', []],
    'smptest': ['smptest', 'smpstart.S', 'link.ld', []],
  }
    fw_ppc += {name: custom_target('ppc-' + name,
      output: name + '.elf',
      input: files(v[1], v[0] + '.c'),
      depend_files: files(v[2], '../fuzz/fuzztarget.h', '../smp/smp_test.h'),
      command: [ppc_gcc, ppc_flags, v[3], '-Wl,-T,' + ppc_dir / v[2], '@INPUT@', '-o', '@OUTPUT@'])}
  endforeach
  foreach name : ['selftest', 'selftest-booke']
    firmware_all += custom_target('ppc-' + name + '-dis',
      output: name + '.dis', input: fw_ppc[name], capture: true,
      command: [ppc_objdump, '-d', '@INPUT@'])
  endforeach
  # The same image without its ELF wrapper, for the raw-image path (--raw-base, --entry).
  fw_ppc += {'fuzztarget.bin': custom_target('ppc-fuzztarget-bin',
    output: 'fuzztarget.bin', input: fw_ppc['fuzztarget'],
    command: [find_program(get_option('ppc_cross') + 'objcopy'), '-O', 'binary', '@INPUT@', '@OUTPUT@'])}
  foreach name, t : fw_ppc
    firmware_all += t
  endforeach
endif
