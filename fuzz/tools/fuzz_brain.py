#!/usr/bin/env python3
"""fuzz_brain.py -- ember's coverage-guided fuzz brain.

ember runs and mutates the inputs; this chooses what to work on. Start the brain,
then start ember with `uam-run --fuzz-net HOST:PORT ...` (within --accept-timeout
seconds): ember dials in, boots its firmware and says HELLO. The brain mirrors its queue
into ember (CORPUS) and asks for batches of mutations of one entry (HAVOC): ember
mutates, runs and judges them itself and reports only what is worth keeping (FIND) and
the batch's totals (HAVOC_DONE). Single inputs (seeds, trimming, input-to-state,
minimizing, imports) go as INPUT and get one COVERAGE (or SPARSE) map or CRASH verdict. The protocol (UAMF) is specified in docs/fuzz-formats.md; its constants
are in fuzz/formats.h. The design of the campaign is docs/fuzz-ideas.md; its
logic lives in fuzz/tools/fuzz_queue.py.

The brain
  - keeps no coverage record of its own: ember judges every run (JUDGE before each
    INPUT's reply, FIND in a batch) with AFL-style hit-count classes, and replies with
    each iteration's set buckets and instruction count (SPARSE);
  - keeps a disk-backed **queue** under <out>/queue (id:NNNNNN,... names that
    record lineage) and **resumes** it on start;
  - walks the queue in cycles: a **favored** set (the cheapest entry per bucket),
    **calibration** (a new entry runs twice; ember is deterministic, so a
    difference is a harness bug, reported as UNSTABLE), **trimming**, and an
    **energy** per entry from a performance score times its **power schedule**
    (--schedule fast, explore, exploit, coe, rare, lin, quad, seek, mmopt);
  - with **compare logging** (ember announces it in HELLO; --no-cmplog turns it off):
    runs each queue entry once with its compares logged (CMP_INPUT) and tries the
    **input-to-state** replacements fuzz/tools/cmplog.py derives from them (one
    operand's bytes found in the input, the other written there), and adds the code's
    constants the input lacks to the dictionary;
  - has ember **mutate** (fuzz/mutate.c: havoc + splice, inputs that grow up to
    --max-size), with an optional ember **dictionary** (--dict, fuzz/tools/fuzz_dict.py;
    integer tokens in the target's byte order, from HELLO), and operator weights it
    **adapts** to what finds coverage. An ember without in-ember mutation (HELLO bit 13)
    is refused: rebuild it;
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
    caps the mutated executions (in whole batches of at most --batch).

Usage:
  python3 fuzz/tools/fuzz_brain.py --listen 127.0.0.1:7000 --seeds seeds/ \\
      --out fuzz_out --status-file fuzz_out/status.jsonl &
  uam-run --fuzz-net 127.0.0.1:7000 ... firmware.elf
  python3 fuzz/tools/status_tui.py fuzz_out/status.jsonl

  fuzz_brain.py --listen HOST:PORT [--seeds DIR] [--out DIR] [--iters N]
                [--schedule S] [--dict FILE]... [--no-trim] [--trim-probes N]
                [--no-calibrate] [--reweight-every N] [--no-cmplog] [--cmplog-max N]
                [--batch N] [--min-energy N] [--stack LO-HI] [--splice PERMILLE]
                [--iter-insns N|auto] [--crashes-per-sig N]
                [--name NAME --sync-dir DIR [--sync-interval S]]
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
from cmplog import candidates as i2s_candidates, parse_cmps, tokens as i2s_tokens  # noqa: E402
from fuzz_dict import Dictionary, IntToken, parse_dict  # noqa: E402
from fuzz_queue import (SCHEDULES, Entry, OpStats, Queue, entry_name,  # noqa: E402
                        hits_from_map, load_queue, new_peer_inputs, peers, save_entry,
                        save_state, set_synced, synced_upto)

# ---- the fuzz protocol (fuzz/formats.h, docs/fuzz-formats.md) ----------
NET_MAGIC = b"UAMF"
NET_VERSION = 1
NET_FRAME_MAX = 64 << 20                 # bytes in one frame, type included
HELLO, INPUT, COVERAGE, CRASH, BYE, OPTIONS, SPARSE, CMP_INPUT, CMPS = 1, 2, 3, 4, 5, 6, 7, 8, 9
MUTATOR, MUTATOR_INFO, CORPUS, HAVOC, FIND, HAVOC_DONE = 10, 11, 12, 13, 14, 15
JUDGE, PROGRESS = 16, 17
FIND_HDR_LEN, JUDGE_LEN = 49, 25
OPT_JUDGE = 1 << 2                       # OPTIONS: a JUDGE frame before each INPUT's reply
HAVOC_EDGES = 1 << 0                     # HAVOC flags: HAVOC_DONE lists executions per bucket
HAVOC_STATUS = {1: "no accepted MUTATOR", 2: "no such corpus entry", 3: "a bad batch size"}
HAVOC_MAX = 1 << 20                      # most mutations in one HAVOC
HELLO_FIXED_LEN, ARCH_MAX = 10, 64
COVERAGE_HDR_LEN, CRASH_LEN, SPARSE_HDR_LEN = 13, 13, 25
HELLO_BIG_ENDIAN = 1 << 15               # HELLO flags: the target is big-endian
HELLO_CMP = 1 << 14                      # HELLO flags: compare logging is understood
HELLO_HAVOC = 1 << 13                    # HELLO flags: MUTATOR, CORPUS and HAVOC are understood
OPT_SPARSE = 1 << 0                      # OPTIONS: reply with SPARSE, not COVERAGE
OPT_CMP_LOG = 1 << 1                     # OPTIONS: instrument compares for CMP_INPUT
CMPLOG_TOKENS_MAX = 256                  # dictionary tokens taken from compare logs
KIND_INIT, KIND_ITER = 1, 2

# HELLO flags: the coverage scheme (the coverage region's flags, same bits).
SCHEME, SCHEME_PC, SCHEME_ROTATE, SCHEME_BLOCK = 1 << 0, 1 << 1, 1 << 2, 1 << 3
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
        "block" if flags & SCHEME_BLOCK else "pc" if flags & SCHEME_PC else "site",
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
                    help="max mutated input bytes (default 65536): ember's mutations grow "
                         "inputs up to it, from seeds of any size")
    ap.add_argument("--batch", type=int, default=1024,
                    help="most mutations in one HAVOC batch (default 1024; smaller keeps "
                         "status, sync and --iters finer-grained)")
    ap.add_argument("--min-energy", type=int, default=64,
                    help="fewest mutations of an entry each time it is chosen (default 64; "
                         "a schedule's zero still skips it): small batches cost round trips")
    ap.add_argument("--stack", default="2-128",
                    help="havoc operators stacked per mutation, LO-HI; the depth is drawn "
                         "log-uniformly from the powers of two in range (default 2-128)")
    ap.add_argument("--splice", type=int, default=150,
                    help="per mille of mutations that first splice with another queue entry "
                         "(default 150)")
    ap.add_argument("--schedule", default="fast", choices=SCHEDULES,
                    help="power schedule: how much energy each queue entry gets "
                         "(default fast; docs/fuzzing.md has the table)")
    ap.add_argument("--dict", action="append", default=[],
                    help="ember dictionary file (fuzz/tools/fuzz_dict.py's format; "
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
    ap.add_argument("--no-cmplog", dest="cmplog", action="store_false",
                    help="no compare logging and input-to-state stage (on when ember "
                         "offers it)")
    ap.add_argument("--cmplog-max", type=int, default=256,
                    help="most input-to-state candidates tried per queue entry (default 256)")
    ap.add_argument("--name", default=None,
                    help="this worker's name (parallel campaigns; default main)")
    ap.add_argument("--sync-dir", default=None,
                    help="share work with the other workers whose output directories "
                         "are in SYNC_DIR (this one's is SYNC_DIR/NAME)")
    ap.add_argument("--sync-interval", type=float, default=60.0,
                    help="seconds between imports from the other workers (default 60)")
    ap.add_argument("--no-minimize", dest="minimize", action="store_false",
                    help="skip crash minimization (on by default)")
    ap.add_argument("--minimize-timeouts", action="store_true",
                    help="also minimize HANG/TIMEOUT inputs (off by default: "
                         "every probe costs ember a full per-input budget)")
    ap.add_argument("--crashes-per-sig", type=int, default=4,
                    help="save at most N crashes with one (reason, status): the first, "
                         "minimized, then those that ran code no earlier crash ran (default 4)")
    ap.add_argument("--iter-insns", default=None,
                    help="set ember's per-input instruction budget: N, or auto for 5x the "
                         "slowest queue entry after the baseline (default: ember's own "
                         "--fuzz-iter-insns)")
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
    m = re.fullmatch(r"(\d+)-(\d+)", args.stack)
    if not m or not 1 <= int(m.group(1)) <= int(m.group(2)) <= 1024:
        sys.exit("brain: --stack must be LO-HI with 1 <= LO <= HI <= 1024, got %r" % args.stack)
    stack_lo, stack_hi = int(m.group(1)), int(m.group(2))
    if args.iter_insns not in (None, "auto"):
        try:
            args.iter_insns = int(args.iter_insns, 0)
            if args.iter_insns < 1:
                raise ValueError
        except ValueError:
            sys.exit("brain: --iter-insns must be a positive number or auto")
    if not 0 <= args.splice <= 1000:
        sys.exit("brain: --splice is per mille, 0..1000")
    if args.min_energy < 1:
        sys.exit("brain: --min-energy must be at least 1")
    if not 1 <= args.batch <= HAVOC_MAX:
        sys.exit("brain: --batch must be 1..%d" % HAVOC_MAX)
    if not 1 <= args.max_size <= NET_FRAME_MAX // 2:
        sys.exit("brain: --max-size must be 1..%d" % (NET_FRAME_MAX // 2))

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
    role_name = "main" if name == "main" else "secondary"   # a label for the status screen
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
    if not flags & HELLO_HAVOC:
        sys.exit("brain: this ember does not mutate inputs itself (HELLO lacks bit 13): "
                 "rebuild uam-run from a tree that has fuzz/mutate.c")
    # Ask for SPARSE replies: the hit buckets instead of the whole map. An ember that
    # does not know OPTIONS ignores it and keeps sending COVERAGE; both are handled.
    # Compare logging only from an ember that announced it (it answers CMP_INPUT).
    cmplog_on = bool(args.cmplog and flags & HELLO_CMP)
    opt_flags = OPT_SPARSE | OPT_JUDGE | (OPT_CMP_LOG if cmplog_on else 0)

    def send_options(iter_insns=0):
        """Each OPTIONS replaces the last; iter_insns 0 leaves ember's budget as it is."""
        send_frame(conn, OPTIONS, struct.pack(">IQ", opt_flags, iter_insns))
    send_options(args.iter_insns if isinstance(args.iter_insns, int) else 0)
    if cmplog_on:
        log("compare logging on: an input-to-state stage for each new queue entry")
    elif args.cmplog:
        log("NOTE: this ember does not offer compare logging (an older build)")

    dictionary = Dictionary()
    if args.dict:
        for p in args.dict:
            try:
                dictionary.merge(parse_dict(p, order))
            except (OSError, ValueError) as e:
                sys.exit("brain: --dict: %s" % e)
        log("dictionary: %d tokens (%d integers, %s-endian unless marked) from %d file(s)"
            % (len(dictionary), len(dictionary.ints), order, len(args.dict)))

    rng = random.Random(args.seed_rng)
    edges = [0]            # buckets ever hit, as ember judged (JUDGE, FIND)
    last_judge = [None]    # (new_buckets, new_classes, path, novel_crash) of the last INPUT
    queue = Queue(args.schedule)
    opstats = OpStats()
    default_weights = {}   # op -> ember's default weight (MUTATOR_INFO)
    op_names = {}          # op -> ember's name for it
    weights = {}           # op -> the weight in use (adapted by maybe_reweight)

    execs = mutated = new_cov = crashes = hangs = rejected = 0
    unstable = imported = trimmed_bytes = 0
    cmplog_execs = cmplog_finds = cmplog_tokens = 0
    cmplog_done = set()    # queue entry ids already through the input-to-state stage
    last_cmps = [None]     # the records of the last CMP_INPUT
    init_edges = [None]    # edges in ember's INIT map (boot), once it arrived
    crash_sigs = {}        # (reason, status) -> count; the dedup key
    saved_per_sig = {}     # (reason, status) -> crashes saved (--crashes-per-sig)
    hang_sigs = {}         # same, for HANG / TIMEOUT
    hang_inputs = set()    # sha1 of every hang input already saved
    next_id = [1]          # protocol input ids
    next_eid = [max([e.id for e in resumed], default=-1) + 1]   # queue entry ids
    sparse_seen = [False]
    batches = 0

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
                  "schedule": args.schedule, "worker": name, "role": role_name,
                  "cmplog": str(cmplog_on)}
    if args.iters:
        run_config["iters_limit"] = args.iters
    if args.dict:
        run_config["dict"] = len(dictionary)

    def emit_status(finished=False):
        if not status_fp:
            return
        el = time.monotonic() - start
        status_fp.write(json.dumps({
            "t": round(el, 1), "execs": execs,
            "execs_per_sec": round(execs / el, 1) if el > 0 else 0.0,
            "corpus": len(queue.entries), "edges": edges[0], "map_size": map_size,
            "new_edges": edges[0] - baseline_edges[0] if baseline_edges[0] is not None else 0,
            "init_edges": init_edges[0],
            "crashes": crashes, "crashes_unique": len(crash_sigs),
            "hangs": hangs, "hangs_unique": len(hang_sigs),
            "hangs_saved": len(hang_inputs), "rejected": rejected,
            "cycles": queue.cycle, "favored": queue.favored_count(),
            "pending_favored": queue.pending_favored, "unstable": unstable,
            "imported": imported, "sparse": sparse_seen[0],
            "cmplog_execs": cmplog_execs, "cmplog_finds": cmplog_finds,
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

    def next_frame():
        """The next frame from ember, past its unsolicited INIT map (noted) and BYE (raised)."""
        while True:
            ftype, payload = recv_frame(conn)
            if ftype == BYE:
                raise EmberBye()
            if (ftype in (COVERAGE, SPARSE) and len(payload) >= 9
                    and payload[8] == KIND_INIT and payload[:8] == bytes(8)):
                if ftype == SPARSE and len(payload) >= SPARSE_HDR_LEN:
                    init_edges[0] = struct.unpack(">I", payload[21:25])[0]
                else:
                    init_edges[0] = len(hits_from_map(payload[COVERAGE_HDR_LEN:]))
                log("ember sent its INIT map (%d edges); not scored" % init_edges[0])
                continue
            return ftype, payload

    def send_mutator():
        """Configure ember's mutator (MUTATOR) and read its operator table (MUTATOR_INFO).
        A refusal is a bug on one side or the other: the session stops."""
        body = struct.pack(">IHHHIH", args.max_size, stack_lo, stack_hi, args.splice,
                           min(args.max_hangs, 0xFFFFFFFF), len(weights))
        body += b"".join(struct.pack(">BH", op, w) for op, w in sorted(weights.items()))
        body += dictionary.wire()
        send_frame(conn, MUTATOR, body)
        ftype, payload = next_frame()
        if ftype != MUTATOR_INFO:
            raise ProtocolError("expected MUTATOR_INFO after MUTATOR, got frame type %d" % ftype)
        try:
            status, mlen = struct.unpack(">BH", payload[:3])
            msg = payload[3:3 + mlen].decode("utf-8", "replace")
            p = 3 + mlen
            (n,) = struct.unpack(">H", payload[p:p + 2])
            p += 2
            for _ in range(n):
                op, dw, _w, nl = struct.unpack(">BHHB", payload[p:p + 6])
                op_names[op] = payload[p + 6:p + 6 + nl].decode("ascii", "replace")
                default_weights[op] = dw
                p += 6 + nl
        except struct.error:
            raise ProtocolError("MUTATOR_INFO is cut short (%d bytes)" % len(payload)) from None
        if status:
            raise ProtocolError("ember refused the mutator settings: %s" % msg)
        if not weights:
            weights.update(default_weights)

    def corpus_put(e):
        """Mirror a queue entry into ember (a parent and a splice donor)."""
        send_frame(conn, CORPUS, struct.pack(">II", e.id, len(e.data)) + e.data)

    def havoc(e, n):
        """One HAVOC batch: n mutations of e, run and judged by ember. Returns its FINDs as
        (reason, status, insns, new_buckets, new_classes, path, data, hits); the totals are
        accounted here."""
        nonlocal execs, mutated, crashes, hangs, rejected, batches
        first = next_id[0]
        next_id[0] += n
        flags = HAVOC_EDGES if queue.track_edges else 0
        send_frame(conn, HAVOC, struct.pack(">QIIQI", first, e.id, n, rng.getrandbits(64), flags))
        finds = []
        while True:
            ftype, payload = next_frame()
            if ftype == FIND:
                try:
                    (fid, reason, status, insns, ops, nb, nc, path,
                     dlen) = struct.unpack(">QBIQQIIQI", payload[:FIND_HDR_LEN])
                    data = payload[FIND_HDR_LEN:FIND_HDR_LEN + dlen]
                    p = FIND_HDR_LEN + dlen
                    (nh,) = struct.unpack(">I", payload[p:p + 4])
                except struct.error:
                    raise ProtocolError("FIND is cut short (%d bytes)" % len(payload)) from None
                if len(data) != dlen or len(payload) != p + 4 + 5 * nh:
                    raise ProtocolError("FIND's lengths do not add up (%d bytes)" % len(payload))
                if not first <= fid < first + n:
                    raise ProtocolError("FIND id=%d is not in the batch %d..%d"
                                        % (fid, first, first + n - 1))
                hits = list(struct.iter_unpack(">IB", payload[p + 4:]))
                finds.append((reason, status, insns, nb, nc, path.to_bytes(8, "big"), data, hits))
                continue
            if ftype == PROGRESS:         # the batch is alive: the reply timeout starts anew
                maybe_status()
                continue
            if ftype != HAVOC_DONE:
                raise ProtocolError("expected FIND or HAVOC_DONE in a batch, got frame type %d"
                                    % ftype)
            break
        try:
            done_id, st, ex = struct.unpack(">QBI", payload[:13])
            by = struct.unpack(">7I", payload[13:41])
            _nfinds, nops = struct.unpack(">IH", payload[41:47])
            p = 47
            op_totals = [struct.unpack(">BII", payload[p + 9 * k:p + 9 * k + 9]) for k in range(nops)]
            p += 9 * nops
            (npaths,) = struct.unpack(">I", payload[p:p + 4])
            p += 4
            paths = list(struct.iter_unpack(">QI", payload[p:p + 12 * npaths]))
            p += 12 * npaths
            (nedges,) = struct.unpack(">I", payload[p:p + 4])
            p += 4
            edges = list(struct.iter_unpack(">II", payload[p:p + 8 * nedges]))
        except struct.error:
            raise ProtocolError("HAVOC_DONE is cut short (%d bytes)" % len(payload)) from None
        if done_id != first:
            raise ProtocolError("HAVOC_DONE for batch %d while batch %d runs" % (done_id, first))
        if st:
            raise ProtocolError("ember refused HAVOC for queue entry %s: %s"
                                % (e.name, HAVOC_STATUS.get(st, "status %d" % st)))
        batches += 1
        execs += ex
        mutated += ex
        crashes += by[FAULT] + by[GUARD] + by[ORACLE]
        hangs += by[HANG] + by[TIMEOUT]
        rejected += by[REJECTED]
        for op, used, found in op_totals:
            opstats.add(op, used, found)
        pf = queue.path_freq
        for path, cnt in paths:
            k = path.to_bytes(8, "big")
            pf[k] = pf.get(k, 0) + cnt
        if queue.track_edges:
            ef = queue.edge_freq
            for i, cnt in edges:
                ef[i] = ef.get(i, 0) + cnt
        return finds

    def take_finds(e, finds):
        """After a batch: keep its new coverage as queue entries, save its crashes."""
        nonlocal new_cov
        for reason, status, insns, nb, nc, path, data, hits in finds:
            if reason:
                save_crash(data, reason, status, counted=True, novel=True)
                continue
            edges[0] += nb                # ember judged it: its record is the only one
            c = add_entry(data, hits, insns, path, nb, parent=e, op="havoc")
            if nb:
                new_cov += 1
            log("new coverage +%d buckets, +%d classes (queue=%d, %d bytes) %s"
                % (nb, nc, len(queue.entries), len(data), c.name))
            emit_event("find", "queue=%d %s" % (len(queue.entries), c.name), edges=nb)

    def probe(data, cmp=False):
        """Send one input and return ember's verdict, with no accounting:
        ('crash', reason, status) | ('iter', hits, insns) | ('other', None, None).
        hits: (index, count) pairs, ascending; insns 0 when ember sent a whole map.
        cmp: as CMP_INPUT; its compare records land in last_cmps[0]."""
        ident = next_id[0]
        next_id[0] += 1
        last_judge[0] = None
        send_frame(conn, CMP_INPUT if cmp else INPUT, struct.pack(">QI", ident, len(data)) + data)
        while True:
            ftype, payload = recv_frame(conn)
            if ftype == BYE:
                raise EmberBye()
            if ftype == JUDGE:
                if len(payload) < JUDGE_LEN:
                    raise ProtocolError("JUDGE payload is %d bytes (needs %d)"
                                        % (len(payload), JUDGE_LEN))
                jid, nb, nc, path, novel = struct.unpack(">QIIQB", payload[:JUDGE_LEN])
                if jid != ident:
                    raise ProtocolError("JUDGE for id=%d while input id=%d is running"
                                        % (jid, ident))
                last_judge[0] = (nb, nc, path.to_bytes(8, "big"), bool(novel))
                continue
            if ftype == CMPS:
                try:
                    cid, _dropped, recs = parse_cmps(payload)
                except ValueError as e:
                    raise ProtocolError(str(e)) from None
                if cid != ident or not cmp:
                    raise ProtocolError("CMPS for id=%d while input id=%d is a%s"
                                        % (cid, ident, " CMP_INPUT" if cmp else "n INPUT"))
                last_cmps[0] = recs
                continue
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
            if last_judge[0] is None:
                raise ProtocolError("input id=%d was answered without a JUDGE frame "
                                    "(OPTIONS asked for one)" % ident)
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
                r = probe(cand) if cand else None
                if r == ("crash", reason, status):
                    best = cand           # the drop kept the crash
                    continue
                if (r and r[0] == "crash" and r[1] not in HANG_REASONS and r[1] != REJECTED
                        and (r[1], r[2]) not in crash_sigs):
                    # Another bug on the way: keep it (as found; it is not minimized in turn).
                    crash_sigs[(r[1], r[2])] = 1
                    fname = write_finding(crash_dir, "crash", cand, r[1], r[2])
                    log("CRASH %s status=%s NEW while minimizing (saved %s)"
                        % (reason_name(r[1]), fmt_status(r[1], r[2]), fname))
                    emit_event("crash", "%s status=%s %s"
                               % (reason_name(r[1]), fmt_status(r[1], r[2]), fname))
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

    def save_crash(data, reason, status, counted=False, novel=False):
        """counted: a HAVOC batch's totals already counted it. novel: ember judged that the
        crash ran code no earlier crash ran (or has a new (reason, status))."""
        nonlocal crashes, hangs, rejected
        if reason == REJECTED:            # the input route refused it: no bug
            rejected += 0 if counted else 1
            return
        is_hang = reason in HANG_REASONS
        if is_hang:
            hangs += 0 if counted else 1
            nth, sigs, where, label, ev = hangs, hang_sigs, hang_dir, "HANG", "hang"
        else:
            crashes += 0 if counted else 1
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
        elif n > 1 and not (novel and saved_per_sig.get(sig, 0) < args.crashes_per_sig):
            log("%s #%d %s status=%s dup (count=%d, not saved)"   # this bug again: don't flood
                % (label, nth, rname, st, n))
            return
        variant = not is_hang and n > 1   # the same (reason, status) through other code
        if not is_hang:
            saved_per_sig[sig] = saved_per_sig.get(sig, 0) + 1
        prefix = "hang" if is_hang else "crash"
        # Save the input as found first: minimizing re-sends it, and a BYE, a
        # timeout or a dropped connection there must not lose the finding.
        fname = write_finding(where, prefix, data, reason, status)
        # A variant stays as found: minimizing it would lead back to the first one.
        small = data if variant else minimize(data, reason, status)
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
        nb, nc, path, novel = last_judge[0]
        if r[0] == "crash":
            save_crash(data, r[1], r[2], novel=novel)
            return r
        if r[0] != "iter":
            return r
        hits, insns = r[1], r[2]
        queue.note_execution(hits, path)
        edges[0] += nb
        return ("iter", hits, insns, path, nb, nc)

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
                  orig=None, keep_name=None, resumed=None):
        """resumed: a saved entry coming back, with its own id, name and state."""
        e = Entry(id=resumed.id if resumed else next_eid[0], data=data,
                  parent=parent.id if parent else src,
                  depth=parent.depth + 1 if parent else 0, insns=insns,
                  buckets=[i for i, _ in hits], path=pid, found_at=queue.cycle,
                  new_cov=nb > 0)
        if resumed:
            e.parent, e.depth, e.found_at = resumed.parent, resumed.depth, resumed.found_at
            e.times_chosen, e.trimmed, e.new_cov = (resumed.times_chosen, resumed.trimmed,
                                                    resumed.new_cov)
        else:
            next_eid[0] += 1
        e.name = keep_name or entry_name(e.id, src=parent.id if parent else src, op=op,
                                          new_cov=nb > 0, sync=sync, orig=orig)
        if not keep_name:
            save_entry(queue_dir, e)
        queue.add(e)
        corpus_put(e)
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
            corpus_put(e)
        else:
            save_state(queue_dir, e)

    def add_tokens(toks):
        """Constants the target compares with, as dictionary integers (target order)."""
        nonlocal cmplog_tokens
        added = 0
        for value, width in toks:
            if cmplog_tokens >= CMPLOG_TOKENS_MAX:
                break
            if dictionary.add(IntToken(value % (1 << (8 * width)), width, order)):
                cmplog_tokens += 1
                added += 1
        if added:
            send_mutator()

    def i2s(e):
        """The input-to-state stage: e's compares, logged once, each operand written where
        the other one is in the input. Its candidates count as mutated execs."""
        nonlocal mutated, cmplog_execs, cmplog_finds, new_cov
        cmplog_done.add(e.id)
        stage[0] = "cmplog %s" % e.name
        last_cmps[0] = None
        probe(e.data, cmp=True)
        nonlocal_execs(1)
        recs = last_cmps[0] or []
        if not recs:
            return
        add_tokens(i2s_tokens(e.data, recs))
        for cand in i2s_candidates(e.data, recs, args.cmplog_max):
            if stop["flag"] or (args.iters and mutated >= args.iters):
                break
            if len(cand) > args.max_size:
                continue
            r = run(cand)
            mutated += 1
            cmplog_execs += 1
            if r[0] == "iter" and r[5] > 0:
                c = add_entry(cand, r[1], r[2], r[3], r[4], parent=e, op="cmplog")
                cmplog_finds += 1
                if r[4]:
                    new_cov += 1
                log("cmplog: new coverage +%d buckets, +%d classes (queue=%d) %s"
                    % (r[4], r[5], len(queue.entries), c.name))
                emit_event("find", "queue=%d %s" % (len(queue.entries), c.name), edges=r[4])
            maybe_status()
            sync()

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

    last_reweight = [0]
    state_dirty = {}       # id -> entry whose times_chosen changed since its state was saved
    last_state = [time.monotonic()]

    def flush_state(force=False):
        """Save the changed entries' state (resumable campaigns), every few seconds."""
        if not state_dirty or (not force and time.monotonic() - last_state[0] < 5.0):
            return
        for x in state_dirty.values():
            save_state(queue_dir, x)
        state_dirty.clear()
        last_state[0] = time.monotonic()

    def maybe_reweight():
        if not args.reweight_every or mutated - last_reweight[0] < args.reweight_every:
            return
        last_reweight[0] = mutated
        new = opstats.weights(default_weights)
        if new != weights:
            weights.clear()
            weights.update(new)
            send_mutator()

    status = 0
    ember_bye = False
    try:
        # ---- ember's mutator: settings, dictionary, operator table ----
        send_mutator()
        log("ember mutates: %d operators, stack %d-%d, splice %d/1000, max size %d, %d tokens"
            % (len(op_names), stack_lo, stack_hi, args.splice, args.max_size, len(dictionary)))
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
                add_entry(data, r[1], r[2], r[3], r[4], keep_name=src.name, resumed=src)
            elif r[5] or not queue.entries:
                add_entry(data, r[1], r[2], r[3], r[4], orig=src)
            maybe_status()
        next_eid[0] = max([e.id for e in queue.entries], default=-1) + 1
        baseline_edges[0] = edges[0]
        slow = max((e.insns for e in queue.entries), default=0)
        if slow and args.iter_insns == "auto":
            budget = max(5 * slow, 10000)
            send_options(budget)
            log("calibration: the slowest queue entry ran %d instructions; ember's per-input "
                "budget is now %d (--iter-insns auto)" % (slow, budget))
        elif slow:
            log("calibration: the slowest queue entry ran %d instructions; a per-input "
                "budget of about 5x that catches hangs early (--iter-insns auto, or "
                "--iter-insns %d)" % (slow, 5 * slow))
        sync(force=True)
        if not queue.entries:
            log("no queue entry ran normally (every seed crashed, hung or was rejected); "
                "fuzzing from an empty input")
            r = run(b"")
            if r[0] == "iter":
                add_entry(b"", r[1], r[2], r[3], r[4], orig="empty")
        log("baseline: %d queue entries, %d edges; schedule %s"
            % (len(queue.entries), edges[0], args.schedule))

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
                if cmplog_on and e.id not in cmplog_done:
                    i2s(e)
                n = queue.energy(e)
                if n <= 0:
                    continue
                n = max(n, args.min_energy)   # a batch worth its round trip
                stage[0] = "havoc %s (%s, cycle %d)" % (e.name, args.schedule, queue.cycle)
                pending = e.favored and not e.times_chosen
                left = n
                while left > 0 and not limit():
                    k = min(left, args.batch)
                    if args.iters:
                        k = min(k, args.iters - mutated)
                    take_finds(e, havoc(e, k))
                    left -= k
                    maybe_status()
                    maybe_reweight()
                    sync()
                e.times_chosen += 1
                state_dirty[e.id] = e
                flush_state()
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

    flush_state(force=True)
    if not ember_bye:
        try:
            send_frame(conn, BYE)
        except OSError:
            pass
    # Hangs are deduplicated by input, not by (reason, status): unique counts
    # the keys, saved the distinct inputs kept under hangs/.
    log("done: execs=%d new_cov=%d crashes=%d (unique=%d) hangs=%d (unique=%d, "
        "saved=%d) rejected=%d queue=%d favored=%d cycles=%d edges=%d unstable=%d "
        "imported=%d trimmed_bytes=%d cmplog_execs=%d cmplog_finds=%d cmplog_tokens=%d"
        % (execs, new_cov, crashes, len(crash_sigs), hangs, len(hang_sigs),
           len(hang_inputs), rejected, len(queue.entries), queue.favored_count(),
           queue.cycle, edges[0], unstable, imported, trimmed_bytes, cmplog_execs,
           cmplog_finds, cmplog_tokens))
    emit_status(finished=True)
    if status_fp:
        status_fp.close()
    conn.close()
    lsock.close()
    return status


if __name__ == "__main__":
    sys.exit(main())
