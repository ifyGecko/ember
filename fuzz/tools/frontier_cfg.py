#!/usr/bin/env python3
"""frontier_cfg.py -- the control-flow graph fuzz_frontier.py works on, and where it
comes from. Imported, not run.

fuzz_frontier.py needs one thing from a disassembler: the firmware's basic blocks, with
each block's successors and its last few instructions. Nothing in it is tied to one tool.
This module holds that graph (Cfg) and the ways to get one:

  a CFG file    JSON, the format below. Anything can write it: fuzz_frontier.py --save-cfg,
                fuzz/tools/ghidra/ExportCfg.java run on the program open in any Ghidra
                (GUI or headless), or a script of your own for another disassembler.
  rizin         the `rizin` program, driven over its own pipe (no rzpipe module).
  Ghidra        headless Ghidra (analyzeHeadless) running ExportCfg.java, either on a
                program of an existing project (read-only) or on a fresh import.

An image is described once, the same way for every source (ImageSpec): the file, whether
it is raw, and for a raw image its load address and processor. Raw options are spelt as
ember's other tools spell them (--raw --base --arch --bits --endian --cpu); Ghidra's
language id comes from them, or from --processor when the default is not the right one.
Anything a tool takes beyond that passes straight through (--rizin-arg, --rizin-cmd,
--ghidra-arg), so no loader option is out of reach.

The CFG file (version 1), all addresses the pcs the code runs at:

  {"format": "ember-cfg", "version": 1,
   "source": "free text: what made it",
   "functions": [{"name": "parse", "addr": 4660, "size": 120}, ...],
   "blocks": [{"addr": 4660, "size": 12, "func": "parse",
               "jump": 4700,            # the taken / only successor, or null
               "fail": 4672,            # the fall-through of a conditional branch, or null
               "cases": [[0, 4720], ["default", 4760]],   # a switch's (value, target)s
               "tail": [[4668, "cmp r0, #0x46"], [4670, "beq 0x125c"]]}, ...],
   "untyped": [[addr, "disasm"], ...]}  # optional: branches the analyser did not classify

A block that only flows on (falls into the next block, or jumps without a condition) has
`jump` set and `fail` null. A conditional branch has both. A switch has `cases`.
"""
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
GHIDRA_SCRIPTS = os.path.join(HERE, "ghidra")
FORMAT, VERSION = "ember-cfg", 1


class Unusable(Exception):
    """This source cannot give a CFG for this image (tool missing, no disassembler for the
    machine, an option it needs was not given). The message says why; the caller may try
    another source."""


class Block:
    __slots__ = ("addr", "size", "jump", "fail", "cases", "func", "tail")

    def __init__(self, addr, size, jump, fail, cases, func, tail):
        self.addr, self.size, self.jump, self.fail = addr, size, jump, fail
        self.cases = cases             # [(value, target)] of a switch's jump table
        self.func, self.tail = func, tail

    @property
    def end(self):
        return self.addr + self.size


class Cfg:
    def __init__(self, blocks=None, functions=None, untyped=None, source=""):
        self.blocks = blocks or []
        self.functions = functions or []     # [(name, addr, size)]
        self.untyped = untyped or []         # [(addr, disasm)]
        self.source = source

    def merge(self, other):
        self.blocks += other.blocks
        self.functions += other.functions
        self.untyped += other.untyped
        self.source = ", ".join(s for s in (self.source, other.source) if s)

    def slide(self, delta):
        """Move every address by delta: the code runs at analysis address + delta."""
        if not delta:
            return
        for b in self.blocks:
            b.addr += delta
            b.jump = None if b.jump is None else b.jump + delta
            b.fail = None if b.fail is None else b.fail + delta
            b.cases = [(v, t + delta) for v, t in b.cases]
            b.tail = [(a + delta, d) for a, d in b.tail]
        self.functions = [(n, a + delta, s) for n, a, s in self.functions]
        self.untyped = [(a + delta, d) for a, d in self.untyped]

    def restrict(self, ranges):
        """Keep only the blocks that start inside ranges ([(lo, hi)]; empty: all)."""
        if ranges:
            self.blocks = [b for b in self.blocks
                           if any(lo <= b.addr < hi for lo, hi in ranges)]
            self.untyped = [(a, d) for a, d in self.untyped
                            if any(lo <= a < hi for lo, hi in ranges)]

    def to_json(self):
        return {"format": FORMAT, "version": VERSION, "source": self.source,
                "functions": [{"name": n, "addr": a, "size": s}
                              for n, a, s in self.functions],
                "blocks": [{"addr": b.addr, "size": b.size, "func": b.func,
                            "jump": b.jump, "fail": b.fail,
                            "cases": [[v, t] for v, t in b.cases],
                            "tail": [[a, d] for a, d in b.tail]} for b in self.blocks],
                "untyped": [[a, d] for a, d in self.untyped]}


