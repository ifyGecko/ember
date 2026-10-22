/*
 * uam/dev/test_finisher.h - Test finisher: lets guest test code end the run.
 *
 * A 32-bit write to offset 0 ends the simulation:
 *   0x5555             pass   -> finish(ctx, 0)
 *   (code << 16)|0x3333 fail  -> finish(ctx, code)
 *   0x7777             reset  -> finish(ctx, -1)
 * Reads return 0. The command may arrive in either half of the word (a
 * big-endian guest's store reaches the device byte-swapped).
 */
#ifndef UAM_DEV_TEST_FINISHER_H
#define UAM_DEV_TEST_FINISHER_H

#include "uam/device.h"

#define UAM_TEST_FINISHER_SIZE 0x1000u

typedef void (*uam_finish_fn)(void *ctx, int64_t status);

uam_device *uam_test_finisher_create(const char *name, uam_finish_fn finish, void *ctx);

#endif /* UAM_DEV_TEST_FINISHER_H */
