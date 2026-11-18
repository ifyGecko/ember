#!/usr/bin/env python3
"""strings_scan.py - sort a firmware image's strings into leads.

A firmware image holds thousands of strings and most are noise. This prints the ones that
answer recon questions, grouped by what each kind is good for:

  identity  version banners, build dates, copyright, compiler   -> RTOS, SDK, vendor, board
  chip      part names of known chip families                   -> which datasheet to read
  source    source file names and paths (assert/log macros)     -> the driver a function is from
  func      log lines that start with a function name           -> names for functions
  wait      timeout / not responding / not detected messages    -> waits the board must satisfy
  error     assert, panic, checksum and other failure messages  -> checks the image must pass
  device    lines naming a peripheral class (uart, phy, flash)  -> which devices are driven
  shell     usage lines, help text, prompts                     -> console and command tables
  config    key=value text, file paths, URLs, addresses         -> NVRAM/config the flash holds

A string is a lead, not a fact: find the code that uses it (Ghidra get_xrefs_to, or
ghidra-query.sh xrefs) before writing anything down. references/reading-firmware.md says
how to work each kind.

Usage:
  strings_scan.py IMAGE [--base ADDR]            the grouped leads (addresses with --base)
  strings_scan.py IMAGE --kind wait,chip --max 0   only these kinds, every line
  strings_scan.py IMAGE --grep 'uart|baud'       every string matching a regex
  strings_scan.py IMAGE --near 0x12a340          the strings stored next to an offset or
                                                 address (same source file, usually)
  strings_scan.py IMAGE --base ADDR --ptrs --grep '^help$'
                                                 data words that point at each string, with
                                                 the word that follows (name + handler tables)
  strings_scan.py IMAGE --all                    every string, unclassified ones too

Offsets are file offsets. With --base (a raw image loaded in one piece at ADDR) or for an
ELF (its PT_LOAD segments) the address is printed next to each: that is the address to
give Ghidra. ASCII and UTF-16LE (PC firmware) strings are found; a string need not be
NUL-terminated. Python 3 standard library only.
"""
import argparse
import re
import struct
import sys

KINDS = [
    ("identity", "versions, banners, build dates, compiler",
     "names the RTOS, SDK, vendor and board (RECON.md Identity). A BSP or board name leads "
     "to the SoC and its public register map."),
    ("chip", "part names of known chip families",
     "each is a device the board may need. A name alone is 'guess': a multi-board image "
     "carries drivers for chips that are not fitted. Find the ID compare or probe that "
     "selects it."),
    ("source", "source file names and paths",
     "an assert or log macro left its file name: every function that references the string "
     "is from that file (sysLib.c, ag7240.c, flash_spi.c). Name the function after it."),
    ("func", "log lines that start with a function name",
     "the prefix is the function's own name. Xref the string and rename the function."),
    ("wait", "timeouts, no response, not detected",
     "each marks a wait loop or probe. Xref it, read the condition that avoids the "
     "message, list it under Polling loops with the register and the value that ends it."),
    ("error", "assert, panic, checksum and other failures",
     "checks the firmware makes on itself and its data (image CRC, magic, NVRAM): what the "
     "emulated flash and memory must hold. The panic/assert function is worth naming first."),
    ("device", "lines naming a peripheral class",
     "which device classes the image drives, by how much text each has. Read the largest "
     "groups first; match each against a page of the MMIO table."),
    ("shell", "usage lines, help text, prompts",
     "a prompt is the 'it booted' marker for the build phase. Command names sit in a table "
     "of {name, handler}: find it with --ptrs."),
    ("config", "key=value text, file paths, URLs, addresses",
     "defaults for NVRAM/config partitions the image does not contain, and the files and "
     "devices the OS opens."),
]
KIND_NAMES = [k[0] for k in KINDS]

MONTHS = "Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec"
RE_IDENTITY = re.compile(
    r"(?i)\b(?:version|copyright|\(c\) ?(?:19|20)\d\d|built? (?:on|at|date|time)|build date|"
    r"compiled|release (?:date|\d)|u-boot|vxworks|wind river|linux version|ecos|redboot|threadx|"
    r"freertos|nucleus|zephyr|nuttx|esp-idf|sdk ver|busybox|uclibc|newlib|bootrom|bootloader|"
    r"american megatrends|insyde|phoenix|coreboot|edk ?ii|bsp)\b|GCC: |"
    r"\b(?:19|20)\d\d[-/.]\d\d[-/.]\d\d\b|\b(?:" + MONTHS + r") +\d{1,2} +(?:19|20)\d\d\b|"
    r"\b[vV]\d+\.\d+\.\d+")

