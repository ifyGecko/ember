#!/usr/bin/env python3
"""brain_unit.py - unit tests of the fuzz brain's campaign logic (fuzz/tools/fuzz_queue.py),
on synthetic data: no ember, no sockets.

  - hit-count classes (ember keeps the coverage record: tests/fuzz/fake_brain.py);
  - path ids (same hits, same path; a count class change, a new path);
  - the favored set (the cheapest and the longest entry per bucket, greedy covers) and
    AFL's skip rule; an entry of unknown cost ranking last and scoring as average;
  - a stage's run limit shrinking with an entry's cost (the compare-guided stage);
  - every schedule's factor and energy behaving as specified;
  - adaptive operator weights (toward the operators that find coverage, with a floor);
  - entry names, saving and resuming a queue with its state;
  - sync bookkeeping (a peer's new entries, its own imports left out, .synced/);
  - crash triage (fuzz/tools/fuzz_crashes.py): the first crash is minimized at once, the
    others later within a share of the runs (most new code among crashes first), and a
    crash another worker holds minimized is left as found.
"""
import os
import random
import sys
import tempfile

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "fuzz", "tools"))
import fuzz_crashes as fc  # noqa: E402
import fuzz_queue as fq  # noqa: E402

failures = []


def check(cond, what):
    if not cond:
        failures.append(what)


def entry(eid, buckets, insns=100, data=b"x", path=None, chosen=0, depth=0, found_at=0):
    return fq.Entry(id=eid, data=data, insns=insns, buckets=list(buckets),
                    path=path if path is not None else bytes([eid]) * 8,
                    times_chosen=chosen, depth=depth, found_at=found_at)


def test_classes_and_fold():
    check([fq.CLASS[c] for c in (0, 1, 2, 3, 4, 7, 8, 15, 16, 31, 32, 127, 128, 255)] ==
          [0, 1, 2, 4, 8, 8, 16, 16, 32, 32, 64, 64, 128, 128], "count classes")
    check(fq.hits_from_map(b"\0\x02\0\x01") == [(1, 2), (3, 1)], "a whole map as hits")


def test_path_id():
    a = fq.path_id([(1, 1), (2, 5)])
    check(a == fq.path_id([(1, 1), (2, 6)]), "counts within one class: the same path")
    check(a != fq.path_id([(1, 1), (2, 9)]), "another class: another path")
    check(a != fq.path_id([(1, 1)]), "fewer buckets: another path")
    # ember names paths the same way (fuzz/formats.h; fake_brain.py checks it against ember)
    check(fq.path_hash([(1, 1), (2, 5)]) == 0xb59d16a3494e612a and fq.path_hash([]) == 0x0
          and fq.path_hash([(2, 5), (1, 1)]) == fq.path_hash([(1, 1), (2, 5)]),
          "path_hash: mix of the sum of mixed (index, class) terms, order-independent")
    check(a == fq.path_hash([(1, 1), (2, 5)]).to_bytes(8, "big"), "path_id is path_hash, big-endian")


def test_favored_and_skip():
    q = fq.Queue("explore")
    big = entry(0, [1, 2, 3], insns=1000, data=b"x" * 50)
    small = entry(1, [1, 2], insns=10, data=b"y")
    other = entry(2, [3], insns=500, data=b"z" * 5)
    mid = entry(3, [1, 2, 3], insns=800, data=b"m" * 20)
    for e in (big, small, other, mid):
        q.add(e)
    q.cull()
    check(small.favored and other.favored,
          "favored: the cheapest entry per bucket, a greedy cover")
    check(big.favored and not mid.favored,
          "favored: and the longest entry per bucket, a second cover")
    check(q.pending_favored == 3, "the favored entries are pending")
    check(not q.skip(small, 0.999), "a favored entry is never skipped")
    check(q.skip(mid, 0.98) and not q.skip(mid, 0.995), "99% skipped while favored ones wait")
    q.pending_favored = 0
    check(q.skip(mid, 0.7) and not q.skip(mid, 0.8), "75% of new non-favored entries")
    mid.times_chosen = 1
    check(q.skip(mid, 0.9) and not q.skip(mid, 0.96), "95% of fuzzed non-favored entries")
    small.insns = 100000                       # trimmed elsewhere would lower it; here it grows
    big.insns, big.data = 1, b"q"              # trimmed to one byte
    q.rescore(big)
    q.cull()
    check(big.favored, "a cheaper entry after rescore becomes favored")
    check(mid.favored, "and the next longest takes the place of one that got shorter")


