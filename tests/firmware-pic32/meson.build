# The PIC32 self-test, once per pic32-ref model and instruction set, and the fuzz target:
#
#   image              board options                  code
#   selftest-mz        -o model=mz                    MIPS32, the FPU with FR = 1, the DSP ASE
#   selftest-mz-fr0    -o model=mz                    MIPS32, the FPU with FR = 0, the DSP ASE
#   selftest-mx        -o model=mx                    MIPS16e (handlers MIPS32)
#   selftest-mm        -o model=mm                    microMIPS
#   selftest-mz-um     -o model=mz -o bootisa=micromips  microMIPS, the FPU, the DSP ASE (calls MIPS32 code too)
#   fuzztarget-mz, fuzztarget-mx                      the fuzz target (tests/fuzz): MIPS32, MIPS16e

pic32_cross = get_option('mipsel_cross')
pic32_gcc = find_program(pic32_cross + 'gcc', required: false)
have_pic32 = pic32_gcc.found()
fw_pic32 = {}

if have_pic32
  pic32_dir = meson.current_source_dir()
  pic32_out = meson.current_build_dir()
  pic32_objdump = find_program(pic32_cross + 'objdump')
  pic32_cflags = ['-O2', '-g', '-std=gnu11', '-Wall', '-Wextra', '-ffreestanding', '-fno-builtin',
    '-fno-pic', '-mno-abicalls', '-fno-stack-protector', '-nostdlib', '-nostartfiles', '-G0', '-EL',
    '-mabi=32', '-fno-optimize-sibling-calls']
  pic32_ldflags = ['-static', '-no-pie', '-Wl,-T,' + pic32_dir / 'link.ld', '-Wl,--build-id=none']
  pic32_deps = files('link.ld', 'fptest.c', 'dsptest.c', '../fuzz/fuzztarget.h')

  pic32_fpvectors = custom_target('pic32-fpvectors',
    output: 'fpvectors.h', input: 'gen_fpvectors.py',
    command: [python, '-I', '@INPUT@', '@OUTPUT@'])
  pic32_dspvectors = custom_target('pic32-dspvectors',
    output: 'dspvectors.h', input: 'gen_dspvectors.py',
    env: {'CROSS': pic32_cross},
    command: [python, '-I', '@INPUT@', '@OUTPUT@'])
  pic32_vectors = [pic32_fpvectors, pic32_dspvectors]

  # Images built in one step: [sources, compiler flags, generated headers].
  pic32_selftest = files('start.S', 'probe.S', 'selftest.c')
  foreach name, v : {
    'selftest-mz': [pic32_selftest, ['-march=mips32r2', '-mhard-float', '-mfp64', '-mdspr2',
      '-DMODEL_MZ', '-DMODEL_NAME="mz"', '-DBOOT_UMIPS=0'], pic32_vectors],
    'selftest-mz-fr0': [pic32_selftest, ['-march=mips32r2', '-mhard-float', '-mfp32', '-mdspr2',
      '-DMODEL_MZ', '-DMODEL_NAME="mz FR=0"', '-DBOOT_UMIPS=0'], pic32_vectors],
    'selftest-mm': [pic32_selftest, ['-march=m14k', '-mmicromips', '-msoft-float',
      '-DMODEL_MM', '-DMODEL_NAME="mm"', '-DBOOT_UMIPS=1'], []],
    'selftest-mz-um': [pic32_selftest, ['-march=mips32r2', '-mmicromips', '-mhard-float', '-mfp64',
      '-mdspr2', '-DMODEL_MZ', '-DMODEL_NAME="mz microMIPS"', '-DBOOT_UMIPS=1'], pic32_vectors],
    'fuzztarget-mz': [files('start.S', 'fuzztarget.c'), ['-march=mips32r2', '-msoft-float'], []],
  }
    fw_pic32 += {name: custom_target('pic32-' + name,
      output: name + '.elf',
      input: v[0],
      depends: v[2],
      depend_files: pic32_deps,
      command: [pic32_gcc, v[1], pic32_cflags, pic32_ldflags, '-I' + pic32_out, '@INPUT@', '-o', '@OUTPUT@'])}
  endforeach

  # MIPS16e on mx: the C code and the probe in MIPS16e, the vectors (start.S) in MIPS32,
  # so each source is compiled on its own.
  pic32_m4k = ['-march=m4k', '-msoft-float']
  pic32_mx_objs = {}
  foreach obj, v : {
    'start-mx.o': ['start.S', []],
    'probe-mx.o': ['probe.S', ['-mips16']],
    'selftest-mx.o': ['selftest.c', ['-mips16', '-DMODEL_MX', '-DMODEL_NAME="mx"', '-DBOOT_UMIPS=0']],
    'fz-mx.o': ['fuzztarget.c', ['-mips16']],
  }
    pic32_mx_objs += {obj: custom_target('pic32-' + obj,
      output: obj, input: v[0], depend_files: pic32_deps,
      command: [pic32_gcc, pic32_m4k, v[1], pic32_cflags, '-c', '@INPUT@', '-o', '@OUTPUT@'])}
  endforeach
  foreach name, objs : {
    'selftest-mx': ['start-mx.o', 'probe-mx.o', 'selftest-mx.o'],
    'fuzztarget-mx': ['start-mx.o', 'fz-mx.o'],
  }
    inputs = []
    foreach o : objs
      inputs += pic32_mx_objs[o]
    endforeach
    fw_pic32 += {name: custom_target('pic32-' + name,
      output: name + '.elf', input: inputs, depend_files: files('link.ld'),
      command: [pic32_gcc, pic32_m4k, pic32_cflags, pic32_ldflags, '@INPUT@', '-o', '@OUTPUT@'])}
  endforeach

  foreach name : ['selftest-mz', 'selftest-mx', 'selftest-mm', 'selftest-mz-um']
    firmware_all += custom_target('pic32-' + name + '-dis',
      output: name + '.dis', input: fw_pic32[name], capture: true,
      command: [pic32_objdump, '-d', '@INPUT@'])
  endforeach
  foreach name, t : fw_pic32
    firmware_all += t
  endforeach
endif