def _num(v, what, path):
    if v is None:
        return None
    if isinstance(v, str):
        try:
            return int(v, 0)
        except ValueError:
            pass
    elif isinstance(v, int) and not isinstance(v, bool):
        return v
    raise Unusable("%s: %s is not an address: %r" % (path, what, v))


def load_file(path):
    """Read a CFG file. Addresses may be numbers or "0x..." strings."""
    try:
        with open(path, encoding="utf-8") as f:
            d = json.load(f)
    except (OSError, ValueError) as e:
        raise Unusable("%s: cannot read the CFG file: %s" % (path, e))
    if not isinstance(d, dict) or d.get("format") != FORMAT:
        raise Unusable('%s: not a CFG file (no "format": "%s")' % (path, FORMAT))
    if d.get("version") != VERSION:
        raise Unusable("%s: CFG file version %r; this tool reads version %d"
                       % (path, d.get("version"), VERSION))
    cfg = Cfg(source=d.get("source") or os.path.basename(path))
    for f in d.get("functions") or []:
        cfg.functions.append((str(f.get("name", "")), _num(f.get("addr"), "a function", path),
                              int(f.get("size") or 0)))
    for b in d.get("blocks") or []:
        addr = _num(b.get("addr"), "a block", path)
        size = int(b.get("size") or 0)
        if addr is None or size <= 0:
            continue
        cases = [(c[0], _num(c[1], "a case target", path)) for c in b.get("cases") or []]
        tail = [(_num(t[0], "an instruction", path), str(t[1])) for t in b.get("tail") or []]
        cfg.blocks.append(Block(addr, size, _num(b.get("jump"), "jump", path),
                                _num(b.get("fail"), "fail", path), cases,
                                str(b.get("func") or ""), tail))
    for u in d.get("untyped") or []:
        cfg.untyped.append((_num(u[0], "an instruction", path), str(u[1])))
    return cfg


def save_file(cfg, path):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(cfg.to_json(), f)
    os.replace(tmp, path)


# ---- the image ------------------------------------------------------------------------

class ImageSpec:
    """What to analyse and how it is loaded. For an ELF (or any format the analyser's own
    loader reads) only `path` matters. For a raw image: `base` is the address its first
    byte runs at; arch/bits/endian/cpu name the processor as rizin does, `processor` (and
    `cspec`) as Ghidra does. `entries` are addresses known to be code (analysis seeds: a
    raw image has no entry point of its own); `symbols` is {addr: name}."""

    def __init__(self, path, raw=False, base=None, arch=None, bits=None, endian=None,
                 cpu=None, processor=None, cspec=None, entries=(), symbols=None):
        self.path, self.raw, self.base = path, raw, base
        self.arch, self.bits, self.endian, self.cpu = arch, bits, endian, cpu
        self.processor, self.cspec = processor, cspec
        self.entries = list(entries)
        self.symbols = symbols or {}

    def seeds(self):
        """Addresses to make functions before analysis: the known function entries. A data
        symbol's address is not seeded (it only names, via `symbols`)."""
        return sorted(set(self.entries))


# rizin arch name (lower case) -> Ghidra processor, and the variant used when --cpu and
# --processor say nothing. --processor overrides the lot.
_GHIDRA_PROC = {
    "mips": ("MIPS", "default"), "arm": ("ARM", "v7"), "riscv": ("RISCV", "default"),
    "ppc": ("PowerPC", "default"), "x86": ("x86", "default"), "avr": ("avr8", "default"),
    "avr32": ("avr32", "default"), "xtensa": ("Xtensa", "default"),
    "8051": ("8051", "default"), "z80": ("z80", "default"), "6502": ("6502", "default"),
}
# Processors with one byte order and width, whatever --bits/--endian say.
_GHIDRA_FIXED = {"8051": "8051:BE:16:default", "z80": "z80:LE:16:default",
                 "6502": "6502:LE:16:default", "avr": "avr8:LE:16:default",
                 "avr32": "avr32:BE:32:default"}


