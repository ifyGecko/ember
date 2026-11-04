# Builds the PIC32 self-test for the pic32-ref board. The image is MIPS32r2
# (the mz model: TLB, shadow register sets, the external interrupt controller);
# it exercises the parts of the module PIC32 cores add to the MIPS base. The
# MIPS16e and microMIPS instruction sets, the FPU and the DSP ASE are not yet
# lifted (see the ISA module), so there is no mx/mm image here.

CROSS ?= mipsel-linux-gnu-
OUT   ?= ../../build/firmware-pic32

FW_CFLAGS  = -O2 -g -std=gnu11 -Wall -Wextra -ffreestanding -fno-builtin -fno-pic -mno-abicalls \
             -fno-stack-protector -nostdlib -nostartfiles -G0 -EL -mabi=32 -march=mips32r2 -msoft-float
FW_LDFLAGS = -static -no-pie -Wl,-T,link.ld -Wl,--build-id=none

all: $(OUT)/selftest-mz.elf

$(OUT)/selftest-mz.elf: start.S probe.S selftest.c link.ld
	@mkdir -p $(OUT)
	$(CROSS)gcc $(FW_CFLAGS) $(FW_LDFLAGS) -DMODEL_MZ -DMODEL_NAME='"mz"' -DBOOT_UMIPS=0 \
	    start.S probe.S selftest.c -o $@
	$(CROSS)objdump -d $@ > $(OUT)/selftest-mz.dis

.PHONY: all
