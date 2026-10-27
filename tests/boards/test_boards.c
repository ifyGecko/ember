/*
 * test_boards.c - Boards for ember's own tests, built on the reference boards.
 *
 * The reference boards hold only what every board of their architecture has.
 * The tests also exercise generic components no reference board wires, so these
 * boards run a reference board's build and add them on top, the way an
 * out-of-tree board can start from a reference board. They are not shipped:
 * only build/uam-test-run (uam-test-run.c) registers them.
 *
 * "x86_64-test": x86_64-ref plus
 *   0xB2 software SMI port (any write raises SMI# on every processor), for the
 *        SMM tests' SMIs raised by I/O instructions
 *   -o disk=PATH     ATA disk on AHCI port 0 (PCI 00:02.0), -o cdrom=PATH an
 *                    ATAPI disc on port 1 (-o <key>-mode=rw|ro|volatile)
 *   -o nvme=PATH     NVMe namespace 1 (PCI 00:03.0)
 *   -o ide0=PATH, -o ide1=PATH  ATA disks on the IDE primary channel,
 *                    -o idecd=PATH an ATAPI disc on the secondary master; the
 *                    controller (PCI 00:04.0, compatibility mode) decodes
 *                    0x1F0/0x3F6 (IRQ 14) and 0x170/0x376 (IRQ 15)
 *   -o floppy=PATH   82077 floppy controller (0x3F0-0x3F5/0x3F7, IRQ 6,
 *                    DMA 2), drive A: (-o floppyb=PATH: drive B:)
 *   -o vga=1         VGA at 0x3B0-0x3DF and 0xA0000-0xBFFFF (PCI 00:05.0);
 *                    --image vgabios=PATH is its BIOS, at 0xC0000 and behind
 *                    the function's expansion ROM BAR; -o vgadump=1 prints the
 *                    text screen when the machine is torn down
 *   -o lpt=1         parallel port at 0x378 (IRQ 7) printing to the console
 *
 * "rv32-test": rv32-ref plus virtio-mmio network slots: port "netN" (N < 8,
 * --nic netN=SPEC) puts a virtio-net device at 0x10001000 + 0x1000 * N, IRQ to
 * PLIC source 1 + N; -o netN.mac=XX:XX:XX:XX:XX:XX (default 02:75:61:6d:01:0N).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uam/boards.h"
#include "uam/dev/ahci.h"
#include "uam/dev/apic.h"
#include "uam/dev/fdc.h"
#include "uam/dev/i8259.h"
#include "uam/dev/ide.h"
#include "uam/dev/lpt.h"
#include "uam/dev/nvme.h"
#include "uam/dev/pc_misc.h"
#include "uam/dev/pci.h"
#include "uam/dev/plic.h"
#include "uam/dev/vga.h"
#include "uam/dev/virtio_net.h"
#include "uam/isa/x86.h"
#include "uam/loader.h"
#include "uam/machine.h"

#include "test_boards.h"

/* PCI functions carry a vendor ID no vendor is assigned: drivers bind them by
 * class code. */
#define TEST_PCI_VENDOR 0xFFFEu

static uam_status add(uam_machine *m, uam_device *d)
{
    if (!d)
        return UAM_ERR_NOMEM;
    uam_machine_add_device(m, d);
    return UAM_OK;
}

/* ---- x86_64-test ---------------------------------------------------------- */

typedef struct x86_test {
    uam_machine  *m;
    uam_device   *pic, *ioapic;
    void         *ref_data;                    /* x86_64-ref's board state */
    void        (*ref_fini)(uam_machine *m);
    uam_blockdev *disk, *cdrom, *nvme, *ide[3], *floppy[2];
    uam_device   *vga;
    bool          vgadump;
} x86_test;

static uam_status map_io(uam_machine *m, const char *name, uint64_t port, uint64_t size,
                         uam_device *d)
{
    return uam_bus_map_mmio(&m->bus, name, UAM_X86_PIO_BASE + port, size, d);
}

static bool opt_on(const uam_board_config *cfg, const char *key)
{
    const char *v = uam_board_config_option(cfg, key);
    return v && strcmp(v, "0") != 0;
}

