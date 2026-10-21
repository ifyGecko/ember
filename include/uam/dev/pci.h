/*
 * uam/dev/pci.h - PCI configuration space and BAR decoding (bus 0).
 *
 * A PCI device model embeds a uam_pci_func: its 4 KiB configuration space
 * with per-byte write masks, and up to six BARs, each naming the uam_device
 * that serves it. uam_pci_register puts the function on the bus at
 * device:function; from then on the bus maps each BAR on the system bus at
 * the address software programs, while the command register enables I/O or
 * memory decoding, and unmaps it otherwise. I/O BARs decode at
 * `io_base + port`.
 *
 * Configuration access:
 *   uam_pci_cam(bus)   8 bytes at the PC's 0xCF8 (address) / 0xCFC (data).
 *                      A byte write to 0xCF9 is not a configuration access:
 *                      it goes to the hook set with uam_pci_set_cf9_hook
 *                      (the PC reset-control register).
 *   uam_pci_ecam(bus)  enhanced configuration window: bus << 20 |
 *                      device << 15 | function << 12 | register.
 *
 * The bus device (uam_pci_bus_device) owns the configuration spaces: add it
 * to the machine for reset and snapshots. Function models keep any other
 * state themselves.
 */
#ifndef UAM_DEV_PCI_H
#define UAM_DEV_PCI_H

#include "uam/bus.h"
#include "uam/device.h"

#define UAM_PCI_CFG_SIZE 4096u
#define UAM_PCI_ECAM_SIZE(nbus) ((uint64_t)(nbus) << 20)

/* BAR flags */
enum {
    UAM_PCI_BAR_MEM = 0, UAM_PCI_BAR_IO = 1u << 0, UAM_PCI_BAR_64 = 1u << 1,
    UAM_PCI_BAR_PREFETCH = 1u << 2,
};

/* Standard configuration registers */
enum {
    UAM_PCI_VENDOR = 0x00, UAM_PCI_DEVICE = 0x02, UAM_PCI_COMMAND = 0x04, UAM_PCI_STATUS = 0x06,
    UAM_PCI_REVISION = 0x08, UAM_PCI_CLASS = 0x09, UAM_PCI_HEADER = 0x0E, UAM_PCI_BAR0 = 0x10,
    UAM_PCI_SUBSYS_VENDOR = 0x2C, UAM_PCI_SUBSYS = 0x2E, UAM_PCI_CAP_PTR = 0x34,
    UAM_PCI_INT_LINE = 0x3C, UAM_PCI_INT_PIN = 0x3D,
};
#define UAM_PCI_CMD_IO   0x0001u
#define UAM_PCI_CMD_MEM  0x0002u

typedef struct uam_pci_bus uam_pci_bus;
typedef struct uam_pci_func uam_pci_func;

typedef struct uam_pci_func_ops {
    /* Optional: refresh cfg[] before software reads [off, off+size). */
    void (*config_read)(uam_pci_func *f, unsigned off, unsigned size);
    /* Optional: after software wrote [off, off+size) (masks already applied). */
    void (*config_write)(uam_pci_func *f, unsigned off, unsigned size, uint32_t val);
    /* Optional: after the configuration space returned to its reset values. */
    void (*reset)(uam_pci_func *f);
} uam_pci_func_ops;

typedef struct uam_pci_bar {
    uint64_t    size;      /* power of two; 0 = unused */
    unsigned    flags;
    uam_device *dev;       /* serves accesses (offsets relative to the BAR) */
    uam_region *region;    /* mapping on the system bus, once decoded */
    char        rname[48]; /* the mapping's name: "<function>.bar<n>" */
} uam_pci_bar;

struct uam_pci_func {
    uam_pci_bus *bus;
    const char  *name;
    unsigned     devfn;
    const uam_pci_func_ops *ops;
    void        *ctx;
    uint8_t      cfg[UAM_PCI_CFG_SIZE];
    uint8_t      wmask[UAM_PCI_CFG_SIZE];   /* bits software can write */
    uint8_t      w1c[UAM_PCI_CFG_SIZE];     /* bits cleared by writing 1 */
    uint8_t      init[UAM_PCI_CFG_SIZE];    /* reset values (filled by uam_pci_register) */
    uam_pci_bar  bar[6];
    uam_pci_func *next;
};

uam_pci_bus *uam_pci_bus_create(const char *name, uam_bus *sys, uint64_t io_base);
uam_device  *uam_pci_bus_device(uam_pci_bus *pb);   /* = uam_pci_cam */
uam_device  *uam_pci_cam(uam_pci_bus *pb);
uam_device  *uam_pci_ecam(uam_pci_bus *pb);
void         uam_pci_set_cf9_hook(uam_pci_bus *pb, void (*fn)(void *ctx, uint8_t val), void *ctx);

/*
 * Set up a function: zeroed configuration space with the IDs, class and
 * header type 0, command/latency/interrupt-line writable, status bits 11-15
 * write-1-to-clear. Then describe BARs with uam_pci_func_bar and adjust cfg[]
 * and wmask[] as the model needs before uam_pci_register.
 */
void         uam_pci_func_init(uam_pci_func *f, const char *name, uint16_t vendor,
                               uint16_t device, uint32_t class_rev,
                               const uam_pci_func_ops *ops, void *ctx);
void         uam_pci_func_bar(uam_pci_func *f, unsigned idx, uint64_t size, unsigned flags,
                              uam_device *dev);
uam_status   uam_pci_register(uam_pci_bus *pb, uam_pci_func *f, unsigned dev, unsigned fn);
/* Re-evaluate BAR decoding (after a model changes cfg[] itself). */
void         uam_pci_update_bars(uam_pci_func *f);

/* Configuration access for chipset models (bus 0 only). */
uint32_t     uam_pci_cfg_read(uam_pci_bus *pb, unsigned bus, unsigned devfn, unsigned off,
                              unsigned size);
void         uam_pci_cfg_write(uam_pci_bus *pb, unsigned bus, unsigned devfn, unsigned off,
                               unsigned size, uint32_t val);

/* Little-endian accessors for cfg[] */
static inline uint32_t uam_pci_get(const uint8_t *cfg, unsigned off, unsigned size)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++)
        v |= (uint32_t)cfg[off + i] << (8 * i);
    return v;
}
static inline void uam_pci_set(uint8_t *cfg, unsigned off, unsigned size, uint32_t v)
{
    for (unsigned i = 0; i < size; i++)
        cfg[off + i] = (uint8_t)(v >> (8 * i));
}

#endif /* UAM_DEV_PCI_H */
