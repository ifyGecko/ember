/*
 * uam/bus.h - The Bus interface: everything the machine knows about memory.
 *
 * The bus owns a map of regions:
 *   RAM    host-backed, read/write
 *   ROM    host-backed, read-only for guest accesses (loader may write)
 *   MMIO   dispatched to a device (device.h)
 *   ROMD   a "ROM device": host-backed and fetchable while in ROM mode, with
 *          every write (and, out of ROM mode, every read) going to its device.
 *          Models memory-mapped flash with a command/register interface.
 *   ALIAS  a window onto (part of) another region, e.g. mirrors of a flash
 *          chip; accesses behave exactly as on the target.
 * Accesses that hit no region go to an optional "unmapped" device (the
 * unknown-MMIO policy, see devices/unknown_mmio.c) or fault.
 *
 * Regions may overlap when their priorities differ: an access goes to the
 * highest-priority region containing it (so catch-all windows sit at a low
 * priority under real devices, and a register can overlay part of a RAM or
 * flash page). Overlap at equal priority is an error. An access that spans
 * several regions is split into bytes.
 *
 * A small direct-mapped page cache (a software TLB) makes the common RAM/ROM
 * case cheap; it caches only pages wholly owned by one region and free of
 * watchpoints. The bus also tracks which RAM/ROM/ROMD pages contain
 * translated code, keeps a generation counter per page, and calls back into
 * the executor when such a page changes (self-modifying code,
 * docs/uam-isa.md 8.6).
 *
 * Byte order: RAM/ROM store guest bytes; the access attributes (LE/BE) decide
 * how bytes become a value. Devices see values in their own declared byte
 * order (device.h); the bus converts.
 *
 * Dependencies: common.h, access.h. The bus does not know about CPUs or the IR.
 */
#ifndef UAM_BUS_H
#define UAM_BUS_H

#include <stdio.h>

#include "uam/access.h"
#include "uam/common.h"

#define UAM_PAGE_SHIFT 12u
#define UAM_PAGE_SIZE  (UINT64_C(1) << UAM_PAGE_SHIFT)
#define UAM_PAGE_MASK  (~(UAM_PAGE_SIZE - 1))
#define UAM_BUS_TLB_SIZE 64u

/* Priority for catch-all windows such as devices/unimp.c, below every real device. */
#define UAM_PRIO_BACKGROUND (-1000)
/* Lowest priority for per-register stubs (devices/mmio_stub.c): above catch-all
 * windows, below every real device. Stubs use UAM_PRIO_STUB .. UAM_PRIO_STUB + 511. */
#define UAM_PRIO_STUB       (-900)

struct uam_device;

typedef enum uam_region_kind {
    UAM_REGION_RAM, UAM_REGION_ROM, UAM_REGION_MMIO, UAM_REGION_ROMD, UAM_REGION_ALIAS,
} uam_region_kind;

/* Optional mapping attributes; a zeroed struct (or NULL) gives the defaults. */
typedef struct uam_map_attrs {
    int      prio;    /* overlapping regions: the highest priority wins (default 0) */
    unsigned endian;  /* MMIO/ROMD: override the device's byte order (0 = device's own) */
    bool     disabled;/* start disabled (uam_bus_region_enable) */
} uam_map_attrs;

typedef struct uam_region {
    const char        *name;
    uint64_t           base, size;
    uam_region_kind    kind;
    int                prio;
    unsigned           endian;       /* MMIO/ROMD: byte-order override, 0 = device's */
    uint8_t           *host;         /* RAM/ROM/ROMD backing store */
    struct uam_device *dev;          /* MMIO/ROMD target */
    uint32_t          *page_gen;     /* RAM/ROM/ROMD: per-page generation */
    uint8_t           *code_page;    /* RAM/ROM/ROMD: 1 if the page holds translated code */
    uint64_t          *code_lines;   /* RAM/ROM/ROMD: per page, bit n = 64-byte line n does */
    uint64_t           n_code_pages; /* number of code_page entries set */
    uint64_t          *dirty;        /* RAM/ROM/ROMD: bitmap of pages written since the last
                                      * uam_bus_dirty_reset (snapshots) */
    bool               rom_mode;     /* ROMD: reads and fetches use the backing store */
    struct uam_region *target;       /* ALIAS: the aliased region (never an alias) */
    uint64_t           target_off;   /* ALIAS: offset of `base` within the target */
    bool               disabled;     /* not decoded (uam_bus_region_enable) */
} uam_region;

/* Called when a page that holds translated code changes. */
typedef void (*uam_code_write_fn)(void *ctx, uint64_t page_base);

