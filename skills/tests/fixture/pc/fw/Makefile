# Builds the PC fixture firmware (tests/run-tests.sh does the same): fw.bin is 128 KiB of
# boot flash (the code in the top 64 KiB), spi.bin a 1 MiB SPI image with a flash
# descriptor whose BIOS region ends with fw.bin, spd.bin a DDR4 SPD.
CC  ?= gcc
OUT ?= .
CFLAGS := -m32 -march=i686 -O2 -std=gnu11 -ffreestanding -fno-builtin -fno-pic -fno-pie \
          -fno-stack-protector -fno-asynchronous-unwind-tables -mgeneral-regs-only -Wall -Wextra
all: $(OUT)/fw.bin $(OUT)/spi.bin $(OUT)/spd.bin
$(OUT)/fw.elf: start.S main.c link.ld
	$(CC) $(CFLAGS) -nostdlib -static -no-pie -Wl,-T,link.ld -Wl,--build-id=none \
	    -Wl,--no-warn-rwx-segments start.S main.c -o $@
$(OUT)/fw.bin $(OUT)/spi.bin $(OUT)/spd.bin: $(OUT)/fw.elf mkimages.py
	objcopy -O binary $(OUT)/fw.elf $(OUT)/fw64.bin
	python3 mkimages.py $(OUT)/fw64.bin $(OUT)
