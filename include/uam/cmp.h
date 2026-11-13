/*
 * uam/cmp.h - Compare instrumentation: the values the guest compares, for a fuzzer.
 *
 * The compare pass inserts UAM_OP_CMPLOG (docs/uam-isa.md section 7.6) in front of each
 * compare the guest makes. It works on the IR alone, so on every ISA. It recognises
 *   - EQ, NE, ULT, ULE, SLT, SLE of width >= 8 between two values;
 *   - a difference x - y or x ^ y tested against zero (EQ, NE, SLT, SLE of it with 0), and
 *     SBORROW x, y: the zero, sign and overflow flags of a flags ISA's compare. The record
 *     names x and y, and goes in front of the SUB or XOR.
 * x ^ c1, x + c1 or x - c1 compared with a constant c2 is recorded as x against the
 * constant it must equal (c1 ^ c2, c2 - c1, c2 + c1): a compiler's way of testing a
 * constant in parts (PowerPC's xoris + cmplwi) or a range (sub + cmp).
 * One record per distinct pair of operands per guest instruction (the flags of one
 * compare make one record, their kinds or-ed together). Left out: compares with the
 * constant 0 (a zero flag of any arithmetic, which a mutator needs no help with), of a
 * value with itself, and of two constants. With a constant operand, it is b and the kind
 * has UAM_CMP_IMM.
 *
 * A uam_cmp_state receives what CMPLOG reports: while logging (uam_cmp_set_logging), the
 * distinct (site, a, b) of a run, up to the log's capacity and at most UAM_CMP_SITE_MAX
 * per site (a loop's counter would fill the log otherwise), so that a fuzzer can write
 * one operand into the input where the other came from (input-to-state).
 * uam_cmp_note is the reference the interpreter runs and the JIT calls. The state is not
 * machine state: it is never in a snapshot, and the executor only points at it
 * (uam_exec_set_cmp in exec.h).
 *
 * Dependencies: pass.h.
 */
#ifndef UAM_CMP_H
#define UAM_CMP_H

#include "uam/pass.h"

/* ---- the state CMPLOG reports to ---------------------------------------- */

typedef struct uam_cmp_entry {
    uint32_t site;      /* uam_cmp_site(pc, k) */
    uint8_t  width;     /* bits: 8, 16, 32 or 64 */
    uint8_t  kind;      /* UAM_CMP_* (ir.h) */
    uint16_t reserved;  /* 0 */
    uint64_t a, b;      /* the operands, zero-extended */
} uam_cmp_entry;

#define UAM_CMP_SITE_MAX     8u     /* distinct pairs logged per site and run */
#define UAM_CMP_DEFAULT_LOG  1024u  /* entries a log holds by default */

typedef struct uam_cmp_state uam_cmp_state;

/* A state whose log holds up to log_cap entries (0: UAM_CMP_DEFAULT_LOG), logging off.
 * NULL when out of memory. */
uam_cmp_state       *uam_cmp_state_create(uint32_t log_cap);
void                 uam_cmp_state_destroy(uam_cmp_state *s);

void                 uam_cmp_set_logging(uam_cmp_state *s, bool on);
bool                 uam_cmp_logging(const uam_cmp_state *s);
/* Empty the log. */
void                 uam_cmp_reset(uam_cmp_state *s);
/* The log since the last reset, in the order first seen; *dropped: records left out
 * because the log or their site was full. */
const uam_cmp_entry *uam_cmp_log(const uam_cmp_state *s, size_t *n, uint64_t *dropped);

/* One CMPLOG: site = its aux, wk = width | kind << 8, a and b its operand values. */
void                 uam_cmp_note(uam_cmp_state *s, uint32_t site, uint32_t wk, uint64_t a,
                                  uint64_t b);

/* ---- the pass ------------------------------------------------------------ */

typedef struct uam_cmp_pass uam_cmp_pass;

uam_status      uam_cmp_pass_create(uam_cmp_pass **out);
const uam_pass *uam_cmp_pass_get(const uam_cmp_pass *cp);
void            uam_cmp_pass_destroy(uam_cmp_pass *cp);

/* The site id of the k-th record of the instruction at pc. */
uint32_t        uam_cmp_site(uint64_t pc, unsigned k);

#endif /* UAM_CMP_H */