def test_unknown_cost():
    # An entry whose instruction count is unknown (0) is not the cheapest one.
    q = fq.Queue("explore")
    slow = entry(0, [1, 2], insns=0, data=b"s")            # unknown, one byte
    known = entry(1, [1, 2], insns=5000, data=b"k" * 40)   # counted, and the longest
    for e in (slow, known):
        q.add(e)
    q.cull()
    check(known.favored and not slow.favored,
          "favored: an unknown cost ranks after every known one")
    u2 = entry(2, [7], insns=0, data=b"uu")
    u3 = entry(3, [7], insns=0, data=b"u")
    q.add(u2)
    q.add(u3)
    check(q.top_rated[7] is u3, "among unknown costs the shorter entry is the cheaper")
    check(q.top_rated[1] is known, "a later unknown entry does not displace a known one")
    # scoring: unknown counts as average, and stays out of the average
    check(q.averages()[0] == 5000, "the average cost is over the entries with a count")
    avg_i, avg_b = q.averages()
    same = entry(9, [1, 2], insns=5000)
    check(q.perf_score(slow, avg_i, avg_b) == q.perf_score(same, avg_i, avg_b),
          "perf score: an unknown cost scores as an average one (not as the cheapest)")
    # the count arrives (trim's run of the entry): it competes on its real cost
    slow.insns = 10
    q.rescore(slow)
    q.cull()
    check(q.top_rated[1] is slow and slow.favored, "once counted, a cheap entry is top-rated")
    empty = fq.Queue("explore")
    empty.add(entry(0, [1], insns=0))
    check(empty.averages()[0] == 1.0 and empty.energy(empty.entries[0]) > 0,
          "a queue with no known cost still schedules")


def test_scaled_limit():
    q = fq.Queue("explore")
    for i, n in enumerate((1000, 1000, 1000, 5000)):       # average 2000
        q.add(entry(i, [i], insns=n))
    a, _, _, slow = q.entries
    check(q.scaled_limit(a, 256, 16) == 256, "an entry cheaper than average: the whole limit")
    check(q.scaled_limit(slow, 256, 16) == 102, "2.5x the average cost: 1/2.5 of the limit")
    very = entry(8, [8], insns=10 ** 9)
    check(q.scaled_limit(very, 256, 16) == 16, "never under the floor")
    check(q.scaled_limit(very, 8, 16) == 8, "nor above the limit asked for")
    check(q.scaled_limit(entry(9, [9], insns=0), 256, 16) == 256,
          "an unknown cost counts as average")


