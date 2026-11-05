#!/usr/bin/env bash
# new-emulation.sh - scaffold an ember emulation project directory.
#
# Usage:
#   new-emulation.sh <board> [--dir DIR] [--isa ISA] [--model M] [--recon RECON.md]
#                    [--ember-src DIR | --ember-upstream URL|PATH] [--ember-commit SHA|TAG]
#                    [--ram-base A] [--ram-size N] [--image-base A] [--entry A] [--reset-vec A]
#
#   <board>        ember board name to register (-b <board>), e.g. "myboard"
#   --dir            project directory (default: $EMU_ROOT/<board>, EMU_ROOT=~/emu).
#                    May already exist (e.g. the session directory holding RECON.md);
#                    existing files are never overwritten.
#   --isa            ember ISA descriptor to instantiate (default mips32be), and --model, the
#                    CPU model the board configures (first = default; boards/<isa>_ref.c):
#                      mips32be | mips32le   MIPS32r2 (uam_isa_mips32be / _le)
#                      rv32                  RV32IMAC M-mode (uam_isa_rv32)
#                      arm | armeb           ARMv7-A, little-endian / BE8 (uam_isa_arm / _armeb)
#                      ppc                   32-bit PowerPC: classic | booke
#                      avr                   8-bit AVR: eplus | e | xm | xt | rc
#                      avr32                 AVR32: a (AVR32A, MPU) | b (AVR32B, MMU)
#                      pic                   8-bit PIC: pic18 | baseline | mid | enhanced
#                      pic32                 PIC32 (little-endian MIPS32 with PIC32's map and
#                                            interrupt controller): mx | mm | mz
#                      mcs51 (8051)          8052 | 8051 | cip51
#                      m6502 (6502)          nmos | 2a03 | 65c02 | r65c02
#                      z80                   nmos | cmos
#                      xtensa | xtensaeb     Xtensa, little-/big-endian: lx6 | lx106 | lx7
#                      x86_64                an x86-64 PC (uam_isa_x86_64): the board comes from
#                                            templates/board-pc.c.in - ember's generic PC devices,
#                                            boot flash ending at 4 GiB, chipset left to add
#   --ember-upstream clone source for the project's own ember clone (<dir>/ember-src, the
#                    default and the rule). Default: $EMBER_UPSTREAM, else ~/ember.
#   --ember-src      use this existing clone instead (recorded in ember.src): only a clone
#                    dedicated to this project, never someone's ember working copy. Reset to
#                    ember.commit by build-ember.sh, which refuses to discard or move work.
#   --ember-commit   commit/tag to pin. Default and rule: the latest ember commit (upstream's
#                    HEAD). Anything older needs a reason in the README.
#   --ram-base ...   numbers from RECON.md for the generated board (all optional; the board
#                    file marks them for review either way; addresses are bus addresses, so
#                    a Harvard ISA's data RAM includes its data-space base). Defaults follow
#                    ember's reference boards (boards/*_ref.c): MIPS, ARM and PowerPC RAM at
#                    phys 0, RV32 RAM at 0x80000000, 32 MiB (PowerPC 64); a PC has 512 MiB at
#                    0 (above 4 GiB beyond the MMIO hole) and starts at the reset vector
#                    0xFFFFFFF0; the microcontroller ISAs get the reference model's program
#                    memory, data RAM and reset vector.
#
# Creates: emulation/<board>_board.{c,h} (a minimal board that already builds), emulation/
# main.c (a PC also emulation/master_abort_log.{c,h}), Makefile, patches/, ember.commit,
# ember.upstream or ember.src, run.sh, scripts/ (copies of the helpers, so the project is
# self-sufficient), tools/, README.md, .gitignore; initializes git if the directory is not
# already a repository. Writes NOTHING into ember.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TPL="$HERE/../templates"
die() { echo "new-emulation: $*" >&2; exit 1; }

BOARD=""; DIR=""; ISA="mips32be"; RECON=""; SRC=""; UPSTREAM=""; COMMIT=""
RAM_BASE=""; RAM_SIZE=""; IMAGE_BASE=""; ENTRY=""; RESET_VEC=""; MODEL=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --dir) DIR="$2"; shift 2;;
        --isa) ISA="$2"; shift 2;;
        --model) MODEL="$2"; shift 2;;
        --recon) RECON="$2"; shift 2;;
        --ember-src) SRC="$2"; shift 2;;
        --ember-upstream) UPSTREAM="$2"; shift 2;;
        --ember-commit) COMMIT="$2"; shift 2;;
        --ram-base) RAM_BASE="$2"; shift 2;;
        --ram-size) RAM_SIZE="$2"; shift 2;;
        --image-base) IMAGE_BASE="$2"; shift 2;;
        --entry) ENTRY="$2"; shift 2;;
        --reset-vec) RESET_VEC="$2"; shift 2;;
        -h|--help) sed -n '2,51p' "$0"; exit 0;;
        -*) die "unknown option: $1";;
        *) BOARD="$1"; shift;;
    esac
done
[[ -n "$BOARD" ]] || die "usage: new-emulation.sh <board> [--dir DIR] ..."
[[ "$BOARD" =~ ^[a-z0-9][a-z0-9_-]*$ ]] || die "board name must be lowercase [a-z0-9_-]"
CNAME="$(echo "$BOARD" | tr '-' '_')"
GUARD="$(echo "$CNAME" | tr 'a-z' 'A-Z')"

BOARD_TPL="$TPL/board.c.in"
UNKNOWN_DEFAULT="zero"   # run.sh: unclaimed addresses read 0 and are logged
# Per-ISA pieces of templates/board.c.in (empty unless the ISA needs them): extra includes,
# file-scope helpers, the CPU model, memory besides RAM, platform devices every board of the
# architecture has, default catch-all windows, notes. The defaults follow boards/<isa>_ref.c.
INCLUDES=""; HELPERS=""; CPU_MODEL=""; MEMORY=""; PLATFORM=""; DEFAULT_WINDOWS=""
MEM_NOTE=""; MEM_DEFINES=""; DEVICE_NOTE=""; MAX_CPUS=8; GDB_ENDIAN=little
RAM_MAP='    st = uam_bus_map_ram(&m->bus, "ram", BOARD_RAM_BASE, cfg->ram_size ? cfg->ram_size : BOARD_RAM_SIZE);'
irq_cpu() { IRQ_CONNECT="uam_signal_connect(soc_uart_irq_line(uart), uam_cpu_irq_sink, &m->cpu, $1);${2:+
     *       ($2)}"; }
