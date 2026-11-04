# Builds the 8-bit PIC self-tests, one per pic-ref model (boards/pic_ref.c), with gpasm
# (gputils). The processor named gives gpasm the instruction set; the board's layout is
# the reference one. gpasm writes Intel hex: hex2elf.py makes the ELF images uam-run and the
# test scripts load, with gpasm's labels as symbols.
#
#   model     instruction set (gpasm -p)  flag vectors at (words; PIC18 bytes)  reset vector
#   baseline  16f59                       0x200-0x7EF                           0xFFE (last word)
#   mid       16f877                      0x800-0x1FEF                          0
#   enhanced  16f1939                     0x800-0x7FEF                          0
#   pic18     18f4620                     0x4000-0xFFEF                         0
# Plus selftest-xinst (pic18 with -o xinst=1) and fuzztarget (pic18).

GPASM ?= gpasm
OUT   ?= ../../build/firmware-pic

MODELS := baseline mid enhanced pic18
all: $(foreach m,$(MODELS),$(OUT)/selftest-$(m).elf) $(OUT)/selftest-xinst.elf $(OUT)/fuzztarget.elf

proc_baseline := 16f59
proc_mid      := 16f877
proc_enhanced := 16f1939
proc_pic18    := 18f4620
proc_xinst    := 18f4620
proc_fuzztarget := 18f4620
vec_baseline  := 0x200 0x7EF
vec_mid       := 0x800 0x1FEF
vec_enhanced  := 0x800 0x7FEF
vec_pic18     := 0x4000 0xFFEF
entry_baseline := 0xFFE
words_baseline := --words
words_mid      := --words
words_enhanced := --words
# program memory: the 14-bit configuration words are not part of it
below_baseline := 0x1000
below_mid      := 0x4000
below_enhanced := 0x10000

.SECONDARY:
$(OUT)/vectors-%.inc: gen_vectors.py
	@mkdir -p $(OUT)
	python3 gen_vectors.py $* $(vec_$*) $@

$(OUT)/selftest-%.hex: selftest-%.asm $(OUT)/vectors-%.inc
	$(GPASM) -q -p $(proc_$*) -I $(OUT) -o $@ $<

$(OUT)/selftest-xinst.hex: selftest-xinst.asm
	@mkdir -p $(OUT)
	$(GPASM) -q -y -p $(proc_xinst) -o $@ $<

$(OUT)/fuzztarget.hex: fuzztarget.asm
	@mkdir -p $(OUT)
	$(GPASM) -q -p $(proc_fuzztarget) -o $@ $<

$(OUT)/%.elf: $(OUT)/%.hex hex2elf.py
	python3 hex2elf.py $< $(OUT)/$*.lst $@ $(or $(entry_$(*:selftest-%=%)),0) \
	    $(words_$(*:selftest-%=%)) $(if $(below_$(*:selftest-%=%)),--below $(below_$(*:selftest-%=%)))

.PHONY: all
