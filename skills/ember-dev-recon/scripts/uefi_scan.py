#!/usr/bin/env python3
"""uefi_scan.py - triage a PC firmware image (UEFI, coreboot or legacy BIOS) for an ember
x86-64 board. Self-contained (python3 stdlib; the `brotli` module only for Brotli sections).

Reports, with the physical address each part has when the BIOS region is decoded below
4 GiB (the reset vector at 0xFFFFFFF0):
  - the Intel flash descriptor: regions (descriptor, BIOS, ME, GbE, PDR, EC, ...), flash
    size, and where the BIOS region lands (window_top - BIOS size);
  - the reset vector (what the first instruction jumps to), the FIT (microcode, ACM, Boot
    Guard manifests) and every Intel microcode update with its CPU signature - the CPUID
    signatures the firmware supports, i.e. what the ember board should report;
  - firmware volumes (nested ones too, from compressed sections), their FFS files by type
    (SEC, PEI core, PEIM, DXE core, driver, SMM, application, ...) and name (UI section,
    else the PE debug path, else a known GUID), and for each PE32/TE image its machine,
    image base, entry point and - for images in flash - its execute-in-place address and
    whether the image was rebased there;
  - the UEFI variable store(s) (edk2 "VSS" format with its live variables; AMI NVAR names);
  - vendor/BIOS-ID strings ($IBIOSI$, AMI, Insyde, Phoenix, EDK II), coreboot FMAP/CBFS,
    PCI option ROMs, legacy BIOS tables ($PIR, _32_, $PnP, _SM_).

With --extract DIR it writes every module (PE32 as .efi, TE converted to a loadable PE as
.efi plus the raw .te), every decompressed volume (.fv) and raw/freeform section (.bin),
and DIR/modules.tsv: one row per image (name, guid, type, machine, image base, entry, XIP
address, absolute file path) - what to import into Ghidra (PE loader; 32-bit modules x86:LE:32:default,
64-bit x86:LE:64:default).

usage: uefi_scan.py IMAGE [--top 0x100000000] [--extract DIR] [--json] [--all-files]
                          [--no-microcode]
  --top        where the end of the BIOS region (or of the whole image, without a flash
               descriptor) is decoded; default 4 GiB. An OVMF_CODE.fd alone ends at 4 GiB,
               an OVMF_VARS.fd sits just below the code: pass --top <4 GiB - code size>.
  --all-files  list pad files and raw files too
"""
import argparse
import json
import lzma
import os
import re
import struct
import sys
import zlib

FOUR_GIB = 0x100000000

# ---- GUIDs -----------------------------------------------------------------------------

def guid_str(b):
    a, b1, c = struct.unpack_from("<IHH", b, 0)
    d = b[8:16]
    return "%08X-%04X-%04X-%02X%02X-%s" % (a, b1, c, d[0], d[1], d[2:].hex().upper())


FS_FFS2 = "8C8CE578-8A3D-4F1C-9935-896185C32DD3"
FS_FFS3 = "5473C07A-3DCB-4DCA-BD6F-1E9689E7349A"
FS_NVRAM = "FFF12B8D-7696-4C8B-A985-2747075B4F50"
FS_NAMES = {FS_FFS2: "FFS2", FS_FFS3: "FFS3", FS_NVRAM: "NVRAM (variables)",
            "04ADEEAD-61FF-4D31-B6BA-64F8BF901F5A": "Apple FS",
            "7A9354D9-0468-444A-81CE-0BF617D890DF": "FFS (Intel, legacy)",
            "CD17FF5E-336A-4B55-9C9C-2F5D24B1AD9D": "AMI NVRAM",
            "DE1E4FA1-C78E-4C7D-ACEA-F9A9F3E3F5B4": "Insyde",
            "D6A1CD70-4B33-4994-A6EA-375F2CCC5437": "Phoenix?"}
SECT_LZMA = "EE4E5898-3914-4259-9D6E-DC7BD79403CF"
SECT_LZMAF86 = "D42AE6BD-1352-4BFB-909A-CA72A6EAE889"
SECT_TIANO = "A31280AD-481E-41B6-95E8-127F4C984779"
SECT_BROTLI = "3D532050-5CDA-4FD0-879E-0F7F630D5AFB"
SECT_CRC32 = "FC1BCDB0-7D31-49AA-936A-A4600D9DD083"
SECT_SIGNED = "0F9D89E8-9259-4F76-A5AF-0C89E34023DF"
VTF_GUID = "1BA0062E-C779-4582-8566-336AE8F78F09"
VAR_GUIDS = {"DDCF3616-3275-4164-98B6-FE85707FFE7D": "variables",
             "AAF32C78-947B-439A-A180-2E144EC37792": "authenticated variables"}
KNOWN = {  # files a scan often meets without a name of their own
    VTF_GUID: "Volume Top File (reset vector)",
    "52C05B14-0B98-496C-BC3B-04B50211D680": "PeiCore",
    "D6A2CB7F-6A18-4E2F-B43B-9920A733700A": "DxeCore",
    "DF1CCEF6-F301-4A63-9661-FC6030DCC880": "SecMain",
    "1D7ACAB3-4F15-4D88-BB9D-8DB1F8F2C14A": "Microcode FV / patches",
    "197DB236-F856-4924-90F8-CDF12FB875F3": "Microcode (Intel)",
    "9E21FD93-9C72-4C15-8C4B-E77F1DB2D792": "FvMainCompact (compressed DXE volume)",
}
FILE_TYPES = {0x01: "RAW", 0x02: "FREEFORM", 0x03: "SEC", 0x04: "PEI_CORE", 0x05: "DXE_CORE",
              0x06: "PEIM", 0x07: "DRIVER", 0x08: "PEIM+DRIVER", 0x09: "APPLICATION",
              0x0A: "SMM", 0x0B: "FV_IMAGE", 0x0C: "SMM+DXE", 0x0D: "SMM_CORE",
              0x0E: "MM_STANDALONE", 0x0F: "MM_CORE_STANDALONE", 0xF0: "PAD"}
SECT_TYPES = {0x01: "COMPRESSION", 0x02: "GUID_DEFINED", 0x03: "DISPOSABLE", 0x10: "PE32",
              0x11: "PIC", 0x12: "TE", 0x13: "DXE_DEPEX", 0x14: "VERSION", 0x15: "UI",
              0x16: "COMPAT16", 0x17: "FV_IMAGE", 0x18: "FREEFORM_GUID", 0x19: "RAW",
              0x1B: "PEI_DEPEX", 0x1C: "MM_DEPEX"}
