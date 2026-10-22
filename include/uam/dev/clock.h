/*
 * uam/dev/clock.h - Converting virtual time to a device clock.
 *
 * Virtual time (sched.h) counts ticks; a board decides what a tick is worth
 * by giving its timer devices `tick_hz`, the number of ticks per emulated
 * second (e.g. 1000000000: one instruction is a nanosecond). A device with
 * its own input clock (an 8254 at 1.193182 MHz, an ACPI timer at 3.579545
 * MHz) converts with these helpers, exactly and without drift.
 */
#ifndef UAM_DEV_CLOCK_H
#define UAM_DEV_CLOCK_H

#include "uam/common.h"

/* Device-clock cycles elapsed by virtual tick `t`. */
static inline uint64_t uam_clock_from_ticks(uint64_t t, uint64_t tick_hz, uint64_t dev_hz)
{
    return (uint64_t)((unsigned __int128)t * dev_hz / tick_hz);
}

/* The first virtual tick at which `cycles` device-clock cycles have elapsed;
 * UINT64_MAX (never) when that lies beyond the range of virtual time. */
static inline uint64_t uam_clock_to_ticks(uint64_t cycles, uint64_t tick_hz, uint64_t dev_hz)
{
    unsigned __int128 t = ((unsigned __int128)cycles * tick_hz + dev_hz - 1) / dev_hz;
    return t > UINT64_MAX ? UINT64_MAX : (uint64_t)t;
}

#endif /* UAM_DEV_CLOCK_H */
