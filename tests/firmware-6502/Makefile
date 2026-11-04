# Builds the 6502 self-test once per 6502-ref model (boards/m6502_ref.c) with sdas6500:
# the common part, the model's part and the generated instruction vectors, as one
# source, linked at 0x8000 (the board's ROM) and written as a raw image of the ROM.
# `make fuzz` builds the fuzz target with SDCC (mos6502): fuzztarget.bin and its
# symbols (fuzztarget.bin.syms, for tests/fuzz/fuzz-check.sh).

SDAS ?= sdas6500
SDLD ?= sdld
SDCC ?= sdcc
OUT  ?= ../../build/firmware-6502

MODELS := nmos 2a03 65c02
all: $(foreach m,$(MODELS),$(OUT)/selftest-$(m).bin)
fuzz: $(OUT)/fuzztarget.bin

$(OUT)/vectors-%.s: gen_vectors.py
	@mkdir -p $(OUT)
	python3 gen_vectors.py $* $@

# The NMOS undefined-opcode tests (nmos.s) go into the NMOS and 2A03 builds.
$(OUT)/selftest-%.s: selftest.s model-%.s nmos.s $(OUT)/vectors-%.s
	cat selftest.s $(if $(filter 65c02,$*),,nmos.s) model-$*.s $(OUT)/vectors-$*.s > $@

$(OUT)/selftest-%.bin: $(OUT)/selftest-%.s ihx2bin.py
	$(SDAS) -los $(OUT)/selftest-$*.rel $<
	$(SDLD) -i $(OUT)/selftest-$*.ihx $(OUT)/selftest-$*.rel > /dev/null
	python3 ihx2bin.py $(OUT)/selftest-$*.ihx $@

$(OUT)/fuzztarget.bin: fuzztarget.c ../fuzz/fuzztarget.h ihx2bin.py
	@mkdir -p $(OUT)/fuzz
	$(SDCC) -mmos6502 --std-c11 --code-loc 0x8000 -Wl-bGSINIT=0x8000 fuzztarget.c \
	    -o $(OUT)/fuzz/fuzztarget.ihx
	python3 ihx2bin.py $(OUT)/fuzz/fuzztarget.ihx $@
	awk '$$2 ~ /^_/ && $$1 ~ /^0000[89A-F]/ { print substr($$2, 2), substr($$1, 5) }' \
	    $(OUT)/fuzz/fuzztarget.map > $@.syms

.SECONDARY:
.PHONY: all fuzz
