#!/usr/bin/env python3
"""lcvs_check.py REGION WANT_FLAGS ITER_MAP... - check an LCVS region ember wrote.

Written from suzzer's docs/FORMATS.md section 3 alone (no suzzer code): the header
fields and their offsets, the ring layout, the slot flags and the seqlock stamps. The
region must hold one INIT snapshot (gen 1, gen_init 1) followed by one ITER snapshot
per ITER_MAP file, in order, each bitmap equal to that file, and a live bitmap equal
to the saturating sum of every published bitmap. WANT_FLAGS is the header flags
expected (decimal or 0x...).
"""
import struct
import sys


def fail(msg):
    sys.exit("lcvs_check: " + msg)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    data = open(sys.argv[1], "rb").read()
    want_flags = int(sys.argv[2], 0)
    iters = [open(p, "rb").read() for p in sys.argv[3:]]

    if data[:4] != b"LCVS":
        fail("no LCVS magic")
    order = "<" if struct.unpack_from("<H", data, 4)[0] == 1 else ">"
    (version, ring_size, map_size, slot_stride, flags, pad0, gen_current, gen_init,
     bitmap_off, ring_off, dropped, owner_pid, owner_start) = struct.unpack_from(
        order + "HHIIIIQQQQIII", data, 4)
    reserved = data[68:72]
    if version != 1 or pad0 != 0 or dropped != 0 or reserved != b"\0\0\0\0":
        fail("header: version %d pad0 %d dropped %d reserved %r" % (version, pad0, dropped, reserved))
    if flags != want_flags:
        fail("flags 0x%x, expected 0x%x" % (flags, want_flags))
    if map_size & (map_size - 1) or slot_stride < 32 + map_size:
        fail("map_size %d slot_stride %d" % (map_size, slot_stride))
    if bitmap_off < 72 or bitmap_off + map_size > ring_off \
       or ring_off + ring_size * slot_stride > len(data):
        fail("layout: bitmap %d ring %d x %d in %d bytes" % (bitmap_off, ring_size, slot_stride, len(data)))
    if owner_pid == 0:
        fail("owner_pid not set")
    n = 1 + len(iters)
    if gen_current != n or gen_init != 1:
        fail("gen_current %d gen_init %d, expected %d and 1" % (gen_current, gen_init, n))
    if n > ring_size:
        fail("test needs a ring of at least %d slots" % n)

    live = bytearray(map_size)
    init = None
    for g in range(1, n + 1):
        off = ring_off + (g % ring_size) * slot_stride
        slot_gen, _tid, ns, sflags = struct.unpack_from(order + "QQQQ", data, off)
        bm = data[off + 32:off + 32 + map_size]
        want_kind = 1 if g == 1 else 2
        if slot_gen != g or sflags != want_kind or ns == 0:
            fail("gen %d: slot_gen %d flags %d ns %d" % (g, slot_gen, sflags, ns))
        if g == 1:
            init = bm
        elif bm != iters[g - 2]:
            fail("gen %d: bitmap differs from %s" % (g, sys.argv[3 + g - 2]))
        for i, v in enumerate(bm):
            live[i] = min(255, live[i] + v)
    if not any(init):
        fail("the INIT snapshot is empty")
    if bytes(live) != data[bitmap_off:bitmap_off + map_size]:
        fail("the live bitmap is not the union of the published snapshots")
    print("lcvs_check: %d snapshots, %d-byte maps, %d slots" % (n, map_size, ring_size))


if __name__ == "__main__":
    main()