# Part names of chip families that turn up in firmware. Extend freely: a miss here only
# moves the name to the "part-number shaped" list below it.
RE_CHIP = re.compile(
    r"(?i)(?<![A-Za-z0-9])(?:"
    # SoCs and microcontrollers
    r"AR[1-9]\d{3}[A-Z]?|QCA\d{4}|IPQ\d{4}|BCM\d{4,5}[A-Z]?\d?|RTL\d{4}[A-Z]*|RT[2-6]\d{3}[A-Z]?|"
    r"MT7\d{3}[A-Z]?|ADM\d{4}|TNETD?\d{4}|IXP4\d\d|MPC\d{3,4}[A-Z]?|PPC4\d\d[A-Z]*|P[12]0[12]\d|"
    r"AT91\w+|AT32UC3\w+|AT32AP\w+|STM(?:32|8)\w+|LPC\d{3,4}|i\.?MX\d\w*|OMAP\d+|AM3\d\d\w*|"
    r"ATmega\w+|ATtiny\w+|ATxmega\w+|AT90\w+|PIC(?:1[0268]|32)\w+|ESP(?:32|8266|8285)[-\w]*|"
    r"C8051F\w+|EFM8\w+|CC[12]\d{3}|nRF\d+\w*|Z8[04]\w*|W65C\w+|"
    # Ethernet MACs, PHYs, switches
    r"KS8\d{3}\w*|KSZ\d{4}\w*|LAN\d{4}\w*|DM9\d{3}\w*|DP838\d+\w*|88E\d{4}\w*|88F\d{4}|"
    r"VSC\d{4}|IP1\d\d[A-Z]+|RTL8\d{3}\w*|AR8\d{3}|"
    # flash and EEPROM
    r"W25[QXN]\w+|MX25\w+|MX29\w+|S25F\w+|S29\w+|N25Q\w+|M25P\w+|GD25\w+|EN25\w+|"
    r"SST(?:25|39)\w+|AT45\w+|AT25\w+|IS25\w+|AT24C?\d\w*|24[AL]?C\d+|"
    # RTCs, sensors, UARTs and other small parts
    r"DS1[23]\d\d\w*|PCF8\d{3}\w*|LM7\d\w*|TMP\d{3}|ADT7\d+|NS16[45]50A?|16[45]50A?|"
    r"PL0[1-9]\d|SP80[45]|"
    # PC chipsets and Super I/O
    r"i82\d\d\w*|ICH\d+|PIIX\d?|Q35|W836\w+|IT87\w+|SCH\d{4}|NCT\d{4}\w*"
    r")(?![A-Za-z0-9])")
RE_PARTLIKE = re.compile(r"\b[A-Z]{2,6}\d{3,6}[A-Z]{0,3}\d?\b")
NOT_PARTS = re.compile(
    r"^(?:IEE|WEP|RFC|ISO|UTF|SHA|MD|AES|DES|CRC|IEEE|BASE|WPA|HTTP|RSA|ECC|ERR|MSG|ID|IP|TCP|UDP|"
    r"VLAN|PORT|ETH|COM|TTY|GPIO|IRQ|INT|REG|BIT|CH|ANSI|ASCII|GMT|UTC|PKCS|MIPS|ARM|PPC|"
    r"OPT|CMD|TEST|TASK|MAX|MIN|NUM|LEN|BUF|SIZE|ADDR|DATA|CODE|TYPE|MODE|VER|REV)\d")

RE_SOURCE = re.compile(r"[\w./\\:~-]*\w{2}\.(?:c|h|cc|cpp|cxx|hpp|S|s|asm|inc)\b(?![\w.]*@)")
RE_FUNC = re.compile(
    r"^[\[<(]?(?:[a-z]+\d*[A-Z]\w*|[A-Za-z]\w*_\w+|[A-Z][a-z0-9]+[A-Z]\w*)[\]>)]?(?:\(\))?\s?:(?:\s|$)|"
    r"^[\[<]?%s[\]>]?(?:\(\))?\s?(?::|\(%d\)|,? ?line)")
