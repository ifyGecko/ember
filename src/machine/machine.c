/*
 * machine.c - Machine lifecycle, resets, and the board registry (uam/machine.h).
 *
 * The registry holds no boards of its own: linking the library does not pull
 * in any ISA or device. Programs register the boards they use.
 */
#include <stdlib.h>
#include <string.h>

#include "uam/machine.h"

/* Scheduler kick: a timer was armed earlier than the current deadline; the
 * processor running now must leave its block. */
static void sched_kick(void *ctx)
{
    uam_exec *x = ctx;
    uam_cpu_request_exit(x->running ? x->running : x->cpu);
}

/* Bus code-write hook with several processors: every translation cache. */
static void machine_code_write(void *ctx, uint64_t page)
{
    (void)page;
    uam_machine *m = ctx;
    for (uint32_t i = 0; i <= m->n_aps; i++) {
        uam_exec *x = i ? m->ap_execs[i - 1] : &m->exec;
        x->tc.gen++;
        uam_cpu_request_exit(x->cpu);
    }
}

/* Executor reset handler: runs between instructions inside uam_exec_run. */
static void machine_do_reset(void *ctx, uam_reset_kind kind)
{
    uam_machine *m = ctx;
    if (m->on_reset)
        m->on_reset(m, kind, m->on_reset_ctx);
    if (kind == UAM_RESET_SYSTEM) {
        uam_machine_reset(m, m->reset_pc);
    } else {
        m->cpu.isa->reset(&m->cpu, m->reset_pc);
        for (uint32_t i = 0; i < m->n_aps; i++)
            m->aps[i]->isa->reset(m->aps[i], m->reset_pc);
    }
}

void uam_machine_reset_sink(void *ctx, unsigned input, int level)
{
    uam_machine *m = ctx;
    if (level)
        uam_exec_request_reset(&m->exec, input == UAM_RESET_CPU ? UAM_RESET_CPU : UAM_RESET_SYSTEM);
}

/* ---- ABI handshake ------------------------------------------------------ */

bool uam_abi_check(const uam_abi_layout *caller, const char *entry)
{
    static const char *const names[] = { "uam_machine", "uam_exec", "uam_cpu", "uam_bus",
                                         "uam_sched", "uam_timer", "uam_device", "uam_chardev",
                                         "uam_netdev", "uam_board", "uam_board_config" };
    const uam_abi_layout *lib = UAM_ABI_LAYOUT;
    if (caller->version != lib->version || caller->self != lib->self) {
        uam_log(UAM_LOG_ERROR, "abi", "%s: caller compiled against UAM ABI %u, library is ABI %u; "
                "rebuild one against the other's headers", entry, (unsigned)caller->version,
                (unsigned)lib->version);
        return false;
    }
    const uint32_t *got = &caller->machine, *want = &lib->machine;
    bool ok = true;
    for (size_t i = 0; i < UAM_ARRAY_LEN(names); i++) {
        if (got[i] != want[i]) {
            uam_log(UAM_LOG_ERROR, "abi", "%s: sizeof(%s) is %u in the caller, %u in the library",
                    entry, names[i], (unsigned)got[i], (unsigned)want[i]);
            ok = false;
        }
    }
    if (!ok)
        uam_log(UAM_LOG_ERROR, "abi", "%s: the caller and libuam were built from different UAM "
                "headers (same ABI version %u); rebuild both from one tree", entry,
                (unsigned)lib->version);
    return ok;
}

static void abi_require(const uam_abi_layout *caller, const char *entry)
{
    /* Nothing has touched the caller's structs yet: a plain error exit is safe. */
    if (!uam_abi_check(caller, entry))
        exit(UAM_ABI_EXIT_STATUS);
}