/* An ISA IRQ other than 0: to the 8259 and to the I/O APIC pin of the same number. */
static void isa_sink(void *ctx, unsigned irq, int level)
{
    x86_test *t = ctx;
    uam_i8259_input(t->pic, irq, level);
    uam_ioapic_input(t->ioapic, irq, level);
}

static void smi_all(void *ctx, unsigned input, int level)
{
    (void)input;
    uam_machine *m = ctx;
    for (unsigned i = 0; i < uam_machine_ncpus(m); i++)
        uam_cpu_irq_sink(uam_machine_cpu(m, i), UAM_X86_IRQ_SMI, level);
}

static void x86_test_fini(uam_machine *m)
{
    x86_test *t = m->board_data;
    if (!t)
        return;
    if (t->vga && t->vgadump)
        uam_vga_text_dump(t->vga, stderr);
    uam_blockdev_close(t->disk);
    uam_blockdev_close(t->cdrom);
    uam_blockdev_close(t->nvme);
    for (unsigned i = 0; i < 3; i++)
        uam_blockdev_close(t->ide[i]);
    for (unsigned i = 0; i < 2; i++)
        uam_blockdev_close(t->floppy[i]);
    m->board_data = t->ref_data;
    m->board_fini = t->ref_fini;
    free(t);
    if (m->board_fini)
        m->board_fini(m);
}

static uam_status add_storage(x86_test *t, const uam_board_config *cfg, uam_pci_bus *pci,
                              uam_device *dma)
{
    uam_machine *m = t->m;
    uam_status st;
    if ((st = uam_board_open_disk(cfg, "disk", 512, &t->disk)) != UAM_OK ||
        (st = uam_board_open_disk(cfg, "cdrom", 2048, &t->cdrom)) != UAM_OK)
        return st;
    if (t->disk || t->cdrom) {
        uam_device *ahci = uam_ahci_create("ahci", 2, TEST_PCI_VENDOR, 0x0001, 0);
        if ((st = add(m, ahci)) != UAM_OK ||
            (st = uam_pci_register(pci, uam_ahci_func(ahci), 2, 0)) != UAM_OK)
            return st;
        if ((t->disk && (st = uam_ahci_attach(ahci, 0, t->disk, false)) != UAM_OK) ||
            (t->cdrom && (st = uam_ahci_attach(ahci, 1, t->cdrom, true)) != UAM_OK))
            return st;
    }
    if ((st = uam_board_open_disk(cfg, "nvme", 512, &t->nvme)) != UAM_OK)
        return st;
    if (t->nvme) {
        uam_device *nvme = uam_nvme_create("nvme", TEST_PCI_VENDOR, 0x0002, 0);
        if ((st = add(m, nvme)) != UAM_OK ||
            (st = uam_pci_register(pci, uam_nvme_func(nvme), 3, 0)) != UAM_OK ||
            (st = uam_nvme_attach(nvme, 1, t->nvme)) != UAM_OK)
            return st;
    }
    static const char *const ide_keys[3] = { "ide0", "ide1", "idecd" };
    for (unsigned i = 0; i < 3; i++)
        if ((st = uam_board_open_disk(cfg, ide_keys[i], i == 2 ? 2048 : 512, &t->ide[i])) != UAM_OK)
            return st;
    if (t->ide[0] || t->ide[1] || t->ide[2]) {
        uam_device *ide = uam_ide_create("ide", TEST_PCI_VENDOR, 0x0003, 0);
        if ((st = add(m, ide)) != UAM_OK ||
            (st = uam_pci_register(pci, uam_ide_func(ide), 4, 0)) != UAM_OK ||
            (st = map_io(m, "ide0", 0x1F0, 8, uam_ide_cmd(ide, 0))) != UAM_OK ||
            (st = map_io(m, "ide0-ctl", 0x3F6, 1, uam_ide_ctl(ide, 0))) != UAM_OK ||
            (st = map_io(m, "ide1", 0x170, 8, uam_ide_cmd(ide, 1))) != UAM_OK ||
            (st = map_io(m, "ide1-ctl", 0x376, 1, uam_ide_ctl(ide, 1))) != UAM_OK)
            return st;
        if ((t->ide[0] && (st = uam_ide_attach(ide, 0, 0, t->ide[0], false)) != UAM_OK) ||
            (t->ide[1] && (st = uam_ide_attach(ide, 0, 1, t->ide[1], false)) != UAM_OK) ||
            (t->ide[2] && (st = uam_ide_attach(ide, 1, 0, t->ide[2], true)) != UAM_OK))
            return st;
        uam_signal_connect(uam_ide_irq_line(ide, 0), isa_sink, t, 14);
        uam_signal_connect(uam_ide_irq_line(ide, 1), isa_sink, t, 15);
    }
    static const char *const fd_keys[2] = { "floppy", "floppyb" };
    for (unsigned i = 0; i < 2; i++)
        if ((st = uam_board_open_disk(cfg, fd_keys[i], 512, &t->floppy[i])) != UAM_OK)
            return st;
    if (t->floppy[0] || t->floppy[1]) {
        uam_device *fdc = uam_fdc_create("fdc", dma, 2);
        if ((st = add(m, fdc)) != UAM_OK ||
            (st = map_io(m, "fdc", 0x3F0, 6, uam_fdc_regs(fdc))) != UAM_OK ||
            (st = map_io(m, "fdc-dir", 0x3F7, 1, uam_fdc_dir(fdc))) != UAM_OK)
            return st;
        for (unsigned i = 0; i < 2; i++)
            if (t->floppy[i] && (st = uam_fdc_insert(fdc, i, t->floppy[i], false)) != UAM_OK) {
                uam_log(UAM_LOG_ERROR, "x86_64-test", "%s: not a floppy image size", fd_keys[i]);
                return st;
            }
        uam_signal_connect(uam_fdc_irq_line(fdc), isa_sink, t, 6);
    }
    return UAM_OK;
}

