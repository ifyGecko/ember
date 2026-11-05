# Builds the Xtensa self-test once per xtensa-ref model (boards/xtensa_ref.c) with that
# core's GNU toolchain: selftest-lx106.elf (xtensa-lx106-elf), selftest-lx6.elf (the
# ESP32's xtensa-esp32-elf), selftest-lx7.elf (the ESP32-S3's xtensa-esp32s3-elf); the
# big-endian builds (-be.bin, raw images at 0x40000000) are the little-endian ones laid
# out the big-endian way by ../isa-xtensa/le2be.py. `make c` builds the C programs: the
# fuzz target (fuzztarget-MODEL.elf).
#   LX106_CROSS / LX6_CROSS / LX7_CROSS  toolchain prefixes

LX106_CROSS ?= xtensa-lx106-elf-
LX6_CROSS   ?= xtensa-esp32-elf-
LX7_CROSS   ?= xtensa-esp32s3-elf-
OUT ?= ../../build/firmware-xtensa

MODELS ?= lx106 lx6 lx7
cross = $($(shell echo $(1) | tr a-z A-Z)_CROSS)
LDFLAGS := -nostdlib -nostartfiles -Wl,--build-id=none -Wl,--no-warn-rwx-segments

all: $(foreach m,$(MODELS),$(OUT)/selftest-$(m).elf $(OUT)/selftest-$(m)-be.bin)
c: $(foreach m,$(MODELS),$(OUT)/fuzztarget-$(m).elf)

$(OUT)/vectors.S: gen_vectors.py
	@mkdir -p $(OUT)
	python3 -I -B gen_vectors.py $@

$(OUT)/selftest-%.elf: selftest.S model.h window.inc selftest.ld $(OUT)/vectors.S
	$(call cross,$*)gcc -DXT_$(shell echo $* | tr a-z A-Z) -I. -I$(OUT) $(LDFLAGS) -T selftest.ld \
	    selftest.S -o $@

$(OUT)/selftest-%-be.elf: selftest.S model.h window.inc selftest.ld $(OUT)/vectors.S
	$(call cross,$*)gcc -DXT_$(shell echo $* | tr a-z A-Z) -DSELFTEST_BE -I. -I$(OUT) $(LDFLAGS) \
	    -T selftest.ld selftest.S -o $@

$(OUT)/selftest-%-be.bin: $(OUT)/selftest-%-be.elf ../isa-xtensa/le2be.py
	python3 -I -B ../isa-xtensa/le2be.py $(call cross,$*)objdump $< $@

C_FLAGS := -O2 -g -std=gnu11 -Wall -Wextra -ffreestanding -fno-builtin -fno-pic \
           -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables
$(OUT)/fuzztarget-lx106.elf: fuzztarget.c start.S window.inc link.ld lx106-libgcc.c ../fuzz/fuzztarget.h
	@mkdir -p $(OUT)
	$(LX106_CROSS)gcc $(C_FLAGS) -mtext-section-literals $(LDFLAGS) -T link.ld start.S fuzztarget.c \
	    lx106-libgcc.c -lgcc -o $@
$(OUT)/fuzztarget-%.elf: fuzztarget.c start.S window.inc link.ld ../fuzz/fuzztarget.h
	@mkdir -p $(OUT)
	$(call cross,$*)gcc $(C_FLAGS) $(LDFLAGS) -T link.ld start.S fuzztarget.c -lgcc -o $@

.SECONDARY:
.PHONY: all c
