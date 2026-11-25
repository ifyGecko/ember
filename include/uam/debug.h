/*
 * uam/debug.h - A debugger's access to a machine: guest memory and registers read and
 * written without device side effects, as the gdb stub, uam-run --dump-mem and the
 * monitor see them.
 *
 * Memory: a virtual address is translated as `cpu` sees it (UAM_XLATE_DEBUG: no TLB
 * fill, no fault); a physical one goes to the bus directly. Device regions answer only
 * through their side-effect-free read (uam_device_ops.peek); a write goes through the
 * bus as a debug access.
 *
 * Registers: the ISA's debugger view (uam_isa.gdb), by number - names as
 * uam_isa_reg_by_name / uam_isa_reg_names give them. A register of up to 8 bytes is a
 * value; a wider one (an x87 register's 10) is bytes in target order.
 *
 * Dependencies: machine.h.
 */
#ifndef UAM_DEBUG_H
#define UAM_DEBUG_H

#include "uam/machine.h"

#define UAM_DEBUG_REG_MAX 16u   /* bytes in the widest register a view may have */

/* Virtual to physical as `cpu` sees it (identity on an ISA without translation). */
bool     uam_debug_vtop(uam_cpu *cpu, uint64_t va, uint64_t *pa);
/* Read / write len bytes at addr (virtual through `cpu`, or physical). Returns how many
 * leading bytes were done; if not all, `why` (may be NULL) says what stopped it. */
uint64_t uam_debug_read(uam_machine *m, uam_cpu *cpu, uint64_t addr, bool phys, void *buf,
                        uint64_t len, char *why, size_t why_len);
uint64_t uam_debug_write(uam_machine *m, uam_cpu *cpu, uint64_t addr, bool phys,
                         const void *buf, uint64_t len, char *why, size_t why_len);

/* A memory range as written on a command line: [p:]ADDR+LEN[=FILE], LEN with an
 * optional K/M/G suffix (at most 1 GiB). `file` points into the parsed text. */
typedef struct uam_debug_range {
    uint64_t    addr, len;
    bool        phys;
    const char *file;
} uam_debug_range;
bool     uam_debug_parse_range(const char *text, uam_debug_range *r);
/* [p:]ADDR alone. */
bool     uam_debug_parse_addr(const char *text, uint64_t *addr, bool *phys);

/* Registers of the debugger view: how many, one's size in bytes (0: none such), and
 * access. read: `*val` for a register of up to 8 bytes, else `buf` (UAM_DEBUG_REG_MAX
 * bytes) in target order. False: the ISA has no such register or cannot access it. */
unsigned uam_debug_reg_count(const uam_cpu *cpu);
unsigned uam_debug_reg_size(const uam_cpu *cpu, unsigned regno);
bool     uam_debug_reg_read(uam_cpu *cpu, unsigned regno, uint64_t *val, uint8_t *buf);
bool     uam_debug_reg_write(uam_cpu *cpu, unsigned regno, uint64_t val, const uint8_t *buf);

#endif /* UAM_DEBUG_H */