def ghidra_language(spec):
    """The Ghidra language id for a raw image: --processor, else derived from the rizin
    names. Raises Unusable when neither gives one."""
    if spec.processor:
        return spec.processor
    arch = (spec.arch or "").lower()
    if arch in _GHIDRA_FIXED:
        return _GHIDRA_FIXED[arch]
    if arch == "x86" and spec.bits == 16:
        return "x86:LE:16:Real Mode"
    if arch in _GHIDRA_PROC and spec.bits and spec.endian:
        proc, variant = _GHIDRA_PROC[arch]
        return "%s:%s:%d:%s" % (proc, "BE" if spec.endian == "big" else "LE", spec.bits,
                                variant)
    raise Unusable("Ghidra needs the raw image's language: give --processor LANGID (e.g. "
                   "ARM:LE:32:Cortex), or --arch/--bits/--endian for one this tool can "
                   "name (%s)" % ", ".join(sorted(_GHIDRA_PROC)))


# ---- ELF symbols, read here (no readelf) -----------------------------------------------

def elf_symbols(path):
    """({name: (addr, size)}, e_machine) of an ELF's symbol table; ({}, None) if the file
    is not an ELF or cannot be read. On ARM and MIPS a function symbol's state bit (Thumb,
    MIPS16e/microMIPS) is cleared, so the address is the function's first instruction."""
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return {}, None
    if len(data) < 52 or data[:4] != b"\x7fELF":
        return {}, None
    cls, enc = struct.unpack_from("BB", data, 4)
    e = "<" if enc == 1 else ">"
    machine, = struct.unpack_from(e + "H", data, 18)
    if cls == 1:
        shoff, = struct.unpack_from(e + "I", data, 32)
        shentsize, shnum = struct.unpack_from(e + "HH", data, 46)
        sh_fmt, sym_size = e + "IIIIIIIIII", 16
    elif cls == 2 and len(data) >= 64:
        shoff, = struct.unpack_from(e + "Q", data, 40)
        shentsize, shnum = struct.unpack_from(e + "HH", data, 58)
        sh_fmt, sym_size = e + "IIQQQQIIQQ", 24
    else:
        return {}, None
    secs = []
    for i in range(shnum):
        at = shoff + i * shentsize
        if at + struct.calcsize(sh_fmt) > len(data):
            break
        _n, typ, _f, _a, off, size, link = struct.unpack_from(sh_fmt, data, at)[:7]
        secs.append((typ, off, size, link))
    out = {}
    tabs = [s for s in secs if s[0] == 2] or [s for s in secs if s[0] == 11]
    for _t, off, size, link in tabs:
        if not link < len(secs):
            continue
        stroff, strsize = secs[link][1], secs[link][2]
        for at in range(off, min(off + size, len(data)) - sym_size + 1, sym_size):
            if cls == 1:
                n, value, ssize, info, _o, shndx = struct.unpack_from(e + "IIIBBH", data, at)
            else:
                n, info, _o, shndx, value, ssize = struct.unpack_from(e + "IBBHQQ", data, at)
            typ = info & 0xf
            if not n or n >= strsize or shndx == 0 or typ in (3, 4):   # SECTION, FILE
                continue
            end = data.find(b"\0", stroff + n, stroff + strsize)
            name = data[stroff + n:end if end >= 0 else stroff + strsize].decode(
                "utf-8", "replace")
            if typ in (2, 10) and machine in (8, 40):
                value &= ~1
            if name and not name.startswith("$") and name not in out:
                out[name] = (value, ssize)
    return out, machine


# ---- rizin ----------------------------------------------------------------------------