uam_status uam_machine_init_abi(uam_machine *m, const uam_isa *isa, const uam_abi_layout *abi)
{
    abi_require(abi, "uam_machine_init");
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
    uam_sched_set_kick(&m->sched, sched_kick, &m->exec);
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
    for (uint32_t i = 0; i < m->n_aps; i++) {
        uam_exec_fini(m->ap_execs[i]);
        free(m->ap_execs[i]);
        if (m->aps[i]->isa_state)
            m->aps[i]->isa->fini(m->aps[i]);
        free(m->aps[i]->regfile);
        free(m->aps[i]);
    }
    free(m->aps);
    free(m->ap_execs);
    free(m->all_execs);
    free(m->ap_cov);
    m->ap_cov = NULL;
    m->aps = NULL;
    m->ap_execs = m->all_execs = NULL;
    m->n_aps = 0;
    m->exec.smp = NULL;
    m->exec.n_smp = 0;
    uam_device *d = m->devices;
    while (d) {
        uam_device *n = d->next;
        d->ops->destroy(d);
        d = n;
    }
    m->devices = NULL;
    for (uint32_t i = 0; i < m->n_state_parts; i++)
        free(m->state_parts[i].name);
    free(m->state_parts);
    m->state_parts = NULL;
    m->n_state_parts = m->cap_state_parts = 0;
    for (uint32_t i = 0; i < m->n_inputs; i++)
        free(m->inputs[i].name);
    free(m->inputs);
    m->inputs = NULL;
    m->n_inputs = m->cap_inputs = 0;
    if (m->cpu.isa && m->cpu.isa_state)
        m->cpu.isa->fini(&m->cpu);
    free(m->cpu.regfile);
    m->cpu.regfile = NULL;
    uam_bus_fini(&m->bus);
}

uam_status uam_machine_add_input(uam_machine *m, const char *name, uam_input_fn fn, void *ctx)
{
    if (!name || !fn || uam_machine_find_input(m, name))
        return UAM_ERR_INVALID;
    if (m->n_inputs == m->cap_inputs) {
        uint32_t ncap = m->cap_inputs ? m->cap_inputs * 2 : 4;
        uam_machine_input *ni = realloc(m->inputs, sizeof(*ni) * ncap);
        if (!ni)
            return UAM_ERR_NOMEM;
        m->inputs = ni;
        m->cap_inputs = ncap;
    }
    char *nm = strdup(name);
    if (!nm)
        return UAM_ERR_NOMEM;
    m->inputs[m->n_inputs++] = (uam_machine_input){ .name = nm, .fn = fn, .ctx = ctx };
    return UAM_OK;
}

const uam_machine_input *uam_machine_find_input(const uam_machine *m, const char *name)
{
    for (uint32_t i = 0; i < m->n_inputs; i++)
        if (!strcmp(m->inputs[i].name, name))
            return &m->inputs[i];
    return NULL;
}

void uam_machine_add_device_abi(uam_machine *m, uam_device *dev, const uam_abi_layout *abi)
{
    abi_require(abi, "uam_machine_add_device");
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
    if (!m->exec.running) {
        /* A reset from the host starts the interleaving afresh (one requested
         * by the guest happens inside a slice, which carries on). */
        m->exec.slice_cpu = 0;
        m->exec.slice_left = 0;
        m->exec.round_ran = false;
    }
    for (uam_device *d = m->devices; d; d = d->next)
        if (d->ops->reset)
            d->ops->reset(d);
    m->cpu.isa->reset(&m->cpu, entry);
    for (uint32_t i = 0; i < m->n_aps; i++)
        m->aps[i]->isa->reset(m->aps[i], entry);
}

uam_status uam_machine_add_cpus(uam_machine *m, unsigned count)
{
    if (!count)
        return UAM_OK;
    uint32_t n = m->n_aps + count;
    uam_cpu **aps = realloc(m->aps, sizeof(*aps) * n);
    if (aps)
        m->aps = aps;
    uam_exec **xs = realloc(m->ap_execs, sizeof(*xs) * n);
    if (xs)
        m->ap_execs = xs;
    uam_exec **all = realloc(m->all_execs, sizeof(*all) * (n + 1));
    if (all)
        m->all_execs = all;
    if (!aps || !xs || !all)
        return UAM_ERR_NOMEM;
    const uam_isa *isa = m->cpu.isa;
    while (m->n_aps < n) {
        uam_cpu *cpu = calloc(1, sizeof(*cpu));
        uam_exec *x = calloc(1, sizeof(*x));
        if (!cpu || !x || !(cpu->regfile = calloc(1, isa->regfile_size))) {
            if (cpu) free(cpu->regfile);
            free(cpu);
            free(x);
            return UAM_ERR_NOMEM;
        }
        cpu->isa = isa;
        cpu->bus = &m->bus;
        cpu->sched = &m->sched;
        cpu->regfile_size = isa->regfile_size;
        cpu->hartid = m->n_aps + 1;
        uam_status st = isa->init(cpu);
        if (st == UAM_OK)
            st = uam_exec_init(x, cpu, &m->sched);
        if (st != UAM_OK) {
            if (cpu->isa_state) isa->fini(cpu);
            free(cpu->regfile);
            free(cpu);
            free(x);
            return st;
        }
        x->ctl = &m->exec;
        m->aps[m->n_aps] = cpu;
        m->ap_execs[m->n_aps] = x;
        m->n_aps++;
    }
    m->all_execs[0] = &m->exec;
    for (uint32_t i = 0; i < m->n_aps; i++)
        m->all_execs[i + 1] = m->ap_execs[i];
    m->exec.smp = m->all_execs;
    m->exec.n_smp = m->n_aps + 1;
    uam_bus_set_code_write_hook(&m->bus, machine_code_write, m);
    return UAM_OK;
}

