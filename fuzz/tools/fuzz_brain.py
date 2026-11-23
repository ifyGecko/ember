#!/usr/bin/env python3
"""fuzz_brain.py -- ember's coverage-guided fuzz brain.

ember runs and mutates the inputs; this chooses what to work on. Start the brain,
then start ember with `uam-run --fuzz-net HOST:PORT ...` (within --accept-timeout
seconds): ember dials in, boots its firmware and says HELLO. The brain mirrors its queue
into ember (CORPUS) and asks for batches of mutations of one entry (HAVOC): ember
mutates, runs and judges them itself and reports only what is worth keeping (FIND) and
the batch's totals (HAVOC_DONE). Trimming and input-to-state are stages ember runs the
same way (STAGE). Single inputs (seeds, calibration, imports) go as INPUT, the minimizer's
cuts as PROBE_INPUT (judged, not recorded), and each gets one COVERAGE (or SPARSE) map or
CRASH verdict. The protocol (UAMF) is specified in
docs/fuzz-formats.md; its constants are in fuzz/formats.h. The design of the campaign is docs/fuzz-ideas.md; its
logic lives in fuzz/tools/fuzz_queue.py.

The brain
  - keeps no coverage record of its own: ember judges every run (JUDGE before each
    INPUT's reply, FIND in a batch) with AFL-style hit-count classes, and replies with
    each iteration's set buckets and instruction count (SPARSE);
  - keeps a disk-backed **queue** under <out>/queue (id:NNNNNN,... names that
    record lineage) and **resumes** it on start;
  - walks the queue in cycles: a **favored** set (the cheapest and the longest entry
    per bucket),
    **calibration** (a new entry runs twice; ember is deterministic, so a
    difference is a harness bug, reported as UNSTABLE), **trimming**, and an
    **energy** per entry from a performance score times its **power schedule**
    (--schedule fast, explore, exploit, coe, rare, lin, quad, seek, mmopt);
  - with **compare logging** (ember announces it in HELLO; --no-cmplog turns it off):
    has ember run each queue entry once with its compares logged and then the
    **input-to-state** replacements it derives from them (uam/fuzz_i2s.h: one operand's
    bytes found in the input, the other written there; an entry slower than the queue's
    average gets proportionally fewer of them, 16 at least), and adds the code's
    constants the input lacks to the dictionary;
  - has ember **mutate** (fuzz/mutate.c: havoc + splice, inputs that grow up to
    --max-size), with an optional ember **dictionary** (--dict, fuzz/tools/fuzz_dict.py;
    integer tokens in the target's byte order, from HELLO), and operator weights it
    **adapts** to what finds coverage. An ember without in-ember mutation and stages
    (HELLO bits 13 and 12) is refused: rebuild it;
  - saves each crash as the **full input plus a .info sidecar** (reason name,
    status, size, hex) so it can be replayed (`uam-run --fuzz-replay`), and
    **deduplicates** by (reason, status): every distinct one is kept. HANG and TIMEOUT
    verdicts go to
    <out>/hangs: every distinct hang input is kept (up to --max-hangs). REJECTED
    (the input route refused the input) is not a bug: it is only counted. A
    finding is written before it is minimized, so it survives a lost connection;
  - **minimizes** a newly found crash (bounded block dropping). Not hangs: every
    probe of one costs ember a full per-input budget (--minimize-timeouts opts in).
    Its probes are counted (execs, and minimize_execs on their own) and the status
    is written while it works, with the stage "minimize ...". Only the first crash
    is minimized at once: one bug often faults at many instructions, and minimizing
    each as it turns up costs more runs than fuzzing. The later ones are saved as
    found all the same and minimized one by one within --minimize-share percent of
    the runs, those that ran the most code no earlier crash ran first. A crash
    another worker already holds minimized is saved as found and not minimized again
    (fuzz/tools/fuzz_crashes.py; the .info says minimized=...);
  - with --sync-dir and --name, **shares work** with other workers: it imports
    their new queue entries that are new to it (fuzz/tools/fuzz_launch.py runs a
    whole parallel campaign). An outside helper (an LLM) can act as a peer by the
    same route -- it writes inputs into its own SYNC_DIR/NAME/queue, which this
    brain imports; --import-log records what each imported input did (new coverage,
    none, kept, a crash), and an input named with ,keep is queued even without new
    coverage. --watch-dict FILE adds dictionary tokens live (see docs/fuzzing.md,
    "LLM-assisted fuzzing", and fuzz/tools/fuzz_frontier.py for what is uncovered);
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
                [--name NAME --sync-dir DIR [--sync-interval S]] [--import-log FILE]
                [--watch-dict FILE]... [--watch-dict-interval S]
                [--max-size N] [--no-minimize] [--minimize-timeouts]
                [--minimize-share PERCENT]
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
from fuzz_crashes import LATER, NOW, Triage, read_info, saved_crashes, waiting  # noqa: E402
from fuzz_dict import Dictionary, IntToken, parse_dict  # noqa: E402
from fuzz_queue import (SCHEDULES, Entry, OpStats, Queue, entry_name,  # noqa: E402
                        hits_from_map, load_queue, new_peer_inputs, peers, save_entry,
                        save_state, set_synced, synced_upto)

# ---- the fuzz protocol (fuzz/formats.h, docs/fuzz-formats.md) ----------
NET_MAGIC = b"UAMF"
NET_VERSION = 2                          # 2: 64-bit crash status, PROBE_INPUT
NET_FRAME_MAX = 64 << 20                 # bytes in one frame, type included
HELLO, INPUT, COVERAGE, CRASH, BYE, OPTIONS, SPARSE, CMP_INPUT, CMPS = 1, 2, 3, 4, 5, 6, 7, 8, 9
MUTATOR, MUTATOR_INFO, CORPUS, HAVOC, FIND, HAVOC_DONE = 10, 11, 12, 13, 14, 15
JUDGE, PROGRESS, STAGE, STAGE_RESULT = 16, 17, 18, 19
UNINIT = 20                              # ember -> brain: a run that depended on memory nothing wrote
PROBE_INPUT = 21                         # an INPUT judged without adding to ember's record
UNINIT_HDR_LEN, UNINIT_TOUCH_LEN = 31, 16
UNINIT_KINDS = ((1, "verdict"), (2, "detail"), (4, "insns"), (8, "path"), (16, "output"))
UNINIT_VERDICTS = ("ok", "fault", "oracle", "hang", "timeout", "rejected", "guard")
FIND_HDR_LEN, JUDGE_LEN = 53, 25
OPT_JUDGE = 1 << 2                       # OPTIONS: a JUDGE frame before each INPUT's reply
HAVOC_EDGES = 1 << 0                     # HAVOC flags: HAVOC_DONE lists executions per bucket
HAVOC_STATUS = {1: "no accepted MUTATOR", 2: "no such corpus entry", 3: "a bad batch size",
                4: "compare logging is off"}
HAVOC_MAX = 1 << 20                      # most mutations in one HAVOC
STAGE_I2S, STAGE_TRIM = 1, 2             # STAGE kinds
STAGE_I2S_MAX, STAGE_TRIM_MAX = 4096, 65536   # most candidates / cuts in one STAGE
STAGE_RESULT_HDR_LEN = 9
HELLO_FIXED_LEN, ARCH_MAX = 10, 64
COVERAGE_HDR_LEN, CRASH_LEN, SPARSE_HDR_LEN = 13, 17, 25
PROGRESS_LEN = 12
HELLO_BIG_ENDIAN = 1 << 15               # HELLO flags: the target is big-endian
HELLO_CMP = 1 << 14                      # HELLO flags: compare logging is understood
HELLO_HAVOC = 1 << 13                    # HELLO flags: MUTATOR, CORPUS and HAVOC are understood
HELLO_STAGES = 1 << 12                   # HELLO flags: STAGE is understood
OPT_SPARSE = 1 << 0                      # OPTIONS: reply with SPARSE, not COVERAGE
OPT_CMP_LOG = 1 << 1                     # OPTIONS: instrument compares (input-to-state)
CMPLOG_TOKENS_MAX = 256                  # dictionary tokens taken from compare logs
CMPLOG_MIN = 16                          # candidates a slow entry still gets (--cmplog-max if lower)
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
    ap.add_argument("--watch-dict", action="append", default=[], metavar="FILE",
                    help="a dictionary file re-read while the campaign runs: new tokens are "
                         "merged into ember's mutator live (for an outside helper, an LLM, "
                         "that discovers constants); repeatable, same format as --dict")
    ap.add_argument("--watch-dict-interval", type=float, default=5.0, metavar="S",
                    help="seconds between re-reads of a --watch-dict file (default 5)")
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
    ap.add_argument("--import-log", default=None, metavar="FILE",
                    help="append one JSON line per imported peer input (its verdict: new "
                         "coverage, no coverage, kept, a crash or a hang), so an outside "
                         "helper that writes inputs into a peer queue learns what each did. "
                         "Off unless given; see docs/fuzzing.md")
    ap.add_argument("--no-minimize", dest="minimize", action="store_false",
                    help="skip crash minimization (on by default)")
    ap.add_argument("--minimize-timeouts", action="store_true",
                    help="also minimize HANG/TIMEOUT inputs (off by default: "
                         "every probe costs ember a full per-input budget)")
    ap.add_argument("--minimize-share", type=float, default=2.0, metavar="PERCENT",
                    help="most of the runs, in percent, spent minimizing crashes after the "
                         "first, which is minimized at once (default 2; 0: they stay as "
                         "found)")
    ap.add_argument("--crashes-per-sig", type=int, default=4,
                    help="save at most N crashes with one (reason, status): the first, "
                         "minimized, then those that hit a bucket no earlier crash with it had "
                         "hit (default 4)")
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
    if not 0 <= args.minimize_share <= 100:
        sys.exit("brain: --minimize-share is a percentage, 0..100")
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
    if not flags & HELLO_STAGES:
        sys.exit("brain: this ember does not trim or derive input-to-state candidates itself "
                 "(HELLO lacks bit 12): rebuild uam-run from a tree that has fuzz/i2s.c")
    # Ask for SPARSE replies: the hit buckets instead of the whole map. An ember that
    # does not know OPTIONS ignores it and keeps sending COVERAGE; both are handled.
    # Compare logging only from an ember that announced it.
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
    # --watch-dict files are read now and again while the campaign runs; remember each
    # file's mtime+size so an unchanged file is not re-parsed (watch_dicts(), below).
    watch_dict_seen = {}       # path -> (mtime, size) last read
    for p in args.watch_dict:
        try:
            dictionary.merge(parse_dict(p, order))
            st = os.stat(p)
            watch_dict_seen[p] = (st.st_mtime, st.st_size)
        except OSError:
            watch_dict_seen[p] = None          # not there yet: picked up when it appears
        except ValueError as e:
            # As in watch_dicts(): a helper may be part way through writing it.
            watch_dict_seen[p] = None
            log("watch-dict: %s (left for next time)" % e)
    if args.watch_dict:
        log("watching %d dictionary file(s) every %gs for new tokens (%d so far)"
            % (len(args.watch_dict), args.watch_dict_interval, len(dictionary)))

    rng = random.Random(args.seed_rng)
    edges = [0]            # buckets ever hit, as ember judged (JUDGE, FIND)
    last_judge = [None]    # (new_buckets, new_classes, path, novel_crash) of the last INPUT
    queue = Queue(args.schedule)
    opstats = OpStats()
    default_weights = {}   # op -> ember's default weight (MUTATOR_INFO)
    op_names = {}          # op -> ember's name for it
    weights = {}           # op -> the weight in use (adapted by maybe_reweight)

    execs = mutated = new_cov = crashes = hangs = rejected = 0
    uninit_n = 0           # inputs saved under uninit/ (ember started with --fuzz-uninit)
    uninit_details = set()
    unstable = imported = trimmed_bytes = 0
    cmplog_execs = cmplog_finds = cmplog_tokens = 0
    minimize_execs = 0        # runs spent shrinking findings (part of execs, not of mutated)
    deferred_execs = 0        # those of them spent on crashes left for later (--minimize-share)
    triage = Triage(name)     # which crash is minimized, and when (fuzz_crashes.py)
    for left in waiting(crash_dir):   # an earlier session's queue (a restarted worker's)
        triage.defer(*left)
    if len(triage):
        log("%d saved crashes from an earlier session wait to be minimized" % len(triage))
    cmplog_done = set()    # queue entry ids already through the input-to-state stage
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
    # The import log is appended to (not truncated): a helper reads it across sessions.
    import_fp = open(args.import_log, "a", buffering=1) if args.import_log else None
    last_status = [0.0]
    stage = ["baseline"]
    running = [0]             # runs of the batch in flight (PROGRESS); execs has the finished ones
    baseline_edges = [None]   # edges after the corpus replay; new_edges counts past it
    run_config = {"arch": arch, "scheme": scheme, "map_size": map_size,
                  "proto": ver, "ember": "%s:%d" % peer, "listen": listen_addr,
                  "out": out, "seeds": len(seeds), "resumed": len(resumed),
                  "max_size": args.max_size, "minimize": str(bool(args.minimize)),
                  "schedule": args.schedule, "worker": name, "role": role_name,
                  "cmplog": str(cmplog_on)}
    if args.iters:
        run_config["iters_limit"] = args.iters
    if args.dict or args.watch_dict:
        run_config["dict"] = len(dictionary)
    if args.watch_dict:
        run_config["watch_dict"] = len(args.watch_dict)
    if args.import_log:
        run_config["import_log"] = True

    def emit_status(finished=False):
        if not status_fp:
            return
        el = time.monotonic() - start
        status_fp.write(json.dumps({
            "t": round(el, 1), "execs": execs + running[0],
            "execs_per_sec": round((execs + running[0]) / el, 1) if el > 0 else 0.0,
            "corpus": len(queue.entries), "edges": edges[0], "map_size": map_size,
            "new_edges": edges[0] - baseline_edges[0] if baseline_edges[0] is not None else 0,
            "init_edges": init_edges[0],
            "crashes": crashes, "crashes_unique": len(crash_sigs), "uninit": uninit_n,
            "hangs": hangs, "hangs_unique": len(hang_sigs),
            "hangs_saved": len(hang_inputs), "rejected": rejected,
            "cycles": queue.cycle, "favored": queue.favored_count(),
            "pending_favored": queue.pending_favored, "unstable": unstable,
            "imported": imported, "sparse": sparse_seen[0],
            "cmplog_execs": cmplog_execs, "cmplog_finds": cmplog_finds,
            "minimize_execs": minimize_execs,
            "crashes_deferred": len(triage),
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

    def log_import(record):
        """One line of the import log (--import-log): what became of a peer input. record
        is a dict; t (seconds since this session began), time (Unix seconds, which orders
        lines across sessions) and the worker name are added here."""
        if not import_fp:
            return
        record = dict(record)
        record["t"] = round(time.monotonic() - start, 1)
        record["time"] = round(time.time(), 3)    # the log spans sessions: t restarts each
        record["worker"] = name
        import_fp.write(json.dumps(record) + "\n")

    def save_uninit(payload):
        """An UNINIT frame (ember runs inputs a second time with the memory nothing wrote
        filled, started with --fuzz-uninit): the input and what is known about it go to
        uninit/. These are findings of their own: neither crashes nor coverage."""
        nonlocal uninit_n
        try:
            (uid, kinds, detail, verdict, insns, nt) = struct.unpack(">QIQBQH", payload[:UNINIT_HDR_LEN])
            p = UNINIT_HDR_LEN
            touches = list(struct.iter_unpack(">QQ", payload[p:p + UNINIT_TOUCH_LEN * nt]))
            p += UNINIT_TOUCH_LEN * nt
            (dlen,) = struct.unpack(">I", payload[p:p + 4])
            data = payload[p + 4:p + 4 + dlen]
        except struct.error:
            raise ProtocolError("UNINIT is cut short (%d bytes)" % len(payload)) from None
        if len(touches) != nt or len(data) != dlen or len(payload) != p + 4 + dlen:
            raise ProtocolError("UNINIT's lengths do not add up (%d bytes)" % len(payload))
        if detail in uninit_details:
            return
        uninit_details.add(detail)
        uninit_n += 1
        uninit_dir = os.path.join(out, "uninit")
        os.makedirs(uninit_dir, exist_ok=True)
        fname = "uninit_d%x_%s" % (detail, hashlib.sha1(data).hexdigest()[:12])
        with open(os.path.join(uninit_dir, fname), "wb") as f:
            f.write(data)
        what = ",".join(n for bit, n in UNINIT_KINDS if kinds & bit) or "?"
        vname = UNINIT_VERDICTS[verdict] if verdict < len(UNINIT_VERDICTS) else "v%d" % verdict
        with open(os.path.join(uninit_dir, fname + ".info"), "w") as f:
            f.write("detail=0x%x\ndiffers=%s\nfilled_verdict=%s\nfilled_insns=%d\n"
                    % (detail, what, vname, insns))
            for pc, pa in touches:
                f.write("read_before_write=pc 0x%x at 0x%x\n" % (pc, pa))
            f.write("len=%d\nhex=%s\n" % (len(data), data.hex()))
        log("UNINIT #%d differs=%s (filled run: %s) %s %s"
            % (uninit_n, what, vname,
               "first read pc 0x%x at 0x%x" % touches[0] if touches else "no page read first",
               fname))
        emit_event("uninit", "differs=%s %s" % (what, fname))

    def next_frame():
        """The next frame from ember, past its unsolicited INIT map (noted), UNINIT findings
        (saved) and BYE (raised)."""
        while True:
            ftype, payload = recv_frame(conn)
            if ftype == BYE:
                raise EmberBye()
            if ftype == UNINIT:
                save_uninit(payload)
                continue
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

    def batch(ftype, body, first, span, e):
        """One batch (HAVOC or STAGE) to its HAVOC_DONE; its inputs' ids are first ..
        first + span - 1. Returns (finds, a stage's STAGE_RESULT payload or None, runs):
        the FINDs as (reason, status, insns, new_buckets, new_classes, path, data, hits).
        The totals are accounted here, except for `mutated`."""
        nonlocal execs, crashes, hangs, rejected
        send_frame(conn, ftype, body)
        finds = []
        result = None
        while True:
            ftype, payload = next_frame()
            if ftype == FIND:
                try:
                    (fid, reason, status, insns, ops, nb, nc, path,
                     dlen) = struct.unpack(">QBQQQIIQI", payload[:FIND_HDR_LEN])
                    data = payload[FIND_HDR_LEN:FIND_HDR_LEN + dlen]
                    p = FIND_HDR_LEN + dlen
                    (nh,) = struct.unpack(">I", payload[p:p + 4])
                except struct.error:
                    raise ProtocolError("FIND is cut short (%d bytes)" % len(payload)) from None
                if len(data) != dlen or len(payload) != p + 4 + 5 * nh:
                    raise ProtocolError("FIND's lengths do not add up (%d bytes)" % len(payload))
                if not first <= fid < first + span:
                    raise ProtocolError("FIND id=%d is not in the batch %d..%d"
                                        % (fid, first, first + span - 1))
                hits = list(struct.iter_unpack(">IB", payload[p + 4:]))
                finds.append((reason, status, insns, nb, nc, path.to_bytes(8, "big"), data, hits))
                continue
            if ftype == PROGRESS:         # the batch is alive: the reply timeout starts anew
                if len(payload) < PROGRESS_LEN:
                    raise ProtocolError("PROGRESS payload is %d bytes (needs %d)"
                                        % (len(payload), PROGRESS_LEN))
                pid, sofar = struct.unpack(">QI", payload[:PROGRESS_LEN])
                if pid != first:
                    raise ProtocolError("PROGRESS for batch %d while batch %d runs" % (pid, first))
                running[0] = sofar        # the status counts them before HAVOC_DONE does
                maybe_status()
                continue
            if ftype == STAGE_RESULT:
                if len(payload) < STAGE_RESULT_HDR_LEN or struct.unpack(">Q", payload[:8])[0] != first:
                    raise ProtocolError("STAGE_RESULT does not belong to the stage %d" % first)
                result = payload[8:]      # from the kind on
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
        running[0] = 0
        if st:
            raise ProtocolError("ember refused a batch for queue entry %s: %s"
                                % (e.name, HAVOC_STATUS.get(st, "status %d" % st)))
        execs += ex
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
        return finds, result, ex

    def havoc(e, n):
        """One HAVOC batch: n mutations of e, run and judged by ember. Returns its FINDs."""
        nonlocal mutated, batches
        first = next_id[0]
        next_id[0] += n
        flags = HAVOC_EDGES if queue.track_edges else 0
        finds, _, ex = batch(HAVOC, struct.pack(">QIIQI", first, e.id, n, rng.getrandbits(64), flags),
                             first, n, e)
        batches += 1
        mutated += ex
        return finds

    def run_stage(kind, e, limit):
        """One STAGE on e: the entry's own run and at most `limit` inputs ember derives
        from it. Returns (finds, the STAGE_RESULT payload from its kind on, runs)."""
        first = next_id[0]
        next_id[0] += limit + 1
        flags = HAVOC_EDGES if queue.track_edges else 0
        finds, result, ex = batch(STAGE, struct.pack(">QIBII", first, e.id, kind, limit, flags),
                                  first, limit + 1, e)
        if not result:
            raise ProtocolError("a stage for queue entry %s ended without a STAGE_RESULT" % e.name)
        return finds, result, ex

    def take_finds(e, finds, op="havoc"):
        """After a batch: keep its new coverage as queue entries, save its crashes. Returns
        how many entries it added."""
        nonlocal new_cov
        kept = 0
        for reason, status, insns, nb, nc, path, data, hits in finds:
            if reason:
                save_crash(data, reason, status, counted=True, novel=True, fresh=nb)
                continue
            edges[0] += nb                # ember judged it: its record is the only one
            c = add_entry(data, hits, insns, path, nb, parent=e, op=op)
            kept += 1
            if nb:
                new_cov += 1
            log("%snew coverage +%d buckets, +%d classes (queue=%d, %d bytes) %s"
                % ("" if op == "havoc" else op + ": ", nb, nc, len(queue.entries), len(data), c.name))
            emit_event("find", "queue=%d %s" % (len(queue.entries), c.name), edges=nb)
        return kept

    def probe(data, record=True):
        """Send one input and return ember's verdict, with no accounting:
        ('crash', reason, status) | ('iter', hits, insns) | ('other', None, None).
        hits: (index, count) pairs, ascending; insns 0 when ember sent a whole map.
        record=False: a PROBE_INPUT, judged by ember without adding to its record (an
        input that is not kept as it is: what it ran stays new to the runs after it)."""
        ident = next_id[0]
        next_id[0] += 1
        last_judge[0] = None
        send_frame(conn, INPUT if record else PROBE_INPUT,
                   struct.pack(">QI", ident, len(data)) + data)
        while True:
            ftype, payload = recv_frame(conn)
            if ftype == BYE:
                raise EmberBye()
            if ftype == UNINIT:
                save_uninit(payload)
                continue
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
            insns = 0
            if ftype == CRASH:
                if len(payload) < CRASH_LEN:
                    raise ProtocolError("CRASH payload is %d bytes (needs %d)"
                                        % (len(payload), CRASH_LEN))
                cid, reason, status = struct.unpack(">QBQ", payload[:CRASH_LEN])
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
        a detail code) is the bug's identity. A readable tmin, not an optimal one.
        Its runs are runs like any other: each one counts in execs (and in minimize_execs,
        which tells them apart), and the status is written while it works. The totals of
        crashes and hangs stay what fuzzing found: a probe that fails the same way again is
        this finding once more, not another one.

        The cuts go as PROBE_INPUTs: ember judges them without adding them to its record,
        so a cut that is thrown away does not make what it ran look old to later crashes.
        A cut that crashes elsewhere is a find like any other: a new (reason, status), or
        a known one reached through buckets no crash of it had hit (ember's novel_crash),
        goes to save_crash, and once saved it runs again as an INPUT to be recorded. So
        does the result."""
        nonlocal execs, minimize_execs
        if not args.minimize or len(data) <= 1:
            return data
        if reason in HANG_REASONS and not args.minimize_timeouts:
            return data                   # each probe would cost a timeout
        best = data
        chunk = max(1, len(best) // 2)
        budget = 200                      # cap re-sends so minimize stays cheap
        was = stage[0]
        announced = False
        what = "minimize %s status=%s" % (reason_name(reason), fmt_status(reason, status))
        while chunk >= 1 and budget > 0:
            i = 0
            while i < len(best) and budget > 0:
                cand = best[:i] + best[i + chunk:]
                budget -= 1
                stage[0] = "%s (%d bytes, %d probes left)" % (what, len(best), budget)
                if not announced:
                    announced = True
                    emit_status()         # the stage shows at once, not an interval later
                    last_status[0] = time.monotonic()
                r = probe(cand, record=False) if cand else None
                if r:
                    execs += 1
                    minimize_execs += 1
                    maybe_status()
                if r == ("crash", reason, status):
                    best = cand           # the drop kept the crash
                    continue
                if r and r[0] == "crash" and r[1] not in HANG_REASONS and r[1] != REJECTED:
                    # Another bug on the way, or another way into a known one: keep it as
                    # found (its turn to be minimized comes later), and have ember record it.
                    # (A known one past --crashes-per-sig is left alone: a cut is not a
                    # crash fuzzing found, so it is neither saved nor counted.)
                    nb, _, _, novel = last_judge[0]
                    sig = (r[1], r[2])
                    if ((sig not in crash_sigs
                         or novel and saved_per_sig.get(sig, 0) < args.crashes_per_sig)
                            and save_crash(cand, r[1], r[2], novel=novel, nested=True, fresh=nb)):
                        record(cand)
                i += chunk
            chunk //= 2
        stage[0] = was
        if best is not data and reason not in HANG_REASONS:
            record(best)                  # kept: ember's record takes what it ran
        return best

    def record(data):
        """Run a kept input again as an INPUT, so that ember's record holds what it ran
        (it ran first as a PROBE_INPUT). Counted as a minimizing run."""
        nonlocal execs, minimize_execs
        probe(data)
        execs += 1
        minimize_execs += 1

    def write_finding(where, prefix, data, reason, status, minimized="no", origin=None):
        """Write one finding plus its .info sidecar; return the file name. minimized: yes |
        no (kept as found) | later (in the queue) | peer (another worker holds it minimized).
        origin, when set, is the imported input that produced this finding: it is recorded
        in the sidecar (src_peer, src_id, src_orig) so a crash from a helper's input names
        the input that found it."""
        fname = "%s_r%d_s%d_%s" % (prefix, reason, status,
                                   hashlib.sha1(data).hexdigest()[:12])
        if read_info(os.path.join(where, fname + ".info")).get("minimized") == "yes":
            minimized = "yes"             # this very input, minimized before: it stays so
        with open(os.path.join(where, fname), "wb") as f:
            f.write(data)
        with open(os.path.join(where, fname + ".info"), "w") as f:
            f.write("reason=%d\nreason_name=%s\nstatus=%d\n"
                    % (reason, reason_name(reason), status))
            if reason in HEX_STATUS:
                f.write("status_hex=%s\n" % fmt_status(reason, status))
            f.write("minimized=%s\n" % minimized)
            f.write("len=%d\nsig=r%d/s%d\nhex=%s\n"
                    % (len(data), reason, status, data.hex()))
            if origin:
                f.write("src_peer=%s\nsrc_id=%d\n" % (origin["peer"], origin["id"]))
                if origin.get("orig"):
                    f.write("src_orig=%s\n" % origin["orig"])
        return fname

    def drop_finding(where, fname):
        for n in (fname, fname + ".info"):
            try:
                os.unlink(os.path.join(where, n))
            except OSError:
                pass

    def on_disk():
        """The crashes saved so far: this worker's (an earlier session's too) and, in a
        parallel campaign, the other workers'."""
        dirs = [(name, crash_dir)]
        if args.sync_dir:
            dirs += [(p, os.path.join(args.sync_dir, p, "crashes"))
                     for p in peers(args.sync_dir, name)]
        return saved_crashes(dirs)

    def minimize_deferred():
        """Minimize one crash left for later, if the share of runs allows one now."""
        nonlocal deferred_execs
        if not args.minimize or not triage.due(execs, deferred_execs, args.minimize_share):
            return
        fname, data, reason, status = triage.pop()
        rname, st = reason_name(reason), fmt_status(reason, status)
        if not os.path.exists(os.path.join(crash_dir, fname)):
            return                        # removed by hand: nothing to replace
        # Carry the provenance the as-found sidecar recorded through re-minimizing, so a
        # crash from an imported input keeps naming the input that found it.
        info = read_info(os.path.join(crash_dir, fname + ".info"))
        origin = ({"peer": info["src_peer"], "id": int(info.get("src_id", -1)),
                   "orig": info.get("src_orig")} if info.get("src_peer") else None)
        who = triage.held_by(reason, status, on_disk())
        if who == name:
            return                        # queued twice (found again after a restart)
        if who is not None:
            write_finding(crash_dir, "crash", data, reason, status, "peer", origin=origin)
            log("CRASH %s status=%s stays as found: %s holds it minimized (%s)"
                % (rname, st, who, fname))
            return
        before = minimize_execs
        small = minimize(data, reason, status)
        deferred_execs += minimize_execs - before
        if small != data:
            drop_finding(crash_dir, fname)
        new = write_finding(crash_dir, "crash", small, reason, status, "yes", origin=origin)
        log("CRASH %s status=%s minimized later, len=%d -> %d (saved %s; %d left for later)"
            % (rname, st, len(data), len(small), new, len(triage)))

    last_finding_file = [None]   # the file name save_crash last wrote (for the import log)

    def save_crash(data, reason, status, counted=False, novel=False, nested=False, fresh=0,
                   origin=None):
        """counted: a HAVOC batch's totals already counted it. novel: ember judged that the
        crash has a new (reason, status) or hit a bucket no earlier crash with its (reason,
        status) hit. nested: met while minimizing another crash. fresh: how many buckets it
        hit that no earlier crash of any (reason, status) had (ember's count). origin: the
        imported input that produced it, recorded in the sidecar. Returns whether it was
        saved."""
        nonlocal crashes, hangs, rejected
        last_finding_file[0] = None
        if reason == REJECTED:            # the input route refused it: no bug
            rejected += 0 if counted else 1
            return False
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
                return False
            hang_inputs.add(h)
        elif n > 1 and not (novel and saved_per_sig.get(sig, 0) < args.crashes_per_sig):
            log("%s #%d %s status=%s dup (count=%d, not saved)"   # this bug again: don't flood
                % (label, nth, rname, st, n))
            return False
        variant = not is_hang and n > 1   # the same (reason, status) through other code
        if not is_hang:
            saved_per_sig[sig] = saved_per_sig.get(sig, 0) + 1
        prefix = "hang" if is_hang else "crash"
        # Save the input as found first: minimizing re-sends it, and a BYE, a
        # timeout or a dropped connection there must not lose the finding.
        fname = write_finding(where, prefix, data, reason, status, origin=origin)
        note = ""
        if is_hang:
            small = minimize(data, reason, status)
            if small != data:
                drop_finding(where, fname)
                fname = write_finding(where, prefix, small, reason, status, "yes", origin=origin)
                data = small
        elif variant or not args.minimize:
            pass   # a variant stays as found: minimizing it would lead back to the first one
        else:
            # Every (reason, status) is saved; which one is minimized now is another matter.
            when, why = triage.decide(reason, status, on_disk(), nested)
            if when == NOW:
                small = minimize(data, reason, status)
                if small != data:
                    drop_finding(where, fname)
                    data = small
                fname = write_finding(where, prefix, data, reason, status, "yes", origin=origin)
            else:
                fname = write_finding(where, prefix, data, reason, status, when, origin=origin)
                if when == LATER:
                    triage.defer(fname, data, reason, status, fresh)
                note = ", as found: " + why
        last_finding_file[0] = fname
        log("%s #%d %s status=%s NEW len=%d (saved %s%s)"
            % (label, nth, rname, st, len(data), fname, note))
        emit_event(ev, "%s status=%s %s" % (rname, st, fname))
        return True

    def run(data, origin=None):
        """Run one input and account for it: ('crash', ...) after saving it, or
        ('iter', hits, insns, path, new_buckets, new_classes) | ('other', ...). origin,
        when set, is where an imported input came from (sync()): it reaches the crash's
        .info sidecar and the import log."""
        nonlocal execs
        r = probe(data)
        execs += 1
        nb, nc, path, novel = last_judge[0]
        if r[0] == "crash":
            save_crash(data, r[1], r[2], novel=novel, fresh=nb, origin=origin)
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
        """Have ember cut chunks out of e while its path stays the same (AFL's trim
        stage): smaller entries mutate faster and splice better."""
        nonlocal trimmed_bytes
        e.trimmed = True
        if len(e.data) < 5 or args.trim_probes <= 0:
            save_state(queue_dir, e)
            return
        stage[0] = "trim %s" % e.name
        finds, res, _ = run_stage(STAGE_TRIM, e, min(args.trim_probes, STAGE_TRIM_MAX))
        try:
            kind, ran, insns, _path, dlen = struct.unpack(">BBQQI", res[:22])
        except struct.error:
            raise ProtocolError("STAGE_RESULT is cut short (%d bytes)" % len(res)) from None
        data = res[22:]
        if kind != STAGE_TRIM or len(data) != dlen:
            raise ProtocolError("STAGE_RESULT is not a trim's (%d bytes)" % len(res))
        if ran and len(data) < len(e.data):
            trimmed_bytes += len(e.data) - len(data)
            e.data = data
            e.insns = insns
            save_entry(queue_dir, e)
            queue.rescore(e)
            corpus_put(e)
        else:
            if ran and insns and not e.insns:
                e.insns = insns           # its own run, counted: no longer an unknown cost
                queue.rescore(e)
            save_state(queue_dir, e)
        take_finds(e, finds, op="trim")   # a cut that ran new code is an entry of its own

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
        """The input-to-state stage: ember runs e with its compares logged and then every
        input with one operand written where the other one is. The candidates count as
        mutated execs."""
        nonlocal mutated, cmplog_execs, cmplog_finds
        cmplog_done.add(e.id)
        stage[0] = "cmplog %s" % e.name
        limit = queue.scaled_limit(e, min(args.cmplog_max, STAGE_I2S_MAX), CMPLOG_MIN)
        if args.iters:
            limit = min(limit, args.iters - mutated)
        if limit <= 0:
            return
        finds, res, ex = run_stage(STAGE_I2S, e, limit)
        try:
            kind, _compares, _dropped, _cands, ntok = struct.unpack(">BIIIH", res[:15])
            toks = [(v, w) for w, v in struct.iter_unpack(">BQ", res[15:15 + 9 * ntok])]
        except struct.error:
            raise ProtocolError("STAGE_RESULT is cut short (%d bytes)" % len(res)) from None
        if kind != STAGE_I2S or len(res) != 15 + 9 * ntok:
            raise ProtocolError("STAGE_RESULT is not an input-to-state stage's (%d bytes)" % len(res))
        mutated += max(ex - 1, 0)
        cmplog_execs += max(ex - 1, 0)
        cmplog_finds += take_finds(e, finds, op="cmplog")
        add_tokens(toks)
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
            done = after                  # the highest id run so far in this pass
            finished = False
            try:
                for eid, path in todo:
                    if stop["flag"]:
                        break             # the rest next time
                    try:
                        with open(path, "rb") as f:
                            data = f.read()
                    except OSError:
                        continue
                    base = os.path.basename(path)
                    mo = re.search(r",orig:([^,]*)", base)
                    orig = mo.group(1) if mo else None
                    # ,keep in the name adds the input to the queue even without new coverage:
                    # a helper's structurally-useful input that is not yet a find still becomes
                    # a mutation parent.
                    keep = "keep" in base.split(",")[1:]
                    origin = {"peer": p_name, "id": eid, "orig": orig}
                    r = run(data, origin=origin)
                    done = eid
                    rec = {"peer": p_name, "id": eid, "orig": orig, "size": len(data)}
                    if r[0] == "iter":
                        nb, nc = r[4], r[5]
                        # New coverage is a new bucket or a new hit-count class of one, the
                        # same test as for a seed or a worker's find.
                        if nc or keep:
                            c = add_entry(data, r[1], r[2], r[3], nb, op=None, sync=p_name,
                                          src=eid, orig=orig)
                            got += 1
                            rec.update(verdict="newcov" if nc else "kept", new_buckets=nb,
                                       new_classes=nc, insns=r[2], entry=c.name)
                        else:
                            rec.update(verdict="nocov", new_buckets=0, new_classes=0,
                                       insns=r[2])
                    elif r[0] == "crash":
                        reason, status = r[1], r[2]
                        rec.update(verdict=("rejected" if reason == REJECTED else
                                            "hang" if reason in HANG_REASONS else "crash"),
                                   reason_name=reason_name(reason),
                                   status=fmt_status(reason, status),
                                   file=last_finding_file[0])
                    else:
                        rec.update(verdict="other")
                    log_import(rec)
                else:
                    finished = True       # not cut short by a stop
            finally:
                # Record what this pass ran even when it stops part way (Ctrl-C, BYE, a
                # dropped connection), so a restart neither runs those inputs again nor
                # logs them twice.
                mark = top if finished else done
                if mark > after:
                    set_synced(out, p_name, mark)
                imported += got
            if got:
                log("sync: imported %d of %d new entries from %s" % (got, len(todo), p_name))
                emit_event("info", "imported %d from %s" % (got, p_name))
            if not finished:
                return

    last_watch_dict = [0.0]

    def watch_dicts(force=False):
        """Re-read the --watch-dict files; merge any new tokens into ember's mutator live.
        A file unchanged since its last read (mtime and size) is not re-parsed. A parse
        error is logged and the file is left for next time, not fatal: a helper may be part
        way through writing it (it should write atomically: a temp file then a rename)."""
        if not args.watch_dict:
            return
        now = time.monotonic()
        if not force and now - last_watch_dict[0] < args.watch_dict_interval:
            return
        last_watch_dict[0] = now
        added = 0
        for p in args.watch_dict:
            try:
                st = os.stat(p)
            except OSError:
                continue                   # not there (yet / any more): try again later
            key = (st.st_mtime, st.st_size)
            if watch_dict_seen.get(p) == key:
                continue
            try:
                d = parse_dict(p, order)
            except (OSError, ValueError) as e:
                log("watch-dict: %s: %s (left for next time)" % (p, e))
                continue
            watch_dict_seen[p] = key
            for t in d.tokens:
                if dictionary.add(t):
                    added += 1
        if added:
            send_mutator()
            log("watch-dict: merged %d new token(s) live (%d total)" % (added, len(dictionary)))
            emit_event("info", "dictionary: +%d tokens (%d total)" % (added, len(dictionary)))

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
        watch_dicts(force=True)
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
                    watch_dicts()
                    if not limit():
                        minimize_deferred()
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
        "imported=%d trimmed_bytes=%d cmplog_execs=%d cmplog_finds=%d cmplog_tokens=%d "
        "minimize_execs=%d crashes_deferred=%d"
        % (execs, new_cov, crashes, len(crash_sigs), hangs, len(hang_sigs),
           len(hang_inputs), rejected, len(queue.entries), queue.favored_count(),
           queue.cycle, edges[0], unstable, imported, trimmed_bytes, cmplog_execs,
           cmplog_finds, cmplog_tokens, minimize_execs, len(triage)))
    emit_status(finished=True)
    if status_fp:
        status_fp.close()
    if import_fp:
        import_fp.close()
    conn.close()
    lsock.close()
    return status


if __name__ == "__main__":
    sys.exit(main())
