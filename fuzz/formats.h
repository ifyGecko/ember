/*
 * formats.h - The files and the wire protocol of ember's fuzz layer, header-only.
 *
 *   site list        (UAMS)  the coverage sites: one record per site, its id is its index
 *   boundary file    (UAMB)  where an iteration begins and ends
 *   coverage region  (UAMC)  the harness's maps in shared memory, for readers outside ember
 *   fuzz protocol    (UAMF)  inputs from a fuzz brain over TCP, verdicts and maps back
 *
 * The prose specification is docs/fuzz-formats.md. The writers are ember's own tools
 * (fuzz/tools/: gen_sites.py, gen_boundaries.py, fuzz_brain.py) and fuzz/cov_region.c,
 * fuzz/frontend_net.c; the readers are fuzz/site_list.c, fuzz/tools/cov_region.py and the
 * brain. Every format starts at version 1.
 *
 * Byte order: the site list and boundary file are little-endian on disk; the coverage
 * region is in the byte order of the ember process that created it; the fuzz protocol is
 * big-endian (network order). The structs describe layout only, and the C readers decode
 * each field explicitly, so they work on any host.
 *
 * Only <stdint.h>; C11; builds with ember's strict warning flags on its own.
 */
#ifndef UAM_FUZZ_FORMATS_H
#define UAM_FUZZ_FORMATS_H

#include <stdint.h>

#define UAM_FMT_PACKED __attribute__((packed))

/* ===================================================================== *
 * Site list                                                              *
 * ===================================================================== *
 *
 *   [struct uam_sites_header]                          24 bytes
 *   [count x struct uam_sites_record]                  16 bytes each
 *   [module name table]  at modules_offset, modules_size bytes: module_count
 *                        NUL-terminated names, back to back
 *   [module base table]  only with UAM_SITES_FLAG_BASES: module_count x u64 link
 *                        bases, at UAM_SITES_BASES_OFFSET(modules_offset, modules_size)
 *
 * record.offset is the site's address minus its module's link base, without any
 * instruction-set state bits; UAM_SITE_ALT_STATE says the site is in the ISA's alternate
 * state (ARM: Thumb; MIPS: MIPS16e or microMIPS), so ember sets the state bits in its pc.
 * orig[] holds the first `len` bytes of the instruction as they sit in memory (0..4; 0:
 * nothing to check), which --cov-strict compares with the guest's memory to catch a stale
 * list. */

#define UAM_SITES_MAGIC   "UAMS"
#define UAM_SITES_VERSION 1u

#define UAM_SITES_FLAG_BASES (1u << 0)        /* a module base table follows the names */
#define UAM_SITES_BASES_OFFSET(modules_offset, modules_size) \
    ((((uint64_t)(modules_offset) + (uint64_t)(modules_size)) + 7u) & ~(uint64_t)7u)

#define UAM_SITE_ALT_STATE   (1u << 0)        /* record.flags, marker.flags */

#define UAM_SITES_ORIG_MAX     4u
#define UAM_SITES_MAX_MODULES  255u
#define UAM_SITES_NAMES_MAX    4096u          /* bytes in the module name table */

struct uam_sites_header {
    char     magic[4];        /* "UAMS" */
    uint32_t version;         /* 1 */
    uint32_t count;           /* records following the header */
    uint8_t  flags;           /* UAM_SITES_FLAG_*; other bits must be 0 */
    uint8_t  module_count;    /* 1..255 names in the name table */
    uint16_t reserved;        /* 0 */
    uint32_t modules_offset;  /* file offset of the name table */
    uint32_t modules_size;    /* bytes in the name table, last byte NUL */
} UAM_FMT_PACKED;

struct uam_sites_record {
    uint64_t offset;          /* address - link base of module[module] */
    uint8_t  module;          /* index into the name table */
    uint8_t  len;             /* bytes of orig[] to check, 0..4 */
    uint8_t  flags;           /* UAM_SITE_ALT_STATE; other bits must be 0 */
    uint8_t  reserved;        /* 0 */
    uint8_t  orig[UAM_SITES_ORIG_MAX];   /* first `len` bytes meaningful, the rest 0 */
} UAM_FMT_PACKED;

_Static_assert(sizeof(struct uam_sites_header) == 24, "site list header is 24 bytes");
_Static_assert(sizeof(struct uam_sites_record) == 16, "site list record is 16 bytes");

