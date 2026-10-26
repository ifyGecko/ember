# Coverage and fuzzing

ember fuzzes firmware in-process: boot once, snapshot at the start of the code under test,
and run every input from that snapshot, counting edge coverage on the way. It works on its own
(no fuzzer needed to build, run, collect coverage or replay inputs). Interoperation with
suzzer goes only through suzzer's published formats, read from a suzzer checkout the build is
given ([`suzzer.md`](suzzer.md)): its site lists and boundary files, its SZR1 network
protocol (ember as the executor a suzzer brain drives) and its LCVS shared-memory region (see
the end of this page).

## Coverage model

Coverage is one IR opcode, `UAM_OP_COV A, B` ([`uam-isa.md`](uam-isa.md) §7.5), which the
coverage pass inserts right after the instruction marker of each instrumented instruction:

```
idx = (A ^ prev) & mask
if (map[idx] != 0xff) map[idx]++      // saturating 8-bit counters
prev = B
```

(the general form folds a longer history, for N-grams, and can skip a site that counted
last; see the ISA document).

`A` and `B` are fixed when a block is translated, so the scheme is just a table of
`(guest pc, A, B)` sites (`uam_exec_set_coverage`, `uam/exec.h`):

| Scheme | A | B |
|---|---|---|
| AFL edges | `cur` | `cur >> 1` |
| block counts | `id` | `0` |
| suzzer site model, N=1 | `site_id` | `0` |
| suzzer site model, N=2 | `site_id` | `spread(site_id)` |
| suzzer hashed-PC model, N=1 | `hash_pc(offset, module)` | `0` |
| suzzer hashed-PC model, N=2 | `hash_pc(offset, module)` | `A >> 1` |

Without a site table, block mode (`--cov-blocks`, `uam_exec_set_block_coverage`) puts a site
at every translated block's start with `A = hash(pc)`, `B = A >> 1`. It is approximate: ember
blocks are not basic blocks (they end at a size limit, at hooks and page ends, and an
interrupted block resumes mid-way), so the edges depend on how code was split, though never
on the backend.

Properties:

- **Coverage off costs nothing**: no block contains COV.
- **The guest is unaffected**: COV touches only the map. `icount`, timing, registers and
  memory are identical with coverage on.
- **Backends agree**: the interpreter and the JIT (chained or not) produce byte-identical
  maps. The JIT emits COV inline, about ten host instructions with no call.
- The map is never snapshot state. Sites match the guest *virtual* pc. Instructions in delay
  slots cannot be sites (translating one is an internal error that stops the run, naming
  the pc).
- **Repeats** (`uam_cov_state.skip_repeat`, `uam_fuzz_config.skip_repeats`): optionally a
  site counts only when another site counted since its last count. A loop with a single
  site then counts once per visit, not once per turn, and no edge from a site to itself
  appears. That is the emit stream of a trap-based coverage runtime such as suzzer's libcov
  (`--cov-mode`, below).

Overhead, synthetic worst case (a MIPS loop of 2-7 instruction blocks): JIT +31%,
interpreter +4%.

## The harness (`uam/fuzz.h`)

1. **Boot** until the first arrival at the begin pc. Coverage so far is the *INIT* map.
   `-n` (`uam_fuzz_config.boot_insns`) bounds the boot; without it a firmware that never
   reaches the begin pc runs until interrupted. `--fuzz-begin-skip N`
   (`uam_fuzz_config.begin_skip`) lets the boot pass the begin pc N times first, for
   firmware that runs its handler during start-up (a self-test, a first request of its
   own): the snapshot is taken at arrival N+1, and everything before it, the skipped
   passes included, is INIT. It is ember's `LIBCOV_BOUNDARY_INIT_GRACE`.
2. **Snapshot** there.
3. **Each input**: restore the snapshot, clear the map and `prev`, deliver the input, run
   until the iteration ends, a crash check fires or the budget runs out, then classify.
   A crash does not end the campaign; the next input starts from the snapshot. Neither
   does an input the route refuses (verdict *rejected*, below). An internal error of ember
   itself (a site it cannot instrument, bad IR) does: it would recur on every input, and
   it is not the input's doing.

Iteration boundaries mirror LCVB1: *paired* (`--fuzz-begin` and one or more `--fuzz-end`:
reaching any end pc ends the iteration) or *begin-only* (only `--fuzz-begin`: arriving there
again ends the iteration). In paired mode, an input that never reaches an end runs out of
budget (a timeout). All are PC hooks. The same input from the same snapshot gives the same map
and verdict, on every backend.

