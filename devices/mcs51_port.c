/*
 * mcs51_port.c - Single-byte 8051 SFRs: a port and a plain register
 * (uam/dev/mcs51_port.h).
 */
#include <stdlib.h>

#include "uam/dev/mcs51_port.h"

typedef struct m51byte {
    uam_device        dev;
    uint8_t           value, pins, reset_value;
    uam_mcs51_reg_fn  fn;   /* told every value (optional) */
    void             *ctx;
} m51byte;

static m51byte *to_b(uam_device *d) { return UAM_CONTAINER_OF(d, m51byte, dev); }

static uam_status port_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    (void)size;
    m51byte *b = to_b(d);
    *val = off == 0 ? (uint8_t)(b->value & b->pins) : 0;
    return UAM_OK;
}

static uam_status byte_write(uam_device *d, uint64_t off, unsigned size, uint64_t val)
{
    (void)size;
    m51byte *b = to_b(d);
    if (off == 0) {
        b->value = (uint8_t)val;
        if (b->fn)
            b->fn(b->ctx, b->value);
    }
    return UAM_OK;
}

static void byte_reset(uam_device *d)
{
    m51byte *b = to_b(d);
    b->value = b->reset_value;
    if (b->fn)
        b->fn(b->ctx, b->value);
}

static void byte_destroy(uam_device *d) { free(to_b(d)); }

static void byte_save(uam_device *d, uam_state_writer *w)
{
    m51byte *b = to_b(d);
    uam_state_put_u8(w, b->value);
    uam_state_put_u8(w, b->pins);
}

static void byte_restore(uam_device *d, uam_state_reader *r)
{
    m51byte *b = to_b(d);
    b->value = uam_state_get_u8(r);
    b->pins = uam_state_get_u8(r);
    if (b->fn)
        b->fn(b->ctx, b->value);
}

static const uam_device_ops byte_ops = {
    .type = "uam,mcs51-sfr-byte", .impl_min = 1, .impl_max = 1,
    .read = port_read, .peek = port_read, .write = byte_write, .reset = byte_reset,
    .destroy = byte_destroy, .save = byte_save, .restore = byte_restore,
};

static uam_device *create(const char *name, uint8_t reset_value, uam_mcs51_reg_fn fn, void *ctx)
{
    m51byte *b = calloc(1, sizeof(*b));
    if (!b)
        return NULL;
    uam_device_init(&b->dev, &byte_ops, name);
    b->value = b->reset_value = reset_value;
    b->pins = 0xFF;
    b->fn = fn;
    b->ctx = ctx;
    return &b->dev;
}

uam_device *uam_mcs51_port_create(const char *name) { return create(name, 0xFF, NULL, NULL); }

void uam_mcs51_port_set_pins(uam_device *d, uint8_t pins) { to_b(d)->pins = pins; }

uam_device *uam_mcs51_reg_create(const char *name, uint8_t reset_value, uam_mcs51_reg_fn fn, void *ctx)
{
    return create(name, reset_value, fn, ctx);
}
