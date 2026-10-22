/*
 * uam/blockdev.h - Block device interface: fixed-size sectors between a guest
 * storage controller (the frontend: AHCI, NVMe) and the host (the backend: a
 * disk image file).
 *
 * Devices depend only on this header and call the ops through the inline
 * wrappers. Transfers are whole sectors of `sector_size` bytes. A backend
 * opened read-only refuses writes; one opened volatile keeps every write in
 * host memory, so the image file never changes (runs stay repeatable).
 *
 * Backends are created in src/host (uam_blockdev_open_file). The contents of
 * a backend are host state: machine snapshots do not include them.
 *
 * Dependencies: common.h.
 */
#ifndef UAM_BLOCKDEV_H
#define UAM_BLOCKDEV_H

#include "uam/common.h"

typedef struct uam_blockdev uam_blockdev;

typedef struct uam_blockdev_ops {
    const char *type;
    uam_status (*read)(uam_blockdev *bd, uint64_t lba, void *buf, uint32_t count);
    uam_status (*write)(uam_blockdev *bd, uint64_t lba, const void *buf, uint32_t count);
    uam_status (*flush)(uam_blockdev *bd);
    void       (*destroy)(uam_blockdev *bd);
} uam_blockdev_ops;

struct uam_blockdev {
    const uam_blockdev_ops *ops;
    uint64_t sectors;
    uint32_t sector_size;
    bool     readonly;
};

enum {
    UAM_BLK_READONLY = 1u << 0,
    UAM_BLK_VOLATILE = 1u << 1,   /* writes go to a host-memory overlay */
};

/* A disk image file; its size is rounded down to whole sectors. */
uam_blockdev *uam_blockdev_open_file(const char *path, uint32_t sector_size, unsigned flags);

static inline uam_status uam_blockdev_read(uam_blockdev *bd, uint64_t lba, void *buf, uint32_t n)
{
    if (lba > bd->sectors || n > bd->sectors - lba)
        return UAM_ERR_INVALID;
    return bd->ops->read(bd, lba, buf, n);
}
static inline uam_status uam_blockdev_write(uam_blockdev *bd, uint64_t lba, const void *buf,
                                            uint32_t n)
{
    if (bd->readonly)
        return UAM_ERR_ILLEGAL;
    if (lba > bd->sectors || n > bd->sectors - lba)
        return UAM_ERR_INVALID;
    return bd->ops->write(bd, lba, buf, n);
}
static inline uam_status uam_blockdev_flush(uam_blockdev *bd)
{
    return bd->ops->flush ? bd->ops->flush(bd) : UAM_OK;
}
static inline void uam_blockdev_close(uam_blockdev *bd)
{
    if (bd)
        bd->ops->destroy(bd);
}

#endif /* UAM_BLOCKDEV_H */
