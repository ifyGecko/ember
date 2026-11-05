#!/usr/bin/env bash
# recon.sh - firmware reconnaissance orchestrator (self-contained; needs python3, optional
# binwalk, optional Ghidra for the analysis pass).
#
# Usage:
#   recon.sh <blob> [--arch ARCH] [--endian auto|be|le]
#            [--processor <ghidra-lang-id>] [--name <ghidra-project-name>]
#            [--base 0x...]     # skip the base search and use this base
#            [--align 0x1000]   # candidate base alignment for basefind.py
#            [--report FILE]    # draft fact sheet (default: ./RECON.md)
#            [--out DIR]        # working artifacts (default: ./.recon)
#            [--no-ghidra]      # stop after the Python tools (fwinfo, binwalk, basefind,
#                               # raw MMIO scan, uefi_scan)
#            [--reuse]          # re-run the Ghidra scripts on an existing project (no re-import)
#            [--uefi]           # PC firmware: force the UEFI flow (auto for x86 images that
#                               # hold firmware volumes or a flash descriptor)
#            [--modules pei|all|NAME,NAME,...]  # UEFI flow: which extracted modules Ghidra
#                               # analyses (default pei: SEC, PEI core and PEIMs - the code
#                               # that runs first and touches the chipset)
#
#   ARCH (default: what fwinfo.py reads from the container - ELF e_machine, an Espressif
#   image - or, for a raw image, a clear instruction-fingerprint or vector-table lead):
#         mips32 | pic32 (= mips32, little-endian) | rv32 | arm | armeb (BE8: big-endian
#         data, little-endian code) | x86_16 (real mode: a legacy BIOS, a reset stub) |
#         x86_32 | x86_64 | ppc32 | avr | avr32 | pic-baseline | pic-mid | pic-enhanced |
#         pic18 | mcs51 | 6502 | 65c02 | z80 | xtensa | xtensaeb
#
# Flow (embedded images): fwinfo.py (container and architecture: ELF, Intel HEX -> raw
#       binary, Espressif image -> ELF) -> binwalk container scan -> basefind.py
#       (string-pointer voting, paired-hit re-rank; skipped for ELF and where the
#       architecture fixes the base: AVR, PIC, 8051 and Z80 program memory at 0, 6502 from
#       its vectors) -> mmio_scan.py (raw MIPS32 register-access table + polling loops) ->
#       flashmap_scan.py (partition-table candidates + flash references) -> Ghidra headless
#       import (raw: at the chosen base; ELF: its own segments) + FirmwareRecon.java +
#       FindMmio.java (+ FindX86Io.java on x86) -> draft RECON.md.
# Flow (PC / UEFI images): uefi_scan.py (flash descriptor, BIOS window, reset vector, FIT,
#       microcode CPU signatures, firmware volumes, modules with their XIP addresses, variable
#       store; --extract into OUT/modules) -> Ghidra headless import of the selected modules
#       (PE loader: each image at its own base) + FirmwareRecon + FindMmio + FindX86Io per
#       module -> draft RECON.md with the per-module port / PCI / MSR / CPUID / MMIO tables.
#
# Run it once per CODE STAGE at that stage's run address, including an XIP bootstrap that
# executes from the reset vector (e.g. the first bytes of the flash at --base 0xBFC00000
# with --no-ghidra): early stages touch clocks, DRAM, on-chip SRAM and polls that the
# main OS never does, and the ember board has to satisfy them first.
#
# Env (all optional):
#   GHIDRA_HOME      Ghidra install dir (auto-detected: $GHIDRA_HOME, analyzeHeadless on
#                    PATH, ~/ghidra*, /opt/ghidra*)
#   GHIDRA_PROJECTS  where headless projects live (default ~/GhidraProjects)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
die() { echo "recon: $*" >&2; exit 1; }

find_headless() {
    local c
    if [[ -n "${GHIDRA_HOME:-}" && -x "$GHIDRA_HOME/support/analyzeHeadless" ]]; then
        echo "$GHIDRA_HOME/support/analyzeHeadless"; return
    fi
    if command -v analyzeHeadless >/dev/null 2>&1; then command -v analyzeHeadless; return; fi
    for c in "$HOME"/ghidra*/support/analyzeHeadless /opt/ghidra*/support/analyzeHeadless; do
        [[ -x "$c" ]] && { echo "$c"; return; }
    done
}

