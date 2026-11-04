# UAM firmware emulator - top-level build.
#
#   make                 build $(BUILD)/uam-run and the libraries
#   make lib             only the libraries (for out-of-tree projects)
#   make test            unit tests, firmware self-tests (RISC-V, MIPS BE/LE, ARM and Thumb LE/BE8,
#                        x86-64 real/protected/long mode, 32-bit PowerPC, 8-bit AVR in five core families,
#                        AVR32A/B when an AVR32 toolchain is found: AVR32_CROSS, 8-bit PIC in four
#                        core families when gputils is: GPASM, the 8051 in three core models when
#                        as31 (and, for its fuzz target, SDCC) is found),
#                        external-board example,
#                        snapshot round trips through the runner, coverage maps across backends,
#                        the fuzz harness on planted-bug firmware
#   make suzzer          clone suzzer into ./suzzer (ignored by git) for the suzzer integration
#   make net-check       boot the network test on rv32-test against a real tap and tun interface
#                        (rootless: in a user+network namespace; skipped if unavailable)
#   make install         install headers, libraries, uam-run and uam.pc under $(PREFIX)
#   make clean
#
# suzzer (optional; docs/suzzer.md): ember works on its own. To use it with suzzer, either point
# the build at an existing checkout with SUZZER_DIR=path, or run `make suzzer` once to clone one
# into ./suzzer, which is then picked up automatically. Without either, the suzzer options
# (--cov-sites, --cov-boundary, ...) are left out and their tests are skipped. The choice is
# remembered in $(BUILD)/suzzer.cfg: a later plain `make` keeps it; `make SUZZER_DIR=` turns the
# integration off until a path is given again.
#
# Out-of-tree use: `make -C path/to/uam BUILD=$PWD/build/uam lib` builds into another directory;
# link $(BUILD)/libuam.a with -I path/to/uam/include (or use uam.pc after `make install`).
#
# Libraries, one per module group (each is also contained in libuam.a):
#   libuam-core.a     IR, bus, scheduler, signals/chardev/netdev plumbing, executor, analysis passes,
#                     loader, machine
#   libuam-riscv.a    RISC-V ISA module          libuam-mips.a   MIPS32 ISA module
#   libuam-arm.a      ARM (AArch32) ISA module   libuam-ppc.a    32-bit PowerPC ISA module
#   libuam-avr.a      8-bit AVR ISA module       libuam-avr32.a  AVR32 (A and B) ISA module
#   libuam-pic.a      8-bit PIC ISA module
#   libuam-mcs51.a    8051 (MCS-51) ISA module
#   libuam-devices.a  stock device models        libuam-boards.a stock boards (rv32-ref, mips-ref, ...)
#   libuam-fuzz.a     the fuzz harness and its frontends (fuzz/)
#   libuam-host.a     host backends, gdb stub, the stock runner (uam_run_main; uses libuam-fuzz)

CC      ?= cc
AR      ?= ar
CFLAGS  ?= -O2 -g    # tunable from the command line, e.g. CFLAGS="-O1 -fsanitize=address"
LDFLAGS ?=
LDLIBS  := -lm      # the x87 model uses long-double libm functions
PREFIX  ?= /usr/local
DESTDIR ?=
# Required flags, always applied.
UAM_COMPILE := -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -Wshadow -Wstrict-prototypes \
               -Wmissing-prototypes -Iinclude $(CFLAGS)

BUILD ?= build

# The AVR32 firmware needs avr32-gcc (the 4.x AVR32 GNU toolchain), which few hosts have:
# AVR32_CROSS is its prefix; without it the AVR32 firmware tests are skipped.
AVR32_CROSS ?= avr32-
HAVE_AVR32  := $(if $(shell command -v $(AVR32_CROSS)gcc 2>/dev/null),1)
AVR32_SKIP  = @echo "$@: AVR32 firmware skipped (no $(AVR32_CROSS)gcc; set AVR32_CROSS=prefix)"
# The 8-bit PIC firmware is assembled with gpasm (gputils); without it those tests are skipped.
GPASM       ?= gpasm
HAVE_GPASM  := $(if $(shell command -v $(GPASM) 2>/dev/null),1)
PIC_SKIP    = @echo "$@: PIC firmware skipped (no $(GPASM): install gputils, or set GPASM=path)"

# The 8051 self-tests need the as31 assembler, its fuzz target SDCC; without them they
# are skipped.
AS31       ?= as31
SDCC       ?= sdcc
HAVE_AS31  := $(if $(shell command -v $(AS31) 2>/dev/null),1)
HAVE_SDCC  := $(if $(shell command -v $(SDCC) 2>/dev/null),1)
MCS51_SKIP  = @echo "$@: 8051 firmware skipped (no $(AS31); set AS31=path)"
SDCC_SKIP   = @echo "$@: 8051 fuzz target skipped (no $(SDCC); set SDCC=path)"

# ---- suzzer ------------------------------------------------------------------------------
# SUZZER_DIR: an existing suzzer checkout to use in place; empty turns the integration off.
# Not given: the choice remembered in $(BUILD)/suzzer.cfg (a checkout's path, or "off"), else
# ./suzzer if `make suzzer` cloned one there. Ember needs the format header suzzer publishes
# (src/lcv_formats.h). SUZZER_URL unset: `make suzzer` tries SSH, then HTTPS.
SUZZER_URL ?=
SUZZER_REV ?=
SUZZER_SAVED := $(strip $(shell cat $(BUILD)/suzzer.cfg 2>/dev/null))
ifeq ($(origin SUZZER_DIR),undefined)
ifeq ($(SUZZER_SAVED),off)
SUZZER_DIR :=
else ifneq ($(and $(SUZZER_SAVED),$(wildcard $(SUZZER_SAVED)/src/lcv_formats.h)),)
SUZZER_DIR := $(SUZZER_SAVED)
else
ifneq ($(SUZZER_SAVED),)
$(warning the suzzer checkout this build used ($(SUZZER_SAVED)) is gone; looking for ./suzzer)
endif
SUZZER_DIR := $(if $(wildcard suzzer/src/lcv_formats.h),suzzer,)
endif
SUZZER_RECORD := $(if $(SUZZER_DIR),$(abspath $(SUZZER_DIR)),$(if $(filter off,$(SUZZER_SAVED)),off,))
else ifneq ($(SUZZER_DIR),)
ifeq ($(wildcard $(SUZZER_DIR)/src/lcv_formats.h),)
$(error SUZZER_DIR=$(SUZZER_DIR) has no src/lcv_formats.h: not a suzzer checkout, or one older than its published format header)
endif
SUZZER_RECORD := $(abspath $(SUZZER_DIR))
else
SUZZER_RECORD := off
endif
HAVE_SUZZER := $(if $(SUZZER_DIR),1,)

