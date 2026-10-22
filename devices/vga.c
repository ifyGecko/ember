/*
 * vga.c - VGA-compatible display adapter (uam/dev/vga.h).
 *
 * Display memory is stored with the four planes interleaved: plane p's byte at
 * offset o is vram[o * 4 + p], so chain-4 addressing (mode 13h) is the linear
 * address itself.
 */
#include <stdlib.h>
#include <string.h>

#include "uam/dev/vga.h"

typedef struct vga {
    uam_device   io;            /* owns the state */
    uam_device   mem;
    uam_pci_func fn;
    uint8_t      misc, vss, feature;
    uint8_t      seq_idx, seq[8];
    uint8_t      crtc_idx, crtc[0x20];
    uint8_t      gc_idx, gc[16];
    uint8_t      attr_idx, attr[0x20];
    bool         attr_ff;       /* attribute controller: next write is data */
    uint8_t      dac_mask, dac_state, dac_rd, dac_wr, dac_sub;
    uint8_t      dac[256][3];
    uint8_t      latch[4];
    uint32_t     status_reads;  /* input status 1 reads: the retrace pattern */
    uint8_t      vram[UAM_VGA_VRAM_SIZE];
} vga;

static vga *from_io(uam_device *d)  { return UAM_CONTAINER_OF(d, vga, io); }
static vga *from_mem(uam_device *d) { return UAM_CONTAINER_OF(d, vga, mem); }

/* ---- ports ------------------------------------------------------------------ */

static bool color(const vga *v) { return v->misc & 1; }

static uint8_t port_read(vga *v, unsigned port)
{
    if (((port == 0x3B4 || port == 0x3B5 || port == 0x3BA) && color(v)) ||
        ((port == 0x3D4 || port == 0x3D5 || port == 0x3DA) && !color(v)))
        return 0xFF;
    switch (port) {
    case 0x3C0: return v->attr_idx;
    case 0x3C1: return v->attr[v->attr_idx & 0x1F];
    case 0x3C2: return 0x10;                            /* switch sense: a colour display */
    case 0x3C3: return v->vss;
    case 0x3C4: return v->seq_idx;
    case 0x3C5: return v->seq[v->seq_idx & 7];
    case 0x3C6: return v->dac_mask;
    case 0x3C7: return v->dac_state;
    case 0x3C8: return v->dac_wr;
    case 0x3C9: {
        uint8_t b = v->dac[v->dac_rd][v->dac_sub];
        if (++v->dac_sub == 3) {
            v->dac_sub = 0;
            v->dac_rd++;
        }
        return b;
    }
    case 0x3CA: return v->feature;
    case 0x3CC: return v->misc;
    case 0x3CE: return v->gc_idx;
    case 0x3CF: return v->gc[v->gc_idx & 15];
    case 0x3B4: case 0x3D4: return v->crtc_idx;
    case 0x3B5: case 0x3D5: return v->crtc_idx < 0x19 ? v->crtc[v->crtc_idx] : 0xFF;
    case 0x3BA: case 0x3DA: {                           /* input status 1 */
        v->attr_ff = false;
        uint32_t n = v->status_reads++;
        return (uint8_t)((n & 1) | ((n & 4) ? 0x08 : 0));   /* blanking, vertical retrace */
    }
    default: return 0xFF;
    }
}

static void port_write(vga *v, unsigned port, uint8_t b)
{
    if (((port == 0x3B4 || port == 0x3B5 || port == 0x3BA) && color(v)) ||
        ((port == 0x3D4 || port == 0x3D5 || port == 0x3DA) && !color(v)))
        return;
    switch (port) {
    case 0x3C0:
        if (!v->attr_ff)
            v->attr_idx = b & 0x3F;
        else if ((v->attr_idx & 0x1F) < 0x15)
            v->attr[v->attr_idx & 0x1F] = b;
        v->attr_ff = !v->attr_ff;
        break;
    case 0x3C2: v->misc = b; break;
    case 0x3C3: v->vss = b; break;
    case 0x3C4: v->seq_idx = b; break;
    case 0x3C5: if ((v->seq_idx & 7) < 5) v->seq[v->seq_idx & 7] = b; break;
    case 0x3C6: v->dac_mask = b; break;
    case 0x3C7: v->dac_rd = b; v->dac_sub = 0; v->dac_state = 0x03; break;
    case 0x3C8: v->dac_wr = b; v->dac_sub = 0; v->dac_state = 0x00; break;
    case 0x3C9:
        v->dac[v->dac_wr][v->dac_sub] = b & 0x3F;
        if (++v->dac_sub == 3) {
            v->dac_sub = 0;
            v->dac_wr++;
        }
        break;
    case 0x3CE: v->gc_idx = b; break;
    case 0x3CF: if ((v->gc_idx & 15) < 9) v->gc[v->gc_idx & 15] = b; break;
    case 0x3B4: case 0x3D4: v->crtc_idx = b; break;
    case 0x3B5: case 0x3D5: {
        unsigned i = v->crtc_idx;
        if (i >= 0x19)
            break;
        if ((v->crtc[0x11] & 0x80) && i <= 7) {         /* write protected; overflow bit 4 is not */
            if (i == 7)
                v->crtc[7] = (uint8_t)((v->crtc[7] & ~0x10) | (b & 0x10));
            break;
        }
        v->crtc[i] = b;
        break;
    }
    case 0x3BA: case 0x3DA: v->feature = b; break;
    default: break;
    }
}