# --- args -------------------------------------------------------------------
BLOB=""; ARCH=""; ENDIAN="auto"; PROC=""; NAME=""; OUT=""; BASE=""; REPORT=""
ALIGN="0x1000"; USE_GHIDRA=1; REUSE=0; UEFI=""; MODULES="pei"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) ARCH="$2"; shift 2;;
        --endian) ENDIAN="$2"; shift 2;;
        --processor) PROC="$2"; shift 2;;
        --name) NAME="$2"; shift 2;;
        --out) OUT="$2"; shift 2;;
        --report) REPORT="$2"; shift 2;;
        --base) BASE="$2"; shift 2;;
        --align) ALIGN="$2"; shift 2;;
        --no-ghidra) USE_GHIDRA=0; shift;;
        --reuse) REUSE=1; shift;;
        --uefi) UEFI=1; shift;;
        --modules) MODULES="$2"; shift 2;;
        -h|--help) sed -n '2,50p' "$0"; exit 0;;
        -*) die "unknown option: $1";;
        *) BLOB="$1"; shift;;
    esac
done
[[ -n "$BLOB" && -f "$BLOB" ]] || die "give a readable firmware blob"
BLOB="$(cd "$(dirname "$BLOB")" && pwd)/$(basename "$BLOB")"
BLOBNAME="$(basename "$BLOB")"
ORIG="$BLOB"
NAME="${NAME:-${BLOBNAME}_recon}"
REPORT="${REPORT:-$(pwd)/RECON.md}"
OUT="${OUT:-$(pwd)/.recon}"
GHIDRA_PROJECTS="${GHIDRA_PROJECTS:-$HOME/GhidraProjects}"
# absolute paths: they end up in modules.tsv, symlink targets and Ghidra script arguments
[[ "$OUT" == /* ]] || OUT="$(pwd)/$OUT"
[[ "$REPORT" == /* ]] || REPORT="$(pwd)/$REPORT"
[[ "$GHIDRA_PROJECTS" == /* ]] || GHIDRA_PROJECTS="$(pwd)/$GHIDRA_PROJECTS"
mkdir -p "$OUT"
[[ "$ARCH" == pic32 ]] && { ARCH=mips32; ENDIAN=le; }

echo "== recon: $BLOB ($(stat -c%s "$BLOB") bytes) arch=${ARCH:-auto} endian=$ENDIAN =="
echo "== working dir: $OUT =="

# --- 0. container and architecture (fwinfo.py) -----------------------------------
# ELF: its own segments; Intel HEX: program memory as a raw binary; Espressif image: an ELF
jget() { python3 -c 'import json,sys
d=json.load(open(sys.argv[1]))
for k in sys.argv[2].split("."):
    d = d.get(k, "") if isinstance(d, dict) else ""
print(d if d is not None else "")' "$OUT/fwinfo.json" "$1"; }
FW_ARGS=()
case "$(head -c 1 "$BLOB" | od -An -tx1 | tr -d ' ')" in
    3a) FW_ARGS=(--bin "$OUT/$BLOBNAME.bin");;            # ':' Intel HEX
    e9|ea) FW_ARGS=(--elf "$OUT/$BLOBNAME.elf");;         # Espressif image
esac
[[ $(stat -c%s "$BLOB") -gt 32800 ]] && [[ ${#FW_ARGS[@]} -eq 0 ]] && FW_ARGS=(--elf "$OUT/$BLOBNAME.elf")
rm -rf "$OUT/$BLOBNAME.bin" "$OUT/$BLOBNAME.elf" "$OUT/segments"
echo "--- fwinfo.py (container, architecture hints) ---"
python3 "$HERE/fwinfo.py" "$BLOB" "${FW_ARGS[@]}" --segments "$OUT/segments" | tee "$OUT/fwinfo.txt"
python3 "$HERE/fwinfo.py" "$BLOB" --json > "$OUT/fwinfo.json"
FORMAT="$(jget format)"
SUGG_ARCH="$(jget suggest.arch)"; SUGG_ENDIAN="$(jget suggest.endian)"
SUGG_PROC="$(jget suggest.ghidra)"; SUGG_BASE="$(jget suggest.base)"
IS_ELF=0
case "$FORMAT" in
    ELF*) IS_ELF=1;;
    "Intel HEX")
        BLOB="$OUT/$BLOBNAME.bin"; BLOBNAME="$BLOBNAME.bin"
        [[ -n "$BASE" ]] || { BASE="$(jget program.base)"; HEXBASE=1; }
        echo "== Intel HEX: program memory written to $BLOB (base $BASE; far ranges listed above) ==";;
    Espressif*)
        if [[ -f "$OUT/$BLOBNAME.elf" ]]; then
            BLOB="$OUT/$BLOBNAME.elf"; BLOBNAME="$BLOBNAME.elf"; IS_ELF=1
            echo "== Espressif image: segments written as $BLOB (ELF) =="
        fi;;
esac
if [[ -z "$ARCH" ]]; then
    if [[ -z "$UEFI" ]] && python3 - "$BLOB" <<'EOF'
import sys
d = open(sys.argv[1], "rb").read(32 << 20)
sys.exit(0 if d.find(b"_FVH") >= 0x28 or b"\x5a\xa5\xf0\x0f" in (d[16:20], d[0:4]) else 1)
EOF
    then
        ARCH=x86_64
    elif [[ -n "$SUGG_ARCH" ]]; then
        ARCH="$SUGG_ARCH"
        [[ "$ENDIAN" == auto && -n "$SUGG_ENDIAN" ]] && ENDIAN="$SUGG_ENDIAN"
        echo "== arch from fwinfo.py: $ARCH/$ENDIAN ($(jget suggest.basis)) =="
    elif [[ "$IS_ELF" == 1 ]]; then
        die "ELF for $(jget machine): no ember ISA module takes it (notes in $OUT/fwinfo.txt; the gate in SKILL.md). Pass --arch to recon it anyway"
    else
        die "architecture not recognised from the container or the code: read $OUT/fwinfo.txt and the arch cheat sheet, then pass --arch"
    fi
fi
[[ -z "$PROC" && -n "$SUGG_PROC" && "$ARCH" == "$SUGG_ARCH" ]] && PROC="$SUGG_PROC"

case "$ARCH" in
    mips32|rv32|arm|armeb|x86_16|x86_32|x86_64|ppc32|avr|avr32|pic-baseline|pic-mid|pic-enhanced|pic18|mcs51|6502|65c02|z80|xtensa|xtensaeb) ;;
    *) echo "WARNING: ember has no ISA module for '$ARCH' (see the gate in SKILL.md). Recon can"
       echo "         continue, but an ember board cannot run this target without new ISA work.";;
esac
# fixed byte orders (mcs51: Ghidra's 8051 language is big-endian for its 16-bit values)
case "$ARCH" in
    armeb|avr32|ppc32|xtensaeb|mcs51) [[ "$ENDIAN" == auto ]] && ENDIAN=be;;
    x86_*|arm|rv32|avr|pic-*|pic18|6502|65c02|z80|xtensa) [[ "$ENDIAN" == auto ]] && ENDIAN=le;;
esac
[[ "$ARCH" == armeb ]] && ENDIAN=be
if [[ -z "$UEFI" && "$ARCH" == x86_* ]]; then
    python3 - "$BLOB" <<'EOF' && UEFI=1 || UEFI=0
import sys
d = open(sys.argv[1], "rb").read(32 << 20)
fvh = d.find(b"_FVH")
ifd = d[16:20] == b"\x5a\xa5\xf0\x0f" or d[0:4] == b"\x5a\xa5\xf0\x0f"
sys.exit(0 if (fvh >= 0x28 or ifd) else 1)
EOF
fi
[[ "$UEFI" == 1 ]] && echo "== PC/UEFI image: firmware volumes or a flash descriptor (uefi_scan.py flow) =="

# --- 1. container scan --------------------------------------------------------
if command -v binwalk >/dev/null 2>&1; then
    echo "--- binwalk ---"
    binwalk "$ORIG" | tee "$OUT/binwalk.txt" || true
else
    echo "note: binwalk not installed; skipping container scan" | tee "$OUT/binwalk.txt"
fi

# --- Ghidra language id (once the byte order is known) ----------------------------
ghidra_lang() {
    case "$ARCH/$ENDIAN" in
        mips32/be) echo "MIPS:BE:32:default";;
        mips32/le) echo "MIPS:LE:32:default";;
        arm/le)    echo "ARM:LE:32:v7";;
        armeb/be)  echo "ARM:LEBE:32:v7LEInstruction";;
        arm/be)    echo "ARM:BE:32:v7";;            # BE32: Ghidra reads it, ember does not run it
        rv32/*)    echo "RISCV:LE:32:default";;
        x86_16/*)  echo "x86:LE:16:Real Mode";;
        x86_32/*)  echo "x86:LE:32:default";;
        x86_64/*)  echo "x86:LE:64:default";;
        ppc32/be)  echo "PowerPC:BE:32:default";;   # Book E (e500): PowerPC:BE:32:e500
        ppc32/le)  echo "PowerPC:LE:32:default";;
        avr/*)     if [[ $(stat -c%s "$BLOB") -gt 131072 && $IS_ELF == 0 ]]; then
                       echo "avr8:LE:16:atmega256"; else echo "avr8:LE:16:default"; fi;;
        avr32/*)   echo "avr32:BE:32:default";;
        pic-baseline/*) echo "PIC-16:LE:16:PIC-16C5x";;
        pic-mid/*) echo "PIC-16:LE:16:PIC-16";;
        pic-enhanced/*) echo "PIC-16:LE:16:PIC-16F";;
        pic18/*)   echo "PIC-18:LE:24:PIC-18";;
        mcs51/*)   echo "8051:BE:16:default";;      # banked CIP-51 code: 8051:BE:24:cip-51
        6502/*)    echo "6502:LE:16:default";;
        65c02/*)   echo "65C02:LE:16:default";;
        z80/*)     echo "z80:LE:16:default";;
        xtensa/*)  echo "Xtensa:LE:32:default";;
        xtensaeb/*) echo "Xtensa:BE:32:default";;
    esac
}

if [[ "$UEFI" == 1 ]]; then
    # --- 2u. PC firmware structure ---------------------------------------------
    echo "--- uefi_scan.py (descriptor, BIOS window, reset vector, FIT, microcode, volumes, modules, variables) ---"
    rm -rf "$OUT/modules"
    python3 "$HERE/uefi_scan.py" "$BLOB" --extract "$OUT/modules" | tee "$OUT/uefi-scan.txt"
    {
      echo "# Recon fact sheet: $BLOBNAME (DRAFT - confirm and annotate; see recon-report-schema.md)"
      echo
      echo "- blob: \`$BLOB\` ($(stat -c%s "$BLOB") bytes, sha256 $(sha256sum "$BLOB" | cut -c1-64))"
      echo "- arch: **x86 PC firmware** (UEFI/PI volumes or a flash descriptor); modules in \`$OUT/modules\`"
      echo "- ember support: _TODO (x86-64: real, protected, long mode, SMM; see the gate in SKILL.md)_"
      echo "- platform: _TODO (chipset from PCI IDs and the BIOS ID; CPU from the microcode signatures)_"
      echo
      echo "## Container scan"; echo '```'; cat "$OUT/binwalk.txt"; echo '```'
      echo; echo "## Image structure (uefi_scan.py)"; echo '```'; cat "$OUT/uefi-scan.txt"; echo '```'
    } > "$REPORT"
    [[ "$USE_GHIDRA" -eq 1 ]] || { echo "== --no-ghidra: draft report at $REPORT =="; exit 0; }
    HEADLESS="$(find_headless)"
    [[ -n "$HEADLESS" ]] || die "Ghidra analyzeHeadless not found (set GHIDRA_HOME) - or use --no-ghidra"
    # which modules: the manifest's rows (name guid type format machine base entry xip file)
    SEL="$OUT/selected"; rm -rf "$SEL"; mkdir -p "$SEL" "$OUT/ghidra"
    python3 - "$OUT/modules/modules.tsv" "$MODULES" "$SEL" <<'EOF'
import os, sys
rows = [l.rstrip("\n").split("\t") for l in open(sys.argv[1])][1:]
want, sel = sys.argv[2], sys.argv[3]
early = {"SEC", "PEI_CORE", "PEIM", "PEIM+DRIVER"}
names = set(want.split(",")) if want not in ("pei", "all") else set()
n = 0
for name, guid, typ, fmt, mach, base, entry, xip, path in rows:
    if path == "-" or not os.path.exists(path):
        continue
    if want == "all" or (want == "pei" and typ in early) or name in names or guid in names:
        # a relative target would resolve against $SEL, not the cwd
        os.symlink(os.path.abspath(path), os.path.join(sel, os.path.basename(path)))
        n += 1
print("selected %d modules (%s)" % (n, want))
EOF
    mkdir -p "$GHIDRA_PROJECTS"
    echo "--- Ghidra: import + analysis of the selected modules, FirmwareRecon + FindMmio + FindX86Io ---"
    POST=(-scriptPath "$HERE/ghidra" -postScript FirmwareRecon.java "$OUT/ghidra"
          -postScript FindMmio.java "$OUT/ghidra" -postScript FindX86Io.java "$OUT/ghidra")
    if [[ "$REUSE" -eq 1 && -d "$GHIDRA_PROJECTS/$NAME.rep" ]]; then
        "$HEADLESS" "$GHIDRA_PROJECTS" "$NAME" -process -noanalysis "${POST[@]}" 2>&1 | tee "$OUT/headless.log" >/dev/null
    else
        "$HEADLESS" "$GHIDRA_PROJECTS" "$NAME" -import "$SEL" -recursive -overwrite "${POST[@]}" \
            2>&1 | tee "$OUT/headless.log" >/dev/null
    fi
    for f in "$OUT"/ghidra/*.x86io.txt; do
        [[ -f "$f" ]] || continue
        { echo; echo "## $(basename "$f" .x86io.txt): ports, PCI, MSRs, CPUID, MMIO accessors (FindX86Io)"
          echo '```'; cat "$f"; echo '```'
          m="${f%.x86io.txt}.mmio.txt"
          [[ -f "$m" ]] && { echo; echo "### MMIO references (FindMmio)"; echo '```'; cat "$m"; echo '```'; }
        } >> "$REPORT"
    done
    echo "== done. Draft: $REPORT  (Ghidra project: $GHIDRA_PROJECTS/$NAME; modules: $OUT/modules) =="
    echo "== Next: identify the chipset (PCI IDs the PEIMs compare, BIOS ID), the CPU (microcode,  =="
    echo "== CPUID compares), the memory init path (MRC/FSP-M, SPD reads) and the console; then   =="
    echo "== rewrite RECON.md into the schema (references/recon-report-schema.md, PC section).    =="
    exit 0
fi

# --- 2. base address ------------------------------------------------------------
METHOD="supplied"
[[ -n "${HEXBASE:-}" ]] && METHOD="Intel HEX: lowest program-memory address"
SIZE="$(stat -c%s "$BLOB")"
if [[ "$IS_ELF" == 1 ]]; then
    BASE="(ELF segments)"; METHOD="ELF program headers"
elif [[ -z "$BASE" && -n "$SUGG_BASE" && "$ARCH" == "$SUGG_ARCH" ]]; then
    BASE="$SUGG_BASE"; METHOD="fwinfo.py: $(jget suggest.basis)"
elif [[ -z "$BASE" ]]; then
    case "$ARCH" in
        avr|pic-*|pic18|mcs51|z80)
            BASE=0x0; METHOD="architectural: program memory and the reset vector at 0";;
        6502|65c02)
            [[ "$SIZE" -le 65536 ]] || die "a 6502 image over 64 KiB is banked: pass --base for the bank under study"
            BASE="$(printf '0x%x' $((65536 - SIZE)))"; METHOD="assumed: the image ends at 0xFFFF (vectors at the top)";;
    esac
fi
if [[ "$IS_ELF" == 0 && ( -z "$BASE" || "$ARCH" == z80 || "$ARCH" == 6502 || "$ARCH" == 65c02 ) ]]; then
    WORD=4; [[ "$ARCH" == z80 || "$ARCH" == 6502 || "$ARCH" == 65c02 ]] && WORD=2
    BFE="$ENDIAN"; [[ "$WORD" == 2 ]] && BFE=auto
    [[ "$WORD" == 2 && "$ALIGN" == 0x1000 ]] && ALIGN=0x100
    echo "--- basefind.py (string-pointer voting, paired-hit re-rank; --word $WORD) ---"
    python3 "$HERE/basefind.py" "$BLOB" --endian "$BFE" --align "$ALIGN" --word "$WORD" \
        | tee "$OUT/basefind.txt" || true
    python3 "$HERE/basefind.py" "$BLOB" --endian "$BFE" --align "$ALIGN" --word "$WORD" --json \
        > "$OUT/basefind.json" || true
    if [[ -z "$BASE" ]]; then
        BASE="$(python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print(d.get("winner","") if d.get("paired",0)>0 else "")' "$OUT/basefind.json" 2>/dev/null || true)"
        [[ -n "$BASE" ]] || die "no corroborated base (0 paired hits). Is this decompressed code? Try --align 0x100, or pass --base."
        METHOD="basefind.py"
        [[ "$ENDIAN" == "auto" ]] && ENDIAN="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["endian"])' "$OUT/basefind.json")"
    fi
fi
echo "== base: $BASE [$METHOD], endian: $ENDIAN =="
if [[ "$ENDIAN" == "auto" ]]; then
    ENDIAN="be"
    echo "WARNING: byte order unknown (base supplied, no fingerprint lead): assuming big-endian; pass --endian"
fi
[[ -n "$PROC" ]] || PROC="$(ghidra_lang)"
[[ -n "$PROC" ]] || die "no default Ghidra language for $ARCH/$ENDIAN; pass --processor"

# --- 3. raw MMIO scan (MIPS32 encoding only) ---------------------------------------
if [[ "$ARCH" == mips32 ]]; then
    SCAN=()
    if [[ "$IS_ELF" == 1 ]]; then
        # each executable segment at its virtual address
        while read -r f va; do SCAN+=("$f:$va"); done < <(python3 - "$OUT/fwinfo.json" "$OUT/segments" <<'EOF'
import json, os, sys
d = json.load(open(sys.argv[1]))
for s in d.get("segments", []):
    p = os.path.join(sys.argv[2], "seg-%08x.bin" % s["vaddr"])
    if "x" in s["flags"] and s["filesz"] and os.path.exists(p):
        print(p, hex(s["vaddr"]))
EOF
)
    else
        SCAN=("$BLOB:$BASE")
    fi
    : > "$OUT/mmio-scan.txt"; : > "$OUT/flashmap.txt"
    for s in "${SCAN[@]}"; do
        f="${s%:*}"; b="${s##*:}"
        echo "--- mmio_scan.py $(basename "$f") @ $b (raw lui+offset register accesses; MIPS32 encoding only) ---"
        { echo "# $(basename "$f") at $b"; python3 "$HERE/mmio_scan.py" "$f" --endian "$ENDIAN" --base "$b"; } \
            | tee -a "$OUT/mmio-scan.txt"
        echo "--- flashmap_scan.py $(basename "$f") (partition-table candidates + flash references) ---"
        { echo "# $(basename "$f") at $b"; python3 "$HERE/flashmap_scan.py" "$f" --endian "$ENDIAN" --base "$b"; } \
            | tee -a "$OUT/flashmap.txt" || true
    done
fi

# --- draft report ---------------------------------------------------------------
{
  echo "# Recon fact sheet: $BLOBNAME (DRAFT - confirm and annotate; see recon-report-schema.md)"
  echo
  echo "- blob: \`$ORIG\` ($(stat -c%s "$ORIG") bytes, sha256 $(sha256sum "$ORIG" | cut -c1-64))"
  [[ "$BLOB" != "$ORIG" ]] && echo "- converted for ember and Ghidra: \`$BLOB\` ($FORMAT)"
  echo "- arch / endian: **$ARCH / $ENDIAN**; Ghidra language id \`$PROC\`"
  echo "- ember support: _TODO (gate in SKILL.md: ISA descriptor, CPU model, gaps, verdict)_"
  echo "- base address: **$BASE** [$METHOD]"
  echo "- entry point: _TODO (FirmwareRecon output + disassembly at the base / reset vector)_"
  echo "- RTOS / kernel: _TODO (banner strings below)_"
  echo
  echo "## Container and architecture (fwinfo.py)"
  echo '```'; cat "$OUT/fwinfo.txt"; echo '```'
  echo
  echo "## Container scan"
  echo '```'; cat "$OUT/binwalk.txt"; echo '```'
  if [[ -f "$OUT/basefind.txt" ]]; then
      echo; echo "## Base evidence (basefind.py)"; echo '```'; cat "$OUT/basefind.txt"; echo '```'
  fi
  if [[ -s "$OUT/mmio-scan.txt" ]]; then
      echo; echo "## Raw MMIO scan (mmio_scan.py)"; echo '```'; cat "$OUT/mmio-scan.txt"; echo '```'
  fi
  if [[ -s "$OUT/flashmap.txt" ]]; then
      echo; echo "## Flash map evidence (flashmap_scan.py)"; echo '```'; cat "$OUT/flashmap.txt"; echo '```'
  fi
} > "$REPORT"

if [[ "$USE_GHIDRA" -eq 0 ]]; then
    echo "== --no-ghidra: draft report at $REPORT =="; exit 0
fi

# --- 4. Ghidra headless pass ---------------------------------------------------------
HEADLESS="$(find_headless)"
[[ -n "$HEADLESS" ]] || die "Ghidra analyzeHeadless not found (set GHIDRA_HOME) - or use --no-ghidra"
mkdir -p "$GHIDRA_PROJECTS"
POST=(-scriptPath "$HERE/ghidra"
      -postScript FirmwareRecon.java "$OUT/firmware-recon.txt"
      -postScript FindMmio.java "$OUT/find-mmio.txt")
[[ "$ARCH" == x86_* ]] && POST+=(-postScript FindX86Io.java "$OUT/find-x86io.txt")
if [[ "$REUSE" -eq 1 && -d "$GHIDRA_PROJECTS/$NAME.rep" ]]; then
    echo "--- reusing Ghidra project $NAME (no re-import) ---"
    "$HEADLESS" "$GHIDRA_PROJECTS" "$NAME" -process "$BLOBNAME" -noanalysis "${POST[@]}" \
        2>&1 | tee "$OUT/headless.log"
elif [[ "$IS_ELF" == 1 ]]; then
    echo "--- Ghidra import of the ELF ($PROC) + analysis + FirmwareRecon + FindMmio ---"
    "$HEADLESS" "$GHIDRA_PROJECTS" "$NAME" -import "$BLOB" -overwrite \
        -processor "$PROC" "${POST[@]}" 2>&1 | tee "$OUT/headless.log"
else
    # languages that name no entry point: start the analysis at the reset (and 8051
    # interrupt) vectors
    PRE=()
    case "$ARCH" in
        avr) PRE=(-preScript SeedEntries.java "$BASE");;
        mcs51) PRE=(-preScript SeedEntries.java $(python3 -c '
import sys
d, base = open(sys.argv[1], "rb").read(), int(sys.argv[2], 0)
seeds = [0] + [o for o in range(3, 0x83, 8)
               if o < len(d) and (d[o] in (0x02, 0x32) or d[o] & 0x1F == 0x01)]
print(" ".join(hex(base + o) for o in seeds))' "$BLOB" "$BASE"));;
    esac
    echo "--- Ghidra import at $BASE ($PROC) + analysis + FirmwareRecon + FindMmio ---"
    "$HEADLESS" "$GHIDRA_PROJECTS" "$NAME" -import "$BLOB" -overwrite \
        -loader BinaryLoader -loader-baseAddr "$BASE" -processor "$PROC" "${POST[@]}" "${PRE[@]}" \
        2>&1 | tee "$OUT/headless.log"
fi

for f in firmware-recon find-mmio find-x86io; do
    [[ -f "$OUT/$f.txt" ]] || continue
    { echo; echo "## $f (Ghidra)"; echo '```'; cat "$OUT/$f.txt"; echo '```'; } >> "$REPORT"
done
echo "== done. Draft: $REPORT  (Ghidra project: $GHIDRA_PROJECTS/$NAME) =="
echo "== Next: confirm base/entry/RTOS, label MMIO pages by reading the listed functions, =="
echo "== then rewrite RECON.md into the schema in references/recon-report-schema.md.      =="
echo "== Repeat for every other code stage (incl. an XIP bootstrap at its reset address).  =="
