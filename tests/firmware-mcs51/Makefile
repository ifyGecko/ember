# Builds the 8051 self-test once per mcs51-ref model (boards/mcs51_ref.c) with as31: the
# common part, the model's part and the generated arithmetic vectors, as one source.
# The CIP-51 image also carries three code banks (bank.asm, laid out by mkimage.py).
# `make fuzz` builds the fuzz target with SDCC: fuzztarget.bin and its symbols
# (fuzztarget.bin.syms, for tests/fuzz/fuzz-check.sh).

AS31 ?= as31
SDCC ?= sdcc
OUT  ?= ../../build/firmware-mcs51

MODELS := 8051 8052 cip51
all: $(foreach m,$(MODELS),$(OUT)/selftest-$(m).bin)
fuzz: $(OUT)/fuzztarget.bin

$(OUT)/fuzztarget.bin: fuzztarget.c ../fuzz/fuzztarget.h
	@mkdir -p $(OUT)/fuzz
	$(SDCC) -mmcs51 --model-large --std-c11 --iram-size 256 --xram-size 0xff00 fuzztarget.c \
	    -o $(OUT)/fuzz/fuzztarget.ihx
	makebin -p $(OUT)/fuzz/fuzztarget.ihx $@
	awk '$$1 == "C:" && substr($$3, 1, 1) == "_" { print substr($$3, 2), $$2 }' \
	    $(OUT)/fuzz/fuzztarget.map > $@.syms

$(OUT)/vectors.asm: gen_vectors.py
	@mkdir -p $(OUT)
	python3 gen_vectors.py $@

$(OUT)/selftest-%.asm: selftest.asm model-%.asm $(OUT)/vectors.asm
	cat selftest.asm model-$*.asm $(OUT)/vectors.asm > $@

$(OUT)/main-%.bin: $(OUT)/selftest-%.asm
	$(AS31) -l -Fbin -O$@ $< > /dev/null

$(OUT)/selftest-8051.bin $(OUT)/selftest-8052.bin: $(OUT)/selftest-%.bin: $(OUT)/main-%.bin
	cp $< $@

$(OUT)/bank-%.bin: bank.asm
	@mkdir -p $(OUT)
	sed 's/BANK/$*/g' bank.asm > $(OUT)/bank-$*.asm
	$(AS31) -Fbin -O$@ $(OUT)/bank-$*.asm > /dev/null

$(OUT)/selftest-cip51.bin: $(OUT)/main-cip51.bin $(OUT)/bank-1.bin $(OUT)/bank-2.bin $(OUT)/bank-3.bin mkimage.py
	python3 mkimage.py $@ $(OUT)/main-cip51.bin $(OUT)/bank-1.bin $(OUT)/bank-2.bin $(OUT)/bank-3.bin

.SECONDARY:
.PHONY: all fuzz
