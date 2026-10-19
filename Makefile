# UAM firmware emulator - top-level build.
#
#   make                 build $(BUILD)/uam-run and the libraries
#   make lib             only the libraries (for out-of-tree projects)
#   make test            unit tests, firmware self-tests (RISC-V, MIPS BE/LE), external-board example,
#                        snapshot round trips through the runner, coverage maps across backends,
#                        the fuzz harness on planted-bug firmware
#   make suzzer          clone suzzer into ./suzzer (ignored by git) for the suzzer integration
#   make net-check       boot the network test on rv32-ref against a real tap and tun interface
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
#   libuam-devices.a  stock device models        libuam-boards.a stock boards (rv32-ref, mips-ref)
#   libuam-fuzz.a     the fuzz harness and its frontends (fuzz/)
#   libuam-host.a     host backends, gdb stub, the stock runner (uam_run_main; uses libuam-fuzz)

CC      ?= cc
AR      ?= ar
CFLAGS  ?= -O2 -g    # tunable from the command line, e.g. CFLAGS="-O1 -fsanitize=address"
LDFLAGS ?=
PREFIX  ?= /usr/local
DESTDIR ?=
# Required flags, always applied.
UAM_COMPILE := -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -Wshadow -Wstrict-prototypes \
               -Wmissing-prototypes -Iinclude $(CFLAGS)

BUILD ?= build

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
EXEC_SRC    := src/exec/interp.c src/exec/tcache.c src/exec/run.c src/exec/cpu.c src/exec/jit_x86.c \
               src/exec/coverage.c
ANALYSIS_SRC := analysis/cov.c
LOADER_SRC  := src/loader/loader.c
MACHINE_SRC := src/machine/machine.c src/machine/snapshot.c
RISCV_SRC   := isa/riscv/rv_lift.c isa/riscv/rv_system.c
MIPS_SRC    := isa/mips32/mips_lift.c isa/mips32/mips_system.c
DEV_SRC     := devices/clint.c devices/plic.c devices/uart16550.c devices/sifive_test.c \
               devices/unknown_mmio.c devices/unimp.c devices/virtio_net.c devices/mmio_stub.c
BOARD_SRC   := boards/rv32_ref.c boards/mips_ref.c boards/stock.c boards/mmio_rules.c
SUZZER_SRC  := fuzz/suzzer/scheme.c fuzz/suzzer/import.c fuzz/suzzer/cli.c \
               fuzz/suzzer/frontend_szr1.c fuzz/suzzer/lcvs.c
FUZZ_SRC    := fuzz/harness.c fuzz/frontend_replay.c fuzz/stock_exts.c \
               $(if $(HAVE_SUZZER),$(SUZZER_SRC))
HOST_SRC    := src/host/chardev_backends.c src/host/netdev_backends.c src/host/hostloop.c \
               src/debug/gdbstub.c src/runner/runner.c

CORE_ALL := $(IR_SRC) $(CORE_SRC) $(EXEC_SRC) $(ANALYSIS_SRC) $(LOADER_SRC) $(MACHINE_SRC)
LIB_SRC  := $(CORE_ALL) $(RISCV_SRC) $(MIPS_SRC) $(DEV_SRC) $(BOARD_SRC) $(FUZZ_SRC) $(HOST_SRC)
obj = $(1:%.c=$(BUILD)/obj/%.o)
LIB_OBJ  := $(call obj,$(LIB_SRC))

LIB        := $(BUILD)/libuam.a
MODULE_LIBS := $(BUILD)/libuam-core.a $(BUILD)/libuam-riscv.a $(BUILD)/libuam-mips.a \
               $(BUILD)/libuam-devices.a $(BUILD)/libuam-boards.a $(BUILD)/libuam-fuzz.a \
               $(BUILD)/libuam-host.a
RUN       := $(BUILD)/uam-run
TEST_IR   := $(BUILD)/test_ir
TEST_CORE := $(BUILD)/test_core
TEST_HOST := $(BUILD)/test_host
TEST_SNAP := $(BUILD)/test_snapshot
TEST_JIT  := $(BUILD)/test_jit
TEST_MMIO := $(BUILD)/test_mmio
TEST_COV  := $(BUILD)/test_cov
TEST_FUZZ := $(BUILD)/test_fuzz
TEST_SUZZER := $(BUILD)/test_suzzer
FIRMWARE  := $(BUILD)/firmware/selftest.elf
FW_MIPS_BE := $(BUILD)/firmware-mips/selftest-be.elf
FW_MIPS_EL := $(BUILD)/firmware-mips/selftest-el.elf
EXAMPLE   := $(BUILD)/examples/external-board/ext-run

.PHONY: all lib test unit-test firmware-test firmware firmware-mips mips-test example-test \
        snapshot-test cov-test fuzz-test suzzer-test suzzer net-check install clean FORCE

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
$(BUILD)/libuam-devices.a: $(call obj,$(DEV_SRC))
$(BUILD)/libuam-boards.a:  $(call obj,$(BOARD_SRC))
$(BUILD)/libuam-fuzz.a:    $(call obj,$(FUZZ_SRC))
$(BUILD)/libuam-host.a:    $(call obj,$(HOST_SRC))
$(MODULE_LIBS):
	@rm -f $@
	$(AR) rcs $@ $^