static uam_status io_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    vga *v = from_io(d);
    *val = 0;
    for (unsigned i = 0; i < size; i++)
        *val |= (uint64_t)port_read(v, 0x3B0 + (unsigned)off + i) << (8 * i);
    return UAM_OK;
}

static uam_status io_write(uam_device *d, uint64_t off, unsigned size, uint64_t val)
{
    vga *v = from_io(d);
    for (unsigned i = 0; i < size; i++)
        port_write(v, 0x3B0 + (unsigned)off + i, (uint8_t)(val >> (8 * i)));
    return UAM_OK;
}

/* ---- display memory ----------------------------------------------------------- */

/* The offset into the memory map's window, or -1 if the address is not mapped. */
static long window(const vga *v, uint64_t off)
{
    static const uint32_t base[4] = { 0x00000, 0x00000, 0x10000, 0x18000 };
    static const uint32_t size[4] = { 0x20000, 0x10000, 0x08000, 0x08000 };
    unsigned m = (v->gc[6] >> 2) & 3;
    if (off < base[m] || off >= base[m] + size[m])
        return -1;
    return (long)(off - base[m]);
}

static uint8_t *plane(vga *v, uint32_t o, unsigned p) { return &v->vram[((o & 0xFFFF) << 2) | p]; }

static uint8_t mem_read8(vga *v, uint64_t off)
{
    long r = window(v, off);
    if (r < 0)
        return 0xFF;
    uint32_t a = (uint32_t)r;
    if (v->seq[4] & 0x08)                               /* chain-4 */
        return *plane(v, a >> 2, a & 3);
    if (!(v->seq[4] & 0x04))                            /* odd/even */
        return *plane(v, a & ~1u, (v->gc[4] & 2) | (a & 1));
    for (unsigned p = 0; p < 4; p++)
        v->latch[p] = *plane(v, a, p);
    if (!(v->gc[5] & 0x08))                             /* read mode 0 */
        return v->latch[v->gc[4] & 3];
    uint8_t r1 = 0;                                     /* read mode 1: colour compare */
    for (unsigned bit = 0; bit < 8; bit++) {
        bool match = true;
        for (unsigned p = 0; p < 4; p++)
            if ((v->gc[7] >> p) & 1)
                match &= ((v->latch[p] >> bit) & 1) == ((v->gc[2] >> p) & 1);
        if (match)
            r1 |= (uint8_t)(1u << bit);
    }
    return r1;
}

static uint8_t alu(const vga *v, uint8_t val, unsigned p)
{
    switch ((v->gc[3] >> 3) & 3) {
    case 1:  return val & v->latch[p];
    case 2:  return val | v->latch[p];
    case 3:  return val ^ v->latch[p];
    default: return val;
    }
}