model_of() {  # default, allowed...: validate --model
    local def="$1"; shift
    MODEL="${MODEL:-$def}"
    local m; for m in "$@"; do [[ "$MODEL" == "$m" ]] && return; done
    die "--model for --isa $ISA: $* (default $def)"
}
no_model() { [[ -z "$MODEL" ]] || die "--isa $ISA has one reference CPU model (no --model): copy and change it in the board"; }
# Read-only memory (program flash, boot ROM), erased until the loader places an image in it.
FLASH_HELPER='/* Read-only memory, erased (0xFF) until an image is loaded into it: the loader writes ROM,
 * the guest cannot (a guest write is a bus fault; flash that takes commands is ROMD). */
static uam_status map_flash(uam_machine *m, const char *name, uint64_t base, uint64_t size)
{
    uint8_t *erased = malloc((size_t)size);
    if (!erased)
        return UAM_ERR_NOMEM;
    memset(erased, 0xFF, (size_t)size);
    uam_status st = uam_bus_map_rom(&m->bus, name, base, size, erased, (size_t)size);
    free(erased);
    return st;
}

'
case "$ISA" in
    mips32be|mips32le)
        no_model
        ISA_HEADER="uam/isa/mips32.h"; ISA_DESC="uam_isa_$ISA"
        RAM_BASE="${RAM_BASE:-0x0}"; IMAGE_BASE="${IMAGE_BASE:-0x80010000}"
        RESET_VEC="${RESET_VEC:-0xbfc00000}"; irq_cpu UAM_MIPS_IRQ_HW0 "Cause.IP2"
        [[ $ISA == mips32be ]] && GDB_ENDIAN=big
        CPU_MODEL="    /* CPU identity the firmware checks (RECON: PRId/Config compares, TLB size): a
     * board-local copy of ember's reference identity, changed where the firmware looks
     * (prid, config0-3, status_rw, tlb_entries, shadow_sets). Config1.CA/FP and Config3.ISA/
     * DSPP/DSP2P/VEIC switch MIPS16e, the FPU, microMIPS, the DSP ASE and an external
     * interrupt controller on (uam/isa/mips32.h). Every processor gets it. */
    uam_mips_config cpu_model = uam_mips_ref;
    cpu_model.name = \"@BOARD@-cpu\";
    for (unsigned i = 0; i < ncpu; i++)
        if ((st = uam_mips_configure(uam_machine_cpu(m, i), &cpu_model)) != UAM_OK)
            return st;
";;
    rv32)
        no_model
        ISA_HEADER="uam/isa/riscv.h"; ISA_DESC="uam_isa_rv32"
        RAM_BASE="${RAM_BASE:-0x80000000}"; IMAGE_BASE="${IMAGE_BASE:-0x80000000}"
        RESET_VEC="${RESET_VEC:-0x80000000}"; irq_cpu UAM_RV_IRQ_MEI "mip.MEIP; usually through a PLIC"
        DEVICE_NOTE="
     * Stock SiFive-layout timers and interrupt controller: uam/dev/clint.h, uam/dev/plic.h.";;
    arm|armeb)
        no_model
        ISA_HEADER="uam/isa/arm.h"; ISA_DESC="uam_isa_$ISA"
        RAM_BASE="${RAM_BASE:-0x0}"; IMAGE_BASE="${IMAGE_BASE:-0x0}"
        RESET_VEC="${RESET_VEC:-0x0}"; irq_cpu UAM_ARM_IRQ "usually through a GIC"
        [[ $ISA == armeb ]] && GDB_ENDIAN=big
        DEVICE_NOTE="
     * Stock interrupt controller: uam/dev/gic.h (GICv2 distributor + CPU interfaces)."
        CPU_MODEL="    /* CPU identity the firmware checks (RECON: MIDR/MPIDR/CTR reads, CNTFRQ): a
     * board-local copy of ember's reference identity (ARMv7-A, no VFP/NEON). Every processor
     * gets it; each reads its number in MPIDR.Aff0. The generic timer is inside each
     * processor: route its outputs once the interrupt controller exists, e.g. to GIC PPIs
     * 30/27 with uam_arm_set_timer_sink(uam_machine_cpu(m, i), fn, ctx). A Thumb entry
     * point has bit 0 set (BOARD_ENTRY | 1), as in a Thumb function symbol. */
    uam_arm_config cpu_model = uam_arm_ref;
    cpu_model.name = \"@BOARD@-cpu\";
    for (unsigned i = 0; i < ncpu; i++)
        if ((st = uam_arm_configure(uam_machine_cpu(m, i), &cpu_model)) != UAM_OK)
            return st;
";;
    ppc|ppc32)
        ISA=ppc; model_of classic classic booke
        ISA_HEADER="uam/isa/ppc.h"; ISA_DESC="uam_isa_ppc32"; GDB_ENDIAN=big
        RAM_BASE="${RAM_BASE:-0x0}"; RAM_SIZE="${RAM_SIZE:-0x4000000}"; IMAGE_BASE="${IMAGE_BASE:-0x0}"
        irq_cpu "UAM_PPC_IRQ_INT(0)" "a source of the external interrupt pin"
        MEM_DEFINES='#define BOARD_ROM_BASE   UINT64_C(0xfff00000)  /* boot ROM under the reset vector */
#define BOARD_ROM_SIZE   UINT64_C(0x100000)
'
        HELPERS="$FLASH_HELPER"
        MEMORY='    if ((st = map_flash(m, "boot-rom", BOARD_ROM_BASE, BOARD_ROM_SIZE)) != UAM_OK)
        return st;
'
        if [[ $MODEL == booke ]]; then
            RESET_VEC="${RESET_VEC:-0xfffffffc}"
            HELPERS+='/* The Book E watchdog'"'"'s reset (TCR[WRC]): 1 resets the processors, 2 and 3 the machine. */
static void watchdog_reset(void *ctx, uam_cpu *cpu, unsigned kind)
{
    (void)cpu;
    uam_machine_reset_sink(ctx, kind == 1 ? UAM_RESET_CPU : UAM_RESET_SYSTEM, 1);
}

'
            CPU_MODEL="    /* CPU identity the firmware checks (RECON: PVR compares, the TLB geometry it
     * programs): a board-local copy of ember's Book E reference identity (uam_ppc_booke_ref:
     * FPU, isel, TLB0 512 x 4-way, TLB1 16 entries, MAS interface v1, the boot page
     * 0xFFFFF000 mapped at reset around the reset vector 0xFFFFFFFC). Implementation SPRs
     * (HID0, L1CSR0, ...): uam_ppc_set_spr_ops; device control registers:
     * uam_ppc_set_dcr_ops. Each processor reads its number in PIR. The decrementer, the
     * fixed-interval timer and the watchdog are inside the processor. */
    uam_ppc_config cpu_model = uam_ppc_booke_ref;
    cpu_model.name = \"@BOARD@-cpu\";
    for (unsigned i = 0; i < ncpu; i++)
        if ((st = uam_ppc_configure(uam_machine_cpu(m, i), &cpu_model)) != UAM_OK
            || (st = uam_ppc_set_reset_sink(uam_machine_cpu(m, i), watchdog_reset, m)) != UAM_OK)
            return st;