MACHINES = {0x014C: "IA32", 0x8664: "X64", 0x01C2: "ARM", 0x01C4: "ARMv7", 0xAA64: "AARCH64",
            0x0EBC: "EBC", 0x5032: "RISCV32", 0x5064: "RISCV64"}
SUBSYS = {10: "app", 11: "boot-service driver", 12: "runtime driver", 13: "SAL"}
REGIONS = ["descriptor", "BIOS", "ME", "GbE", "PDR", "DevExp1", "BIOS2", "uCode", "EC",
           "DevExp2", "IE", "10GbE0", "10GbE1", "reserved13", "reserved14", "PTT"]


def u8(b, o): return b[o]
def u16(b, o): return struct.unpack_from("<H", b, o)[0]
def u24(b, o): return b[o] | b[o + 1] << 8 | b[o + 2] << 16
def u32(b, o): return struct.unpack_from("<I", b, o)[0]
def u64(b, o): return struct.unpack_from("<Q", b, o)[0]
def align(v, a): return (v + a - 1) & ~(a - 1)


# ---- decompression ---------------------------------------------------------------------

class _Tiano:
    """EFI 1.1 (version 1, PBIT 4) and Tiano (version 2, PBIT 5) decompression, after
    edk2 MdePkg/Library/BaseUefiDecompressLib."""
    NC, CBIT, MAXNP, NT, TBIT = 510, 9, 31, 19, 5
    NPT = 31

    def __init__(self, src, version):
        if len(src) < 8:
            raise ValueError("short")
        self.comp, self.orig = u32(src, 0), u32(src, 4)
        if self.orig > 64 << 20:
            raise ValueError("size")
        self.src, self.inp = src, 8
        self.pbit = 4 if version == 1 else 5
        self.bitbuf = self.subbitbuf = self.bitcount = 0
        self.left = [0] * (2 * self.NC - 1)
        self.right = [0] * (2 * self.NC - 1)
        self.clen, self.ctable = [0] * self.NC, [0] * 4096
        self.ptlen, self.pttable = [0] * self.NPT, [0] * 256
        self.blocksize, self.bad = 0, False
        self.fillbuf(32)

    def fillbuf(self, n):
        self.bitbuf = (self.bitbuf << n) & 0xFFFFFFFF
        while n > self.bitcount:
            n -= self.bitcount
            self.bitbuf |= (self.subbitbuf << n) & 0xFFFFFFFF
            if self.comp > 0:
                self.comp -= 1
                self.subbitbuf = self.src[self.inp] if self.inp < len(self.src) else 0
                self.inp += 1
            else:
                self.subbitbuf = 0
            self.bitcount = 8
        self.bitcount -= n
        self.bitbuf |= self.subbitbuf >> self.bitcount

    def getbits(self, n):
        v = self.bitbuf >> (32 - n)
        self.fillbuf(n)
        return v

    def maketable(self, nchar, bitlen, tbits, table):
        count, weight, start = [0] * 17, [0] * 17, [0] * 18
        for i in range(nchar):
            if bitlen[i] > 16:
                return False
            count[bitlen[i]] += 1
        for i in range(1, 17):
            start[i + 1] = (start[i] + (count[i] << (16 - i))) & 0xFFFF
        if start[17] != 0:
            return False
        jub = 16 - tbits
        for i in range(1, tbits + 1):
            start[i] >>= jub
            weight[i] = 1 << (tbits - i)
        for i in range(tbits + 1, 17):
            weight[i] = 1 << (16 - i)
        i = start[tbits + 1] >> jub
        if i != 0:
            for k in range(i, 1 << tbits):
                table[k] = 0
        avail, mask = nchar, 1 << (15 - tbits)
        for ch in range(nchar):
            ln = bitlen[ch]
            if ln == 0 or ln >= 17:
                continue
            nextc = start[ln] + weight[ln]
            if ln <= tbits:
                if start[ln] >= nextc or nextc > (1 << tbits):
                    return False
                for k in range(start[ln], nextc):
                    table[k] = ch
            else:
                i3 = start[ln]
                ref = (table, i3 >> jub)
                for _ in range(ln - tbits):
                    arr, idx = ref
                    if arr[idx] == 0 and avail < 2 * self.NC - 1:
                        self.right[avail] = self.left[avail] = 0
                        arr[idx] = avail
                        avail += 1
                    if arr[idx] < 2 * self.NC - 1:
                        ref = (self.right if i3 & mask else self.left, arr[idx])
                    i3 = (i3 << 1) & 0xFFFF
                arr, idx = ref
                arr[idx] = ch
            start[ln] = nextc
        return True

    def read_ptlen(self, nn, nbit, special):
        n = self.getbits(nbit)
        if n > self.NPT or nn > self.NPT:
            return False
        if n == 0:
            c = self.getbits(nbit)
            for k in range(256):
                self.pttable[k] = c
            for k in range(nn):
                self.ptlen[k] = 0
            return True
        i = 0
        while i < n and i < self.NPT:
            c = self.bitbuf >> 29
            if c == 7:
                m = 1 << 28
                while m & self.bitbuf:
                    m >>= 1
                    c += 1
            self.fillbuf(3 if c < 7 else c - 3)
            self.ptlen[i] = c
            i += 1
            if i == special:
                c = self.getbits(2)
                while c > 0 and i < self.NPT:
                    c -= 1
                    self.ptlen[i] = 0
                    i += 1
        while i < nn and i < self.NPT:
            self.ptlen[i] = 0
            i += 1
        return self.maketable(nn, self.ptlen, 8, self.pttable)

    def read_clen(self):
        n = self.getbits(self.CBIT)
        if n == 0:
            c = self.getbits(self.CBIT)
            self.clen = [0] * self.NC
            self.ctable = [c] * 4096
            return
        i = 0
        while i < n and i < self.NC:
            c = self.pttable[self.bitbuf >> 24]
            if c >= self.NT:
                m = 1 << 23
                while c >= self.NT:
                    c = self.right[c] if self.bitbuf & m else self.left[c]
                    m >>= 1
            self.fillbuf(self.ptlen[c])
            if c <= 2:
                c = 1 if c == 0 else self.getbits(4) + 3 if c == 1 else self.getbits(self.CBIT) + 20
                while c > 0 and i < self.NC:
                    c -= 1
                    self.clen[i] = 0
                    i += 1
            else:
                self.clen[i] = c - 2
                i += 1
        for k in range(i, self.NC):
            self.clen[k] = 0
        self.maketable(self.NC, self.clen, 12, self.ctable)

    def decode_c(self):
        if self.blocksize == 0:
            self.blocksize = self.getbits(16)
            if not self.read_ptlen(self.NT, self.TBIT, 3):
                self.bad = True
                return 0
            self.read_clen()
            if not self.read_ptlen(self.MAXNP, self.pbit, -1):
                self.bad = True
                return 0
        self.blocksize -= 1
        c = self.ctable[self.bitbuf >> 20]
        if c >= self.NC:
            m = 1 << 19
            while c >= self.NC:
                c = self.right[c] if self.bitbuf & m else self.left[c]
                m >>= 1
        self.fillbuf(self.clen[c])
        return c

    def decode_p(self):
        v = self.pttable[self.bitbuf >> 24]
        if v >= self.MAXNP:
            m = 1 << 23
            while v >= self.MAXNP:
                v = self.right[v] if self.bitbuf & m else self.left[v]
                m >>= 1
        self.fillbuf(self.ptlen[v])
        return ((1 << (v - 1)) + self.getbits(v - 1)) if v > 1 else v

    def run(self):
        out = bytearray()
        while len(out) < self.orig:
            c = self.decode_c()
            if self.bad:
                raise ValueError("bad table")
            if c < 256:
                out.append(c)
            else:
                n = c - (256 - 3)
                src = len(out) - self.decode_p() - 1
                if src < 0:
                    raise ValueError("bad distance")
                for _ in range(n):
                    if len(out) >= self.orig:
                        break
                    out.append(out[src])
                    src += 1
        return bytes(out)


