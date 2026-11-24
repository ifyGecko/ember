#!/usr/bin/env python3
"""fake_brain.py RUN BOARD FIRMWARE [SITES BOUNDARY] - uam-run --fuzz-net against a scripted brain.

FIRMWARE is a build of tests/fuzz/fuzztarget.h. The brain listens on a free port, starts
uam-run with --fuzz-net, and checks, frame by frame (docs/fuzz-formats.md "Fuzz protocol"):
  - the magic and HELLO (version 2, the arch, map size, scheme flags: the block model N=2
    with --cov-blocks, the site-id N=2 scheme with SITES/BOUNDARY from mk_lists.py);
  - the unsolicited INIT snapshot (id 0, kind INIT) before any reply;
  - COVERAGE kind ITER for a good input, and the same map for the same input;
  - CRASH FAULT (the store's pc), ORACLE (fuzz_panic), TIMEOUT (status 0) for the
    planted bugs;
  - CRASH MALFORMED for a short payload (id 0) and for a len overrun (its id);
  - an unknown frame type being ignored;
  - HELLO's big-endian flag on big-endian boards;
  - OPTIONS asking for SPARSE: the same input's SPARSE list equals its COVERAGE map, with
    indices ascending and the iteration's instruction count; maps stay right across inputs
    (the harness then clears only the buckets it listed); crashes are still CRASH;
  - compare logging: HELLO announces it; a CMP_INPUT before OPTIONS CMP_LOG gets an empty
    CMPS, then its reply; after it, a CMP_INPUT of the magic-value input one bit off gets a
    CMPS (its id, the layout exact) holding the equality compare of the input's word with
    FUZZ_MAGIC, then the same SPARSE map as the plain INPUT; the exact value gets a CMPS
    and CRASH FAULT; a plain INPUT gets no CMPS;
  - BYE ending the run with exit 0, and EOF before BYE being an error;
  - CRASH REJECTED (status 0) for an input the route refuses (mem:...,reject), the
    session going on after it;
  - CRASH GUARD (the pc of the access) for a read of a --guard range;
  - --fuzz-net-timeout giving up on a brain that goes silent;
  - JUDGE (OPTIONS bit 2): before each INPUT's reply, what the run added (new buckets and
    classes, zero the second time), its path (Python's path_hash of the reply) and whether a
    crash is novel; CMPS comes before it;
  - PROBE_INPUT: judged and answered as an INPUT, and recorded not at all: the same input
    again, as a PROBE_INPUT and then as an INPUT, is new each time, and only after the
    INPUT is it not;
  - OPTIONS' budget: a batch of long timeouts sends PROGRESS while it is silent;
  - mutation in ember: HELLO announces it; HAVOC before an accepted MUTATOR, for an unknown
    corpus entry or of 0 mutations gets HAVOC_DONE with that status and nothing runs; a bad
    MUTATOR gets MUTATOR_INFO refusing it, with the operator table (INSERT_BLOCK and RESIZE
    in it, the retired 20 and 21 not); with SWAP_CHUNKS alone on an input of equal bytes
    (a mutation that changes nothing) a batch reports no FIND (an INPUT showed that path
    already), its totals are exact, and its one path is the one Python's path_hash names
    for the INPUT's SPARSE list, with per-bucket counts when asked; a batch of timeouts
    reports as many FINDs as MUTATOR's hang cap; a batch with default weights reports
    well-formed FINDs, and a crash already seen as an INPUT only in the totals;
  - stages in ember: HELLO announces them; a STAGE of an unknown kind, for an unknown
    entry or with a limit of 0 gets HAVOC_DONE with that status, input-to-state without
    compare logging status 4, and nothing runs; an input-to-state stage on the magic-value
    input one bit off sends STAGE_RESULT (its compares, as many candidates as it then
    runs, its tokens) before HAVOC_DONE and reaches the planted FAULT; a limit bounds it;
    a trim returns a shorter entry that starts as the entry did and, sent as an INPUT,
    runs on the path and in the instructions STAGE_RESULT said.
Exit status 0 when everything holds.
"""
import os
import socket
import struct
import subprocess
import sys
import time

HELLO, INPUT, COVERAGE, CRASH, BYE, OPTIONS, SPARSE, CMP_INPUT, CMPS = 1, 2, 3, 4, 5, 6, 7, 8, 9
MUTATOR, MUTATOR_INFO, CORPUS, HAVOC, FIND, HAVOC_DONE = 10, 11, 12, 13, 14, 15
JUDGE, PROGRESS, STAGE, STAGE_RESULT = 16, 17, 18, 19
PROBE_INPUT = 21
STAGE_I2S, STAGE_TRIM = 1, 2
OPT_JUDGE = 4
FLAG_BIG_ENDIAN = 1 << 15
FLAG_CMP = 1 << 14
FLAG_HAVOC = 1 << 13
FLAG_STAGES = 1 << 12
SWAP_CHUNKS, INSERT_BLOCK, RESIZE = 17, 23, 24
OPT_SPARSE, OPT_CMP_LOG = 1, 2
CMP_EQ = 1
FUZZ_MAGIC = 0x6A3C91E5
MAGIC_IN = b"Mabc" + struct.pack("<I", FUZZ_MAGIC)
NEAR_IN = b"Mabc" + struct.pack("<I", FUZZ_MAGIC ^ 0x01000000)   # one bit off
BIG_ENDIAN_BOARDS = ("mips-ref", "armeb-ref", "ppc-ref")
FAULT, GUARD, ORACLE, HANG, TIMEOUT, REJECTED, MALFORMED = 1, 2, 3, 4, 5, 6, 7
# RAM the fuzz target does not use: rv32-ref and the MIPS boards (kseg0) reach it at
# 0x81f00000; the ARM and PowerPC boards' RAM starts at 0.
SCRATCH_OF = {"arm-ref": 0x03f00000, "armeb-ref": 0x03f00000, "ppc-ref": 0x03f00000}
SCRATCH_DEFAULT = 0x81f00000
FLAG_SCHEME = 0x1
FLAG_BLOCK = 0x8

