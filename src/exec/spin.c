/*
 * spin.c - Spin loops whose passes the run loop may skip (spin.h).
 */
#include <stdlib.h>
#include <string.h>

#include "uam/bus.h"

#include "../ir/ir_eval.h"
#include "execops.h"
#include "spin.h"

#define SPIN_MAX_OPS   64u     /* longer blocks are not spin loops worth the evaluation */
#define SPIN_MAX_PUTS  16u
#define SPIN_MAX_REGFILE 8192u /* register files up to this size (bytes) */

bool uam_spin_skip_enabled(void)
{
    const char *off = getenv("UAM_SPIN_SKIP");   /* UAM_SPIN_SKIP=0: never skip */
    return !(off && strcmp(off, "0") == 0);
}

bool uam_spin_candidate(const uam_block *blk)
{
    if (blk->n_insn > SPIN_MAX_OPS || blk->n_guest == 0)
        return false;
    const uint64_t self = blk->guest_pc & uam_mask(blk->addr_width);

    /* The register bytes the block writes. */
    struct { uint32_t lo, hi; } put[SPIN_MAX_PUTS];
    unsigned n_put = 0;
    for (uint32_t i = 0; i < blk->n_insn; i++) {
        const uam_insn *in = &blk->insn[i];
        if (in->op == UAM_OP_PUT) {
            if (n_put == SPIN_MAX_PUTS)
                return false;
            put[n_put].lo = in->aux;
            put[n_put].hi = in->aux + (in->width + 7) / 8;
            n_put++;
        }
    }

    /* Which temps may differ from one pass to the next: those that depend on a register
     * the block writes. A load's value is the same while memory is (nothing else runs). */
    bool var[SPIN_MAX_OPS] = { false };   /* per temp index below n_insn's temps */
    if (blk->n_temp > SPIN_MAX_OPS)
        return false;
#define VAR(r) (!uam_ref_is_imm(r) && !uam_ref_is_none(r) && var[r])
    bool loops = false;
    for (uint32_t i = 0; i < blk->n_insn; i++) {
        const uam_insn *in = &blk->insn[i];
        bool v = VAR(in->guard);
        switch ((uam_opcode)in->op) {
        case UAM_OP_IMARK:
            if (in->flags & UAM_IMARK_BREAK)
                return false;
            continue;
        case UAM_OP_FENCE:
            continue;
        case UAM_OP_GET: {
            const uint32_t lo = in->aux, hi = in->aux + (in->width + 7) / 8;
            for (unsigned k = 0; k < n_put; k++)
                v |= lo < put[k].hi && put[k].lo < hi;
            break;
        }
        case UAM_OP_PUT:
            if (v || VAR(in->a))
                return false;
            continue;
        case UAM_OP_LD:
            if (in->flags & UAM_ACC_X)
                return false;   /* a load-reserved sets the reservation */
            v |= VAR(in->a);
            break;
        case UAM_OP_EXIT:
            if (v || VAR(in->a))
                return false;
            if (uam_ref_is_imm(in->a)
                && (blk->imm[uam_ref_index(in->a)] & uam_mask(blk->addr_width)) == self)
                loops = true;
            continue;
        default: {
            const uam_opinfo *oi = &uam_opinfo_table[in->op];
            if (oi->props != 0)
                return false;   /* stores, system registers, helpers, traps, waits, coverage */
            v |= VAR(in->a) || (oi->n_src > 1 && VAR(in->b)) || (oi->n_src > 2 && VAR(in->c));
            break;
        }
        }
        if (!uam_ref_is_none(in->dst))
            var[in->dst] = v;
    }
#undef VAR
    return loops;
}

static inline uint64_t opnd(const uam_block *blk, const uint64_t *t, uam_ref r, unsigned w)
{
    if (uam_ref_is_imm(r))
        return blk->imm[uam_ref_index(r)] & uam_mask(w);
    return t[r];
}

bool uam_spin_never_dev;

/* A guest read of pa goes to a device (MMIO, a ROM device in device mode, unmapped). */
static bool is_device(const uam_bus *bus, uint64_t pa)
{
    const uam_region *r = uam_bus_find(bus, pa);
    if (!r)
        return bus->unmapped != NULL;
    if (r->kind == UAM_REGION_ALIAS && r->target)
        r = r->target;
    return r->kind == UAM_REGION_MMIO || (r->kind == UAM_REGION_ROMD && !r->rom_mode);
}

