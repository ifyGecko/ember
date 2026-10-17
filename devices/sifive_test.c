/*
 * sifive_test.c - Simulation finisher (uam/dev/sifive_test.h).
 */
#include <stdlib.h>

#include "uam/dev/sifive_test.h"

#define FINISHER_FAIL  0x3333u
#define FINISHER_PASS  0x5555u
#define FINISHER_RESET 0x7777u

typedef struct sifive_test {
    uam_device    dev;
    uam_finish_fn finish;
    void         *ctx;
} sifive_test;

static uam_status test_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    (void)d; (void)off; (void)size;
    *val = 0;
    return UAM_OK;
}

static uam_status test_write(uam_device *d, uint64_t off, unsigned size, uint64_t v)
{
    sifive_test *t = UAM_CONTAINER_OF(d, sifive_test, dev);
    if (off != 0 || size != 4)
        return UAM_OK;
    /* The command is in the low 16 bits and the exit code in the high 16.
     * A big-endian guest's MMIO access arrives byte-swapped, so accept the
     * command from whichever half carries it (this finisher is a test aid and
     * must work regardless of guest byte order). */
    uint32_t lo = v & 0xFFFF, hi = (v >> 16) & 0xFFFF;
    uint32_t cmd, code;
    if (lo == FINISHER_PASS || lo == FINISHER_FAIL || lo == FINISHER_RESET) {
        cmd = lo;
        code = hi;
    } else {
        cmd = hi;
        code = lo;
    }
    switch (cmd) {
    case FINISHER_PASS:  t->finish(t->ctx, 0); break;
    case FINISHER_FAIL:  t->finish(t->ctx, (int64_t)(code ? code : 1)); break;
    case FINISHER_RESET: t->finish(t->ctx, -1); break;
    default: break;
    }
    return UAM_OK;
}

static void test_destroy(uam_device *d)
{
    free(UAM_CONTAINER_OF(d, sifive_test, dev));
}

static const uam_device_ops test_ops = {
    .type = "sifive,test0",
    .read = test_read,
    .write = test_write,
    .destroy = test_destroy,
};

uam_device *uam_sifive_test_create(const char *name, uam_finish_fn finish, void *ctx)
{
    sifive_test *t = calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    uam_device_init(&t->dev, &test_ops, name);
    t->finish = finish;
    t->ctx = ctx;
    return &t->dev;
}
