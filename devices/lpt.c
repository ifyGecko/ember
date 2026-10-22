/*
 * lpt.c - PC parallel port, standard mode (uam/dev/lpt.h).
 */
#include <stdlib.h>

#include "uam/dev/lpt.h"

#define CTL_STROBE 0x01u
#define CTL_IRQ    0x10u
#define CTL_DIR    0x20u           /* bidirectional ports: input */

typedef struct lpt {
    uam_device   dev;
    uam_chardev *out;
    uam_signal   irq;
    uint8_t      data, ctl;
} lpt;

static lpt *from_dev(uam_device *d) { return UAM_CONTAINER_OF(d, lpt, dev); }

static uam_status lpt_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    (void)size;
    lpt *p = from_dev(d);
    switch (off) {
    case 0:  *val = (p->ctl & CTL_DIR) ? 0xFF : p->data; break;
    case 1:  *val = 0xDF; break;     /* not busy, no ACK, paper, selected, no error */
    case 2:  *val = 0xE0 | p->ctl; break;
    default: *val = 0xFF; break;
    }
    return UAM_OK;
}

static uam_status lpt_write(uam_device *d, uint64_t off, unsigned size, uint64_t val)
{
    (void)size;
    lpt *p = from_dev(d);
    uint8_t b = (uint8_t)val;
    if (off == 0) {
        p->data = b;
    } else if (off == 2) {
        bool strobe_end = (p->ctl & CTL_STROBE) && !(b & CTL_STROBE);
        p->ctl = b & 0x3F;
        if (strobe_end) {
            if (p->out)
                uam_chardev_write(p->out, &p->data, 1);
            if (p->ctl & CTL_IRQ) {  /* the printer's ACK */
                uam_signal_set(&p->irq, 1);
                uam_signal_set(&p->irq, 0);
            }
        }
    }
    return UAM_OK;
}

static void lpt_reset(uam_device *d)
{
    lpt *p = from_dev(d);
    p->data = 0;
    p->ctl = 0x0C;                   /* init high (inactive), select */
    uam_signal_set(&p->irq, 0);
}

static void lpt_destroy(uam_device *d) { free(from_dev(d)); }

static void lpt_save(uam_device *d, uam_state_writer *w)
{
    uam_state_put_u8(w, from_dev(d)->data);
    uam_state_put_u8(w, from_dev(d)->ctl);
}

static void lpt_restore(uam_device *d, uam_state_reader *r)
{
    from_dev(d)->data = uam_state_get_u8(r);
    from_dev(d)->ctl = uam_state_get_u8(r);
}

static const uam_device_ops lpt_ops = {
    .type = "pc,parallel", .impl_min = 1, .impl_max = 1, .read = lpt_read, .write = lpt_write,
    .reset = lpt_reset, .destroy = lpt_destroy, .save = lpt_save, .restore = lpt_restore,
};

uam_device *uam_lpt_create(const char *name, uam_chardev *out)
{
    lpt *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    uam_device_init(&p->dev, &lpt_ops, name);
    uam_signal_init(&p->irq);
    p->out = out;
    lpt_reset(&p->dev);
    return &p->dev;
}

uam_signal *uam_lpt_irq_line(uam_device *d) { return &from_dev(d)->irq; }
