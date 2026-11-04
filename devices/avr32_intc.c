/*
 * avr32_intc.c - AVR32 interrupt controller (uam/dev/avr32_intc.h).
 *
 * Requests are levels: a group's IRR follows its inputs, and the device that
 * raised a request clears it. There is no acknowledge; ICRn says which group the
 * processor is taking at level n (0 when none requests that level).
 */
#include <stdlib.h>

#include "uam/bus.h"
#include "uam/dev/avr32_intc.h"

#define OFF_IPR 0x000u
#define OFF_IRR 0x100u
#define OFF_ICR 0x200u

typedef struct avr32_intc {
    uam_device dev;
    unsigned   n_groups;
    uint32_t   ipr[UAM_AVR32_INTC_MAX_GROUPS];
    uint32_t   irr[UAM_AVR32_INTC_MAX_GROUPS];
    uam_signal out[4];
} avr32_intc;

static avr32_intc *to_intc(void *d) { return UAM_CONTAINER_OF((uam_device *)d, avr32_intc, dev); }

/* The group taken at `level`: the lowest-numbered requesting group, or -1. */
static int group_at(const avr32_intc *c, unsigned level)
{
    for (unsigned g = 0; g < c->n_groups; g++)
        if (c->irr[g] && (c->ipr[g] >> 30) == level)
            return (int)g;
    return -1;
}

static void update(avr32_intc *c)
{
    for (unsigned l = 0; l < 4; l++)
        uam_signal_set(&c->out[l], group_at(c, l) >= 0);
}

void uam_avr32_intc_input(void *intc, unsigned line, int level)
{
    avr32_intc *c = to_intc(intc);
    unsigned g = line / 32;
    if (g >= c->n_groups)
        return;
    if (level)
        c->irr[g] |= 1u << (line % 32);
    else
        c->irr[g] &= ~(1u << (line % 32));
    update(c);
}

uint32_t uam_avr32_intc_autovector(void *intc, unsigned level)
{
    avr32_intc *c = to_intc(intc);
    int g = level < 4 ? group_at(c, level) : -1;
    return g < 0 ? 0 : c->ipr[g] & 0x3FFFu;
}

static uam_status intc_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    avr32_intc *c = to_intc(d);
    (void)size;
    *val = 0;
    if (off < OFF_IRR) {
        if (off / 4 < c->n_groups)
            *val = c->ipr[off / 4];
    } else if (off < OFF_ICR) {
        if ((off - OFF_IRR) / 4 < c->n_groups)
            *val = c->irr[(off - OFF_IRR) / 4];
    } else if (off < OFF_ICR + 16) {
        int g = group_at(c, 3 - (unsigned)((off - OFF_ICR) / 4));
        *val = g < 0 ? 0 : (uint32_t)g;
    }
    return UAM_OK;
}

static uam_status intc_write(uam_device *d, uint64_t off, unsigned size, uint64_t v)
{
    avr32_intc *c = to_intc(d);
    (void)size;
    if (off < OFF_IRR && off / 4 < c->n_groups) {
        c->ipr[off / 4] = (uint32_t)v & 0xC0003FFFu;
        update(c);
    }
    return UAM_OK;   /* IRR and ICR are read-only */
}

static void intc_reset(uam_device *d)
{
    avr32_intc *c = to_intc(d);
    for (unsigned g = 0; g < c->n_groups; g++)
        c->ipr[g] = 0;   /* the requests are the inputs' levels: they stay */
    update(c);
}

static void intc_destroy(uam_device *d) { free(to_intc(d)); }

static void intc_save(uam_device *d, uam_state_writer *w)
{
    avr32_intc *c = to_intc(d);
    uam_state_put_u32(w, c->n_groups);
    for (unsigned g = 0; g < c->n_groups; g++) {
        uam_state_put_u32(w, c->ipr[g]);
        uam_state_put_u32(w, c->irr[g]);
    }
    for (unsigned l = 0; l < 4; l++)
        uam_state_put_signal(w, &c->out[l]);
}

static void intc_restore(uam_device *d, uam_state_reader *r)
{
    avr32_intc *c = to_intc(d);
    uam_state_expect_u32(r, c->n_groups, "AVR32 INTC group count");
    if (!uam_state_ok(r))
        return;
    for (unsigned g = 0; g < c->n_groups; g++) {
        c->ipr[g] = uam_state_get_u32(r);
        c->irr[g] = uam_state_get_u32(r);
    }
    for (unsigned l = 0; l < 4; l++)
        uam_state_get_signal(r, &c->out[l]);
}

static const uam_device_ops intc_ops = {
    .type = "avr32,intc",
    .endian = UAM_ACC_BE,   /* the registers of a big-endian processor's peripheral bus */
    .read = intc_read,
    .write = intc_write,
    .reset = intc_reset,
    .destroy = intc_destroy,
    .save = intc_save,
    .restore = intc_restore,
};

uam_device *uam_avr32_intc_create(const char *name, unsigned n_groups)
{
    if (n_groups == 0 || n_groups > UAM_AVR32_INTC_MAX_GROUPS)
        return NULL;
    avr32_intc *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    uam_device_init(&c->dev, &intc_ops, name);
    c->n_groups = n_groups;
    for (unsigned l = 0; l < 4; l++)
        uam_signal_init(&c->out[l]);
    return &c->dev;
}

uam_signal *uam_avr32_intc_level_line(uam_device *d, unsigned level)
{
    return level < 4 ? &to_intc(d)->out[level] : NULL;
}
