# Builds the bare-metal AVR32 self-test once for AVR32A (UC3 class, -mpart=uc3a0512) and
# once for AVR32B (AP7 class, -mpart=ap7000), and the fuzz target for AVR32A.
#
# CROSS is the toolchain prefix (avr32-gcc 4.x from the AVR32 GNU toolchain).

CROSS ?= avr32-
OUT   ?= ../../build/firmware-avr32

# The 4.x toolchain asserts in non-C locales.
export LC_ALL := C

FW_CFLAGS  = -O2 -g -std=gnu99 -Wall -Wextra -ffreestanding -fno-builtin -mno-pic \
             -fno-stack-protector -nostdlib -nostartfiles
FW_LDFLAGS = -static -Wl,--build-id=none
A_FLAGS    = -mpart=uc3a0512 -Wl,-T,link-a.ld
B_FLAGS    = -mpart=ap7000 -DAVR32B -Wl,-T,link-b.ld

all: $(OUT)/selftest-a.elf $(OUT)/selftest-b.elf $(OUT)/fuzztarget-a.elf

$(OUT)/selftest-a.elf: start.S selftest.c link-a.ld
	@mkdir -p $(OUT)
	$(CROSS)gcc $(A_FLAGS) $(FW_CFLAGS) $(FW_LDFLAGS) start.S selftest.c -lgcc -o $@
	$(CROSS)objdump -d $@ > $(OUT)/selftest-a.dis

$(OUT)/selftest-b.elf: start.S selftest.c link-b.ld
	@mkdir -p $(OUT)
	$(CROSS)gcc $(B_FLAGS) $(FW_CFLAGS) $(FW_LDFLAGS) start.S selftest.c -lgcc -o $@
	$(CROSS)objdump -d $@ > $(OUT)/selftest-b.dis

$(OUT)/fuzztarget-a.elf: start.S fuzztarget.c ../fuzz/fuzztarget.h link-a.ld
	@mkdir -p $(OUT)
	$(CROSS)gcc $(A_FLAGS) $(FW_CFLAGS) $(FW_LDFLAGS) start.S fuzztarget.c -o $@

.PHONY: all