def tiano_decompress(data):
    """Tiano first (most images), then EFI 1.1; each checked by its stated sizes."""
    for v in (2, 1):
        try:
            out = _Tiano(data, v).run()
            if len(out) == u32(data, 4):
                return out, "Tiano" if v == 2 else "EFI"
        except (ValueError, IndexError):
            pass
    raise ValueError("Tiano/EFI decompression failed")


def lzma_decompress(data, f86=False):
    if len(data) < 13:
        raise ValueError("short LZMA stream")
    size = u64(data, 5)
    if not f86:
        return lzma.decompress(data, format=lzma.FORMAT_ALONE)
    props = data[0]
    lc, rest = props % 9, props // 9
    filters = [{"id": lzma.FILTER_X86},
               {"id": lzma.FILTER_LZMA1, "lc": lc, "lp": rest % 5, "pb": rest // 5,
                "dict_size": u32(data, 1)}]
    d = lzma.LZMADecompressor(format=lzma.FORMAT_RAW, filters=filters)
    out = d.decompress(data[13:], max_length=size)
    if len(out) < size:
        # the x86 filter holds back its last (at most 4) lookahead bytes, which BCJ never
        # converts: they are the plain LZMA output's
        out += lzma.decompress(data, format=lzma.FORMAT_ALONE)[len(out):size]
    return out[:size]


def brotli_decompress(data):
    try:
        import brotli
    except ImportError:
        raise ValueError("Brotli section: install the python brotli module")
    size = u64(data, 0)
    return brotli.decompress(data[16:])[:size]


# ---- PE / TE ---------------------------------------------------------------------------

def pe_info(b):
    """PE32/PE32+ header facts, or None."""
    if len(b) < 0x40 or b[:2] != b"MZ":
        return None
    pe = u32(b, 0x3C)
    if pe + 24 > len(b) or b[pe:pe + 4] != b"PE\0\0":
        return None
    mach, nsec = u16(b, pe + 4), u16(b, pe + 6)
    opt = pe + 24
    magic = u16(b, opt)
    is64 = magic == 0x20B
    entry = u32(b, opt + 16)
    base = u64(b, opt + 24) if is64 else u32(b, opt + 28)
    size = u32(b, opt + 56)
    subsys = u16(b, opt + 68)
    ndirs = u32(b, opt + (108 if is64 else 92))
    dirs = opt + (112 if is64 else 96)
    info = {"format": "PE32+" if is64 else "PE32", "machine": MACHINES.get(mach, hex(mach)),
            "image_base": base, "entry_rva": entry, "size": size,
            "subsystem": SUBSYS.get(subsys, str(subsys)), "pdb": None}
    if ndirs > 6 and dirs + 7 * 8 <= len(b):
        dbg_rva, dbg_size = u32(b, dirs + 6 * 8), u32(b, dirs + 6 * 8 + 4)
        info["pdb"] = _codeview(b, _pe_rva_to_off(b, pe, nsec, dbg_rva), dbg_size)
    return info


def _pe_rva_to_off(b, pe, nsec, rva):
    sec = pe + 24 + u16(b, pe + 20)
    for i in range(nsec):
        s = sec + 40 * i
        if s + 40 > len(b):
            break
        va, vs, raw = u32(b, s + 12), u32(b, s + 8), u32(b, s + 20)
        if va <= rva < va + max(vs, u32(b, s + 16)):
            return rva - va + raw
    return rva   # headers, or images whose sections are file-aligned at their RVAs


