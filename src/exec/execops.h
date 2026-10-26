/*
 * execops.h - Memory-access semantics shared by every execution backend.
 *
 * The interpreter (interp.c) and the JIT's memory bridges (jit_x86.c) both go through
 * these inline helpers, so a load or store means exactly one thing no matter which
 * backend runs the block. They perform address translation (isa->translate, honouring
 * alignment and UAM_ACC_PHYS) and the bus access, and maintain the exclusive-access
 * reservation. See docs/uam-isa.md sections 6 and 9.
 *
 * Internal to the core; not a public header.
 */
#ifndef UAM_EXECOPS_H
#define UAM_EXECOPS_H

#include "uam/bus.h"
#include "uam/cpu.h"
#include "uam/exec.h"
#include "uam/isa.h"

/* Exception kind for a failed data access (translation, alignment or bus). */
static inline uam_exc_kind uam_execops_fault_kind(uam_status st, bool store)
{
    if (st == UAM_ERR_MISALIGNED)
        return store ? UAM_EXC_STORE_MISALIGNED : UAM_EXC_LOAD_MISALIGNED;
    if (st == UAM_ERR_XLATE)
        return store ? UAM_EXC_STORE_XLATE : UAM_EXC_LOAD_XLATE;
    return store ? UAM_EXC_STORE_FAULT : UAM_EXC_LOAD_FAULT;
}

static inline bool uam_execops_resv_covers(const uam_reservation *r, uint64_t addr, unsigned size)
{
    return r->valid && addr >= r->addr && addr + size <= r->addr + r->size;
}

/*
 * Translate a data address. Alignment is checked before translation (architectures
 * raise address-misaligned in preference to a translation fault). Returns the physical
 * address, or a non-OK status with the ISA's fault sub-cause in *xfault.
 */
static inline uam_status uam_execops_xlate(uam_cpu *cpu, uint64_t vaddr, uam_xlate_kind kind,
                                           unsigned flags, unsigned size, uint64_t *paddr,
                                           uint32_t *xfault)
{
    if ((flags & UAM_ACC_ALIGNED) && (vaddr & (size - 1)))
        return UAM_ERR_MISALIGNED;
    if (!cpu->isa->translate || (flags & UAM_ACC_PHYS)) {
        *paddr = vaddr;
        return UAM_OK;
    }
    uam_xlate x = { 0 };
    uam_status st = cpu->isa->translate(cpu, vaddr, kind, &x);
    *paddr = x.paddr;
    *xfault = x.fault;
    return st;
}

/*
 * An access that crosses a page boundary under address translation: the two pages may
 * map to unrelated physical pages, so both ends are translated and, unless they turn out
 * contiguous, the bytes are moved one at a time (in the access's byte order). A
 * translation fault on the second page reports that page's first byte (*fault_va), the
 * address a fault handler must make accessible.
 */
static inline bool uam_execops_crosses(const uam_cpu *cpu, uint64_t addr, unsigned size,
                                       unsigned flags)
{
    return size > 1 && cpu->isa->translate && !(flags & UAM_ACC_PHYS)
        && ((addr ^ (addr + size - 1)) & UAM_PAGE_MASK);
}

/* Translate the bytes of a page-crossing access: pa[0] for its first byte, pa[1] for the
 * first byte of the second page; n0 bytes lie on the first page. */
static inline uam_status uam_execops_xlate2(uam_cpu *cpu, uint64_t addr, uam_xlate_kind kind,
                                            unsigned flags, unsigned size, uint64_t pa[2],
                                            unsigned *n0, uint32_t *xfault, uint64_t *fault_va)
{
    uint64_t second = (addr + size - 1) & UAM_PAGE_MASK;
    *n0 = (unsigned)(second - addr);
    uam_status st = uam_execops_xlate(cpu, addr, kind, flags, size, &pa[0], xfault);
    if (st == UAM_OK) {
        st = uam_execops_xlate(cpu, second, kind, flags & ~UAM_ACC_ALIGNED, 1, &pa[1], xfault);
        if (st != UAM_OK)
            *fault_va = second;
    }
    return st;
}

/* Byte k of a size-byte value in the access's byte order (k = 0: lowest address). */
static inline unsigned uam_execops_byte_shift(unsigned flags, unsigned size, unsigned k)
{
    return 8 * ((flags & UAM_ACC_BE) ? size - 1 - k : k);
}

/* LD (section 6.2). On success sets *out and, for an exclusive load, the reservation.
 * On failure *fault_va is the faulting address (addr, except as above). */
