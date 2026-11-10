/*
 * jit.c - The host-independent part of the native-code backend (jit.h, jit_impl.h).
 *
 * The runtime context and the C bridges native code calls; the analyses every block
 * goes through before a host's code generator (jit_x86.c, jit_a64.c) emits it; the code
 * arena; and block linking. test_jitdiff checks that the backend agrees with the
 * interpreter.
 *
 * The arena is one anonymous mapping. On Linux it is readable, writable and executable.
 * On macOS it is a MAP_JIT mapping, which a thread sees either writable or executable:
 * the writes made here (placing a block, linking a slot) open it for writing around
 * them (arena_write), and no code runs meanwhile. After placing code, the host's
 * instruction cache is made coherent with it (arena_sync); slots are data, read by
 * loads, so linking one needs no such step.
 */
#include "jit_impl.h"

#if !defined(UAM_JIT_HOST)

uam_jit *uam_jit_create(uam_exec *x) { (void)x; return NULL; }
void     uam_jit_destroy(uam_jit *j) { (void)j; }
bool     uam_jit_compile(uam_jit *j, uam_block *blk) { (void)j; blk->backend = UAM_JIT_INTERP; return true; }
void     uam_jit_run(uam_jit *j, const uam_block *blk, uint64_t *t, uam_jit_chain *c,
                     uam_block_result *r)
{ (void)j; (void)blk; (void)t; (void)c; (void)r; }

#else

#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <pthread.h>
#endif

#include "../ir/ir_eval.h"
#include "execops.h"

/* ---- C bridges ----------------------------------------------------------------- */

/* After a bridge: if the bus mapping, the timer queue or the privilege level changed,
 * the run loop has work to do (data-TLB flush, a new deadline, blocks translated at the
 * new level) before the next block, so stop chaining. A helper may change cpu->priv
 * without an exit request (an exception return); a timer armed earlier usually
 * requests an exit too (the machine's scheduler kick). */
static void chain_check(uam_jit_ctx *jc)
{
    if (jc->cpu->bus->direct_gen != jc->dgen || jc->cpu->priv != jc->priv
        || uam_sched_next(jc->sched) != jc->next_when) {
        jc->chain_budget -= jc->chain_left;   /* keeps the hop count */
        jc->chain_left = 0;
    }
}

static int jit_fault(uam_jit_ctx *jc, uam_status st, uint64_t addr, uint32_t xf, bool store)
{
    uam_block_result *r = &jc->res;
    r->kind = UAM_BR_FAULT;
    r->pc = jc->site->pc;
    r->exc.kind = uam_execops_fault_kind(st, store);
    r->exc.flags = jc->site->cont ? UAM_EXC_F_CONT : 0;
    r->exc.pc = jc->site->pc;
    r->exc.parent_pc = jc->site->parent_pc;
    r->exc.cause = xf;
    r->exc.tval = addr;
    return 1;
}

/*
 * After a successful access, cache the page in the data-TLB so later accesses to it
 * take the inline fast path. Only RAM (both directions) and ROM (reads) are cached:
 * their host bytes are stable for the current generation, and the whole page must be
 * owned by one region and unwatched (uam_bus_page_host enforces both). A RAM page that
 * holds translated code goes into the write set tagged DTLB_CODE, with its code-line
 * mask in jc->clines: stores to it stay inline only where they miss the code. Device
 * and ROMD pages are never cached.
 */
static void dtlb_fill(uam_jit_ctx *jc, uint64_t addr, uint32_t flags, unsigned size, bool write)
{
    uam_cpu *cpu = jc->cpu;
    if (flags & (UAM_ACC_PHYS | UAM_ACC_UNPRIV))
        return;   /* fast path only models the ordinary translation regime */
    uint64_t pa;
    uint32_t xf = 0;
    if (uam_execops_xlate(cpu, addr, write ? UAM_XLATE_STORE : UAM_XLATE_LOAD, flags & ~UAM_ACC_ALIGNED,
                          size, &pa, &xf) != UAM_OK)
        return;
    const uam_region *r = uam_bus_find(cpu->bus, pa);
    if (!r || (r->kind != UAM_REGION_RAM && !(r->kind == UAM_REGION_ROM && !write)))
        return;
    const uint64_t *lines = NULL;
    uint8_t *h = write ? uam_bus_page_host_code(cpu->bus, pa, &lines)
                       : uam_bus_page_host(cpu->bus, pa, false);
    if (!h)
        return;
    uint64_t vpage = addr >> UAM_PAGE_SHIFT;
    unsigned idx = (unsigned)vpage & (UAM_DTLB_SIZE - 1);
    uint64_t base = (uint64_t)(uintptr_t)h - (vpage << UAM_PAGE_SHIFT);
    if (write) {
        cpu->dtlb_wt[idx] = lines ? vpage | DTLB_CODE : vpage;
        cpu->dtlb_wb[idx] = base;
        jc->clines[idx] = lines;
    } else {
        cpu->dtlb_rt[idx] = vpage;
        cpu->dtlb_rb[idx] = base;
    }
}

int jit_br_load(uam_jit_ctx *jc, uint64_t addr, uint32_t flags, uint32_t size)
{
    uint32_t xf = 0;
    uint64_t fva;
    uam_status st = uam_execops_load(jc->cpu, addr, flags, size, &jc->scratch, &xf, &fva);
    jc->stats->jit_load_calls++;
    chain_check(jc);
    if (st != UAM_OK)
        return jit_fault(jc, st, fva, xf, false);
    if (!(flags & UAM_ACC_X))
        dtlb_fill(jc, addr, flags, size, false);
    return 0;
}

int jit_br_store(uam_jit_ctx *jc, uint64_t addr, uint64_t val, uint32_t flags, uint32_t size)
{
    uint32_t xf = 0;
    uint64_t fva;
    uam_status st = uam_execops_store(jc->cpu, addr, val, flags, size, &xf, &fva);
    jc->stats->jit_store_calls++;
    chain_check(jc);
    if (st != UAM_OK)
        return jit_fault(jc, st, fva, xf, true);
    dtlb_fill(jc, addr, flags, size, true);
    return 0;
}

