#!/usr/bin/env python3
"""fuzz_queue.py -- the campaign logic of ember's fuzz brain (fuzz/tools/fuzz_brain.py).

Kept apart from the protocol and the main loop so that it can be tested on its own
(tests/fuzz/brain_unit.py). Stdlib-only. docs/fuzz-ideas.md is the design.

  Coverage       folds an iteration's hit list ((index, count) pairs) into the campaign's
                 coverage, with AFL-style hit-count classes: an input is interesting when
                 it sets a bucket never hit, or reaches a count class that bucket never had.
                 O(buckets hit), never O(map size).
  path_id        a hash of a hit list (indices and count classes): "the same path".
  Entry          one queue entry and its statistics.
  Queue          the entries, path and edge frequencies, the favored set (culling), and
                 the power schedules that turn an entry into its energy.
  OpStats        which mutation operators found coverage, for adaptive weights.
  entry names    id:000123,src:000045,op:havoc,+cov / id:000124,sync:w2,src:000310 /
                 id:000000,orig:seedname -- lineage in the file name.
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import struct
from dataclasses import dataclass, field
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

# ---- hit-count classes ---------------------------------------------------------------

def _count_class(c: int) -> int:
    if c == 0:
        return 0
    if c <= 3:
        return {1: 1, 2: 2, 3: 4}[c]
    if c <= 7:
        return 8
    if c <= 15:
        return 16
    if c <= 31:
        return 32
    if c <= 127:
        return 64
    return 128


CLASS = bytes(_count_class(c) for c in range(256))

Hits = Sequence[Tuple[int, int]]          # (bucket index, count), ascending indices


def hits_from_map(bitmap: bytes) -> List[Tuple[int, int]]:
    """A whole map (a COVERAGE reply) as a hit list. O(map size): the fallback only."""
    return [(i, b) for i, b in enumerate(bitmap) if b]


def path_id(hits: Hits) -> bytes:
    """8 bytes naming the path: the hit buckets and their count classes."""
    h = hashlib.blake2b(digest_size=8)
    for i, c in hits:
        h.update(struct.pack("<IB", i, CLASS[c]))
    return h.digest()


class Coverage:
    """The campaign's coverage: for each bucket, the count classes seen so far."""

    def __init__(self, map_size: int):
        self.map_size = map_size
        self.virgin = bytearray(map_size)      # OR of the classes seen per bucket
        self.edges = 0                         # buckets ever hit

    def fold(self, hits: Hits) -> Tuple[int, int]:
        """Merge an iteration: (new buckets, new bucket/class pairs). Interesting if the
        second is non-zero (a new bucket is also a new class)."""
        new_b = new_c = 0
        v = self.virgin
        for i, c in hits:
            cl = CLASS[c]
            old = v[i]
            if not old:
                new_b += 1
            if not old & cl:
                v[i] = old | cl
                new_c += 1
        self.edges += new_b
        return new_b, new_c

    def would_be_new(self, hits: Hits) -> bool:
        v = self.virgin
        return any(not v[i] & CLASS[c] for i, c in hits)


# ---- queue entries -----------------------------------------------------------------

@dataclass
class Entry:
    id: int
    data: bytes
    name: str = ""
    parent: Optional[int] = None
    depth: int = 0
    insns: int = 0                 # instruction count of its run (the cost); 0: unknown
    buckets: List[int] = field(default_factory=list)   # indices it hits
    path: bytes = b""
    found_at: int = 0              # campaign cycle when it was found
    times_chosen: int = 0          # fuzz_level
    favored: bool = False
    trimmed: bool = False
    calibrated: bool = False
    new_cov: bool = False          # brought new buckets, not only new count classes

    @property
    def size(self) -> int:
        return len(self.data)

    def cost(self) -> int:
        return max(1, self.insns) * max(1, self.size)

    def state(self) -> dict:
        """What a resumed campaign keeps (the rest is measured again)."""
        return {"parent": self.parent, "depth": self.depth, "found_at": self.found_at,
                "times_chosen": self.times_chosen, "trimmed": self.trimmed,
                "new_cov": self.new_cov}


# ---- names ---------------------------------------------------------------------------

_NAME_ID = re.compile(r"^id:(\d+)(?:,|$)")


