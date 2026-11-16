/*
 * uam/heap.h - A guarded heap: an allocator for guest memory that catches heap misuse.
 *
 * Firmware on flat memory can run off the end of a heap block, use one after freeing it or
 * free one twice, and nothing faults. A guarded heap is an allocator ember runs in place
 * of the firmware's own, over a range of guest memory the board gives it (the arena):
 *
 *   - every block lies between redzones, and every byte of the arena that is not inside a
 *     live block is poisoned: a guest access that touches one is a hit, to the byte (an
 *     overflow or underflow by one, a block used after its free, a stray pointer);
 *   - the bookkeeping is the host's, so the guest cannot corrupt it and no access by an
 *     allocator has to be excused;
 *   - a freed block stays poisoned in a quarantine and its memory is not handed out again
 *     until newer frees push it out, so a late use still hits;
 *   - a free of a freed block or of an address that starts no block is a hit as well.
 *
 * Ember does not know which firmware functions allocate, nor how they take arguments:
 * that is the board's knowledge. The board hooks the firmware's allocation and release
 * functions (uam_machine_add_pc_hook at their entry), reads the arguments from the
 * registers or stack slots its calling convention uses, calls the heap, writes the result
 * where the caller expects it and continues at the caller's return address, so the
 * firmware's allocator never runs:
 *
 *     static uam_hook_action on_malloc(void *ctx, uam_cpu *cpu, uint64_t pc)
 *     {
 *         uint64_t size = <first argument>, ra = <return address>;
 *         <result register> = uam_heap_alloc(ctx, size, 0, ra);
 *         cpu->pc = ra;
 *         return UAM_HOOK_CONTINUE;
 *     }
 *
 * (skills/ember-dev-build/references/board-skeleton.md has a worked board.) Accesses are
 * checked from the heap's first allocation on; code of the firmware that walks or sets up
 * its own heap in the arena after that must be hooked or stubbed the same way, or the
 * heap disarmed around it (uam_heap_arm).
 *
 * Hits are guard hits (guard.h): they are reported through uam_machine_guard_report with
 * the heap's action - STOP ends the run with UAM_STOP_GUARD before the access happens
 * (the fuzz harness's "guard" verdict), FAULT gives the guest a bus fault, LOG prints a
 * line and lets the access through - and with the block, where it was allocated and
 * where it was freed (the `site` values the board passed, normally return addresses).
 * Instruction fetches, debugger accesses and DMA (a device model's own accesses) are not
 * checked.
 *
 * The blocks and the quarantine are machine state: snapshots carry them, and a fuzz
 * harness's restore brings the heap back to what it was at the begin pc. Create the heap
 * before the machine runs and before a snapshot is taken or loaded.
 *
 * Cost: accesses to the arena's pages leave the JIT's inline memory path (each is checked);
 * the allocator itself is host code.
 *
 * Dependencies: guard.h.
 */
#ifndef UAM_HEAP_H
#define UAM_HEAP_H

#include "uam/guard.h"

typedef struct uam_heap uam_heap;

typedef struct uam_heap_opts {
    uint64_t         base, size;   /* the arena [base, base + size) */
    bool             phys;         /* base is physical (else virtual, as processor 0 maps it
                                    * when the heap is created) */
    uint32_t         align;        /* every block's alignment at least: a power of two
                                    * (0: 8) */
    uint32_t         redzone;      /* poisoned bytes kept between blocks at least (0: 32) */
    uint64_t         quarantine;   /* freed bytes held back from reuse (0: a quarter of the
                                    * arena; UINT64_MAX: none) */
    int              fill_alloc;   /* the byte new blocks are filled with, 0..255 (-1: left
                                    * as they are); uam_heap_calloc zeroes regardless */
    int              fill_free;    /* the byte freed blocks are filled with (-1: left) */
    uam_guard_action action;       /* what a hit does (UAM_GUARD_STOP is 0) */
    const char      *name;         /* in reports and in snapshots; copied; unique among the
                                    * machine's heaps (NULL: "heap") */
} uam_heap_opts;

/* Defaults for everything but the arena: fills off, STOP. */
#define UAM_HEAP_OPTS_INIT { .fill_alloc = -1, .fill_free = -1 }

/* NULL with err saying why: an empty or wrapping arena, a virtual arena processor 0 does
 * not map, a name in use, out of memory. */
uam_heap *uam_heap_create(uam_machine *m, const uam_heap_opts *o, char *err, size_t errlen);
void      uam_heap_destroy(uam_heap *h);

/*
 * The allocator. `site` identifies the caller in reports (0: unknown). Addresses are the
 * arena's (virtual if it is). A block of size 0 is a valid block no byte of which may be
 * touched.
 *   alloc:   `align` 0 means the heap's. 0: no room (or a bad alignment).
 *   calloc:  n * size bytes, zeroed. 0: no room or the product overflows.
 *   realloc: p == 0 allocates; size == 0 frees and returns 0; otherwise the contents
 *            move to a new block (always: stale pointers to the old one then hit) and the
 *            old one is freed. 0: no room (the old block stays), or p is no live block
 *            (reported as a bad free).
 *   free:    p == 0 does nothing. false: p is no live block; it was reported
 *            (HEAP_DOUBLE_FREE or HEAP_BAD_FREE) and nothing was freed.
 */
uint64_t uam_heap_alloc(uam_heap *h, uint64_t size, uint64_t align, uint64_t site);
uint64_t uam_heap_calloc(uam_heap *h, uint64_t n, uint64_t size, uint64_t site);
uint64_t uam_heap_realloc(uam_heap *h, uint64_t p, uint64_t size, uint64_t site);
bool     uam_heap_free(uam_heap *h, uint64_t p, uint64_t site);

typedef struct uam_heap_info {
    uint64_t addr, size;
    bool     live;                 /* false: freed, still in quarantine */
    uint64_t alloc_site, free_site;
} uam_heap_info;

/* The block `addr` lies in (live or quarantined). false: none. For a firmware function
 * that asks for a block's size. */
bool uam_heap_block(const uam_heap *h, uint64_t addr, uam_heap_info *out);

/* Checking starts with the heap's first allocation, so that start-up code which clears
 * or prepares the arena's memory is not a hit. This starts it at once (true) or suspends
 * it (false) until told otherwise; allocation works either way. Part of the heap's state
 * in snapshots. */
void uam_heap_arm(uam_heap *h, bool on);

typedef struct uam_heap_stats {
    uint64_t allocs, frees, failed;       /* calls; failed: allocations with no room */
    uint64_t live_blocks, live_bytes;
    uint64_t peak_bytes;                  /* the most live_bytes has been */
    uint64_t quarantined_blocks, quarantined_bytes;
    uint64_t hits;
} uam_heap_stats;

void uam_heap_get_stats(const uam_heap *h, uam_heap_stats *out);

#endif /* UAM_HEAP_H */