static void mem_write8(vga *v, uint64_t off, uint8_t b)
{
    long r = window(v, off);
    if (r < 0)
        return;
    uint32_t a = (uint32_t)r;
    uint8_t mask = v->seq[2] & 0x0F;
    if (v->seq[4] & 0x08) {                             /* chain-4 */
        if (mask & (1u << (a & 3)))
            *plane(v, a >> 2, a & 3) = b;
        return;
    }
    if (!(v->seq[4] & 0x04)) {                          /* odd/even */
        unsigned p = a & 1;
        for (unsigned q = p; q < 4; q += 2)
            if (mask & (1u << q))
                *plane(v, a & ~1u, q) = b;
        return;
    }
    unsigned rot = v->gc[3] & 7;
    uint8_t data = (uint8_t)((b >> rot) | (b << ((8 - rot) & 7)));
    uint8_t bitmask = v->gc[8];
    for (unsigned p = 0; p < 4; p++) {
        if (!(mask & (1u << p)))
            continue;
        uint8_t res;
        uint8_t sr = ((v->gc[0] >> p) & 1) ? 0xFF : 0x00;
        switch (v->gc[5] & 3) {
        case 0: {
            uint8_t val = ((v->gc[1] >> p) & 1) ? sr : data;
            res = (uint8_t)((alu(v, val, p) & bitmask) | (v->latch[p] & ~bitmask));
            break;
        }
        case 1:
            res = v->latch[p];
            break;
        case 2: {
            uint8_t val = ((b >> p) & 1) ? 0xFF : 0x00;
            res = (uint8_t)((alu(v, val, p) & bitmask) | (v->latch[p] & ~bitmask));
            break;
        }
        default: {
            uint8_t m = data & bitmask;
            res = (uint8_t)((sr & m) | (v->latch[p] & ~m));
            break;
        }
        }
        *plane(v, a, p) = res;
    }
}

static uam_status mem_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    vga *v = from_mem(d);
    *val = 0;
    for (unsigned i = 0; i < size; i++)
        *val |= (uint64_t)mem_read8(v, off + i) << (8 * i);
    return UAM_OK;
}

static uam_status mem_write(uam_device *d, uint64_t off, unsigned size, uint64_t val)
{
    vga *v = from_mem(d);
    for (unsigned i = 0; i < size; i++)
        mem_write8(v, off + i, (uint8_t)(val >> (8 * i)));
    return UAM_OK;
}

/* ---- the display, for the host ----------------------------------------------- */

static unsigned vertical_lines(const vga *v)
{
    return (v->crtc[0x12] | (unsigned)(v->crtc[7] & 0x02) << 7 | (unsigned)(v->crtc[7] & 0x40) << 3) + 1;
}

static uint32_t start_address(const vga *v) { return (uint32_t)v->crtc[0x0C] << 8 | v->crtc[0x0D]; }

/* The memory address of character counter value `n` (byte, word or dword mode). */
static uint32_t crtc_addr(const vga *v, uint32_t n)
{
    if (v->crtc[0x14] & 0x40) return n << 2;
    if (!(v->crtc[0x17] & 0x40)) return n << 1;
    return n;
}

bool uam_vga_text_mode(uam_device *d, unsigned *cols, unsigned *rows)
{
    vga *v = from_io(d);
    unsigned ch = (v->crtc[9] & 0x1F) + 1;
    if (cols) *cols = v->crtc[1] + 1u;
    if (rows) *rows = vertical_lines(v) / ch;
    return !(v->gc[6] & 1);
}

unsigned uam_vga_text(uam_device *d, uint16_t *cells, unsigned max)
{
    vga *v = from_io(d);
    unsigned cols, rows;
    uam_vga_text_mode(d, &cols, &rows);
    uint32_t pitch = v->crtc[0x13] * 2u, start = start_address(v);
    unsigned n = 0;
    for (unsigned r = 0; r < rows; r++)
        for (unsigned c = 0; c < cols && n < max; c++) {
            uint32_t a = crtc_addr(v, start + r * pitch + c);
            cells[n++] = (uint16_t)(*plane(v, a, 0) | *plane(v, a, 1) << 8);
        }
    return n;
}

void uam_vga_text_dump(uam_device *d, FILE *out)
{
    unsigned cols, rows;
    uam_vga_text_mode(d, &cols, &rows);
    if (cols > 256 || rows > 128)
        return;
    uint16_t cells[256 * 128];
    unsigned n = uam_vga_text(d, cells, cols * rows);
    for (unsigned r = 0; r * cols < n; r++) {
        char line[257];
        unsigned len = 0;
        for (unsigned c = 0; c < cols; c++) {
            uint8_t ch = (uint8_t)cells[r * cols + c];
            line[c] = ch >= 0x20 && ch < 0x7F ? (char)ch : ch == 0 ? ' ' : '.';
            if (line[c] != ' ')
                len = c + 1;
        }
        line[len] = 0;
        fprintf(out, "%s\n", line);
    }
}

