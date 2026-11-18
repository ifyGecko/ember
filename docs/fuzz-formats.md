# Fuzz formats

The files and the protocol of ember's fuzz layer: the **site list**, the **boundary file**,
the **coverage region** and the **fuzz protocol**, and the **coverage bucket formula** their
maps follow. The C definitions are in [`fuzz/formats.h`](../fuzz/formats.h). The writers are
ember's own tools in `fuzz/tools/`; `fuzz/site_list.c` reads the two files,
`fuzz/cov_region.c` writes the region and `fuzz/frontend_net.c` speaks the protocol; [`fuzzing.md`](fuzzing.md#site-lists-boundary-files-and-the-brain)
explains how to use them. The site list and the boundary file are opt-in: by default ember
counts its own blocks (the block model below) and takes begin pcs by address or symbol.

| Format | Magic | Version | Byte order | Written by | Read by |
|---|---|---|---|---|---|
| site list | `UAMS` | 1 | little-endian | `gen_sites.py` | `--cov-sites` |
| boundary file | `UAMB` | 1 | little-endian | `gen_boundaries.py` | `--cov-boundary` |
| coverage region | `UAMC` | 1 | the writer's | `--cov-shm` | `cov_region.py`, outside readers |
| fuzz protocol | `UAMF` | 1 (OPTIONS, SPARSE and compare logging are optional, negotiated) | big-endian | ember and `fuzz_brain.py` | each other |

Readers check the magic and the version, and refuse a file of any other version. For a
site list or a boundary file the error names the tool that makes a current one.

---

## Site list

```
[header]                 24 bytes
[count × record]         16 bytes each
[module name table]      at modules_offset, modules_size bytes
[module base table]      only with flag BASES: module_count × u64,
                         at modules_offset + modules_size rounded up to 8
```

**Header**

| Offset | Field | Type | |
|---|---|---|---|
| 0 | magic | 4 bytes | `UAMS` |
| 4 | version | u32 | 1 |
| 8 | count | u32 | records after the header |
| 12 | flags | u8 | bit 0 `BASES`: a module base table follows the names; other bits 0 |
| 13 | module_count | u8 | 1..255 |
| 14 | reserved | u16 | 0 |
| 16 | modules_offset | u32 | file offset of the name table |
| 20 | modules_size | u32 | bytes in the name table, at most 4096, last byte NUL |

**Record** (site *i* is record *i*: its index is its site id)

| Offset | Field | Type | |
|---|---|---|---|
| 0 | offset | u64 | the site's address minus its module's link base, without ISA state bits |
| 8 | module | u8 | index into the name table |
| 9 | len | u8 | 0..4: bytes of `orig` that `--cov-strict` checks (0: none) |
| 10 | flags | u8 | bit 0 `ALT_STATE`: the site is in the ISA's alternate state (ARM: Thumb; MIPS: MIPS16e or microMIPS); other bits 0 |
| 11 | reserved | u8 | 0 |
| 12 | orig | 4 bytes | the instruction's first `len` bytes as they sit in memory; the rest 0 |

The **name table** holds `module_count` non-empty NUL-terminated names, back to back. The
**base table** gives each module's link base. `--cov-module NAME=BASE` overrides it for one
module. With neither, module 0 takes the image's base (the lowest ELF `PT_LOAD` address, or
`--raw-base`) and any other module is an error.

A site's pc is `base[module] + offset`, reduced to the target's address width. With
`ALT_STATE`, ember also sets the ISA's pc state bits (`uam_isa_pc_state_bits`: bit 0 on ARM and on MIPS).

**Reading a site list.** A reader refuses the file if:
- the magic or the version is wrong, or the file is shorter than the header;
- an unknown header flag is set, or a reserved byte is not 0;
- the records don't fit in the file;
- there are no modules;
- the name table is empty, longer than 4096 bytes, runs past the end, doesn't end in NUL,
  or holds fewer than `module_count` names, or an empty one;
