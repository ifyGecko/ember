# Step 13 design: the x86 gap (narrow register reads, writes carried across chained exits)

Status: **approved** (2026-10-28). Stage N is implemented (§9); stages C1 and C2 are not yet. It builds on step 9 (docs/jit-dead-puts-design.md) and on the chaining of steps 7 and 10 (docs/jit-perf.md).

## 1. What it is, in one paragraph

On ember's cross-ISA benchmark, x86-64 still takes 1.4–1.8× the time of RV32 per kernel, though it executes about as many guest instructions (crc: 187 M against 196 M). Two things make up nearly all of the difference, and neither is about x86 as such:
1. **Writes live at a block exit.** Step 9 removes a register write that a later instruction of the same block overwrites. It cannot remove the last write before an exit, because the next block might read it. In a hot loop every pass ends at an exit, so the last flag-setting instruction of each pass computes and stores all six flags. The next pass then overwrites all six before reading any.
2. **Narrow reads of a register that was just written whole.** The optimizer forwards a register value to a later read only on an exact offset and width match. So `get.32 {reg+56}` after `put.64 {rdi}` reads memory again, and the folds that would follow are lost.

This step fixes both generically:
- **Stage N:** forward a narrow read from a known wider value. This is an IR optimization.
- **Stages C1 and C2:** extend step 9's deferral across a chained exit. The writes pending at an exit are not done there when the block it chains into overwrites them before anything can observe them. The successor finishes them only on its own cold paths, if it stops before overwriting them.

## 2. Why it is worth doing

### 2.1 Where the time goes

**Loop exit (x86 crc kernel, block 0x120, `inc rdi`; from `-t ir`).** The whole block-exit cost is in this shape. One guest instruction, 18 ops after the optimizer:

```
t0:64  = get.64  {rdi}
t1:64  = add.64 t0, 1
         put.64 t1 {rdi}
t2:1   = ult.64 t1, t0          ; CF
t3:64  = xor.64 t1, 1           ; OF: 4 ops
...
         put.8 t7 {cf}  ... put.8 t8 {of}  ... put.8 t13 {af}
t14:1  = eq.64 t1, 0
         put.8 t15 {zf}  ... put.8 t17 {sf}  ... put.8 t18 {pf}
```

Step 9 already removes all of this when a later instruction of the same block overwrites the flags. At the last flag writer before an exit it removes none of it. On a RISC-V or MIPS guest the same loop step is one `add` and one `put`.

**Kill check.** A static check on the final IR of both x86 kernels asked whether a block overwrites all six flags before it reads any or reaches a hard point, as defined below. The hot blocks all do. Over all blocks: crc 18 of 29, matmul 25 of 36. So nothing reads the flags the previous block left in these loops.

### 2.2 Upper bounds, measured

All runs are the per-kernel medians of 5, under the same conditions as RV32. The prototypes were made on a scratch copy of the tree and thrown away:
- **E1** forwards narrow reads (stage N).
- **E2** drops every flag write. It is wrong in general and only bounds what stage C can gain.

| x86 time / RV32 time | today | E1 + E2 |
|---|---:|---:|
| crc | 1.76 | 1.13 |
| matmul | 1.72 | 1.13 |
| sort | 1.53 | 1.02 |
| string | 1.53 | 0.98 |
| fmt | 1.73 | 1.17 |
| list | 1.44 | 1.11 |

- **E1 alone:** 3–5% on each kernel.
- **E2 alone:** 1.3–1.4×.
- **ARM and Thumb:** the same E2 on NZCV bounds them at 1.15–1.25×. Some ARM kernel outputs came out wrong under E2. So ARM code does read flags across blocks, and any real fix has to tell those blocks apart. Stage C2 does that at link time (§4.4).
- **RV32 and MIPS** have no flags and gain nothing from C; this step does not slow them (§6).

### 2.3 Parity

This attacks the same spread step 9 did, where flag-heavy ISAs are slower than flagless ones, at the next boundary out: the block exit. It does not know what a flag is. It applies to any register bytes that the next block overwrites before it reads them: x86 flags, ARM/Thumb NZCV, PowerPC CR fields, or a MIPS/RISC-V temporary register.

## 3. Stage N: narrow reads of a known wider value

**Where:** `src/ir/opt.c`, at its GET case (`lookup_known`, line 108).

**Rule:** a GET of bytes [off, off+w) whose bytes all lie inside a known value v at [koff, koff+kw) becomes:
- `TRUNC.w v` when off == koff;
- `TRUNC.w (LSHR v, 8·(off-koff))` otherwise. That case covers x86 `ah` and the sub-registers of other ISAs. (Not implemented: see §9.)

The register file is little-endian by definition, so this holds for every guest byte order. The new ops then go through the existing simplify, peephole and CSE paths. For example, `TRUNC.32 (ZEXT.64 x)` becomes x.

