/*
 * i801_smbus.c - Intel SMBus host controller with SPD EEPROMs (i801_smbus.h).
 *
 * Semantics as the firmware's SMBus library uses them (edk2 SmbusLib/PchSmbusLib,
 * coreboot southbridge/intel/common/smbus.c): clear HST_STS, write XMIT_SLVA, HST_CMD (and
 * HST_D0 for a write), then HST_CNT = START | command; poll HST_STS until INTR (done) or an
 * error bit; read HST_D0/HST_D1. An address with nothing on it answers DEV_ERR, which is
 * how firmware finds empty DIMM slots. The block, process-call and I2C-read commands are
 * not modelled (FAILED).
 * Local emulation model; not part of ember.
 */
#include <stdlib.h>
#include <string.h>

#include "i801_smbus.h"

enum { HST_STS = 0, HST_CNT = 2, HST_CMD = 3, XMIT_SLVA = 4, HST_D0 = 5, HST_D1 = 6, BLOCK_DB = 7 };
#define ST_BUSY      0x01u
#define ST_INTR      0x02u
#define ST_DEV_ERR   0x04u
#define ST_FAILED    0x10u
#define ST_INUSE     0x40u
#define ST_W1C       0xBEu      /* INTR..BYTE_DONE; INUSE is cleared by a 1 as well */
#define CNT_INTREN   0x01u
#define CNT_START    0x40u
enum { CMD_QUICK = 0, CMD_BYTE = 1, CMD_BYTE_DATA = 2, CMD_WORD_DATA = 3 };
#define HOSTC        0x40       /* PCI config: HST_EN bit 0 */
#define SPD_FIRST    0x50u
#define SPD_N        8u
#define SPD_MAX      512u

typedef struct i801 {
    uam_device   dev;           /* owns everything */
    uam_device   io;            /* BAR4 */
    uam_pci_func fn;
    /* configuration: the EEPROMs (read-only to the guest) */
    uint8_t     *spd[SPD_N];
    size_t       spd_len[SPD_N];
    /* guest-visible state (saved) */
    uint8_t      regs[I801_SMBUS_IO_SIZE];
    uint8_t      page;          /* DDR4 SPD page (0/1), shared by every EEPROM */
    uint8_t      ptr[SPD_N];    /* each EEPROM's current address (byte read, no command) */
} i801;

static i801 *from_dev(uam_device *d) { return UAM_CONTAINER_OF(d, i801, dev); }
static i801 *from_io(uam_device *d)  { return UAM_CONTAINER_OF(d, i801, io); }

static void update_intx(i801 *s)
{
    uam_pci_set_intx(&s->fn, (s->regs[HST_CNT] & CNT_INTREN) && (s->regs[HST_STS] & ST_INTR));
}

/* Run the command in HST_CNT; the transaction completes at once. */
static void transact(i801 *s)
{
    uint8_t cnt = s->regs[HST_CNT], slva = s->regs[XMIT_SLVA];
    unsigned cmd = (cnt >> 2) & 7, addr = slva >> 1;
    bool rd = slva & 1;
    uint8_t st = 0;
    if (!(s->fn.cfg[HOSTC] & 1)) {
        st = ST_FAILED;
    } else if ((addr == 0x36 || addr == 0x37) && !rd) {     /* DDR4 SPD page select */
        s->page = (uint8_t)(addr - 0x36);
        st = ST_INTR;
    } else if (addr < SPD_FIRST || addr >= SPD_FIRST + SPD_N || !s->spd[addr - SPD_FIRST]) {
        st = ST_DEV_ERR;                                     /* nobody acknowledged */
    } else {
        unsigned n = addr - SPD_FIRST;
        const uint8_t *mem = s->spd[n];
        size_t len = s->spd_len[n];
        unsigned base = s->page * 256u;
        st = ST_INTR;
        switch (cmd) {
        case CMD_QUICK:
            break;
        case CMD_BYTE:                                       /* read at the current address */
            if (rd) {
                unsigned a = base + s->ptr[n];
                s->regs[HST_D0] = a < len ? mem[a] : 0xFF;
                s->ptr[n]++;
            } else {
                s->ptr[n] = s->regs[HST_CMD];
            }
            break;
        case CMD_BYTE_DATA:
        case CMD_WORD_DATA: {
            unsigned a = base + s->regs[HST_CMD];
            if (rd) {
                s->regs[HST_D0] = a < len ? mem[a] : 0xFF;
                if (cmd == CMD_WORD_DATA)
                    s->regs[HST_D1] = a + 1 < len ? mem[a + 1] : 0xFF;
                s->ptr[n] = (uint8_t)(s->regs[HST_CMD] + (cmd == CMD_WORD_DATA ? 2 : 1));
            }                                                /* SPD writes: ignored */
            break;
        }
        default:
            st = ST_FAILED;
            uam_log(UAM_LOG_WARN, "i801-smbus", "%s: SMBus command %u not modelled", s->dev.name, cmd);
        }
    }
    s->regs[HST_CNT] &= (uint8_t)~CNT_START;
    s->regs[HST_STS] = (uint8_t)((s->regs[HST_STS] & ST_INUSE) | st);
    update_intx(s);
}