RE_WAIT = re.compile(
    r"(?i)time[d ]?[- ]?out|not respond|no response|no reply|not ready|waiting for|wait for|"
    r"did ?n[o']t (?:come|complete|finish|clear|lock|start)|"
    r"fail(?:ed|ure)? to (?:init|reset|lock|detect|calibrat|probe|sync|respond|become)|"
    r"calibration fail|pll\b.*\block|not locked|not detected|not present|"
    r"no (?:ack|device|phy|carrier|link|card|flash|eeprom)\b|probe fail|link (?:is )?(?:down|up)\b|"
    r"(?:phy|device|chip|flash)\b.* not found")
RE_ERROR = re.compile(
    r"(?i)\bassert|panic|fatal|\bexception\b|\babort|\berror\b|\bfail(?:ed|ure|s)?\b|"
    r"\binvalid\b|\billegal\b|corrupt|checksum|\bcrc\b|\bmagic\b|mismatch|unsupported|"
    r"\bunknown\b|overflow|out of memory|cannot|can't|unable to")
DEVICE_GROUPS = [
    ("uart / console", r"\buart|\bserial\b|console|\btty|baud|16550|\bsio\b"),
    ("timer / clock", r"timer|\btick|clock|\bclk\b|\bpll\b|watchdog|\bwdt?\b|\bhz\b"),
    ("interrupts", r"\birq|interrupts?\b|\bintc?\b|\bisr\b|\bvector"),
    ("flash / storage", r"flash|\bnand|\bspi\b|erase|sector|\bmtd|\bsd ?card|\bmmc\b|\bata\b"),
    ("i2c / eeprom", r"\bi2c|\biic\b|smbus|eeprom|\btwi\b"),
    ("ethernet", r"ether|\beth\d?\b|\bphy\b|mdio|\bmii\b|\bmac\b|switch|vlan|\bgmac|duplex"),
    ("wireless", r"wlan|wi-?fi|wireless|radio|802\.11|\bssid|calibrat|\bart\b|bluetooth|\bble\b"),
    ("usb", r"\busb|ehci|ohci|xhci"),
    ("pci", r"\bpci(?:e|[ _-]|\b)|\bbar\d"),
    ("gpio / leds / buttons", r"gpio|\bleds?\b|button|\bkey ?pad|\bstrap"),
    ("memory", r"\bddr|sdram|\bdram|memory test|mem ?size|\bram\b|\bcache"),
    ("rtc", r"\brtc\b|real.?time clock"),
    ("nvram / config", r"nvram|\benv\b|bootline|bootargs|factory|\bconfig"),
    ("reset / power", r"reset|reboot|power ?down|\bsleep\b|suspend"),
]
DEVICE_RES = [(n, re.compile("(?i)" + r)) for n, r in DEVICE_GROUPS]
RE_SHELL = re.compile(
    r"(?i)\busage\b|\bsyntax\b|\bhelp\b|^\s*-{1,2}\w[\w-]*[ ,=<\[]|"
    r"^(?:login|password|username|user name)\s*: ?$|"
    r"^[A-Za-z][\w()\[\]@ .:/~-]{0,22}(?:->|[>#]|\$ ) ?$|press (?:any|a|the) key|hit (?:any|a) key|"
    r"to (?:stop|abort|interrupt) (?:auto)?boot")
RE_CONFIG = re.compile(
    r"^[A-Za-z][\w.]{1,30}=\S*$|^/(?:dev|etc|proc|sys|tmp|var|bin|sbin|usr|mnt|lib|tffs\d?|flash|ata\d?|"
    r"ram\d?|tyCo|pty)\b|\b[a-z][a-z0-9+.-]{1,8}://|\b\d{1,3}(?:\.\d{1,3}){3}\b|"
    r"\b(?:[0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}\b|(?:%02[xX][:-]){5}%02[xX]|\.(?:cfg|conf|ini|xml|html?|js|css|bin|img)\b")

RE_ASCII = re.compile(rb"[\x20-\x7e\t\r\n]{%d,}")
RE_WIDE = re.compile(rb"(?:[\x20-\x7e\t\r\n]\x00){%d,}")
RE_WORDY = re.compile(r"[A-Za-z]{3}")


def find_strings(data, min_len, wide):
    """[(offset, text, is_wide)] sorted by offset. Text is stripped of line ends."""
    out = []
    for m in re.finditer(RE_ASCII.pattern % min_len, data):
        out.append((m.start(), m.group().decode("ascii"), False))
    if wide:
        for m in re.finditer(RE_WIDE.pattern % min_len, data):
            out.append((m.start(), m.group().decode("utf-16-le"), True))
    res = []
    for off, s, w in sorted(out):
        t = s.strip("\r\n\t ")
        if len(t) < min_len or not RE_WORDY.search(t):
            continue
        good = sum(c.isalnum() or c in " _-.:/%" for c in t)
        if good * 10 < len(t) * 7:
            continue
        res.append((off, t, w))
    return res


