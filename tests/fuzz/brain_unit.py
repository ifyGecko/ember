#!/usr/bin/env python3
"""brain_unit.py - unit tests of the fuzz brain's campaign logic (fuzz/tools/fuzz_queue.py),
on synthetic data: no ember, no sockets.

  - hit-count classes and the coverage fold (new buckets, new classes, O(hits));
  - path ids (same hits, same path; a count class change, a new path);
  - the favored set (the cheapest entry per bucket, greedy cover) and AFL's skip rule;
  - every schedule's factor and energy behaving as specified;
  - adaptive operator weights (toward the operators that find coverage, with a floor);
  - entry names, saving and resuming a queue with its state;
  - sync bookkeeping (a peer's new entries, its own imports left out, .synced/).
"""
import os
import random
import sys
import tempfile

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "fuzz", "tools"))
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
    cov = fq.Coverage(1 << 16)
    check(cov.fold([(5, 1), (9, 1)]) == (2, 2), "two new buckets")
    check(cov.fold([(5, 1), (9, 1)]) == (0, 0), "nothing new the second time")
    check(cov.fold([(5, 3)]) == (0, 1), "a new count class of a known bucket")
    check(cov.fold([(5, 2), (70000 % (1 << 16), 200)]) == (1, 2), "class 2 and a new bucket")
    check(cov.edges == 3, "edges counts buckets ever hit")
    check(not cov.would_be_new([(5, 1), (9, 1)]) and cov.would_be_new([(9, 5)]), "would_be_new")
    check(fq.hits_from_map(b"\0\x02\0\x01") == [(1, 2), (3, 1)], "a whole map as hits")


def test_path_id():
    a = fq.path_id([(1, 1), (2, 5)])
    check(a == fq.path_id([(1, 1), (2, 6)]), "counts within one class: the same path")
    check(a != fq.path_id([(1, 1), (2, 9)]), "another class: another path")
    check(a != fq.path_id([(1, 1)]), "fewer buckets: another path")


def test_favored_and_skip():
    q = fq.Queue("explore")
    big = entry(0, [1, 2, 3], insns=1000, data=b"x" * 50)
    small = entry(1, [1, 2], insns=10, data=b"y")
    other = entry(2, [3], insns=500, data=b"z" * 5)
    for e in (big, small, other):
        q.add(e)
    q.cull()
    check(small.favored and other.favored and not big.favored,
          "favored: the cheapest entry per bucket, a greedy cover")
    check(q.pending_favored == 2, "both favored entries are pending")
    check(not q.skip(small, 0.999), "a favored entry is never skipped")
    check(q.skip(big, 0.98) and not q.skip(big, 0.995), "99% skipped while favored ones wait")
    q.pending_favored = 0
    check(q.skip(big, 0.7) and not q.skip(big, 0.8), "75% of new non-favored entries")
    big.times_chosen = 1
    check(q.skip(big, 0.9) and not q.skip(big, 0.96), "95% of fuzzed non-favored entries")
    small.insns = 100000                       # trimmed elsewhere would lower it; here it grows
    big.insns, big.data = 1, b"q"
    q.rescore(big)
    q.cull()
    check(big.favored, "a cheaper entry after rescore becomes favored")


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


def main():
    for t in (test_classes_and_fold, test_path_id, test_favored_and_skip, test_schedules,
              test_opstats, test_names_and_persistence, test_sync_bookkeeping):
        t()
    if failures:
        for f in failures:
            print("brain_unit: FAIL: " + f, file=sys.stderr)
        return 1
    print("brain_unit: ok (coverage fold, paths, favored set, schedules, operator weights, "
          "queue files, sync bookkeeping)")
    return 0


if __name__ == "__main__":
    random.seed(0)
    sys.exit(main())
