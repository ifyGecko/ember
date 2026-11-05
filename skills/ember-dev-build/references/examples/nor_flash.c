/*
 * nor_flash.c - parallel NOR flash, AMD/JEDEC command subset (nor_flash.h). Driver:
 * flashProgram 0x........ (AA/55/A0 then data, polls DQ7), flashErase 0x........
 * (AA/55/80/AA/55/30), flashId 0x........ (AA/55/90, reads +0 / +2).
 */
#include <stdlib.h>
#include <string.h>

#include "nor_flash.h"

enum { ST_READ, ST_UNLOCK1, ST_UNLOCK2, ST_PROGRAM, ST_ERASE1, ST_ERASE2, ST_ID };

typedef struct nor_flash {
    uam_device  dev;
    uam_bus    *bus;
    uam_region *region;
    uint64_t    sector;
    uint32_t    id;
    int         state;
    bool        erase_armed;
} nor_flash;

static nor_flash *to_nf(uam_device *d) { return UAM_CONTAINER_OF(d, nor_flash, dev); }

static void to_read_mode(nor_flash *f)
{
    f->state = ST_READ;
    f->erase_armed = false;
    uam_bus_romd_set_mode(f->bus, f->region, true);  /* reads/fetches from the array again */
}

/* Only reached out of ROM mode (ID / status reads). */
static uam_status nf_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    nor_flash *f = to_nf(d);
    (void)size;
    if (f->state == ST_ID)
        *val = (off & 0xFF) == 0 ? f->id >> 16 : (off & 0xFF) == 2 ? (f->id & 0xFFFF) : 0;
    else
        *val = 0x80;            /* DQ7 = 1: operation complete (they complete instantly) */
    return UAM_OK;
}

static uam_status nf_write(uam_device *d, uint64_t off, unsigned size, uint64_t v)
{
    nor_flash *f = to_nf(d);
    uint8_t *mem = uam_bus_romd_backing(f->region);
    uint8_t cmd = (uint8_t)v;
    if (cmd == 0xF0 && f->state != ST_PROGRAM) {        /* reset to read array */
        to_read_mode(f);
        return UAM_OK;
    }
    switch (f->state) {
    case ST_READ: case ST_ID:
        f->state = cmd == 0xAA ? ST_UNLOCK1 : f->state;
        break;
    case ST_UNLOCK1: f->state = cmd == 0x55 ? ST_UNLOCK2 : ST_READ; break;
    case ST_UNLOCK2:
        if (cmd == 0xA0) {
            f->state = ST_PROGRAM;
        } else if (cmd == 0x90) {
            f->state = ST_ID;
            uam_bus_romd_set_mode(f->bus, f->region, false);   /* reads come to nf_read */
        } else if (cmd == 0x80) {
            f->state = ST_ERASE1;
        } else if (cmd == 0x30 && f->erase_armed) {        /* sector erase */
            uint64_t base = off & ~(f->sector - 1);
            memset(mem + base, 0xFF, f->sector);
            uam_bus_romd_dirty(f->bus, f->region, base, f->sector);
            to_read_mode(f);
        } else {
            f->state = ST_READ;
        }
        break;
    case ST_ERASE1: f->state = cmd == 0xAA ? ST_ERASE2 : ST_READ; break;
    case ST_ERASE2: f->state = cmd == 0x55 ? ST_UNLOCK2 : ST_READ; f->erase_armed = true; break;
    case ST_PROGRAM:                                     /* NOR programming only clears bits */
        for (unsigned i = 0; i < size; i++)
            mem[off + i] &= (uint8_t)(v >> (8 * (size - 1 - i)));   /* big-endian value order */
        uam_bus_romd_dirty(f->bus, f->region, off, size);
        to_read_mode(f);
        break;
    }
    return UAM_OK;
}

static void nf_reset(uam_device *d)
{
    nor_flash *f = to_nf(d);
    if (f->region)
        to_read_mode(f);
}

static void nf_destroy(uam_device *d) { free(to_nf(d)); }

/* Snapshots: the command state only. The array (the ROMD backing store) and the region's
 * ROM/device mode are memory, which the snapshot saves and restores before calling us -
 * provided every direct write to the backing store was reported (uam_bus_romd_dirty). */
static void nf_save(uam_device *d, uam_state_writer *w)
{
    nor_flash *f = to_nf(d);
    uam_state_put_u8(w, (uint8_t)f->state);
    uam_state_put_bool(w, f->erase_armed);
}

static void nf_restore(uam_device *d, uam_state_reader *r)
{
    nor_flash *f = to_nf(d);
    f->state = uam_state_get_u8(r);
    f->erase_armed = uam_state_get_bool(r);
    if (f->state > ST_ID)
        uam_state_fail(r, "unknown command state %d", f->state);
}

static const uam_device_ops nor_flash_ops = {
    .type = "nor-flash",
    .endian = UAM_ACC_BE,
    .read = nf_read,
    .write = nf_write,
    .reset = nf_reset,
    .destroy = nf_destroy,
    .save = nf_save,
    .restore = nf_restore,
};

uam_device *nor_flash_create(const char *name, uam_bus *bus, uint64_t sector, uint32_t id)
{
    nor_flash *f = calloc(1, sizeof(*f));
    if (!f)
        return NULL;
    uam_device_init(&f->dev, &nor_flash_ops, name);
    f->bus = bus;
    f->sector = sector;
    f->id = id;
    return &f->dev;
}

void nor_flash_set_region(uam_device *d, uam_region *region) { to_nf(d)->region = region; }
