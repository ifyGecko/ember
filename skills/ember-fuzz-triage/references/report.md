# The triage report

One report per distinct bug, saved next to the input (`triage-<name>/report.md`;
`scripts/triage.py` starts it for you with the facts it can collect). Fill in every
section. Where you do not know, write "not established" and what would establish it: a
report with an honest gap is better than a guess stated as fact.

Paste real output. Do not retype addresses.

```markdown
# <one line: what goes wrong, in which function>   e.g. "Store through an input-controlled pointer in parse()"

## Summary
Two or three sentences a developer can act on: what input, what the firmware does with
it, what goes wrong, and the classification (firmware bug / emulation gap / harness
artefact / ember bug).

## Reproduce
- Firmware: <file, and its sha256sum>
- ember: <git commit of the ember checkout; the board or project and its commit>
- Input: <file name, length>, as hex:
      <output of: xxd INPUT>
- Command:
      <the exact FUZZ ... --fuzz-replay INPUT command>
- Result:
      <the replay: line>
- Repeatable: <yes: the same line twice, and with --jit / no: what differs>
- Minimal: <yes: removing any byte stops the crash / no: not minimised>

## What
<the verdict in words: "a 4-byte store to address 0x40000000 raised a bus fault">

## Where
- pc: <address>, in <function> (<file:line if known>)
- Instruction: <disassembly of the faulting instruction>
- Registers that matter at the stop: <the base register, the value, the length...>
- Instruction count: <insns= from the begin pc>

## How execution got there
- Call chain (innermost first):
      <the backtrace: lines>
- Path through the function: <the branches taken, from --history or stepping; which
  input bytes each branch tested>

## Why
The chain from the input to the bad value, one step per line, each with its evidence:
1. Input bytes <offset..offset> (<hex>) are read at pc <...> into <register/variable>.
2. <what the code computes from them>
3. <the check that is missing, or wrong, and where it should have been>
4. The value reaches <instruction> as <register>, which <faults / overruns / ...>.

Evidence: <the --watch / --on / gdb output that shows each step>

Confirmed by changing the input: <"with byte 3 changed from 0x21 to 0x00 the verdict is
ok"; or "not tested">

## Classification
<firmware bug | emulation gap | harness artefact | ember bug>, because <the evidence:
where the bad value came from>. If an emulation gap: which device and register, and what
hardware would return.

## Impact
- The input controls: <the address / the value / the length / the jump target / nothing>
- Consequence: <memory write, memory read, crash and reboot, permanent hang>
- Reachable by: <who can send this input on the real device>

## Open questions
<anything not established, and the experiment that would settle it>

## Commands used
<every command that produced evidence above, in order, so the work can be repeated>
```

## Before you hand it in

- [ ] Someone else can reproduce it by pasting the command.
- [ ] The crash site and the cause are both named, and they are not confused.
- [ ] Every claim in "Why" has output that shows it.
- [ ] The classification says where the bad value came from.
- [ ] Nothing is stated as fact that you only suspect.
