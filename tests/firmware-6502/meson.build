# The 6502 self-test, once per 6502-ref model (boards/m6502_ref.c), assembled with
# sdas6500: the common part, the model's part and the generated instruction vectors, as
# one source, linked at 0x8000 (the board's ROM) and written as a raw image of the ROM.
# The fuzz target is built with SDCC (mos6502): fuzztarget.bin and its symbols
# (fuzztarget.bin.syms, for tests/fuzz/fuzz-check.sh).

sdas6500 = find_program(get_option('sdas6500'), required: false)
sdld = find_program(get_option('sdld'), required: false)
have_6502 = sdas6500.found() and sdld.found()
have_6502_fuzz = sdcc.found()
fw_6502 = {}
m6502_models = ['nmos', '2a03', '65c02']

if have_6502
  foreach m : m6502_models
    vectors = custom_target('6502-vectors-' + m,
      output: 'vectors-' + m + '.s', input: 'gen_vectors.py',
      command: [python, '@INPUT@', m, '@OUTPUT@'])
    # The NMOS undefined-opcode tests (nmos.s) go into the NMOS and 2A03 builds.
    parts = m == '65c02' ? files('selftest.s', 'model-' + m + '.s') : files('selftest.s', 'nmos.s', 'model-' + m + '.s')
    src = custom_target('6502-selftest-' + m + '-s',
      output: 'selftest-' + m + '.s', input: [parts, vectors], capture: true,
      command: [cat, '@INPUT@'])
    fw_6502 += {'selftest-' + m: custom_target('6502-selftest-' + m,
      output: 'selftest-' + m + '.bin',
      input: src,
      depend_files: files('ihx2bin.py'),
      command: [sh, '-ec', '''
        sdas=$1 sdld=$2 src=$3 work=$4 bin=$5; shift 5
        mkdir -p "$work"
        "$sdas" -los "$work/selftest.rel" "$src"
        "$sdld" -i "$work/selftest.ihx" "$work/selftest.rel" > /dev/null
        "$@" "$work/selftest.ihx" "$bin"
        ''', 'sh', sdas6500, sdld, '@INPUT@', '@PRIVATE_DIR@', '@OUTPUT@', python, files('ihx2bin.py')])}
  endforeach
  foreach name, t : fw_6502
    firmware_all += t
  endforeach
endif

fw_6502_fuzz = []   # [fuzztarget.bin, fuzztarget.bin.syms]
if have_6502_fuzz
  fw_6502_fuzz = custom_target('6502-fuzztarget',
    output: ['fuzztarget.bin', 'fuzztarget.bin.syms'],
    input: 'fuzztarget.c',
    depend_files: files('../fuzz/fuzztarget.h', 'ihx2bin.py'),
    command: [sh, '-ec', '''
      sdcc=$1 src=$2 work=$3 bin=$4 syms=$5; shift 5
      mkdir -p "$work"
      "$sdcc" -mmos6502 --std-c11 --code-loc 0x8000 -Wl-bGSINIT=0x8000 "$src" -o "$work/fuzztarget.ihx"
      "$@" "$work/fuzztarget.ihx" "$bin"
      awk '$2 ~ /^_/ && $1 ~ /^0000[89A-F]/ { print substr($2, 2), substr($1, 5) }' "$work/fuzztarget.map" > "$syms"
      ''', 'sh', sdcc, '@INPUT@', '@PRIVATE_DIR@', '@OUTPUT0@', '@OUTPUT1@', python, files('ihx2bin.py')])
  firmware_all += fw_6502_fuzz
endif