class RizinPipe:
    """A rizin session over its -0 pipe protocol (the one rzpipe speaks): each command's
    output ends in a NUL byte."""

    def __init__(self, exe, path, flags):
        try:
            self.p = subprocess.Popen([exe, "-q0", "-2"] + list(flags) + [path],
                                      stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                      stderr=subprocess.DEVNULL)
        except OSError as e:
            raise Unusable("cannot run %s: %s" % (exe, e))
        self._read()

    def _read(self):
        buf = b""
        while True:
            c = os.read(self.p.stdout.fileno(), 65536)
            if not c:
                raise Unusable("rizin stopped (it could not open the image, or one of "
                               "--rizin-arg/--rizin-cmd made it exit)")
            buf += c
            if buf.endswith(b"\0"):
                return buf[:-1].decode("utf-8", "replace")

    def cmd(self, c):
        self.p.stdin.write(c.encode() + b"\n")
        self.p.stdin.flush()
        return self._read()

    def cmdj(self, c):
        out = self.cmd(c).strip()
        try:
            return json.loads(out) if out else None
        except ValueError:
            return None

    def quit(self):
        try:
            self.p.stdin.write(b"q!\n")
            self.p.stdin.close()
            self.p.wait(timeout=5)
        except Exception:
            self.p.kill()


def find_rizin():
    return shutil.which(os.environ.get("RIZIN", "rizin"))


def _arm_be8(path):
    try:
        with open(path, "rb") as f:
            h = f.read(52)
    except OSError:
        return False
    if len(h) < 52 or h[:4] != b"\x7fELF" or h[5] != 2:
        return False
    machine, = struct.unpack_from(">H", h, 18)
    flags, = struct.unpack_from(">I", h, 36)
    return machine == 40 and bool(flags & 0x00800000)


def rizin_cfg(spec, ranges=(), all_functions=False, map_base=None, extra_args=(),
              extra_cmds=()):
    """The CFG of one image from rizin. `ranges` ([(lo, hi)]; empty: everything) limits
    which functions' blocks are read. `map_base` rebases an ELF (a module loaded away from
    its link address). extra_args go on rizin's command line, extra_cmds run before the
    analysis (e.g. "e asm.cpu=cortex", "ahb 16 @ 0x8000")."""
    exe = find_rizin()
    if not exe:
        raise Unusable("rizin is not on PATH")
    if not os.path.isfile(spec.path):
        raise Unusable("%s: no such file" % spec.path)
    flags = []
    if spec.raw:
        missing = [n for n, v in (("--base", spec.base), ("--arch", spec.arch),
                                  ("--bits", spec.bits), ("--endian", spec.endian))
                   if v is None]
        if missing:
            raise Unusable("rizin needs %s for a raw image" % ", ".join(missing))
        flags = ["-m", "0x%x" % spec.base, "-a", spec.arch, "-b", str(spec.bits),
                 "-E", spec.endian]
        if spec.cpu:
            flags += ["-e", "asm.cpu=" + spec.cpu]
    elif _arm_be8(spec.path):
        raise Unusable("%s: an ARM BE8 ELF, whose code rizin decodes in the wrong byte "
                       "order" % spec.path)
    rz = RizinPipe(exe, spec.path, flags + list(extra_args))
    try:
        rz.cmd("e scr.color=0")
        if spec.raw:
            if spec.arch not in rz.cmd("e asm.arch").strip():
                raise Unusable("this rizin has no disassembler for %r (`rz-asm -L` lists "
                               "those it has)" % spec.arch)
        else:
            # A rizin built without the arch's disassembler plugin loads the ELF but keeps
            # another asm.arch, and decodes garbage.
            bin_arch = ((rz.cmdj("ij") or {}).get("bin") or {}).get("arch")
            if not bin_arch:
                raise Unusable("%s: rizin's loader does not know this format (a raw "
                               "image needs --raw --base --arch --bits --endian)"
                               % spec.path)
            if rz.cmd("e asm.arch").strip() != bin_arch:
                raise Unusable("%s: this rizin has no disassembler for %r (`rz-asm -L` "
                               "lists those it has)" % (spec.path, bin_arch))
            if map_base:
                rz.cmd("omba 0x%x" % map_base)
        for c in extra_cmds:
            rz.cmd(c)
        rz.cmd("aaa")
        for a in spec.seeds():
            rz.cmd("af @ 0x%x" % a)               # a no-op where one exists
        funcs = rz.cmdj("aflj") or []
        cfg = Cfg(source="rizin: " + os.path.basename(spec.path))
        for fn in funcs:
            off = fn.get("offset", 0)
            name = spec.symbols.get(off) or fn.get("name", "fcn")
            cfg.functions.append((name, off, fn.get("size", 0)))
            if not all_functions and off not in spec.symbols \
                    and fn.get("name", "").startswith("fcn."):
                continue                          # analysis-only function: opt in
            if ranges and not any(lo < off + fn.get("size", 0) and off < hi
                                  for lo, hi in ranges):
                continue
            for b in rz.cmdj("afbj @ 0x%x" % off) or []:
                addr, size = b.get("addr"), b.get("size", 0)
                if addr is None or not size:
                    continue
                if ranges and not any(lo <= addr < hi for lo, hi in ranges):
                    continue
                sw = b.get("switch_op") or {}
                cases = [(c.get("value"), c["jump"]) for c in sw.get("cases") or []
                         if c.get("jump") is not None]
                ops = rz.cmdj("pdbj @ 0x%x" % addr) or []
                cfg.untyped.extend((o.get("offset"), o.get("disasm", "")) for o in ops
                                   if o.get("type") == "null")
                tail = [(o.get("offset"), o.get("disasm", "")) for o in ops[-3:]]
                cfg.blocks.append(Block(addr, size, b.get("jump"), b.get("fail"), cases,
                                        name, tail))
        return cfg
    finally:
        rz.quit()


