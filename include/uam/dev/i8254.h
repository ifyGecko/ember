/*
 * uam/dev/i8254.h - 8254 programmable interval timer (three counters at
 * 1.193182 MHz) and the PC's port-61h control/status register, which gates
 * counter 2 and reads back its output.
 *
 * Views, mapped by the board:
 *   uam_i8254_ports(dev)   4 bytes: counters 0-2, control word   PC: 0x40-0x43
 *   uam_i8254_port61(dev)  1 byte                                PC: 0x61
 *
 * Counter 0's output is the IRQ 0 line.
 */
#ifndef UAM_DEV_I8254_H
#define UAM_DEV_I8254_H

#include "uam/device.h"
#include "uam/sched.h"
#include "uam/signal.h"

#define UAM_I8254_HZ 1193182u

uam_device *uam_i8254_create(const char *name, uam_sched *sched, uint64_t tick_hz);
uam_device *uam_i8254_ports(uam_device *dev);   /* the device itself */
uam_device *uam_i8254_port61(uam_device *dev);
uam_signal *uam_i8254_irq_line(uam_device *dev);

#endif /* UAM_DEV_I8254_H */