int jit_br_store_x(uam_jit_ctx *jc, uint64_t addr, uint64_t val, uint32_t flags, uint32_t size)
{
    uint32_t xf = 0;
    bool ok = false;
    uint64_t fva;
    uam_status st = uam_execops_store_x(jc->cpu, addr, val, flags, size, &ok, &xf, &fva);
    chain_check(jc);
    if (st != UAM_OK)
        return jit_fault(jc, st, fva, xf, true);
    jc->scratch = ok;
    return 0;
}

int jit_br_rmw(uam_jit_ctx *jc, uint64_t addr, uint64_t b, uint64_t c, uint32_t flags,
               uint32_t packed)
{
    uint32_t xf = 0;
    const bool cas = (packed & 0xff) == UAM_OP_CAS;
    uam_status st = uam_execops_rmw(jc->cpu, cas, (uam_amo_op)((packed >> 8) & 0xff), addr, b, c,
                                    flags, (packed >> 16) & 0xff, &jc->scratch, &xf);
    chain_check(jc);
    return st == UAM_OK ? 0 : jit_fault(jc, st, addr, xf, true);
}

static int jit_fault_simple(uam_jit_ctx *jc, uam_exc_kind kind, uint64_t tval)
{
    uam_block_result *r = &jc->res;
    r->kind = UAM_BR_FAULT;
    r->pc = jc->site->pc;
    r->exc.kind = kind;
    r->exc.flags = jc->site->cont ? UAM_EXC_F_CONT : 0;
    r->exc.pc = jc->site->pc;
    r->exc.parent_pc = jc->site->parent_pc;
    r->exc.cause = 0;
    r->exc.tval = tval;
    return 1;
}

int jit_br_sysrd(uam_jit_ctx *jc, uint32_t id)
{
    uint64_t v = 0;
    uam_status st = jc->cpu->isa->sysreg_read(jc->cpu, id, &v);
    chain_check(jc);
    if (st != UAM_OK)
        return jit_fault_simple(jc, UAM_EXC_SYSREG, id);
    jc->scratch = v;
    return 0;
}

int jit_br_syswr(uam_jit_ctx *jc, uint32_t id, uint64_t val)
{
    uam_status st = jc->cpu->isa->sysreg_write(jc->cpu, id, val);
    chain_check(jc);
    return st == UAM_OK ? 0 : jit_fault_simple(jc, UAM_EXC_SYSREG, id);
}

/* Arguments and result masked to their declared widths, as the interpreter does. */
int jit_br_helper(uam_jit_ctx *jc, uint32_t id, uint64_t a, uint64_t b, uint64_t c)
{
    const uam_helper_desc *hd = &jc->helpers->desc[id];
    const uint64_t args[3] = { a, b, c };
    uint64_t v[3] = { 0, 0, 0 };
    for (unsigned k = 0; k < hd->n_args; k++)
        v[k] = args[k] & uam_mask(hd->arg_width[k]);
    uam_status st = UAM_OK;
    uint64_t r = hd->fn(jc->cpu, v[0], v[1], v[2], &st);
    chain_check(jc);
    if (st != UAM_OK)
        return jit_fault_simple(jc, UAM_EXC_HELPER, id);
    jc->scratch = hd->result_width ? r & uam_mask(hd->result_width) : 0;
    return 0;
}

uint64_t jit_br_eval(uam_jit_ctx *jc, uint32_t packed, uint64_t x, uint64_t y, uint64_t z)
{
    uint64_t out = 0;
    jc->stats->jit_eval_calls++;
    uam_ir_eval_pure((uam_opcode)(packed & 0xff), (packed >> 8) & 0xff, x, y, z,
                     (packed >> 16) & 0xff, &out);
    return out;
}

/* ---- compiler support ---------------------------------------------------------- */

static const char *const mark_name[] = { "prologue", "op", "imark-pc", "slow", "chain", "stub",
                                         "epilogue", "slots", "replay", "code-st" };

void jit_mark(uam_jit *j, unsigned kind, uint32_t idx)
{
    if (!j->opmap)
        return;
    if (j->n_marks == j->cap_marks) {
        uint32_t cap = j->cap_marks ? 2 * j->cap_marks : 256;
        jit_opmark *m = realloc(j->marks, sizeof(*m) * cap);
        if (!m)
            return;
        j->marks = m;
        j->cap_marks = cap;
    }
    j->marks[j->n_marks++] = (jit_opmark){ (uint32_t)j->n, idx, (uint8_t)kind };
}

bool jit_grow(uam_jit *j, void *p, uint32_t *cap, uint32_t need, size_t size)
{
    if (need <= *cap)
        return true;
    uint32_t ncap = *cap ? 2 * *cap : 16;
    while (ncap < need)
        ncap *= 2;
    void *n = realloc(*(void **)p, size * ncap);
    if (!n) {
        j->overflow = true;
        return false;
    }
    *(void **)p = n;
    *cap = ncap;
    return true;
}

bool jit_const_slow(uint64_t addr, unsigned size, unsigned flags)
{
    return (size > 1 && (addr & (UAM_PAGE_SIZE - 1)) > UAM_PAGE_SIZE - size)
        || ((flags & UAM_ACC_ALIGNED) && (addr & (size - 1)));
}

bool jit_site_ref(uam_jit *j, size_t pos)
{
    if (!j->cur_emitted) {
        if (!jit_grow(j, &j->sites, &j->cap_sites, j->n_sites + 1, sizeof(jit_site)))
            return false;
        j->sites[j->n_sites++] = j->cur;
        j->cur_emitted = true;
    }
    if (!jit_grow(j, &j->site_refs, &j->cap_site_refs, j->n_site_refs + 1, sizeof(*j->site_refs)))
        return false;
    j->site_refs[j->n_site_refs].pos = pos;
    j->site_refs[j->n_site_refs].site = j->n_sites - 1;
    j->n_site_refs++;
    return true;
}

