#!/usr/bin/env python3
"""fuzz_brain.py -- ember's coverage-guided fuzz brain.

ember runs the inputs; this chooses them. Start the brain, then start ember
with `uam-run --fuzz-net HOST:PORT ...` (within --accept-timeout seconds):
ember dials in, boots its firmware, says HELLO, and from then on runs each
INPUT the brain sends and answers it with one COVERAGE map or CRASH verdict.
The protocol (UAMF) is specified in docs/fuzz-formats.md; its constants are
in fuzz/formats.h.

The brain
  - mutates inputs with fuzz/tools/mutate.py (Engine: havoc + splice);
  - folds each returned map into a cumulative one, keeps inputs that reached
    new coverage, and saves + triages the inputs that crashed or hung.

What makes this a campaign rather than a coverage dump:

  * It runs **until you stop it** (Ctrl-C / SIGTERM) or ember says BYE.
    `--iters N` caps the run only if you ask; the default (0) is open-ended.
  * It keeps a **disk-backed corpus** under <out>/corpus and **reloads it on
    start**, so a restart resumes the campaign instead of throwing it away.
  * It saves each crash as the **full input plus a .info sidecar** (reason
    name, status, size, hex) so it can be replayed (`uam-run --fuzz-replay`),
    and **deduplicates** by (reason, status) so one bug does not fill the
    directory with copies. HANG and TIMEOUT verdicts go to <out>/hangs,
    counted apart from crashes; every distinct hang input is kept (up to
    --max-hangs), since (reason, status) says little about one. REJECTED
    (the input route refused the input) is not a bug: it is only counted.
    A finding is written before it is minimized, so it survives a lost
    connection.
  * It **minimizes** a newly found crash (bounded block dropping) before
    saving it. Not hangs: every probe of one costs ember a full per-input
    budget (--minimize-timeouts opts in).
  * The default `--max-size` is 64 KiB, a real input budget (it is unrelated
    to the coverage map size).

Usage:
  python3 fuzz/tools/fuzz_brain.py --listen 127.0.0.1:7000 --seeds seeds/ \\
      --out fuzz_out --status-file fuzz_out/status.jsonl &
  uam-run --fuzz-net 127.0.0.1:7000 ... firmware.elf
  python3 fuzz/tools/status_tui.py fuzz_out/status.jsonl

  fuzz_brain.py --listen HOST:PORT [--seeds DIR] [--out DIR] [--iters N]
                [--max-size N] [--no-minimize] [--minimize-timeouts]
                [--max-hangs N] [--seed-rng N] [--accept-timeout S]
                [--hello-timeout S] [--io-timeout S]
                [--status-file FILE] [--status-interval S] [--quiet]

A missing or empty --seeds directory falls back to built-in seeds (plus any
resumed corpus). --listen with port 0 picks a free port; the log line
"listening on HOST:PORT" names it.

Exit status: 0 after a clean session (--iters reached, a signal, or ember's
BYE), 1 on a protocol error, a timeout or a lost connection.
"""
import argparse
import hashlib
import json
import os
import random
import signal
import socket
import struct
import sys
import time

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mutate import Engine  # noqa: E402