# One list per module, mirroring the dependency rules in docs/architecture.md.
IR_SRC      := src/ir/builder.c src/ir/verify.c src/ir/print.c src/ir/opt.c src/ir/pass.c
CORE_SRC    := src/core/common.c src/core/bus.c src/core/sched.c src/core/chardev.c src/core/netdev.c \
               src/core/state.c
EXEC_SRC    := src/exec/interp.c src/exec/tcache.c src/exec/run.c src/exec/cpu.c src/exec/jit.c \
               src/exec/jit_x86.c src/exec/jit_a64.c src/exec/coverage.c src/exec/spin.c
ANALYSIS_SRC := analysis/cov.c
LOADER_SRC  := src/loader/loader.c
MACHINE_SRC := src/machine/machine.c src/machine/snapshot.c src/machine/guard.c
RISCV_SRC   := isa/riscv/rv_lift.c isa/riscv/rv_system.c
MIPS_SRC    := isa/mips32/mips_lift.c isa/mips32/mips_system.c isa/mips32/mips16.c \
               isa/mips32/mips_umips.c isa/mips32/mips_fpu.c isa/mips32/mips_dsp.c
ARM_SRC     := isa/arm/arm_lift.c isa/arm/arm_thumb.c isa/arm/arm_system.c
PPC_SRC     := isa/ppc32/ppc_lift.c isa/ppc32/ppc_fpu.c isa/ppc32/ppc_mmu.c isa/ppc32/ppc_booke.c \
               isa/ppc32/ppc_system.c
AVR_SRC     := isa/avr8/avr_lift.c isa/avr8/avr_system.c isa/avr8/avr_des.c
AVR32_SRC   := isa/avr32/avr32_lift.c isa/avr32/avr32_dec.c isa/avr32/avr32_dsp.c isa/avr32/avr32_system.c
PIC_SRC     := isa/pic8/pic_lift.c isa/pic8/pic_system.c
MCS51_SRC   := isa/mcs51/mcs51_lift.c isa/mcs51/mcs51_system.c
X86_SRC     := isa/x86/x86_decode.c isa/x86/x86_f80.c isa/x86/x86_lift.c isa/x86/x86_lift_ops.c isa/x86/x86_lift0f.c \
               isa/x86/x86_string.c isa/x86/x86_sse.c isa/x86/x86_sse_ops.c isa/x86/x86_sse_fp.c isa/x86/x86_x87.c isa/x86/x86_helpers.c \
               isa/x86/x86_mmu.c isa/x86/x86_seg.c isa/x86/x86_msr.c isa/x86/x86_smm.c \
               isa/x86/x86_gdb.c isa/x86/x86_system.c isa/x86/x86_task.c
DEV_SRC     := devices/clint.c devices/plic.c devices/uart16550.c devices/test_finisher.c \
               devices/unknown_mmio.c devices/unimp.c devices/virtio_net.c devices/mmio_stub.c \
               devices/gic.c devices/i8259.c devices/i8254.c devices/mc146818.c devices/lapic.c \
               devices/ioapic.c devices/pci.c devices/ata_core.c devices/ahci.c devices/ide.c devices/nvme.c \
               devices/cfi_flash.c devices/pc_misc.c devices/acpi_pm.c devices/ipi.c devices/i8237.c devices/vga.c devices/fdc.c devices/lpt.c \
               devices/i8042.c devices/hpet.c devices/gptimer.c devices/avr32_intc.c \
               devices/mcs51_timer.c devices/mcs51_serial.c devices/mcs51_port.c devices/pic32_intc.c
BOARD_SRC   := boards/rv32_ref.c boards/mips_ref.c boards/arm_ref.c boards/x86_64_ref.c boards/ppc_ref.c \
               boards/avr_ref.c boards/avr32_ref.c boards/pic_ref.c boards/mcs51_ref.c boards/pic32_ref.c \
               boards/stock.c boards/mmio_rules.c
SUZZER_SRC  := fuzz/suzzer/scheme.c fuzz/suzzer/import.c fuzz/suzzer/cli.c \
               fuzz/suzzer/frontend_szr1.c fuzz/suzzer/lcvs.c
FUZZ_SRC    := fuzz/harness.c fuzz/frontend_replay.c fuzz/stock_exts.c \
               $(if $(HAVE_SUZZER),$(SUZZER_SRC))
HOST_SRC    := src/host/chardev_backends.c src/host/netdev_backends.c src/host/blockdev_file.c \
               src/host/hostloop.c \
               src/debug/gdbstub.c src/runner/runner.c

CORE_ALL := $(IR_SRC) $(CORE_SRC) $(EXEC_SRC) $(ANALYSIS_SRC) $(LOADER_SRC) $(MACHINE_SRC)
LIB_SRC  := $(CORE_ALL) $(RISCV_SRC) $(MIPS_SRC) $(ARM_SRC) $(X86_SRC) $(PPC_SRC) $(AVR_SRC) $(AVR32_SRC) $(PIC_SRC) $(MCS51_SRC) $(DEV_SRC) $(BOARD_SRC) $(FUZZ_SRC) $(HOST_SRC)
obj = $(1:%.c=$(BUILD)/obj/%.o)
LIB_OBJ  := $(call obj,$(LIB_SRC))

LIB        := $(BUILD)/libuam.a
MODULE_LIBS := $(BUILD)/libuam-core.a $(BUILD)/libuam-riscv.a $(BUILD)/libuam-mips.a \
               $(BUILD)/libuam-arm.a $(BUILD)/libuam-x86.a $(BUILD)/libuam-ppc.a $(BUILD)/libuam-avr.a \
               $(BUILD)/libuam-avr32.a $(BUILD)/libuam-pic.a $(BUILD)/libuam-mcs51.a \
               $(BUILD)/libuam-devices.a $(BUILD)/libuam-boards.a $(BUILD)/libuam-fuzz.a \
               $(BUILD)/libuam-host.a
