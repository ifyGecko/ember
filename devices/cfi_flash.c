/*
 * cfi_flash.c - Intel/Sharp command-set NOR flash as a ROM device
 * (uam/dev/cfi_flash.h).
 */
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "uam/dev/cfi_flash.h"

enum { MODE_ARRAY, MODE_STATUS, MODE_ID, MODE_QUERY };
enum { PEND_NONE, PEND_PROGRAM, PEND_ERASE, PEND_LOCK };
enum { SR_READY = 0x80, SR_ERASE_ERR = 0x20, SR_PROG_ERR = 0x10, SR_PROTECT = 0x02 };

typedef struct flash {
    uam_device  dev;
    uam_bus    *bus;
    uam_region *region;
    uint64_t    size, block;
    uint8_t     mode, pending, status;
    bool        protect;
    uint8_t     query[0x40];
    bool      (*gate)(void *ctx, uint64_t off);
    void       *gate_ctx;
    int         fd;          /* write-through backing file, or -1 */
    uint8_t     manuf, devid; /* read-identifier codes (board-supplied) */
} flash;

static flash *from_dev(uam_device *d) { return UAM_CONTAINER_OF(d, flash, dev); }

static void set_mode(flash *f, uint8_t mode)
{
    f->mode = mode;
    uam_bus_romd_set_mode(f->bus, f->region, mode == MODE_ARRAY);
}

static uint8_t read_byte(flash *f, uint64_t off)
{
    uint8_t *mem = uam_bus_romd_backing(f->region);
    switch (f->mode) {
    case MODE_STATUS: return f->status;
    case MODE_ID:
        switch (off & 3) {
        case 0: return f->manuf;
        case 1: return f->devid;
        default: return 0;     /* block unlocked */
        }
    case MODE_QUERY: {
        uint64_t q = off & (f->block - 1);
        return q < sizeof(f->query) ? f->query[q] : 0;
    }
    default: return mem[off];
    }
}

static uam_status flash_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    flash *f = from_dev(d);
    uint64_t v = 0;
    for (unsigned i = 0; i < size && off + i < f->size; i++)
        v |= (uint64_t)read_byte(f, off + i) << (8 * i);
    *val = v;
    return UAM_OK;
}

static bool may_write(flash *f, uint64_t off)
{
    return !f->protect && (!f->gate || f->gate(f->gate_ctx, off));
}

static void write_through(flash *f, uint64_t off, uint64_t len)
{
    if (f->fd < 0)
        return;
    const uint8_t *mem = uam_bus_romd_backing(f->region) + off;
    while (len) {
        ssize_t n = pwrite(f->fd, mem, len, (off_t)off);
        if (n <= 0) {
            uam_log(UAM_LOG_WARN, "cfi-flash", "%s: write-through failed", f->dev.name);
            return;
        }
        mem += n;
        off += (uint64_t)n;
        len -= (uint64_t)n;
    }
}

static void program(flash *f, uint64_t off, uint8_t v)
{
    if (!may_write(f, off)) {
        f->status |= SR_PROG_ERR | SR_PROTECT;
        return;
    }
    uint8_t *mem = uam_bus_romd_backing(f->region);
    mem[off] &= v;   /* NOR programming only clears bits */
    uam_bus_romd_dirty(f->bus, f->region, off, 1);
    write_through(f, off, 1);
}

static void erase(flash *f, uint64_t off)
{
    if (!may_write(f, off)) {
        f->status |= SR_ERASE_ERR | SR_PROTECT;
        return;
    }
    uint64_t base = off & ~(f->block - 1);
    memset(uam_bus_romd_backing(f->region) + base, 0xFF, f->block);
    uam_bus_romd_dirty(f->bus, f->region, base, f->block);
    write_through(f, base, f->block);
}

static void write_byte(flash *f, uint64_t off, uint8_t v)
{
    uint8_t p = f->pending;
    f->pending = PEND_NONE;
    switch (p) {
    case PEND_PROGRAM:
        program(f, off, v);
        set_mode(f, MODE_STATUS);
        return;
    case PEND_ERASE:
        if (v == 0xD0)
            erase(f, off);
        else
            f->status |= SR_ERASE_ERR | SR_PROG_ERR;   /* command sequence error */
        set_mode(f, MODE_STATUS);
        return;
    case PEND_LOCK:
        set_mode(f, MODE_STATUS);
        return;
    default:
        break;
    }
    switch (v) {
    case 0xFF: set_mode(f, MODE_ARRAY); break;
    case 0x70: set_mode(f, MODE_STATUS); break;
    case 0x50: f->status = SR_READY; break;
    case 0x90: set_mode(f, MODE_ID); break;
    case 0x98: set_mode(f, MODE_QUERY); break;
    case 0x40: case 0x10: f->pending = PEND_PROGRAM; set_mode(f, MODE_STATUS); break;
    case 0x20: f->pending = PEND_ERASE; set_mode(f, MODE_STATUS); break;
    case 0x60: f->pending = PEND_LOCK; set_mode(f, MODE_STATUS); break;
    default:   break;   /* unknown commands are ignored */
    }
}