/* ===================================================================== *
 * Boundary file                                                          *
 * ===================================================================== *
 *
 * [struct uam_bounds_header][count x struct uam_bounds_record], little-endian.
 * record.offset is relative to module 0's link base (the base the site list's module-0
 * offsets use). A file with an END marker brackets each iteration BEGIN..END; one with
 * only BEGIN markers runs each iteration from a BEGIN to the next. Every marker of a file
 * has the same depth filter: DEPTH_ZERO keeps nested arrivals (recursion) inside one
 * iteration. */

#define UAM_BOUNDS_MAGIC   "UAMB"
#define UAM_BOUNDS_VERSION 1u

#define UAM_BOUNDS_BEGIN 0u            /* record.kind */
#define UAM_BOUNDS_END   1u

#define UAM_BOUNDS_DEPTH_ANY  0u       /* record.depth */
#define UAM_BOUNDS_DEPTH_ZERO 1u

struct uam_bounds_header {
    char     magic[4];        /* "UAMB" */
    uint32_t version;         /* 1 */
    uint32_t count;
    uint32_t reserved;        /* 0 */
} UAM_FMT_PACKED;

struct uam_bounds_record {
    uint64_t offset;          /* from module 0's link base, without state bits */
    uint8_t  kind;            /* UAM_BOUNDS_BEGIN / _END */
    uint8_t  depth;           /* UAM_BOUNDS_DEPTH_* */
    uint8_t  flags;           /* UAM_SITE_ALT_STATE; other bits must be 0 */
    uint8_t  reserved[5];     /* 0 */
} UAM_FMT_PACKED;

_Static_assert(sizeof(struct uam_bounds_header) == 16, "boundary header is 16 bytes");
_Static_assert(sizeof(struct uam_bounds_record) == 16, "boundary record is 16 bytes");

/* ===================================================================== *
 * Coverage region (shared memory or a file)                              *
 * ===================================================================== *
 *
 *   offset 0         struct uam_cov_region_header (64 bytes)
 *   bitmap_offset    the union of every published map (saturating), map_size bytes
 *   ring_offset      ring_size slots of slot_stride bytes: struct uam_cov_slot + map
 *
 * Host byte order of the writer (a reader tells it from `version`). The magic is written
 * last. A reader takes gen_current (acquire), reads slot (gen % ring_size), and accepts it
 * only if slot_gen == gen both before and after copying the map (a seqlock). */

#define UAM_COV_REGION_MAGIC   "UAMC"
#define UAM_COV_REGION_VERSION 1u

/* header.flags: the coverage scheme the bucket indices follow. SCHEME says the other
 * bits are valid (clear: block coverage, no scheme); MODEL_PC = hashed-pc model (clear:
 * site ids); ROTATE = N-gram rotation; bits 8..11 = N. The fuzz protocol's HELLO flags
 * are the same bits. */
#define UAM_COV_SCHEME          (1u << 0)
#define UAM_COV_SCHEME_PC       (1u << 1)
#define UAM_COV_SCHEME_ROTATE   (1u << 2)
#define UAM_COV_NGRAM_SHIFT     8
#define UAM_COV_NGRAM_MASK      (0xfu << UAM_COV_NGRAM_SHIFT)

struct uam_cov_region_header {
    char     magic[4];        /* "UAMC" */
    uint16_t version;         /* 1 */
    uint16_t ring_size;       /* slots in the ring, 2..65535 */
    uint32_t map_size;        /* map bytes; a power of two */
    uint32_t slot_stride;     /* bytes per slot (slot header + map) */
    uint32_t flags;           /* UAM_COV_SCHEME* */
    uint32_t reserved0;       /* 0 */
    uint64_t gen_current;     /* atomic: highest generation assigned */
    uint64_t gen_init;        /* generation of the INIT snapshot (0: none yet) */
    uint64_t bitmap_offset;   /* from the region's start */
    uint64_t ring_offset;     /* from the region's start */
    uint32_t owner_pid;       /* the writer's pid ... */
    uint32_t owner_start;     /* ... and start time (clock ticks since boot, low 32 bits) */
};

_Static_assert(sizeof(struct uam_cov_region_header) == 64, "region header is 64 bytes");

/* Snapshot kinds: the region's slot.kind and the fuzz protocol's COVERAGE kind. */
#define UAM_COV_KIND_INIT 1u   /* boot up to the first begin */
#define UAM_COV_KIND_ITER 2u   /* one iteration (one input) */

struct uam_cov_slot {
    uint64_t slot_gen;        /* atomic: 0 while being written; else its generation */
    uint64_t ns;              /* CLOCK_MONOTONIC when published */
    uint32_t kind;            /* UAM_COV_KIND_* */
    uint32_t reserved;        /* 0 */
};                            /* followed by map_size bytes */