**Unchanged:**
- A narrower PUT still makes `forget_overlapping` drop the wide value.
- Guarded writes and the state clobbers of helpers and system registers behave as today.
- Only the source of a GET's value changes, and the replacement computes exactly the bytes the GET would read.

**Tests:**
- test_ir checks the rewrite. Cases: same offset; higher offset; a narrower PUT in between, which must block it; a helper in between, which must block it; a guarded wider PUT.
- test_jitdiff level C gets the same shapes, run against the unoptimized interpreter.

**Expected:** 3–5% on x86 kernels. On other ISAs it should be neutral or slightly positive: ARM byte loads into a register that is then read whole do not match, but PowerPC and x86 sub-register reads do.

This stage is independent of the others and lands first.

## 4. Stage C: writes carried across a chained exit

### 4.1 Correctness model

The invariant is unchanged. Wherever guest state can be observed, the JIT's register file equals the interpreter's, and the interpreter is untouched.

The new fact this stage relies on: when B's exit E jumps straight into S's native code, nothing runs in between. No run loop, no hook, no interrupt check: the chain path checks those before jumping. So the register file is observable next at S's own observation points, and S is just a continuation of B. A write pending at E is then exactly like a write step 9 defers within one block. It is safe as long as it is replayed at every soft point before the write that overwrites it, and nothing reads or hard-observes it first.

**Terms, for a block S:**
- **Window:** S from its entry up to its first hard point or its first EXIT, guarded or not. The hard points are those of step 9 (§3 there, §9 for the loads and stores that always call C), and exits end the window as well.
- **kill(S):** the register bytes that an unguarded PUT inside the window writes before any GET of them.
- **pinned(S):** the bytes of kill(S) that S writes in place (not deferred) with a soft point after the write still inside the window.
- **carry-safe(S):** kill(S) minus pinned(S).

**Carry condition for a link E → S:**
- pending(E) ⊆ carry-safe(S), where pending(E) is the set of register bytes B's writes still pending at E cover;
- E has at most `CARRY_MAX_VALS` inputs (§4.3);
- the link is E's own slot, not a jump-table entry.

The condition is checked when the link is made (§4.4). It is exact, so everything else that decides what is pending at an exit is only a heuristic that affects speed.

**Inside S's window, at each soft point p,** the replay becomes: first B's carried writes (the materializer, §4.3), then S's own pending writes at p, as step 9 does today. This is correct byte by byte:
- **A byte S has not written yet:** it holds B's carried value, which the interpreter would also have there.
- **A byte S has written through a deferred PUT:** that PUT, or a later one covering it, is pending at p. S's replay runs after the materializer, so S's value wins.
- **A byte S has written in place:** it is pinned, because p follows the write inside the window. So it is not in pending(E), and the materializer does not touch it.

**Why the window rule and not something simpler.** A simpler successor rule was considered: S clears the carry at its first observation point after its first write of any byte, with no materializer-then-own-replay ordering. It fails because a block usually writes a general register before its first flag-setting instruction, and an IMARK or load separates the two. On the crc and matmul IR, all six flags are killed under that rule in only 5 of 29 and 9 of 36 blocks, against 18 and 25 under the window rule.

**After the window:** every carried byte is overwritten, so the carry is dead. S never consults it again.

**Leaving native code** (an exit that does not chain, a stop stub, a fault): either B's carried writes were materialized at a soft point of S, or the code that leaves materializes them first. The run loop, hooks, snapshots, gdb and the other CPUs never see a pending carry. Nothing outside `jit_x86.c` changes.

### 4.2 Analysis (extends `defer_analyze`)

**Exits become replay points.** Today an EXIT is a hard point: the backward pass clears K there, so no write is pending at an exit. In stage C it becomes a soft point of its own kind:
- Backward pass, at an unguarded EXIT: K := C(E).
- Backward pass, at a guarded EXIT: K := K ∪ C(E).
- **C(E)** is the candidate set: the bytes worth leaving pending at E.
- **Forward pass:** records pending(E) at each EXIT, as it does at other soft points. The cold-op analysis (stage D2) works unchanged, so arithmetic that only feeds pending writes moves off the fast path too.

**Choosing C(E)** affects speed only, since the link-time check is exact:
- **Self-loop** (E's constant target is B's own pc): C(E) = kill(B).
- **Other constant target already translated:** C(E) = carry-safe of that block (a tcache lookup at compile time).
- **Otherwise:** C(E) = kill(B). The guess is that the next block overwrites what this one does: flags in flag-heavy code, nothing much elsewhere.
- **Computed exit:** C(E) = kill(B). Its own slot can carry (§4.4).

