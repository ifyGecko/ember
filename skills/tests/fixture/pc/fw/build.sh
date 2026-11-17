#!/bin/sh
# build.sh OUT - builds the PC fixture firmware into OUT (tests/run-tests.sh calls it): fw.bin
# is 128 KiB of boot flash (the code in the top 64 KiB), spi.bin a 1 MiB SPI image with a
# flash descriptor whose BIOS region ends with fw.bin, spd.bin a DDR4 SPD.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-.}
${CC:-gcc} -m32 -march=i686 -O2 -std=gnu11 -ffreestanding -fno-builtin -fno-pic -fno-pie \
    -fno-stack-protector -fno-asynchronous-unwind-tables -mgeneral-regs-only -Wall -Wextra \
    -nostdlib -static -no-pie -Wl,-T,"$here/link.ld" -Wl,--build-id=none \
    -Wl,--no-warn-rwx-segments "$here/start.S" "$here/main.c" -o "$out/fw.elf"
objcopy -O binary "$out/fw.elf" "$out/fw64.bin"
python3 "$here/mkimages.py" "$out/fw64.bin" "$out"
