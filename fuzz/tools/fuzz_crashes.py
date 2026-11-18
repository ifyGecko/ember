#!/usr/bin/env python3
"""
fuzz_crashes.py -- which saved crash the fuzz brain minimizes, and when. Imported by
fuzz_brain.py, not run.

Two keys, kept apart on purpose:

  - a crash's **identity** is (reason, status), as ember reports it (fuzz/formats.h). It
    decides what is SAVED: every identity is written to crashes/ as found, whatever else
    is known about it. Nothing below ever drops one.
  - a crash's **group** is (reason, the function its pc is in), when the brain was given
    the image's functions (--functions) and the status is a pc (FAULT, GUARD). It decides
    only what is MINIMIZED FIRST. One bug often faults at many instructions of one
    function, and minimizing each of them as it turns up costs more runs than fuzzing
    does; two bugs in one function also share a group, which is why a group never decides
    what is kept. Without functions, or for a status that is not a pc (ORACLE), the group
    is the identity.

The first identity of a group is minimized at once. Later ones are saved as found and
queued; the queue is worked off within a share of the campaign's runs (--minimize-share).
A crash another worker of the campaign already holds minimized is saved as found and not
minimized again.
"""
import bisect
import collections
import os
import sys

from image_resolve import elf_code_entries, load_symbols

FAULT, GUARD = 1, 2                      # enum uam_fuzz_net_reason: the status is a pc
PC_REASONS = (FAULT, GUARD)

NOW, LATER, PEER = "now", "later", "peer"

INFO_HEAD = 512                          # the keys read back precede the input's hex


class Functions:
    """The start addresses of an image's functions, as far as the brain was told. A crash
    status is the low 32 bits of a pc, so the starts are kept that way too."""

    def __init__(self, starts=()):
        self.starts = sorted({s & 0xffffffff for s in starts})

    def __len__(self):
        return len(self.starts)

    def find(self, pc):
        """Start of the function `pc` is in: the nearest start at or below it. None below
        the first one. (A file of starts has no ends: a pc past the last function lands in
        it, which costs a deferred minimization at worst.)"""
        i = bisect.bisect_right(self.starts, pc & 0xffffffff)
        return self.starts[i - 1] if i else None


def load_functions(paths):
    """Functions of the files named by --functions: an ELF's function symbols, or a
    symbols file in any format gen_sites.py --symbols reads."""
    starts = set()
    for path in paths:
        try:
            with open(path, "rb") as f:
                data = f.read()
        except OSError as e:
            sys.exit("brain: --functions %s: %s" % (path, e))
        found = elf_code_entries(data)
        if found is not None:
            if not found[1]:
                sys.exit("brain: --functions %s: the ELF file has no function symbols" % path)
            starts |= found[1]
            continue
        try:
            starts |= set(load_symbols(path))
        except SystemExit as e:
            sys.exit(str(e).replace("error: --symbols", "brain: --functions"))
    return Functions(starts)


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

    def __init__(self, functions=None, me="main"):
        self.functions = functions or Functions()
        self.me = me
        self.first = {}                  # group -> the identity minimized at once here
        self.groups = set()              # every group seen
        self.deferred = collections.deque()   # (file name, data, reason, status)

    def function(self, reason, status):
        """Start of the function a crash is in, or None: not known, or no pc."""
        if reason not in PC_REASONS:
            return None
        return self.functions.find(status)

    def group(self, reason, status):
        f = self.function(reason, status)
        return (reason, "s", status) if f is None else (reason, "f", f)

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
        g = self.group(reason, status)
        self.groups.add(g)
        who = self.held_by(reason, status, others)
        if who is not None:
            return PEER, "%s holds it minimized" % who
        if g in self.first:
            return LATER, "its function already has a minimized crash"
        for o in others:
            if o.minimized and self.group(o.reason, o.status) == g:
                return LATER, "%s holds a minimized crash in its function" % o.worker
            if (o.reason, o.status) == (reason, status) and o.worker < self.me:
                # Saved there moments ago and most likely being minimized: look again later.
                return LATER, "%s saved it first" % o.worker
        if nested:
            return LATER, "found while minimizing another crash"
        self.first[g] = (reason, status)
        return NOW, "the first crash in its function" if g[1] == "f" else "a new crash"

    def defer(self, fname, data, reason, status):
        self.deferred.append((fname, data, reason, status))

    def due(self, execs, deferred_execs, share):
        """Is a deferred crash to be minimized now? Only while the runs spent on deferred
        ones stay under `share` percent of all runs."""
        return bool(self.deferred) and deferred_execs * 100 < share * execs