RUN       := $(BUILD)/uam-run
# uam-run plus the boards only ember's tests use (tests/boards/): the reference boards with
# the generic components they leave out (the 0xB2 SMI port, storage, VGA, printer, virtio-net)
TRUN      := $(BUILD)/uam-test-run
TEST_IR   := $(BUILD)/test_ir
TEST_CORE := $(BUILD)/test_core
TEST_HOST := $(BUILD)/test_host
TEST_SNAP := $(BUILD)/test_snapshot
TEST_JIT  := $(BUILD)/test_jit
TEST_SPIN := $(BUILD)/test_spin
TEST_CODESTORE := $(BUILD)/test_codestore
TEST_MMIO := $(BUILD)/test_mmio
TEST_COV  := $(BUILD)/test_cov
TEST_SMP  := $(BUILD)/test_smp
TEST_PPC  := $(BUILD)/test_ppc
TEST_JITDIFF := $(BUILD)/test_jitdiff
TEST_F80  := $(BUILD)/test_f80
TEST_AVR  := $(BUILD)/test_avr
TEST_PIC  := $(BUILD)/test_pic
TEST_MCS51 := $(BUILD)/test_mcs51
TEST_FUZZ := $(BUILD)/test_fuzz
TEST_SUZZER := $(BUILD)/test_suzzer
FIRMWARE  := $(BUILD)/firmware/selftest.elf
FW_MIPS_BE := $(BUILD)/firmware-mips/selftest-be.elf
FW_MIPS_EL := $(BUILD)/firmware-mips/selftest-el.elf
FW_ARM    := $(BUILD)/firmware-arm/selftest-arm.elf
FW_THUMB  := $(BUILD)/firmware-arm/selftest-thumb.elf
FW_ARM_BE := $(BUILD)/firmware-arm/selftest-arm-be.elf
FW_THUMB_BE := $(BUILD)/firmware-arm/selftest-thumb-be.elf
FW_X86    := $(BUILD)/firmware-x86/selftest.bin
FW_PPC    := $(BUILD)/firmware-ppc/selftest.elf
FW_PPC_BOOKE := $(BUILD)/firmware-ppc/selftest-booke.elf
FW_PIC32  := $(BUILD)/firmware-pic32/selftest-mz.elf
FW_PIC32_MX := $(BUILD)/firmware-pic32/selftest-mx.elf
MIPS16_DEC := $(BUILD)/mips16-dec
FW_AVR    := $(BUILD)/firmware-avr
AVR_MODELS := e eplus xm xt rc
FW_AVR32_A := $(BUILD)/firmware-avr32/selftest-a.elf
FW_AVR32_B := $(BUILD)/firmware-avr32/selftest-b.elf
FW_PIC    := $(BUILD)/firmware-pic
PIC_MODELS := baseline mid enhanced pic18
FW_MCS51  := $(BUILD)/firmware-mcs51
MCS51_MODELS := 8051 8052 cip51
EXAMPLE   := $(BUILD)/examples/external-board/ext-run

.PHONY: all lib test runner-test unit-test firmware-test firmware firmware-mips firmware-arm mips-test arm-test \
        firmware-x86 x86-test firmware-ppc ppc-test firmware-avr avr-test firmware-avr32 avr32-test firmware-pic pic-test \
        firmware-mcs51 mcs51-test firmware-pic32 pic32-test \
        example-test \
        snapshot-test cov-test fuzz-test suzzer-test suzzer net-check gdb-test jitdiff-soak \
        bench-test xbench install \
        clean FORCE

all: $(RUN) lib

lib: $(LIB) $(MODULE_LIBS) $(BUILD)/uam.pc

$(BUILD)/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(UAM_COMPILE) -MMD -MP -c $< -o $@

# fuzz/suzzer/ alone sees suzzer, and only its format header (src/lcv_formats.h).
$(BUILD)/obj/fuzz/suzzer/%.o: fuzz/suzzer/%.c $(BUILD)/suzzer.cfg
	@mkdir -p $(dir $@)
	$(CC) $(UAM_COMPILE) -I$(SUZZER_DIR)/src -MMD -MP -c $< -o $@

# The stock runner extensions include the suzzer one only when suzzer is configured.
$(BUILD)/obj/fuzz/stock_exts.o: fuzz/stock_exts.c $(BUILD)/suzzer.cfg
	@mkdir -p $(dir $@)
	$(CC) $(UAM_COMPILE) $(if $(HAVE_SUZZER),-DUAM_HAVE_SUZZER) -MMD -MP -c $< -o $@

# Which suzzer the objects were built against (a path, "off" when turned off with
# SUZZER_DIR=, empty when none was found): the choice a later plain `make` keeps. Rewritten
# only when that changes, so switching SUZZER_DIR (or cloning one) rebuilds what depends on it.
$(BUILD)/suzzer.cfg: FORCE
	@mkdir -p $(BUILD)
	@echo '$(SUZZER_RECORD)' | cmp -s - $@ || echo '$(SUZZER_RECORD)' > $@

$(LIB): $(LIB_OBJ)
	@rm -f $@
	$(AR) rcs $@ $^

$(BUILD)/libuam-core.a:    $(call obj,$(CORE_ALL))
$(BUILD)/libuam-riscv.a:   $(call obj,$(RISCV_SRC))
$(BUILD)/libuam-mips.a:    $(call obj,$(MIPS_SRC))
$(BUILD)/libuam-arm.a:     $(call obj,$(ARM_SRC))
$(BUILD)/libuam-x86.a:     $(call obj,$(X86_SRC))
$(BUILD)/libuam-ppc.a:     $(call obj,$(PPC_SRC))
$(BUILD)/libuam-avr.a:     $(call obj,$(AVR_SRC))
$(BUILD)/libuam-avr32.a:   $(call obj,$(AVR32_SRC))
$(BUILD)/libuam-pic.a:     $(call obj,$(PIC_SRC))
$(BUILD)/libuam-mcs51.a:   $(call obj,$(MCS51_SRC))
$(BUILD)/libuam-devices.a: $(call obj,$(DEV_SRC))
$(BUILD)/libuam-boards.a:  $(call obj,$(BOARD_SRC))
$(BUILD)/libuam-fuzz.a:    $(call obj,$(FUZZ_SRC))
$(BUILD)/libuam-host.a:    $(call obj,$(HOST_SRC))
$(MODULE_LIBS):
	@rm -f $@
	$(AR) rcs $@ $^

# pkg-config file for prefix $(1), written to $(2).
define write_pc
	printf 'prefix=%s\nincludedir=$${prefix}/include\nlibdir=$${prefix}/lib\n\nName: uam\nDescription: UAM firmware emulator library\nVersion: 0.2\nCflags: -I$${includedir}\nLibs: -L$${libdir} -luam -lm\n' '$(1)' > $(2)
endef

$(BUILD)/uam.pc: Makefile
	@mkdir -p $(BUILD)
	$(call write_pc,$(PREFIX),$@)

