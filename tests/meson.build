# ember's tests: `meson test -C build` runs all of them, `--suite NAME` one group:
#
#   unit          the unit tests (tests/unit: one test_<arch>.c per ISA among them), the
#                 JIT differential test, the fuzz tools' self-tests
#   firmware      the RISC-V firmware self-tests, interpreter and JIT, and scripted consoles
#   mips arm x86 ppc avr avr32 pic mcs51 pic32 6502 z80 xtensa
#                 each ISA's firmware self-test on its reference board, every model,
#                 interpreter and JIT
#   example       the out-of-tree example board (examples/external-board)
#   snapshot      runs split by --save-snapshot / --load-snapshot equal the uninterrupted run
#   cov           block coverage maps: the same from every backend
#   fuzz          the fuzz harness on planted-bug firmware, the memory oracles
#   fuzz-lists    site lists, boundary files, the coverage region, the fuzz brain, the
#                 network frontend, a parallel campaign
#   gdb           the gdb stub over the remote protocol
#   runner        uam-run's debugging options, traces and --on rules
#   link          links between emulations: a hub and two or three uam-test-run processes
#                 in the free, loose and exact modes (tests/link/link-check.sh)
#   bench         the cross-ISA benchmark's correctness run (tools/xbench.sh --check)
#   fuzz-throughput  what a fuzz execution costs, against tests/fuzz/throughput.ref
#
# Targets outside the test run (`meson compile -C build NAME`):
#   firmware                 every test firmware image
#   net-check                boot the network test on rv32-test against a real tap and tun
#                            interface (rootless: in a user+network namespace)
#   jitdiff-soak             the JIT differential test at length (-Djitdiff_blocks=N)
#   fuzz-throughput-record   rewrite tests/fuzz/throughput.ref (on a quiet machine)
#   xbench                   time the cross-ISA benchmark (-Dxbench_args="--per-kernel")
#
# The firmware is built with each guest's toolchain (meson_options.txt); a toolchain that is
# not found leaves its images and the tests that run them out, and the configure summary
# says so.

python = find_program('python3')
sh = find_program('sh')
cat = find_program('cat')
sdcc = find_program(get_option('sdcc'), required: false)
makebin = find_program('makebin', required: false)

src_root = meson.project_source_root()
build_root = meson.project_build_root()
# Every check runs from the source tree, as its scripts expect, and gets time to finish
# under a loaded, parallel test run.
std = {'workdir': src_root, 'timeout': 1800}
# Checks that measure time or run a campaign against deadlines run on their own.
alone = {'workdir': src_root, 'timeout': 3600, 'is_parallel': false}

firmware_all = []
subdir('firmware')
subdir('firmware-mips')
subdir('firmware-arm')
subdir('firmware-x86')
subdir('firmware-ppc')
subdir('firmware-pic32')
subdir('firmware-avr')
subdir('firmware-avr32')
subdir('firmware-pic')
subdir('firmware-mcs51')
subdir('firmware-6502')
subdir('firmware-z80')
subdir('firmware-xtensa')
alias_target('firmware', firmware_all)

summary({
  'RISC-V': have_rv,
  'MIPS': have_mips,
  'ARM': have_arm,
  'x86 (nasm)': have_x86,
  'PowerPC': have_ppc,
  'PIC32': have_pic32,
  'AVR': have_avr,
  'AVR32': have_avr32,
  'PIC (gpasm)': have_pic,
  '8051 (as31)': have_mcs51,
  '6502 (sdas6500)': have_6502,
  'Z80 (sdasz80)': have_z80,
  'SDCC fuzz targets': sdcc.found(),
  'Xtensa models': xtensa_models.length() > 0 ? ' '.join(xtensa_models) : 'none',
}, section: 'Test firmware (a missing toolchain leaves its tests out)', bool_yn: true)

# uam-run plus the boards only ember's tests use (tests/boards/): the reference boards with
# the generic components they leave out (the 0xB2 SMI port, storage, VGA, printer, virtio-net).
uam_test_run = executable('uam-test-run',
  'boards/uam-test-run.c', 'boards/test_boards.c', 'boards/heap_hooks.c',
  dependencies: uam_dep)

# ---------------------------------------------------------------------------------------
# unit
# ---------------------------------------------------------------------------------------
foreach n : ['ir', 'core', 'host', 'snapshot', 'jit', 'spin', 'codestore', 'mmio', 'cov', 'fuzz',
             'mutate', 'oracles', 'smp', 'ppc', 'avr', 'pic', 'mcs51', 'm6502', 'z80', 'xtensa',
             'arm', 'riscv', 'x86', 'mips', 'avr32', 'f80', 'rules', 'link']
  test(n, executable('test_' + n, 'unit/test_' + n + '.c', dependencies: uam_dep),
    suite: 'unit', kwargs: std)
endforeach

# The Xtensa FPU test's vectors are computed with exact arithmetic when it is built.
xtensa_fpu_vectors = custom_target('xtensa_fpu_vectors.h',
  output: 'xtensa_fpu_vectors.h',
  input: 'isa-xtensa/gen-fpu-vectors.py',
  depend_files: files('isa-xtensa/opcodes.py'),
  capture: true,
  command: [python, '-I', '-B', '@INPUT@'])
