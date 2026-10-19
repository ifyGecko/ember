/*
 * uam/ir.h - UAM-IR: the Universal Abstract Machine instruction set.
 *
 * This header is the single contract between frontends (ISA modules that
 * lift guest code) and backends (interpreter, future JIT, analysis passes).
 * The normative specification is docs/uam-isa.md; section numbers in the
 * comments below refer to it.
 *
 * Contents:
 *   - opcode table (one X-macro, the only list of opcodes in the code base)
 *   - operand references, instruction and block representation
 *   - register descriptors and helper descriptors
 *   - block builder API (used by lifters and passes)
 *   - verifier and printer
 *
 * Dependencies: common.h, access.h. Nothing else: the IR knows nothing about
 * CPUs, buses, or devices.
 */
#ifndef UAM_IR_H
#define UAM_IR_H

#include <stdio.h>

#include "uam/access.h"
#include "uam/common.h"

/* ------------------------------------------------------------------------ */
/* Opcode table (section 5)                                                  */
/* ------------------------------------------------------------------------ */

/* Result kind of an opcode. */
enum uam_result_kind {
    UAM_RK_NONE,    /* no destination */
    UAM_RK_W,       /* destination has the operating width W */
    UAM_RK_BOOL,    /* destination is 1 bit */
    UAM_RK_SPECIAL, /* depends on flags/descriptor: ST.X -> bool, HELPER -> descriptor */
};

/* Opcode properties (bitmask). */
enum uam_op_prop {
    UAM_P_GUARD  = 1u << 0, /* accepts a guard operand (section 4.1) */
    UAM_P_FAULT  = 1u << 1, /* may fault; constrained by the commit rule (section 8.5) */
    UAM_P_EFFECT = 1u << 2, /* has side effects; never removed or reordered freely */
    UAM_P_TERM   = 1u << 3, /* terminates the block (EXIT only when unguarded) */
};

/*
 * X(name, mnemonic, n_src, result_kind, props)
 *   n_src: number of value operands (a, b, c) the op consumes.
 *
 * New opcodes go at the end, so existing values keep their numbers.
 */
