/*
 * uam/dev/gic.h - A GICv2-architecture interrupt controller for one to eight
 * CPUs. The identification registers name no implementer (IIDR 0; GICC_IIDR
 * reports architecture version 2).
 *
 * Two register frames, each its own device: the distributor and the CPU
 * interface. Interrupt ids 0-15 are SGIs (raised through GICD_SGIR), 16-31 PPIs
 * and 32 up SPIs, fed through uam_gic_input(). One group (no security
 * extensions), priorities in 8 bits with preemption by the full priority value
 * (GICC_BPR is stored but not used). Each CPU has an output line, its IRQ.
 *
 * With several CPUs, ids 0-31 are banked per CPU (enable, pending, active,
 * priority, configuration; their PPIs are fed through uam_gic_ppi_input), the
 * CPU interface is banked, and an access goes to the bank of the CPU making it,
 * which the `who` callback names (uam_machine_current_cpu). SGIs remember their
 * senders (GICC_IAR reports the source CPU in bits 10-12; GICD_SGIR's target
 * list, all-but-self and self filters all work), and SPIs reach the CPUs in
 * their ITARGETSR byte (0 at reset: firmware routes them).
 *
 *   distributor (UAM_GIC_DIST_SIZE)       CPU interface (UAM_GIC_CPU_SIZE)
 *   0x000 CTLR    0x004 TYPER  0x008 IIDR  0x00 CTLR  0x04 PMR   0x08 BPR
 *   0x080 IGROUPR (RAZ/WI)                 0x0C IAR   0x10 EOIR  0x14 RPR
 *   0x100 ISENABLER   0x180 ICENABLER      0x18 HPPIR 0xFC IIDR
 *   0x200 ISPENDR     0x280 ICPENDR
 *   0x300 ISACTIVER   0x380 ICACTIVER
 *   0x400 IPRIORITYR (bytes)  0x800 ITARGETSR (bytes; ids 0-31 read as the reader)
 *   0xC00 ICFGR   0xF00 SGIR
 */
#ifndef UAM_DEV_GIC_H
#define UAM_DEV_GIC_H

#include "uam/device.h"
#include "uam/signal.h"

#define UAM_GIC_DIST_SIZE 0x1000u
#define UAM_GIC_CPU_SIZE  0x1000u
#define UAM_GIC_MAX_IRQS  992u   /* ids 1020-1023 are special */
#define UAM_GIC_MAX_CPUS  8u

/* n_irqs: a multiple of 32, from 64 up to UAM_GIC_MAX_IRQS. One CPU. */
uam_device *uam_gic_create(const char *name, unsigned n_irqs);
/* n_cpus CPUs; who(who_ctx) is the CPU making an access (required with more than one). */
uam_device *uam_gic_create_cpus(const char *name, unsigned n_irqs, unsigned n_cpus,
                                unsigned (*who)(void *ctx), void *who_ctx);
/* The CPU interface frame of a GIC (map it as its own region). */
uam_device *uam_gic_cpu_iface(uam_device *gic);
/* The IRQ output of CPU 0 / of CPU `cpu` (connect to that CPU's IRQ input). */
uam_signal *uam_gic_irq_line(uam_device *gic);
uam_signal *uam_gic_cpu_irq_line(uam_device *gic, unsigned cpu);
/* Signal sink for interrupt inputs (SPIs, and CPU 0's PPIs, by id):
 * uam_signal_connect(&dev_irq, uam_gic_input, gic, id). */
void        uam_gic_input(void *gic, unsigned id, int level);
/* Signal sink for a CPU's PPIs: input = 32 * cpu + id. */
void        uam_gic_ppi_input(void *gic, unsigned input, int level);

#endif /* UAM_DEV_GIC_H */