static uam_status add_display(x86_test *t, const uam_board_config *cfg, uam_pci_bus *pci)
{
    uam_machine *m = t->m;
    uam_status st;
    const char *vgabios = uam_board_config_image(cfg, "vgabios");
    if (opt_on(cfg, "vga") || vgabios) {
        uam_device *vga = uam_vga_create("vga", TEST_PCI_VENDOR, 0x0004, 0);
        if ((st = add(m, vga)) != UAM_OK ||
            (st = map_io(m, "vga", 0x3B0, UAM_VGA_IO_SIZE, uam_vga_io(vga))) != UAM_OK ||
            (st = uam_bus_map_mmio_ex(&m->bus, "vga-mem", 0xA0000, UAM_VGA_MEM_SIZE, uam_vga_mem(vga),
                                      &(uam_map_attrs){ .prio = 1 })) != UAM_OK)
            return st;
        if (vgabios) {
            uint8_t *data;
            size_t len;
            if ((st = uam_load_file(vgabios, &data, &len)) != UAM_OK)
                return st;
            uint64_t romsz = 2048;
            while (romsz < len)
                romsz <<= 1;
            uam_device *img = uam_pci_rom_image_create("vgabios", data, len);
            st = img ? uam_bus_map_rom(&m->bus, "vgabios-isa", 0xC0000, (len + 511) & ~511ull, data, len)
                     : UAM_ERR_NOMEM;
            free(data);
            if (st != UAM_OK || (st = add(m, img)) != UAM_OK)
                return st;
            uam_pci_func_rom(uam_vga_func(vga), romsz, img);
        }
        if ((st = uam_pci_register(pci, uam_vga_func(vga), 5, 0)) != UAM_OK)
            return st;
        t->vga = vga;
        t->vgadump = opt_on(cfg, "vgadump");
    }
    if (opt_on(cfg, "lpt")) {
        uam_device *lpt = uam_lpt_create("lpt1", cfg->console);
        if ((st = add(m, lpt)) != UAM_OK || (st = map_io(m, "lpt1", 0x378, 3, lpt)) != UAM_OK)
            return st;
        uam_signal_connect(uam_lpt_irq_line(lpt), isa_sink, t, 7);
    }
    return UAM_OK;
}