static inline uam_status uam_execops_load(uam_cpu *cpu, uint64_t addr, unsigned flags,
                                          unsigned size, uint64_t *out, uint32_t *xfault,
                                          uint64_t *fault_va)
{
    uint64_t pa;
    uam_status st;
    *fault_va = addr;
    if (uam_execops_crosses(cpu, addr, size, flags)) {
        uint64_t p[2];
        unsigned n0;
        st = uam_execops_xlate2(cpu, addr, UAM_XLATE_LOAD, flags, size, p, &n0, xfault, fault_va);
        if (st != UAM_OK)
            return st;
        if (p[1] == p[0] + n0) {
            pa = p[0];
            st = uam_bus_read(cpu->bus, pa, size, flags, out);
        } else {
            uint64_t v = 0;
            for (unsigned k = 0; k < size && st == UAM_OK; k++) {
                uint64_t b = 0;
                st = uam_bus_read(cpu->bus, k < n0 ? p[0] + k : p[1] + (k - n0), 1, flags, &b);
                v |= b << uam_execops_byte_shift(flags, size, k);
            }
            *out = v;
        }
    } else {
        st = uam_execops_xlate(cpu, addr, UAM_XLATE_LOAD, flags, size, &pa, xfault);
        if (st == UAM_OK)
            st = uam_bus_read(cpu->bus, pa, size, flags, out);
    }
    if (st != UAM_OK)
        return st;
    if (flags & UAM_ACC_X)
        cpu->resv = (uam_reservation){ .valid = true, .addr = addr, .size = size };
    return UAM_OK;
}

/* Plain ST (section 6.2). */
static inline uam_status uam_execops_store(uam_cpu *cpu, uint64_t addr, uint64_t val,
                                           unsigned flags, unsigned size, uint32_t *xfault,
                                           uint64_t *fault_va)
{
    uint64_t pa;
    *fault_va = addr;
    if (uam_execops_crosses(cpu, addr, size, flags)) {
        uint64_t p[2];
        unsigned n0;
        uam_status st = uam_execops_xlate2(cpu, addr, UAM_XLATE_STORE, flags, size, p, &n0,
                                           xfault, fault_va);
        if (st != UAM_OK || p[1] == p[0] + n0)
            return st != UAM_OK ? st : uam_bus_write(cpu->bus, p[0], size, flags, val);
        for (unsigned k = 0; k < size && st == UAM_OK; k++)
            st = uam_bus_write(cpu->bus, k < n0 ? p[0] + k : p[1] + (k - n0), 1, flags,
                               (val >> uam_execops_byte_shift(flags, size, k)) & 0xFF);
        return st;
    }
    uam_status st = uam_execops_xlate(cpu, addr, UAM_XLATE_STORE, flags, size, &pa, xfault);
    if (st == UAM_OK)
        st = uam_bus_write(cpu->bus, pa, size, flags, val);
    return st;
}

/*
 * Exclusive ST (store-conditional, section 6.3). On UAM_OK, *ok is 1 if the store was
 * performed (reservation held) and 0 otherwise; the reservation is always cleared.
 * A non-OK return is a precise fault (misalignment, translation or bus).
 */
static inline uam_status uam_execops_store_x(uam_cpu *cpu, uint64_t addr, uint64_t val,
                                             unsigned flags, unsigned size, bool *ok,
                                             uint32_t *xfault, uint64_t *fault_va)
{
    *fault_va = addr;
    if ((flags & UAM_ACC_ALIGNED) && (addr & (size - 1)))
        return UAM_ERR_MISALIGNED;
    bool held = uam_execops_resv_covers(&cpu->resv, addr, size);
    cpu->resv.valid = false;
    *ok = held;
    if (!held)
        return UAM_OK;
    return uam_execops_store(cpu, addr, val, flags, size, xfault, fault_va);
}

/* The new value an AMO stores, given the old memory value and the operand. */
static inline uint64_t uam_execops_amo_apply(uam_amo_op op, uint64_t old, uint64_t v, unsigned W)
{
    int64_t so = uam_sext(old, W), sv = uam_sext(v, W);
    switch (op) {
    case UAM_AMO_XCHG: return v;
    case UAM_AMO_ADD:  return (old + v) & uam_mask(W);
    case UAM_AMO_AND:  return old & v;
    case UAM_AMO_OR:   return old | v;
    case UAM_AMO_XOR:  return old ^ v;
    case UAM_AMO_SMIN: return so < sv ? old : v;
    case UAM_AMO_SMAX: return so > sv ? old : v;
    case UAM_AMO_UMIN: return old < v ? old : v;
    case UAM_AMO_UMAX: return old > v ? old : v;
    case UAM_AMO__COUNT: break;
    }
    return v;
}

/*
 * AMO / CAS (section 6.4): read-modify-write of one location. For an AMO, b is the
 * operand and sub the operation; for a CAS (cas = true), b is the expected value and
 * c the new one, written only on a match. *old receives the memory value. A fault on
 * either half is reported as a store fault, as architectures do for atomics.
 */
static inline uam_status uam_execops_rmw(uam_cpu *cpu, bool cas, uam_amo_op sub, uint64_t addr,
                                         uint64_t b, uint64_t c, unsigned flags, unsigned W,
                                         uint64_t *old, uint32_t *xfault)
{
    const unsigned size = W / 8;
    uint64_t pa;
    *old = 0;
    uam_status st = uam_execops_xlate(cpu, addr, UAM_XLATE_STORE, flags, size, &pa, xfault);
    if (st == UAM_OK)
        st = uam_bus_read(cpu->bus, pa, size, flags, old);
    if (st != UAM_OK)
        return st;
    if (cas && *old != b)
        return UAM_OK;
    uint64_t nv = cas ? c : uam_execops_amo_apply(sub, *old, b, W);
    *xfault = 0;
    return uam_bus_write(cpu->bus, pa, size, flags, nv);
}

#endif /* UAM_EXECOPS_H */
