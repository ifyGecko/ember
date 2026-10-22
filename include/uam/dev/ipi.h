/*
 * uam/dev/ipi.h - Inter-processor interrupt doorbells: one interrupt line per
 * processor, raised and lowered by any processor through memory-mapped
 * registers. For machines whose architecture defines no interrupt controller
 * that can interrupt another processor.
 *
 * Register map (offsets from base), 32 bits each, for processor n:
 *   0x000 + 4*n  processor n's line: any non-zero write raises it, zero lowers
 *                it (so the byte order of the access does not matter); reads
 *                give 1 while it is raised
 */
#ifndef UAM_DEV_IPI_H
#define UAM_DEV_IPI_H

#include "uam/device.h"
#include "uam/signal.h"

#define UAM_IPI_SIZE     0x1000u
#define UAM_IPI_MAX_CPUS 1024u

uam_device *uam_ipi_create(const char *name, unsigned n_cpus);
/* Processor n's line (NULL beyond the last). */
uam_signal *uam_ipi_line(uam_device *dev, unsigned n);

#endif /* UAM_DEV_IPI_H */
