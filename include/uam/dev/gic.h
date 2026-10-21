/*
 * uam/dev/gic.h - A GICv2-style interrupt controller for one CPU.
 *
 * Two register frames, each its own device: the distributor and the CPU
 * interface. Interrupt ids 0-15 are SGIs (raised through GICD_SGIR), 16-31 PPIs
 * and 32 up SPIs, fed through uam_gic_input(). One group (no security
 * extensions), one CPU, priorities in 8 bits with preemption by the full priority
 * value (GICC_BPR is stored but not used). The output line is the CPU's IRQ.
 *
 *   distributor (UAM_GIC_DIST_SIZE)       CPU interface (UAM_GIC_CPU_SIZE)
 *   0x000 CTLR    0x004 TYPER  0x008 IIDR  0x00 CTLR  0x04 PMR   0x08 BPR
 *   0x080 IGROUPR (RAZ/WI)                 0x0C IAR   0x10 EOIR  0x14 RPR
 *   0x100 ISENABLER   0x180 ICENABLER      0x18 HPPIR 0xFC IIDR
 *   0x200 ISPENDR     0x280 ICPENDR
 *   0x300 ISACTIVER   0x380 ICACTIVER
 *   0x400 IPRIORITYR (bytes)  0x800 ITARGETSR (bytes, read as CPU 0)
 *   0xC00 ICFGR   0xF00 SGIR
 */
#ifndef UAM_DEV_GIC_H
#define UAM_DEV_GIC_H

#include "uam/device.h"
#include "uam/signal.h"

#define UAM_GIC_DIST_SIZE 0x1000u
#define UAM_GIC_CPU_SIZE  0x1000u
#define UAM_GIC_MAX_IRQS  992u   /* ids 1020-1023 are special */

/* n_irqs: a multiple of 32, from 64 up to UAM_GIC_MAX_IRQS. */
uam_device *uam_gic_create(const char *name, unsigned n_irqs);
/* The CPU interface frame of a GIC (map it as its own region). */
uam_device *uam_gic_cpu_iface(uam_device *gic);
/* The IRQ output (connect to the CPU's IRQ input). */
uam_signal *uam_gic_irq_line(uam_device *gic);
/* Signal sink for interrupt inputs (PPIs and SPIs, by id):
 * uam_signal_connect(&dev_irq, uam_gic_input, gic, id). */
void        uam_gic_input(void *gic, unsigned id, int level);

#endif /* UAM_DEV_GIC_H */
