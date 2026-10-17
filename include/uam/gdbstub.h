/*
 * uam/gdbstub.h - gdb remote serial protocol server for one machine.
 *
 * Built on core services only: PC hooks (software/hardware breakpoints),
 * bus watchpoints (watch/rwatch/awatch, translated to physical addresses when
 * set), single-stepping by instruction budget, and the ISA's register view
 * (uam_isa.gdb, with a target description so gdb needs no configuration).
 *
 *     uam_gdb *g = uam_gdb_create(&m);
 *     uam_gdb_listen(g, "1234");              blocks until gdb connects
 *     uam_stop_reason why = uam_gdb_serve(g, 0);
 *     uam_gdb_destroy(g);
 *
 * gdb: `target remote :1234`. Ctrl-C interrupts a running guest. When gdb
 * detaches, the guest keeps running to completion; when it kills, serve
 * returns UAM_STOP_REQUEST.
 */
#ifndef UAM_GDBSTUB_H
#define UAM_GDBSTUB_H

#include "uam/machine.h"

typedef struct uam_gdb uam_gdb;

uam_gdb        *uam_gdb_create(uam_machine *m);
/* Listen on localhost:port and wait for one client. */
uam_status      uam_gdb_listen(uam_gdb *g, const char *port);
/* Serve the session; returns how the guest finally stopped. */
uam_stop_reason uam_gdb_serve(uam_gdb *g, uint64_t max_insns);
void            uam_gdb_destroy(uam_gdb *g);

#endif /* UAM_GDBSTUB_H */
