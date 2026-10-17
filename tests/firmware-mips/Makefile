# Builds the bare-metal MIPS32 self-test image (one ELF per endianness).

CROSS_BE ?= mips-linux-gnu-
CROSS_EL ?= mipsel-linux-gnu-
OUT      ?= ../../build/firmware-mips

ARCH   := -march=mips32r2 -mabi=32 -EB
FW_CFLAGS  = -O2 -g -std=gnu11 -Wall -Wextra -ffreestanding -fno-builtin -fno-pic -mno-abicalls \
          -fno-stack-protector -nostdlib -nostartfiles -G0
FW_LDFLAGS = -static -no-pie -Wl,-T,link.ld -Wl,--build-id=none

all: $(OUT)/selftest-be.elf $(OUT)/selftest-el.elf

$(OUT)/selftest-be.elf: start.S selftest.c link.ld
	@mkdir -p $(OUT)
	$(CROSS_BE)gcc -march=mips32r2 -mabi=32 -EB $(FW_CFLAGS) $(FW_LDFLAGS) start.S selftest.c -o $@
	$(CROSS_BE)objdump -d $@ > $(OUT)/selftest-be.dis

$(OUT)/selftest-el.elf: start.S selftest.c link.ld
	@mkdir -p $(OUT)
	$(CROSS_EL)gcc -march=mips32r2 -mabi=32 -EL $(FW_CFLAGS) $(FW_LDFLAGS) start.S selftest.c -o $@
	$(CROSS_EL)objdump -d $@ > $(OUT)/selftest-el.dis

.PHONY: all
