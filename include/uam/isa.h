/*
 * uam/isa.h - The ISA module interface.
 *
 * An ISA module turns guest machine code into UAM-IR and supplies the
 * architecture's privileged semantics. It is the only place that knows the
 * guest instruction set. It never sees devices or the bus directly:
 *   - code is fetched through uam_lift_ctx.fetch
 *   - interrupts arrive through irq_input (wired by the board)
 *   - faults and traps arrive through take_exception
 *
 * Dependencies: ir.h, cpu.h.
 */
#ifndef UAM_ISA_H
#define UAM_ISA_H

#include <stdio.h>

#include "uam/cpu.h"
#include "uam/ir.h"

/*
 * Debugger view of an ISA's registers (gdb remote protocol). Registers are
 * numbered as in `target_xml`; the 'g' packet carries registers 0..n_regs-1,
 * each reg_bytes wide, in the ISA's default byte order.
 */
typedef struct uam_gdb_arch {
    const char *target_xml;   /* complete target description */
    unsigned    n_regs;
    unsigned    reg_bytes;
    /* Side-effect-free access by gdb register number; false if unknown. */
    bool (*read_reg)(uam_cpu *cpu, unsigned regno, uint64_t *val);
    bool (*write_reg)(uam_cpu *cpu, unsigned regno, uint64_t val);
} uam_gdb_arch;

/* What went wrong, delivered to take_exception (docs/uam-isa.md section 9). */
typedef enum uam_exc_kind {
    UAM_EXC_TRAP,             /* TRAP EXCEPTION op: cause/tval are ISA-defined */
    UAM_EXC_FETCH_FAULT,      /* instruction fetch: bus fault at tval (physical side) */
    UAM_EXC_LOAD_FAULT,       /* load: bus fault */
    UAM_EXC_STORE_FAULT,      /* store (also AMO, CAS, store-conditional): bus fault */
    UAM_EXC_LOAD_MISALIGNED,
    UAM_EXC_STORE_MISALIGNED,
    UAM_EXC_SYSREG,           /* SYSRD/SYSWR rejected by sysreg handler */
    UAM_EXC_HELPER,           /* helper reported failure */
    UAM_EXC_FETCH_XLATE,      /* instruction fetch: isa->translate failed for tval */
    UAM_EXC_LOAD_XLATE,       /* load: translation failed (TLB miss, protection, ...) */
    UAM_EXC_STORE_XLATE,      /* store/AMO/CAS/SC: translation failed */
} uam_exc_kind;

/* uam_exception.flags */
enum uam_exc_flag {
    /* The faulting instruction is a continuation (UAM_IMARK_CONT) of the
     * instruction at parent_pc - e.g. it sits in a branch delay slot. ISAs with
     * delay slots restart at parent_pc (MIPS: EPC = branch, Cause.BD = 1). */
    UAM_EXC_F_CONT = 1u << 0,
};

typedef struct uam_exception {
    uam_exc_kind kind;
    unsigned     flags;     /* enum uam_exc_flag */
    uint64_t     pc;        /* pc of the faulting/trapping instruction */
    uint64_t     parent_pc; /* pc of the instruction `pc` continues; == pc if not CONT */
    uint64_t     cause;     /* UAM_EXC_TRAP: ISA cause. *_XLATE: uam_xlate.fault. */
    uint64_t     tval;      /* faulting (virtual) address or ISA value */
} uam_exception;

/* Which kind of access an isa->translate call is resolving. DEBUG is a
 * side-effect-free lookup for loaders and debuggers: it must not change ISA
 * state and ignores the current privilege level. */
typedef enum uam_xlate_kind {
    UAM_XLATE_LOAD, UAM_XLATE_STORE, UAM_XLATE_FETCH, UAM_XLATE_DEBUG,
} uam_xlate_kind;

