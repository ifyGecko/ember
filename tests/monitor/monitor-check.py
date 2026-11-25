#!/usr/bin/env python3
"""monitor-check.py - the monitor (uam-run --monitor), over its ways in.

    monitor-check.py UAM_RUN UAM_MON

Runs a few bytes of x86 real-mode code on x86_64-ref that print 'A' to COM1 in a
loop, and checks: the Unix socket (greeting, status, stop and cont, ids, JSON,
errors, help, quit with a status), --monitor-wait, --monitor-hold at the end of a
run, --monitor-script, tcp:PORT, two clients at once, a stale socket file, uam-mon
-c, SIGINT while stopped, the interpreter and the JIT, the option checks, and
Ctrl-A m on a raw stdio console (guest output held while the terminal is the
monitor's, then written out).
"""
import json
import os
import pty
import select
import signal
import socket
import subprocess
import sys
import tempfile
import time

runner, uam_mon = sys.argv[1:3]
fails = 0
tmp = tempfile.mkdtemp(prefix="monitor-check.")
# mov dx,0x3f8; mov al,'A'; out dx,al; mov cx,0x1000; loop $; jmp 0
spin = os.path.join(tmp, "spin.bin")
with open(spin, "wb") as f:
    f.write(bytes.fromhex("baf803b041eeb90010e2feebf3"))
BASE = [runner, "-b", "x86_64-ref", "--raw-base", "0", "--entry", "0"]


def check(cond, what):
    global fails
    print(("[ ok ] " if cond else "[FAIL] ") + what)
    if not cond:
        fails += 1


def start(*extra, sock=None):
    path = sock or os.path.join(tmp, "m%d.sock" % time.monotonic_ns())
    p = subprocess.Popen(BASE + ["--monitor", "unix:" + path] + list(extra) + [spin],
                         stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE)
    for _ in range(200):
        if os.path.exists(path):
            break
        time.sleep(0.01)
    return p, path


class Mon:
    def __init__(self, path=None, port=None):
        if port:
            self.s = socket.create_connection(("127.0.0.1", port), timeout=10)
        else:
            self.s = socket.socket(socket.AF_UNIX)
            self.s.settimeout(10)
            for _ in range(100):
                try:
                    self.s.connect(path)
                    break
                except OSError:
                    time.sleep(0.02)
        self.f = self.s.makefile("rw")
        self.events = []
        self.hello = self.f.readline().strip()

    def cmd(self, line, tag="t"):
        """Send '#tag line'; return (ok, lines, error)."""
        self.f.write("#%s %s\n" % (tag, line))
        self.f.flush()
        out = []
        while True:
            l = self.f.readline()
            if not l:
                return None, out, "closed"
            l = l.rstrip("\n")
            if l.startswith("! "):
                self.events.append(l)
            elif l.startswith("#%s " % tag):
                rest = l[len(tag) + 2:]
                return rest == "ok", out, rest[7:] if rest.startswith("error: ") else None
            else:
                out.append(l)

    def jcmd(self, line, tag="j"):
        self.f.write("#%s %s\n" % (tag, line))
        self.f.flush()
        while True:
            o = json.loads(self.f.readline())
            if o.get("event"):
                self.events.append(o)
                continue
            return o

    def event(self, kind):
        """Wait for an event (already seen ones count)."""
        for e in self.events:
            if e.startswith("! " + kind):
                return e
        while True:
            l = self.f.readline()
            if not l:
                return None
            l = l.rstrip("\n")
            if l.startswith("! "):
                self.events.append(l)
                if l.startswith("! " + kind):
                    return l

    def close(self):
        self.s.close()


def fields(lines):
    return dict(l.split(": ", 1) for l in lines if ": " in l)


def finish(p, timeout=20):
    try:
        out, err = p.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        p.kill()
        out, err = p.communicate()
        return None, out, err
    return p.returncode, out, err