# ---- Ghidra ---------------------------------------------------------------------------

def find_ghidra(home=None):
    """The analyzeHeadless to run: --ghidra-home, $GHIDRA_HEADLESS (the program itself),
    $GHIDRA_HOME or $GHIDRA_INSTALL_DIR, analyzeHeadless on PATH, else the newest of
    ~/ghidra* and /opt/ghidra*. None if there is none."""
    import glob
    cands = []
    if home:
        cands.append(os.path.join(home, "support", "analyzeHeadless"))
    else:
        if os.environ.get("GHIDRA_HEADLESS") is not None:
            cands.append(os.environ["GHIDRA_HEADLESS"])
        for v in ("GHIDRA_HOME", "GHIDRA_INSTALL_DIR"):
            if os.environ.get(v):
                cands.append(os.path.join(os.environ[v], "support", "analyzeHeadless"))
        w = shutil.which("analyzeHeadless")
        if w:
            cands.append(w)
        for pat in ("~/ghidra*/support/analyzeHeadless", "/opt/ghidra*/support/analyzeHeadless"):
            cands += sorted(glob.glob(os.path.expanduser(pat)), reverse=True)
    for c in cands:
        if c and os.path.isfile(c) and os.access(c, os.X_OK):
            return c
    return None


def _headless(exe, argv, outdir, log):
    """Run analyzeHeadless; return the CFG files ExportCfg.java wrote into outdir."""
    with open(log, "w") as lf:
        try:
            rc = subprocess.run([exe] + argv, stdout=lf, stderr=subprocess.STDOUT,
                                stdin=subprocess.DEVNULL).returncode
        except OSError as e:
            raise Unusable("cannot run %s: %s" % (exe, e))
    return rc, sorted(os.path.join(outdir, n) for n in os.listdir(outdir)
                      if n.endswith(".json"))


def _log_tail(log):
    try:
        lines = open(log, errors="replace").read().splitlines()
    except OSError:
        return ""
    errs = [l for l in lines if l.startswith("ERROR") or "ExportCfg:" in l]
    keep = errs or [l for l in lines if l.strip() and not l.startswith("INFO ")]
    return "\n".join("    " + l for l in keep[-15:])


def _export_args(outdir, ranges, seeds):
    args = ["-scriptPath", GHIDRA_SCRIPTS, "-postScript", "ExportCfg.java", outdir]
    for lo, hi in ranges:
        args += ["--range", "0x%x-0x%x" % (lo, hi)]
    for a in seeds:
        args += ["--seed", "0x%x" % a]
    return args


def _collect(files, rc, log, what):
    if not files:
        raise Unusable("headless Ghidra wrote no CFG for %s (exit %d); its log says:\n%s"
                       % (what, rc, _log_tail(log)))
    if len(files) > 1:
        raise Unusable("%s holds several programs (%s): name one with --ghidra-program"
                       % (what, ", ".join(os.path.basename(f)[:-5] for f in files)))
    return load_file(files[0])