/* As uam_interp_block would run one pass, but with no effect: see spin.h. */
bool uam_spin_fixed(uam_cpu *cpu, const uam_block *blk, uint64_t *t, bool *never)
{
    const unsigned aw = blk->addr_width;
    *never = false;
    uam_spin_never_dev = false;
    /* The pass runs on a copy of the register file: a later GET reads an earlier PUT,
     * and only the state at the end matters (a register may be written twice). */
    uint8_t rf[SPIN_MAX_REGFILE];
    if (cpu->regfile_size > sizeof(rf)) {
        *never = true;
        return false;
    }
    memcpy(rf, cpu->regfile, cpu->regfile_size);
    uint32_t lo = UINT32_MAX, hi = 0;   /* bytes written */
    for (uint32_t i = 0; i < blk->n_insn; i++) {
        const uam_insn *in = &blk->insn[i];
        const unsigned W = in->width;
        if (!uam_ref_is_none(in->guard) && !opnd(blk, t, in->guard, 1)) {
            if (!uam_ref_is_none(in->dst))
                t[in->dst] = 0;
            continue;
        }
        switch ((uam_opcode)in->op) {
        case UAM_OP_IMARK:
        case UAM_OP_FENCE:
            break;
        case UAM_OP_GET: {   /* as uam_cpu_reg_read */
            uint64_t v = 0;
            for (unsigned k = 0; k < W / 8; k++)
                v |= (uint64_t)rf[in->aux + k] << (8 * k);
            t[in->dst] = v;
            break;
        }
        case UAM_OP_PUT: {   /* as uam_cpu_reg_write */
            const uint64_t v = opnd(blk, t, in->a, W);
            for (unsigned k = 0; k < W / 8; k++)
                rf[in->aux + k] = (uint8_t)(v >> (8 * k));
            if (W >= 8) {
                lo = in->aux < lo ? in->aux : lo;
                hi = in->aux + W / 8 > hi ? in->aux + W / 8 : hi;
            }
            break;
        }
        case UAM_OP_LD: {
            const unsigned size = W / 8;
            const uint64_t addr = opnd(blk, t, in->a, aw);
            if ((in->flags & UAM_ACC_X) || uam_execops_crosses(cpu, addr, size, in->flags)) {
                *never = true;
                return false;
            }
            uint64_t pa = 0, v = 0;
            uint32_t xf = 0;
            if (uam_execops_xlate(cpu, addr, UAM_XLATE_LOAD, in->flags, size, &pa, &xf) != UAM_OK)
                return false;   /* the pass would fault */
            /* Only memory that reads without effect and that nothing watches: a RAM or
             * ROM page owned by one region (uam_bus_page_host). */
            if (!uam_bus_page_host(cpu->bus, pa, false)
                || (pa & UAM_PAGE_MASK) != ((pa + size - 1) & UAM_PAGE_MASK)) {
                *never = true;
                uam_spin_never_dev = (pa & UAM_PAGE_MASK) == ((pa + size - 1) & UAM_PAGE_MASK)
                                     && is_device(cpu->bus, pa);
                return false;
            }
            if (uam_bus_read(cpu->bus, pa, size, in->flags, &v) != UAM_OK) {
                *never = true;
                return false;
            }
            t[in->dst] = v;
            break;
        }
        case UAM_OP_EXIT:
            return (opnd(blk, t, in->a, aw) & uam_mask(aw)) == (blk->guest_pc & uam_mask(aw))
                && (lo >= hi || memcmp(rf + lo, cpu->regfile + lo, hi - lo) == 0);
        default: {
            const uam_opinfo *oi = &uam_opinfo_table[in->op];
            if (oi->props != 0)
                return false;
            unsigned src_w = W;
            if (in->op == UAM_OP_ZEXT || in->op == UAM_OP_SEXT || in->op == UAM_OP_TRUNC)
                src_w = blk->temp_width[in->a];
            uint64_t x = oi->n_src > 0 ? opnd(blk, t, in->a, in->op == UAM_OP_SELECT ? 1 : src_w) : 0;
            uint64_t y = oi->n_src > 1 ? opnd(blk, t, in->b, W) : 0;
            uint64_t z = oi->n_src > 2 ? opnd(blk, t, in->c, W) : 0;
            uint64_t r;
            if (!uam_ir_eval_pure((uam_opcode)in->op, W, x, y, z, src_w, &r))
                return false;
            t[in->dst] = r;
            break;
        }
        }
    }
    return false;
}