"
        else
            RESET_VEC="${RESET_VEC:-0xfff00100}"
            CPU_MODEL="    /* CPU identity the firmware checks (RECON: PVR compares, HID0 and the other
     * implementation SPRs): a board-local copy of ember's classic reference identity
     * (uam_ppc_ref: FPU, 4 BAT pairs, 32-byte cache blocks). It resets with MSR[IP] = 1:
     * exception vectors at 0xFFF00000 until the firmware clears it (reset_ip = false starts
     * with them at 0). Implementation SPRs: uam_ppc_set_spr_ops (an SPR nobody claims
     * reads back what was written). Each processor reads its number in PIR. The decrementer
     * and the time base are inside the processor. */
    uam_ppc_config cpu_model = uam_ppc_ref;
    cpu_model.name = \"@BOARD@-cpu\";
    for (unsigned i = 0; i < ncpu; i++)
        if ((st = uam_ppc_configure(uam_machine_cpu(m, i), &cpu_model)) != UAM_OK)
            return st;
"
        fi
        DEVICE_NOTE="
     * Several processors interrupt each other through a doorbell block: uam/dev/ipi.h.";;
    avr)
        model_of eplus e eplus xm xt rc
        ISA_HEADER="uam/isa/avr.h"; ISA_DESC="uam_isa_avr"; MAX_CPUS=1
        case $MODEL in   # internal SRAM (data addresses) and the I/O space, as avr-ref lays them out
            e)     sram=0x0100; ssz=0x800;  io=0x20; iosz=0xE0;;
            eplus) sram=0x0200; ssz=0x2000; io=0x20; iosz=0x1E0;;
            xm)    sram=0x2000; ssz=0x2000; io=0x0;  iosz=0x1000;;
            xt)    sram=0x2800; ssz=0x1800; io=0x0;  iosz=0x1000;;
            rc)    sram=0x0040; ssz=0x100;  io=0x0;  iosz=0x40;;
        esac
        RAM_BASE="${RAM_BASE:-$(printf '0x%x' $((0x800000 + sram)))}"; RAM_SIZE="${RAM_SIZE:-$ssz}"
        IMAGE_BASE="${IMAGE_BASE:-0x0}"; RESET_VEC="${RESET_VEC:-0x0}"; ENTRY="${ENTRY:-$RESET_VEC}"
        irq_cpu 1 "interrupt vector 1"
        MEM_NOTE="
 * AVR is Harvard: program memory at bus 0 (byte addresses; the pc is twice the word PC),
 * data address a at UAM_AVR_DATA_BASE + a (0x800000, as in avr-gcc's ELF files). RAM below
 * is the internal SRAM; board_windows[] are bus addresses too (data 0x$(printf %x $io) = $(printf '0x%x' $((0x800000 + io))))."
        HELPERS="$FLASH_HELPER"
        DEFAULT_WINDOWS="    { \"io\", UAM_AVR_DATA_BASE + $(printf '0x%x' $io), $(printf '0x%x' $iosz) },   /* the I/O space: peripherals */
"
        MEMORY='    /* Program memory: the model'"'"'s flash, at bus 0. */
    if ((st = map_flash(m, "flash", 0, cpu_model.flash_size)) != UAM_OK)
        return st;
'
        if [[ $MODEL == xt || $MODEL == rc ]]; then
            MEMORY+='    /* AVRxt and AVRrc parts also see program memory in the data space at 0x4000. */
    if ((st = uam_bus_map_alias(&m->bus, "flash-data", UAM_AVR_DATA_BASE + 0x4000,
                                cpu_model.flash_size < 0xC000 ? cpu_model.flash_size : 0xC000,
                                uam_bus_region(&m->bus, "flash"), 0, NULL)) != UAM_OK)
        return st;
'
        fi
        levels=""
        [[ $MODEL == xm ]] && levels="
     * AVRxm: give each vector its PMIC level, uam_avr_set_irq_level(&m->cpu, vector, 1..3)."
        [[ $MODEL == xt ]] && levels="
     * AVRxt: give each vector its CPUINT level, uam_avr_set_irq_level(&m->cpu, vector, 0|1)."
        CPU_MODEL="    /* CPU model (RECON: the part, avr-gcc -mmcu=): a board-local copy of ember's
     * reference model of the core family (uam_avr_${MODEL}_ref): flash_size, sp_reset (the end
     * of SRAM), vector_size, features (MUL, JMP/CALL, ELPM, SPM, BREAK). r0-r31 and the
     * core's I/O registers (SREG, SP, RAMPx, EIND, CCP, PMIC/CPUINT) are the hart's.$levels
     * Peripherals whose flag clears when their vector is taken: uam_avr_set_irq_ack;
     * self-programming (SPM): uam_avr_set_spm. One processor. */
    uam_avr_config cpu_model = uam_avr_${MODEL}_ref;
    cpu_model.name = \"@BOARD@-cpu\";
    if ((st = uam_avr_configure(&m->cpu, &cpu_model)) != UAM_OK)
        return st;
"
        DEVICE_NOTE="
     * A stand-in timer while the part's own is not modelled: uam/dev/gptimer.h.";;
    avr32)
        model_of a a b
        ISA_HEADER="uam/isa/avr32.h"; ISA_DESC="uam_isa_avr32"; MAX_CPUS=1; GDB_ENDIAN=big
        INCLUDES='#include "uam/dev/avr32_intc.h"'
        if [[ $MODEL == b ]]; then
            RAM_BASE="${RAM_BASE:-0x10000000}"; RAM_SIZE="${RAM_SIZE:-0x2000000}"; code=0x0
            IMAGE_BASE="${IMAGE_BASE:-0xa0000000}"; RESET_VEC="${RESET_VEC:-0xa0000000}"
            MEM_NOTE="
 * AVR32B: segmented map, P1 (0x80000000) and P2 (0xA0000000) reach physical 0-0x1FFFFFFF
 * untranslated, P4 (0xE0000000-) the peripherals; the loader translates image addresses."
        else
            RAM_BASE="${RAM_BASE:-0x0}"; RAM_SIZE="${RAM_SIZE:-0x1000000}"; code=0x80000000
            IMAGE_BASE="${IMAGE_BASE:-0x80000000}"; RESET_VEC="${RESET_VEC:-0x80000000}"
        fi
        ENTRY="${ENTRY:-$RESET_VEC}"
        IRQ_CONNECT="uam_signal_connect(soc_uart_irq_line(uart), uam_avr32_intc_input, intc, 1 * 32);
     *       (group 1, line 0)"
        MEM_DEFINES="#define BOARD_CODE_BASE  UINT64_C($code)  /* code memory: the reset address */
