/*
 * soc_timer.h - <SoC> free-running timer with a compare-match interrupt.
 */
#ifndef SOC_TIMER_H
#define SOC_TIMER_H

#include "uam/access.h"
#include "uam/device.h"
#include "uam/sched.h"
#include "uam/signal.h"

#define SOC_TIMER_SIZE 0x10u

/* ticks_per_count: virtual ticks (= guest instructions) per counter increment. The board
 * chooses it from the counter's clock and its own ticks-per-microsecond. */
uam_device *soc_timer_create(const char *name, uam_sched *sched, uint64_t ticks_per_count);
uam_signal *soc_timer_irq_line(uam_device *dev);

#endif
