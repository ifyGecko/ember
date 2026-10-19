# UAM-IR: The Universal Abstract Machine Instruction Set

Version 0.1 (draft) — core integer profile

This document defines the instruction set of the Universal Abstract Machine (UAM): the
ISA-neutral intermediate representation that every guest instruction set is lifted into and
that every execution backend (interpreter, JIT, analysis pass) consumes. It is the single
contract between *frontends* (ISA modules) and *backends*. See `architecture.md` for how the
IR fits into the rest of the emulator.

---

## 1. Design goals

1. **Small but proficient.** 47 opcodes. (The native JIT was added without adding any: the
   optimizer and an inline memory fast path carried the speed, so the universal opcode set
   stayed exactly as below.)  Enough to express real ISAs (RISC-V, ARM, MIPS,
   PowerPC, x86) directly, without encoding tricks. Not so many that every backend and every
   analysis pass becomes a large switch of special cases.
2. **Total, defined semantics.** No undefined behaviour. Every operation produces a defined
   result for every input (division by zero, over-wide shifts, etc. are specified). Where guest
   ISAs disagree, the IR picks one rule and lifters adapt with explicit ops.
3. **Explicit state access.** Computation happens only on *temporaries*. Guest state is
   touched only by dedicated ops: `GET`/`PUT` (registers), `LD`/`ST`/`AMO`/`CAS` (memory),
   `SYSRD`/`SYSWR` (system registers), `HELPER` (declared native code). An analysis pass can find
   every side effect by opcode alone.
4. **Straight-line blocks.** A block has one entry and no internal control flow. Conditional
   behaviour is expressed with `SELECT` (values), *guards* (conditional side effects), and
   guarded `EXIT`s (side exits). This keeps blocks trivially analysable (taint, symbolic
   execution, dataflow) and trivially compilable.
5. **Precise by construction.** Instruction boundaries are explicit (`IMARK`), and a
   well-formedness rule (§8) ensures that a fault never leaves an instruction half-committed in
   the register file.
6. **Environment-neutral.** The IR knows nothing about MMIO, devices, interrupts, or time.
   Those live behind the core interfaces (bus, scheduler, signals) described in `architecture.md`.

### Non-goals (v0.1)

- Native floating point and vectors. Floating point is lifted through `HELPER` for now. Reserved
  opcode ranges exist for future `F` (float) and `V` (vector / 128-bit) profiles (§11).
- Cycle accuracy. The IR models architectural behaviour, not pipelines.

---

## 2. Execution model

A **hart** (hardware thread) executing UAM-IR has:

| State             | Description |
|-------------------|-------------|
| `pc`              | Guest program counter. Owned by the core, *not* part of the register file. Set by `EXIT`, `TRAP` handling, `WAIT`, and interrupt entry. |
| `regfile`         | A flat byte array whose layout the ISA module defines through a register descriptor table (§3.2). |
| temporaries       | Block-local SSA values `t0, t1, …`. They do not survive past the end of the block. |
| guest memory      | Reached only through the bus interface, with access attributes (§6). |
| system registers  | Reached only through the ISA module's `sysreg` handler (§7.3). |
| `icount`          | Instructions issued. Incremented by every executed `IMARK`. It drives virtual time. |
| reservation       | The exclusive-access monitor used by `LD.X`/`ST.X` (§6.3). |

Execution proceeds **one block at a time**:

```
loop:
    block = translate_or_lookup(pc)        # ISA module lifts guest code into IR
    result = execute(block)                # interpreter or JIT
    handle(result)                         # next pc, trap, wait, fault, exit request
    deliver pending interrupts (ISA rule)  # only at block boundaries
```

Ops inside a block execute in order. A block ends at its first taken exit: a guarded `EXIT`
whose guard is true, the final unguarded terminator, or an abort (fault or exit request, §9).