unsigned uam_machine_ncpus(const uam_machine *m) { return m->n_aps + 1; }

uam_cpu *uam_machine_cpu(uam_machine *m, unsigned i)
{
    return i == 0 ? &m->cpu : i <= m->n_aps ? m->aps[i - 1] : NULL;
}

uam_exec *uam_machine_exec(uam_machine *m, unsigned i)
{
    return i == 0 ? &m->exec : i <= m->n_aps ? m->ap_execs[i - 1] : NULL;
}

unsigned uam_machine_current_cpu(void *ctx)
{
    uam_machine *m = ctx;
    return m->exec.running ? m->exec.running->hartid : 0;
}

void uam_machine_set_quantum(uam_machine *m, uint32_t insns)
{
    m->exec.quantum = insns;
}

/* ---- observers on every processor --------------------------------------- */

uam_status uam_machine_add_pc_hook(uam_machine *m, uint64_t pc, uam_pc_hook_fn fn, void *ctx)
{
    for (uint32_t i = 0; i <= m->n_aps; i++) {
        uam_status st = uam_exec_add_pc_hook(uam_machine_exec(m, i), pc, fn, ctx);
        if (st != UAM_OK) {
            while (i--)
                uam_exec_remove_pc_hook(uam_machine_exec(m, i), pc, fn, ctx);
            return st;
        }
    }
    return UAM_OK;
}

bool uam_machine_remove_pc_hook(uam_machine *m, uint64_t pc, uam_pc_hook_fn fn, void *ctx)
{
    bool any = false;
    for (uint32_t i = 0; i <= m->n_aps; i++)
        any |= uam_exec_remove_pc_hook(uam_machine_exec(m, i), pc, fn, ctx);
    return any;
}

void uam_machine_set_exc_hook(uam_machine *m, uam_exc_hook_fn fn, void *ctx)
{
    for (uint32_t i = 0; i <= m->n_aps; i++)
        uam_exec_set_exc_hook(uam_machine_exec(m, i), fn, ctx);
}

void uam_machine_set_irq_hook(uam_machine *m, uam_irq_hook_fn fn, void *ctx)
{
    for (uint32_t i = 0; i <= m->n_aps; i++)
        uam_exec_set_irq_hook(uam_machine_exec(m, i), fn, ctx);
}

uam_status uam_machine_add_pass(uam_machine *m, const uam_pass *p)
{
    for (uint32_t i = 0; i <= m->n_aps; i++) {
        uam_status st = uam_exec_add_pass(uam_machine_exec(m, i), p);
        if (st != UAM_OK) {
            while (i--)
                uam_exec_remove_pass(uam_machine_exec(m, i), p);
            return st;
        }
    }
    return UAM_OK;
}

bool uam_machine_remove_pass(uam_machine *m, const uam_pass *p)
{
    bool any = false;
    for (uint32_t i = 0; i <= m->n_aps; i++)
        any |= uam_exec_remove_pass(uam_machine_exec(m, i), p);
    return any;
}

