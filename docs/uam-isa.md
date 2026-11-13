# UAM-IR: The Universal Abstract Machine Instruction Set

Version 0.1 (draft) — core integer profile

This document defines the instruction set of the Universal Abstract Machine (UAM): the
ISA-neutral intermediate representation that every guest instruction set is lifted into and
that every execution backend (interpreter, JIT, analysis pass) consumes. It is the single
contract between *frontends* (ISA modules) and *backends*. See `architecture.md` for how the
IR fits into the rest of the emulator.

---

## 1. Design goals

1. **Small but proficient.** 47 opcodes, plus the instrumentation ops `COV` (§7.5) and `CMPLOG` (§7.6). (The native JIT was added without adding any: the
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

`GET`/`PUT` address the register file by `(offset, width)`. The table gives
names for trace printing (the gdb stub has its own, `uam_gdb_arch`). The IR itself only sees offsets.
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
| `guard` | Optional 1-bit operand: a temporary, or an immediate the optimizer folds. Only effectful ops accept a guard (§4.1). |
| `dst`   | Result temporary, or none. |
| `a,b,c` | Source operands. |
| `aux`   | Opcode-specific 32-bit field: register offset, system register id, helper id, AMO sub-op, trap class, exit kind, fence kind or instruction length. |
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
| Instrumentation | `COV CMPLOG` (inserted by passes only, §7.5, §7.6) | 2 |
| **Total** | | **49** |

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
| `UNPRIV`          | Reserved: the verifier accepts it and the JIT keeps such an access off its fast path, but the executor gives it no meaning and no lifter emits it. |
| `PHYS`            | Bypass address translation. Lifters use it for accesses to a space of its own: AVR `LPM`, PIC `TBLRD`, x86 `IN`/`OUT`. |

The executor translates an address through `isa->translate`, which sees the hart's privilege
(`cpu->priv`). The bus then decodes the physical address, and uses only the byte order,
`ALIGNED` and the core-only fetch and debug attributes.

Address operands have the hart's **address width** (AW = 32 or 64), which the ISA module declares.

### 6.2 Memory opcodes

| Op | Form | Semantics |
|---|---|---|
| `LD` | `dst:W = LD.W addr {flags}` | Read W bits (8–64) from `addr`. May fault. |
| `ST` | `ST.W addr, val {flags}` | Write W bits. May fault. With `X`: `dst:1 = ST.W.X addr, val` (§6.3). |
| `AMO` | `dst:W = AMO.W addr, val {aux=subop, flags}` | Atomically: `old = mem[addr]; mem[addr] = subop(old, val); dst = old`. Sub-ops: `XCHG ADD AND OR XOR SMIN SMAX UMIN UMAX`. |
| `CAS` | `dst:W = CAS.W addr, expected, new {flags}` | Atomically: `old = mem[addr]; if old == expected: mem[addr] = new; dst = old`. |
| `FENCE` | `FENCE {aux=kind}` | Ordering barrier. Kinds: `ACQ`, `REL`, `FULL`, `INSN`. `INSN` synchronises the instruction stream. The op does nothing itself, since a write to translated code already invalidates it; it must be directly followed by the block's terminator, so the next block is looked up afresh. |

### 6.3 Exclusive access

- `LD.X` performs the load and sets the hart's reservation to `(addr, W)`.
- `ST.X` succeeds only if the hart holds a reservation covering `(addr, W)`. It returns
  `dst = 1` on success (the store is performed) and `0` on failure (no store). Either way the
  reservation is cleared.
- A reservation is not snooped. With several processors it is dropped at the end of each
  scheduling slice, so a store-conditional may always fail; stores from devices do not clear
  it. The ISA's exception and interrupt entry and return clear it.

Guest conventions differ (RISC-V `sc` writes 0 on success). Lifters invert as needed.

### 6.4 Faults

Any memory op may fault: unmapped address, permission, alignment, or a device error. A fault
aborts the block precisely (§9). The ISA module turns the fault into a guest exception through
`isa->take_exception`, which receives a `uam_exception` (§9).

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
amount emit an explicit `AND` (RISC-V 31; x86 31, or 63 at 64 bits, whatever the operand width), and ISAs that saturate (ARM register
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
| `IMARK` | `IMARK pc {aux=len}` | Starts guest instruction `pc` of `len` bytes. Records the *current instruction pc* for faults, increments `icount`, and is the point where pending exit requests are honoured (§9), except at the block's first `IMARK`, so every block makes progress, and at continuations. |
| `GET` | `dst:W = GET {aux=offset}` | Read the register file. Pure storage, no side effects. |
| `PUT` | `[g] PUT val {aux=offset}` | Write the register file. Pure storage, no side effects. |
| `SYSRD` | `[g] dst:W = SYSRD {aux=id}` | Read system register `id` through the ISA module's handler. May have side effects (counters, read-to-clear) and may fault (illegal or privileged). |
| `SYSWR` | `[g] SYSWR val {aux=id}` | Write system register `id` through the ISA handler. May fault. The handler may raise an *exit request* (for example, interrupts enabled or translation regime changed). |
| `HELPER` | `[g] dst = HELPER a,b,c {aux=id}` | Call a native function registered by an ISA module or analysis pass (§7.4). |
| `TRAP` | `[g] TRAP cause, tval {aux=class}` | Raise a synchronous event. Ends the block. Classes: `EXCEPTION` (guest exception, passed to `isa->take_exception` in a `uam_exception` with an ISA-defined `cause`), `DEBUG` (stop into the emulator's debugger, not the guest), `HALT` (stop emulation, e.g. semihosting exit). |
| `WAIT` | `WAIT next_pc` | The hart idles until `isa->irq_wakeup` is true (an interrupt pending and enabled, whether or not interrupts are globally on). The scheduler may fast-forward virtual time to the next event, and execution resumes at `next_pc`. Terminator. |
| `EXIT` | `[g] EXIT target {aux=kind}` | Leave the block and continue at `target`. An immediate target is a *direct* exit, a temporary target an *indirect* one; the JIT may chain both. Kinds `JUMP`, `CALL`, `RET`, `EXC_RETURN` are hints for analysis (call graphs, shadow stacks) and do not change semantics. |

### 7.4 Helpers

Each helper is registered with a descriptor:

```
{ name, n_args, arg_width[3], result_width, effect, fn }     (HELPER's aux indexes the table)
effect ∈ { PURE, READS_STATE, WRITES_STATE, MAY_FAULT }   (a bitmask)
```

- `PURE` helpers are deterministic functions of their arguments. They do not accept guards,
  and optimisers may move or remove them.
- `MAY_FAULT` helpers count as faulting ops for the commit rule (§8). The optimizer never moves
  or removes a `HELPER`, and forgets what it knew about register values across one.
- Instrumentation that needs host code (taint) is inserted as helpers. Edge coverage and
  compare logging are the exceptions, with opcodes of their own (§7.5, §7.6): coverage runs
  on every instrumented instruction and must stay inline in native code, and a compare
  record must cost next to nothing while nothing is logging, which a helper (a pass cannot
  add one to an ISA's table, and a call flushes the instruction count) does not.

Helpers are the pressure valve for complex semantics (floating point, crypto, `mret`-style mode
switches). The rule of thumb: **if an operation can be expressed in fewer than ~10 core ops, do
not make it a helper.**

### 7.5 Coverage

`COV.32 A, B` counts one edge in the hart's coverage state (`uam_cov_state`, owned by
whoever enabled coverage). The state holds the map, its `mask`, and a history of up to
`UAM_COV_HIST_MAX` (7) earlier `B` values `hist[]`, of which `depth` are folded (0 means 1),
each rotated left by `rot[j]` within the map's width `bits` first (0: no rotation):

```
if skip_repeat: { if last_pc == pc: done; last_pc = pc }   // pc: the instruction's IMARK
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
folds the last N-1 sites, and rotation makes the fold order-sensitive (the site-list schemes,
[`fuzz-formats.md`](fuzz-formats.md); `uam_cov_count` in `cpu.h` is the reference implementation). `skip_repeat`, also part of
the shape, makes a site count only when it is not the site that counted last (`last_pc`,
which `uam_cov_reset_prev` keeps): back-to-back executions of one site count once and no
site-to-itself edge is ever made (`--cov-mode leapfrog`, the default for site lists). `COV` reads and writes nothing else: no register, no
memory, no bus access, and it does not count as an instruction (`icount` is unchanged). With
no coverage state attached (`cov` is NULL) it does nothing. Lifters never emit it; a pass
inserts it directly after the `IMARK` of an instrumented instruction (§8 rule 8), so it runs
exactly once each time that instruction starts, including when a block is interrupted and
resumed.

### 7.6 Compare records

`CMPLOG.W a, b` (W = 8, 16, 32 or 64; `aux` = a site id, `flags` = what was compared:
`UAM_CMP_EQ`, `UAM_CMP_ORDER`, `UAM_CMP_SIGNED`, `UAM_CMP_IMM`) reports the two values of
one guest compare to the hart's compare state (`cpu->cmp`, a `uam_cmp_state`, `cmp.h`):

```
if cmp != NULL: uam_cmp_note(cmp, aux, W | flags << 8, a, b)   // the log keeps (site, a, b)
```

`a` and `b` are temporaries or immediates of width `W`. Like `COV`, `CMPLOG` reads and
writes no register, memory or bus, cannot fault, does not count as an instruction and
never changes what the guest does; with no state attached it does nothing (native code
tests `cpu->cmp` and branches past it). Lifters never emit it: the compare pass
(`analysis/cmp.c`) inserts one in front of each compare it recognises (`cmp.h` lists the
shapes), anywhere after the block's first `IMARK` (§8 rule 11).

---

## 8. Block well-formedness

The verifier (run on every lifted block unless disabled with `--no-verify`) enforces these rules:

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
7. **Length.** At most `max_block_insns` guest instructions per block (default 32,
   `--block-insns`), which bounds interrupt latency.
8. **Coverage op.** `COV` has width 32, two immediate operands and no guard, and comes
   directly after an `IMARK` that is not a continuation (`UAM_IMARK_CONT`).
9. **`IMARK` shape.** Its pc is an immediate of address width, its length is non-zero, its only
   flags are `CONT` and `BREAK`, and a `CONT` mark cannot start a block. `n_guest` equals the
   number of `IMARK`s.
10. **Operand limits.** `W = 1` is legal only for `AND`, `OR`, `XOR`, `EQ`, `NE`, `SELECT` and as
    a `TRUNC` destination; everything else, memory and `GET`/`PUT` included, needs `W >= 8`
    (`BSWAP` 16). A conversion's source is a temporary, never an immediate. `TRAP` and `WAIT`
    operands are address-width. The fetch and debug access attributes are core-only and are
    rejected in IR.
11. **Compare record.** `CMPLOG` has width 8..64, operands `a` and `b` of that width, no `c`,
    no guard and no destination, only `UAM_CMP_*` flags, and does not start a block.

The verifier runs after the lifter, again after the optimizer, and after each pass. The
optimizer (`uam_optimize`) keeps side effects, `IMARK`s and the order of the remaining `PUT`s;
it removes unreachable and dead code and a `PUT` that a later `PUT` of the same guest
instruction overwrites. A pass (`uam/pass.h`) may only insert ops, and never removes or
reorders guest-semantic ops or `IMARK`s.

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
`cpu->exit_request`. The executor checks it at each `IMARK` but a block's first and continuations, and before
chaining into another block. Because of the commit rule, stopping at an `IMARK` is always a clean architectural
boundary. Interrupt acceptance (`isa->irq_take`) runs between blocks, and is guaranteed to be
re-evaluated only after an exit request (chained native code does not return to the loop
otherwise), so an ISA must raise one on every change that can make an interrupt
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
  unchanged or, after it changes, while the block's pages still translate to the frames it
  was fetched from; the ISA bumps it with `uam_cpu_xlate_changed()` on TLB writes and
  address-space switches. A load or store (not an `AMO` or `CAS`) that crosses a page boundary is translated at
  both ends (its pages may map to unrelated frames); unless they turn out physically
  contiguous the executor moves its bytes one at a time, in the access's byte order, and
  a translation fault on the second page reports that page's first byte as `tval`.

- **Executor stop marks (`UAM_IMARK_BREAK`).** The executor (never a lifter)
  may set this flag on an `IMARK` of a translated block so the block ends
  before that instruction; PC hooks and debugger breakpoints use it to run at a
  precise instruction boundary without per-instruction checks.

The ISA also declares `default_endian`, the CPU's byte order, so the executor
reads instructions in the guest's order (MIPS is bi-endian), and optionally
`fetch_endian` when code and data differ (ARM BE8: little-endian instructions,
big-endian data; the lifter puts the data order on each memory op).

Later MIPS conventions (`isa/mips32`, for PIC32-class cores), again without new IR:

- **What the core has is read from its Config registers** (`uam_mips_config`): Config.MT
  (TLB or fixed mapping), Config1.CA (MIPS16e), Config3.ISA/ISAOnExc (microMIPS),
  Config1.FP, Config3.DSPP/DSP2P, Config3.MCU, Config3.VInt/VEIC, and `shadow_sets`.
- **ISA mode in pc bit 0**, as for ARM's Thumb bit (`pc_state_bits = 1`): EPC, ErrorEPC, jump
  targets and link addresses carry it; a handler enters in Config3.ISAOnExc's ISA.
- **Lift-time Status bits.** CU1, FR and MX change how instructions lift (the FPU's
  register layout, whether DSP instructions trap), so they are tagged in `cpu->priv` above the
  operating mode (`MIPS_PRIV_MODE`): code translated under one setting is not reused under
  another.
- **Shadow register sets** are swapped in and out of the register file on exception entry
  and ERET (only there does the current set change), so lifted code always reads the current
  set at fixed offsets; RDPGPR/WRPGPR reach the previous set through helpers.
- **MIPS16e** (`mips16.c`) recodes each instruction as the MIPS32 instruction that does the
  same (lifted by the MIPS32 lifter) where one exists; branches, jumps, ADDIUPC/LWPC and
  SAVE/RESTORE are lifted directly. JAL/JALX/JR/JALR's delay slot uses a continuation IMARK;
  the decoder is a pure function, checked against objdump by `tests/isa-mips`. **microMIPS**
  (`mips_umips.c`) works the same way; its delay slots are 16 or 32 bits (the `S` forms and
  JALRS need 16), and LWM/SWM/LWP/SWP, MOVEP, JRADDIUSP and LWXS are lifted directly.
- **FPU** (`mips_fpu.c`): loads, stores and moves are inline (register offsets from FR at
  lift time); each arithmetic, conversion or compare is one helper on the register file
  that writes FCSR and, for an enabled exception, fails with the destination unchanged.
  Results come from the host's IEEE arithmetic with the MIPS rules computed in software
  (legacy NaNs and the default NaN, tininess before rounding, invalid conversions), so
  they are the same on every host.
- **DSP ASE** (`mips_dsp.c`): the encoding is checked at lift time (reserved, or DSP Disabled
  with Status.MX clear, tagged in `cpu->priv`); LWX/LHX/LBUX are inline loads; every other
  instruction is one helper on the register file, the accumulators (ac0 = HI/LO) and
  DSPControl. Its microMIPS encodings (all in POOL32A) are recoded to MIPS32 from a table,
  `umips_dsp.h`, generated from the GNU assembler by `tests/isa-mips/gen-umips-dsp.py` (each
  instruction assembled in both ISAs with one operand bit set at a time).
- **External interrupt controller.** The controller pushes its request (RIPL, vector,
  optional explicit offset, shadow set) with `uam_mips_eic_input`, which requests an exit;
  `irq_take` compares RIPL with Status.IPL.

## 10b. Two instruction sets and IT blocks (ARMv7)

ARM needed no new IR; these are conventions of its lifter (`isa/arm`).

- **The instruction set is pc bit 0.** Thumb code runs at odd pcs (the address
  plus one, as ARM's interworking branches and ELF symbols already write it),
  ARM code at word-aligned pcs. Block and `IMARK` pcs carry the bit, so blocks
  of either set are cached and resumed apart, and a branch that may switch sets
  (`BX`, `LDR pc`, `POP {pc}`) is an ordinary computed `EXIT` to
  `SELECT (addr & 1), addr, addr & ~3`. Code is fetched at the even address.
- **Conditional execution is guards.** An instruction's condition becomes the
  guard of each of its side effects; a conditional branch is a guarded `EXIT`,
  followed by the block's unguarded `EXIT` to the next instruction. Flags live in the register file, one
  byte each, so a condition is one or two pure ops.
- **IT blocks are continuations.** The instructions an IT instruction covers are
  lifted with `UAM_IMARK_CONT` under their own conditions, so no interrupt is
  taken inside an IT block, and a fault in one reports the IT instruction as
  `parent_pc` (from which the ISA recomputes the IT state for SPSR). When a block
  must stop inside an IT block, or an exception returns into one, the remaining
  IT state goes into `cpu->priv`, which tags translated code: the next block is
  lifted for it, one instruction at a time, until the IT block ends.

## 10c. Condition register fields and lift-time privilege (32-bit PowerPC)

PowerPC needed no new IR either; these are conventions of its lifter (`isa/ppc32`).

- **Flags as bytes.** Each CR field is one byte of the register file (LT GT EQ SO in
  bits 3..0), and XER's SO, OV and CA one byte each, so a compare is one `PUT` of a
  `SELECT` ORed with SO, a conditional branch tests one byte, and `mfcr`/`mtcrf`/`mfxer` assemble
  or scatter the bytes. Carry and overflow of three-operand sums (`adde`, `subfe`,
  ...) are `CARRY` of each addition and the sign rule on the result.
- **The MSR bits code depends on are in `cpu->priv`.** Problem state, floating point
  available and the trace enables tag translated code, so a privileged instruction in
  problem state, or an FP instruction with MSR[FP] = 0, lifts to its exception
  directly, and with MSR[SE] or MSR[BE] (Book E: MSR[DE] with DBCR0's instruction-complete
  or branch-taken event) a block holds one instruction and arms the trace exception
  before it. Which privileged instructions exist (classic or Book E) is the CPU model's,
  fixed per hart.
- **Floating point is one helper.** FP arithmetic, rounding, conversions, compares and
  the FPSCR instructions call one `HELPER` with the instruction word, which reads and
  writes FPRs, the FPSCR and CR1 itself; FP loads, stores and the sign moves stay IR (`lfs`/`stfs` convert
  between single and double through a helper).
- **No delay slots, no IT state**: every branch ends its block with guarded `EXIT`s;
  `bc` decrements CTR with a `PUT` before them.

## 10d. Two address spaces and registers in memory (8-bit AVR)

The AVR needed no new IR either; these are conventions of its lifter (`isa/avr8`).

- **A Harvard machine on one bus.** Program memory and the data space are separate address
  spaces with overlapping addresses. The board puts program memory at bus address 0 and the
  data space at 0x800000, as the GNU tools do in ELF files; `isa->translate` adds the base to
  loads and stores and leaves fetches alone, so the IR addresses the data space with plain
  `LD`/`ST`, and LPM/ELPM read program memory with `PHYS` loads. The pc is a byte address
  (twice the AVR's word PC). A debugger or loader (`UAM_XLATE_DEBUG`) uses bus addresses.
- **Registers that are also memory.** On some cores data addresses 0-31 are the register
  file, and SREG, SP, RAMPx and EIND (and the AVRxm/AVRxt interrupt controllers' registers)
  are I/O registers in the data space. An access with a constant address (IN, OUT, SBI, CBI,
  LDS, STS) becomes a `GET`/`PUT` or a `SYSRD`/`SYSWR`. An access through a pointer computes
  whether its address is one of those registers, and does a guarded `HELPER` for them and a
  guarded `LD`/`ST` for the rest of the data space; a guard that is false leaves 0, so the
  two results are ORed.
- **Flags as bytes.** SREG is one byte per flag in the register file, as on ARM and PowerPC;
  the half-carry is bit 4 of `a ^ b ^ result` for additions and subtractions alike.
- **SEI's shadow.** SEI sets SREG.I with a `SYSWR` that requests an exit and lifts the next
  instruction as a continuation (`UAM_IMARK_CONT`), so that instruction runs before any
  interrupt, as the core promises (`sei; sleep` sleeps).
- **Skips** (CPSE, SBRC/SBRS, SBIC/SBIS) end the block with a guarded `EXIT` past the next
  instruction, whose length (one or two words) the lifter reads.

## 10e. Two microarchitectures, one descriptor (AVR32)

AVR32 needed no new IR; these are conventions of its lifter (`isa/avr32`).

- **One descriptor, the model decides.** AVR32A and AVR32B share the instruction set; the CPU
  model (`uam_avr32_configure`) says which a hart is, and with it how events save state, the
  memory map, and whether SIMD exists. `cpu->priv` is 0 in application mode and 1 in the
  privileged modes, so a privileged instruction in application mode lifts to its exception.
- **Flags as bytes.** C, Z, N, V, Q, L and T are a byte each in the register file; the rest of
  SR (mode, masks, debug) is in the ISA's state, so a mode change goes through a system
  register write or a helper and requests an exit.
- **The PC is not a register.** R15 reads fold to the instruction's address; writes to it are
  computed `EXIT`s.
- **Interrupts from outside.** The handler of an interrupt is EVBA plus an autovector that the
  board's interrupt controller supplies when the CPU takes it (`uam_avr32_set_autovector`);
  COUNT==COMPARE is an output line of the CPU that the board routes back in, as on the
  hardware. COUNT counts scheduler ticks.
- **Translation without side effects.** AVR32B's TLB lookup is `isa->translate`; a miss or a
  protection fault becomes the event, and the TLB entries' Accessed bits are set by tlbw only.

## 10f. Banks, a hardware stack and four instruction sets (8-bit PIC)

The 8-bit PIC needed no new IR; these are conventions of its lifter (`isa/pic8`).

- **One descriptor, four instruction sets.** The baseline (12-bit), mid-range and enhanced
  mid-range (14-bit) and PIC18 (16-bit) families share a data-space model and differ in
  encodings; the CPU model (`uam_pic_configure`) picks the decoder.
- **Program memory as the tools lay it out.** Program memory is at bus address 0 as gpasm's
  hex files have it: a 12- or 14-bit word in two little-endian bytes (the pc is twice the word
  PC, the upper bits of each word ignored on fetch), PIC18 byte addressed. The data space is
  at 0x800000 through `isa->translate`, as on the AVR; program-memory reads (TBLRD, an enhanced
  FSR at 0x8000 and above) are `PHYS` loads.
- **Operands resolved at lift time where the hardware fixes them.** A file-register operand
  becomes a data address. The core's registers mirrored in every bank (INDF, PCL, STATUS,
  FSR, PCLATH, INTCON, and the enhanced core's 0x00-0x0B) and PIC18's access bank are
  constant addresses: GET/PUT, or SYSRD/SYSWR for INTCON, RCON, STKPTR and TOS (an INTCON
  write may enable an interrupt, so it requests an exit). A banked operand (RP bits, BSR,
  baseline FSR<7:5>) or an indirect one is computed at run time; where it can reach a core
  register, a guarded `HELPER` handles those and a guarded `LD`/`ST` the rest, the results
  ORed as on the AVR. A named INDF (PIC18: POSTINC, POSTDEC, PREINC, PLUSW) is an indirect
  access with its FSR update; through an FSR, INDF-class registers read 0. The enhanced FSR
  map (traditional, linear, program memory) is computed in IR, its unimplemented ranges by a
  false guard.
- **Register writes last, flags after the destination.** Every instruction queues its
  register writes and commits them after its loads, stores and helpers (the commit rule). The
  data sheets' rule that an instruction setting flags overrides its own write of STATUS
  follows: the flags are queued after the destination.
- **PCL writes and run-time redirects.** A constant write to PCL is a computed `EXIT` (with
  PCLATH, PCLATU, or baseline STATUS PA). A run-time core write returns `pc | 1` when it hit
  PCL, and the instruction ends with a guarded `EXIT` on that bit. GOTO/CALL targets that take
  page bits from PCLATH or PA exit directly when the page is the one the code is in (the
  common case, so the cache can chain) and indirectly otherwise.
- **A stack outside memory.** The hardware return stack is ISA state, pushed and popped by
  helpers whose result also says whether an overflow or underflow reset the core (STVREN);
  then the instruction leaves for the reset vector. Interrupt entry, the enhanced core's
  context save and PIC18's fast register stack are in `irq_take`.
- **Skips** end the block with a guarded `EXIT` one word on: a PIC18 skip over a two-word
  instruction lands on its second word, which executes as a NOP, as on the hardware.

## 10g. Four address spaces and banked registers (8051)

The 8051 needed no new IR; these are conventions of its lifter (`isa/mcs51`).

- **Spaces on one bus, RAM in the hart.** Code is at bus 0, external data (MOVX) at
  `UAM_MCS51_XDATA_BASE` and the SFRs at `UAM_MCS51_SFR_BASE` (plus page x 0x100 with SFR
  paging), so MOVX and peripheral SFR accesses are plain `LD`/`ST`. The 256 bytes of internal
  RAM are the first bytes of the register file: a direct address below 0x80 (and a bit in the
  bit-addressable area) is a `GET`/`PUT`, an indirect one (@R0, @R1, the stack) a `HELPER`.
  With SFR paging the page is a register-file byte the address adds in.
- **The bank as translation state.** R0-R7 are internal RAM 8 x PSW.RS + n. A block is lifted
  for the bank (and, on a core with two, the data pointer) in effect, which the ISA keeps in
  `cpu->priv`; a write to PSW or the data-pointer select is a `SYSWR` that updates it and
  ends the block, so the next block is looked up, and translated, for the new state.
- **The core's SFRs.** ACC, B, SP, DPL/DPH and the page register are register-file bytes
  (`GET`/`PUT` for a direct address); PSW is CY, AC and OV one byte each plus a byte of its
  other bits, P being computed from ACC when PSW is read; IE, IP, PCON and the model's
  extended interrupt registers are `SYSRD`/`SYSWR`. Bit instructions on any SFR are a
  read-modify-write of its byte.
- **IE's shadow.** A write to IE or IP requests an exit and lifts the next instruction as a
  continuation (`UAM_IMARK_CONT`), so it runs before the interrupt the write enabled. PCON's
  IDL (and PD) make the hart wait from the system register write.
- **Code banks through translation.** Above the model's `bank_from`, `isa->translate` maps
  fetches and MOVC loads onto the bank each selects; those translations are not fixed, and a
  bank change calls `uam_cpu_xlate_changed`.

## 10h. Decimal mode as translation state and the interrupt poll (6502)

The 6502 needed no new IR; these are conventions of its lifter (`isa/m6502`).

- **One space, 16-bit arithmetic.** Address a is bus address a. Effective addresses are
  computed in 16 bits and zero-extended, so indexing wraps at 64 KiB as on the chip; the
  zero-page modes add in 8 bits (zp,X wraps within the page, and a zero-page pointer at 0xFF
  takes its high byte from 0x00). The stack is `LD`/`ST` at 0x100 | S.
- **Flags one byte each.** C, Z, V and N are register-file bytes (0 or 1); I and D, whose
  changes have effects beyond the bit, are one byte only the system code writes (a `SYSWR`).
  P is assembled when PHP, BRK or an interrupt pushes it, with B and bit 5 set as each does.
- **D as translation state.** On a model with decimal mode, a block is lifted for the value
  of D in effect, which the ISA keeps in `cpu->priv`: ADC and SBC lift to binary IR, or to a
  pure `HELPER` that applies the model's decimal rules (the NMOS and 65C02 flags differ). SED,
  CLD and PLP are a `SYSWR` that updates D and ends the block; SED or CLD that leaves D as
  translated lifts to nothing. On the 2A03 D is a flag only and blocks run on.
- **The interrupt poll.** The chip polls IRQ before an instruction's last cycle, so after CLI
  (or a PLP clearing I) one more instruction runs before a pending IRQ, and after SEI (or a
  PLP setting I) a request already pending is still taken, with I set in the pushed P. The
  lifter ends the block after those instructions; the `SYSWR` records the old I, and at the
  boundary `irq_take` either lets the pending IRQ in as if I were still clear, or declines it
  and requests an exit, which the executor honours after the next instruction. RTI's `SYSWR`
  takes effect at once.
- **Read-modify-write on NMOS.** An NMOS INC, DEC, shift or combined instruction on memory is
  `LD`, `ST` of the byte read, `ST` of the result: a device register sees both stores.
- **WAI, STP, JAM.** WAI is `WAIT` (the ISA wakes on any request, masked or not); STP and the
  NMOS JAM opcodes are a `SYSWR` that stops the core and a `WAIT` it never wakes from.

## 10i. Prefixes, flags whole and a register that counts (Z80)

The Z80 needed no new IR; these are conventions of its lifter (`isa/z80`).

- **Two spaces on one bus.** Memory is at bus 0 and the I/O ports at `UAM_Z80_IO_BASE`, so
  IN/OUT are plain `LD`/`ST`. The port address is the 16 bits the Z80 puts out (A or B on the
  high byte), ANDed with the board's decoding at lift time. Memory addresses wrap at 64 KiB,
  so a 16-bit memory access is two byte accesses.
- **Prefixes are part of the instruction.** DD and FD select IX or IY for HL (and IXH/IXL for
  H and L, except in an instruction with an (IX+d) operand); an instruction is its prefixes
  and opcode, one `IMARK`. A prefix that another prefix follows does nothing and is an
  instruction of its own, lifted with the next as a continuation (`UAM_IMARK_CONT`), since the
  Z80 takes no interrupt between them.
- **Flags whole.** F is the byte the Z80 has (bits 3 and 5 included), computed whole by each
  instruction that sets flags: from the result, or MEMPTR for BIT n,(HL), or the address of a
  repeating block instruction while it repeats. MEMPTR is a register-file word that the
  instructions setting it write.
- **Q and R as register-file state.** Each instruction records in a byte whether it set the
  flags (SCF and CCF take bits 3 and 5 from `(Q ^ F) | A`, Q being F or 0); the lifter writes
  it only when it changes within the block. R is a counter byte each instruction adds its
  opcode fetches to (one per prefix and opcode), with bit 7 kept apart.
- **Register writes last.** Every instruction queues its register writes and commits them
  after its loads, stores and system register accesses (the commit rule); conditional
  CALL/RET use guarded stores, loads and writes.
- **EI's shadow.** EI is a `SYSWR` that requests an exit and lifts the next instruction as a
  continuation, so it runs before the interrupt. When the block ends at EI, the core's shadow
  flag (set by EI, cleared by the next instruction's `SYSWR`) makes `irq_take` let one more
  instruction run. RETN/RETI (IFF1 from IFF2), IM and LD A,I/R's IFF2 are system registers too.
- **Repeating block instructions** do one iteration and `EXIT` to themselves while they repeat,
  so each iteration is an instruction and an interrupt can come between them.

## 10j. A configurable core, register windows and zero-overhead loops (Xtensa)

Xtensa needed no new IR; these are conventions of its lifter (`isa/xtensa`).

- **The options are the model's.** One module for every configuration: the CPU model lists the
  options, and an instruction or special register of an option the model lacks is an illegal
  instruction, decided when the block is lifted. The decoder is a table of fixed bits over the
  little-endian instruction word (generated from `tests/isa-xtensa/opcodes.py`, checked against
  the GNU tools of each core); a big-endian core lays each format's fields out in the opposite
  order, and the decoder rearranges them to the little-endian word first.
- **The current window in the register file.** With the Windowed Register option the physical
  file holds every frame, and the 16 registers WindowBase selects are copied to fixed offsets
  while they are current; ENTRY, RETW, ROTW, a window exception and its return, and a write of
  WINDOWBASE put them back and load the new ones (in a helper or a system register write), so
  lifted code reads `a0`-`a15` at fixed offsets. The window-overflow check of an instruction
  that names `a4`-`a15` is a guarded `TRAP` on a register-file word the system code keeps
  (WindowStart rotated to WindowBase, zero while PS.WOE is clear or PS.EXCM set), tested once
  per window group per block; the exception picks the frame to spill and its vector.
- **Zero-overhead loops through a lift-time loop end.** The loop-back happens when an
  instruction falls through to LEND with LCOUNT not zero and PS.EXCM clear. A block is lifted
  with LEND's value at the time and ends there when LEND is one of its boundaries, followed by
  the loop-back: `[LENDA == LEND] PUT LCOUNT - 1; [..] PUT LENDA; [..] EXIT LBEG`. LENDA, a register-file word,
  is LEND while a loop-back can happen and all ones otherwise. On entry every block checks that
  LENDA is not at another of its boundaries; if it is (the loop end moved into code translated
  before), the hart runs one instruction per block (a bit of `cpu->priv`) with the loop-back
  tested after each, and the code's next translation sees the new LEND. LCOUNT therefore is not
  translation state, and a loop's iteration count never causes a new translation.
- **Exceptions as values, vectors at VECBASE.** EXCCAUSE values are `TRAP` causes; window
  overflow and underflow, BREAK (a debug exception, or nothing at or above DEBUGLEVEL), and the
  helpers' failures (ENTRY's overflow, RETW's underflow, MOVSP's Alloca) have causes of their
  own that `take_exception` turns into the vector and the state the ISA defines. Bus faults are
  LoadStoreError and InstructionFetchError, Region Protection's refusals (`isa->translate`)
  Load/StoreProhibited and InstFetchProhibited, misaligned accesses LoadStoreAlignment on a core
  with the Unaligned Exception option. There is no PrivilegedCause: the cores have one ring
  (their configuration's RING field has no bits, so PS.RING reads 0), and PS.UM only chooses
  the user or kernel vector.
- **Interrupt levels between blocks.** INTERRUPT is the level lines as they are plus the
  latched edge, software and timer bits; `irq_take` takes the highest level above PS.INTLEVEL
  (and above EXCMLEVEL while PS.EXCM is set; the NMI regardless): level 1 through the user or
  kernel vector with EXCCAUSE 4, higher levels through their own with EPCn and EPSn. Writes of
  PS, INTENABLE, INTSET, INTCLEAR and CCOMPARE, RSIL and the returns request an exit. CCOUNT
  counts ticks of virtual time; each CCOMPARE match is a scheduler timer.
- **Coprocessors by a dynamic check.** The FPU's instructions test CPENABLE once per block (a
  register-file copy), so changing CPENABLE, as an RTOS's lazy context switch does, never
  retranslates code. FP arithmetic, conversions and compares are one helper on the register
  file; FP loads, stores and moves stay IR. MAC16's 40-bit accumulator is IR on 64-bit values.

## 10k. Privilege as translation state, and interrupt shadows (x86-64)

- **`cpu->priv` holds everything decoding and translation depend on**: CPL (bits 0-1), mode
  (bits 2-3: real, protected, IA-32e, virtual-8086), SMM (4), default code size (5-6), 32-bit
  stack (7), paging (8), A20M# (9), an interrupt shadow carried into the block (10) and the SMM
  code-fetch check (11). Translated code is never reused across a change of any of them.
- **`CONT` for the interrupt shadow.** The instruction after `STI` or `MOV SS` is a
  continuation, so no interrupt lands between them. Unlike a delay slot, its fault is its own:
  the parent has completed.
- **I/O ports are memory.** `IN` and `OUT` are `PHYS` `LD`/`ST` at `UAM_X86_PIO_BASE` + port.
- **128-bit division is a helper**, as is everything that touches descriptors or system state.

## 11. Opcode numbering and extension profiles

| Range | Profile |
|---|---|
| `0x00–0x3F` | **Core** (this document) |
| `0x40–0x7F` | **F**: native floating point (reserved; until defined, floating point uses `HELPER`) |
| `0x80–0xBF` | **V**: 128-bit and vector lanes (reserved) |
| `0xC0–0xFF` | Implementation-private (debug and experimental only; never emitted by shipped lifters) |

Only Core is implemented; the verifier rejects any opcode at or above `UAM_OP__COUNT`.

---

## 12. Text form

The canonical printed form is used for traces, tests and documentation. One op per line, with
attributes after the mnemonic, then the width, and auxiliary fields in braces (an `AMO`'s sub-op
is a dotted suffix). There is no parser: the form is output only.

```
imark   0x80000010 {len=4}
t0:32 = get.32  {a0}
t1:32 = add.32  t0, 0x10
t2:32 = ld.le.32 t1
        put.32  t2 {a1}
t3:1  = eq.32   t2, 0
[t3]    exit.32 0x80000100 {jump}
        exit.32 0x80000014 {jump}
```

Each block starts with a `block 0x...: N guest insns, N ops, N temps` line. An `imark` shows
`,cont` or `,break`. Further access attributes print as `.al`, `.sc`, `.unpriv` and `.phys`.
Immediates below 10 are decimal and the rest hex. A register offset prints by name when the
ISA's table has one of that width (`{reg+N}` otherwise), and a system register as `{sys 0xN}`.

---

## 13. Worked examples: RISC-V (RV32IMAC)

`pc` is the instruction's address. `#imm` is the decoded immediate. `{rs1}`, `{rd}` and `{csr}`
stand for the operand's name, and the listings leave out the `.W` width suffix a trace prints
on every op (`get.32`, `ld.le.8`), except where it matters.

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
        exit t2 {ret if rd==x0 && rs1 is ra or t0, call if rd is ra or t0, else jump}
```

**`lr.w.aq rd, (rs1)` / `sc.w rd, rs2, (rs1)`**
```
t1:32 = ld.le.x.al.acq.32 t0    ;  put t1 {rd}
t2:1  = st.le.x.al.32 t0, t1    ;  t3:32 = zext.32 t2
t4:32 = xor t3, 1               ;  put t4 {rd}      # RISC-V: 0 = success
```

**`amoadd.w rd, rs2, (rs1)`** — `t2:32 = amo.le.al.add.32 t0, t1` then `put t2 {rd}`.

**`csrrw rd, csr, rs1`** — ends the block, because a CSR write can change interrupt enables or privilege.
```
t0:32 = get {rs1}
t1:32 = sysrd {sys csr}        # omitted when rd == x0 (no read side effects)
        syswr t0 {sys csr}
        put t1 {rd}
        exit pc+4 {jump}
```

**`ecall`** — `trap RV_EXC_ECALL_U, 0 {exception}`. The ISA module adds the privilege level to
get the cause; the modelled hart is M-mode only, so that is 11.

**`wfi`** — `wait pc+4`.

**`fence.i`** — `fence {insn}` then `exit pc+4`.

**`mret`** — a mode switch with many coupled CSR effects, so it is a helper:
```
t0:32 = helper {rv_mret}       # effect: READS_STATE|WRITES_STATE|MAY_FAULT; restores MIE/MPP, returns mepc
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
    uint8_t   addr_width;              /* guest address width (32 or 64) */
    uint32_t  n_insn, n_temp, n_imm;
    uint32_t  n_guest;                 /* guest instructions (IMARKs) */
    uam_insn *insn;
    uint8_t  *temp_width;              /* per-temporary width */
    uint64_t *imm;                     /* immediate pool */
    uint32_t  n_pages;                 /* code pages covered, at most 2 */
    uint64_t  page[2], page_gen[2];    /* for invalidation */
    void     *backend;                 /* owned by an execution backend (compiled code) */
    void    (*backend_free)(void *backend);
} uam_block;
```
