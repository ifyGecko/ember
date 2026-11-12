# Ghidra Jython ports of ember's site-list and boundary-file generators

These are **Ghidra Jython 2.7** ports of the rizin-based generators one
directory up. They write the same kinds of file from the program open in
Ghidra, with **no rizin / rzpipe dependency**:

| Ghidra script (here) | rizin original (`fuzz/tools/`) | Writes |
|----------------------|--------------------------------|--------|
| `gen_sites.py`       | `gen_sites.py`                 | site list (`UAMS`), for `uam-run --cov-sites` |
| `gen_boundaries.py`  | `gen_boundaries.py`            | boundary file (`UAMB`), for `uam-run --cov-boundary` |
| `image_resolve.py`   | `image_resolve.py`             | shared name/address resolver and ELF symbol reader (imported, not run) |

The formats are specified in `docs/fuzz-formats.md` (and `fuzz/formats.h`).
A script follows the same rules as its rizin original: the site selection
(entry / terminator, the MIPS delay-slot and ARM IT-block skips -- ember
runs those instructions as part of the one before them, so a site there
stops the run -- the first mid-block call), the function filters, the
`--root`/`--cutoff`/`--max-depth` slice, the overlap check and the packing.
So for the same image and options the two routes write **the same file,
byte for byte, when both analysers recover the same functions and blocks**.
These rules take as much as they can from the image instead of the
analyser:

* **Which functions get sites** is read from the image: the ELF's
  `STT_FUNC` symbols and entry point (from the original file bytes Ghidra
  keeps with the program) and the `--symbols` addresses. Functions an
  analyser finds on its own (rizin `fcn.*`, Ghidra `FUN_*` or code labels)
  need `--all-functions`.
* **What is code** is read from the image: a site lies in an executable
  section of the ELF (an executable segment, if it has no section headers;
  for a raw image, an executable memory block). Ghidra also disassembles
  bytes elsewhere, such as a jump table in `.rodata` behind a call that
  never returns; those get no site.
* **A block** starts at the function's entry, at an instruction that one
  other than the instruction before it transfers to, and after every jump
  or return (on MIPS after its delay slot); never after a call. Neither
  analyser's own blocks are quite that. rizin runs a block on past a
  PowerPC return, so the rizin tool cuts its blocks there. Ghidra's
  `BasicBlockModel` starts a block at every labelled instruction (the
  computed jump of a switch, the instruction after PowerPC's get-pc
  `bcl 20,31,$+4`), so the script joins a block that is entered only from
  the instruction just before it to that instruction's block.
* **The default exclusion** is import stubs and an exact list of C-runtime
  start/stop stubs: `_init`, `_fini`, `frame_dummy`, `register_tm_clones`,
  `deregister_tm_clones`, `__do_global_dtors_aux`, `__libc_csu_init`,
  `__libc_csu_fini`. A firmware function named `register_handler` or
  `_init_board` keeps its sites.

Records are written in offset order, so a site's id does not depend on the
order an analyser lists functions or blocks in.

**Where the two files can differ.** Where one analyser resolves a jump
table and the other does not, the case blocks of the switch are blocks,
with sites, in one list only: Ghidra resolves ARM `add pc, pc, rN, lsl #2`,
MIPS `jr` and PowerPC `bctr` tables that rizin leaves alone. The same goes
for any other block or function only one of them finds
(`--all-functions`). And **module 0's name** is the image's file name in
the rizin tool and the program's name in the Ghidra project here: the files
are identical only if the image was imported under its file name, which is
what `-import` does.

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
| `ij` bin info                      | `Language`: processor, size, endianness; the link base from the ELF's program headers |
| `aflj` (functions)                 | `FunctionManager.getFunctions(True)` |
| function `bits` 16 (Thumb)         | the `TMode` (ARM) / `ISA_MODE` (MIPS) context register at the entry |
| `afbj @ fn` (basic blocks)         | `BasicBlockModel.getCodeBlocksContaining(body, monitor)`, joined as above |
| `pdj N @ addr` (instructions)      | `Listing.getInstructions(block, True)` |
| op type call/jmp/ret/indirect      | `Instruction.getFlowType()` (`isCall`/`isJump`/`isTerminal`/`isComputed`) |
| a branch 4 bytes before (MIPS delay slot) | `Instruction.isInDelaySlot()` |
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
* **Position-independent ELFs.** Ghidra loads a PIE or shared object at an
  image base of its own. The files are the rizin tool's all the
  same: offsets from the ELF's lowest `PT_LOAD` address, which is also the
  base in the list's base table (0 for a PIE). `--symbols` addresses are
  the image's link addresses, as `nm` prints them (a symbol-table CSV
  exported from the rebased program has Ghidra's); an address given to
  `--root`, `--cutoff` or as a marker is Ghidra's.
* **`--module NAME:PROJECTPATH`** opens another module from the **same
  Ghidra project** (it must already be imported and analysed there);
  `PROJECTPATH` is the program's path inside the project, e.g. `/app.elf`.
  The rizin tool takes a filesystem path instead.
* **Name prefixes.** rizin decorates names (`sym.`/`dbg.`/`fcn.`/`sym.imp.`);
  Ghidra names are bare, so `--include-pat` / `--exclude-pat` regexes see
  different spellings. The default exclusion of import stubs skips the
  **thunk** functions (`Function.isThunk()`) that start at no function
  symbol; a function that is only a tail call is a thunk to Ghidra too, but
  it has a symbol and keeps its sites. The C-runtime exclusion is the same
  exact names in both.
* **Errors.** Ghidra prints only `SystemExit` for a script that stops with
  an error, so the scripts print the `error: ...` line themselves first;
  look for it in the headless log (or the script console).

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

Generated both ways and compared with `cmp` (Ghidra 12.2, rizin 0.9.0):

* The ember test firmware in `build/` (`tests/fuzz/gen-check.sh`, which
  `make fuzz-lists-test` runs): site lists and boundary files are
  byte-identical for MIPS32 BE and LE, ARM, RV32 and PowerPC with
  `--strategy entry`, and for ARM, RV32 and PowerPC with the default
  terminator strategy.
* A small `-O2` program with a seven-case switch, with both strategies:
  identical for x86-64, an x86-64 shared object (PIC, which Ghidra
  rebases) and ARM Thumb; for ARM, MIPS32 and PowerPC the lists differ in
  exactly the seven case blocks, which only Ghidra finds.

On AVR the two analysers split blocks differently (conditional skips), so
the lists differ there.