void jit_stub_add(uam_jit *j, const uam_block *blk, size_t pos, uint64_t pc, uint32_t i)
{
    if (!jit_grow(j, &j->stubs, &j->cap_stubs, j->n_stubs + 1, sizeof(*j->stubs)))
        return;
    j->stubs[j->n_stubs].pos = pos;
    j->stubs[j->n_stubs].pc = pc;
    j->stubs[j->n_stubs].i = i;
    j->stubs[j->n_stubs].snap = j->n_snaps;
    const unsigned nr = jit_insn_reads(j, blk, i);   /* the replay's reads */
    if (nr && jit_grow(j, &j->snaps, &j->cap_snaps, j->n_snaps + nr, sizeof(jit_snap)))
        for (unsigned e = 0; e < nr; e++)
            j->snaps[j->n_snaps++] = (jit_snap){ j->rd[e], j->reg_of[j->rd[e]] };
    j->stubs[j->n_stubs].n_snap = j->n_snaps - j->stubs[j->n_stubs].snap;
    j->n_stubs++;
}

/* Each entry swaps the temp's register now with the one it had at the IMARK, so doing it
 * twice, the second time in reverse (a temp may repeat), restores the allocation. */
void jit_stub_regs(uam_jit *j, uint32_t k, bool enter)
{
    jit_snap *sn = &j->snaps[j->stubs[k].snap];
    const uint32_t n = j->stubs[k].n_snap;
    for (uint32_t c = 0; c < n; c++) {
        const uint32_t e = enter ? c : n - 1 - c;
        const int8_t r = j->reg_of[sn[e].t];
        j->reg_of[sn[e].t] = sn[e].reg;
        sn[e].reg = r;
    }
}

/* ---- register allocation ---------------------------------------------------------
 *
 * Temps are block-local SSA values that no one sees after the block, so they can live
 * in host registers. A linear scan in instruction order: before an op's code, jit_ra_dst
 * picks the register its result goes to - a free one, or one whose temp the op itself
 * reads for the last time, so that the op computes in place and no move follows it (or,
 * with none of either, the one whose temp is read furthest in the future, storing that
 * temp to its slot in jc->temps, or none: the new temp stays in its slot if it is read
 * later than all of them) - and after it jit_ra_def binds the temp to that register; a
 * register is freed after its temp's last read. A temp once stored is never stored
 * again (SSA), so a spilled temp is simply read from memory from then on. The register
 * of a guarded op's result is picked before the guard's branch and bound after its
 * paths join, and calls save and restore the live call-clobbered registers, so the
 * allocation is the same on every path.
 * A deferred PUT or a cold op (jit_defer_analyze) reads nothing where it stands; instead
 * the values a replay needs - the writes' values and guards, the operands of the cold
 * ops it recomputes - are read at every soft point that replays them, so they stay
 * live up to the last. Cold temps are never in a register: a replay computes them into
 * their slots. */

/* A temp only cold ops and deferred PUTs read, computed by the replays that need it. */
static bool cold_temp(const uam_jit *j, uam_ref t)
{
    return j->def_of[t] != NO_TEMP && j->cold[j->def_of[t]];
}

/* Append the temp operands of `in` that are not cold to out[]. */
static unsigned push_reads(const uam_jit *j, const uam_insn *in, bool guard_and_a_only,
                           uam_ref *out, unsigned n)
{
    const uam_ref refs[4] = { in->guard, in->a, in->b, in->c };
    for (unsigned k = 0; k < (guard_and_a_only ? 2u : 4u); k++)
        if (!uam_ref_is_none(refs[k]) && !uam_ref_is_imm(refs[k]) && !cold_temp(j, refs[k]))
            out[n++] = refs[k];
    return n;
}

/* The temps insn i reads: its operands (none for a deferred PUT or a cold op), then
 * those of each cold op and write its replay recomputes and replays. The room in j->rd
 * is max_reads(j, i). */
unsigned jit_insn_reads(const uam_jit *j, const uam_block *blk, uint32_t i)
{
    uam_ref *out = j->rd;
    unsigned n = 0;
    if (!j->dfr[i] && !j->cold[i])
        n = push_reads(j, &blk->insn[i], false, out, n);
    for (uint32_t e = j->clo_start[i]; e < j->clo_start[i + 1]; e++)
        n = push_reads(j, &blk->insn[j->clo_list[e]], false, out, n);
    for (uint32_t e = j->obs_start[i]; e < j->obs_start[i + 1]; e++)
        n = push_reads(j, &blk->insn[j->obs_list[e]], true, out, n);
    return n;
}

/* Reads of one insn at most: 4 operands, 4 per recomputed cold op, 2 per write. */
static unsigned max_reads(const uam_jit *j, uint32_t i)
{
    return 4 + 4 * (j->clo_start[i + 1] - j->clo_start[i])
             + 2 * (j->obs_start[i + 1] - j->obs_start[i]);
}

bool jit_ra_begin(uam_jit *j, const uam_block *blk)
{
    const uint32_t nt = blk->n_temp ? blk->n_temp : 1;
    if (nt + 1 > j->cap_temp) {
        uint32_t cap = nt + 1;
        int8_t *r = realloc(j->reg_of, cap);
        if (r) j->reg_of = r;
        uint8_t *m = realloc(j->in_mem, cap);
        if (m) j->in_mem = m;
        uint32_t *l = realloc(j->last_use, sizeof(uint32_t) * cap);
        if (l) j->last_use = l;
        uint32_t *s = realloc(j->use_start, sizeof(uint32_t) * cap);
        if (s) j->use_start = s;
        uint32_t *c = realloc(j->use_cur, sizeof(uint32_t) * cap);
        if (c) j->use_cur = c;
        if (!r || !m || !l || !s || !c)
            return false;
        j->cap_temp = cap;
    }
    uint32_t n_use = 0, max = 4;
    for (uint32_t i = 0; i < blk->n_insn; i++)
        if (max_reads(j, i) > max)
            max = max_reads(j, i);
    if (max > j->cap_rd) {
        uam_ref *r2 = realloc(j->rd, sizeof(uam_ref) * max);
        if (!r2)
            return false;
        j->rd = r2;
        j->cap_rd = max;
    }
    const uam_ref *rd = j->rd;
    memset(j->use_start, 0, sizeof(uint32_t) * (nt + 1));
    for (uint32_t i = 0; i < blk->n_insn; i++) {
        unsigned n = jit_insn_reads(j, blk, i);
        for (unsigned k = 0; k < n; k++)
            j->use_start[rd[k] + 1]++;
        n_use += n;
    }
    if (n_use > j->cap_use) {
        uint32_t *u = realloc(j->use_pos, sizeof(uint32_t) * n_use);
        if (!u)
            return false;
        j->use_pos = u;
        j->cap_use = n_use;
    }
    for (uint32_t t = 0; t < nt; t++)
        j->use_start[t + 1] += j->use_start[t];
    memcpy(j->use_cur, j->use_start, sizeof(uint32_t) * nt);
    for (uint32_t i = 0; i < blk->n_insn; i++) {   /* fill in insn order: sorted */
        unsigned n = jit_insn_reads(j, blk, i);
        for (unsigned k = 0; k < n; k++)
            j->use_pos[j->use_cur[rd[k]]++] = i;
    }
    for (uint32_t t = 0; t < nt; t++) {
        j->use_cur[t] = j->use_start[t];
        j->last_use[t] = j->use_start[t + 1] > j->use_start[t] ? j->use_pos[j->use_start[t + 1] - 1]
                                                               : NO_TEMP;
        j->reg_of[t] = -1;
        j->in_mem[t] = 0;
    }
    for (unsigned r = 0; r < MAX_HOST_REGS; r++)
        j->holder[r] = NO_TEMP;
    return true;
}

