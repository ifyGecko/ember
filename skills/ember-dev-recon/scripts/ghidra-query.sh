#!/usr/bin/env bash
# ghidra-query.sh - decompile, disassemble and cross-reference firmware in an existing Ghidra
# project with headless Ghidra: the fallback when the Ghidra MCP server is down, unreachable
# or holds another program. Read-only (nothing is saved to the project) and no re-analysis.
#
# Usage:
#   ghidra-query.sh [PROJECT] [--program NAME] MODE [TARGET...]
#     MODE        info | decompile T... | disasm T... | xrefs T... | functions [SUBSTR] |
#                 strings [SUBSTR]          (scripts/ghidra/Decompile.java has the details)
#     TARGET      an address (0x80012340, segment:offset in real mode, or SPACE:ADDR such as
#                 code:0x34 on a Harvard processor) or a function name;
#                 give several per call: each call loads the project once (seconds to minutes)
#   PROJECT, one of:
#     --project NAME|PATH.gpr   a project in $GHIDRA_PROJECTS, or a .gpr file anywhere
#     --blob FILE               the project recon.sh made for FILE (<file name>_recon)
#     (none)                    the only project in $GHIDRA_PROJECTS
#     --program NAME            one program of the project (default: all of them; in a UEFI
#                               project an address is reported by the module that holds it)
#     --out FILE                also keep the output in FILE
#
# Examples:
#   ghidra-query.sh --blob fw.bin info
#   ghidra-query.sh --blob fw.bin decompile 0x80012340 sysUartInit
#   ghidra-query.sh --project bios.bin_recon --program 038_PlatformPei_222C386D.efi xrefs 0xfffd4394
#
# The project must exist and be analysed: recon.sh creates it (a stage at its base, or a
# UEFI image's modules). A project the Ghidra GUI holds open is locked: the query then runs
# on a temporary copy.
#
# Env (all optional):
#   GHIDRA_HOME      Ghidra install dir (auto-detected: $GHIDRA_HOME, analyzeHeadless on
#                    PATH, ~/ghidra*, /opt/ghidra*)
#   GHIDRA_PROJECTS  where headless projects live (default ~/GhidraProjects)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
die() { echo "ghidra-query: $*" >&2; exit 1; }

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

PROJ=""; BLOB=""; PROGRAM=""; KEEP=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --project) PROJ="$2"; shift 2;;
        --blob) BLOB="$2"; shift 2;;
        --program) PROGRAM="$2"; shift 2;;
        --out) KEEP="$2"; shift 2;;
        -h|--help) sed -n '2,33p' "$0"; exit 0;;
        -*) die "unknown option: $1";;
        *) break;;
    esac
done
[[ $# -ge 1 ]] || die "give a mode: info, decompile, disasm, xrefs, functions or strings (--help)"
MODE="$1"; shift
case "$MODE" in
    info|functions|strings) ;;
    decompile|disasm|xrefs) [[ $# -ge 1 ]] || die "$MODE needs at least one address or function name";;
    *) die "unknown mode: $MODE (info, decompile, disasm, xrefs, functions, strings)";;
esac
for t in "$@"; do [[ -n "$t" ]] || die "empty target"; done

HEADLESS="$(find_headless)"
[[ -n "$HEADLESS" ]] || die "Ghidra analyzeHeadless not found (set GHIDRA_HOME)"
GHIDRA_PROJECTS="${GHIDRA_PROJECTS:-$HOME/GhidraProjects}"

# --- the project: <dir>/<name>.gpr -------------------------------------------------
if [[ -n "$BLOB" ]]; then
    PROJ="$(basename "$BLOB")_recon"
fi
if [[ "$PROJ" == *.gpr ]]; then
    [[ -f "$PROJ" ]] || die "no such project file: $PROJ"
    PDIR="$(cd "$(dirname "$PROJ")" && pwd)"; PNAME="$(basename "$PROJ" .gpr)"
elif [[ -n "$PROJ" ]]; then
    PDIR="$GHIDRA_PROJECTS"; PNAME="$PROJ"
else
    shopt -s nullglob
    gprs=("$GHIDRA_PROJECTS"/*.gpr)
    shopt -u nullglob
    [[ ${#gprs[@]} -eq 1 ]] || {
        echo "ghidra-query: name the project (--project or --blob); $GHIDRA_PROJECTS holds:" >&2
        for g in "${gprs[@]}"; do echo "  $(basename "$g" .gpr)" >&2; done
        exit 1
    }
    PDIR="$GHIDRA_PROJECTS"; PNAME="$(basename "${gprs[0]}" .gpr)"
fi
PDIR="$(cd "$PDIR" 2>/dev/null && pwd)" || die "no project directory: $PDIR"
[[ -f "$PDIR/$PNAME.gpr" && -d "$PDIR/$PNAME.rep" ]] ||
    die "no Ghidra project $PDIR/$PNAME (run recon.sh on the image first, or pass --project)"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/ghidra-query.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
OUTF="$WORK/out.txt"; LOG="$WORK/headless.log"
: > "$OUTF"

run_headless() {  # $1 = project dir, then the targets
    local dir="$1" proc=(-process)
    shift
    [[ -n "$PROGRAM" ]] && proc+=("$PROGRAM")
    "$HEADLESS" "$dir" "$PNAME" "${proc[@]}" -recursive -readOnly -noanalysis \
        -scriptPath "$HERE/ghidra" -postScript Decompile.java "$OUTF" "$MODE" "$@" \
        > "$LOG" 2>&1
}

rc=0
run_headless "$PDIR" "$@" || rc=$?
if grep -qi "unable to lock project\|LockException\|project is locked" "$LOG"; then
    # Open in the Ghidra GUI (or a stale lock): query a copy without the lock files.
    echo "ghidra-query: $PNAME is locked (open in Ghidra?); querying a temporary copy" >&2
    mkdir -p "$WORK/copy"
    cp -r "$PDIR/$PNAME.gpr" "$PDIR/$PNAME.rep" "$WORK/copy/"
    : > "$OUTF"; rc=0
    run_headless "$WORK/copy" "$@" || rc=$?
fi

if [[ -s "$OUTF" ]]; then
    cat "$OUTF"
    [[ -n "$KEEP" ]] && cp "$OUTF" "$KEEP"
    exit 0
fi
# nothing written: say why
if [[ $rc -ne 0 ]] || grep -q "ERROR" "$LOG"; then
    echo "ghidra-query: headless Ghidra failed (exit $rc):" >&2
    if grep -q "^ERROR" "$LOG"; then
        grep -A3 "^ERROR" "$LOG" | grep -v "^\s*at " | head -20 >&2
    else
        grep -v "^\s*$" "$LOG" | tail -15 >&2
    fi
    exit 1
fi
if grep -q "Unable to find\|No program\|not found" "$LOG"; then
    echo "ghidra-query: no program ${PROGRAM:+'$PROGRAM' }in $PNAME:" >&2
    grep -i "Unable to find\|No program\|not found" "$LOG" | head -3 >&2
    exit 1
fi
case "$MODE" in
    decompile|disasm|xrefs)
        echo "ghidra-query: no program in $PNAME holds ${*} (an address outside every memory block," \
             "or no function by that name; check with: info / functions NAME)" >&2;;
    *) echo "ghidra-query: nothing matched in $PNAME" >&2;;
esac
exit 1
