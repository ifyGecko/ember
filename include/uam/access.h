/*
 * uam/access.h - Memory access attributes.
 *
 * Shared vocabulary between the IR (memory-op flags, docs/uam-isa.md section 6.1)
 * and the bus interface. Kept in its own header so that neither ir.h nor
 * bus.h depends on the other.
 */
#ifndef UAM_ACCESS_H
#define UAM_ACCESS_H

enum uam_access_attr {
    /* Attributes that may appear on IR memory ops. */
    UAM_ACC_LE      = 1u << 0, /* little-endian byte order */
    UAM_ACC_BE      = 1u << 1, /* big-endian byte order */
    UAM_ACC_ALIGNED = 1u << 2, /* misaligned address faults instead of splitting */
    UAM_ACC_X       = 1u << 3, /* exclusive (load-reserved / store-conditional) */
    UAM_ACC_ACQ     = 1u << 4, /* acquire ordering */
    UAM_ACC_REL     = 1u << 5, /* release ordering */
    UAM_ACC_UNPRIV  = 1u << 6, /* perform at user privilege */
    UAM_ACC_PHYS    = 1u << 7, /* bypass address translation */

    /* Attributes set only by the core, never by lifted code. */
    UAM_ACC_FETCH   = 1u << 8, /* instruction fetch */
    UAM_ACC_DEBUG   = 1u << 9, /* side-effect-free peek/poke (loader, debugger) */
};

/* Attributes a lifter is allowed to place on an IR memory op. */
#define UAM_ACC_IR_MASK 0x00FFu

#endif /* UAM_ACCESS_H */
