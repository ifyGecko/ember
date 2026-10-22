/*
 * i8237.c - The PC's two 8237 DMA controllers and page registers (uam/dev/i8237.h).
 */
#include <stdlib.h>
#include <string.h>

#include "uam/dev/i8237.h"

typedef struct dma_ctl {
    uint16_t base_addr[4], cur_addr[4], base_count[4], cur_count[4];
    uint8_t  mode[4];
    uint8_t  command, status, request, mask, temp;
    bool     flip;           /* byte pointer: the next access is the high byte */
} dma_ctl;

typedef struct i8237 {
    uam_device dev;          /* controller 1 */
    uam_device dev2;         /* controller 2 (stateless view) */
    uam_device page_view;    /* page registers (stateless view) */
    uam_bus   *bus;
    dma_ctl    c[2];
    uint8_t    page[15];     /* ports 0x81-0x8F */
} i8237;

/* page register (port - 0x81) of each channel */
static const uint8_t page_of[8] = { 0x6, 0x2, 0x0, 0x1, 0xE, 0xA, 0x8, 0x9 };

enum { MODE_VERIFY = 0, MODE_WRITE = 1, MODE_READ = 2 };
#define MODE_AUTOINIT 0x10u
#define MODE_DEC      0x20u
#define CMD_DISABLE   0x04u

static i8237 *from1(uam_device *d) { return UAM_CONTAINER_OF(d, i8237, dev); }
static i8237 *from2(uam_device *d) { return UAM_CONTAINER_OF(d, i8237, dev2); }
static i8237 *fromp(uam_device *d) { return UAM_CONTAINER_OF(d, i8237, page_view); }

static void master_clear(dma_ctl *c)
{
    c->command = c->status = c->request = c->temp = 0;
    c->mask = 0xF;
    c->flip = false;
}

static uint8_t reg_read(dma_ctl *c, unsigned r)
{
    if (r < 8) {
        unsigned n = r / 2;
        uint16_t v = (r & 1) ? c->cur_count[n] : c->cur_addr[n];
        uint8_t b = c->flip ? (uint8_t)(v >> 8) : (uint8_t)v;
        c->flip = !c->flip;
        return b;
    }
    switch (r) {
    case 0x8: {   /* status: TC bits clear on read */
        uint8_t v = (uint8_t)(c->status | (c->request << 4));
        c->status &= 0xF0;
        return v;
    }
    case 0xD: return c->temp;
    case 0xF: return (uint8_t)(0xF0 | c->mask);
    default:  return 0xFF;
    }
}

static void reg_write(dma_ctl *c, unsigned r, uint8_t v)
{
    if (r < 8) {
        unsigned n = r / 2;
        uint16_t *base = (r & 1) ? &c->base_count[n] : &c->base_addr[n];
        uint16_t *cur = (r & 1) ? &c->cur_count[n] : &c->cur_addr[n];
        uint16_t nv = c->flip ? (uint16_t)((*base & 0x00FF) | (v << 8)) : (uint16_t)((*base & 0xFF00) | v);
        *base = *cur = nv;
        c->flip = !c->flip;
        return;
    }
    switch (r) {
    case 0x8: c->command = v; break;
    case 0x9:
        if (v & 4) c->request |= (uint8_t)(1u << (v & 3));
        else       c->request &= (uint8_t)~(1u << (v & 3));
        break;
    case 0xA:
        if (v & 4) c->mask |= (uint8_t)(1u << (v & 3));
        else       c->mask &= (uint8_t)~(1u << (v & 3));
        break;
    case 0xB: c->mode[v & 3] = v; break;
    case 0xC: c->flip = false; break;
    case 0xD: master_clear(c); break;
    case 0xE: c->mask = 0; break;
    case 0xF: c->mask = v & 0xF; break;
    default: break;
    }
}

static uam_status dma1_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    (void)size;
    *val = reg_read(&from1(d)->c[0], (unsigned)off & 0xF);
    return UAM_OK;
}

static uam_status dma1_write(uam_device *d, uint64_t off, unsigned size, uint64_t val)
{
    (void)size;
    reg_write(&from1(d)->c[0], (unsigned)off & 0xF, (uint8_t)val);
    return UAM_OK;
}

static uam_status dma2_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    (void)size;
    *val = (off & 1) ? 0xFF : reg_read(&from2(d)->c[1], (unsigned)(off >> 1) & 0xF);
    return UAM_OK;
}

static uam_status dma2_write(uam_device *d, uint64_t off, unsigned size, uint64_t val)
{
    (void)size;
    if (!(off & 1))
        reg_write(&from2(d)->c[1], (unsigned)(off >> 1) & 0xF, (uint8_t)val);
    return UAM_OK;
}

static uam_status page_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    (void)size;
    *val = off < 15 ? fromp(d)->page[off] : 0xFF;
    return UAM_OK;
}

static uam_status page_write(uam_device *d, uint64_t off, unsigned size, uint64_t val)
{
    (void)size;
    if (off < 15)
        fromp(d)->page[off] = (uint8_t)val;
    return UAM_OK;
}

/* ---- transfers ------------------------------------------------------------ */

bool uam_i8237_writes_memory(uam_device *d, unsigned chan)
{
    i8237 *k = from1(d);
    return chan < 8 && ((k->c[chan / 4].mode[chan % 4] >> 2) & 3) == MODE_WRITE;
}

