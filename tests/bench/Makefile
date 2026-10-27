# Builds the cross-ISA benchmark (bench.c) for every reference board, with each
# board's own start code, and natively for the host (the reference checksums).
#   ROUNDS=n   rounds per kernel (default: bench.c's BENCH_ROUNDS)
#   ONLY=k     run kernel k only

OUT   ?= ../../build/firmware-bench
HOSTCC ?= cc

RV_CROSS  ?= $(shell for p in riscv64-unknown-elf- riscv64-linux-gnu- riscv32-unknown-elf-; do \
                command -v $${p}gcc >/dev/null 2>&1 && { echo $$p; break; }; done)
MIPS_CROSS   ?= mips-linux-gnu-
MIPSEL_CROSS ?= mipsel-linux-gnu-
ARM_CROSS    ?= arm-linux-gnueabi-
PPC_CROSS    ?= powerpc64le-linux-gnu-
X86_CROSS    ?= x86_64-linux-gnu-

DEFS   = $(if $(ROUNDS),-DBENCH_ROUNDS=$(ROUNDS)) $(if $(ONLY),-DBENCH_ONLY=$(ONLY))
COMMON = -O2 -g -std=gnu11 -Wall -Wextra -ffreestanding -fno-builtin -fno-pic \
         -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables \
         -fno-tree-loop-distribute-patterns -nostdlib -nostartfiles $(DEFS)
LDCOMMON = -static -no-pie -Wl,--build-id=none -Wl,--no-warn-rwx-segments

RV_FLAGS   = -march=rv32imac_zicsr_zifencei -mabi=ilp32 -mcmodel=medany
MIPS_FLAGS = -march=mips32r2 -mabi=32 -mno-abicalls -G0
ARM_FLAGS  = -march=armv7ve -mfloat-abi=soft
PPC_FLAGS  = -m32 -mbig-endian -mcpu=powerpc -mno-altivec -mno-vsx -msdata=none -G0 -Wa,-mregnames
X86_FLAGS  = -m64 -mno-red-zone -mgeneral-regs-only

FW := ../firmware
ALL := $(OUT)/bench-rv32.elf $(OUT)/bench-mips.elf $(OUT)/bench-mipsel.elf \
       $(OUT)/bench-arm.elf $(OUT)/bench-thumb.elf $(OUT)/bench-thumb-be.elf \
       $(OUT)/bench-ppc.elf $(OUT)/bench-x86.bin $(OUT)/bench-host

all: $(ALL)

# Rebuild everything when ROUNDS or ONLY changes.
$(OUT)/.config: FORCE
	@mkdir -p $(OUT)
	@echo '$(DEFS)' | cmp -s - $@ || echo '$(DEFS)' > $@

$(OUT)/bench-host: bench.c $(OUT)/.config
	$(HOSTCC) -O2 -std=gnu11 -Wall -Wextra -DBENCH_HOST $(DEFS) bench.c -o $@

$(OUT)/bench-rv32.elf: bench.c $(FW)/start.S $(OUT)/.config
	@test -n "$(RV_CROSS)" || { echo "no RISC-V cross compiler found"; exit 1; }
	$(RV_CROSS)gcc $(RV_FLAGS) $(COMMON) -static -T $(FW)/link.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments \
	    $(FW)/start.S bench.c -o $@

$(OUT)/bench-mips.elf: bench.c $(FW)-mips/start.S $(OUT)/.config
	$(MIPS_CROSS)gcc $(MIPS_FLAGS) -EB $(COMMON) $(LDCOMMON) -Wl,-T,$(FW)-mips/link.ld \
	    $(FW)-mips/start.S bench.c -o $@

$(OUT)/bench-mipsel.elf: bench.c $(FW)-mips/start.S $(OUT)/.config
	$(MIPSEL_CROSS)gcc $(MIPS_FLAGS) -EL $(COMMON) $(LDCOMMON) -Wl,-T,$(FW)-mips/link.ld \
	    $(FW)-mips/start.S bench.c -o $@

$(OUT)/bench-arm.elf: bench.c $(FW)-arm/start.S $(OUT)/.config
	$(ARM_CROSS)gcc -marm $(ARM_FLAGS) $(COMMON) $(LDCOMMON) -Wl,-T,$(FW)-arm/link.ld \
	    $(FW)-arm/start.S bench.c -o $@

$(OUT)/bench-thumb.elf: bench.c $(FW)-arm/start.S $(OUT)/.config
	$(ARM_CROSS)gcc -mthumb $(ARM_FLAGS) $(COMMON) $(LDCOMMON) -Wl,-T,$(FW)-arm/link.ld \
	    $(FW)-arm/start.S bench.c -o $@

$(OUT)/bench-thumb-be.elf: bench.c $(FW)-arm/start.S $(OUT)/.config
	$(ARM_CROSS)gcc -mthumb -mbig-endian -Wl,--be8 $(ARM_FLAGS) $(COMMON) $(LDCOMMON) \
	    -Wl,-T,$(FW)-arm/link.ld $(FW)-arm/start.S bench.c -o $@

$(OUT)/bench-ppc.elf: bench.c $(FW)-ppc/start.S $(OUT)/.config
	$(PPC_CROSS)gcc $(PPC_FLAGS) $(COMMON) $(LDCOMMON) -Wl,-T,$(FW)-ppc/link.ld \
	    $(FW)-ppc/start.S bench.c -o $@

$(OUT)/bench-x86.elf: bench.c x86-start.S x86.ld $(OUT)/.config
	$(X86_CROSS)gcc $(X86_FLAGS) $(COMMON) $(LDCOMMON) -Wl,-T,x86.ld x86-start.S bench.c -o $@

$(OUT)/bench-x86.bin: $(OUT)/bench-x86.elf
	$(X86_CROSS)objcopy -O binary $< $@

.PHONY: all FORCE