/* The first insn after the current one that reads temp t, or NO_TEMP. */
static uint32_t next_use(uam_jit *j, uint32_t t)
{
    while (j->use_cur[t] < j->use_start[t + 1] && j->use_pos[j->use_cur[t]] <= j->cur_i)
        j->use_cur[t]++;
    return j->use_cur[t] < j->use_start[t + 1] ? j->use_pos[j->use_cur[t]] : NO_TEMP;
}

void jit_ra_free_dead(uam_jit *j, const uam_block *blk)
{
    const uint32_t i = j->cur_i;
    const unsigned n = jit_insn_reads(j, blk, i);
    for (unsigned k = 0; k < n; k++) {
        uam_ref t = j->rd[k];
        if (j->reg_of[t] < 0 || j->last_use[t] > i)
            continue;
        j->holder[j->reg_of[t]] = NO_TEMP;
        j->reg_of[t] = -1;
    }
}

/* Can an insn's result go to pool register r: is it free, or does it hold a temp the
 * current insn reads for the last time? */
static bool reg_avail(const uam_jit *j, int r)
{
    const uint32_t t = j->holder[r];
    return t == NO_TEMP || j->last_use[t] <= j->cur_i;
}

int jit_ra_dst(uam_jit *j, uam_ref dst, int want0, int want1)
{
    if (j->last_use[dst] == NO_TEMP)
        return j->res_reg;                         /* never read */
    if (want0 >= 0 && reg_avail(j, want0))
        return want0;
    if (want1 >= 0 && reg_avail(j, want1))
        return want1;
    for (unsigned k = 0; k < j->n_pool; k++)
        if (j->holder[j->pool[k]] == NO_TEMP)
            return j->pool[k];
    for (unsigned k = 0; k < j->n_pool; k++)
        if (reg_avail(j, j->pool[k]))
            return j->pool[k];
    if (j->n_pool) {
        int victim = j->pool[0];
        uint32_t far = 0;
        for (unsigned k = 0; k < j->n_pool; k++) {
            uint32_t nu = next_use(j, j->holder[j->pool[k]]);
            if (nu >= far) {
                far = nu;
                victim = j->pool[k];
            }
        }
        if (far > next_use(j, dst)) {
            uint32_t t = j->holder[victim];
            if (!j->in_mem[t]) {
                jit_host_spill(j, t, victim);
                j->in_mem[t] = 1;
            }
            j->reg_of[t] = -1;
            j->holder[victim] = NO_TEMP;
            return victim;
        }
    }
    return j->res_reg;                             /* stays in its slot */
}

void jit_ra_def(uam_jit *j, uam_ref dst, int reg)
{
    if (j->last_use[dst] == NO_TEMP)
        return;                                    /* never read */
    if (reg == j->res_reg) {
        jit_host_spill(j, dst, reg);
        j->in_mem[dst] = 1;
        return;
    }
    j->holder[reg] = dst;
    j->reg_of[dst] = (int8_t)reg;
}

/* The stores change no allocation state, so this is also right on a path that is taken
 * only sometimes. */
void jit_ra_save(uam_jit *j, bool restore)
{
    for (unsigned k = j->pool_kept; k < j->n_pool; k++) {
        int r = j->pool[k];
        uint32_t t = j->holder[r];
        if (t == NO_TEMP || j->last_use[t] <= j->cur_i)
            continue;
        if (restore)
            jit_host_fill(j, t, r);
        else if (!j->in_mem[t])
            jit_host_spill(j, t, r);
    }
}

bool jit_fusable(const uam_jit *j, const uam_block *blk, uint32_t i)
{
    const uam_insn *in = &blk->insn[i];
    switch (in->op) {
    case UAM_OP_EQ: case UAM_OP_NE: case UAM_OP_ULT: case UAM_OP_ULE: case UAM_OP_SLT:
    case UAM_OP_SLE: case UAM_OP_CARRY: case UAM_OP_SCARRY: case UAM_OP_SBORROW:
        break;
    default:
        return false;
    }
    const uam_ref t = in->dst;
    if (j->use_start[t + 1] - j->use_start[t] != 1)
        return false;
    uint32_t k = i + 1;
    if (k < blk->n_insn && blk->insn[k].op == UAM_OP_IMARK && (blk->insn[k].flags & UAM_IMARK_CONT))
        k++;
    if (k >= blk->n_insn || j->dfr[k] || j->cold[k])   /* not emitted here */
        return false;
    const uam_insn *c = &blk->insn[k];
    return c->guard == t || (c->op == UAM_OP_SELECT && c->a == t && c->b != t && c->c != t);
}

/* ---- deferred register writes (docs/jit-dead-puts-design.md) ----------------------
 *
 * A PUT that a later unguarded PUT of the same block overwrites before anything reads
 * it is not done where it stands. Between the two the block can still stop at a soft
 * point - an IMARK's interrupt/deadline check, a load or store's slow path - and there
 * the register file must be exact, so the writes still pending are replayed on that
 * point's cold path (the IMARK stop stub, before the bridge call), in program order.
 * The inline data-TLB path cannot observe the register file, so it skips them. At a hard
 * point (an exit, a C call that may read the register file, a breakpoint) nothing is
 * pending. Only ISA helpers write the register file behind the block's back, and they
 * are hard points, so a replay never undoes a write made since. The interpreter does
 * every write in place; where the difference could be seen, the two agree. */

