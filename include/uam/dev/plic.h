/*
 * uam/dev/plic.h - RISC-V Platform-Level Interrupt Controller (RISC-V PLIC
 * specification).
 *
 * Level-triggered sources 1..n_sources-1 feed per-context outputs. Register
 * map (offsets from base):
 *   0x000000 + 4*src          priority
 *   0x001000 + 4*(src/32)     pending bits (read-only)
 *   0x002000 + 0x80*ctx       enable bits
 *   0x200000 + 0x1000*ctx     threshold
 *   0x200004 + 0x1000*ctx     claim (read) / complete (write)
 */
#ifndef UAM_DEV_PLIC_H
#define UAM_DEV_PLIC_H

#include "uam/device.h"
#include "uam/signal.h"

#define UAM_PLIC_SIZE        0x400000u
#define UAM_PLIC_MAX_SOURCES 1024u
#define UAM_PLIC_MAX_CONTEXTS 16u

uam_device *uam_plic_create(const char *name, unsigned n_sources, unsigned n_contexts);
/* Output line of a context (connect to a hart's external interrupt input). */
uam_signal *uam_plic_context_line(uam_device *dev, unsigned ctx);
/* Signal sink for source inputs: uam_signal_connect(&dev_irq, uam_plic_input, plic, src). */
void        uam_plic_input(void *plic, unsigned src, int level);

#endif /* UAM_DEV_PLIC_H */
