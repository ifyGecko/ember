#!/usr/bin/env python3
"""fake_brain.py RUN BOARD FIRMWARE [SITES BOUNDARY] - ember's SZR1 executor against a scripted brain.

FIRMWARE is a build of tests/fuzz/fuzztarget.h. The brain listens on a free port, starts
uam-run with --fuzz-szr, and checks, frame by frame (suzzer's docs/FORMATS.md section 4):
  - the magic and HELLO (version 1, ring size 1, the arch, map size, scheme flags: none
    with --cov-blocks, the site-id N=2 scheme with SITES/BOUNDARY from mk_lists.py);
  - the unsolicited INIT snapshot (id 0, kind INIT) before any reply;
  - COVERAGE kind ITER for a good input, and the same map for the same input;
  - CRASH GUEST_FAULT (the store's pc), ORACLE (fuzz_panic), TIMEOUT (status 0) for
    the planted bugs;
  - CRASH MALFORMED for a short payload (id 0) and for a len overrun (its id);
  - an unknown frame type being ignored;
  - BYE ending the run with exit 0, and EOF before BYE being an error;
  - CRASH DELIVERY (status 0) for an input the route refuses (mem:...,reject), the
    session going on after it;
  - --fuzz-szr-timeout giving up on a brain that goes silent.
Exit status 0 when everything holds.
"""
import os
import socket
import struct
import subprocess
import sys
import time

HELLO, INPUT, COVERAGE, CRASH, BYE = 1, 2, 3, 4, 5
GUEST_FAULT, ORACLE, MALFORMED, TIMEOUT, DELIVERY = 5, 6, 4, 8, 3
SCRATCH = 0x81f00000   # RAM on every stock board, unused by the fuzz target
FLAG_SCHEME, FLAG_MT, FLAG_LIVE_EMPTY = 0x4, 0x1, 0x2

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
    iid, gen, kind, size = struct.unpack(">QQBI", p[:21])
    return iid, gen, kind, p[21:21 + size], size


