/*
 * superio.c - LPC Super I/O configuration space (superio.h).
 *
 * Semantics as firmware drivers use them (edk2 SioBusDxe/platform PEIMs, coreboot
 * src/superio/): write the enter key to the index port, then pairs of (index, data); select
 * a logical device in 0x07; set its base in 0x60/0x61, its IRQ in 0x70, activate it with
 * 0x30 = 1; write the exit key. Outside configuration mode the ports read 0xFF and ignore
 * everything except the enter key. Registers without a meaning here are storage.
 * Local emulation model; not part of ember.
 */
#include <stdlib.h>
#include <string.h>

#include "superio.h"

#define R_LDN    0x07u
#define R_ID_HI  0x20u
#define R_ID_LO  0x21u
#define R_ACT    0x30u
#define R_BASE   0x60u
#define R_IRQ    0x70u
#define GLOBALS  0x30u            /* 0x00-0x2F are global, the rest per logical device */

typedef struct sio_ldn {
    uam_region *io;               /* NULL: no function behind this logical device */
    uint8_t     reset_regs[0x100 - GLOBALS];
} sio_ldn;

typedef struct superio {
    uam_device     dev;           /* first member */
    superio_config cfg;
    uam_bus       *sys;
    uint64_t       io_base;
    sio_ldn        ldn[SUPERIO_MAX_LDN];
    /* guest-visible state (saved) */
    bool           config_mode;
    unsigned       key_pos;       /* how much of the enter key has arrived */
    uint8_t        index;
    uint8_t        global[GLOBALS];
    uint8_t        regs[SUPERIO_MAX_LDN][0x100 - GLOBALS];
} superio;

static superio *to_sio(uam_device *d) { return UAM_CONTAINER_OF(d, superio, dev); }

/* Follow a logical device's activate and base registers with its I/O region. */
static void update_decode(superio *s, unsigned n)
{
    sio_ldn *l = &s->ldn[n];
    if (!l->io)
        return;
    const uint8_t *r = s->regs[n];
    uint16_t base = (uint16_t)(r[R_BASE - GLOBALS] << 8 | r[R_BASE + 1 - GLOBALS]);
    bool on = (r[R_ACT - GLOBALS] & 1) && base;
    if (on)
        uam_bus_region_move(s->sys, l->io, s->io_base + base);
    uam_bus_region_enable(s->sys, l->io, on);
}

static void global_defaults(superio *s)
{
    memset(s->global, 0, sizeof(s->global));
    s->global[R_ID_HI] = (uint8_t)(s->cfg.chip_id >> 8);
    s->global[R_ID_LO] = (uint8_t)s->cfg.chip_id;
}

static uint8_t *reg_at(superio *s, uint8_t idx)
{
    if (idx < GLOBALS)
        return &s->global[idx];
    unsigned n = s->global[R_LDN];
    return n < SUPERIO_MAX_LDN ? &s->regs[n][idx - GLOBALS] : NULL;
}

static uam_status sio_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    (void)size;
    superio *s = to_sio(d);
    *val = 0xFF;
    if (!s->config_mode)
        return UAM_OK;
    if (off == 0) {
        *val = s->index;
    } else {
        uint8_t *r = reg_at(s, s->index);
        if (r)
            *val = *r;
    }
    return UAM_OK;
}