$(RUN): $(BUILD)/obj/tools/uam-run.o $(LIB)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(TRUN): $(BUILD)/obj/tests/boards/uam-test-run.o $(BUILD)/obj/tests/boards/test_boards.o $(LIB)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# The MIPS16e decoder's oracle harness sees the module's internal header.
$(BUILD)/obj/tests/isa-mips/mips16-dec.o: UAM_COMPILE += -Iisa/mips32
$(MIPS16_DEC): $(BUILD)/obj/tests/isa-mips/mips16-dec.o $(LIB)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(TEST_IR): $(BUILD)/obj/tests/unit/test_ir.o $(LIB)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(TEST_CORE): $(BUILD)/obj/tests/unit/test_core.o $(LIB)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(TEST_HOST): $(BUILD)/obj/tests/unit/test_host.o $(LIB)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(TEST_SNAP): $(BUILD)/obj/tests/unit/test_snapshot.o $(LIB)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(TEST_JIT): $(BUILD)/obj/tests/unit/test_jit.o $(LIB)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(TEST_SPIN): $(BUILD)/obj/tests/unit/test_spin.o $(LIB)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(TEST_CODESTORE): $(BUILD)/obj/tests/unit/test_codestore.o $(LIB)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(TEST_MMIO): $(BUILD)/obj/tests/unit/test_mmio.o $(LIB)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(TEST_COV): $(BUILD)/obj/tests/unit/test_cov.o $(LIB)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(TEST_FUZZ): $(BUILD)/obj/tests/unit/test_fuzz.o $(LIB)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(TEST_SMP): $(BUILD)/obj/tests/unit/test_smp.o $(LIB)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(TEST_PPC): $(BUILD)/obj/tests/unit/test_ppc.o $(LIB)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(TEST_JITDIFF): $(BUILD)/obj/tests/unit/test_jitdiff.o $(LIB)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(TEST_F80): $(BUILD)/obj/tests/unit/test_f80.o $(LIB)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(TEST_AVR): $(BUILD)/obj/tests/unit/test_avr.o $(LIB)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(TEST_PIC): $(BUILD)/obj/tests/unit/test_pic.o $(LIB)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(TEST_MCS51): $(BUILD)/obj/tests/unit/test_mcs51.o $(LIB)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# test_suzzer checks fuzz/suzzer/ against suzzer's conformance vectors (its test/vectors).
$(BUILD)/obj/tests/unit/test_suzzer.o: tests/unit/test_suzzer.c $(BUILD)/suzzer.cfg
	@mkdir -p $(dir $@)
	$(CC) $(UAM_COMPILE) -Ifuzz/suzzer -I$(SUZZER_DIR)/src -MMD -MP -c $< -o $@

$(TEST_SUZZER): $(BUILD)/obj/tests/unit/test_suzzer.o $(LIB)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# suzzer's format header must build on its own with ember's strict flags.
$(BUILD)/suzzer-header-check.stamp: $(SUZZER_DIR)/src/lcv_formats.h $(BUILD)/suzzer.cfg
	@mkdir -p $(BUILD)
	printf '#include "lcv_formats.h"\n' | $(CC) $(UAM_COMPILE) -I$(SUZZER_DIR)/src -x c -c - -o /dev/null
	@touch $@

# Clone suzzer into ./suzzer (SUZZER_URL, SUZZER_REV); a no-op if one is already there.
suzzer:
	SUZZER_URL='$(SUZZER_URL)' SUZZER_REV='$(SUZZER_REV)' sh tools/fetch-suzzer.sh suzzer

# The suzzer tests, or a note that they were skipped.
ifeq ($(HAVE_SUZZER),1)
SUZZER_UNIT := $(TEST_SUZZER) $(BUILD)/suzzer-header-check.stamp
else
SUZZER_UNIT :=
endif

firmware:
	$(MAKE) -C tests/firmware OUT=$(abspath $(BUILD))/firmware

firmware-mips:
	$(MAKE) -C tests/firmware-mips OUT=$(abspath $(BUILD))/firmware-mips

firmware-arm:
	$(MAKE) -C tests/firmware-arm OUT=$(abspath $(BUILD))/firmware-arm

firmware-x86:
	$(MAKE) -C tests/firmware-x86 OUT=$(abspath $(BUILD))/firmware-x86

firmware-ppc:
	$(MAKE) -C tests/firmware-ppc OUT=$(abspath $(BUILD))/firmware-ppc

firmware-pic32:
	$(MAKE) -C tests/firmware-pic32 OUT=$(abspath $(BUILD))/firmware-pic32

firmware-avr:
	$(MAKE) -C tests/firmware-avr OUT=$(abspath $(FW_AVR))

firmware-mcs51:
ifeq ($(HAVE_AS31),1)
	$(MAKE) -C tests/firmware-mcs51 AS31=$(AS31) OUT=$(abspath $(FW_MCS51))
else
	$(MCS51_SKIP)
endif
ifeq ($(HAVE_SDCC),1)
	$(MAKE) -C tests/firmware-mcs51 SDCC=$(SDCC) OUT=$(abspath $(FW_MCS51)) fuzz
else
	$(SDCC_SKIP)
endif

firmware-avr32:
ifeq ($(HAVE_AVR32),1)
	$(MAKE) -C tests/firmware-avr32 CROSS=$(AVR32_CROSS) OUT=$(abspath $(BUILD))/firmware-avr32
else
	$(AVR32_SKIP)
endif

firmware-pic:
ifeq ($(HAVE_GPASM),1)
	$(MAKE) -C tests/firmware-pic GPASM=$(GPASM) OUT=$(abspath $(FW_PIC))
else
	$(PIC_SKIP)
endif

$(FIRMWARE): firmware

unit-test: $(TEST_IR) $(TEST_CORE) $(TEST_HOST) $(TEST_SNAP) $(TEST_JIT) $(TEST_SPIN) $(TEST_CODESTORE) $(TEST_MMIO) $(TEST_COV) $(TEST_FUZZ) \
           $(TEST_SMP) $(TEST_PPC) $(TEST_AVR) $(TEST_PIC) $(TEST_MCS51) $(TEST_JITDIFF) $(TEST_F80) \
           $(SUZZER_UNIT)
	$(TEST_IR)
	$(TEST_CORE)
	$(TEST_HOST)
	$(TEST_SNAP)
	$(TEST_JIT)
	$(TEST_SPIN)
	$(TEST_CODESTORE)
	$(TEST_MMIO)
	$(TEST_COV)
	$(TEST_FUZZ)
	$(TEST_SMP)
	$(TEST_PPC)
	$(TEST_AVR)
	$(TEST_PIC)
	$(TEST_MCS51)
	$(TEST_JITDIFF)
	UAM_JIT_BASELINE=1 $(TEST_JITDIFF) 1000 2
	UAM_JIT_REGS=1 $(TEST_JITDIFF) 1000 3
	$(TEST_F80)
ifeq ($(HAVE_SUZZER),1)
	$(TEST_SUZZER) $(SUZZER_DIR)/test/vectors
else
	@echo "test_suzzer: skipped (no suzzer: make suzzer, or SUZZER_DIR=path)"
endif

# The JIT differential test at length: JITDIFF_BLOCKS random blocks per seed, several seeds.
JITDIFF_BLOCKS ?= 200000
jitdiff-soak: $(TEST_JITDIFF)
	$(TEST_JITDIFF) $(JITDIFF_BLOCKS) 1
	$(TEST_JITDIFF) $(JITDIFF_BLOCKS) 2
	$(TEST_JITDIFF) $(JITDIFF_BLOCKS) 3
	UAM_JIT_REGS=1 $(TEST_JITDIFF) $(JITDIFF_BLOCKS) 4
	UAM_JIT_REGS=2 $(TEST_JITDIFF) $(JITDIFF_BLOCKS) 5