def session(run, board, fw, extra, script):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    srv.listen(1)
    srv.settimeout(0.2)
    port = srv.getsockname()[1]
    args = [run, "-b", board, "--fuzz-crash-pc", hex(sym(fw, "fuzz_panic")), "--fuzz-input", "uart0",
            "--fuzz-iter-insns", "200000", "-n", "10000000", *extra,
            "--fuzz-szr", f"127.0.0.1:{port}", fw]
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
    arch = {"rv32-ref": "rv32", "mips-ref": "mips32be", "mipsel-ref": "mips32le"}[board]
    modes = [("blocks", ["--cov-blocks", "--fuzz-begin", hex(sym(fw, "fuzz_iteration"))], 0)]
    if sites:
        modes.append(("sites", ["--cov-sites", sites[0], "--cov-boundary", sites[1]],
                      FLAG_SCHEME | (2 << 8)))

    for mode, extra, want_flags in modes:
        tag = f"{board} {mode}"
        st = {}

        def script(c):
            check(recv_exact(c, 4) == b"SZR1", f"{tag}: magic")
            t, p = recv_frame(c)
            check(t == HELLO, f"{tag}: first frame is HELLO")
            ver, flags, msize, ring, alen = struct.unpack(">HHIHH", p[:12])
            check(ver == 1 and ring == 1, f"{tag}: HELLO version {ver} ring {ring}")
            check(p[12:12 + alen].decode() == arch, f"{tag}: HELLO arch {p[12:12 + alen]!r}")
            check(flags == want_flags, f"{tag}: HELLO flags {flags:#x}, expected {want_flags:#x}")
            check(not flags & (FLAG_MT | FLAG_LIVE_EMPTY), f"{tag}: MT/LIVE_EMPTY set")
            t, p = recv_frame(c)
            iid, gen0, kind, bitmap, size = coverage(p) if t == COVERAGE else (None,) * 5
            check(t == COVERAGE and iid == 0 and kind == 1 and size == msize == len(bitmap),
                  f"{tag}: INIT snapshot (type {t}, id {iid}, kind {kind})")
            check(any(bitmap or b""), f"{tag}: INIT map is empty")

            send_frame(c, 99, b"ignore me")                       # unknown type
            maps = []
            for iid, data in ((1, b"hello"), (6, b"hello")):
                send_input(c, iid, data)
                t, p = recv_frame(c)
                r = coverage(p) if t == COVERAGE else None
                check(r and r[0] == iid and r[2] == 2 and r[4] == msize,
                      f"{tag}: input {iid}: ITER coverage (type {t})")
                if r:
                    maps.append(r[3])
                    check(r[1] > gen0, f"{tag}: gen did not advance")
            check(len(maps) == 2 and maps[0] == maps[1] and any(maps[0]),
                  f"{tag}: same input, same non-empty map")

            def crash(iid, data, reason, status=None):
                send_input(c, iid, data)
                t, p = recv_frame(c)
                if not check(t == CRASH and len(p) == 13, f"{tag}: input {iid}: CRASH (type {t})"):
                    return None
                rid, rsn, sts = struct.unpack(">QBi", p)
                check(rid == iid and rsn == reason, f"{tag}: input {iid}: id {rid} reason {rsn}")
                if status is not None:
                    check(sts == status, f"{tag}: input {iid}: status {sts:#x}, expected {status:#x}")
                return sts

            fault = crash(2, b"FUZ!", GUEST_FAULT)
            check(fault not in (None, 0), f"{tag}: GUEST_FAULT status is the pc")
            crash(3, b"FUZ?", ORACLE, struct.unpack(">i", struct.pack(">I", panic & 0xffffffff))[0])
            crash(4, b"Lxx", TIMEOUT, 0)
            check(crash(5, b"FUZ!", GUEST_FAULT) == fault, f"{tag}: GUEST_FAULT status is stable")

            send_frame(c, INPUT, b"\0\0\0\0\0")                    # < 8 bytes: id 0
            t, p = recv_frame(c)
            check(t == CRASH and p == struct.pack(">QBi", 0, MALFORMED, 0), f"{tag}: short INPUT")
            send_frame(c, INPUT, struct.pack(">QI", 7, 100) + b"abc")   # len overruns
            t, p = recv_frame(c)
            check(t == CRASH and p == struct.pack(">QBi", 7, MALFORMED, 0), f"{tag}: overrun INPUT")

            send_input(c, 8, b"ok")                                # still in lockstep
            t, p = recv_frame(c)
            check(t == COVERAGE and coverage(p)[0] == 8, f"{tag}: input after MALFORMED")

            if st.get("eof"):
                return                                            # close without BYE
            send_frame(c, BYE)
            try:
                extra_frame = c.recv(1)
            except OSError:
                extra_frame = b""
            check(extra_frame == b"", f"{tag}: executor sent more after BYE")

        rc, err = session(run, board, fw, extra, script)
        if not check(rc == 0, f"{tag}: exit {rc} after BYE"):
            sys.stderr.write(err)
        st["eof"] = True
        rc, err = session(run, board, fw, extra, script)
        if not check(rc not in (0, None) and "without BYE" in err, f"{tag}: EOF before BYE: exit {rc}"):
            sys.stderr.write(err)

    blocks = ["--cov-blocks", "--fuzz-begin", hex(sym(fw, "fuzz_iteration"))]

    def handshake(c, tag):
        check(recv_exact(c, 4) == b"SZR1", f"{tag}: magic")
        t, _ = recv_frame(c)
        check(t == HELLO, f"{tag}: HELLO")
        t, p = recv_frame(c)
        check(t == COVERAGE and coverage(p)[0] == 0, f"{tag}: INIT")

    # An input the route refuses: CRASH DELIVERY, nothing ran, and the session goes on.
    def rejecting(c):
        tag = f"{board} reject"
        handshake(c, tag)
        send_input(c, 1, b"0123456789")
        t, p = recv_frame(c)
        check(t == CRASH and p == struct.pack(">QBi", 1, DELIVERY, 0),
              f"{tag}: over max: CRASH DELIVERY status 0 (type {t}, {p.hex()})")
        send_input(c, 2, b"ok")
        t, p = recv_frame(c)
        rid = struct.unpack(">Q", p[:8])[0] if len(p) >= 8 else None
        check(rid == 2 and not (t == CRASH and p[8] == DELIVERY), f"{tag}: within max: it runs")
        send_frame(c, BYE)

    rc, err = session(run, board, fw, [*blocks, "--fuzz-input", f"mem:{SCRATCH:#x},max=4,reject"],
                      rejecting)
    if not check(rc == 0, f"{board} reject: exit {rc}"):
        sys.stderr.write(err)

    # A brain that goes silent: --fuzz-szr-timeout gives up instead of waiting forever.
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

    rc, err = session(run, board, fw, [*blocks, "--fuzz-szr-timeout", "1"], silent)
    if not check(rc not in (0, None) and "--fuzz-szr-timeout" in err and waited.get("s", 99) < 10,
                 f"{board} silent: exit {rc} after {waited.get('s')} s"):
        sys.stderr.write(err)

    # A frontend needs fuzzing boundaries.
    r = subprocess.run([run, "-b", board, "--fuzz-szr", "127.0.0.1:1", fw], stdin=subprocess.DEVNULL,
                       capture_output=True, text=True, timeout=30)
    check(r.returncode != 0 and "needs --fuzz-begin" in r.stderr, f"{board}: --fuzz-szr without fuzzing")

    for f in failures:
        print(f"fake_brain: {f}", file=sys.stderr)
    if not failures:
        print(f"fake_brain: {board}: {os.path.basename(fw)}: SZR1 executor agrees with the protocol")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
