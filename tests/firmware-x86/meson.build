# The bare-metal x86-64 images: flat binaries entered in real mode at physical 0 (the
# self-test walks through protected mode into long mode), each with its listing.

nasm = find_program(get_option('nasm'), required: false)
have_x86 = nasm.found()
fw_x86 = {}       # image name -> the .bin
fw_x86_lst = {}   # image name -> its listing

if have_x86
  foreach name : ['selftest', 'smp', 'legacy', 'biosrom', 'smm-amd']
    fw_x86 += {name: custom_target('x86-' + name,
      output: name + '.bin',
      input: name + '.asm',
      depfile: name + '.d',
      command: [nasm, '-f', 'bin', '-MD', '@DEPFILE@', '@INPUT@', '-o', '@OUTPUT@'])}
    fw_x86_lst += {name: custom_target('x86-' + name + '-lst',
      output: name + '.lst',
      input: name + '.asm',
      depfile: name + '.lst.d',
      command: [nasm, '-f', 'bin', '-MD', '@DEPFILE@', '-MT', '@OUTPUT@', '-l', '@OUTPUT@', '@INPUT@',
                '-o', '/dev/null'])}
    firmware_all += [fw_x86[name], fw_x86_lst[name]]
  endforeach
endif
