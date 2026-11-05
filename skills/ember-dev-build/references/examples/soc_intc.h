/*
 * soc_intc.h - <SoC> cascaded interrupt controller: 32 inputs, one output.
 */
#ifndef SOC_INTC_H
#define SOC_INTC_H

#include "uam/access.h"
#include "uam/device.h"
#include "uam/signal.h"

#define SOC_INTC_SIZE 0x8u

uam_device *soc_intc_create(const char *name);
/* Signal sink for the inputs: uam_signal_connect(&dev_irq, soc_intc_input, intc, bit). */
void        soc_intc_input(void *intc, unsigned n, int level);
/* The output: connect it to a CPU line (uam_cpu_irq_sink) or to a parent controller. */
uam_signal *soc_intc_output(uam_device *dev);

#endif