A backend may go straight from one block's `EXIT` into the next block, without the loop's
steps in between (the JIT's block chaining), only when those steps would do nothing: no exit
request pending and the deadline not reached, the same test an `IMARK` makes. The result must
be the same as running the loop.

---

## 3. Types, operands and registers

### 3.1 Widths

Every value is a fixed-width bit-vector. Legal widths are **1, 8, 16, 32 and 64 bits**. Width 128
is reserved for the `V` profile.

- Width **1** is the boolean type. Comparisons and flag ops produce it, and `SELECT`, guards and
  `EXIT` consume it.
- Arithmetic wraps modulo 2^W. Values carry no signedness. Opcodes that care about sign come in
  signed and unsigned variants.

### 3.2 Register descriptor table

Each ISA module publishes a table of the registers it keeps in the register file:

```
{ name, offset, width, class }    class ∈ { GPR, FLAG, SYS_SHADOW, OTHER }
```

`GET`/`PUT` address the register file by `(offset, width)`. The table exists for debuggers
(GDB stub), snapshots, trace printing and verification. The IR itself only sees offsets.
Hardwired registers such as RISC-V `x0` are the lifter's job: `GET x0` is replaced by the
immediate 0, and writes to `x0` are dropped.

### 3.3 Operands

An operand (a `ref`) is one of the following:

- a **temporary** `tN`, which has a declared width;
- an **immediate** constant, which takes the width the op requires (and must fit in it);
- **none**, for unused slots.

Every op has an operating width `W` (the `.W` suffix in text form). Unless a rule below says
otherwise, all value operands and the result have width `W`.

### 3.4 Temporaries

Temporaries are in SSA form. Each is defined exactly once, before any use, with a fixed width.
The block header declares how many there are and the width of each.

---

## 4. Instruction format

Every instruction has the same shape:

```
[guard] dst = OP.W  a, b, c   {aux, flags}
```

| Field   | Meaning |
|---------|---------|
| `op`    | Opcode (§5). |
| `W`     | Operating width. |
| `guard` | Optional 1-bit temporary. Only effectful ops accept a guard (§4.1). |
| `dst`   | Result temporary, or none. |
| `a,b,c` | Source operands. |
| `aux`   | Opcode-specific 32-bit field: register offset, system register id, helper id, AMO sub-op, trap class, exit kind or instruction length. |
| `flags` | Memory access attributes (§6.1) or opcode modifiers. |

### 4.1 Guards

A guard makes a **side effect** conditional. If the guard is false:

- the op has no effect: no store, no register write, no fault, no trap, no exit;
- if the op has a `dst`, `dst` is defined to be **0**.

Pure ops (arithmetic, logic, compares, conversions) **do not accept guards**. Use `SELECT`
instead. This keeps pure dataflow free of control dependence.

Ops that accept a guard: `PUT, LD, ST, AMO, CAS, SYSRD, SYSWR, HELPER (effectful), TRAP, EXIT`.

---

## 5. Opcode summary

| Group | Opcodes | Count |
|---|---|---|
| Instruction marker | `IMARK` | 1 |
| Register file | `GET PUT` | 2 |
| Memory | `LD ST AMO CAS FENCE` | 5 |
| Arithmetic | `ADD SUB MUL MULHU MULHS UDIV SDIV UREM SREM` | 9 |
| Logic | `AND OR XOR` | 3 |
| Shift / rotate | `SHL LSHR ASHR ROTR` | 4 |
| Compare | `EQ NE ULT ULE SLT SLE` | 6 |
| Carry / overflow | `CARRY SCARRY SBORROW` | 3 |
| Bit manipulation | `CLZ CTZ POPCNT BSWAP` | 4 |
| Conversion | `ZEXT SEXT TRUNC` | 3 |
| Select | `SELECT` | 1 |
| Control | `EXIT` | 1 |
| System / escape | `SYSRD SYSWR TRAP WAIT HELPER` | 5 |
| Instrumentation | `COV` (inserted by passes only, §7.5) | 1 |
| **Total** | | **48** |

Some opcodes are left out on purpose. `NOT`, `NEG`, `MOV`, `SGT`/`UGT`, `ROTL`, `MULHSU`, and
partial-register insert/extract are all short compositions of the ops above (§10). Leaving them
out keeps every backend smaller. A peephole pass can always fuse common patterns later.

---

## 6. Memory

### 6.1 Access attributes (`flags` on memory ops)

| Flag              | Meaning |
|-------------------|---------|
| `LE` / `BE`       | Byte order of the access. Required, so the IR never assumes host endianness. |
| `ALIGNED`         | A misaligned address raises an alignment fault instead of being split by the bus. |
| `X`               | Exclusive: load-reserved or store-conditional (§6.3). |
| `ACQ` / `REL`     | Acquire and/or release ordering. Both together mean sequentially consistent. |
| `UNPRIV`          | Perform the access at the ISA's *user* privilege level (ARM `LDRT`, RISC-V `MPRV`-style). |
| `PHYS`            | Bypass address translation. Used by page-table-walk helpers. |

The bus combines these attributes with the hart's current privilege (maintained by the ISA
module) when it decides whether to translate, permit, fault, or dispatch to a device.

