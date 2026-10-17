/*
 * virtio_net.c - virtio-net over virtio-mmio version 2 (uam/dev/virtio_net.h).
 *
 * Virtio 1.x, split virtqueues: queue 0 receives, queue 1 transmits. Every
 * buffer starts with the 12-byte virtio_net_hdr_v1; nothing is offloaded, so
 * the header is ignored on transmit and zero (num_buffers = 1) on receive.
 * Rings live in guest memory and are reached with bus DMA; all virtio
 * structures are little-endian whatever the guest's byte order.
 *
 * Registers (offsets from the slot base; all 32-bit):
 *   000 MagicValue  004 Version (2)  008 DeviceID  00c VendorID
 *   010 DeviceFeatures  014 DeviceFeaturesSel  020 DriverFeatures  024 DriverFeaturesSel
 *   030 QueueSel  034 QueueNumMax  038 QueueNum  044 QueueReady  050 QueueNotify
 *   060 InterruptStatus  064 InterruptACK  070 Status
 *   080/084 QueueDesc  090/094 QueueDriver  0a0/0a4 QueueDevice  0fc ConfigGeneration
 *   100 config: mac[6], status (u16)
 */
#include <stdlib.h>
#include <string.h>

#include "uam/dev/virtio_net.h"

#define VIRTIO_MAGIC     0x74726976u   /* "virt" */
#define VIRTIO_VENDOR    0x004d4155u   /* "UAM" */
#define VIRTIO_ID_NET    1u

#define R_MAGIC          0x000
#define R_VERSION        0x004
#define R_DEVICE_ID      0x008
#define R_VENDOR_ID      0x00c
#define R_DEV_FEATURES   0x010
#define R_DEV_FEAT_SEL   0x014
#define R_DRV_FEATURES   0x020
#define R_DRV_FEAT_SEL   0x024
#define R_QUEUE_SEL      0x030
#define R_QUEUE_NUM_MAX  0x034
#define R_QUEUE_NUM      0x038
#define R_QUEUE_READY    0x044
#define R_QUEUE_NOTIFY   0x050
#define R_INT_STATUS     0x060
#define R_INT_ACK        0x064
#define R_STATUS         0x070
#define R_QUEUE_DESC_LO  0x080
#define R_QUEUE_DESC_HI  0x084
#define R_QUEUE_DRV_LO   0x090
#define R_QUEUE_DRV_HI   0x094
#define R_QUEUE_DEV_LO   0x0a0
#define R_QUEUE_DEV_HI   0x0a4
#define R_CONFIG_GEN     0x0fc
#define R_CONFIG         0x100

#define F_NET_MAC        (UINT64_C(1) << 5)
#define F_NET_STATUS     (UINT64_C(1) << 16)
#define F_VERSION_1      (UINT64_C(1) << 32)
#define DEVICE_FEATURES  (F_NET_MAC | F_NET_STATUS | F_VERSION_1)

#define S_DRIVER_OK      0x04u
#define S_NEEDS_RESET    0x40u

#define INT_VRING        0x1u
#define INT_CONFIG       0x2u

#define NET_S_LINK_UP    0x1u

#define DESC_F_NEXT      0x1u
#define DESC_F_WRITE     0x2u
#define AVAIL_F_NO_INTERRUPT 0x1u

#define QUEUE_MAX        256u
#define NET_HDR_LEN      12u
#define RXQ              0u
#define TXQ              1u

typedef struct vq {
    uint32_t num;
    bool     ready;
    uint64_t desc, avail, used;
    uint16_t last_avail;   /* next available-ring entry to take */
    uint16_t used_idx;     /* next used-ring entry to fill */
} vq;

typedef struct vnet {
    uam_device  dev;
    uam_bus    *bus;
    uam_netdev *nd;
    uam_signal  irq;
    uint8_t     mac[6];
    uint32_t    dev_feat_sel, drv_feat_sel, queue_sel, status, isr;
    uint64_t    driver_features;
    vq          q[2];
    bool        link_up;
    uint8_t     buf[NET_HDR_LEN + UAM_NETDEV_MTU];
} vnet;

static vnet *to_vnet(uam_device *d) { return UAM_CONTAINER_OF(d, vnet, dev); }

/* ---- guest memory (little-endian virtio structures) ----------------------- */

typedef struct desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags, next;
} desc;

static uint64_t le(const uint8_t *p, unsigned n)
{
    uint64_t v = 0;
    while (n--)
        v = v << 8 | p[n];
    return v;
}

