/*
 * gptimer.c - General-purpose timer (uam/dev/gptimer.h).
 */
#include <stdlib.h>

#include "uam/dev/gptimer.h"

enum { CTRL_EN = 1u << 0, CTRL_PERIODIC = 1u << 1, CTRL_IE = 1u << 2, STATUS_IF = 1u << 0 };

typedef struct gptimer {
    uam_device dev;
    uam_sched *sched;
    uam_timer  t;
    uam_signal irq;
    uint32_t   ctrl, status, load;
    uint64_t   deadline;   /* virtual time the period ends (while counting) */
} gptimer;

static gptimer *to_gpt(uam_device *d) { return UAM_CONTAINER_OF(d, gptimer, dev); }

static void update_irq(gptimer *g)
{
    uam_signal_set(&g->irq, (g->status & STATUS_IF) && (g->ctrl & CTRL_IE));
}

/* Start a period now (if counting and LOAD is not 0). */
static void start(gptimer *g)
{
    uam_timer_cancel(g->sched, &g->t);
    if ((g->ctrl & CTRL_EN) && g->load) {
        g->deadline = uam_sched_now(g->sched) + g->load;
        uam_timer_arm(g->sched, &g->t, g->deadline);
    }
}

static void fired(void *ctx)
{
    gptimer *g = ctx;
    g->status |= STATUS_IF;
    if (g->ctrl & CTRL_PERIODIC) {
        g->deadline += g->load;
        uam_timer_arm(g->sched, &g->t, g->deadline);
    } else {
        g->ctrl &= ~CTRL_EN;
    }
    update_irq(g);
}

static uam_status gpt_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    (void)size;
    gptimer *g = to_gpt(d);
    switch (off) {
    case 0x0: *val = g->ctrl; break;
    case 0x4: *val = g->status; break;
    case 0x8: *val = g->load; break;
    case 0xC: {
        const uint64_t now = uam_sched_now(g->sched);
        *val = (g->ctrl & CTRL_EN) && g->load && g->deadline > now ? (uint32_t)(g->deadline - now) : 0;
        break;
    }
    default:  *val = 0; break;
    }
    return UAM_OK;
}

static uam_status gpt_write_masked(uam_device *d, uint64_t off, unsigned size, uint64_t val, uint64_t mask)
{
    (void)size;
    gptimer *g = to_gpt(d);
    const uint32_t m = (uint32_t)mask, v = (uint32_t)val & m;
    switch (off) {
    case 0x0: {
        const uint32_t old = g->ctrl;
        g->ctrl = ((g->ctrl & ~m) | v) & (CTRL_EN | CTRL_PERIODIC | CTRL_IE);
        if ((g->ctrl ^ old) & CTRL_EN)
            start(g);
        break;
    }
    case 0x4: g->status &= ~(v & STATUS_IF); break;
    case 0x8:
        g->load = (g->load & ~m) | v;
        start(g);
        break;
    default: break;
    }
    update_irq(g);
    return UAM_OK;
}

static uam_status gpt_write(uam_device *d, uint64_t off, unsigned size, uint64_t val)
{
    return gpt_write_masked(d, off, size, val, 0xFFFFFFFFu);
}

static void gpt_reset(uam_device *d)
{
    gptimer *g = to_gpt(d);
    uam_timer_cancel(g->sched, &g->t);
    g->ctrl = g->status = g->load = 0;
    g->deadline = 0;
    update_irq(g);
}

static void gpt_destroy(uam_device *d)
{
    gptimer *g = to_gpt(d);
    uam_timer_cancel(g->sched, &g->t);
    free(g);
}

static void gpt_save(uam_device *d, uam_state_writer *w)
{
    gptimer *g = to_gpt(d);
    uam_state_put_u32(w, g->ctrl);
    uam_state_put_u32(w, g->status);
    uam_state_put_u32(w, g->load);
    uam_state_put_u64(w, g->deadline);
    uam_state_put_signal(w, &g->irq);
    uam_state_put_timer(w, &g->t);
}

static void gpt_restore(uam_device *d, uam_state_reader *r)
{
    gptimer *g = to_gpt(d);
    g->ctrl = uam_state_get_u32(r);
    g->status = uam_state_get_u32(r);
    g->load = uam_state_get_u32(r);
    g->deadline = uam_state_get_u64(r);
    uam_state_get_signal(r, &g->irq);
    uam_state_get_timer(r, &g->t);
}

static const uam_device_ops gpt_ops = {
    .type = "uam,gptimer", .impl_min = 4, .impl_max = 4,
    .read = gpt_read, .peek = gpt_read, .write = gpt_write, .write_masked = gpt_write_masked,
    .reset = gpt_reset, .destroy = gpt_destroy, .save = gpt_save, .restore = gpt_restore,
};

uam_device *uam_gptimer_create(const char *name, uam_sched *sched)
{
    if (!sched)
        return NULL;
    gptimer *g = calloc(1, sizeof(*g));
    if (!g)
        return NULL;
    uam_device_init(&g->dev, &gpt_ops, name);
    g->sched = sched;
    uam_timer_init(&g->t, fired, g);
    uam_signal_init(&g->irq);
    return &g->dev;
}

uam_signal *uam_gptimer_irq(uam_device *d) { return &to_gpt(d)->irq; }

void uam_gptimer_ack(uam_device *d)
{
    gptimer *g = to_gpt(d);
    g->status &= ~STATUS_IF;
    update_irq(g);
}