def _codeview(b, off, size, adj=0):
    """The PDB path from a debug directory (CodeView RSDS/NB10/MTOC), or None. adj: what
    to add to a file pointer in it (a TE image lost StrippedSize - 40 header bytes)."""
    for e in range(max(0, size // 28)):
        d = off + 28 * e
        if d < 0 or d + 28 > len(b):
            return None
        if u32(b, d + 12) != 2:                          # IMAGE_DEBUG_TYPE_CODEVIEW
            continue
        p = u32(b, d + 24) + adj
        sig = b[p:p + 4]
        name_off = {b"RSDS": 24, b"NB10": 16, b"MTOC": 20}.get(sig)
        if name_off is None:
            return None
        end = b.find(b"\0", p + name_off)
        return b[p + name_off:end].decode("latin-1") if end > 0 else None
    return None


def te_info(b):
    if len(b) < 40 or b[:2] != b"VZ":
        return None
    mach, nsec, subsys, stripped = u16(b, 2), b[4], b[5], u16(b, 6)
    entry, base = u32(b, 8), u64(b, 16)
    info = {"format": "TE", "machine": MACHINES.get(mach, hex(mach)), "image_base": base,
            "entry_rva": entry, "subsystem": SUBSYS.get(subsys, str(subsys)),
            "stripped": stripped, "nsec": nsec, "pdb": None}
    dbg_rva, dbg_size = u32(b, 32), u32(b, 36)
    if dbg_rva:
        info["pdb"] = _codeview(b, dbg_rva - stripped + 40, dbg_size, 40 - stripped)
    return info


def te_to_pe(b):
    """Rebuild a loadable PE32/PE32+ from a TE image (headers back in their StrippedSize
    bytes, the section table and data where the original image had them), or None when the
    stripped header room is too small for a PE header."""
    t = te_info(b)
    if not t:
        return None
    mach, nsec, subsys, stripped = u16(b, 2), b[4], b[5], u16(b, 6)
    is64 = mach in (0x8664, 0xAA64, 0x5064)
    opt_size = 240 if is64 else 224
    lfanew = stripped - 24 - opt_size
    if lfanew < 0x40 or nsec * 40 + 40 > len(b):
        return None
    hdr = bytearray(stripped)
    hdr[0:2] = b"MZ"
    struct.pack_into("<I", hdr, 0x3C, lfanew)
    hdr[lfanew:lfanew + 4] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", hdr, lfanew + 4, mach, nsec, 0, 0, 0, opt_size, 0x0102 | (0 if is64 else 0x0100))
    o = lfanew + 24
    secs = [struct.unpack_from("<8sIIIIIIHHI", b, 40 + 40 * i) for i in range(nsec)]
    size_image = align(max([s[2] + max(s[1], s[3]) for s in secs] + [stripped]), 0x20)
    code = sum(s[3] for s in secs if s[9] & 0x20)
    struct.pack_into("<HBBIII", hdr, o, 0x20B if is64 else 0x10B, 0, 0, code, 0, 0)
    struct.pack_into("<II", hdr, o + 16, u32(b, 8), u32(b, 12))       # entry, base of code
    if is64:
        struct.pack_into("<Q", hdr, o + 24, u64(b, 16))
        p = o + 32
    else:
        struct.pack_into("<II", hdr, o + 24, 0, u64(b, 16) & 0xFFFFFFFF)
        p = o + 32
    struct.pack_into("<II", hdr, p, 0x20, 0x20)                         # section/file alignment
    struct.pack_into("<IIII", hdr, p + 24, size_image, stripped + 40 * nsec, 0, subsys)
    dirs = o + (112 if is64 else 96)
    struct.pack_into("<I", hdr, dirs - 4, 16)
    struct.pack_into("<IIII", hdr, dirs + 5 * 8, u32(b, 24), u32(b, 28), u32(b, 32), u32(b, 36))
    return bytes(hdr) + b[40:]


# ---- sections, files, volumes ------------------------------------------------------------

class Scan:
    def __init__(self, data, top):
        self.data, self.top = data, top
        self.window_base = None          # physical address of image offset self.window_off
        self.window_off = 0
        self.fvs, self.modules, self.vars, self.notes = [], [], [], []
        self.extract = None
        self.n_extracted = 0
        self.all_files = False

    def phys(self, off):
        if off is None or self.window_base is None or off < self.window_off:
            return None
        return self.window_base + off - self.window_off

    def out(self, name, data):
        if not self.extract:
            return None
        self.n_extracted += 1
        safe = re.sub(r"[^A-Za-z0-9_.-]+", "_", name)[:80]
        path = os.path.join(self.extract, "%03d_%s" % (self.n_extracted, safe))
        with open(path, "wb") as f:
            f.write(data)
        return path

    # sections of one file (or of a decompressed stream)
    def sections(self, b, start, end, ctx, depth, top_off):
        o = start
        while o + 4 <= end:
            size, typ = u24(b, o), b[o + 3]
            hl = 4
            if size == 0xFFFFFF:
                if o + 8 > end:
                    break
                size, hl = u32(b, o + 4), 8
            if size < hl or o + size > end:
                break
            body = b[o + hl:o + size]
            boff = o + hl if top_off is not None else None
            yield typ, body, (top_off + boff if boff is not None else None)
            if typ == 0x01 and len(body) >= 5:                 # COMPRESSION
                ctype = body[4]
                try:
                    inner = body[5:] if ctype == 0 else tiano_decompress(body[5:])[0]
                    ctx["compression"] = "none" if ctype == 0 else "Tiano/EFI"
                    yield from self.sections(inner, 0, len(inner), ctx, depth + 1, None)
                except ValueError as e:
                    ctx["error"] = str(e)
            elif typ == 0x02 and len(body) >= 20:              # GUID_DEFINED
                g, doff = guid_str(body[:16]), u16(body, 16)
                payload = b[o + doff:o + size] if doff >= hl else body[20:]
                inner, how = None, None
                try:
                    if g == SECT_LZMA:
                        inner, how = lzma_decompress(payload), "LZMA"
                    elif g == SECT_LZMAF86:
                        inner, how = lzma_decompress(payload, f86=True), "LZMA+x86"
                    elif g == SECT_TIANO:
                        inner, how = tiano_decompress(payload)
                    elif g == SECT_BROTLI:
                        inner, how = brotli_decompress(payload), "Brotli"
                    elif g == SECT_CRC32:
                        inner, how = payload[4:] if len(payload) > 4 and doff < 24 else payload, "CRC32"
                    elif g == SECT_SIGNED:
                        inner, how = payload, "signed contents"
                    else:
                        ctx["error"] = "unknown GUID-defined section %s" % g
                except (ValueError, lzma.LZMAError, EOFError) as e:
                    ctx["error"] = "%s: %s" % (g, e)
                if inner is not None:
                    ctx["compression"] = how
                    xip = top_off is not None and how in ("CRC32", "signed contents")
                    base = (top_off + o + doff) if xip else None
                    yield from self.sections(inner, 0, len(inner), ctx, depth + 1, base)
            o = align(o + size, 4)

    def file(self, fv, b, o, hl, size, typ, guid, top_off, depth):
        f = {"guid": guid, "type": FILE_TYPES.get(typ, hex(typ)), "size": size,
             "offset": top_off, "phys": self.phys(top_off), "name": None, "images": [],
             "compression": None, "error": None, "depth": depth}
        ctx = {}
        if typ in (0xF0,) or (typ == 0x01 and not self.all_files):
            f["name"] = KNOWN.get(guid)
            return f
        if typ == 0x01:
            return f
        start = o + hl
        for st, body, boff in self.sections(b, start, o + size, ctx, 0, top_off - o if top_off is not None else None):
            if st == 0x15:
                f["name"] = body.decode("utf-16-le", "replace").split("\0")[0]
            elif st in (0x10, 0x12):
                info = pe_info(body) if st == 0x10 else te_info(body)
                if info:
                    info["xip"] = None
                    if boff is not None and self.phys(boff) is not None:
                        p = self.phys(boff)
                        info["xip"] = p + (40 - info["stripped"] if st == 0x12 else 0)
                    info["data"] = body
                    f["images"].append(info)
            elif st == 0x17:
                self.volume(body, 0, len(body), boff, depth + 1, parent=guid)
            elif st in (0x19, 0x18) and self.extract:
                self.out("%s_%s.bin" % (f["name"] or guid, guid[:8]), body)
            elif st == 0x16 and self.extract:
                self.out("%s_%s.compat16.bin" % (f["name"] or guid, guid[:8]), body)
        f["compression"] = ctx.get("compression")
        f["error"] = ctx.get("error")
        if not f["name"]:
            for im in f["images"]:
                if im.get("pdb"):
                    f["name"] = re.split(r"[\\/]", im["pdb"])[-1].rsplit(".", 1)[0]
                    break
        f["name"] = f["name"] or KNOWN.get(guid)
        return f

    def volume(self, b, off, limit, top_off, depth, parent=None):
        """Parse the FV at b[off:]; top_off is its offset in the image when it is stored
        there unmodified (None: it came out of a compressed section)."""
        if off + 0x48 > len(b) or b[off + 0x28:off + 0x2C] != b"_FVH":
            return None
        fs = guid_str(b[off + 0x10:off + 0x20])
        length = u64(b, off + 0x20)
        hlen = u16(b, off + 0x30)
        if length < hlen or length > limit - off or hlen < 0x48 or hlen > length:
            return None
        csum = sum(struct.unpack_from("<%dH" % (hlen // 2), b, off)) & 0xFFFF
        ext = u16(b, off + 0x34)
        name = guid_str(b[off + ext:off + ext + 16]) if ext and off + ext + 16 <= off + length else None
        fv = {"offset": top_off, "phys": self.phys(top_off), "length": length,
              "fs": FS_NAMES.get(fs, fs), "name_guid": name, "checksum_ok": csum == 0,
              "depth": depth, "parent": parent, "files": [], "attributes": u32(b, off + 0x2C)}
        self.fvs.append(fv)
        if self.extract and top_off is None:
            self.out("fv_%s.fv" % (name or parent or "nested"), b[off:off + length])
        if fs == FS_NVRAM or fs in FS_NAMES and "NVRAM" in FS_NAMES[fs]:
            self.varstore(b, off + hlen, off + length, top_off)
            return fv
        if fs not in (FS_FFS2, FS_FFS3):
            return fv
        o = off + (align(ext + u32(b, off + ext + 16), 8) if ext else hlen)
        end = off + length
        while o + 24 <= end:
            if b[o:o + 24] == b"\xff" * 24:          # free space (a pad file has an all-ones name too)
                break
            guid = guid_str(b[o:o + 16])
            typ, attr = b[o + 18], b[o + 19]
            size, hl = u24(b, o + 20), 24
            if attr & 0x01 and fs == FS_FFS3 and size == 0:       # FFS_ATTRIB_LARGE_FILE
                size, hl = u64(b, o + 24), 32
            if size < hl or o + size > end:
                self.notes.append("FV %s: file header at +0x%x is damaged; stopping" % (name or fs, o - off))
                break
            f = self.file(fv, b, o, hl, size, typ, guid, top_off + (o - off) if top_off is not None else None, depth)
            fv["files"].append(f)
            for im in f["images"]:
                self.modules.append((f, im))
            o = align(o + size, 8)
        return fv

    # ---- variable stores ----
    def varstore(self, b, start, end, top_off):
        o = start
        sig = guid_str(b[o:o + 16]) if o + 28 <= end else ""
        if sig in VAR_GUIDS:
            auth, hl, size = "authenticated" in VAR_GUIDS[sig], 28, u32(b, o + 16)
        elif b[o:o + 4] in (b"$VSS", b"$BVS", b"$SVS"):
            auth, hl, size = False, 16, u32(b, o + 4)
        else:
            m = b.find(b"NVAR", start, end)
            if m >= 0:
                self.nvar(b, m, end)
            return
        store = {"format": "authenticated" if auth else "VSS", "offset": top_off and top_off + (o - start),
                 "size": size, "vars": []}
        p, stop = o + hl, min(end, o + size)
        while p + 32 <= stop and u16(b, p) == 0x55AA:
            state, attr = b[p + 2], u32(b, p + 4)
            if auth:
                ns, ds, g, h = u32(b, p + 36), u32(b, p + 40), b[p + 44:p + 60], 60
            else:
                ns, ds, g, h = u32(b, p + 8), u32(b, p + 12), b[p + 16:p + 32], 32
            if ns > 0x1000 or ds > 0x100000:
                break
            name = b[p + h:p + h + ns].decode("utf-16-le", "replace").rstrip("\0")
            if state in (0x3F, 0x3E):                             # VAR_ADDED (+ IN_DELETED_TRANSITION)
                store["vars"].append({"name": name, "guid": guid_str(g), "size": ds, "attr": attr})
            p = align(p + h + ns + ds, 4)
        self.vars.append(store)

    def nvar(self, b, o, end):
        store = {"format": "AMI NVAR (names only; best effort)", "offset": None, "size": end - o, "vars": []}
        seen = set()
        while o + 10 <= end and b[o:o + 4] == b"NVAR":
            size, attr = u16(b, o + 4), b[o + 9]
            if size < 10 or o + size > end:
                break
            if attr & 0x80 and not attr & 0x08:                   # valid, not data-only
                p = o + 10 + (16 if attr & 0x04 else 1)
                if attr & 0x02:
                    e = b.find(b"\0", p, o + size)
                    name = b[p:e].decode("latin-1") if e > 0 else "?"
                else:
                    e = p
                    while e + 1 < o + size and b[e:e + 2] != b"\0\0":
                        e += 2
                    name = b[p:e].decode("utf-16-le", "replace")
                if name not in seen:
                    seen.add(name)
                    store["vars"].append({"name": name, "guid": "", "size": None, "attr": attr})
            o += size
        self.vars.append(store)


# ---- top-level structures -----------------------------------------------------------------

def parse_ifd(d):
    for sigoff in (0x10, 0x0):
        if len(d) >= 0x1000 and u32(d, sigoff) == 0x0FF0A55A:
            break
    else:
        return None
    flmap0, flmap1 = u32(d, sigoff + 4), u32(d, sigoff + 8)
    fcba, nc = (flmap0 & 0xFF) << 4, ((flmap0 >> 8) & 3) + 1
    frba, nr = ((flmap0 >> 16) & 0xFF) << 4, (flmap0 >> 24) & 7
    fmba = (flmap1 & 0xFF) << 4
    regions = []
    # FLREGn run from FRBA up to the next section (usually FMBA): 5 on old PCHs, 16 on new
    nmax = (fmba - frba) // 4 if fmba > frba else max(nr, 5)
    for i in range(min(nmax, 16)):
        o = frba + 4 * i
        if o + 4 > len(d):
            break
        v = u32(d, o)
        base, limit = (v & 0x7FFF) << 12, ((v >> 16) & 0x7FFF) << 12 | 0xFFF
        if v == 0xFFFFFFFF or base > limit:
            continue
        regions.append({"index": i, "name": REGIONS[i], "base": base, "limit": limit})
    return {"signature_at": sigoff, "fcba": fcba, "frba": frba, "fmba": fmba,
            "components": nc, "regions": regions}


def reset_vector(d, sc):
    """Decode the instruction at 0xFFFFFFF0 (the last 16 bytes of the window)."""
    if sc.window_base is None or sc.window_base + (len(d) - sc.window_off) != sc.top or sc.top != FOUR_GIB:
        return None
    o = len(d) - 16
    v = d[o:o + 16]
    i, txt, target = 0, [], None
    if v == b"\xff" * 16:
        return {"bytes": v.hex(), "insn": "erased: no reset vector here (not the top of the boot flash; "
                "pass --top, or scan the whole flash)", "target": None}
    while i < 4 and v[i] == 0x90:
        txt.append("nop")
        i += 1
    if v[i] not in (0xE9, 0xEB, 0xEA):
        # not a plain jump: e.g. OVMF's reset vector first checks whether it already runs in
        # protected mode; its real-mode path is the last jmp rel16 in the 16 bytes
        j = max((k for k in range(i, 14) if v[k] == 0xE9), default=None)
        txt.append("code before the jump (%s)" % v[i:j if j is not None else 16].hex())
        if j is None:
            return {"bytes": v.hex(), "insn": "; ".join(txt), "target": None}
        i = j
    if v[i] == 0xE9:                                   # jmp rel16 (real mode)
        rel = struct.unpack_from("<h", v, i + 1)[0]
        ip = (0xFFF0 + i + 3 + rel) & 0xFFFF
        target = 0xFFFF0000 + ip
        txt.append("jmp near 0x%04x" % ip)
    elif v[i] == 0xEB:
        rel = struct.unpack_from("<b", v, i + 1)[0]
        ip = (0xFFF0 + i + 2 + rel) & 0xFFFF
        target = 0xFFFF0000 + ip
        txt.append("jmp short 0x%04x" % ip)
    elif v[i] == 0xEA:                                 # jmp far ptr16:16 (legacy BIOS)
        ip, cs = u16(v, i + 1), u16(v, i + 3)
        target = (cs << 4) + ip
        txt.append("jmp far %04x:%04x (linear 0x%x: legacy BIOS style, F000 segment below 1 MiB)" % (cs, ip, target))
    else:
        txt.append("bytes %s" % v.hex())
    return {"bytes": v.hex(), "insn": "; ".join(txt), "target": target}


FIT_TYPES = {0: "header", 1: "microcode", 2: "startup ACM", 3: "diagnostic ACM", 7: "BIOS startup module",
             8: "TPM policy", 9: "BIOS policy", 0xA: "TXT policy", 0xB: "key manifest (Boot Guard)",
             0xC: "boot policy manifest (Boot Guard)", 0x10: "CSE secure boot", 0x2D: "ACM feature policy",
             0x2F: "JMP $ debug policy", 0x7F: "skip"}


def parse_fit(d, sc):
    if sc.top != FOUR_GIB or sc.window_base is None:
        return None
    ptr = u32(d, len(d) - 0x40)
    off = ptr - sc.window_base + sc.window_off
    if not (0 <= off < len(d) - 16) or d[off:off + 8] != b"_FIT_   ":
        return None
    n = u24(d, off + 8)
    ents = []
    for i in range(1, min(n, 64)):
        e = off + 16 * i
        addr, typ = u64(d, e), d[e + 14] & 0x7F
        ents.append({"type": FIT_TYPES.get(typ, hex(typ)), "address": addr})
    return {"address": ptr, "entries": ents}


def find_microcode(d):
    found = []
    for o in range(0, len(d) - 48, 16):
        if d[o:o + 4] != b"\x01\x00\x00\x00" or d[o + 0x14:o + 0x18] != b"\x01\x00\x00\x00":
            continue
        rev, date, sig, dsize, tsize = u32(d, o + 4), u32(d, o + 8), u32(d, o + 12), u32(d, o + 28), u32(d, o + 32)
        if dsize == 0:
            dsize, tsize = 2000, 2048
        if tsize % 1024 or tsize < 2048 or o + tsize > len(d) or dsize + 48 > tsize:
            continue
        y = date & 0xFFFF
        if not (0x1995 <= y <= 0x2099):
            continue
        if sum(struct.unpack_from("<%dI" % (tsize // 4), d, o)) & 0xFFFFFFFF:
            continue
        sigs = [sig]
        if tsize > dsize + 48 + 20:                       # extended signature table
            x = o + 48 + dsize
            for k in range(min(u32(d, x), 16)):
                sigs.append(u32(d, x + 20 + 12 * k))
        found.append({"offset": o, "revision": rev, "date": "%04x-%02x-%02x" % (y, date >> 24, (date >> 16) & 0xFF),
                      "signatures": sigs, "pf": u32(d, o + 24), "size": tsize})
    return found


def cpuid_name(sig):
    fam, model = (sig >> 8) & 0xF, (sig >> 4) & 0xF
    if fam == 0xF:
        fam += (sig >> 20) & 0xFF
    if fam in (6, 0xF):
        model |= ((sig >> 16) & 0xF) << 4
    return "family 0x%x model 0x%x stepping %d" % (fam, model, sig & 0xF)


def find_strings(d):
    out = {}
    m = re.search(rb"\$IBIOSI\$(.{0,64})", d)
    if m:
        s = m.group(1).replace(b"\0", b"")
        out["BIOS ID"] = re.sub(rb"[^\x20-\x7e]", b"", s).decode()
    for tag, pat in [("AMI", rb"American Megatrends|AMITSE|\$AMIBIOS"), ("Insyde", rb"InsydeH2O|\$IBIOSD"),
                     ("Phoenix", rb"Phoenix Technologies|PhoenixBIOS|SecureCore"), ("EDK II", rb"EDK II|edk2"),
                     ("coreboot", rb"coreboot"), ("SeaBIOS", rb"SeaBIOS"), ("Award", rb"Award Software"),
                     ("Apple", rb"Apple ROM Version")]:
        if re.search(pat, d) or re.search(pat.decode().encode("utf-16-le"), d):
            out.setdefault("vendor hints", []).append(tag)
    for pat, what in [(rb"\$PIR", "$PIR (PCI IRQ routing table: legacy BIOS)"), (rb"_32_", "_32_ (BIOS32)"),
                      (rb"\$PnP", "$PnP (PnP BIOS)"), (rb"_SM_", "_SM_ (SMBIOS 2.x entry)"),
                      (rb"_SM3_", "_SM3_ (SMBIOS 3 entry)"), (rb"RSD PTR ", "RSD PTR (ACPI RSDP)"),
                      (rb"_MP_", "_MP_ (MP table)"), (rb"\$FID", "$FID (AMI firmware ID)")]:
        if re.search(pat, d):
            out.setdefault("tables", []).append(what)
    return out


def find_option_roms(d):
    roms = []
    for o in range(0, len(d) - 0x40, 512):
        if d[o:o + 2] != b"\x55\xaa":
            continue
        p = u16(d, o + 0x18)
        if not (0x1A <= p < 0x400) or o + p + 0x18 > len(d) or d[o + p:o + p + 4] != b"PCIR":
            continue
        ven, dev = u16(d, o + p + 4), u16(d, o + p + 6)
        cls, code = u24(d, o + p + 0xD), d[o + p + 0x14]
        roms.append({"offset": o, "vendor": ven, "device": dev, "class": cls,
                     "code": {0: "x86 legacy", 1: "Open Firmware", 3: "EFI"}.get(code, hex(code)),
                     "size": d[o + 2] * 512})
        if len(roms) >= 32:
            break
    return roms


def find_fmap(d):
    o = d.find(b"__FMAP__")
    if o < 0 or o + 56 > len(d):
        return None
    base, size, name, n = u64(d, o + 10), u32(d, o + 18), d[o + 22:o + 54].split(b"\0")[0].decode("latin-1"), u16(d, o + 54)
    areas = []
    for i in range(min(n, 64)):
        a = o + 56 + 42 * i
        if a + 42 > len(d):
            break
        areas.append({"offset": u32(d, a), "size": u32(d, a + 4),
                      "name": d[a + 8:a + 40].split(b"\0")[0].decode("latin-1")})
    return {"base": base, "size": size, "name": name, "areas": areas}


CBFS_TYPES = {0x02: "cbfs header", 0x10: "stage", 0x11: "stage (legacy)", 0x20: "payload (SELF)",
              0x30: "option ROM", 0x31: "bootsplash", 0x40: "raw", 0x50: "VSA", 0x51: "MBI",
              0x52: "microcode", 0x53: "FSP", 0x54: "MRC", 0x55: "MMA", 0x56: "EFI", 0x57: "struct",
              0xAA: "cmos default", 0x1AA: "cmos layout", 0xFFFFFFFF: "null"}


def find_cbfs(d):
    files = []
    for m in re.finditer(rb"LARCHIVE", d):
        o = m.start()
        if o % 64 or o + 24 > len(d):
            continue
        ln, typ, off = struct.unpack_from(">III", d, o + 8)[0], struct.unpack_from(">I", d, o + 12)[0], struct.unpack_from(">I", d, o + 20)[0]
        name = d[o + 24:o + off].split(b"\0")[0].decode("latin-1", "replace") if off > 24 else ""
        if typ != 0xFFFFFFFF:
            files.append({"offset": o, "name": name, "type": CBFS_TYPES.get(typ, hex(typ)), "size": ln})
        if len(files) >= 200:
            break
    return files


# ---- main ---------------------------------------------------------------------------------

def scan(path, top, extract=None, all_files=False, microcode=True):
    d = open(path, "rb").read()
    sc = Scan(d, top)
    sc.all_files = all_files
    if extract:
        extract = os.path.abspath(extract)  # modules.tsv paths must not depend on the reader's cwd
        os.makedirs(extract, exist_ok=True)
        sc.extract = extract
    rep = {"file": path, "size": len(d), "top": top}
    ifd = parse_ifd(d)
    rep["descriptor"] = ifd
    bios = None
    if ifd:
        bios = next((r for r in ifd["regions"] if r["name"] == "BIOS"), None)
    if bios and bios["limit"] < len(d):
        size = bios["limit"] - bios["base"] + 1
        sc.window_off, sc.window_base = bios["base"], top - size
        rep["bios_window"] = {"flash": [bios["base"], bios["limit"]], "phys": [top - size, top - 1]}
        region_end = bios["limit"] + 1
    elif len(d) <= 32 << 20:
        sc.window_off, sc.window_base = 0, top - len(d)
        rep["bios_window"] = {"flash": [0, len(d) - 1], "phys": [top - len(d), top - 1]}
        region_end = len(d)
    else:
        region_end = len(d)
    # top-level volumes: scan for _FVH, skipping what earlier volumes cover
    o, covered = 0, 0
    while True:
        o = d.find(b"_FVH", o)
        if o < 0:
            break
        start = o - 0x28
        if start >= covered and start >= 0:
            fv = sc.volume(d, start, len(d), start, 0)
            if fv:
                covered = start + fv["length"]
        o += 4
    rep["fvs"] = sc.fvs
    if bios or sc.window_base is not None:
        dd = d[:region_end] if bios else d
        rv = reset_vector(dd, sc)
        rep["reset_vector"] = rv
        rep["fit"] = parse_fit(dd, sc)
    rep["microcode"] = find_microcode(d) if microcode else []
    rep["strings"] = find_strings(d)
    rep["option_roms"] = find_option_roms(d)
    rep["fmap"] = find_fmap(d)
    rep["cbfs"] = find_cbfs(d)
    rep["variables"] = sc.vars
    rep["notes"] = sc.notes
    mods = []
    for f, im in sc.modules:
        row = {"name": f["name"] or "", "guid": f["guid"], "type": f["type"], "format": im["format"],
               "machine": im["machine"], "image_base": im["image_base"], "entry": im["image_base"] + im["entry_rva"],
               "xip": im.get("xip"), "subsystem": im["subsystem"], "pdb": im.get("pdb"), "file": None}
        if extract:
            base = "%s_%s" % (f["name"] or "unnamed", f["guid"][:8])
            if im["format"] == "TE":
                sc.out(base + ".te", im["data"])
                pe = te_to_pe(im["data"])
                row["file"] = sc.out(base + ".efi", pe) if pe else None
            else:
                row["file"] = sc.out(base + ".efi", im["data"])
        mods.append(row)
    rep["modules"] = mods
    if extract:
        with open(os.path.join(extract, "modules.tsv"), "w") as t:
            t.write("name\tguid\ttype\tformat\tmachine\timage_base\tentry\txip\tfile\n")
            for r in mods:
                t.write("%s\t%s\t%s\t%s\t%s\t0x%x\t0x%x\t%s\t%s\n" % (
                    r["name"], r["guid"], r["type"], r["format"], r["machine"], r["image_base"],
                    r["entry"], "0x%x" % r["xip"] if r["xip"] is not None else "-", r["file"] or "-"))
    return rep


def hx(v):
    return "-" if v is None else "0x%x" % v


def print_report(rep, all_files):
    print("== %s (%d bytes)" % (rep["file"], rep["size"]))
    ifd = rep["descriptor"]
    if ifd:
        print("\n== Intel flash descriptor (signature at 0x%x): %d component(s)" % (ifd["signature_at"], ifd["components"]))
        for r in ifd["regions"]:
            print("   region %-2d %-10s flash 0x%07x-0x%07x (%d KiB)" % (r["index"], r["name"], r["base"], r["limit"],
                                                                   (r["limit"] - r["base"] + 1) >> 10))
    else:
        print("\n== no flash descriptor: the whole file is treated as the BIOS region")
    bw = rep.get("bios_window")
    if bw:
        print("   BIOS region flash 0x%x-0x%x -> physical 0x%x-0x%x (decoded below 0x%x)" % (
            bw["flash"][0], bw["flash"][1], bw["phys"][0], bw["phys"][1], rep["top"]))
    rv = rep.get("reset_vector")
    if rv:
        print("\n== reset vector 0xFFFFFFF0: %s -> %s" % (rv["insn"], hx(rv["target"])))
    fit = rep.get("fit")
    if fit:
        print("== FIT at 0x%x: %s" % (fit["address"], ", ".join("%s @0x%x" % (e["type"], e["address"]) for e in fit["entries"])))
    if rep["microcode"]:
        print("\n== Intel microcode updates (the CPUs this firmware supports; set the board's CPUID signature to one):")
        for m in rep["microcode"]:
            print("   0x%07x rev 0x%x %s  CPUID %s  (%s)  platform 0x%x" % (
                m["offset"], m["revision"], m["date"], " ".join("0x%05x" % s for s in m["signatures"]),
                cpuid_name(m["signatures"][0]), m["pf"]))
    print("\n== firmware volumes (offset/phys: '-' = inside a compressed section, loaded to RAM at run time)")
    for fv in rep["fvs"]:
        print("%s FV %s  offset %s phys %s  0x%x bytes  %s%s%s" % (
            "   " * fv["depth"], fv["name_guid"] or "-", hx(fv["offset"]), hx(fv["phys"]), fv["length"], fv["fs"],
            "" if fv["checksum_ok"] else " (bad header checksum)", " (in %s)" % fv["parent"] if fv["parent"] else ""))
        for f in fv["files"]:
            if f["type"] in ("PAD",) and not all_files:
                continue
            ims = "; ".join("%s %s base 0x%x entry 0x%x%s" % (
                im["format"], im["machine"], im["image_base"], im["image_base"] + im["entry_rva"],
                (" XIP 0x%x%s" % (im["xip"], " (rebased)" if im["xip"] == im["image_base"] else " (NOT at its base)"))
                if im.get("xip") is not None else "") for im in f["images"])
            print("%s   %-18s %-36s %s%s%s%s" % (
                "   " * fv["depth"], f["type"], f["guid"], f["name"] or "", "  [%s]" % f["compression"] if f["compression"] else "",
                "  " + ims if ims else "", "  ERROR " + f["error"] if f["error"] else ""))
    for st in rep["variables"]:
        print("\n== variable store (%s, %s bytes): %d live variables" % (st["format"], st["size"], len(st["vars"])))
        for v in st["vars"][:60]:
            print("   %-28s %s %s" % (v["name"], v["guid"], "%d bytes" % v["size"] if v["size"] is not None else ""))
        if len(st["vars"]) > 60:
            print("   ... %d more" % (len(st["vars"]) - 60))
    s = rep["strings"]
    if s:
        print("\n== identity: %s" % "; ".join("%s: %s" % (k, ", ".join(v) if isinstance(v, list) else v) for k, v in s.items()))
    if rep["option_roms"]:
        print("\n== PCI option ROMs")
        for r in rep["option_roms"]:
            print("   0x%07x %04x:%04x class %06x %s (%d bytes)" % (r["offset"], r["vendor"], r["device"], r["class"], r["code"], r["size"]))
    if rep["fmap"]:
        fm = rep["fmap"]
        print("\n== coreboot FMAP '%s' base 0x%x size 0x%x" % (fm["name"], fm["base"], fm["size"]))
        for a in fm["areas"]:
            print("   0x%07x 0x%07x %s" % (a["offset"], a["size"], a["name"]))
    if rep["cbfs"]:
        print("\n== coreboot CBFS files")
        for f in rep["cbfs"][:80]:
            print("   0x%07x %-12s %8d  %s" % (f["offset"], f["type"], f["size"], f["name"]))
    for n in rep["notes"]:
        print("note: " + n)
    print("\n== %d images in %d volumes; %d with an XIP address" % (
        len(rep["modules"]), len(rep["fvs"]), sum(1 for m in rep["modules"] if m["xip"] is not None)))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("image")
    ap.add_argument("--top", type=lambda v: int(v, 0), default=FOUR_GIB)
    ap.add_argument("--extract")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--all-files", action="store_true")
    ap.add_argument("--no-microcode", action="store_true")
    a = ap.parse_args(argv)
    rep = scan(a.image, a.top, a.extract, a.all_files, not a.no_microcode)
    if a.json:
        for fv in rep["fvs"]:
            for f in fv["files"]:
                for im in f["images"]:
                    im.pop("data", None)
        print(json.dumps(rep, indent=1, default=str))
    else:
        print_report(rep, a.all_files)
    return 0


if __name__ == "__main__":
    sys.exit(main())
