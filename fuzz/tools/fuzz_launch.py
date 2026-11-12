#!/usr/bin/env python3
"""fuzz_launch.py -- run a parallel campaign: N fuzz brains, each with its own ember.

Each worker is a fuzz_brain.py + uam-run pair on its own port, with its own output
directory SYNC_DIR/NAME (queue/, crashes/, hangs/, status.jsonl, brain.log, ember.log).
The workers import each other's queue entries through SYNC_DIR (fuzz_brain.py
--sync-dir). The first worker is "main", the others w1, w2, ...; each gets a distinct
RNG seed (derived from --seed-rng and its name: identical workers would only repeat each
other) and the next schedule of --schedules, round robin. A worker whose brain or ember
exits unexpectedly is restarted (up to --max-restarts times); it resumes from its queue.
Ctrl-C / SIGTERM stops every worker cleanly (the brains say BYE).

  python3 fuzz/tools/fuzz_launch.py -n 8 --out camp --seeds seeds/ \\
      --schedules fast,explore,exploit,coe,rare --dict fw.dict -- \\
      build/uam-run -b rv32-ref --cov-sites s.uams --cov-boundary b.uamb --fuzz-input uart0 fw.elf
  python3 fuzz/tools/status_tui.py --sync-dir camp          # every worker, one row each

Everything after "--" is the ember command line; the launcher appends
"--fuzz-net 127.0.0.1:PORT" to it. With --iters, each worker stops after that many
mutated executions and the launcher ends when all have. Exit status: 0 when every worker
ended cleanly, 1 otherwise (a worker that ran out of restarts), 2 on a usage error.
"""
import argparse
import hashlib
import os
import signal
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
BRAIN = os.path.join(HERE, "fuzz_brain.py")
SCHEDULES = ("fast", "explore", "exploit", "coe", "rare")


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def worker_seed(base, name):
    return int.from_bytes(hashlib.sha256(b"%d/%s" % (base, name.encode())).digest()[:4], "big")


class Worker:
    def __init__(self, name, role, schedule, seed, args, ember_cmd):
        self.name, self.role, self.schedule, self.seed = name, role, schedule, seed
        self.args, self.ember_cmd = args, ember_cmd
        self.dir = os.path.join(args.out, name)
        self.brain = self.ember = None
        self.restarts = 0
        self.done = False
        self.failed = False

    def start(self):
        os.makedirs(self.dir, exist_ok=True)
        port = free_port()
        a = self.args
        cmd = [sys.executable, BRAIN, "--listen", "127.0.0.1:%d" % port,
               "--sync-dir", a.out, "--name", self.name, "--" + self.role,
               "--schedule", self.schedule, "--seed-rng", str(self.seed),
               "--sync-interval", str(a.sync_interval),
               "--status-interval", str(a.status_interval)]
        if a.seeds:
            cmd += ["--seeds", a.seeds]
        if a.iters:
            cmd += ["--iters", str(a.iters)]
        for d in a.dict:
            cmd += ["--dict", d]
        cmd += a.brain_arg
        mode = "ab" if self.restarts else "wb"
        self.blog = open(os.path.join(self.dir, "brain.log"), mode)
        self.elog = open(os.path.join(self.dir, "ember.log"), mode)
        self.brain = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=self.blog,
                                      stderr=subprocess.STDOUT, start_new_session=True)
        time.sleep(0.2)                      # let it listen; ember retries for a few seconds
        self.ember = subprocess.Popen(self.ember_cmd + ["--fuzz-net", "127.0.0.1:%d" % port],
                                      stdin=subprocess.DEVNULL, stdout=self.elog,
                                      stderr=subprocess.STDOUT, start_new_session=True)
        log("%s: started (%s, schedule %s, seed %d, port %d%s)"
            % (self.name, self.role, self.schedule, self.seed, port,
               ", restart %d" % self.restarts if self.restarts else ""))

    def poll(self):
        """None while running; else (brain status, ember status) once both ended."""
        b, e = self.brain.poll(), self.ember.poll()
        if b is not None and e is None:
            # the brain is gone: ember notices the closed connection; give it a moment
            try:
                e = self.ember.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.ember.kill()
                e = self.ember.wait()
        elif e is not None and b is None:
            try:
                b = self.brain.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.brain.terminate()
                b = self.brain.wait()
        if b is None or e is None:
            return None
        self.blog.close()
        self.elog.close()
        return b, e

    def stop(self):
        for p in (self.brain,):
            if p and p.poll() is None:
                p.send_signal(signal.SIGTERM)   # the brain says BYE; ember then exits


