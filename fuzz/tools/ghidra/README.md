# Ghidra Jython ports of ember's site-list and boundary-file generators

These are **Ghidra Jython 2.7** ports of the rizin-based generators one
directory up. They write the same files from the program open in Ghidra,
with **no rizin / rzpipe dependency**:

| Ghidra script (here) | rizin original (`fuzz/tools/`) | Writes |
|----------------------|--------------------------------|--------|
| `gen_sites.py`       | `gen_sites.py`                 | site list (`UAMS`), for `uam-run --cov-sites` |
| `gen_boundaries.py`  | `gen_boundaries.py`            | boundary file (`UAMB`), for `uam-run --cov-boundary` |
| `image_resolve.py`   | `image_resolve.py`             | shared name/address resolver and ELF symbol reader (imported, not run) |

The formats are specified in `docs/fuzz-formats.md` (and `fuzz/formats.h`).
For the same image and options a script writes **the same file, byte for
byte**, as its rizin original: the site selection (entry / terminator, the
MIPS delay-slot and ARM IT-block skips -- ember runs those instructions
as part of the one before them, so a site there stops the run -- the first
mid-block call), the function filters, the
`--root`/`--cutoff`/`--max-depth` slice, the overlap check and the packing
are the same code. Two rules make the result independent of the analyser:

* **Which functions get sites** is read from the image, not from the
  analysis: the ELF's `STT_FUNC` symbols and entry point (from the original
  file bytes Ghidra keeps with the program) and the `--symbols` addresses.
  Functions an analyser finds on its own (rizin `fcn.*`, Ghidra `FUN_*` or
  code labels) need `--all-functions`.
* **Blocks end after every jump or return** (on MIPS after its delay slot),
  never at a call. Ghidra's `BasicBlockModel` already does this; rizin does
  not always (PowerPC conditional returns), so the rizin tool cuts its
  blocks the same way.

Records are written in offset order, so a site's id does not depend on the
order an analyser lists functions or blocks in.

The rizin versions are the reference implementation.

## Installing Jython (Ghidra 11.4+ / 12.x)

These are **Jython 2.7** scripts. Ghidra **11.4+ / 12.x does not bundle
Jython**; add it as an extension:

- In the GUI: *File → Install Extensions → +*, pick the bundled
  `Extensions/Ghidra/ghidra_<ver>_PUBLIC_<date>_Jython.zip`, and restart.
- Or headless: unzip that file into `<ghidra>/Ghidra/Extensions/` so a
  `Jython/` module (with `lib/jython-standalone-2.7.x.jar`) appears there.

## How the rizin queries map to Ghidra

| rizin                              | Ghidra Program API |
|------------------------------------|--------------------|
| `rzpipe.open` + `aaa`              | `currentProgram` (import + auto-analysis) |
| `af @ ADDR`                        | `DisassembleCommand` + `CreateFunctionCmd` |
| `ij` bin info                      | `Language`: processor, size, endianness; `getImageBase()` |
| `aflj` (functions)                 | `FunctionManager.getFunctions(True)` |
| function `bits` 16 (Thumb)         | the `TMode` (ARM) / `ISA_MODE` (MIPS) context register at the entry |
| `afbj @ fn` (basic blocks)         | `BasicBlockModel.getCodeBlocksContaining(body, monitor)` |
| `pdj N @ addr` (instructions)      | `Listing.getInstructions(block, True)` |
| op type call/jmp/ret/indirect      | `Instruction.getFlowType()` (`isCall`/`isJump`/`isTerminal`/`isComputed`) |
| `afij @@F` callrefs (CALL only)    | `Function.getCalledFunctions(monitor)` |
| `isj`                              | `SymbolTable` |

## Running them (headless)

Analyse the firmware in a Ghidra project, then run the scripts as
`-postScript`s so they operate on the analysed program. `-scriptPath` puts
this directory on Ghidra's script path, so the scripts find
`image_resolve.py` and each other:

