# Builds the Z80 self-test once per z80-ref model (boards/z80_ref.c) with sdasz80: the
# model's part, the common part and the generated vectors, as one source, linked at 0
# into a raw image. `make fuzz` builds the fuzz target with SDCC: fuzztarget.bin and its
# symbols (fuzztarget.bin.syms, for tests/fuzz/fuzz-check.sh).

SDAS ?= sdasz80
SDLD ?= sdldz80
SDCC ?= sdcc
OUT  ?= ../../build/firmware-z80

MODELS := nmos cmos
all: $(foreach m,$(MODELS),$(OUT)/selftest-$(m).bin)
fuzz: $(OUT)/fuzztarget.bin

$(OUT)/vectors.s: gen_vectors.py
	@mkdir -p $(OUT)
	python3 gen_vectors.py $@

$(OUT)/selftest-%.s: model-%.s selftest.s $(OUT)/vectors.s
	cat model-$*.s selftest.s $(OUT)/vectors.s > $@

$(OUT)/selftest-%.bin: $(OUT)/selftest-%.s
	$(SDAS) -l -o $(OUT)/selftest-$*.rel $<
	$(SDLD) -m -i $(OUT)/selftest-$*.ihx $(OUT)/selftest-$*.rel > /dev/null
	makebin -p -s 65536 $(OUT)/selftest-$*.ihx $@

# The fuzz target: SDCC's z80 port with our start code (crt0.s) instead of its own.
$(OUT)/fuzztarget.bin: fuzztarget.c crt0.s ../fuzz/fuzztarget.h
	@mkdir -p $(OUT)/fuzz
	$(SDAS) -o $(OUT)/fuzz/crt0.rel crt0.s
	$(SDCC) -mz80 --no-std-crt0 --code-loc 0x0200 --data-loc 0x8000 fuzztarget.c \
	    $(OUT)/fuzz/crt0.rel -o $(OUT)/fuzz/fuzztarget.ihx
	makebin -p -s 65536 $(OUT)/fuzz/fuzztarget.ihx $@
	awk '$$1 ~ /^0000/ && substr($$2, 1, 1) == "_" { print substr($$2, 2), substr($$1, 5) }' \
	    $(OUT)/fuzz/fuzztarget.map > $@.syms

.SECONDARY:
.PHONY: all fuzz
