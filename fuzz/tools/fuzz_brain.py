#!/usr/bin/env python3
"""fuzz_brain.py -- ember's coverage-guided fuzz brain.

ember runs the inputs; this chooses them. Start the brain, then start ember
with `uam-run --fuzz-net HOST:PORT ...` (within --accept-timeout seconds):
ember dials in, boots its firmware, says HELLO, and from then on runs each
INPUT the brain sends and answers it with one COVERAGE (or SPARSE) map or CRASH
verdict. The protocol (UAMF) is specified in docs/fuzz-formats.md; its constants
are in fuzz/formats.h. The design of the campaign is docs/fuzz-ideas.md; its
logic lives in fuzz/tools/fuzz_queue.py.

The brain
  - asks ember for SPARSE replies (OPTIONS): each iteration's set buckets and its
    instruction count, folded in O(buckets hit) with AFL-style hit-count classes;
  - keeps a disk-backed **queue** under <out>/queue (id:NNNNNN,... names that
    record lineage) and **resumes** it on start;
  - walks the queue in cycles: a **favored** set (the cheapest entry per bucket),
    **calibration** (a new entry runs twice; ember is deterministic, so a
    difference is a harness bug, reported as UNSTABLE), **trimming**, and an
    **energy** per entry from a performance score times its **power schedule**
    (--schedule fast, explore, exploit, coe, rare);
  - mutates with fuzz/tools/mutate.py (havoc + splice), an optional ember
    **dictionary** (--dict; integer tokens in the target's byte order, from
    HELLO), and operator weights it **adapts** to what finds coverage;
  - saves each crash as the **full input plus a .info sidecar** (reason name,
    status, size, hex) so it can be replayed (`uam-run --fuzz-replay`), and
    **deduplicates** by (reason, status). HANG and TIMEOUT verdicts go to
    <out>/hangs: every distinct hang input is kept (up to --max-hangs). REJECTED
    (the input route refused the input) is not a bug: it is only counted. A
    finding is written before it is minimized, so it survives a lost connection;
  - **minimizes** a newly found crash (bounded block dropping). Not hangs: every
    probe of one costs ember a full per-input budget (--minimize-timeouts opts in);
  - with --sync-dir and --name, **shares work** with other workers: it imports
    their new queue entries that are new to it (fuzz/tools/fuzz_launch.py runs a
    whole parallel campaign).
  * It runs **until you stop it** (Ctrl-C / SIGTERM) or ember says BYE; --iters N
    caps the mutated executions.

Usage:
  python3 fuzz/tools/fuzz_brain.py --listen 127.0.0.1:7000 --seeds seeds/ \\
      --out fuzz_out --status-file fuzz_out/status.jsonl &
  uam-run --fuzz-net 127.0.0.1:7000 ... firmware.elf
  python3 fuzz/tools/status_tui.py fuzz_out/status.jsonl

  fuzz_brain.py --listen HOST:PORT [--seeds DIR] [--out DIR] [--iters N]
                [--schedule S] [--dict FILE]... [--no-trim] [--trim-probes N]
                [--no-calibrate] [--reweight-every N]
                [--name NAME --sync-dir DIR [--sync-interval S] [--main|--secondary]]
                [--max-size N] [--no-minimize] [--minimize-timeouts]
                [--max-hangs N] [--seed-rng N] [--accept-timeout S]
                [--hello-timeout S] [--io-timeout S]
                [--status-file FILE] [--status-interval S] [--quiet]

A missing or empty --seeds directory falls back to built-in seeds (plus any
resumed queue). --listen with port 0 picks a free port; the log line
"listening on HOST:PORT" names it.

Exit status: 0 after a clean session (--iters reached, a signal, or ember's
BYE), 1 on a protocol error, a timeout or a lost connection.
"""
import argparse
import hashlib
import json
import os
import random
import re
import signal
import socket
import struct
import sys
import time

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mutate import DEFAULT_OP_WEIGHTS, Dictionary, Engine, parse_dict  # noqa: E402
from fuzz_queue import (SCHEDULES, Coverage, Entry, OpStats, Queue, entry_name,  # noqa: E402
                        hits_from_map, load_queue, new_peer_inputs, peers, save_entry,
                        save_state, set_synced, synced_upto)

# ---- the fuzz protocol (fuzz/formats.h, docs/fuzz-formats.md) ----------
NET_MAGIC = b"UAMF"
NET_VERSION = 1
NET_FRAME_MAX = 64 << 20                 # bytes in one frame, type included
HELLO, INPUT, COVERAGE, CRASH, BYE, OPTIONS, SPARSE = 1, 2, 3, 4, 5, 6, 7
HELLO_FIXED_LEN, ARCH_MAX = 10, 64
COVERAGE_HDR_LEN, CRASH_LEN, SPARSE_HDR_LEN = 13, 13, 25
HELLO_BIG_ENDIAN = 1 << 15               # HELLO flags: the target is big-endian
OPT_SPARSE = 1 << 0                      # OPTIONS: reply with SPARSE, not COVERAGE
KIND_INIT, KIND_ITER = 1, 2