- the base table (when flagged) runs past the end;
- a record names a module that doesn't exist, has `len` over 4, has an unknown flag, or
  has a non-zero reserved byte or `orig` byte beyond `len`.

## Boundary file

`[header][count × record]`. The header is 16 bytes: `UAMB`, u32 version 1, u32 count, and
u32 reserved 0. Each record is 16 bytes:

| Offset | Field | Type | |
|---|---|---|---|
| 0 | offset | u64 | from module 0's link base (the base the site list uses), without state bits |
| 8 | kind | u8 | 0 BEGIN, 1 END |
| 9 | depth | u8 | 0 any depth, 1 depth zero only |
| 10 | flags | u8 | bit 0 `ALT_STATE`, as in the site list; other bits 0 |
| 11 | reserved | 5 bytes | 0 |

A file with an END marker is **paired**: each iteration runs from a BEGIN to an END. A file
with only BEGIN markers is **begin-only**: each BEGIN ends the iteration before it and starts
the next. Every marker in a file must have the same depth filter. With **depth zero**, a
BEGIN reached during an iteration opens a nested level and an END closes one, so recursion
stays inside one input. With **any depth**, the first END ends the iteration.

**Reading a boundary file.** A reader refuses the file if:
- the magic or the version is wrong, or the file is shorter than the header;
- the header's reserved field is not 0;
- the records don't fit in the file;
- a record has a kind or a depth above 1, an unknown flag, or a non-zero reserved byte;
- it has no BEGIN marker, or its markers don't all have the same depth filter.

ember takes its snapshot at the first BEGIN that the boot reaches. Every input starts from
that snapshot, whichever BEGIN marker it ran through. A marker need not be a site.

## Coverage region

Shared memory (or a file) in which ember publishes the harness's maps for readers outside
the process: `fuzz/tools/cov_region.py`, a status screen, a corpus tool. It is in the byte
order of the ember process that wrote it. A reader can tell the byte order from `version`,
which must read as 1.

```
offset 0         header, 64 bytes
bitmap_offset    the union of every published map (saturating sums), map_size bytes
ring_offset      ring_size slots of slot_stride bytes: slot header (24 bytes) + map
```

| Offset | Field | Type | |
|---|---|---|---|
| 0 | magic | 4 bytes | `UAMC`, written last |
| 4 | version | u16 | 1 |
| 6 | ring_size | u16 | slots, 2..65535 |
| 8 | map_size | u32 | a power of two |
| 12 | slot_stride | u32 | 24 + map_size |
| 16 | flags | u32 | the coverage scheme: bit 0 `SCHEME` (the other bits are valid; clear for plain block edges, which have no scheme), bit 1 `PC` (hashed-pc model), bit 2 `ROTATE`, bit 3 `BLOCK` (block model), bits 8..11 N |
| 20 | reserved | u32 | 0 |
| 24 | gen_current | u64 | atomic: the highest generation assigned |
| 32 | gen_init | u64 | the INIT snapshot's generation (0: none yet) |
| 40 | bitmap_offset | u64 | |
| 48 | ring_offset | u64 | |
| 56 | owner_pid | u32 | the writer's pid |
| 60 | owner_start | u32 | its start time, clock ticks since boot, low 32 bits |

Each **slot** header holds `slot_gen` (u64, atomic; 0 while it is being written), `ns` (u64,
`CLOCK_MONOTONIC` at publish), `kind` (u32: 1 INIT, the boot up to the first BEGIN; 2 ITER,
one input) and a reserved u32 0.

Generation *g* (starting at 1) lives in slot `g % ring_size`. ember publishes INIT, then each
iteration that ended normally. Crashes, hangs and timeouts are not published.

- **Writer:** `g = atomic_add_fetch(gen_current, 1)`; store `slot_gen = 0`, release fence;
  write `ns`, `kind` and the map; store `slot_gen = g` (release); add the map into the live
  bitmap.