/* ---- device polls (exec.h, uam_poll) ------------------------------------------ */

typedef struct poll_eval {
    unsigned n;                              /* device reads reached */
    uint64_t pc[UAM_POLL_MAX_READS], pa[UAM_POLL_MAX_READS];
    unsigned size[UAM_POLL_MAX_READS], flags[UAM_POLL_MAX_READS];
} poll_eval;

/* One pass from the current state, without effects, device read k answering val[k].
 * Returns 1 if the pass goes round, 0 if it leaves, -1 if it cannot be evaluated (too
 * many device reads, a fault, an op with effects). */
static int poll_pass(uam_cpu *cpu, const uam_block *blk, uint64_t *t, const uint64_t *val,
                     poll_eval *pe)
{
    const unsigned aw = blk->addr_width;
    uint8_t rf[SPIN_MAX_REGFILE];
    if (cpu->regfile_size > sizeof(rf))
        return -1;
    memcpy(rf, cpu->regfile, cpu->regfile_size);
    uint64_t pc = blk->guest_pc;
    pe->n = 0;
    for (uint32_t i = 0; i < blk->n_insn; i++) {
        const uam_insn *in = &blk->insn[i];
        const unsigned W = in->width;
        if (!uam_ref_is_none(in->guard) && !opnd(blk, t, in->guard, 1)) {
            if (!uam_ref_is_none(in->dst))
                t[in->dst] = 0;
            continue;
        }
        switch ((uam_opcode)in->op) {
        case UAM_OP_IMARK:
            pc = opnd(blk, t, in->a, aw);
            break;
        case UAM_OP_FENCE:
            break;
        case UAM_OP_GET: {
            uint64_t v = 0;
            for (unsigned k = 0; k < W / 8; k++)
                v |= (uint64_t)rf[in->aux + k] << (8 * k);
            t[in->dst] = v;
            break;
        }
        case UAM_OP_PUT: {
            const uint64_t v = opnd(blk, t, in->a, W);
            for (unsigned k = 0; k < W / 8; k++)
                rf[in->aux + k] = (uint8_t)(v >> (8 * k));
            break;
        }
        case UAM_OP_LD: {
            const unsigned size = W / 8;
            const uint64_t addr = opnd(blk, t, in->a, aw);
            if ((in->flags & UAM_ACC_X) || uam_execops_crosses(cpu, addr, size, in->flags))
                return -1;
            uint64_t pa = 0, v = 0;
            uint32_t xf = 0;
            if (uam_execops_xlate(cpu, addr, UAM_XLATE_LOAD, in->flags, size, &pa, &xf) != UAM_OK)
                return -1;
            if ((pa & UAM_PAGE_MASK) != ((pa + size - 1) & UAM_PAGE_MASK))
                return -1;
            if (!is_device(cpu->bus, pa)) {
                /* memory: read it as a debugger would (no watch fires) */
                if (uam_bus_read(cpu->bus, pa, size, (in->flags & ~UAM_ACC_ALIGNED) | UAM_ACC_DEBUG,
                                 &v) != UAM_OK)
                    return -1;
            } else {
                if (pe->n == UAM_POLL_MAX_READS)
                    return -1;
                const unsigned k = pe->n++;
                pe->pc[k] = pc;
                pe->pa[k] = pa;
                pe->size[k] = size;
                pe->flags[k] = in->flags;
                v = val[k] & uam_mask(W);
            }
            t[in->dst] = v;
            break;
        }
        case UAM_OP_EXIT:
            return (opnd(blk, t, in->a, aw) & uam_mask(aw)) == (blk->guest_pc & uam_mask(aw));
        default: {
            const uam_opinfo *oi = &uam_opinfo_table[in->op];
            if (oi->props != 0)
                return -1;
            unsigned src_w = W;
            if (in->op == UAM_OP_ZEXT || in->op == UAM_OP_SEXT || in->op == UAM_OP_TRUNC)
                src_w = blk->temp_width[in->a];
            uint64_t x = oi->n_src > 0 ? opnd(blk, t, in->a, in->op == UAM_OP_SELECT ? 1 : src_w) : 0;
            uint64_t y = oi->n_src > 1 ? opnd(blk, t, in->b, W) : 0;
            uint64_t z = oi->n_src > 2 ? opnd(blk, t, in->c, W) : 0;
            uint64_t r;
            if (!uam_ir_eval_pure((uam_opcode)in->op, W, x, y, z, src_w, &r))
                return -1;
            t[in->dst] = r;
            break;
        }
        }
    }
    return -1;
}