# HELLO flags: the coverage scheme (the coverage region's flags, same bits).
SCHEME, SCHEME_PC, SCHEME_ROTATE = 1 << 0, 1 << 1, 1 << 2
NGRAM_SHIFT, NGRAM_MASK = 8, 0xf << 8

FAULT, GUARD, ORACLE, HANG, TIMEOUT, REJECTED, MALFORMED = 1, 2, 3, 4, 5, 6, 7
REASON_NAMES = {
    FAULT: "FAULT", GUARD: "GUARD", ORACLE: "ORACLE", HANG: "HANG",
    TIMEOUT: "TIMEOUT", REJECTED: "REJECTED", MALFORMED: "MALFORMED",
}
HEX_STATUS = (FAULT, GUARD, ORACLE)      # status is a pc or a detail code
HANG_REASONS = (HANG, TIMEOUT)           # saved under hangs/, not crashes/


def reason_name(reason):
    """Symbolic name of a CRASH reason; unknown ones stay opaque ('r<N>')."""
    return REASON_NAMES.get(reason, "r%d" % reason)


def fmt_status(reason, status):
    if reason in HEX_STATUS:
        return "0x%x" % status
    return "%d" % status


def scheme_str(flags):
    """The coverage scheme of HELLO flags, e.g. 'n=2 model=site rotate=0'."""
    if not flags & SCHEME:
        return "blocks"
    return "n=%d model=%s rotate=%d" % (
        (flags & NGRAM_MASK) >> NGRAM_SHIFT,
        "pc" if flags & SCHEME_PC else "site",
        1 if flags & SCHEME_ROTATE else 0)


class EmberBye(Exception):
    """ember sent BYE: a clean end of the session."""


class ProtocolError(Exception):
    """ember broke the framing or a frame's layout."""


