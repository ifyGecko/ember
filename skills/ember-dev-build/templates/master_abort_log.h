/*
 * master_abort_log.h - a PC's unclaimed-address behaviour (reads all ones, writes dropped,
 * as ember's uam_master_abort) that also says so: the first access to each address is
 * logged, and the hottest 64 KiB pages are printed at exit (see master_abort_log.c).
 */
#ifndef MASTER_ABORT_LOG_H
#define MASTER_ABORT_LOG_H

#include <stdbool.h>

#include "uam/device.h"

/* Install with uam_bus_set_unmapped(); it sees absolute addresses. log = false is the
 * silent uam_master_abort behaviour (the board's -o abort-log=0). */
uam_device *master_abort_log_create(const char *name, bool log);

#endif
