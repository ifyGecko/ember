#!/usr/bin/env python3
"""status_tui.py -- a live status board for ember's fuzz brain.

Follows the JSON-lines status feed that fuzz/tools/fuzz_brain.py writes with
--status-file and renders it as a static terminal board: boxed "process
timing / overall results / map coverage / stage progress / findings" panels,
a run-config box (ember's arch, coverage scheme, map size, ...) and a log of
the latest events.

  python3 fuzz/tools/fuzz_brain.py --listen 127.0.0.1:7000 \\
      --status-file fuzz_out/status.jsonl ... &
  python3 fuzz/tools/status_tui.py fuzz_out/status.jsonl          # live board
  python3 fuzz/tools/status_tui.py fuzz_out/status.jsonl --refresh 2
  python3 fuzz/tools/status_tui.py --once fuzz_out/status.jsonl   # one frame, no curses
  python3 fuzz/tools/status_tui.py --input - --once < status.jsonl
  python3 fuzz/tools/status_tui.py --demo                         # synthetic data
  python3 fuzz/tools/status_tui.py --selftest                     # render checks

The board is static: no hotkeys, no subscreens. It redraws itself every
--refresh seconds, flags a run that has stopped updating (STALLED) or ended
(RUN FINISHED), and exits on Ctrl-C. With --once, or when stdout is not a
terminal, it prints one frame (each row tagged by its highlight) and exits.
Stdlib-only (curses + json).

The feed: one JSON object per line. Scalars are "latest wins"; an
{"event": ...} line is appended to the event log. Keys (all optional --
unknown keys are ignored, missing ones render as "?"):

  t              run time, seconds
  execs          total executions
  execs_per_sec  average exec rate          (alias: "rate"); the current rate
                 is derived from the last ~10 s of (t, execs) snapshots
  corpus         corpus size
  cycles         corpus cycles done (else derived execs//corpus)
  edges          map entries hit (cumulative)
  map_size       coverage map size (for the density %)
  new_edges      edges found beyond the baseline (the initial corpus replay)
  init_edges     edges in ember's INIT map (boot up to the first begin)
  crashes        total crashes            crashes_unique  distinct (reason, status)
  hangs          total HANG/TIMEOUT       hangs_unique    distinct (reason, status)
  hangs_saved    hang inputs kept (the brain keeps each distinct hang input,
                 so this can exceed hangs_unique)
  rejected       inputs the input route refused (REJECTED; not bugs)
  stage          current stage, e.g. "havoc + splice"
  config         object of label:value pairs for the "run config" box
  finished       true when the run is over
  event          {"kind":"find"|"crash"|"hang"|"info", "msg":"...",
                  "edges":N (optional)}
"""
import argparse
import json
import os
import sys
import time
from collections import deque

DEFAULT_MAP = 65536


# ---- format helpers ------------------------------------------------------------

def fmt_n(x):
    try:
        return f"{int(x):,}"
    except (TypeError, ValueError):
        return str(x)


def fmt_rate(x):
    try:
        return f"{float(x):.1f}/sec"
    except (TypeError, ValueError):
        return "?"


def fmt_dur(sec):
    sec = int(max(0, sec))
    h, rem = divmod(sec, 3600)
    m, s = divmod(rem, 60)
    return f"{h}:{m:02d}:{s:02d}" if h else f"{m:02d}:{s:02d}"


def fmt_long_dur(sec):
    """'0 days, 1 hrs, 23 min, 45 sec'."""
    sec = int(max(0, sec))
    d, rem = divmod(sec, 86400)
    h, rem = divmod(rem, 3600)
    m, s = divmod(rem, 60)
    return f"{d} days, {h} hrs, {m} min, {s} sec"


def fmt_short_dur(sec):
    """Compact '0d 01:23:45' for panels too narrow for fmt_long_dur."""
    sec = int(max(0, sec))
    d, rem = divmod(sec, 86400)
    h, rem = divmod(rem, 3600)
    m, s = divmod(rem, 60)
    return f"{d}d {h:02d}:{m:02d}:{s:02d}"


# ---- boxed panels -------------------------------------------------------------

def _field(label, value, labw):
    return f"{label:>{labw}} : {value}"