#define UAM_OPCODES(X)                                                          \
    /* instruction marker */                                                    \
    X(IMARK,   "imark",   1, UAM_RK_NONE, UAM_P_EFFECT)                         \
    /* register file */                                                         \
    X(GET,     "get",     0, UAM_RK_W,    0)                                    \
    X(PUT,     "put",     1, UAM_RK_NONE, UAM_P_GUARD | UAM_P_EFFECT)           \
    /* memory */                                                                \
    X(LD,      "ld",      1, UAM_RK_W,    UAM_P_GUARD | UAM_P_FAULT | UAM_P_EFFECT) \
    X(ST,      "st",      2, UAM_RK_SPECIAL, UAM_P_GUARD | UAM_P_FAULT | UAM_P_EFFECT) \
    X(AMO,     "amo",     2, UAM_RK_W,    UAM_P_GUARD | UAM_P_FAULT | UAM_P_EFFECT) \
    X(CAS,     "cas",     3, UAM_RK_W,    UAM_P_GUARD | UAM_P_FAULT | UAM_P_EFFECT) \
    X(FENCE,   "fence",   0, UAM_RK_NONE, UAM_P_EFFECT)                         \
    /* arithmetic */                                                            \
    X(ADD,     "add",     2, UAM_RK_W,    0)                                    \
    X(SUB,     "sub",     2, UAM_RK_W,    0)                                    \
    X(MUL,     "mul",     2, UAM_RK_W,    0)                                    \
    X(MULHU,   "mulhu",   2, UAM_RK_W,    0)                                    \
    X(MULHS,   "mulhs",   2, UAM_RK_W,    0)                                    \
    X(UDIV,    "udiv",    2, UAM_RK_W,    0)                                    \
    X(SDIV,    "sdiv",    2, UAM_RK_W,    0)                                    \
    X(UREM,    "urem",    2, UAM_RK_W,    0)                                    \
    X(SREM,    "srem",    2, UAM_RK_W,    0)                                    \
    /* logic */                                                                 \
    X(AND,     "and",     2, UAM_RK_W,    0)                                    \
    X(OR,      "or",      2, UAM_RK_W,    0)                                    \
    X(XOR,     "xor",     2, UAM_RK_W,    0)                                    \
    /* shift / rotate */                                                        \
    X(SHL,     "shl",     2, UAM_RK_W,    0)                                    \
    X(LSHR,    "lshr",    2, UAM_RK_W,    0)                                    \
    X(ASHR,    "ashr",    2, UAM_RK_W,    0)                                    \
    X(ROTR,    "rotr",    2, UAM_RK_W,    0)                                    \
    /* compare */                                                               \
    X(EQ,      "eq",      2, UAM_RK_BOOL, 0)                                    \
    X(NE,      "ne",      2, UAM_RK_BOOL, 0)                                    \
    X(ULT,     "ult",     2, UAM_RK_BOOL, 0)                                    \
    X(ULE,     "ule",     2, UAM_RK_BOOL, 0)                                    \
    X(SLT,     "slt",     2, UAM_RK_BOOL, 0)                                    \
    X(SLE,     "sle",     2, UAM_RK_BOOL, 0)                                    \
    /* carry / overflow */                                                      \
    X(CARRY,   "carry",   2, UAM_RK_BOOL, 0)                                    \
    X(SCARRY,  "scarry",  2, UAM_RK_BOOL, 0)                                    \
    X(SBORROW, "sborrow", 2, UAM_RK_BOOL, 0)                                    \
    /* bit manipulation */                                                      \
    X(CLZ,     "clz",     1, UAM_RK_W,    0)                                    \
    X(CTZ,     "ctz",     1, UAM_RK_W,    0)                                    \
    X(POPCNT,  "popcnt",  1, UAM_RK_W,    0)                                    \
    X(BSWAP,   "bswap",   1, UAM_RK_W,    0)                                    \
    /* conversion */                                                            \
    X(ZEXT,    "zext",    1, UAM_RK_W,    0)                                    \
    X(SEXT,    "sext",    1, UAM_RK_W,    0)                                    \
    X(TRUNC,   "trunc",   1, UAM_RK_W,    0)                                    \
    /* select */                                                                \
    X(SELECT,  "select",  3, UAM_RK_W,    0)                                    \
    /* control */                                                               \
    X(EXIT,    "exit",    1, UAM_RK_NONE, UAM_P_GUARD | UAM_P_EFFECT | UAM_P_TERM) \
    /* system / escape */                                                       \
    X(SYSRD,   "sysrd",   0, UAM_RK_W,    UAM_P_GUARD | UAM_P_FAULT | UAM_P_EFFECT) \
    X(SYSWR,   "syswr",   1, UAM_RK_NONE, UAM_P_GUARD | UAM_P_FAULT | UAM_P_EFFECT) \
    X(TRAP,    "trap",    2, UAM_RK_NONE, UAM_P_GUARD | UAM_P_FAULT | UAM_P_EFFECT | UAM_P_TERM) \
    X(WAIT,    "wait",    1, UAM_RK_NONE, UAM_P_EFFECT | UAM_P_TERM)            \
    X(HELPER,  "helper",  3, UAM_RK_SPECIAL, UAM_P_GUARD | UAM_P_EFFECT)      \
    /* instrumentation (inserted by passes, never by lifters) */                \
    X(COV,     "cov",     2, UAM_RK_NONE, UAM_P_EFFECT)

typedef enum uam_opcode {
#define UAM_X_ENUM(name, mn, nsrc, rk, props) UAM_OP_##name,
    UAM_OPCODES(UAM_X_ENUM)
#undef UAM_X_ENUM
    UAM_OP__COUNT
} uam_opcode;

/* Static description of an opcode, indexed by uam_opcode. */
typedef struct uam_opinfo {
    const char *mnemonic;
    uint8_t n_src;
    uint8_t result;   /* enum uam_result_kind */
    uint8_t props;    /* enum uam_op_prop */
} uam_opinfo;

extern const uam_opinfo uam_opinfo_table[UAM_OP__COUNT];

/* ------------------------------------------------------------------------ */
/* Auxiliary-field enumerations                                              */
/* ------------------------------------------------------------------------ */

