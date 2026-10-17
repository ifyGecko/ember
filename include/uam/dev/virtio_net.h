/*
 * uam/dev/virtio_net.h - virtio network device on the virtio-mmio transport
 * (virtio 1.x, MMIO register layout version 2).
 *
 * Offers VIRTIO_F_VERSION_1, VIRTIO_NET_F_MAC and VIRTIO_NET_F_STATUS: one
 * receive and one transmit split virtqueue, no offloads, no mergeable receive
 * buffers. The rings and frame buffers are reached with bus DMA. Frames go to
 * and from a uam_netdev; its link state is the device's link status.
 */
#ifndef UAM_DEV_VIRTIO_NET_H
#define UAM_DEV_VIRTIO_NET_H

#include "uam/bus.h"
#include "uam/device.h"
#include "uam/netdev.h"
#include "uam/signal.h"

#define UAM_VIRTIO_MMIO_SIZE 0x200u

/* `bus` is where the guest's rings live; `nd` is required. `mac` NULL picks
 * 02:75:61:6d:01:00 (locally administered). */
uam_device *uam_virtio_net_create(const char *name, uam_bus *bus, uam_netdev *nd,
                                  const uint8_t mac[6]);
uam_signal *uam_virtio_net_irq_line(uam_device *dev);

#endif /* UAM_DEV_VIRTIO_NET_H */