def chip_name(m):
    return m.group().upper().split("_")[0]


def classify(s):
    kinds = set()
    if s.startswith(("Assertion failed", "assertion failed", "ASSERT")):
        # the text is the asserted expression: only its file name and the failure count
        return {"source", "error"} if RE_SOURCE.search(s) else {"error"}
    if RE_IDENTITY.search(s):
        kinds.add("identity")
    if RE_CHIP.search(s):
        kinds.add("chip")
    if RE_SOURCE.search(s):
        kinds.add("source")
    if RE_FUNC.search(s):
        kinds.add("func")
    if RE_WAIT.search(s):
        kinds.add("wait")
    elif RE_ERROR.search(s):
        kinds.add("error")
    if RE_SHELL.search(s):
        kinds.add("shell")
    if RE_CONFIG.search(s):
        kinds.add("config")
    for _, r in DEVICE_RES:
        if r.search(s):
            kinds.add("device")
            break
    return kinds


class Image:
    """File offset <-> address, for a raw image at one base or an ELF's PT_LOAD segments."""

    def __init__(self, data, base):
        self.data = data
        self.segs = []          # (file offset, size, address)
        self.word, self.endian, self.how = 4, None, "file offsets only (no --base)"
        if base is not None:
            self.segs = [(0, len(data), base)]
            self.how = "base 0x%x" % base
        elif data[:4] == b"\x7fELF" and len(data) > 0x34:
            self._elf()

    def _elf(self):
        d = self.data
        is64 = d[4] == 2
        e = "<" if d[5] == 1 else ">"
        self.word, self.endian = (8 if is64 else 4), ("le" if d[5] == 1 else "be")
        try:
            if is64:
                phoff, = struct.unpack_from(e + "Q", d, 0x20)
                phentsize, phnum = struct.unpack_from(e + "HH", d, 0x36)
            else:
                phoff, = struct.unpack_from(e + "I", d, 0x1c)
                phentsize, phnum = struct.unpack_from(e + "HH", d, 0x2a)
            for i in range(phnum):
                p = phoff + i * phentsize
                if is64:
                    ptype, _, off, vaddr, _, filesz = struct.unpack_from(e + "IIQQQQ", d, p)
                else:
                    ptype, off, vaddr, _, filesz = struct.unpack_from(e + "IIIII", d, p)
                if ptype == 1 and filesz:
                    self.segs.append((off, filesz, vaddr))
        except struct.error:
            self.segs = []
        if self.segs:
            self.how = "ELF PT_LOAD segments (virtual addresses)"

    def addr(self, off):
        for o, n, a in self.segs:
            if o <= off < o + n:
                return a + (off - o)
        return None

    def offset(self, addr):
        for o, n, a in self.segs:
            if a <= addr < a + n:
                return o + (addr - a)
        return None

    def contains(self, addr):
        return self.offset(addr) is not None


def show(s, limit=110):
    t = s.replace("\\", "\\\\").replace("\r", "\\r").replace("\n", "\\n").replace("\t", "\\t")
    return '"%s"' % (t if len(t) <= limit else t[:limit] + "...")


def line(img, off, s, wide, count=1):
    a = img.addr(off)
    return "  0x%08x  %s  %s%s%s" % (off, "0x%08x" % a if a is not None else "-" * 10,
                                     "u" if wide else " ", show(s),
                                     "  (x%d)" % count if count > 1 else "")