def entry_name(eid: int, src: Optional[int] = None, op: Optional[str] = None,
               new_cov: bool = False, sync: Optional[str] = None,
               orig: Optional[str] = None) -> str:
    parts = ["id:%06d" % eid]
    if orig is not None:
        parts.append("orig:" + re.sub(r"[^A-Za-z0-9._-]", "_", orig)[:40])
    if sync is not None:
        parts.append("sync:" + sync)
    if src is not None:
        parts.append("src:%06d" % src)
    if op:
        parts.append("op:" + op)
    if new_cov:
        parts.append("+cov")
    return ",".join(parts)


def name_id(name: str) -> Optional[int]:
    m = _NAME_ID.match(name)
    return int(m.group(1)) if m else None


# ---- schedules -----------------------------------------------------------------------

SCHEDULES = ("fast", "explore", "exploit", "coe", "rare", "lin", "quad", "seek", "mmopt")
MAX_FACTOR = 32           # the most a schedule multiplies the performance score by
HAVOC_BASE = 128          # mutations for a score of 100
SCORE_MAX = 1600          # the highest performance score after the factor


def _next_pow2(n: int) -> int:
    p = 1
    while p < n:
        p <<= 1
    return p


class Queue:
    """The campaign's queue: entries, frequencies, the favored set and the schedules."""

    def __init__(self, schedule: str = "fast", track_edges: Optional[bool] = None):
        if schedule not in SCHEDULES:
            raise ValueError("unknown schedule %r (%s)" % (schedule, ", ".join(SCHEDULES)))
        self.schedule = schedule
        self.entries: List[Entry] = []
        self.path_freq: Dict[bytes, int] = {}      # executions per path (AFLFast's n_fuzz)
        # executions per bucket: only the rare schedule needs it (it costs a dict update
        # per hit bucket per execution)
        self.track_edges = schedule == "rare" if track_edges is None else track_edges
        self.edge_freq: Dict[int, int] = {}
        self.top_rated: Dict[int, Entry] = {}      # bucket -> cheapest entry hitting it
        self.dirty = False                         # top_rated changed: cull again
        self.pending_favored = 0                   # favored entries never chosen
        self.cycle = 0

    # -- bookkeeping per execution ----------------------------------------------------
    def note_execution(self, hits: Hits, pid: Optional[bytes] = None) -> bytes:
        pid = pid if pid is not None else path_id(hits)
        self.path_freq[pid] = self.path_freq.get(pid, 0) + 1
        if self.track_edges:
            ef = self.edge_freq
            for i, _ in hits:
                ef[i] = ef.get(i, 0) + 1
        return pid

    def add(self, e: Entry):
        self.entries.append(e)
        cost = e.cost()
        for i in e.buckets:
            cur = self.top_rated.get(i)
            if cur is None or cost < cur.cost():
                self.top_rated[i] = e
                self.dirty = True

    def rescore(self, e: Entry):
        """An entry's cost changed (trimmed): it may now be top-rated for its buckets."""
        cost = e.cost()
        for i in e.buckets:
            cur = self.top_rated.get(i)
            if cur is None or (cur is not e and cost < cur.cost()):
                self.top_rated[i] = e
                self.dirty = True

    def cull(self):
        """The favored set: walk the buckets; an uncovered one brings in its top-rated
        entry, whose buckets are then all covered (AFL's cull_queue)."""
        if not self.dirty:
            return
        self.dirty = False
        for e in self.entries:
            e.favored = False
        covered = set()
        for i, e in self.top_rated.items():
            if i in covered:
                continue
            e.favored = True
            covered.update(e.buckets)
        self.pending_favored = sum(1 for e in self.entries if e.favored and not e.times_chosen)

    def favored_count(self) -> int:
        return sum(1 for e in self.entries if e.favored)

    # -- skipping ---------------------------------------------------------------------
    def skip(self, e: Entry, rnd: float) -> bool:
        """AFL's rule: while favored entries wait, skip 99% of the others; otherwise
        95% of non-favored entries already fuzzed and 75% of new ones. rnd in [0, 1)."""
        if e.favored:
            return False
        if self.pending_favored:
            return rnd < 0.99
        return rnd < (0.95 if e.times_chosen else 0.75)

    # -- energy -----------------------------------------------------------------------
    def averages(self) -> Tuple[float, float]:
        n = len(self.entries) or 1
        return (sum(max(1, e.insns) for e in self.entries) / n,
                sum(len(e.buckets) for e in self.entries) / n)

    def perf_score(self, e: Entry, avg_insns: float, avg_buckets: float) -> float:
        """AFL's performance score: cheaper and wider than average scores higher, late
        and deep entries get a bonus. seek leaves out the cost term (every entry scores
        as if it ran in average time); mmopt adds a bonus for the deepest lineages."""
        cost = max(1, e.insns)
        if self.schedule == "seek":
            score = 100.0
        elif cost * 0.1 > avg_insns:
            score = 10.0
        elif cost * 0.25 > avg_insns:
            score = 25.0
        elif cost * 0.5 > avg_insns:
            score = 50.0
        elif cost * 0.75 > avg_insns:
            score = 75.0
        elif cost * 4 < avg_insns:
            score = 300.0
        elif cost * 3 < avg_insns:
            score = 200.0
        elif cost * 2 < avg_insns:
            score = 150.0
        else:
            score = 100.0
        b = len(e.buckets)
        if b * 0.3 > avg_buckets:
            score *= 3
        elif b * 0.5 > avg_buckets:
            score *= 2
        elif b * 0.75 > avg_buckets:
            score *= 1.5
        elif b * 3 < avg_buckets:
            score *= 0.25
        elif b * 2 < avg_buckets:
            score *= 0.5
        elif b * 1.5 < avg_buckets:
            score *= 0.75
        if e.found_at > 0 and e.times_chosen < 2:    # found late: catch up
            score *= 2
        d = e.depth
        score *= 1 if d <= 3 else 2 if d <= 7 else 3 if d <= 13 else 4 if d <= 25 else 5
        if self.schedule == "mmopt":
            # the newest discoveries: entries within a few generations of the deepest
            gap = self.max_depth() - d
            score *= 2 if gap < 3 else 1.5 if gap < 5 else 1
        return score

    def max_depth(self) -> int:
        return max((x.depth for x in self.entries), default=0)

    def factor(self, e: Entry) -> float:
        """The schedule's multiplier (0: skip this entry for now)."""
        s = self.schedule
        if s in ("explore", "seek", "mmopt"):
            return 1.0
        if s == "exploit":
            return float(MAX_FACTOR)
        freq = max(1, self.path_freq.get(e.path, 1))
        lvl = e.times_chosen
        if s in ("lin", "quad"):
            # AFL++'s fuzz_level / (freq + 1) and fuzz_level^2 / (freq + 1); the level
            # counts this pick, so an entry never chosen is not starved at 0
            n = lvl + 1
            f = (n if s == "lin" else n * n) / (freq + 1)
            return min(float(MAX_FACTOR), max(f, 1.0 / MAX_FACTOR))
        fast = ((1 << lvl) / freq) if lvl < 16 else MAX_FACTOR / _next_pow2(freq)
        fast = min(float(MAX_FACTOR), fast)
        if s == "fast":
            return max(fast, 1.0 / MAX_FACTOR)
        if s == "coe":
            freqs = [self.path_freq.get(x.path, 1) for x in self.entries]
            mean = sum(freqs) / (len(freqs) or 1)
            if freq > mean:
                return 0.0
            return max(fast, 1.0 / MAX_FACTOR)
        # rare: entries that hit the rarest buckets
        ef = self.edge_freq
        if not e.buckets or not ef:
            return 1.0
        rarest = min(ef.get(i, 0) for i in e.buckets)
        floor = min(ef.values())
        threshold = _next_pow2(max(1, floor))
        if rarest <= threshold:
            return float(MAX_FACTOR) / max(1, (rarest.bit_length()))
        return 0.25

    def energy(self, e: Entry) -> int:
        """Mutations to run from e this time round (0: skip it for now)."""
        avg_i, avg_b = self.averages()
        f = self.factor(e)
        if f <= 0:
            return 0
        score = min(SCORE_MAX, self.perf_score(e, avg_i, avg_b) * f)
        return max(1, int(HAVOC_BASE * score / 100))