failures = []


def check(cond, what):
    if not cond:
        failures.append(what)
    return cond


def sym(fw, name):
    out = subprocess.run([os.environ.get("READELF", "readelf"), "-sW", fw], capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        f = line.split()
        if len(f) >= 8 and f[7] == name:
            return int(f[1], 16)
    sys.exit(f"fake_brain: no symbol {name} in {fw}")


def recv_exact(c, n):
    b = b""
    while len(b) < n:
        chunk = c.recv(n - len(b))
        if not chunk:
            raise EOFError
        b += chunk
    return b


def recv_frame(c):
    (n,) = struct.unpack(">I", recv_exact(c, 4))
    body = recv_exact(c, n)
    return body[0], body[1:]


def send_frame(c, t, payload=b""):
    c.sendall(struct.pack(">IB", len(payload) + 1, t) + payload)


def send_input(c, iid, data):
    send_frame(c, INPUT, struct.pack(">QI", iid, len(data)) + data)


def coverage(p):
    iid, kind, size = struct.unpack(">QBI", p[:13])
    return iid, kind, p[13:13 + size], size


def sparse(p):
    """A SPARSE payload as (id, kind, insns, map rebuilt from the list, map_size, indices)."""
    iid, kind, insns, size, n = struct.unpack(">QBQII", p[:25])
    m = bytearray(size)
    idx = []
    for k in range(n):
        i, cnt = struct.unpack(">IB", p[25 + 5 * k:30 + 5 * k])
        m[i] = cnt
        idx.append(i)
    return iid, kind, insns, bytes(m), size, idx, len(p) == 25 + 5 * n


def cmps(p):
    """A CMPS payload as (id, dropped, [(site, width, kind, a, b)], exact layout)."""
    iid, n, dropped = struct.unpack(">QII", p[:16])
    ents = [struct.unpack(">IBBQQ", p[16 + 22 * k:38 + 22 * k]) for k in range(n)]
    return iid, dropped, ents, len(p) == 16 + 22 * n


sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "fuzz", "tools"))
from fuzz_queue import path_hash  # noqa: E402


def mutator(max_size=64, stack=(1, 1), splice=0, hangs=0, weights=(), tokens=b"\0\0\0\0"):
    """A MUTATOR payload; weights: (op, weight) pairs; tokens: the encoded token list."""
    return (struct.pack(">IHHHIH", max_size, stack[0], stack[1], splice, hangs, len(weights))
            + b"".join(struct.pack(">BH", o, w) for o, w in weights) + tokens)


def mutator_info(p):
    """(status, message, {op: (default, weight, name)})."""
    st, ml = struct.unpack(">BH", p[:3])
    msg = p[3:3 + ml].decode()
    q = 3 + ml
    (n,) = struct.unpack(">H", p[q:q + 2])
    q += 2
    ops = {}
    for _ in range(n):
        op, dw, w, nl = struct.unpack(">BHHB", p[q:q + 6])
        ops[op] = (dw, w, p[q + 6:q + 6 + nl].decode())
        q += 6 + nl
    return st, msg, ops, q == len(p)


def find(p):
    """(id, reason, status, insns, ops, data, hits, exact layout, (new_buckets, new_classes, path))."""
    fid, reason, status, insns, ops, nb, nc, path, dlen = struct.unpack(">QBQQQIIQI", p[:53])
    data = p[53:53 + dlen]
    (n,) = struct.unpack(">I", p[53 + dlen:57 + dlen])
    hits = list(struct.iter_unpack(">IB", p[57 + dlen:57 + dlen + 5 * n]))
    return fid, reason, status, insns, ops, data, hits, len(p) == 57 + dlen + 5 * n, (nb, nc, path)


def judged(c):
    """The JUDGE frame and the reply that follows it: ((new_buckets, new_classes, path,
    novel_crash), reply type, reply payload)."""
    t, p = recv_frame(c)
    if t != JUDGE or len(p) != 25:
        raise RuntimeError("expected JUDGE (25 bytes), got type %d, %d bytes" % (t, len(p)))
    jid, nb, nc, path, novel = struct.unpack(">QIIQB", p)
    t, p = recv_frame(c)
    return (jid, nb, nc, path, novel), t, p