def recvn(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("ember closed the connection")
        buf += chunk
    return buf


def recv_frame(sock):
    (length,) = struct.unpack(">I", recvn(sock, 4))
    # length counts the type byte, so 0 is not a frame; past the limit the
    # stream is most likely out of sync.
    if length < 1 or length > NET_FRAME_MAX:
        raise ProtocolError("frame length %d (must be 1..%d)" % (length, NET_FRAME_MAX))
    body = recvn(sock, length)
    return body[0], body[1:]        # type, payload


def send_frame(sock, ftype, payload=b""):
    sock.sendall(struct.pack(">IB", len(payload) + 1, ftype) + payload)


def main():
    ap = argparse.ArgumentParser(
        description="ember's coverage-guided fuzz brain: listens for "
                    "`uam-run --fuzz-net HOST:PORT` and drives a campaign.")
    ap.add_argument("--listen", required=True,
                    help="HOST:PORT to listen on (port 0: any free port)")
    ap.add_argument("--seeds", default=None,
                    help="directory of seed inputs (missing or empty: "
                         "built-in seeds)")
    ap.add_argument("--out", default=None,
                    help="output dir (holds crashes/, hangs/ and the resumable "
                         "queue/; default ./fuzz_out; with --sync-dir: SYNC_DIR/NAME)")
    ap.add_argument("--iters", type=int, default=0,
                    help="stop after N mutated execs (0 = run until stopped)")
    ap.add_argument("--max-size", type=int, default=65536,
                    help="max mutated input bytes (default 65536; an input "
                         "budget, not a protocol limit)")
    ap.add_argument("--schedule", default="fast", choices=SCHEDULES,
                    help="power schedule: how much energy each queue entry gets "
                         "(default fast; docs/fuzz-ideas.md)")
    ap.add_argument("--dict", action="append", default=[],
                    help="ember dictionary file (fuzz/tools/mutate.py's format; "
                         "integer tokens follow the target's byte order); repeatable")
    ap.add_argument("--no-trim", dest="trim", action="store_false",
                    help="do not trim new queue entries")
    ap.add_argument("--trim-probes", type=int, default=64,
                    help="most runs spent trimming one entry (default 64)")
    ap.add_argument("--no-calibrate", dest="calibrate", action="store_false",
                    help="do not run new entries twice to check that ember is deterministic")
    ap.add_argument("--reweight-every", type=int, default=5000,
                    help="adapt the mutation operators' weights every N execs "
                         "(0 = keep the defaults; default 5000)")
    ap.add_argument("--name", default=None,
                    help="this worker's name (parallel campaigns; default main)")
    ap.add_argument("--sync-dir", default=None,
                    help="share work with the other workers whose output directories "
                         "are in SYNC_DIR (this one's is SYNC_DIR/NAME)")
    ap.add_argument("--sync-interval", type=float, default=60.0,
                    help="seconds between imports from the other workers (default 60)")
    role = ap.add_mutually_exclusive_group()
    role.add_argument("--main", dest="role", action="store_const", const="main",
                      help="this worker is the main one of a parallel campaign")
    role.add_argument("--secondary", dest="role", action="store_const", const="secondary",
                      help="this worker is a secondary one")
    ap.add_argument("--no-minimize", dest="minimize", action="store_false",
                    help="skip crash minimization (on by default)")
    ap.add_argument("--minimize-timeouts", action="store_true",
                    help="also minimize HANG/TIMEOUT inputs (off by default: "
                         "every probe costs ember a full per-input budget)")
    ap.add_argument("--max-hangs", type=int, default=64,
                    help="save at most N distinct HANG/TIMEOUT inputs "
                         "(default 64)")
    ap.add_argument("--seed-rng", type=int, default=0xC0FFEE,
                    help="RNG seed (default 0xC0FFEE; parallel workers need distinct ones)")
    ap.add_argument("--accept-timeout", type=float, default=15.0,
                    help="seconds to wait for ember to connect (default 15)")
    ap.add_argument("--hello-timeout", type=float, default=600.0,
                    help="seconds to wait for HELLO once ember has connected "
                         "(0 = forever; default 600: ember boots its firmware "
                         "before it says HELLO)")
    ap.add_argument("--io-timeout", type=float, default=60.0,
                    help="seconds to wait for each reply (0 = forever; "
                         "default 60)")
    ap.add_argument("--status-file",
                    help="write a JSON-lines status feed here, anew each session (watch it with "
                         "fuzz/tools/status_tui.py; default with --sync-dir: OUT/status.jsonl)")
    ap.add_argument("--status-interval", type=float, default=1.0,
                    help="seconds between status snapshots (default 1.0)")
    ap.add_argument("--quiet", action="store_true",
                    help="no log lines (errors still go to stderr)")
    args = ap.parse_args()

    host, _, port = args.listen.rpartition(":")
    if not port.isdigit():
        sys.exit("brain: --listen must be HOST:PORT (e.g. 127.0.0.1:7000), got %r"
                 % args.listen)
    name = args.name or "main"
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,32}", name):
        sys.exit("brain: --name must be 1-32 of [A-Za-z0-9_-], got %r" % name)
    if args.sync_dir:
        out = os.path.join(args.sync_dir, name)
        if args.out and os.path.abspath(args.out) != os.path.abspath(out):
            sys.exit("brain: with --sync-dir the output directory is SYNC_DIR/NAME (%s)" % out)
    else:
        out = args.out or os.path.join(os.getcwd(), "fuzz_out")
    role_name = args.role or ("main" if not args.sync_dir else "secondary")
    crash_dir = os.path.join(out, "crashes")
    hang_dir = os.path.join(out, "hangs")
    queue_dir = os.path.join(out, "queue")
    for d in (crash_dir, hang_dir, queue_dir):
        os.makedirs(d, exist_ok=True)
    if args.sync_dir and not args.status_file:
        args.status_file = os.path.join(out, "status.jsonl")

    def log(*a):
        if not args.quiet:
            print("brain:%s" % ("" if not args.sync_dir else " [%s]" % name), *a, flush=True)

    # ---- seeds, and RESUME any previously saved queue ---------------------------
    def load_dir(d):
        found = []
        for fname in (sorted(os.listdir(d)) if d and os.path.isdir(d) else []):
            p = os.path.join(d, fname)
            if os.path.isfile(p) and not fname.endswith(".info"):
                with open(p, "rb") as f:
                    found.append((fname, f.read()))
        return found

    if args.seeds and not os.path.isdir(args.seeds):
        print("brain: WARNING: seed directory %r does not exist; using built-in seeds"
              % args.seeds, file=sys.stderr)
    seeds = load_dir(args.seeds)
    resumed = load_queue(queue_dir)
    if resumed:
        log("resumed %d queue entries from %s" % (len(resumed), queue_dir))

    # ---- stop cleanly on Ctrl-C / SIGTERM (campaign state is on disk) ----
    stop = {"flag": False}

    def _stop(_signum, _frame):
        stop["flag"] = True
    signal.signal(signal.SIGINT, _stop)
    signal.signal(signal.SIGTERM, _stop)

    # ---- accept ember ----
    lsock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    lsock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    lsock.bind((host, int(port)))
    lsock.listen(1)
    lsock.settimeout(args.accept_timeout)
    listen_addr = "%s:%d" % (host, lsock.getsockname()[1])
    log("listening on %s" % listen_addr)
    try:
        conn, peer = lsock.accept()
    except socket.timeout:
        sys.exit("brain: ember did not connect within %gs (--accept-timeout)"
                 % args.accept_timeout)
    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    log("ember connected from %s:%d" % peer)

    conn.settimeout(args.hello_timeout or None)
    try:
        magic = recvn(conn, 4)
        if magic != NET_MAGIC:
            sys.exit("brain: bad magic %r (expected %r)" % (magic, NET_MAGIC))
        ftype, payload = recv_frame(conn)
    except socket.timeout:
        sys.exit("brain: ember sent no HELLO within %gs (--hello-timeout)"
                 % args.hello_timeout)
    except (ConnectionError, ProtocolError) as e:
        sys.exit("brain: before HELLO: %s" % e)
    conn.settimeout(args.io_timeout or None)
    if ftype == BYE:
        sys.exit("brain: ember said BYE before HELLO (it could not start)")
    if ftype != HELLO:
        sys.exit("brain: expected HELLO, got frame type %d" % ftype)
    if len(payload) < HELLO_FIXED_LEN:
        sys.exit("brain: HELLO payload is %d bytes (needs %d)"
                 % (len(payload), HELLO_FIXED_LEN))
    ver, flags, map_size, arch_len = struct.unpack(">HHIH", payload[:HELLO_FIXED_LEN])
    if ver != NET_VERSION:
        sys.exit("brain: ember speaks protocol version %d; this brain speaks %d"
                 % (ver, NET_VERSION))
    if arch_len > ARCH_MAX or len(payload) < HELLO_FIXED_LEN + arch_len:
        sys.exit("brain: HELLO arch_len %d is bad (max %d, payload %d bytes)"
                 % (arch_len, ARCH_MAX, len(payload)))
    if map_size == 0:
        sys.exit("brain: HELLO map_size is 0")
    arch = payload[HELLO_FIXED_LEN:HELLO_FIXED_LEN + arch_len].decode("ascii", "replace")
    order = "big" if flags & HELLO_BIG_ENDIAN else "little"
    scheme = scheme_str(flags)
    if flags & SCHEME and (flags & NGRAM_MASK) >> NGRAM_SHIFT < 3:
        log("NOTE: coverage scheme is %s; an N>=3 scheme with rotation is more "
            "path-sensitive for fuzzing (uam-run --cov-ngram 3 --cov-rotate)" % scheme)
    log("HELLO version=%d arch=%s (%s-endian) map_size=%d flags=0x%x scheme=%s"
        % (ver, arch, order, map_size, flags, scheme))
    # Ask for SPARSE replies: the hit buckets instead of the whole map. An ember that
    # does not know OPTIONS ignores it and keeps sending COVERAGE; both are handled.
    send_frame(conn, OPTIONS, struct.pack(">I", OPT_SPARSE))

    dictionary = None
    if args.dict:
        dictionary = Dictionary()
        for p in args.dict:
            try:
                dictionary.merge(parse_dict(p, order))
            except (OSError, ValueError) as e:
                sys.exit("brain: --dict: %s" % e)
        log("dictionary: %d tokens (%d integers, %s-endian unless marked) from %d file(s)"
            % (len(dictionary), len(dictionary.ints), order, len(args.dict)))

    rng = random.Random(args.seed_rng)
    cov = Coverage(map_size)
    queue = Queue(args.schedule)
    opstats = OpStats()
    default_weights = {int(op): w for op, w in DEFAULT_OP_WEIGHTS.items()}

    def splice_donor():
        return rng.choice(queue.entries).data if queue.entries else b""

    engine = Engine(rng, dictionary=dictionary, corpus=splice_donor, max_size=args.max_size)

    execs = mutated = new_cov = crashes = hangs = rejected = 0
    unstable = imported = trimmed_bytes = 0
    init_edges = [None]    # edges in ember's INIT map (boot), once it arrived
    crash_sigs = {}        # (reason, status) -> count; the dedup key
    hang_sigs = {}         # same, for HANG / TIMEOUT
    hang_inputs = set()    # sha1 of every hang input already saved
    next_id = [1]          # protocol input ids
    next_eid = [max([e.id for e in resumed], default=-1) + 1]   # queue entry ids
    sparse_seen = [False]

    # ---- optional JSON-lines status feed (for fuzz/tools/status_tui.py) --
    start = time.monotonic()
    status_fp = open(args.status_file, "w", buffering=1) if args.status_file else None
    last_status = [0.0]
    stage = ["baseline"]
    baseline_edges = [None]   # edges after the corpus replay; new_edges counts past it
    run_config = {"arch": arch, "scheme": scheme, "map_size": map_size,
                  "proto": ver, "ember": "%s:%d" % peer, "listen": listen_addr,
                  "out": out, "seeds": len(seeds), "resumed": len(resumed),
                  "max_size": args.max_size, "minimize": str(bool(args.minimize)),
                  "schedule": args.schedule, "worker": name, "role": role_name}
    if args.iters:
        run_config["iters_limit"] = args.iters
    if args.dict:
        run_config["dict"] = len(dictionary)

    def emit_status(finished=False):
        if not status_fp:
            return
        el = time.monotonic() - start
        edges = cov.edges
        status_fp.write(json.dumps({
            "t": round(el, 1), "execs": execs,
            "execs_per_sec": round(execs / el, 1) if el > 0 else 0.0,
            "corpus": len(queue.entries), "edges": edges, "map_size": map_size,
            "new_edges": edges - baseline_edges[0] if baseline_edges[0] is not None else 0,
            "init_edges": init_edges[0],
            "crashes": crashes, "crashes_unique": len(crash_sigs),
            "hangs": hangs, "hangs_unique": len(hang_sigs),
            "hangs_saved": len(hang_inputs), "rejected": rejected,
            "cycles": queue.cycle, "favored": queue.favored_count(),
            "pending_favored": queue.pending_favored, "unstable": unstable,
            "imported": imported, "sparse": sparse_seen[0],
            "worker": name, "role": role_name, "schedule": args.schedule,
            "stage": stage[0], "finished": finished, "config": run_config,
        }) + "\n")

    def emit_event(kind, msg, edges=None):
        if not status_fp:
            return
        ev = {"kind": kind, "msg": msg}
        if edges is not None:
            ev["edges"] = edges
        status_fp.write(json.dumps({"event": ev}) + "\n")

    def maybe_status():
        now = time.monotonic()
        if now - last_status[0] >= args.status_interval:
            emit_status()
            last_status[0] = now

    def probe(data):
        """Send one input and return ember's verdict, with no accounting:
        ('crash', reason, status) | ('iter', hits, insns) | ('other', None, None).
        hits: (index, count) pairs, ascending; insns 0 when ember sent a whole map."""
        ident = next_id[0]
        next_id[0] += 1
        send_frame(conn, INPUT, struct.pack(">QI", ident, len(data)) + data)
        while True:
            ftype, payload = recv_frame(conn)
            if ftype == BYE:
                raise EmberBye()
            insns = 0
            if ftype == CRASH:
                if len(payload) < CRASH_LEN:
                    raise ProtocolError("CRASH payload is %d bytes (needs %d)"
                                        % (len(payload), CRASH_LEN))
                cid, reason, status = struct.unpack(">QBI", payload[:CRASH_LEN])
                if reason == MALFORMED:
                    # ember could not parse our INPUT frame: a bug on this side.
                    raise ProtocolError("ember reports input id=%d as MALFORMED "
                                        "(its reply id %d)" % (ident, cid))
            elif ftype == SPARSE:
                if len(payload) < SPARSE_HDR_LEN:
                    raise ProtocolError("SPARSE payload is %d bytes (needs %d)"
                                        % (len(payload), SPARSE_HDR_LEN))
                cid, kind, insns, msz, n = struct.unpack(">QBQII", payload[:SPARSE_HDR_LEN])
                if msz != map_size or len(payload) != SPARSE_HDR_LEN + 5 * n:
                    raise ProtocolError("SPARSE map_size %d with %d entries in %d bytes "
                                        "(HELLO said %d)" % (msz, n, len(payload), map_size))
                hits = list(struct.iter_unpack(">IB", payload[SPARSE_HDR_LEN:]))
                sparse_seen[0] = True
            elif ftype == COVERAGE:
                if len(payload) < COVERAGE_HDR_LEN:
                    raise ProtocolError("COVERAGE payload is %d bytes (needs %d)"
                                        % (len(payload), COVERAGE_HDR_LEN))
                cid, kind, msz = struct.unpack(">QBI", payload[:COVERAGE_HDR_LEN])
                bitmap = payload[COVERAGE_HDR_LEN:COVERAGE_HDR_LEN + msz]
                if msz != map_size or len(bitmap) != msz:
                    raise ProtocolError("COVERAGE map_size %d with %d map bytes "
                                        "(HELLO said %d)"
                                        % (msz, len(payload) - COVERAGE_HDR_LEN, map_size))
                hits = hits_from_map(bitmap)
            else:
                log("ignoring unexpected frame type %d (%d bytes)"
                    % (ftype, len(payload)))
                continue
            if ftype != CRASH and cid == 0 and kind == KIND_INIT:
                # The unsolicited INIT map (boot up to the first begin): not a reply
                # to this input, which is still to come.
                init_edges[0] = len(hits)
                log("ember sent its INIT map (%d edges); not scored" % init_edges[0])
                continue
            if cid != ident:
                raise ProtocolError("input id=%d answered with id=%d (lockstep "
                                    "broken: one reply per INPUT, echoing its id)"
                                    % (ident, cid))
            if ftype == CRASH:
                return ("crash", reason, status)
            if kind != KIND_ITER:      # only per-input (ITER) coverage is scored
                return ("other", None, None)
            return ("iter", hits, insns)

    def minimize(data, reason, status):
        """Bounded block dropping: shrink `data` while it still fails with the
        same (reason, status) -- for FAULT / GUARD / ORACLE the status (a pc or
        a detail code) is the bug's identity. A readable tmin, not an optimal one."""
        if not args.minimize or len(data) <= 1:
            return data
        if reason in HANG_REASONS and not args.minimize_timeouts:
            return data                   # each probe would cost a timeout
        best = data
        chunk = max(1, len(best) // 2)
        budget = 200                      # cap re-sends so minimize stays cheap
        while chunk >= 1 and budget > 0:
            i = 0
            while i < len(best) and budget > 0:
                cand = best[:i] + best[i + chunk:]
                budget -= 1
                if cand and probe(cand) == ("crash", reason, status):
                    best = cand           # the drop kept the crash
                else:
                    i += chunk
            chunk //= 2
        return best

    def write_finding(where, prefix, data, reason, status):
        """Write one finding plus its .info sidecar; return the file name."""
        fname = "%s_r%d_s%d_%s" % (prefix, reason, status,
                                   hashlib.sha1(data).hexdigest()[:12])
        with open(os.path.join(where, fname), "wb") as f:
            f.write(data)
        with open(os.path.join(where, fname + ".info"), "w") as f:
            f.write("reason=%d\nreason_name=%s\nstatus=%d\n"
                    % (reason, reason_name(reason), status))
            if reason in HEX_STATUS:
                f.write("status_hex=%s\n" % fmt_status(reason, status))
            f.write("len=%d\nsig=r%d/s%d\nhex=%s\n"
                    % (len(data), reason, status, data.hex()))
        return fname

    def drop_finding(where, fname):
        for n in (fname, fname + ".info"):
            try:
                os.unlink(os.path.join(where, n))
            except OSError:
                pass

    def save_crash(data, reason, status):
        nonlocal crashes, hangs, rejected
        if reason == REJECTED:            # the input route refused it: no bug
            rejected += 1
            return
        is_hang = reason in HANG_REASONS
        if is_hang:
            hangs += 1
            nth, sigs, where, label, ev = hangs, hang_sigs, hang_dir, "HANG", "hang"
        else:
            crashes += 1
            nth, sigs, where, label, ev = crashes, crash_sigs, crash_dir, "CRASH", "crash"
        sig = (reason, status)
        n = sigs.get(sig, 0) + 1
        sigs[sig] = n
        rname, st = reason_name(reason), fmt_status(reason, status)
        if is_hang:
            # (reason, status) says little about a hang (TIMEOUT's status is
            # 0), so keep every distinct input, up to a cap.
            h = hashlib.sha1(data).digest()
            if h in hang_inputs or len(hang_inputs) >= args.max_hangs:
                log("%s #%d %s status=%s not saved (%s)"
                    % (label, nth, rname, st,
                       "already saved" if h in hang_inputs
                       else "--max-hangs %d reached" % args.max_hangs))
                return
            hang_inputs.add(h)
        elif n > 1:                       # already have this bug: don't flood
            log("%s #%d %s status=%s dup (count=%d, not saved)"
                % (label, nth, rname, st, n))
            return
        prefix = "hang" if is_hang else "crash"
        # Save the input as found first: minimizing re-sends it, and a BYE, a
        # timeout or a dropped connection there must not lose the finding.
        fname = write_finding(where, prefix, data, reason, status)
        small = minimize(data, reason, status)
        if small != data:
            drop_finding(where, fname)
            fname = write_finding(where, prefix, small, reason, status)
            data = small
        log("%s #%d %s status=%s NEW len=%d (saved %s)"
            % (label, nth, rname, st, len(data), fname))
        emit_event(ev, "%s status=%s %s" % (rname, st, fname))

    def run(data):
        """Run one input and account for it: ('crash', ...) after saving it, or
        ('iter', hits, insns, path, new_buckets, new_classes) | ('other', ...)."""
        nonlocal execs
        r = probe(data)
        execs += 1
        if r[0] == "crash":
            save_crash(data, r[1], r[2])
            return r
        if r[0] != "iter":
            return r
        hits, insns = r[1], r[2]
        pid = queue.note_execution(hits)
        nb, nc = cov.fold(hits)
        return ("iter", hits, insns, pid, nb, nc)

    def calibrate(e, hits):
        """Run a new entry again: ember is deterministic, so the hits must match. A
        difference is a harness problem (state leaking between iterations), not noise."""
        nonlocal unstable
        e.calibrated = True
        if not args.calibrate:
            return
        r = probe(e.data)
        nonlocal_execs(1)
        if r[0] == "iter" and r[1] == hits:
            return
        unstable += 1
        if r[0] != "iter":
            diff = "the second run ended as %s" % (
                "%s status=%s" % (reason_name(r[1]), fmt_status(r[1], r[2])) if r[0] == "crash"
                else r[0])
        else:
            a, b = dict(hits), dict(r[1])
            keys = sorted(k for k in set(a) | set(b) if a.get(k) != b.get(k))
            diff = "%d buckets differ (%s%s)" % (
                len(keys), ", ".join("%d: %d->%d" % (k, a.get(k, 0), b.get(k, 0)) for k in keys[:8]),
                ", ..." if len(keys) > 8 else "")
        msg = ("UNSTABLE: queue entry %s ran differently twice: %s. ember is deterministic, so "
               "the harness leaks state between iterations (a device the snapshot misses, "
               "an uninitialised buffer); fix that before trusting coverage" % (e.name, diff))
        print("brain: " + msg, file=sys.stderr, flush=True)
        emit_event("info", msg)

    def nonlocal_execs(n):
        nonlocal execs
        execs += n

    def add_entry(data, hits, insns, pid, nb, parent=None, op=None, sync=None, src=None,
                  orig=None, keep_name=None):
        e = Entry(id=next_eid[0], data=data, parent=parent.id if parent else src,
                  depth=parent.depth + 1 if parent else 0, insns=insns,
                  buckets=[i for i, _ in hits], path=pid, found_at=queue.cycle,
                  new_cov=nb > 0)
        next_eid[0] += 1
        e.name = keep_name or entry_name(e.id, src=parent.id if parent else src, op=op,
                                          new_cov=nb > 0, sync=sync, orig=orig)
        if not keep_name:
            save_entry(queue_dir, e)
        queue.add(e)
        calibrate(e, hits)
        return e

    def trim(e):
        """Remove chunks while the path stays the same (AFL's trim stage): smaller
        entries mutate faster and splice better. One run per probe: ember is exact."""
        nonlocal trimmed_bytes
        e.trimmed = True
        data = e.data
        if len(data) < 5 or args.trim_probes <= 0:
            save_state(queue_dir, e)
            return
        stage[0] = "trim %s" % e.name
        lp2 = 1
        while lp2 < len(data):
            lp2 <<= 1
        remove = max(lp2 // 16, 4)
        floor = max(lp2 // 1024, 4)
        probes = 0
        best_insns = e.insns
        while remove >= floor and probes < args.trim_probes and not stop["flag"]:
            pos = remove
            while pos < len(data) and probes < args.trim_probes:
                cand = data[:pos] + data[pos + remove:]
                probes += 1
                r = run(cand)
                if r[0] == "iter" and r[3] == e.path:
                    data = cand
                    best_insns = r[2]
                else:
                    pos += remove
            remove //= 2
        if len(data) < len(e.data):
            trimmed_bytes += len(e.data) - len(data)
            e.data = data
            e.insns = best_insns
            save_entry(queue_dir, e)
            queue.rescore(e)
        else:
            save_state(queue_dir, e)

    last_sync = [0.0]

    def sync(force=False):
        """Import the other workers' new queue entries that are new here."""
        nonlocal imported
        if not args.sync_dir:
            return
        now = time.monotonic()
        if not force and now - last_sync[0] < args.sync_interval:
            return
        last_sync[0] = now
        for p_name in peers(args.sync_dir, name):
            after = synced_upto(out, p_name)
            todo, top = new_peer_inputs(args.sync_dir, p_name, after)
            got = 0
            for eid, path in todo:
                if stop["flag"]:
                    return                # .synced stays put: the rest next time
                try:
                    with open(path, "rb") as f:
                        data = f.read()
                except OSError:
                    continue
                r = run(data)
                if r[0] == "iter" and r[5]:
                    add_entry(data, r[1], r[2], r[3], r[4], op=None, sync=p_name, src=eid)
                    got += 1
            if top > after:
                set_synced(out, p_name, top)
            if got:
                imported += got
                log("sync: imported %d of %d new entries from %s" % (got, len(todo), p_name))
                emit_event("info", "imported %d from %s" % (got, p_name))

    def maybe_reweight():
        if args.reweight_every and mutated and mutated % args.reweight_every == 0:
            engine.set_op_weights(opstats.weights(default_weights))

    status = 0
    ember_bye = False
    try:
        # ---- baseline: the resumed queue (as saved) and the seeds ----
        emit_status()
        seen = set()
        todo = [(e.data, e) for e in resumed] + [(d, fname) for fname, d in seeds]
        if not todo:
            todo = [(b, "builtin%d" % k) for k, b in enumerate((b"A", b"B", b"C"))]
        for k, (data, src) in enumerate(todo, 1):
            if stop["flag"]:
                break
            h = hashlib.sha1(data).digest()
            if h in seen:
                continue
            seen.add(h)
            stage[0] = "baseline %d/%d" % (k, len(todo))
            r = run(data)
            if r[0] != "iter":
                continue
            if isinstance(src, Entry):       # a resumed entry: kept, with its name and state
                e = add_entry(data, r[1], r[2], r[3], r[4], keep_name=src.name)
                e.id, e.parent, e.depth = src.id, src.parent, src.depth
                e.found_at, e.times_chosen, e.trimmed = src.found_at, src.times_chosen, src.trimmed
                e.new_cov = src.new_cov
                next_eid[0] -= 1             # add_entry took an id it did not use
            elif r[5] or not queue.entries:
                add_entry(data, r[1], r[2], r[3], r[4], orig=src)
            maybe_status()
        next_eid[0] = max([e.id for e in queue.entries], default=-1) + 1
        baseline_edges[0] = cov.edges
        slow = max((e.insns for e in queue.entries), default=0)
        if slow:
            log("calibration: the slowest queue entry ran %d instructions; a per-input "
                "budget of about 5x that (uam-run --fuzz-iter-insns %d) catches hangs early"
                % (slow, 5 * slow))
        sync(force=True)
        if not queue.entries:
            log("no queue entry ran normally (every seed crashed, hung or was rejected); "
                "fuzzing from an empty input")
            r = run(b"")
            if r[0] == "iter":
                add_entry(b"", r[1], r[2], r[3], r[4], orig="empty")
        log("baseline: %d queue entries, %d edges; schedule %s"
            % (len(queue.entries), cov.edges, args.schedule))

        # ---- the campaign: cycles over the queue, energy from the schedule ----
        def limit():
            return stop["flag"] or (args.iters and mutated >= args.iters)

        while not limit() and queue.entries:
            queue.cull()
            for e in list(queue.entries):
                if limit():
                    break
                queue.cull()
                if queue.skip(e, rng.random()):
                    continue
                if args.trim and not e.trimmed:
                    trim(e)
                n = queue.energy(e)
                if n <= 0:
                    continue
                stage[0] = "havoc %s (%s, cycle %d)" % (e.name, args.schedule, queue.cycle)
                pending = e.favored and not e.times_chosen
                for _ in range(n):
                    if limit():
                        break
                    child = engine.mutate(e.data)
                    r = run(child)
                    mutated += 1
                    interesting = r[0] == "iter" and r[5] > 0
                    opstats.note(engine.last_ops, interesting)
                    if interesting:
                        c = add_entry(child, r[1], r[2], r[3], r[4], parent=e, op="havoc")
                        if r[4]:
                            new_cov += 1
                        log("new coverage +%d buckets, +%d classes (queue=%d) %s"
                            % (r[4], r[5], len(queue.entries), c.name))
                        emit_event("find", "queue=%d %s" % (len(queue.entries), c.name),
                                   edges=r[4])
                    maybe_status()
                    maybe_reweight()
                    sync()
                e.times_chosen += 1
                save_state(queue_dir, e)
                if pending and queue.pending_favored:
                    queue.pending_favored -= 1
            queue.cycle += 1
    except socket.timeout:
        print("brain: ember sent no reply within %gs (--io-timeout)"
              % args.io_timeout, file=sys.stderr)
        status = 1
    except ConnectionError as e:
        print("brain: %s" % e, file=sys.stderr)
        status = 1
    except ProtocolError as e:
        print("brain: protocol error: %s" % e, file=sys.stderr)
        status = 1
    except EmberBye:
        log("ember said BYE; ending the session")
        ember_bye = True

    if not ember_bye:
        try:
            send_frame(conn, BYE)
        except OSError:
            pass
    # Hangs are deduplicated by input, not by (reason, status): unique counts
    # the keys, saved the distinct inputs kept under hangs/.
    log("done: execs=%d new_cov=%d crashes=%d (unique=%d) hangs=%d (unique=%d, "
        "saved=%d) rejected=%d queue=%d favored=%d cycles=%d edges=%d unstable=%d "
        "imported=%d trimmed_bytes=%d"
        % (execs, new_cov, crashes, len(crash_sigs), hangs, len(hang_sigs),
           len(hang_inputs), rejected, len(queue.entries), queue.favored_count(),
           queue.cycle, cov.edges, unstable, imported, trimmed_bytes))
    emit_status(finished=True)
    if status_fp:
        status_fp.close()
    conn.close()
    lsock.close()
    return status


if __name__ == "__main__":
    sys.exit(main())
