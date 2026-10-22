/*
 * i8259.c - Cascaded 8259A PICs with ELCR (uam/dev/i8259.h).
 *
 * Implemented: ICW1-ICW4 initialization, IMR, IRR/ISR reads (OCW3), the EOI
 * and rotation commands (OCW2), auto-EOI, edge and level triggering (LTIM or
 * the ELCR), cascade through master input 2 and spurious interrupts.
 * Not implemented: special mask mode, special fully nested mode, poll mode
 * and buffered mode, which PC firmware does not use.
 */
#include <stdlib.h>

#include "uam/dev/i8259.h"

typedef struct chip {
    uint8_t irr, imr, isr;
    uint8_t lines;        /* input levels */
    uint8_t elcr;         /* 1 = level-triggered */
    uint8_t base;         /* vector base (ICW2) */
    uint8_t lowest;       /* lowest-priority input (rotation) */
    uint8_t init_step;    /* 0 = operational, 2..4 = expecting ICWn */
    bool    need_icw4, single, auto_eoi, ltim, read_isr;
} chip;

typedef struct i8259 {
    uam_device dev;        /* master view; owns the state */
    uam_device slave_view, elcr_view;
    uam_signal intr;
    chip       c[2];
} i8259;

static i8259 *from_master(uam_device *d) { return UAM_CONTAINER_OF(d, i8259, dev); }
static i8259 *from_slave(uam_device *d)  { return UAM_CONTAINER_OF(d, i8259, slave_view); }
static i8259 *from_elcr(uam_device *d)   { return UAM_CONTAINER_OF(d, i8259, elcr_view); }

/* Highest-priority bit of `mask`, honouring rotation; -1 if none. */
static int highest(const chip *c, uint8_t mask)
{
    for (unsigned i = 1; i <= 8; i++) {
        unsigned b = (c->lowest + i) & 7;
        if (mask & (1u << b))
            return (int)b;
    }
    return -1;
}

/* The request a chip would present: highest unmasked IRR above every in-service level. */
static int pending(const chip *c)
{
    int r = highest(c, c->irr & ~c->imr);
    if (r < 0)
        return -1;
    int s = highest(c, c->isr);
    if (s >= 0) {
        /* compare priorities: position after `lowest` */
        unsigned pr = ((unsigned)r - c->lowest - 1) & 7, ps = ((unsigned)s - c->lowest - 1) & 7;
        if (pr >= ps)
            return -1;
    }
    return r;
}

static void set_level(chip *c, unsigned n, int level);

static void update(i8259 *p)
{
    /* the slave's output drives master input 2 */
    set_level(&p->c[0], 2, pending(&p->c[1]) >= 0);
    uam_signal_set(&p->intr, pending(&p->c[0]) >= 0);
}

static void set_level(chip *c, unsigned n, int level)
{
    uint8_t bit = (uint8_t)(1u << n);
    bool lvl = (c->elcr & bit) || c->ltim;
    if (level) {
        if (lvl || !(c->lines & bit))
            c->irr |= bit;   /* rising edge, or level held */
        c->lines |= bit;
    } else {
        c->lines &= (uint8_t)~bit;
        if (lvl)
            c->irr &= (uint8_t)~bit;
    }
}

void uam_i8259_input(void *ctx, unsigned irq, int level)
{
    i8259 *p = from_master(ctx);
    if (irq > 15 || irq == 2)
        return;
    set_level(&p->c[irq >> 3], irq & 7, level);
    update(p);
}

/* Acknowledge on one chip: in-service, out of IRR (edge), auto-EOI. */
static void ack(chip *c, int n)
{
    uint8_t bit = (uint8_t)(1u << n);
    if (!((c->elcr & bit) || c->ltim) || !(c->lines & bit))
        c->irr &= (uint8_t)~bit;
    if (!c->auto_eoi)   /* rotation in auto-EOI mode is not modelled */
        c->isr |= bit;
}

int uam_i8259_inta(uam_device *d)
{
    i8259 *p = from_master(d);
    int vec;
    int m = pending(&p->c[0]);
    if (m < 0) {
        vec = p->c[0].base + 7;   /* spurious */
    } else if (m == 2 && !p->c[0].single) {
        int s = pending(&p->c[1]);
        ack(&p->c[0], 2);
        if (s < 0) {
            vec = p->c[1].base + 7;
        } else {
            ack(&p->c[1], s);
            vec = p->c[1].base + s;
        }
    } else {
        ack(&p->c[0], m);
        vec = p->c[0].base + m;
    }
    update(p);
    return vec;
}

static void eoi(chip *c, uint8_t cmd)
{
    unsigned op = cmd >> 5, lvl = cmd & 7;
    int s;
    switch (op) {
    case 1: case 5:            /* non-specific EOI (and rotate) */
        s = highest(c, c->isr);
        if (s >= 0) {
            c->isr &= (uint8_t)~(1u << s);
            if (op == 5) c->lowest = (uint8_t)s;
        }
        break;
    case 3: case 7:            /* specific EOI (and rotate) */
        c->isr &= (uint8_t)~(1u << lvl);
        if (op == 7) c->lowest = (uint8_t)lvl;
        break;
    case 6:                    /* set priority */
        c->lowest = (uint8_t)lvl;
        break;
    default:                   /* rotate-in-auto-EOI set/clear: no effect here */
        break;
    }
}

