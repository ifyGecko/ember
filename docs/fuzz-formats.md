# Fuzz formats

The files and the protocol of ember's fuzz layer: the **site list**, the **boundary file**,
the **coverage region** and the **fuzz protocol**, and the **coverage bucket formula** their
maps follow. The C definitions are in [`fuzz/formats.h`](../fuzz/formats.h). The writers are
ember's own tools in `fuzz/tools/`; `fuzz/site_list.c` reads the two files,
`fuzz/cov_region.c` writes the region and `fuzz/frontend_net.c` speaks the protocol; [`fuzzing.md`](fuzzing.md#site-lists-boundary-files-and-the-brain)
explains how to use them.

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
| 16 | flags | u32 | the coverage scheme: bit 0 `SCHEME` (the other bits are valid; clear for block coverage), bit 1 `PC` (hashed-pc model), bit 2 `ROTATE`, bits 8..11 N |
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
| 1 HELLO | ember → brain, once | u16 version (1), u16 flags (the region's scheme bits; bit 14: compare logging is understood (OPTIONS bit 1, CMP_INPUT); bit 15: the target's data byte order is big-endian), u32 map_size, u16 arch_len (≤ 64), arch (the ISA's name, e.g. `rv32imac`, `mips32be`) |
| 2 INPUT | brain → ember | u64 id (non-zero), u32 len, data |
| 3 COVERAGE | ember → brain | u64 id, u8 kind (1 INIT, 2 ITER), u32 map_size, map |
| 4 CRASH | ember → brain | u64 id, u8 reason, u32 status |
| 5 BYE | either way | none |
| 6 OPTIONS | brain → ember, optional, after HELLO (normally once, before the first INPUT; each one replaces the last) | u32 flags: bit 0 SPARSE (reply with SPARSE instead of COVERAGE), bit 1 CMP_LOG (instrument the target's compares, for CMP_INPUT); later fields, if any, are ignored |
| 7 SPARSE | ember → brain | u64 id, u8 kind (1 INIT, 2 ITER), u64 insns (instructions the iteration ran; 0 for INIT), u32 map_size, u32 n, then n × (u32 index, u8 count), indices ascending |
| 8 CMP_INPUT | brain → ember | as INPUT; the run also logs what its compares saw |
| 9 CMPS | ember → brain, right before a CMP_INPUT's reply | u64 id, u32 n, u32 dropped, then n × (u32 site, u8 width (bits), u8 kind, u64 a, u64 b) |

**Lockstep.** After HELLO, ember sends the boot's map once, unsolicited, as COVERAGE id 0,
kind INIT. Every INPUT then gets exactly one reply with its id: COVERAGE (or SPARSE) kind
ITER if the input ended normally, CRASH otherwise. Unknown frame types are ignored.

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

**Parameters:** the model (`--cov-scheme site|pc`, default site), N (`--cov-ngram 1..8`,
default 2), rotation (`--cov-rotate`), and `map_size`, a power of two. Let
`mask = map_size − 1` and `W = max(N − 1, 1)`. The history `prev[0..W−1]` is all zeros at
the start of every iteration (and of the boot).

**One count** (u64 arithmetic wraps; `^` is XOR):

```
mix(v):  v ^= v >> 33;  v *= 0xff51afd7ed558ccd;  v ^= v >> 33

site model, site id s:        cur = s;   win = mix(s) & mask
pc model, offset off in m:    cur = mix(off ^ (m * 0x9E3779B97F4A7C15)) & mask;   win = cur
                              (off = pc − base[m], reduced to the address width)

idx = cur
if N ≥ 2: for j in 0..W−1: idx ^= rotate ? rot(prev[j], j) : prev[j]
map[idx & mask] = min(map[idx & mask] + 1, 255)
if N ≥ 2: prev[W−1..1] = prev[W−2..0];  prev[0] = site model ? win : win >> 1
```

**Rotation** with `bits = log2(map_size)`: `step = max(1, (bits − 1) / W)`; slot *j*
rotates by `r = 1 + j × step` (if `r ≥ bits`, `r = 1 + r mod (bits − 1)`), within the map
width. So the order of sites changes the bucket.

**Map size.** The pc model uses 64 KiB. The site model sizes the map from the site count *n*:
`F = 2` for N = 1, `16 << (N − 2)` otherwise; `size = min(next_pow2(max(n,1) × F), 2^24)`,
then at least 4 KiB and at least `next_pow2(n)`, so every site id has its own bucket.
No map is larger than 2 GiB. `--cov-map-size` overrides the rule. HELLO and the region
header carry the actual size.

**Which executions count.** In `--cov-mode leapfrog`, the default, a site counts again only
after another site has counted: a block looping on itself counts once per entry, and no
self-edge appears. `--cov-mode every` counts each execution. A boundary marker's own hit
counts before the map is published and the history is reset. A BEGIN's hit therefore lands
in the snapshot it closes (INIT, or the previous iteration in begin-only mode), and an
iteration's first count is the next site after the marker, with an empty history.

## Versioning

These formats are ember's. A change that a current reader would misread gets a new version
number in that format's header, and the readers refuse the old version with a message
naming the tool that writes the new one. New optional data goes into reserved fields only
together with a flag bit that says it is there. A change to the bucket formula changes
`tests/unit/cov_golden.txt` (`build/test_cov_formats --write-golden
tests/unit/cov_golden.txt` writes it anew) and makes every saved corpus's coverage
incomparable, so make one only on purpose.