/* AMO sub-operations (section 6.2). */
typedef enum uam_amo_op {
    UAM_AMO_XCHG, UAM_AMO_ADD, UAM_AMO_AND, UAM_AMO_OR, UAM_AMO_XOR,
    UAM_AMO_SMIN, UAM_AMO_SMAX, UAM_AMO_UMIN, UAM_AMO_UMAX,
    UAM_AMO__COUNT
} uam_amo_op;

/* FENCE kinds (section 6.2). */
typedef enum uam_fence_kind {
    UAM_FENCE_ACQ, UAM_FENCE_REL, UAM_FENCE_FULL, UAM_FENCE_INSN,
} uam_fence_kind;

/* EXIT kinds: analysis hints only (section 7.3). */
typedef enum uam_exit_kind {
    UAM_EXIT_JUMP, UAM_EXIT_CALL, UAM_EXIT_RET, UAM_EXIT_EXC_RETURN,
} uam_exit_kind;

/*
 * IMARK flags (the `flags` field on an IMARK instruction).
 *
 * UAM_IMARK_CONT marks an instruction as a *continuation* of the previous one:
 * the executor must not take an asynchronous exit (interrupt / deadline) at
 * this boundary. It is how a frontend keeps an instruction group atomic with
 * respect to interrupts - chiefly a branch and the instruction in its delay
 * slot (MIPS, SPARC), which must retire together or the pending control
 * transfer would be lost. A CONT imark still ends the previous instruction for
 * fault precision and still advances icount.
 */
#define UAM_IMARK_CONT 0x1u

/*
 * UAM_IMARK_BREAK asks the executor to stop *before* this instruction, as if
 * an asynchronous exit were pending (it is ignored on the first instruction of
 * a block and on CONT boundaries). Lifters never set it: the executor sets it
 * on translated blocks to end them at instrumented addresses (PC hooks,
 * breakpoints), so that the hook runs at a block boundary with precise state.
 */
#define UAM_IMARK_BREAK 0x2u

/* TRAP classes (section 7.3). */
typedef enum uam_trap_class {
    UAM_TRAP_EXCEPTION, /* guest exception: isa->take_exception(cause, tval) */
    UAM_TRAP_DEBUG,     /* stop into the emulator's debugger */
    UAM_TRAP_HALT,      /* stop emulation; cause is the exit status */
} uam_trap_class;

/* ------------------------------------------------------------------------ */
/* Operands, instructions, blocks (sections 3, 4, appendix A)                */
/* ------------------------------------------------------------------------ */

/*
 * An operand reference: none, a temporary index, or an immediate (index into
 * the block's immediate pool, tagged with UAM_IMM_BIT).
 */
typedef uint32_t uam_ref;
#define UAM_REF_NONE  0xFFFFFFFFu
#define UAM_IMM_BIT   0x80000000u

static inline bool uam_ref_is_none(uam_ref r) { return r == UAM_REF_NONE; }
static inline bool uam_ref_is_imm(uam_ref r)  { return r != UAM_REF_NONE && (r & UAM_IMM_BIT); }
static inline bool uam_ref_is_temp(uam_ref r) { return !(r & UAM_IMM_BIT); }
static inline uint32_t uam_ref_index(uam_ref r) { return r & ~UAM_IMM_BIT; }

typedef struct uam_insn {
    uint8_t  op;      /* uam_opcode */
    uint8_t  width;   /* operating width W: 1, 8, 16, 32, 64 */
    uint16_t flags;   /* memory access attributes (access.h) */
    uint32_t aux;     /* opcode-specific: reg offset, sysreg/helper id, subop, kind, len */
    uam_ref  guard, dst, a, b, c;
} uam_insn;

typedef struct uam_block {
    uint64_t  guest_pc;     /* address of the first guest instruction */
    uint8_t   addr_width;   /* guest address width AW (32 or 64) */
    uint32_t  n_insn;
    uint32_t  n_temp;
    uint32_t  n_imm;
    uint32_t  n_guest;      /* number of guest instructions (IMARKs) */
    uam_insn *insn;
    uint8_t  *temp_width;   /* width of each temporary */
    uint64_t *imm;          /* immediate pool */
    uint32_t  n_pages;      /* code pages covered (section 8.6), at most 2 */
    uint64_t  page[2];
    uint64_t  page_gen[2];  /* bus page generations when translated */
    /* Opaque per-block state owned by an execution backend (e.g. the JIT's
     * compiled code or a "do not compile" marker). Freed with the block. */
    void     *backend;
    void    (*backend_free)(void *backend);
} uam_block;

