#!/usr/bin/env python3
"""gdb-check.py - the gdb stub on a multiprocessor machine, over the remote protocol.

    gdb-check.py RUNNER SMP_BIN SMP_LST

Runs tests/firmware-x86/smp.bin on four processors under --gdb and checks that
every processor is a thread: the thread list and its extra info, thread
selection (Hg) and liveness (T), registers per thread, a breakpoint in code all
processors run (each one reports itself in the stop reply), a step of a chosen
thread, and the run finishing (W00) once gdb lets it go.
"""
import re
import socket
import subprocess
import sys
import time

runner, binary, listing = sys.argv[1:4]
fails = 0


def check(cond, what):
    global fails
    print(("[ ok ] " if cond else "[FAIL] ") + what)
    if not cond:
        fails += 1


def label(name):
    """The address of `name:` in the nasm listing."""
    with open(listing) as f:
        for line in f:
            m = re.match(r"\s*\d+\s+([0-9A-F]{8})\s+\S+\s+" + re.escape(name) + r":", line)
            if m:
                return int(m.group(1), 16)
    sys.exit("label %s not in %s" % (name, listing))


class Rsp:
    def __init__(self, port):
        for _ in range(100):
            try:
                self.s = socket.create_connection(("127.0.0.1", port))
                break
            except OSError:
                time.sleep(0.05)
        else:
            sys.exit("cannot connect to the stub")
        self.buf = b""

    def _byte(self):
        while not self.buf:
            d = self.s.recv(4096)
            if not d:
                raise EOFError
            self.buf += d
        b, self.buf = self.buf[:1], self.buf[1:]
        return b

    def cmd(self, data):
        cs = sum(data.encode()) & 0xFF
        self.s.sendall(b"$%s#%02x" % (data.encode(), cs))
        while True:   # skip acks up to the reply
            b = self._byte()
            if b == b"$":
                break
        body = b""
        while True:
            b = self._byte()
            if b == b"#":
                break
            body += b
        self._byte(), self._byte()
        self.s.sendall(b"+")
        return body.decode()


def reg(r, n):
    v = r.cmd("p%x" % n)
    return int.from_bytes(bytes.fromhex(v), "little")


RIP = 16
work = label("work")
port = 40000 + (int(time.time() * 1000) % 20000)
proc = subprocess.Popen([runner, "-b", "x86_64-ref", "-o", "cpus=4", "--raw-base", "0",
                         "--entry", "0", "--gdb", str(port), binary],
                        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE)
try:
    r = Rsp(port)
    check(r.cmd("qfThreadInfo") == "m1,2,3,4" and r.cmd("qsThreadInfo") == "l",
          "four threads, one per processor")
    check(r.cmd("?") == "T05thread:1;" and r.cmd("qC") == "QC1", "stopped in thread 1")
    info = bytes.fromhex(r.cmd("qThreadExtraInfo,2")).decode()
    check(info == "cpu 1 (waiting)", "thread 2 is cpu 1, waiting for its start-up (%s)" % info)
    check(r.cmd("T4") == "OK" and r.cmd("T5") == "E01" and r.cmd("Hg5") == "E01",
          "thread 4 exists, 5 does not")
    # gdb accepts an amd64 description only with the whole core register set:
    # 16 GPRs, rip (8 bytes each), eflags and 6 selectors (4), st0-7 (10), 8 x87 (4)
    g = r.cmd("g")
    check(len(g) == 2 * (17 * 8 + 7 * 4 + 8 * 10 + 8 * 4), "g: the amd64 core set (%d)" % len(g))
    check(len(r.cmd("p18")) == 20 and r.cmd("p20") == "7f030000", "p: st0 is 10 bytes, fctrl 0x37f")

    check(r.cmd("Z0,%x,1" % work) == "OK", "breakpoint at work (0x%x)" % work)
    seen = []
    for _ in range(4):
        stop = r.cmd("c")
        m = re.fullmatch(r"T05swbreak:;thread:([0-9a-f]+);", stop)
        if not m:
            check(False, "breakpoint stop reply (%s)" % stop)
            break
        t = int(m.group(1), 16)
        seen.append(t)
        check(r.cmd("qC") == "QC%x" % t and reg(r, RIP) == work,
              "thread %d stopped at work, and is current" % t)
        r.cmd("Hg%x" % (1 if t != 1 else 2))
        check(reg(r, RIP) != work or t == 1, "another thread is elsewhere")
    check(sorted(seen) == [1, 2, 3, 4], "every processor hit the breakpoint (%s)" % seen)
    check(r.cmd("z0,%x,1" % work) == "OK", "breakpoint removed")

    r.cmd("Hg3")
    before = reg(r, RIP)
    stop = r.cmd("vCont;s:3")
    check(stop == "T05thread:3;", "step of thread 3 stops in thread 3 (%s)" % stop)
    check(reg(r, RIP) != before, "thread 3 moved (0x%x -> 0x%x)" % (before, reg(r, RIP)))

    check(r.cmd("c") == "W00", "the run finishes with status 0")
finally:
    try:
        out, err = proc.communicate(timeout=60)
    except subprocess.TimeoutExpired:
        proc.kill()
        out, err = proc.communicate()
check(b"smp log hash" in out, "the guest printed its hash")
sys.exit(1 if fails else 0)
