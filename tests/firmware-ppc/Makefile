# Builds the bare-metal 32-bit PowerPC self-test, the fuzz target and the multiprocessor
# self-test. Any GCC for PowerPC works: a 32-bit one (CROSS=powerpc-linux-gnu-) or, as here
# by default, the 64-bit little-endian one told to make 32-bit big-endian code.

CROSS  ?= powerpc64le-linux-gnu-
OUT    ?= ../../build/firmware-ppc

ARCH       = -m32 -mbig-endian -mcpu=powerpc -mno-altivec -mno-vsx -msdata=none -G0
FW_CFLAGS  = $(ARCH) -O2 -g -std=gnu11 -Wall -Wextra -ffreestanding -fno-builtin -fno-pic \
             -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables \
             -nostdlib -nostartfiles -Wa,-mregnames
FW_LDFLAGS = -static -no-pie -Wl,-T,link.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments

all: $(OUT)/selftest.elf $(OUT)/selftest-booke.elf $(OUT)/fuzztarget.elf $(OUT)/fuzztarget.bin \
     $(OUT)/smptest.elf

$(OUT)/selftest.elf: start.S selftest.c link.ld
	@mkdir -p $(OUT)
	$(CROSS)gcc $(FW_CFLAGS) $(FW_LDFLAGS) start.S selftest.c -o $@
	$(CROSS)objdump -d $@ > $(OUT)/selftest.dis

# The same self-test for the Book E model (ppc-ref -o model=booke).
$(OUT)/selftest-booke.elf: start-booke.S selftest.c link-booke.ld
	@mkdir -p $(OUT)
	$(CROSS)gcc $(FW_CFLAGS) -DPPC_BOOKE -Wa,-many $(subst link.ld,link-booke.ld,$(FW_LDFLAGS)) \
	    start-booke.S selftest.c -o $@
	$(CROSS)objdump -d $@ > $(OUT)/selftest-booke.dis

$(OUT)/fuzztarget.elf: start.S fuzztarget.c ../fuzz/fuzztarget.h link.ld
	@mkdir -p $(OUT)
	$(CROSS)gcc $(FW_CFLAGS) $(FW_LDFLAGS) start.S fuzztarget.c -o $@

# The same image without its ELF wrapper, for the raw-image path (--raw-base, --entry).
$(OUT)/fuzztarget.bin: $(OUT)/fuzztarget.elf
	$(CROSS)objcopy -O binary $< $@

$(OUT)/smptest.elf: smpstart.S smptest.c ../smp/smp_test.h link.ld
	@mkdir -p $(OUT)
	$(CROSS)gcc $(FW_CFLAGS) $(FW_LDFLAGS) smpstart.S smptest.c -o $@

.PHONY: all
