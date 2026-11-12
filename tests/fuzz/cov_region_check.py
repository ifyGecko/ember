#!/usr/bin/env python3
"""cov_region_check.py REGION WANT_FLAGS ITER_MAP... - check a coverage region ember wrote.

Written from docs/fuzz-formats.md "Coverage region" alone (apart from fuzz/tools/
cov_region.py): the header fields and their offsets, the ring layout, the slot kinds and
the seqlock stamps. The region must hold one INIT snapshot (gen 1, gen_init 1) followed by
one ITER snapshot per ITER_MAP file, in order, each map equal to that file, and a live
bitmap equal to the saturating sum of every published map. WANT_FLAGS is the header flags
expected (decimal or 0x...).
"""
import struct
import sys

HDR, SLOT = 64, 24


def fail(msg):
    sys.exit("cov_region_check: " + msg)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    data = open(sys.argv[1], "rb").read()
    want_flags = int(sys.argv[2], 0)
    iters = [open(p, "rb").read() for p in sys.argv[3:]]

    if data[:4] != b"UAMC":
        fail("no UAMC magic")
    order = "<" if struct.unpack_from("<H", data, 4)[0] == 1 else ">"
    (version, ring_size, map_size, slot_stride, flags, reserved0, gen_current, gen_init,
     bitmap_off, ring_off, owner_pid, owner_start) = struct.unpack_from(
        order + "HHIIIIQQQQII", data, 4)
    if version != 1 or reserved0 != 0:
        fail("header: version %d reserved %d" % (version, reserved0))
    if flags != want_flags:
        fail("flags 0x%x, expected 0x%x" % (flags, want_flags))
    if map_size & (map_size - 1) or slot_stride < SLOT + map_size:
        fail("map_size %d slot_stride %d" % (map_size, slot_stride))
    if bitmap_off < HDR or bitmap_off + map_size > ring_off \
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
        slot_gen, ns, kind, sres = struct.unpack_from(order + "QQII", data, off)
        bm = data[off + SLOT:off + SLOT + map_size]
        want_kind = 1 if g == 1 else 2
        if slot_gen != g or kind != want_kind or ns == 0 or sres != 0:
            fail("gen %d: slot_gen %d kind %d ns %d" % (g, slot_gen, kind, ns))
        if g == 1:
            init = bm
        elif bm != iters[g - 2]:
            fail("gen %d: map differs from %s" % (g, sys.argv[3 + g - 2]))
        for i, v in enumerate(bm):
            live[i] = min(255, live[i] + v)
    if not any(init):
        fail("the INIT snapshot is empty")
    if bytes(live) != data[bitmap_off:bitmap_off + map_size]:
        fail("the live bitmap is not the union of the published snapshots")
    print("cov_region_check: %d snapshots, %d-byte maps, %d slots" % (n, map_size, ring_size))


if __name__ == "__main__":
    main()