/* The same device reads, in the same order, as `ref`. */
static bool same_reads(const poll_eval *a, const poll_eval *ref)
{
    if (a->n != ref->n)
        return false;
    for (unsigned k = 0; k < a->n; k++)
        if (a->pa[k] != ref->pa[k] || a->size[k] != ref->size[k] || a->pc[k] != ref->pc[k])
            return false;
    return true;
}

/* Whether a pass with read k answering probe (the others their last values) leaves the
 * loop. */
static bool leaves(uam_cpu *cpu, const uam_block *blk, uint64_t *t, uint64_t *val, unsigned k,
                   uint64_t probe)
{
    poll_eval pe;
    const uint64_t keep = val[k];
    val[k] = probe;
    const int r = poll_pass(cpu, blk, t, val, &pe);
    val[k] = keep;
    return r == 0;
}

/* What ends the loop through read k: fills p's exit fields; false if nothing tried does. */
static bool exits_by(uam_cpu *cpu, const uam_block *blk, uint64_t *t, uint64_t *val, unsigned k,
                     unsigned size, uam_poll *p)
{
    const unsigned W = size * 8;
    const uint64_t m = uam_mask(W), v = val[k] & m;
    p->exit_set = p->exit_clear = p->exit_all_set = p->exit_all_clear = 0;
    p->exit_other = false;
    for (unsigned b = 0; b < W; b++) {
        const uint64_t bit = UINT64_C(1) << b;
        if (leaves(cpu, blk, t, val, k, v ^ bit)) {
            if (v & bit)
                p->exit_clear |= bit;
            else
                p->exit_set |= bit;
        }
    }
    if (p->exit_set || p->exit_clear)
        return true;
    /* Several bits at once: the block's constants as masks, all set or all clear, then
     * only the bits of the mask that are needed (a constant may hold more: an address). */
    for (uint32_t i = 0; i < blk->n_imm; i++) {
        const uint64_t c = blk->imm[i] & m;
        for (int set = 1; set >= 0; set--) {
            uint64_t bits = set ? c & ~v : c & v;
            if (!bits || !leaves(cpu, blk, t, val, k, set ? v | bits : v & ~bits))
                continue;
            for (unsigned b = 0; b < W; b++) {
                const uint64_t bit = UINT64_C(1) << b, less = bits & ~bit;
                if ((bits & bit) && less && leaves(cpu, blk, t, val, k, set ? v | less : v & ~less))
                    bits = less;
            }
            if (set)
                p->exit_all_set = bits;
            else
                p->exit_all_clear = bits;
            return true;
        }
    }
    /* A comparison with a whole value: the block's constants, then 0 and all ones. */
    for (uint32_t i = 0; i <= blk->n_imm + 1; i++) {
        const uint64_t c = i < blk->n_imm ? blk->imm[i] & m : i == blk->n_imm ? 0 : m;
        if (c != v && leaves(cpu, blk, t, val, k, c)) {
            p->exit_other = true;
            p->exit_value = c;
            return true;
        }
    }
    return false;
}

bool uam_spin_poll_analyse(uam_cpu *cpu, const uam_block *blk, uint64_t *t, uint64_t first_read,
                           uam_poll *p)
{
    uam_bus *bus = cpu->bus;
    uint64_t val[UAM_POLL_MAX_READS] = { 0 };
    poll_eval ref, pe;
    /* Where the reads go: one pass with any values. */
    if (poll_pass(cpu, blk, t, val, &ref) < 0 || ref.n == 0)
        return false;
    /* What the pass that just went round read: the bus remembers the last reads; a
     * device's peek for any it no longer holds. */
    const bool held = bus->n_dev_reads - first_read == ref.n && ref.n <= UAM_BUS_DEV_READS;
    bool have[UAM_POLL_MAX_READS] = { false };
    for (unsigned k = 0; k < ref.n; k++) {
        const uint64_t seq = first_read + k;
        const uint32_t slot = (uint32_t)(seq % UAM_BUS_DEV_READS);
        if (held && bus->dev_reads[slot].addr == ref.pa[k]) {
            val[k] = bus->dev_reads[slot].val;
            have[k] = true;
        } else if (uam_bus_read(bus, ref.pa[k], ref.size[k],
                                (ref.flags[k] & (UAM_ACC_LE | UAM_ACC_BE)) | UAM_ACC_DEBUG,
                                &val[k]) == UAM_OK) {
            have[k] = true;   /* the device's peek */
        }
    }
    /* Those values must keep it looping, through the same reads: else this is not a
     * pass the analysis can stand for (the addresses depend on what was read). */
    if (poll_pass(cpu, blk, t, val, &pe) != 1 || !same_reads(&pe, &ref))
        return false;
    unsigned k = 0;
    while (k < ref.n && !exits_by(cpu, blk, t, val, k, ref.size[k], p))
        k++;
    if (k == ref.n) {   /* nothing tried ends it: report the first read */
        k = 0;
        p->exit_set = p->exit_clear = p->exit_all_set = p->exit_all_clear = 0;
        p->exit_other = false;
    }
    p->n_reads = ref.n;
    p->load_pc = ref.pc[k];
    p->addr = ref.pa[k];
    p->size = ref.size[k];
    p->have_value = have[k];
    p->value = val[k] & uam_mask(ref.size[k] * 8);
    return true;
}

