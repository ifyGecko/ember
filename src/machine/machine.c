/*
 * machine.c - Machine lifecycle, resets, and the board registry (uam/machine.h).
 *
 * The registry holds no boards of its own: linking the library does not pull
 * in any ISA or device. Programs register the boards they use.
 */
#include <stdlib.h>
#include <string.h>

#include "uam/machine.h"

/* Scheduler kick: a timer was armed earlier than the current deadline. */
static void sched_kick(void *ctx)
{
    uam_cpu_request_exit(ctx);
}

/* Executor reset handler: runs between instructions inside uam_exec_run. */
static void machine_do_reset(void *ctx, uam_reset_kind kind)
{
    uam_machine *m = ctx;
    if (m->on_reset)
        m->on_reset(m, kind, m->on_reset_ctx);
    if (kind == UAM_RESET_SYSTEM)
        uam_machine_reset(m, m->reset_pc);
    else
        m->cpu.isa->reset(&m->cpu, m->reset_pc);
}

void uam_machine_reset_sink(void *ctx, unsigned input, int level)
{
    uam_machine *m = ctx;
    if (level)
        uam_exec_request_reset(&m->exec, input == UAM_RESET_CPU ? UAM_RESET_CPU : UAM_RESET_SYSTEM);
}

uam_status uam_machine_init(uam_machine *m, const uam_isa *isa)
{
    memset(m, 0, sizeof(*m));
    uam_bus_init(&m->bus);

    m->cpu.isa = isa;
    m->cpu.bus = &m->bus;
    m->cpu.regfile_size = isa->regfile_size;
    m->cpu.regfile = calloc(1, isa->regfile_size);
    if (!m->cpu.regfile)
        return UAM_ERR_NOMEM;
    uam_status st = isa->init(&m->cpu);
    if (st != UAM_OK)
        return st;

    uam_sched_init(&m->sched, &m->cpu.icount);
    uam_sched_set_kick(&m->sched, sched_kick, &m->cpu);
    m->cpu.sched = &m->sched;
    st = uam_exec_init(&m->exec, &m->cpu, &m->sched);
    if (st != UAM_OK)
        return st;
    uam_exec_set_reset_handler(&m->exec, machine_do_reset, m);
    return UAM_OK;
}

void uam_machine_fini(uam_machine *m)
{
    if (m->board_fini)
        m->board_fini(m);
    m->board_fini = NULL;
    uam_exec_fini(&m->exec);
    uam_device *d = m->devices;
    while (d) {
        uam_device *n = d->next;
        d->ops->destroy(d);
        d = n;
    }
    m->devices = NULL;
    if (m->cpu.isa && m->cpu.isa_state)
        m->cpu.isa->fini(&m->cpu);
    free(m->cpu.regfile);
    m->cpu.regfile = NULL;
    uam_bus_fini(&m->bus);
}

void uam_machine_add_device(uam_machine *m, uam_device *dev)
{
    /* Append, so devices reset in creation order. */
    uam_device **pp = &m->devices;
    while (*pp)
        pp = &(*pp)->next;
    dev->next = NULL;
    *pp = dev;
}

void uam_machine_reset(uam_machine *m, uint64_t entry)
{
    m->reset_pc = entry;
    for (uam_device *d = m->devices; d; d = d->next)
        if (d->ops->reset)
            d->ops->reset(d);
    m->cpu.isa->reset(&m->cpu, entry);
}

void uam_machine_stop_cb(void *ctx, int64_t status)
{
    uam_machine *m = ctx;
    uam_exec_stop(&m->exec, status);
}

/* ---- board configuration ----------------------------------------------- */

const char *uam_board_config_option(const uam_board_config *cfg, const char *key)
{
    const char *v = NULL;
    for (size_t i = 0; cfg && i < cfg->n_options; i++)
        if (strcmp(cfg->options[i].key, key) == 0)
            v = cfg->options[i].value;
    return v;
}

const char *uam_board_config_image(const uam_board_config *cfg, const char *name)
{
    const char *v = NULL;
    for (size_t i = 0; cfg && i < cfg->n_images; i++)
        if (strcmp(cfg->images[i].name, name) == 0)
            v = cfg->images[i].path;
    return v;
}

uam_netdev *uam_board_config_netdev(const uam_board_config *cfg, const char *name)
{
    uam_netdev *nd = NULL;
    for (size_t i = 0; cfg && i < cfg->n_netdevs; i++)
        if (strcmp(cfg->netdevs[i].name, name) == 0)
            nd = cfg->netdevs[i].nd;
    return nd;
}

/* ---- board registry ---------------------------------------------------- */

static const uam_board *registry[UAM_MAX_BOARDS];
static size_t n_registered;

uam_status uam_board_register(const uam_board *board)
{
    if (!board || !board->name || !board->build)
        return UAM_ERR_INVALID;
    for (size_t i = 0; i < n_registered; i++) {
        if (strcmp(registry[i]->name, board->name) == 0) {
            registry[i] = board;
            return UAM_OK;
        }
    }
    if (n_registered == UAM_MAX_BOARDS)
        return UAM_ERR_NOMEM;
    registry[n_registered++] = board;
    return UAM_OK;
}

const uam_board *uam_board_find(const char *name)
{
    for (size_t i = 0; i < n_registered; i++)
        if (strcmp(registry[i]->name, name) == 0)
            return registry[i];
    return NULL;
}

void uam_board_list(FILE *out)
{
    for (size_t i = 0; i < n_registered; i++)
        fprintf(out, "  %-12s %s\n", registry[i]->name,
                registry[i]->description ? registry[i]->description : "");
}
