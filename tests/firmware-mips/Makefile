# Builds the bare-metal MIPS32 self-test image and the fuzz target (one ELF per endianness).

CROSS_BE ?= mips-linux-gnu-
CROSS_EL ?= mipsel-linux-gnu-
OUT      ?= ../../build/firmware-mips

ARCH   := -march=mips32r2 -mabi=32 -EB
FW_CFLAGS  = -O2 -g -std=gnu11 -Wall -Wextra -ffreestanding -fno-builtin -fno-pic -mno-abicalls \
          -fno-stack-protector -nostdlib -nostartfiles -G0
FW_LDFLAGS = -static -no-pie -Wl,-T,link.ld -Wl,--build-id=none

all: $(OUT)/selftest-be.elf $(OUT)/selftest-el.elf $(OUT)/fuzztarget-be.elf $(OUT)/fuzztarget-el.elf \
     $(OUT)/fuzztarget-be.bin $(OUT)/smptest-be.elf $(OUT)/smptest-el.elf

$(OUT)/selftest-be.elf: start.S selftest.c link.ld
	@mkdir -p $(OUT)
	$(CROSS_BE)gcc -march=mips32r2 -mabi=32 -EB $(FW_CFLAGS) $(FW_LDFLAGS) start.S selftest.c -o $@
	$(CROSS_BE)objdump -d $@ > $(OUT)/selftest-be.dis

$(OUT)/selftest-el.elf: start.S selftest.c link.ld
	@mkdir -p $(OUT)
	$(CROSS_EL)gcc -march=mips32r2 -mabi=32 -EL $(FW_CFLAGS) $(FW_LDFLAGS) start.S selftest.c -o $@
	$(CROSS_EL)objdump -d $@ > $(OUT)/selftest-el.dis

$(OUT)/fuzztarget-be.elf: start.S fuzztarget.c ../fuzz/fuzztarget.h link.ld
	@mkdir -p $(OUT)
	$(CROSS_BE)gcc -march=mips32r2 -mabi=32 -EB $(FW_CFLAGS) $(FW_LDFLAGS) start.S fuzztarget.c -o $@

# The same image without its ELF wrapper, for the raw-image path (--raw-base, --entry).
$(OUT)/fuzztarget-be.bin: $(OUT)/fuzztarget-be.elf
	$(CROSS_BE)objcopy -O binary $< $@

$(OUT)/fuzztarget-el.elf: start.S fuzztarget.c ../fuzz/fuzztarget.h link.ld
	@mkdir -p $(OUT)
	$(CROSS_EL)gcc -march=mips32r2 -mabi=32 -EL $(FW_CFLAGS) $(FW_LDFLAGS) start.S fuzztarget.c -o $@

$(OUT)/smptest-be.elf: smpstart.S smptest.c ../smp/smp_test.h link.ld
	@mkdir -p $(OUT)
	$(CROSS_BE)gcc -march=mips32r2 -mabi=32 -EB $(FW_CFLAGS) $(FW_LDFLAGS) smpstart.S smptest.c -o $@

$(OUT)/smptest-el.elf: smpstart.S smptest.c ../smp/smp_test.h link.ld
	@mkdir -p $(OUT)
	$(CROSS_EL)gcc -march=mips32r2 -mabi=32 -EL $(FW_CFLAGS) $(FW_LDFLAGS) smpstart.S smptest.c -o $@

.PHONY: all