static uam_status io_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    (void)size;
    i801 *s = from_io(d);
    *val = s->regs[off];
    if (off == HST_STS)
        s->regs[HST_STS] |= ST_INUSE;           /* the INUSE semaphore: reading takes it */
    return UAM_OK;
}

static uam_status io_write(uam_device *d, uint64_t off, unsigned size, uint64_t val)
{
    (void)size;
    i801 *s = from_io(d);
    uint8_t v = (uint8_t)val;
    if (off == HST_STS) {
        s->regs[HST_STS] &= (uint8_t)~(v & (ST_W1C | ST_INUSE));
        update_intx(s);
        return UAM_OK;
    }
    s->regs[off] = v;
    if (off == HST_CNT && (v & CNT_START))
        transact(s);
    return UAM_OK;
}

static void smb_reset(uam_device *d)
{
    i801 *s = from_dev(d);
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->ptr, 0, sizeof(s->ptr));
    s->page = 0;
}

static void smb_destroy(uam_device *d)
{
    i801 *s = from_dev(d);
    for (unsigned i = 0; i < SPD_N; i++)
        free(s->spd[i]);
    free(s);
}

static void smb_save(uam_device *d, uam_state_writer *w)
{
    i801 *s = from_dev(d);
    uam_state_put_bytes(w, s->regs, sizeof(s->regs));
    uam_state_put_u8(w, s->page);
    uam_state_put_bytes(w, s->ptr, sizeof(s->ptr));
}

static void smb_restore(uam_device *d, uam_state_reader *r)
{
    i801 *s = from_dev(d);
    uam_state_get_bytes(r, s->regs, sizeof(s->regs));
    s->page = uam_state_get_u8(r) & 1;
    uam_state_get_bytes(r, s->ptr, sizeof(s->ptr));
}

static const uam_device_ops smb_ops = {
    .type = "intel,i801-smbus", .reset = smb_reset, .destroy = smb_destroy,
    .save = smb_save, .restore = smb_restore,
};
static const uam_device_ops io_ops = {
    .type = "intel,i801-smbus-io", .impl_min = 1, .impl_max = 1,
    .read = io_read, .write = io_write, .flags = UAM_DEVICE_STATELESS,   /* saved by its owner */
};

uam_device *i801_smbus_create(const char *name, uint16_t vendor, uint16_t device)
{
    i801 *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    uam_device_init(&s->dev, &smb_ops, name);
    uam_device_init(&s->io, &io_ops, name);
    uam_pci_func *f = &s->fn;
    uam_pci_func_init(f, name, vendor, device, 0x0C050000, NULL, s);
    uam_pci_func_bar(f, 4, I801_SMBUS_IO_SIZE, UAM_PCI_BAR_IO, &s->io);
    f->cfg[UAM_PCI_INT_PIN] = 2;               /* INTB */
    f->wmask[HOSTC] = 0x1F;
    return &s->dev;
}

uam_pci_func *i801_smbus_func(uam_device *d) { return &from_dev(d)->fn; }

uam_status i801_smbus_add_spd(uam_device *d, uint8_t addr, const uint8_t *data, size_t len)
{
    i801 *s = from_dev(d);
    if (addr < SPD_FIRST || addr >= SPD_FIRST + SPD_N || !len || len > SPD_MAX)
        return UAM_ERR_INVALID;
    unsigned n = addr - SPD_FIRST;
    uint8_t *copy = malloc(len);
    if (!copy)
        return UAM_ERR_NOMEM;
    memcpy(copy, data, len);
    free(s->spd[n]);
    s->spd[n] = copy;
    s->spd_len[n] = len;
    return UAM_OK;
}