def test_schedules():
    def queue(sched):
        q = fq.Queue(sched, track_edges=True)
        a = entry(0, [1, 2], path=b"A" * 8)
        b = entry(1, [3], path=b"B" * 8)
        q.add(a)
        q.add(b)
        for _ in range(100):
            q.note_execution([(1, 1), (2, 1)], b"A" * 8)
        q.note_execution([(3, 1)], b"B" * 8)
        return q, a, b

    q, a, b = queue("explore")
    check(q.factor(a) == q.factor(b) == 1.0, "explore: no factor")
    q, a, b = queue("exploit")
    check(q.factor(a) == fq.MAX_FACTOR, "exploit: the maximum factor")
    q, a, b = queue("fast")
    check(q.factor(b) > q.factor(a), "fast: the rarely exercised path gets more")
    fa = q.factor(a)
    a.times_chosen = 5
    check(q.factor(a) > fa, "fast: energy grows each time an entry is chosen")
    check(q.factor(a) <= fq.MAX_FACTOR, "fast: capped")
    q, a, b = queue("coe")
    check(q.factor(a) == 0.0 and q.factor(b) > 0, "coe: entries on common paths get nothing")
    check(q.energy(a) == 0, "coe: zero energy means skip")
    q, a, b = queue("rare")
    check(q.factor(b) > q.factor(a), "rare: the entry with the rarest bucket gets more")
    for s in fq.SCHEDULES:
        q, a, b = queue(s)
        for e in (a, b):
            n = q.energy(e)
            check(0 <= n <= fq.HAVOC_BASE * fq.SCORE_MAX // 100, "%s: energy %d in range" % (s, n))
    for s in ("lin", "quad"):
        q, a, b = queue(s)
        check(0 < q.factor(a) < q.factor(b), "%s: never chosen is not starved; rare paths get more" % s)
    q, a, b = queue("lin")
    f1 = q.factor(b)
    b.times_chosen = 3
    check(abs(q.factor(b) - 4 * f1) < 1e-9, "lin: linear in the times chosen")
    q2, a2, b2 = queue("quad")
    b2.times_chosen = 3
    check(abs(q2.factor(b2) - 16 * q2.factor(entry(9, [3], path=b"B" * 8))) < 1e-9,
          "quad: quadratic in the times chosen")
    check(q2.factor(b2) > q.factor(b), "quad grows faster than lin")
    b2.times_chosen = 1000
    check(q2.factor(b2) == fq.MAX_FACTOR, "quad: capped")
    q, a, b = queue("seek")
    check(q.factor(a) == q.factor(b) == 1.0, "seek: no factor")
    q = fq.Queue("seek")
    cheap, dear = entry(0, [1], insns=10), entry(1, [2], insns=10000)
    q.add(cheap)
    q.add(dear)
    check(q.energy(cheap) == q.energy(dear), "seek: the cost term is left out")
    q = fq.Queue("mmopt")
    old, mid, new = entry(0, [1], depth=1), entry(1, [2], depth=6), entry(2, [3], depth=9)
    for e in (old, mid, new):
        q.add(e)
    check(q.factor(old) == 1.0, "mmopt: no factor")
    ex = fq.Queue("explore")
    for e in (old, mid, new):
        ex.add(e)
    check(q.energy(new) == 2 * ex.energy(new) and q.energy(mid) == int(1.5 * ex.energy(mid))
          and q.energy(old) == ex.energy(old),
          "mmopt: x2 within 3 generations of the deepest, x1.5 within 5, else as explore")
    q = fq.Queue("explore")
    cheap, dear = entry(0, [1], insns=10), entry(1, [2], insns=10000)
    q.add(cheap)
    q.add(dear)
    check(q.energy(cheap) > q.energy(dear), "the performance score favors cheap entries")
    deep = entry(2, [3], insns=10, depth=20)
    q.add(deep)
    check(q.energy(deep) > q.energy(cheap), "the performance score favors deep entries")
    try:
        fq.Queue("nope")
        check(False, "an unknown schedule is refused")
    except ValueError:
        pass
    check(fq.Queue("rare").track_edges and not fq.Queue("fast").track_edges,
          "only rare pays for per-bucket frequencies")


def test_opstats():
    s = fq.OpStats()
    for _ in range(100):
        s.note([1, 2], interesting=False)
    for _ in range(20):
        s.note([1], interesting=True)
    s.note([3], interesting=True)              # too few uses to judge
    w = s.weights({1: 4, 2: 4, 3: 4, 4: 0})
    check(w[1] > w[2] >= 1, "the operator that found coverage gains weight, the other keeps a floor")
    check(w[3] == 4, "an operator with too few uses keeps its weight")
    check(w[4] == 0, "a disabled operator stays disabled")
    check(all(v <= 16 for v in w.values()), "at most 4x the default")
    check(fq.OpStats().weights({1: 3}) == {1: 3}, "no data: the defaults")


def test_names_and_persistence():
    n = fq.entry_name(12, src=3, op="havoc", new_cov=True)
    check(n == "id:000012,src:000003,op:havoc,+cov", n)
    check(fq.entry_name(4, sync="w2", src=310) == "id:000004,sync:w2,src:000310", "sync name")
    check(fq.entry_name(0, orig="seed a/b") == "id:000000,orig:seed_a_b", "orig name")
    check(fq.name_id(n) == 12 and fq.name_id("crash_x") is None, "name_id")
    with tempfile.TemporaryDirectory() as d:
        e = entry(7, [1], data=b"hello", chosen=3, depth=2)
        e.name = fq.entry_name(7, src=1, op="havoc")
        e.trimmed = True
        fq.save_entry(d, e)
        open(os.path.join(d, "junk"), "w").close()
        got = fq.load_queue(d)
        check(len(got) == 1 and got[0].data == b"hello" and got[0].id == 7, "a queue reloads")
        check(got[0].times_chosen == 3 and got[0].depth == 2 and got[0].trimmed, "with its state")
        check(not any(x.startswith(".tmp-") for x in os.listdir(d)), "no temporary file left")


def test_sync_bookkeeping():
    with tempfile.TemporaryDirectory() as d:
        for w in ("main", "w1", "w2"):
            os.makedirs(os.path.join(d, w, "queue"))
        os.makedirs(os.path.join(d, "notaworker"))
        check(fq.peers(d, "main") == ["w1", "w2"], "peers: the other workers")
        q = os.path.join(d, "w1", "queue")
        for name in ("id:000000,orig:a", "id:000001,src:000000,op:havoc,+cov",
                     "id:000002,sync:main,src:000005"):
            open(os.path.join(q, name), "wb").close()
        todo, top = fq.new_peer_inputs(d, "w1", -1)
        check([eid for eid, _ in todo] == [0, 1] and top == 2,
              "a peer's own entries, not its imports; the highest id looked at")
        me = os.path.join(d, "main")
        check(fq.synced_upto(me, "w1") == -1, "nothing synced yet")
        fq.set_synced(me, "w1", top)
        check(fq.synced_upto(me, "w1") == 2, ".synced/ records it")
        todo, top = fq.new_peer_inputs(d, "w1", 2)
        check(todo == [] and top == 2, "nothing new after it")


def test_crash_triage():
    F, G = 1, 2                                          # FAULT, GUARD
    S = fc.Saved
    t = fc.Triage("w1")
    check(t.decide(F, 0x1010)[0] == fc.NOW, "the first crash: now")
    check(t.decide(F, 0x1020)[0] == fc.LATER and t.decide(G, 0x9000)[0] == fc.LATER,
          "every later one: later, wherever it is")
    t = fc.Triage("w1")
    check(t.decide(F, 0x3000, nested=True)[0] == fc.LATER and t.decide(F, 0x3004)[0] == fc.NOW,
          "met while minimizing: later, and it is not the first")
    # the queue: the most buckets new to crashes first, then the oldest; a share of the runs
    check(not t.due(1000, 0, 2.0) and len(t) == 0, "nothing queued: nothing due")
    t.defer("a", b"a", F, 1, fresh=2)
    t.defer("b", b"b", F, 2, fresh=9)
    t.defer("c", b"c", F, 3, fresh=2)
    t.defer("d", b"d", F, 4)
    check(len(t) == 4 and t.due(1000, 0, 2.0) and t.due(10000, 199, 2.0)
          and not t.due(10000, 200, 2.0), "due while queued runs stay under the share")
    check(not t.due(10**9, 0, 0.0), "a share of 0: never")
    check([t.pop()[0] for _ in range(4)] == ["b", "a", "c", "d"] and len(t) == 0,
          "the queue's order: most new buckets, then first come")
    # what the other workers hold
    t = fc.Triage("w1")
    check(t.decide(F, 0x1010, [S("main", F, 0x1010, True)])[0] == fc.PEER,
          "a peer holds it minimized: not minimized here")
    check(t.decide(F, 0x1020, [S("main", F, 0x1010, True)])[0] == fc.LATER,
          "the campaign already has a minimized crash: later")
    check(t.decide(F, 0x2010, [S("main", F, 0x2010, False)])[0] == fc.LATER,
          "a peer of a lower name saved it first: later")
    check(t.decide(G, 0x2010, [S("w2", G, 0x2010, False), S("w1", G, 0x2010, False)])[0] == fc.NOW,
          "a peer of a higher name saved it as found, and this worker's own copy: now")
    check(t.held_by(F, 0x2010, [S("main", F, 0x2010, False)]) is None
          and t.held_by(F, 0x2010, [S("main", F, 0x2010, True)]) == "main", "held_by")
    with tempfile.TemporaryDirectory() as d:
        for w, files in (("main", (("crash_r1_s4112_aa", "reason=1\nstatus=4112\nminimized=yes\n"),
                                   ("crash_r1_s4128_bb", "reason=1\nstatus=4128\nminimized=later\n"),
                                   ("crash_old", "reason=3\nstatus=7\nlen=1\nhex=00\n"),
                                   ("junk", "hello\n"))),
                         ("w2", ())):
            os.makedirs(os.path.join(d, w, "crashes"))
            for n, text in files:
                open(os.path.join(d, w, "crashes", n), "wb").close()
                with open(os.path.join(d, w, "crashes", n + ".info"), "w") as f:
                    f.write(text + "hex=" + "00" * 4096 + "\n")
        got = sorted(fc.saved_crashes([(w, os.path.join(d, w, "crashes"))
                                       for w in ("main", "w2", "gone")]))
        check(got == [S("main", 1, 4112, True), S("main", 1, 4128, False), S("main", 3, 7, False)],
              "saved crashes read back from .info files (one without minimized= counts as not)")
        check(fc.waiting(os.path.join(d, "main", "crashes")) == [("crash_r1_s4128_bb", b"", 1, 4128)]
              and fc.waiting(os.path.join(d, "gone")) == [],
              "the crashes an earlier session left for later")


def main():
    for t in (test_crash_triage, test_classes_and_fold, test_path_id, test_favored_and_skip, test_schedules,
              test_opstats, test_names_and_persistence, test_sync_bookkeeping):
        t()
    if failures:
        for f in failures:
            print("brain_unit: FAIL: " + f, file=sys.stderr)
        return 1
    print("brain_unit: ok (coverage fold, paths, favored set, unknown costs, stage limits, "
          "schedules, operator weights, queue files, sync bookkeeping, crash triage)")
    return 0


if __name__ == "__main__":
    random.seed(0)
    sys.exit(main())