Address operands have the hart's **address width** (AW = 32 or 64), which the ISA module declares.

### 6.2 Memory opcodes

| Op | Form | Semantics |
|---|---|---|
| `LD` | `dst:W = LD.W addr {flags}` | Read W bits (8–64) from `addr`. May fault. |
| `ST` | `ST.W addr, val {flags}` | Write W bits. May fault. With `X`: `dst:1 = ST.W.X addr, val` (§6.3). |
| `AMO` | `dst:W = AMO.W addr, val {aux=subop, flags}` | Atomically: `old = mem[addr]; mem[addr] = subop(old, val); dst = old`. Sub-ops: `XCHG ADD AND OR XOR SMIN SMAX UMIN UMAX`. |
| `CAS` | `dst:W = CAS.W addr, expected, new {flags}` | Atomically: `old = mem[addr]; if old == expected: mem[addr] = new; dst = old`. |
| `FENCE` | `FENCE {aux=kind}` | Ordering barrier. Kinds: `ACQ`, `REL`, `FULL`, `INSN`. `INSN` synchronises the instruction stream: the translation cache revalidates the hart's code pages, and the op must be followed by the block's `EXIT`. |

### 6.3 Exclusive access

- `LD.X` performs the load and sets the hart's reservation to `(addr, W)`.
- `ST.X` succeeds only if the hart holds a reservation covering `(addr, W)`. It returns
  `dst = 1` on success (the store is performed) and `0` on failure (no store). Either way the
  reservation is cleared.
- Any store by *another* agent (hart or DMA) that overlaps the reservation granule clears it.
  So do `TRAP` and interrupt entry.

Guest conventions differ (RISC-V `sc` writes 0 on success). Lifters invert as needed.

### 6.4 Faults

Any memory op may fault: unmapped address, permission, alignment, or a device error. A fault
aborts the block precisely (§9). The ISA module turns the fault into a guest exception through
`take_exception(cause, tval)`.

---

## 7. Operation semantics

Notation: `u(x)` and `s(x)` read the W-bit value `x` as unsigned or two's-complement signed.

### 7.1 Integer

| Op | Semantics |
|---|---|
| `ADD a,b` / `SUB a,b` / `MUL a,b` | `(a op b) mod 2^W` |
| `MULHU a,b` | High W bits of `u(a) × u(b)` (2W-bit product) |
| `MULHS a,b` | High W bits of `s(a) × s(b)` |
| `UDIV a,b` | `u(a) / u(b)`, truncated. **If b = 0: all ones.** |
| `UREM a,b` | `u(a) mod u(b)`. **If b = 0: a.** |
| `SDIV a,b` | `s(a) / s(b)`, truncated toward zero. **b = 0 → −1. MIN / −1 → MIN.** |
| `SREM a,b` | Remainder taking the sign of the dividend. **b = 0 → a. MIN rem −1 → 0.** |
| `AND OR XOR a,b` | Bitwise. Also legal at W = 1 as boolean logic. |
| `SHL a,n` | `a << u(n)`. **If u(n) ≥ W: 0.** |
| `LSHR a,n` | Logical right shift. **If u(n) ≥ W: 0.** |
| `ASHR a,n` | Arithmetic right shift. **If u(n) ≥ W: every bit equals the sign bit.** |
| `ROTR a,n` | Rotate right by `u(n) mod W`. |

