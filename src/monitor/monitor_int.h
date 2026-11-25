/*
 * monitor_int.h - Between the monitor's engine (monitor.c) and its command groups.
 */
#ifndef UAM_MONITOR_INT_H
#define UAM_MONITOR_INT_H

#include "uam/frames.h"
#include "uam/monitor.h"

/* monitor_inspect.c: regs, reg, x, write, load, map, history, bt, dump */
uam_status       mon_add_inspect_commands(uam_monitor *mon);
/* The machine is not running (stopped by the monitor, or ended). */
bool             mon_stopped(const uam_monitor *mon);
/* What names an address for "bt" (uam_monitor_set_symbolizer), or NULL. */
uam_symbolize_fn mon_symbolizer(const uam_monitor *mon, void **ctx);

#endif /* UAM_MONITOR_INT_H */