#define BOARD_CODE_SIZE  UINT64_C(0x800000)
#define BOARD_INTC_BASE  UINT64_C(0xffff0800)
"
        MEMORY='    /* Code memory, writable as on avr32-ref (internal flash is written through its own
     * page buffer; a part whose firmware never writes it can make this a ROM). */
    if ((st = uam_bus_map_ram(&m->bus, "code", BOARD_CODE_BASE, BOARD_CODE_SIZE)) != UAM_OK)
        return st;
'
        CPU_MODEL="    /* CPU model (RECON: CONFIG0/CONFIG1 reads, the part): a board-local copy of ember's
     * AVR32${MODEL^^} reference model (uam_avr32${MODEL}_ref: arch, config0, mmu_entries (MPU
     * regions on A, entries per TLB on B), int3_shadow, simd). One processor. */
    uam_avr32_config cpu_model = uam_avr32${MODEL}_ref;
    cpu_model.name = \"@BOARD@-cpu\";
    if ((st = uam_avr32_configure(&m->cpu, &cpu_model)) != UAM_OK)
        return st;
"
        PLATFORM='    /* The interrupt controller every AVR32 board has (uam/dev/avr32_intc.h: request line l
     * of group g is input g * 32 + l; IPRg gives each group a level and an autovector). It
     * drives INT0..INT3 and answers the autovector; the COUNT/COMPARE request is an output
     * of the processor, routed to it (group 0, line 0, as on avr32-ref). Base: RECON. */
    uam_device *intc = uam_avr32_intc_create("intc", 32);
    if ((st = attach(m, intc, BOARD_INTC_BASE, UAM_AVR32_INTC_SIZE)) != UAM_OK)
        return st;
    for (unsigned l = 0; l < 4; l++)
        uam_signal_connect(uam_avr32_intc_level_line(intc, l), uam_cpu_irq_sink, &m->cpu,
                           UAM_AVR32_IRQ_INT0 + l);
    uam_avr32_set_autovector(&m->cpu, uam_avr32_intc_autovector, intc);
    uam_signal_connect(uam_avr32_compare_line(&m->cpu), uam_avr32_intc_input, intc, 0);
';;
    pic)
        model_of pic18 baseline mid enhanced pic18
        ISA_HEADER="uam/isa/pic.h"; ISA_DESC="uam_isa_pic"; MAX_CPUS=1
        case $MODEL in   # data RAM and the reset vector, as pic-ref lays them out
            baseline) dsz=0x100;  rv=0xffe; common=0;;
            mid)      dsz=0x200;  rv=0x0;   common=4;;
            enhanced) dsz=0x1000; rv=0x0;   common=32;;
            pic18)    dsz=0x800;  rv=0x0;   common=0;;
        esac
        RAM_BASE="${RAM_BASE:-0x800000}"; RAM_SIZE="${RAM_SIZE:-$dsz}"
        IMAGE_BASE="${IMAGE_BASE:-0x0}"; RESET_VEC="${RESET_VEC:-$rv}"; ENTRY="${ENTRY:-$RESET_VEC}"
        irq_cpu UAM_PIC_IRQ_PERIPH "PIC18 with IPEN: high priority; UAM_PIC_IRQ_LOW the low one"
        [[ $MODEL == baseline ]] && IRQ_CONNECT="(baseline parts have no interrupts)"
        MEM_NOTE="
 * PIC is Harvard: program memory at bus 0 (12/14-bit words two bytes each, so the pc is
 * twice the word PC; PIC18 byte addressed), data address a at UAM_PIC_DATA_BASE + a
 * (0x800000). board_windows[] are bus addresses too. The core's SFRs never reach the bus."
        MEM_DEFINES="#define BOARD_RAM_PRIO   (UAM_PRIO_BACKGROUND - 1000)  /* data RAM under everything */
"
        RAM_MAP='    /* Data RAM as pic-ref lays it out, mapped UNDER everything: the part'"'"'s peripheral
     * SFRs sit in the banks among the general-purpose bytes, so a device, an --mmio-rule or a
     * board_windows[] catch-all over an SFR range wins over it. */
    st = uam_bus_map_ram_ex(&m->bus, "ram", BOARD_RAM_BASE, cfg->ram_size ? cfg->ram_size : BOARD_RAM_SIZE,
                            &(uam_map_attrs){ .prio = BOARD_RAM_PRIO }, NULL);'
        HELPERS="$FLASH_HELPER"'/* A RESET instruction, or a stack overflow/underflow with STVREN, reset the core: reset
 * the rest of the device too. */
static void core_reset(void *ctx, uam_pic_reset_cause cause)
{
    (void)cause;
    uam_machine_reset_sink(ctx, UAM_RESET_SYSTEM, 1);
}

'
        MEMORY='    /* Program memory: the model'"'"'s flash, at bus 0. */
    if ((st = map_flash(m, "flash", 0, 2u * cpu_model.prog_words)) != UAM_OK)
        return st;
'
        if [[ $common -gt 0 ]]; then
            INCLUDES='#include <stdio.h>'
            MEMORY+="    /* 0x70-0x7F of every bank is one common RAM, as on the parts. */
    for (unsigned bank = 1; bank < $common; bank++) {
        char name[24];
        snprintf(name, sizeof(name), \"common%u\", bank);
        if ((st = uam_bus_map_alias(&m->bus, name, BOARD_RAM_BASE + bank * 0x80u + 0x70u, 0x10,
                                    uam_bus_region(&m->bus, \"ram\"), 0x70,
                                    &(uam_map_attrs){ .prio = BOARD_RAM_PRIO + 1 })) != UAM_OK)
            return st;
    }
"
        fi
        [[ $MODEL == pic18 ]] && DEFAULT_WINDOWS='    { "sfr", UAM_PIC_DATA_BASE + 0xF60, 0xA0 },   /* the peripheral SFRs (access bank) */
'
        [[ $MODEL == baseline ]] && MEM_NOTE+="
 * Baseline: OPTION and the TRIS registers that the OPTION/TRIS instructions write are at
 * data address UAM_PIC_SPECIAL (+f): unmapped here, so --unknown-mmio logs them."
        CPU_MODEL="    /* CPU model (RECON: the part): a board-local copy of ember's reference model of the
     * core family (uam_pic_${MODEL}_ref: prog_words, banks, features). Configuration-word
     * options the firmware relies on go in features: UAM_PIC_F_STVREN, and on PIC18
     * UAM_PIC_F_XINST (the extended instruction set). INTCON's timer 0 / external /
     * port-change flags: uam_pic_intcon_set; PIC18 TBLWT: uam_pic_set_tblwt. One processor. */
    uam_pic_config cpu_model = uam_pic_${MODEL}_ref;
    cpu_model.name = \"@BOARD@-cpu\";
    if ((st = uam_pic_configure(&m->cpu, &cpu_model)) != UAM_OK)
        return st;
    uam_pic_set_reset_hook(&m->cpu, core_reset, m);
"
        CPU_MODEL="${CPU_MODEL//uam_pic_pic18_ref/uam_pic18_ref}"
        DEVICE_NOTE="
     * A stand-in timer while the part's own is not modelled: uam/dev/gptimer.h.";;
    pic32)
        model_of mx mx mm mz
        ISA_HEADER="uam/isa/mips32.h"; ISA_DESC="uam_isa_mips32le"; MAX_CPUS=1
        INCLUDES='#include "uam/dev/pic32_intc.h"'
        case $MODEL in   # pic32-ref's models
            mx) cpu=uam_mips_m16_ref;   ram=0x20000; flash=0x80000;  intc=0x1f881000; vo=false; nirq=96;  nvec=64;  srs=7;;
            mm) cpu=uam_mips_umips_ref; ram=0x8000;  flash=0x40000;  intc=0x1f810000; vo=true;  nirq=64;  nvec=64;  srs=0;;
            mz) cpu=uam_mips_mz_ref;    ram=0x80000; flash=0x200000; intc=0x1f810000; vo=true;  nirq=256; nvec=256; srs=0;;
        esac
        RAM_BASE="${RAM_BASE:-0x0}"; RAM_SIZE="${RAM_SIZE:-$ram}"
        IMAGE_BASE="${IMAGE_BASE:-0xbfc00000}"; RESET_VEC="${RESET_VEC:-0xbfc00000}"; ENTRY="${ENTRY:-$RESET_VEC}"
        IRQ_CONNECT="uam_signal_connect(soc_uart_irq_line(uart), uam_pic32_intc_input, intc, 3);
     *       (source 3)"
        MEM_NOTE="
 * PIC32: kseg0 = physical + 0x80000000, kseg1 = physical + 0xA0000000 (fixed mapping or TLB)."
        MEM_DEFINES="#define BOARD_FLASH_BASE UINT64_C(0x1d000000)  /* program flash */
