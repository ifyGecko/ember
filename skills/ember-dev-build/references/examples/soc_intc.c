/*
 * soc_intc.c - <SoC> "misc" interrupt controller @ phys 0x........: 32 level inputs,
 * STATUS (latched, W1C) and MASK, one output to a CPU line. Demux: miscIntDemux
 * 0x........ reads STATUS & MASK and returns base_vector + bit.
 */
#include <stdlib.h>

#include "soc_intc.h"

#define R_STATUS 0x0u   /* read: raw | latched; write 1 to clear latched bits */
#define R_MASK   0x4u

typedef struct soc_intc {
    uam_device dev;
    uam_signal out;
    uint32_t   level;   /* current input levels */
    uint32_t   latched; /* edges seen since the last ack */
    uint32_t   mask;
} soc_intc;

static soc_intc *to_intc(uam_device *d) { return UAM_CONTAINER_OF(d, soc_intc, dev); }

static void update(soc_intc *c)
{
    uam_signal_set(&c->out, ((c->level | c->latched) & c->mask) != 0);
}

void soc_intc_input(void *ctx, unsigned n, int level)
{
    soc_intc *c = ctx;
    uint32_t bit = UINT32_C(1) << (n & 31);
    if (level) {
        c->level |= bit;
        c->latched |= bit;
    } else {
        c->level &= ~bit;
    }
    update(c);
}

static uam_status intc_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    soc_intc *c = to_intc(d);
    (void)size;
    *val = off == R_STATUS ? (c->level | c->latched) : off == R_MASK ? c->mask : 0;
    return UAM_OK;
}

static uam_status intc_write(uam_device *d, uint64_t off, unsigned size, uint64_t v)
{
    soc_intc *c = to_intc(d);
    (void)size;
    if (off == R_STATUS)
        c->latched &= ~(uint32_t)v;     /* a level that is still high shows up again */
    else if (off == R_MASK)
        c->mask = (uint32_t)v;
    update(c);
    return UAM_OK;
}

static void intc_reset(uam_device *d)
{
    soc_intc *c = to_intc(d);
    c->latched = c->level;  /* inputs keep their level across a reset; their owners reset them */
    c->mask = 0;
    update(c);
}

static void intc_destroy(uam_device *d) { free(to_intc(d)); }

/* Snapshots. `level` is saved too: the input lines' owners restore their own outputs
 * without calling this sink again (uam_state_get_signal), so our view must come back here. */
static void intc_save(uam_device *d, uam_state_writer *w)
{
    soc_intc *c = to_intc(d);
    uam_state_put_u32(w, c->level);
    uam_state_put_u32(w, c->latched);
    uam_state_put_u32(w, c->mask);
    uam_state_put_signal(w, &c->out);
}

static void intc_restore(uam_device *d, uam_state_reader *r)
{
    soc_intc *c = to_intc(d);
    c->level = uam_state_get_u32(r);
    c->latched = uam_state_get_u32(r);
    c->mask = uam_state_get_u32(r);
    uam_state_get_signal(r, &c->out);
}

static const uam_device_ops soc_intc_ops = {
    .type = "soc-intc",
    .endian = UAM_ACC_BE,
    .impl_min = 4, .impl_max = 4,
    .read = intc_read,
    .write = intc_write,
    .reset = intc_reset,
    .destroy = intc_destroy,
    .save = intc_save,
    .restore = intc_restore,
};

uam_device *soc_intc_create(const char *name)
{
    soc_intc *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    uam_device_init(&c->dev, &soc_intc_ops, name);
    uam_signal_init(&c->out);
    return &c->dev;
}

uam_signal *soc_intc_output(uam_device *d) { return &to_intc(d)->out; }
