#!/usr/bin/env python3
"""throughput.py RUN BOARD FIRMWARE [--ref FILE] [--record] [--runs N] [--execs N]

How fast ember fuzzes, as a figure that can be compared from one build to the next.

FIRMWARE is a build of tests/fuzz/fuzztarget.h. The script plays the brain over the fuzz
protocol (docs/fuzz-formats.md) and times, with ember on the JIT:

  plain   one input that loops until a long per-input budget runs out: guest instructions
          per second, the yardstick for this host;
  havoc   a HAVOC batch with a fixed seed (the same mutations every time): executions per
          second with everything between two inputs in it (restore, mutation, the run,
          the judgement);
  i2s     STAGE input-to-state batches of one entry (when HELLO offers stages);
  trim    STAGE trim batches of one entry (when HELLO offers stages).

Executions per second depend on the host, so what is compared is each stage's cost of one
execution in guest instructions at the plain speed (plain / execs per second): what an
input costs beyond running it scales with the host the same way the yardstick does. The
best of --runs is taken.

With --ref FILE the costs are checked against the figures recorded there for this host
architecture and BOARD: a stage that costs more than TOLERANCE times its figure fails.
Without a figure for the host nothing is checked. --record writes this run's figures to
FILE instead (run it on a quiet machine, after a change that is meant to move them).

Exit status 0 when every checked stage is within its tolerance.
"""
import argparse
import os
import platform
import socket
import struct
import subprocess
import sys
import time

HELLO, INPUT, COVERAGE, CRASH, BYE, OPTIONS, SPARSE = 1, 2, 3, 4, 5, 6, 7
MUTATOR, MUTATOR_INFO, CORPUS, HAVOC, FIND, HAVOC_DONE = 10, 11, 12, 13, 14, 15
JUDGE, PROGRESS, STAGE, STAGE_RESULT = 16, 17, 18, 19
FLAG_HAVOC, FLAG_STAGES = 1 << 13, 1 << 12
OPT_SPARSE, OPT_CMP_LOG = 1, 2
STAGE_I2S, STAGE_TRIM = 1, 2
TOLERANCE = 1.5          # a stage may cost this many times its recorded figure
ITER_INSNS = 20000       # per input while fuzzing
PLAIN_INSNS = 100_000_000

SEED = b"hello world, this is a seed for the fuzz target: 0123456789abcdef"
# An input the input-to-state stage has work on: the magic value's compare, and a first
# byte the parser compares with several constants, eight times over.
I2S_SEED = b"MMMM" + struct.pack("<I", 0x6A3C91E5 ^ 0x01000000) + b"MMMM-and-some-more-bytes"
TRIM_SEED = bytes(range(32, 127)) * 2