#define BOARD_FLASH_SIZE UINT64_C($flash)
#define BOARD_BOOT_BASE  UINT64_C(0x1fc00000)  /* boot flash: the reset address 0xBFC00000 */
#define BOARD_BOOT_SIZE  UINT64_C(0x10000)
#define BOARD_INTC_BASE  UINT64_C($intc)
"
        HELPERS="$FLASH_HELPER"
        MEMORY='    if ((st = map_flash(m, "flash", BOARD_FLASH_BASE, BOARD_FLASH_SIZE)) != UAM_OK
        || (st = map_flash(m, "boot-flash", BOARD_BOOT_BASE, BOARD_BOOT_SIZE)) != UAM_OK)
        return st;
'
        DEFAULT_WINDOWS='    { "sfr", UINT64_C(0x1f800000), 0x100000 },   /* the peripheral SFRs */
'
        CPU_MODEL="    /* CPU identity (RECON: the part): a board-local copy of ember's ${MODEL^^}-class reference
     * identity ($cpu, uam/isa/mips32.h): Config1.CA (MIPS16e), Config3.ISA (microMIPS;
     * 3 resets into it), Config1.FP, Config3.DSPP/DSP2P, VEIC (the external interrupt
     * controller below), fixed mapping or a TLB, shadow_sets. One processor. */
    uam_mips_config cpu_model = $cpu;
    cpu_model.name = \"@BOARD@-cpu\";
    if ((st = uam_mips_configure(&m->cpu, &cpu_model)) != UAM_OK)
        return st;
"
        PLATFORM="    /* The interrupt controller every PIC32 has (uam/dev/pic32_intc.h; layout and source
     * counts as pic32-ref's \"$MODEL\"): it presents its request to the core's external
     * interrupt controller interface (Config3.VEIC), so the core timer and the software
     * interrupts are its sources 0-2, not Cause.IP bits. */
    const uam_pic32_intc_config intc_cfg = {
        .n_irqs = $nirq, .n_vectors = $nvec, .variable_offset = $vo, .srs_priority = $srs,
    };
    uam_device *intc = uam_pic32_intc_create(\"intc\", &intc_cfg);
    if ((st = attach(m, intc, BOARD_INTC_BASE, UAM_PIC32_INTC_SIZE)) != UAM_OK)
        return st;
    uam_pic32_intc_set_eic(intc, uam_mips_eic_input, &m->cpu);
    uam_signal_connect(uam_mips_timer_line(&m->cpu), uam_pic32_intc_input, intc, 0);
    uam_signal_connect(uam_mips_swint_line(&m->cpu, 0), uam_pic32_intc_input, intc, 1);
    uam_signal_connect(uam_mips_swint_line(&m->cpu, 1), uam_pic32_intc_input, intc, 2);
"
        DEVICE_NOTE="
     * A stand-in timer while the part's own is not modelled: uam/dev/gptimer.h.";;
    mcs51|8051)
        ISA=mcs51; model_of 8052 8051 8052 cip51
        ISA_HEADER="uam/isa/mcs51.h"; ISA_DESC="uam_isa_mcs51"; MAX_CPUS=1
        code=0x10000; sfr=0x100; [[ $MODEL == cip51 ]] && { code=0x20000; sfr=0x1000; }
        RAM_BASE="${RAM_BASE:-0x1000000}"; RAM_SIZE="${RAM_SIZE:-0x10000}"
        IMAGE_BASE="${IMAGE_BASE:-0x0}"; RESET_VEC="${RESET_VEC:-0x0}"; ENTRY="${ENTRY:-$RESET_VEC}"
        irq_cpu 4 "source 4, the serial port: vector 0x23"
        MEM_NOTE="
 * 8051 spaces on the one bus: code at 0, external data (MOVX) at UAM_MCS51_XDATA_BASE + a
 * (RAM below), SFRs at UAM_MCS51_SFR_BASE + a (with SFR paging + page * 0x100). The
 * internal RAM and the core's SFRs (ACC, B, PSW, SP, DPTR, IE, IP, PCON, ...) are the hart's."
        MEM_DEFINES="#define BOARD_CODE_SIZE  UINT64_C($code)  /* program memory at bus 0 */
"
        HELPERS="$FLASH_HELPER"
        MEMORY='    if ((st = map_flash(m, "code", 0, BOARD_CODE_SIZE)) != UAM_OK)
        return st;
'
        DEFAULT_WINDOWS="    { \"sfr\", UAM_MCS51_SFR_BASE, $sfr },   /* every SFR the core does not own */
"
        banking=""
        [[ $MODEL == cip51 ]] && banking="
     * CIP-51: SFR paging (SFRPAGE at 0x84) and code banking from 0x8000: the bank-select
     * register is a board device (uam_mcs51_reg_create) calling uam_mcs51_set_code_bank."
        CPU_MODEL="    /* CPU model (RECON: the part): a board-local copy of ember's reference model
     * (uam_mcs51_${MODEL}_ref: iram_size, interrupt sources and their enable/priority SFRs,
     * dual DPTR, the MOVX @Ri page register, SFR paging, code banking).$banking
     * Peripherals that clear their flag when the source is taken: uam_mcs51_set_irq_ack.
     * One processor; every instruction is one tick of virtual time. */
    uam_mcs51_config cpu_model = uam_mcs51_${MODEL}_ref;
    cpu_model.name = \"@BOARD@-cpu\";
    if ((st = uam_mcs51_configure(&m->cpu, &cpu_model)) != UAM_OK)
        return st;
