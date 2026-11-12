#!/usr/bin/env python3
"""cov_region.py -- read ember's coverage region (UAMC).

`uam-run --cov-shm PATH` publishes the harness's coverage maps in a shared
memory region (or a file); this reads one. The layout is specified in
docs/fuzz-formats.md (constants in fuzz/formats.h).

  python3 fuzz/tools/cov_region.py header  REGION         every header field
  python3 fuzz/tools/cov_region.py gen     REGION         gen_current
  python3 fuzz/tools/cov_region.py scheme  REGION         "n=<N> model=site|pc rotate=0|1"
                                                          ("blocks": no scheme)
  python3 fuzz/tools/cov_region.py stats   REGION [WHICH] "<nonzero> <sum> <max>"
  python3 fuzz/tools/cov_region.py extract REGION OUT [WHICH]   write a map to OUT

WHICH picks the map: "bitmap" (default: the union of every published map),
"current" (the slot of gen_current), "init" (the slot of gen_init) or a
generation number. A slot is read with the region's seqlock: it is accepted
only if its slot_gen equals the generation before and after copying the map,
so reading a live region is safe (a slot that has since been overwritten is an
error, not a torn map).

The region is in the byte order of the ember that wrote it; the reader tells
it from the u16 version (1) at offset 4.
"""
import struct
import sys

MAGIC = b"UAMC"
VERSION = 1
HEADER_LEN = 64
SLOT_HDR_LEN = 24
KIND_NAMES = {1: "INIT", 2: "ITER"}

SCHEME, SCHEME_PC, SCHEME_ROTATE = 1 << 0, 1 << 1, 1 << 2
NGRAM_SHIFT, NGRAM_MASK = 8, 0xf << 8

# magic, version, ring_size, map_size, slot_stride, flags, reserved0,
# gen_current, gen_init, bitmap_offset, ring_offset, owner_pid, owner_start
HEADER_FMT = "4sHHIIIIQQQQII"
FIELDS = ("magic", "version", "ring_size", "map_size", "slot_stride", "flags",
          "reserved0", "gen_current", "gen_init", "bitmap_offset", "ring_offset",
          "owner_pid", "owner_start")


def die(msg):
    sys.exit("cov_region: " + msg)


class Region:
    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            head = f.read(HEADER_LEN)
        if len(head) < HEADER_LEN or head[:4] != MAGIC:
            die("%s: not an ember coverage region (no %r magic)" % (path, MAGIC))
        for order in ("<", ">"):
            if struct.unpack_from(order + "H", head, 4)[0] == VERSION:
                self.order = order
                break
        else:
            die("%s: unknown region version" % path)
        self.hdr = dict(zip(FIELDS, struct.unpack(self.order + HEADER_FMT, head)))
        h = self.hdr
        if h["ring_size"] < 2:
            die("%s: ring_size %d (must be >= 2)" % (path, h["ring_size"]))
        if h["map_size"] == 0 or h["map_size"] & (h["map_size"] - 1):
            die("%s: map_size %d is not a power of two" % (path, h["map_size"]))
        if h["slot_stride"] < SLOT_HDR_LEN + h["map_size"]:
            die("%s: slot_stride %d is smaller than a slot" % (path, h["slot_stride"]))

    def _read(self, off, n):
        with open(self.path, "rb") as f:
            f.seek(off)
            data = f.read(n)
        if len(data) != n:
            die("%s: truncated (%d bytes at %d)" % (self.path, n, off))
        return data

    def gen_current(self):
        return struct.unpack(self.order + "Q", self._read(24, 8))[0]

    def gen_init(self):
        return struct.unpack(self.order + "Q", self._read(32, 8))[0]

    def scheme(self):
        flags = self.hdr["flags"]
        if not flags & SCHEME:
            return "blocks"
        return "n=%d model=%s rotate=%d" % (
            (flags & NGRAM_MASK) >> NGRAM_SHIFT,
            "pc" if flags & SCHEME_PC else "site",
            1 if flags & SCHEME_ROTATE else 0)

    def bitmap(self):
        return self._read(self.hdr["bitmap_offset"], self.hdr["map_size"])

    def slot(self, gen):
        """(kind, ns, map) of generation `gen`, read under the seqlock."""
        if gen == 0:
            die("generation 0 was never published")
        h = self.hdr
        off = h["ring_offset"] + (gen % h["ring_size"]) * h["slot_stride"]
        before = struct.unpack(self.order + "Q", self._read(off, 8))[0]
        if before != gen:
            die("generation %d is not in the ring (its slot holds %d)" % (gen, before))
        raw = self._read(off, SLOT_HDR_LEN + h["map_size"])
        after = struct.unpack(self.order + "Q", self._read(off, 8))[0]
        slot_gen, ns, kind, _res = struct.unpack_from(self.order + "QQII", raw)
        if slot_gen != gen or after != gen:
            die("generation %d was overwritten while reading it" % gen)
        return kind, ns, raw[SLOT_HDR_LEN:]

    def pick(self, which):
        if which in (None, "bitmap"):
            return self.bitmap()
        if which == "current":
            gen = self.gen_current()
        elif which == "init":
            gen = self.gen_init()
            if gen == 0:
                die("no INIT map published yet")
        else:
            try:
                gen = int(which, 0)
            except ValueError:
                die("WHICH must be bitmap, current, init or a generation, got %r" % which)
        return self.slot(gen)[2]


def stats(m):
    return "%d %d %d" % (sum(1 for b in m if b), sum(m), max(m) if m else 0)


def main(argv):
    if len(argv) >= 2 and argv[1] in ("-h", "--help"):
        print(__doc__)
        return 0
    cmds = {"header": 3, "gen": 3, "scheme": 3, "stats": 3, "extract": 4}
    if len(argv) < 3 or argv[1] not in cmds or len(argv) < cmds[argv[1]]:
        sys.stderr.write(__doc__)
        return 2
    cmd, region = argv[1], Region(argv[2])
    if cmd == "gen":
        print(region.gen_current())
    elif cmd == "scheme":
        print(region.scheme())
    elif cmd == "header":
        h = dict(region.hdr)
        h["magic"] = h["magic"].decode("ascii", "replace")
        h["gen_current"], h["gen_init"] = region.gen_current(), region.gen_init()
        h["byte_order"] = "little" if region.order == "<" else "big"
        h["scheme"] = region.scheme()
        for k, v in h.items():
            print("%s=%s" % (k, "0x%x" % v if k == "flags" else v))
    elif cmd == "stats":
        print(stats(region.pick(argv[3] if len(argv) > 3 else None)))
    elif cmd == "extract":
        with open(argv[3], "wb") as f:
            f.write(region.pick(argv[4] if len(argv) > 4 else None))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