enum { OK_NONE, OK_PUT, OK_GET, OK_SOFT, OK_HARD };

static bool guard_false(const uam_block *blk, const uam_insn *in)
{
    return !uam_ref_is_none(in->guard) && uam_ref_is_imm(in->guard)
        && !(blk->imm[uam_ref_index(in->guard)] & 1);
}
bool jit_guard_temp(const uam_insn *in)
{
    return !uam_ref_is_none(in->guard) && !uam_ref_is_imm(in->guard);
}

/* A load or store that the code generator gives the inline data-TLB path. */
static bool mem_fast(const uam_block *blk, const uam_insn *in)
{
    if (in->flags & (UAM_ACC_X | UAM_ACC_PHYS | UAM_ACC_UNPRIV))
        return false;
    return !(uam_ref_is_imm(in->a)
             && jit_const_slow(blk->imm[uam_ref_index(in->a)] & uam_mask(blk->addr_width), in->width / 8,
                               in->flags));
}

/* What insn i is to the analysis. */
static uint8_t obs_kind(const uam_block *blk, const uam_insn *in, bool *first)
{
    if (guard_false(blk, in))
        return OK_NONE;   /* not emitted */
    switch ((uam_opcode)in->op) {
    case UAM_OP_PUT:
        return in->width >= 8 ? OK_PUT : OK_HARD;
    case UAM_OP_GET:
        return OK_GET;
    case UAM_OP_IMARK: {
        bool f = *first;
        *first = false;
        if (in->flags & UAM_IMARK_BREAK)
            return f ? OK_NONE : OK_HARD;
        return f || (in->flags & UAM_IMARK_CONT) ? OK_NONE : OK_SOFT;
    }
    case UAM_OP_LD:
    case UAM_OP_ST:
        return mem_fast(blk, in) ? OK_SOFT : OK_HARD;
    case UAM_OP_EXIT: case UAM_OP_TRAP: case UAM_OP_WAIT: case UAM_OP_SYSRD: case UAM_OP_SYSWR:
    case UAM_OP_HELPER: case UAM_OP_AMO: case UAM_OP_CAS:
        return OK_HARD;
    default:
        return OK_NONE;   /* pure ops, COV, FENCE */
    }
}

/* Bits of the bytes [daux, daux + dlen) that [aux, aux + len) covers. */
static uint8_t cover(uint32_t daux, unsigned dlen, uint32_t aux, unsigned len)
{
    uint8_t m = 0;
    for (unsigned k = 0; k < dlen; k++)
        if (daux + k >= aux && daux + k < aux + len)
            m |= (uint8_t)(1u << k);
    return m;
}

#define CLO_MAX 32u   /* cold ops one replay may recompute (more: they stay on the fast path) */

/* A pure op that can be left out of the fast path and recomputed by a replay: inline
 * (no C call), unguarded, with a result. */
static bool cold_eligible(const uam_jit *j, const uam_block *blk, uint32_t i)
{
    const uam_insn *in = &blk->insn[i];
    const uam_opcode op = (uam_opcode)in->op;
    const uam_opinfo *oi = &uam_opinfo_table[op];
    return !j->forced[i] && op != UAM_OP_GET && oi->result != UAM_RK_NONE
        && !(oi->props & (UAM_P_EFFECT | UAM_P_GUARD | UAM_P_FAULT)) && jit_host_pure_inline(j, op)
        && uam_ref_is_none(in->guard) && !uam_ref_is_none(in->dst) && !uam_ref_is_imm(in->dst);
}

/* Append insn c to soft point i's cold list, unless it is there already. */
static bool clo_add(uam_jit *j, uint32_t c, uint32_t tag, uint32_t *nc)
{
    if (j->stamp[c] == tag)
        return true;
    j->stamp[c] = tag;
    if (!jit_grow(j, &j->clo_list, &j->cap_clo, *nc + 1, sizeof(uint32_t)))
        return false;
    j->clo_list[(*nc)++] = c;
    return true;
}

/* The cold ops of the deferred writes (j->dfr): a pure op is cold if every reader is a
 * deferred PUT or another cold op, so the fast path never needs its value. Each soft
 * point recomputes, before its replays and in program order, the cold ops its pending
 * writes depend on (j->clo_start / clo_list). A soft point that would recompute more
 * than CLO_MAX of them keeps them all on the fast path instead, and the pass starts over
 * (keeping ops hot only shrinks the other lists). */