"
        DEVICE_NOTE="
     * Every 8051's own blocks are stock devices for the SFR space: uam/dev/mcs51_timer.h
     * (TCON at 0x88, sources 0-3), mcs51_serial.h (SCON at 0x98, source 4: the console),
     * mcs51_port.h (P0-P3 at 0x80/0x90/0xA0/0xB0)." ;;
    m6502|6502)
        ISA=m6502; model_of nmos nmos 2a03 65c02 r65c02
        ISA_HEADER="uam/isa/m6502.h"; ISA_DESC="uam_isa_m6502"; MAX_CPUS=1
        case $MODEL in nmos) cpu=uam_m6502_nmos;; 2a03) cpu=uam_m6502_2a03;; 65c02) cpu=uam_m6502_w65c02;; r65c02) cpu=uam_m6502_r65c02;; esac
        RAM_BASE="${RAM_BASE:-0x0}"; RAM_SIZE="${RAM_SIZE:-0x8000}"
        IMAGE_BASE="${IMAGE_BASE:-0x8000}"; RESET_VEC="${RESET_VEC:-0x8000}"; ENTRY="${ENTRY:-$RESET_VEC}"
        irq_cpu 0 "lines 0-29 are wired-OR into IRQ; UAM_M6502_LINE_NMI is the NMI"
        MEM_NOTE="
 * 6502: one 64 KiB space. Reset starts at the address in the reset vector 0xFFFC (the
 * entry passed to the reset is used only if it cannot be read): BOARD_ENTRY matters only
 * for entry registers, so set it to that address."
        MEM_DEFINES='#define BOARD_ROM_BASE   UINT64_C(0x8000)  /* ROM up to 0xFFFF, the vectors at its top */
#define BOARD_ROM_SIZE   UINT64_C(0x8000)
'
        HELPERS="$FLASH_HELPER"
        MEMORY='    if ((st = map_flash(m, "rom", BOARD_ROM_BASE, BOARD_ROM_SIZE)) != UAM_OK)
        return st;
'
        CPU_MODEL="    /* CPU model (RECON: NMOS or CMOS, decimal mode, the 65C02 extras): a board-local
     * copy of ember's $cpu. A 6502 has no on-chip peripheral and no interrupt acknowledge:
     * a device clears its own line. One processor; one instruction is one tick. */
    uam_m6502_config cpu_model = $cpu;
    cpu_model.name = \"@BOARD@-cpu\";
    if ((st = uam_m6502_configure(&m->cpu, &cpu_model)) != UAM_OK)
        return st;
"
        DEVICE_NOTE="
     * A stand-in timer while the system's own is not modelled: uam/dev/gptimer.h.";;
    z80)
        model_of nmos nmos cmos
        ISA_HEADER="uam/isa/z80.h"; ISA_DESC="uam_isa_z80"; MAX_CPUS=1
        RAM_BASE="${RAM_BASE:-0x0}"; RAM_SIZE="${RAM_SIZE:-0x10000}"
        IMAGE_BASE="${IMAGE_BASE:-0x0}"; RESET_VEC="${RESET_VEC:-0x0}"; ENTRY="${ENTRY:-$RESET_VEC}"
        irq_cpu UAM_Z80_LINE_INT "/INT; UAM_Z80_LINE_NMI is /NMI"
        MEM_NOTE="
 * Z80: memory at bus 0 + address, I/O port p at UAM_Z80_IO_BASE + p (devices on ports
 * are mapped there; board_windows[] too)."
        DEFAULT_WINDOWS='    { "io", UAM_Z80_IO_BASE, 0x100 },   /* every I/O port (low byte decoded) */
'
        CPU_MODEL="    /* CPU model: a board-local copy of ember's uam_z80_${MODEL}_ref. Port decoding: most
     * systems decode the low byte of the port address (RECON: check the I/O decode logic).
     * IM 2 vectors and IM 0 opcodes come from the interrupt-acknowledge hook
     * (uam_z80_set_inta; without one the bus reads 0xFF); peripherals that watch for RETI
     * are told through uam_z80_set_reti. One processor; one instruction is one tick. */
    uam_z80_config cpu_model = uam_z80_${MODEL}_ref;
    cpu_model.name = \"@BOARD@-cpu\";
    if ((st = uam_z80_configure(&m->cpu, &cpu_model)) != UAM_OK)
        return st;
    uam_z80_set_io_decode(&m->cpu, 0x00FF);
"
        DEVICE_NOTE="
     * Stock Z80 CTC (counter/timer, IM 2 vector, daisy chain): uam/dev/z80_ctc.h.";;
    xtensa|xtensaeb)
        model_of lx6 lx106 lx6 lx7
        ISA_HEADER="uam/isa/xtensa.h"; ISA_DESC="uam_isa_$ISA"; MAX_CPUS=4
        [[ $ISA == xtensaeb ]] && GDB_ENDIAN=big
        rv=0x40000400; [[ $MODEL == lx106 ]] && rv=0x40000080
        RAM_BASE="${RAM_BASE:-0x40000000}"; RAM_SIZE="${RAM_SIZE:-0x400000}"
        IMAGE_BASE="${IMAGE_BASE:-0x40000000}"; RESET_VEC="${RESET_VEC:-$rv}"; ENTRY="${ENTRY:-$RESET_VEC}"
        irq_cpu 5 "interrupt 5: external, level 1, in every model's table"
        CPU_MODEL="    /* CPU model (RECON: the chip): a board-local copy of ember's uam_xtensa_$MODEL (the
     * options, interrupt table, vectors, VECBASE and reset vector of that core as its chip
     * configures it); every processor gets it, PRID 0xCDCD, then 0xABAB, 0xABAC, ... (the
     * ESP32's two cores answer 0xCDCD and 0xABAB). CCOUNT/CCOMPARE are the core's own.
     * RER/WER reach a board ERI bus through uam_xtensa_set_eri. */
    uam_xtensa_config cpu_model = uam_xtensa_$MODEL;
    cpu_model.name = \"@BOARD@-cpu\";
    for (unsigned i = 0; i < ncpu; i++) {
        if ((st = uam_xtensa_configure(uam_machine_cpu(m, i), &cpu_model)) != UAM_OK)
            return st;
        uam_xtensa_set_prid(uam_machine_cpu(m, i), i == 0 ? 0xCDCDu : 0xABAAu + i);
    }
";;
    x86_64|x86-64|pc)
        ISA="x86_64"; no_model; ISA_HEADER="uam/isa/x86.h"; ISA_DESC="uam_isa_x86_64"
        BOARD_TPL="$TPL/board-pc.c.in"
        RAM_BASE="0x0"; RAM_SIZE="${RAM_SIZE:-0x20000000}"
        IMAGE_BASE="${IMAGE_BASE:-0x0}"; RESET_VEC="${RESET_VEC:-0xfffffff0}"
        irq_cpu UAM_X86_IRQ_INTR
        # A PC master-aborts (all ones) and firmware depends on it (absent TPM, PCI MMIO,
        # SMRR-protected memory): for "fault" the board installs a master abort that also
        # logs each first access (emulation/master_abort_log.c, from templates/).
        UNKNOWN_DEFAULT="fault";;
    *) die "--isa: mips32be mips32le rv32 arm armeb x86_64 ppc avr avr32 pic pic32 mcs51 m6502 z80 xtensa xtensaeb (ember has no other ISA module)";;
