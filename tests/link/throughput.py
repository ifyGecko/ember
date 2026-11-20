#!/usr/bin/env python3
"""throughput.py HUB RUN FIRMWARE [--ref FILE] [--record] [--runs N]

What a message between two emulations costs, as a figure that can be compared from one
build to the next.

HUB is uam-link-hub, RUN is uam-test-run and FIRMWARE is tests/firmware/linktest.c, two of
which exchange numbered messages on the rv32-test board, on the JIT. Timed:

  plain        one guest in a busy loop: guest instructions per second, the yardstick for
               this host;
  udp          frames over --nic udp: (what there was before links: no hub, no time kept);
  free         frames through a hub that keeps no time;
  loose        the same, no node more than 1 ms ahead;
  exact        the same in lockstep, the guests asleep between frames (an idle machine
               tells the hub how long it stays quiet, so each frame costs a few round
               trips whatever the latency);
  exact-busy   lockstep with both guests polling, at a latency of 500 us, 50 us and 5 us:
               nothing is ever quiet, so each latency's worth of guest time is one round
               trip;
  bytes        the console as the link, a message a byte, in lockstep.

Each is run with two message counts and the difference taken, so that starting the
processes is not in the figure. Messages per second depend on the host; what is compared is
the cost of one message in guest instructions at the plain speed (plain / messages per
second). For the busy runs the guests execute a latency's worth of instructions per
message anyway, so their figure is the slowdown: host time per guest instruction, relative
to plain (1.0: lockstep costs nothing), times 1000.

With --ref FILE the figures are checked against those recorded there for this host
architecture: one that is more than TOLERANCE times its figure fails. Without a figure for
the host nothing is checked. --record writes this run's figures to FILE instead (on a
quiet machine, after a change that is meant to move them).

Exit status 0 when every checked figure is within its tolerance.
"""
import argparse
import os
import platform
import re
import socket
import struct
import subprocess
import sys
import tempfile
import time

TOLERANCE = 2.0           # two or three processes and a socket: noisier than one
SMALL, LARGE = 2000, 22000
BYTES_SMALL, BYTES_LARGE = 50, 250
PLAIN_SMALL, PLAIN_LARGE = 50_000_000, 450_000_000


def mac(rounds, mode, role):
    return "02:00:%02x:%02x:%02x:%02x" % (rounds >> 8, rounds & 0xFF, mode, role)


def free_udp_ports():
    socks = [socket.socket(socket.AF_INET, socket.SOCK_DGRAM) for _ in range(2)]
    for s in socks:
        s.bind(("127.0.0.1", 0))
    ports = [s.getsockname()[1] for s in socks]
    for s in socks:
        s.close()
    return ports


def insns(text):
    m = re.search(r"after (\d+) instructions", text)
    return int(m.group(1)) if m else 0


class Bench:
    def __init__(self, hub, run, fw, tmp):
        self.hub, self.run, self.fw, self.tmp = hub, run, fw, tmp
        self.sock = os.path.join(tmp, "hub.sock")

    def node(self, name, role, mode, rounds, args):
        cmd = [self.run, "--jit", "-b", "rv32-test", "-o", "net0.mac=" + mac(rounds, mode, role)]
        cmd += args + [self.fw]
        return subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True)

    def start_hub(self, hub_args):
        if os.path.exists(self.sock):
            os.unlink(self.sock)
        h = subprocess.Popen([self.hub, "unix:" + self.sock] + hub_args,
                             stderr=subprocess.DEVNULL)
        for _ in range(200):
            if os.path.exists(self.sock):
                return h
            time.sleep(0.01)
        h.kill()
        sys.exit("throughput: the hub did not start")

    def pair(self, hub_args, mode, rounds, node_args):
        """Seconds for the pair to finish, and the instructions both executed."""
        h = self.start_hub(hub_args) if hub_args is not None else None
        t0 = time.perf_counter()
        ps = [self.node(n, r, mode, rounds, node_args(n)) for n, r in (("a", 1), ("b", 2))]
        outs = [p.communicate(timeout=600)[0] for p in ps]
        dt = time.perf_counter() - t0
        if h:
            h.terminate()
            h.wait()
        if any(p.returncode != 0 for p in ps):
            sys.exit("throughput: a node failed (%s):\n%s" % ([p.returncode for p in ps],
                                                              "\n".join(outs)))
        return dt, sum(insns(o) for o in outs)

    def plain(self, n):
        t0 = time.perf_counter()
        p = self.node("p", 2, 2, 0xFFFF, ["--nic", "net0=null", "--console", "null",
                                          "--max-insns", str(n)])
        p.communicate(timeout=600)
        return time.perf_counter() - t0