# ---- the Unix socket: status, stop/cont, ids, JSON, errors, quit ----------------
for jit in ([], ["--jit"]):
    name = "jit" if jit else "interp"
    p, path = start(*jit, "-n", "2000000000")
    m = Mon(path)
    check(m.hello == "! hello protocol=1 state=running", "%s: greeting (%s)" % (name, m.hello))
    ok, out, _ = m.cmd("status")
    st = fields(out)
    check(ok and st.get("state") == "running" and st.get("board") == "x86_64-ref"
          and st.get("cpus") == "1" and any(l.startswith("cpu 0 pc=") for l in out),
          "%s: status while running" % name)
    ok, out, _ = m.cmd("stop")
    st = fields(out)
    check(ok and st.get("state") == "stopped", "%s: stop answers once stopped" % name)
    ev = m.event("stopped")
    check(ev is not None and "reason=monitor" in ev, "%s: a stopped event (%s)" % (name, ev))
    ic = st.get("icount")
    time.sleep(0.2)
    ok, out, _ = m.cmd("status")
    check(ok and fields(out).get("icount") == ic and fields(out).get("reason") == "monitor",
          "%s: a stopped machine does not move (icount %s)" % (name, ic))
    ok, out, _ = m.cmd("cont")
    check(ok and fields(out).get("state") == "running", "%s: cont" % name)
    time.sleep(0.1)
    ok, out, _ = m.cmd("status")
    check(int(fields(out).get("icount", 0)) > int(ic), "%s: it runs on after cont" % name)
    ok, _, err = m.cmd("frobnicate")
    check(ok is False and "unknown command" in err, "%s: an unknown command (%s)" % (name, err))
    ok, _, err = m.cmd('help "unclosed')
    check(ok is False and "quote" in err, "%s: an unclosed quote (%s)" % (name, err))
    ok, out, _ = m.cmd("help")
    names = {l.split()[0] for l in out}
    check(ok and {"help", "status", "stop", "cont", "quit", "json"} <= names, "%s: help" % name)
    ok, out, _ = m.cmd("help stop")
    check(ok and out and out[0] == "stop", "%s: help on one command" % name)
    m.cmd("# a comment is not a command")
    o = m.jcmd("json on")
    check(o == {"id": "j", "ok": True, "lines": [], "data": {}}, "%s: json on (%s)" % (name, o))
    o = m.jcmd("status", tag="x7")
    check(o.get("id") == "x7" and o["data"].get("state") == "running"
          and o["lines"][0].startswith("cpu 0"), "%s: a JSON reply" % name)
    o = m.jcmd("nope")
    check(o.get("ok") is False and "unknown command" in o.get("error", ""), "%s: a JSON error" % name)
    ok, _, _ = m.cmd("json off")
    check(ok, "%s: json off answers in text" % name)
    ok, _, err = m.cmd("quit 200")
    check(ok is False, "%s: quit refuses a status beyond 123" % name)
    ok, _, _ = m.cmd("quit 7")
    check(ok, "%s: quit 7 answered" % name)
    rc, _, err = finish(p)
    check(rc == 7, "%s: uam-run exits 7 (%s)" % (name, rc))
    check(not os.path.exists(path), "%s: the socket file is removed" % name)

# ---- --monitor-wait: stopped before the first instruction ---------------------------
p, path = start("--monitor-wait", "-n", "100000")
m = Mon(path)
check(m.hello == "! hello protocol=1 state=stopped", "wait: greeting says stopped")
ok, out, _ = m.cmd("status")
st = fields(out)
check(st.get("icount") == "0" and st.get("reason") == "start", "wait: nothing has run yet")
m.cmd("cont")
ev = m.event("ended")
check(ev is not None and "reason=instruction-limit-reached" in ev and "icount=100000" in ev,
      "wait: the run ends after cont, with an ended event (%s)" % ev)
rc, out, _ = finish(p)
check(rc == 124 and out.count(b"A") > 0, "wait: exit 124 at the limit, the guest printed")

# ---- --monitor-hold: stay at the end; quit keeps or sets the exit status --------------
for q, want in (("quit", 124), ("quit 5", 5)):
    p, path = start("--monitor-hold", "-n", "50000")
    m = Mon(path)
    if m.hello != "! hello protocol=1 state=ended":   # it may end before the client is there
        m.event("ended")
    ok, out, _ = m.cmd("status")
    st = fields(out)
    check(st.get("state") == "ended" and st.get("icount") == "50000",
          "hold: still there after the end (%s)" % st.get("state"))
    ok, _, err = m.cmd("cont")
    check(ok is False and "ended" in err, "hold: cont refused after the end")
    m.cmd(q)
    rc, _, _ = finish(p)
    check(rc == want, "hold: '%s' exits %s (%s)" % (q, want, rc))

# ---- --monitor-script -----------------------------------------------------------------
script = os.path.join(tmp, "stop.mon")
with open(script, "w") as f:
    f.write("# stay stopped at the start\nstatus\nstop\n")
p, path = start("--monitor-script", script, "-n", "100000")
m = Mon(path)
ok, out, _ = m.cmd("status")
check(fields(out).get("icount") == "0", "script: 'stop' keeps the machine stopped at the start")
m.cmd("quit")
rc, _, err = finish(p)
check(rc == 0 and b"uam-run: monitor: state: stopped" in err,
      "script: its output on stderr, quit exits 0 (%s)" % rc)
with open(script, "w") as f:
    f.write("status\nbogus\nquit 9\n")
rc, _, err = finish(subprocess.Popen(BASE + ["--monitor-script", script, "-n", "1000", spin],
                                     stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE))