esac
RAM_SIZE="${RAM_SIZE:-0x2000000}"
ENTRY="${ENTRY:-$IMAGE_BASE}"

DIR="${DIR:-${EMU_ROOT:-$HOME/emu}/$BOARD}"
mkdir -p "$DIR"; PROJ="$(cd "$DIR" && pwd)"
for f in emulation patches ember.commit run.sh scripts Makefile; do
    [[ -e "$PROJ/$f" ]] && die "$PROJ/$f already exists; refusing to clobber an existing project"
done

# --- ember checkout + commit ---------------------------------------------------------
if [[ -n "$SRC" ]]; then
    SRC="$(cd "$SRC" && pwd)"; [[ -d "$SRC/.git" ]] || die "--ember-src is not a git checkout: $SRC"
    [[ -f "$SRC/include/uam/machine.h" ]] || die "$SRC does not look like ember (no include/uam/machine.h)"
    echo "$SRC" > "$PROJ/ember.src"
    [[ -n "$COMMIT" ]] || COMMIT="$(git -C "$SRC" rev-parse HEAD)"
else
    if [[ -z "$UPSTREAM" ]]; then
        UPSTREAM="${EMBER_UPSTREAM:-}"
        [[ -z "$UPSTREAM" && -d "$HOME/ember/.git" ]] && UPSTREAM="$HOME/ember"
    fi
    [[ -n "$UPSTREAM" ]] || die "no ember source: pass --ember-src or --ember-upstream (or set EMBER_UPSTREAM)"
    [[ -d "$UPSTREAM" ]] && UPSTREAM="$(cd "$UPSTREAM" && pwd)"
    echo "$UPSTREAM" > "$PROJ/ember.upstream"
    if [[ -z "$COMMIT" ]]; then
        # the pattern HEAD also matches refs/remotes/*/HEAD in a local clone: take HEAD itself
        COMMIT="$(git ls-remote "$UPSTREAM" HEAD | awk '$2 == "HEAD" {print $1; exit}')" || true
        [[ -n "$COMMIT" ]] || die "could not resolve HEAD of $UPSTREAM; pass --ember-commit"
    fi
fi
LATEST="$(if [[ -n "$SRC" ]]; then git -C "$SRC" rev-parse HEAD; else git ls-remote "$UPSTREAM" HEAD | awk '$2 == "HEAD" {print $1; exit}'; fi 2>/dev/null || true)"
if [[ -n "$LATEST" && "$COMMIT" != "$LATEST"* ]]; then
    echo "note: pinning $COMMIT, not the latest ember commit ${LATEST:0:12}; record why in README.md" >&2
fi
echo "$COMMIT" > "$PROJ/ember.commit"
mkdir -p "$PROJ/emulation" "$PROJ/patches" "$PROJ/scripts" "$PROJ/tools"
touch "$PROJ/patches/.keep" "$PROJ/tools/.keep"

# --- project-local copies of the helpers ---------------------------------------------
cp "$HERE"/_common.sh "$HERE"/build-ember.sh "$HERE"/rebuild.sh "$HERE"/regen-patches.sh \
   "$HERE"/repin.sh "$HERE"/check-layering.sh "$HERE"/log_summary.py "$HERE"/gdb_trace_calls.py "$PROJ/scripts/"