# pkg-config file for prefix $(1), written to $(2).
define write_pc
	printf 'prefix=%s\nincludedir=$${prefix}/include\nlibdir=$${prefix}/lib\n\nName: uam\nDescription: UAM firmware emulator library\nVersion: 0.2\nCflags: -I$${includedir}\nLibs: -L$${libdir} -luam\n' '$(1)' > $(2)
endef

$(BUILD)/uam.pc: Makefile
	@mkdir -p $(BUILD)
	$(call write_pc,$(PREFIX),$@)

$(RUN): $(BUILD)/obj/tools/uam-run.o $(LIB)
	$(CC) $(LDFLAGS) -o $@ $^

$(TEST_IR): $(BUILD)/obj/tests/unit/test_ir.o $(LIB)
	$(CC) $(LDFLAGS) -o $@ $^

$(TEST_CORE): $(BUILD)/obj/tests/unit/test_core.o $(LIB)
	$(CC) $(LDFLAGS) -o $@ $^

$(TEST_HOST): $(BUILD)/obj/tests/unit/test_host.o $(LIB)
	$(CC) $(LDFLAGS) -o $@ $^

$(TEST_SNAP): $(BUILD)/obj/tests/unit/test_snapshot.o $(LIB)
	$(CC) $(LDFLAGS) -o $@ $^

$(TEST_JIT): $(BUILD)/obj/tests/unit/test_jit.o $(LIB)
	$(CC) $(LDFLAGS) -o $@ $^

$(TEST_MMIO): $(BUILD)/obj/tests/unit/test_mmio.o $(LIB)
	$(CC) $(LDFLAGS) -o $@ $^

$(TEST_COV): $(BUILD)/obj/tests/unit/test_cov.o $(LIB)
	$(CC) $(LDFLAGS) -o $@ $^

$(TEST_FUZZ): $(BUILD)/obj/tests/unit/test_fuzz.o $(LIB)
	$(CC) $(LDFLAGS) -o $@ $^

# test_suzzer checks fuzz/suzzer/ against suzzer's conformance vectors (its test/vectors).
$(BUILD)/obj/tests/unit/test_suzzer.o: tests/unit/test_suzzer.c $(BUILD)/suzzer.cfg
	@mkdir -p $(dir $@)
	$(CC) $(UAM_COMPILE) -Ifuzz/suzzer -I$(SUZZER_DIR)/src -MMD -MP -c $< -o $@

$(TEST_SUZZER): $(BUILD)/obj/tests/unit/test_suzzer.o $(LIB)
	$(CC) $(LDFLAGS) -o $@ $^

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

$(FIRMWARE): firmware

unit-test: $(TEST_IR) $(TEST_CORE) $(TEST_HOST) $(TEST_SNAP) $(TEST_JIT) $(TEST_MMIO) $(TEST_COV) $(TEST_FUZZ) \
           $(SUZZER_UNIT)
	$(TEST_IR)
	$(TEST_CORE)
	$(TEST_HOST)
	$(TEST_SNAP)
	$(TEST_JIT)
	$(TEST_MMIO)
	$(TEST_COV)
	$(TEST_FUZZ)
ifeq ($(HAVE_SUZZER),1)
	$(TEST_SUZZER) $(SUZZER_DIR)/test/vectors
else
	@echo "test_suzzer: skipped (no suzzer: make suzzer, or SUZZER_DIR=path)"
endif

firmware-test: $(RUN) firmware
	$(RUN) --stats $(FIRMWARE) < /dev/null

mips-test: $(RUN) firmware-mips
	$(RUN) -b mips-ref --stats $(FW_MIPS_BE) < /dev/null
	$(RUN) -b mipsel-ref --stats $(FW_MIPS_EL) < /dev/null

# The out-of-tree example builds against `make lib` output only, as an external project would.
example-test: lib firmware-mips
	$(MAKE) -C examples/external-board UAM=$(CURDIR) UAM_BUILD=$(abspath $(BUILD)) \
	        OUT=$(abspath $(BUILD))/examples/external-board
	$(EXAMPLE) -b ext-mips -o banner=external --stats $(FW_MIPS_BE) < /dev/null

# A run split by --save-snapshot / --load-snapshot must equal the uninterrupted run,
# on the stock boards and on the out-of-tree example board.
snapshot-test: $(RUN) firmware firmware-mips example-test
	sh tests/snapshot/snapshot-check.sh $(RUN) rv32-ref $(FIRMWARE) 1 3000 6900
	sh tests/snapshot/snapshot-check.sh $(RUN) mips-ref $(FW_MIPS_BE) 777 6000 11800
	sh tests/snapshot/snapshot-check.sh $(RUN) mipsel-ref $(FW_MIPS_EL) 5000
	sh tests/snapshot/snapshot-check.sh $(EXAMPLE) ext-mips $(FW_MIPS_BE) 4000