test('xtensa_fpu',
  executable('test_xtensa_fpu', 'unit/test_xtensa_fpu.c', xtensa_fpu_vectors,
    include_directories: include_directories('../isa/xtensa'),
    dependencies: uam_dep),
  suite: 'unit', kwargs: std)

# The JIT differential test: default length, and two more seeds on the baseline code
# generator and with register allocation forced.
test_jitdiff = executable('test_jitdiff', 'unit/test_jitdiff.c', dependencies: uam_dep)
test('jitdiff', test_jitdiff, suite: 'unit', kwargs: std)
test('jitdiff-baseline', test_jitdiff, args: ['1000', '2'], env: {'UAM_JIT_BASELINE': '1'},
  suite: 'unit', kwargs: std)
test('jitdiff-regs', test_jitdiff, args: ['1000', '3'], env: {'UAM_JIT_REGS': '1'},
  suite: 'unit', kwargs: std)

# test_cov_formats: the site-list and boundary readers, the coverage region's ownership and
# the coverage formula's golden maps (tests/unit/cov_golden.txt) on every backend. With it,
# fuzz/formats.h must build on its own with ember's strict flags.
fuzz_inc = include_directories('../fuzz')
formats_header_check = static_library('formats-header-check', 'unit/formats_header_check.c',
  include_directories: [uam_inc, fuzz_inc])
test('cov_formats',
  executable('test_cov_formats', 'unit/test_cov_formats.c',
    include_directories: fuzz_inc, dependencies: uam_dep),
  args: files('unit/cov_golden.txt'),
  depends: formats_header_check,
  suite: 'unit', kwargs: std)

test('fuzz_dict-selftest', python, args: [files('../fuzz/tools/fuzz_dict.py'), '--selftest'],
  suite: 'unit', kwargs: std)
test('status_tui-selftest', python, args: [files('../fuzz/tools/status_tui.py'), '--selftest'],
  suite: 'unit', kwargs: std)
test('brain_unit', python, args: files('fuzz/brain_unit.py'), suite: 'unit', kwargs: std)

# ---------------------------------------------------------------------------------------
# The firmware runs. Each table row is one check; the registration loops are at the end.
# ---------------------------------------------------------------------------------------
# [suite, name, runner, arguments, image, modes]: the image on the interpreter and/or the JIT
runs = []
# [name, runner, board, image, splits and, after '--', runner arguments] (snapshot-check.sh)
snapshots = []
# [name, board, image, runner arguments] (cov-check.sh)
covs = []
# [name, board, images (ELF, raw), environment, targets to build that are not among the images] (fuzz-check.sh)
fuzzes = []
# [name, board, image]: the fuzz target for the fuzz-lists checks named
lists = {'formats': [], 'gen': [], 'brain': [], 'fuzz-net': [], 'parallel': [], 'uninit-net': []}
both = ['interp', 'jit']
raw0 = ['--raw-base', '0', '--entry', '0']
snapshot_check = files('snapshot/snapshot-check.sh')
cov_check = files('cov/cov-check.sh')
fuzz_check = files('fuzz/fuzz-check.sh')
uninit_check = files('fuzz/uninit-check.sh')
no_traps = {'FUZZ_CHECK_NO_TRAPS': '1'}

if have_rv
  rv = fw_rv
  runs += [
    ['firmware', 'selftest', uam_run, ['--stats'], rv['selftest'], both],
    ['firmware', 'smptest', uam_run, ['-o', 'cpus=4'], rv['smptest'], both],
  ]
  # Scripted console input: queued at power-on it is eaten by the boot (the run times out
  # at the prompt); released by the prompt it reaches the shell.
  test('console-input-at-power-on', sh,
    args: [files('expect-exit.sh'), '124', uam_run, '-n', '2000000', '--uart-input', 'exit\\n',
           rv['consoletest']],
    suite: 'firmware', kwargs: std)
  test('console-input-after-prompt', uam_run,
    args: ['--console', 'string:ping\\nexit\\n,after=sh# ', rv['consoletest']],
    suite: 'firmware', kwargs: std)
  test('console-script', uam_run,
    args: ['--jit', '--console', 'script:tests/firmware/consoletest.script', rv['consoletest']],
    suite: 'firmware', kwargs: std)

  snapshots += [
    ['rv32-selftest', uam_run, 'rv32-ref', rv['selftest'], ['1', '3000', '6900']],
    ['rv32-smptest', uam_run, 'rv32-ref', rv['smptest'],
     ['3000', '70001', '--', '-o', 'cpus=4', '--quantum', '97']],
  ]
  covs += [['rv32-selftest', 'rv32-ref', rv['selftest'], []]]
  fuzzes += [['rv32', 'rv32-ref', [rv['fuzztarget']], {}, []]]
  # --fuzz-uninit on the planted read of RAM nothing writes (uninit-check.sh): the last
  # argument is the physical address the target reads.
  test('uninit-rv32', sh, args: [uninit_check, uam_run, 'rv32-ref', rv['fuzztarget'], '0x80100000'],
    suite: 'fuzz', kwargs: std)
  foreach k : ['formats', 'gen', 'brain', 'fuzz-net', 'parallel', 'uninit-net']
    lists += {k: lists[k] + [['rv32', 'rv32-ref', rv['fuzztarget']]]}
  endforeach

  # The memory oracles: the guarded heap and the return guard on planted bugs.
  test('oracles', sh, args: [files('fuzz/oracle-check.sh'), uam_test_run, rv['heaptarget']],
    suite: 'fuzz', kwargs: std)
  # --fuzz-uninit with the guarded heap: fresh allocations are filled in the filled run.
  test('uninit-heap', sh, args: [files('fuzz/uninit-heap-check.sh'), uam_test_run, rv['heaptarget']],
    suite: 'fuzz', kwargs: std)

  # Fuzz throughput: what one execution costs (a HAVOC batch, the input-to-state and trim
  # stages), in guest instructions at this host's plain JIT speed, against the figures
  # recorded for the host architecture in tests/fuzz/throughput.ref: a stage that costs
  # over 1.5 times its figure fails. -Dthroughput_ref= (empty) only reports.
  # fuzz-throughput-record rewrites the figures (on a quiet machine, after a change meant
  # to move them).
  throughput = [files('fuzz/throughput.py'), uam_run, 'rv32-ref', rv['fuzztarget']]
  throughput_ref = get_option('throughput_ref')
  test('fuzz-throughput', python,
    args: throughput + (throughput_ref != '' ? ['--ref', src_root / throughput_ref] : []),
    suite: 'fuzz-throughput', kwargs: alone)
  run_target('fuzz-throughput-record',
    command: [python, throughput, '--ref', src_root / 'tests/fuzz/throughput.ref', '--record',
              '--runs', '7'])

  run_target('net-check',
    command: [sh, files('net/net-check.sh'), uam_test_run, rv['nettest'], 'both'])
