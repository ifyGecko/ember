/*
 * uam/dev/pci.h - PCI configuration space, BAR decoding, bridges, INTx and MSI.
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
 *   uam_pci_cam(bus)   configuration mechanism #1: 8 bytes, CONFIG_ADDRESS
 *                      then CONFIG_DATA (0xCF8 / 0xCFC on a PC). Only a dword
 *                      access to CONFIG_ADDRESS is a configuration access;
 *                      the bridge passes narrower ones to the device set with
 *                      uam_pci_cam_passthrough (a PC chipset's reset-control
 *                      register at 0xCF9, for one), else ignores the writes.
 *   uam_pci_ecam(bus)  enhanced configuration window: bus << 20 |
 *                      device << 15 | function << 12 | register.
 *
 * The bus device (uam_pci_bus_device) owns the configuration spaces: add it
 * to the machine for reset and snapshots. Function models keep any other
 * state themselves.
 *
 * Hierarchy: functions registered with uam_pci_register sit on the root bus
 * (bus 0). A PCI-to-PCI bridge is a function set up with
 * uam_pci_bridge_init (header type 1); functions registered behind it with
 * uam_pci_register_at answer configuration cycles for the bridge's
 * secondary bus once software programs the bus numbers, and their BARs
 * decode only inside the bridge's enabled I/O, memory and prefetchable
 * windows.
 *
 * Interrupts: a function raises INTx with uam_pci_set_intx. The pin is
 * swizzled at each bridge ((pin + device) mod 4) and the result goes to the
 * board's router as (root-bus device, pin 0-3 = INTA-INTD), wired-OR over
 * every function sharing it. MSI (uam_pci_msi_init) and MSI-X
 * (uam_pci_msix_init) messages go to the bus's MSI sink, the platform's
 * interrupt-message decoder, or are written to memory when there is none;
 * either one, when enabled, takes over from INTx.
 *
 * DMA: uam_pci_dma_read/write reach system memory only while the function
 * and every bridge above it have bus mastering enabled.
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
#define UAM_PCI_CMD_IO      0x0001u
#define UAM_PCI_CMD_MEM     0x0002u
#define UAM_PCI_CMD_MASTER  0x0004u
#define UAM_PCI_CMD_INTX_OFF 0x0400u
#define UAM_PCI_STATUS_INTX 0x0008u
#define UAM_PCI_STATUS_CAPS 0x0010u

/* Type-1 (bridge) header registers */
enum {
    UAM_PCI_PRIMARY_BUS = 0x18, UAM_PCI_SECONDARY_BUS = 0x19, UAM_PCI_SUBORDINATE_BUS = 0x1A,
    UAM_PCI_IO_BASE = 0x1C, UAM_PCI_IO_LIMIT = 0x1D, UAM_PCI_SEC_STATUS = 0x1E,
    UAM_PCI_MEM_BASE = 0x20, UAM_PCI_MEM_LIMIT = 0x22,
    UAM_PCI_PREF_BASE = 0x24, UAM_PCI_PREF_LIMIT = 0x26,
    UAM_PCI_PREF_BASE_HI = 0x28, UAM_PCI_PREF_LIMIT_HI = 0x2C,
    UAM_PCI_IO_BASE_HI = 0x30, UAM_PCI_IO_LIMIT_HI = 0x32,
    UAM_PCI_BRIDGE_CTL = 0x3E,
};

/* Capability IDs */
enum {
    UAM_PCI_CAP_PM = 0x01, UAM_PCI_CAP_MSI = 0x05, UAM_PCI_CAP_VENDOR = 0x09,
    UAM_PCI_CAP_SSVID = 0x0D, UAM_PCI_CAP_PCIE = 0x10, UAM_PCI_CAP_MSIX = 0x11,
    UAM_PCI_CAP_SATA = 0x12,
};

/* PCI Express device/port types (PCIe capability bits 4-7) */
enum {
    UAM_PCIE_ENDPOINT = 0x0, UAM_PCIE_LEGACY_ENDPOINT = 0x1, UAM_PCIE_ROOT_PORT = 0x4,
    UAM_PCIE_UPSTREAM = 0x5, UAM_PCIE_DOWNSTREAM = 0x6, UAM_PCIE_INTEGRATED = 0x9,
};

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

typedef struct uam_pci_msix uam_pci_msix;

struct uam_pci_func {
    uam_pci_bus *bus;
    uam_pci_func *parent;     /* the bridge above; NULL on the root bus */
    const char  *name;
    unsigned     devfn;
    const uam_pci_func_ops *ops;
    void        *ctx;
    uint8_t      cfg[UAM_PCI_CFG_SIZE];
    uint8_t      wmask[UAM_PCI_CFG_SIZE];   /* bits software can write */
    uint8_t      w1c[UAM_PCI_CFG_SIZE];     /* bits cleared by writing 1 */
    uint8_t      init[UAM_PCI_CFG_SIZE];    /* reset values (filled by uam_pci_register) */
    uam_pci_bar  bar[6];
    uam_pci_bar  rom;         /* expansion ROM (uam_pci_func_rom), header type 0 */
    bool         bridge;      /* header type 1 */
    unsigned     cap_free;    /* next free byte for uam_pci_add_cap */
    unsigned     msi_cap;     /* offsets of the capabilities, 0 = absent */
    unsigned     msix_cap;
    uint32_t     msi_pending;
    bool         intx;        /* the function's INTx request */
    bool         intx_out;    /* what it currently drives (after the gates) */
    uam_pci_msix *msix;
    uam_pci_func *next;
};

