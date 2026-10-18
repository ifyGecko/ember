/*
 * uam/device.h - The Device interface.
 *
 * A device is a register file reachable through the bus plus whatever lines
 * (signal.h) and timers (sched.h) it owns. Devices embed `uam_device` as
 * their first member and implement `uam_device_ops`:
 *
 *     typedef struct my_uart { uam_device dev; ... } my_uart;
 *     static const uam_device_ops my_uart_ops = { .type = "my-uart", ... };
 *
 * Devices never see the CPU, the ISA, or the IR. Offsets passed to read/write
 * are relative to the mapped base (or the absolute address for the bus's
 * unmapped fallback device). Sizes are in bytes.
 *
 * Byte order and access width are the bus's job, not the device's:
 *
 *   - `endian` is the device's register byte order. Registers are passed as
 *     values; the bus byte-swaps only when an access's byte order differs from
 *     the device's, so a big-endian CPU reading a big-endian device register
 *     gets the register value, and a byte load at offset 0 gets its most
 *     significant byte. 0 means UAM_ACC_LE. A mapping can override it
 *     (uam_map_attrs.endian) when the same device sits on a bus of the other
 *     byte order.
 *   - `impl_min`/`impl_max` are the access sizes the device implements
 *     (0 = any). A narrower access is served by reading the containing
 *     impl_min-sized register and extracting its bytes; a wider one is split
 *     into impl_max-sized accesses. A narrow *write* goes to `write_masked`
 *     (the full register plus a byte-enable mask) if the device has one;
 *     otherwise the bus does read-merge-write. Devices whose reads have side
 *     effects should implement `write_masked`.
 *
 * With every field left 0 the bus passes accesses through unchanged and treats
 * the device as little-endian.
 *
 * Snapshots (snapshot.h): a device takes part by implementing `save` and
 * `restore` with the state stream in state.h, or by setting
 * UAM_DEVICE_STATELESS when it holds no guest-visible state. Taking a snapshot
 * of a machine with any other device fails, naming the device.
 *
 * Dependencies: common.h, state.h.
 */
#ifndef UAM_DEVICE_H
#define UAM_DEVICE_H

#include "uam/common.h"
#include "uam/state.h"

typedef struct uam_device uam_device;

/* uam_device_ops.flags */
enum uam_device_flag {
    UAM_DEVICE_STATELESS = 1u << 0,  /* nothing to save: snapshots skip the device */
};

typedef struct uam_device_ops {
    const char *type;
    unsigned    endian;              /* UAM_ACC_LE (0 = default) or UAM_ACC_BE */
    unsigned    impl_min, impl_max;  /* implemented access sizes in bytes; 0 = any */
    uam_status (*read)(uam_device *dev, uint64_t off, unsigned size, uint64_t *val);
    uam_status (*write)(uam_device *dev, uint64_t off, unsigned size, uint64_t val);
    /* Optional: write the bytes of `val` selected by `mask` (0xFF per enabled
     * byte, same layout as val) into the register at off. */
    uam_status (*write_masked)(uam_device *dev, uint64_t off, unsigned size, uint64_t val,
                               uint64_t mask);
    void       (*reset)(uam_device *dev);    /* optional */
    void       (*destroy)(uam_device *dev);  /* frees the device */
    /* Optional, together: write / read back the device's state (state.h). */
    void       (*save)(uam_device *dev, uam_state_writer *w);
    void       (*restore)(uam_device *dev, uam_state_reader *r);
    unsigned    flags;                       /* enum uam_device_flag */
} uam_device_ops;

struct uam_device {
    const uam_device_ops *ops;
    const char           *name;
    uam_device           *next;   /* owner's list (machine), not used by the device */
};

static inline void uam_device_init(uam_device *dev, const uam_device_ops *ops, const char *name)
{
    dev->ops = ops;
    dev->name = name;
    dev->next = NULL;
}

#endif /* UAM_DEVICE_H */