endif

if have_mips
  runs += [
    ['mips', 'selftest-be', uam_run, ['-b', 'mips-ref', '--stats'], fw_mips['selftest-be'], both],
    ['mips', 'selftest-el', uam_run, ['-b', 'mipsel-ref', '--stats'], fw_mips['selftest-el'], both],
    ['mips', 'smptest-be', uam_run, ['-b', 'mips-ref', '-o', 'cpus=4'], fw_mips['smptest-be'], ['interp']],
    ['mips', 'smptest-el', uam_run, ['-b', 'mipsel-ref', '-o', 'cpus=4'], fw_mips['smptest-el'], ['jit']],
  ]
  snapshots += [
    ['mips-selftest-be', uam_run, 'mips-ref', fw_mips['selftest-be'], ['777', '6000', '11800']],
    ['mips-selftest-el', uam_run, 'mipsel-ref', fw_mips['selftest-el'], ['5000']],
    ['mips-smptest-be', uam_run, 'mips-ref', fw_mips['smptest-be'],
     ['50000', '--', '-o', 'cpus=4', '--quantum', '333']],
  ]
  covs += [
    ['mips-selftest-be', 'mips-ref', fw_mips['selftest-be'], []],
    ['mips-selftest-el', 'mipsel-ref', fw_mips['selftest-el'], []],
  ]
  fuzzes += [
    ['mips', 'mips-ref', [fw_mips['fuzztarget-be'], fw_mips['fuzztarget-be.bin']], {}, []],
    ['mipsel', 'mipsel-ref', [fw_mips['fuzztarget-el']], {}, []],
  ]
  test('uninit-mips', sh, args: [uninit_check, uam_run, 'mips-ref', fw_mips['fuzztarget-be'], '0x800000'],
    suite: 'fuzz', kwargs: std)
  test('uninit-mipsel', sh, args: [uninit_check, uam_run, 'mipsel-ref', fw_mips['fuzztarget-el'], '0x800000'],
    suite: 'fuzz', kwargs: std)
  foreach k : ['formats', 'gen', 'brain', 'fuzz-net']
    lists += {k: lists[k] + [['mips', 'mips-ref', fw_mips['fuzztarget-be']]]}
  endforeach
  foreach k : ['formats', 'gen', 'fuzz-net']
    lists += {k: lists[k] + [['mipsel', 'mipsel-ref', fw_mips['fuzztarget-el']]]}
  endforeach
endif

