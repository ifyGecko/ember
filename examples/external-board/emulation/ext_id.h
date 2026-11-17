/*
 * ext_id.h - the identification block: a read-only ID word and a scratch register.
 */
#ifndef EXT_ID_H
#define EXT_ID_H

#include "uam/access.h"
#include "uam/device.h"

#define EXT_ID_SIZE 0x10u

/* `id` is what the ID register reads; the board knows which chip this is. */
uam_device *ext_id_create(const char *name, uint32_t id);

#endif