def havoc_done(p):
    """(first_id, status, execs, by_reason, finds, {op: (used, found)}, {path: n}, {idx: n}, exact)."""
    first, st, ex = struct.unpack(">QBI", p[:13])
    by = struct.unpack(">7I", p[13:41])
    nf, nops = struct.unpack(">IH", p[41:47])
    q = 47
    ops = {}
    for _ in range(nops):
        op, u, f = struct.unpack(">BII", p[q:q + 9])
        ops[op] = (u, f)
        q += 9
    (npaths,) = struct.unpack(">I", p[q:q + 4])
    paths = dict(struct.iter_unpack(">QI", p[q + 4:q + 4 + 12 * npaths]))
    q += 4 + 12 * npaths
    (nedges,) = struct.unpack(">I", p[q:q + 4])
    edges = dict(struct.iter_unpack(">II", p[q + 4:q + 4 + 8 * nedges]))
    q += 4 + 8 * nedges
    return first, st, ex, by, nf, ops, paths, edges, q == len(p)


progress_seen = []


def run_havoc(c, first, entry, n, seed, flags=0):
    """Send HAVOC; return (finds, done) once HAVOC_DONE arrives (PROGRESS frames noted)."""
    send_frame(c, HAVOC, struct.pack(">QIIQI", first, entry, n, seed, flags))
    finds = []
    while True:
        t, p = recv_frame(c)
        if t == PROGRESS:
            progress_seen.append(struct.unpack(">QI", p))
        elif t == FIND:
            finds.append(find(p))
        elif t == HAVOC_DONE:
            return finds, havoc_done(p)
        else:
            raise RuntimeError("frame type %d in a HAVOC batch" % t)


def run_stage(c, first, entry, kind, limit, flags=0):
    """Send STAGE; return (finds, STAGE_RESULT payload or None, done) once HAVOC_DONE arrives."""
    send_frame(c, STAGE, struct.pack(">QIBII", first, entry, kind, limit, flags))
    finds, result = [], None
    while True:
        t, p = recv_frame(c)
        if t == PROGRESS:
            continue
        if t == FIND:
            finds.append(find(p))
        elif t == STAGE_RESULT:
            result = p
        elif t == HAVOC_DONE:
            return finds, result, havoc_done(p)
        else:
            raise RuntimeError("frame type %d in a stage" % t)


def stage_checks(c, tag):
    """Stages in ember (STAGE, STAGE_RESULT). SPARSE and compare logging are on, a MUTATOR
    was accepted."""
    send_frame(c, CORPUS, struct.pack(">II", 8, len(NEAR_IN)) + NEAR_IN)
    long_in = b"hello world" + b"x" * 53
    send_frame(c, CORPUS, struct.pack(">II", 9, len(long_in)) + long_in)
    # Refusals: nothing runs, no STAGE_RESULT.
    for what, args, want in (("an unknown kind", (8, 7, 16), 3), ("an unknown entry", (77, STAGE_I2S, 16), 2),
                             ("a limit of 0", (8, STAGE_TRIM, 0), 3),
                             ("a limit over the maximum", (8, STAGE_I2S, 4097), 3)):
        _, res, done = run_stage(c, 5000, *args)
        check(res is None and done[0] == 5000 and done[1] == want and done[2] == 0,
              f"{tag}: STAGE with {what}: status {want}, nothing ran ({done[:3]})")
    send_frame(c, OPTIONS, struct.pack(">I", OPT_SPARSE))
    _, res, done = run_stage(c, 5000, 8, STAGE_I2S, 16)
    check(res is None and done[1] == 4 and done[2] == 0,
          f"{tag}: input-to-state without compare logging: status 4 ({done[:3]})")
    send_frame(c, OPTIONS, struct.pack(">I", OPT_SPARSE | OPT_CMP_LOG))
    # Input-to-state on the magic value one bit off.
    finds, res, done = run_stage(c, 6000, 8, STAGE_I2S, 256)
    if check(res is not None and len(res) >= 23 and done[1] == 0, f"{tag}: an i2s stage ran ({done[:3]})"):
        first, kind, compares, _dropped, cands, ntok = struct.unpack(">QBIIIH", res[:23])
        check(first == 6000 and kind == STAGE_I2S and len(res) == 23 + 9 * ntok,
              f"{tag}: STAGE_RESULT's layout (i2s, {len(res)} bytes, {ntok} tokens)")
        check(compares >= 1 and cands >= 1 and done[2] == cands + 1 and sum(done[3]) == done[2],
              f"{tag}: i2s: the entry's run and its {cands} candidates ({compares} compares, {done[2]} runs)")
        check(done[3][FAULT] >= 1, f"{tag}: i2s reaches the bug behind the magic value ({done[3]})")
        check(all(w in (2, 4, 8) for w, _ in struct.iter_unpack(">BQ", res[23:])),
              f"{tag}: i2s tokens are 2, 4 or 8 bytes wide")
        check(all(f[7] and 6000 <= f[0] <= 6256 and f[4] == 0 for f in finds) and done[4] == len(finds)
              and not done[5], f"{tag}: a stage's FINDs carry no operators ({len(finds)})")
    _, res, done = run_stage(c, 6500, 8, STAGE_I2S, 1)
    check(res is not None and done[2] == 2 and struct.unpack(">I", res[17:21])[0] == 1,
          f"{tag}: i2s with a limit of 1: two runs ({done[2]})")
    # Trimming.
    send_frame(c, OPTIONS, struct.pack(">I", OPT_SPARSE | OPT_CMP_LOG | OPT_JUDGE))
    _, res, done = run_stage(c, 7000, 9, STAGE_TRIM, 64, 1)
    if check(res is not None and len(res) >= 30 and done[1] == 0, f"{tag}: a trim ran ({done[:3]})"):
        first, kind, ran, insns, path, dlen = struct.unpack(">QBBQQI", res[:30])
        data = res[30:]
        check(first == 7000 and kind == STAGE_TRIM and ran == 1 and len(data) == dlen,
              f"{tag}: STAGE_RESULT's layout (trim, {len(res)} bytes)")
        check(4 <= dlen < len(long_in) and data[:4] == long_in[:4] and 2 <= done[2] <= 65,
              f"{tag}: the trim cut the entry ({len(long_in)} -> {dlen} bytes in {done[2]} runs)")
        check(path in done[6] and done[7], f"{tag}: a stage's paths and per-bucket executions")
        send_input(c, 7100, data)
        j, t, p = judged(c)
        check(t == SPARSE and j[3] == path and sparse(p)[2] == insns,
              f"{tag}: the trimmed entry runs on the path and in the instructions said")
    _, res, done = run_stage(c, 7200, 9, STAGE_TRIM, 1)
    check(res is not None and done[2] == 2, f"{tag}: a trim with one probe: two runs ({done[2]})")
    send_frame(c, CORPUS, struct.pack(">II", 10, 4) + b"FUZ!")
    _, res, done = run_stage(c, 7300, 10, STAGE_TRIM, 8)
    check(res is not None and res[9] == 0 and res[30:] == b"FUZ!" and done[2] == 1,
          f"{tag}: a crashing entry is not trimmed ({done[2]} runs)")
    send_frame(c, OPTIONS, struct.pack(">I", OPT_SPARSE | OPT_CMP_LOG))


