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

/* LD (section 6.2). On success sets *out and, for an exclusive load, the reservation. */
static inline uam_status uam_execops_load(uam_cpu *cpu, uint64_t addr, unsigned flags,
                                          unsigned size, uint64_t *out, uint32_t *xfault)
{
    uint64_t pa;
    uam_status st = uam_execops_xlate(cpu, addr, UAM_XLATE_LOAD, flags, size, &pa, xfault);
    if (st == UAM_OK)
        st = uam_bus_read(cpu->bus, pa, size, flags, out);
    if (st != UAM_OK)
        return st;
    if (flags & UAM_ACC_X)
        cpu->resv = (uam_reservation){ .valid = true, .addr = addr, .size = size };
    return UAM_OK;
}

/* Plain ST (section 6.2). */
static inline uam_status uam_execops_store(uam_cpu *cpu, uint64_t addr, uint64_t val,
                                           unsigned flags, unsigned size, uint32_t *xfault)
{
    uint64_t pa;
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
                                             uint32_t *xfault)
{
    if ((flags & UAM_ACC_ALIGNED) && (addr & (size - 1)))
        return UAM_ERR_MISALIGNED;
    bool held = uam_execops_resv_covers(&cpu->resv, addr, size);
    cpu->resv.valid = false;
    *ok = held;
    if (!held)
        return UAM_OK;
    return uam_execops_store(cpu, addr, val, flags, size, xfault);
}

#endif /* UAM_EXECOPS_H */
