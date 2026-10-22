# Builds the bare-metal ARMv7-A self-test (as ARM and as Thumb-2 code) and the fuzz target,
# little-endian and big-endian (BE8).

CROSS ?= arm-linux-gnueabi-
OUT   ?= ../../build/firmware-arm

FW_CFLAGS  = -march=armv7ve -mfloat-abi=soft -O2 -g -std=gnu11 -Wall -Wextra -ffreestanding \
             -fno-builtin -fno-pic -fno-stack-protector -fno-unwind-tables \
             -fno-asynchronous-unwind-tables -nostdlib -nostartfiles
FW_LDFLAGS = -static -no-pie -Wl,-T,link.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments

BE := -mbig-endian -Wl,--be8   # big-endian data, little-endian code (BE8)

all: $(OUT)/selftest-arm.elf $(OUT)/selftest-thumb.elf $(OUT)/fuzztarget.elf $(OUT)/fuzztarget.bin \
     $(OUT)/selftest-arm-be.elf $(OUT)/selftest-thumb-be.elf $(OUT)/fuzztarget-be.elf \
     $(OUT)/fuzztarget-be.bin \
     $(OUT)/smptest-arm.elf $(OUT)/smptest-thumb-be.elf

# The multiprocessor self-test: ARM little-endian, and Thumb-2 BE8.
$(OUT)/smptest-arm.elf: smpstart.S smptest.c ../smp/smp_test.h link.ld
	@mkdir -p $(OUT)
	$(CROSS)gcc -marm $(FW_CFLAGS) $(FW_LDFLAGS) smpstart.S smptest.c -o $@

$(OUT)/smptest-thumb-be.elf: smpstart.S smptest.c ../smp/smp_test.h link.ld
	@mkdir -p $(OUT)
	$(CROSS)gcc -mthumb $(BE) $(FW_CFLAGS) $(FW_LDFLAGS) smpstart.S smptest.c -o $@

$(OUT)/selftest-arm.elf: start.S selftest.c link.ld
	@mkdir -p $(OUT)
	$(CROSS)gcc -marm $(FW_CFLAGS) $(FW_LDFLAGS) start.S selftest.c -o $@
	$(CROSS)objdump -d $@ > $(OUT)/selftest-arm.dis

$(OUT)/selftest-thumb.elf: start.S selftest.c link.ld
	@mkdir -p $(OUT)
	$(CROSS)gcc -mthumb $(FW_CFLAGS) $(FW_LDFLAGS) start.S selftest.c -o $@
	$(CROSS)objdump -d $@ > $(OUT)/selftest-thumb.dis

$(OUT)/selftest-arm-be.elf: start.S selftest.c link.ld
	@mkdir -p $(OUT)
	$(CROSS)gcc -marm $(BE) $(FW_CFLAGS) $(FW_LDFLAGS) start.S selftest.c -o $@

$(OUT)/selftest-thumb-be.elf: start.S selftest.c link.ld
	@mkdir -p $(OUT)
	$(CROSS)gcc -mthumb $(BE) $(FW_CFLAGS) $(FW_LDFLAGS) start.S selftest.c -o $@

# Thumb-2 code, so the fuzz harness runs on Thumb pcs (bit 0 set).
$(OUT)/fuzztarget.elf: start.S fuzztarget.c ../fuzz/fuzztarget.h link.ld
	@mkdir -p $(OUT)
	$(CROSS)gcc -mthumb $(FW_CFLAGS) $(FW_LDFLAGS) start.S fuzztarget.c -o $@

$(OUT)/fuzztarget-be.elf: start.S fuzztarget.c ../fuzz/fuzztarget.h link.ld
	@mkdir -p $(OUT)
	$(CROSS)gcc -mthumb $(BE) $(FW_CFLAGS) $(FW_LDFLAGS) start.S fuzztarget.c -o $@

# The same image without its ELF wrapper, for the raw-image path (--raw-base, --entry).
$(OUT)/fuzztarget.bin: $(OUT)/fuzztarget.elf
	$(CROSS)objcopy -O binary $< $@

$(OUT)/fuzztarget-be.bin: $(OUT)/fuzztarget-be.elf
	$(CROSS)objcopy -O binary $< $@

.PHONY: all