- **Reader** for generation *g*: if `gen_current < g`, it isn't there yet. If
  `gen_current − g ≥ ring_size`, it was overwritten. Otherwise read `slot_gen` (acquire),
  copy the slot, fence, and read `slot_gen` again. Accept the copy only if both reads equal
  *g*; otherwise retry a bounded number of times.

**Ownership.** ember refuses a region that another live process owns. It decides under an
exclusive `flock` on the region, so two ember processes can't both claim it. An owner is
alive if its pid exists with the recorded start time, or, without `/proc`, if the pid
exists at all. A PATH with a `/` after its first character is a file. Any other PATH is a
POSIX shared-memory name: `/dev/shm` on Linux, at most 31 characters on macOS, where a
region of another size is made anew under the same name.

## Fuzz protocol

One TCP connection. The brain listens; ember (`--fuzz-net HOST:PORT`) dials it, retrying for
a few seconds while the brain starts. ember sends the 4-byte magic `UAMF` once, then both
sides exchange frames:

```
[u32 length][u8 type][payload]      length counts type + payload, at most 64 MiB
```

Every integer is big-endian, and maps are raw u8 counters.

| Type | Direction | Payload |
|---|---|---|
| 1 HELLO | ember → brain, once | u16 version (1), u16 flags (the region's scheme bits, bit 3 the block model; bit 12: stages in ember are understood (STAGE); bit 13: mutation in ember is understood (MUTATOR, CORPUS, HAVOC); bit 14: compare logging is understood (OPTIONS bit 1, CMP_INPUT); bit 15: the target's data byte order is big-endian), u32 map_size, u16 arch_len (≤ 64), arch (the ISA's name, e.g. `rv32imac`, `mips32be`) |
| 2 INPUT | brain → ember | u64 id (non-zero), u32 len, data |
| 3 COVERAGE | ember → brain | u64 id, u8 kind (1 INIT, 2 ITER), u32 map_size, map |
| 4 CRASH | ember → brain | u64 id, u8 reason, u32 status |
| 5 BYE | either way | none |
| 6 OPTIONS | brain → ember, optional, after HELLO (normally once, before the first INPUT; each one replaces the last) | u32 flags: bit 0 SPARSE (reply with SPARSE instead of COVERAGE), bit 1 CMP_LOG (instrument the target's compares, for CMP_INPUT), bit 2 JUDGE (a JUDGE frame before each reply; needs HELLO bit 13); then optionally u64 iter_insns (the per-input instruction budget from now on; 0 or absent: unchanged); later fields, if any, are ignored |
| 7 SPARSE | ember → brain | u64 id, u8 kind (1 INIT, 2 ITER), u64 insns (instructions the iteration ran; 0 for INIT), u32 map_size, u32 n, then n × (u32 index, u8 count), indices ascending |
| 8 CMP_INPUT | brain → ember | as INPUT; the run also logs what its compares saw |
| 9 CMPS | ember → brain, right before a CMP_INPUT's reply | u64 id, u32 n, u32 dropped, then n × (u32 site, u8 width (bits), u8 kind, u64 a, u64 b) |
| 10 MUTATOR | brain → ember | u32 max_size, u16 stack_lo, u16 stack_hi, u16 splice (per mille), u32 max_hang_finds, u16 n, then n × (u8 op, u16 weight), u32 n_tokens, then each token: u8 0, u16 len, bytes; or u8 1, u8 width (1, 2, 4, 8), u8 big_endian, u64 value |
| 11 MUTATOR_INFO | ember → brain, the reply to each MUTATOR | u8 status (0 accepted, 1 refused), u16 msg_len, msg (why, when refused), u16 n, then n × (u8 op, u16 default weight, u16 weight, u8 name_len, name) |
| 12 CORPUS | brain → ember | u32 entry, u32 len, data |
| 13 HAVOC | brain → ember | u64 first_id, u32 entry, u32 n, u64 seed, u32 flags (bit 0 EDGES) |
| 14 FIND | ember → brain, during a batch | u64 id, u8 reason (0: new coverage, else a CRASH reason), u32 status, u64 insns, u64 ops (bit k: operator k was applied), u32 new_buckets, u32 new_classes, u64 path (new_classes, path and insns 0 unless reason 0; new_buckets of a crash: the buckets no earlier crash had hit), u32 len, data, u32 n, then n × (u32 index, u8 count), indices ascending (n = 0 unless reason 0) |
| 15 HAVOC_DONE | ember → brain, closes a batch | u64 first_id, u8 status (0 ok, 1 no accepted MUTATOR, 2 no such CORPUS entry, 3 n (a stage's limit) is 0 or over its maximum (HAVOC: 2^20), first_id is 0, or the stage's kind is unknown, 4 input-to-state without compare logging), u32 execs, 7 × u32 (executions per reason: index 0 ended normally, 1-6 the CRASH reasons), u32 finds, u16 n, then n × (u8 op, u32 used, u32 found), u32 n_paths, then n × (u64 path, u32 execs), u32 n_edges (always there; 0 without EDGES), then n_edges × (u32 index, u32 execs) |
| 16 JUDGE | ember → brain, before an INPUT's or CMP_INPUT's reply (OPTIONS bit 2) | u64 id, u32 new_buckets, u32 new_classes, u64 path (a run that ended normally; else 0, but for a crash's new_buckets: the buckets no earlier crash had hit), u8 novel_crash |
| 17 PROGRESS | ember → brain, in a batch silent for 1 s | u64 first_id, u32 execs (runs so far) |
| 18 STAGE | brain → ember | u64 first_id, u32 entry, u8 kind (1 input-to-state, 2 trimming), u32 limit (inputs derived at most: 1 .. 4096 candidates, 1 .. 65536 cuts), u32 flags (bit 0 EDGES) |
| 19 STAGE_RESULT | ember → brain, before the HAVOC_DONE of a stage that ran | u64 first_id, u8 kind, then for input-to-state: u32 compares, u32 dropped, u32 candidates, u16 n, then n × (u8 width (bytes: 2, 4 or 8), u64 value); for trimming: u8 ran, u64 insns, u64 path, u32 len, data |
| 20 UNINIT | ember → brain, right before the input's usual reply or FIND (only from an ember started with `--fuzz-uninit`) | u64 id, u32 kinds (bit 0 verdict, 1 detail, 2 insns, 3 path, 4 output: what differed between the input's two runs), u64 detail (the pc of the first read of a page nothing had written, else kinds in the top byte and the pc the filled run ended at), u8 verdict (the filled run's: 0 ok, 1 fault, 2 oracle, 3 hang, 4 timeout, 5 rejected, 6 guard), u64 insns (the filled run's), u16 n, then n × (u64 pc, u64 paddr) (its first reads of pages nothing had written), u32 len, data (the input) |

**UNINIT** needs no negotiation: ember sends it only when started with `--fuzz-uninit`, and a
brain that does not know frame 20 ignores it (unknown frames are ignored on both sides). It
is sent once per distinct `detail`, with the input, so that the finding is saved whole, and it
does not replace the input's reply: the run's coverage is reported as usual. `docs/fuzzing.md`,
"Reads of memory nothing wrote", says what the two runs are and what the check cannot see.

**Lockstep.** After HELLO, ember sends the boot's map once, unsolicited, as COVERAGE id 0,
kind INIT. Every INPUT then gets exactly one reply with its id: COVERAGE (or SPARSE) kind
ITER if the input ended normally, CRASH otherwise. Every MUTATOR gets one MUTATOR_INFO, every
HAVOC any number of FIND and PROGRESS frames and then one HAVOC_DONE, every STAGE the same
with one STAGE_RESULT before the HAVOC_DONE; CORPUS and OPTIONS get none. Unknown frame types are ignored.

**Malformed frames.** A malformed INPUT gets CRASH MALFORMED. A CORPUS, HAVOC or STAGE
whose payload is short (or a CORPUS whose length field does not match) gets no answer:
ember ends the session with an error (exit 2). A MUTATOR is refused (MUTATOR_INFO status 1,
with the reason as text) when max_size is not 1 .. 32 MiB, the stack bounds are not
1 ≤ lo ≤ hi ≤ 1024, splice is over 1000, a byte token is not 1 .. 256 bytes, an integer
token's width is not 1, 2, 4 or 8, bytes follow the last token, or no operator is left
with a weight.

**SPARSE.** A brain that sends OPTIONS with bit 0 gets every later map as the list of its
set buckets instead of the whole map: a typical iteration sets a few hundred to a few
thousand buckets of a map of 4 KiB to 16 MiB, so the frame, and the brain's work on it,
follow what the iteration touched rather than the map size. A map with more than
map_size / 5 set buckets, for which the list would be larger, still goes as COVERAGE, so a
brain that asks for SPARSE handles both. The INIT map, sent before OPTIONS can arrive, is
always COVERAGE. An ember that predates OPTIONS ignores it and keeps sending COVERAGE; a
brain that sends no OPTIONS gets COVERAGE as before. `insns` is the iteration's cost, which
the brain's schedules use.

**Compare logging.** An ember whose HELLO has bit 14 instruments the target's compares
when OPTIONS has bit 1 (the compare pass, [`uam-isa.md`](uam-isa.md) §7.6). From then on a
CMP_INPUT runs like an INPUT, and its usual reply is preceded by one CMPS frame with the
same id: the distinct (site, a, b) its compares saw, in the order first seen, at most 8
per site and 1024 in all (`dropped` counts the records left out). `kind` says what was
compared: bit 0 EQ (an equality, or a difference tested for zero: a flags ISA's compare),
bit 1 ORDER, bit 2 SIGNED, bit 3 IMM (b is a constant of the code). A compare of `x ^ c1`,
`x + c1` or `x - c1` with a constant comes as `x` against the constant `x` must equal.
Compares with 0, of a value with itself and of two constants are left out. Without bit 1 a
CMP_INPUT still gets an empty CMPS (n = 0). A brain sends CMP_INPUT only to an ember that
announced it: an older one would ignore the frame and never answer. Plain INPUTs are not
logged; while compare logging is on they cost a load and an untaken branch per compare.

**Mutation in ember.** An ember whose HELLO has bit 13 mutates inputs itself with its
mutator ([`fuzzing.md`](fuzzing.md), "Mutation in ember"; `uam/fuzz_mutate.h`). MUTATOR
sets it up: the longest mutation, the range the stack depth is drawn from (powers of two,
log-uniform), the splice rate, how many hangs and timeouts the session reports as FIND,
operator weights (an operator not listed keeps its default; ids as MUTATOR_INFO lists
them) and the whole dictionary. Every MUTATOR is answered with MUTATOR_INFO, which also
carries ember's operator table; a refused one (a bad field, or no operator left enabled)
leaves the mutator unusable until a good one. CORPUS adds or replaces an entry: the
parents and the splice donors. HAVOC runs n mutations of one entry, seeded with `seed`
(the same seed, settings, corpus and earlier history give the same batch); they have ids
first_id .. first_id + n − 1 and run back to back with no frame from the brain in between.

**One record of what has been seen.** ember's campaign engine (`uam/fuzz_campaign.h`)
judges every run, of a batch or of an INPUT, against one record, and the brain keeps none.
A run that ended normally is new when it sets a bucket never hit or a count class a bucket
never had (the class of a count is 1, 2 or 4 for 1, 2 or 3, then 8 (4-7), 16 (8-15), 32
(16-31), 64 (32-127) and 128 (128-255)). A crash is novel when its (reason, status) is new
or when it ran code no earlier crash ran (a bucket no crash had hit), which tells two bugs
at one pc apart; how many such buckets it hit is the crash's new_buckets, a rough measure
of how unlike the earlier crashes its way was. A path is named by its set of (index, class) pairs, whatever order they
were hit in: `mix(sum of mix((index << 8 | class) + 1))` in 64-bit arithmetic, with the
coverage formula's `mix`. With OPTIONS bit 2 (JUDGE), every INPUT and CMP_INPUT reply is
preceded by a JUDGE frame (after CMPS, if there is one) with what the run added.

In a batch, ember sends FIND for a run with new coverage, for a novel crash, and for a hang
or timeout while the session's count is under MUTATOR's cap; REJECTED runs are only
counted. When a batch has sent nothing for a second it sends PROGRESS (a batch of long
timeouts is alive, not stuck: a brain's reply timeout starts anew). HAVOC_DONE then gives
the batch's totals: per operator, the runs it took part in and those of them that were
coverage FINDs (id 25 marks a splice); per path, the runs; per bucket, with EDGES, the
runs that hit it. A HAVOC that cannot run is answered
at once with HAVOC_DONE and its status. INPUT and CMP_INPUT keep working as before between
batches.

**Stages in ember.** An ember whose HELLO has bit 12 also derives inputs from a corpus
entry itself (`uam/fuzz_campaign.h`, "stages"). A STAGE is a batch like a HAVOC's: its
runs have the ids first_id .. first_id + limit, the entry's own run first; they are judged
against the same record, reported as FIND (with `ops` 0) on the same terms and counted in
the HAVOC_DONE that closes the stage. Each derived input goes through the route's fix-up.
Right before that HAVOC_DONE, a stage that ran sends one STAGE_RESULT.

- *Input-to-state* (kind 1; needs OPTIONS bit 1): the entry runs once with its compares
  logged, then every candidate derived from the log runs (`uam/fuzz_i2s.h`): for each
  record, one operand's bytes are looked for in the entry, in either byte order, at the
  compare's width and at the narrowest of 1, 2 and 4 bytes that holds both values, and the
  other operand is written there, plus and minus one as well for an ordered compare. A
  constant of the code (IMM) is never looked for, only written; a pattern of zero bytes is
  not looked for; the first 8 places of a pattern are tried; candidates are distinct.
  STAGE_RESULT gives the number of records, of records left out, and of candidates, and
  the code's constants with two or more significant bytes that the entry holds in neither
  byte order (at most 64 per stage): dictionary material.
- *Trimming* (kind 2): the entry runs once for its path; then chunks are cut out of it,
  from a sixteenth of its length (rounded up to a power of two) down to a 1024th, at least
  4 bytes, never the first chunk, and a cut stays when the run still ends normally on the
  same path and the input got shorter. Entries under 5 bytes are left alone. STAGE_RESULT
  gives whether the entry's own run ended normally (otherwise nothing was cut), and the
  entry as trimmed with its instruction count and path; when nothing went, that is the
  entry itself.

The session ends with BYE from either side. EOF before a BYE is an error. ember notices a
vanished brain host through TCP keepalive, and a brain that stops answering through
`--fuzz-net-timeout`.

**Crash reasons.** `(reason, status)` identifies a crash for deduplication.

| Reason | Verdict | status |
|---|---|---|
| 1 FAULT | a guest exception (`crash-fault`) | the faulting pc, low 32 bits |
| 2 GUARD | a redzone or `--guard` hit (`crash-guard`) | the pc of the access, low 32 bits |
| 3 ORACLE | a crash pc, halt status or interrupt (`crash-oracle`) | the detail, low 32 bits |
| 4 HANG | no forward progress (`hang`) | the stop reason |
| 5 TIMEOUT | the per-input budget ran out (`timeout`) | 0 |
| 6 REJECTED | the input route refused the input; nothing ran | 0 |
| 7 MALFORMED | a malformed INPUT frame; nothing ran | 0; id 0 if the payload was under 8 bytes |

## Coverage buckets

How the sequence of sites an iteration executes becomes map buckets. `fuzz/cov_scheme.c`
turns each site into the two operands of a `UAM_OP_COV` and the coverage state's history
does the rest ([`uam-isa.md`](uam-isa.md)). `tests/unit/test_cov_formats.c` pins the result
with golden maps.

**Parameters:** the model (the block model without a site list, which is the default when fuzzing; with one, `--cov-scheme site|pc`, default site, an option the block model refuses), N (`--cov-ngram 1..8`,
default 2), rotation (`--cov-rotate`), and `map_size`, a power of two. Let
`mask = map_size − 1` and `W = max(N − 1, 1)`. The history `prev[0..W−1]` is all zeros at
the start of every iteration (and of the boot).

**One count** (u64 arithmetic wraps; `^` is XOR):

```
mix(v):  v ^= v >> 33;  v *= 0xff51afd7ed558ccd;  v ^= v >> 33

site model, site id s:        cur = s;   win = mix(s) & mask
pc model, offset off in m:    cur = mix(off ^ (m * 0x9E3779B97F4A7C15)) & mask;   win = cur
                              (off = pc − base[m], reduced to the address width)
block model, block at pc:     cur = mix(pc) & mask (low 32 bits);   win = cur
                              (a site at every block start ember counts: fuzzing.md,
                              "Block coverage"; no list, no modules)

idx = cur
if N ≥ 2: for j in 0..W−1: idx ^= rotate ? rot(prev[j], j) : prev[j]
map[idx & mask] = min(map[idx & mask] + 1, 255)
if N ≥ 2: prev[W−1..1] = prev[W−2..0];  prev[0] = site model ? win : win >> 1
```

**Rotation** with `bits = log2(map_size)`: `step = max(1, (bits − 1) / W)`; slot *j*
rotates by `r = 1 + j × step` (if `r ≥ bits`, `r = 1 + r mod (bits − 1)`), within the map
width. So the order of sites changes the bucket.

**Map size.** The pc model uses 64 KiB, the block model 64 KiB for N ≤ 2 and four times as
much for each N above (at most 4 MiB). The site model sizes the map from the site count *n*:
`F = 2` for N = 1, `16 << (N − 2)` otherwise; `size = min(next_pow2(max(n,1) × F), 2^24)`,
then at least 4 KiB and at least `next_pow2(n)`, so every site id has its own bucket.
No map is larger than 2 GiB. `--cov-map-size` overrides the rule (a power of two up to 1 GiB; with `--fuzz-net` the map must also fit a frame, 64 MiB). HELLO and the region
header carry the actual size.

**Which executions count.** In `--cov-mode leapfrog`, the default for site lists, a site counts again only
after another site has counted: a block looping on itself counts once per entry, and no
self-edge appears. `--cov-mode every`, the default for the block model, counts each execution. A boundary marker's own hit
counts before the map is published and the history is reset. A BEGIN's hit therefore lands
in the snapshot it closes (INIT, or the previous iteration in begin-only mode), and an
iteration's first count is the next site after the marker, with an empty history.

## Versioning

These formats are ember's. A change that a current reader would misread gets a new version
number in that format's header, and the readers refuse the old version with a message
naming the tool that writes the new one. New optional data goes into reserved fields only
together with a flag bit that says it is there. A change to the bucket formula changes
`tests/unit/cov_golden.txt` (`build/tests/test_cov_formats --write-golden
tests/unit/cov_golden.txt` writes it anew) and makes every saved corpus's coverage
incomparable, so make one only on purpose.