def _panel(title, fields, width):
    """One bordered box; every returned line is exactly `width` columns wide."""
    inner = max(8, width - 2)
    pre = "─ " + title + " "
    if len(pre) > inner:
        pre = pre[:inner]
    out = ["┌" + pre + "─" * (inner - len(pre)) + "┐"]
    for f in fields:
        s = " " + f
        if len(s) > inner:
            s = s[:inner]
        out.append("│" + s + " " * (inner - len(s)) + "│")
    out.append("└" + "─" * inner + "┘")
    return out


def _pair(ltitle, lfields, rtitle, rfields, lw, rw):
    """Two panels side by side, padded to the same height so the boxes line up."""
    n = max(len(lfields), len(rfields))
    lf = lfields + [""] * (n - len(lfields))
    rf = rfields + [""] * (n - len(rfields))
    return [a + b for a, b in zip(_panel(ltitle, lf, lw), _panel(rtitle, rf, rw))]


def _config_fields(cfg, width):
    """label : value cells for every config entry; two per row when they fit."""
    if not cfg:
        return ["(no config in feed)"]
    items = [(str(k), str(v)) for k, v in cfg.items()]
    labw = min(14, max(len(k) for k, _ in items))
    cells = [_field(k, v, labw) for k, v in items]
    colw = (width - 3) // 2
    if width < 80 or any(len(c) > colw - 1 for c in cells):
        return cells
    return [cells[i].ljust(colw) + (cells[i + 1] if i + 1 < len(cells) else "")
            for i in range(0, len(cells), 2)]


# ---- generic status model ------------------------------------------------------

class Event:
    __slots__ = ("id", "kind", "text", "t", "edges")

    def __init__(self, id_, kind, text, t, edges=None):
        self.id, self.kind, self.text, self.t, self.edges = id_, kind, text, t, edges


