# UAM firmware emulator - top-level build.
#
#   make                 build $(BUILD)/uam-run and the libraries
#   make lib             only the libraries (for out-of-tree projects)
#   make test            unit tests, firmware self-tests (RISC-V, MIPS BE/LE), external-board example
#   make net-check       boot the network test on rv32-ref against a real tap and tun interface
#                        (rootless: in a user+network namespace; skipped if unavailable)
#   make install         install headers, libraries, uam-run and uam.pc under $(PREFIX)
#   make clean
#
# Out-of-tree use: `make -C path/to/uam BUILD=$PWD/build/uam lib` builds into another directory;
# link $(BUILD)/libuam.a with -I path/to/uam/include (or use uam.pc after `make install`).
#
# Libraries, one per module group (each is also contained in libuam.a):
#   libuam-core.a     IR, bus, scheduler, signals/chardev/netdev plumbing, executor, loader, machine
#   libuam-riscv.a    RISC-V ISA module          libuam-mips.a   MIPS32 ISA module
#   libuam-devices.a  stock device models        libuam-boards.a stock boards (rv32-ref, mips-ref)
#   libuam-host.a     host backends, gdb stub, the stock runner (uam_run_main)

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

# One list per module, mirroring the dependency rules in docs/architecture.md.
IR_SRC      := src/ir/builder.c src/ir/verify.c src/ir/print.c src/ir/opt.c
CORE_SRC    := src/core/common.c src/core/bus.c src/core/sched.c src/core/chardev.c src/core/netdev.c
EXEC_SRC    := src/exec/interp.c src/exec/tcache.c src/exec/run.c src/exec/cpu.c src/exec/jit_x86.c
LOADER_SRC  := src/loader/loader.c
MACHINE_SRC := src/machine/machine.c
RISCV_SRC   := isa/riscv/rv_lift.c isa/riscv/rv_system.c
MIPS_SRC    := isa/mips32/mips_lift.c isa/mips32/mips_system.c
DEV_SRC     := devices/clint.c devices/plic.c devices/uart16550.c devices/sifive_test.c \
               devices/unknown_mmio.c devices/unimp.c devices/virtio_net.c
BOARD_SRC   := boards/rv32_ref.c boards/mips_ref.c boards/stock.c
HOST_SRC    := src/host/chardev_backends.c src/host/netdev_backends.c src/host/hostloop.c \
               src/debug/gdbstub.c src/runner/runner.c

CORE_ALL := $(IR_SRC) $(CORE_SRC) $(EXEC_SRC) $(LOADER_SRC) $(MACHINE_SRC)
LIB_SRC  := $(CORE_ALL) $(RISCV_SRC) $(MIPS_SRC) $(DEV_SRC) $(BOARD_SRC) $(HOST_SRC)
obj = $(1:%.c=$(BUILD)/obj/%.o)
LIB_OBJ  := $(call obj,$(LIB_SRC))

LIB        := $(BUILD)/libuam.a
MODULE_LIBS := $(BUILD)/libuam-core.a $(BUILD)/libuam-riscv.a $(BUILD)/libuam-mips.a \
               $(BUILD)/libuam-devices.a $(BUILD)/libuam-boards.a $(BUILD)/libuam-host.a
RUN       := $(BUILD)/uam-run
TEST_IR   := $(BUILD)/test_ir
TEST_CORE := $(BUILD)/test_core
TEST_HOST := $(BUILD)/test_host
FIRMWARE  := $(BUILD)/firmware/selftest.elf
FW_MIPS_BE := $(BUILD)/firmware-mips/selftest-be.elf
FW_MIPS_EL := $(BUILD)/firmware-mips/selftest-el.elf
EXAMPLE   := $(BUILD)/examples/external-board/ext-run

.PHONY: all lib test unit-test firmware-test firmware firmware-mips mips-test example-test \
        net-check install clean

all: $(RUN) lib

lib: $(LIB) $(MODULE_LIBS) $(BUILD)/uam.pc

$(BUILD)/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(UAM_COMPILE) -MMD -MP -c $< -o $@

$(LIB): $(LIB_OBJ)
	@rm -f $@
	$(AR) rcs $@ $^

$(BUILD)/libuam-core.a:    $(call obj,$(CORE_ALL))
$(BUILD)/libuam-riscv.a:   $(call obj,$(RISCV_SRC))
$(BUILD)/libuam-mips.a:    $(call obj,$(MIPS_SRC))
$(BUILD)/libuam-devices.a: $(call obj,$(DEV_SRC))
$(BUILD)/libuam-boards.a:  $(call obj,$(BOARD_SRC))
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

firmware:
	$(MAKE) -C tests/firmware OUT=$(abspath $(BUILD))/firmware

firmware-mips:
	$(MAKE) -C tests/firmware-mips OUT=$(abspath $(BUILD))/firmware-mips

$(FIRMWARE): firmware

unit-test: $(TEST_IR) $(TEST_CORE) $(TEST_HOST)
	$(TEST_IR)
	$(TEST_CORE)
	$(TEST_HOST)

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

test: unit-test firmware-test mips-test example-test

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

-include $(LIB_OBJ:.o=.d) $(BUILD)/obj/tools/uam-run.d $(BUILD)/obj/tests/unit/test_ir.d \
         $(BUILD)/obj/tests/unit/test_core.d $(BUILD)/obj/tests/unit/test_host.d
