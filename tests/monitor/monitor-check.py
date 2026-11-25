#!/usr/bin/env python3
"""monitor-check.py - the monitor (uam-run --monitor), over its ways in.

    monitor-check.py UAM_RUN UAM_MON UAM_TEST_RUN

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

runner, uam_mon, test_runner = sys.argv[1:4]
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

# ---- phase 1: step, until, run-for, cpu; registers and memory; map, history, bt ----------
for jit in ([], ["--jit"]):
    name = "jit" if jit else "interp"
    p, path = start(*jit, "--monitor-wait", "--history", "8", "--backtrace", "-n", "100000000")
    m = Mon(path)
    ok, out, _ = m.cmd("step")
    st = fields(out)
    check(ok and st.get("reason") == "step" and st.get("icount") == "1" and st.get("pc") == "0x3",
          "%s: step runs one instruction (%s)" % (name, st))
    ok, out, _ = m.cmd("step 3")
    st = fields(out)
    check(st.get("icount") == "4" and st.get("pc") == "0x9", "%s: step 3" % name)
    ok, out, _ = m.cmd("reg rcx")
    check(ok and fields(out).get("rcx") == "0x0000000000001000", "%s: reg reads" % name)
    ok, out, _ = m.cmd("reg rcx 5")
    check(ok and fields(out).get("rcx") == "0x0000000000000005", "%s: reg writes" % name)
    ok, out, _ = m.cmd("until 0x6")
    st = fields(out)
    # five turns of 'loop $' (rcx 5), the jmp, then mov dx / mov al / out: at 0x6
    check(st.get("reason") == "until" and st.get("pc") == "0x6" and st.get("icount") == "13",
          "%s: until PC (%s)" % (name, st))
    ok, out, _ = m.cmd("until 0x6")
    st = fields(out)
    check(st.get("pc") == "0x6" and int(st.get("icount", 0)) > 13,
          "%s: until where it stands runs to the next arrival" % name)
    ok, out, _ = m.cmd("status")
    t0 = int(fields(out)["time"])
    ok, out, _ = m.cmd("run-for 1000")
    ok2, out2, _ = m.cmd("status")
    check(fields(out).get("reason") == "run-for" and int(fields(out2)["time"]) == t0 + 1000,
          "%s: run-for 1000 ticks" % name)
    ok, out, _ = m.cmd("x p:0 16")
    check(ok and out and out[0].startswith("0000000000000000: ba f8 03 b0 41 ee b9 00 10 e2 fe eb f3"),
          "%s: x shows bytes (%s)" % (name, out[:1]))
    ok, out, _ = m.cmd("x 0 8 w4")
    check(ok and out == ["0000000000000000: 0xb003f8ba 0x00b9ee41"], "%s: x w4 in target order" % name)
    ok, _, _ = m.cmd("write p:0x2000 w2 0x1234 0xabcd")
    ok2, out, _ = m.cmd("x p:0x2000 4")
    check(ok and out and " 34 12 cd ab " in out[0], "%s: write w2" % name)
    ok, _, err = m.cmd("write p:0x2000 w1 0x100")
    check(ok is False and "1-byte" in err, "%s: write refuses a value too wide" % name)
    blob = os.path.join(tmp, "blob.bin")
    with open(blob, "wb") as f:
        f.write(b"ember!")
    ok, out, _ = m.cmd("load p:0x3000 " + blob)
    ok2, out2, _ = m.cmd("x p:0x3000 6")
    check(ok and fields(out).get("written") == "6" and "|ember!|" in out2[0], "%s: load" % name)
    dumped = os.path.join(tmp, "dump.bin")
    ok, out, _ = m.cmd("dump p:0+13=" + dumped)
    with open(dumped, "rb") as f, open(spin, "rb") as g:
        check(ok and f.read() == g.read(), "%s: dump to a file" % name)
    ok, _, err = m.cmd("x p:0x7fffffffffff 4")
    check(ok is False and "nothing is mapped" in err, "%s: x where nothing is mapped (%s)" % (name, err))
    o = m.jcmd("json on")
    o = m.jcmd("regs")
    check(o["ok"] and o["data"].get("rip") == "0x0000000000000009" and "eflags" in o["data"]
          and len(o["data"]) > 20, "%s: regs (as JSON data)" % name)
    m.cmd("json off")
    ok, _, err = m.cmd("reg nope")
    check(ok is False and "rax" in err, "%s: reg names the registers when it has no such" % name)
    ok, out, _ = m.cmd("map")
    check(ok and any("com1" in l for l in out) and any(l.split()[1] == "ram" for l in out),
          "%s: map" % name)
    ok, out, _ = m.cmd("history 3")
    check(ok and len(out) == 3 and all(l.startswith("blk ") for l in out), "%s: history 3 (%s)" % (name, out))
    ok, out, _ = m.cmd("bt")
    check(ok and out == ["(no calls)"], "%s: bt with no calls" % name)
    ok, out, _ = m.cmd("cpu")
    check(ok and fields(out).get("cpu") == "0", "%s: cpu" % name)
    ok, _, err = m.cmd("cpu 1")
    check(ok is False, "%s: cpu refuses a processor the machine has not" % name)
    m.cmd("cont")
    ok, _, err = m.cmd("regs")
    check(ok is False and "running" in err, "%s: regs refused while running" % name)
    ok, _, err = m.cmd("step")
    check(ok is False and "running" in err, "%s: step refused while running" % name)
    ok, out, _ = m.cmd("map")
    check(ok, "%s: map works while running" % name)
    m.cmd("quit")
    finish(p)

# history and bt say how to have them
p, path = start("--monitor-wait", "-n", "1000")
m = Mon(path)
ok, _, err = m.cmd("history")
check(ok is False and "history on" in err, "history: says how to keep it")
ok, _, err = m.cmd("bt")
check(ok is False and "--backtrace" in err, "bt: says to start with --backtrace")
ok, _, err = m.cmd("step 2000")   # past the end of the run (no --monitor-hold): it ends
check(ok is False and "ended" in (err or ""), "a step past the end is answered: %s" % err)
rc, _, _ = finish(p)
check(rc == 124, "... and the run ends at its limit (%s)" % rc)
script = os.path.join(tmp, "step.mon")
with open(script, "w") as f:
    f.write("step\n")
rc, _, err = finish(subprocess.Popen(BASE + ["--monitor-script", script, "-n", "1000", spin],
                                     stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE))
check(rc == 2 and b"not in a script" in err, "script: step is refused there")

# ---- phase 2: break, guard, watch, rule, history on/off, trace, mmio-rule, wait ------------
# mov [0x5000],ax; jmp 0: a store on every turn
store = os.path.join(tmp, "store.bin")
with open(store, "wb") as f:
    f.write(bytes.fromhex("a30050ebfb"))
for jit in ([], ["--jit"]):
    name = "jit" if jit else "interp"
    p, path = start(*jit, "--monitor-wait", "-n", "100000000")
    m = Mon(path)
    ok, out, _ = m.cmd("break add 0x6")
    check(ok and fields(out).get("id") == "1", "%s: break add" % name)
    ok, _, err = m.cmd("break add 0x6")
    check(ok is False and "already" in err, "%s: one breakpoint per pc" % name)
    m.cmd("cont")
    ok, out, _ = m.cmd("wait")
    st = fields(out)
    check(st.get("reason") == "break 1" and st.get("pc") == "0x6" and st.get("icount") == "3",
          "%s: the breakpoint stops the run into the monitor (%s)" % (name, st))
    m.cmd("cont")
    ok, out, _ = m.cmd("wait")
    st = fields(out)
    check(st.get("reason") == "break 1" and int(st.get("icount")) > 3,
          "%s: cont from a breakpoint goes on to its next hit" % name)
    ok, out, _ = m.cmd("break list")
    check(out == ["1 pc=0x6 hits=2"], "%s: break list counts hits (%s)" % (name, out))
    m.cmd("break del 1")
    ok, out, _ = m.cmd("break list")
    check(ok and out == [], "%s: break del" % name)
    ok, out, _ = m.cmd("rule add pc=0x9,if rcx==0x800,name=half,stop")
    check(ok and fields(out).get("id") == "0", "%s: rule add" % name)
    m.cmd("cont")
    ok, out, _ = m.cmd("wait")
    ok2, out2, _ = m.cmd("reg rcx")
    check(fields(out).get("reason") == "rule half" and fields(out2).get("rcx") == "0x0000000000000800",
          "%s: a rule's stop stops into the monitor, its condition held" % name)
    ok, out, _ = m.cmd("rule list")
    check(ok and out and out[0].startswith("0 half on ") and "fired=1" in out[0],
          "%s: rule list (%s)" % (name, out))
    m.cmd("rule off 0")
    ok, out, _ = m.cmd("rule list")
    check(out and out[0].startswith("0 half off "), "%s: rule off" % name)
    ok, _, err = m.cmd("rule add pc=nosuch,stop")
    check(ok is False, "%s: a bad rule is refused (%s)" % (name, err))
    m.cmd("rule del 0")
    ok, out, _ = m.cmd("rule list")
    check(out == [], "%s: rule del" % name)
    ok, out, _ = m.cmd("history on 4")
    check(ok and fields(out).get("kept") == "4", "%s: history on" % name)
    m.cmd("step 10")
    ok, out, _ = m.cmd("history")
    check(ok and len(out) == 5 and out[0].startswith("cpu0: the last 4"), "%s: history kept" % name)
    m.cmd("history clear")
    ok, out, _ = m.cmd("history off")
    ok2, _, err = m.cmd("history")
    check(ok and ok2 is False, "%s: history off" % name)
    ok, out, _ = m.cmd("trace")
    check(fields(out).get("traces") == "none", "%s: trace shows none" % name)
    tracef = os.path.join(tmp, "trace-%s.txt" % name)
    m.cmd("trace file " + tracef)
    ok, out, _ = m.cmd("trace on mmio,blocks")
    check(fields(out).get("traces") == "blocks,mmio", "%s: trace on (%s)" % (name, out))
    m.cmd("reg rcx 1")   # out of the delay loop at once: the next turn writes to COM1
    m.cmd("step 20")
    m.cmd("trace off all")
    ok, out, _ = m.cmd("trace")
    check(fields(out).get("traces") == "none", "%s: trace off all" % name)
    m.cmd("trace file -")
    with open(tracef) as f:
        t = f.read()
    check("com1" in t and "blk" in t, "%s: the traces went to the trace file" % name)
    ok, _, err = m.cmd("trace on nonsense")
    check(ok is False and "unknown trace" in err, "%s: an unknown trace" % name)
    ok, out, _ = m.cmd("mmio-rule 0x90000000+8=fixed:0x55")
    ok2, out2, _ = m.cmd("map")
    check(ok and any("rule@0x90000000#mon0" in l for l in out2), "%s: mmio-rule maps a stub" % name)
    ok, _, err = m.cmd("mmio-rule 0x90000000=input")
    check(ok is False and "input" in err, "%s: mmio-rule refuses input" % name)
    m.cmd("cont")
    ok, _, err = m.cmd("break add 0x0")
    check(ok is False and "running" in err, "%s: break refused while running" % name)
    ok, out, _ = m.cmd("trace on irq")
    check(ok, "%s: trace on irq works while running" % name)
    ok, _, err = m.cmd("trace on blocks")
    check(ok is False and "stop first" in err, "%s: trace on blocks needs it stopped" % name)
    m.cmd("quit")
    finish(p)

    # guards and watches on a store
    sock = os.path.join(tmp, "g%s.sock" % name)
    p = subprocess.Popen(BASE + jit + ["--monitor", "unix:" + sock, "--monitor-wait",
                                       "-n", "100000000", store],
                         stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    for _ in range(200):
        if os.path.exists(sock):
            break
        time.sleep(0.01)
    m = Mon(sock)
    ok, out, _ = m.cmd("guard add p:0x5000+2:w")
    check(ok and fields(out).get("id") == "1", "%s: guard add" % name)
    m.cmd("reg rax 0x1234")
    m.cmd("cont")
    ok, out, _ = m.cmd("wait")
    st = fields(out)
    # the store that was refused counts in icount, its effect does not happen
    ok2, out2, _ = m.cmd("x p:0x5000 2")
    check(st.get("reason") == "guard 1" and st.get("pc") == "0x0" and st.get("icount") == "1"
          and " 00 00 " in out2[0], "%s: a guard stops at the store, before it (%s)" % (name, st))
    m.cmd("cont")
    ok, out, _ = m.cmd("wait")
    st = fields(out)
    ok2, out2, _ = m.cmd("x p:0x5000 2")
    check(st.get("reason") == "guard 1" and st.get("pc") == "0x0" and st.get("icount") == "4"
          and " 34 12 " in out2[0],
          "%s: cont steps over the guarded store (it happens), the next one stops again (%s)"
          % (name, st))
    ok, out, _ = m.cmd("guard list")
    check(out and "hits=2" in out[0], "%s: guard list (%s)" % (name, out))
    m.cmd("guard del 1")
    ok, out, _ = m.cmd("watch add p:0x5000+2:w")
    check(ok and fields(out).get("id") == "2", "%s: watch add" % name)
    m.cmd("step 6")
    ev = m.event("watch")
    check(ev is not None and "id=2" in ev and "write" in ev, "%s: a watch is an event (%s)" % (name, ev))
    ok, out, _ = m.cmd("watch list")
    check(out and "hits=3" in out[0], "%s: watch counts its hits (%s)" % (name, out))
    ok, _, err = m.cmd("guard del 7")
    check(ok is False, "%s: guard del of none" % name)
    m.cmd("quit")
    finish(p)

# ---- phase 3: snap, reset, devices, dev, heap, irq, cpustate -----------------------------
for jit in ([], ["--jit"]):
    name = "jit" if jit else "interp"
    snapf = os.path.join(tmp, "s-%s.snap" % name)
    p, path = start(*jit, "--monitor-wait", "--monitor-hold", "-n", "20000")
    m = Mon(path)
    m.cmd("step 100")
    ok, out, _ = m.cmd("snap save a " + snapf)
    st = fields(out)
    check(ok and st.get("snapshot") == "a" and st.get("icount") == "100" and os.path.exists(snapf),
          "%s: snap save, to a file too" % name)
    m.cmd("reg rcx 7")
    m.cmd("step 50")
    ok, out, _ = m.cmd("snap list")
    check(ok and out and out[0].startswith("a pc=0x9 icount=100 "), "%s: snap list (%s)" % (name, out))
    ok, out, _ = m.cmd("snap load a")
    ok2, out2, _ = m.cmd("status")
    ok3, out3, _ = m.cmd("reg rcx")
    check(ok and fields(out2).get("icount") == "100" and fields(out2).get("reason") == "snap"
          and fields(out3).get("rcx") != "0x0000000000000007",
          "%s: snap load puts the machine back (registers too)" % name)
    ev = m.event("stopped reason=snap")
    check(ev is not None, "%s: a stopped event for the restore" % name)
    m.cmd("cont")
    ok, out, _ = m.cmd("wait")
    check(fields(out).get("state") == "ended", "%s: the run ends at its limit" % name)
    ok, out, _ = m.cmd("snap load a")
    ok2, out2, _ = m.cmd("status")
    check(ok and fields(out2).get("state") == "stopped" and fields(out2).get("icount") == "100",
          "%s: snap load after the end brings the run back" % name)
    m.cmd("step 10")
    ok, out, _ = m.cmd("status")
    check(fields(out).get("icount") == "110", "%s: ... and it runs on" % name)
    ok, out, _ = m.cmd("snap load " + snapf)
    ok2, out2, _ = m.cmd("status")
    check(ok and fields(out2).get("icount") == "100", "%s: snap load from a file" % name)
    m.cmd("snap read b " + snapf)
    ok, out, _ = m.cmd("snap list")
    check(len(out) == 2 and out[1].startswith("b "), "%s: snap read" % name)
    wf = os.path.join(tmp, "w-%s.snap" % name)
    ok, _, _ = m.cmd("snap write b " + wf)
    check(ok and os.path.exists(wf), "%s: snap write" % name)
    m.cmd("snap drop b")
    ok, out, _ = m.cmd("snap list")
    check(len(out) == 1, "%s: snap drop" % name)
    ok, _, err = m.cmd("snap load nosuch")
    check(ok is False and "no snapshot" in err, "%s: snap load of nothing" % name)
    m.cmd("step 3")
    ok, out, _ = m.cmd("reset")
    ok2, out2, _ = m.cmd("reg rip")
    check(ok and fields(out).get("pc") == "0x0" and fields(out2).get("rip") == "0x0000000000000000",
          "%s: reset" % name)
    ok, out, _ = m.cmd("devices")
    check(ok and any(l.startswith("com1 16550a-uart regions=1") and l.endswith("describe")
                     for l in out) and any(l.startswith("pit ") for l in out), "%s: devices" % name)
    ok, out, _ = m.cmd("dev com1")
    check(ok and fields(out).get("type") == "16550a-uart" and any(l.startswith("region com1 ") for l in out)
          and any(l.startswith("ier=0x00 lcr=") for l in out), "%s: dev com1 describes it" % name)
    ok, out, _ = m.cmd("dev pit")
    check(ok and any("neither describe" in l for l in out), "%s: dev of a device that cannot" % name)
    ok, _, err = m.cmd("dev nosuch")
    check(ok is False and "no device" in err, "%s: dev of none" % name)
    ok, out, _ = m.cmd("heap")
    check(ok and out == ["(the board has no guarded heap)"], "%s: heap with none" % name)
    ok, out, _ = m.cmd("irq 4 1")
    ok2, out2, _ = m.cmd("irq")
    check(ok and out2 == ["cpu 0 line 4 level 1"], "%s: irq drives a line, and lists it" % name)
    m.cmd("irq 4 0")
    ok, out, _ = m.cmd("cpustate")
    check(ok and any(l.startswith("cr0=") for l in out), "%s: cpustate (system registers)" % name)
    m.cmd("quit")
    finish(p)

# a guarded heap on the RISC-V test board (an arena, no allocator hooked: nothing allocated)
with open(os.path.join(tmp, "rvspin.bin"), "wb") as f:
    f.write(bytes.fromhex("6f000000"))   # j .
hs = os.path.join(tmp, "h.sock")
p = subprocess.Popen([test_runner, "-b", "rv32-test", "-o", "heap=0x80100000+0x1000",
                      "--raw-base", "0x80000000", "--monitor", "unix:" + hs, "--monitor-wait",
                      "-n", "1000", os.path.join(tmp, "rvspin.bin")],
                     stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
m = Mon(hs)
ok, out, _ = m.cmd("heap")
check(ok and out and out[0].startswith("heap 0x80100000+0x1000 allocs=0 "), "heap: lists the board's (%s)" % out)
ok, _, err = m.cmd("heap 0x80100010")
check(ok is False and "no block" in err, "heap ADDR: in no block")
m.cmd("quit")
finish(p)

# ---- phase 4: send, expect, tap, input, link -------------------------------------------------
# wait for COM1 to have a byte, read it, write it back: an echo
echo = os.path.join(tmp, "echo.bin")
with open(echo, "wb") as f:
    f.write(bytes.fromhex("bafd03eca80174f8baf803eceeebf1"))


def start_on(binary, *extra):
    path = os.path.join(tmp, "e%d.sock" % time.monotonic_ns())
    p = subprocess.Popen(BASE + ["--monitor", "unix:" + path] + list(extra) + [binary],
                         stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    for _ in range(200):
        if os.path.exists(path):
            break
        time.sleep(0.01)
    return p, path


for jit in ([], ["--jit"]):
    name = "jit" if jit else "interp"
    p, path = start_on(echo, *jit, "-n", "4000000000")
    m = Mon(path)
    ok, out, _ = m.cmd('send console "hello\\n"')
    check(ok and fields(out).get("sent") == "6", "%s: send" % name)
    ok, out, _ = m.cmd('expect console llo 5')
    check(ok and fields(out).get("matched") == "yes", "%s: expect sees the echo that came before it" % name)
    ok, _, err = m.cmd('expect console llo 0.3')
    check(ok is False and "timed out" in err, "%s: a match is used up; expect times out" % name)
    ok, _, err = m.cmd('send nosuch x')
    check(ok is False and "console" in err, "%s: send to no such chardev names the ones there are" % name)
    m.cmd("tap console on")
    m.cmd('send console hex:6f6b0a')   # "ok\n"
    ev = m.event("output")
    check(ev == '! output chardev=console text="ok\\n"', "%s: tap: the output as an event (%s)" % (name, ev))
    m.cmd("tap console off")
    o = m.jcmd("json on")
    m.jcmd("tap console on")
    m.jcmd('send console "a b"')
    o = m.jcmd("expect console b 5")
    seen = [e for e in m.events if isinstance(e, dict) and e.get("event") == "output"]
    while not seen:
        line = json.loads(m.f.readline())
        if line.get("event") == "output":
            seen.append(line)
    check(o.get("ok") and seen[0].get("chardev") == "console" and "a" in seen[0].get("text", ""),
          "%s: tap in JSON (%s)" % (name, seen[:1]))
    m.jcmd("tap console off")
    m.cmd("json off")
    ok, out, _ = m.cmd("stop")
    ok, out, _ = m.cmd("input")
    check(ok and "com1" in out, "%s: input lists the board's inputs (%s)" % (name, out))
    ok, out, _ = m.cmd("input com1 xyz")
    check(ok and fields(out).get("delivered") == "3", "%s: input" % name)
    ok, _, err = m.cmd("input nosuch x")
    check(ok is False and "no input" in err, "%s: input of none" % name)
    m.cmd("cont")
    ok, out, _ = m.cmd("expect console xyz 5")
    check(ok, "%s: what input handed in comes back" % name)
    ok, _, err = m.cmd("input com1 x")
    check(ok is False and "running" in err, "%s: input needs it stopped" % name)
    m.cmd("quit")
    finish(p)

# an expect the run's end beats
p, path = start_on(spin, "--monitor-wait", "-n", "300000")
m = Mon(path)
m.cmd("cont")
ok, _, err = m.cmd("expect console neverprinted")
check(ok is False and "ended" in (err or ""), "expect: the run ended first (%s)" % err)
finish(p)

# links: the console on a link port with no hub
p, path = start_on(echo, "--link", "p0=none", "--console", "link:p0", "-n", "4000000000")
m = Mon(path)
ok, out, _ = m.cmd("link")
check(ok and out and out[0].startswith("node ") and out[1].startswith("port p0 bus=p0 down "),
      "link: status (%s)" % out)
m.cmd("link tap on")
ok, out, _ = m.cmd('link inject p0 "hi\\n"')
check(ok and fields(out).get("injected") == "3", "link inject")
ev_in = m.event("link port=p0 dir=in")
ev_out = m.event("link port=p0 dir=out")
check(ev_in is not None and 'data="hi\\n"' in ev_in and ev_out is not None,
      "link tap: in and out as events (%s / %s)" % (ev_in, ev_out))
ok, _, _ = m.cmd("expect console hi 5")
check(ok, "expect on a console that is a link port")
ok, out, _ = m.cmd("link")
check(any("rx=1" in l for l in out), "link: counts what came in")
ok, _, err = m.cmd("link inject nosuch x")
check(ok is False and "no port" in err, "link inject to no such port")
m.cmd("quit")
finish(p)
p, path = start_on(echo, "--link", "p0=none", "--console", "link:p0", "-t", "link",
                   "-n", "4000000000")
m = Mon(path)
ok, _, err = m.cmd("link tap on")
check(ok is False and "-t link" in err, "link tap: taken by -t link")
m.cmd("quit")
finish(p)

# ---- phase 5: gdb's "monitor CMD", --monitor-log and its replay ---------------------------
gport = 46000 + (os.getpid() % 9000)
p = subprocess.Popen(BASE + ["--gdb", str(gport), spin], stdin=subprocess.DEVNULL,
                     stdout=subprocess.PIPE, stderr=subprocess.PIPE)
gs = None
for _ in range(200):
    try:
        gs = socket.create_connection(("127.0.0.1", gport), timeout=10)
        break
    except OSError:
        time.sleep(0.02)


def rsp_send(d):
    gs.sendall(b"$" + d.encode() + b"#%02x" % (sum(d.encode()) & 0xff))


def rsp_get():
    buf = b""
    while gs.recv(1) != b"$":
        pass
    while True:
        c = gs.recv(1)
        if c == b"#":
            gs.recv(2)
            gs.sendall(b"+")
            return buf.decode()
        buf += c


def rcmd(cmd):
    rsp_send("qRcmd," + cmd.encode().hex())
    out = ""
    while True:
        r = rsp_get()
        if r.startswith("O") and r != "OK":
            out += bytes.fromhex(r[1:]).decode()
        else:
            return out, r


out, r = rcmd("status")
check(r == "OK" and "state: stopped" in out and "reason: debugger" in out and not out.endswith("ok\n"),
      "gdb: monitor status (%r)" % out[:60])
out, r = rcmd("x p:0 4")
check(out.startswith("0000000000000000: ba f8 03 b0"), "gdb: monitor x")
out, r = rcmd("reg rcx 0x99")
rsp_send("p2")   # gdb register 2 on amd64: rcx
check("rcx: 0x0000000000000099" in out and rsp_get().startswith("99000000"),
      "gdb: monitor reg sets what gdb then reads")
out, r = rcmd("step")
check("not from here" in out and r == "OK", "gdb: run control stays gdb's (%r)" % out)
out, r = rcmd("snap save a")
check("snapshot: a" in out, "gdb: monitor snap save")
out, r = rcmd("nonsense")
check(out.startswith("error: unknown command"), "gdb: an unknown monitor command")
rsp_send("k")
finish(p)

for jit in ([], ["--jit"]):
    name = "jit" if jit else "interp"
    log = os.path.join(tmp, "session-%s.log" % name)
    common = jit + ["-d", "--dump-mem", "p:0x3000+4", "-n", "300000"]
    p, path = start("--monitor-wait", "--monitor-log", log, *common)
    m = Mon(path)
    m.cmd("step 1000")
    m.cmd("reg rcx 3")
    m.cmd("step 5000")
    m.cmd("write p:0x3000 w4 0xdeadbeef")
    m.cmd("status")             # reads nothing into the log
    m.cmd("step 1182")          # (a cont; stop here would race the end of a fast run)
    m.cmd("reg rax 0x77")
    m.cmd("cont")
    rc1, _, err1 = finish(p)
    with open(log) as f:
        lines = [l.strip() for l in f if not l.startswith("#")]
    check(lines == ["@1000 reg rcx 3", "@6000 write p:0x3000 w4 0xdeadbeef", "@7182 reg rax 0x77"],
          "%s: --monitor-log records what changed the machine, and when (%s)" % (name, lines))
    r = subprocess.run(BASE + ["--monitor-script", log] + common + [spin], capture_output=True,
                       timeout=60)
    keep = lambda e: [l for l in e.decode().splitlines() if not l.startswith("uam-run: monitor")]
    check(r.returncode == rc1 and keep(r.stderr) == keep(err1),
          "%s: --monitor-script replays it: the same end, registers and memory" % name)
bad = os.path.join(tmp, "bad.log")
with open(bad, "w") as f:
    f.write("@12x status\n")
r = subprocess.run(BASE + ["--monitor-script", bad, "-n", "10", spin], capture_output=True, timeout=30)
check(r.returncode == 2 and b"expected @ICOUNT COMMAND" in r.stderr, "a malformed @ line")

# ---- review fixes: robustness and edge cases ---------------------------------------------
# a client that never reads does not hold the run (or other clients) up
p, path = start("-n", "4000000000")
slow = socket.socket(socket.AF_UNIX)
slow.connect(path)
slow.sendall(b"tap console on\n")   # floods it with output events it never reads
time.sleep(3)
m = Mon(path)
ok, out, _ = m.cmd("status")
check(ok and fields(out).get("state") == "running", "a client that does not read holds nothing up")
p.send_signal(signal.SIGINT)
rc, _, _ = finish(p)
check(rc == 130, "... and SIGINT still ends the run (%s)" % rc)
slow.close()

# the rest of a too-long line is not a command
p, path = start("--monitor-wait", "-n", "100000")
m = Mon(path)
m.f.write("help " + "x" * 9000 + "quit 9\n")
m.f.flush()
ok, out, _ = m.cmd("status")
check(ok and fields(out).get("state") == "stopped", "a too-long line is dropped whole (status %s)" % ok)
m.cmd("quit 4")
rc, _, _ = finish(p)
check(rc == 4, "... its tail never ran (exit %s)" % rc)

# wait answers at the end of a run without --monitor-hold
p, path = start("--monitor-wait", "-n", "100000")
m = Mon(path)
m.cmd("cont")
ok, out, err = m.cmd("wait")
check(ok and fields(out).get("state") == "ended", "wait: the end of the run is its answer (%s)" % err)
finish(p)

# a script that leaves the machine stopped with no way in: an error, not a spin
script = os.path.join(tmp, "stuck.mon")
with open(script, "w") as f:
    f.write("stop\n")
r = subprocess.run(BASE + ["--monitor-script", script, "-n", "1000", spin], capture_output=True,
                   timeout=30)
check(r.returncode == 2 and b"nothing can reach" in r.stderr,
      "a stopped machine nothing can reach is an error (exit %s)" % r.returncode)

# --monitor stdio: an expect whose input then ends is still answered, the lines after it run
r = subprocess.run(BASE + ["--console", "file:/dev/null", "--monitor", "stdio", "-n", "4000000000",
                           echo], input=b'expect console ZZZ 0.5\nquit 3\n', capture_output=True,
                   timeout=30)
check(r.returncode == 3 and b"timed out" in r.stdout,
      "stdio: an expect still pending at the end of input is answered (exit %s)" % r.returncode)

# rules past 256 from a file, and their del
many = os.path.join(tmp, "many.rules")
with open(many, "w") as f:
    for i in range(300):
        f.write("pc=0x%x,count=1,name=r%d,log\n" % (0x100000 + 4 * i, i))
p, path = start("--monitor-wait", "-n", "100000")
m = Mon(path)
ok, out, _ = m.cmd("rule load " + many)
check(ok and len([l for l in out if l.startswith("id:")]) == 300, "rule load takes 300 rules")
ok, _, _ = m.cmd("rule del 299")
ok2, _, _ = m.cmd("rule del 150")
ok3, out, _ = m.cmd("rule list")
check(ok and ok2 and len(out) == 298 and not any(l.startswith("299 ") for l in out),
      "rule del past 256")
ok, _, err = m.cmd("rule del 299")
check(ok is False, "a deleted rule is gone")
m.cmd("quit")
rc, _, _ = finish(p)
check(rc == 0, "... and the run ends cleanly (exit %s)" % rc)

# a watch with ",stop" stops into the monitor; trace on all; strict hex
p = subprocess.Popen(BASE + ["--monitor", "unix:" + os.path.join(tmp, "w.sock"), "--monitor-wait",
                             "-n", "100000000", store],
                     stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
m = Mon(os.path.join(tmp, "w.sock"))
m.cmd("watch add p:0x5000+2:w,stop")
m.cmd("cont")
ok, out, _ = m.cmd("wait")
check(fields(out).get("state") == "stopped" and fields(out).get("reason", "").startswith("guard"),
      "a watch with ,stop stops into the monitor (%s)" % fields(out))
ok, out, err = m.cmd("trace on all")
check(ok, "trace on all (without -t link) (%s)" % err)
m.cmd("trace off all")
ok, _, err = m.cmd("send console hex:0x41")
check(ok is False and "digits" in err, "hex: takes digits only")
ok, _, err = m.cmd('send console "\\x-1"')
check(ok is False, "\\x takes hex digits only")
ok, _, _ = m.cmd("irq 40 1")
check(ok, "irq on a line the ISA has not is harmless")
m.cmd("quit")
finish(p)

# a replayed log keeps the processor a command was for
p, path = start("--monitor-wait", "--monitor-log", os.path.join(tmp, "cpu.log"), "-o", "cpus=2",
                "-d", "-n", "20000")
m = Mon(path)
m.cmd("cpu 1")
m.cmd("reg rbx 0x4242")
m.cmd("cpu 0")
m.cmd("reg rbx 0x77")
m.cmd("cont")
rc1, _, err1 = finish(p)
r = subprocess.run(BASE + ["--monitor-script", os.path.join(tmp, "cpu.log"), "-o", "cpus=2", "-d",
                           "-n", "20000", spin], capture_output=True, timeout=60)
keep = lambda e: [l for l in e.decode().splitlines() if not l.startswith("uam-run: monitor")]
check(r.returncode == rc1 and keep(r.stderr) == keep(err1) and b"0000000000004242" in r.stderr,
      "a replayed log writes each register on the processor it was for")

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
