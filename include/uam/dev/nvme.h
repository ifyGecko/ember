/*
 * uam/dev/nvme.h - NVM Express controller (NVMe 1.4, a PCI Express function).
 *
 * The controller is a PCI mass-storage function (class 01:08:02) with its
 * registers in a 16 KiB 64-bit BAR0: the controller registers, the doorbells
 * from 0x1000 (stride 4), and the MSI-X table (0x2000) and pending bits
 * (0x3000). It also has MSI and a PCI Express capability, interrupt pin INTA.
 * The board registers the function (uam_nvme_func) wherever its platform puts
 * it, and may adjust the IDs first.
 *
 * Admin commands: Identify (controller, namespace, active namespace list,
 * namespace descriptors), Create/Delete I/O Submission/Completion Queue,
 * Get/Set Features (number of queues, volatile write cache, interrupt
 * coalescing and others accepted), Get Log Page (error, SMART, firmware
 * pages: zeroes), Asynchronous Event Request (held, never completed), Abort.
 * NVM commands: Read, Write, Flush, Write Zeroes, Dataset Management
 * (accepted). Data moves through PRP1/PRP2 and PRP lists. Commands execute
 * when software rings a submission doorbell; their completions are posted
 * before the doorbell write returns.
 *
 * Namespaces: up to UAM_NVME_MAX_NS, each a block device with 512-byte
 * logical blocks.
 */
#ifndef UAM_DEV_NVME_H
#define UAM_DEV_NVME_H

#include "uam/blockdev.h"
#include "uam/dev/pci.h"

#define UAM_NVME_MAX_NS     8u
#define UAM_NVME_MAX_QUEUES 16u    /* I/O queue pairs */

uam_device   *uam_nvme_create(const char *name, uint16_t vendor, uint16_t device, uint8_t revision);
uam_pci_func *uam_nvme_func(uam_device *nvme);
/* Attach `bd` (sector size 512) as namespace `nsid` (1..UAM_NVME_MAX_NS). The
 * controller does not take ownership of `bd`. */
uam_status    uam_nvme_attach(uam_device *nvme, unsigned nsid, uam_blockdev *bd);

#endif /* UAM_DEV_NVME_H */
