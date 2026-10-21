/*
 * uam/dev/i8259.h - Cascaded pair of 8259A programmable interrupt controllers
 * with the edge/level control registers (ELCR) of PC chipsets.
 *
 * Three register views, each mapped by the board:
 *   uam_i8259_master(dev)   2 bytes (command, data)   PC: ports 0x20-0x21
 *   uam_i8259_slave(dev)    2 bytes                   PC: ports 0xA0-0xA1
 *   uam_i8259_elcr(dev)     2 bytes (ELCR1, ELCR2)    PC: ports 0x4D0-0x4D1
 *
 * Inputs 0-15 (8-15 on the slave, cascaded on master input 2). The INTR output
 * goes to the processor (or the local APIC's LINT0); the processor's interrupt
 * acknowledge cycle is uam_i8259_inta.
 */
#ifndef UAM_DEV_I8259_H
#define UAM_DEV_I8259_H

#include "uam/device.h"
#include "uam/signal.h"

uam_device *uam_i8259_create(const char *name);
uam_device *uam_i8259_master(uam_device *dev);   /* the device itself */
uam_device *uam_i8259_slave(uam_device *dev);
uam_device *uam_i8259_elcr(uam_device *dev);
uam_signal *uam_i8259_intr_line(uam_device *dev);
/* Signal sink for interrupt inputs 0-15. */
void        uam_i8259_input(void *pic, unsigned irq, int level);
/* Interrupt acknowledge: the vector of the highest-priority request (spurious
 * IRQ 7 / 15 if it went away), which moves to in-service. */
int         uam_i8259_inta(uam_device *dev);

#endif /* UAM_DEV_I8259_H */