def find_pointers(img, targets, endian):
    """{address: [(file offset of the word, value of the next word)]} for aligned data words."""
    w = img.word
    fmt = {2: "H", 4: "I", 8: "Q"}[w]
    best = {}
    for e in (["be", "le"] if endian == "auto" else [endian]):
        hits = {}
        vals = [v[0] for v in struct.iter_unpack((">" if e == "be" else "<") + fmt,
                                                 img.data[:len(img.data) // w * w])]
        for i, v in enumerate(vals):
            if v in targets:
                hits.setdefault(v, []).append((i * w, vals[i + 1] if i + 1 < len(vals) else None))
        if sum(len(h) for h in hits.values()) > sum(len(h) for h in best.get("hits", {}).values()) \
                or "hits" not in best:
            best = {"hits": hits, "endian": e}
    return best["hits"], best["endian"]


def main():
    ap = argparse.ArgumentParser(description="sort a firmware image's strings into leads",
                                 epilog="kinds: " + ", ".join(KIND_NAMES))
    ap.add_argument("image")
    ap.add_argument("--base", type=lambda x: int(x, 0),
                    help="load address of a raw image: print addresses next to the offsets")
    ap.add_argument("--min", type=int, default=4, dest="min_len", help="shortest string (4)")
    ap.add_argument("--max", type=int, dest="max_per",
                    help="lines per kind (25; 0 = all)")
    ap.add_argument("--kind", help="only these kinds (comma list)")
    ap.add_argument("--grep", help="only strings matching this regex (case-insensitive)")
    ap.add_argument("--near", type=lambda x: int(x, 0),
                    help="the strings stored around this file offset (or address, with --base)")
    ap.add_argument("--window", type=lambda x: int(x, 0), default=0x300,
                    help="bytes either side for --near (0x300)")
    ap.add_argument("--ptrs", action="store_true",
                    help="list data words that hold each printed string's address (needs "
                         "--base or an ELF)")
    ap.add_argument("--endian", choices=["be", "le", "auto"], default="auto",
                    help="byte order for --ptrs (auto: the one with more hits)")
    ap.add_argument("--word", type=int, choices=[2, 4, 8], help="pointer size for --ptrs (4)")
    ap.add_argument("--all", action="store_true", help="print every string, in file order")
    ap.add_argument("--no-wide", action="store_true", help="skip UTF-16LE strings")
    a = ap.parse_args()

    try:
        data = open(a.image, "rb").read()
    except OSError as e:
        sys.exit("strings_scan: %s" % e)
    img = Image(data, a.base)
    if a.word:
        img.word = a.word
    if a.ptrs and not img.segs:
        sys.exit("strings_scan: --ptrs needs --base (or an ELF) to know the addresses")
    endian = a.endian if a.endian != "auto" or not img.endian else img.endian
    listing = a.all or a.near is not None or (a.grep and not a.kind)
    if a.max_per is None:
        a.max_per = 0 if listing else 25
    strs = find_strings(data, a.min_len, not a.no_wide)
    print("strings_scan: %s, %d bytes, %d strings of %d or more characters (%d UTF-16), %s"
          % (a.image, len(data), len(strs), a.min_len, sum(1 for s in strs if s[2]), img.how))
    if not strs:
        print("no strings: compressed or encrypted (decompress the member first), or a "
              "microcontroller image without text (references/reading-firmware.md, "
              "'When there are no strings').")
        return
    if a.grep:
        try:
            g = re.compile(a.grep, re.I)
        except re.error as e:
            sys.exit("strings_scan: --grep: %s" % e)
        strs = [s for s in strs if g.search(s[1])]

    picked = []                 # what --ptrs works on

    def emit(items, limit):
        seen, n = {}, 0
        for off, s, w in items:
            if s in seen:
                seen[s][3] += 1
                continue
            seen[s] = [off, s, w, 1]
        rows = list(seen.values())
        for off, s, w, c in rows if not limit else rows[:limit]:
            print(line(img, off, s, w, c))
            picked.append(off)
            n += 1
        if limit and len(rows) > limit:
            print("  ... %d more (--max 0 for all)" % (len(rows) - limit))

    if a.near is not None:
        off = img.offset(a.near)        # an address, when it is one
        if off is None:
            off = a.near
        print("\n== strings within 0x%x bytes of file offset 0x%x ==" % (a.window, off))
        emit([s for s in strs if abs(s[0] - off) <= a.window], a.max_per)
    elif listing:
        print("\n== %s, in file order ==" % ("matching strings" if a.grep else "every string"))
        emit(strs, a.max_per)
    else:
        want = KIND_NAMES
        if a.kind:
            want = [k.strip() for k in a.kind.split(",")]
            bad = [k for k in want if k not in KIND_NAMES]
            if bad:
                sys.exit("strings_scan: unknown kind %s (kinds: %s)" % (bad[0], ", ".join(KIND_NAMES)))
        by = {k: [] for k in KIND_NAMES}
        for item in strs:
            for k in classify(item[1]):
                by[k].append(item)
        print("leads: " + ", ".join("%s %d" % (k, len(by[k])) for k in KIND_NAMES))
        if len(strs) < 20:
            print("very little text: a compressed or encrypted member (unpack it first), or "
                  "an image that prints nothing (references/reading-firmware.md, 'When there "
                  "are no strings').")
        print("columns: file offset, address (give this to Ghidra), string. u = UTF-16. "
              "File order is roughly source-file order.")
        for k, title, use in KINDS:
            if k not in want:
                continue
            print("\n== %s: %s (%d) ==" % (k, title, len(by[k])))
            print("   use: " + use)
            other = {}
            if k == "chip":
                for off, s, w in strs:
                    for m in RE_PARTLIKE.finditer(s):
                        t = m.group()
                        if not RE_CHIP.search(t) and not NOT_PARTS.match(t):
                            other[t] = other.get(t, 0) + 1
            if not by[k]:
                print("  (none)")
            elif k == "chip":
                names = {}
                for off, s, w in by[k]:
                    for m in RE_CHIP.finditer(s):
                        names.setdefault(chip_name(m), []).append((off, s, w))
                rows = sorted(names.items(), key=lambda kv: (-len(kv[1]), kv[0]))
                lim = a.max_per or len(rows)
                for name, items in rows[:lim]:
                    off, s, w = items[0]
                    ad = img.addr(off)
                    print("  %-14s x%-3d first 0x%08x  %s  %s" % (
                        name, len(items), off, "0x%08x" % ad if ad is not None else "-" * 10, show(s, 70)))
                    picked.append(off)
                if len(rows) > lim:
                    print("  ... %d more names (--max 0 for all)" % (len(rows) - lim))
            elif k == "source":
                files = {}
                for off, s, w in by[k]:
                    for m in RE_SOURCE.finditer(s):
                        path = m.group()
                        f = files.setdefault(re.split(r"[/\\]", path)[-1], [0, off, path])
                        f[0] += 1
                        if len(path) > len(f[2]):
                            f[2] = path
                rows = sorted(files.items(), key=lambda kv: kv[1][1])
                lim = a.max_per * 3 or len(rows)
                print("  one line per file, in file order: file, strings naming it, first one, "
                      "longest path seen")
                for name, (n, off, path) in rows[:lim]:
                    ad = img.addr(off)
                    print("  %-22s x%-3d first 0x%08x  %s  %s" % (
                        name, n, off, "0x%08x" % ad if ad is not None else "-" * 10,
                        path if path != name else ""))
                    picked.append(off)
                if len(rows) > lim:
                    print("  ... %d more files (--max 0 for all)" % (len(rows) - lim))
            elif k == "device":
                for name, r in DEVICE_RES:
                    items = [i for i in by[k] if r.search(i[1])]
                    if not items:
                        continue
                    print("  -- %s: %d strings (--grep '%s' lists them)" % (
                        name, len(items), dict(DEVICE_GROUPS)[name]))
                    emit(items, min(a.max_per, 5) if a.max_per else 0)
            else:
                emit(by[k], a.max_per)
            if k == "chip":
                # names the known-family list does not have: often the SoC or a vendor part
                o = sorted(other.items(), key=lambda kv: (-kv[1], kv[0]))[:40]
                if o:
                    print("  other part-number shaped words (check each; many are not chips): "
                          + ", ".join("%s x%d" % kv for kv in o))

    if a.ptrs:
        if not img.segs:
            sys.exit("strings_scan: --ptrs needs --base (or an ELF) to know the addresses")
        targets = {}
        for off in picked:
            ad = img.addr(off)
            if ad is not None:
                targets[ad] = off
        hits, e = find_pointers(img, targets, endian)
        print("\n== data words holding a string's address (%d-byte %s words; code that builds "
              "the address in instructions is not found here: use Ghidra xrefs) =="
              % (img.word, e))
        if not hits:
            print("  (none: the strings are reached from code, through gp or an offset, or "
                  "the base is wrong)")
        for ad in sorted(hits):
            for woff, nxt in hits[ad][:8]:
                wa = img.addr(woff)
                tag = ""
                if nxt is not None and img.contains(nxt):
                    tag = "  next word 0x%x is inside the image: a {name, pointer} table?" % nxt
                print("  string 0x%08x <- word at offset 0x%08x%s%s" % (
                    ad, woff, " (0x%08x)" % wa if wa is not None else "", tag))
            if len(hits[ad]) > 8:
                print("  string 0x%08x: %d more words" % (ad, len(hits[ad]) - 8))


if __name__ == "__main__":
    main()