# ---- the fuzz protocol (fuzz/formats.h, docs/fuzz-formats.md) ----------
NET_MAGIC = b"UAMF"
NET_VERSION = 1
NET_FRAME_MAX = 64 << 20                 # bytes in one frame, type included
HELLO, INPUT, COVERAGE, CRASH, BYE = 1, 2, 3, 4, 5
HELLO_FIXED_LEN, ARCH_MAX = 10, 64
COVERAGE_HDR_LEN, CRASH_LEN = 13, 13
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
                         "corpus/; default ./fuzz_out)")
    ap.add_argument("--iters", type=int, default=0,
                    help="stop after N mutated execs (0 = run until stopped)")
    ap.add_argument("--max-size", type=int, default=65536,
                    help="max mutated input bytes (default 65536; an input "
                         "budget, not a protocol limit)")
    ap.add_argument("--no-minimize", dest="minimize", action="store_false",
                    help="skip crash minimization (on by default)")
    ap.add_argument("--minimize-timeouts", action="store_true",
                    help="also minimize HANG/TIMEOUT inputs (off by default: "
                         "every probe costs ember a full per-input budget)")
    ap.add_argument("--max-hangs", type=int, default=64,
                    help="save at most N distinct HANG/TIMEOUT inputs "
                         "(default 64)")
    ap.add_argument("--seed-rng", type=int, default=0xC0FFEE,
                    help="RNG seed (default 0xC0FFEE)")
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
                         "fuzz/tools/status_tui.py)")
    ap.add_argument("--status-interval", type=float, default=1.0,
                    help="seconds between status snapshots (default 1.0)")
    ap.add_argument("--quiet", action="store_true",
                    help="no log lines (errors still go to stderr)")
    args = ap.parse_args()

    host, _, port = args.listen.rpartition(":")
    if not port.isdigit():
        sys.exit("brain: --listen must be HOST:PORT (e.g. 127.0.0.1:7000), got %r"
                 % args.listen)
    out = args.out or os.path.join(os.getcwd(), "fuzz_out")
    crash_dir = os.path.join(out, "crashes")
    hang_dir = os.path.join(out, "hangs")
    corpus_dir = os.path.join(out, "corpus")
    for d in (crash_dir, hang_dir, corpus_dir):
        os.makedirs(d, exist_ok=True)

    def log(*a):
        if not args.quiet:
            print("brain:", *a, flush=True)

    # ---- load seeds, and RESUME any previously saved corpus --------------
    def load_dir(d):
        found = []
        for name in (sorted(os.listdir(d)) if d and os.path.isdir(d) else []):
            p = os.path.join(d, name)
            if os.path.isfile(p) and not name.endswith(".info"):
                with open(p, "rb") as f:
                    found.append(f.read())
        return found

    if args.seeds and not os.path.isdir(args.seeds):
        print("brain: WARNING: seed directory %r does not exist; using built-in seeds"
              % args.seeds, file=sys.stderr)
    seeds = load_dir(args.seeds)
    resumed = load_dir(corpus_dir)
    if resumed:
        log("resumed %d inputs from %s" % (len(resumed), corpus_dir))
    seen_inputs = set()                  # de-dup by content: reload is idempotent
    corpus = []
    for data in seeds + resumed:
        h = hashlib.sha1(data).digest()
        if h not in seen_inputs:
            seen_inputs.add(h)
            corpus.append(data)
    if not corpus:
        corpus = [b"A", b"B", b"C"]
    saved_corpus = set(hashlib.sha1(d).hexdigest() for d in resumed)

    def save_corpus_input(data):
        """Persist a new-coverage input so the campaign is resumable."""
        h = hashlib.sha1(data).hexdigest()
        if h in saved_corpus:
            return
        saved_corpus.add(h)
        with open(os.path.join(corpus_dir, "cov_%s" % h[:16]), "wb") as f:
            f.write(data)

    for data in seeds:                   # so a later session resumes without --seeds
        save_corpus_input(data)

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
    scheme = scheme_str(flags)
    if flags & SCHEME and (flags & NGRAM_MASK) >> NGRAM_SHIFT < 3:
        log("NOTE: coverage scheme is %s; an N>=3 scheme with rotation is more "
            "path-sensitive for fuzzing (uam-run --cov-ngram 3 --cov-rotate)" % scheme)
    log("HELLO version=%d arch=%s map_size=%d flags=0x%x scheme=%s"
        % (ver, arch, map_size, flags, scheme))

    cumulative = bytearray(map_size)
    rng = random.Random(args.seed_rng)

    def splice_donor():
        return rng.choice(corpus) if corpus else b""

    engine = Engine(rng, corpus=splice_donor, max_size=args.max_size)

    execs = new_cov = crashes = hangs = rejected = 0
    init_edges = [None]    # edges in ember's INIT map (boot), once it arrived
    crash_sigs = {}        # (reason, status) -> count; the dedup key
    hang_sigs = {}         # same, for HANG / TIMEOUT
    hang_inputs = set()    # sha1 of every hang input already saved
    next_id = [1]

    # ---- optional JSON-lines status feed (for fuzz/tools/status_tui.py) --
    start = time.monotonic()
    status_fp = open(args.status_file, "w", buffering=1) if args.status_file else None
    last_status = [0.0]
    stage = ["baseline 0/%d" % len(corpus)]
    baseline_edges = [None]   # edges after the corpus replay; new_edges counts past it
    run_config = {"arch": arch, "scheme": scheme, "map_size": map_size,
                  "proto": ver, "ember": "%s:%d" % peer, "listen": listen_addr,
                  "out": out, "seeds": len(seeds), "resumed": len(resumed),
                  "max_size": args.max_size, "minimize": str(bool(args.minimize))}
    if args.iters:
        run_config["iters_limit"] = args.iters

    def emit_status(finished=False):
        if not status_fp:
            return
        el = time.monotonic() - start
        edges = sum(1 for b in cumulative if b)
        status_fp.write(json.dumps({
            "t": round(el, 1), "execs": execs,
            "execs_per_sec": round(execs / el, 1) if el > 0 else 0.0,
            "corpus": len(corpus), "edges": edges, "map_size": map_size,
            "new_edges": edges - baseline_edges[0] if baseline_edges[0] is not None else 0,
            "init_edges": init_edges[0],
            "crashes": crashes, "crashes_unique": len(crash_sigs),
            "hangs": hangs, "hangs_unique": len(hang_sigs),
            "hangs_saved": len(hang_inputs), "rejected": rejected,
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

    def fold(bitmap):
        """Merge bitmap into cumulative; return the count of newly set bytes."""
        newly = 0
        for i, b in enumerate(bitmap):
            if b and not cumulative[i]:
                cumulative[i] = b
                newly += 1
        return newly

    def probe(data):
        """Send one input and return ember's verdict, with no accounting:
        ('crash', reason, status) | ('iter', map, None) | ('other', None, None)."""
        ident = next_id[0]
        next_id[0] += 1
        send_frame(conn, INPUT, struct.pack(">QI", ident, len(data)) + data)
        while True:
            ftype, payload = recv_frame(conn)
            if ftype == BYE:
                raise EmberBye()
            if ftype == CRASH:
                if len(payload) < CRASH_LEN:
                    raise ProtocolError("CRASH payload is %d bytes (needs %d)"
                                        % (len(payload), CRASH_LEN))
                cid, reason, status = struct.unpack(">QBI", payload[:CRASH_LEN])
                if reason == MALFORMED:
                    # ember could not parse our INPUT frame: a bug on this side.
                    raise ProtocolError("ember reports input id=%d as MALFORMED "
                                        "(its reply id %d)" % (ident, cid))
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
                if cid == 0 and kind == KIND_INIT:
                    # The unsolicited INIT map (boot up to the first begin):
                    # not a reply to this input, which is still to come.
                    init_edges[0] = sum(1 for b in bitmap if b)
                    log("ember sent its INIT map (%d edges); not scored" % init_edges[0])
                    continue
            else:
                log("ignoring unexpected frame type %d (%d bytes)"
                    % (ftype, len(payload)))
                continue
            if cid != ident:
                raise ProtocolError("input id=%d answered with id=%d (lockstep "
                                    "broken: one reply per INPUT, echoing its id)"
                                    % (ident, cid))
            if ftype == CRASH:
                return ("crash", reason, status)
            if kind != KIND_ITER:      # only per-input (ITER) coverage is scored
                return ("other", None, None)
            return ("iter", bitmap, None)

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
        name = "%s_r%d_s%d_%s" % (prefix, reason, status,
                                  hashlib.sha1(data).hexdigest()[:12])
        with open(os.path.join(where, name), "wb") as f:
            f.write(data)
        with open(os.path.join(where, name + ".info"), "w") as f:
            f.write("reason=%d\nreason_name=%s\nstatus=%d\n"
                    % (reason, reason_name(reason), status))
            if reason in HEX_STATUS:
                f.write("status_hex=%s\n" % fmt_status(reason, status))
            f.write("len=%d\nsig=r%d/s%d\nhex=%s\n"
                    % (len(data), reason, status, data.hex()))
        return name

    def drop_finding(where, name):
        for n in (name, name + ".info"):
            try:
                os.unlink(os.path.join(where, n))
            except OSError:
                pass

    def save_crash(data, reason, status):
        nonlocal crashes, hangs
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
        name = write_finding(where, prefix, data, reason, status)
        small = minimize(data, reason, status)
        if small != data:
            drop_finding(where, name)
            name = write_finding(where, prefix, small, reason, status)
            data = small
        log("%s #%d %s status=%s NEW len=%d (saved %s)"
            % (label, nth, rname, st, len(data), name))
        emit_event(ev, "%s status=%s %s" % (rname, st, name))

    def run_one(data):
        nonlocal execs, new_cov, rejected
        kind, a, b = probe(data)
        execs += 1
        if kind == "crash":
            if a == REJECTED:             # the input route refused it: no bug
                rejected += 1
                return
            save_crash(data, a, b)
            return
        if kind == "iter":
            newly = fold(a)
            if newly:
                new_cov += 1
                corpus.append(data)
                save_corpus_input(data)
                log("new coverage +%d (corpus=%d) from %r"
                    % (newly, len(corpus), data[:24]))
                emit_event("find", "corpus=%d" % len(corpus), edges=newly)

    status = 0
    ember_bye = False
    try:
        # ---- baseline: run the (resumed) corpus once ----
        emit_status()
        replay = list(corpus)
        for i, s in enumerate(replay, 1):
            if stop["flag"]:
                break
            stage[0] = "baseline %d/%d" % (i, len(replay))
            run_one(s)
            maybe_status()
        baseline_edges[0] = sum(1 for b in cumulative if b)
        stage[0] = "havoc + splice"
        # ---- fuzz: mutate corpus entries until stopped (or --iters) ----
        mutated = 0
        while not stop["flag"]:
            if args.iters and mutated >= args.iters:
                break
            base = rng.choice(corpus) if corpus else b"\n"
            run_one(engine.mutate(base))
            mutated += 1
            maybe_status()
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
    total = sum(1 for b in cumulative if b)
    # Hangs are deduplicated by input, not by (reason, status): unique counts
    # the keys, saved the distinct inputs kept under hangs/.
    log("done: execs=%d new_cov=%d crashes=%d (unique=%d) hangs=%d (unique=%d, "
        "saved=%d) rejected=%d corpus=%d edges=%d"
        % (execs, new_cov, crashes, len(crash_sigs), hangs, len(hang_sigs),
           len(hang_inputs), rejected, len(corpus), total))
    emit_status(finished=True)
    if status_fp:
        status_fp.close()
    conn.close()
    lsock.close()
    return status


if __name__ == "__main__":
    sys.exit(main())