class Status:
    """The brain's campaign state. Feed it with apply(obj); render() draws it."""

    SCALARS = {
        "t": "t", "execs": "execs", "execs_per_sec": "execs_per_sec",
        "rate": "execs_per_sec", "corpus": "corpus", "cycles": "cycles",
        "edges": "edges", "map_size": "map_size",
        "new_edges": "new_edges", "crashes": "crashes",
        "crashes_unique": "crashes_unique", "hangs": "hangs",
        "hangs_unique": "hangs_unique", "hangs_saved": "hangs_saved",
        "init_edges": "init_edges", "rejected": "rejected", "stage": "stage",
    }

    def __init__(self):
        self.t = 0.0
        self.execs = 0
        self.execs_per_sec = 0.0
        self.corpus = 0
        self.cycles = None
        self.edges = 0
        self.map_size = DEFAULT_MAP
        self.new_edges = 0
        self.crashes = 0
        self.crashes_unique = 0
        self.hangs = 0
        self.hangs_unique = 0
        self.hangs_saved = None
        self.init_edges = None
        self.rejected = None
        self.stage = "—"
        self.config = {}
        self.finished = False
        self.last_find_t = None
        self.last_crash_t = None
        self.last_hang_t = None
        self.last_msg = {}             # event kind -> text of the newest one
        self.finds = 0
        self._crash_evs = 0
        self._hang_evs = 0
        self.find_times = deque(maxlen=4000)
        self.events = deque(maxlen=400)
        self._ev_id = 0
        self._snaps = deque(maxlen=64)  # recent distinct (t, execs) snapshots
        self.last_update_wall = None   # wall-clock time of the newest feed data
        self.have_data = False

    # -- feed -------------------------------------------------------------------
    def apply(self, obj):
        """Merge one JSON status object (scalars latest-wins; event appended)."""
        if not isinstance(obj, dict):
            return
        touched_snapshot = False
        for k, attr in self.SCALARS.items():
            if k in obj:
                setattr(self, attr, obj[k])
                touched_snapshot = True
        cfg = obj.get("config")
        if isinstance(cfg, dict):
            self.config.update(cfg)
        if "finished" in obj:
            self.finished = bool(obj["finished"])
        if touched_snapshot:
            self.have_data = True
            if self._snaps and self._snaps[-1][0] == self.t:
                self._snaps[-1] = (self.t, self.execs)
            else:
                self._snaps.append((self.t, self.execs))
        ev = obj.get("event")
        if isinstance(ev, dict):
            self.add_event(ev.get("kind", "info"), ev.get("msg", ""),
                           ev.get("edges"))

    def add_event(self, kind, msg, edges=None):
        self.have_data = True
        self._ev_id += 1
        txt = msg
        if kind == "find" and edges is not None:
            txt = f"+{edges} edges  {msg}" if msg else f"+{edges} edges"
        ev = Event(self._ev_id, kind, txt, self.t, edges)
        self.events.append(ev)
        self.last_msg[kind] = txt
        if kind == "find":
            self.finds += 1
            self.find_times.append(self.t)
            self.last_find_t = self.t
        elif kind == "crash":
            self._crash_evs += 1
            self.last_crash_t = self.t
        elif kind == "hang":
            self._hang_evs += 1
            self.last_hang_t = self.t
        return ev

    # -- derived ----------------------------------------------------------------
    def uniq_crashes(self):
        return self.crashes_unique or self._crash_evs

    def uniq_hangs(self):
        # With hangs_saved in the feed, the hang events are saved inputs, not
        # distinct keys: keep the two apart (hangs_label).
        if self.hangs_saved is not None:
            return self.hangs_unique
        return max(self.hangs_unique, self._hang_evs)

    def hangs_label(self):
        uh = self.uniq_hangs()
        if self.hangs_saved is not None and self.hangs_saved != uh:
            return f"{fmt_n(self.hangs)}  ({uh} unique, {self.hangs_saved} saved)"
        return f"{fmt_n(self.hangs)}  ({uh} unique)"

    def cycles_done(self):
        if self.cycles is not None:
            return self.cycles
        return self.execs // self.corpus if self.corpus else 0

    def finds_per_min(self):
        if not self.find_times:
            return 0.0
        lo = self.t - 600.0
        n = sum(1 for x in self.find_times if x >= lo)
        return n * 60.0 / 600.0

    def current_rate(self, window=10.0):
        """execs/sec over roughly the last `window` seconds of snapshots (at
        least the last two), or None if unknown."""
        if len(self._snaps) < 2:
            return None
        snaps = list(self._snaps)
        t1, e1 = snaps[-1]
        try:
            base = next((s for s in snaps[:-1] if float(s[0]) >= float(t1) - window),
                        snaps[-2])
            t0, e0 = base
            dt = float(t1) - float(t0)
            return (float(e1) - float(e0)) / dt if dt > 0 else None
        except (TypeError, ValueError):
            return None


# ---- JSON-lines feed follower --------------------------------------------------

class JsonFeed:
    """Follows a growing JSON-lines file (or a FIFO); handles each line. The
    brain starts its feed anew each session: when the file shrinks, the board
    starts over with it."""

    def __init__(self, path, status):
        self.path = path
        self.st = status
        self.fh = None
        self.off = 0
        self.buf = b""

    def handle_line(self, ln):
        ln = ln.strip()
        if not ln:
            return
        try:
            obj = json.loads(ln)
        except ValueError:
            return
        self.st.apply(obj)

    def read_new(self):
        if self.fh is None:
            try:
                self.fh = open(self.path, "rb", buffering=0)
            except OSError:
                return 0
        try:
            if os.fstat(self.fh.fileno()).st_size < self.off:
                self.st.__init__()
                self.off, self.buf = 0, b""
            self.fh.seek(self.off)
            data = self.fh.read(1 << 20)
        except OSError:
            return 0
        if not data:
            return 0
        self.off += len(data)
        self.st.last_update_wall = time.time()
        blob = self.buf + data
        parts = blob.split(b"\n")
        self.buf = parts.pop()            # trailing partial line
        for ln in parts:
            self.handle_line(ln)
        return len(data)

    def initial_scan(self):
        while self.read_new():
            pass
        if self.st.last_update_wall is not None:
            try:                          # age of existing data = file's mtime
                self.st.last_update_wall = os.path.getmtime(self.path)
            except OSError:
                pass


def feed_from_stdin(status):
    for ln in sys.stdin:
        ln = ln.strip()
        if not ln:
            continue
        try:
            status.apply(json.loads(ln))
        except ValueError:
            continue