**Coverage at the boundaries.** A coverage site at a boundary pc is counted by its hook,
before the map is published and the history reset, as libcov counts its markers: the
begin's hit goes to the map it closes (INIT at boot; in begin-only mode, the iteration
that ends back at begin), an end's to the iteration it ends. An iteration's first count is
the site after the begin, with an empty history.

**Several begins.** `--fuzz-begin` is repeatable (`uam_fuzz_config.more_begin_pcs`), and a
boundary file may hold several BEGIN markers: a target with two request handlers, say. The
snapshot is taken at whichever begin the boot reaches first (after `--fuzz-begin-skip`
arrivals at any of them), and every input starts there, so it reaches whichever handler the
firmware runs for it. During an iteration any begin acts as the begin does: it ends the
iteration in begin-only mode (as each BEGIN closes the current iteration in libcov) and
opens a level when the boundaries are nested.

**Recursion.** With nested boundaries (`uam_fuzz_config.nested`, set from depth-zero-only
LCVB1 markers), a begin reached during an iteration opens a nested level and every end
closes one. Only the end that closes the outermost level ends the iteration, so a recursive
handler is one input. Without nesting, the first end reached ends it.

**Between iterations.** With paired boundaries, the code between an end and the next begin
(the target's own loop, reading the next request) never runs per input, because each input
starts from the snapshot at the begin. It runs once during boot and is counted in the INIT
map. That is intended: it does not depend on the input. libcov, which runs the target
continuously, publishes it as a separate GAP map; ember has none.

### Input routes (`--fuzz-input`)

| Route | Delivery |
|---|---|
| `mem:ADDR[,len=ADDR][,max=N][,reject]` | the input written at virtual ADDR (truncated to N bytes, default 4096; with `reject`, a longer input is refused instead: verdict rejected), and optionally its length as a 32-bit word in the CPU's byte order |
| `mmio` | the stream that `--mmio-rule ...:input` registers read |
| a board input | anything a board published with `uam_machine_add_input`; the stock boards publish `uart0` (the receive FIFO; what does not fit is dropped) |

The harness never names a device: boards publish inputs, the harness picks one by name.
A route that cannot deliver a particular input (too long for its buffer, not in the shape
it needs) returns `UAM_ERR_REJECTED`: that input's verdict is *rejected*, nothing runs, and
the campaign goes on. Any other error from a route is a harness error and stops the run.

### Verdicts

| Verdict | Cause | detail | `--fuzz-replay` exit |
|---|---|---|---|
| ok | the iteration ended (or the firmware halted with status 0) | 0 | 0 |
| crash (fault) | an exception of a crashing class (`--fuzz-crash-exc`), a fetch fault loop | faulting pc | 20 |
| crash (oracle) | a `--fuzz-crash-pc` reached, a `--fuzz-crash-irq` interrupt taken, a reset during the iteration, a non-zero halt, a debug trap | the pc (0 for a reset, the status for a halt, the crashing lines for an interrupt) | 21 |
| hang | deadlock or livelock | the stop reason (`uam_stop_reason`: `UAM_STOP_DEADLOCK` or `UAM_STOP_LIVELOCK`, numbered as in `uam/exec.h` of this ember) | 22 |
| timeout | `--fuzz-iter-insns` (default 1e7) exhausted | 0 | 23 |
| rejected | the input route refused the input (`UAM_ERR_REJECTED`); nothing ran | 0 | 24 |

Exception classes: `fetch`, `load`, `store` (bus faults), `align`, `xlate` (TLB misses and
protection: firmware with a TLB takes these routinely), `trap` (illegal instructions, but also
system calls and breakpoints), `sysreg`, `all`, `none`. The default is
`fetch,load,store,align`. Exceptions outside the policy are delivered to the firmware as
usual; pointing `--fuzz-crash-pc` at its fatal-error handler catches those that end there.

`--fuzz-crash-irq N` (repeatable, 0..63) makes taking an interrupt from line N a crash: a
watchdog's warning interrupt, say. Lines are numbered as the ISA reports them: on RV32 the
`mcause` interrupt code (7 machine timer, 11 machine external), on MIPS32 the Cause.IP bit
(0..7) that was pending and enabled, on ARMv7 0 for IRQ and 1 for FIQ (on `arm-ref` every
device interrupt arrives as IRQ, through the GIC), on PowerPC the external interrupt sources
0..7 that were high (on `ppc-ref`: 0 the UART, 1 the IPI doorbell), 8 soft reset and 9 machine
check; the decrementer has no line. The interrupt is reported once the CPU has taken it
(`uam_exec_set_irq_hook`, `uam_interrupt`: the interrupted pc, the handler pc, the lines); the
detail is the crashing lines as a mask. Without the option no interrupt hook is installed.

### Replay (`--fuzz-replay FILE|DIR`)

Runs each input once (a directory in name order) and prints one line per input:

```
build/uam-run -b rv32-ref --cov-blocks --fuzz-begin 0x80000336 --fuzz-crash-pc 0x80000240 \
    --fuzz-input uart0 --fuzz-replay crashes/ build/firmware/fuzztarget.elf
replay: crashes//2-store: crash-fault pc=0x800002b2 (store-fault cause 0x0 tval 0x40000000) insns=95 buckets=17
```

(The pcs are `fuzz_iteration` and `fuzz_panic` from `readelf -s`; they move with the
toolchain.) The exit status is the most severe verdict of any input: a crash (the first
one's kind), else a hang, else a timeout, else a rejected input (24), else 0. So a directory of crashes fails however
it is ordered and whatever other files it holds. Every regular file runs as an input, except
a fuzzer's notes saved beside an input: a runner extension can name their suffixes, and a
file X plus that suffix is then skipped when X is in the directory too. With suzzer
configured that is `.info`, so a brain's `crashes/` or `hangs/` directory replays its inputs
and not their `.info` sidecars (`uam_fuzz_replay_create_ex` in a program). `--cov-out FILE` writes the last input's map. `tests/fuzz/fuzztarget.h`
is a small target with planted bugs used by `make fuzz-test`.

### In a program

```c
uam_fuzz_config cfg = { .begin_pc = BEGIN, .crash_pcs = &panic_pc, .n_crash_pcs = 1,
                        .crash_exc = UAM_FUZZ_EXC_DEFAULT };
uam_fuzz *f = uam_fuzz_create(&m, &cfg, err, sizeof(err));
uam_fuzz_set_block_coverage(f);                  /* or uam_fuzz_set_sites */
uam_fuzz_select_route(f, "uart0", err, sizeof(err));
uam_fuzz_boot(f, &r);
for (each input)
    uam_fuzz_run_one(f, data, len, &r);          /* r.verdict, r.detail, uam_fuzz_map(f) */
```

or hand the loop to a frontend (`uam_fuzz_run(f, &frontend, ctx)`: `hello`, an INIT report
with id 0, then one report per input, `bye`).

## Limitations

- Sites are keyed on the virtual pc. Code reached through both kseg0 and kseg1 needs two
  sites, and TLB-mapped code (ASIDs) is not distinguished.
- One hart. Coverage state is per hart, so more harts need no format change.
- Interrupts are reported only when taken, and only as a crash (`--fuzz-crash-irq`); an
  interrupt line that is raised but masked is invisible to the harness.
- `uam_fuzz_abort` (SIGINT in the runner) ends a campaign at the next instruction boundary.

## suzzer

ember reads suzzer's files through its published formats only. The options below exist when
the build has a suzzer checkout (`make SUZZER_DIR=path`, or `make suzzer` to clone one into
`./suzzer`); [`suzzer.md`](suzzer.md) explains the setup. `fuzz/suzzer/` is the only ember
code that knows suzzer exists. Supported format versions: LCV3 3, LCVB1 1, LCVS 1, SZR1 1.

### Site lists and boundaries

```
build/uam-run -b mips-ref --cov-sites fw.lcv3 --cov-boundary fw.lcvb --cov-strict \
    --fuzz-input uart0 --fuzz-crash-pc 0x80102222 --fuzz-replay crashes/ fw.elf
```

| Option | |
|---|---|
| `--cov-sites LCV3` | coverage sites from a patch list; record *i* is site id *i* |
| `--cov-module NAME=BASE` | a module's load base (repeatable). Otherwise the list's module base table (suzzer S2), then, for module 0, the image's link base (lowest ELF `PT_LOAD` vaddr, or `--raw-base`) |
| `--cov-scheme site\|pc`, `--cov-ngram N`, `--cov-rotate` | libcov's model, N-gram length (1..8) and N-gram rotation; default site, N=2, no rotation, like libcov. For a map comparable with a device, match its libcov build (`LIBCOV_NGRAM_N`, `LIBCOV_NGRAM_ROTATE`); suzzer's brain recommends N=3 with rotation for fuzzing |
| `--cov-mode leapfrog\|every` | which executions count. `leapfrog` (the default, libcov's default `LIBCOV_MODE`): a site counts again only after another site did, so loops count visits and no self-edge appears, exactly as on the device. `every`: each execution counts (finer counts, but maps no longer equal libcov's). libcov's `oneshot` sanity-check mode is not offered |
| `--cov-map-size N` | overrides libcov's map-size rule (site model: from the site count and N; hashed-PC: 64 KiB) |
| `--cov-strict` | each site's bytes in memory must equal the list's `orig[]`, checked at the first begin when fuzzing, else at the end of the run. Without it the check is only reported. A wrong base or byte order shows up here |
| `--cov-boundary LCVB1` | iteration boundaries instead of `--fuzz-begin`/`--fuzz-end`: any number of BEGIN markers (the snapshot is at the first one the boot reaches) and of END markers, all with the same depth filter. Depth-zero-only markers keep nested BEGIN/END arrivals (recursion) inside one iteration. With "any depth", ember ends the iteration at the first END reached, where libcov would publish each nested segment separately |

**Where the BEGIN marker sits.** ember snapshots at the BEGIN marker and delivers each input
there, so the marker must come before the firmware reads its input. suzzer's generators
pick a site strategy per architecture; on RV32 the default (`terminator`) puts a function's
site on its last instruction, which in the fuzz target is *after* the UART status read, so
every input would run as "no data". Generate both lists with `--strategy entry` (the MIPS
default anyway) and name the input handler: `gen_patchlist.py --strategy entry fw.elf fw.lcv3`
and `gen_boundary.py --strategy entry --patchlist fw.lcv3 fw.elf fw.lcvb handler:begin`.
ember accepts a marker that is not a patch-list site (it simply has no coverage of its own);
libcov would never fire such a marker, so keep the two lists from one generator run.

**ARM and Thumb.** ember names Thumb code by its address with bit 0 set (the ARM convention,
`uam/isa/arm.h`): `--fuzz-begin`, `--fuzz-end`, `--fuzz-crash-pc` and `--cov-blocks` maps use
pcs such as `0x10024d` for Thumb code at `0x10024c`, as a Thumb function symbol already gives
them. suzzer's lists instead give every site its even address and mark Thumb sites by their
2-byte length, so on `arm-ref` the importer sets bit 0 of each 2-byte site, and of each
boundary marker whose closest site at or below it is a Thumb site (a marker inside a Thumb
function). A Thumb marker needs `--cov-sites` next to `--cov-boundary` for that.

A site that is a branch delay slot cannot be instrumented. The run stops with an error naming
the patch-list record and module. This is a policy, not a technical limit: suzzer's generator
never places a site in a delay slot (libcov's trap-based sites cannot work there), so such a
site usually means a wrong base or a hand-made list. Two designs remain open for later, and
either would be fine:

- **Count them.** ember runs a branch and its delay slot as one unit, so a COV right after the
  delay slot's instruction marker would still count once per execution; the verifier's rule
  that COV follows only non-continuation markers would be relaxed for it.
- **Reject them, earlier.** Keep the policy, but detect delay-slot sites when the list is
  loaded (a small ISA hook, "is this pc a delay slot?") instead of when that code is first
  translated, which can be well into a run.

The bucket formula is reimplemented in `fuzz/suzzer/scheme.c` from suzzer's specification.
N>=2 uses the coverage state's history: depth N-1, with libcov's per-slot rotation amounts
when `--cov-rotate` is given. `test_suzzer` replays every vector in the checkout's `test/vectors` (site and pc, N=1,
2, 3, 4 and 8, rotation off and on, three map sizes) as guest code through `UAM_OP_COV` on
each backend, and requires suzzer's expected bitmaps byte for byte. Suzzer has no vectors
for N=5..7; the formula is the same for every N, but those lengths are untested. `tests/suzzer/suzzer-check.sh` runs the formats end to end with lists written by
`tests/suzzer/mk_lists.py`.

### The brain (`--fuzz-szr HOST:PORT`)

ember plays `suzzer-relay`'s part: it dials `suzzer_brain.py` (retrying for a few seconds
while the brain starts), runs each INPUT through the harness and answers it, in lockstep,
with one COVERAGE or CRASH frame. It needs fuzzing boundaries (`--fuzz-begin` or
`--cov-boundary`) and replaces `--fuzz-replay`. ember connects first, boots to the begin
pc, and only then sends HELLO and the INIT map, so a long firmware boot is not spent between
HELLO and the first reply. The brain waits for HELLO up to its `--hello-timeout` (600 s in
suzzer versions that have it; older brains apply `--io-timeout`, 60 s by default, so raise
that for a boot that takes longer). Any coverage source works: with
`--cov-sites`, HELLO describes the scheme (site or hashed-PC, N, rotation) so the brain can
tell maps apart; with `--cov-blocks` it describes none. HELLO's arch is `rv32`, `mips32be`,
`mips32le`, `armv7`, `armv7be` or `ppc32`, and the boot map goes out first as an unsolicited INIT snapshot (id 0).

| Verdict | SZR1 reply | `status` |
|---|---|---|
| ok | COVERAGE, kind ITER | |
| crash (fault) | CRASH `GUEST_FAULT` (5) | the faulting pc, low 32 bits |
| crash (oracle) | CRASH `ORACLE` (6) | the detail, low 32 bits: the crash pc, a halt status, the interrupt lines |
| hang | CRASH `HANG` (7) | the stop reason (ember's `uam_stop_reason` number) |
| timeout | CRASH `TIMEOUT` (8) | 0 |
| rejected | CRASH `DELIVERY` (3) | 0 |

A malformed INPUT gets CRASH `MALFORMED` (4) and runs nothing; unknown frame types are
ignored. A BYE from the brain ends the run (exit 0); the brain closing the connection without
one is an error (exit 2), and so is an internal error of ember's, which sends BYE first. SIGINT
sends BYE in place of the pending reply and exits 130. TCP keepalive (probes after a minute
of silence, giving up after about two more) notices a brain host that vanished without
closing the connection. A brain that is still there but stops answering is waited for
unless `--fuzz-szr-timeout SEC` is given: then SEC seconds without a frame from the brain
(or without it taking one of ember's) ends the run with an error.

```
python3 suzzer/tools/suzzer_brain.py --listen 127.0.0.1:7000 --seeds seeds/ --out run/
build/uam-run -b rv32-ref --cov-blocks --fuzz-begin 0x80000336 --fuzz-crash-pc 0x80000240 \
    --fuzz-input uart0 --fuzz-iter-insns 200000 --fuzz-szr 127.0.0.1:7000 build/firmware/fuzztarget.elf
```

`make suzzer-test` checks the protocol frame by frame against a scripted brain
(`tests/szr/fake_brain.py`) and runs a session with suzzer's own brain from the checkout
(`tests/szr/brain-check.sh`: the planted crash is reported, saved with its reason,
minimized, and the session ends with BYE).

### Shared memory (`--cov-shm PATH`)

ember publishes the harness's maps in an LCVS region, as libcov does on a device, so
suzzer's region readers (`test/shm_bitmap.py`, ring readers) can watch a run. It is a side
channel next to any frontend (`--fuzz-replay` or `--fuzz-szr`) and needs fuzzing.

- The ring (`--cov-shm-ring N`, default 8 slots) gets the INIT map, then one ITER snapshot
  per ok iteration, written with the format's seqlock protocol. Crashed, hung and timed-out
  iterations are not published: on a device the process dies before it can publish those.
- The live bitmap is the union (saturating sums) of everything published, so `LIVE_EMPTY` is
  clear and `shm_bitmap.py stats` shows the campaign's coverage so far.
- The header carries the scheme flags (with `--cov-sites`; none for `--cov-blocks`),
  `ACTIVE`, and ember's pid and start time as the owner. A region another live process owns
  is refused, decided as libcov decides it: under an exclusive `flock`, so ember and libcov
  starting at once cannot both take it, with an owner alive only if its pid still has the
  recorded start time (a reused pid is not the owner). PATH follows `LIBCOV_SHM_PATH`: with a `/` after its first character it is a
  file, otherwise a shared-memory name under `/dev/shm`. The region stays after ember exits.

```
build/uam-run -b mips-ref --cov-sites fw.lcv3 --cov-boundary fw.lcvb --fuzz-input uart0 \
    --cov-shm /tmp/ember.lcvs --fuzz-szr 127.0.0.1:7000 fw.elf
python3 suzzer/test/shm_bitmap.py stats /tmp/ember.lcvs
```
