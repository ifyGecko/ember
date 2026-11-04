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
AVR_CROSS    ?= avr-
AVR32_CROSS  ?= avr32-
HAVE_AVR32   := $(if $(shell command -v $(AVR32_CROSS)gcc 2>/dev/null),1)
SDCC         ?= sdcc
HAVE_SDCC    := $(if $(shell command -v $(SDCC) 2>/dev/null),1)
# Without the toolchain, images from an earlier build would be tested as current: drop them.
ifneq ($(HAVE_AVR32),1)
$(shell rm -f $(OUT)/bench-avr32a.elf $(OUT)/bench-avr32b.elf)
endif
ifneq ($(HAVE_SDCC),1)
$(shell rm -f $(OUT)/bench-mcs51.bin)
endif

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
       $(OUT)/bench-ppc.elf $(OUT)/bench-x86.bin $(OUT)/bench-avr.elf $(OUT)/bench-host \
       $(OUT)/bench-pic32mx.elf $(OUT)/bench-pic32mm.elf $(OUT)/bench-pic32mz.elf \
       $(if $(HAVE_AVR32),$(OUT)/bench-avr32a.elf $(OUT)/bench-avr32b.elf) \
       $(if $(HAVE_SDCC),$(OUT)/bench-mcs51.bin)

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

# AVR (avr-ref, model eplus): the data does not fit a part's SRAM, so the board runs with
# --ram 0xFE00 (SRAM up to the end of the 64 KiB data space) and the stack starts there.
$(OUT)/bench-avr.elf: bench.c $(OUT)/.config
	$(AVR_CROSS)gcc -mmcu=atmega2560 -O2 -g -std=gnu11 -Wall -Wextra -ffreestanding -fno-builtin \
	    -fno-tree-loop-distribute-patterns $(DEFS) -Wl,--defsym=__DATA_REGION_LENGTH__=0xFE00 \
	    -Wl,--defsym=__stack=0xFFFF bench.c -o $@

# AVR32 (avr32-ref, models a and b), only when an AVR32 toolchain is found (AVR32_CROSS): its
# gcc 4.4 lacks -fno-tree-loop-distribute-patterns, and wants the C locale.
AVR32_FLAGS = -O2 -g -std=gnu99 -Wall -Wextra -ffreestanding -fno-builtin -mno-pic -fno-stack-protector \
              -nostdlib -nostartfiles -static $(DEFS)
$(OUT)/bench-avr32a.elf: bench.c $(FW)-avr32/start.S $(OUT)/.config
	LC_ALL=C $(AVR32_CROSS)gcc -mpart=uc3a0512 $(AVR32_FLAGS) -Wl,-T,$(FW)-avr32/link-a.ld \
	    $(FW)-avr32/start.S bench.c -lgcc -o $@
$(OUT)/bench-avr32b.elf: bench.c $(FW)-avr32/start.S $(OUT)/.config
	LC_ALL=C $(AVR32_CROSS)gcc -mpart=ap7000 $(AVR32_FLAGS) -Wl,-T,$(FW)-avr32/link-b.ld \
	    $(FW)-avr32/start.S bench.c -lgcc -o $@

# 8051 (mcs51-ref, model 8052), only when SDCC is found: its large reentrant model
# (data in external RAM, locals on the internal-RAM stack, whose 128 directly addressed
# bytes the non-reentrant model's spill slots overflow). SDCC ships no 8051 build of its
# 64-bit multiply, so it is compiled here from SDCC's own library source.
SDCC_LIBSRC ?= $(dir $(shell command -v $(SDCC) 2>/dev/null))../share/sdcc/lib/src
MCS51_FLAGS = -mmcs51 --model-large --stack-auto --std-c11 --iram-size 256 --xram-size 0xff00 \
              --code-size 0x10000 $(DEFS)
$(OUT)/bench-mcs51.bin: bench.c $(OUT)/.config
	@mkdir -p $(OUT)/mcs51
	$(SDCC) $(MCS51_FLAGS) -c $(SDCC_LIBSRC)/_mullonglong.c -o $(OUT)/mcs51/mullonglong.rel
	$(SDCC) $(MCS51_FLAGS) -c bench.c -o $(OUT)/mcs51/bench.rel
	$(SDCC) $(MCS51_FLAGS) $(OUT)/mcs51/bench.rel $(OUT)/mcs51/mullonglong.rel -o $(OUT)/mcs51/bench.ihx
	makebin -p $(OUT)/mcs51/bench.ihx $@

# PIC32 (pic32-ref, with the PIC32 self-test's start code and map): MIPS16e on mx,
# microMIPS on mm, MIPS32 with the FPU enabled on mz.
PIC32 = -DBENCH_PIC32 -mabi=32 -mno-abicalls -G0 -EL $(COMMON) $(LDCOMMON) -Wl,-T,pic32.ld \
        -fno-optimize-sibling-calls
$(OUT)/bench-pic32mx.elf: bench.c $(FW)-pic32/start.S pic32.ld $(OUT)/.config
	$(MIPSEL_CROSS)gcc -march=m4k -msoft-float $(PIC32) -c $(FW)-pic32/start.S -o $(OUT)/pic32mx-start.o
	$(MIPSEL_CROSS)gcc -march=m4k -msoft-float -mips16 $(PIC32) $(OUT)/pic32mx-start.o bench.c -o $@
$(OUT)/bench-pic32mm.elf: bench.c $(FW)-pic32/start.S pic32.ld $(OUT)/.config
	$(MIPSEL_CROSS)gcc -march=m14k -mmicromips -msoft-float $(PIC32) $(FW)-pic32/start.S bench.c -o $@
$(OUT)/bench-pic32mz.elf: bench.c $(FW)-pic32/start.S pic32.ld $(OUT)/.config
	$(MIPSEL_CROSS)gcc -march=mips32r2 -mhard-float -mfp64 $(PIC32) $(FW)-pic32/start.S bench.c -o $@

$(OUT)/bench-ppc.elf: bench.c $(FW)-ppc/start.S $(OUT)/.config
	$(PPC_CROSS)gcc $(PPC_FLAGS) $(COMMON) $(LDCOMMON) -Wl,-T,$(FW)-ppc/link.ld \
	    $(FW)-ppc/start.S bench.c -o $@

$(OUT)/bench-x86.elf: bench.c x86-start.S x86.ld $(OUT)/.config
	$(X86_CROSS)gcc $(X86_FLAGS) $(COMMON) $(LDCOMMON) -Wl,-T,x86.ld x86-start.S bench.c -o $@

$(OUT)/bench-x86.bin: $(OUT)/bench-x86.elf
	$(X86_CROSS)objcopy -O binary $< $@

.PHONY: all FORCE
