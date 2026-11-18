#!/usr/bin/env python3
"""
fuzz_crashes.py -- which saved crash the fuzz brain minimizes, and when. Imported by
fuzz_brain.py, not run.

What is SAVED and what is MINIMIZED are decided apart:

  - a crash's identity is (reason, status), as ember reports it (fuzz/formats.h). Every
    identity is written to crashes/ as found. Nothing here ever drops one, and nothing
    here judges two crashes to be the same bug: no such judgement is reliable.
  - minimizing costs runs (up to 200 a crash), and one bug often faults at many
    instructions, each an identity of its own. So only the first crash of a campaign is
    minimized at once. Every later one is saved as found and queued, and the queue is
    worked off within a share of the campaign's runs (--minimize-share): that cap, not a
    guess at which crashes are alike, is what bounds the cost.

The queue's order is a guess, and only an order: the crash that hit the most buckets no
earlier crash had hit (ember's count, a crash's new_buckets) goes first, as the one most
likely to be another bug. A crash another worker of the campaign already holds minimized
is saved as found and not minimized again.
"""
import collections
import heapq
import os

NOW, LATER, PEER = "now", "later", "peer"

INFO_HEAD = 512                          # the keys read back precede the input's hex


def read_info(path):
    """The leading key=value lines of a finding's .info sidecar ({} if unreadable)."""
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            head = f.read(INFO_HEAD)
    except OSError:
        return {}
    out = {}
    for line in head.splitlines():
        k, eq, v = line.partition("=")
        if eq:
            out[k] = v
    return out


Saved = collections.namedtuple("Saved", "worker reason status minimized")


def saved_crashes(dirs):
    """The crashes saved under `dirs`, a list of (worker name, its crashes/ directory):
    one Saved per .info sidecar that names a reason and a status."""
    out = []
    for worker, d in dirs:
        try:
            names = os.listdir(d)
        except OSError:
            continue
        for n in names:
            if not n.endswith(".info"):
                continue
            info = read_info(os.path.join(d, n))
            try:
                out.append(Saved(worker, int(info["reason"]), int(info["status"]),
                                 info.get("minimized") == "yes"))
            except (KeyError, ValueError):
                continue
    return out


class Triage:
    """Decides, for each new crash identity, whether it is minimized now, later or not at
    all here; holds the queue of the ones left for later."""

    def __init__(self, me="main"):
        self.me = me
        self.started = False             # a crash was minimized at once here
        self.queue = []                  # heap of (-fresh, order of arrival, crash)
        self.arrived = 0

    def __len__(self):
        return len(self.queue)

    def held_by(self, reason, status, others):
        """The worker that holds this identity minimized (None: nobody)."""
        for o in others:
            if o.minimized and (o.reason, o.status) == (reason, status):
                return o.worker
        return None

    def decide(self, reason, status, others=(), nested=False):
        """For an identity seen for the first time in this session: (NOW | LATER | PEER,
        why). `others`: the crashes on disk, this worker's and its peers' (saved_crashes).
        `nested`: found while another crash is being minimized, so it cannot be NOW."""
        who = self.held_by(reason, status, others)
        if who is not None:
            return PEER, "%s holds it minimized" % who
        if nested:
            return LATER, "found while minimizing another crash"
        if self.started or any(o.minimized for o in others):
            return LATER, "it waits its turn to be minimized"
        for o in others:
            if (o.reason, o.status) == (reason, status) and o.worker < self.me:
                # Saved there moments ago and most likely being minimized: look again later.
                return LATER, "%s saved it first" % o.worker
        self.started = True
        return NOW, "the first crash"

    def defer(self, fname, data, reason, status, fresh=0):
        """Queue a saved crash. fresh: the buckets it hit that no earlier crash had."""
        heapq.heappush(self.queue, (-fresh, self.arrived, (fname, data, reason, status)))
        self.arrived += 1

    def pop(self):
        """The queued crash to minimize next: the most fresh buckets, then the oldest."""
        return heapq.heappop(self.queue)[2]

    def due(self, execs, deferred_execs, share):
        """Is a queued crash to be minimized now? Only while the runs spent on queued
        ones stay under `share` percent of all runs."""
        return bool(self.queue) and deferred_execs * 100 < share * execs