# ---- adaptive operator weights -------------------------------------------------------

class OpStats:
    """Per mutation operator: how often it was used, and in children that found coverage.
    `weights(defaults)` moves weight toward operators that pay off, with a floor."""

    def __init__(self):
        self.used: Dict[int, int] = {}
        self.found: Dict[int, int] = {}

    def note(self, ops: Iterable[int], interesting: bool):
        for op in set(int(o) for o in ops):
            self.used[op] = self.used.get(op, 0) + 1
            if interesting:
                self.found[op] = self.found.get(op, 0) + 1

    def weights(self, defaults: Dict[int, int], min_uses: int = 50) -> Dict[int, int]:
        rates = {op: self.found.get(op, 0) / u for op, u in self.used.items() if u >= min_uses}
        if not rates:
            return dict(defaults)
        mean = sum(rates.values()) / len(rates)
        out = {}
        for op, w in defaults.items():
            if w <= 0 or op not in rates or mean <= 0:
                out[op] = w
                continue
            scale = 0.5 + 1.5 * rates[op] / mean          # 0.5x .. (rate-proportional)
            out[op] = max(1, min(4 * w, int(round(w * scale))))
        return out


# ---- persistence ---------------------------------------------------------------------

def save_entry(qdir: str, e: Entry):
    """Write the entry's input atomically (other workers may be reading the directory)."""
    tmp = os.path.join(qdir, ".tmp-" + e.name)
    with open(tmp, "wb") as f:
        f.write(e.data)
    os.replace(tmp, os.path.join(qdir, e.name))
    save_state(qdir, e)