check(rc == 2 and b"bogus: unknown command" in err,
      "script: a failing command stops it and the run (exit %s)" % rc)

# ---- tcp:PORT, two clients, a stale socket file, uam-mon -c ------------------------------
port = 41000 + (os.getpid() % 9000)
stale = os.path.join(tmp, "stale.sock")
s = socket.socket(socket.AF_UNIX)
s.bind(stale)
s.close()   # the file stays, nobody listens
p = subprocess.Popen(BASE + ["--monitor", "unix:" + stale, "--monitor", "tcp:%d" % port,
                             "-n", "2000000000", spin],
                     stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
time.sleep(0.3)
a = Mon(stale)
b = Mon(port=port)
check(a.hello.startswith("! hello") and b.hello.startswith("! hello"),
      "a stale socket file is replaced; tcp: answers too")
a.cmd("stop")
ev = b.event("stopped")
check(ev is not None, "the other client hears of the stop")
r = subprocess.run([uam_mon, "unix:" + stale, "-c", "status; cont; stop; status"],
                   capture_output=True, text=True, timeout=10)
check(r.returncode == 0 and r.stdout.count("state: stopped") == 3
      and "state: running" in r.stdout, "uam-mon -c runs several commands")
r = subprocess.run([uam_mon, "tcp:%d" % port, "-c", "status; nope"], capture_output=True,
                   text=True, timeout=10)
check(r.returncode == 1 and "unknown command" in r.stderr, "uam-mon exits 1 on a failed command")
r = subprocess.run([uam_mon, "tcp:%d" % port, "-j", "-c", "status"], capture_output=True,
                   text=True, timeout=10)
check(r.returncode == 0 and json.loads(r.stdout)["data"]["state"] == "stopped", "uam-mon -j")
# SIGINT while stopped ends the run like any SIGINT
p.send_signal(signal.SIGINT)
rc, _, _ = finish(p)
check(rc == 130, "SIGINT while stopped exits 130 (%s)" % rc)
r = subprocess.run([uam_mon, os.path.join(tmp, "none.sock"), "-c", "status"],
                   capture_output=True, text=True, timeout=10)
check(r.returncode == 2, "uam-mon exits 2 with no monitor there")

# ---- option checks ----------------------------------------------------------------------
for args, what in (
        (["--monitor", "stdio"], "--monitor stdio with the console on stdio"),
        (["--monitor", "bogus:1"], "an unknown --monitor spec"),
        (["--monitor-wait"], "--monitor-wait with no way in"),
        (["--monitor", "unix:" + os.path.join(tmp, "g.sock"), "--gdb", "1"], "--monitor with --gdb"),
        (["--monitor", "unix:" + spin], "a file that is not a socket in the way")):
    r = subprocess.run(BASE + args + ["-n", "1000", spin], capture_output=True, timeout=10)
    check(r.returncode == 2, "refused: %s (exit %d)" % (what, r.returncode))

# ---- Ctrl-A m on a raw stdio console -----------------------------------------------------
pid, fd = pty.fork()
if pid == 0:
    os.execv(runner, BASE + ["--console", "stdio-raw", "-n", "2000000000", spin])


def drain(t):
    out, end = b"", time.time() + t
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.05)
        if r:
            try:
                out += os.read(fd, 65536)
            except OSError:
                break
    return out


before = drain(0.5)
os.write(fd, b"\x01m")
enter = drain(0.3)
os.write(fd, b"stop\r")
stopped = drain(0.5)
os.write(fd, b"status\r")
status = drain(0.3)
os.write(fd, b"cont\r")
time.sleep(0.2)
os.write(fd, b"\x01m")   # cont gave the terminal back already: this switches to the monitor
again = drain(0.3)
os.write(fd, b"\x01m")
back = drain(0.5)
os.write(fd, b"\x01x")
rest = drain(1.0)
_, wst = os.waitpid(pid, 0)
check(before.count(b"A") > 100, "console: the guest prints")
check(b"ember-mon> " in enter and b"A" not in enter.split(b"ember-mon> ")[-1],
      "console: Ctrl-A m gives the prompt, guest output is held")
check(b"state: stopped" in stopped and b"! stopped reason=monitor" in stopped,
      "console: stop from the prompt")
check(b"state: stopped" in status and b"reason: monitor" in status, "console: status")
check(b"ember-mon> " in again, "console: back to the prompt after cont")
check(back.count(b"A") > 0, "console: Ctrl-A m again writes the held output out")
check(os.WIFEXITED(wst) and os.WEXITSTATUS(wst) == 0, "console: Ctrl-A x still quits")

print("%d failure(s)" % fails)
sys.exit(1 if fails else 0)