# Each self-test runs on the interpreter and on the JIT.
firmware-test: $(RUN) firmware
	$(RUN) --stats $(FIRMWARE) < /dev/null
	$(RUN) --jit --stats $(FIRMWARE) < /dev/null
	$(RUN) -o cpus=4 $(dir $(FIRMWARE))smptest.elf < /dev/null
	$(RUN) -o cpus=4 --jit $(dir $(FIRMWARE))smptest.elf < /dev/null

mips-test: $(RUN) firmware-mips
	$(RUN) -b mips-ref --stats $(FW_MIPS_BE) < /dev/null
	$(RUN) -b mipsel-ref --stats $(FW_MIPS_EL) < /dev/null
	$(RUN) -b mips-ref --jit --stats $(FW_MIPS_BE) < /dev/null
	$(RUN) -b mipsel-ref --jit --stats $(FW_MIPS_EL) < /dev/null
	$(RUN) -b mips-ref -o cpus=4 $(dir $(FW_MIPS_BE))smptest-be.elf < /dev/null
	$(RUN) -b mipsel-ref -o cpus=4 --jit $(dir $(FW_MIPS_EL))smptest-el.elf < /dev/null

arm-test: $(RUN) firmware-arm
	$(RUN) -b arm-ref --stats $(FW_ARM) < /dev/null
	$(RUN) -b arm-ref --stats $(FW_THUMB) < /dev/null
	$(RUN) -b armeb-ref --stats $(FW_ARM_BE) < /dev/null
	$(RUN) -b armeb-ref --stats $(FW_THUMB_BE) < /dev/null
	$(RUN) -b arm-ref --jit --stats $(FW_ARM) < /dev/null
	$(RUN) -b arm-ref --jit --stats $(FW_THUMB) < /dev/null
	$(RUN) -b armeb-ref --jit --stats $(FW_ARM_BE) < /dev/null
	$(RUN) -b armeb-ref --jit --stats $(FW_THUMB_BE) < /dev/null
	$(RUN) -b arm-ref -o cpus=4 $(dir $(FW_ARM))smptest-arm.elf < /dev/null
	$(RUN) -b armeb-ref -o cpus=4 --jit $(dir $(FW_ARM))smptest-thumb-be.elf < /dev/null

# A flat image entered in real mode at 0; interpreter and JIT. The self-test and the AMD SMM
# test raise SMIs through the 0xB2 port, and the BIOS boot needs storage, VGA and a printer:
# x86_64-test (tests/boards/).
x86-test: $(RUN) $(TRUN) firmware-x86
	$(TRUN) -b x86_64-test --raw-base 0 --entry 0 --stats $(FW_X86) < /dev/null
	$(TRUN) -b x86_64-test --raw-base 0 --entry 0 --jit --stats $(FW_X86) < /dev/null
	$(RUN) -b x86_64-ref --raw-base 0 --entry 0 $(dir $(FW_X86))legacy.bin < /dev/null
	$(RUN) -b x86_64-ref --raw-base 0 --entry 0 --jit $(dir $(FW_X86))legacy.bin < /dev/null
	$(TRUN) -b x86_64-test -o smm-map=amd64 --raw-base 0 --entry 0 $(dir $(FW_X86))smm-amd.bin < /dev/null
	$(TRUN) -b x86_64-test -o smm-map=amd64 --raw-base 0 --entry 0 --jit $(dir $(FW_X86))smm-amd.bin < /dev/null
	python3 tests/firmware-x86/bios-check.py $(TRUN) $(dir $(FW_X86))biosrom.bin
	$(RUN) -b x86_64-ref -o cpus=4 --raw-base 0 --entry 0 $(dir $(FW_X86))smp.bin < /dev/null
	$(RUN) -b x86_64-ref -o cpus=4 --raw-base 0 --entry 0 --jit $(dir $(FW_X86))smp.bin < /dev/null

ppc-test: $(RUN) firmware-ppc
	$(RUN) -b ppc-ref --stats $(FW_PPC) < /dev/null
	$(RUN) -b ppc-ref --jit --stats $(FW_PPC) < /dev/null
	$(RUN) -b ppc-ref -o cpus=4 $(dir $(FW_PPC))smptest.elf < /dev/null
	$(RUN) -b ppc-ref -o cpus=4 --jit $(dir $(FW_PPC))smptest.elf < /dev/null
	$(RUN) -b ppc-ref -o model=booke --stats $(FW_PPC_BOOKE) < /dev/null
	$(RUN) -b ppc-ref -o model=booke --jit --stats $(FW_PPC_BOOKE) < /dev/null

# PIC32: the self-test as MIPS32 on the mz model (TLB, shadow sets, the external
# interrupt controller) and as MIPS16e on mx; the MIPS16e decoder against objdump.
pic32-test: $(RUN) firmware-pic32 $(MIPS16_DEC)
	python3 -I tests/isa-mips/mips16-check.py $(MIPS16_DEC) $(BUILD)
	$(RUN) -b pic32-ref -o model=mz --stats $(FW_PIC32) < /dev/null
	$(RUN) -b pic32-ref -o model=mz --jit --stats $(FW_PIC32) < /dev/null
	$(RUN) -b pic32-ref -o model=mx --stats $(FW_PIC32_MX) < /dev/null
	$(RUN) -b pic32-ref -o model=mx --jit --stats $(FW_PIC32_MX) < /dev/null

# The 8-bit AVR self-test on every core family the board models.
avr-test: $(RUN) firmware-avr
	@set -e; for m in $(AVR_MODELS); do \
	    echo "$(RUN) -b avr-ref -o model=$$m [--jit] --stats $(FW_AVR)/selftest-$$m.elf"; \
	    $(RUN) -b avr-ref -o model=$$m --stats $(FW_AVR)/selftest-$$m.elf < /dev/null; \
	    $(RUN) -b avr-ref -o model=$$m --jit --stats $(FW_AVR)/selftest-$$m.elf < /dev/null; \
	done

# The 8051 self-test on every core the board models.
mcs51-test: $(RUN) firmware-mcs51
ifeq ($(HAVE_AS31),1)
	@set -e; for m in $(MCS51_MODELS); do \
	    echo "$(RUN) -b mcs51-ref -o model=$$m [--jit] --stats $(FW_MCS51)/selftest-$$m.bin"; \
	    $(RUN) -b mcs51-ref -o model=$$m --stats $(FW_MCS51)/selftest-$$m.bin < /dev/null; \
	    $(RUN) -b mcs51-ref -o model=$$m --jit --stats $(FW_MCS51)/selftest-$$m.bin < /dev/null; \
	done
else
	$(MCS51_SKIP)
