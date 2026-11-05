/*
 * soc_reset.h - <SoC> reset/clock block: chip reset, clock-status polls.
 */
#ifndef SOC_RESET_H
#define SOC_RESET_H

#include "uam/access.h"
#include "uam/device.h"
#include "uam/signal.h"

#define SOC_RESET_SIZE 0x100u

uam_device *soc_reset_create(const char *name);
/* Raised when the guest resets the chip. Board: uam_signal_connect(line,
 * uam_machine_reset_sink, m, UAM_RESET_SYSTEM). */
uam_signal *soc_reset_line(uam_device *dev);

#endif