def link_args(sock, latency, console="null"):
    def args(name):
        return ["--link", "p0=unix:%s,bus=wire,latency=%s" % (sock, latency), "--link-name", name,
                "--nic", "net0=link:p0", "--console", console]
    return args


def measure(b, runs):
    best = {}

    def keep(name, value, higher_is_better):
        if name not in best or (value > best[name]) == higher_is_better:
            best[name] = value

    def rate(name, hub_args, mode, node_args, small=SMALL, large=LARGE):
        t1, _ = b.pair(hub_args, mode, small, node_args)
        t2, _ = b.pair(hub_args, mode, large, node_args)
        if t2 > t1:
            keep(name, (large - small) / (t2 - t1), True)

    def slowdown(name, hub_args, mode, node_args, plain_ips):
        t1, i1 = b.pair(hub_args, mode, 500, node_args)
        t2, i2 = b.pair(hub_args, mode, 8500, node_args)
        # Two guests run side by side: at plain speed the pair would take the time of
        # the longer half.
        ideal = (i2 - i1) / 2 / plain_ips
        if t2 > t1 and ideal > 0:       # a run disturbed beyond use is left out
            keep(name, (t2 - t1) / ideal, False)

    for _ in range(runs):
        t1 = b.plain(PLAIN_SMALL)
        t2 = b.plain(PLAIN_LARGE)
        keep("plain", (PLAIN_LARGE - PLAIN_SMALL) / max(t2 - t1, 1e-9), True)
    for _ in range(runs):
        pa, pb = free_udp_ports()
        rate("udp", None, 4, lambda n: ["--nic", "net0=udp:%d:127.0.0.1:%d"
                                        % ((pa, pb) if n == "a" else (pb, pa)),
                                        "--virtual-time", "--console", "null"])
        rate("free", [], 0, link_args(b.sock, "50us"))
        rate("loose", ["--sync", "loose", "--nodes", "2"], 0, link_args(b.sock, "50us"))
        rate("exact", ["--sync", "exact", "--nodes", "2"], 0, link_args(b.sock, "50us"))
        for lat in ("500us", "50us", "5us"):
            slowdown("exact-busy-" + lat, ["--sync", "exact", "--nodes", "2"], 2,
                     link_args(b.sock, lat), best["plain"])

        def byte_args(name):
            return ["--link", "u0=unix:%s,bus=serial,latency=87us" % b.sock, "--link-name", name,
                    "--console", "link:u0", "--nic", "net0=null"]
        rate("bytes", ["--sync", "exact", "--nodes", "2"], 1, byte_args, BYTES_SMALL, BYTES_LARGE)
    return best


def host_arch(run):
    names = {62: "x86_64", 183: "aarch64", 3: "i386", 40: "arm", 243: "riscv"}
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
                if len(w) == 3:
                    ref[(w[0], w[1])] = float(w[2])
    return ref


def write_ref(path, ref):
    with open(path, "w") as f:
        f.write("# What a message between two emulations costs, in guest instructions at the host's\n"
                "# plain JIT speed; for exact-busy-*, lockstep's slowdown times 1000 (tests/link/\n"
                "# throughput.py; `meson compile -C build link-throughput-record` rewrites it).\n"
                "# host-arch run figure\n")
        for (arch, name), v in sorted(ref.items()):
            f.write("%s %s %.0f\n" % (arch, name, v))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[1])
    ap.add_argument("hub")
    ap.add_argument("run")
    ap.add_argument("firmware")
    ap.add_argument("--ref")
    ap.add_argument("--record", action="store_true")
    ap.add_argument("--runs", type=int, default=3)
    a = ap.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        best = measure(Bench(a.hub, a.run, a.firmware, tmp), a.runs)
    plain = best.pop("plain")
    arch = host_arch(a.run)
    ref = read_ref(a.ref)
    print("link-throughput: %s: plain %.0f M guest instructions/s" % (arch, plain / 1e6))
    fail = False
    for name, v in best.items():
        busy = name.startswith("exact-busy")
        figure = v * 1000 if busy else plain / v
        key = (arch, name)
        note = ""
        if a.record:
            ref[key] = figure
        elif key in ref:
            note = " (recorded %.0f)" % ref[key]
            if figure > ref[key] * TOLERANCE:
                note += " FAIL: over %.1fx the recorded figure" % TOLERANCE
                fail = True
        elif a.ref:
            note = " (no figure recorded for this host)"
        if busy:
            print("link-throughput:   %-16s %6.2fx plain speed's time%s" % (name, v, note))
        else:
            print("link-throughput:   %-16s %8.0f messages/s, %7.0f instructions per message%s"
                  % (name, v, figure, note))
    if a.record:
        if not a.ref:
            sys.exit("link-throughput: --record needs --ref FILE")
        write_ref(a.ref, ref)
        print("link-throughput: recorded in %s" % a.ref)
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