def session(run, board, fw, extra, script):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    srv.listen(1)
    srv.settimeout(0.2)
    port = srv.getsockname()[1]
    args = [run, "-b", board, "--fuzz-crash-pc", hex(sym(fw, "fuzz_panic")), "--fuzz-input", "uart0",
            "--fuzz-iter-insns", "200000", "-n", "10000000", *extra,
            "--fuzz-net", f"127.0.0.1:{port}", fw]
    proc = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE, text=True)
    try:
        for _ in range(150):                 # 30 s, unless uam-run gives up first
            try:
                c, _ = srv.accept()
                break
            except socket.timeout:
                if proc.poll() is not None:
                    raise RuntimeError("uam-run exited before connecting") from None
        else:
            raise RuntimeError("uam-run did not connect")
        c.settimeout(30)
        script(c)
        c.close()
        _, err = proc.communicate(timeout=30)
    except Exception as e:  # noqa: BLE001 - any failure is a test failure
        proc.kill()
        _, err = proc.communicate()
        failures.append(f"{board}: session failed: {e!r}")
    srv.close()
    return proc.returncode, err


def havoc_checks(c, tag, msize, slow):
    """Mutation in ember (MUTATOR, CORPUS, HAVOC, FIND, HAVOC_DONE). SPARSE is on."""
    # JUDGE: ember's judgement of an INPUT's run, before its reply.
    send_frame(c, OPTIONS, struct.pack(">I", OPT_SPARSE | OPT_CMP_LOG | OPT_JUDGE))
    fresh = b"zzzz novel input for judge"
    # PROBE_INPUT: judged as an INPUT is, recorded not at all: twice as a probe, then as an
    # INPUT, the same judgement (which shows something only where the input is new: with
    # block coverage; a site list has seen it all by now)
    probes = []
    for pid in (38, 39):
        send_frame(c, PROBE_INPUT, struct.pack(">QI", pid, len(fresh)) + fresh)
        pj, pt, _ = judged(c)
        probes.append((pj, pt))
    send_input(c, 40, fresh)
    j, t, p = judged(c)
    check(all(pt == SPARSE for _, pt in probes) and [pj[0] for pj, _ in probes] == [38, 39]
          and all(pj[1:4] == j[1:4] for pj, _ in probes),
          f"{tag}: a PROBE_INPUT is judged and recorded not at all ({probes} then {j})")
    hits = None
    if check(t == SPARSE and j[0] == 40, f"{tag}: JUDGE then SPARSE (type {t}, id {j[0]})"):
        _, _, _, m, _, idx, _ = sparse(p)
        hits = [(i, m[i]) for i in idx]
        check(j[1] <= j[2] and j[4] == 0 and j[3] == path_hash(hits),
              f"{tag}: JUDGE's path is path_hash of the reply ({j})")
    send_input(c, 41, fresh)
    j2, t, p = judged(c)
    check(t == SPARSE and j2[1] == 0 and j2[2] == 0 and j2[3] == j[3],
          f"{tag}: the same input again: nothing new, the same path ({j2})")
    send_input(c, 42, b"FUZ!")
    j3, t, p = judged(c)
    check(t == CRASH and j3[1] == 0 and j3[2] == 0 and j3[4] == 0,
          f"{tag}: a crash seen before is not novel and hit no bucket new to crashes ({j3}, type {t})")
    send_frame(c, CMP_INPUT, struct.pack(">QI", 43, len(NEAR_IN)) + NEAR_IN)
    t, p = recv_frame(c)
    check(t == CMPS, f"{tag}: CMPS comes before JUDGE (type {t})")
    j4, t, p = judged(c)
    check(t == SPARSE and j4[0] == 43, f"{tag}: CMP_INPUT: CMPS, JUDGE, reply (type {t})")
    send_frame(c, OPTIONS, struct.pack(">I", OPT_SPARSE | OPT_CMP_LOG))
    # Refusals: nothing runs, the batch is answered at once.
    _, done = run_havoc(c, 100, 5, 4, 1)
    check(done[0] == 100 and done[1] == 1 and done[2] == 0 and done[8],
          f"{tag}: HAVOC before MUTATOR: status 1, nothing ran ({done[:3]})")
    send_frame(c, MUTATOR, mutator(stack=(8, 2)))
    t, p = recv_frame(c)
    r = mutator_info(p) if t == MUTATOR_INFO else None
    if check(r is not None, f"{tag}: MUTATOR_INFO (type {t})"):
        st, msg, ops, exact = r
        check(st == 1 and "stack" in msg and exact, f"{tag}: a bad MUTATOR is refused ({st}, {msg!r})")
        check(INSERT_BLOCK in ops and RESIZE in ops and 20 not in ops and 21 not in ops
              and ops[SWAP_CHUNKS][2] == "SWAP_CHUNKS" and all(v[0] > 0 for v in ops.values()),
              f"{tag}: the operator table ({sorted(ops)})")
        everything = sorted(ops)
    else:
        everything = []
    # SWAP_CHUNKS alone on equal bytes: every mutation is the parent itself.
    send_frame(c, MUTATOR, mutator(weights=[(o, 1 if o == SWAP_CHUNKS else 0) for o in everything],
                                   hangs=1))
    t, p = recv_frame(c)
    check(t == MUTATOR_INFO and mutator_info(p)[0] == 0 and mutator_info(p)[2][SWAP_CHUNKS][1] == 1,
          f"{tag}: MUTATOR accepted, the weight in use reported")
    same = b"AAAAAAAAAAAA"
    send_frame(c, CORPUS, struct.pack(">II", 5, len(same)) + same)
    send_input(c, 30, same)
    t, p = recv_frame(c)
    hits = None
    if check(t == SPARSE, f"{tag}: INPUT of the corpus entry (type {t})"):
        _, _, _, m, _, idx, _ = sparse(p)
        hits = [(i, m[i]) for i in idx]
    _, done = run_havoc(c, 100, 99, 4, 1)
    check(done[1] == 2 and done[2] == 0, f"{tag}: HAVOC of an unknown entry: status 2 ({done[1]})")
    _, done = run_havoc(c, 100, 5, 0, 1)
    check(done[1] == 3 and done[2] == 0, f"{tag}: HAVOC of 0 mutations: status 3 ({done[1]})")
    finds, done = run_havoc(c, 200, 5, 3, 7, flags=1)
    first, st, ex, by, nf, ops, paths, edges, exact = done
    check(first == 200 and st == 0 and ex == 3 and by[0] == 3 and nf == 0 and not finds and exact,
          f"{tag}: an identity batch: 3 runs, no FIND ({done[:5]}, {len(finds)} finds)")
    check(ops == {SWAP_CHUNKS: (3, 0)}, f"{tag}: the batch's operator totals ({ops})")
    if hits is not None:
        check(paths == {path_hash(hits): 3},
              f"{tag}: ember's path name is Python's path_hash ({paths}, {path_hash(hits):#x})")
        check(edges == {i: 3 for i, _ in hits}, f"{tag}: per-bucket executions with HAVOC_EDGES")
    # Timeouts: MUTATOR's cap (1) says how many are reported.
    loop = b"LLLLLLLL"
    send_frame(c, CORPUS, struct.pack(">II", 6, len(loop)) + loop)
    finds, done = run_havoc(c, 300, 6, 3, 1)
    check(done[1] == 0 and done[3][5] == 3 and len(finds) == 1 and finds[0][1] == TIMEOUT
          and finds[0][5] == loop and finds[0][7], f"{tag}: 3 timeouts, 1 FIND (cap 1): "
          f"{done[3]}, {[f[1] for f in finds]}")
    if slow:
        # OPTIONS sets the budget: runs long enough that the batch must say PROGRESS. PROGRESS
        # goes out between runs only, so each run must take more than its 1 s and much less
        # than this socket's timeout: one timed run measures the host, and the budget is sized
        # for about 2 s a run (the same instructions take far longer on a slow or emulated host).
        probe = 10000000
        send_frame(c, OPTIONS, struct.pack(">IQ", OPT_SPARSE | OPT_CMP_LOG, probe))
        t0 = time.monotonic()
        _, done = run_havoc(c, 350, 6, 1, 1)
        rate = probe / max(time.monotonic() - t0, 1e-3)
        check(done[3][5] == 1, f"{tag}: a timed run to the budget ({done[3]})")
        budget = max(20000000, int(rate * 2.0))
        send_frame(c, OPTIONS, struct.pack(">IQ", OPT_SPARSE | OPT_CMP_LOG, budget))
        del progress_seen[:]
        t0 = time.monotonic()
        finds, done = run_havoc(c, 400, 6, 3, 1)
        took = time.monotonic() - t0
        check(done[3][5] == 3 and not finds, f"{tag}: 3 long timeouts past the hang cap ({done[3]})")
        check(took > 1.2 and progress_seen and all(f == 400 and 0 < n < 3 for f, n in progress_seen),
              f"{tag}: a silent batch of {took:.1f} s ({budget} instructions a run) sends "
              f"PROGRESS ({progress_seen})")
        send_frame(c, OPTIONS, struct.pack(">IQ", OPT_SPARSE | OPT_CMP_LOG, 200000))
    # Default weights, a token: well-formed FINDs; FUZ! crashes, known from INPUTs, only count.
    send_frame(c, MUTATOR, mutator(max_size=200, stack=(2, 128), splice=150,
                                   tokens=struct.pack(">IBH", 1, 0, 4) + b"FUZ!"))
    t, p = recv_frame(c)
    check(t == MUTATOR_INFO and mutator_info(p)[0] == 0, f"{tag}: default MUTATOR accepted")
    seed = b"hello world"
    send_frame(c, CORPUS, struct.pack(">II", 7, len(seed)) + seed)
    finds, done = run_havoc(c, 1000, 7, 3000, 12345)
    check(done[1] == 0 and done[2] == 3000 and sum(done[3]) == 3000 and done[4] == len(finds),
          f"{tag}: a 3000-mutation batch's totals ({done[:5]})")
    faults = [f for f in finds if f[1] == FAULT]
    check(done[3][FAULT] > len(faults) and len(faults) < 8,
          f"{tag}: the known FAULT counted ({done[3][FAULT]}), reported again only when it ran "
          f"code no crash had ({len(faults)})")
    good = all(f[7] and 1000 <= f[0] < 4000 and len(f[5]) <= 200 and f[4] for f in finds)
    good &= all(f[1] or (f[3] > 0 and f[6] == sorted(f[6]) and f[6]) for f in finds)
    good &= all(f[1] or (0 < f[8][1] and f[8][0] <= f[8][1] and f[8][2] == path_hash(f[6]))
                for f in finds)
    # a crash: no classes, no path; its new_buckets are the buckets no earlier crash hit,
    # which is what made a FAULT known by its (reason, status) worth reporting again
    good &= all(not f[1] or f[8][1:] == (0, 0) for f in finds)
    good &= all(f[8][0] > 0 for f in faults)
    good &= all(f[8][0] == 0 for f in finds if f[1] in (HANG, TIMEOUT))
    check(good, f"{tag}: FINDs well formed ({len(finds)})")
    used = sum(u for u, _ in done[5].values())
    check(used > 3000 and all(o in everything or o == 25 for o in done[5]),
          f"{tag}: operator totals ({used})")