if have_arm
  runs += [
    ['arm', 'selftest-arm', uam_run, ['-b', 'arm-ref', '--stats'], fw_arm['selftest-arm'], both],
    ['arm', 'selftest-thumb', uam_run, ['-b', 'arm-ref', '--stats'], fw_arm['selftest-thumb'], both],
    ['arm', 'selftest-arm-be', uam_run, ['-b', 'armeb-ref', '--stats'], fw_arm['selftest-arm-be'], both],
    ['arm', 'selftest-thumb-be', uam_run, ['-b', 'armeb-ref', '--stats'], fw_arm['selftest-thumb-be'], both],
    ['arm', 'smptest-arm', uam_run, ['-b', 'arm-ref', '-o', 'cpus=4'], fw_arm['smptest-arm'], ['interp']],
    ['arm', 'smptest-thumb-be', uam_run, ['-b', 'armeb-ref', '-o', 'cpus=4'], fw_arm['smptest-thumb-be'], ['jit']],
  ]
  snapshots += [
    ['arm-selftest-arm', uam_run, 'arm-ref', fw_arm['selftest-arm'], ['900', '30000', '52000']],
    ['arm-selftest-thumb', uam_run, 'arm-ref', fw_arm['selftest-thumb'], ['4321', '40000', '61000']],
    ['arm-selftest-thumb-be', uam_run, 'armeb-ref', fw_arm['selftest-thumb-be'], ['2500', '52000', '66000']],
    ['arm-smptest-thumb-be', uam_run, 'armeb-ref', fw_arm['smptest-thumb-be'],
     ['40000', '--', '-o', 'cpus=4', '--quantum', '211']],
  ]
  covs += [
    ['arm-selftest-arm', 'arm-ref', fw_arm['selftest-arm'], []],
    ['arm-selftest-thumb', 'arm-ref', fw_arm['selftest-thumb'], []],
    ['arm-selftest-arm-be', 'armeb-ref', fw_arm['selftest-arm-be'], []],
  ]
  fuzzes += [
    ['arm', 'arm-ref', [fw_arm['fuzztarget'], fw_arm['fuzztarget.bin']], {}, []],
    ['armeb', 'armeb-ref', [fw_arm['fuzztarget-be'], fw_arm['fuzztarget-be.bin']], {}, []],
  ]
  test('uninit-arm', sh, args: [uninit_check, uam_run, 'arm-ref', fw_arm['fuzztarget'], '0x800000'],
    suite: 'fuzz', kwargs: std)
  test('uninit-armeb', sh, args: [uninit_check, uam_run, 'armeb-ref', fw_arm['fuzztarget-be'], '0x800000'],
    suite: 'fuzz', kwargs: std)
  foreach k : ['formats', 'gen', 'brain', 'fuzz-net']
    lists += {k: lists[k] + [['arm', 'arm-ref', fw_arm['fuzztarget']]]}
  endforeach
  foreach k : ['formats', 'gen', 'fuzz-net']
    lists += {k: lists[k] + [['armeb', 'armeb-ref', fw_arm['fuzztarget-be']]]}
  endforeach
endif

# Flat images entered in real mode at 0. The self-test and the AMD SMM test raise SMIs
# through the 0xB2 port, and the BIOS boot needs storage, VGA and a printer: x86_64-test
# (tests/boards/).
if have_x86
  x86_test = ['-b', 'x86_64-test'] + raw0
  x86_ref = ['-b', 'x86_64-ref'] + raw0
  runs += [
    ['x86', 'selftest', uam_test_run, x86_test + ['--stats'], fw_x86['selftest'], both],
    ['x86', 'legacy', uam_run, x86_ref, fw_x86['legacy'], both],
    ['x86', 'smm-amd', uam_test_run, ['-b', 'x86_64-test', '-o', 'smm-map=amd64'] + raw0, fw_x86['smm-amd'], both],
    ['x86', 'selftest-smm-confine', uam_test_run,
     ['-b', 'x86_64-test', '-o', 'smm-confine=1', '-o', 'smm-code-chk=1'] + raw0 + ['--stats'],
     fw_x86['selftest'], ['jit']],
    ['x86', 'smm-amd-confine', uam_test_run,
     ['-b', 'x86_64-test', '-o', 'smm-map=amd64', '-o', 'smm-confine=1'] + raw0, fw_x86['smm-amd'], ['interp']],
    ['x86', 'smp', uam_run, ['-b', 'x86_64-ref', '-o', 'cpus=4'] + raw0, fw_x86['smp'], both],
  ]
  test('bios', python, args: [files('firmware-x86/bios-check.py'), uam_test_run, fw_x86['biosrom']],
    suite: 'x86', kwargs: std)
  snapshots += [
    ['x86-selftest', uam_test_run, 'x86_64-test', fw_x86['selftest'], ['1000', '5000', '15000', '--'] + raw0],
    ['x86-legacy', uam_run, 'x86_64-ref', fw_x86['legacy'], ['300', '700', '900', '--'] + raw0],
    ['x86-smm-amd', uam_test_run, 'x86_64-test', fw_x86['smm-amd'],
     ['200', '370', '420', '2000', '14000', '--', '-o', 'smm-map=amd64'] + raw0],
    ['x86-smp', uam_run, 'x86_64-ref', fw_x86['smp'],
     ['1', '5000', '20001', '40000', '--', '-o', 'cpus=4', '--quantum', '1000'] + raw0],
  ]
  # The gdb stub over the remote protocol, on a four-processor machine.
  test('gdb-stub', python,
    args: [files('gdb/gdb-check.py'), uam_run, fw_x86['smp'], fw_x86_lst['smp']],
    suite: 'gdb', kwargs: std)
  # uam-run's debugging options (--log-pc, --dump-mem, --trace-after, traces with --jit)
  test('runner-options', sh, args: [files('runner/runner-check.sh'), uam_test_run, fw_x86['selftest']],
    suite: 'runner', kwargs: std)
endif

# Links between processes: a hub and emulations that talk through it.
if have_rv
  test('link-processes', sh,
    args: [files('link/link-check.sh'), uam_link_hub, uam_run, uam_test_run, fw_rv['linktest'],
           fw_rv['fuzztarget']],
    suite: 'link', kwargs: std)
endif