chmod +x "$PROJ"/scripts/*.sh "$PROJ"/scripts/*.py

subst() {  # template -> file
    sed -e "s|@BOARD@|$BOARD|g" -e "s|@CNAME@|$CNAME|g" -e "s|@GUARD@|$GUARD|g" \
        -e "s|@ISA_HEADER@|$ISA_HEADER|g" -e "s|@ISA_DESC@|$ISA_DESC|g" \
        -e "s|@RAM_BASE@|$RAM_BASE|g" -e "s|@RAM_SIZE@|$RAM_SIZE|g" \
        -e "s|@IMAGE_BASE@|$IMAGE_BASE|g" -e "s|@ENTRY@|$ENTRY|g" -e "s|@RESET_VEC@|$RESET_VEC|g" \
        -e "s|@MAX_CPUS@|$MAX_CPUS|g" -e "s|@DESC@|$BOARD firmware emulation ($ISA${MODEL:+, $MODEL})|g" \
        "$1" > "$2"
}
# The board: the per-ISA pieces go in first (they may hold @BOARD@), then the substitution.
TMPB="$(mktemp)"; trap 'rm -f "$TMPB"' EXIT
SNIP_INCLUDES="$INCLUDES" SNIP_HELPERS="$HELPERS" SNIP_CPU_MODEL="$CPU_MODEL" SNIP_MEMORY="$MEMORY" \
SNIP_PLATFORM="$PLATFORM" SNIP_DEFAULT_WINDOWS="$DEFAULT_WINDOWS" SNIP_MEM_NOTE="$MEM_NOTE" \
SNIP_MEM_DEFINES="$MEM_DEFINES" SNIP_DEVICE_NOTE="$DEVICE_NOTE" SNIP_RAM_MAP="$RAM_MAP" \
SNIP_IRQ_CONNECT="$IRQ_CONNECT" python3 - "$BOARD_TPL" "$TMPB" <<'EOF'
import os, sys
src, dst = sys.argv[1:3]
s = open(src).read()
for key in ("INCLUDES", "HELPERS", "CPU_MODEL", "MEMORY", "PLATFORM", "DEFAULT_WINDOWS",
            "MEM_NOTE", "MEM_DEFINES", "DEVICE_NOTE", "RAM_MAP", "IRQ_CONNECT"):
    v = os.environ.get("SNIP_" + key, "")
    tag = "@%s@" % key
    if key in ("INCLUDES", "RAM_MAP"):              # whole lines
        s = s.replace(tag + "\n", v.rstrip("\n") + "\n" if v else "")
    elif key in ("CPU_MODEL", "PLATFORM"):          # blocks, a blank line after them
        s = s.replace(tag + "\n", v.rstrip("\n") + "\n\n" if v else "")
    s = s.replace(tag, v)
open(dst, "w").write(s)
EOF
subst "$TMPB" "$PROJ/emulation/${CNAME}_board.c"
subst "$TPL/board.h.in" "$PROJ/emulation/${CNAME}_board.h"
subst "$TPL/main.c.in" "$PROJ/emulation/main.c"
subst "$TPL/Makefile.in" "$PROJ/Makefile"
[[ "$ISA" == x86_64 ]] && cp "$TPL/master_abort_log.c" "$TPL/master_abort_log.h" "$PROJ/emulation/"

# The images run.sh hands the board (literal text for the script below).
if [[ "$ISA" == x86_64 ]]; then
    RUN_USAGE='FLASH=bios.bin'
    RUN_IMAGES='IMG=()
[[ -n "${FLASH:-}" ]] && IMG+=(--image flash="$FLASH")   # boot flash (BIOS region) ending at 4 GiB
[[ -n "${CODE:-}" ]] && IMG+=(--image code="$CODE")      # or an OVMF-style split: code ...
[[ -n "${VARS:-}" ]] && IMG+=(--image vars="$VARS")      # ... and the variable store below it
[[ -n "${CMOS:-}" ]] && IMG+=(--image cmos="$CMOS")      # initial CMOS contents
[[ -n "${IMAGE:-}" ]] && IMG+=(--image os="$IMAGE")      # a raw stage in RAM, real mode
[[ ${#IMG[@]} -gt 0 ]] || { echo "run.sh: set FLASH=bios.bin, or CODE= and VARS=, or IMAGE=stage.bin" >&2; exit 2; }'
    RUN_IMAGE_ARGS='"${IMG[@]}"'
else
    RUN_USAGE='IMAGE=fw.bin'
    RUN_IMAGES='IMAGE="${IMAGE:?set IMAGE=/path/to/firmware (raw blob or ELF)}"'
    RUN_IMAGE_ARGS='--image os="$IMAGE"'
fi
cat > "$PROJ/run.sh" <<EOF
#!/usr/bin/env bash
# run.sh - launch the $BOARD emulation. Extra args go to the runner, e.g.
#   -n 200000000 (instruction budget)  --gdb 1234  --log-pc 0x...  --break 0x... -d  --jit
# Console on stdout, diagnostics (catch-all logs, --trace exc, exit line, stats) on stderr:
#   $RUN_USAGE timeout -k 5 300 ./run.sh -n 200000000 > build/console.txt 2> build/emu.log < /dev/null
set -euo pipefail
ISA=$ISA
GDB_ENDIAN=$GDB_ENDIAN
PROJ="\$(cd "\$(dirname "\${BASH_SOURCE[0]}")" && pwd)"
RUN="\${EMU_BIN:-\$PROJ/build/$BOARD-run}"
$RUN_IMAGES

exec "\$RUN" -b $BOARD \\
    $RUN_IMAGE_ARGS \\
    --unknown-mmio "\${UNKNOWN_MMIO:-$UNKNOWN_DEFAULT}" \\
    --trace "\${TRACE-exc}" --stats \\
    "\$@"
# Grows with the board, e.g.:
#   --image flash="\$PROJ/build/flash.bin"        (a named image the board places itself)
#   -o key=value                                  (board options: uam_board_config_option)
#   --mmio-rule 0x18040010=fixed:0x1              (answer one register before it has a device;
#                                                  an x86 I/O port is at 0x10000000000 + port)
#   --nic lan=tap:tap0,pcap=build/lan.pcap        (ports named by the board)
#   --console string:'help\\r'                     (deterministic console input; ,after='> ' waits for
#                                                  the prompt; script:FILE for expect/send steps)
#   --watch p:0x80001000+4 / --guard ADDR+LEN:w   (log / stop on guest accesses to memory)
#   --snapshot-at 0x... --save-snapshot build/booted.snap   then  --load-snapshot build/booted.snap
#                                                 (boot once; same board, options and rules)
EOF
chmod +x "$PROJ/run.sh"

[[ -e "$PROJ/README.md" ]] || cat > "$PROJ/README.md" <<EOF
# $BOARD emulation

An ember board for \`$BOARD\` ($ISA). **No code here lives in the ember tree**: the
project links against ember's library, built from a pinned commit.

| path | what |
|---|---|
| \`RECON.md\` | recon fact sheet (input) |
| \`iter-XX.md\` | bring-up journal, one per iteration |
| \`emulation/\` | our C sources: \`${CNAME}_board.c\` (wiring only), one file per hardware component, \`main.c\` |
| \`ember.commit\` | pinned ember commit; \`ember.upstream\` / \`ember.src\`: where ember comes from |
| \`patches/\` | every change to ember itself, as \`git format-patch\` files applied as commits on the pin (normally empty) |
| \`scripts/\` | build-ember.sh, rebuild.sh, regen-patches.sh, repin.sh, check-layering.sh, log_summary.py, gdb_trace_calls.py |
| \`tools/\` | generators for data the firmware reads (flash, NVRAM, calibration) |
| \`run.sh\` | the runner invocation under construction |

## ember.commit
\`$COMMIT\`: the latest ember commit when the project was created. Move it to the latest
with \`scripts/repin.sh\` (it rebases patches/ onto the new commit), and note each move here.

## Build
    scripts/build-ember.sh    # full: ember-src reset to ember.commit -> patches/ as commits -> libuam -> build/$BOARD-run
    scripts/rebuild.sh        # fast: after editing emulation/ files
    scripts/regen-patches.sh  # after committing a change to ember in ember-src: export it to patches/
    scripts/repin.sh          # move to the latest ember commit, carrying patches/ along
## Run
    IMAGE=/path/to/firmware timeout -k 5 300 ./run.sh -n 200000000 > build/console.txt 2> build/emu.log < /dev/null
    python3 scripts/log_summary.py build/emu.log
EOF

if [[ -n "$RECON" ]]; then
    [[ -f "$RECON" ]] || die "recon report not found: $RECON"
    [[ "$(cd "$(dirname "$RECON")" && pwd)/$(basename "$RECON")" == "$PROJ/RECON.md" ]] || cp -n "$RECON" "$PROJ/RECON.md"
fi

touch "$PROJ/.gitignore"
for pat in .ember-build/ ember-src/ build/ '*.log' __pycache__/; do
    grep -qxF "$pat" "$PROJ/.gitignore" || echo "$pat" >> "$PROJ/.gitignore"
done
if ! git -C "$PROJ" rev-parse --git-dir >/dev/null 2>&1; then
    git -C "$PROJ" init -q
fi
echo "== scaffolded $PROJ (board '$BOARD', $ISA, ember ${COMMIT:0:12}) =="
echo "next: fill BOARD_* constants and board_windows[] in emulation/${CNAME}_board.c from RECON.md,"
echo "      then scripts/build-ember.sh"
