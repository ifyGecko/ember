# Builds the PIC32 self-test once per pic32-ref model and instruction set:
#
#   image              board options                  code
#   selftest-mz        -o model=mz                    MIPS32
#   selftest-mx        -o model=mx                    MIPS16e (handlers MIPS32)

CROSS ?= mipsel-linux-gnu-
OUT   ?= ../../build/firmware-pic32

FW_CFLAGS  = -O2 -g -std=gnu11 -Wall -Wextra -ffreestanding -fno-builtin -fno-pic -mno-abicalls \
             -fno-stack-protector -nostdlib -nostartfiles -G0 -EL -mabi=32 \
             -fno-optimize-sibling-calls
FW_LDFLAGS = -static -no-pie -Wl,-T,link.ld -Wl,--build-id=none

all: $(OUT)/selftest-mz.elf $(OUT)/selftest-mx.elf

SRC = start.S probe.S selftest.c link.ld

$(OUT)/selftest-mz.elf: $(SRC)
	@mkdir -p $(OUT)
	$(CROSS)gcc -march=mips32r2 -msoft-float $(FW_CFLAGS) $(FW_LDFLAGS) -DMODEL_MZ \
	    -DMODEL_NAME='"mz"' -DBOOT_UMIPS=0 start.S probe.S selftest.c -o $@
	$(CROSS)objdump -d $@ > $(OUT)/selftest-mz.dis

# MIPS16e: C and the probe in MIPS16e, the vectors (start.S) in MIPS32.
$(OUT)/selftest-mx.elf: $(SRC)
	@mkdir -p $(OUT)
	$(CROSS)gcc -march=m4k -msoft-float $(FW_CFLAGS) -c start.S -o $(OUT)/start-mx.o
	$(CROSS)gcc -march=m4k -msoft-float -mips16 $(FW_CFLAGS) -c probe.S -o $(OUT)/probe-mx.o
	$(CROSS)gcc -march=m4k -msoft-float -mips16 $(FW_CFLAGS) -DMODEL_MX -DMODEL_NAME='"mx"' \
	    -DBOOT_UMIPS=0 -c selftest.c -o $(OUT)/selftest-mx.o
	$(CROSS)gcc -march=m4k -msoft-float $(FW_CFLAGS) $(FW_LDFLAGS) $(OUT)/start-mx.o \
	    $(OUT)/probe-mx.o $(OUT)/selftest-mx.o -o $@
	$(CROSS)objdump -d $@ > $(OUT)/selftest-mx.dis

.PHONY: all