def log(msg):
    print("fuzz_launch: " + msg, flush=True)


def main():
    ap = argparse.ArgumentParser(
        description="Run N fuzz_brain.py + uam-run workers that share their work.",
        usage="%(prog)s -n N --out DIR [options] -- UAM-RUN COMMAND...")
    ap.add_argument("-n", "--workers", type=int, required=True, help="number of workers")
    ap.add_argument("--out", required=True, help="the sync directory (one subdirectory per worker)")
    ap.add_argument("--schedules", default="fast,explore,exploit,coe,rare",
                    help="comma list, assigned round robin (default: all five)")
    ap.add_argument("--seeds", help="seed directory for every worker")
    ap.add_argument("--dict", action="append", default=[], help="dictionary for every worker")
    ap.add_argument("--seed-rng", type=int, default=0xC0FFEE,
                    help="campaign seed; each worker's is derived from it and its name")
    ap.add_argument("--iters", type=int, default=0,
                    help="each worker stops after N mutated execs (0: until stopped)")
    ap.add_argument("--sync-interval", type=float, default=60.0,
                    help="seconds between a worker's imports (default 60)")
    ap.add_argument("--status-interval", type=float, default=1.0)
    ap.add_argument("--max-restarts", type=int, default=5,
                    help="restarts per worker after an unexpected exit (default 5)")
    ap.add_argument("--brain-arg", action="append", default=[],
                    help="extra fuzz_brain.py argument for every worker (repeatable)")
    if "--" not in sys.argv:
        ap.print_usage(sys.stderr)
        sys.stderr.write("fuzz_launch: give the ember command after --\n")
        return 2
    cut = sys.argv.index("--")
    args = ap.parse_args(sys.argv[1:cut])
    ember_cmd = sys.argv[cut + 1:]
    if not ember_cmd:
        ap.error("the ember command after -- is empty")
    if args.workers < 1 or args.workers > 256:
        ap.error("-n must be 1..256")
    scheds = [s.strip() for s in args.schedules.split(",") if s.strip()]
    bad = [s for s in scheds if s not in SCHEDULES]
    if not scheds or bad:
        ap.error("unknown schedule(s) %s (%s)" % (", ".join(bad) or "none", ", ".join(SCHEDULES)))
    os.makedirs(args.out, exist_ok=True)

    workers = []
    for i in range(args.workers):
        name = "main" if i == 0 else "w%d" % i
        workers.append(Worker(name, "main" if i == 0 else "secondary", scheds[i % len(scheds)],
                              worker_seed(args.seed_rng, name), args, ember_cmd))

    stopping = {"flag": False}

    def on_signal(_s, _f):
        stopping["flag"] = True
    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGTERM, on_signal)

    for w in workers:
        w.start()
    stop_sent = False
    while True:
        if stopping["flag"] and not stop_sent:
            log("stopping every worker")
            for w in workers:
                w.stop()
            stop_sent = True
        running = 0
        for w in workers:
            if w.done:
                continue
            r = w.poll()
            if r is None:
                running += 1
                continue
            b, e = r
            clean = b == 0 and e == 0
            if clean or stopping["flag"]:
                w.done = True
                log("%s: finished (brain %s, ember %s)" % (w.name, b, e))
                continue
            if w.restarts >= args.max_restarts:
                w.done = w.failed = True
                log("%s: gave up after %d restarts (brain %s, ember %s); see %s"
                    % (w.name, w.restarts, b, e, w.dir))
                continue
            w.restarts += 1
            log("%s: exited unexpectedly (brain %s, ember %s); restarting from its queue"
                % (w.name, b, e))
            w.start()
            running += 1
        if not running:
            break
        time.sleep(0.5)
    failed = [w.name for w in workers if w.failed]
    log("all workers ended%s" % (": failed " + ", ".join(failed) if failed else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
