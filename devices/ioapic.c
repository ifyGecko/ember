/*
 * ioapic.c - x86 I/O APIC (uam/dev/apic.h).
 *
 * IOREGSEL at 0x00, IOWIN at 0x10 and the EOI register at 0x40. Registers:
 * 0x00 ID, 0x01 version (24 entries), 0x02 arbitration, 0x10 + 2n the
 * redirection entries. Edge-triggered entries deliver on an asserting edge;
 * level-triggered ones deliver while asserted and Remote IRR is clear, which
 * the local APIC's EOI broadcast (or the EOI register) clears again.
 * Polarity applies to the input pins.
 */
#include <stdlib.h>

#include "uam/dev/apic.h"

#define RTE_VECTOR   0xFFull
#define RTE_MODE(e)  ((unsigned)((e) >> 8) & 7)
#define RTE_LOGICAL  (1ull << 11)
#define RTE_LOW      (1ull << 13)   /* active-low input */
#define RTE_RIRR     (1ull << 14)
#define RTE_LEVEL    (1ull << 15)
#define RTE_MASKED   (1ull << 16)
#define RTE_RW_MASK  0xFF0000000001AFFFull

typedef struct ioapic {
    uam_device      dev;
    uint8_t         id;
    uint8_t         sel;
    uint32_t        lines;      /* raw pin levels */
    uint64_t        rte[UAM_IOAPIC_PINS];
    uam_apic_msg_fn target;
    void           *target_ctx;
} ioapic;

static ioapic *from_dev(uam_device *d) { return UAM_CONTAINER_OF(d, ioapic, dev); }

static bool asserted(const ioapic *io, unsigned pin)
{
    bool raw = (io->lines >> pin) & 1;
    return (io->rte[pin] & RTE_LOW) ? !raw : raw;
}

static void deliver(ioapic *io, unsigned pin)
{
    uint64_t e = io->rte[pin];
    if (e & RTE_MASKED)
        return;
    bool level = (e & RTE_LEVEL) != 0;
    if (level) {
        if (e & RTE_RIRR)
            return;
        io->rte[pin] |= RTE_RIRR;
    }
    if (io->target)
        io->target(io->target_ctx, (uint8_t)(e & RTE_VECTOR), RTE_MODE(e), level,
                   (uint8_t)(e >> 56), (e & RTE_LOGICAL) != 0);
}

/* Level-triggered entries re-deliver while asserted. */
static void service_level(ioapic *io, unsigned pin)
{
    if ((io->rte[pin] & RTE_LEVEL) && asserted(io, pin))
        deliver(io, pin);
}

void uam_ioapic_input(void *ctx, unsigned pin, int level)
{
    ioapic *io = from_dev(ctx);
    if (pin >= UAM_IOAPIC_PINS)
        return;
    bool before = asserted(io, pin);
    if (level) io->lines |= 1u << pin;
    else       io->lines &= ~(1u << pin);
    bool now = asserted(io, pin);
    if (io->rte[pin] & RTE_LEVEL)
        service_level(io, pin);
    else if (now && !before)
        deliver(io, pin);
}

void uam_ioapic_eoi(void *ctx, uint8_t vector)
{
    ioapic *io = from_dev(ctx);
    for (unsigned pin = 0; pin < UAM_IOAPIC_PINS; pin++) {
        if ((io->rte[pin] & RTE_VECTOR) == vector && (io->rte[pin] & RTE_RIRR)) {
            io->rte[pin] &= ~RTE_RIRR;
            service_level(io, pin);
        }
    }
}

static uint32_t reg_read(ioapic *io, unsigned r)
{
    switch (r) {
    case 0: return (uint32_t)(io->id & 0xF) << 24;
    case 1: return ((UAM_IOAPIC_PINS - 1) << 16) | 0x20;
    case 2: return (uint32_t)(io->id & 0xF) << 24;
    }
    if (r >= 0x10 && r < 0x10 + 2 * UAM_IOAPIC_PINS) {
        uint64_t e = io->rte[(r - 0x10) / 2];
        return (r & 1) ? (uint32_t)(e >> 32) : (uint32_t)e;
    }
    return 0;
}

static void reg_write(ioapic *io, unsigned r, uint32_t v)
{
    if (r == 0) {
        io->id = (v >> 24) & 0xF;
        return;
    }
    if (r < 0x10 || r >= 0x10 + 2 * UAM_IOAPIC_PINS)
        return;
    unsigned pin = (r - 0x10) / 2;
    uint64_t e = io->rte[pin];
    if (r & 1)
        e = (e & 0xFFFFFFFFull) | ((uint64_t)v << 32);
    else
        e = (e & ~0xFFFFFFFFull) | v;
    io->rte[pin] = (io->rte[pin] & ~RTE_RW_MASK) | (e & RTE_RW_MASK);
    if (!(io->rte[pin] & RTE_LEVEL))
        io->rte[pin] &= ~RTE_RIRR;
    service_level(io, pin);   /* unmasking a held level line delivers it */
}

static uam_status ioapic_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    (void)size;
    ioapic *io = from_dev(d);
    if (off == 0x00)      *val = io->sel;
    else if (off == 0x10) *val = reg_read(io, io->sel);
    else                  *val = 0;
    return UAM_OK;
}

static uam_status ioapic_write(uam_device *d, uint64_t off, unsigned size, uint64_t v)
{
    (void)size;
    ioapic *io = from_dev(d);
    if (off == 0x00)      io->sel = (uint8_t)v;
    else if (off == 0x10) reg_write(io, io->sel, (uint32_t)v);
    else if (off == 0x40) uam_ioapic_eoi(io, (uint8_t)v);
    return UAM_OK;
}

static void ioapic_reset(uam_device *d)
{
    ioapic *io = from_dev(d);
    io->sel = 0;
    for (unsigned i = 0; i < UAM_IOAPIC_PINS; i++)
        io->rte[i] = RTE_MASKED;
}

static void ioapic_destroy(uam_device *d) { free(from_dev(d)); }

static void ioapic_save(uam_device *d, uam_state_writer *w)
{
    ioapic *io = from_dev(d);
    uam_state_put_u8(w, io->id);
    uam_state_put_u8(w, io->sel);
    uam_state_put_u32(w, io->lines);
    for (unsigned i = 0; i < UAM_IOAPIC_PINS; i++)
        uam_state_put_u64(w, io->rte[i]);
}

static void ioapic_restore(uam_device *d, uam_state_reader *r)
{
    ioapic *io = from_dev(d);
    io->id = uam_state_get_u8(r);
    io->sel = uam_state_get_u8(r);
    io->lines = uam_state_get_u32(r);
    for (unsigned i = 0; i < UAM_IOAPIC_PINS; i++)
        io->rte[i] = uam_state_get_u64(r);
}

static const uam_device_ops ioapic_ops = {
    .type = "x86,ioapic", .impl_min = 4, .impl_max = 4,
    .read = ioapic_read, .write = ioapic_write, .reset = ioapic_reset,
    .destroy = ioapic_destroy, .save = ioapic_save, .restore = ioapic_restore,
};

uam_device *uam_ioapic_create(const char *name, uint8_t id)
{
    ioapic *io = calloc(1, sizeof(*io));
    if (!io)
        return NULL;
    uam_device_init(&io->dev, &ioapic_ops, name);
    io->id = id;
    ioapic_reset(&io->dev);
    return &io->dev;
}

void uam_ioapic_set_target(uam_device *d, uam_apic_msg_fn fn, void *ctx)
{
    ioapic *io = from_dev(d);
    io->target = fn;
    io->target_ctx = ctx;
}
