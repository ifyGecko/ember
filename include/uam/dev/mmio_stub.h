/*
 * uam/dev/mmio_stub.h - Per-register stand-ins for unmodelled peripheral registers.
 *
 * A stub answers one address range with one policy. It is how a bring-up gets
 * past a register the board does not model yet ("this status register must read
 * 1"), and how fuzz input reaches the firmware through MMIO reads.
 *
 * Policies:
 *   ZERO   reads return 0, writes are dropped
 *   ECHO   reads return the last value written to that register (else 0)
 *   FIXED  reads return `value`, writes are dropped
 *   FAULT  every access is a bus error (the guest takes a bus-error exception)
 *   INPUT  reads take the next `width` bytes of an input stream (uam_mmio_input),
 *          first byte least significant; 0 once the stream is exhausted
 * and for every policy except FAULT a read result is then adjusted:
 *          (v & ~clear) | set
 * so an ECHO register can keep a "done" bit set, say.
 *
 * Registers are `width` bytes wide (default 4) and aligned to it within the
 * range: a narrower access reads or writes part of one register, as on real
 * hardware. Values are register values as the CPU sees them: map a stub with
 * the CPU's byte order (uam_board_add_mmio_rules does) so `fixed:0x1` reads
 * as 1. The first access to each register is logged (UAM_LOG_INFO, `-v`), and
 * a summary of each stub's accesses when it is destroyed.
 *
 * Stubs are meant to be mapped at UAM_PRIO_STUB and above: over catch-all
 * windows (devices/unimp.c, UAM_PRIO_BACKGROUND) and unmapped holes, under
 * every real device. uam_board_add_mmio_rules (uam/boards.h) maps a list of
 * rules that way, later rules winning where they overlap.
 *
 * Snapshots: a stub saves its echo values and access log; the input stream's
 * position is saved separately (uam_mmio_input_save, as extra machine state).
 */
#ifndef UAM_DEV_MMIO_STUB_H
#define UAM_DEV_MMIO_STUB_H

#include "uam/device.h"
#include "uam/state.h"

typedef enum uam_mmio_policy {
    UAM_MMIO_ZERO,
    UAM_MMIO_ECHO,
    UAM_MMIO_FIXED,
    UAM_MMIO_FAULT,
    UAM_MMIO_INPUT,
} uam_mmio_policy;

typedef struct uam_mmio_rule {
    uint64_t        addr, size;   /* the range; size is a multiple of width */
    unsigned        width;        /* register width in bytes: 1, 2, 4 or 8 (0 = 4) */
    uam_mmio_policy policy;
    uint64_t        value;        /* FIXED: the value read */
    uint64_t        set, clear;   /* applied to every read result (not FAULT) */
} uam_mmio_rule;

/*
 * Parse a rule from text:
 *
 *     ADDR[+SIZE]=POLICY[,width=N][,set=MASK][,clear=MASK]
 *     POLICY = zero | echo | fixed:VALUE | fault | input
 *
 * SIZE defaults to the register width. Numbers are C literals (0x.., decimal).
 * Examples: "0x18040000=fixed:0x1", "0x18020000+0x100=echo,set=0x60",
 * "0xb8000000+0x1000=fault", "0x18060010=input,width=1". On failure returns
 * UAM_ERR_INVALID and describes the problem in err.
 */
uam_status  uam_mmio_rule_parse(const char *spec, uam_mmio_rule *out, char *err, size_t errlen);
/* Checks a rule built in code (width, size, alignment, wrap-around). */
uam_status  uam_mmio_rule_check(const uam_mmio_rule *rule, char *err, size_t errlen);
/* "fixed 0x1", "echo set=0x60", ... for logs. */
void        uam_mmio_rule_describe(const uam_mmio_rule *rule, char *buf, size_t len);

/* ---- input stream (INPUT policy) ---------------------------------------- */

typedef struct uam_mmio_input uam_mmio_input;

uam_mmio_input *uam_mmio_input_create(void);
void            uam_mmio_input_free(uam_mmio_input *in);
/* Use `data` (not copied: it must stay valid while the machine runs) and start
 * from its first byte. Call it after restoring a snapshot to feed a new input. */
void            uam_mmio_input_set(uam_mmio_input *in, const void *data, size_t len);
size_t          uam_mmio_input_pos(const uam_mmio_input *in);   /* bytes consumed */
bool            uam_mmio_input_exhausted(const uam_mmio_input *in);
/* Called once, by the read that first finds the stream exhausted (a fuzz harness
 * stops the run here; that read still returns 0). */
void            uam_mmio_input_on_exhausted(uam_mmio_input *in, void (*fn)(void *ctx), void *ctx);
/* Extra machine state (uam_machine_add_state(m, "mmio-input", save, restore, in)):
 * the position in the stream. */
void            uam_mmio_input_save(void *in, uam_state_writer *w);
void            uam_mmio_input_restore(void *in, uam_state_reader *r);

/* ---- the stub device ---------------------------------------------------- */

/* A stub for `rule` (copied; checked with uam_mmio_rule_check), named `name`
 * (copied, up to 47 characters). Map it over
 * exactly [rule->addr, rule->addr + rule->size). `in` is required for INPUT and
 * must outlive the device. NULL on an invalid rule or out of memory. */
uam_device *uam_mmio_stub_create(const char *name, const uam_mmio_rule *rule, uam_mmio_input *in);
const uam_mmio_rule *uam_mmio_stub_rule(uam_device *dev);
uint64_t    uam_mmio_stub_reads(uam_device *dev);
uint64_t    uam_mmio_stub_writes(uam_device *dev);

#endif /* UAM_DEV_MMIO_STUB_H */