```bash
G=<ghidra>/support/analyzeHeadless

# site list for the whole image, then a begin marker checked against it
$G /tmp/proj fw -import build/firmware-mips/fuzztarget-be.elf \
    -scriptPath fuzz/tools/ghidra \
    -postScript gen_sites.py /tmp/fw.sites --strategy entry \
    -postScript gen_boundaries.py /tmp/fw.bounds fuzz_iteration:begin \
        --strategy entry --sites /tmp/fw.sites

# re-run on the already-imported program, only the slice below main
$G /tmp/proj fw -process fuzztarget-be.elf -noanalysis \
    -scriptPath fuzz/tools/ghidra \
    -postScript gen_sites.py /tmp/fw.sites --root main
```

then

```bash
uam-run -b BOARD --cov-sites /tmp/fw.sites --cov-boundary /tmp/fw.bounds \
    --cov-strict ... build/firmware-mips/fuzztarget-be.elf
```

From the GUI, run them from the Script Manager on the open program, with
the arguments set as the script's arguments (the scripts take them only
from Ghidra's script-arguments mechanism, never from prompts).

## Interface differences from the rizin tools

* **No positional `<binary>`.** Module 0 is the program open in Ghidra, so
  the first positional is the **output path**: `gen_sites.py OUT [options]`,
  `gen_boundaries.py OUT SPEC... [options]`.
* **No `--raw/--base/--arch/--bits/--endian/--cpu`.** A raw image's
  processor, base and byte order are chosen when it is imported into Ghidra
  (e.g. `-loader BinaryLoader -loader-baseAddr 0x8000 -processor 6502:LE:16:default`).
  `--symbols FILE` is kept (same `nm` / `ADDR NAME` / Ghidra-CSV formats);
  its names are applied undecorated.
* **`--module NAME:PROJECTPATH`** opens another module from the **same
  Ghidra project** (it must already be imported and analysed there);
  `PROJECTPATH` is the program's path inside the project, e.g. `/app.elf`.
  The rizin tool takes a filesystem path instead.
* **Name prefixes.** rizin decorates names (`sym.`/`dbg.`/`fcn.`/`sym.imp.`);
  Ghidra names are bare, so `--include-pat` / `--exclude-pat` regexes see
  different spellings. The default exclusion of import stubs skips **thunk**
  functions (`Function.isThunk()`), and the CRT/init exclusion is the rizin
  tool's name prefixes, undecorated.

Everything else -- `--include-pat`, `--exclude-pat`, `--no-default-exclude`,
`--all-functions`, `--strategy`, `--root`/`--cutoff`/`--max-depth`/
`--print-reachable`, `--fail-on-overlap`, `--allow-empty`, the boundary
specs `SYM[:begin|end[:any|zero]]`, `--sites` and `--force` -- behaves as
documented in the rizin tools' `--help` and docstrings.

## Where Ghidra goes further than rizin

The rizin tools refuse an ARM BE8 ELF, an ELF of a machine rizin has no
disassembler for (AVR32, 8-bit PIC), and give MIPS16e/microMIPS functions no
sites. Ghidra decodes those; MIPS16e/microMIPS and Thumb sites and markers
get the alternate-state flag. On such images there is no rizin file to
compare with.

## Notes

* The program must be **auto-analysed** first (headless `-import` does
  this; `-process` assumes it was analysed when imported).
* The scripts add functions at function-symbol addresses Ghidra did not
  make functions (as the rizin tool's `af @ ADDR`), so the program is
  modified and, headless, saved.

## Verified against the rizin tools

Generated both ways and compared with `cmp` (Ghidra 12.2, rizin 0.9.0), on
the ember test firmware in `build/`: site lists and boundary files are
byte-identical for MIPS32 BE and LE, ARM (ARM + Thumb), RV32 and PowerPC
with `--strategy entry`, and for ARM, RV32 and PowerPC with the default
terminator strategy. On AVR the two analysers split blocks differently
(conditional skips), so the lists differ there.