# ---- the renderer ---------------------------------------------------------------

def _clamp(rows, tw, h):
    """Truncate every row to the panel width so nothing overflows the frame."""
    return [(t if len(t) <= tw else t[:tw], a) for t, a in rows[:h]]


def _ev_line(ev):
    return f"  T+{fmt_dur(ev.t):>8}  {ev.text}", {
        "find": "ok", "crash": "err", "hang": "warn", "info": "dim",
    }.get(ev.kind, "")


def render(st, w, h, title="ember fuzz status", source=None,
           stall_after=None, now=None):
    """Render the status board as a list of (text, attr) rows.

    stall_after: seconds without new feed data before the run is flagged
    STALLED (None disables the check, e.g. for --demo or a drained stdin)."""
    now = time.time() if now is None else now
    rows = []
    tw = min(max(w - 1, 44), 104)
    rows.append((title.center(tw), "head"))
    feed = f"    feed : {source}" if source else ""

    if not st.have_data:
        rows.append((f"  status : waiting for status data (no feed lines yet){feed}",
                     "warn"))
        return _clamp(rows, tw, h)

    age = None if st.last_update_wall is None else max(0.0, now - st.last_update_wall)
    if st.finished:
        rows.append((f"  status : RUN FINISHED{feed}", "head"))
    elif stall_after is not None and age is not None and age > stall_after:
        rows.append((f"  status : STALLED: no feed update for {fmt_dur(age)}{feed}",
                     "err"))
    else:
        rows.append((f"  status : running{feed}", "ok"))

    pct = 100.0 * st.edges / st.map_size if st.map_size else 0.0
    uc = st.uniq_crashes()
    cur = st.current_rate()

    lw = (tw * 56) // 100          # the left column is the wider one
    rw = tw - lw

    def dur(sec):                  # the long form, unless it won't fit the box
        s = fmt_long_dur(sec)
        return s if len(s) <= lw - 23 else fmt_short_dur(sec)

    def ago(t_ev):
        if t_ev is None:
            return "none seen yet"
        return dur(max(0, st.t - t_ev))

    rows += [(ln, "") for ln in _pair("process timing", [
        _field("run time", dur(st.t), 16),
        _field("last new find", ago(st.last_find_t), 16),
        _field("last saved crash", ago(st.last_crash_t), 16),
        _field("last saved hang", ago(st.last_hang_t), 16),
        _field("last feed update", "?" if age is None else dur(age), 16),
    ], "overall results", [
        _field("cycles done", fmt_n(st.cycles_done()), 14),
        _field("corpus count", fmt_n(st.corpus), 14),
        _field("saved crashes", f"{fmt_n(st.crashes)}  ({uc} unique)", 14),
        _field("saved hangs", st.hangs_label(), 14),
        _field("rejected", "?" if st.rejected is None else fmt_n(st.rejected), 14),
    ], lw, rw)]

    rows += [(ln, "") for ln in _pair("map coverage", [
        _field("map edges", f"{fmt_n(st.edges)} / {fmt_n(st.map_size)}", 12),
        _field("map density", f"{pct:.2f}%", 12),
        _field("new edges", fmt_n(st.new_edges), 12),
        _field("boot (INIT)", "?" if st.init_edges is None else fmt_n(st.init_edges), 12),
    ], "stage progress", [
        _field("now trying", str(st.stage), 16),
        _field("total execs", fmt_n(st.execs), 16),
        _field("exec speed (avg)", fmt_rate(st.execs_per_sec), 16),
        _field("exec speed (now)", "?" if cur is None else
               fmt_rate(cur) + ("  (slow!)" if cur <= 1.0 else ""), 16),
        _field("new finds/min", f"{st.finds_per_min():.2f}", 16),
    ], lw, rw)]

    try:
        dups = fmt_n(max(0, int(st.crashes) - int(uc)))
    except (TypeError, ValueError):
        dups = "?"
    none = "none seen yet"
    rows += [(ln, "") for ln in _pair("findings in depth", [
        _field("corpus paths", fmt_n(st.corpus), 14),
        _field("new-cov finds", fmt_n(st.finds), 14),
        _field("total crashes", f"{fmt_n(st.crashes)}  ({uc} unique)", 14),
        _field("dup crashes", dups, 14),
        _field("total hangs", st.hangs_label(), 14),
    ], "latest findings", [
        _field("find", st.last_msg.get("find", none), 5),
        _field("crash", st.last_msg.get("crash", none), 5),
        _field("hang", st.last_msg.get("hang", none), 5),
        _field("info", st.last_msg.get("info", none), 5),
    ], lw, rw)]

    rows += [(ln, "") for ln in _panel("run config", _config_fields(st.config, tw - 2), tw)]

    rows.append((" last events", "dim"))
    n_ev = max(1, h - len(rows))
    for ev in list(st.events)[-n_ev:]:
        rows.append(_ev_line(ev))
    return _clamp(rows, tw, h)