static bool cold_analyze(uam_jit *j, const uam_block *blk)
{
    const uint32_t n = blk->n_insn, nt = blk->n_temp;
    for (;;) {
        memset(j->bad, 0, sizeof(uint32_t) * nt);
        memset(j->nread, 0, sizeof(uint32_t) * nt);
        for (uint32_t i = 0; i < n; i++) {
            const uam_insn *in = &blk->insn[i];
            const uam_ref refs[4] = { in->guard, in->a, in->b, in->c };
            for (unsigned k = 0; k < 4; k++)
                if (!uam_ref_is_none(refs[k]) && !uam_ref_is_imm(refs[k])) {
                    j->nread[refs[k]]++;
                    if (!j->dfr[i])
                        j->bad[refs[k]]++;
                }
        }
        for (uint32_t i = n; i-- > 0;) {   /* readers come after: decided already */
            j->cold[i] = 0;
            if (!cold_eligible(j, blk, i))
                continue;
            const uam_insn *in = &blk->insn[i];
            if (!j->nread[in->dst] || j->bad[in->dst])
                continue;
            j->cold[i] = 1;
            const uam_ref refs[3] = { in->a, in->b, in->c };
            for (unsigned k = 0; k < 3; k++)
                if (!uam_ref_is_none(refs[k]) && !uam_ref_is_imm(refs[k]))
                    j->bad[refs[k]]--;
        }

        bool over = false;
        uint32_t nc = 0;
        memset(j->stamp, 0, sizeof(uint32_t) * n);
        for (uint32_t i = 0; i < n; i++) {
            j->clo_start[i] = nc;
            const uint32_t first = nc, tag = i + 1;
            for (uint32_t e = j->obs_start[i]; e < j->obs_start[i + 1]; e++) {
                const uam_insn *d = &blk->insn[j->obs_list[e]];
                const uam_ref refs[2] = { d->a, d->guard };
                for (unsigned k = 0; k < 2; k++)
                    if (!uam_ref_is_none(refs[k]) && !uam_ref_is_imm(refs[k]) && cold_temp(j, refs[k])
                        && !clo_add(j, j->def_of[refs[k]], tag, &nc))
                        return false;
            }
            for (uint32_t w = first; w < nc; w++) {   /* nc grows: the transitive closure */
                const uam_insn *c = &blk->insn[j->clo_list[w]];
                const uam_ref refs[3] = { c->a, c->b, c->c };
                for (unsigned k = 0; k < 3; k++)
                    if (!uam_ref_is_none(refs[k]) && !uam_ref_is_imm(refs[k]) && cold_temp(j, refs[k])
                        && !clo_add(j, j->def_of[refs[k]], tag, &nc))
                        return false;
            }
            for (uint32_t a = first + 1; a < nc; a++) {   /* program order (insertion sort) */
                const uint32_t v = j->clo_list[a];
                uint32_t b = a;
                for (; b > first && j->clo_list[b - 1] > v; b--)
                    j->clo_list[b] = j->clo_list[b - 1];
                j->clo_list[b] = v;
            }
            if (nc - first > CLO_MAX) {
                for (uint32_t w = first; w < nc; w++)
                    j->forced[j->clo_list[w]] = 1;
                over = true;
            }
        }
        j->clo_start[n] = nc;
        if (!over)
            return true;
    }
}

bool jit_defer_analyze(uam_jit *j, const uam_block *blk)
{
    const uint32_t n = blk->n_insn;
    if (n + 1 > j->cap_ins) {
        const uint32_t cap = n + 1;
        uint8_t *ok = realloc(j->okind, cap);
        if (ok) j->okind = ok;
        uint8_t *df = realloc(j->dfr, cap);
        if (df) j->dfr = df;
        uint32_t *os = realloc(j->obs_start, sizeof(uint32_t) * cap);
        if (os) j->obs_start = os;
        uint32_t *pe = realloc(j->pend, sizeof(uint32_t) * cap);
        if (pe) j->pend = pe;
        uint8_t *pl = realloc(j->pend_live, cap);
        if (pl) j->pend_live = pl;
        uint8_t *co = realloc(j->cold, cap);
        if (co) j->cold = co;
        uint8_t *fo = realloc(j->forced, cap);
        if (fo) j->forced = fo;
        uint32_t *cs = realloc(j->clo_start, sizeof(uint32_t) * cap);
        if (cs) j->clo_start = cs;
        uint32_t *st = realloc(j->stamp, sizeof(uint32_t) * cap);
        if (st) j->stamp = st;
        if (!ok || !df || !os || !pe || !pl || !co || !fo || !cs || !st)
            return false;
        j->cap_ins = cap;
    }
    const uint32_t nt = blk->n_temp;
    if (nt + 1 > j->cap_tmp) {
        const uint32_t cap = nt + 1;
        uint32_t *de = realloc(j->def_of, sizeof(uint32_t) * cap);
        if (de) j->def_of = de;
        uint32_t *ba = realloc(j->bad, sizeof(uint32_t) * cap);
        if (ba) j->bad = ba;
        uint32_t *nr = realloc(j->nread, sizeof(uint32_t) * cap);
        if (nr) j->nread = nr;
        if (!de || !ba || !nr)
            return false;
        j->cap_tmp = cap;
    }
    memset(j->dfr, 0, n);
    memset(j->cold, 0, n);
    memset(j->forced, 0, n);
    memset(j->obs_start, 0, sizeof(uint32_t) * (n + 1));
    memset(j->clo_start, 0, sizeof(uint32_t) * (n + 1));
    for (uint32_t t = 0; t < nt; t++)
        j->def_of[t] = NO_TEMP;
    for (uint32_t i = 0; i < n; i++)
        if (!uam_ref_is_none(blk->insn[i].dst) && !uam_ref_is_imm(blk->insn[i].dst))
            j->def_of[blk->insn[i].dst] = i;
    if (!j->defer)
        return true;

    /* classify, and size the byte map */
    bool first = true;
    uint32_t nb = 0;
    for (uint32_t i = 0; i < n; i++) {
        const uam_insn *in = &blk->insn[i];
        j->okind[i] = obs_kind(blk, in, &first);
        if ((j->okind[i] == OK_PUT || j->okind[i] == OK_GET) && in->aux + in->width / 8u > nb)
            nb = in->aux + in->width / 8u;
    }
    if (!jit_grow(j, &j->kbytes, &j->cap_kb, nb + 1, 1))
        return false;

    /* backward: kbytes = the bytes a later unguarded PUT overwrites before any read */
    memset(j->kbytes, 0, nb);
    for (uint32_t i = n; i-- > 0;) {
        const uam_insn *in = &blk->insn[i];
        const unsigned len = in->width / 8u;
        switch (j->okind[i]) {
        case OK_PUT: {
            bool all = true;
            for (unsigned b = 0; b < len; b++)
                all = all && j->kbytes[in->aux + b];
            j->dfr[i] = all;
            if (!jit_guard_temp(in))
                memset(j->kbytes + in->aux, 1, len);
            break;
        }
        case OK_GET:
            memset(j->kbytes + in->aux, 0, len);
            break;
        case OK_HARD:
            memset(j->kbytes, 0, nb);
            break;
        default:
            break;
        }
    }

    /* forward: the pending writes at each soft point. A write still pending where a PUT
     * done in place overlaps it would undo that PUT when replayed later: it is done in
     * place too (un-deferred), and the pass starts over until nothing changes. */
    for (;;) {
        bool changed = false;
        uint32_t np = 0, no = 0;
        for (uint32_t i = 0; i <= n; i++) {
            j->obs_start[i] = no;
            const uint8_t k = i < n ? j->okind[i] : OK_HARD;   /* the block's end */
            const uam_insn *in = i < n ? &blk->insn[i] : NULL;
            if (k == OK_PUT) {
                const unsigned len = in->width / 8u;
                const bool g = jit_guard_temp(in);
                uint32_t keep = 0;
                for (uint32_t p = 0; p < np; p++) {
                    const uam_insn *d = &blk->insn[j->pend[p]];
                    const uint8_t m = cover(d->aux, d->width / 8u, in->aux, len);
                    if (m && j->dfr[i]) {
                        if (!g)
                            j->pend_live[p] &= (uint8_t)~m;   /* replayed after: it wins */
                    } else if (m) {
                        if (g || (j->pend_live[p] & (uint8_t)~m)) {
                            j->dfr[j->pend[p]] = 0;          /* the overlap rule */
                            changed = true;
                        }
                        j->pend_live[p] = 0;
                    }
                    if (j->pend_live[p]) {
                        j->pend[keep] = j->pend[p];
                        j->pend_live[keep++] = j->pend_live[p];
                    }
                }
                np = keep;
                if (j->dfr[i]) {
                    j->pend[np] = i;
                    j->pend_live[np++] = (uint8_t)((1u << len) - 1);
                }
            } else if (k == OK_SOFT && np) {
                if (!jit_grow(j, &j->obs_list, &j->cap_obs, no + np, sizeof(uint32_t)))
                    return false;
                memcpy(j->obs_list + no, j->pend, sizeof(uint32_t) * np);
                no += np;
            } else if (k == OK_HARD) {
                for (uint32_t p = 0; p < np; p++) {   /* none by construction; to be safe */
                    j->dfr[j->pend[p]] = 0;
                    changed = true;
                }
                np = 0;
            }
        }
        if (!changed)
            break;
    }
    return cold_analyze(j, blk);
}