The division edge cases match RISC-V. ARM (x/0 = 0) adds a `SELECT`, and x86 (#DE) adds a
guarded `TRAP`. The shift rule is "mathematical" (no implicit masking). ISAs that mask the
amount (RISC-V, x86) emit an explicit `AND n, W−1`, and ISAs that saturate (ARM register
shifts) use the rule as it is.

### 7.2 Compare, flags, bits, conversion, select

| Op | Result width | Semantics |
|---|---|---|
| `EQ NE a,b` | 1 | Equality. Also legal at W = 1. |
| `ULT ULE a,b` | 1 | Unsigned `<`, `≤` |
| `SLT SLE a,b` | 1 | Signed `<`, `≤` |
| `CARRY a,b` | 1 | Unsigned carry out of `a + b` |
| `SCARRY a,b` | 1 | Signed overflow of `a + b` |
| `SBORROW a,b` | 1 | Signed overflow of `a − b` (unsigned borrow is `ULT a,b`) |
| `CLZ a` / `CTZ a` | W | Count leading/trailing zeros. **a = 0 → W.** |
| `POPCNT a` | W | Number of set bits |
| `BSWAP a` | W | Reverse byte order (W ≥ 16) |
| `ZEXT.W a` | W | Zero-extend. Requires `width(a) < W`. A 1-bit source turns a boolean into an integer. |
| `SEXT.W a` | W | Sign-extend. Requires `width(a) < W`. |
| `TRUNC.W a` | W | Keep the low W bits. Requires `width(a) > W`. |
| `SELECT c,a,b` | W | `c ? a : b` (`c` has width 1) |

For conversions, `W` is the *destination* width and the source width comes from the operand.
Comparisons and flag ops take W-bit operands and produce a 1-bit result.

### 7.3 Register file, system and escape ops

| Op | Form | Semantics |
|---|---|---|
| `IMARK` | `IMARK pc {aux=len}` | Starts guest instruction `pc` of `len` bytes. Records the *current instruction pc* for faults, increments `icount`, and is the point where pending exit requests are honoured (§9). |
| `GET` | `dst:W = GET {aux=offset}` | Read the register file. Pure storage, no side effects. |
| `PUT` | `[g] PUT val {aux=offset}` | Write the register file. Pure storage, no side effects. |
| `SYSRD` | `[g] dst:W = SYSRD {aux=id}` | Read system register `id` through the ISA module's handler. May have side effects (counters, read-to-clear) and may fault (illegal or privileged). |
| `SYSWR` | `[g] SYSWR val {aux=id}` | Write system register `id` through the ISA handler. May fault. The handler may raise an *exit request* (for example, interrupts enabled or translation regime changed). |
| `HELPER` | `[g] dst = HELPER a,b,c {aux=id}` | Call a native function registered by an ISA module or analysis pass (§7.4). |
| `TRAP` | `[g] TRAP cause, tval {aux=class}` | Raise a synchronous event. Ends the block. Classes: `EXCEPTION` (guest exception, passed to `isa->take_exception(cause, tval)` with an ISA-defined `cause`), `DEBUG` (stop into the emulator's debugger, not the guest), `HALT` (stop emulation, e.g. semihosting exit). |
| `WAIT` | `WAIT next_pc` | The hart idles until the ISA's interrupt-accept rule is satisfied. The scheduler may fast-forward virtual time to the next event, and execution resumes at `next_pc`. Terminator. |
| `EXIT` | `[g] EXIT target {aux=kind}` | Leave the block and continue at `target`. An immediate target is a *direct* exit, which the cache may chain to the next block. A temporary target is an *indirect* exit. Kinds `JUMP`, `CALL`, `RET`, `EXC_RETURN` are hints for analysis (call graphs, shadow stacks) and do not change semantics. |

### 7.4 Helpers

Each helper is registered with a descriptor:

```
{ id, name, arg_widths[3], result_width, effect }
effect ∈ { PURE, READS_STATE, WRITES_STATE, MAY_FAULT }   (a bitmask)
```

- `PURE` helpers are deterministic functions of their arguments. They do not accept guards,
  and optimisers may move or remove them.
- Any helper with `WRITES_STATE` or `MAY_FAULT` is treated like a store for ordering and for the
  commit rule (§8).
- Instrumentation that needs host code (cmp-logging, taint) is inserted as helpers. Edge
  coverage is the exception: it has its own opcode (§7.5), because it runs on every
  instrumented instruction and must stay inline in native code.

Helpers are the pressure valve for complex semantics (floating point, crypto, `mret`-style mode
switches). The rule of thumb: **if an operation can be expressed in fewer than ~10 core ops, do
not make it a helper.**

### 7.5 Coverage

`COV.32 A, B` counts one edge in the hart's coverage state (`uam_cov_state`, owned by
whoever enabled coverage). The state holds the map, its `mask`, and a history of up to
`UAM_COV_HIST_MAX` (7) earlier `B` values `hist[]`, of which `depth` are folded (0 means 1),
each rotated left by `rot[j]` within the map's width `bits` first (0: no rotation):

```
idx = A
for j < depth: idx ^= rot[j] ? rotl(hist[j], rot[j]) : hist[j]
idx &= mask
if (map[idx] != 0xff) map[idx]++               // saturating 8-bit counter
hist[depth-1..1] = hist[depth-2..0]; hist[0] = B
rotl(v, r) = ((v << r) | (v >> (bits - r))) & mask      // 32-bit arithmetic
```

`A` and `B` are 32-bit immediates chosen when the block is translated. The history shape
(`depth`, `rot`, `bits`, set with `uam_cov_state_init` and checked by
`uam_exec_set_coverage`) and `mask` are fixed while coverage is on: the JIT compiles the
shape in (the default shape costs exactly what the one-predecessor op always did), and
changing it means calling `uam_exec_set_coverage` again, which flushes every translation.
With the default shape (depth 1, no rotation; `hist[0]` is also called `prev`) the rule is
`idx = (A ^ prev) & mask; prev = B`, which covers AFL-style edges (`A = cur`,
`B = cur >> 1`) and block counts (`B = 0`). Deeper histories give N-gram coverage that
folds the last N-1 sites, and rotation makes the fold order-sensitive (suzzer's schemes;
`uam_cov_count` in `cpu.h` is the reference implementation). `COV` reads and writes nothing else: no register, no
memory, no bus access, and it does not count as an instruction (`icount` is unchanged). With
no coverage state attached (`cov` is NULL) it does nothing. Lifters never emit it; a pass
inserts it directly after the `IMARK` of an instrumented instruction (§8 rule 8), so it runs
exactly once each time that instruction starts, including when a block is interrupted and
resumed.

---

## 8. Block well-formedness

The verifier (run on every lifted block in debug builds) enforces these rules:

1. **Shape.** The block starts with `IMARK` and ends with exactly one terminator: an unguarded
   `EXIT`, `TRAP`, or `WAIT`. Nothing follows the terminator. Guarded `EXIT`s (side exits) may
   appear anywhere.
2. **SSA.** Every temporary is defined exactly once, before any use, and its width matches every use.
3. **Widths.** Operand and result widths follow §7. Immediates fit their width.
4. **Register bounds.** `GET`/`PUT` stay inside the register file.
5. **Commit rule (precise exceptions).** Within one instruction (from one `IMARK` to the next),
   no `PUT` may come before an op that can fault: `LD`, `ST`, `AMO`, `CAS`, `SYSRD`, `SYSWR`,
   `TRAP`, or a `MAY_FAULT` helper. In other words, an instruction does all of its checks into
   temporaries first, then commits register writes. A fault therefore always leaves the register
   file exactly as it was before the faulting instruction, so rollback costs nothing. (Partially
   completed multi-store instructions are allowed, as real hardware does the same.)
6. **Coverage.** A block covers guest code from at most two pages, and it records which pages
   those are so that self-modifying code can invalidate it.
7. **Length.** At most `MAX_BLOCK_INSNS` guest instructions per block (a tunable that bounds
   interrupt latency).
8. **Coverage op.** `COV` has width 32, two immediate operands and no guard, and comes
   directly after an `IMARK` that is not a continuation (`UAM_IMARK_CONT`).

---

## 9. Block results and asynchronous events

Executing a block returns exactly one of these results:

| Result | Cause | Next `pc` |
|---|---|---|
| `NEXT(target)` | An `EXIT` was taken | `target` |
| `TRAPPED(class, cause, tval)` | `TRAP` | Decided by `isa->take_exception` or the debugger |
| `WAITING(next_pc)` | `WAIT` | `next_pc` once woken |
| `FAULT(kind, addr)` | A memory, sysreg or helper fault | Decided by `isa->take_exception`. The faulting instruction's pc is the last `IMARK`. |
| `INTERRUPTED(pc)` | An exit request, a deadline or a `BREAK` mark seen at an `IMARK` | `pc` of that `IMARK` (instruction not yet started) |

Every exception report (`uam_exception`, `isa.h`) carries the faulting
instruction's `pc` and, when that instruction is a continuation
(`UAM_IMARK_CONT`), the flag `UAM_EXC_F_CONT` and the `parent_pc` of the
instruction it continues. Memory faults distinguish **translation** faults
(`*_XLATE`: `isa->translate` refused the virtual address; `cause` carries the
ISA's sub-cause) from **bus** faults (`*_FAULT`: nothing answered at the
physical address). Alignment is checked before translation.

**Exit requests** are how the outside world interrupts a running block without polling on
every op. Any interrupt line change, `SYSWR` handlers that change interrupt enables, software
interrupt bits or the privilege level, translation changes, writes to translated code, stop
and reset requests, and a timer armed earlier than the current deadline all set
`cpu->exit_request`. The executor checks it at each `IMARK`, and before chaining into another
block. Because of the commit rule, stopping at an `IMARK` is always a clean architectural
boundary. Interrupt acceptance (`isa->irq_take`) runs between blocks, and is only re-evaluated
after an exit request, so an ISA must raise one on every change that can make an interrupt
deliverable.

---

## 10. Common idioms

These are canonical compositions that lifters should emit and optimisers should recognise:

| Need | Expression |
|---|---|
| `NOT a` | `XOR a, -1` |
| `NEG a` | `SUB 0, a` |
| move / copy | Use the source operand directly (SSA) |
| `a > b` (unsigned) | `ULT b, a` |
| `ROTL a, n` | `ROTR a, (SUB W, n)` |
| `MULHSU a, b` (signed × unsigned) | `SUB (MULHU a,b), (SELECT (SLT a,0), b, 0)` |
| Insert byte into 32-bit reg | `OR (AND r, ~0xFF00), (SHL (ZEXT.32 v), 8)` |
| Extract bits [hi:lo] | `TRUNC (LSHR a, lo)` or `AND (LSHR a, lo), mask` |
| Conditional store (ARM `STRxx`) | `[c] ST addr, v` |
| Instruction loops (x86 `REP`) | Execute one iteration, then `[more] EXIT <same pc>` |

---

## 10a. Delay slots and address translation (added for MIPS32)

These capabilities were added to the core when MIPS32 became the second ISA;
all are ISA-neutral and unused by ISAs that do not need them.

- **Continuation instructions (`UAM_IMARK_CONT`).** A frontend may mark an
  `IMARK` as a continuation of the previous instruction. The executor then does
  not take an asynchronous exit (interrupt / scheduler deadline) at that
  boundary, so an instruction group retires atomically with respect to
  interrupts. MIPS uses this to keep a branch and the instruction in its delay
  slot together: the branch computes its condition and target into temporaries,
  the delay slot is lifted under a continuation `IMARK`, and the block then
  exits - so a pending control transfer can never be lost to an interrupt
  landing between the two. A *synchronous* exception inside a continuation is
  reported with `UAM_EXC_F_CONT` and the branch's `parent_pc`, so the ISA can
  restart the whole group (MIPS: `EPC` = branch, `Cause.BD` = 1).

- **Optional address translation (`isa->translate`).** An ISA may supply a
  virtual-to-physical translation that the executor calls for every fetch, load
  and store except `PHYS` accesses (and that loaders and debuggers use with the
  side-effect-free `UAM_XLATE_DEBUG` kind). ISAs that address physical memory
  directly (RISC-V M-mode) leave it NULL. A failure returns `UAM_ERR_XLATE`
  plus an ISA-defined sub-cause, which reaches `take_exception` unchanged (MIPS:
  address error, TLB refill, TLB invalid, TLB modified). Translated code stays
  valid while the hart runs at the same `cpu->priv` and, for code fetched
  through a mapping the ISA did not mark `fixed`, while `cpu->xlate_gen` is
  unchanged; the ISA bumps it with `uam_cpu_xlate_changed()` on TLB writes and
  address-space switches.

- **Executor stop marks (`UAM_IMARK_BREAK`).** The executor (never a lifter)
  may set this flag on an `IMARK` of a translated block so the block ends
  before that instruction; PC hooks and debugger breakpoints use it to run at a
  precise instruction boundary without per-instruction checks.

The ISA also declares `default_endian`, the byte order of instruction fetch,
so the executor reads instructions in the guest's order (MIPS is bi-endian).

## 11. Opcode numbering and extension profiles

| Range | Profile |
|---|---|
| `0x00–0x3F` | **Core** (this document) |
| `0x40–0x7F` | **F**: native floating point (reserved; until defined, floating point uses `HELPER`) |
| `0x80–0xBF` | **V**: 128-bit and vector lanes (reserved) |
| `0xC0–0xFF` | Implementation-private (debug and experimental only; never emitted by shipped lifters) |

A backend states which profiles it implements. A block that uses an unsupported profile is
rejected at translation time. It never fails silently at run time.

---

## 12. Text form

The canonical printed form is used for traces, tests and documentation. One op per line, with
attributes after the mnemonic and auxiliary fields in braces:

```
imark   0x80000010 {len=4}
t0:32 = get     {x10}
t1:32 = add     t0, 0x10
t2:32 = ld.le   t1
        put     t2 {x11}
t3:1  = eq      t2, 0
[t3]    exit    0x80000100 {jump}
        exit    0x80000014 {jump}
```

Register offsets print by name, using the ISA's register descriptor table, when one is available.

---

## 13. Worked examples: RISC-V (RV32IMAC)

`pc` is the instruction's address. `#imm` is the decoded immediate.

**`add rd, rs1, rs2`**
```
t0:32 = get {rs1};  t1:32 = get {rs2}
t2:32 = add t0, t1
        put t2 {rd}
```

**`lb rd, #imm(rs1)`** — the load comes before the `PUT` (commit rule).
```
t0:32 = get {rs1}
t1:32 = add t0, #imm
t2:8  = ld.le t1
t3:32 = sext.32 t2
        put t3 {rd}
```

**`sll rd, rs1, rs2`** — RISC-V masks the shift amount explicitly.
```
t2:32 = and t1, 31
t3:32 = shl t0, t2
```

**`div rd, rs1, rs2`** — a single `sdiv`, since the IR's edge cases match RISC-V.

**`beq rs1, rs2, #off`** — ends the block.
```
t2:1 = eq t0, t1
[t2]   exit pc+#off {jump}
       exit pc+4    {jump}
```

**`jalr rd, #imm(rs1)`** — the target is computed before `rd` is written, so `rd == rs1` works.
```
t0:32 = get {rs1}
t1:32 = add t0, #imm
t2:32 = and t1, 0xFFFFFFFE
        put pc+4 {rd}
        exit t2 {ret if rd==x0 && rs1==ra, call if rd==ra, else jump}
```

**`lr.w.aq rd, (rs1)` / `sc.w rd, rs2, (rs1)`**
```
t1:32 = ld.le.x.acq t0          ;  put t1 {rd}
t2:1  = st.le.x     t0, t1      ;  t3:32 = zext.32 t2
t4:32 = xor t3, 1               ;  put t4 {rd}      # RISC-V: 0 = success
```

**`amoadd.w rd, rs2, (rs1)`** — `t2:32 = amo.le t0, t1 {add}` then `put t2 {rd}`.

**`csrrw rd, csr, rs1`** — ends the block, because a CSR write can change interrupt enables or privilege.
```
t0:32 = get {rs1}
t1:32 = sysrd {csr}            # omitted when rd == x0 (no read side effects)
        syswr t0 {csr}
        put t1 {rd}
        exit pc+4 {jump}
```

**`ecall`** — `trap RV_CAUSE_ECALL, 0 {exception}`. The ISA module maps this to cause 8, 9 or 11
depending on the current privilege level.

**`wfi`** — `wait pc+4`.

**`fence.i`** — `fence {insn}` then `exit pc+4`.

**`mret`** — a mode switch with many coupled CSR effects, so it is a helper:
```
t0:32 = helper {rv_mret}       # effect: READS_STATE|WRITES_STATE; restores MIE/MPP, returns mepc
        exit t0 {exc_return}
```

**Compressed instructions** lift to exactly the same IR as their 32-bit expansions, with `{len=2}`.

---

## Appendix A. Reference C encoding (informative)

The in-memory representation is a fixed-size record. This encoding is not normative: backends
see only the semantics above.

```c
typedef uint32_t uam_ref;              /* UAM_REF_NONE, temp index, or UAM_IMM_BIT|pool index */
#define UAM_REF_NONE  0xFFFFFFFFu
#define UAM_IMM_BIT   0x80000000u

typedef struct {
    uint8_t  op;                       /* uam_opcode */
    uint8_t  width;                    /* 1, 8, 16, 32, 64 */
    uint16_t flags;                    /* memory attributes / modifiers */
    uint32_t aux;                      /* reg offset, sysreg id, helper id, subop, kind, len */
    uam_ref  guard, dst, a, b, c;
} uam_insn;                            /* 28 bytes */

typedef struct {
    uint64_t  guest_pc;
    uint32_t  n_insn, n_temp, n_imm;
    uam_insn *insn;
    uint8_t  *temp_width;              /* per-temporary width */
    uint64_t *imm;                     /* immediate pool */
    uint64_t  page[2], page_gen[2];    /* code pages covered, for invalidation */
} uam_block;
```