static uam_status x86_test_build(uam_machine *m, const uam_board_config *cfg)
{
    uam_status st = uam_board_x86_64_ref.build(m, cfg);
    if (st != UAM_OK)
        return st;
    m->board_name = "x86_64-test";

    x86_test *t = calloc(1, sizeof(*t));
    if (!t)
        return UAM_ERR_NOMEM;
    t->m = m;
    t->ref_data = m->board_data;
    t->ref_fini = m->board_fini;
    m->board_data = t;
    m->board_fini = x86_test_fini;
    t->pic = uam_machine_find_device(m, "pic");
    t->ioapic = uam_machine_find_device(m, "ioapic");
    uam_device *dma = uam_machine_find_device(m, "dma");
    uam_pci_bus *pci = uam_pci_bus_of(uam_machine_find_device(m, "pci"));
    if (!t->pic || !t->ioapic || !dma || !pci) {
        uam_log(UAM_LOG_ERROR, "x86_64-test", "x86_64-ref has no pic, ioapic, dma or pci device");
        return UAM_ERR_INVALID;
    }

    uam_device *smip = uam_smi_port_create("smi-port");
    if ((st = add(m, smip)) != UAM_OK || (st = map_io(m, "smi-port", 0xB2, 1, smip)) != UAM_OK)
        return st;
    uam_signal_connect(uam_smi_port_line(smip), smi_all, m, 0);

    if ((st = add_storage(t, cfg, pci, dma)) != UAM_OK || (st = add_display(t, cfg, pci)) != UAM_OK)
        return st;
    uam_machine_reset(m, m->reset_pc);
    return UAM_OK;
}

const uam_board test_board_x86_64 = {
    .name = "x86_64-test",
    .description = "x86_64-ref plus the 0xB2 SMI port, storage, VGA and printer options (tests only)",
    .build = x86_test_build,
};

/* ---- rv32-test ------------------------------------------------------------ */

#define RV32TEST_VIRTIO_BASE   UINT64_C(0x10001000)
#define RV32TEST_VIRTIO_STRIDE UINT64_C(0x1000)
#define RV32TEST_VIRTIO_COUNT  8u
#define RV32TEST_VIRTIO_IRQ    1u

static const char *const virtio_names[RV32TEST_VIRTIO_COUNT] = {
    "virtio0", "virtio1", "virtio2", "virtio3", "virtio4", "virtio5", "virtio6", "virtio7",
};

static uam_status rv32_test_build(uam_machine *m, const uam_board_config *cfg)
{
    uam_status st = uam_board_rv32_ref.build(m, cfg);
    if (st != UAM_OK)
        return st;
    m->board_name = "rv32-test";
    uam_device *plic = uam_machine_find_device(m, "plic");
    if (!plic) {
        uam_log(UAM_LOG_ERROR, "rv32-test", "rv32-ref has no plic device");
        return UAM_ERR_INVALID;
    }
    for (unsigned i = 0; i < RV32TEST_VIRTIO_COUNT; i++) {
        char port[8], key[16];
        snprintf(port, sizeof(port), "net%u", i);
        snprintf(key, sizeof(key), "net%u.mac", i);
        uam_netdev *nd = uam_board_config_netdev(cfg, port);
        if (!nd)
            continue;   /* unplugged: nothing in this slot */
        uint8_t mac[6] = { 0x02, 0x75, 0x61, 0x6d, 0x01, (uint8_t)i };
        const char *opt = uam_board_config_option(cfg, key);
        unsigned b[6];
        if (opt) {
            if (sscanf(opt, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
                uam_log(UAM_LOG_ERROR, "rv32-test", "bad %s '%s'", key, opt);
                return UAM_ERR_INVALID;
            }
            for (int k = 0; k < 6; k++)
                mac[k] = (uint8_t)b[k];
        }
        uam_device *dev = uam_virtio_net_create(virtio_names[i], &m->bus, nd, mac);
        if ((st = add(m, dev)) != UAM_OK ||
            (st = uam_bus_map_mmio(&m->bus, dev->name, RV32TEST_VIRTIO_BASE + i * RV32TEST_VIRTIO_STRIDE,
                                   UAM_VIRTIO_MMIO_SIZE, dev)) != UAM_OK)
            return st;
        uam_signal_connect(uam_virtio_net_irq_line(dev), uam_plic_input, plic, RV32TEST_VIRTIO_IRQ + i);
    }
    uam_machine_reset(m, m->reset_pc);
    return UAM_OK;
}

const uam_board test_board_rv32 = {
    .name = "rv32-test",
    .description = "rv32-ref plus virtio-net ports net0..net7 (tests only)",
    .build = rv32_test_build,
};