# ---- curses UI -----------------------------------------------------------------

ATTR_MAP = {
    "": (0, 0), "dim": ("dim", 0), "head": ("bold", 7), "ok": (0, 2),
    "err": ("bold", 3), "warn": ("bold", 4),
}


def run_curses(feed, st, refresh, title, source, stall_after):
    """Static board: redraw every `refresh` seconds until Ctrl-C. No hotkeys."""
    import curses

    def on_term(scr):
        scr.timeout(max(50, int(refresh * 1000)))
        try:
            curses.use_default_colors()
            for i, name in enumerate(("black", "green", "red", "magenta",
                                      "yellow", "cyan", "white"), start=1):
                try:
                    curses.init_pair(i, getattr(curses, "COLOR_" + name.upper()),
                                     -1)
                except curses.error:
                    pass
        except curses.error:
            pass
        try:
            curses.curs_set(0)
        except curses.error:
            pass

        def attr(name):
            a, pair = ATTR_MAP.get(name, (0, 0))
            flags = 0
            if a == "bold":
                flags |= curses.A_BOLD
            elif a == "dim":
                flags |= curses.A_DIM
            if pair:
                flags |= curses.color_pair(pair)
            return flags

        while True:
            if feed is not None:
                feed.read_new()
            h, w = scr.getmaxyx()
            scr.erase()
            rows = render(st, w, h, title=title, source=source,
                          stall_after=stall_after)
            for r, (txt, a) in enumerate(rows[:h]):
                try:
                    scr.addnstr(r, 0, txt, max(0, w - 1), attr(a))
                except curses.error:
                    pass
            scr.refresh()
            scr.getch()        # timed wait only: picks up resizes, keys are ignored
            curses.flushinp()

    try:
        curses.wrapper(on_term)
    except KeyboardInterrupt:
        pass


# ---- demo + selftest -----------------------------------------------------------

def make_demo_status():
    st = Status()
    st.map_size = 131072
    for i in range(1, 46):
        t = i * 85
        edges = int(700 + 760 * (1 - 0.5 ** (i / 10.0)))
        st.apply({"t": t, "execs": i * 6800, "execs_per_sec": 79.5,
                  "corpus": 180 + i, "edges": edges, "map_size": 131072,
                  "new_edges": edges - 700, "init_edges": 640,
                  "crashes": i * 4, "rejected": i // 3,
                  "crashes_unique": min(i, 3), "stage": "havoc + splice",
                  "config": {"arch": "mips32", "scheme": "n=3 model=site rotate=1",
                             "map_size": 131072, "proto": 1,
                             "ember": "127.0.0.1:41234",
                             "listen": "0.0.0.0:7000", "out": "./out",
                             "seeds": 1, "resumed": 228, "max_size": 65536,
                             "minimize": "True"}})
        if i == 6:
            st.add_event("find", "reached parse_header", edges=180)
        if i == 20:
            st.add_event("crash", "FAULT status=0x80001234 crash_r1_s2147488308_3f9a01c2b7d4")
        if i == 40:
            st.add_event("find", "new state in handle_request()", edges=25)
    st.last_update_wall = time.time()
    return st


