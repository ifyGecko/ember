/*
 * ext_id.c - the identification block of the "ext-mips" board: one hardware component,
 * one file. It includes only ember's device-side interfaces (never the CPU, the machine
 * or the board); what it needs from the board arrives as a parameter of its create
 * function.
 *
 *   0x0  ID       read-only: the value the board gave
 *   0x4  SCRATCH  read/write, cleared by a reset
 */
#include <stdlib.h>

#include "uam/state.h"

#include "ext_id.h"

#define R_ID      0x0u
#define R_SCRATCH 0x4u

typedef struct ext_id {
    uam_device dev;
    uint32_t   id;        /* configuration: not saved */
    uint32_t   scratch;   /* guest-visible state: saved */
} ext_id;

static ext_id *to_id(uam_device *d) { return UAM_CONTAINER_OF(d, ext_id, dev); }

static uam_status id_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    ext_id *s = to_id(d);
    (void)size;
    switch (off) {
    case R_ID:      *val = s->id; break;
    case R_SCRATCH: *val = s->scratch; break;
    default:        *val = 0; break;
    }
    return UAM_OK;
}

static uam_status id_write(uam_device *d, uint64_t off, unsigned size, uint64_t v)
{
    (void)size;
    if (off == R_SCRATCH)
        to_id(d)->scratch = (uint32_t)v;
    return UAM_OK;
}

static void id_reset(uam_device *d) { to_id(d)->scratch = 0; }
static void id_destroy(uam_device *d) { free(to_id(d)); }
static void id_save(uam_device *d, uam_state_writer *w) { uam_state_put_u32(w, to_id(d)->scratch); }
static void id_restore(uam_device *d, uam_state_reader *r) { to_id(d)->scratch = uam_state_get_u32(r); }

static const uam_device_ops ext_id_ops = {
    .type = "ext-id",
    .endian = UAM_ACC_BE,
    .impl_min = 4, .impl_max = 4,
    .read = id_read,
    .write = id_write,
    .reset = id_reset,
    .destroy = id_destroy,
    .save = id_save,
    .restore = id_restore,
};

uam_device *ext_id_create(const char *name, uint32_t id)
{
    ext_id *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    uam_device_init(&s->dev, &ext_id_ops, name);
    s->id = id;
    return &s->dev;
}