# ... and its traces (--trace exc/blocks/mmio/sysreg/irq/console/dma, --history,
# --trace-file, --log-stamp, --unknown-mmio stop, with --fuzz-replay)
if have_rv
  test('runner-traces', sh,
    args: [files('runner/trace-check.sh'), uam_run, fw_rv['selftest'], fw_rv['smptest'],
           fw_rv['fuzztarget'], uam_test_run, fw_rv['nettest']],
    suite: 'runner', kwargs: std)
  # ... and its --on rules (set, log, if, stop, count and snapshots, --on-file, with fuzzing)
  test('runner-rules', sh,
    args: [files('runner/rules-check.sh'), uam_run, fw_rv['selftest'], fw_rv['fuzztarget']],
    suite: 'runner', kwargs: std)
endif

if have_ppc
  booke = ['-o', 'model=booke']
  runs += [
    ['ppc', 'selftest', uam_run, ['-b', 'ppc-ref', '--stats'], fw_ppc['selftest'], both],
    ['ppc', 'smptest', uam_run, ['-b', 'ppc-ref', '-o', 'cpus=4'], fw_ppc['smptest'], both],
    ['ppc', 'selftest-booke', uam_run, ['-b', 'ppc-ref'] + booke + ['--stats'], fw_ppc['selftest-booke'], both],
  ]
  snapshots += [
    ['ppc-selftest', uam_run, 'ppc-ref', fw_ppc['selftest'], ['777', '7300', '21000', '39800', '40400', '41000']],
    ['ppc-smptest', uam_run, 'ppc-ref', fw_ppc['smptest'],
     ['50000', '120001', '--', '-o', 'cpus=4', '--quantum', '97']],
    ['ppc-selftest-booke', uam_run, 'ppc-ref', fw_ppc['selftest-booke'],
     ['1', '8000', '8400', '9500', '11900', '13000', '14850', '--'] + booke],
  ]
  covs += [
    ['ppc-selftest', 'ppc-ref', fw_ppc['selftest'], []],
    ['ppc-selftest-booke', 'ppc-ref', fw_ppc['selftest-booke'], ['--'] + booke],
  ]
  fuzzes += [['ppc', 'ppc-ref', [fw_ppc['fuzztarget'], fw_ppc['fuzztarget.bin']], {}, []]]
  test('uninit-ppc', sh, args: [uninit_check, uam_run, 'ppc-ref', fw_ppc['fuzztarget'], '0x800000'],
    suite: 'fuzz', kwargs: std)
  foreach k : ['formats', 'gen', 'brain', 'fuzz-net']
    lists += {k: lists[k] + [['ppc', 'ppc-ref', fw_ppc['fuzztarget']]]}
  endforeach
endif

# PIC32: the self-test as MIPS32 on the mz model (TLB, shadow sets, the external interrupt
# controller, the FPU with FR = 1 and FR = 0, the DSP ASE), as MIPS16e on mx, as microMIPS
# on mm and on mz reset into microMIPS; the MIPS16e, microMIPS and DSP decoders against
# binutils.
if have_pic32
  mips_inc = include_directories('../isa/mips32')   # the decoders' oracle harnesses see the module's internal header
  foreach d : ['mips16', 'umips', 'dsp']
    dec = executable(d + '-dec', 'isa-mips/' + d + '-dec.c',
      include_directories: mips_inc, dependencies: uam_dep)
    test(d + '-decoder', python,
      args: ['-I', files('isa-mips/' + d + '-check.py'), dec, meson.current_build_dir()],
      env: {'CROSS': get_option('mipsel_cross')},
      suite: 'pic32', kwargs: std)
  endforeach
  # image: its board options
  pic32_images = {
    'selftest-mz': ['-o', 'model=mz'],
    'selftest-mz-fr0': ['-o', 'model=mz'],
    'selftest-mx': ['-o', 'model=mx'],
    'selftest-mm': ['-o', 'model=mm'],
    'selftest-mz-um': ['-o', 'model=mz', '-o', 'bootisa=micromips'],
  }
  # image: its snapshot splits
  pic32_splits = {
    'selftest-mz': ['777', '50000', '120000', '180000'],
    'selftest-mz-fr0': ['777', '120000', '200000', '230000'],
    'selftest-mx': ['777', '9000', '18000', '27000'],
    'selftest-mm': ['777', '9000', '18000', '27000'],
    'selftest-mz-um': ['777', '9000', '18000', '27000'],
  }
  foreach name, opts : pic32_images
    runs += [['pic32', name, uam_run, ['-b', 'pic32-ref'] + opts + ['--stats'], fw_pic32[name], both]]
    snapshots += [['pic32-' + name, uam_run, 'pic32-ref', fw_pic32[name], pic32_splits[name] + ['--'] + opts]]
    covs += [['pic32-' + name, 'pic32-ref', fw_pic32[name], ['--'] + opts]]
  endforeach
  fuzzes += [
    ['pic32-mz', 'pic32-ref', [fw_pic32['fuzztarget-mz']], {}, []],
    ['pic32-mx', 'pic32-ref', [fw_pic32['fuzztarget-mx']], {}, []],
  ]
endif