def sym(fw, name):
    out = subprocess.run(["readelf", "-sW", fw], capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        f = line.split()
        if len(f) >= 8 and f[7] == name:
            return int(f[1], 16)
    sys.exit("throughput: no symbol %s in %s" % (name, fw))


def recv_exact(c, n):
    b = b""
    while len(b) < n:
        chunk = c.recv(n - len(b))
        if not chunk:
            raise EOFError("ember closed the connection")
        b += chunk
    return b


def recv_frame(c):
    (n,) = struct.unpack(">I", recv_exact(c, 4))
    body = recv_exact(c, n)
    return body[0], body[1:]


def send_frame(c, t, payload=b""):
    c.sendall(struct.pack(">IB", len(payload) + 1, t) + payload)


class Session:
    def __init__(self, run, board, fw, extra):
        srv = socket.socket()
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("127.0.0.1", 0))
        srv.listen(1)
        port = srv.getsockname()[1]
        cmd = [run, "-b", board, "--jit", "--cov-blocks",
               "--fuzz-begin", hex(sym(fw, "fuzz_iteration")),
               "--fuzz-crash-pc", hex(sym(fw, "fuzz_panic")), "--fuzz-input", "uart0",
               "--fuzz-iter-insns", str(ITER_INSNS), "--fuzz-net", "127.0.0.1:%d" % port]
        self.proc = subprocess.Popen(cmd + extra + [fw], stdin=subprocess.DEVNULL,
                                     stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        srv.settimeout(60)
        self.c, _ = srv.accept()
        srv.close()
        self.c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        if recv_exact(self.c, 4) != b"UAMF":
            sys.exit("throughput: no magic from ember")
        t, p = recv_frame(self.c)
        if t != HELLO:
            sys.exit("throughput: no HELLO from ember")
        self.flags = struct.unpack(">HH", p[:4])[1]
        self.next_id = 1
        self.options(OPT_SPARSE)
        self.frame()      # the INIT map

    def options(self, flags, iter_insns=0):
        send_frame(self.c, OPTIONS, struct.pack(">IQ", flags, iter_insns))

    def frame(self):
        return recv_frame(self.c)

    def input(self, data):
        """One INPUT; the type of its reply."""
        send_frame(self.c, INPUT, struct.pack(">QI", self.next_id, len(data)) + data)
        self.next_id += 1
        while True:
            t, _ = self.frame()
            if t in (COVERAGE, SPARSE, CRASH):
                return t

    def mutator(self):
        send_frame(self.c, MUTATOR, struct.pack(">IHHHIHI", 256, 2, 128, 150, 0, 0, 0))
        t, p = self.frame()
        if t != MUTATOR_INFO or p[0]:
            sys.exit("throughput: ember refused the mutator settings")

    def corpus(self, eid, data):
        send_frame(self.c, CORPUS, struct.pack(">II", eid, len(data)) + data)

    def batch(self, ftype, payload):
        """A batch to its HAVOC_DONE: (seconds, executions)."""
        t0 = time.perf_counter()
        send_frame(self.c, ftype, payload)
        while True:
            t, p = self.frame()
            if t == HAVOC_DONE:
                dt = time.perf_counter() - t0
                _first, status, execs = struct.unpack(">QBI", p[:13])
                if status:
                    sys.exit("throughput: ember refused a batch (status %d)" % status)
                return dt, execs
            if t not in (FIND, PROGRESS, STAGE_RESULT):
                sys.exit("throughput: frame type %d in a batch" % t)

    def havoc(self, eid, n, seed):
        first = self.next_id
        self.next_id += n
        return self.batch(HAVOC, struct.pack(">QIIQI", first, eid, n, seed, 0))

    def stage(self, kind, eid, limit):
        first = self.next_id
        self.next_id += limit + 2
        return self.batch(STAGE, struct.pack(">QIBII", first, eid, kind, limit, 0))

    def close(self):
        send_frame(self.c, BYE)
        self.c.close()
        err = self.proc.communicate(timeout=60)[1]
        if self.proc.returncode:
            sys.stderr.write(err.decode("utf-8", "replace"))
            sys.exit("throughput: ember exited %d" % self.proc.returncode)


def measure(run, board, fw, extra, runs, execs):
    """{stage: best rate}: 'plain' in guest instructions per second, the others in
    executions per second."""
    s = Session(run, board, fw, extra)
    if not s.flags & FLAG_HAVOC:
        sys.exit("throughput: this ember does not mutate (HELLO)")
    stages = bool(s.flags & FLAG_STAGES)
    best = {}

    def keep(name, rate):
        best[name] = max(best.get(name, 0.0), rate)

    s.mutator()
    s.corpus(1, SEED)
    s.corpus(2, I2S_SEED)
    s.corpus(3, TRIM_SEED)
    s.input(SEED)
    s.havoc(1, 2000, 1)              # translate what the batches run
    for k in range(runs):
        s.options(OPT_SPARSE, PLAIN_INSNS)
        t0 = time.perf_counter()
        if s.input(b"Lxx") != CRASH:
            sys.exit("throughput: the looping input did not run out of budget")
        keep("plain", PLAIN_INSNS / (time.perf_counter() - t0))
        s.options(OPT_SPARSE, ITER_INSNS)
        dt, n = s.havoc(1, execs, 0x1234)
        keep("havoc", n / dt)
    if stages:
        s.options(OPT_SPARSE | OPT_CMP_LOG, ITER_INSNS)
        s.input(I2S_SEED)
        for k in range(runs):
            total, dt = 0, 0.0
            for _ in range(max(1, execs // 4000)):
                d, n = s.stage(STAGE_I2S, 2, 256)
                dt += d
                total += n
            keep("i2s", total / dt)
        s.options(OPT_SPARSE, ITER_INSNS)
        for k in range(runs):
            total, dt = 0, 0.0
            for _ in range(max(1, execs // 2000)):
                d, n = s.stage(STAGE_TRIM, 3, 64)
                dt += d
                total += n
            keep("trim", total / dt)
    s.close()
    return best


def host_arch(run):
    """The architecture RUN is built for (its ELF header), else this machine's."""
    names = {62: "x86_64", 183: "aarch64"}
    try:
        with open(run, "rb") as f:
            h = f.read(20)
        if h[:4] == b"\x7fELF":
            m = struct.unpack("<H" if h[5] == 1 else ">H", h[18:20])[0]
            return names.get(m, "elf-machine-%d" % m)
    except OSError:
        pass
    return platform.machine()


def read_ref(path):
    ref = {}
    if path and os.path.exists(path):
        with open(path) as f:
            for line in f:
                w = line.split("#")[0].split()
                if len(w) == 4:
                    ref[(w[0], w[1], w[2])] = float(w[3])
    return ref


def write_ref(path, ref):
    with open(path, "w") as f:
        f.write("# What one fuzz execution costs, in guest instructions at the host's plain JIT\n"
                "# speed (tests/fuzz/throughput.py; `meson compile -C build fuzz-throughput-record` rewrites it).\n"
                "# host-arch board stage cost\n")
        for (arch, board, stage), cost in sorted(ref.items()):
            f.write("%s %s %s %.0f\n" % (arch, board, stage, cost))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[1])
    ap.add_argument("run")
    ap.add_argument("board")
    ap.add_argument("firmware")
    ap.add_argument("--ref")
    ap.add_argument("--record", action="store_true")
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--execs", type=int, default=40000)
    ap.add_argument("extra", nargs="*", help="further uam-run options (after --)")
    a = ap.parse_args()
    best = measure(a.run, a.board, a.firmware, a.extra, a.runs, a.execs)
    plain = best.pop("plain")
    arch = host_arch(a.run)
    ref = read_ref(a.ref)
    print("throughput: %s on %s: plain %.0f M guest instructions/s" % (a.board, arch, plain / 1e6))
    fail = False
    for stage, rate in best.items():
        cost = plain / rate
        key = (arch, a.board, stage)
        note = ""
        if a.record:
            ref[key] = cost
        elif key in ref:
            note = " (recorded %.0f)" % ref[key]
            if cost > ref[key] * TOLERANCE:
                note += " FAIL: over %.1fx the recorded cost" % TOLERANCE
                fail = True
        elif a.ref:
            note = " (no figure recorded for this host)"
        print("throughput:   %-5s %8.0f execs/s, %6.0f instructions per exec%s"
              % (stage, rate, cost, note))
    if a.record:
        if not a.ref:
            sys.exit("throughput: --record needs --ref FILE")
        write_ref(a.ref, ref)
        print("throughput: recorded in %s" % a.ref)
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