static void put_le(uint8_t *p, uint64_t v, unsigned n)
{
    for (unsigned i = 0; i < n; i++, v >>= 8)
        p[i] = (uint8_t)v;
}

/* A malformed ring or a DMA fault: the device stops until the driver resets it. */
static void broken(vnet *s, const char *why, uint64_t addr)
{
    if (!(s->status & S_NEEDS_RESET))
        uam_log(UAM_LOG_WARN, s->dev.name, "%s (0x%llx): device needs reset", why,
                (unsigned long long)addr);
    s->status |= S_NEEDS_RESET;
    s->isr |= INT_CONFIG;
    uam_signal_set(&s->irq, 1);
}

static bool rd16(vnet *s, uint64_t addr, uint16_t *v)
{
    uint8_t b[2];
    if (uam_bus_dma_read(s->bus, addr, b, 2) != UAM_OK) {
        broken(s, "ring read fault", addr);
        return false;
    }
    *v = (uint16_t)le(b, 2);
    return true;
}

static bool rd_desc(vnet *s, const vq *q, unsigned i, desc *d)
{
    uint8_t b[16];
    uint64_t a = q->desc + 16u * i;
    if (i >= q->num || uam_bus_dma_read(s->bus, a, b, 16) != UAM_OK) {
        broken(s, "bad descriptor", a);
        return false;
    }
    d->addr = le(b, 8);
    d->len = (uint32_t)le(b + 8, 4);
    d->flags = (uint16_t)le(b + 12, 2);
    d->next = (uint16_t)le(b + 14, 2);
    return true;
}

static bool queue_live(const vnet *s, const vq *q)
{
    return q->ready && q->num && (s->status & S_DRIVER_OK) && !(s->status & S_NEEDS_RESET);
}

/* Head of the next available chain, if any. */
static bool pop(vnet *s, vq *q, uint16_t *head)
{
    uint16_t idx;
    if (!queue_live(s, q) || !rd16(s, q->avail + 2, &idx) || idx == q->last_avail)
        return false;
    if (!rd16(s, q->avail + 4 + 2u * (q->last_avail % q->num), head))
        return false;
    q->last_avail++;
    return true;
}

static void push_used(vnet *s, vq *q, uint16_t head, uint32_t len)
{
    uint8_t e[8], idx[2];
    put_le(e, head, 4);
    put_le(e + 4, len, 4);
    q->used_idx++;
    put_le(idx, q->used_idx, 2);
    uint64_t slot = q->used + 4 + 8u * ((uint16_t)(q->used_idx - 1) % q->num);
    if (uam_bus_dma_write(s->bus, slot, e, 8) != UAM_OK
        || uam_bus_dma_write(s->bus, q->used + 2, idx, 2) != UAM_OK)
        broken(s, "used ring write fault", q->used);
}

static void notify_guest(vnet *s, vq *q)
{
    uint16_t flags;
    if (rd16(s, q->avail, &flags) && (flags & AVAIL_F_NO_INTERRUPT))
        return;
    s->isr |= INT_VRING;
    uam_signal_set(&s->irq, 1);
}

/* ---- transmit ------------------------------------------------------------- */

static void process_tx(vnet *s)
{
    vq *q = &s->q[TXQ];
    bool any = false;
    uint16_t head;
    while (pop(s, q, &head)) {
        size_t len = 0;
        bool ok = true;
        desc d = { .flags = DESC_F_NEXT, .next = head };
        for (unsigned n = 0; ok && (d.flags & DESC_F_NEXT); n++) {
            if (n == q->num || !rd_desc(s, q, d.next, &d)) {
                ok = false;
                break;
            }
            if (d.flags & DESC_F_WRITE)
                continue;    /* a device-writable buffer has no data to send */
            size_t take = d.len;
            if (take > sizeof(s->buf) - len) {
                uam_log(UAM_LOG_WARN, s->dev.name, "transmit frame too long; dropped");
                ok = false;
                break;
            }
            if (uam_bus_dma_read(s->bus, d.addr, s->buf + len, take) != UAM_OK) {
                broken(s, "transmit buffer fault", d.addr);
                return;
            }
            len += take;
        }
        if (s->status & S_NEEDS_RESET)
            return;
        if (ok && len > NET_HDR_LEN)
            uam_netdev_send(s->nd, s->buf + NET_HDR_LEN, len - NET_HDR_LEN);
        push_used(s, q, head, 0);
        any = true;
    }
    if (any)
        notify_guest(s, q);
}

/* ---- receive -------------------------------------------------------------- */