/* Watchpoint callback: a guest data access hit a watched physical range. Runs
 * after the access completed; `val` is the value read or written. */
typedef void (*uam_watch_fn)(void *ctx, uint64_t addr, unsigned size, bool is_write, uint64_t val);

enum { UAM_WATCH_READ = 1u << 0, UAM_WATCH_WRITE = 1u << 1 };

typedef struct uam_watch {
    int          id;
    uint64_t     base, size;
    unsigned     kinds;     /* UAM_WATCH_READ | UAM_WATCH_WRITE */
    uam_watch_fn fn;
    void        *ctx;
} uam_watch;

typedef struct uam_bus {
    uam_region       **regions;            /* sorted by priority, highest first */
    uint32_t           n_regions, cap_regions;
    struct uam_device *unmapped;           /* fallback for unmapped data accesses */
    struct {
        uint64_t    page;
        uam_region *r;        /* resolved region (never an alias); NULL = empty */
        uint64_t    origin;   /* offset in r = addr - origin */
    } tlb[UAM_BUS_TLB_SIZE];
    uam_code_write_fn  on_code_write;
    void              *code_write_ctx;
    uam_watch         *watches;
    uint32_t           n_watches, cap_watches;
    int                next_watch_id;
    uint32_t           direct_gen;         /* bumped when uam_bus_page_host answers may change */
    uint32_t           gen_seq;            /* source of page generations (unique bus-wide) */
    bool               track_dirty;        /* uam_bus_page_host withholds clean pages from writers */
} uam_bus;

void       uam_bus_init(uam_bus *bus);
void       uam_bus_fini(uam_bus *bus);

uam_status uam_bus_map_ram(uam_bus *bus, const char *name, uint64_t base, uint64_t size);
uam_status uam_bus_map_ram_ex(uam_bus *bus, const char *name, uint64_t base, uint64_t size,
                              const uam_map_attrs *attrs, uam_region **out);
uam_status uam_bus_map_rom(uam_bus *bus, const char *name, uint64_t base, uint64_t size,
                           const void *data, size_t len);
uam_status uam_bus_map_mmio(uam_bus *bus, const char *name, uint64_t base, uint64_t size,
                            struct uam_device *dev);
uam_status uam_bus_map_mmio_ex(uam_bus *bus, const char *name, uint64_t base, uint64_t size,
                               struct uam_device *dev, const uam_map_attrs *attrs);
/* ROM device. Starts in ROM mode with `data` (may be NULL) as its contents.
 * *out (optional) receives the region handle, for the device. */
uam_status uam_bus_map_romd(uam_bus *bus, const char *name, uint64_t base, uint64_t size,
                            struct uam_device *dev, const void *data, size_t len,
                            const uam_map_attrs *attrs, uam_region **out);
/* Map [base, base+size) onto target's [offset, offset+size). */
uam_status uam_bus_map_alias(uam_bus *bus, const char *name, uint64_t base, uint64_t size,
                             const uam_region *target, uint64_t offset,
                             const uam_map_attrs *attrs);
void       uam_bus_set_unmapped(uam_bus *bus, struct uam_device *dev);
void       uam_bus_set_code_write_hook(uam_bus *bus, uam_code_write_fn fn, void *ctx);

/* Region handle by name (the name given when mapping), or NULL. */
uam_region *uam_bus_region(uam_bus *bus, const char *name);

/*
 * Run-time decode changes, for chipset and bus-bridge models (PCI BARs, shadow
 * RAM, SMRAM windows): a disabled region is skipped as if unmapped, and a moved
 * region decodes at its new base. Overlap at equal priority is only checked
 * when mapping; where enabled regions overlap at one priority, the one mapped
 * first wins. Translated code is revalidated against the new map (page
 * generations are unique across regions), and direct host-page answers are
 * withdrawn.
 */
void       uam_bus_region_enable(uam_bus *bus, uam_region *r, bool on);
void       uam_bus_region_move(uam_bus *bus, uam_region *r, uint64_t base);

/* ROMD control, for the owning device. Leaving ROM mode discards code
 * translated from the region. After changing the backing store, call
 * uam_bus_romd_dirty so translated code from those bytes is discarded. */
void       uam_bus_romd_set_mode(uam_bus *bus, uam_region *r, bool rom);
uint8_t   *uam_bus_romd_backing(uam_region *r);
void       uam_bus_romd_dirty(uam_bus *bus, uam_region *r, uint64_t off, uint64_t len);