static void bits_str(char *buf, size_t len, uint64_t mask, bool all)
{
    if (!(mask & (mask - 1))) {
        unsigned b = 0;
        while (!((mask >> b) & 1))
            b++;
        snprintf(buf, len, "bit %u (0x%llx)", b, (unsigned long long)mask);
    } else {
        snprintf(buf, len, "%s bits 0x%llx", all ? "all of" : "any of", (unsigned long long)mask);
    }
}

void uam_poll_describe(const uam_poll *p, const uam_bus *bus, char *buf, size_t len)
{
    char where[96] = "";
    const uam_region *r = bus ? uam_bus_find(bus, p->addr) : NULL;
    if (r)
        snprintf(where, sizeof(where), " (%s+0x%llx)", r->name ? r->name : "?",
                 (unsigned long long)(p->addr - r->base));
    else if (bus)
        snprintf(where, sizeof(where), " (unmapped)");
    size_t n = 0;
#define ADD(...) do { if (n < len) { int k_ = snprintf(buf + n, len - n, __VA_ARGS__); \
                                     if (k_ > 0) n += (size_t)k_; } } while (0)
    ADD("pc 0x%llx: loop at 0x%llx reads %u byte%s at 0x%llx%s", (unsigned long long)p->load_pc,
        (unsigned long long)p->pc, p->size, p->size == 1 ? "" : "s", (unsigned long long)p->addr,
        where);
    if (p->n_reads > 1)
        ADD(" (of %u device reads a pass)", p->n_reads);
    if (p->have_value)
        ADD(", last 0x%llx", (unsigned long long)p->value);
    const uint64_t m = uam_mask(p->size * 8);
    char b1[64], b2[64];
    if ((p->exit_set | p->exit_clear) == m && p->have_value) {
        ADD("; leaves the loop when the value changes from 0x%llx", (unsigned long long)p->value);
    } else if (p->exit_set && p->exit_clear) {
        bits_str(b1, sizeof(b1), p->exit_set, false);
        bits_str(b2, sizeof(b2), p->exit_clear, false);
        ADD("; leaves the loop if %s becomes 1 or %s becomes 0", b1, b2);
    } else if (p->exit_set) {
        bits_str(b1, sizeof(b1), p->exit_set, false);
        ADD("; leaves the loop if %s becomes 1", b1);
    } else if (p->exit_clear) {
        bits_str(b1, sizeof(b1), p->exit_clear, false);
        ADD("; leaves the loop if %s becomes 0", b1);
    } else if (p->exit_all_set) {
        bits_str(b1, sizeof(b1), p->exit_all_set, true);
        ADD("; leaves the loop if %s become 1 (no single bit does)", b1);
    } else if (p->exit_all_clear) {
        bits_str(b1, sizeof(b1), p->exit_all_clear, true);
        ADD("; leaves the loop if %s become 0 (no single bit does)", b1);
    } else if (p->exit_other) {
        ADD("; leaves the loop on a read of 0x%llx (no single bit does)",
            (unsigned long long)p->exit_value);
    } else {
        ADD("; no value tried leaves the loop (it may wait for something else: an interrupt, "
            "a timeout)");
    }
    if (p->changes)
        ADD("; the value changed on %llu pass%s while it looped (a counter or a timer?)",
            (unsigned long long)p->changes, p->changes == 1 ? "" : "es");
#undef ADD
}