# Block coverage: identical maps from the interpreter and the JIT (chained or not), and
# runs that end exactly as without coverage, on every ISA.
cov-test: $(RUN) firmware firmware-mips
	sh tests/cov/cov-check.sh $(RUN) rv32-ref $(FIRMWARE)
	sh tests/cov/cov-check.sh $(RUN) mips-ref $(FW_MIPS_BE)
	sh tests/cov/cov-check.sh $(RUN) mipsel-ref $(FW_MIPS_EL)

# The fuzz harness end to end: planted bugs found, the same on every backend, crash
# files reproduce on their own.
fuzz-test: $(RUN) firmware firmware-mips
	sh tests/fuzz/fuzz-check.sh $(RUN) rv32-ref $(BUILD)/firmware/fuzztarget.elf
	sh tests/fuzz/fuzz-check.sh $(RUN) mips-ref $(BUILD)/firmware-mips/fuzztarget-be.elf \
	    $(BUILD)/firmware-mips/fuzztarget-be.bin
	sh tests/fuzz/fuzz-check.sh $(RUN) mipsel-ref $(BUILD)/firmware-mips/fuzztarget-el.elf

# suzzer's formats and the SZR1 executor through the runner (lists written by
# tests/suzzer/mk_lists.py, and by suzzer's own generators when rizin is installed): the
# LCVS export read back by suzzer's test/shm_bitmap.py,
# a session with suzzer's own brain (tools/suzzer_brain.py), and the protocol frame by
# frame against a scripted brain (tests/szr/fake_brain.py).
ifeq ($(HAVE_SUZZER),1)
suzzer-test: $(RUN) firmware firmware-mips
	sh tests/suzzer/suzzer-check.sh $(RUN) rv32-ref $(BUILD)/firmware/fuzztarget.elf $(SUZZER_DIR)
	sh tests/suzzer/suzzer-check.sh $(RUN) mips-ref $(BUILD)/firmware-mips/fuzztarget-be.elf $(SUZZER_DIR)
	sh tests/suzzer/suzzer-check.sh $(RUN) mipsel-ref $(BUILD)/firmware-mips/fuzztarget-el.elf $(SUZZER_DIR)
	sh tests/suzzer/gen-check.sh $(RUN) rv32-ref $(BUILD)/firmware/fuzztarget.elf $(SUZZER_DIR)
	sh tests/suzzer/gen-check.sh $(RUN) mips-ref $(BUILD)/firmware-mips/fuzztarget-be.elf $(SUZZER_DIR)
	sh tests/suzzer/gen-check.sh $(RUN) mipsel-ref $(BUILD)/firmware-mips/fuzztarget-el.elf $(SUZZER_DIR)
	sh tests/szr/brain-check.sh $(RUN) rv32-ref $(BUILD)/firmware/fuzztarget.elf $(SUZZER_DIR)
	sh tests/szr/brain-check.sh $(RUN) mips-ref $(BUILD)/firmware-mips/fuzztarget-be.elf $(SUZZER_DIR)
	sh tests/szr/szr-check.sh $(RUN) rv32-ref $(BUILD)/firmware/fuzztarget.elf
	sh tests/szr/szr-check.sh $(RUN) mips-ref $(BUILD)/firmware-mips/fuzztarget-be.elf
	sh tests/szr/szr-check.sh $(RUN) mipsel-ref $(BUILD)/firmware-mips/fuzztarget-el.elf
else
suzzer-test:
	@echo "suzzer-test: skipped (no suzzer: make suzzer, or SUZZER_DIR=path)"
endif

test: unit-test firmware-test mips-test example-test snapshot-test cov-test fuzz-test suzzer-test

net-check: $(RUN) firmware
	sh tests/net/net-check.sh $(RUN) $(BUILD)/firmware/nettest.elf both

install: all
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/lib/pkgconfig $(DESTDIR)$(PREFIX)/include
	install -m 755 $(RUN) $(DESTDIR)$(PREFIX)/bin/
	install -m 644 $(LIB) $(MODULE_LIBS) $(DESTDIR)$(PREFIX)/lib/
	$(call write_pc,$(PREFIX),$(DESTDIR)$(PREFIX)/lib/pkgconfig/uam.pc)
	cp -R include/uam $(DESTDIR)$(PREFIX)/include/

clean:
	rm -rf $(BUILD)

FORCE:

-include $(LIB_OBJ:.o=.d) $(BUILD)/obj/tools/uam-run.d $(BUILD)/obj/tests/unit/test_ir.d \
         $(BUILD)/obj/tests/unit/test_core.d $(BUILD)/obj/tests/unit/test_host.d \
         $(BUILD)/obj/tests/unit/test_snapshot.d \
         $(BUILD)/obj/tests/unit/test_jit.d $(BUILD)/obj/tests/unit/test_mmio.d \
         $(BUILD)/obj/tests/unit/test_cov.d $(BUILD)/obj/tests/unit/test_fuzz.d \
         $(BUILD)/obj/tests/unit/test_suzzer.d
