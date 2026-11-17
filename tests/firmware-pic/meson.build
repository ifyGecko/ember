# The 8-bit PIC self-tests, one per pic-ref model (boards/pic_ref.c), assembled with gpasm
# (gputils). The processor named gives gpasm the instruction set; the board's layout is the
# reference one. gpasm writes Intel hex: hex2elf.py makes the ELF images uam-run and the
# test scripts load, with gpasm's labels as symbols.
#
#   model     instruction set (gpasm -p)  flag vectors at (words; PIC18 bytes)  reset vector
#   baseline  16f59                       0x200-0x7EF                           0xFFE (last word)
#   mid       16f877                      0x800-0x1FEF                          0
#   enhanced  16f1939                     0x800-0x7FEF                          0
#   pic18     18f4620                     0x4000-0xFFEF                         0
# Plus selftest-xinst (pic18 with -o xinst=1) and fuzztarget (pic18).

gpasm = find_program(get_option('gpasm'), required: false)
have_pic = gpasm.found()
fw_pic = {}
pic_models = ['baseline', 'mid', 'enhanced', 'pic18']

if have_pic
  # image: [processor, gpasm flags, flag vectors (or none), hex2elf arguments: the entry,
  # --words, and --below (program memory: the 14-bit configuration words are not part of it)]
  foreach name, v : {
    'selftest-baseline': ['16f59', [], ['0x200', '0x7EF'], ['0xFFE', '--words', '--below', '0x1000']],
    'selftest-mid': ['16f877', [], ['0x800', '0x1FEF'], ['0', '--words', '--below', '0x4000']],
    'selftest-enhanced': ['16f1939', [], ['0x800', '0x7FEF'], ['0', '--words', '--below', '0x10000']],
    'selftest-pic18': ['18f4620', [], ['0x4000', '0xFFEF'], ['0']],
    'selftest-xinst': ['18f4620', ['-y'], [], ['0']],
    'fuzztarget': ['18f4620', [], [], ['0']],
  }
    deps = []
    if v[2].length() > 0
      model = name.split('-')[1]
      deps += custom_target('pic-vectors-' + model,
        output: 'vectors-' + model + '.inc', input: 'gen_vectors.py',
        command: [python, '@INPUT@', model, v[2], '@OUTPUT@'])
    endif
    hex = custom_target('pic-' + name + '-hex',
      output: [name + '.hex', name + '.lst'],
      input: name + '.asm',
      depends: deps,
      command: [gpasm, '-q', v[1], '-p', v[0], '-I', meson.current_build_dir(), '-o', '@OUTPUT0@', '@INPUT@'])
    fw_pic += {name: custom_target('pic-' + name,
      output: name + '.elf',
      input: [hex[0], hex[1]],
      depend_files: files('hex2elf.py'),
      command: [python, files('hex2elf.py'), '@INPUT0@', '@INPUT1@', '@OUTPUT@', v[3]])}
  endforeach
  foreach name, t : fw_pic
    firmware_all += t
  endforeach
endif