void uam_block_free(uam_block *blk);

/* ------------------------------------------------------------------------ */
/* Register and helper descriptors (sections 3.2, 7.4)                       */
/* ------------------------------------------------------------------------ */

typedef enum uam_reg_class {
    UAM_REG_GPR, UAM_REG_FLAG, UAM_REG_SYS_SHADOW, UAM_REG_OTHER,
} uam_reg_class;

typedef struct uam_regdesc {
    const char *name;
    uint32_t offset;   /* byte offset in the register file */
    uint8_t  width;    /* bits */
    uint8_t  cls;      /* uam_reg_class */
} uam_regdesc;

enum uam_helper_effect {
    UAM_HELPER_PURE         = 0,
    UAM_HELPER_READS_STATE  = 1u << 0,
    UAM_HELPER_WRITES_STATE = 1u << 1,
    UAM_HELPER_MAY_FAULT    = 1u << 2,
};

/*
 * A helper implementation. `env` is the executing hart (opaque to the IR).
 * On failure the helper stores a non-OK status in *st and the instruction
 * faults precisely.
 */
typedef uint64_t (*uam_helper_fn)(void *env, uint64_t a, uint64_t b, uint64_t c,
                                  uam_status *st);

typedef struct uam_helper_desc {
    const char   *name;
    uint8_t       n_args;        /* 0..3 */
    uint8_t       arg_width[3];
    uint8_t       result_width;  /* 0 = no result */
    uint8_t       effect;        /* enum uam_helper_effect */
    uam_helper_fn fn;
} uam_helper_desc;

/* A table of helpers; HELPER's aux field indexes into it. */
typedef struct uam_helper_table {
    const uam_helper_desc *desc;
    uint32_t count;
} uam_helper_table;

/* ------------------------------------------------------------------------ */
/* Block builder                                                             */
/* ------------------------------------------------------------------------ */

/*
 * The builder accumulates one block. Usage:
 *
 *     uam_builder b;
 *     uam_builder_init(&b, 32);
 *     uam_builder_begin(&b, pc);
 *     uam_imark(&b, pc, 4);
 *     uam_ref x = uam_get(&b, 32, OFF_A0);
 *     uam_put(&b, 32, OFF_A1, uam_binop(&b, UAM_OP_ADD, 32, x, uam_imm(&b, 1)), UAM_REF_NONE);
 *     uam_exit(&b, UAM_REF_NONE, uam_imm(&b, pc + 4), UAM_EXIT_JUMP);
 *     uam_block *blk = uam_builder_finish(&b);
 *
 * The builder does not validate. Run uam_verify() on the finished block.
 */
typedef struct uam_builder {
    uint64_t  guest_pc;
    uint8_t   addr_width;
    uint32_t  n_guest;
    uam_insn *insn;  uint32_t n_insn, cap_insn;
    uint8_t  *tw;    uint32_t n_temp, cap_temp;
    uint64_t *imm;   uint32_t n_imm,  cap_imm;
    uint32_t  n_pages;
    uint64_t  page[2];
    bool      oom;   /* sticky allocation failure; finish() returns NULL */
} uam_builder;

void      uam_builder_init(uam_builder *b, unsigned addr_width);
void      uam_builder_free(uam_builder *b);
void      uam_builder_begin(uam_builder *b, uint64_t guest_pc);
uam_block *uam_builder_finish(uam_builder *b);

/* Record that the block's code lies on the page containing `page_addr`.
 * Returns false if the block would cover more than two pages. */
bool      uam_builder_add_page(uam_builder *b, uint64_t page_base);

/* Allocate a temporary of the given width without emitting anything. */
uam_ref   uam_builder_new_temp(uam_builder *b, unsigned width);
unsigned  uam_builder_ref_width(const uam_builder *b, uam_ref r); /* 0 for immediates */

/* Immediate operand. The value is stored as given; it must fit its use. */
uam_ref   uam_imm(uam_builder *b, uint64_t value);