static bool vnet_can_receive(void *ctx)
{
    vnet *s = ctx;
    vq *q = &s->q[RXQ];
    uint16_t idx;
    return queue_live(s, q) && rd16(s, q->avail + 2, &idx) && idx != q->last_avail;
}

static void vnet_receive(void *ctx, const uint8_t *frame, size_t len)
{
    vnet *s = ctx;
    vq *q = &s->q[RXQ];
    uint16_t head;
    if (!pop(s, q, &head))
        return;
    memset(s->buf, 0, NET_HDR_LEN);
    put_le(s->buf + 10, 1, 2);                /* num_buffers */
    memcpy(s->buf + NET_HDR_LEN, frame, len);
    size_t total = NET_HDR_LEN + len, done = 0;
    desc d = { .flags = DESC_F_NEXT, .next = head };
    for (unsigned n = 0; done < total && (d.flags & DESC_F_NEXT); n++) {
        if (n == q->num || !rd_desc(s, q, d.next, &d))
            return;
        if (!(d.flags & DESC_F_WRITE))
            continue;
        size_t put = total - done < d.len ? total - done : d.len;
        if (uam_bus_dma_write(s->bus, d.addr, s->buf + done, put) != UAM_OK) {
            broken(s, "receive buffer fault", d.addr);
            return;
        }
        done += put;
    }
    if (done < total) {
        /* No mergeable buffers: a chain too small for the frame truncates it;
         * the driver sees the short length and drops it. */
        uam_log(UAM_LOG_DEBUG, s->dev.name, "receive buffer too small (%zu < %zu)", done, total);
    }
    push_used(s, q, head, (uint32_t)done);
    notify_guest(s, q);
}

static void vnet_link_changed(void *ctx, bool up)
{
    vnet *s = ctx;
    if (s->link_up == up)
        return;
    s->link_up = up;
    if ((s->status & S_DRIVER_OK) && (s->driver_features & F_NET_STATUS)) {
        s->isr |= INT_CONFIG;
        uam_signal_set(&s->irq, 1);
    }
}

/* ---- registers ------------------------------------------------------------ */

static void vnet_reset(uam_device *d)
{
    vnet *s = to_vnet(d);
    s->dev_feat_sel = s->drv_feat_sel = s->queue_sel = s->status = s->isr = 0;
    s->driver_features = 0;
    memset(s->q, 0, sizeof(s->q));
    uam_signal_set(&s->irq, 0);
}

static uint8_t config_byte(const vnet *s, uint64_t off)
{
    if (off < 6)
        return s->mac[off];
    if (off == 6)
        return s->link_up ? NET_S_LINK_UP : 0;
    return 0;
}

static uam_status vnet_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    vnet *s = to_vnet(d);
    if (off >= R_CONFIG) {
        uint64_t v = 0;
        for (unsigned i = 0; i < size; i++)
            v |= (uint64_t)config_byte(s, off - R_CONFIG + i) << (8 * i);
        *val = v;
        return UAM_OK;
    }
    vq *q = &s->q[s->queue_sel < 2 ? s->queue_sel : 0];
    bool qok = s->queue_sel < 2;
    uint32_t v = 0;
    switch (off & ~UINT64_C(3)) {
    case R_MAGIC:     v = VIRTIO_MAGIC; break;
    case R_VERSION:   v = 2; break;
    case R_DEVICE_ID: v = VIRTIO_ID_NET; break;
    case R_VENDOR_ID: v = VIRTIO_VENDOR; break;
    default:
        switch (off & ~UINT64_C(3)) {
        case R_DEV_FEATURES:
            v = s->dev_feat_sel < 2 ? (uint32_t)(DEVICE_FEATURES >> (32 * s->dev_feat_sel)) : 0;
            break;
        case R_QUEUE_NUM_MAX: v = qok ? QUEUE_MAX : 0; break;
        case R_QUEUE_NUM:     v = qok ? q->num : 0; break;
        case R_QUEUE_READY:   v = qok && q->ready; break;
        case R_INT_STATUS:    v = s->isr; break;
        case R_STATUS:        v = s->status; break;
        case R_QUEUE_DESC_LO: v = qok ? (uint32_t)q->desc : 0; break;
        case R_QUEUE_DESC_HI: v = qok ? (uint32_t)(q->desc >> 32) : 0; break;
        case R_QUEUE_DRV_LO:  v = qok ? (uint32_t)q->avail : 0; break;
        case R_QUEUE_DRV_HI:  v = qok ? (uint32_t)(q->avail >> 32) : 0; break;
        case R_QUEUE_DEV_LO:  v = qok ? (uint32_t)q->used : 0; break;
        case R_QUEUE_DEV_HI:  v = qok ? (uint32_t)(q->used >> 32) : 0; break;
        case R_CONFIG_GEN:    v = 0; break;
        default: break;
        }
    }
    *val = (v >> (8 * (off & 3))) & (uint32_t)uam_mask(8 * size);
    return UAM_OK;
}