def save_state(qdir: str, e: Entry):
    sdir = os.path.join(qdir, ".state")
    os.makedirs(sdir, exist_ok=True)
    with open(os.path.join(sdir, e.name + ".json"), "w") as f:
        json.dump(e.state(), f)


def load_queue(qdir: str) -> List[Entry]:
    """The entries saved in qdir, by id, with their saved state."""
    out = []
    if not os.path.isdir(qdir):
        return out
    for name in os.listdir(qdir):
        eid = name_id(name)
        p = os.path.join(qdir, name)
        if eid is None or not os.path.isfile(p):
            continue
        with open(p, "rb") as f:
            e = Entry(id=eid, data=f.read(), name=name)
        try:
            with open(os.path.join(qdir, ".state", name + ".json")) as f:
                st = json.load(f)
            for k in ("parent", "depth", "found_at", "times_chosen", "trimmed", "new_cov"):
                if k in st:
                    setattr(e, k, st[k])
        except (OSError, ValueError):
            pass
        out.append(e)
    out.sort(key=lambda x: x.id)
    return out


# ---- sync bookkeeping ----------------------------------------------------------------

def peers(sync_dir: str, me: str) -> List[str]:
    """The other workers in a sync directory (each a directory with a queue/)."""
    if not os.path.isdir(sync_dir):
        return []
    return sorted(n for n in os.listdir(sync_dir)
                  if n != me and os.path.isdir(os.path.join(sync_dir, n, "queue")))


def synced_upto(out_dir: str, peer: str) -> int:
    try:
        with open(os.path.join(out_dir, ".synced", peer)) as f:
            return int(f.read().strip() or -1)
    except (OSError, ValueError):
        return -1


def set_synced(out_dir: str, peer: str, last_id: int):
    d = os.path.join(out_dir, ".synced")
    os.makedirs(d, exist_ok=True)
    tmp = os.path.join(d, peer + ".tmp")
    with open(tmp, "w") as f:
        f.write("%d\n" % last_id)
    os.replace(tmp, os.path.join(d, peer))


def new_peer_inputs(sync_dir: str, peer: str, after: int) -> Tuple[List[Tuple[int, str]], int]:
    """(id, path) of a peer's queue entries above `after`, oldest first, and the highest id
    looked at (to record in .synced/). An entry the peer itself imported (sync:) is left
    out: it came from a worker this one reads directly."""
    q = os.path.join(sync_dir, peer, "queue")
    out = []
    top = after
    for name in os.listdir(q):
        eid = name_id(name)
        if eid is None or eid <= after:
            continue
        p = os.path.join(q, name)
        if not os.path.isfile(p):
            continue
        top = max(top, eid)
        if ",sync:" not in name:
            out.append((eid, p))
    out.sort()
    return out, top