/* Generic emitter. dst_width = 0 means no destination. Returns dst or NONE. */
uam_ref uam_emit(uam_builder *b, uam_opcode op, unsigned width, unsigned flags,
                 uint32_t aux, uam_ref guard, uam_ref a, uam_ref bb, uam_ref c,
                 unsigned dst_width);

/* Convenience emitters; each maps 1:1 onto an opcode. */
void    uam_imark(uam_builder *b, uint64_t pc, unsigned len);
void    uam_imark_cont(uam_builder *b, uint64_t pc, unsigned len); /* UAM_IMARK_CONT (delay slots) */
uam_ref uam_get(uam_builder *b, unsigned w, uint32_t reg_off);
void    uam_put(uam_builder *b, unsigned w, uint32_t reg_off, uam_ref v, uam_ref guard);
uam_ref uam_ld(uam_builder *b, unsigned w, unsigned flags, uam_ref addr, uam_ref guard);
void    uam_st(uam_builder *b, unsigned w, unsigned flags, uam_ref addr, uam_ref v, uam_ref guard);
uam_ref uam_stx(uam_builder *b, unsigned w, unsigned flags, uam_ref addr, uam_ref v, uam_ref guard);
uam_ref uam_amo(uam_builder *b, unsigned w, unsigned flags, uam_amo_op sub, uam_ref addr, uam_ref v, uam_ref guard);
uam_ref uam_cas(uam_builder *b, unsigned w, unsigned flags, uam_ref addr, uam_ref expect, uam_ref nv, uam_ref guard);
void    uam_fence(uam_builder *b, uam_fence_kind kind);
uam_ref uam_binop(uam_builder *b, uam_opcode op, unsigned w, uam_ref x, uam_ref y);
uam_ref uam_unop(uam_builder *b, uam_opcode op, unsigned w, uam_ref x);
uam_ref uam_select(uam_builder *b, unsigned w, uam_ref cond, uam_ref x, uam_ref y);
void    uam_exit(uam_builder *b, uam_ref guard, uam_ref target, uam_exit_kind kind);
uam_ref uam_sysrd(uam_builder *b, unsigned w, uint32_t id, uam_ref guard);
void    uam_syswr(uam_builder *b, unsigned w, uint32_t id, uam_ref v, uam_ref guard);
void    uam_trap(uam_builder *b, uam_ref guard, uam_trap_class cls, uam_ref cause, uam_ref tval);
void    uam_wait(uam_builder *b, uam_ref next_pc);
uam_ref uam_helper(uam_builder *b, const uam_helper_table *ht, uint32_t id,
                   uam_ref a, uam_ref bb, uam_ref c, uam_ref guard);

/* ------------------------------------------------------------------------ */
/* Optimizer                                                                 */
/* ------------------------------------------------------------------------ */

/*
 * Local, semantics-preserving optimization of a verified block, in place: constant
 * and copy propagation, constant folding, algebraic identities, register forwarding
 * (GET after GET/PUT of the same register), constant guards, unreachable-code and
 * dead-code removal. Side effects, IMARKs and PUT order are kept, so the result
 * still verifies and behaves identically. Returns false on allocation failure (the
 * block is then unusable).
 */
bool uam_optimize(uam_block *blk);

/* ------------------------------------------------------------------------ */
/* Verifier (section 8) and printer (section 12)                             */
/* ------------------------------------------------------------------------ */

typedef struct uam_verify_ctx {
    size_t                  regfile_size;   /* bound for GET/PUT */
    const uam_helper_table *helpers;        /* may be NULL if no HELPER ops */
    uint32_t                max_guest_insns;/* 0 = unlimited */
} uam_verify_ctx;

/* Returns true if the block is well formed; otherwise writes a reason. */
bool uam_verify(const uam_block *blk, const uam_verify_ctx *ctx, char *err, size_t errlen);

typedef struct uam_print_ctx {
    const uam_regdesc      *regs;     /* optional: names for GET/PUT offsets */
    size_t                  n_regs;
    const uam_helper_table *helpers;  /* optional: names for HELPER ids */
} uam_print_ctx;

void uam_print_insn(FILE *out, const uam_block *blk, const uam_insn *in, const uam_print_ctx *ctx);
void uam_print_block(FILE *out, const uam_block *blk, const uam_print_ctx *ctx);

#endif /* UAM_IR_H */