/* Coverage off everywhere; with `state`, the other processors' copies of it. */
static uam_status cov_prepare(uam_machine *m, const uam_cov_state *state)
{
    for (uint32_t i = 0; i <= m->n_aps; i++)
        uam_exec_set_coverage(uam_machine_exec(m, i), NULL, 0, NULL, NULL, 0);
    if (!state || !m->n_aps)
        return UAM_OK;
    if (!m->ap_cov && !(m->ap_cov = calloc(m->n_aps, sizeof(*m->ap_cov))))
        return UAM_ERR_NOMEM;
    for (uint32_t i = 0; i < m->n_aps; i++) {
        m->ap_cov[i] = *state;
        uam_cov_reset_prev(&m->ap_cov[i]);
        m->ap_cov[i].last_pc = UAM_COV_NO_PC;
    }
    return UAM_OK;
}

uam_status uam_machine_set_coverage(uam_machine *m, const uam_cov_site *sites, size_t n,
                                    uam_cov_state *state, char *err, size_t errlen)
{
    uam_status st = cov_prepare(m, sites && n ? state : NULL);
    if (st == UAM_OK)
        st = uam_exec_set_coverage(&m->exec, sites, n, state, err, errlen);
    for (uint32_t i = 0; st == UAM_OK && i < m->n_aps && sites && n && state; i++)
        st = uam_exec_set_coverage(m->ap_execs[i], sites, n, &m->ap_cov[i], err, errlen);
    if (st != UAM_OK)
        cov_prepare(m, NULL);
    return st;
}

uam_status uam_machine_set_block_coverage(uam_machine *m, uam_cov_state *state)
{
    uam_status st = cov_prepare(m, state);
    if (st == UAM_OK)
        st = uam_exec_set_block_coverage(&m->exec, state);
    for (uint32_t i = 0; st == UAM_OK && i < m->n_aps && state; i++)
        st = uam_exec_set_block_coverage(m->ap_execs[i], &m->ap_cov[i]);
    if (st != UAM_OK)
        cov_prepare(m, NULL);
    return st;
}

uam_cov_state *uam_machine_cov_state(uam_machine *m, unsigned i)
{
    uam_cpu *cpu = uam_machine_cpu(m, i);
    return cpu ? cpu->cov : NULL;
}

void uam_machine_get_stats(const uam_machine *m, uam_exec_stats *out)
{
    *out = m->exec.stats;
    for (uint32_t i = 0; i < m->n_aps; i++) {
        const uam_exec_stats *s = &m->ap_execs[i]->stats;
        out->blocks_translated += s->blocks_translated;
        out->blocks_executed += s->blocks_executed;
        out->exceptions += s->exceptions;
        out->interrupts += s->interrupts;
        out->idle_ticks += s->idle_ticks;
        out->resets += s->resets;
        out->hooks += s->hooks;
        out->blocks_native += s->blocks_native;
        out->blocks_compiled += s->blocks_compiled;
        out->blocks_interp += s->blocks_interp;
        out->blocks_chained += s->blocks_chained;
    }
}

uint64_t uam_machine_icount(const uam_machine *m)
{
    uint64_t n = m->cpu.icount;
    for (uint32_t i = 0; i < m->n_aps; i++)
        n += m->aps[i]->icount;
    return n;
}

uam_status uam_machine_add_state(uam_machine *m, const char *name, uam_state_save_fn save,
                                 uam_state_restore_fn restore, void *ctx)
{
    if (!name || !*name || !save || !restore)
        return UAM_ERR_INVALID;
    for (uint32_t i = 0; i < m->n_state_parts; i++)
        if (strcmp(m->state_parts[i].name, name) == 0)
            return UAM_ERR_INVALID;
    if (m->n_state_parts == m->cap_state_parts) {
        uint32_t ncap = m->cap_state_parts ? m->cap_state_parts * 2 : 4;
        uam_state_part *np = realloc(m->state_parts, sizeof(*np) * ncap);
        if (!np)
            return UAM_ERR_NOMEM;
        m->state_parts = np;
        m->cap_state_parts = ncap;
    }
    char *copy = strdup(name);
    if (!copy)
        return UAM_ERR_NOMEM;
    m->state_parts[m->n_state_parts++] = (uam_state_part){ copy, save, restore, ctx };
    return UAM_OK;
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

uam_status uam_board_register_abi(const uam_board *board, const uam_abi_layout *abi)
{
    abi_require(abi, "uam_board_register");
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
