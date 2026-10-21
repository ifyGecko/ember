# Builds the bare-metal x86-64 self-test: a flat image entered in real mode at
# physical 0 that walks through protected mode into long mode.

NASM ?= nasm
OUT  ?= ../../build/firmware-x86

all: $(OUT)/selftest.bin

$(OUT)/selftest.bin: selftest.asm
	@mkdir -p $(OUT)
	$(NASM) -f bin -l $(OUT)/selftest.lst $< -o $@

.PHONY: all