**Why not every byte.** Leaving every write that reaches an exit pending would add to every replay list after it, and would keep its value live to the exit, for writes that are usually read next (general registers). C bounds that.

**Per block, B records** carry-safe(B) as a short sorted list of byte ranges. It is computed after B's own deferral decisions, since pinned depends on them.

**Per exit, B records:**
- pending(E) as byte ranges;
- the inputs its materializer needs: the non-cold temps that its cold ops and pending writes read, guards included.

**Cap:** an exit with more than `CARRY_MAX_VALS` inputs (start at 8) is never carried. It still replays its pending writes, as in C1.

### 4.3 Code generation

**At an exit E with pending writes**, every path does exactly one of two things:
- **Carry** (the chain hit path, when the slot's carry flag is set): store E's inputs to `jc->carry_vals[0..n)`, store the address of E's materializer to `jc->carry`, then `dec chain_left; jmp [slot.entry]`.
- **Replay** (every other path: the chain checks fail, a slot miss, the jump table, or a hit without the carry flag): store the inputs, then `call` the materializer, then go on as today.

**The materializer** is one per exit, out of line after the block's code. It:
- recomputes E's cold ops from `carry_vals` (in program order, as step 9's replays do);
- does the pending writes, each under its guard;
- stores 0 to `jc->carry`;
- returns with `ret`.

It uses only rax, rcx, rdx and the flags, as step 9's replays do. Both of its uses already treat those as scratch: S's stop stubs and load/store slow paths, and B's exit.

**Clearing on an exit path.** An exit always leaves `jc->carry` either cleared (by the materializer) or set to its own stub. So a block that inherited a carry and leaves through an exit needs no separate clear: its window ends at or before the exit, and the inherited carry is dead by then.

**In S (any block with a non-empty carry-safe set), before its own replays at each soft point inside the window:**
- `mov rax,[rbx+carry]; test rax,rax; jz 1f; call rax; 1:`
- This sits on the cold paths only: the IMARK stop stubs, and the load/store slow paths before the bridge call. The load/store address in rdx is reloaded after the replays, as step 9 already does.

**At S's window end, if it is not an exit:** one `mov qword [rbx+carry], 0`. This is the only fast-path cost a successor pays.

**Blocks entered from the run loop:** `jc->carry` is 0 on entry, because native code never returns with it set. In debug builds `uam_jit_run` asserts that.

**Cost on the hot path, crc loop:**
- Today: about 18 IR ops and 6 byte stores of flag work at the loop's last `inc`/`cmp`.
- Carried: the exit stores about 3 inputs and one pointer, and the slot check gains a byte compare.

### 4.4 Linking

**The slot.** `jit_slot` gains a `carry` byte and a pointer to its exit's static descriptor (pending ranges and input count). The jump table keeps the same type and never sets `carry`.

**At link time** (`uam_jit_run`, where `pend_slot` is linked today), `carry` is set when the carry condition of §4.1 holds. The check uses:
- pending(E), from the slot's descriptor;
- carry-safe(S), from the target block's descriptor, reachable from `blk->backend` like `body_off`.

This runs once per link, not per hop.

**Invalidation:** a link (and with it its carry flag) dies with the epoch, as today. A retranslated S gets new links and new decisions.

**Off switches:**
- `UAM_JIT_NOCARRY=1` never sets `carry`. Exits then replay, which is stage C1's behaviour.
- `UAM_JIT_NODEFER=1` turns off all of step 9 and this stage.

### 4.5 Stages

- **C1, exits become replay points:** the analysis of §4.2, the per-exit materializer, and the replay call on every exit path. Nothing is carried. Performance should be about neutral: the same work, moved from the fast path to the exit path. This stage validates the analysis, the exit materializers and the code-size cost on their own.
- **C2, carrying:** the slot flag, link-time check, `carry_vals`, the successor's checks and window-end clear, and `UAM_JIT_NOCARRY`. This is where x86 and ARM gain.

Each stage is one commit, measured with `tools/xbench.sh --per-kernel` and on both end-to-end workloads.

## 5. Testing

**test_jitdiff level E (new): chained block pairs and loops.** Levels B–D run one block at a time with chaining off (`ch.link = false`), so they cannot see this stage at all. Level E keeps chaining on and drives the run loop with `uam_exec_run`. Patterns:
- B writes flag-like bytes and chains to S, which overwrites them after:
  - a load or store to RAM;
  - a load from a device whose callback hashes the register file;
  - a faulting load;
  - an IMARK stop (every deadline k);
  - a page-crossing access.
- S reads one carried byte before its kill, which must not carry.
- S writes a carried byte in place and then hits a soft point (pinned), which must not carry.
- S overwrites only part of a carried wide write.
- A guarded write in S, which does not kill.
- A guarded exit carrying to one target while the fall-through exits to another.
- A self-loop carrying to itself.
- A computed exit: the own slot carries, the jump-table targets replay.
- An exit request raised by a device inside S's window.
- S retranslated after its code is overwritten (the link and carry flag die with the epoch).

