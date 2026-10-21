/*
 * uam/boards.h - The stock boards shipped with UAM.
 *
 * Kept out of the core library: a program that only needs its own board does
 * not link these (or their ISAs and devices). uam-run registers them all.
 */
#ifndef UAM_BOARDS_H
#define UAM_BOARDS_H

#include "uam/machine.h"

extern const uam_board uam_board_rv32_ref;    /* "rv32-ref": RV32IMAC */
extern const uam_board uam_board_mips_ref;    /* "mips-ref": MIPS32 big-endian */
extern const uam_board uam_board_mipsel_ref;  /* "mipsel-ref": MIPS32 little-endian */
extern const uam_board uam_board_arm_ref;     /* "arm-ref": ARMv7-A (ARM + Thumb-2) */

/* Register every stock board with uam_board_register(). */
void uam_boards_register_stock(void);

/* ---- helpers for any board (stock or out of tree) ----------------------- */

#include "uam/dev/mmio_stub.h"

#define UAM_MAX_MMIO_RULES 512

/*
 * Answer unmodelled registers with per-register rules (uam/dev/mmio_stub.h): one
 * stub device per rule, added to the machine and mapped over the rule's range at
 * UAM_PRIO_STUB + i in the CPU's byte order, so later rules win where rules
 * overlap, every rule wins over catch-all windows and unmapped holes, and every
 * real device wins over the rules. Devices are named "rule@0xADDR" ("#n" on
 * duplicates). `in` feeds INPUT rules (required if any, and must outlive the
 * machine); when given it is also registered as the extra state "mmio-input", so
 * snapshots carry its position. Call after the board is built, with the same rules
 * when restoring a snapshot taken with them. At most UAM_MAX_MMIO_RULES rules.
 */
uam_status uam_board_add_mmio_rules(uam_machine *m, const uam_mmio_rule *rules, size_t n,
                                    uam_mmio_input *in);

/*
 * Publish a NS16550A's receive FIFO as the machine input `name` (machine.h): the
 * bytes are queued as if received on the line; what does not fit the FIFO's free
 * space is dropped. The stock boards publish "uart0".
 */
uam_status uam_board_add_uart_input(uam_machine *m, const char *name, uam_device *uart);

#endif /* UAM_BOARDS_H */
