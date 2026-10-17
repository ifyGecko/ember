# Builds the bare-metal RV32IMAC self-test image.
# Uses any riscv64 GCC (rv64 toolchains build rv32 freestanding code fine).

CROSS ?= $(shell for p in riscv64-unknown-elf- riscv64-linux-gnu- riscv32-unknown-elf-; do \
            command -v $${p}gcc >/dev/null 2>&1 && { echo $$p; break; }; done)
OUT   ?= ../../build/firmware

ARCH_FLAGS := -march=rv32imac_zicsr_zifencei -mabi=ilp32 -mcmodel=medany
FW_CFLAGS     := $(ARCH_FLAGS) -O2 -g -std=gnu11 -Wall -Wextra -ffreestanding -fno-builtin \
              -fno-pic -fno-stack-protector -nostdlib -nostartfiles
FW_LDFLAGS    := -static -T link.ld -Wl,--build-id=none

all: $(OUT)/selftest.elf

$(OUT)/selftest.elf: start.S selftest.c link.ld
	@test -n "$(CROSS)" || { echo "no RISC-V cross compiler found"; exit 1; }
	@mkdir -p $(OUT)
	$(CROSS)gcc $(FW_CFLAGS) $(FW_LDFLAGS) start.S selftest.c -o $@
	$(CROSS)objdump -d $@ > $(OUT)/selftest.dis

.PHONY: all
