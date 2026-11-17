# The 8051 self-test, once per mcs51-ref model (boards/mcs51_ref.c), assembled with as31:
# the common part, the model's part and the generated arithmetic vectors, as one source.
# The CIP-51 image also carries three code banks (bank.asm, laid out by mkimage.py).
# The fuzz target is built with SDCC: fuzztarget.bin and its symbols (fuzztarget.bin.syms,
# for tests/fuzz/fuzz-check.sh).

as31 = find_program(get_option('as31'), required: false)
have_mcs51 = as31.found()
have_mcs51_fuzz = sdcc.found() and makebin.found()
fw_mcs51 = {}
mcs51_models = ['8051', '8052', 'cip51']

if have_mcs51
  # as31 OUT.bin IN.asm OPTIONS: it reports on stdout, and writes a listing next to its source
  as31_cmd = [sh, '-ec', 'as31=$1 out=$2 src=$3; shift 3; "$as31" "$@" "-O$out" "$src" > /dev/null', 'sh', as31]
  mcs51_vectors = custom_target('mcs51-vectors',
    output: 'vectors.asm', input: 'gen_vectors.py',
    command: [python, '@INPUT@', '@OUTPUT@'])
  mcs51_main = {}
  foreach m : mcs51_models
    src = custom_target('mcs51-selftest-' + m + '-asm',
      output: 'selftest-' + m + '.asm',
      input: [files('selftest.asm', 'model-' + m + '.asm'), mcs51_vectors],
      capture: true,
      command: [cat, '@INPUT@'])
    mcs51_main += {m: custom_target('mcs51-main-' + m,
      output: (m == 'cip51' ? 'main-' : 'selftest-') + m + '.bin',
      input: src,
      command: [as31_cmd, '@OUTPUT@', '@INPUT@', '-l', '-Fbin'])}
  endforeach
  mcs51_banks = []
  foreach n : ['1', '2', '3']
    bank_src = custom_target('mcs51-bank-' + n + '-asm',
      output: 'bank-' + n + '.asm', input: 'bank.asm', capture: true,
      command: [find_program('sed'), 's/BANK/' + n + '/g', '@INPUT@'])
    mcs51_banks += custom_target('mcs51-bank-' + n,
      output: 'bank-' + n + '.bin', input: bank_src,
      command: [as31_cmd, '@OUTPUT@', '@INPUT@', '-Fbin'])
  endforeach
  fw_mcs51 += {
    'selftest-8051': mcs51_main['8051'],
    'selftest-8052': mcs51_main['8052'],
    'selftest-cip51': custom_target('mcs51-selftest-cip51',
      output: 'selftest-cip51.bin',
      input: [mcs51_main['cip51'], mcs51_banks],
      depend_files: files('mkimage.py'),
      command: [python, files('mkimage.py'), '@OUTPUT@', '@INPUT@']),
  }
  foreach name, t : fw_mcs51
    firmware_all += t
  endforeach
endif

fw_mcs51_fuzz = []   # [fuzztarget.bin, fuzztarget.bin.syms]
if have_mcs51_fuzz
  fw_mcs51_fuzz = custom_target('mcs51-fuzztarget',
    output: ['fuzztarget.bin', 'fuzztarget.bin.syms'],
    input: 'fuzztarget.c',
    depend_files: files('../fuzz/fuzztarget.h'),
    command: [sh, '-ec', '''
      sdcc=$1 makebin=$2 src=$3 work=$4 bin=$5 syms=$6
      mkdir -p "$work"
      "$sdcc" -mmcs51 --model-large --std-c11 --iram-size 256 --xram-size 0xff00 "$src" -o "$work/fuzztarget.ihx"
      "$makebin" -p "$work/fuzztarget.ihx" "$bin"
      awk '$1 == "C:" && substr($3, 1, 1) == "_" { print substr($3, 2), $2 }' "$work/fuzztarget.map" > "$syms"
      ''', 'sh', sdcc, makebin, '@INPUT@', '@PRIVATE_DIR@', '@OUTPUT0@', '@OUTPUT1@'])
  firmware_all += fw_mcs51_fuzz
endif