/* An attribute-controller colour (0-15) to RGB through the palette and the DAC. */
static uint32_t rgb_of(const vga *v, unsigned dac_index)
{
    const uint8_t *e = v->dac[dac_index & v->dac_mask];
    return (uint32_t)(e[0] * 255 / 63) << 16 | (uint32_t)(e[1] * 255 / 63) << 8 | (uint32_t)(e[2] * 255 / 63);
}

static uint32_t attr_rgb(const vga *v, unsigned c)
{
    uint8_t pal = v->attr[c & 0x0F];
    unsigned idx = (v->attr[0x10] & 0x80) ? ((pal & 0x0F) | (v->attr[0x14] & 0x0F) << 4)
                                          : ((pal & 0x3F) | (v->attr[0x14] & 0x0C) << 4);
    return rgb_of(v, idx);
}

const uint8_t *uam_vga_vram(uam_device *d) { return from_io(d)->vram; }

bool uam_vga_render(uam_device *d, uint32_t *rgb, unsigned maxw, unsigned maxh, unsigned *w,
                    unsigned *h)
{
    vga *v = from_io(d);
    unsigned scan = (v->crtc[9] & 0x1F) + 1, dbl = (v->crtc[9] & 0x80) ? 2 : 1;
    uint32_t start = start_address(v), pitch = v->crtc[0x13] * 2u;
    if (!(v->gc[6] & 1)) {                              /* text, 8-dot characters */
        unsigned cols = v->crtc[1] + 1u, rows = vertical_lines(v) / scan;
        *w = cols * 8;
        *h = rows * scan;
        if (*w > maxw || *h > maxh)
            return false;
        static const uint32_t map_off[8] = { 0x0000, 0x4000, 0x8000, 0xC000, 0x2000, 0x6000, 0xA000, 0xE000 };
        unsigned sa = ((v->seq[3] >> 2) & 3) | ((v->seq[3] >> 3) & 4);
        unsigned sb = (v->seq[3] & 3) | ((v->seq[3] >> 2) & 4);
        for (unsigned r = 0; r < rows; r++)
            for (unsigned c = 0; c < cols; c++) {
                uint32_t a = crtc_addr(v, start + r * pitch + c);
                uint8_t ch = *plane(v, a, 0), at = *plane(v, a, 1);
                uint32_t font = map_off[(at & 0x08) ? sa : sb] + ch * 32u;
                uint32_t fg = attr_rgb(v, at & 0x0F);
                uint32_t bg = attr_rgb(v, (v->attr[0x10] & 0x08) ? (at >> 4) & 7 : at >> 4);
                for (unsigned y = 0; y < scan; y++) {
                    uint8_t bits = *plane(v, font + y, 2);
                    for (unsigned x = 0; x < 8; x++)
                        rgb[(r * scan + y) * *w + c * 8 + x] = (bits & (0x80 >> x)) ? fg : bg;
                }
            }
        return true;
    }
    unsigned lines = vertical_lines(v) / (v->crtc[9] & 0x80 ? 2 : 1) / ((v->crtc[9] & 0x1F) + 1);
    (void)dbl;
    if (v->attr[0x10] & 0x40) {                         /* 256 colours, chain-4 */
        *w = (v->crtc[1] + 1u) * 4;
        *h = lines;
        if (*w > maxw || *h > maxh)
            return false;
        for (unsigned y = 0; y < *h; y++)
            for (unsigned x = 0; x < *w; x++)
                rgb[y * *w + x] = rgb_of(v, v->vram[(start * 4 + y * pitch * 4 + x) & (UAM_VGA_VRAM_SIZE - 1)]);
        return true;
    }
    if (v->gc[5] & 0x60)                                /* the CGA-compatible shift modes */
        return false;
    *w = (v->crtc[1] + 1u) * 8;                         /* 16 colours, planar */
    *h = lines;
    if (*w > maxw || *h > maxh)
        return false;
    for (unsigned y = 0; y < *h; y++)
        for (unsigned x = 0; x < *w; x++) {
            uint32_t o = start + y * pitch + x / 8;
            unsigned bit = 7 - (x & 7), c = 0;
            for (unsigned p = 0; p < 4; p++)
                c |= ((*plane(v, o, p) >> bit) & 1u) << p;
            rgb[y * *w + x] = attr_rgb(v, c & v->attr[0x12]);
        }
    return true;
}

/* ---- lifecycle ------------------------------------------------------------------ */

