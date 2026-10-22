/*
 * ipi.c - Inter-processor interrupt doorbells (uam/dev/ipi.h).
 */
#include <stdlib.h>

#include "uam/dev/ipi.h"

typedef struct ipi {
    uam_device dev;
    unsigned   n;
    uam_signal line[];
} ipi;

static ipi *to_ipi(uam_device *d) { return UAM_CONTAINER_OF(d, ipi, dev); }

static uam_status ipi_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    (void)size;
    ipi *p = to_ipi(d);
    *val = off / 4 < p->n ? (uint64_t)p->line[off / 4].level : 0;
    return UAM_OK;
}

static uam_status ipi_write(uam_device *d, uint64_t off, unsigned size, uint64_t val)
{
    (void)size;
    ipi *p = to_ipi(d);
    if (off / 4 < p->n)
        uam_signal_set(&p->line[off / 4], val != 0);
    return UAM_OK;
}

static void ipi_reset(uam_device *d)
{
    ipi *p = to_ipi(d);
    for (unsigned i = 0; i < p->n; i++)
        uam_signal_set(&p->line[i], 0);
}

static void ipi_destroy(uam_device *d) { free(to_ipi(d)); }

static void ipi_save(uam_device *d, uam_state_writer *w)
{
    ipi *p = to_ipi(d);
    uam_state_put_u32(w, p->n);
    for (unsigned i = 0; i < p->n; i++)
        uam_state_put_signal(w, &p->line[i]);
}

static void ipi_restore(uam_device *d, uam_state_reader *r)
{
    ipi *p = to_ipi(d);
    uam_state_expect_u32(r, p->n, "ipi processor count");
    for (unsigned i = 0; i < p->n; i++)
        uam_state_get_signal(r, &p->line[i]);
}

static const uam_device_ops ipi_ops = {
    .type = "uam,ipi", .impl_min = 4, .impl_max = 4,
    .read = ipi_read, .write = ipi_write, .reset = ipi_reset, .destroy = ipi_destroy,
    .save = ipi_save, .restore = ipi_restore,
};

uam_device *uam_ipi_create(const char *name, unsigned n_cpus)
{
    if (n_cpus < 1 || n_cpus > UAM_IPI_MAX_CPUS)
        return NULL;
    ipi *p = calloc(1, sizeof(*p) + n_cpus * sizeof(uam_signal));
    if (!p)
        return NULL;
    uam_device_init(&p->dev, &ipi_ops, name);
    p->n = n_cpus;
    for (unsigned i = 0; i < n_cpus; i++)
        uam_signal_init(&p->line[i]);
    return &p->dev;
}

uam_signal *uam_ipi_line(uam_device *d, unsigned n)
{
    ipi *p = to_ipi(d);
    return n < p->n ? &p->line[n] : NULL;
}