/* Root-bus interrupt router: `slot` is the device number on bus 0. */
typedef void (*uam_pci_intx_fn)(void *ctx, unsigned slot, unsigned pin, int level);
/* MSI sink: the message's address and data. */
typedef void (*uam_pci_msi_fn)(void *ctx, uint64_t addr, uint32_t data);

uam_pci_bus *uam_pci_bus_create(const char *name, uam_bus *sys, uint64_t io_base);
uam_device  *uam_pci_bus_device(uam_pci_bus *pb);   /* = uam_pci_cam */
uam_device  *uam_pci_cam(uam_pci_bus *pb);
uam_device  *uam_pci_ecam(uam_pci_bus *pb);
/* The device that sees accesses to CONFIG_ADDRESS other than a dword (offsets 0-3,
 * little-endian), or NULL. Reads go to it too: without one they return
 * CONFIG_ADDRESS's bytes. */
void         uam_pci_cam_passthrough(uam_pci_bus *pb, uam_device *dev);

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
/* An expansion ROM (configuration offset 0x30): `size` a power of two of at
 * least 2 KiB, served by `dev` (offsets relative to the ROM) while software
 * enables it (bit 0) and memory decoding. Firmware finds option ROMs there. */
void         uam_pci_func_rom(uam_pci_func *f, uint64_t size, uam_device *dev);
/* A read-only device holding a copy of `len` bytes (an option ROM image),
 * reading as 0xFF beyond them. Add it to the machine for its lifetime. */
uam_device  *uam_pci_rom_image_create(const char *name, const uint8_t *data, size_t len);
uam_status   uam_pci_register(uam_pci_bus *pb, uam_pci_func *f, unsigned dev, unsigned fn);
/* Register behind `bridge` (NULL: the root bus). */
uam_status   uam_pci_register_at(uam_pci_bus *pb, uam_pci_func *bridge, uam_pci_func *f,
                                 unsigned dev, unsigned fn);

/*
 * Set up a PCI-to-PCI bridge function: header type 1 with writable bus
 * numbers, I/O (32-bit), memory and 64-bit prefetchable windows and bridge
 * control; BARs 0-1 remain available.
 */
void         uam_pci_bridge_init(uam_pci_func *f, const char *name, uint16_t vendor,
                                 uint16_t device, uint32_t class_rev,
                                 const uam_pci_func_ops *ops, void *ctx);

/* Append a capability of `len` bytes to the list; returns its offset (0 when
 * the 256-byte space is full). Call before uam_pci_register. */
unsigned     uam_pci_add_cap(uam_pci_func *f, uint8_t id, unsigned len);
/* A PCI Express capability (version 2, x1 link at 2.5 GT/s, link up) of
 * the given port type; root and downstream ports get a slot with presence
 * detected. */
unsigned     uam_pci_add_pcie_cap(uam_pci_func *f, unsigned port_type, unsigned slot_nr);

/* MSI with 2^log2_vectors vectors (0-5), optionally 64-bit / per-vector masking. */
unsigned     uam_pci_msi_init(uam_pci_func *f, unsigned log2_vectors, bool addr64, bool maskable);
/* MSI-X with `n` vectors; the table and the pending-bit array live in BARs
 * (which may be the same), at 8-byte-aligned offsets. The model's BAR
 * handler passes accesses through uam_pci_msix_access first. */
unsigned     uam_pci_msix_init(uam_pci_func *f, unsigned n, unsigned table_bar, uint32_t table_off,
                               unsigned pba_bar, uint32_t pba_off);
bool         uam_pci_msix_access(uam_pci_func *f, unsigned bar, uint64_t off, unsigned size,
                                 bool write, uint64_t *val);
bool         uam_pci_msi_enabled(const uam_pci_func *f);    /* MSI or MSI-X */
/* Signal message `vector` (MSI: below the enabled count; MSI-X: table entry). */
void         uam_pci_msi_notify(uam_pci_func *f, unsigned vector);

/* Drive the function's INTx request (the pin in INT_PIN, 1-4). */
void         uam_pci_set_intx(uam_pci_func *f, int level);
void         uam_pci_set_intx_router(uam_pci_bus *pb, uam_pci_intx_fn fn, void *ctx);
void         uam_pci_set_msi_sink(uam_pci_bus *pb, uam_pci_msi_fn fn, void *ctx);

/* Bus-master DMA to system memory; UAM_ERR_FAULT when mastering is off. */
uam_status   uam_pci_dma_read(uam_pci_func *f, uint64_t addr, void *buf, size_t len);
uam_status   uam_pci_dma_write(uam_pci_func *f, uint64_t addr, const void *buf, size_t len);
/* Re-evaluate BAR decoding (after a model changes cfg[] itself). */
void         uam_pci_update_bars(uam_pci_func *f);

/* Configuration access for chipset models. */
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
