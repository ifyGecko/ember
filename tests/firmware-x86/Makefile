# Builds the bare-metal x86-64 self-test: a flat image entered in real mode at
# physical 0 that walks through protected mode into long mode.

NASM ?= nasm
OUT  ?= ../../build/firmware-x86

all: $(OUT)/selftest.bin $(OUT)/smp.bin $(OUT)/legacy.bin $(OUT)/biosrom.bin $(OUT)/smm-amd.bin

$(OUT)/selftest.bin: selftest.asm
	@mkdir -p $(OUT)
	$(NASM) -f bin -l $(OUT)/selftest.lst $< -o $@

.PHONY: all

$(OUT)/smp.bin: smp.asm
	@mkdir -p $(OUT)
	$(NASM) -f bin -l $(OUT)/smp.lst $< -o $@

$(OUT)/legacy.bin: legacy.asm
	@mkdir -p $(OUT)
	$(NASM) -f bin -l $(OUT)/legacy.lst $< -o $@

$(OUT)/biosrom.bin: biosrom.asm
	@mkdir -p $(OUT)
	$(NASM) -f bin -l $(OUT)/biosrom.lst $< -o $@

$(OUT)/smm-amd.bin: smm-amd.asm
	@mkdir -p $(OUT)
	$(NASM) -f bin -l $(OUT)/smm-amd.lst $< -o $@
