/*
 * blockdev_file.c - The disk-image-file block backend (uam/blockdev.h).
 *
 * The volatile mode keeps written data in 64 KiB host chunks, allocated on
 * first write; reads take each sector from its chunk when one exists and
 * from the file otherwise.
 */
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "uam/blockdev.h"

#define CHUNK 65536u

typedef struct {
    uam_blockdev bd;
    int          fd;
    bool         overlay;
    uint8_t    **chunks;     /* volatile: one pointer per CHUNK of the disk */
    uint8_t     *present;    /* volatile: per-sector "written" bits, per chunk */
    uint64_t     nchunks;
    unsigned     spc;        /* sectors per chunk */
} file_bd;

static file_bd *from_bd(uam_blockdev *bd) { return UAM_CONTAINER_OF(bd, file_bd, bd); }

static uam_status file_read_raw(file_bd *f, uint64_t lba, uint8_t *buf, uint32_t n)
{
    uint64_t off = lba * f->bd.sector_size, len = (uint64_t)n * f->bd.sector_size;
    while (len) {
        ssize_t r = pread(f->fd, buf, len, (off_t)off);
        if (r <= 0)
            return UAM_ERR_FAULT;
        buf += r; off += (uint64_t)r; len -= (uint64_t)r;
    }
    return UAM_OK;
}

static bool sector_present(file_bd *f, uint64_t lba)
{
    uint64_t c = lba / f->spc;
    unsigned s = (unsigned)(lba % f->spc);
    return f->chunks[c] && (f->present[c * (f->spc / 8) + s / 8] >> (s % 8) & 1);
}

static uam_status fbd_read(uam_blockdev *bd, uint64_t lba, void *buf, uint32_t n)
{
    file_bd *f = from_bd(bd);
    if (!f->overlay)
        return file_read_raw(f, lba, buf, n);
    uint8_t *p = buf;
    const uint32_t ss = bd->sector_size;
    for (uint32_t i = 0; i < n; i++, p += ss) {
        uint64_t s = lba + i;
        if (sector_present(f, s)) {
            memcpy(p, f->chunks[s / f->spc] + (s % f->spc) * ss, ss);
        } else {
            uam_status st = file_read_raw(f, s, p, 1);
            if (st != UAM_OK)
                return st;
        }
    }
    return UAM_OK;
}

static uam_status fbd_write(uam_blockdev *bd, uint64_t lba, const void *buf, uint32_t n)
{
    file_bd *f = from_bd(bd);
    const uint32_t ss = bd->sector_size;
    if (!f->overlay) {
        const uint8_t *p = buf;
        uint64_t off = lba * ss, len = (uint64_t)n * ss;
        while (len) {
            ssize_t w = pwrite(f->fd, p, len, (off_t)off);
            if (w <= 0)
                return UAM_ERR_FAULT;
            p += w; off += (uint64_t)w; len -= (uint64_t)w;
        }
        return UAM_OK;
    }
    const uint8_t *p = buf;
    for (uint32_t i = 0; i < n; i++, p += ss) {
        uint64_t s = lba + i, c = s / f->spc;
        unsigned k = (unsigned)(s % f->spc);
        if (!f->chunks[c] && !(f->chunks[c] = malloc(CHUNK)))
            return UAM_ERR_NOMEM;
        memcpy(f->chunks[c] + (uint64_t)k * ss, p, ss);
        f->present[c * (f->spc / 8) + k / 8] |= (uint8_t)(1u << (k % 8));
    }
    return UAM_OK;
}

static uam_status fbd_flush(uam_blockdev *bd)
{
    file_bd *f = from_bd(bd);
    if (!f->overlay && !bd->readonly)
        (void)fdatasync(f->fd);
    return UAM_OK;
}

static void fbd_destroy(uam_blockdev *bd)
{
    file_bd *f = from_bd(bd);
    if (f->chunks)
        for (uint64_t i = 0; i < f->nchunks; i++)
            free(f->chunks[i]);
    free(f->chunks);
    free(f->present);
    close(f->fd);
    free(f);
}

static const uam_blockdev_ops file_ops = {
    .type = "file", .read = fbd_read, .write = fbd_write, .flush = fbd_flush,
    .destroy = fbd_destroy,
};

uam_blockdev *uam_blockdev_open_file(const char *path, uint32_t sector_size, unsigned flags)
{
    if (!sector_size || (sector_size & (sector_size - 1)) || sector_size > CHUNK / 8)
        return NULL;
    bool ro = flags & UAM_BLK_READONLY, overlay = flags & UAM_BLK_VOLATILE;
    int fd = open(path, (ro || overlay) ? O_RDONLY : O_RDWR);
    if (fd < 0)
        return NULL;
    struct stat sb;
    file_bd *f = calloc(1, sizeof(*f));
    if (!f || fstat(fd, &sb) != 0) {
        free(f);
        close(fd);
        return NULL;
    }
    f->bd.ops = &file_ops;
    f->bd.sector_size = sector_size;
    f->bd.sectors = (uint64_t)sb.st_size / sector_size;
    f->bd.readonly = ro;
    f->fd = fd;
    f->overlay = overlay && !ro;
    if (f->overlay) {
        f->spc = CHUNK / sector_size;
        f->nchunks = (f->bd.sectors + f->spc - 1) / f->spc;
        f->chunks = calloc(f->nchunks ? f->nchunks : 1, sizeof(*f->chunks));
        f->present = calloc(f->nchunks ? f->nchunks : 1, f->spc / 8 ? f->spc / 8 : 1);
        if (!f->chunks || !f->present) {
            fbd_destroy(&f->bd);
            return NULL;
        }
    }
    return &f->bd;
}