static uam_status flash_write(uam_device *d, uint64_t off, unsigned size, uint64_t v)
{
    flash *f = from_dev(d);
    for (unsigned i = 0; i < size && off + i < f->size; i++)
        write_byte(f, off + i, (uint8_t)(v >> (8 * i)));
    return UAM_OK;
}

static void flash_reset(uam_device *d)
{
    flash *f = from_dev(d);
    f->pending = PEND_NONE;
    f->status = SR_READY;
    set_mode(f, MODE_ARRAY);
}

static void flash_destroy(uam_device *d)
{
    flash *f = from_dev(d);
    if (f->fd >= 0)
        close(f->fd);
    free(f);
}

/* The array itself is RAM-like backing store: the bus's snapshot code saves it. */
static void flash_save(uam_device *d, uam_state_writer *w)
{
    flash *f = from_dev(d);
    uam_state_put_u8(w, f->mode);
    uam_state_put_u8(w, f->pending);
    uam_state_put_u8(w, f->status);
    uam_state_put_bool(w, f->protect);
}

static void flash_restore(uam_device *d, uam_state_reader *r)
{
    flash *f = from_dev(d);
    uint8_t mode = uam_state_get_u8(r);
    f->pending = uam_state_get_u8(r);
    f->status = uam_state_get_u8(r);
    f->protect = uam_state_get_bool(r);
    set_mode(f, mode);
}

static const uam_device_ops flash_ops = {
    .type = "cfi-flash", .read = flash_read, .write = flash_write, .reset = flash_reset,
    .destroy = flash_destroy, .save = flash_save, .restore = flash_restore,
};

static void build_query(flash *f)
{
    uint8_t *q = f->query;
    unsigned log2size = 0, nblocks = (unsigned)(f->size / f->block) - 1;
    while ((1ull << log2size) < f->size)
        log2size++;
    q[0x10] = 'Q'; q[0x11] = 'R'; q[0x12] = 'Y';
    q[0x13] = 0x01;                          /* Intel/Sharp extended command set */
    q[0x1B] = 0x27; q[0x1C] = 0x36;          /* Vcc 2.7-3.6 V */
    q[0x1F] = 4; q[0x21] = 10;               /* typical program 16 us, block erase 1 s */
    q[0x23] = 4; q[0x25] = 4;
    q[0x27] = (uint8_t)log2size;
    q[0x28] = 0x00;                          /* x8 interface */
    q[0x2C] = 1;                             /* one erase-block region */
    q[0x2D] = (uint8_t)nblocks; q[0x2E] = (uint8_t)(nblocks >> 8);
    q[0x2F] = (uint8_t)(f->block >> 8); q[0x30] = (uint8_t)(f->block >> 16);
}

uam_device *uam_cfi_flash_map(uam_bus *bus, const char *name, uint64_t base, uint64_t size,
                              uint64_t block_size, const void *data, size_t len)
{
    if (!size || !block_size || (block_size & (block_size - 1)) || size % block_size)
        return NULL;
    flash *f = calloc(1, sizeof(*f));
    if (!f)
        return NULL;
    uam_device_init(&f->dev, &flash_ops, name);
    f->bus = bus;
    f->size = size;
    f->block = block_size;
    f->status = SR_READY;
    f->fd = -1;
    build_query(f);
    if (uam_bus_map_romd(bus, name, base, size, &f->dev, NULL, 0, NULL, &f->region) != UAM_OK) {
        free(f);
        return NULL;
    }
    uint8_t *mem = uam_bus_romd_backing(f->region);
    memset(mem, 0xFF, size);
    if (data && len)
        memcpy(mem, data, len < size ? len : size);
    return &f->dev;
}

uam_region *uam_cfi_flash_region(uam_device *d) { return from_dev(d)->region; }

void uam_cfi_flash_set_protected(uam_device *d, bool on) { from_dev(d)->protect = on; }

void uam_cfi_flash_set_write_gate(uam_device *d, bool (*allowed)(void *ctx, uint64_t off), void *ctx)
{
    flash *f = from_dev(d);
    f->gate = allowed;
    f->gate_ctx = ctx;
}

uam_status uam_cfi_flash_set_backing_file(uam_device *d, const char *path)
{
    flash *f = from_dev(d);
    int fd = open(path, O_RDWR);
    if (fd < 0) {
        uam_log(UAM_LOG_ERROR, "cfi-flash", "cannot open %s for writing", path);
        return UAM_ERR_INVALID;
    }
    if (f->fd >= 0)
        close(f->fd);
    f->fd = fd;
    return UAM_OK;
}

void uam_cfi_flash_set_ids(uam_device *dev, uint8_t manufacturer, uint8_t device)
{
    flash *f = from_dev(dev);
    f->manuf = manufacturer;
    f->devid = device;
}
