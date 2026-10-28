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
 * excludes read-only pages, and holds pages with translated code only under a tag the
 * plain probe cannot match: the JIT checks their code lines first). Flushed whenever a mapping or translation
 * changes (uam_cpu_dtlb_flush), and by the JIT whenever cpu->priv differs from the
 * level its entries were filled at; a miss falls back to the bus. */
#define UAM_DTLB_BITS 10u
#define UAM_DTLB_SIZE (1u << UAM_DTLB_BITS)
#define UAM_DTLB_NONE UINT64_MAX   /* a tag no guest page number can equal */

/* The most earlier B values UAM_OP_COV folds into an index (an N-gram of N = 8). */
#define UAM_COV_HIST_MAX 7u

/*
 * The shape of the coverage history: how many earlier B values fold into each index,
 * and how each is rotated first. All zero is the classic one-predecessor edge
 * (AFL-style). rot[j] rotates hist[j] left within the map's width (0: no rotation);
 * it must be below log2(map size).
 */
typedef struct uam_cov_history {
    uint32_t depth;                   /* 1..UAM_COV_HIST_MAX; 0 means 1 */
    uint8_t  rot[UAM_COV_HIST_MAX];
} uam_cov_history;

/*
 * Edge-coverage state read and written by UAM_OP_COV (docs/uam-isa.md section 7.5;
 * uam_cov_count below is the reference):
 *     idx = A;  for j < depth: idx ^= rot[j] ? rotl(hist[j], rot[j]) : hist[j]
 *     idx &= mask;  if (map[idx] != 0xff) map[idx]++
 *     hist[depth-1..1] = hist[depth-2..0];  hist[0] = B
 * with rotl(v, r) = ((v << r) | (v >> (bits - r))) & mask in 32-bit arithmetic.
 * `map` has mask + 1 bytes (a power of two). depth 0 means 1, so { .map, .mask } alone
 * is the classic edge (`prev` is hist[0]). Set the history with uam_cov_state_init;
 * uam_exec_set_coverage checks it. mask, depth, bits and rot are fixed while coverage
 * is on (the JIT compiles the shape in): to change them, call uam_exec_set_coverage
 * again. The state belongs to whoever enabled coverage (exec.h); it is never part of a
 * snapshot.
 *
 * skip_repeat (also part of the shape) counts a site only when it is not the site
 * that counted last: back-to-back executions of one site, such as a loop with no
 * other site in it, count once, and no edge from a site to itself is ever made. This
 * is the emit stream of a trap-and-rearm coverage runtime (one that disarms a site
 * when it fires and re-arms the one before it). last_pc is that site's pc
 * (UAM_COV_NO_PC: none yet); it is not history and uam_cov_reset_prev keeps it.
 */
#define UAM_COV_NO_PC UINT64_MAX

typedef struct uam_cov_state {
    uint8_t  *map;
    uint32_t  mask;
    uint32_t  depth;                  /* 0..UAM_COV_HIST_MAX; 0 means 1 */
    uint32_t  bits;                   /* log2(mask + 1); used by rotation */
    uint8_t   rot[UAM_COV_HIST_MAX];
    union {
        uint32_t prev;                /* hist[0], the newest */
        uint32_t hist[UAM_COV_HIST_MAX];
    };
    bool      skip_repeat;
    uint64_t  last_pc;                /* with skip_repeat: the site that counted last */
} uam_cov_state;

/* A state for a map of `map_size` bytes (a power of two) and the history shape `h`
 * (NULL: the classic one-predecessor edge). */
static inline void uam_cov_state_init(uam_cov_state *s, uint8_t *map, uint32_t map_size,
                                      const uam_cov_history *h)
{
    *s = (uam_cov_state){ .map = map, .mask = map_size - 1, .last_pc = UAM_COV_NO_PC };
    while (s->bits < 32 && (UINT64_C(1) << s->bits) < map_size)
        s->bits++;
    if (h) {
        s->depth = h->depth;
        for (unsigned j = 0; j < UAM_COV_HIST_MAX; j++)
            s->rot[j] = h->rot[j];
    }
}

/* Forget the history, e.g. at the start of a fuzz iteration. */
static inline void uam_cov_reset_prev(uam_cov_state *s)
{
    for (unsigned j = 0; j < UAM_COV_HIST_MAX; j++)
        s->hist[j] = 0;
}

/* One UAM_OP_COV A, B at guest pc `pc`: the reference the interpreter runs and the JIT
 * matches. */
static inline void uam_cov_count(uam_cov_state *s, uint64_t pc, uint32_t a, uint32_t b)
{
    if (s->skip_repeat) {
        if (s->last_pc == pc)
            return;
        s->last_pc = pc;
    }
    const uint32_t d = s->depth ? s->depth : 1;
    uint32_t idx = a;
    for (uint32_t j = 0; j < d; j++) {
        uint32_t v = s->hist[j], r = s->rot[j];
        idx ^= r ? ((v << r) | (v >> (s->bits - r))) & s->mask : v;
    }
    idx &= s->mask;
    if (s->map[idx] != 0xff)
        s->map[idx]++;
    for (uint32_t j = d - 1; j > 0; j--)
        s->hist[j] = s->hist[j - 1];
    s->hist[0] = b;
}

typedef struct uam_cpu {
    uint64_t              pc;
    uint8_t              *regfile;
    size_t                regfile_size;
    uint64_t              icount;          /* instructions issued (IMARKs executed) */
    uint64_t              icount_deadline; /* stop at an IMARK once icount reaches this */
    bool                  exit_request;    /* stop at the next IMARK (section 9) */
    /* For native code: exit_request ? 0 : icount_deadline, so one compare per IMARK
     * covers both. The JIT sets it on entry; uam_cpu_request_exit zeroes it. */
    uint64_t              icount_stop;
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
    uam_cov_state        *cov;      /* UAM_OP_COV target; NULL while coverage is off */
    /* JIT data-TLB (see above). tag = guest vpage, base = host_page - vpage*4096. */
    uint64_t              dtlb_rt[UAM_DTLB_SIZE], dtlb_rb[UAM_DTLB_SIZE];
    uint64_t              dtlb_wt[UAM_DTLB_SIZE], dtlb_wb[UAM_DTLB_SIZE];
} uam_cpu;

static inline void uam_cpu_dtlb_flush(uam_cpu *cpu)
{
    for (unsigned i = 0; i < UAM_DTLB_SIZE; i++)
        cpu->dtlb_rt[i] = cpu->dtlb_wt[i] = UAM_DTLB_NONE;
}

/* Ask the hart to leave its current block at the next instruction boundary
 * (and stop the JIT from chaining into another block). The run loop clears it
 * before every block it dispatches. Call it on anything the run loop must act
 * on between blocks: in particular every change that can make isa->irq_take()
 * newly succeed (isa.h). */
static inline void uam_cpu_request_exit(uam_cpu *cpu)
{
    cpu->exit_request = true;
    cpu->icount_stop = 0;   /* native code compares icount with it at every instruction */
}

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