/* ---- the code arena ------------------------------------------------------------ */

static void *arena_map(void)
{
#if defined(__APPLE__)
    void *m = mmap(NULL, ARENA_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0);
#else
    void *m = mmap(NULL, ARENA_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
    return m == MAP_FAILED ? NULL : m;
}

/* Let this thread write the arena (on), or execute it again (off). */
static void arena_write(bool on)
{
#if defined(__APPLE__)
    pthread_jit_write_protect_np(!on);
#else
    (void)on;
#endif
}

/* Code was just written to [p, p + n): make instruction fetch see it. */
static void arena_sync(void *p, size_t n)
{
    __builtin___clear_cache((char *)p, (char *)p + n);
}

/* ---- public interface -------------------------------------------------- */

uam_jit *uam_jit_create(uam_exec *x)
{
    void *mem = arena_map();
    if (!mem)
        return NULL;
    uam_jit *j = calloc(1, sizeof(*j));
    if (!j) {
        munmap(mem, ARENA_SIZE);
        return NULL;
    }
    j->x = x;
    j->arena = mem;
    /* Host features and registers. UAM_JIT_BASELINE=1 turns optional features off (to
     * test the fallbacks). */
    if (!jit_host_init(j)) {
        munmap(mem, ARENA_SIZE);
        free(j);
        return NULL;
    }
    size_t stub_off[16];
    jit_host_stubs(j, stub_off);
    arena_write(true);
    memcpy(mem, j->cb, j->n);
    arena_write(false);
    arena_sync(mem, j->n);
    for (unsigned v = 0; v < 16; v++)
        j->jc.cstub[v] = (uint8_t *)mem + stub_off[v];
    j->arena_base = j->arena_used = (j->n + 15) & ~(size_t)15;
    j->jc.cpu = x->cpu;
    j->jc.regfile = x->cpu->regfile;
    j->jc.helpers = &x->cpu->isa->helpers;
    j->jc.sched = x->sched;
    j->jc.stats = &x->stats;
    /* UAM_JIT_REGS=N allocates temps to only N registers (0: none), to test spilling. */
    const char *nr = getenv("UAM_JIT_REGS");
    j->n_pool = j->pool_max;
    if (nr && *nr && strtoul(nr, NULL, 0) < j->pool_max)
        j->n_pool = (unsigned)strtoul(nr, NULL, 0);
    /* UAM_JIT_NODEFER=1 does every register write in place (no deferral to cold paths). */
    const char *nd = getenv("UAM_JIT_NODEFER");
    j->defer = !(nd && *nd && strcmp(nd, "0") != 0);
    /* UAM_JIT_JTAB_BITS=N uses only 2^N jump table entries (1..12), to test collisions. */
    const char *jb = getenv("UAM_JIT_JTAB_BITS");
    j->jtab_bits = JTAB_BITS;
    if (jb && *jb && strtoul(jb, NULL, 0) >= 1 && strtoul(jb, NULL, 0) < JTAB_BITS)
        j->jtab_bits = (unsigned)strtoul(jb, NULL, 0);
    for (unsigned k = 0; k < (1u << JTAB_BITS); k++)
        j->jc.jtab[k].epoch = LINK_NONE;
    /* Profiling output. Every JIT of the process appends to the same files. */
    const char *pm = getenv("UAM_PERF_MAP");
    if (pm && *pm && strcmp(pm, "0") != 0) {
        char path[64];
        snprintf(path, sizeof(path), "/tmp/perf-%d.map", (int)getpid());
        j->perf_map = fopen(path, "a");
        if (j->perf_map)
            fprintf(j->perf_map, "%llx %zx uam:code-store-stubs\n",
                    (unsigned long long)(uintptr_t)j->arena, j->arena_base);
    }
    const char *om = getenv("UAM_JIT_OPMAP");
    if (om && *om)
        j->opmap = fopen(om, "a");
    return j;
}

/* Describe the block just placed at `code` in the perf map and the op map. */
static void write_profile_maps(uam_jit *j, const uam_block *blk, const uint8_t *code)
{
    const char *isa = j->x->cpu->isa->name;
    if (j->perf_map) {
        fprintf(j->perf_map, "%llx %zx uam:%s:0x%llx\n", (unsigned long long)(uintptr_t)code,
                j->n, isa, (unsigned long long)blk->guest_pc);
        fflush(j->perf_map);
    }
    if (!j->opmap)
        return;
    fprintf(j->opmap, "B %llx %zx %llx %s\n", (unsigned long long)(uintptr_t)code, j->n,
            (unsigned long long)blk->guest_pc, isa);
    for (uint32_t k = 0; k < j->n_marks; k++) {
        const jit_opmark *m = &j->marks[k];
        fprintf(j->opmap, "M %x %s %u", m->off, mark_name[m->kind], m->idx);
        if (m->kind == MK_OP || m->kind == MK_SLOW) {
            const uam_insn *in = &blk->insn[m->idx];
            fprintf(j->opmap, " %s.%u", uam_opinfo_table[in->op].mnemonic, in->width);
            if (in->op == UAM_OP_GET || in->op == UAM_OP_PUT)
                fprintf(j->opmap, "@%u", in->aux);
            if (in->op == UAM_OP_IMARK)
                fprintf(j->opmap, " 0x%llx", (unsigned long long)blk->imm[uam_ref_index(in->a)]);
        }
        fputc('\n', j->opmap);
    }
    fflush(j->opmap);
}

void uam_jit_destroy(uam_jit *j)
{
    if (!j)
        return;
    munmap(j->arena, ARENA_SIZE);
    if (j->perf_map)
        fclose(j->perf_map);
    if (j->opmap)
        fclose(j->opmap);
    free(j->marks);
    free(j->sites);
    free(j->site_refs);
    free(j->stubs);
    free(j->okind);
    free(j->dfr);
    free(j->obs_start);
    free(j->obs_list);
    free(j->pend);
    free(j->pend_live);
    free(j->kbytes);
    free(j->cold);
    free(j->forced);
    free(j->clo_start);
    free(j->clo_list);
    free(j->stamp);
    free(j->def_of);
    free(j->bad);
    free(j->nread);
    free(j->snaps);
    free(j->rd);
    free(j->reg_of);
    free(j->in_mem);
    free(j->last_use);
    free(j->use_start);
    free(j->use_cur);
    free(j->use_pos);
    free(j);
}

/* The common start of every block's compilation: no slots, sites, stubs or marks yet,
 * the site at the block's first instruction. */
static void compile_reset(uam_jit *j, const uam_block *blk)
{
    j->n = 0;
    j->overflow = false;
    j->n_slots = 0;
    j->n_marks = 0;
    j->n_sites = j->n_site_refs = j->n_stubs = j->n_snaps = 0;
    j->flag_temp = UAM_REF_NONE;
    j->cur = (jit_site){ blk->guest_pc, blk->guest_pc, 0 };
    j->cur_emitted = false;
}

bool uam_jit_compile(uam_jit *j, uam_block *blk)
{
    compile_reset(j, blk);
    if (!jit_host_compile(j, blk) || j->overflow) {
        blk->backend = UAM_JIT_INTERP;
        return true;
    }
    if (j->n > ARENA_SIZE - j->arena_base)
        return false;   /* a single block larger than the arena: impossible in practice */
    if (j->arena_used + j->n > ARENA_SIZE) {
        /* Arena full: drop every compiled block (and every link), then restart this one. */
        uam_tcache_flush(&j->x->tc);
        j->arena_used = j->arena_base;   /* the shared stubs stay */
        j->arena_gen++;
        if (j->opmap)
            fputs("R\n", j->opmap);   /* the addresses below are reused from here on */
    }
    uint8_t *code = j->arena + j->arena_used;
    arena_write(true);
    memcpy(code, j->cb, j->n);
    arena_write(false);
    arena_sync(code, j->n);
    j->x->stats.code_bytes += j->n;
    write_profile_maps(j, blk, code);
    j->arena_used += (j->n + 15) & ~(size_t)15;   /* keep 16-byte alignment */
    blk->backend = code;
    blk->backend_free = NULL;                      /* bulk-freed with the arena */
    return true;
}

void uam_jit_run(uam_jit *j, const uam_block *blk, uint64_t *temps, uam_jit_chain *chain,
                 uam_block_result *res)
{
    uam_jit_ctx *jc = &j->jc;
    uam_exec *x = j->x;
    uam_cpu *cpu = x->cpu;

    /* The data-TLB holds translations made at one privilege level: translation
     * permissions depend on it (a user-mode access to a kernel page must fault). */
    if (cpu->priv != j->key_priv)
        uam_cpu_dtlb_flush(cpu);
    /* A linked block may have become invalid since the epoch was set: kill all links. */
    if (!j->key_valid || x->tc.gen != j->key_tc || cpu->xlate_gen != j->key_xgen
        || cpu->priv != j->key_priv) {
        jc->epoch++;
        j->key_valid = true;
        j->key_tc = x->tc.gen;
        j->key_xgen = cpu->xlate_gen;
        j->key_priv = cpu->priv;
    }
    /* The last native block's EXIT missed its link and led here: link it. The run loop
     * just validated blk for the current epoch and has nothing to do before entering it
     * that it would not also check at the EXIT (no hook on blk: chain->link). The slot
     * may be in the arena (a block's own) or in jc (the jump table). */
    if (j->pend_slot) {
        if (chain->link && j->pend_arena == j->arena_gen && blk->guest_pc == j->pend_pc) {
            arena_write(true);
            j->pend_slot->entry = (uint64_t)(uintptr_t)blk->backend + j->body_off;
            j->pend_slot->pc = blk->guest_pc;
            j->pend_slot->epoch = jc->epoch;
            arena_write(false);
        }
        j->pend_slot = NULL;
    }

    jc->temps = temps;   /* the executor may grow it between runs */
    jc->chain_left = jc->chain_budget = chain->budget;
    jc->link_slot = NULL;
    jc->dgen = cpu->bus->direct_gen;
    jc->priv = cpu->priv;
    jc->next_when = uam_sched_next(x->sched);
    cpu->icount_stop = cpu->exit_request ? 0 : cpu->icount_deadline;
    ((void (*)(uam_jit_ctx *))blk->backend)(jc);
    *res = jc->res;
    chain->hops = jc->chain_budget - jc->chain_left;
    if (jc->link_slot && res->kind == UAM_BR_NEXT) {
        j->pend_slot = jc->link_slot;
        j->pend_pc = res->pc;
        j->pend_arena = j->arena_gen;
    }
}

#endif /* UAM_JIT_HOST */