unsigned uam_i8237_transfer(uam_device *d, unsigned chan, uint8_t *buf, unsigned len, bool *tc)
{
    i8237 *k = from1(d);
    if (tc)
        *tc = false;
    if (chan >= 8 || chan == 4)
        return 0;
    dma_ctl *c = &k->c[chan / 4];
    unsigned n = chan % 4;
    if ((c->mask >> n) & 1 || (c->command & CMD_DISABLE))
        return 0;
    bool word = chan >= 4;
    unsigned unit = word ? 2u : 1u;
    uint8_t mode = c->mode[n];
    unsigned type = (mode >> 2) & 3;
    unsigned done = 0;
    while (done + unit <= len) {
        uint64_t pa = word ? ((uint64_t)(k->page[page_of[chan]] & 0xFE) << 16) | ((uint64_t)c->cur_addr[n] << 1)
                           : ((uint64_t)k->page[page_of[chan]] << 16) | c->cur_addr[n];
        if (type == MODE_WRITE)
            uam_bus_dma_write(k->bus, pa, buf + done, unit);
        else if (type == MODE_READ)
            uam_bus_dma_read(k->bus, pa, buf + done, unit);
        done += unit;
        c->cur_addr[n] = (uint16_t)(c->cur_addr[n] + ((mode & MODE_DEC) ? -1 : 1));
        if (c->cur_count[n]-- == 0) {             /* terminal count */
            c->status |= (uint8_t)(1u << n);
            c->request &= (uint8_t)~(1u << n);
            if (tc)
                *tc = true;
            if (mode & MODE_AUTOINIT) {
                c->cur_addr[n] = c->base_addr[n];
                c->cur_count[n] = c->base_count[n];
            } else {
                c->mask |= (uint8_t)(1u << n);
            }
            break;
        }
    }
    return done;
}

/* ---- lifecycle -------------------------------------------------------------- */

static void i8237_reset(uam_device *d)
{
    i8237 *k = from1(d);
    memset(k->c, 0, sizeof(k->c));
    master_clear(&k->c[0]);
    master_clear(&k->c[1]);
    memset(k->page, 0, sizeof(k->page));
}

static void i8237_destroy(uam_device *d) { free(from1(d)); }

static void i8237_save(uam_device *d, uam_state_writer *w)
{
    i8237 *k = from1(d);
    for (unsigned i = 0; i < 2; i++) {
        const dma_ctl *c = &k->c[i];
        for (unsigned n = 0; n < 4; n++) {
            uam_state_put_u16(w, c->base_addr[n]);
            uam_state_put_u16(w, c->cur_addr[n]);
            uam_state_put_u16(w, c->base_count[n]);
            uam_state_put_u16(w, c->cur_count[n]);
            uam_state_put_u8(w, c->mode[n]);
        }
        const uint8_t b[] = { c->command, c->status, c->request, c->mask, c->temp, c->flip };
        uam_state_put_bytes(w, b, sizeof(b));
    }
    uam_state_put_bytes(w, k->page, sizeof(k->page));
}

static void i8237_restore(uam_device *d, uam_state_reader *r)
{
    i8237 *k = from1(d);
    for (unsigned i = 0; i < 2; i++) {
        dma_ctl *c = &k->c[i];
        for (unsigned n = 0; n < 4; n++) {
            c->base_addr[n] = uam_state_get_u16(r);
            c->cur_addr[n] = uam_state_get_u16(r);
            c->base_count[n] = uam_state_get_u16(r);
            c->cur_count[n] = uam_state_get_u16(r);
            c->mode[n] = uam_state_get_u8(r);
        }
        uint8_t b[6];
        uam_state_get_bytes(r, b, sizeof(b));
        c->command = b[0]; c->status = b[1]; c->request = b[2]; c->mask = b[3]; c->temp = b[4];
        c->flip = b[5] != 0;
    }
    uam_state_get_bytes(r, k->page, sizeof(k->page));
}

static const uam_device_ops dma1_ops = {
    .type = "pc,8237-dma", .impl_min = 1, .impl_max = 1,
    .read = dma1_read, .write = dma1_write, .reset = i8237_reset, .destroy = i8237_destroy,
    .save = i8237_save, .restore = i8237_restore,
};
static const uam_device_ops dma2_ops = {
    .type = "pc,8237-dma2", .impl_min = 1, .impl_max = 1,
    .read = dma2_read, .write = dma2_write, .flags = UAM_DEVICE_STATELESS,
};
static const uam_device_ops page_ops = {
    .type = "pc,dma-page", .impl_min = 1, .impl_max = 1,
    .read = page_read, .write = page_write, .flags = UAM_DEVICE_STATELESS,
};

uam_device *uam_i8237_create(const char *name, uam_bus *bus)
{
    i8237 *k = calloc(1, sizeof(*k));
    if (!k)
        return NULL;
    uam_device_init(&k->dev, &dma1_ops, name);
    uam_device_init(&k->dev2, &dma2_ops, name);
    uam_device_init(&k->page_view, &page_ops, name);
    k->bus = bus;
    i8237_reset(&k->dev);
    return &k->dev;
}

uam_device *uam_i8237_dma1(uam_device *d) { return d; }
uam_device *uam_i8237_dma2(uam_device *d) { return &from1(d)->dev2; }
uam_device *uam_i8237_page(uam_device *d) { return &from1(d)->page_view; }