Every pattern runs:
- against the unoptimized interpreter;
- with `UAM_JIT_NOCARRY=1`;
- with `UAM_JIT_REGS=1` for allocator pressure;
- with every deadline.

The comparison covers the register file, RAM, icount and the device log.

**Mutation checks,** as in earlier steps. Each must be caught:
- skip S's materializer call at a soft point;
- run S's own replays before the materializer;
- skip the window-end clear;
- ignore pinned;
- link without the subset check;
- let a jump-table entry carry;
- leave an exit path without replaying;
- forget to clear `jc->carry` in the materializer.

**Unit tests and end to end:**
- test_jit: a hop count that shows carried links happen on a looping guest.
- `make test` (every firmware self-test on the interpreter and the JIT).
- Both end-to-end workloads: identical consoles and identical interpreter-vs-JIT `-d` state, and `ovmf-check`. `secureboot-check` after C2.

**Measurement:**
- The op map labels materializers and carry stores as their own region kinds, `carry`, so `tools/jitprof.py` can show their share.
- No `--stats` counters, for the reason step 9 gave: they would change the library ABI.

## 6. Risks

| Risk | Mitigation |
|---|---|
| A path out of native code that skips the replay, leaving a pending carry visible to the run loop | Every exit path calls the materializer unless it jumps with `carry` set. The debug assert in `uam_jit_run` checks it, and level E stops at every point |
| A successor observation point that misses the materializer call | The window ends at the first hard point or exit, so only soft points need the call, and they all go through the one replay emitter (`emit_replays`) |
| Code size: one materializer per exit with pending writes, longer replay lists in C1 | Measured per stage; `CARRY_MAX_VALS`, and C(E) bounds what is pending at exits |
| Slower ISAs without flags | Their C(E) is mostly empty, because their blocks rarely overwrite a register before reading it, so their exits change little. xbench checks every guest per stage |
| Allocator pressure from inputs kept live to the exit | Same as step 9: use positions at the exit; soak with `UAM_JIT_REGS=1` |

## 7. Alternatives rejected

- **Lazy flags in the x86 lifter.** Store the operation and operands instead of the flags, and compute flags when read. It fixes only x86, and it changes the register-file layout that the ABI, snapshots, gdb and C readers of RFLAGS rely on. Stage C gets the effect for every ISA without changing what the register file holds.
- **Dropping writes at an exit when a static scan of the target says it overwrites them.** Without the materializer, a successor that stops before its overwrite (an interrupt, a fault or a device read in between) would expose a stale register file. The interpreter would differ there.
- **Deciding the carry at compile time instead of at link time.** The target block is often not translated yet, and a retranslated target may differ. The link is the first point where both blocks are fixed, and the epoch already ends a link's life with them.

## 8. What remains after this

If both stages reach their bounds, x86 sits at about 1.0–1.17× RV32 on the kernels. The rest is common to every ISA: each loop pass re-reads its guest registers from memory. That is the "keep guest registers in host registers across a self-loop" item, a separate step judged on all guests.

## 9. As implemented

**Stage N** (`lookup_wider` and the GET case of `uam_optimize`, `src/ir/opt.c`):
- **Only the same-offset case.** A GET of the low bytes of a known wider value becomes `TRUNC.w v`, which then goes through simplify and CSE like any pure op.
- **The higher-offset case is left out.** It needs a second op (the LSHR), and so an insertion in the middle of the optimizer's in-place compacting pass. Counted with a throwaway build, statically over every translated block, it does not pay for that:

| Workload | exact-match reads (forwarded before) | same offset, wider (now forwarded) | higher offset |
|---|---:|---:|---:|
| bench, x86 | 845 | 455 | 0 |
| bench, RV32 / MIPS / ARM / Thumb / PPC | 847–1159 | 0 | 0 |
| OVMF boot, 1e9 instructions | 105098 | 7802 | 91 |
| VxWorks, 6e7 instructions | 30714 | 0 | 0 |

- **Other guests are unaffected.** On the other guests the bench IR is identical before and after, since the new case never fires there.
- **Tests:**
  - test_jitdiff level C pattern 9 covers narrow reads: of a written value, of a constant, of an earlier wider GET, after a narrower write, at a higher offset, after a guarded write, and after a helper.
  - `check_narrow_reads` checks the optimizer's output: the forwardable GETs are gone and the others remain.
  - Mutation checks caught all five variants tried: a higher offset treated as the low bytes, no forwarding, a narrower write not forgetting the wide value, a guarded write remembered, and a helper keeping known values.