static void vga_reset(uam_device *d)
{
    vga *v = from_io(d);
    v->misc = v->vss = v->feature = 0;
    v->seq_idx = v->crtc_idx = v->gc_idx = v->attr_idx = 0;
    memset(v->seq, 0, sizeof(v->seq));
    memset(v->crtc, 0, sizeof(v->crtc));
    memset(v->gc, 0, sizeof(v->gc));
    memset(v->attr, 0, sizeof(v->attr));
    v->seq[4] = 0x04;                                   /* planar until a mode is set */
    v->seq[2] = 0x0F;
    v->gc[8] = 0xFF;
    v->attr_ff = false;
    v->dac_mask = 0xFF;
    v->dac_state = v->dac_rd = v->dac_wr = v->dac_sub = 0;
    memset(v->latch, 0, sizeof(v->latch));
    v->status_reads = 0;
}

static void vga_destroy(uam_device *d) { free(from_io(d)); }
static void mem_destroy(uam_device *d) { (void)d; }

static void vga_save(uam_device *d, uam_state_writer *w)
{
    vga *v = from_io(d);
    const uint8_t b[] = { v->misc, v->vss, v->feature, v->seq_idx, v->crtc_idx, v->gc_idx,
                          v->attr_idx, v->attr_ff, v->dac_mask, v->dac_state, v->dac_rd,
                          v->dac_wr, v->dac_sub };
    uam_state_put_bytes(w, b, sizeof(b));
    uam_state_put_bytes(w, v->seq, sizeof(v->seq));
    uam_state_put_bytes(w, v->crtc, sizeof(v->crtc));
    uam_state_put_bytes(w, v->gc, sizeof(v->gc));
    uam_state_put_bytes(w, v->attr, sizeof(v->attr));
    uam_state_put_bytes(w, &v->dac[0][0], sizeof(v->dac));
    uam_state_put_bytes(w, v->latch, sizeof(v->latch));
    uam_state_put_u32(w, v->status_reads);
    uam_state_put_bytes(w, v->vram, sizeof(v->vram));
}

static void vga_restore(uam_device *d, uam_state_reader *r)
{
    vga *v = from_io(d);
    uint8_t b[13];
    uam_state_get_bytes(r, b, sizeof(b));
    v->misc = b[0]; v->vss = b[1]; v->feature = b[2]; v->seq_idx = b[3]; v->crtc_idx = b[4];
    v->gc_idx = b[5]; v->attr_idx = b[6]; v->attr_ff = b[7]; v->dac_mask = b[8];
    v->dac_state = b[9]; v->dac_rd = b[10]; v->dac_wr = b[11]; v->dac_sub = b[12] % 3;
    uam_state_get_bytes(r, v->seq, sizeof(v->seq));
    uam_state_get_bytes(r, v->crtc, sizeof(v->crtc));
    uam_state_get_bytes(r, v->gc, sizeof(v->gc));
    uam_state_get_bytes(r, v->attr, sizeof(v->attr));
    uam_state_get_bytes(r, &v->dac[0][0], sizeof(v->dac));
    uam_state_get_bytes(r, v->latch, sizeof(v->latch));
    v->status_reads = uam_state_get_u32(r);
    uam_state_get_bytes(r, v->vram, sizeof(v->vram));
}

static const uam_device_ops io_ops = {
    .type = "vga", .impl_min = 1, .impl_max = 1, .read = io_read, .write = io_write,
    .reset = vga_reset, .destroy = vga_destroy, .save = vga_save, .restore = vga_restore,
};
static const uam_device_ops mem_ops = {
    .type = "vga-memory", .read = mem_read, .write = mem_write, .destroy = mem_destroy,
    .flags = UAM_DEVICE_STATELESS,
};

uam_device *uam_vga_create(const char *name, uint16_t vendor, uint16_t device, uint8_t revision)
{
    vga *v = calloc(1, sizeof(*v));
    if (!v)
        return NULL;
    uam_device_init(&v->io, &io_ops, name);
    uam_device_init(&v->mem, &mem_ops, name);
    uam_pci_func_init(&v->fn, name, vendor, device, 0x03000000u | revision, NULL, v);
    vga_reset(&v->io);
    return &v->io;
}

uam_device   *uam_vga_io(uam_device *d)   { return d; }
uam_device   *uam_vga_mem(uam_device *d)  { return &from_io(d)->mem; }
uam_pci_func *uam_vga_func(uam_device *d) { return &from_io(d)->fn; }