# The 8-bit AVR self-test on every core family the board models.
if have_avr
  foreach m : avr_models
    image = fw_avr['selftest-' + m]
    runs += [['avr', 'selftest-' + m, uam_run, ['-b', 'avr-ref', '-o', 'model=' + m, '--stats'], image, both]]
    covs += [['avr-selftest-' + m, 'avr-ref', image, ['--', '-o', 'model=' + m]]]
  endforeach
  snapshots += [
    ['avr-selftest-eplus', uam_run, 'avr-ref', fw_avr['selftest-eplus'], ['1', '5000', '100000', '200000', '280000']],
    ['avr-selftest-xm', uam_run, 'avr-ref', fw_avr['selftest-xm'], ['777', '150000', '260000', '--', '-o', 'model=xm']],
    ['avr-selftest-rc', uam_run, 'avr-ref', fw_avr['selftest-rc'], ['300', '7000', '14000', '--', '-o', 'model=rc']],
  ]
  fuzzes += [['avr', 'avr-ref', [fw_avr['fuzztarget']], no_traps, []]]
endif

if have_avr32
  model_b = ['-o', 'model=b']
  runs += [
    ['avr32', 'selftest-a', uam_run, ['-b', 'avr32-ref', '--stats'], fw_avr32['selftest-a'], both],
    ['avr32', 'selftest-b', uam_run, ['-b', 'avr32-ref'] + model_b + ['--stats'], fw_avr32['selftest-b'], both],
  ]
  snapshots += [
    ['avr32-selftest-a', uam_run, 'avr32-ref', fw_avr32['selftest-a'],
     ['777', '164500', '165000', '165300', '165600', '165900']],
    ['avr32-selftest-b', uam_run, 'avr32-ref', fw_avr32['selftest-b'],
     ['777', '189000', '189500', '190000', '190300', '190600', '--'] + model_b],
  ]
  covs += [
    ['avr32-selftest-a', 'avr32-ref', fw_avr32['selftest-a'], []],
    ['avr32-selftest-b', 'avr32-ref', fw_avr32['selftest-b'], ['--'] + model_b],
  ]
  fuzzes += [['avr32', 'avr32-ref', [fw_avr32['fuzztarget-a']], {}, []]]
endif

# The 8-bit PIC self-test on every core family the board models, and the extended
# instruction set.
if have_pic
  foreach m : pic_models
    image = fw_pic['selftest-' + m]
    runs += [['pic', 'selftest-' + m, uam_run, ['-b', 'pic-ref', '-o', 'model=' + m, '--stats'], image, both]]
    covs += [['pic-selftest-' + m, 'pic-ref', image, ['--', '-o', 'model=' + m]]]
  endforeach
  runs += [['pic', 'selftest-xinst', uam_run, ['-b', 'pic-ref', '-o', 'xinst=1', '--stats'], fw_pic['selftest-xinst'], both]]
  covs += [['pic-selftest-xinst', 'pic-ref', fw_pic['selftest-xinst'], ['--', '-o', 'xinst=1']]]
  snapshots += [
    ['pic-selftest-pic18', uam_run, 'pic-ref', fw_pic['selftest-pic18'], ['1', '5000', '12000', '18000', '19000', '19300']],
    ['pic-selftest-enhanced', uam_run, 'pic-ref', fw_pic['selftest-enhanced'],
     ['777', '16500', '16800', '16950', '--', '-o', 'model=enhanced']],
    ['pic-selftest-mid', uam_run, 'pic-ref', fw_pic['selftest-mid'], ['300', '5200', '5300', '--', '-o', 'model=mid']],
    ['pic-selftest-baseline', uam_run, 'pic-ref', fw_pic['selftest-baseline'], ['300', '1300', '--', '-o', 'model=baseline']],
  ]
  fuzzes += [['pic', 'pic-ref', [fw_pic['fuzztarget']], no_traps + {'FUZZ_CHECK_NO_MAGIC': '1'}, []]]
endif

# The 8051 self-test on every core the board models.
if have_mcs51
  foreach m : mcs51_models
    image = fw_mcs51['selftest-' + m]
    runs += [['mcs51', 'selftest-' + m, uam_run, ['-b', 'mcs51-ref', '-o', 'model=' + m, '--stats'], image, both]]
    covs += [['mcs51-selftest-' + m, 'mcs51-ref', image, ['--', '-o', 'model=' + m]]]
  endforeach
  snapshots += [
    ['mcs51-selftest-8052', uam_run, 'mcs51-ref', fw_mcs51['selftest-8052'],
     ['1', '5000', '263500', '264700', '264900', '265000', '265100', '265400']],
    ['mcs51-selftest-cip51', uam_run, 'mcs51-ref', fw_mcs51['selftest-cip51'],
     ['777', '265100', '265400', '--', '-o', 'model=cip51']],
  ]
endif
if have_mcs51_fuzz
  fuzzes += [['mcs51', 'mcs51-ref', [fw_mcs51_fuzz[0].full_path()], no_traps, [fw_mcs51_fuzz]]]
endif

# The 6502 self-test on every core the board models.
if have_6502
  foreach m : m6502_models
    image = fw_6502['selftest-' + m]
    runs += [['6502', 'selftest-' + m, uam_run, ['-b', '6502-ref', '-o', 'model=' + m, '--stats'], image, both]]
    covs += [['6502-selftest-' + m, '6502-ref', image, ['--', '-o', 'model=' + m]]]
  endforeach
  snapshots += [
    ['6502-selftest-nmos', uam_run, '6502-ref', fw_6502['selftest-nmos'],
     ['1', '5000', '60000', '127000', '127500', '127800', '128000']],
    ['6502-selftest-65c02', uam_run, '6502-ref', fw_6502['selftest-65c02'],
     ['777', '40000', '86400', '86900', '87200', '--', '-o', 'model=65c02']],
  ]