def selftest():
    st = make_demo_status()
    assert st.uniq_crashes() >= 1
    assert st.cycles_done() > 0
    titles = ["process timing", "overall results", "map coverage",
              "stage progress", "findings in depth", "latest findings",
              "run config", "last events"]

    def text(rows):
        return "\n".join(t for t, _ in rows)

    for w in (60, 80, 100, 132):
        rows = render(st, w, 60, source="demo", stall_after=10)
        blob = text(rows)
        for title in titles:
            assert title in blob, f"missing panel {title!r} at w={w}"
        for t, _ in rows:
            assert len(t) <= w, f"row wider than {w}: {t!r}"
        assert "running" in blob, f"run state missing at w={w}"
        assert "[q]uit" not in blob and not any(c in blob for c in "▁▂▃▄▅▆▇█"), \
            f"hotkey footer or chart glyphs at w={w}"
    blob = text(render(st, 132, 60))
    for k, v in st.config.items():
        assert f"{k} : {v}" in blob, f"config entry {k!r} not shown"
    assert "80.0/sec" in blob, "current exec speed not derived from snapshots"
    # current rate from two snapshots
    s2 = Status()
    s2.apply({"t": 10, "execs": 100})
    s2.apply({"t": 20, "execs": 600})
    assert abs(s2.current_rate() - 50.0) < 1e-9
    # stalled / finished run state
    s3 = Status()
    s3.apply({"t": 1, "execs": 1})
    s3.last_update_wall = time.time() - 3600
    assert "STALLED" in text(render(s3, 100, 40, stall_after=10))
    assert "STALLED" not in text(render(s3, 100, 40, stall_after=None))
    s3.apply({"finished": True})
    blob = text(render(s3, 100, 40, stall_after=10))
    assert "RUN FINISHED" in blob and "STALLED" not in blob
    # feed/apply round-trip
    s4 = Status()
    s4.apply({"execs": 10, "edges": 5, "map_size": 100,
              "event": {"kind": "find", "edges": 5}})
    assert s4.execs == 10 and s4.finds == 1 and s4.last_find_t is not None
    print("[selftest] ok: static board renders at 60/80/100/132 cols, "
          "full config, run state, apply() works")
    return 0


# ---- main ----------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(
        description="Live status board for ember's fuzz brain "
                    "(fuzz/tools/fuzz_brain.py --status-file).")
    ap.add_argument("feed", nargs="?", default=None,
                    help="the brain's JSON-lines status feed to follow")
    ap.add_argument("--input", "-i", default=None,
                    help="the same as FEED; '-' reads the feed from stdin")
    ap.add_argument("--refresh", type=float, default=1.0,
                    help="seconds between redraws (default 1.0)")
    ap.add_argument("--once", "--dump", dest="once", action="store_true",
                    help="print one text frame to stdout and exit (no curses)")
    ap.add_argument("--demo", action="store_true",
                    help="render synthetic data (no feed needed)")
    ap.add_argument("--selftest", action="store_true",
                    help="render checks on synthetic data; exit 0 on success")
    ap.add_argument("--title", default="ember fuzz status")
    args = ap.parse_args()
    if args.feed and args.input:
        ap.error("give the feed once: FEED or --input")
    args.input = args.input or args.feed

    if args.selftest:
        return selftest()
    if args.refresh <= 0:
        ap.error("--refresh must be > 0")

    st = Status()
    feed = None
    stall_after = None                 # only a followed file can stall
    if args.demo:
        st = make_demo_status()
        source = "demo"
    elif args.input == "-":
        feed_from_stdin(st)            # drain stdin (one-shot; implies --once)
        st.last_update_wall = time.time()
        source = "stdin"
        args.once = True
    elif args.input:
        feed = JsonFeed(args.input, st)
        feed.initial_scan()
        source = args.input
        stall_after = max(10.0, 5 * args.refresh)
    else:
        ap.error("need FEED (the brain's --status-file), --input -, or --demo")

    if args.once or not sys.stdout.isatty():
        import shutil
        w, h = shutil.get_terminal_size((100, 44))
        for txt, attr in render(st, w, max(24, h), title=args.title,
                                source=source, stall_after=stall_after):
            tag = {"err": "!!", "warn": "**", "head": ">>",
                   "ok": "++", "dim": "  "}.get(attr, "  ")
            print(f"{tag} {txt}")
        return 0

    run_curses(feed, st, args.refresh, args.title, source, stall_after)
    return 0


if __name__ == "__main__":
    sys.exit(main())