static void set_lo(uint64_t *r, uint32_t v) { *r = (*r & ~UINT64_C(0xffffffff)) | v; }
static void set_hi(uint64_t *r, uint32_t v) { *r = (*r & UINT64_C(0xffffffff)) | (uint64_t)v << 32; }

static uam_status vnet_write(uam_device *d, uint64_t off, unsigned size, uint64_t val)
{
    (void)size;
    vnet *s = to_vnet(d);
    if (off >= R_CONFIG)
        return UAM_OK;   /* the net config space is read-only */
    uint32_t v = (uint32_t)val;
    vq *q = s->queue_sel < 2 ? &s->q[s->queue_sel] : NULL;
    switch (off) {
    case R_DEV_FEAT_SEL: s->dev_feat_sel = v; break;
    case R_DRV_FEAT_SEL: s->drv_feat_sel = v; break;
    case R_DRV_FEATURES:
        if (s->drv_feat_sel == 0)
            set_lo(&s->driver_features, v & (uint32_t)DEVICE_FEATURES);
        else if (s->drv_feat_sel == 1)
            set_hi(&s->driver_features, v & (uint32_t)(DEVICE_FEATURES >> 32));
        break;
    case R_QUEUE_SEL: s->queue_sel = v; break;
    case R_QUEUE_NUM:
        if (q)
            q->num = v <= QUEUE_MAX ? v : QUEUE_MAX;
        break;
    case R_QUEUE_READY:
        if (q) {
            q->ready = v & 1;
            q->last_avail = q->used_idx = 0;
        }
        break;
    case R_QUEUE_DESC_LO: if (q) set_lo(&q->desc, v); break;
    case R_QUEUE_DESC_HI: if (q) set_hi(&q->desc, v); break;
    case R_QUEUE_DRV_LO:  if (q) set_lo(&q->avail, v); break;
    case R_QUEUE_DRV_HI:  if (q) set_hi(&q->avail, v); break;
    case R_QUEUE_DEV_LO:  if (q) set_lo(&q->used, v); break;
    case R_QUEUE_DEV_HI:  if (q) set_hi(&q->used, v); break;
    case R_QUEUE_NOTIFY:
        if (v == TXQ)
            process_tx(s);
        else if (v == RXQ)
            uam_netdev_accept_input(s->nd);   /* new receive buffers */
        break;
    case R_INT_ACK:
        s->isr &= ~v;
        uam_signal_set(&s->irq, s->isr != 0);
        break;
    case R_STATUS:
        if (v == 0) {
            vnet_reset(d);
        } else {
            bool go = (v & S_DRIVER_OK) && !(s->status & S_DRIVER_OK);
            s->status = v | (s->status & S_NEEDS_RESET);
            if (go) {
                process_tx(s);
                uam_netdev_accept_input(s->nd);
            }
        }
        break;
    default:
        break;
    }
    return UAM_OK;
}

static void vnet_destroy(uam_device *d)
{
    vnet *s = to_vnet(d);
    uam_netdev_set_frontend(s->nd, NULL, NULL, NULL, NULL);
    free(s);
}

static const uam_device_ops vnet_ops = {
    .type = "virtio-net-mmio",
    .endian = UAM_ACC_LE,
    .impl_max = 4,
    .read = vnet_read,
    .write = vnet_write,
    .reset = vnet_reset,
    .destroy = vnet_destroy,
};

uam_device *uam_virtio_net_create(const char *name, uam_bus *bus, uam_netdev *nd,
                                  const uint8_t mac[6])
{
    static const uint8_t default_mac[6] = { 0x02, 0x75, 0x61, 0x6d, 0x01, 0x00 };
    if (!nd)
        return NULL;
    vnet *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    uam_device_init(&s->dev, &vnet_ops, name);
    s->bus = bus;
    s->nd = nd;
    memcpy(s->mac, mac ? mac : default_mac, 6);
    uam_signal_init(&s->irq);
    s->link_up = true;
    uam_netdev_set_frontend(nd, vnet_can_receive, vnet_receive, vnet_link_changed, s);
    return &s->dev;
}

uam_signal *uam_virtio_net_irq_line(uam_device *d) { return &to_vnet(d)->irq; }