def ghidra_cfg(spec, ranges=(), home=None, extra_args=(), map_base=None):
    """The CFG of one image from a fresh headless import: Ghidra's own loader for an ELF
    (or any format it knows), its BinaryLoader at spec.base for a raw image. extra_args go
    on analyzeHeadless's command line after the import options (-loader-fileOffset,
    -cspec, -analysisTimeoutPerFile ...)."""
    exe = find_ghidra(home)
    if not exe:
        raise Unusable("no headless Ghidra found (--ghidra-home, $GHIDRA_HOME, or "
                       "analyzeHeadless on PATH)")
    if not os.path.isfile(spec.path):
        raise Unusable("%s: no such file" % spec.path)
    imp = ["-import", os.path.abspath(spec.path)]
    if spec.raw:
        if spec.base is None:
            raise Unusable("Ghidra needs --base for a raw image")
        imp += ["-loader", "BinaryLoader", "-loader-baseAddr", "0x%x" % spec.base,
                "-processor", ghidra_language(spec)]
    else:
        if map_base:
            # An extra ELF module loaded away from its link address. Ghidra's own loader
            # keeps the ELF's link addresses; rebasing it headless is fiddly and
            # version-specific, so steer that one module to a route that rebases cleanly.
            raise Unusable("a module at a load base other than its link address "
                           "(--base NAME=ADDR); read it with --cfg-from rizin, or export "
                           "its CFG to a file and pass --cfg")
        if spec.processor:
            imp += ["-processor", spec.processor]
    if spec.cspec:
        imp += ["-cspec", spec.cspec]
    tmp = tempfile.mkdtemp(prefix="frontier-ghidra-")
    try:
        out = os.path.join(tmp, "out")
        os.mkdir(out)
        log = os.path.join(tmp, "headless.log")
        rc, files = _headless(exe, [tmp, "frontier"] + imp + list(extra_args)
                              + _export_args(out, ranges, spec.seeds()), out, log)
        cfg = _collect(files, rc, log, spec.path)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    _apply_symbols(cfg, spec.symbols)
    return cfg


def ghidra_project_cfg(project, program=None, ranges=(), home=None, extra_args=()):
    """The CFG of a program already loaded and analysed in a Ghidra project (the one
    ember-dev-recon made, or the user's own): PATH.gpr, or DIR/NAME. Read-only: nothing is
    saved to the project. A project the Ghidra GUI holds open is read from a copy."""
    exe = find_ghidra(home)
    if not exe:
        raise Unusable("no headless Ghidra found (--ghidra-home, $GHIDRA_HOME, or "
                       "analyzeHeadless on PATH)")
    p = project[:-4] if project.endswith(".gpr") else project
    pdir, pname = os.path.dirname(os.path.abspath(p)), os.path.basename(p)
    if not os.path.isfile(os.path.join(pdir, pname + ".gpr")):
        raise Unusable("no Ghidra project %s.gpr" % os.path.join(pdir, pname))
    tmp = tempfile.mkdtemp(prefix="frontier-ghidra-")
    try:
        log = os.path.join(tmp, "headless.log")

        def run(d):
            out = os.path.join(tmp, "out")
            shutil.rmtree(out, ignore_errors=True)
            os.mkdir(out)
            argv = [d, pname, "-process"] + ([program] if program else []) \
                + ["-recursive", "-readOnly", "-noanalysis"] + list(extra_args) \
                + _export_args(out, ranges, ())
            return _headless(exe, argv, out, log)

        rc, files = run(pdir)
        if not files and re.search(r"unable to lock project|LockException|project is locked",
                                   open(log, errors="replace").read(), re.I):
            copy = os.path.join(tmp, "copy")
            os.mkdir(copy)
            shutil.copy(os.path.join(pdir, pname + ".gpr"), copy)
            shutil.copytree(os.path.join(pdir, pname + ".rep"),
                            os.path.join(copy, pname + ".rep"),
                            ignore=shutil.ignore_patterns("*.lock", "*.lock~"))
            rc, files = run(copy)
        return _collect(files, rc, log, "the project %s" % pname
                        + (" (program %s)" % program if program else ""))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def _apply_symbols(cfg, symbols):
    """Name functions (and their blocks) that start at a --symbols address."""
    if not symbols:
        return
    ren = {}
    fns = []
    for n, a, s in cfg.functions:
        if a in symbols and symbols[a] != n:
            ren[n] = symbols[a]
            n = symbols[a]
        fns.append((n, a, s))
    cfg.functions = fns
    for b in cfg.blocks:
        b.func = ren.get(b.func, b.func)
