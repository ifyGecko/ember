/*
 * pc_misc.c - System Control Port A, POST-code latch and debug console port
 * (uam/dev/pc_misc.h).
 */
#include <stdlib.h>

#include "uam/dev/pc_misc.h"

/* ---- System Control Port A ----------------------------------------------- */

typedef struct sysctl {
    uam_device dev;
    uam_signal reset;
    uint8_t    val;
} sysctl;

static sysctl *to_sysctl(uam_device *d) { return UAM_CONTAINER_OF(d, sysctl, dev); }

static uam_status sysctl_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    (void)off; (void)size;
    *val = to_sysctl(d)->val | 0x02;   /* A20 reads as enabled */
    return UAM_OK;
}

static uam_status sysctl_write(uam_device *d, uint64_t off, unsigned size, uint64_t v)
{
    (void)off; (void)size;
    sysctl *s = to_sysctl(d);
    bool rise = (v & 1) && !(s->val & 1);
    s->val = (uint8_t)v & 0x03;
    if (rise) {
        uam_signal_set(&s->reset, 1);
        uam_signal_set(&s->reset, 0);
    }
    return UAM_OK;
}

static void sysctl_reset(uam_device *d)
{
    sysctl *s = to_sysctl(d);
    s->val = 0;
    uam_signal_set(&s->reset, 0);
}

static void sysctl_save(uam_device *d, uam_state_writer *w) { uam_state_put_u8(w, to_sysctl(d)->val); }
static void sysctl_restore(uam_device *d, uam_state_reader *r) { to_sysctl(d)->val = uam_state_get_u8(r); }
static void free_dev(uam_device *d) { free(d); }

static const uam_device_ops sysctl_ops = {
    .type = "pc,port92", .impl_min = 1, .impl_max = 1,
    .read = sysctl_read, .write = sysctl_write, .reset = sysctl_reset, .destroy = free_dev,
    .save = sysctl_save, .restore = sysctl_restore,
};

uam_device *uam_pc_sysctl_create(const char *name)
{
    sysctl *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    uam_device_init(&s->dev, &sysctl_ops, name);
    uam_signal_init(&s->reset);
    return &s->dev;
}

uam_signal *uam_pc_sysctl_reset_line(uam_device *d) { return &to_sysctl(d)->reset; }

/* ---- POST codes ---------------------------------------------------------- */

typedef struct postcode {
    uam_device dev;
    uint8_t    last;
} postcode;

static postcode *to_post(uam_device *d) { return UAM_CONTAINER_OF(d, postcode, dev); }

static uam_status post_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    (void)off; (void)size;
    *val = to_post(d)->last;
    return UAM_OK;
}

static uam_status post_write(uam_device *d, uint64_t off, unsigned size, uint64_t v)
{
    (void)off;
    postcode *p = to_post(d);
    p->last = (uint8_t)v;
    uam_log(UAM_LOG_DEBUG, "post", "code 0x%0*llx", (int)size * 2, (unsigned long long)v);
    return UAM_OK;
}

static void post_save(uam_device *d, uam_state_writer *w) { uam_state_put_u8(w, to_post(d)->last); }
static void post_restore(uam_device *d, uam_state_reader *r) { to_post(d)->last = uam_state_get_u8(r); }

static const uam_device_ops post_ops = {
    .type = "pc,postcode", .read = post_read, .write = post_write, .destroy = free_dev,
    .save = post_save, .restore = post_restore,
};

uam_device *uam_postcode_create(const char *name)
{
    postcode *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    uam_device_init(&p->dev, &post_ops, name);
    return &p->dev;
}

uint8_t uam_postcode_last(uam_device *d) { return to_post(d)->last; }

/* ---- debug console ------------------------------------------------------- */

typedef struct debugcon {
    uam_device   dev;
    uam_chardev *out;
    uint8_t      read_val;
} debugcon;

static debugcon *to_dc(uam_device *d) { return UAM_CONTAINER_OF(d, debugcon, dev); }

static uam_status dc_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    (void)off; (void)size;
    *val = to_dc(d)->read_val;
    return UAM_OK;
}

static uam_status dc_write(uam_device *d, uint64_t off, unsigned size, uint64_t v)
{
    (void)off; (void)size;
    debugcon *c = to_dc(d);
    uint8_t b = (uint8_t)v;
    if (c->out)
        uam_chardev_write(c->out, &b, 1);
    return UAM_OK;
}

static const uam_device_ops dc_ops = {
    .type = "debug-console", .impl_min = 1, .impl_max = 1,
    .read = dc_read, .write = dc_write, .destroy = free_dev, .flags = UAM_DEVICE_STATELESS,
};

uam_device *uam_debugcon_create(const char *name, uam_chardev *out, uint8_t read_val)
{
    debugcon *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    uam_device_init(&c->dev, &dc_ops, name);
    c->out = out;
    c->read_val = read_val;
    return &c->dev;
}

/* ---- master abort -------------------------------------------------------- */

static uam_status abort_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    (void)d; (void)off;
    *val = size >= 8 ? ~0ull : (1ull << (8 * size)) - 1;
    return UAM_OK;
}

static uam_status abort_write(uam_device *d, uint64_t off, unsigned size, uint64_t v)
{
    (void)d; (void)off; (void)size; (void)v;
    return UAM_OK;
}

static const uam_device_ops abort_ops = {
    .type = "pc,master-abort", .read = abort_read, .write = abort_write, .destroy = free_dev,
    .flags = UAM_DEVICE_STATELESS,
};

uam_device *uam_master_abort_create(const char *name)
{
    uam_device *d = calloc(1, sizeof(*d));
    if (d)
        uam_device_init(d, &abort_ops, name);
    return d;
}
