# Builds the 8-bit AVR self-test once per avr-ref model (boards/avr_ref.c): the part named
# gives the compiler the model's memory layout; the devices are the board's own.
#
#   model  part          device base  flag vectors  program memory above 64 KiB
#   e      atmega328p    0x00E0       medium        -
#   eplus  atmega2560    0x00E0       full          .fartable at 0x30000
#   xm     atxmega128a1u 0x0FE0       full          .fartable at 0x18000
#   xt     atmega4809    0x0FE0       medium        -
#   rc     attiny40      0x1000       small         -

CROSS ?= avr-
OUT   ?= ../../build/firmware-avr

FW_CFLAGS = -Os -g -std=gnu11 -Wall -Wextra -mrelax -ffunction-sections -Wl,--gc-sections

MODELS := e eplus xm xt rc
all: $(foreach m,$(MODELS),$(OUT)/selftest-$(m).elf) $(OUT)/fuzztarget.elf

part_e     := atmega328p
part_eplus := atmega2560
part_xm    := atxmega128a1u
part_xt    := atmega4809
part_rc    := attiny40
dev_e      := 0x00E0
dev_eplus  := 0x00E0
dev_xm     := 0x0FE0
dev_xt     := 0x0FE0
dev_rc     := 0x1000
vec_e      := medium
vec_eplus  := full
vec_xm     := full
vec_xt     := medium
vec_rc     := small
extra_eplus := -DFAR_TABLE -Wl,--section-start=.fartable=0x30000
extra_xm    := -DFAR_TABLE -DMODEL_XM -Wl,--section-start=.fartable=0x18000

.SECONDARY:
$(OUT)/%/vectors.h: gen_vectors.py
	@mkdir -p $(dir $@)
	python3 gen_vectors.py $@ $(vec_$*)

$(OUT)/selftest-%.elf: selftest.c $(OUT)/%/vectors.h
	$(CROSS)gcc -mmcu=$(part_$*) $(FW_CFLAGS) -I$(OUT)/$* -DDEV_BASE=$(dev_$*) -DMODEL_NAME='"$*"' \
	    $(extra_$*) selftest.c -o $@
	$(CROSS)objdump -d $@ > $(OUT)/selftest-$*.dis

$(OUT)/fuzztarget.elf: fuzztarget.c ../fuzz/fuzztarget.h
	@mkdir -p $(OUT)
	$(CROSS)gcc -mmcu=$(part_eplus) $(FW_CFLAGS) fuzztarget.c -o $@

.PHONY: all