_Static_assert(sizeof(struct uam_cov_slot) == 24, "slot header is 24 bytes");

/* ===================================================================== *
 * Fuzz protocol                                                          *
 * ===================================================================== *
 *
 * One TCP connection: ember (runs inputs) dials the brain (chooses them), sends the
 * 4-byte magic once, then both sides exchange frames
 *
 *     [u32 length][u8 type][payload...]        length counts type + payload
 *
 * big-endian throughout, maps as raw u8 counters. Lockstep: each INPUT gets exactly one
 * COVERAGE (or SPARSE) or CRASH reply carrying its id; ember sends one unsolicited COVERAGE
 * (id 0, kind INIT) first. A brain that sends OPTIONS with UAM_NET_OPT_SPARSE (after HELLO,
 * before its first INPUT) gets SPARSE replies in place of COVERAGE from then on; an ember
 * that predates OPTIONS ignores it, as every peer ignores unknown frame types. */

#define UAM_FUZZ_NET_MAGIC     "UAMF"
#define UAM_FUZZ_NET_VERSION   1u
#define UAM_FUZZ_NET_FRAME_MAX (64u << 20)     /* bytes in one frame, type included */

enum uam_fuzz_net_frame {
    UAM_NET_HELLO    = 1,   /* ember -> brain, once after the magic */
    UAM_NET_INPUT    = 2,   /* brain -> ember: one input */
    UAM_NET_COVERAGE = 3,   /* ember -> brain: one map */
    UAM_NET_CRASH    = 4,   /* ember -> brain: the input did not end normally */
    UAM_NET_BYE      = 5,   /* either way: clean shutdown */
    UAM_NET_OPTIONS  = 6,   /* brain -> ember, optional, once after HELLO: what it wants */
    UAM_NET_SPARSE   = 7,   /* ember -> brain: one map as its set buckets (OPTIONS asked) */
};

/* HELLO flags beyond the UAM_COV_SCHEME* bits */
#define UAM_NET_HELLO_BIG_ENDIAN  (1u << 15)  /* the target's data byte order is big-endian */

/* OPTIONS flags */
#define UAM_NET_OPT_SPARSE        (1u << 0)   /* reply with SPARSE instead of COVERAGE */

/* HELLO:    u16 version, u16 flags (UAM_COV_SCHEME* bits), u32 map_size,
 *           u16 arch_len (<= UAM_NET_ARCH_MAX), u8 arch[arch_len] (the ISA's name)
 * INPUT:    u64 id (non-zero), u32 len, u8 data[len]
 * COVERAGE: u64 id (0: the INIT map), u8 kind (UAM_COV_KIND_*), u32 map_size, u8 map[]
 * CRASH:    u64 id, u8 reason (enum uam_fuzz_net_reason), u32 status
 * OPTIONS:  u32 flags (UAM_NET_OPT_*); more fields may follow, to be ignored
 * SPARSE:   u64 id (0: the INIT map), u8 kind, u64 insns (instructions the iteration ran),
 *           u32 map_size, u32 n, then n x (u32 index, u8 count), indices ascending. A map
 *           with too many set buckets for the list to be smaller goes as COVERAGE instead. */
#define UAM_NET_HELLO_FIXED_LEN    10u
#define UAM_NET_ARCH_MAX           64u
#define UAM_NET_INPUT_HDR_LEN      12u
#define UAM_NET_COVERAGE_HDR_LEN   13u
#define UAM_NET_CRASH_LEN          13u
#define UAM_NET_OPTIONS_MIN_LEN    4u
#define UAM_NET_SPARSE_HDR_LEN     25u
#define UAM_NET_SPARSE_ENTRY_LEN   5u

/* (reason, status) is a crash's identity for deduplication. */
enum uam_fuzz_net_reason {
    UAM_NET_FAULT     = 1,  /* a guest exception;        status = faulting pc (low 32 bits) */
    UAM_NET_GUARD     = 2,  /* a redzone or --guard hit; status = pc of the access */
    UAM_NET_ORACLE    = 3,  /* a crash pc, halt status or interrupt fired; status = detail */
    UAM_NET_HANG      = 4,  /* no forward progress;      status = the stop reason */
    UAM_NET_TIMEOUT   = 5,  /* the per-input budget ran out; status 0 */
    UAM_NET_REJECTED  = 6,  /* the input route refused the input; status 0 */
    UAM_NET_MALFORMED = 7,  /* a malformed INPUT frame, nothing ran; status 0; id 0 if the
                             * payload was shorter than 8 bytes */
};

#endif /* UAM_FUZZ_FORMATS_H */