static uam_status sio_write(uam_device *d, uint64_t off, unsigned size, uint64_t val)
{
    (void)size;
    superio *s = to_sio(d);
    uint8_t v = (uint8_t)val;
    if (!s->config_mode) {
        if (off != 0)
            return UAM_OK;
        if (v == s->cfg.enter_key[s->key_pos]) {
            if (++s->key_pos == s->cfg.enter_len) {
                s->config_mode = true;
                s->key_pos = 0;
            }
        } else {
            s->key_pos = v == s->cfg.enter_key[0] ? 1 : 0;
        }
        return UAM_OK;
    }
    if (off == 0) {
        if (v == s->cfg.exit_key)
            s->config_mode = false;
        else
            s->index = v;
        return UAM_OK;
    }
    if (s->index == R_ID_HI || s->index == R_ID_LO)
        return UAM_OK;                              /* read-only */
    if (s->index == 0x02 && (v & 0x02)) {           /* ITE: leave configuration mode */
        s->config_mode = false;
        return UAM_OK;
    }
    uint8_t *r = reg_at(s, s->index);
    if (!r)
        return UAM_OK;
    *r = v;
    if (s->index == R_ACT || s->index == R_BASE || s->index == R_BASE + 1)
        update_decode(s, s->global[R_LDN]);
    return UAM_OK;
}

static void sio_reset(uam_device *d)
{
    superio *s = to_sio(d);
    s->config_mode = false;
    s->key_pos = 0;
    s->index = 0;
    global_defaults(s);
    for (unsigned n = 0; n < SUPERIO_MAX_LDN; n++) {
        memcpy(s->regs[n], s->ldn[n].reset_regs, sizeof(s->regs[n]));
        update_decode(s, n);
    }
}

static void sio_destroy(uam_device *d) { free(to_sio(d)); }

static void sio_save(uam_device *d, uam_state_writer *w)
{
    superio *s = to_sio(d);
    uam_state_put_bool(w, s->config_mode);
    uam_state_put_u32(w, s->key_pos);
    uam_state_put_u8(w, s->index);
    uam_state_put_bytes(w, s->global, sizeof(s->global));
    uam_state_put_bytes(w, s->regs, sizeof(s->regs));
}

static void sio_restore(uam_device *d, uam_state_reader *r)
{
    superio *s = to_sio(d);
    s->config_mode = uam_state_get_bool(r);
    s->key_pos = uam_state_get_u32(r);
    if (s->key_pos >= s->cfg.enter_len)
        uam_state_fail(r, "superio: key position %u", s->key_pos);
    s->index = uam_state_get_u8(r);
    uam_state_get_bytes(r, s->global, sizeof(s->global));
    uam_state_get_bytes(r, s->regs, sizeof(s->regs));
    for (unsigned n = 0; n < SUPERIO_MAX_LDN; n++)
        update_decode(s, n);                       /* the decode follows the registers */
}

static const uam_device_ops sio_ops = {
    .type = "superio", .impl_min = 1, .impl_max = 1,
    .read = sio_read, .write = sio_write, .reset = sio_reset, .destroy = sio_destroy,
    .save = sio_save, .restore = sio_restore,
};

uam_device *superio_create(const char *name, const superio_config *cfg, uam_bus *sys,
                           uint64_t io_base)
{
    if (!cfg->enter_len || cfg->enter_len > sizeof(cfg->enter_key))
        return NULL;
    superio *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    uam_device_init(&s->dev, &sio_ops, name);
    s->cfg = *cfg;
    s->sys = sys;
    s->io_base = io_base;
    global_defaults(s);
    return &s->dev;
}

uam_status superio_add_ldn(uam_device *d, unsigned ldn, uam_region *io, uint16_t reset_base,
                           uint8_t reset_irq, bool active)
{
    superio *s = to_sio(d);
    if (ldn >= SUPERIO_MAX_LDN || !io)
        return UAM_ERR_INVALID;
    sio_ldn *l = &s->ldn[ldn];
    l->io = io;
    l->reset_regs[R_ACT - GLOBALS] = active;
    l->reset_regs[R_BASE - GLOBALS] = (uint8_t)(reset_base >> 8);
    l->reset_regs[R_BASE + 1 - GLOBALS] = (uint8_t)reset_base;
    l->reset_regs[R_IRQ - GLOBALS] = reset_irq;
    memcpy(s->regs[ldn], l->reset_regs, sizeof(s->regs[ldn]));
    update_decode(s, ldn);
    return UAM_OK;
}