static void chip_write(i8259 *p, chip *c, uint64_t off, uint8_t v)
{
    if (off == 0) {
        if (v & 0x10) {        /* ICW1 */
            c->imr = 0;
            c->isr = 0;
            c->irr = 0;
            c->lowest = 7;
            c->read_isr = false;
            c->auto_eoi = false;
            c->ltim = (v & 0x08) != 0;
            c->single = (v & 0x02) != 0;
            c->need_icw4 = (v & 0x01) != 0;
            c->init_step = 2;
        } else if (v & 0x08) { /* OCW3 */
            if (v & 0x02)
                c->read_isr = (v & 0x01) != 0;
        } else {               /* OCW2 */
            eoi(c, v);
        }
    } else {
        switch (c->init_step) {
        case 2:
            c->base = v & 0xF8;
            c->init_step = c->single ? (c->need_icw4 ? 4 : 0) : 3;
            break;
        case 3:
            c->init_step = c->need_icw4 ? 4 : 0;
            break;
        case 4:
            c->auto_eoi = (v & 0x02) != 0;
            c->init_step = 0;
            break;
        default:
            c->imr = v;
            break;
        }
    }
    update(p);
}

static uint8_t chip_read(const chip *c, uint64_t off)
{
    if (off == 0)
        return c->read_isr ? c->isr : c->irr;
    return c->imr;
}

static uam_status master_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    (void)size;
    *val = chip_read(&from_master(d)->c[0], off & 1);
    return UAM_OK;
}

static uam_status master_write(uam_device *d, uint64_t off, unsigned size, uint64_t v)
{
    (void)size;
    i8259 *p = from_master(d);
    chip_write(p, &p->c[0], off & 1, (uint8_t)v);
    return UAM_OK;
}

static uam_status slave_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    (void)size;
    *val = chip_read(&from_slave(d)->c[1], off & 1);
    return UAM_OK;
}

static uam_status slave_write(uam_device *d, uint64_t off, unsigned size, uint64_t v)
{
    (void)size;
    i8259 *p = from_slave(d);
    chip_write(p, &p->c[1], off & 1, (uint8_t)v);
    return UAM_OK;
}

/* ELCR: IRQ 0, 1, 2, 8 and 13 are always edge-triggered. */
static const uint8_t elcr_mask[2] = { 0xF8, 0xDE };

static uam_status elcr_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    (void)size;
    *val = from_elcr(d)->c[off & 1].elcr;
    return UAM_OK;
}

static uam_status elcr_write(uam_device *d, uint64_t off, unsigned size, uint64_t v)
{
    (void)size;
    i8259 *p = from_elcr(d);
    p->c[off & 1].elcr = (uint8_t)v & elcr_mask[off & 1];
    update(p);
    return UAM_OK;
}

static void i8259_reset(uam_device *d)
{
    i8259 *p = from_master(d);
    for (unsigned i = 0; i < 2; i++) {
        uint8_t lines = p->c[i].lines;
        p->c[i] = (chip){ .lowest = 7, .lines = lines };
    }
    update(p);
}

static void i8259_destroy(uam_device *d) { free(from_master(d)); }

static void save_chip(uam_state_writer *w, const chip *c)
{
    const uint8_t b[] = { c->irr, c->imr, c->isr, c->lines, c->elcr, c->base, c->lowest,
                          c->init_step, c->need_icw4, c->single, c->auto_eoi, c->ltim,
                          c->read_isr };
    uam_state_put_bytes(w, b, sizeof(b));
}

static void restore_chip(uam_state_reader *r, chip *c)
{
    uint8_t b[13];
    uam_state_get_bytes(r, b, sizeof(b));
    *c = (chip){ b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12] };
}

static void i8259_save(uam_device *d, uam_state_writer *w)
{
    i8259 *p = from_master(d);
    save_chip(w, &p->c[0]);
    save_chip(w, &p->c[1]);
    uam_state_put_signal(w, &p->intr);
}

static void i8259_restore(uam_device *d, uam_state_reader *r)
{
    i8259 *p = from_master(d);
    restore_chip(r, &p->c[0]);
    restore_chip(r, &p->c[1]);
    uam_state_get_signal(r, &p->intr);
}

static const uam_device_ops master_ops = {
    .type = "pc,8259-pic", .impl_min = 1, .impl_max = 1,
    .read = master_read, .write = master_write, .reset = i8259_reset, .destroy = i8259_destroy,
    .save = i8259_save, .restore = i8259_restore,
};
static const uam_device_ops slave_ops = {
    .type = "pc,8259-pic-slave", .impl_min = 1, .impl_max = 1,
    .read = slave_read, .write = slave_write, .flags = UAM_DEVICE_STATELESS,
};
static const uam_device_ops elcr_ops = {
    .type = "pc,8259-elcr", .impl_min = 1, .impl_max = 1,
    .read = elcr_read, .write = elcr_write, .flags = UAM_DEVICE_STATELESS,
};

uam_device *uam_i8259_create(const char *name)
{
    i8259 *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    uam_device_init(&p->dev, &master_ops, name);
    uam_device_init(&p->slave_view, &slave_ops, name);
    uam_device_init(&p->elcr_view, &elcr_ops, name);
    uam_signal_init(&p->intr);
    p->c[0].lowest = p->c[1].lowest = 7;
    return &p->dev;
}

uam_device *uam_i8259_master(uam_device *d) { return d; }
uam_device *uam_i8259_slave(uam_device *d)  { return &from_master(d)->slave_view; }
uam_device *uam_i8259_elcr(uam_device *d)   { return &from_master(d)->elcr_view; }
uam_signal *uam_i8259_intr_line(uam_device *d) { return &from_master(d)->intr; }
