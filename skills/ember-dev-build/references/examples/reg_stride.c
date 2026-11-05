/*
 * reg_stride.c - adapter that puts a byte-register device (e.g. ember's uam_uart16550)
 * behind a SoC bus that spaces its registers 1 << shift bytes apart ("reg-shift 2": one
 * 8-bit register per 32-bit word). It owns the inner device: resets, snapshots and
 * destroys it (the machine only knows the adapter).
 *
 * The bus does the lane work: the adapter implements 4-byte accesses only, so a byte
 * load from any lane of a word reaches it as that word, and the board picks the word's
 * byte order with the mapping (uam_map_attrs.endian) - a big-endian SoC's "sb" to
 * base+3 lands in the low byte, as on the hardware.
 */
#include <stdlib.h>

#include "reg_stride.h"

typedef struct reg_stride {
    uam_device  dev;
    uam_device *inner;
    unsigned    shift;
} reg_stride;

static reg_stride *to_rs(uam_device *d) { return UAM_CONTAINER_OF(d, reg_stride, dev); }

static uam_status rs_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    reg_stride *r = to_rs(d);
    uint64_t v = 0;
    (void)size;
    uam_status st = r->inner->ops->read(r->inner, off >> r->shift, 1, &v);
    *val = v & 0xFF;
    return st;
}

static uam_status rs_write(uam_device *d, uint64_t off, unsigned size, uint64_t v)
{
    reg_stride *r = to_rs(d);
    (void)size;
    return r->inner->ops->write(r->inner, off >> r->shift, 1, v & 0xFF);
}

/* A byte store must not read the inner register first (reading RBR pops a byte). */
static uam_status rs_write_masked(uam_device *d, uint64_t off, unsigned size, uint64_t v,
                                  uint64_t mask)
{
    if (!(mask & 0xFF))
        return UAM_OK;          /* store to a padding byte of the word */
    return rs_write(d, off, size, v);
}

static void rs_reset(uam_device *d)
{
    reg_stride *r = to_rs(d);
    if (r->inner->ops->reset)
        r->inner->ops->reset(r->inner);
}

static void rs_destroy(uam_device *d)
{
    reg_stride *r = to_rs(d);
    r->inner->ops->destroy(r->inner);
    free(r);
}

/* Snapshots: the adapter holds no state of its own; the inner device it owns (and the
 * machine never sees) saves and restores through it. */
static void rs_save(uam_device *d, uam_state_writer *w)
{
    reg_stride *r = to_rs(d);
    if (r->inner->ops->save)
        r->inner->ops->save(r->inner, w);
}

static void rs_restore(uam_device *d, uam_state_reader *rd)
{
    reg_stride *r = to_rs(d);
    if (r->inner->ops->restore)
        r->inner->ops->restore(r->inner, rd);
}

static const uam_device_ops reg_stride_ops = {
    .type = "reg-stride",
    .impl_min = 4, .impl_max = 4,
    .read = rs_read,
    .write = rs_write,
    .write_masked = rs_write_masked,
    .reset = rs_reset,
    .destroy = rs_destroy,
    .save = rs_save,
    .restore = rs_restore,
};

uam_device *reg_stride_create(const char *name, uam_device *inner, unsigned shift)
{
    if (!inner)
        return NULL;
    reg_stride *r = shift >= 2 ? calloc(1, sizeof(*r)) : NULL;
    /* rs_save/rs_restore stand in for the inner device: it must take part in snapshots */
    if (r && !(inner->ops->save && inner->ops->restore)
          && !(inner->ops->flags & UAM_DEVICE_STATELESS)) {
        uam_log(UAM_LOG_ERROR, "reg-stride", "%s: inner %s has no save/restore", name,
                inner->ops->type);
        free(r);
        r = NULL;
    }
    if (!r) {
        inner->ops->destroy(inner);
        return NULL;
    }
    uam_device_init(&r->dev, &reg_stride_ops, name);
    r->inner = inner;
    r->shift = shift;
    return &r->dev;
}
