/*
 * ata_core.h - The ATA/ATAPI device side shared by the storage controllers
 * (ahci.c, ide.c): IDENTIFY data, the ATA command set and the ATAPI (SCSI
 * multimedia) commands, independent of how a controller moves the data.
 *
 * Internal to devices/; not a public header.
 */
#ifndef UAM_DEV_ATA_CORE_H
#define UAM_DEV_ATA_CORE_H

#include <stdbool.h>
#include <stdint.h>

#include "uam/blockdev.h"

#define ATA_ST_ERR   0x01
#define ATA_ST_DRQ   0x08
#define ATA_ST_DSC   0x10
#define ATA_ST_DF    0x20
#define ATA_ST_DRDY  0x40
#define ATA_ST_BSY   0x80
#define ATA_ERR_ABRT 0x04

#define ATA_SIG_ATA   0x00000101u   /* LBA high/mid/low, sector count after reset */
#define ATA_SIG_ATAPI 0xEB140101u

#define ATA_XFER_MAX (1u << 22)     /* bounce buffer: larger requests go in pieces */

/* A drive: a disk (512-byte sectors) or a disc (ATAPI, 2048-byte sectors). */
typedef struct ata_drive {
    uam_blockdev *bd;
    bool          atapi;
    uint8_t       sense_key, asc, ascq;
    bool          unit_attention;   /* media present since reset: reported once */
    char          serial[21];
    const char   *model;            /* IDENTIFY model string */
    const char   *product;          /* INQUIRY product, 16 characters */
} ata_drive;

/* How a controller moves a command's data. copy() moves `len` bytes between
 * `buf` and the host (to_host: device to host), continuing where the
 * previous call stopped; it returns the bytes moved (less when the host's
 * buffers run out). `bounce` is ATA_XFER_MAX bytes of scratch. */
typedef struct ata_io {
    void     *ctx;
    uint32_t (*copy)(void *ctx, uint8_t *buf, uint32_t len, bool to_host);
    uint8_t  *bounce;
    bool      pio;                  /* out: the command moved data by PIO */
} ata_io;

/* The command registers (48-bit view; a 28-bit command uses the low bits and
 * device[3:0]). */
typedef struct ata_taskfile {
    uint8_t  command, device;
    uint16_t features, count;
    uint64_t lba;
} ata_taskfile;

typedef struct ata_result {
    uint8_t  status, error;
    uint64_t lba;
    uint16_t count;
} ata_result;

void ata_drive_reset(ata_drive *d);              /* sense data cleared, unit attention (ATAPI) */
uint32_t ata_signature(const ata_drive *d);
void ata_identify(const ata_drive *d, uint16_t w[256]);
/* Execute an ATA command / an ATAPI packet; data moves through `io`. */
void ata_exec(ata_drive *d, const ata_taskfile *tf, ata_io *io, ata_result *r);
void atapi_exec(ata_drive *d, const uint8_t cdb[12], ata_io *io, ata_result *r);

#endif /* UAM_DEV_ATA_CORE_H */