def main():
    run, board, fw = sys.argv[1:4]
    sites = sys.argv[4:6] if len(sys.argv) >= 6 else None
    panic = sym(fw, "fuzz_panic")
    arch = {"rv32-ref": "rv32imac", "mips-ref": "mips32be", "mipsel-ref": "mips32le",
            "arm-ref": "armv7", "armeb-ref": "armv7be", "ppc-ref": "ppc32"}[board]
    # --cov-blocks with the stock extensions: the block model, N=2 (HELLO says so)
    modes = [("blocks", ["--cov-blocks", "--fuzz-begin", hex(sym(fw, "fuzz_iteration"))],
              FLAG_SCHEME | FLAG_BLOCK | (2 << 8))]
    if sites:
        modes.append(("sites", ["--cov-sites", sites[0], "--cov-boundary", sites[1]],
                      FLAG_SCHEME | (2 << 8)))

    for mode, extra, want_flags in modes:
        tag = f"{board} {mode}"
        st = {}

        def script(c):
            check(recv_exact(c, 4) == b"UAMF", f"{tag}: magic")
            t, p = recv_frame(c)
            check(t == HELLO, f"{tag}: first frame is HELLO")
            ver, flags, msize, alen = struct.unpack(">HHIH", p[:10])
            check(ver == 2 and len(p) == 10 + alen, f"{tag}: HELLO version {ver}, {len(p)} bytes")
            check(p[10:10 + alen].decode() == arch, f"{tag}: HELLO arch {p[10:10 + alen]!r}")
            want = want_flags | FLAG_CMP | FLAG_HAVOC | FLAG_STAGES | (FLAG_BIG_ENDIAN if board in BIG_ENDIAN_BOARDS else 0)
            check(flags == want, f"{tag}: HELLO flags {flags:#x}, expected {want:#x}")
            t, p = recv_frame(c)
            iid, kind, bitmap, size = coverage(p) if t == COVERAGE else (None,) * 4
            check(t == COVERAGE and iid == 0 and kind == 1 and size == msize == len(bitmap),
                  f"{tag}: INIT snapshot (type {t}, id {iid}, kind {kind})")
            check(any(bitmap or b""), f"{tag}: INIT map is empty")

            send_frame(c, 99, b"ignore me")                       # unknown type
            maps = []
            for iid, data in ((1, b"hello"), (6, b"hello")):
                send_input(c, iid, data)
                t, p = recv_frame(c)
                r = coverage(p) if t == COVERAGE else None
                check(r and r[0] == iid and r[1] == 2 and r[3] == msize,
                      f"{tag}: input {iid}: ITER coverage (type {t})")
                if r:
                    maps.append(r[2])
            check(len(maps) == 2 and maps[0] == maps[1] and any(maps[0]),
                  f"{tag}: same input, same non-empty map")

            def crash(iid, data, reason, status=None):
                send_input(c, iid, data)
                t, p = recv_frame(c)
                if not check(t == CRASH and len(p) == 17, f"{tag}: input {iid}: CRASH (type {t})"):
                    return None
                rid, rsn, sts = struct.unpack(">QBQ", p)
                check(rid == iid and rsn == reason, f"{tag}: input {iid}: id {rid} reason {rsn}")
                if status is not None:
                    check(sts == status, f"{tag}: input {iid}: status {sts:#x}, expected {status:#x}")
                return sts

            fault = crash(2, b"FUZ!", FAULT)
            check(fault not in (None, 0), f"{tag}: FAULT status is the pc")
            crash(3, b"FUZ?", ORACLE, panic & 0xffffffff)
            crash(4, b"Lxx", TIMEOUT, 0)
            check(crash(5, b"FUZ!", FAULT) == fault, f"{tag}: FAULT status is stable")

            send_frame(c, INPUT, b"\0\0\0\0\0")                    # < 8 bytes: id 0
            t, p = recv_frame(c)
            check(t == CRASH and p == struct.pack(">QBQ", 0, MALFORMED, 0), f"{tag}: short INPUT")
            send_frame(c, INPUT, struct.pack(">QI", 7, 100) + b"abc")   # len overruns
            t, p = recv_frame(c)
            check(t == CRASH and p == struct.pack(">QBQ", 7, MALFORMED, 0), f"{tag}: overrun INPUT")

            send_input(c, 8, b"ok")                                # still in lockstep
            t, p = recv_frame(c)
            check(t == COVERAGE and coverage(p)[0] == 8, f"{tag}: input after MALFORMED")
            ok_map = coverage(p)[2] if t == COVERAGE else None

            # CMP_INPUT with compare logging off: an empty CMPS, then the usual reply
            send_frame(c, CMP_INPUT, struct.pack(">QI", 20, len(NEAR_IN)) + NEAR_IN)
            t, p = recv_frame(c)
            r = cmps(p) if t == CMPS else None
            check(r and r[0] == 20 and r[2] == [] and r[3], f"{tag}: CMPS before CMP_LOG is empty "
                  f"(type {t})")
            t, p = recv_frame(c)
            check(t == COVERAGE and coverage(p)[0] == 20, f"{tag}: CMP_INPUT reply (type {t})")

            # SPARSE: asked for once, then every map comes as its list of set buckets.
            send_frame(c, OPTIONS, struct.pack(">I", 1))
            for iid, data, want_map in ((9, b"hello", maps[0] if maps else None),
                                        (10, b"ok", ok_map), (11, b"hello", maps[0] if maps else None)):
                send_input(c, iid, data)
                t, p = recv_frame(c)
                if not check(t == SPARSE, f"{tag}: input {iid}: SPARSE reply (type {t})"):
                    break
                rid, kind, insns, m, size, idx, exact = sparse(p)
                check(exact and rid == iid and kind == 2 and size == msize,
                      f"{tag}: input {iid}: SPARSE layout (id {rid}, kind {kind}, size {size})")
                check(idx == sorted(idx) and len(set(idx)) == len(idx), f"{tag}: input {iid}: indices ascending")
                check(insns > 0, f"{tag}: input {iid}: SPARSE carries the instruction count")
                check(m == want_map, f"{tag}: input {iid}: SPARSE list equals the COVERAGE map")
            check(crash(12, b"FUZ?", ORACLE, panic & 0xffffffff) is not None, f"{tag}: CRASH while SPARSE")

            # compare logging on (SPARSE stays on)
            send_frame(c, OPTIONS, struct.pack(">I", OPT_SPARSE | OPT_CMP_LOG))
            send_input(c, 13, NEAR_IN)
            t, p = recv_frame(c)
            near_map = sparse(p)[3] if t == SPARSE else None
            check(near_map is not None, f"{tag}: plain INPUT with CMP_LOG on: SPARSE, no CMPS (type {t})")
            send_frame(c, CMP_INPUT, struct.pack(">QI", 14, len(NEAR_IN)) + NEAR_IN)
            t, p = recv_frame(c)
            r = cmps(p) if t == CMPS else None
            if check(r is not None and r[0] == 14 and r[3], f"{tag}: CMPS for CMP_INPUT 14 (type {t})"):
                # the compiler may compare the word as loaded against the swapped constant
                # (a big-endian core reading the little-endian word), or in 16-bit halves
                # (PowerPC's cmplwi takes 16 bits)
                pairs = []
                for order in ("little", "big"):
                    w = [int.from_bytes(x[4:8], order) for x in (MAGIC_IN, NEAR_IN)]
                    pairs += [{w[0], w[1]}, {w[0] >> 16, w[1] >> 16}, {w[0] & 0xFFFF, w[1] & 0xFFFF}]
                pairs = [q for q in pairs if len(q) == 2]
                hit = [e for e in r[2] if {e[3], e[4]} in pairs]
                if not hit:
                    for e in r[2]:
                        print(f"{tag}: cmps site {e[0]:08x} w{e[1]} kind {e[2]:x} {e[3]:#x} {e[4]:#x}",
                              file=sys.stderr)
                check(hit and hit[0][1] == 32 and hit[0][2] & CMP_EQ,
                      f"{tag}: CMPS holds input word == FUZZ_MAGIC ({len(r[2])} entries)")
                check(len({(e[0], e[3], e[4]) for e in r[2]}) == len(r[2]), f"{tag}: CMPS entries distinct")
            t, p = recv_frame(c)
            check(t == SPARSE and sparse(p)[0] == 14 and sparse(p)[3] == near_map,
                  f"{tag}: CMP_INPUT's map is the plain INPUT's (type {t})")
            send_frame(c, CMP_INPUT, struct.pack(">QI", 15, len(MAGIC_IN)) + MAGIC_IN)
            t, p = recv_frame(c)
            check(t == CMPS and cmps(p)[0] == 15, f"{tag}: CMPS before a crash (type {t})")
            t, p = recv_frame(c)
            check(t == CRASH and len(p) == 17 and struct.unpack(">QBQ", p)[:2] == (15, FAULT),
                  f"{tag}: the magic value: CRASH FAULT (type {t})")

            havoc_checks(c, tag, msize, slow=mode == "blocks" and not st.get("eof"))
            stage_checks(c, tag)

            if st.get("eof"):
                return                                            # close without BYE
            send_frame(c, BYE)
            try:
                extra_frame = c.recv(1)
            except OSError:
                extra_frame = b""
            check(extra_frame == b"", f"{tag}: ember sent more after BYE")

        rc, err = session(run, board, fw, extra, script)
        if not check(rc == 0, f"{tag}: exit {rc} after BYE"):
            sys.stderr.write(err)
        st["eof"] = True
        rc, err = session(run, board, fw, extra, script)
        if not check(rc not in (0, None) and "without BYE" in err, f"{tag}: EOF before BYE: exit {rc}"):
            sys.stderr.write(err)

    blocks = ["--cov-blocks", "--fuzz-begin", hex(sym(fw, "fuzz_iteration"))]

    def handshake(c, tag):
        check(recv_exact(c, 4) == b"UAMF", f"{tag}: magic")
        t, _ = recv_frame(c)
        check(t == HELLO, f"{tag}: HELLO")
        t, p = recv_frame(c)
        check(t == COVERAGE and coverage(p)[0] == 0, f"{tag}: INIT")

    # An input the route refuses: CRASH REJECTED, nothing ran, and the session goes on.
    def rejecting(c):
        tag = f"{board} reject"
        handshake(c, tag)
        send_input(c, 1, b"0123456789")
        t, p = recv_frame(c)
        check(t == CRASH and p == struct.pack(">QBQ", 1, REJECTED, 0),
              f"{tag}: over max: CRASH REJECTED status 0 (type {t}, {p.hex()})")
        send_input(c, 2, b"ok")
        t, p = recv_frame(c)
        rid = struct.unpack(">Q", p[:8])[0] if len(p) >= 8 else None
        check(rid == 2 and not (t == CRASH and p[8] == REJECTED), f"{tag}: within max: it runs")
        send_frame(c, BYE)

    scratch = SCRATCH_OF.get(board, SCRATCH_DEFAULT)
    rc, err = session(run, board, fw, [*blocks, "--fuzz-input", f"mem:{scratch:#x},max=4,reject"],
                      rejecting)
    if not check(rc == 0, f"{board} reject: exit {rc}"):
        sys.stderr.write(err)

    # A guarded buffer: the read that touches it is CRASH GUARD with the pc of the access,
    # a reason of its own beside FAULT.
    def guarded(c):
        tag = f"{board} guard"
        handshake(c, tag)
        pcs = []
        for iid in (1, 2):
            send_input(c, iid, b"hello")
            t, p = recv_frame(c)
            rid, rsn, sts = struct.unpack(">QBQ", p) if t == CRASH and len(p) == 17 else (None,) * 3
            check(rid == iid and rsn == GUARD and sts, f"{tag}: input {iid}: CRASH GUARD with a pc "
                  f"(type {t}, id {rid}, reason {rsn}, status {sts})")
            pcs.append(sts)
        check(pcs[0] == pcs[1], f"{tag}: GUARD status is stable")
        send_frame(c, BYE)

    rc, err = session(run, board, fw, [*blocks, "--guard", f"{sym(fw, 'g_buf'):#x}+256:r,name=g_buf"],
                      guarded)
    if not check(rc == 0, f"{board} guard: exit {rc}"):
        sys.stderr.write(err)

    # A brain that goes silent: --fuzz-net-timeout gives up instead of waiting forever.
    waited = {}

    def silent(c):
        handshake(c, f"{board} silent")
        t0 = time.monotonic()
        c.settimeout(15)
        try:
            while c.recv(4096):
                pass
        except OSError:
            pass
        waited["s"] = time.monotonic() - t0

    rc, err = session(run, board, fw, [*blocks, "--fuzz-net-timeout", "1"], silent)
    if not check(rc not in (0, None) and "--fuzz-net-timeout" in err and waited.get("s", 99) < 10,
                 f"{board} silent: exit {rc} after {waited.get('s')} s"):
        sys.stderr.write(err)

    # A frontend needs fuzzing boundaries.
    r = subprocess.run([run, "-b", board, "--fuzz-net", "127.0.0.1:1", fw], stdin=subprocess.DEVNULL,
                       capture_output=True, text=True, timeout=30)
    check(r.returncode != 0 and "needs --fuzz-begin" in r.stderr, f"{board}: --fuzz-net without fuzzing")

    for f in failures:
        print(f"fake_brain: {f}", file=sys.stderr)
    if not failures:
        print(f"fake_brain: {board}: {os.path.basename(fw)}: uam-run --fuzz-net agrees with the protocol")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
