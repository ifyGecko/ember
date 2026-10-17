/*
 * uam/dev/sifive_test.h - "sifive,test" finisher device.
 *
 * A 32-bit write to offset 0 ends the simulation:
 *   0x5555             pass   -> finish(ctx, 0)
 *   (code << 16)|0x3333 fail  -> finish(ctx, code)
 *   0x7777             reset  -> finish(ctx, -1)
 */
#ifndef UAM_DEV_SIFIVE_TEST_H
#define UAM_DEV_SIFIVE_TEST_H

#include "uam/device.h"

#define UAM_SIFIVE_TEST_SIZE 0x1000u

typedef void (*uam_finish_fn)(void *ctx, int64_t status);

uam_device *uam_sifive_test_create(const char *name, uam_finish_fn finish, void *ctx);

#endif /* UAM_DEV_SIFIVE_TEST_H */