endif
if have_6502_fuzz
  fuzzes += [['6502', '6502-ref', [fw_6502_fuzz[0].full_path()], no_traps, [fw_6502_fuzz]]]
endif

# The Z80 self-test on both cores the board models.
if have_z80
  foreach m : z80_models
    image = fw_z80['selftest-' + m]
    runs += [['z80', 'selftest-' + m, uam_run, ['-b', 'z80-ref', '-o', 'model=' + m, '--stats'], image, both]]
    covs += [['z80-selftest-' + m, 'z80-ref', image, ['--', '-o', 'model=' + m]]]
  endforeach
  snapshots += [
    ['z80-selftest-nmos', uam_run, 'z80-ref', fw_z80['selftest-nmos'],
     ['1', '5000', '100000', '180200', '180400', '180600', '180700']],
    ['z80-selftest-cmos', uam_run, 'z80-ref', fw_z80['selftest-cmos'], ['777', '180500', '--', '-o', 'model=cmos']],
  ]
endif
if have_z80_fuzz
  fuzzes += [['z80', 'z80-ref', [fw_z80_fuzz[0].full_path()], no_traps + {'FUZZ_CHECK_ARGS': '--ram 0xFF00'}, [fw_z80_fuzz]]]
endif

# The Xtensa self-test on each model with a toolchain: little-endian (xtensa-ref) and,
# laid out the big-endian way, on xtensaeb-ref; the decoder against each toolchain's
# disassembler.
if have_xtensa
  xt_tools = []
  foreach m : xtensa_models
    opts = ['-o', 'model=' + m]
    reset = m == 'lx106' ? '0x40000080' : '0x40000400'
    runs += [
      ['xtensa', 'selftest-' + m, uam_run, ['-b', 'xtensa-ref'] + opts + ['--stats'], fw_xtensa['selftest-' + m], both],
      ['xtensa', 'selftest-' + m + '-be', uam_run,
       ['-b', 'xtensaeb-ref'] + opts + ['--raw-base', '0x40000000', '--entry', reset],
       fw_xtensa['selftest-' + m + '-be.bin'], both],
    ]
    snapshots += [['xtensa-selftest-' + m, uam_run, 'xtensa-ref', fw_xtensa['selftest-' + m],
                   ['1', '500', '3000', '9000', '14000', '--'] + opts]]
    covs += [['xtensa-selftest-' + m, 'xtensa-ref', fw_xtensa['selftest-' + m], ['--'] + opts]]
    fuzzes += [['xtensa-' + m, 'xtensa-ref', [fw_xtensa['fuzztarget-' + m]],
                {'FUZZ_CHECK_ARGS': '-o model=' + m}, []]]
    xt_tools += m + '=' + xtensa_objdump[m].full_path()
  endforeach
  xt_dec = executable('xt-dec', 'isa-xtensa/xt-dec.c',
    include_directories: include_directories('../isa/xtensa'), dependencies: uam_dep)
  test('decoder', python,
    args: ['-I', '-B', files('isa-xtensa/xt-check.py'), xt_dec, meson.current_build_dir()] + xt_tools,
    suite: 'xtensa', kwargs: std)
endif

# The out-of-tree example board (examples/external-board), configured and built as an
# external project would (build-external.sh): with ember as its subproject, the layout
# every board project uses, and against this build's ember through pkg-config, as with an
# installed ember.
have_example = have_mips and not meson.is_cross_build()
if have_example
  example = '../examples/external-board'
  example_files = files(example / 'meson.build', example / 'emulation/meson.build',
    example / 'emulation/main.c', example / 'emulation/ext_board.c', example / 'emulation/ext_board.h',
    example / 'emulation/ext_id.c', example / 'emulation/ext_id.h', example / 'tests/meson.build',
    example / 'tests/registered.sh', example / 'tests/boot.sh', 'build-external.sh')
  ext_run = custom_target('ext-run',
    output: 'ext-run',
    depends: libuam,
    depend_files: example_files,
    command: [sh, files('build-external.sh'), 'subproject', src_root / 'examples/external-board',
              meson.current_build_dir() / 'external-board', src_root, 'ext-run', '@OUTPUT@'])
  ext_run_pc = custom_target('ext-run-pkgconfig',
    output: 'ext-run-pkgconfig',
    depends: libuam,
    depend_files: example_files,
    command: [sh, files('build-external.sh'), 'pkgconfig', src_root / 'examples/external-board',
              meson.current_build_dir() / 'external-board-pkgconfig', build_root / 'meson-uninstalled',
              'ext-run', '@OUTPUT@'])
  ext_args = ['-b', 'ext-mips', '-o', 'banner=external', '--stats', fw_mips['selftest-be']]
  expect_ok = [files('expect-exit.sh'), '0']
  test('ext-mips', sh, args: expect_ok + [ext_run] + ext_args, suite: 'example', kwargs: std)
  test('ext-mips-pkgconfig', sh, args: expect_ok + [ext_run_pc] + ext_args, suite: 'example', kwargs: std)
  snapshots += [['example-ext-mips', ext_run, 'ext-mips', fw_mips['selftest-be'], ['4000']]]
