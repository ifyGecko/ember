# The Z80 self-test, once per z80-ref model (boards/z80_ref.c), assembled with sdasz80: the
# model's part, the common part and the generated vectors, as one source, linked at 0 into
# a raw image. The fuzz target is built with SDCC's z80 port and our start code (crt0.s)
# instead of its own: fuzztarget.bin and its symbols (fuzztarget.bin.syms, for
# tests/fuzz/fuzz-check.sh).

sdasz80 = find_program(get_option('sdasz80'), required: false)
sdldz80 = find_program(get_option('sdldz80'), required: false)
have_z80 = sdasz80.found() and sdldz80.found() and makebin.found()
have_z80_fuzz = have_z80 and sdcc.found()
fw_z80 = {}
z80_models = ['nmos', 'cmos']

if have_z80
  z80_vectors = custom_target('z80-vectors',
    output: 'vectors.s', input: 'gen_vectors.py',
    command: [python, '@INPUT@', '@OUTPUT@'])
  foreach m : z80_models
    src = custom_target('z80-selftest-' + m + '-s',
      output: 'selftest-' + m + '.s',
      input: [files('model-' + m + '.s', 'selftest.s'), z80_vectors],
      capture: true,
      command: [cat, '@INPUT@'])
    fw_z80 += {'selftest-' + m: custom_target('z80-selftest-' + m,
      output: 'selftest-' + m + '.bin',
      input: src,
      command: [sh, '-ec', '''
        sdas=$1 sdld=$2 makebin=$3 src=$4 work=$5 bin=$6
        mkdir -p "$work"
        "$sdas" -l -o "$work/selftest.rel" "$src"
        "$sdld" -m -i "$work/selftest.ihx" "$work/selftest.rel" > /dev/null
        "$makebin" -p -s 65536 "$work/selftest.ihx" "$bin"
        ''', 'sh', sdasz80, sdldz80, makebin, '@INPUT@', '@PRIVATE_DIR@', '@OUTPUT@'])}
  endforeach
  foreach name, t : fw_z80
    firmware_all += t
  endforeach
endif

fw_z80_fuzz = []   # [fuzztarget.bin, fuzztarget.bin.syms]
if have_z80_fuzz
  fw_z80_fuzz = custom_target('z80-fuzztarget',
    output: ['fuzztarget.bin', 'fuzztarget.bin.syms'],
    input: files('fuzztarget.c', 'crt0.s'),
    depend_files: files('../fuzz/fuzztarget.h'),
    command: [sh, '-ec', '''
      sdas=$1 sdcc=$2 makebin=$3 src=$4 crt0=$5 work=$6 bin=$7 syms=$8
      mkdir -p "$work"
      "$sdas" -o "$work/crt0.rel" "$crt0"
      "$sdcc" -mz80 --no-std-crt0 --code-loc 0x0200 --data-loc 0x8000 "$src" "$work/crt0.rel" -o "$work/fuzztarget.ihx"
      "$makebin" -p -s 65536 "$work/fuzztarget.ihx" "$bin"
      awk '$1 ~ /^0000/ && substr($2, 1, 1) == "_" { print substr($2, 2), substr($1, 5) }' "$work/fuzztarget.map" > "$syms"
      ''', 'sh', sdasz80, sdcc, makebin, '@INPUT0@', '@INPUT1@', '@PRIVATE_DIR@', '@OUTPUT0@', '@OUTPUT1@'])
  firmware_all += fw_z80_fuzz
endif