/* Guest-visible accesses. size is in bytes: 1, 2, 4 or 8. */
uam_status uam_bus_read(uam_bus *bus, uint64_t addr, unsigned size, unsigned attrs, uint64_t *val);
uam_status uam_bus_write(uam_bus *bus, uint64_t addr, unsigned size, unsigned attrs, uint64_t val);

/* Loader access: copy bytes into RAM/ROM/ROMD, ignoring ROM write protection. */
uam_status uam_bus_load_bytes(uam_bus *bus, uint64_t addr, const void *data, size_t len);

/*
 * DMA master access, for devices (NIC descriptor rings and frame buffers, ...):
 * copy guest bytes in memory order, with no byte-order conversion. Reads come
 * from RAM, ROM, or ROMD in ROM mode; writes go to RAM only, and translated
 * code from the written bytes is discarded. Any other target (MMIO, unmapped)
 * fails with UAM_ERR_FAULT, possibly after part of the transfer. Watchpoints
 * are not triggered (they observe the CPU's accesses).
 */
uam_status uam_bus_dma_read(uam_bus *bus, uint64_t addr, void *buf, size_t len);
uam_status uam_bus_dma_write(uam_bus *bus, uint64_t addr, const void *buf, size_t len);

/* Watchpoints on physical ranges, for guest data accesses (not fetches or
 * debug accesses). Returns an id (> 0), or a negative value on failure. */
int        uam_bus_add_watch(uam_bus *bus, uint64_t base, uint64_t size, unsigned kinds,
                             uam_watch_fn fn, void *ctx);
bool       uam_bus_remove_watch(uam_bus *bus, int id);

/*
 * Direct host access for an executor's memory fast path: the host address of the
 * start of the page containing `paddr`, or NULL when accesses to that page must go
 * through uam_bus_read/uam_bus_write (devices, unmapped or watched addresses, pages
 * shared between regions, ROMD in device mode, ROM/ROMD for writes, and - for writes -
 * pages holding translated code, so self-modifying code is still detected).
 * Answers stay valid until bus->direct_gen changes.
 */
uint8_t   *uam_bus_page_host(uam_bus *bus, uint64_t paddr, bool write);

/*
 * Dirty-page tracking, for snapshots (snapshot.h). Every write to a RAM, ROM
 * or ROMD backing store through the bus - guest stores, loader, DMA, and ROMD
 * updates reported with uam_bus_romd_dirty - marks its pages dirty. Once
 * tracking is on, uam_bus_page_host refuses write access to clean pages, so
 * an executor's first store to a page takes the uam_bus_write path, which
 * marks it; later stores to it run on the fast path again.
 *
 * Code that writes a backing store directly (through region->host or
 * uam_bus_romd_backing) must report it with uam_bus_mark_dirty or
 * uam_bus_romd_dirty, or a snapshot restore will not undo the change.
 */
/* Mark every page clean and turn tracking on. Executors must drop cached
 * write access (the bus bumps direct_gen). */
void       uam_bus_dirty_reset(uam_bus *bus);
static inline bool uam_region_page_dirty(const uam_region *r, uint64_t page_index)
{
    return (r->dirty[page_index >> 6] >> (page_index & 63)) & 1;
}
/* Report a direct write to the backing store at physical [addr, addr+len). */
void       uam_bus_mark_dirty(uam_bus *bus, uint64_t addr, uint64_t len);
/* Copy `len` bytes into r's backing store at `off` (RAM, ROM or ROMD; ROM
 * protection does not apply), dropping code translated from those pages. */
void       uam_bus_region_store(uam_bus *bus, uam_region *r, uint64_t off, const void *src,
                                uint64_t len);

/* Code-page tracking for the translation cache. */
void       uam_bus_mark_code(uam_bus *bus, uint64_t addr);
/*
 * Mark the bytes [addr, addr+len) as translated code. Code is tracked in
 * 64-byte lines: a write to a code page invalidates its translations (bumps
 * its generation and calls the code-write hook) only when it touches a line
 * holding code, so data that shares a page with code (firmware sections are
 * often not page-aligned) does not throw the code away. uam_bus_mark_code
 * marks the whole page.
 */
void       uam_bus_mark_code_range(uam_bus *bus, uint64_t addr, uint64_t len);
uint32_t   uam_bus_page_gen(uam_bus *bus, uint64_t addr);

/* The region an access to addr goes to (an alias is returned as itself). */
const uam_region *uam_bus_find(const uam_bus *bus, uint64_t addr);
void       uam_bus_dump_map(const uam_bus *bus, FILE *out);

#endif /* UAM_BUS_H */