endif

# ---------------------------------------------------------------------------------------
# Registration
# ---------------------------------------------------------------------------------------
# Each self-test runs on the interpreter and on the JIT.
foreach r : runs
  foreach mode : r[5]
    test(r[1] + '-' + mode, r[2],
      args: r[3] + (mode == 'jit' ? ['--jit'] : []) + [r[4]],
      suite: r[0], kwargs: std)
  endforeach
endforeach

# A run split by --save-snapshot / --load-snapshot must equal the uninterrupted run, on the
# stock boards and on the out-of-tree example board.
foreach s : snapshots
  test(s[0], sh, args: [snapshot_check, s[1], s[2], s[3]] + s[4], suite: 'snapshot', kwargs: std)
endforeach

# Block coverage: identical maps from the interpreter and the JIT (chained or not), and
# runs that end exactly as without coverage, on every ISA.
foreach c : covs
  test(c[0], sh, args: [cov_check, uam_run, c[1], c[2]] + c[3], suite: 'cov', kwargs: std)
endforeach

# The fuzz harness end to end: planted bugs found, the same on every backend, crash files
# reproduce on their own.
foreach f : fuzzes
  test(f[0], sh, args: [fuzz_check, uam_run, f[1]] + f[2], env: f[3], depends: f[4], suite: 'fuzz', kwargs: std)
endforeach
if have_rv and have_mips and have_arm and have_ppc
  test('cov-blocks', python,
    args: [files('fuzz/cov_blocks_check.py'), uam_run, build_root],
    depends: [fw_rv['fuzztarget'], fw_rv['irqtarget'], fw_mips['fuzztarget-be'], fw_arm['fuzztarget'],
              fw_arm['fuzztarget.bin'], fw_ppc['fuzztarget']],
    suite: 'fuzz', kwargs: std)
endif

# Site lists, boundary files, the coverage region and the fuzz network frontend through the
# runner: lists written by tests/fuzz/mk_lists.py, and by fuzz/tools' rizin generators when
# rizin is installed (gen-check; with headless Ghidra on PATH, the Ghidra route must write
# the same files); the region read back by fuzz/tools/cov_region.py and an independent
# reader; sessions with fuzz/tools/fuzz_brain.py (every power schedule, a dictionary find);
# the protocol frame by frame against a scripted brain (tests/fuzz/fake_brain.py), SPARSE
# replies included; and a parallel campaign through fuzz/tools/fuzz_launch.py.
foreach kind, rows : lists
  foreach l : rows
    test(kind + '-' + l[0], sh,
      args: [files('fuzz/' + kind + '-check.sh'), uam_run, l[1], l[2]],
      suite: 'fuzz-lists',
      kwargs: kind == 'formats' or kind == 'gen' ? std : alone)
  endforeach
endforeach

# The cross-ISA benchmark (tests/bench, a meson project of its own that tools/xbench.sh
# configures and builds): bench checks every guest build, with the interpreter and the
# JIT, against the host build in two rounds; the xbench target times them
# (-Dxbench_args="--per-kernel" for each kernel on its own).
xbench_env = {
  'RISCV_CROSS': get_option('riscv_cross'),
  'MIPS_CROSS': get_option('mips_cross'),
  'MIPSEL_CROSS': get_option('mipsel_cross'),
  'ARM_CROSS': get_option('arm_cross'),
  'PPC_CROSS': get_option('ppc_cross'),
  'AVR_CROSS': get_option('avr_cross'),
  'AVR32_CROSS': get_option('avr32_cross'),
  'XTENSA_LX106_CROSS': get_option('xtensa_lx106_cross'),
  'XTENSA_LX6_CROSS': get_option('xtensa_lx6_cross'),
  'XTENSA_LX7_CROSS': get_option('xtensa_lx7_cross'),
  'SDCC': get_option('sdcc'),
  'SDASZ80': get_option('sdasz80'),
}
xbench = [files('../tools/xbench.sh'), '--run', uam_run, '--out', build_root / 'firmware-bench']
test('xbench-check', sh, args: xbench + ['--check'], env: xbench_env, suite: 'bench', kwargs: alone)
# xbench_args is read as shell words, so a list can be quoted: -Dxbench_args='--variants "rv32 x86"'
run_target('xbench',
  command: [sh, '-c', '''eval 'exec sh "$@"' "$XBENCH_ARGS"''', 'sh', xbench],
  env: xbench_env + {'XBENCH_ARGS': get_option('xbench_args')})

# The JIT differential test at length: -Djitdiff_blocks random blocks per seed, several seeds.
run_target('jitdiff-soak',
  command: [sh, '-ec', '''
    t=$1 n=$2
    "$t" "$n" 1
    "$t" "$n" 2
    "$t" "$n" 3
    UAM_JIT_REGS=1 "$t" "$n" 4
    UAM_JIT_REGS=2 "$t" "$n" 5
    ''', 'sh', test_jitdiff, get_option('jitdiff_blocks').to_string()])