/* Result of isa->translate. */
typedef struct uam_xlate {
    uint64_t paddr;
    /* On UAM_ERR_XLATE: an ISA-defined fault sub-cause (e.g. "no TLB entry"
     * vs "entry invalid" vs "not writable"). The core never interprets it; it
     * is carried to take_exception in uam_exception.cause. */
    uint32_t fault;
    /* The mapping (and permission) for this page can never change at run time,
     * e.g. MIPS kseg0/kseg1. Code translated through a non-fixed mapping is
     * dropped when the ISA calls uam_cpu_xlate_changed(). */
    bool     fixed;
} uam_xlate;

/* Context handed to lift(): where to emit, and how to read code. */
typedef struct uam_lift_ctx {
    uam_cpu     *cpu;
    uam_builder *b;
    uint64_t     pc;          /* first guest instruction of the block */
    uint32_t     max_insns;   /* block length limit */
    /* Read `size` bytes of code. Records the page in the block. Returns
     * UAM_ERR_FAULT if not fetchable (bus or translation fault; the executor
     * raises the precise fetch exception when the block's first instruction
     * is affected), UAM_ERR_INVALID if the block may not grow onto another
     * page. In both cases end the block before this instruction. */
    uam_status (*fetch)(struct uam_lift_ctx *lc, uint64_t addr, unsigned size, uint64_t *val);
    void        *core;        /* private to the core */
} uam_lift_ctx;

typedef struct uam_isa {
    const char             *name;
    unsigned                addr_width;    /* 32 or 64 */
    unsigned                default_endian;/* UAM_ACC_LE or UAM_ACC_BE: byte order of instruction fetch */
    size_t                  regfile_size;  /* bytes */
    const uam_regdesc      *regs;
    size_t                  n_regs;
    uam_helper_table        helpers;

    uam_status (*init)(uam_cpu *cpu);              /* allocate isa_state */
    void       (*fini)(uam_cpu *cpu);
    void       (*reset)(uam_cpu *cpu, uint64_t entry);

    /* Lift one block starting at lc->pc. Must emit at least one instruction
     * (or return UAM_ERR_FAULT if the first fetch fails). */
    uam_status (*lift)(uam_lift_ctx *lc);

    /* Enter the guest exception handler for a synchronous event. */
    void       (*take_exception)(uam_cpu *cpu, const uam_exception *exc);
    /* Between blocks: if an enabled interrupt is pending, enter it; return true if taken. */
    bool       (*irq_take)(uam_cpu *cpu);
    /* WAIT wake condition (pending & enabled, regardless of global enable). */
    bool       (*irq_wakeup)(uam_cpu *cpu);
    /* An interrupt input line changed level. */
    void       (*irq_input)(uam_cpu *cpu, unsigned line, int level);

    uam_status (*sysreg_read)(uam_cpu *cpu, uint32_t id, uint64_t *val);
    uam_status (*sysreg_write)(uam_cpu *cpu, uint32_t id, uint64_t val);

    /* Optional virtual-to-physical address translation. NULL means the guest
     * uses physical addresses directly (e.g. RISC-V M-mode). Architectures
     * with fixed segments or a TLB (e.g. MIPS kseg/TLB) set this; the executor
     * calls it for every fetch, load and store (except UAM_ACC_PHYS accesses).
     * On failure it returns UAM_ERR_XLATE with out->fault set, and the access
     * raises UAM_EXC_{FETCH,LOAD,STORE}_XLATE with the virtual address as tval.
     * It must not change ISA state; exception side effects (BadVAddr, ...)
     * belong in take_exception. Whenever a mapping may change, the ISA calls
     * uam_cpu_xlate_changed(cpu); cpu->priv must reflect the privilege the
     * translation depends on (translated code is tagged with it). */
    uam_status (*translate)(uam_cpu *cpu, uint64_t vaddr, uam_xlate_kind kind, uam_xlate *out);

    /* Optional: debugger register access (gdb remote protocol, see gdbstub.h). */
    const uam_gdb_arch *gdb;

    void       (*dump_state)(uam_cpu *cpu, FILE *out);
} uam_isa;

#endif /* UAM_ISA_H */
