/*
 * uam/cpu.h - Hart state owned by the core.
 *
 * `uam_cpu` is the ISA-neutral part of a hardware thread (docs/uam-isa.md
 * section 2): pc, register file, instruction counter, exclusive reservation,
 * exit request. ISA-specific state (CSRs, privilege details) lives behind
 * `isa_state`, owned by the ISA module.
 *
 * The functions declared here are the core services an ISA module may call.
 *
 * Dependencies: common.h, bus.h (forward-declared only).
 */
#ifndef UAM_CPU_H
#define UAM_CPU_H

#include "uam/common.h"

struct uam_bus;
struct uam_isa;
struct uam_sched;

typedef struct uam_reservation {
    bool     valid;
    uint64_t addr;
    unsigned size;   /* bytes */
} uam_reservation;

/* Data-TLB for the JIT's inline load/store fast path: guest virtual page -> host
 * address of RAM/ROM backing it. Read and write sets are separate (the write set
 * excludes read-only and code pages). Flushed whenever a mapping or translation
 * changes (uam_cpu_dtlb_flush); a miss falls back to the bus. */
#define UAM_DTLB_BITS 10u
#define UAM_DTLB_SIZE (1u << UAM_DTLB_BITS)
#define UAM_DTLB_NONE UINT64_MAX   /* a tag no guest page number can equal */

typedef struct uam_cpu {
    uint64_t              pc;
    uint8_t              *regfile;
    size_t                regfile_size;
    uint64_t              icount;          /* instructions issued (IMARKs executed) */
    uint64_t              icount_deadline; /* stop at an IMARK once icount reaches this */
    bool                  exit_request;    /* stop at the next IMARK (section 9) */
    bool                  waiting;         /* idle in WAIT until isa->irq_wakeup() */
    unsigned              priv;            /* current privilege level (ISA-defined); translated
                                            * code is only reused at the same level */
    uint32_t              xlate_gen;       /* bumped by uam_cpu_xlate_changed() */
    unsigned              hartid;
    uam_reservation       resv;
    struct uam_bus       *bus;
    struct uam_sched     *sched;    /* virtual time; used by CPU-internal timers (e.g. MIPS CP0) */
    const struct uam_isa *isa;
    void                 *isa_state;
    /* JIT data-TLB (see above). tag = guest vpage, base = host_page - vpage*4096. */
    uint64_t              dtlb_rt[UAM_DTLB_SIZE], dtlb_rb[UAM_DTLB_SIZE];
    uint64_t              dtlb_wt[UAM_DTLB_SIZE], dtlb_wb[UAM_DTLB_SIZE];
} uam_cpu;

static inline void uam_cpu_dtlb_flush(uam_cpu *cpu)
{
    for (unsigned i = 0; i < UAM_DTLB_SIZE; i++)
        cpu->dtlb_rt[i] = cpu->dtlb_wt[i] = UAM_DTLB_NONE;
}

/* Ask the hart to leave its current block at the next instruction boundary.
 * The deadline is cleared too, so executors can test a single condition
 * (icount >= icount_deadline) at instruction boundaries; the run loop
 * recomputes the deadline before every block. */
static inline void uam_cpu_request_exit(uam_cpu *cpu) { cpu->exit_request = true; }

/* The ISA's virtual-to-physical mapping changed (e.g. a TLB write or an
 * address-space-id switch). Code translated through non-fixed mappings
 * (uam_xlate.fixed == false) is discarded before it runs again. */
static inline void uam_cpu_xlate_changed(uam_cpu *cpu)
{
    cpu->xlate_gen++;
    uam_cpu_dtlb_flush(cpu);
    uam_cpu_request_exit(cpu);
}

/* Signal sink that forwards an interrupt line into isa->irq_input().
 * Connect device lines with: uam_signal_connect(&sig, uam_cpu_irq_sink, cpu, line). */
void uam_cpu_irq_sink(void *cpu, unsigned line, int level);

/* Side-effect-free read of guest memory (instruction bits for tval, debugger). */
uam_status uam_cpu_peek(uam_cpu *cpu, uint64_t addr, unsigned size, uint64_t *val);

/* Register-file accessors for ISA modules and tools. */
static inline uint64_t uam_cpu_reg_read(const uam_cpu *cpu, uint32_t off, unsigned width)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < width / 8; i++)
        v |= (uint64_t)cpu->regfile[off + i] << (8 * i);
    return v;
}

static inline void uam_cpu_reg_write(uam_cpu *cpu, uint32_t off, unsigned width, uint64_t v)
{
    for (unsigned i = 0; i < width / 8; i++)
        cpu->regfile[off + i] = (uint8_t)(v >> (8 * i));
}

#endif /* UAM_CPU_H */