endif

avr32-test: $(RUN) firmware-avr32
ifeq ($(HAVE_AVR32),1)
	$(RUN) -b avr32-ref --stats $(FW_AVR32_A) < /dev/null
	$(RUN) -b avr32-ref --jit --stats $(FW_AVR32_A) < /dev/null
	$(RUN) -b avr32-ref -o model=b --stats $(FW_AVR32_B) < /dev/null
	$(RUN) -b avr32-ref -o model=b --jit --stats $(FW_AVR32_B) < /dev/null
else
	$(AVR32_SKIP)
endif

# The 8-bit PIC self-test on every core family the board models, and the extended
# instruction set.
pic-test: $(RUN) firmware-pic
ifeq ($(HAVE_GPASM),1)
	@set -e; for m in $(PIC_MODELS); do \
	    echo "$(RUN) -b pic-ref -o model=$$m [--jit] --stats $(FW_PIC)/selftest-$$m.elf"; \
	    $(RUN) -b pic-ref -o model=$$m --stats $(FW_PIC)/selftest-$$m.elf < /dev/null; \
	    $(RUN) -b pic-ref -o model=$$m --jit --stats $(FW_PIC)/selftest-$$m.elf < /dev/null; \
	done
	$(RUN) -b pic-ref -o xinst=1 --stats $(FW_PIC)/selftest-xinst.elf < /dev/null
	$(RUN) -b pic-ref -o xinst=1 --jit --stats $(FW_PIC)/selftest-xinst.elf < /dev/null
else
	$(PIC_SKIP)
endif

# The gdb stub over the remote protocol, on a four-processor machine.
gdb-test: $(RUN) firmware-x86
	python3 tests/gdb/gdb-check.py $(RUN) $(dir $(FW_X86))smp.bin $(dir $(FW_X86))smp.lst

# The out-of-tree example builds against `make lib` output only, as an external project would.
example-test: lib firmware-mips
	$(MAKE) -C examples/external-board UAM=$(CURDIR) UAM_BUILD=$(abspath $(BUILD)) \
	        OUT=$(abspath $(BUILD))/examples/external-board
	$(EXAMPLE) -b ext-mips -o banner=external --stats $(FW_MIPS_BE) < /dev/null

# A run split by --save-snapshot / --load-snapshot must equal the uninterrupted run,
# on the stock boards and on the out-of-tree example board.
snapshot-test: $(RUN) $(TRUN) firmware firmware-mips firmware-arm firmware-x86 firmware-ppc firmware-avr firmware-avr32 firmware-pic \
               firmware-mcs51 firmware-pic32 \
               example-test
	sh tests/snapshot/snapshot-check.sh $(RUN) rv32-ref $(FIRMWARE) 1 3000 6900
	sh tests/snapshot/snapshot-check.sh $(RUN) mips-ref $(FW_MIPS_BE) 777 6000 11800
	sh tests/snapshot/snapshot-check.sh $(RUN) mipsel-ref $(FW_MIPS_EL) 5000
	sh tests/snapshot/snapshot-check.sh $(RUN) arm-ref $(FW_ARM) 900 30000 52000
	sh tests/snapshot/snapshot-check.sh $(RUN) arm-ref $(FW_THUMB) 4321 40000 61000
	sh tests/snapshot/snapshot-check.sh $(RUN) armeb-ref $(FW_THUMB_BE) 2500 52000 66000
	sh tests/snapshot/snapshot-check.sh $(EXAMPLE) ext-mips $(FW_MIPS_BE) 4000
	sh tests/snapshot/snapshot-check.sh $(TRUN) x86_64-test $(FW_X86) 1000 5000 15000 -- --raw-base 0 --entry 0
	sh tests/snapshot/snapshot-check.sh $(RUN) x86_64-ref $(dir $(FW_X86))legacy.bin 300 700 900 \
	    -- --raw-base 0 --entry 0
	sh tests/snapshot/snapshot-check.sh $(TRUN) x86_64-test $(dir $(FW_X86))smm-amd.bin 200 370 420 2000 14000 \
	    -- -o smm-map=amd64 --raw-base 0 --entry 0
	sh tests/snapshot/snapshot-check.sh $(RUN) x86_64-ref $(dir $(FW_X86))smp.bin 1 5000 20001 40000 \
	    -- -o cpus=4 --quantum 1000 --raw-base 0 --entry 0
	sh tests/snapshot/snapshot-check.sh $(RUN) rv32-ref $(dir $(FIRMWARE))smptest.elf 3000 70001 \
	    -- -o cpus=4 --quantum 97
	sh tests/snapshot/snapshot-check.sh $(RUN) mips-ref $(dir $(FW_MIPS_BE))smptest-be.elf 50000 \
	    -- -o cpus=4 --quantum 333
	sh tests/snapshot/snapshot-check.sh $(RUN) armeb-ref $(dir $(FW_ARM))smptest-thumb-be.elf 40000 \
	    -- -o cpus=4 --quantum 211
	sh tests/snapshot/snapshot-check.sh $(RUN) ppc-ref $(FW_PPC) 777 7300 21000 39800 40400 41000
	sh tests/snapshot/snapshot-check.sh $(RUN) ppc-ref $(dir $(FW_PPC))smptest.elf 50000 120001 \
	    -- -o cpus=4 --quantum 97
	sh tests/snapshot/snapshot-check.sh $(RUN) ppc-ref $(FW_PPC_BOOKE) 1 8000 8400 9500 11900 13000 14850 \
	    -- -o model=booke
	sh tests/snapshot/snapshot-check.sh $(RUN) avr-ref $(FW_AVR)/selftest-eplus.elf 1 5000 100000 200000 280000
	sh tests/snapshot/snapshot-check.sh $(RUN) avr-ref $(FW_AVR)/selftest-xm.elf 777 150000 260000 -- -o model=xm
	sh tests/snapshot/snapshot-check.sh $(RUN) avr-ref $(FW_AVR)/selftest-rc.elf 300 7000 14000 -- -o model=rc
ifeq ($(HAVE_GPASM),1)
	sh tests/snapshot/snapshot-check.sh $(RUN) pic-ref $(FW_PIC)/selftest-pic18.elf 1 5000 12000 18000 19000 19300
	sh tests/snapshot/snapshot-check.sh $(RUN) pic-ref $(FW_PIC)/selftest-enhanced.elf 777 16500 16800 16950 \
	    -- -o model=enhanced
	sh tests/snapshot/snapshot-check.sh $(RUN) pic-ref $(FW_PIC)/selftest-mid.elf 300 5200 5300 -- -o model=mid
	sh tests/snapshot/snapshot-check.sh $(RUN) pic-ref $(FW_PIC)/selftest-baseline.elf 300 1300 -- -o model=baseline
else
	$(PIC_SKIP)
