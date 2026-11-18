/*
 * uam/uninit.h - Finding what a run reads before anything wrote it.
 *
 * A firmware that reads a RAM byte nobody wrote since the machine was built gets zero
 * here, and on hardware gets whatever the RAM powered up with. A run that does so can
 * hide a bug in plain sight. This asks the question another way: run the same input
 * twice, once as usual and once with the memory nothing wrote filled with a pattern, and
 * compare. Everything that differs between the two is a behaviour that depends on a byte
 * the firmware never wrote (the fuzz harness does the comparison: uam_fuzz_check_uninit,
 * fuzz.h).
 *
 * What "nothing wrote" means: the pages of RAM that no write has reached between the
 * machine being built and the begin snapshot (the boot's), leaving out what was excluded.
 * The boot runs on ordinary zeroed RAM, so a firmware that relies on zeros during its own
 * start-up is not disturbed. In the filled run each such page fills itself with the
 * pattern when it is first touched (bus.h, uam_bus_lazy_arm), so nothing is paid for RAM
 * the run never reaches, and a restore of the snapshot puts the zeros back.
 *
 * What it does not see: a page the boot wrote even once (its other bytes stay zero in both
 * runs), a read that changes nothing the comparison looks at, and memory hardware zeroes
 * that the firmware relies on (name it with uam_machine_uninit_exclude). A guarded heap
 * (heap.h) fills each fresh allocation with the pattern in the filled run, which also
 * covers allocations from memory the boot had already written.
 *
 * Turning it on is the caller's choice and costs nothing while it is off. When on, the
 * machine keeps a bitmap per RAM region of the pages written (saved in snapshots: a
 * snapshot made with it loads only with it) and the executors' first store to each page
 * takes the bus's slow path, once.
 *
 * Dependencies: machine.h.
 */
#ifndef UAM_UNINIT_H
#define UAM_UNINIT_H

#include "uam/machine.h"

/* The default fill: a 32-bit value, laid down in the guest's data byte order from the
 * start of each page, so that an aligned word of it reads back as this number. */
#define UAM_UNINIT_DEFAULT_PATTERN 0x1337c0d3u

/* The pages a filled run touched by reading before any write, in order: where the firmware
 * read memory nothing had written. At most UAM_UNINIT_TOUCHES are kept. */
#define UAM_UNINIT_TOUCHES 16u
typedef struct uam_uninit_touch {
    uint64_t pc;       /* the instruction that read (for a device's DMA: the last one its
                        * processor ran) */
    uint64_t paddr;    /* the first byte read in the page: a physical address */
    unsigned cpu;
} uam_uninit_touch;

/* Turn it on, before the machine runs (and before a snapshot is loaded into it). */
uam_status uam_machine_uninit_enable(uam_machine *m, uint32_t pattern);
bool       uam_machine_uninit_enabled(const uam_machine *m);
uint32_t   uam_machine_uninit_pattern(const uam_machine *m);

/* Leave [addr, addr + len) (physical) out: memory the hardware zeroes, or that the
 * firmware reads on purpose before writing. */
void       uam_machine_uninit_exclude(uam_machine *m, uint64_t addr, uint64_t len);

/* Call once the boot is over, before the begin snapshot is taken: every page written so
 * far counts as written. */
void       uam_machine_uninit_boot_done(uam_machine *m);

/* A filled run: arm after restoring the snapshot, disarm before restoring it again. */
uam_status uam_machine_uninit_arm(uam_machine *m);
void       uam_machine_uninit_disarm(uam_machine *m);
bool       uam_machine_uninit_armed(const uam_machine *m);

/* The reads-before-writes of the armed run so far; *total counts all of them, the array
 * keeps the first UAM_UNINIT_TOUCHES. */
const uam_uninit_touch *uam_machine_uninit_touches(const uam_machine *m, size_t *n,
                                                   uint64_t *total);

/* The fill's bytes for [addr, addr + len): what an allocator puts in fresh memory while
 * the run is armed. */
void       uam_machine_uninit_bytes(const uam_machine *m, uint64_t addr, uint8_t *buf, size_t len);

#endif /* UAM_UNINIT_H */
