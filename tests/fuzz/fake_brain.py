#!/usr/bin/env python3
"""fake_brain.py RUN BOARD FIRMWARE [SITES BOUNDARY] - uam-run --fuzz-net against a scripted brain.

FIRMWARE is a build of tests/fuzz/fuzztarget.h. The brain listens on a free port, starts
uam-run with --fuzz-net, and checks, frame by frame (docs/fuzz-formats.md "Fuzz protocol"):
  - the magic and HELLO (version 1, the arch, map size, scheme flags: none with
    --cov-blocks, the site-id N=2 scheme with SITES/BOUNDARY from mk_lists.py);
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
  - --fuzz-net-timeout giving up on a brain that goes silent.
Exit status 0 when everything holds.
"""
import os
import socket
import struct
import subprocess
import sys
import time

HELLO, INPUT, COVERAGE, CRASH, BYE, OPTIONS, SPARSE, CMP_INPUT, CMPS = 1, 2, 3, 4, 5, 6, 7, 8, 9
FLAG_BIG_ENDIAN = 1 << 15
FLAG_CMP = 1 << 14
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

failures = []


def check(cond, what):
    if not cond:
        failures.append(what)
    return cond


def sym(fw, name):
    out = subprocess.run(["readelf", "-sW", fw], capture_output=True, text=True, check=True).stdout
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


def main():
    run, board, fw = sys.argv[1:4]
    sites = sys.argv[4:6] if len(sys.argv) >= 6 else None
    panic = sym(fw, "fuzz_panic")
    arch = {"rv32-ref": "rv32imac", "mips-ref": "mips32be", "mipsel-ref": "mips32le",
            "arm-ref": "armv7", "armeb-ref": "armv7be", "ppc-ref": "ppc32"}[board]
    modes = [("blocks", ["--cov-blocks", "--fuzz-begin", hex(sym(fw, "fuzz_iteration"))], 0)]
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
            check(ver == 1 and len(p) == 10 + alen, f"{tag}: HELLO version {ver}, {len(p)} bytes")
            check(p[10:10 + alen].decode() == arch, f"{tag}: HELLO arch {p[10:10 + alen]!r}")
            want = want_flags | FLAG_CMP | (FLAG_BIG_ENDIAN if board in BIG_ENDIAN_BOARDS else 0)
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
                if not check(t == CRASH and len(p) == 13, f"{tag}: input {iid}: CRASH (type {t})"):
                    return None
                rid, rsn, sts = struct.unpack(">QBI", p)
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
            check(t == CRASH and p == struct.pack(">QBI", 0, MALFORMED, 0), f"{tag}: short INPUT")
            send_frame(c, INPUT, struct.pack(">QI", 7, 100) + b"abc")   # len overruns
            t, p = recv_frame(c)
            check(t == CRASH and p == struct.pack(">QBI", 7, MALFORMED, 0), f"{tag}: overrun INPUT")

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
            check(t == CRASH and len(p) == 13 and struct.unpack(">QBI", p)[:2] == (15, FAULT),
                  f"{tag}: the magic value: CRASH FAULT (type {t})")

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
        check(t == CRASH and p == struct.pack(">QBI", 1, REJECTED, 0),
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
            rid, rsn, sts = struct.unpack(">QBI", p) if t == CRASH and len(p) == 13 else (None,) * 3
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