endif
ifeq ($(HAVE_AS31),1)
	sh tests/snapshot/snapshot-check.sh $(RUN) mcs51-ref $(FW_MCS51)/selftest-8052.bin 1 5000 263500 264700 \
	    264900 265000 265100 265400
	sh tests/snapshot/snapshot-check.sh $(RUN) mcs51-ref $(FW_MCS51)/selftest-cip51.bin 777 265100 265400 \
	    -- -o model=cip51
else
	$(MCS51_SKIP)
endif

ifeq ($(HAVE_AVR32),1)
	sh tests/snapshot/snapshot-check.sh $(RUN) avr32-ref $(FW_AVR32_A) 777 164500 165000 165300 165600 165900
	sh tests/snapshot/snapshot-check.sh $(RUN) avr32-ref $(FW_AVR32_B) 777 189000 189500 190000 190300 190600 \
	    -- -o model=b
else
	$(AVR32_SKIP)
endif
	sh tests/snapshot/snapshot-check.sh $(RUN) pic32-ref $(FW_PIC32) 777 50000 120000 180000 -- -o model=mz
	sh tests/snapshot/snapshot-check.sh $(RUN) pic32-ref $(FW_PIC32_MX) 777 9000 18000 27000 -- -o model=mx

# Block coverage: identical maps from the interpreter and the JIT (chained or not), and
# runs that end exactly as without coverage, on every ISA.
cov-test: $(RUN) firmware firmware-mips firmware-arm firmware-ppc firmware-avr firmware-avr32 firmware-pic firmware-mcs51 \
          firmware-pic32
	sh tests/cov/cov-check.sh $(RUN) rv32-ref $(FIRMWARE)
	sh tests/cov/cov-check.sh $(RUN) mips-ref $(FW_MIPS_BE)
	sh tests/cov/cov-check.sh $(RUN) mipsel-ref $(FW_MIPS_EL)
	sh tests/cov/cov-check.sh $(RUN) arm-ref $(FW_ARM)
	sh tests/cov/cov-check.sh $(RUN) arm-ref $(FW_THUMB)
	sh tests/cov/cov-check.sh $(RUN) armeb-ref $(FW_ARM_BE)
	sh tests/cov/cov-check.sh $(RUN) ppc-ref $(FW_PPC)
	sh tests/cov/cov-check.sh $(RUN) ppc-ref $(FW_PPC_BOOKE) -- -o model=booke
	@set -e; for m in $(AVR_MODELS); do \
	    sh tests/cov/cov-check.sh $(RUN) avr-ref $(FW_AVR)/selftest-$$m.elf -- -o model=$$m; done
ifeq ($(HAVE_GPASM),1)
	@set -e; for m in $(PIC_MODELS); do \
	    sh tests/cov/cov-check.sh $(RUN) pic-ref $(FW_PIC)/selftest-$$m.elf -- -o model=$$m; done
	sh tests/cov/cov-check.sh $(RUN) pic-ref $(FW_PIC)/selftest-xinst.elf -- -o xinst=1
else
	$(PIC_SKIP)
endif
ifeq ($(HAVE_AS31),1)
	@set -e; for m in $(MCS51_MODELS); do \
	    sh tests/cov/cov-check.sh $(RUN) mcs51-ref $(FW_MCS51)/selftest-$$m.bin -- -o model=$$m; done
else
	$(MCS51_SKIP)
endif

ifeq ($(HAVE_AVR32),1)
	sh tests/cov/cov-check.sh $(RUN) avr32-ref $(FW_AVR32_A)
	sh tests/cov/cov-check.sh $(RUN) avr32-ref $(FW_AVR32_B) -- -o model=b
else
	$(AVR32_SKIP)
endif
	sh tests/cov/cov-check.sh $(RUN) pic32-ref $(FW_PIC32) -- -o model=mz
	sh tests/cov/cov-check.sh $(RUN) pic32-ref $(FW_PIC32_MX) -- -o model=mx

# The fuzz harness end to end: planted bugs found, the same on every backend, crash
# files reproduce on their own.
fuzz-test: $(RUN) firmware firmware-mips firmware-arm firmware-ppc firmware-avr firmware-avr32 firmware-pic firmware-mcs51
	sh tests/fuzz/fuzz-check.sh $(RUN) rv32-ref $(BUILD)/firmware/fuzztarget.elf
	sh tests/fuzz/fuzz-check.sh $(RUN) mips-ref $(BUILD)/firmware-mips/fuzztarget-be.elf \
	    $(BUILD)/firmware-mips/fuzztarget-be.bin
	sh tests/fuzz/fuzz-check.sh $(RUN) mipsel-ref $(BUILD)/firmware-mips/fuzztarget-el.elf
	sh tests/fuzz/fuzz-check.sh $(RUN) arm-ref $(BUILD)/firmware-arm/fuzztarget.elf \
	    $(BUILD)/firmware-arm/fuzztarget.bin
	sh tests/fuzz/fuzz-check.sh $(RUN) armeb-ref $(BUILD)/firmware-arm/fuzztarget-be.elf \
	    $(BUILD)/firmware-arm/fuzztarget-be.bin
	sh tests/fuzz/fuzz-check.sh $(RUN) ppc-ref $(BUILD)/firmware-ppc/fuzztarget.elf \
	    $(BUILD)/firmware-ppc/fuzztarget.bin
	FUZZ_CHECK_NO_TRAPS=1 sh tests/fuzz/fuzz-check.sh $(RUN) avr-ref $(FW_AVR)/fuzztarget.elf
ifeq ($(HAVE_GPASM),1)
	FUZZ_CHECK_NO_TRAPS=1 sh tests/fuzz/fuzz-check.sh $(RUN) pic-ref $(FW_PIC)/fuzztarget.elf
else
	$(PIC_SKIP)
endif
ifeq ($(HAVE_SDCC),1)
	FUZZ_CHECK_NO_TRAPS=1 sh tests/fuzz/fuzz-check.sh $(RUN) mcs51-ref $(FW_MCS51)/fuzztarget.bin
else
	$(SDCC_SKIP)
endif
ifeq ($(HAVE_AVR32),1)
	sh tests/fuzz/fuzz-check.sh $(RUN) avr32-ref $(BUILD)/firmware-avr32/fuzztarget-a.elf
else
	$(AVR32_SKIP)
endif

# suzzer's formats and the SZR1 executor through the runner (lists written by
# tests/suzzer/mk_lists.py, and by suzzer's own generators when rizin is installed): the
# LCVS export read back by suzzer's test/shm_bitmap.py,
# a session with suzzer's own brain (tools/suzzer_brain.py), and the protocol frame by
# frame against a scripted brain (tests/szr/fake_brain.py).
ifeq ($(HAVE_SUZZER),1)
suzzer-test: $(RUN) firmware firmware-mips firmware-arm firmware-ppc
	sh tests/suzzer/suzzer-check.sh $(RUN) rv32-ref $(BUILD)/firmware/fuzztarget.elf $(SUZZER_DIR)
	sh tests/suzzer/suzzer-check.sh $(RUN) mips-ref $(BUILD)/firmware-mips/fuzztarget-be.elf $(SUZZER_DIR)
	sh tests/suzzer/suzzer-check.sh $(RUN) mipsel-ref $(BUILD)/firmware-mips/fuzztarget-el.elf $(SUZZER_DIR)
	sh tests/suzzer/suzzer-check.sh $(RUN) arm-ref $(BUILD)/firmware-arm/fuzztarget.elf $(SUZZER_DIR)
	sh tests/suzzer/suzzer-check.sh $(RUN) armeb-ref $(BUILD)/firmware-arm/fuzztarget-be.elf $(SUZZER_DIR)
	sh tests/suzzer/suzzer-check.sh $(RUN) ppc-ref $(BUILD)/firmware-ppc/fuzztarget.elf $(SUZZER_DIR)
	sh tests/suzzer/gen-check.sh $(RUN) rv32-ref $(BUILD)/firmware/fuzztarget.elf $(SUZZER_DIR)
	sh tests/suzzer/gen-check.sh $(RUN) mips-ref $(BUILD)/firmware-mips/fuzztarget-be.elf $(SUZZER_DIR)
	sh tests/suzzer/gen-check.sh $(RUN) mipsel-ref $(BUILD)/firmware-mips/fuzztarget-el.elf $(SUZZER_DIR)
	sh tests/suzzer/gen-check.sh $(RUN) arm-ref $(BUILD)/firmware-arm/fuzztarget.elf $(SUZZER_DIR)
	sh tests/suzzer/gen-check.sh $(RUN) armeb-ref $(BUILD)/firmware-arm/fuzztarget-be.elf $(SUZZER_DIR)
	sh tests/suzzer/gen-check.sh $(RUN) ppc-ref $(BUILD)/firmware-ppc/fuzztarget.elf $(SUZZER_DIR)
	sh tests/szr/brain-check.sh $(RUN) rv32-ref $(BUILD)/firmware/fuzztarget.elf $(SUZZER_DIR)
	sh tests/szr/brain-check.sh $(RUN) mips-ref $(BUILD)/firmware-mips/fuzztarget-be.elf $(SUZZER_DIR)
	sh tests/szr/brain-check.sh $(RUN) arm-ref $(BUILD)/firmware-arm/fuzztarget.elf $(SUZZER_DIR)
	sh tests/szr/brain-check.sh $(RUN) ppc-ref $(BUILD)/firmware-ppc/fuzztarget.elf $(SUZZER_DIR)
	sh tests/szr/szr-check.sh $(RUN) rv32-ref $(BUILD)/firmware/fuzztarget.elf
	sh tests/szr/szr-check.sh $(RUN) mips-ref $(BUILD)/firmware-mips/fuzztarget-be.elf
	sh tests/szr/szr-check.sh $(RUN) mipsel-ref $(BUILD)/firmware-mips/fuzztarget-el.elf
	sh tests/szr/szr-check.sh $(RUN) arm-ref $(BUILD)/firmware-arm/fuzztarget.elf
	sh tests/szr/szr-check.sh $(RUN) armeb-ref $(BUILD)/firmware-arm/fuzztarget-be.elf
	sh tests/szr/szr-check.sh $(RUN) ppc-ref $(BUILD)/firmware-ppc/fuzztarget.elf
else
suzzer-test:
	@echo "suzzer-test: skipped (no suzzer: make suzzer, or SUZZER_DIR=path)"
endif

test: unit-test firmware-test mips-test arm-test x86-test ppc-test avr-test avr32-test pic-test mcs51-test pic32-test \
      example-test snapshot-test cov-test fuzz-test \
      suzzer-test \
      gdb-test runner-test bench-test

# The cross-ISA benchmark (tests/bench): bench-test checks every guest build, with the
# interpreter and the JIT, against the host build in two rounds; xbench times them
# (tools/xbench.sh; XBENCH_ARGS="--per-kernel" for each kernel on its own).
bench-test: $(RUN)
	AVR32_CROSS=$(AVR32_CROSS) sh tools/xbench.sh --check --run $(RUN) --out $(abspath $(BUILD))/firmware-bench

xbench: $(RUN)
	AVR32_CROSS=$(AVR32_CROSS) sh tools/xbench.sh --run $(RUN) --out $(abspath $(BUILD))/firmware-bench $(XBENCH_ARGS)

# uam-run's debugging options (--log-pc, --dump-mem, --trace-after, traces with --jit)
runner-test: $(TRUN) firmware-x86
	sh tests/runner/runner-check.sh $(TRUN) $(FW_X86)

net-check: $(TRUN) firmware
	sh tests/net/net-check.sh $(TRUN) $(BUILD)/firmware/nettest.elf both

install: all
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/lib/pkgconfig $(DESTDIR)$(PREFIX)/include
	install -m 755 $(RUN) $(DESTDIR)$(PREFIX)/bin/
	install -m 644 $(LIB) $(MODULE_LIBS) $(DESTDIR)$(PREFIX)/lib/
	$(call write_pc,$(PREFIX),$(DESTDIR)$(PREFIX)/lib/pkgconfig/uam.pc)
	cp -R include/uam $(DESTDIR)$(PREFIX)/include/

clean:
	rm -rf $(BUILD)

FORCE:

-include $(LIB_OBJ:.o=.d) $(BUILD)/obj/tools/uam-run.d $(BUILD)/obj/tests/boards/uam-test-run.d \
         $(BUILD)/obj/tests/boards/test_boards.d $(BUILD)/obj/tests/unit/test_ir.d \
         $(BUILD)/obj/tests/unit/test_core.d $(BUILD)/obj/tests/unit/test_host.d \
         $(BUILD)/obj/tests/unit/test_snapshot.d \
         $(BUILD)/obj/tests/unit/test_jit.d $(BUILD)/obj/tests/unit/test_spin.d \
         $(BUILD)/obj/tests/unit/test_codestore.d \
         $(BUILD)/obj/tests/unit/test_mmio.d \
         $(BUILD)/obj/tests/unit/test_cov.d $(BUILD)/obj/tests/unit/test_fuzz.d \
         $(BUILD)/obj/tests/unit/test_smp.d $(BUILD)/obj/tests/unit/test_ppc.d \
         $(BUILD)/obj/tests/unit/test_avr.d $(BUILD)/obj/tests/unit/test_pic.d \
         $(BUILD)/obj/tests/unit/test_mcs51.d $(BUILD)/obj/tests/unit/test_f80.d \
         $(BUILD)/obj/tests/unit/test_jitdiff.d $(BUILD)/obj/tests/unit/test_suzzer.d
